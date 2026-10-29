/*
 * LACC: a virtual PCIe AI inference card for platform-software bring-up.
 *
 * What it models, all behind one PCIe endpoint (1234:7acc):
 *  - BAR0 registers (generated from regs/lacc.yaml), BAR2 device memory, BAR4 MSI-X.
 *  - A command engine fed by a submission ring in host memory, completing into a completion
 *    ring with a phase bit: copies (contiguous and scatter-gather), fills, INT8 GEMM and an
 *    LNS8 GEMM whose products are additions of logarithms (include/lacc_hw.h).
 *  - Interrupt coalescing, a power and thermal model with a power cap and clock throttling,
 *    and an energy counter.
 *  - Board parts on its own buses: an SMBus controller with a PMBus voltage regulator (0x40)
 *    and a FRU EEPROM (0x50); a SPI controller with a W25Q128 NOR flash holding A/B firmware
 *    images, which a boot ROM verifies (CRC-32, SHA-256, anti-rollback) at every reset.
 *  - A JTAG TAP reachable over OpenOCD's remote_bitbang protocol on a chardev, with a debug
 *    access port that reads registers even while the PCIe link is down.
 *  - Fault injection: DMA errors, hangs, thermal events, link down, bad completions.
 *
 * Timing is modelled on the virtual clock; computation is done for real so results can be
 * checked bit-exactly against the reference in include/lacc_hw.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "hw/pci/pcie.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "chardev/char-fe.h"
#include "crypto/hash.h"
#include "qom/object.h"
#include <math.h>
#include <zlib.h>

#include "lacc_hw.h"

#define TYPE_LACC "lacc"
OBJECT_DECLARE_SIMPLE_TYPE(LaccState, LACC)

#define MSIX_VEC_COMPLETION 0
#define MSIX_VEC_EVENT      1
#define MAX_RING            4096
#define MAX_GEMM_MACS       (1ull << 28)
#define POWER_TICK_NS       (10 * SCALE_MS)
#define RESET_NS            (1 * SCALE_MS)
#define LINK_DOWN_NS        (500 * SCALE_MS)
#define PMBUS_ADDR          0x40
#define EEPROM_ADDR         0x50

enum tap_state {
    TAP_RESET, TAP_IDLE, TAP_SELECT_DR, TAP_CAPTURE_DR, TAP_SHIFT_DR, TAP_EXIT1_DR,
    TAP_PAUSE_DR, TAP_EXIT2_DR, TAP_UPDATE_DR, TAP_SELECT_IR, TAP_CAPTURE_IR, TAP_SHIFT_IR,
    TAP_EXIT1_IR, TAP_PAUSE_IR, TAP_EXIT2_IR, TAP_UPDATE_IR,
};

#define JTAG_IR_LEN      4
#define JTAG_IDCODE      0x1
#define JTAG_DBG_ADDR    0x2
#define JTAG_DBG_DATA    0x3
#define JTAG_BYPASS      0xF

struct LaccState {
    PCIDevice pdev;
    MemoryRegion mmio;
    MemoryRegion devmem;
    uint8_t *mem;

    /* properties */
    char *flash_path;
    char *serial;
    CharBackend jtag;

    /* control and status */
    uint32_t scratch;
    uint32_t control;
    uint32_t err_code;
    bool ready;
    bool halted;
    bool error;
    int64_t time_base;
    uint32_t ts_hi_latch;

    /* rings */
    uint64_t sq_base, cq_base;
    uint32_t sq_size, sq_tail, sq_head;
    uint32_t cq_size, cq_head, cq_tail;
    bool cq_phase;
    bool inflight;
    struct lacc_desc cur;
    uint16_t cur_status;
    QEMUTimer *engine_timer;
    QEMUTimer *reset_timer;
    uint32_t fault;
    int64_t link_down_until;

    /* interrupts */
    uint32_t irq_status, irq_mask, coalesce;
    uint32_t pending_cqes;
    QEMUTimer *coalesce_timer;

    /* firmware */
    uint32_t fw_status, fw_version, fw_min_version, otp_min_version;
    bool fw_ok;
    uint32_t lut[8];
    uint32_t tdp_mw, idle_mw, cap_default_mw;
    uint32_t throttle_mc, critical_mc;
    uint32_t clocks[3];

    /* power and thermal */
    uint32_t power_state, power_cap_mw, clock_mhz, power_mw;
    bool throttled;
    double temp_c;
    uint32_t limit_mhz;           /* clock ceiling set by the power/thermal controller */
    double energy_uj;             /* board energy: integral of board power */
    double active_energy_uj;      /* modelled energy of the work alone */
    double win_dyn_pj, win_model_ns, p_active_mw;
    int64_t busy_since, busy_acc_ns;
    int64_t last_tick;
    uint32_t energy_hi_latch, active_hi_latch;
    QEMUTimer *power_timer;

    /* performance counters */
    uint64_t perf_cmds, perf_dma_bytes, perf_macs, perf_busy_ns, perf_irqs;

    /* SMBus */
    uint32_t i2c_ctrl, i2c_cmd, i2c_len, i2c_status;
    uint8_t i2c_data[64];
    uint8_t eeprom[256];

    /* SPI and the NOR flash */
    uint32_t spi_ctrl, spi_len, spi_status;
    uint8_t spi_tx[256], spi_rx[256];
    uint8_t *flash;
    bool cs;
    uint8_t fcmd;
    uint32_t fpos, faddr;
    uint8_t sr1, sr2;
    bool wel;
    bool flash_dirty;

    /* JTAG */
    enum tap_state tap;
    uint32_t ir, ir_shift;
    uint64_t dr_shift;
    int dr_len;
    uint32_t dbg_addr;
    bool tck, tms, tdi;
};

static uint32_t lacc_reg_read(LaccState *s, hwaddr addr);
static void lacc_reg_write(LaccState *s, hwaddr addr, uint32_t val);
static void lacc_engine_kick(LaccState *s);
static void lacc_fw_boot(LaccState *s);

static int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint64_t dev_ns(LaccState *s)
{
    return now_ns() - s->time_base;
}

/* ---- interrupts --------------------------------------------------------------------------- */

static void lacc_raise(LaccState *s, uint32_t cause)
{
    unsigned vec = cause == LACC_IRQ_COMPLETION ? MSIX_VEC_COMPLETION : MSIX_VEC_EVENT;

    s->irq_status |= cause;
    if (!(s->irq_mask & cause) || now_ns() < s->link_down_until) {
        return;
    }
    if (msix_enabled(&s->pdev)) {
        msix_notify(&s->pdev, vec);
        s->perf_irqs++;
    }
}

static void lacc_coalesce_fire(void *opaque)
{
    LaccState *s = opaque;

    if (s->pending_cqes) {
        s->pending_cqes = 0;
        lacc_raise(s, LACC_IRQ_COMPLETION);
    }
}

static void lacc_completion_irq(LaccState *s)
{
    uint32_t count = s->coalesce & 0xffff;
    uint32_t delay_us = s->coalesce >> 16;

    s->pending_cqes++;
    if (!count || s->pending_cqes >= count || !delay_us) {
        timer_del(s->coalesce_timer);
        lacc_coalesce_fire(s);
    } else if (!timer_pending(s->coalesce_timer)) {
        timer_mod(s->coalesce_timer, now_ns() + delay_us * SCALE_US);
    }
}

