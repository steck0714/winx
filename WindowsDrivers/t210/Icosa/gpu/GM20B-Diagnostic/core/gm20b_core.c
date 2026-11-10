/* SPDX-License-Identifier: MIT */
/*
 * winx-gm20b core: context, journal, bounded waits, stage runner.
 * Freestanding: no libc.
 */
#include "gm20b.h"

/* ---- tiny helpers (non-MMIO memory only) --------------------------------- */
void gm20b_zero(void *p, size_t n)
{
	u8 *b = (u8 *)p;
	while (n--)
		*b++ = 0;
}

void gm20b_copy(void *dst, const void *src, size_t n)
{
	u8 *d = (u8 *)dst;
	const u8 *s = (const u8 *)src;
	while (n--)
		*d++ = *s++;
}

/* ---- names --------------------------------------------------------------- */
static const char *const stage_names[GM20B_STAGE_COUNT] = {
	"MAP", "POWER", "ID", "CLK", "PRIV", "MEM", "FIFO", "ACR", "GR",
};

const char *gm20b_stage_name(u32 stage)
{
	return stage < GM20B_STAGE_COUNT ? stage_names[stage] : "?";
}

const char *gm20b_err_name(int err)
{
	switch (err) {
	case GM20B_OK:        return "OK";
	case GM20B_E_TIMEOUT: return "TIMEOUT";
	case GM20B_E_IO:      return "IO";
	case GM20B_E_NOMEM:   return "NOMEM";
	case GM20B_E_NODEV:   return "NODEV";
	case GM20B_E_NOTIMPL: return "NOTIMPL";
	case GM20B_E_INVAL:   return "INVAL";
	case GM20B_E_FW:      return "FW";
	case GM20B_E_GATED:   return "GATED";
	case GM20B_E_STATE:   return "STATE";
	case GM20B_E_HW:      return "HW";
	default:              return "ERR?";
	}
}

const char *gm20b_jcode_name(u32 code)
{
	switch (code) {
	/* A duplicate value anywhere in the jcodes_*.h files is a compile error here. */
#define X(n, v) case (v): return #n;
	GM20B_JCODES_ALL(X)
#undef X
	default:
		return "J_?";
	}
}

/* ---- journal ------------------------------------------------------------- */
void gm20b_j(struct gm20b *g, u32 code, u32 a, u32 b)
{
	struct gm20b_journal *j = &g->j;
	struct gm20b_jentry *e;

	if (j->head - j->flushed >= GM20B_J_RING) {
		/* ring full of unflushed entries: the oldest one is about to be overwritten */
		j->lost++;
		j->flushed++;
	}
	e = &j->ring[j->head % GM20B_J_RING];
	e->seq  = j->head;
	e->code = code;
	e->a    = a;
	e->b    = b;
	e->t_ns = g->hal.now_ns ? g->hal.now_ns(g->hal.ctx) : 0;
	j->head++;
}

void gm20b_jflush(struct gm20b *g)
{
	struct gm20b_journal *j = &g->j;
	u32 start, n, first;

	if (j->lost != j->lost_reported) {
		u32 reported = j->lost - j->lost_reported;

		j->lost_reported = j->lost;   /* set first: gm20b_j may bump 'lost' again */
		gm20b_j(g, J_JOURNAL_LOST, reported, 0);
	}
	if (j->head == j->flushed)
		return;
	if (!g->hal.journal_flush) {
		j->flushed = j->head;
		return;
	}
	n = j->head - j->flushed;
	start = j->flushed % GM20B_J_RING;
	first = GM20B_J_RING - start;
	if (first > n)
		first = n;
	g->hal.journal_flush(g->hal.ctx, &j->ring[start], first);
	if (n > first)
		g->hal.journal_flush(g->hal.ctx, &j->ring[0], n - first);
	j->flushed = j->head;
}

/* ---- bounded wait -------------------------------------------------------- */
int gm20b_poll(struct gm20b *g, enum gm20b_space sp, u32 off, u32 mask, u32 expect, u32 timeout_us)
{
	u64 t0 = gm20b_now_ns(g);
	u64 limit_ns = (u64)timeout_us * 1000u * g->cfg.poll_scale_pct / 100u;
	u32 max_iter = timeout_us + 64u;   /* even if the HAL clock never advances */
	u32 i, v = 0;

	for (i = 0; i < max_iter; i++) {
		v = gm20b_rd(g, sp, off);
		if ((v & mask) == expect)
			return GM20B_OK;
		if (gm20b_now_ns(g) - t0 >= limit_ns)
			break;
		gm20b_udelay(g, i < 16u ? 1u : 10u);
	}
	gm20b_j(g, J_WAIT_TIMEOUT, GM20B_JADDR(sp, off), v);
	return GM20B_E_TIMEOUT;
}

/* ---- DMA / firmware wrappers --------------------------------------------- */
int gm20b_dma_alloc(struct gm20b *g, struct gm20b_dma *d, size_t size, size_t align)
{
	int err;

	if (!size || (align & (align - 1u)))
		return GM20B_E_INVAL;
	if (!g->hal.dma_alloc)
		return GM20B_E_NOMEM;
	gm20b_zero(d, sizeof(*d));
	err = g->hal.dma_alloc(g->hal.ctx, d, size, align ? align : 4096u);
	if (err || !d->cpu) {
		gm20b_zero(d, sizeof(*d));
		return GM20B_E_NOMEM;
	}
	return GM20B_OK;
}

