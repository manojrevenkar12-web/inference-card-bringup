// SPDX-License-Identifier: GPL-2.0
/*
 * sysfs: firmware, power and telemetry attributes on the PCI device, plus statistics.
 * debugfs: a register dump, ring state, the last crash snapshot and fault injection.
 */

#include <linux/debugfs.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>

#include "lacc.h"

static struct lacc_dev *to_lacc(struct device *dev)
{
	return pci_get_drvdata(to_pci_dev(dev));
}

#define LACC_RO_REG(_name, _reg, _fmt)                                                       \
	static ssize_t _name##_show(struct device *dev, struct device_attribute *a, char *b) \
	{                                                                                    \
		return sysfs_emit(b, _fmt "\n", lacc_rd(to_lacc(dev), _reg));                \
	}                                                                                    \
	static DEVICE_ATTR_RO(_name)

LACC_RO_REG(fw_version, LACC_REG_FW_VERSION, "%u");
LACC_RO_REG(fw_min_version, LACC_REG_FW_MIN_VERSION, "%u");
LACC_RO_REG(fw_status, LACC_REG_FW_STATUS, "%#x");
LACC_RO_REG(card_status, LACC_REG_STATUS, "%#x");
LACC_RO_REG(temp_mc, LACC_REG_TEMP_MC, "%u");
LACC_RO_REG(power_mw, LACC_REG_POWER_MW, "%u");
LACC_RO_REG(clock_mhz, LACC_REG_CLOCK_MHZ, "%u");

static ssize_t booted_slot_show(struct device *dev, struct device_attribute *a, char *b)
{
	u32 st = lacc_rd(to_lacc(dev), LACC_REG_FW_STATUS);

	return sysfs_emit(b, "%s\n",
			  st & LACC_FW_SLOT_A ? "a" :
			  st & LACC_FW_SLOT_B ? "b" :
						"none");
}
static DEVICE_ATTR_RO(booted_slot);

static ssize_t throttled_show(struct device *dev, struct device_attribute *a, char *b)
{
	return sysfs_emit(b, "%u\n",
			  !!(lacc_rd(to_lacc(dev), LACC_REG_STATUS) & LACC_STATUS_THROTTLED));
}
static DEVICE_ATTR_RO(throttled);

/* 64-bit counters: reading LO latches HI, so a pair must not interleave with another reader */
static DEFINE_MUTEX(lacc_latch_lock);

static u64 lacc_rd64_latched(struct lacc_dev *ld, u32 lo_reg, u32 hi_reg)
{
	u64 v;

	mutex_lock(&lacc_latch_lock);
	v = lacc_rd(ld, lo_reg);
	v |= (u64)lacc_rd(ld, hi_reg) << 32;
	mutex_unlock(&lacc_latch_lock);
	return v;
}

static ssize_t energy_uj_show(struct device *dev, struct device_attribute *a, char *b)
{
	return sysfs_emit(b, "%llu\n",
			  lacc_rd64_latched(to_lacc(dev), LACC_REG_ENERGY_UJ_LO,
					    LACC_REG_ENERGY_UJ_HI));
}
static DEVICE_ATTR_RO(energy_uj);

static ssize_t active_energy_uj_show(struct device *dev, struct device_attribute *a, char *b)
{
	return sysfs_emit(b, "%llu\n",
			  lacc_rd64_latched(to_lacc(dev), LACC_REG_ACTIVE_ENERGY_UJ_LO,
					    LACC_REG_ACTIVE_ENERGY_UJ_HI));
}
static DEVICE_ATTR_RO(active_energy_uj);

static ssize_t power_state_show(struct device *dev, struct device_attribute *a, char *b)
{
	return sysfs_emit(b, "%u\n", lacc_rd(to_lacc(dev), LACC_REG_POWER_STATE));
}

static ssize_t power_state_store(struct device *dev, struct device_attribute *a, const char *b,
				 size_t n)
{
	u32 v;

	if (kstrtou32(b, 0, &v) || v > 2)
		return -EINVAL;
	lacc_wr(to_lacc(dev), LACC_REG_POWER_STATE, v);
	return n;
}
static DEVICE_ATTR_RW(power_state);

static ssize_t power_cap_mw_show(struct device *dev, struct device_attribute *a, char *b)
{
	return sysfs_emit(b, "%u\n", lacc_rd(to_lacc(dev), LACC_REG_POWER_CAP_MW));
}

static ssize_t power_cap_mw_store(struct device *dev, struct device_attribute *a, const char *b,
				  size_t n)
{
	u32 v;

	if (kstrtou32(b, 0, &v))
		return -EINVAL;
	lacc_wr(to_lacc(dev), LACC_REG_POWER_CAP_MW, v);
	return n;
}
static DEVICE_ATTR_RW(power_cap_mw);

static ssize_t coalesce_show(struct device *dev, struct device_attribute *a, char *b)
{
	struct lacc_dev *ld = to_lacc(dev);

	return sysfs_emit(b, "%u %u\n", ld->coalesce & 0xffff, ld->coalesce >> 16);
}

