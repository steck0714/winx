/* SPDX-License-Identifier: MIT */
/*
 * IOCTL interface of \\.\Gm20b, shared by the driver and gm20bctl.exe.
 * Fixed-width types only; no dependency on the core or on the WDK.
 * All IOCTLs are METHOD_BUFFERED, FILE_ANY_ACCESS, device type FILE_DEVICE_UNKNOWN (0x22).
 */
#ifndef GM20B_IOCTL_H
#define GM20B_IOCTL_H

typedef unsigned int gm_u32;

#define GM20B_IOCTL_ABI      1u
#define GM20B_USER_PATH      "\\\\.\\Gm20b"

#define GM20B_CTL(n)         ((0x22u << 16) | (0u << 14) | ((0x800u + (n)) << 2) | 0u)
#define GM20B_IOCTL_GET_STATUS    GM20B_CTL(0)  /* out: struct gm20b_ioctl_status */
#define GM20B_IOCTL_READ_JOURNAL  GM20B_CTL(1)  /* in: gm_u32 from_seq; out: array of 24-byte journal entries */
#define GM20B_IOCTL_ADVANCE       GM20B_CTL(2)  /* in: gm_u32 new_max_stage; out: int gm20b status (optional) */
#define GM20B_IOCTL_FINI          GM20B_CTL(3)  /* tear every stage down (GPU back into reset) */
#define GM20B_IOCTL_RAW_READ      GM20B_CTL(4)  /* in: struct gm20b_ioctl_rawrd; out: gm_u32 (needs AllowRawMmio=1) */
#define GM20B_IOCTL_RAW_WRITE     GM20B_CTL(5)  /* in: struct gm20b_ioctl_rawwr (needs AllowRawMmio=1) */

struct gm20b_ioctl_status {
	gm_u32 abi;            /* GM20B_IOCTL_ABI */
	gm_u32 size;           /* sizeof(struct gm20b_ioctl_status) */
	gm_u32 max_stage;      /* cfg.max_stage now */
	gm_u32 stage_done;     /* stages completed (0 = none, 1 = only the HAL check ... ) */
	int    init_status;    /* result of the last gm20b_init / advance */
	int    last_err;       /* core's last failing status (0 = none) */
	gm_u32 fail_stage;
	gm_u32 flags;          /* cfg.flags */
	gm_u32 allow_raw;      /* registry AllowRawMmio */
	gm_u32 j_head;         /* journal entries recorded in total */
	gm_u32 j_flushed;      /* ... handed to the journal file */
	gm_u32 j_lost;         /* ... overwritten before they could be flushed */
	gm_u32 boot0;
	gm_u32 chipset;
	gm_u32 chiprev;
	gm_u32 gpc_count;
	gm_u32 tpc_total;
	gm_u32 ltc_count;
	gm_u32 smmu_enabled;
	gm_u32 jf_open_status; /* NTSTATUS of opening \SystemRoot\gm20b-journal.bin (0 = ok) */
	gm_u32 jf_errors;      /* failed journal-file writes in this driver load */
	gm_u32 jf_last_error;  /* NTSTATUS of the last failed write */
	gm_u32 bc_errors;      /* failed registry breadcrumb updates */
	gm_u32 bad_access;     /* MMIO accesses refused by the HAL (unmapped / unaligned / out of range) */
	gm_u32 jf_bytes;       /* bytes appended to the journal file in this driver load */
	gm_u32 driver_version;
	gm_u32 jf_dropped;     /* records NOT written to the file because of the 2 MiB cap (the registry crumb still follows) */
};

struct gm20b_ioctl_rawrd { gm_u32 space; gm_u32 off; };
struct gm20b_ioctl_rawwr { gm_u32 space; gm_u32 off; gm_u32 val; };

#endif