void gm20b_dma_free(struct gm20b *g, struct gm20b_dma *d)
{
	if (d->cpu && g->hal.dma_free)
		g->hal.dma_free(g->hal.ctx, d);
	gm20b_zero(d, sizeof(*d));
}

int gm20b_fw_get(struct gm20b *g, const char *name, struct gm20b_fw *fw)
{
	gm20b_zero(fw, sizeof(*fw));
	if (!g->hal.fw_get)
		return GM20B_E_FW;
	if (g->hal.fw_get(g->hal.ctx, name, fw) || !fw->data || !fw->size) {
		gm20b_zero(fw, sizeof(*fw));
		return GM20B_E_FW;
	}
	return GM20B_OK;
}

void gm20b_fw_put(struct gm20b *g, struct gm20b_fw *fw)
{
	if (fw->data && g->hal.fw_put)
		g->hal.fw_put(g->hal.ctx, fw);
	gm20b_zero(fw, sizeof(*fw));
}

/* ---- stage runner -------------------------------------------------------- */
static int s0_init(struct gm20b *g)
{
	const struct gm20b_hal *h = &g->hal;

	if (!h->rd32 || !h->wr32 || !h->udelay || !h->msleep || !h->now_ns)
		return GM20B_E_INVAL;
	return GM20B_OK;
}

static void s0_fini(struct gm20b *g)
{
	(void)g;
}

static const struct gm20b_stage_ops stage_table[GM20B_STAGE_COUNT] = {
	{ "MAP",   s0_init,         s0_fini },
	{ "POWER", gm20b_s1_init,   gm20b_s1_fini },
	{ "ID",    gm20b_s2_init,   gm20b_s2_fini },
	{ "CLK",   gm20b_s3_init,   gm20b_s3_fini },
	{ "PRIV",  gm20b_s4_init,   gm20b_s4_fini },
	{ "MEM",   gm20b_s5_init,   gm20b_s5_fini },
	{ "FIFO",  gm20b_s6_init,   gm20b_s6_fini },
	{ "ACR",   gm20b_s7_init,   gm20b_s7_fini },
	{ "GR",    gm20b_s8_init,   gm20b_s8_fini },
};

#ifdef GM20B_TESTHOOKS
const struct gm20b_stage_ops *gm20b_stage_override[GM20B_STAGE_COUNT];
#endif

static const struct gm20b_stage_ops *ops_of(u32 stage)
{
#ifdef GM20B_TESTHOOKS
	if (gm20b_stage_override[stage])
		return gm20b_stage_override[stage];
#endif
	return &stage_table[stage];
}

static void teardown_to(struct gm20b *g, u32 keep)
{
	while (g->stage_done > keep) {
		u32 s = g->stage_done - 1u;

		GM20B_STEP(g, J_FINI_BEGIN, s, 0);
		ops_of(s)->fini(g);
		gm20b_j(g, J_FINI_OK, s, 0);
		g->stage_done--;
	}
	gm20b_jflush(g);
}

static int run_stages(struct gm20b *g)
{
	u32 s;
	int err;

	g->last_err = 0;
	for (s = g->stage_done; s < GM20B_STAGE_COUNT; s++) {
		if (s > g->cfg.max_stage) {
			gm20b_j(g, J_STAGE_SKIP, s, 0);
			break;
		}
		GM20B_STEP(g, J_STAGE_BEGIN, s, 0);
		err = ops_of(s)->init(g);
		if (err) {
			g->last_err = err;
			g->fail_stage = s;
			GM20B_STEP(g, J_STAGE_FAIL, s, (u32)err);
			ops_of(s)->fini(g);          /* partial init of the failing stage */
			teardown_to(g, 0);
			GM20B_STEP(g, J_INIT_DONE, g->stage_done, (u32)err);
			return err;
		}
		g->stage_done = s + 1u;
		GM20B_STEP(g, J_STAGE_OK, s, 0);
	}
	GM20B_STEP(g, J_INIT_DONE, g->stage_done, 0);
	return GM20B_OK;
}

void gm20b_ctx_init(struct gm20b *g, const struct gm20b_hal *hal, const struct gm20b_cfg *cfg)
{
	gm20b_zero(g, sizeof(*g));
	g->hal = *hal;
	g->cfg = *cfg;
	if (g->cfg.poll_scale_pct == 0)
		g->cfg.poll_scale_pct = 100;
	if (g->cfg.max_stage >= GM20B_STAGE_COUNT)
		g->cfg.max_stage = GM20B_STAGE_COUNT - 1u;
}

int gm20b_init(struct gm20b *g)
{
	GM20B_STEP(g, J_INIT_BEGIN, g->cfg.max_stage, g->cfg.flags);
	return run_stages(g);
}

int gm20b_advance(struct gm20b *g, u32 new_max)
{
	if (new_max >= GM20B_STAGE_COUNT)
		new_max = GM20B_STAGE_COUNT - 1u;
	if (new_max > g->cfg.max_stage)
		g->cfg.max_stage = new_max;
	GM20B_STEP(g, J_INIT_BEGIN, g->cfg.max_stage, g->cfg.flags);
	return run_stages(g);
}

void gm20b_fini(struct gm20b *g)
{
	teardown_to(g, 0);
}