/* ---- power and thermal model --------------------------------------------------------------
 * The card's work takes modelled time (descriptor fetch, DMA at 16 GB/s, 16384 MACs per clock)
 * and modelled energy (per MAC and per byte, scaled with the clock because a lower clock runs
 * at a lower voltage). Under emulation the host needs far longer than the modelled time to do
 * the arithmetic, so power is not energy over wall time. Instead:
 *
 *   board power = idle(clock) + activity x active power
 *
 * where activity is the fraction of wall time the engine had work, and active power is the
 * modelled energy of that work over its modelled duration. A queue kept full reads as a fully
 * busy card; an idle card reads as idle. The energy counter integrates board power; a second
 * counter holds the modelled energy of the work alone, for comparing datapaths.
 *
 * Every 10 ms the controller steps the clock ceiling down by 100 MHz while board power is over
 * the cap or the die over the throttle temperature, and back up by 50 MHz once both are
 * comfortably below (power under 90% of the cap, die 5 C under the throttle point). The die
 * follows a first-order thermal model. Constants are model assumptions (docs/MODEL.md).
 */

#define PJ_PER_MAC_I8     2.0
#define PJ_PER_MAC_LNS8   0.8
#define PJ_PER_BYTE_DMA   60.0
#define MACS_PER_CLOCK    16384.0
#define STEP_DOWN_MHZ     100
#define STEP_UP_MHZ       50
#define AMBIENT_C         35.0
#define R_THERMAL_C_PER_W 0.18
#define TAU_S             1.5

static double clock_scale(LaccState *s)
{
    return (double)s->clock_mhz / (double)(s->clocks[0] ? s->clocks[0] : 1500);
}

static void lacc_add_work(LaccState *s, double pj, uint64_t ns)
{
    s->active_energy_uj += pj * 1e-6;
    s->win_dyn_pj += pj;
    s->win_model_ns += ns;
}

static void lacc_set_clock(LaccState *s)
{
    uint32_t want = s->clocks[MIN(s->power_state, 2u)];

    s->limit_mhz = MAX(s->limit_mhz, s->clocks[2]);
    s->clock_mhz = MIN(want, s->limit_mhz);
    s->throttled = s->clock_mhz < want;
}

/* engine occupancy, in wall time */
static void lacc_busy(LaccState *s, bool busy)
{
    int64_t t = now_ns();

    if (busy && s->busy_since < 0) {
        s->busy_since = t;
    } else if (!busy && s->busy_since >= 0) {
        s->busy_acc_ns += t - s->busy_since;
        s->busy_since = -1;
    }
}

static void lacc_power_tick(void *opaque)
{
    LaccState *s = opaque;
    int64_t t = now_ns();
    double dt = (t - s->last_tick) * 1e-9, activity, idle_mw, avg_mw, target_c;
    uint32_t cap = s->power_cap_mw ? s->power_cap_mw : s->cap_default_mw;
    bool was = s->throttled;

    if (dt <= 0) {
        timer_mod(s->power_timer, t + POWER_TICK_NS);
        return;
    }
    if (s->busy_since >= 0) {
        s->busy_acc_ns += t - s->busy_since;
        s->busy_since = t;
    }
    activity = MIN(1.0, s->busy_acc_ns * 1e-9 / dt);
    s->busy_acc_ns = 0;
    if (s->win_model_ns > 0) {
        s->p_active_mw = s->win_dyn_pj / s->win_model_ns;   /* pJ per ns is mW */
    }
    s->win_dyn_pj = s->win_model_ns = 0;
    idle_mw = s->idle_mw * (0.6 + 0.4 * clock_scale(s));
    avg_mw = idle_mw + activity * s->p_active_mw;
    s->energy_uj += avg_mw * dt * 1e3;
    s->power_mw = (uint32_t)avg_mw;
    target_c = AMBIENT_C + R_THERMAL_C_PER_W * avg_mw * 1e-3;
    s->temp_c += (target_c - s->temp_c) * MIN(1.0, dt / TAU_S);
    if (avg_mw > cap || s->temp_c * 1000 > s->throttle_mc) {
        s->limit_mhz = s->clock_mhz > s->clocks[2] + STEP_DOWN_MHZ ? s->clock_mhz - STEP_DOWN_MHZ : s->clocks[2];
    } else if (avg_mw < 0.9 * cap && s->temp_c * 1000 < s->throttle_mc - 5000) {
        s->limit_mhz = MIN(MIN(s->limit_mhz, s->clocks[0]) + STEP_UP_MHZ, s->clocks[0]);
    }
    lacc_set_clock(s);
    if (s->throttled && !was) {
        lacc_raise(s, LACC_IRQ_THERMAL);
    }
    s->last_tick = t;
    timer_mod(s->power_timer, t + POWER_TICK_NS);
}

/* ---- firmware: flash and the boot ROM ----------------------------------------------------- */

static void lacc_flash_persist(LaccState *s, uint32_t off, uint32_t len)
{
    int fd;

    if (!s->flash_path) {
        return;
    }
    fd = open(s->flash_path, O_WRONLY);
    if (fd < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "lacc: cannot write %s\n", s->flash_path);
        return;
    }
    if (pwrite(fd, s->flash + off, len, off) != (ssize_t)len) {
        qemu_log_mask(LOG_GUEST_ERROR, "lacc: short write to %s\n", s->flash_path);
    }
    close(fd);
}

static uint32_t cfg_crc(const struct lacc_flash_config *c)
{
    return crc32(0, (const Bytef *)c, offsetof(struct lacc_flash_config, crc32));
}

static void lacc_cfg_read(LaccState *s, struct lacc_flash_config *c)
{
    memcpy(c, s->flash + LACC_FLASH_CONFIG, sizeof(*c));
    if (le32_to_cpu(c->magic) != LACC_CFG_MAGIC || le32_to_cpu(c->crc32) != cfg_crc(c)) {
        memset(c, 0, sizeof(*c));
    } else {
        c->boot_slot = le32_to_cpu(c->boot_slot);
        c->min_version = le32_to_cpu(c->min_version);
    }
}

static void lacc_cfg_write_min_version(LaccState *s, uint32_t min_version)
{
    struct lacc_flash_config c;
    struct lacc_flash_config w;

    lacc_cfg_read(s, &c);
    w.magic = cpu_to_le32(LACC_CFG_MAGIC);
    w.boot_slot = cpu_to_le32(c.boot_slot);
    w.min_version = cpu_to_le32(min_version);
    w.crc32 = 0;
    w.crc32 = cpu_to_le32(cfg_crc(&w));
    /* the boot ROM owns this sector: erase and program it like the flash would */
    memset(s->flash + LACC_FLASH_CONFIG, 0xff, LACC_FLASH_CONFIG_SIZE);
    memcpy(s->flash + LACC_FLASH_CONFIG, &w, sizeof(w));
    lacc_flash_persist(s, LACC_FLASH_CONFIG, LACC_FLASH_CONFIG_SIZE);
}

typedef struct FwImage {
    bool valid;
    bool rollback;
    uint32_t version, rollback_index;
    const uint8_t *payload;
    uint32_t payload_len;
} FwImage;

static void lacc_fw_check(LaccState *s, uint32_t base, uint32_t min_version, FwImage *img)
{
    struct lacc_fw_header h;
    uint32_t crc;
    uint8_t *digest = NULL;
    size_t dlen = 0;

    memset(img, 0, sizeof(*img));
    memcpy(&h, s->flash + base, sizeof(h));
    if (le32_to_cpu(h.magic) != LACC_FW_MAGIC) {
        return;
    }
    crc = le32_to_cpu(h.header_crc32);
    h.header_crc32 = 0;
    if (crc32(0, (const Bytef *)&h, sizeof(h)) != crc) {
        return;
    }
    img->payload_len = le32_to_cpu(h.payload_len);
    if (img->payload_len > LACC_FLASH_SLOT_SIZE - sizeof(h)) {
        return;
    }
    img->payload = s->flash + base + sizeof(h);
    if (qcrypto_hash_bytes(QCRYPTO_HASH_ALGO_SHA256, (const char *)img->payload, img->payload_len,
                           &digest, &dlen, NULL) < 0 || dlen != 32 || memcmp(digest, h.sha256, 32)) {
        g_free(digest);
        return;
    }
    g_free(digest);
    img->version = le32_to_cpu(h.version);
    img->rollback_index = le32_to_cpu(h.rollback_index);
    img->rollback = img->rollback_index < min_version;
    img->valid = !img->rollback;
}

