/* SPDX-License-Identifier: GPL-2.0 */
/* LACC virtual inference card: driver-internal definitions. */
#ifndef _LACC_DRV_H
#define _LACC_DRV_H

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/spi/spi.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "lacc_hw.h"
#include "uapi/lacc.h"

#define LACC_SQ_ENTRIES 256
#define LACC_CQ_ENTRIES 512
#define LACC_BO_ALIGN 4096
#define LACC_MAX_JOBS 4096

/* One submitted job: one or more descriptors with the same tag. */
struct lacc_job {
	struct list_head link;
	u64 seq;
	u32 ndesc; /* descriptors still outstanding */
	u16 status; /* first non-zero completion status */
	u8 result; /* LACC_WAIT_* once done */
	bool hw_done; /* the card is finished with it (or it was aborted) */
	bool done; /* resources released; waiters may collect it */
	bool mapped;
	u64 submit_ns, done_ns;
	struct file *owner;
	/* resources released when the job completes */
	struct sg_table sgt;
	struct page **pages;
	unsigned int npages;
	enum dma_data_direction dir;
	void *sg_list;
	dma_addr_t sg_list_dma;
	size_t sg_list_size;
};

struct lacc_mem {
	struct mutex lock; /* protects free and used */
	struct list_head free; /* sorted by start */
	u64 size, used;
};

int lacc_mem_init(struct lacc_mem *mem, u64 size);
void lacc_mem_fini(struct lacc_mem *mem);
int lacc_mem_alloc(struct lacc_mem *mem, u64 size, u64 *offset);
void lacc_mem_free(struct lacc_mem *mem, u64 offset, u64 size);

struct lacc_stats {
	u64 submitted, completed, failed, aborted, irqs, cqes, bad_cqes, resets, link_downs,
		thermal_events;
	u64 desc_submitted;
};

struct lacc_dev {
	struct pci_dev *pdev;
	void __iomem *regs;
	resource_size_t devmem_phys;
	resource_size_t devmem_size;
	int id;
	char name[16];
	struct miscdevice misc;

	/* rings */
	struct lacc_desc *sq;
	dma_addr_t sq_dma;
	struct lacc_cqe *cq;
	dma_addr_t cq_dma;
	u32 sq_tail, sq_head_cached, cq_head;
	bool cq_phase;
	struct mutex submit_lock; /* serialises submitters and ring re-initialisation */
	spinlock_t job_lock; /* jobs xarray, completion state */
	struct xarray jobs; /* seq -> job */
	u64 next_seq;
	u64 completed_seq;
	u32 outstanding; /* descriptors submitted and not yet completed */
	u32 njobs; /* jobs not yet collected */
	u32 coalesce; /* IRQ_COALESCE value, kept across resets */
	wait_queue_head_t done_wq;
	wait_queue_head_t space_wq;
	bool dead; /* removed or unrecoverable */

	/* device memory allocator */
	struct lacc_mem mem;

	/* recovery */
	struct delayed_work watchdog;
	u64 wd_last_completed;
	unsigned long wd_stuck_since;
	bool link_lost;        /* watchdog: inside a link outage, counted once */
	atomic_t reset_queued; /* a recovery is queued or running */
	u32 hang_timeout_ms;
	struct work_struct reset_work;
	const char *reset_reason;
	char last_crash[2048];

	/* board buses */
	struct i2c_adapter i2c;
	struct i2c_client *pmbus, *eeprom;
	struct mutex bus_lock; /* SMBus and SPI controllers share the register file */
	struct spi_controller *spi;
	struct spi_device *flash;

	struct lacc_stats stats;
	struct dentry *debugfs;
};

static inline u32 lacc_rd(struct lacc_dev *ld, u32 reg)
{
	return readl(ld->regs + reg);
}

static inline void lacc_wr(struct lacc_dev *ld, u32 reg, u32 val)
{
	writel(val, ld->regs + reg);
}

static inline u64 lacc_dev_ns(struct lacc_dev *ld)
{
	u64 lo = lacc_rd(ld, LACC_REG_TIMESTAMP_LO);

	return lo | ((u64)lacc_rd(ld, LACC_REG_TIMESTAMP_HI) << 32);
}

/* lacc_i2c.c, lacc_spi.c */
int lacc_i2c_init(struct lacc_dev *ld);
void lacc_i2c_fini(struct lacc_dev *ld);
int lacc_spi_init(struct lacc_dev *ld);
void lacc_spi_fini(struct lacc_dev *ld);

/* lacc_sysfs.c */
extern const struct attribute_group *lacc_groups[];
void lacc_debugfs_init(struct lacc_dev *ld);
int lacc_fw_reboot(struct lacc_dev *ld);
void lacc_snapshot(struct lacc_dev *ld, const char *why);

/* lacc_main.c */
int lacc_hw_restart(struct lacc_dev *ld, const char *why);

#endif
