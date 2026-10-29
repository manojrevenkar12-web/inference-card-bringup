// SPDX-License-Identifier: GPL-2.0
/*
 * LACC virtual inference card: PCI driver.
 *
 * Bring-up order in probe: map BAR0, check the ID, reset the card and wait for its boot ROM,
 * set the DMA mask, allocate the rings, allocate MSI-X, start the engine, then publish the
 * character device, the SMBus adapter (PMBus regulator, FRU EEPROM) and the SPI controller
 * (NOR flash with the firmware slots as MTD partitions).
 *
 * Recovery: a watchdog notices a halted engine, jobs that stop completing, or a link that
 * reads all-ones, snapshots the registers, aborts outstanding jobs, resets the card and
 * re-initialises the rings. Waiters get LACC_WAIT_ABORTED instead of hanging forever.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/iopoll.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "lacc.h"

#define CREATE_TRACE_POINTS
#include "lacc_trace.h"

#define LACC_MAX_XFER (64UL << 20)
#define LACC_WD_PERIOD msecs_to_jiffies(200)

static DEFINE_IDA(lacc_ida);

struct lacc_file {
	struct lacc_dev *ld;
	struct mutex lock; /* bos */
	struct list_head bos;
};

struct lacc_bo_node {
	struct list_head node;
	u64 offset, size;
};

/* ---- hardware ---------------------------------------------------------------------------- */

static int lacc_wait_ready(struct lacc_dev *ld, unsigned int timeout_ms)
{
	u32 st;

	return read_poll_timeout(lacc_rd, st, st != ~0u && (st & LACC_STATUS_READY), 200,
				 timeout_ms * 1000, false, ld, LACC_REG_STATUS);
}

static int lacc_hw_reset(struct lacc_dev *ld)
{
	u32 st;
	int ret;

	/* a link that reads all-ones has to come back before a reset can reach the card */
	ret = read_poll_timeout(lacc_rd, st, st != ~0u, 10000, 2000 * 1000, false, ld,
				LACC_REG_STATUS);
	if (ret) {
		dev_err(&ld->pdev->dev, "link did not come back\n");
		return ret;
	}
	lacc_wr(ld, LACC_REG_CONTROL, LACC_CONTROL_RESET);
	ret = lacc_wait_ready(ld, 200);
	if (ret)
		dev_err(&ld->pdev->dev, "card not ready after reset (status %#x)\n",
			lacc_rd(ld, LACC_REG_STATUS));
	return ret;
}

static void lacc_hw_start(struct lacc_dev *ld)
{
	memset(ld->sq, 0, LACC_SQ_ENTRIES * sizeof(*ld->sq));
	memset(ld->cq, 0, LACC_CQ_ENTRIES * sizeof(*ld->cq));
	ld->sq_tail = 0;
	ld->cq_head = 0;
	ld->cq_phase = true; /* the card writes phase 1 on its first pass */
	ld->outstanding = 0;
	lacc_wr(ld, LACC_REG_SQ_BASE_LO, lower_32_bits(ld->sq_dma));
	lacc_wr(ld, LACC_REG_SQ_BASE_HI, upper_32_bits(ld->sq_dma));
	lacc_wr(ld, LACC_REG_SQ_SIZE, LACC_SQ_ENTRIES);
	lacc_wr(ld, LACC_REG_CQ_BASE_LO, lower_32_bits(ld->cq_dma));
	lacc_wr(ld, LACC_REG_CQ_BASE_HI, upper_32_bits(ld->cq_dma));
	lacc_wr(ld, LACC_REG_CQ_SIZE, LACC_CQ_ENTRIES);
	lacc_wr(ld, LACC_REG_CQ_HEAD, 0);
	lacc_wr(ld, LACC_REG_IRQ_COALESCE, ld->coalesce);
	lacc_wr(ld, LACC_REG_IRQ_MASK,
		LACC_IRQ_COMPLETION | LACC_IRQ_ERROR | LACC_IRQ_THERMAL | LACC_IRQ_FW);
	lacc_wr(ld, LACC_REG_CONTROL, LACC_CONTROL_ENABLE);
}

/* ---- jobs ---------------------------------------------------------------------------------- */

static void lacc_job_release(struct lacc_dev *ld, struct lacc_job *job)
{
	struct device *dev = &ld->pdev->dev;

	if (job->sg_list)
		dma_free_coherent(dev, job->sg_list_size, job->sg_list, job->sg_list_dma);
	job->sg_list = NULL;
	if (job->sgt.sgl) {
		if (job->mapped)
			dma_unmap_sgtable(dev, &job->sgt, job->dir, 0);
		sg_free_table(&job->sgt);
		job->sgt.sgl = NULL;
	}
	if (job->pages) {
		unpin_user_pages_dirty_lock(job->pages, job->npages, job->dir == DMA_FROM_DEVICE);
		kvfree(job->pages);
		job->pages = NULL;
	}
}