static bool lacc_fw_apply(LaccState *s, const FwImage *img)
{
    uint32_t off = 0;
    bool lut = false;

    while (off + sizeof(struct lacc_tlv) <= img->payload_len) {
        struct lacc_tlv t;
        const uint8_t *v;
        uint16_t type, len;

        memcpy(&t, img->payload + off, sizeof(t));
        type = le16_to_cpu(t.type);
        len = le16_to_cpu(t.len);
        v = img->payload + off + sizeof(t);
        if (off + sizeof(t) + len > img->payload_len) {
            return false;
        }
        switch (type) {
        case LACC_TLV_LNS_LUT:
            if (len == 32) {
                for (int i = 0; i < 8; i++) {
                    s->lut[i] = ldl_le_p(v + 4 * i);
                }
                lut = true;
            }
            break;
        case LACC_TLV_POWER:
            if (len == 12) {
                s->tdp_mw = ldl_le_p(v);
                s->idle_mw = ldl_le_p(v + 4);
                s->cap_default_mw = ldl_le_p(v + 8);
            }
            break;
        case LACC_TLV_THERMAL:
            if (len == 8) {
                s->throttle_mc = ldl_le_p(v);
                s->critical_mc = ldl_le_p(v + 4);
            }
            break;
        case LACC_TLV_CLOCKS:
            if (len == 12) {
                for (int i = 0; i < 3; i++) {
                    s->clocks[i] = ldl_le_p(v + 4 * i);
                }
            }
            break;
        default:
            break;
        }
        off += sizeof(t) + ((len + 3) & ~3u);
    }
    return lut;
}

static void lacc_fw_defaults(LaccState *s)
{
    memset(s->lut, 0, sizeof(s->lut));
    s->tdp_mw = 75000;
    s->idle_mw = 18000;
    s->cap_default_mw = 75000;
    s->throttle_mc = 95000;
    s->critical_mc = 105000;
    s->clocks[0] = 1500;
    s->clocks[1] = 1000;
    s->clocks[2] = 400;
}

/* The boot ROM: verify both slots, pick one, load its tables, raise the rollback floor. */
static void lacc_fw_boot(LaccState *s)
{
    struct lacc_flash_config cfg;
    FwImage a, b;
    const FwImage *pick = NULL;
    uint32_t stored;
    int slot = 0;

    lacc_fw_defaults(s);
    lacc_cfg_read(s, &cfg);
    /*
     * The anti-rollback floor lives in one-time-programmable fuses; the config sector only
     * mirrors it for the host to read. Rewriting the sector cannot lower it. (Modelled as
     * device state that survives resets for the life of the VM.)
     */
    stored = cfg.min_version;
    cfg.min_version = MAX(cfg.min_version, s->otp_min_version);
    lacc_fw_check(s, LACC_FLASH_SLOT_A, cfg.min_version, &a);
    lacc_fw_check(s, LACC_FLASH_SLOT_B, cfg.min_version, &b);
    s->fw_status = (a.valid ? LACC_FW_A_VALID : 0) | (b.valid ? LACC_FW_B_VALID : 0) |
                   (a.rollback ? LACC_FW_A_ROLLBACK : 0) | (b.rollback ? LACC_FW_B_ROLLBACK : 0);
    if (cfg.boot_slot == 1 && a.valid) {
        pick = &a, slot = 1;
    } else if (cfg.boot_slot == 2 && b.valid) {
        pick = &b, slot = 2;
    } else if (a.valid && (!b.valid || a.version >= b.version)) {
        pick = &a, slot = 1;
    } else if (b.valid) {
        pick = &b, slot = 2;
    }
    s->fw_ok = false;
    s->fw_version = 0;
    if (pick && lacc_fw_apply(s, pick)) {
        s->fw_ok = true;
        s->fw_version = pick->version;
        s->fw_status |= (slot == 1 ? LACC_FW_SLOT_A : LACC_FW_SLOT_B) | LACC_FW_LUT_LOADED;
        cfg.min_version = MAX(cfg.min_version, pick->rollback_index);
    } else {
        lacc_fw_defaults(s);
    }
    if (cfg.min_version != stored) {
        lacc_cfg_write_min_version(s, cfg.min_version);
    }
    s->otp_min_version = cfg.min_version;
    s->fw_min_version = cfg.min_version;
    lacc_set_clock(s);
}

/* ---- the command engine ------------------------------------------------------------------- */

static bool dev_range_ok(LaccState *s, uint64_t off, uint64_t len)
{
    uint64_t size = memory_region_size(&s->devmem);

    return off <= size && len <= size - off;
}

static uint16_t do_copy_h2d(LaccState *s, uint64_t host, uint64_t dev, uint32_t len)
{
    if (!dev_range_ok(s, dev, len)) {
        return LACC_CQE_STATUS_RANGE;
    }
    if (pci_dma_read(&s->pdev, host, s->mem + dev, len) != MEMTX_OK) {
        return LACC_CQE_STATUS_DMA_ERROR;
    }
    s->perf_dma_bytes += len;
    return LACC_CQE_STATUS_OK;
}

static uint16_t do_copy_d2h(LaccState *s, uint64_t dev, uint64_t host, uint32_t len)
{
    if (!dev_range_ok(s, dev, len)) {
        return LACC_CQE_STATUS_RANGE;
    }
    if (pci_dma_write(&s->pdev, host, s->mem + dev, len) != MEMTX_OK) {
        return LACC_CQE_STATUS_DMA_ERROR;
    }
    s->perf_dma_bytes += len;
    return LACC_CQE_STATUS_OK;
}

static uint16_t do_sg(LaccState *s, const struct lacc_desc *d, bool h2d, uint64_t *bytes)
{
    uint64_t list = h2d ? d->addr0 : d->addr1;
    uint64_t dev = h2d ? d->addr1 : d->addr0;
    uint32_t n = d->len;

    if (!n || n > 4096) {
        return LACC_CQE_STATUS_BAD_DESC;
    }
    for (uint32_t i = 0; i < n; i++) {
        struct lacc_sg e;
        uint16_t st;

        if (pci_dma_read(&s->pdev, list + i * sizeof(e), &e, sizeof(e)) != MEMTX_OK) {
            return LACC_CQE_STATUS_DMA_ERROR;
        }
        e.addr = le64_to_cpu(e.addr);
        e.len = le32_to_cpu(e.len);
        st = h2d ? do_copy_h2d(s, e.addr, dev, e.len) : do_copy_d2h(s, dev, e.addr, e.len);
        if (st) {
            return st;
        }
        dev += e.len;
        *bytes += e.len;
    }
    return LACC_CQE_STATUS_OK;
}

static uint16_t do_gemm(LaccState *s, const struct lacc_desc *d, bool lns, uint64_t *macs)
{
    uint64_t m = d->m, n = d->n, k = d->k;
    uint64_t a_len = m * k, b_len = k * n, c_len = m * n * 4;

    if (!m || !n || !k || m * n * k > MAX_GEMM_MACS) {
        return LACC_CQE_STATUS_BAD_DESC;
    }
    if (!dev_range_ok(s, d->addr0, a_len) || !dev_range_ok(s, d->addr1, b_len) ||
        !dev_range_ok(s, d->addr2, c_len)) {
        return LACC_CQE_STATUS_RANGE;
    }
    if (lns) {
        if (!s->fw_ok) {
            return LACC_CQE_STATUS_NO_FIRMWARE;
        }
        lacc_gemm_lns8(s->mem + d->addr0, s->mem + d->addr1, (lacc_s32 *)(s->mem + d->addr2), m, n, k,
                       s->lut);
    } else {
        lacc_gemm_i8((const signed char *)(s->mem + d->addr0), (const signed char *)(s->mem + d->addr1),
                     (lacc_s32 *)(s->mem + d->addr2), m, n, k);
    }
    *macs = m * n * k;
    s->perf_macs += *macs;
    return LACC_CQE_STATUS_OK;
}

