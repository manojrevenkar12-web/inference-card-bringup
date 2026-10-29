/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * LACC hardware interface shared by the device model, the Linux driver and the runtime:
 * descriptor and completion formats, the firmware image format and the LNS8 number format.
 *
 * Everything here is little-endian on the wire. The structures are naturally aligned and
 * sized so the same definition works in the kernel (__le32 and friends, so sparse checks byte
 * order) and in user space and QEMU (plain integers, converted with le32_to_cpu() and friends).
 */
#ifndef LACC_HW_H
#define LACC_HW_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __u8 lacc_u8;
typedef __u16 lacc_u16;
typedef __u32 lacc_u32;
typedef __u64 lacc_u64;
typedef __s32 lacc_s32;
typedef __s64 lacc_s64;
/* wire formats: sparse checks every access goes through cpu_to_le*() / le*_to_cpu() */
typedef __le16 lacc_le16;
typedef __le32 lacc_le32;
typedef __le64 lacc_le64;
#else
#include <stdint.h>
typedef uint8_t lacc_u8;
typedef uint16_t lacc_u16;
typedef uint32_t lacc_u32;
typedef uint64_t lacc_u64;
typedef int32_t lacc_s32;
typedef int64_t lacc_s64;
typedef uint16_t lacc_le16;
typedef uint32_t lacc_le32;
typedef uint64_t lacc_le64;
#endif

#include "lacc_regs.h"

/* ---- submission and completion rings ---------------------------------------------------- */

#define LACC_DESC_IRQ   (1u << 0)   /* ask for an interrupt (subject to coalescing) */

/* One command, 64 bytes. Device offsets address BAR2 memory; host addresses are bus (IOVA). */
struct lacc_desc {
	lacc_u8  opcode;      /* LACC_OPCODE_* */
	lacc_u8  flags;       /* LACC_DESC_* */
	lacc_le16 reserved0;
	lacc_le32 tag;         /* echoed in the completion */
	lacc_le64 addr0;       /* H2D: host src | D2H: device src | GEMM: A | SG_H2D: sg list | FILL: device dst */
	lacc_le64 addr1;       /* H2D: device dst | D2H: host dst | GEMM: B | SG_*: device offset / sg list */
	lacc_le64 addr2;       /* GEMM: C */
	lacc_le32 len;         /* bytes, or sg entries for SG_* */
	lacc_le32 m, n, k;     /* GEMM: C[m x n] = A[m x k] * B[k x n], row-major */
	lacc_le32 value;       /* FILL: 32-bit pattern */
	lacc_le32 reserved1[3];
};

struct lacc_sg {
	lacc_le64 addr;
	lacc_le32 len;
	lacc_le32 reserved;
};

#define LACC_CQE_PHASE  (1u << 0)

/* One completion, 16 bytes. The phase bit flips each time the ring wraps, so the host can
 * find new entries without reading CQ_TAIL. */
struct lacc_cqe {
	lacc_le32 tag;
	lacc_le16 status;      /* LACC_CQE_STATUS_* */
	lacc_le16 flags;       /* LACC_CQE_PHASE */
	lacc_le64 done_ns;     /* device time the command finished */
};

/* ---- firmware image (SPI NOR, A/B slots) -------------------------------------------------- */

#define LACC_FLASH_SIZE         (16u << 20)   /* W25Q128 */
#define LACC_FLASH_SLOT_A       0x000000u
#define LACC_FLASH_SLOT_B       0x100000u
#define LACC_FLASH_SLOT_SIZE    0x100000u
#define LACC_FLASH_CONFIG       0x200000u     /* one 4 KiB sector */
#define LACC_FLASH_CONFIG_SIZE  0x1000u

#define LACC_FW_MAGIC  0x3157464Cu   /* "LFW1" */
#define LACC_CFG_MAGIC 0x4746434Cu   /* "LCFG" */

struct lacc_fw_header {
	lacc_u32 magic;
	lacc_u32 version;          /* shown in FW_VERSION */
	lacc_u32 rollback_index;   /* must be >= FW_MIN_VERSION to boot */
	lacc_u32 payload_len;
	lacc_u8  sha256[32];       /* of the payload */
	lacc_u32 flags;
	lacc_u32 header_crc32;     /* CRC-32 (zlib) of the header with this field zero */
};