/*
 * Jobs whose last descriptor completed (or that were aborted) are on @finished. Release their
 * DMA mappings and pinned pages outside the spinlock, then publish them as done: a waiter must
 * never free a job whose pages are still pinned.
 */
static void lacc_finish_jobs(struct lacc_dev *ld, struct list_head *finished)
{
	struct lacc_job *job, *tmp;

	list_for_each_entry_safe(job, tmp, finished, link) {
		list_del_init(&job->link);
		lacc_job_release(ld, job);
		trace_lacc_complete(job->seq, job->status, job->result,
				    job->done_ns - job->submit_ns);
		spin_lock(&ld->job_lock);
		WRITE_ONCE(job->done, true);
		if (!job->owner) { /* its file is gone: nobody will wait for it */
			xa_erase(&ld->jobs, (u32)job->seq);
			ld->njobs--;
			kfree(job);
		}
		spin_unlock(&ld->job_lock);
	}
	wake_up_all(&ld->done_wq);
	wake_up_all(&ld->space_wq);
}

/* Consume completions. Called from the interrupt thread and from the watchdog. */
static void lacc_process_cq(struct lacc_dev *ld)
{
	LIST_HEAD(finished);
	unsigned int n = 0;

	spin_lock(&ld->job_lock);
	for (;;) {
		struct lacc_cqe *c = &ld->cq[ld->cq_head];
		u16 flags = le16_to_cpu(READ_ONCE(c->flags));
		struct lacc_job *job;
		u32 tag;
		u16 status;

		if (!!(flags & LACC_CQE_PHASE) != ld->cq_phase)
			break;
		dma_rmb();
		tag = le32_to_cpu(c->tag);
		status = le16_to_cpu(c->status);
		job = xa_load(&ld->jobs, tag);
		if (!job || job->hw_done || job->ndesc == 0) {
			ld->stats.bad_cqes++;
			dev_warn_ratelimited(&ld->pdev->dev, "completion with unknown tag %#x\n",
					     tag);
		} else {
			if (status && !job->status)
				job->status = status;
			if (--job->ndesc == 0) {
				job->hw_done = true;
				job->done_ns = le64_to_cpu(c->done_ns);
				job->result = job->status ? LACC_WAIT_FAILED : LACC_WAIT_OK;
				if (job->status)
					ld->stats.failed++;
				ld->stats.completed++;
				ld->completed_seq = job->seq;
				list_add_tail(&job->link, &finished);
			}
			if (ld->outstanding)
				ld->outstanding--;
		}
		ld->stats.cqes++;
		n++;
		ld->cq_head = (ld->cq_head + 1) & (LACC_CQ_ENTRIES - 1);
		if (!ld->cq_head)
			ld->cq_phase = !ld->cq_phase;
	}
	if (n)
		lacc_wr(ld, LACC_REG_CQ_HEAD, ld->cq_head);
	spin_unlock(&ld->job_lock);
	if (n)
		trace_lacc_cq(n, ld->cq_head);
	lacc_finish_jobs(ld, &finished);
}

/* Abort every job the card still owes us (after a hang or before a reset). */
static void lacc_abort_jobs(struct lacc_dev *ld)
{
	LIST_HEAD(finished);
	struct lacc_job *job;
	unsigned long idx;

	spin_lock(&ld->job_lock);
	xa_for_each(&ld->jobs, idx, job) {
		if (job->hw_done)
			continue;
		job->hw_done = true;
		job->status = LACC_CQE_STATUS_ABORTED;
		job->result = LACC_WAIT_ABORTED;
		ld->stats.aborted++;
		list_add_tail(&job->link, &finished);
	}
	ld->outstanding = 0;
	spin_unlock(&ld->job_lock);
	lacc_finish_jobs(ld, &finished);
}

static struct lacc_job *lacc_job_new(struct file *f)
{
	struct lacc_job *job = kzalloc(sizeof(*job), GFP_KERNEL);

	if (job) {
		job->owner = f;
		INIT_LIST_HEAD(&job->link);
	}
	return job;
}

/*
 * Put a job's descriptors on the ring. The interrupt flag is set on the last descriptor only,
 * so a scatter-gather-less transfer of many segments costs one interrupt, not one per page.
 */
static bool lacc_has_room(struct lacc_dev *ld)
{
	return READ_ONCE(ld->outstanding) < LACC_SQ_ENTRIES - 1 || READ_ONCE(ld->dead);
}