/* Execute a descriptor now and return how long the hardware would take, in ns. */
static uint64_t lacc_execute(LaccState *s, struct lacc_desc *d, uint16_t *status)
{
    uint64_t bytes = 0, macs = 0;
    double mhz = s->clock_mhz ? s->clock_mhz : 1;
    uint64_t ns = 200;            /* descriptor fetch and decode */

    *status = LACC_CQE_STATUS_OK;
    switch (d->opcode) {
    case LACC_OPCODE_NOP:
        break;
    case LACC_OPCODE_H2D:
        *status = do_copy_h2d(s, d->addr0, d->addr1, d->len);
        bytes = d->len;
        break;
    case LACC_OPCODE_D2H:
        *status = do_copy_d2h(s, d->addr0, d->addr1, d->len);
        bytes = d->len;
        break;
    case LACC_OPCODE_SG_H2D:
    case LACC_OPCODE_SG_D2H:
        *status = do_sg(s, d, d->opcode == LACC_OPCODE_SG_H2D, &bytes);
        ns += 50 * (uint64_t)d->len;   /* per-entry fetch */
        break;
    case LACC_OPCODE_FILL:
        if (!dev_range_ok(s, d->addr0, d->len) || (d->len & 3)) {
            *status = LACC_CQE_STATUS_RANGE;
            break;
        }
        for (uint32_t i = 0; i < d->len; i += 4) {
            stl_le_p(s->mem + d->addr0 + i, d->value);
        }
        bytes = d->len;
        break;
    case LACC_OPCODE_GEMM_LNS8:
    case LACC_OPCODE_GEMM_I8:
        *status = do_gemm(s, d, d->opcode == LACC_OPCODE_GEMM_LNS8, &macs);
        break;
    default:
        *status = LACC_CQE_STATUS_BAD_DESC;
    }
    /* DMA at 16 GB/s (PCIe 4.0 x16-class), compute at MACS_PER_CLOCK */
    ns += bytes / 16;
    ns += (uint64_t)(macs / (MACS_PER_CLOCK * mhz / 1000.0));
    lacc_add_work(s, bytes * PJ_PER_BYTE_DMA +
                  macs * (d->opcode == LACC_OPCODE_GEMM_LNS8 ? PJ_PER_MAC_LNS8 : PJ_PER_MAC_I8) * clock_scale(s),
                  ns);
    return ns;
}

static void lacc_post_cqe(LaccState *s, uint32_t tag, uint16_t status, bool want_irq)
{
    struct lacc_cqe c = {
        .tag = cpu_to_le32(tag),
        .status = cpu_to_le16(status),
        .flags = cpu_to_le16(s->cq_phase ? LACC_CQE_PHASE : 0),
        .done_ns = cpu_to_le64(dev_ns(s)),
    };

    if (s->fault & LACC_FAULT_BAD_CQE) {
        s->fault &= ~LACC_FAULT_BAD_CQE;
        c.tag = cpu_to_le32(0xdeadbeef);
    }
    if (((s->cq_tail + 1) & (s->cq_size - 1)) == s->cq_head) {
        /* completion ring overflow: the host stopped consuming */
        s->err_code = LACC_ERR_RING;
        s->error = true;
        lacc_raise(s, LACC_IRQ_ERROR);
        return;
    }
    if (pci_dma_write(&s->pdev, s->cq_base + (uint64_t)s->cq_tail * sizeof(c), &c, sizeof(c)) != MEMTX_OK) {
        s->err_code = LACC_ERR_DMA;
        s->error = true;
        lacc_raise(s, LACC_IRQ_ERROR);
        return;
    }
    s->cq_tail = (s->cq_tail + 1) & (s->cq_size - 1);
    if (!s->cq_tail) {
        s->cq_phase = !s->cq_phase;
    }
    if (status) {
        s->err_code = status == LACC_CQE_STATUS_DMA_ERROR ? LACC_ERR_DMA :
                      status == LACC_CQE_STATUS_RANGE ? LACC_ERR_RANGE : LACC_ERR_DESC;
        lacc_raise(s, LACC_IRQ_ERROR);
    }
    if (want_irq) {
        lacc_completion_irq(s);
    }
}

static bool lacc_rings_ok(LaccState *s)
{
    return s->sq_size >= 2 && s->sq_size <= MAX_RING && is_power_of_2(s->sq_size) &&
           s->cq_size >= 2 && s->cq_size <= MAX_RING && is_power_of_2(s->cq_size) && s->sq_base && s->cq_base;
}

/* One timer callback finishes the in-flight descriptor and starts the next. */
static void lacc_engine_step(void *opaque)
{
    LaccState *s = opaque;
    uint64_t ns;

    if (s->inflight) {
        s->inflight = false;
        s->perf_cmds++;
        lacc_post_cqe(s, le32_to_cpu(s->cur.tag), s->cur_status, s->cur.flags & LACC_DESC_IRQ);
    }
    if (!(s->control & LACC_CONTROL_ENABLE) || s->halted || !s->ready || !lacc_rings_ok(s) ||
        s->sq_head == s->sq_tail || now_ns() < s->link_down_until) {
        lacc_busy(s, false);
        return;
    }
    lacc_busy(s, true);
    if (pci_dma_read(&s->pdev, s->sq_base + (uint64_t)s->sq_head * sizeof(s->cur), &s->cur,
                     sizeof(s->cur)) != MEMTX_OK) {
        s->err_code = LACC_ERR_DMA;
        s->error = true;
        lacc_raise(s, LACC_IRQ_ERROR);
        return;
    }
    s->sq_head = (s->sq_head + 1) & (s->sq_size - 1);
    s->cur.tag = le32_to_cpu(s->cur.tag);
    s->cur.addr0 = le64_to_cpu(s->cur.addr0);
    s->cur.addr1 = le64_to_cpu(s->cur.addr1);
    s->cur.addr2 = le64_to_cpu(s->cur.addr2);
    s->cur.len = le32_to_cpu(s->cur.len);
    s->cur.m = le32_to_cpu(s->cur.m);
    s->cur.n = le32_to_cpu(s->cur.n);
    s->cur.k = le32_to_cpu(s->cur.k);
    s->cur.value = le32_to_cpu(s->cur.value);
    s->cur.tag = cpu_to_le32(s->cur.tag);

    if (s->fault & LACC_FAULT_HANG) {
        /* the engine wedges: no completion, ever, until a reset */
        s->fault &= ~LACC_FAULT_HANG;
        s->halted = true;
        s->err_code = LACC_ERR_HANG;
        return;
    }
    if (s->fault & LACC_FAULT_DMA_ERROR) {
        s->fault &= ~LACC_FAULT_DMA_ERROR;
        s->cur_status = LACC_CQE_STATUS_DMA_ERROR;
        ns = 1000;
    } else {
        ns = lacc_execute(s, &s->cur, &s->cur_status);
    }
    s->perf_busy_ns += ns;
    s->inflight = true;
    timer_mod(s->engine_timer, now_ns() + ns);
}

static void lacc_engine_kick(LaccState *s)
{
    if (!s->inflight && !timer_pending(s->engine_timer)) {
        timer_mod(s->engine_timer, now_ns());
    }
}

