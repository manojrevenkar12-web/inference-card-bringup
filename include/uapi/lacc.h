/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT */
/*
 * User-space interface of the LACC driver: one character device per card, /dev/laccN.
 *
 * Device memory is allocated in buffer objects (BOs) addressed by their device offset. Work is
 * submitted as jobs (a transfer, a fill or a GEMM); each submit returns a fence, a 64-bit
 * sequence number that LACC_IOC_WAIT blocks on. Jobs complete in submission order.
 * Device memory can also be mapped directly with mmap() at offset = BO offset.
 */
#ifndef _UAPI_LACC_H
#define _UAPI_LACC_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <linux/ioctl.h>
#include <linux/types.h>
#endif

#define LACC_IOC_MAGIC 'L'

struct lacc_info {
	__u32 hw_version;
	__u32 caps;
	__u32 fw_version;
	__u32 fw_status;
	__u64 devmem_size;
	__u32 status;
	__u32 clock_mhz;
	__u32 sq_entries;
	__u32 cq_entries;
};

struct lacc_bo {
	__u64 size;      /* in: bytes (rounded up to 4 KiB) */
	__u64 offset;    /* out (ALLOC) / in (FREE): device offset */
};

#define LACC_DIR_H2D 0
#define LACC_DIR_D2H 1

#define LACC_XFER_SG       0   /* one scatter-gather descriptor for the whole buffer */
#define LACC_XFER_PER_SEG  1   /* one descriptor per DMA segment */

struct lacc_xfer {
	__u64 user_ptr;
	__u64 dev_offset;
	__u64 size;
	__u32 dir;       /* LACC_DIR_* */
	__u32 mode;      /* LACC_XFER_* */
	__u64 fence;     /* out */
};

#define LACC_GEMM_LNS8 0
#define LACC_GEMM_I8   1

struct lacc_gemm {
	__u64 a, b, c;   /* device offsets */
	__u32 m, n, k;
	__u32 kind;      /* LACC_GEMM_* */
	__u64 fence;     /* out */
};

struct lacc_fill {
	__u64 dev_offset;
	__u64 size;      /* multiple of 4 */
	__u32 value;
	__u32 pad;
	__u64 fence;     /* out */
};

#define LACC_WAIT_OK       0
#define LACC_WAIT_FAILED   1   /* the device reported an error; see status */
#define LACC_WAIT_ABORTED  2   /* lost in a device reset */

struct lacc_wait {
	__u64 fence;
	__u32 timeout_ms;
	__u32 result;    /* out: LACC_WAIT_* */
	__u32 status;    /* out: first non-zero completion status (LACC_CQE_STATUS_*) */
	__u32 pad;
	__u64 submit_ns; /* out: device time at submission */
	__u64 done_ns;   /* out: device time at completion */
};

#define LACC_IOC_INFO   _IOR(LACC_IOC_MAGIC, 0x00, struct lacc_info)
#define LACC_IOC_ALLOC  _IOWR(LACC_IOC_MAGIC, 0x01, struct lacc_bo)
#define LACC_IOC_FREE   _IOW(LACC_IOC_MAGIC, 0x02, struct lacc_bo)
#define LACC_IOC_XFER   _IOWR(LACC_IOC_MAGIC, 0x03, struct lacc_xfer)
#define LACC_IOC_GEMM   _IOWR(LACC_IOC_MAGIC, 0x04, struct lacc_gemm)
#define LACC_IOC_FILL   _IOWR(LACC_IOC_MAGIC, 0x05, struct lacc_fill)
#define LACC_IOC_WAIT   _IOWR(LACC_IOC_MAGIC, 0x06, struct lacc_wait)

#endif /* _UAPI_LACC_H */