static int lacc_submit(struct lacc_dev *ld, struct lacc_job *job, struct lacc_desc *descs, u32 n)
{
	u32 i = 0;
	int ret;

	if (!n)
		return -EINVAL;
	mutex_lock(&ld->submit_lock);
	if (ld->dead) {
		ret = -ENODEV;
		goto out;
	}
	spin_lock(&ld->job_lock);
	if (ld->njobs >= LACC_MAX_JOBS) {
		spin_unlock(&ld->job_lock);
		ret = -EBUSY;
		goto out;
	}
	job->seq = ++ld->next_seq;
	job->ndesc = n;
	ret = xa_insert(&ld->jobs, (u32)job->seq, job, GFP_ATOMIC);
	if (ret) {
		spin_unlock(&ld->job_lock);
		goto out;
	}
	ld->njobs++;
	spin_unlock(&ld->job_lock);
	job->submit_ns = lacc_dev_ns(ld);
	ld->stats.submitted++;
	trace_lacc_submit(job->seq, descs[0].opcode, n);

	while (i < n) {
		u32 room, batch;

		/* never have more descriptors in flight than the ring can hold */
		ret = wait_event_interruptible_timeout(ld->space_wq, lacc_has_room(ld),
						       msecs_to_jiffies(5000));
		if (ld->dead || ret == 0) {
			ret = ld->dead ? -ENODEV : -ETIMEDOUT;
			break;
		}
		if (ret < 0)
			break;
		spin_lock(&ld->job_lock);
		room = LACC_SQ_ENTRIES - 1 - ld->outstanding;
		batch = min(room, n - i);
		ld->outstanding += batch;
		spin_unlock(&ld->job_lock);
		ld->stats.desc_submitted += batch;
		for (; batch; batch--, i++) {
			struct lacc_desc *d = &ld->sq[ld->sq_tail];

			*d = descs[i];
			d->tag = cpu_to_le32((u32)job->seq);
			d->flags = (i == n - 1) ? LACC_DESC_IRQ : 0;
			ld->sq_tail = (ld->sq_tail + 1) & (LACC_SQ_ENTRIES - 1);
		}
		dma_wmb();
		lacc_wr(ld, LACC_REG_SQ_TAIL, ld->sq_tail);
		ret = 0;
	}
	if (ret && i < n) {
		/*
		 * Interrupted half-way: the descriptors already queued will complete; the job is
		 * failed and the rest never sent. The completion path accounts the queued ones.
		 */
		spin_lock(&ld->job_lock);
		job->ndesc -= n - i;
		job->status = LACC_CQE_STATUS_ABORTED;
		if (!job->ndesc) {
			job->hw_done = true;
			job->result = LACC_WAIT_ABORTED;
			spin_unlock(&ld->job_lock);
			mutex_unlock(&ld->submit_lock);
			lacc_job_release(ld, job);
			WRITE_ONCE(job->done, true);
			return 0;
		}
		spin_unlock(&ld->job_lock);
		ret = 0; /* the fence still resolves; the waiter sees the abort */
	}
out:
	mutex_unlock(&ld->submit_lock);
	return ret;
}

/* ---- recovery ------------------------------------------------------------------------------ */

int lacc_hw_restart(struct lacc_dev *ld, const char *why)
{
	int ret;

	mutex_lock(&ld->submit_lock);
	dev_warn(&ld->pdev->dev, "recovering: %s\n", why);
	trace_lacc_reset(why);
	lacc_snapshot(ld, why);
	ld->stats.resets++;
	synchronize_irq(pci_irq_vector(ld->pdev, 0));
	lacc_process_cq(ld); /* anything that did complete counts */
	lacc_abort_jobs(ld);
	ret = lacc_hw_reset(ld);
	if (!ret)
		lacc_hw_start(ld);
	else
		ld->dead = true;
	ld->wd_last_completed = ld->stats.completed;
	ld->wd_stuck_since = 0;
	mutex_unlock(&ld->submit_lock);
	wake_up_all(&ld->done_wq);
	wake_up_all(&ld->space_wq);
	return ret;
}

static void lacc_reset_work(struct work_struct *w)
{
	struct lacc_dev *ld = container_of(w, struct lacc_dev, reset_work);

	lacc_hw_restart(ld, ld->reset_reason);
	atomic_set(&ld->reset_queued, 0);
}