static void lacc_soft_reset(LaccState *s)
{
    timer_del(s->engine_timer);
    timer_del(s->coalesce_timer);
    s->inflight = false;
    lacc_busy(s, false);
    s->halted = false;
    s->error = false;
    s->err_code = LACC_ERR_NONE;
    s->sq_base = s->cq_base = 0;
    s->sq_size = s->cq_size = 0;
    s->sq_head = s->sq_tail = s->cq_head = s->cq_tail = 0;
    s->cq_phase = true;
    s->irq_status = 0;
    s->pending_cqes = 0;
    s->fault &= ~(LACC_FAULT_HANG | LACC_FAULT_DMA_ERROR | LACC_FAULT_BAD_CQE);
    s->control &= ~LACC_CONTROL_ENABLE;
    s->ready = false;
    lacc_fw_boot(s);
    timer_mod(s->reset_timer, now_ns() + RESET_NS);
}

static void lacc_reset_done(void *opaque)
{
    LaccState *s = opaque;

    s->ready = true;
    lacc_raise(s, LACC_IRQ_FW);
}

/* ---- SMBus: PMBus regulator and FRU EEPROM ------------------------------------------------ */

static uint16_t linear11(double v)
{
    int n = -16;

    while (n < 15 && (v / pow(2, n) > 1023 || v / pow(2, n) < -1024)) {
        n++;
    }
    return ((n & 0x1f) << 11) | ((int)lround(v / pow(2, n)) & 0x7ff);
}

static bool pmbus_read(LaccState *s, uint32_t op, uint8_t cmd, uint8_t *buf, uint32_t *len)
{
    double p_w = s->power_mw * 1e-3, vout = 0.55 + 0.35 * clock_scale(s), vin = 12.0;
    double iout = p_w / vout, pin = p_w / 0.9, iin = pin / vin;
    uint16_t w;

    if (op == LACC_I2C_OP_BYTE_DATA) {
        switch (cmd) {
        case 0x00: buf[0] = 0; break;                                   /* PAGE */
        case 0x01: buf[0] = 0x80; break;                                /* OPERATION: on */
        case 0x19: buf[0] = 0x00; break;                                /* CAPABILITY */
        case 0x20: buf[0] = 0x17; break;                                /* VOUT_MODE: linear, 2^-9 */
        case 0x78: buf[0] = s->temp_c * 1000 > s->throttle_mc ? 0x04 : 0; break;  /* STATUS_BYTE */
        case 0x7A: case 0x7B: case 0x7C: case 0x7E: buf[0] = 0; break;
        case 0x7D: buf[0] = s->temp_c * 1000 > s->throttle_mc ? 0x40 : 0; break;  /* STATUS_TEMPERATURE */
        case 0x98: buf[0] = 0x22; break;                                /* PMBUS_REVISION 1.2 */
        default: return false;
        }
        *len = 1;
        return true;
    }
    if (op == LACC_I2C_OP_WORD_DATA) {
        switch (cmd) {
        case 0x79: w = s->temp_c * 1000 > s->throttle_mc ? 0x0004 : 0; break;   /* STATUS_WORD */
        case 0x88: w = linear11(vin); break;
        case 0x89: w = linear11(iin); break;
        case 0x8B: w = (uint16_t)lround(vout * 512); break;            /* linear16, exponent -9 */
        case 0x8C: w = linear11(iout); break;
        case 0x8D: w = linear11(s->temp_c); break;
        case 0x96: w = linear11(p_w); break;
        case 0x97: w = linear11(pin); break;
        default: return false;
        }
        buf[0] = w & 0xff;
        buf[1] = w >> 8;
        *len = 2;
        return true;
    }
    if (op == LACC_I2C_OP_BLOCK_DATA) {
        const char *str = cmd == 0x99 ? "LACC" : cmd == 0x9A ? "LACC-VR1" : cmd == 0x9B ? "A1" : NULL;

        if (!str) {
            return false;
        }
        *len = strlen(str);
        memcpy(buf, str, *len);
        return true;
    }
    return false;
}

static void lacc_i2c_start(LaccState *s)
{
    uint8_t addr = s->i2c_ctrl & 0x7f;
    uint32_t op = (s->i2c_ctrl >> 8) & 0xf;
    bool rd = s->i2c_ctrl & (1u << 12);
    uint8_t cmd = s->i2c_cmd & 0xff;
    uint32_t len = 0;
    bool ack = false;

    if (addr == PMBUS_ADDR) {
        if (rd) {
            ack = pmbus_read(s, op, cmd, s->i2c_data, &len);
            if (ack) {
                s->i2c_len = len;
            }
        } else if (op == LACC_I2C_OP_BYTE) {
            ack = cmd == 0x03;                                         /* CLEAR_FAULTS */
        } else if (op == LACC_I2C_OP_BYTE_DATA) {
            ack = cmd == 0x00 || cmd == 0x01;                          /* PAGE (only 0 sticks), OPERATION */
        }
    } else if (addr == EEPROM_ADDR) {
        if (rd && op == LACC_I2C_OP_BYTE_DATA) {
            s->i2c_data[0] = s->eeprom[cmd];
            s->i2c_len = 1;
            ack = true;
        } else if (rd && op == LACC_I2C_OP_I2C_BLOCK) {
            len = MIN(s->i2c_len, 32u);
            for (uint32_t i = 0; i < len; i++) {
                s->i2c_data[i] = s->eeprom[(cmd + i) & 0xff];
            }
            ack = true;
        } else if (rd && op == LACC_I2C_OP_BYTE) {
            s->i2c_data[0] = s->eeprom[0];
            ack = true;
        } else if (!rd && (op == LACC_I2C_OP_BYTE || op == LACC_I2C_OP_QUICK)) {
            ack = true;                                                /* address set / probe */
        }
        /* writes are refused: the FRU EEPROM is write-protected on the board */
    }
    s->i2c_status = LACC_I2C_DONE | (ack ? 0 : LACC_I2C_NAK);
}

static void lacc_eeprom_init(LaccState *s)
{
    /* A small FRU record: magic, version, board, serial, MAC-like ID; checksum in the last byte. */
    char text[200];
    uint8_t sum = 0;

    memset(s->eeprom, 0xff, sizeof(s->eeprom));
    snprintf(text, sizeof(text), "LFRU1;board=LACC-PCIe-x16;rev=B;serial=%s;mfg=2026-10;tdp=75W",
             s->serial ? s->serial : "LACC0000001");
    memcpy(s->eeprom, text, strlen(text) + 1);
    for (int i = 0; i < 255; i++) {
        sum += s->eeprom[i];
    }
    s->eeprom[255] = (uint8_t)(0x100 - sum);
}

/* ---- SPI: W25Q128 NOR flash --------------------------------------------------------------- */

static void flash_cs_end(LaccState *s)
{
    uint32_t base, size = 0;

    switch (s->fcmd) {
    case 0x20: size = 4 * KiB; break;
    case 0x52: size = 32 * KiB; break;
    case 0xD8: size = 64 * KiB; break;
    case 0xC7: case 0x60:
        if (s->wel) {
            memset(s->flash, 0xff, LACC_FLASH_SIZE);
            lacc_flash_persist(s, 0, LACC_FLASH_SIZE);
        }
        break;
    case 0x01:
        break;
    default:
        break;
    }
    if (size && s->wel && s->fpos >= 4) {
        base = s->faddr & ~(size - 1) & (LACC_FLASH_SIZE - 1);
        memset(s->flash + base, 0xff, size);
        lacc_flash_persist(s, base, size);
    }
    if (s->fcmd == 0x02 && s->flash_dirty) {
        lacc_flash_persist(s, s->faddr & ~0xffu & (LACC_FLASH_SIZE - 1), 256);
    }
    if (s->fcmd == 0x02 || s->fcmd == 0x20 || s->fcmd == 0x52 || s->fcmd == 0xD8 || s->fcmd == 0xC7 ||
        s->fcmd == 0x60 || s->fcmd == 0x01) {
        s->wel = false;
    }
    s->flash_dirty = false;
    s->fpos = 0;
    s->fcmd = 0;
}