/* Boot preferences and the anti-rollback floor, in the config sector. */
struct lacc_flash_config {
	lacc_u32 magic;
	lacc_u32 boot_slot;        /* 1 = A, 2 = B, 0 = newest valid */
	lacc_u32 min_version;      /* one-way: the boot ROM only raises it */
	lacc_u32 crc32;            /* of the three fields above */
};

/* Payload: TLV records. */
#define LACC_TLV_LNS_LUT  1   /* 8 x u32: 2^(i/8) in Q16 */
#define LACC_TLV_POWER    2   /* u32 tdp_mw, u32 idle_mw, u32 default_cap_mw */
#define LACC_TLV_THERMAL  3   /* u32 throttle_mc, u32 critical_mc */
#define LACC_TLV_CLOCKS   4   /* u32 p0_mhz, p1_mhz, p2_mhz */
#define LACC_TLV_BUILD    5   /* build string */

struct lacc_tlv {
	lacc_u16 type;
	lacc_u16 len;
};

/* ---- LNS8: an 8-bit logarithmic number format ---------------------------------------------
 *
 * bit 7: sign; bits 6..0: e. e = 0 is zero. Otherwise value = (-1)^sign * 2^((e - 64) / 8),
 * so magnitudes run from 2^-7.875 to 2^7.875 in steps of an eighth of an octave (~9%).
 *
 * A product is an addition of codes: L = ea + eb - 128 (in eighths of an octave). It is turned
 * into a linear Q16 value for accumulation with an 8-entry table of 2^(f/8) loaded from the
 * firmware, shifted by the integer part. Products are summed in 64 bits, and the result is
 * rounded to Q8 and saturated to 32 bits. This is a textbook hybrid LNS datapath, written to be
 * bit-exact between the device model and the reference; it is not any vendor's format.
 */

#define LACC_LNS_BIAS  64
#define LACC_LNS_FRAC  8

static inline lacc_s64 lacc_lns_product_q16(lacc_u8 a, lacc_u8 b, const lacc_u32 lut[8])
{
	lacc_u32 ea = a & 0x7f, eb = b & 0x7f;
	int l, q, f;
	lacc_s64 mag;

	if (!ea || !eb)
		return 0;
	l = (int)ea + (int)eb - 2 * LACC_LNS_BIAS;  /* -126 .. 126 */
	q = l >> 3;                                    /* floor(l / 8) */
	f = l & 7;
	mag = lut[f];
	if (q >= 0)
		mag <<= q;
	else
		mag = (mag + ((lacc_s64)1 << (-q - 1))) >> (-q);
	return ((a ^ b) & 0x80) ? -mag : mag;
}

static inline lacc_s32 lacc_lns_finish_q8(lacc_s64 acc_q16)
{
	lacc_s64 r = acc_q16 >= 0 ? (acc_q16 + 128) >> 8 : -((-acc_q16 + 128) >> 8);

	if (r > 0x7fffffffLL)
		return 0x7fffffff;
	if (r < -0x80000000LL)
		return (lacc_s32)0x80000000u;
	return (lacc_s32)r;
}

/* C[m x n] (Q8) = A[m x k] * B[k x n], all row-major. */
static inline void lacc_gemm_lns8(const lacc_u8 *A, const lacc_u8 *B, lacc_s32 *C, lacc_u32 m,
				  lacc_u32 n, lacc_u32 k, const lacc_u32 lut[8])
{
	lacc_u32 i, j, p;

	for (i = 0; i < m; i++)
		for (j = 0; j < n; j++) {
			lacc_s64 acc = 0;

			for (p = 0; p < k; p++)
				acc += lacc_lns_product_q16(A[i * k + p], B[p * n + j], lut);
			C[i * n + j] = lacc_lns_finish_q8(acc);
		}
}

static inline void lacc_gemm_i8(const signed char *A, const signed char *B, lacc_s32 *C, lacc_u32 m,
				lacc_u32 n, lacc_u32 k)
{
	lacc_u32 i, j, p;

	for (i = 0; i < m; i++)
		for (j = 0; j < n; j++) {
			lacc_s32 acc = 0;

			for (p = 0; p < k; p++)
				acc += (lacc_s32)A[i * k + p] * (lacc_s32)B[p * n + j];
			C[i * n + j] = acc;
		}
}

#endif /* LACC_HW_H */