static void lacc_watchdog(struct work_struct *w)
{
	struct lacc_dev *ld = container_of(to_delayed_work(w), struct lacc_dev, watchdog);
	u32 st = lacc_rd(ld, LACC_REG_STATUS);
	const char *why = NULL;

	if (ld->dead)
		return;
	if (st == ~0u) {
		/* one outage, one count, one recovery: the reset waits for the link itself */
		if (!ld->link_lost) {
			ld->link_lost = true;
			ld->stats.link_downs++;
			why = "link down (registers read all-ones)";
		}
	} else if (st & LACC_STATUS_HALTED) {
		why = "engine halted";
	} else {
		/* a lost interrupt must not lose completions */
		lacc_process_cq(ld);
		if (READ_ONCE(ld->outstanding) && ld->stats.completed == ld->wd_last_completed) {
			if (!ld->wd_stuck_since)
				ld->wd_stuck_since = jiffies;
			else if (time_after(jiffies, ld->wd_stuck_since +
							     msecs_to_jiffies(ld->hang_timeout_ms)))
				why = "no completion within the hang timeout";
		} else {
			ld->wd_stuck_since = 0;
			ld->wd_last_completed = ld->stats.completed;
		}
	}
	if (st != ~0u)
		ld->link_lost = false;
	if (why && !atomic_xchg(&ld->reset_queued, 1)) {
		ld->reset_reason = why;
		queue_work(system_long_wq, &ld->reset_work);
	}
	schedule_delayed_work(&ld->watchdog, LACC_WD_PERIOD);
}

/* ---- interrupts ---------------------------------------------------------------------------- */

static irqreturn_t lacc_irq_cq(int irq, void *data)
{
	return IRQ_WAKE_THREAD;
}

static irqreturn_t lacc_irq_cq_thread(int irq, void *data)
{
	struct lacc_dev *ld = data;

	ld->stats.irqs++;
	lacc_wr(ld, LACC_REG_IRQ_STATUS, LACC_IRQ_COMPLETION);
	lacc_process_cq(ld);
	return IRQ_HANDLED;
}

static irqreturn_t lacc_irq_event_thread(int irq, void *data)
{
	struct lacc_dev *ld = data;
	u32 cause = lacc_rd(ld, LACC_REG_IRQ_STATUS) & ~LACC_IRQ_COMPLETION;

	if (cause == ~LACC_IRQ_COMPLETION) /* all-ones: the link is down */
		return IRQ_HANDLED;
	lacc_wr(ld, LACC_REG_IRQ_STATUS, cause);
	if (cause & LACC_IRQ_ERROR)
		dev_warn_ratelimited(&ld->pdev->dev, "card error %u\n",
				     lacc_rd(ld, LACC_REG_ERR_CODE));
	if (cause & LACC_IRQ_THERMAL) {
		ld->stats.thermal_events++;
		sysfs_notify(&ld->pdev->dev.kobj, NULL, "throttled");
	}
	return IRQ_HANDLED;
}

/* ---- character device ---------------------------------------------------------------------- */

static int lacc_open(struct inode *inode, struct file *f)
{
	struct lacc_dev *ld = container_of(f->private_data, struct lacc_dev, misc);
	struct lacc_file *lf = kzalloc(sizeof(*lf), GFP_KERNEL);

	if (!lf)
		return -ENOMEM;
	lf->ld = ld;
	mutex_init(&lf->lock);
	INIT_LIST_HEAD(&lf->bos);
	f->private_data = lf;
	get_device(&ld->pdev->dev);
	return 0;
}

static int lacc_release(struct inode *inode, struct file *f)
{
	struct lacc_file *lf = f->private_data;
	struct lacc_dev *ld = lf->ld;
	struct lacc_bo_node *bo, *tmp;
	struct lacc_job *job;
	unsigned long idx;

	/* jobs still running are orphaned and freed when they complete */
	spin_lock(&ld->job_lock);
	xa_for_each(&ld->jobs, idx, job) {
		if (job->owner != f)
			continue;
		if (READ_ONCE(job->done)) {
			xa_erase(&ld->jobs, idx);
			ld->njobs--;
			kfree(job);
		} else {
			job->owner = NULL;
		}
	}
	spin_unlock(&ld->job_lock);
	list_for_each_entry_safe(bo, tmp, &lf->bos, node) {
		lacc_mem_free(&ld->mem, bo->offset, bo->size);
		list_del(&bo->node);
		kfree(bo);
	}
	put_device(&ld->pdev->dev);
	kfree(lf);
	return 0;
}

static bool lacc_owns(struct lacc_file *lf, u64 off, u64 size)
{
	struct lacc_bo_node *bo;
	bool ok = false;

	mutex_lock(&lf->lock);
	list_for_each_entry(bo, &lf->bos, node) {
		if (off >= bo->offset && size <= bo->size && off - bo->offset <= bo->size - size) {
			ok = true;
			break;
		}
	}
	mutex_unlock(&lf->lock);
	return ok;
}