static uint8_t flash_xfer(LaccState *s, uint8_t in)
{
    uint8_t out = 0xff;
    uint32_t pos = s->fpos++;

    if (pos == 0) {
        s->fcmd = in;
        s->faddr = 0;
        if (in == 0x06) {
            s->wel = true;
        } else if (in == 0x04) {
            s->wel = false;
        }
        return 0xff;
    }
    switch (s->fcmd) {
    case 0x9F: {                                  /* JEDEC ID: Winbond W25Q128 */
        static const uint8_t id[] = {0xef, 0x40, 0x18};

        out = pos <= 3 ? id[pos - 1] : 0x00;
        break;
    }
    case 0x05:
        out = s->sr1 | (s->wel ? 0x02 : 0);
        break;
    case 0x35:
        out = s->sr2;
        break;
    case 0x15:
        out = 0x00;
        break;
    case 0x01:
        if (s->wel && pos == 1) {
            s->sr1 = in & 0xfc;
        } else if (s->wel && pos == 2) {
            s->sr2 = in;
        }
        break;
    case 0x31:
        if (s->wel && pos == 1) {
            s->sr2 = in;
        }
        break;
    case 0x03: case 0x0B: case 0x02: case 0x20: case 0x52: case 0xD8:
        if (pos <= 3) {
            s->faddr = (s->faddr << 8) | in;
            break;
        }
        if (s->fcmd == 0x0B && pos == 4) {
            break;                                /* dummy byte */
        }
        if (s->fcmd == 0x03 || s->fcmd == 0x0B) {
            out = s->flash[s->faddr & (LACC_FLASH_SIZE - 1)];
            s->faddr++;
        } else if (s->fcmd == 0x02 && s->wel) {
            uint32_t page = s->faddr & ~0xffu;
            uint32_t a = (page | ((s->faddr + pos - 4) & 0xff)) & (LACC_FLASH_SIZE - 1);

            s->flash[a] &= in;                    /* NOR programming only clears bits */
            s->flash_dirty = true;
        }
        break;
    default:
        break;                                    /* SFDP and anything else: not supported */
    }
    return out;
}

static void lacc_spi_start(LaccState *s)
{
    uint32_t len = MIN(MAX(s->spi_len, 1u), 256u);

    for (uint32_t i = 0; i < len; i++) {
        s->spi_rx[i] = s->cs ? flash_xfer(s, s->spi_tx[i]) : 0xff;
    }
    s->spi_status = LACC_SPI_DONE;
}

static void lacc_spi_ctrl(LaccState *s, uint32_t val)
{
    bool cs = val & LACC_SPI_CS;

    if (s->cs && !cs) {
        flash_cs_end(s);
    }
    if (!s->cs && cs) {
        s->fpos = 0;
    }
    s->cs = cs;
    s->spi_ctrl = val & LACC_SPI_CS;
    if (val & LACC_SPI_START) {
        lacc_spi_start(s);
    }
}

/* ---- JTAG TAP over OpenOCD remote_bitbang ------------------------------------------------- */

static enum tap_state tap_next(enum tap_state st, bool tms)
{
    static const enum tap_state next[16][2] = {
        [TAP_RESET]      = {TAP_IDLE, TAP_RESET},
        [TAP_IDLE]       = {TAP_IDLE, TAP_SELECT_DR},
        [TAP_SELECT_DR]  = {TAP_CAPTURE_DR, TAP_SELECT_IR},
        [TAP_CAPTURE_DR] = {TAP_SHIFT_DR, TAP_EXIT1_DR},
        [TAP_SHIFT_DR]   = {TAP_SHIFT_DR, TAP_EXIT1_DR},
        [TAP_EXIT1_DR]   = {TAP_PAUSE_DR, TAP_UPDATE_DR},
        [TAP_PAUSE_DR]   = {TAP_PAUSE_DR, TAP_EXIT2_DR},
        [TAP_EXIT2_DR]   = {TAP_SHIFT_DR, TAP_UPDATE_DR},
        [TAP_UPDATE_DR]  = {TAP_IDLE, TAP_SELECT_DR},
        [TAP_SELECT_IR]  = {TAP_CAPTURE_IR, TAP_RESET},
        [TAP_CAPTURE_IR] = {TAP_SHIFT_IR, TAP_EXIT1_IR},
        [TAP_SHIFT_IR]   = {TAP_SHIFT_IR, TAP_EXIT1_IR},
        [TAP_EXIT1_IR]   = {TAP_PAUSE_IR, TAP_UPDATE_IR},
        [TAP_PAUSE_IR]   = {TAP_PAUSE_IR, TAP_EXIT2_IR},
        [TAP_EXIT2_IR]   = {TAP_SHIFT_IR, TAP_UPDATE_IR},
        [TAP_UPDATE_IR]  = {TAP_IDLE, TAP_SELECT_DR},
    };
    return next[st][tms];
}

static void tap_reset(LaccState *s)
{
    s->tap = TAP_RESET;
    s->ir = JTAG_IDCODE;
}

static bool tap_tdo(LaccState *s)
{
    if (s->tap == TAP_SHIFT_DR) {
        return s->dr_shift & 1;
    }
    if (s->tap == TAP_SHIFT_IR) {
        return s->ir_shift & 1;
    }
    return false;
}

static void tap_clock(LaccState *s)
{
    switch (s->tap) {
    case TAP_CAPTURE_DR:
        switch (s->ir) {
        case JTAG_IDCODE: s->dr_shift = LACC_JTAG_IDCODE; s->dr_len = 32; break;
        case JTAG_DBG_ADDR: s->dr_shift = s->dbg_addr; s->dr_len = 32; break;
        case JTAG_DBG_DATA:   /* 33 bits: data, then bit 32 = write; a plain scan only reads */
            s->dr_shift = lacc_reg_read(s, s->dbg_addr & 0xfffc);
            s->dr_len = 33;
            break;
        default: s->dr_shift = 0; s->dr_len = 1; break;
        }
        break;
    case TAP_SHIFT_DR:
        s->dr_shift = (s->dr_shift >> 1) | ((uint64_t)s->tdi << (s->dr_len - 1));
        break;
    case TAP_CAPTURE_IR:
        s->ir_shift = 0x1;                         /* IEEE 1149.1: the two LSBs capture 01 */
        break;
    case TAP_SHIFT_IR:
        s->ir_shift = (s->ir_shift >> 1) | ((uint32_t)s->tdi << (JTAG_IR_LEN - 1));
        break;
    default:
        break;
    }
    s->tap = tap_next(s->tap, s->tms);
    if (s->tap == TAP_UPDATE_IR) {
        s->ir = s->ir_shift & 0xf;
    } else if (s->tap == TAP_UPDATE_DR) {
        if (s->ir == JTAG_DBG_ADDR) {
            s->dbg_addr = (uint32_t)s->dr_shift;
        } else if (s->ir == JTAG_DBG_DATA && (s->dr_shift >> 32) & 1) {
            lacc_reg_write(s, s->dbg_addr & 0xfffc, (uint32_t)s->dr_shift);
        }
    } else if (s->tap == TAP_RESET) {
        s->ir = JTAG_IDCODE;
    }
}

static int jtag_can_read(void *opaque)
{
    return 64;
}

