/* SPDX-License-Identifier: MIT */
/*
 * winx-gm20b core: context, config, stage runner, journal, helpers.
 * See docs/DESIGN.md for the rules every stage implementation must follow.
 */
#ifndef GM20B_H
#define GM20B_H

#include "gm20b_types.h"
#include "gm20b_hal.h"
#include "gm20b_jcodes.h"
#include "stage1_power.h"
#include "stage2_id.h"
#include "stage3_clk.h"
#include "stage4_priv.h"
#include "stage5_mem.h"
#include "stage6_fifo.h"
#include "stage7_acr.h"
#include "stage8_gr.h"

/* ---- bring-up ladder ------------------------------------------------------ */
enum gm20b_stage {
	GM20B_STAGE_MAP   = 0, /* validate HAL; no hardware access            */
	GM20B_STAGE_POWER = 1, /* CAR/PMC: clocks, reset, clamp               */
	GM20B_STAGE_ID    = 2, /* BOOT0 check, PMC enable, interrupt masks    */
	GM20B_STAGE_CLK   = 3, /* GPCPLL                                      */
	GM20B_STAGE_PRIV  = 4, /* priv ring, timer, fuses, FB, L2, top table  */
	GM20B_STAGE_MEM   = 5, /* instance memory, BAR1, GMMU                 */
	GM20B_STAGE_FIFO  = 6, /* PFIFO/PBDMA/channels + semaphore smoke test */
	GM20B_STAGE_ACR   = 7, /* PMU / ACR / FECS / GPCCS                    */
	GM20B_STAGE_GR    = 8, /* GR init + compute smoke test                */
	GM20B_STAGE_COUNT = 9
};

/* ---- configuration (filled by the platform shell, e.g. from the registry) -- */
#define GM20B_CF_TRACE_REGS  GM20B_BIT(0) /* journal every MMIO access (verbose) */
#define GM20B_CF_USE_PLL     GM20B_BIT(1) /* S3: lock GPCPLL (else run from the reference/bypass clock) */
#define GM20B_CF_ALLOW_RAIL  GM20B_BIT(2) /* S1: allow hal.rail_gpu_set_uv */
#define GM20B_CF_SELFTEST    GM20B_BIT(3) /* S6/S8 run their smoke tests */
#define GM20B_CF_DIAG_ONLY   GM20B_BIT(4) /* stages read/probe only; no state-changing writes beyond what is needed to read */

/* Hard safety clamp for hal.rail_gpu_set_uv (theory value, confidence C). */
#define GM20B_RAIL_MIN_UV    800000u
#define GM20B_RAIL_MAX_UV   1050000u

struct gm20b_cfg {
	u32 max_stage;       /* highest stage to run (0..8) */
	u32 flags;           /* GM20B_CF_* */
	u32 gpc_khz;         /* S3 target with GM20B_CF_USE_PLL (0 = stage default) */
	u32 rail_uv;         /* S1 target with GM20B_CF_ALLOW_RAIL (0 = leave rail alone) */
	u32 poll_scale_pct;  /* scales every bounded wait; 0 = 100 */
};

/* ---- discovered chip information (filled by stages 2 and 4) -------------- */
#define GM20B_MAX_GPC 4u

struct gm20b_info {
	u32 boot0;                        /* NV_PMC_BOOT_0 as read */
	u32 chipset;                      /* expected 0x12b */
	u32 chiprev;
	u32 gpc_count;
	u32 tpc_per_gpc[GM20B_MAX_GPC];
	u32 tpc_total;
	u32 ltc_count;
	u32 ltc_slices;                   /* per LTC */
	u32 smmu_enabled;                 /* MC SMMU enable bit as read (diagnostic only) */
	u32 reserved[8];
};

/* ---- journal ------------------------------------------------------------- */
#define GM20B_J_RING 512u

struct gm20b_journal {
	struct gm20b_jentry ring[GM20B_J_RING];
	u32 head;      /* total entries ever recorded */
	u32 flushed;   /* total entries handed to hal.journal_flush */
	u32 lost;      /* entries overwritten before they could be flushed */
	u32 lost_reported; /* part of 'lost' already announced with J_JOURNAL_LOST */
};

/* ---- the context --------------------------------------------------------- */
struct gm20b {
	struct gm20b_hal     hal;
	struct gm20b_cfg     cfg;
	struct gm20b_journal j;
	struct gm20b_info    info;

	u32 stage_done;   /* number of stages completed: 0 = none ... 9 = all */
	int last_err;     /* last failing status, 0 if none */
	u32 fail_stage;   /* stage that failed (valid when last_err != 0) */

	struct gm20b_s1 s1;
	struct gm20b_s2 s2;
	struct gm20b_s3 s3;
	struct gm20b_s4 s4;
	struct gm20b_s5 s5;
	struct gm20b_s6 s6;
	struct gm20b_s7 s7;
	struct gm20b_s8 s8;
};

/* ---- lifecycle ----------------------------------------------------------- */
/* Zero *g, copy hal/cfg. cfg->poll_scale_pct == 0 becomes 100. */
void gm20b_ctx_init(struct gm20b *g, const struct gm20b_hal *hal, const struct gm20b_cfg *cfg);
/* Run stages 0..cfg.max_stage. On failure: fini of everything done so far, returns the error. */
int  gm20b_init(struct gm20b *g);
/* Raise cfg.max_stage to new_max and run the additional stages (keeps earlier ones). */
int  gm20b_advance(struct gm20b *g, u32 new_max);
/* Tear down in reverse order. Always safe to call. */
void gm20b_fini(struct gm20b *g);