static long lacc_ioc_alloc(struct lacc_file *lf, void __user *arg)
{
	struct lacc_dev *ld = lf->ld;
	struct lacc_bo req;
	struct lacc_bo_node *bo;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (!req.size || req.size > ld->devmem_size)
		return -EINVAL;
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;
	bo->size = round_up(req.size, LACC_BO_ALIGN);
	if (lacc_mem_alloc(&ld->mem, bo->size, &bo->offset)) {
		kfree(bo);
		return -ENOSPC;
	}
	mutex_lock(&lf->lock);
	list_add(&bo->node, &lf->bos);
	mutex_unlock(&lf->lock);
	req.offset = bo->offset;
	req.size = bo->size;
	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

static long lacc_ioc_free(struct lacc_file *lf, void __user *arg)
{
	struct lacc_bo req;
	struct lacc_bo_node *bo;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	mutex_lock(&lf->lock);
	list_for_each_entry(bo, &lf->bos, node) {
		if (bo->offset == req.offset) {
			list_del(&bo->node);
			mutex_unlock(&lf->lock);
			lacc_mem_free(&lf->ld->mem, bo->offset, bo->size);
			kfree(bo);
			return 0;
		}
	}
	mutex_unlock(&lf->lock);
	return -ENOENT;
}

static long lacc_ioc_xfer(struct lacc_file *lf, struct file *f, void __user *arg)
{
	struct lacc_dev *ld = lf->ld;
	struct device *dev = &ld->pdev->dev;
	struct lacc_xfer x;
	struct lacc_job *job;
	struct lacc_desc *descs = NULL;
	struct scatterlist *sg;
	unsigned long first;
	unsigned int i, nents;
	int pinned, ret;
	u64 off;

	if (copy_from_user(&x, arg, sizeof(x)))
		return -EFAULT;
	if (!x.size || x.size > LACC_MAX_XFER || x.dir > LACC_DIR_D2H || x.mode > LACC_XFER_PER_SEG)
		return -EINVAL;
	if (!lacc_owns(lf, x.dev_offset, x.size))
		return -EACCES;
	job = lacc_job_new(f);
	if (!job)
		return -ENOMEM;
	job->dir = x.dir == LACC_DIR_H2D ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	first = x.user_ptr & PAGE_MASK;
	job->npages = DIV_ROUND_UP(offset_in_page(x.user_ptr) + x.size, PAGE_SIZE);
	job->pages = kvmalloc_array(job->npages, sizeof(*job->pages), GFP_KERNEL);
	if (!job->pages) {
		ret = -ENOMEM;
		goto err;
	}
	pinned = pin_user_pages_fast(first, job->npages, x.dir == LACC_DIR_D2H ? FOLL_WRITE : 0,
				     job->pages);
	if (pinned != job->npages) {
		if (pinned > 0)
			unpin_user_pages(job->pages, pinned);
		kvfree(job->pages);
		job->pages = NULL;
		ret = pinned < 0 ? pinned : -EFAULT;
		goto err;
	}
	ret = sg_alloc_table_from_pages(&job->sgt, job->pages, job->npages,
					offset_in_page(x.user_ptr), x.size, GFP_KERNEL);
	if (ret)
		goto err;
	ret = dma_map_sgtable(dev, &job->sgt, job->dir, 0);
	if (ret)
		goto err;
	job->mapped = true;
	nents = job->sgt.nents;

	if (x.mode == LACC_XFER_SG) {
		struct lacc_sg *list;

		job->sg_list_size = nents * sizeof(*list);
		job->sg_list =
			dma_alloc_coherent(dev, job->sg_list_size, &job->sg_list_dma, GFP_KERNEL);
		if (!job->sg_list) {
			ret = -ENOMEM;
			goto err;
		}
		list = job->sg_list;
		for_each_sgtable_dma_sg(&job->sgt, sg, i) {
			list[i].addr = cpu_to_le64(sg_dma_address(sg));
			list[i].len = cpu_to_le32(sg_dma_len(sg));
			list[i].reserved = 0;
		}
		descs = kzalloc(sizeof(*descs), GFP_KERNEL);
		if (!descs) {
			ret = -ENOMEM;
			goto err;
		}
		descs[0].opcode = x.dir == LACC_DIR_H2D ? LACC_OPCODE_SG_H2D : LACC_OPCODE_SG_D2H;
		descs[0].len = cpu_to_le32(nents);
		if (x.dir == LACC_DIR_H2D) {
			descs[0].addr0 = cpu_to_le64(job->sg_list_dma);
			descs[0].addr1 = cpu_to_le64(x.dev_offset);
		} else {
			descs[0].addr0 = cpu_to_le64(x.dev_offset);
			descs[0].addr1 = cpu_to_le64(job->sg_list_dma);
		}
		nents = 1;
	} else {
		descs = kvcalloc(nents, sizeof(*descs), GFP_KERNEL);
		if (!descs) {
			ret = -ENOMEM;
			goto err;
		}
		off = x.dev_offset;
		for_each_sgtable_dma_sg(&job->sgt, sg, i) {
			descs[i].opcode = x.dir == LACC_DIR_H2D ? LACC_OPCODE_H2D : LACC_OPCODE_D2H;
			descs[i].len = cpu_to_le32(sg_dma_len(sg));
			if (x.dir == LACC_DIR_H2D) {
				descs[i].addr0 = cpu_to_le64(sg_dma_address(sg));
				descs[i].addr1 = cpu_to_le64(off);
			} else {
				descs[i].addr0 = cpu_to_le64(off);
				descs[i].addr1 = cpu_to_le64(sg_dma_address(sg));
			}
			off += sg_dma_len(sg);
		}
	}
	ret = lacc_submit(ld, job, descs, nents);
	kvfree(descs);
	if (ret)
		goto err_nodesc;
	x.fence = job->seq;
	return copy_to_user(arg, &x, sizeof(x)) ? -EFAULT : 0;

err:
	kvfree(descs);
err_nodesc:
	if (!job->seq) {
		lacc_job_release(ld, job);
		kfree(job);
	}
	return ret;
}

static long lacc_ioc_gemm(struct lacc_file *lf, struct file *f, void __user *arg)
{
	struct lacc_gemm g;
	struct lacc_desc d = {};
	struct lacc_job *job;
	u64 a_len, b_len, c_len;
	int ret;

	if (copy_from_user(&g, arg, sizeof(g)))
		return -EFAULT;
	if (!g.m || !g.n || !g.k || g.m > 4096 || g.n > 4096 || g.k > 4096 || g.kind > LACC_GEMM_I8)
		return -EINVAL;
	a_len = (u64)g.m * g.k;
	b_len = (u64)g.k * g.n;
	c_len = (u64)g.m * g.n * 4;
	if (!lacc_owns(lf, g.a, a_len) || !lacc_owns(lf, g.b, b_len) || !lacc_owns(lf, g.c, c_len))
		return -EACCES;
	job = lacc_job_new(f);
	if (!job)
		return -ENOMEM;
	d.opcode = g.kind == LACC_GEMM_LNS8 ? LACC_OPCODE_GEMM_LNS8 : LACC_OPCODE_GEMM_I8;
	d.addr0 = cpu_to_le64(g.a);
	d.addr1 = cpu_to_le64(g.b);
	d.addr2 = cpu_to_le64(g.c);
	d.m = cpu_to_le32(g.m);
	d.n = cpu_to_le32(g.n);
	d.k = cpu_to_le32(g.k);
	ret = lacc_submit(lf->ld, job, &d, 1);
	if (ret) {
		kfree(job);
		return ret;
	}
	g.fence = job->seq;
	return copy_to_user(arg, &g, sizeof(g)) ? -EFAULT : 0;
}

static long lacc_ioc_fill(struct lacc_file *lf, struct file *f, void __user *arg)
{
	struct lacc_fill r;
	struct lacc_desc d = {};
	struct lacc_job *job;
	int ret;

	if (copy_from_user(&r, arg, sizeof(r)))
		return -EFAULT;
	if (!r.size || (r.size & 3) || r.size > U32_MAX)
		return -EINVAL;
	if (!lacc_owns(lf, r.dev_offset, r.size))
		return -EACCES;
	job = lacc_job_new(f);
	if (!job)
		return -ENOMEM;
	d.opcode = LACC_OPCODE_FILL;
	d.addr0 = cpu_to_le64(r.dev_offset);
	d.len = cpu_to_le32((u32)r.size);
	d.value = cpu_to_le32(r.value);
	ret = lacc_submit(lf->ld, job, &d, 1);
	if (ret) {
		kfree(job);
		return ret;
	}
	r.fence = job->seq;
	return copy_to_user(arg, &r, sizeof(r)) ? -EFAULT : 0;
}

static long lacc_ioc_wait(struct lacc_file *lf, struct file *f, void __user *arg)
{
	struct lacc_dev *ld = lf->ld;
	struct lacc_wait w;
	struct lacc_job *job;
	long left;

	if (copy_from_user(&w, arg, sizeof(w)))
		return -EFAULT;
	spin_lock(&ld->job_lock);
	job = xa_load(&ld->jobs, (u32)w.fence);
	if (!job || job->seq != w.fence || job->owner != f) {
		spin_unlock(&ld->job_lock);
		return -ENOENT;
	}
	spin_unlock(&ld->job_lock);
	left = wait_event_interruptible_timeout(ld->done_wq, READ_ONCE(job->done),
						msecs_to_jiffies(w.timeout_ms ? w.timeout_ms :
										10000));
	if (left < 0)
		return left;
	if (!left)
		return -ETIMEDOUT;
	w.result = job->result;
	w.status = job->status;
	w.submit_ns = job->submit_ns;
	w.done_ns = job->done_ns;
	spin_lock(&ld->job_lock);
	xa_erase(&ld->jobs, (u32)job->seq);
	ld->njobs--;
	spin_unlock(&ld->job_lock);
	kfree(job);
	return copy_to_user(arg, &w, sizeof(w)) ? -EFAULT : 0;
}

static long lacc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct lacc_file *lf = f->private_data;
	struct lacc_dev *ld = lf->ld;
	void __user *uarg = (void __user *)arg;

	if (READ_ONCE(ld->dead))
		return -ENODEV;
	switch (cmd) {
	case LACC_IOC_INFO: {
		struct lacc_info info = {
			.hw_version = lacc_rd(ld, LACC_REG_HW_VERSION),
			.caps = lacc_rd(ld, LACC_REG_CAPS),
			.fw_version = lacc_rd(ld, LACC_REG_FW_VERSION),
			.fw_status = lacc_rd(ld, LACC_REG_FW_STATUS),
			.devmem_size = ld->devmem_size,
			.status = lacc_rd(ld, LACC_REG_STATUS),
			.clock_mhz = lacc_rd(ld, LACC_REG_CLOCK_MHZ),
			.sq_entries = LACC_SQ_ENTRIES,
			.cq_entries = LACC_CQ_ENTRIES,
		};

		return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case LACC_IOC_ALLOC:
		return lacc_ioc_alloc(lf, uarg);
	case LACC_IOC_FREE:
		return lacc_ioc_free(lf, uarg);
	case LACC_IOC_XFER:
		return lacc_ioc_xfer(lf, f, uarg);
	case LACC_IOC_GEMM:
		return lacc_ioc_gemm(lf, f, uarg);
	case LACC_IOC_FILL:
		return lacc_ioc_fill(lf, f, uarg);
	case LACC_IOC_WAIT:
		return lacc_ioc_wait(lf, f, uarg);
	default:
		return -ENOTTY;
	}
}

/* Map device memory (BAR2) write-combined; only ranges inside the caller's buffer objects. */
static int lacc_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct lacc_file *lf = f->private_data;
	struct lacc_dev *ld = lf->ld;
	u64 off = (u64)vma->vm_pgoff << PAGE_SHIFT;
	u64 size = vma->vm_end - vma->vm_start;

	if (!lacc_owns(lf, off, size))
		return -EACCES;
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return io_remap_pfn_range(vma, vma->vm_start, (ld->devmem_phys + off) >> PAGE_SHIFT, size,
				  vma->vm_page_prot);
}