static void jtag_read(void *opaque, const uint8_t *buf, int size)
{
    LaccState *s = opaque;

    for (int i = 0; i < size; i++) {
        uint8_t c = buf[i];

        if (c >= '0' && c <= '7') {
            bool tck = (c - '0') & 4;

            s->tms = (c - '0') & 2;
            s->tdi = (c - '0') & 1;
            if (tck && !s->tck) {
                tap_clock(s);
            }
            s->tck = tck;
        } else if (c == 'R') {
            uint8_t r = tap_tdo(s) ? '1' : '0';

            qemu_chr_fe_write_all(&s->jtag, &r, 1);
        } else if (c >= 'r' && c <= 'u') {
            if (c == 's' || c == 'u') {                /* TRST asserted */
                tap_reset(s);
            }
        }
        /* 'B', 'b' (blink) and 'Q' (quit) need no action */
    }
}

/* ---- register file ------------------------------------------------------------------------ */

static uint32_t lacc_status(LaccState *s)
{
    return (s->ready ? LACC_STATUS_READY : 0) | (s->inflight ? LACC_STATUS_BUSY : 0) |
           (s->halted ? LACC_STATUS_HALTED : 0) | (s->fw_ok ? LACC_STATUS_FW_OK : 0) |
           (s->throttled ? LACC_STATUS_THROTTLED : 0) | (s->error ? LACC_STATUS_ERROR : 0) |
           (!s->fw_ok ? LACC_STATUS_RECOVERY : 0);
}

static uint32_t lacc_reg_read(LaccState *s, hwaddr addr)
{
    uint64_t v;

    if (addr >= LACC_REG_I2C_DATA && addr < LACC_REG_I2C_DATA + 64) {
        return ldl_le_p(s->i2c_data + (addr - LACC_REG_I2C_DATA));
    }
    if (addr >= LACC_REG_SPI_RX && addr < LACC_REG_SPI_RX + 256) {
        return ldl_le_p(s->spi_rx + (addr - LACC_REG_SPI_RX));
    }
    switch (addr) {
    case LACC_REG_ID: return LACC_ID_MAGIC;
    case LACC_REG_HW_VERSION: return LACC_HW_VERSION;
    case LACC_REG_CAPS:
        return LACC_CAP_GEMM_LNS8 | LACC_CAP_GEMM_I8 | LACC_CAP_SG_DMA | LACC_CAP_I2C | LACC_CAP_SPI |
               LACC_CAP_JTAG | LACC_CAP_POWER;
    case LACC_REG_SCRATCH: return s->scratch;
    case LACC_REG_STATUS: return lacc_status(s);
    case LACC_REG_CONTROL: return s->control;
    case LACC_REG_DEVMEM_SIZE: return memory_region_size(&s->devmem);
    case LACC_REG_TIMESTAMP_LO:
        v = dev_ns(s);
        s->ts_hi_latch = v >> 32;
        return (uint32_t)v;
    case LACC_REG_TIMESTAMP_HI: return s->ts_hi_latch;
    case LACC_REG_SQ_BASE_LO: return (uint32_t)s->sq_base;
    case LACC_REG_SQ_BASE_HI: return s->sq_base >> 32;
    case LACC_REG_SQ_SIZE: return s->sq_size;
    case LACC_REG_SQ_TAIL: return s->sq_tail;
    case LACC_REG_SQ_HEAD: return s->sq_head;
    case LACC_REG_CQ_BASE_LO: return (uint32_t)s->cq_base;
    case LACC_REG_CQ_BASE_HI: return s->cq_base >> 32;
    case LACC_REG_CQ_SIZE: return s->cq_size;
    case LACC_REG_CQ_HEAD: return s->cq_head;
    case LACC_REG_CQ_TAIL: return s->cq_tail;
    case LACC_REG_IRQ_STATUS: return s->irq_status;
    case LACC_REG_IRQ_MASK: return s->irq_mask;
    case LACC_REG_IRQ_COALESCE: return s->coalesce;
    case LACC_REG_FW_STATUS: return s->fw_status;
    case LACC_REG_FW_VERSION: return s->fw_version;
    case LACC_REG_FW_MIN_VERSION: return s->fw_min_version;
    case LACC_REG_POWER_STATE: return s->power_state;
    case LACC_REG_POWER_CAP_MW: return s->power_cap_mw;
    case LACC_REG_TEMP_MC: return (uint32_t)(s->temp_c * 1000);
    case LACC_REG_ENERGY_UJ_LO:
        v = (uint64_t)s->energy_uj;
        s->energy_hi_latch = v >> 32;
        return (uint32_t)v;
    case LACC_REG_ENERGY_UJ_HI: return s->energy_hi_latch;
    case LACC_REG_ACTIVE_ENERGY_UJ_LO:
        v = (uint64_t)s->active_energy_uj;
        s->active_hi_latch = v >> 32;
        return (uint32_t)v;
    case LACC_REG_ACTIVE_ENERGY_UJ_HI: return s->active_hi_latch;
    case LACC_REG_CLOCK_MHZ: return s->clock_mhz;
    case LACC_REG_POWER_MW: return s->power_mw;
    case LACC_REG_PERF_CMDS: return (uint32_t)s->perf_cmds;
    case LACC_REG_PERF_DMA_KB: return (uint32_t)(s->perf_dma_bytes >> 10);
    case LACC_REG_PERF_MMACS: return (uint32_t)(s->perf_macs / 1000000);
    case LACC_REG_PERF_BUSY_US: return (uint32_t)(s->perf_busy_ns / 1000);
    case LACC_REG_PERF_IRQS: return (uint32_t)s->perf_irqs;
    case LACC_REG_FAULT_INJECT: return s->fault;
    case LACC_REG_ERR_CODE: return s->err_code;
    case LACC_REG_I2C_CTRL: return s->i2c_ctrl & 0x7fffffff;
    case LACC_REG_I2C_CMD: return s->i2c_cmd;
    case LACC_REG_I2C_LEN: return s->i2c_len;
    case LACC_REG_I2C_STATUS: return s->i2c_status;
    case LACC_REG_SPI_CTRL: return s->spi_ctrl;
    case LACC_REG_SPI_LEN: return s->spi_len;
    case LACC_REG_SPI_STATUS: return s->spi_status;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "lacc: read of unknown register 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void lacc_reg_write(LaccState *s, hwaddr addr, uint32_t val)
{
    if (addr >= LACC_REG_I2C_DATA && addr < LACC_REG_I2C_DATA + 64) {
        stl_le_p(s->i2c_data + (addr - LACC_REG_I2C_DATA), val);
        return;
    }
    if (addr >= LACC_REG_SPI_TX && addr < LACC_REG_SPI_TX + 256) {
        stl_le_p(s->spi_tx + (addr - LACC_REG_SPI_TX), val);
        return;
    }
    switch (addr) {
    case LACC_REG_SCRATCH: s->scratch = val; break;
    case LACC_REG_CONTROL:
        if (val & LACC_CONTROL_RESET) {
            lacc_soft_reset(s);
            break;
        }
        s->control = val & LACC_CONTROL_ENABLE;
        if (s->control & LACC_CONTROL_ENABLE) {
            lacc_engine_kick(s);
        }
        break;
    case LACC_REG_SQ_BASE_LO: s->sq_base = deposit64(s->sq_base, 0, 32, val); break;
    case LACC_REG_SQ_BASE_HI: s->sq_base = deposit64(s->sq_base, 32, 32, val); break;
    case LACC_REG_SQ_SIZE: s->sq_size = val; break;
    case LACC_REG_SQ_TAIL:
        if (s->sq_size && val < s->sq_size) {
            s->sq_tail = val;
            lacc_engine_kick(s);
        }
        break;
    case LACC_REG_CQ_BASE_LO: s->cq_base = deposit64(s->cq_base, 0, 32, val); break;
    case LACC_REG_CQ_BASE_HI: s->cq_base = deposit64(s->cq_base, 32, 32, val); break;
    case LACC_REG_CQ_SIZE: s->cq_size = val; break;
    case LACC_REG_CQ_HEAD:
        if (s->cq_size && val < s->cq_size) {
            s->cq_head = val;
            lacc_engine_kick(s);
        }
        break;
    case LACC_REG_IRQ_STATUS: s->irq_status &= ~val; break;
    case LACC_REG_IRQ_MASK: s->irq_mask = val; break;
    case LACC_REG_IRQ_COALESCE: s->coalesce = val; break;
    case LACC_REG_FW_CONTROL:
        if (val == LACC_FW_REBOOT) {
            lacc_soft_reset(s);
        }
        break;
    case LACC_REG_POWER_STATE:
        s->power_state = MIN(val, 2u);
        lacc_set_clock(s);
        break;
    case LACC_REG_POWER_CAP_MW: s->power_cap_mw = val; break;
    case LACC_REG_FAULT_INJECT:
        s->fault = val;
        if (val & LACC_FAULT_THERMAL) {
            s->fault &= ~LACC_FAULT_THERMAL;
            s->temp_c = (s->throttle_mc + 3000) / 1000.0;
            s->limit_mhz = s->clocks[2];
            lacc_set_clock(s);
            lacc_raise(s, LACC_IRQ_THERMAL);
        }
        if (val & LACC_FAULT_LINK_DOWN) {
            s->fault &= ~LACC_FAULT_LINK_DOWN;
            s->link_down_until = now_ns() + LINK_DOWN_NS;
        }
        break;
    case LACC_REG_I2C_CTRL:
        s->i2c_ctrl = val;
        s->i2c_status = 0;
        if (val & (1u << 31)) {
            lacc_i2c_start(s);
        }
        break;
    case LACC_REG_I2C_CMD: s->i2c_cmd = val; break;
    case LACC_REG_I2C_LEN: s->i2c_len = MIN(val, 64u); break;
    case LACC_REG_SPI_CTRL: lacc_spi_ctrl(s, val); break;
    case LACC_REG_SPI_LEN: s->spi_len = val; break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "lacc: write of unknown register 0x%" HWADDR_PRIx "\n", addr);
    }
}

static uint64_t lacc_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    LaccState *s = opaque;

    if (now_ns() < s->link_down_until) {
        return ~0ull;                              /* what a CPU sees when the link is gone */
    }
    return lacc_reg_read(s, addr);
}

