/* SPDX-License-Identifier: MIT */
/* Private declarations of the Windows shell (the C files next to this header). */
#ifndef WIN_PRIV_H
#define WIN_PRIV_H

#include "ntk_min.h"
#include "gm20b.h"
#include "gm20b_ioctl.h"
#include "gm20b_journal_file.h"
#include "gm20b_jcodes_win.h"

#define GM20B_WIN_VERSION   0x00010000u
#define WIN_PEND_MAX        32u
#define WIN_JF_SESSION_CAP  (2u * 1024u * 1024u)   /* stop appending after 2 MiB per driver load */

/* reasons in b of J_WIN_RAW_DENIED */
#define WIN_RAW_OFF         1u   /* AllowRawMmio=0 */
#define WIN_RAW_POLICY      2u   /* space/offset/alias not allowed */
#define WIN_RAW_UNPOWERED   3u   /* BAR0/BAR1 while CAR/PMC say the GPU is not powered */
#define WIN_RAW_BADREQ      4u

/* The one and only device state (a global in .bss: no pool allocations anywhere). */
struct win_dev {
	struct gm20b g;
	int g_valid;                    /* g was initialised by gm20b_ctx_init */
	int map_ok;                     /* all spaces mapped */
	int busy;                       /* IOCTL serialisation */
	void *map[GM20B_SP_COUNT];
	u32 map_size[GM20B_SP_COUNT];

	struct gm20b_cfg cfg;           /* as read from the registry */
	u32 allow_raw;                  /* registry AllowRawMmio */
	int init_status;                /* result of the last init / advance */

	/* journal persistence (win_journal.c) */
	HANDLE jf;                      /* \SystemRoot\gm20b-journal.bin (write-through, append) */
	HANDLE key;                     /* the service key (registry breadcrumb) */
	NTSTATUS jf_open_status, key_status, params_status;
	u32 jf_created;
	u32 jf_bytes, jf_err_count, jf_dropped;
	NTSTATUS jf_last_err;
	u32 bc_err_count;
	NTSTATUS bc_last_err;
	u32 rec_seq, npend;
	struct gm20b_jentry pend[WIN_PEND_MAX];
	u32 bad_access;

	/* control device */
	PDEVICE_OBJECT ctl;
	int link_created;
};

extern struct win_dev g_win;

/* gm20b_sys.c */
NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg_path);

/* win_journal.c */
u64  win_now_ns(void);
void win_jf_open(struct win_dev *d);
void win_jf_close(struct win_dev *d);
void win_rec(struct win_dev *d, u32 code, u32 a, u32 b);    /* queue (older core entries are flushed first) */
void win_step(struct win_dev *d, u32 code, u32 a, u32 b);   /* queue and make durable NOW */
void win_sync(struct win_dev *d);                           /* make everything durable NOW */
void win_jf_commit(struct win_dev *d, const struct gm20b_jentry *e, u32 n);   /* HAL journal_flush */
void win_bad_access(struct win_dev *d, enum gm20b_space sp, u32 off, u32 reason);

/* hal_win.c */
void win_hal_fill(struct win_dev *d, struct gm20b_hal *hal);
int  win_map_all(struct win_dev *d);
void win_unmap_all(struct win_dev *d);

/* win_cfg.c */
void win_cfg_load(struct win_dev *d, PUNICODE_STRING reg_path, struct gm20b_cfg *cfg);

#endif