/* "<completions per interrupt> <max delay in us>" */
static ssize_t coalesce_store(struct device *dev, struct device_attribute *a, const char *b,
			      size_t n)
{
	struct lacc_dev *ld = to_lacc(dev);
	u32 count, us;

	if (sscanf(b, "%u %u", &count, &us) != 2 || count > 0xffff || us > 0xffff)
		return -EINVAL;
	ld->coalesce = count | (us << 16);
	lacc_wr(ld, LACC_REG_IRQ_COALESCE, ld->coalesce);
	return n;
}
static DEVICE_ATTR_RW(coalesce);

static ssize_t hang_timeout_ms_show(struct device *dev, struct device_attribute *a, char *b)
{
	return sysfs_emit(b, "%u\n", to_lacc(dev)->hang_timeout_ms);
}

static ssize_t hang_timeout_ms_store(struct device *dev, struct device_attribute *a, const char *b,
				     size_t n)
{
	u32 v;

	if (kstrtou32(b, 0, &v) || v < 100)
		return -EINVAL;
	to_lacc(dev)->hang_timeout_ms = v;
	return n;
}
static DEVICE_ATTR_RW(hang_timeout_ms);

/* Re-run the boot ROM (after a firmware update): quiesce, reboot, re-initialise. */
int lacc_fw_reboot(struct lacc_dev *ld)
{
	int ret;

	mutex_lock(&ld->submit_lock);
	if (ld->outstanding) {
		mutex_unlock(&ld->submit_lock);
		return -EBUSY;
	}
	lacc_wr(ld, LACC_REG_FW_CONTROL, LACC_FW_REBOOT);
	mutex_unlock(&ld->submit_lock);
	ret = lacc_hw_restart(ld, "firmware reboot requested");
	if (!ret)
		dev_info(&ld->pdev->dev, "firmware %u running from slot %s\n",
			 lacc_rd(ld, LACC_REG_FW_VERSION),
			 lacc_rd(ld, LACC_REG_FW_STATUS) & LACC_FW_SLOT_A ? "A" : "B");
	return ret;
}

static ssize_t fw_reboot_store(struct device *dev, struct device_attribute *a, const char *b,
			       size_t n)
{
	bool v;
	int ret;

	if (kstrtobool(b, &v) || !v)
		return -EINVAL;
	ret = lacc_fw_reboot(to_lacc(dev));
	return ret ? ret : n;
}
static DEVICE_ATTR_WO(fw_reboot);

static ssize_t stats_show(struct device *dev, struct device_attribute *a, char *b)
{
	struct lacc_stats *s = &to_lacc(dev)->stats;

	return sysfs_emit(b,
			  "submitted %llu\ncompleted %llu\nfailed %llu\naborted %llu\n"
			  "descriptors %llu\nirqs %llu\ncqes %llu\nbad_cqes %llu\nresets %llu\n"
			  "link_downs %llu\nthermal_events %llu\n",
			  s->submitted, s->completed, s->failed, s->aborted, s->desc_submitted,
			  s->irqs, s->cqes, s->bad_cqes, s->resets, s->link_downs,
			  s->thermal_events);
}
static DEVICE_ATTR_RO(stats);

/* the card's own counters, in its own time base (modelled device time) */
static ssize_t perf_show(struct device *dev, struct device_attribute *a, char *b)
{
	struct lacc_dev *ld = to_lacc(dev);

	return sysfs_emit(b, "cmds %u\ndma_kb %u\nmmacs %u\nbusy_us %u\nirqs %u\n",
			  lacc_rd(ld, LACC_REG_PERF_CMDS), lacc_rd(ld, LACC_REG_PERF_DMA_KB),
			  lacc_rd(ld, LACC_REG_PERF_MMACS), lacc_rd(ld, LACC_REG_PERF_BUSY_US),
			  lacc_rd(ld, LACC_REG_PERF_IRQS));
}
static DEVICE_ATTR_RO(perf);

static struct attribute *lacc_attrs[] = {
	&dev_attr_fw_version.attr,
	&dev_attr_fw_min_version.attr,
	&dev_attr_fw_status.attr,
	&dev_attr_booted_slot.attr,
	&dev_attr_fw_reboot.attr,
	&dev_attr_card_status.attr,
	&dev_attr_temp_mc.attr,
	&dev_attr_power_mw.attr,
	&dev_attr_clock_mhz.attr,
	&dev_attr_energy_uj.attr,
	&dev_attr_active_energy_uj.attr,
	&dev_attr_throttled.attr,
	&dev_attr_power_state.attr,
	&dev_attr_power_cap_mw.attr,
	&dev_attr_coalesce.attr,
	&dev_attr_hang_timeout_ms.attr,
	&dev_attr_stats.attr,
	&dev_attr_perf.attr,
	NULL,
};