static void lacc_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    LaccState *s = opaque;

    if (now_ns() < s->link_down_until) {
        return;
    }
    lacc_reg_write(s, addr, (uint32_t)val);
}

static const MemoryRegionOps lacc_mmio_ops = {
    .read = lacc_mmio_read,
    .write = lacc_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- device lifecycle --------------------------------------------------------------------- */

static void lacc_load_flash(LaccState *s, Error **errp)
{
    g_autofree gchar *data = NULL;
    gsize len = 0;

    s->flash = g_malloc(LACC_FLASH_SIZE);
    memset(s->flash, 0xff, LACC_FLASH_SIZE);
    if (!s->flash_path) {
        return;
    }
    if (!g_file_get_contents(s->flash_path, &data, &len, NULL)) {
        error_setg(errp, "lacc: cannot read flash image %s", s->flash_path);
        return;
    }
    if (len != LACC_FLASH_SIZE) {
        error_setg(errp, "lacc: flash image must be %u bytes, %s has %zu", LACC_FLASH_SIZE, s->flash_path,
                   (size_t)len);
        return;
    }
    memcpy(s->flash, data, len);
}

static void lacc_realize(PCIDevice *pdev, Error **errp)
{
    LaccState *s = LACC(pdev);
    Error *err = NULL;

    lacc_load_flash(s, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    pci_config_set_interrupt_pin(pdev->config, 1);
    memory_region_init_io(&s->mmio, OBJECT(s), &lacc_mmio_ops, s, "lacc-regs", LACC_BAR0_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64, &s->mmio);
    if (!memory_region_init_ram(&s->devmem, OBJECT(s), "lacc-devmem", LACC_BAR2_SIZE, errp)) {
        return;
    }
    s->mem = memory_region_get_ram_ptr(&s->devmem);
    pci_register_bar(pdev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->devmem);
    if (msix_init_exclusive_bar(pdev, LACC_MSIX_VECTORS, 4, errp)) {
        return;
    }
    for (int i = 0; i < LACC_MSIX_VECTORS; i++) {
        msix_vector_use(pdev, i);
    }
    /* a PCIe endpoint (an integrated one when plugged into the root complex) with AER */
    if (pcie_endpoint_cap_init(pdev, 0x80) < 0) {
        error_setg(errp, "lacc: cannot add the PCIe capability");
        return;
    }
    if (pcie_aer_init(pdev, PCI_ERR_VER, 0x100, PCI_ERR_SIZEOF, errp) < 0) {
        return;
    }

    s->engine_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lacc_engine_step, s);
    s->coalesce_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lacc_coalesce_fire, s);
    s->reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lacc_reset_done, s);
    s->power_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lacc_power_tick, s);
    s->temp_c = AMBIENT_C + 5;
    s->busy_since = -1;
    s->limit_mhz = UINT32_MAX;
    s->last_tick = now_ns();
    timer_mod(s->power_timer, s->last_tick + POWER_TICK_NS);
    lacc_eeprom_init(s);
    tap_reset(s);
    if (qemu_chr_fe_backend_connected(&s->jtag)) {
        qemu_chr_fe_set_handlers(&s->jtag, jtag_can_read, jtag_read, NULL, NULL, s, NULL, true);
    }
}

static void lacc_exit(PCIDevice *pdev)
{
    LaccState *s = LACC(pdev);

    timer_free(s->engine_timer);
    timer_free(s->coalesce_timer);
    timer_free(s->reset_timer);
    timer_free(s->power_timer);
    msix_uninit_exclusive_bar(pdev);
    g_free(s->flash);
}

static void lacc_reset(DeviceState *dev)
{
    LaccState *s = LACC(dev);

    s->time_base = now_ns();
    s->scratch = 0;
    s->irq_mask = 0;
    s->coalesce = 0;
    s->fault = 0;
    s->link_down_until = 0;
    s->power_state = 0;
    s->power_cap_mw = 0;
    s->limit_mhz = UINT32_MAX;
    s->busy_since = -1;
    s->sr1 = s->sr2 = 0;
    s->wel = false;
    s->cs = false;
    lacc_soft_reset(s);
}

static const Property lacc_properties[] = {
    DEFINE_PROP_STRING("flash", LaccState, flash_path),
    DEFINE_PROP_STRING("serial", LaccState, serial),
    DEFINE_PROP_CHR("jtag", LaccState, jtag),
};

static const VMStateDescription vmstate_lacc = {
    .name = "lacc",
    .unmigratable = 1,
};

static void lacc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = lacc_realize;
    k->exit = lacc_exit;
    k->vendor_id = LACC_VENDOR_ID;
    k->device_id = LACC_DEVICE_ID;
    k->revision = LACC_REVISION;
    k->class_id = LACC_CLASS;
    dc->desc = "LACC virtual inference card";
    dc->vmsd = &vmstate_lacc;
    device_class_set_props(dc, lacc_properties);
    device_class_set_legacy_reset(dc, lacc_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo lacc_info = {
    .name = TYPE_LACC,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(LaccState),
    .class_init = lacc_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void lacc_register_types(void)
{
    type_register_static(&lacc_info);
}

type_init(lacc_register_types)