static const struct file_operations lacc_fops = {
	.owner = THIS_MODULE,
	.open = lacc_open,
	.release = lacc_release,
	.unlocked_ioctl = lacc_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.mmap = lacc_mmap,
};

/* ---- probe and remove ---------------------------------------------------------------------- */

static int lacc_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct lacc_dev *ld;
	u32 magic;
	int ret, nvec;

	ld = devm_kzalloc(dev, sizeof(*ld), GFP_KERNEL);
	if (!ld)
		return -ENOMEM;
	ld->pdev = pdev;
	pci_set_drvdata(pdev, ld);
	mutex_init(&ld->submit_lock);
	mutex_init(&ld->bus_lock);
	spin_lock_init(&ld->job_lock);
	xa_init(&ld->jobs);
	init_waitqueue_head(&ld->done_wq);
	init_waitqueue_head(&ld->space_wq);
	INIT_DELAYED_WORK(&ld->watchdog, lacc_watchdog);
	INIT_WORK(&ld->reset_work, lacc_reset_work);
	ld->hang_timeout_ms = 2000;
	ld->coalesce = 0;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	pci_set_master(pdev);
	ld->regs = pcim_iomap_region(pdev, 0, "lacc-regs");
	if (IS_ERR(ld->regs))
		return PTR_ERR(ld->regs);
	ret = pcim_request_region(pdev, 2, "lacc-devmem");
	if (ret)
		return ret;
	ld->devmem_phys = pci_resource_start(pdev, 2);
	ld->devmem_size = pci_resource_len(pdev, 2);

	magic = lacc_rd(ld, LACC_REG_ID);
	if (magic != LACC_ID_MAGIC) {
		dev_err(dev, "unexpected ID %#x (want %#x)\n", magic, LACC_ID_MAGIC);
		return -ENODEV;
	}
	ret = lacc_hw_reset(ld);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	/*
	 * Each DMA descriptor and scatter-gather entry carries a 32-bit length, so the card has no
	 * segment limit of its own. Without this the DMA layer assumes 64 KiB and DMA-API debug
	 * flags the larger segments sg_alloc_table_from_pages() builds from contiguous pages.
	 */
	dma_set_max_seg_size(dev, UINT_MAX);
	ld->sq = dmam_alloc_coherent(dev, LACC_SQ_ENTRIES * sizeof(*ld->sq), &ld->sq_dma,
				     GFP_KERNEL);
	ld->cq = dmam_alloc_coherent(dev, LACC_CQ_ENTRIES * sizeof(*ld->cq), &ld->cq_dma,
				     GFP_KERNEL);
	if (!ld->sq || !ld->cq)
		return -ENOMEM;
	ret = lacc_mem_init(&ld->mem, ld->devmem_size);
	if (ret)
		return ret;

	nvec = pci_alloc_irq_vectors(pdev, LACC_MSIX_VECTORS, LACC_MSIX_VECTORS, PCI_IRQ_MSIX);
	if (nvec < 0) {
		dev_err(dev, "MSI-X not available: %d\n", nvec);
		return nvec;
	}
	ret = devm_request_threaded_irq(dev, pci_irq_vector(pdev, 0), lacc_irq_cq,
					lacc_irq_cq_thread, IRQF_ONESHOT, "lacc-cq", ld);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(dev, pci_irq_vector(pdev, 1), NULL, lacc_irq_event_thread,
					IRQF_ONESHOT, "lacc-event", ld);
	if (ret)
		return ret;

	lacc_hw_start(ld);

	ld->id = ida_alloc(&lacc_ida, GFP_KERNEL);
	if (ld->id < 0)
		return ld->id;
	snprintf(ld->name, sizeof(ld->name), "lacc%d", ld->id);
	ld->misc.minor = MISC_DYNAMIC_MINOR;
	ld->misc.name = ld->name;
	ld->misc.fops = &lacc_fops;
	ld->misc.parent = dev;
	ld->misc.mode = 0660;
	ret = misc_register(&ld->misc);
	if (ret)
		goto err_ida;

	/* board buses: a failure here leaves the compute path working, and says so */
	ret = lacc_i2c_init(ld);
	if (ret)
		dev_warn(dev, "SMBus controller not registered: %d\n", ret);
	ret = lacc_spi_init(ld);
	if (ret)
		dev_warn(dev, "SPI controller not registered: %d\n", ret);

	lacc_debugfs_init(ld);
	schedule_delayed_work(&ld->watchdog, LACC_WD_PERIOD);
	dev_info(dev, "%s: hw %#x, firmware %u (status %#x), %llu MiB device memory\n", ld->name,
		 lacc_rd(ld, LACC_REG_HW_VERSION), lacc_rd(ld, LACC_REG_FW_VERSION),
		 lacc_rd(ld, LACC_REG_FW_STATUS), (unsigned long long)(ld->devmem_size >> 20));
	return 0;