static const struct attribute_group lacc_group = {
	.name = "lacc",
	.attrs = lacc_attrs,
};

const struct attribute_group *lacc_groups[] = {
	&lacc_group,
	NULL,
};

/* ---- debugfs ------------------------------------------------------------------------------- */

static const struct {
	const char *name;
	u32 reg;
} lacc_dump_regs[] = {
	{ "ID", LACC_REG_ID },
	{ "HW_VERSION", LACC_REG_HW_VERSION },
	{ "STATUS", LACC_REG_STATUS },
	{ "CONTROL", LACC_REG_CONTROL },
	{ "SQ_SIZE", LACC_REG_SQ_SIZE },
	{ "SQ_TAIL", LACC_REG_SQ_TAIL },
	{ "SQ_HEAD", LACC_REG_SQ_HEAD },
	{ "CQ_SIZE", LACC_REG_CQ_SIZE },
	{ "CQ_HEAD", LACC_REG_CQ_HEAD },
	{ "CQ_TAIL", LACC_REG_CQ_TAIL },
	{ "IRQ_STATUS", LACC_REG_IRQ_STATUS },
	{ "IRQ_MASK", LACC_REG_IRQ_MASK },
	{ "IRQ_COALESCE", LACC_REG_IRQ_COALESCE },
	{ "FW_STATUS", LACC_REG_FW_STATUS },
	{ "FW_VERSION", LACC_REG_FW_VERSION },
	{ "FW_MIN_VERSION", LACC_REG_FW_MIN_VERSION },
	{ "POWER_STATE", LACC_REG_POWER_STATE },
	{ "POWER_CAP_MW", LACC_REG_POWER_CAP_MW },
	{ "TEMP_MC", LACC_REG_TEMP_MC },
	{ "CLOCK_MHZ", LACC_REG_CLOCK_MHZ },
	{ "POWER_MW", LACC_REG_POWER_MW },
	{ "PERF_CMDS", LACC_REG_PERF_CMDS },
	{ "PERF_DMA_KB", LACC_REG_PERF_DMA_KB },
	{ "PERF_MMACS", LACC_REG_PERF_MMACS },
	{ "PERF_BUSY_US", LACC_REG_PERF_BUSY_US },
	{ "PERF_IRQS", LACC_REG_PERF_IRQS },
	{ "ERR_CODE", LACC_REG_ERR_CODE },
};

static size_t lacc_dump(struct lacc_dev *ld, char *buf, size_t size)
{
	size_t n = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(lacc_dump_regs) && n < size; i++)
		n += scnprintf(buf + n, size - n, "%-15s %#010x\n", lacc_dump_regs[i].name,
			       lacc_rd(ld, lacc_dump_regs[i].reg));
	return n;
}

/* Keep a register snapshot from the moment something went wrong (like a devcoredump). */
void lacc_snapshot(struct lacc_dev *ld, const char *why)
{
	size_t n = scnprintf(ld->last_crash, sizeof(ld->last_crash),
			     "reason: %s\nsq_tail(host) %u cq_head(host) %u outstanding %u\n", why,
			     ld->sq_tail, ld->cq_head, ld->outstanding);

	lacc_dump(ld, ld->last_crash + n, sizeof(ld->last_crash) - n);
}

static int regs_show(struct seq_file *m, void *v)
{
	struct lacc_dev *ld = m->private;
	int i;

	for (i = 0; i < ARRAY_SIZE(lacc_dump_regs); i++)
		seq_printf(m, "%-15s %#010x\n", lacc_dump_regs[i].name,
			   lacc_rd(ld, lacc_dump_regs[i].reg));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regs);

static int crash_show(struct seq_file *m, void *v)
{
	struct lacc_dev *ld = m->private;

	seq_puts(m, ld->last_crash[0] ? ld->last_crash : "none\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(crash);

static int fault_set(void *data, u64 val)
{
	struct lacc_dev *ld = data;

	lacc_wr(ld, LACC_REG_FAULT_INJECT, (u32)val);
	return 0;
}

static int fault_get(void *data, u64 *val)
{
	*val = lacc_rd(data, LACC_REG_FAULT_INJECT);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fault_fops, fault_get, fault_set, "%#llx\n");

void lacc_debugfs_init(struct lacc_dev *ld)
{
	ld->debugfs = debugfs_create_dir(ld->name, NULL);
	debugfs_create_file("regs", 0400, ld->debugfs, ld, &regs_fops);
	debugfs_create_file("last_crash", 0400, ld->debugfs, ld, &crash_fops);
	debugfs_create_file_unsafe("fault_inject", 0600, ld->debugfs, ld, &fault_fops);
	debugfs_create_u32("outstanding", 0400, ld->debugfs, &ld->outstanding);
	debugfs_create_u32("sq_tail", 0400, ld->debugfs, &ld->sq_tail);
	debugfs_create_u32("cq_head", 0400, ld->debugfs, &ld->cq_head);
}