const char *gm20b_stage_name(u32 stage);
const char *gm20b_err_name(int err);
const char *gm20b_jcode_name(u32 code);

/* ---- journal ------------------------------------------------------------- */
void gm20b_j(struct gm20b *g, u32 code, u32 a, u32 b);
void gm20b_jflush(struct gm20b *g);
/* "Intent log": record and persist BEFORE an operation that may hang the bus. */
#define GM20B_STEP(g, code, a, b) \
	do { gm20b_j((g), (code), (a), (b)); gm20b_jflush((g)); } while (0)

/* ---- hardware access ----------------------------------------------------- */
static inline u32 gm20b_rd(struct gm20b *g, enum gm20b_space sp, u32 off)
{
	u32 v = g->hal.rd32(g->hal.ctx, sp, off);
	if (g->cfg.flags & GM20B_CF_TRACE_REGS)
		gm20b_j(g, J_RD, GM20B_JADDR(sp, off), v);
	return v;
}

static inline void gm20b_wr(struct gm20b *g, enum gm20b_space sp, u32 off, u32 v)
{
	if (g->cfg.flags & GM20B_CF_TRACE_REGS)
		gm20b_j(g, J_WR, GM20B_JADDR(sp, off), v);
	g->hal.wr32(g->hal.ctx, sp, off, v);
}

/* read-modify-write: v = (v & ~mask) | (val & mask). NOT atomic: never use on
 * registers shared with another driver (CAR!) -- use the SET/CLR aliases. */
static inline void gm20b_rmw(struct gm20b *g, enum gm20b_space sp, u32 off, u32 mask, u32 val)
{
	u32 v = gm20b_rd(g, sp, off);
	gm20b_wr(g, sp, off, (v & ~mask) | (val & mask));
}

/* BAR0 (GPU register) shorthands. */
#define GRD(g, off)            gm20b_rd((g), GM20B_SP_BAR0, (off))
#define GWR(g, off, v)         gm20b_wr((g), GM20B_SP_BAR0, (off), (v))
#define GRMW(g, off, m, v)     gm20b_rmw((g), GM20B_SP_BAR0, (off), (m), (v))

static inline void gm20b_udelay(struct gm20b *g, u32 us) { g->hal.udelay(g->hal.ctx, us); }
static inline void gm20b_msleep(struct gm20b *g, u32 ms) { g->hal.msleep(g->hal.ctx, ms); }
static inline u64  gm20b_now_ns(struct gm20b *g)         { return g->hal.now_ns(g->hal.ctx); }

/*
 * Bounded wait: poll until (read & mask) == expect. Time- AND iteration-bounded
 * (never spins forever even if the HAL clock does not advance).
 * Returns GM20B_OK or GM20B_E_TIMEOUT (journals J_WAIT_TIMEOUT).
 */
int gm20b_poll(struct gm20b *g, enum gm20b_space sp, u32 off, u32 mask, u32 expect, u32 timeout_us);

/* Memory barrier for CPU stores -> device-visible (DMA memory before doorbell/MMIO). */
#if defined(__aarch64__)
#define GM20B_MB() __asm__ volatile("dsb sy" ::: "memory")
#else
#define GM20B_MB() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

/* ---- DMA buffers --------------------------------------------------------- */
int  gm20b_dma_alloc(struct gm20b *g, struct gm20b_dma *d, size_t size, size_t align);
void gm20b_dma_free(struct gm20b *g, struct gm20b_dma *d);   /* idempotent */

/* DMA memory is accessed ONLY through these (aligned volatile 32-bit accesses). */
static inline void gm20b_dma_wr32(const struct gm20b_dma *d, size_t off, u32 v)
{
	*(volatile u32 *)((u8 *)d->cpu + off) = v;
}

static inline u32 gm20b_dma_rd32(const struct gm20b_dma *d, size_t off)
{
	return *(volatile u32 *)((u8 *)d->cpu + off);
}

/* 64-bit entry: high dword first, barrier, then low dword (valid/present bits live low). */
static inline void gm20b_dma_wr64(const struct gm20b_dma *d, size_t off, u64 v)
{
	gm20b_dma_wr32(d, off + 4, (u32)(v >> 32));
	GM20B_MB();
	gm20b_dma_wr32(d, off, (u32)v);
}

static inline u64 gm20b_dma_rd64(const struct gm20b_dma *d, size_t off)
{
	return ((u64)gm20b_dma_rd32(d, off + 4) << 32) | gm20b_dma_rd32(d, off);
}

/* ---- firmware ------------------------------------------------------------ */
int  gm20b_fw_get(struct gm20b *g, const char *name, struct gm20b_fw *fw);
void gm20b_fw_put(struct gm20b *g, struct gm20b_fw *fw);

/* ---- tiny libc-free helpers (non-MMIO memory only) ----------------------- */
void gm20b_zero(void *p, size_t n);
void gm20b_copy(void *dst, const void *src, size_t n);

/* Entry points of one stage. */
struct gm20b_stage_ops {
	const char *name;
	int  (*init)(struct gm20b *g);
	void (*fini)(struct gm20b *g);   /* must tolerate partial init */
};

#ifdef GM20B_TESTHOOKS
/* Host tests may replace a stage's entry points (NULL = use the real one). */
extern const struct gm20b_stage_ops *gm20b_stage_override[GM20B_STAGE_COUNT];
#endif

#endif /* GM20B_H */