err_ida:
	ida_free(&lacc_ida, ld->id);
	lacc_mem_fini(&ld->mem);
	return ret;
}

static void lacc_remove(struct pci_dev *pdev)
{
	struct lacc_dev *ld = pci_get_drvdata(pdev);

	WRITE_ONCE(ld->dead, true);
	cancel_delayed_work_sync(&ld->watchdog);
	cancel_work_sync(&ld->reset_work);
	misc_deregister(&ld->misc);
	lacc_spi_fini(ld);
	lacc_i2c_fini(ld);
	debugfs_remove_recursive(ld->debugfs);
	lacc_wr(ld, LACC_REG_IRQ_MASK, 0);
	lacc_wr(ld, LACC_REG_CONTROL, 0);
	synchronize_irq(pci_irq_vector(pdev, 0));
	lacc_process_cq(ld);
	lacc_abort_jobs(ld);
	ida_free(&lacc_ida, ld->id);
	xa_destroy(&ld->jobs);
	lacc_mem_fini(&ld->mem);
}

static const struct pci_device_id lacc_ids[] = { { PCI_DEVICE(LACC_VENDOR_ID, LACC_DEVICE_ID) },
						 {} };
MODULE_DEVICE_TABLE(pci, lacc_ids);

static struct pci_driver lacc_driver = {
	.name = "lacc",
	.id_table = lacc_ids,
	.probe = lacc_probe,
	.remove = lacc_remove,
	.dev_groups = lacc_groups,
};
module_pci_driver(lacc_driver);

MODULE_DESCRIPTION("LACC virtual inference card");
MODULE_AUTHOR("Manoj Kumar Prakash");
MODULE_LICENSE("GPL");
