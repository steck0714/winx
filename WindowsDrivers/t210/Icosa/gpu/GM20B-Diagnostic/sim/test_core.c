/* SPDX-License-Identifier: MIT */
/* Tests of the stage-independent core: journal, bounded waits, runner, DMA, mock itself. */
#include "mock.h"
#include "test.h"

#include <string.h>

struct fx {
	struct mock *m;
	struct gm20b g;
};

static void fx_init(struct fx *f, u32 max_stage, u32 flags)
{
	struct gm20b_hal hal;
	struct gm20b_cfg cfg;

	f->m = mock_new();
	mock_hal(f->m, &hal);
	memset(&cfg, 0, sizeof(cfg));
	cfg.max_stage = max_stage;
	cfg.flags = flags;
	gm20b_ctx_init(&f->g, &hal, &cfg);
}

static void fx_done(struct fx *f)
{
	mock_free(f->m);
}

static void clear_overrides(void)
{
	memset((void *)gm20b_stage_override, 0, sizeof(gm20b_stage_override));
}

/* ---- the mock itself ----------------------------------------------------- */
TEST(mock_gating_and_alignment)
{
	struct fx f;
	u32 v;

	fx_init(&f, 0, 0);
	mock_space_alive(f.m, GM20B_SP_BAR0, false);
	v = f.g.hal.rd32(f.g.hal.ctx, GM20B_SP_BAR0, 0x0);
	T_EQ(v, 0xffffffffu);
	T_EQ(mock_nviolations(f.m), 1);
	mock_space_alive(f.m, GM20B_SP_BAR0, true);
	f.g.hal.wr32(f.g.hal.ctx, GM20B_SP_BAR0, 0x100, 0x1234);
	T_EQ(f.g.hal.rd32(f.g.hal.ctx, GM20B_SP_BAR0, 0x100), 0x1234);
	T_EQ(mock_nviolations(f.m), 1);
	f.g.hal.rd32(f.g.hal.ctx, GM20B_SP_BAR0, 0x102);              /* unaligned */
	T_EQ(mock_nviolations(f.m), 2);
	f.g.hal.rd32(f.g.hal.ctx, GM20B_SP_CAR, GM20B_SZ_CAR);        /* past the end */
	T_EQ(mock_nviolations(f.m), 3);
	fx_done(&f);
}

static u32 hook_rd_plus1(struct mock *m, void *user, enum gm20b_space sp, u32 off, u32 cur)
{
	(void)m; (void)user; (void)sp; (void)off;
	return cur + 1;
}

TEST(mock_hook_read_and_trace)
{
	struct fx f;
	int i;

	fx_init(&f, 0, 0);
	mock_hook(f.m, GM20B_SP_BAR0, 0x200, 0x2ff, hook_rd_plus1, NULL, NULL);
	mock_set(f.m, GM20B_SP_BAR0, 0x204, 41);
	T_EQ(f.g.hal.rd32(f.g.hal.ctx, GM20B_SP_BAR0, 0x204), 42);
	T_EQ(f.g.hal.rd32(f.g.hal.ctx, GM20B_SP_BAR0, 0x300), 0);     /* outside the hook */
	T_EQ(mock_ntrace(f.m), 2);
	i = mock_find(f.m, GM20B_SP_BAR0, 0x204, 0, 0);
	T_EQ(i, 0);
	T_EQ(mock_find_val(f.m, GM20B_SP_BAR0, 0x204, 0, 0xffffffffu, 42, 0), 0);
	T_EQ(mock_find(f.m, GM20B_SP_BAR0, 0x204, 1, 0), -1);
	fx_done(&f);
}

/* ---- journal ------------------------------------------------------------- */
TEST(journal_flushes_in_order)
{
	struct fx f;
	const struct gm20b_jentry *log;
	u32 i;

	fx_init(&f, 0, 0);
	for (i = 0; i < 10; i++)
		gm20b_j(&f.g, J_NOTE, i, i * 2);
	gm20b_jflush(&f.g);
	T_EQ(mock_jlen(f.m), 10);
	log = mock_jlog(f.m);
	for (i = 0; i < 10; i++) {
		T_EQ(log[i].seq, i);
		T_EQ(log[i].code, J_NOTE);
		T_EQ(log[i].a, i);
		T_EQ(log[i].b, i * 2);
	}
	gm20b_jflush(&f.g);                    /* nothing new: no extra call */
	T_EQ(mock_jflushes(f.m), 1);
	fx_done(&f);
}

TEST(journal_wrap_reports_loss)
{
	struct fx f;
	const struct gm20b_jentry *log;
	u32 i, n;

	fx_init(&f, 0, 0);
	for (i = 0; i < 700; i++)
		gm20b_j(&f.g, J_NOTE, i, 0);
	gm20b_jflush(&f.g);
	n = mock_jlen(f.m);
	log = mock_jlog(f.m);
	T_ASSERT(n >= GM20B_J_RING - 2 && n <= GM20B_J_RING + 1);
	for (i = 1; i < n; i++)
		T_ASSERT(log[i].seq > log[i - 1].seq);
	T_EQ(log[n - 1].code, J_JOURNAL_LOST);
	T_EQ(log[n - 1].a, 700 - GM20B_J_RING);
	fx_done(&f);
}

TEST(journal_ring_split_flush)
{
	struct fx f;
	u32 i;

	fx_init(&f, 0, 0);
	for (i = 0; i < GM20B_J_RING - 8; i++)
		gm20b_j(&f.g, J_NOTE, i, 0);
	gm20b_jflush(&f.g);                    /* flushed == RING-8 */
	for (i = 0; i < 20; i++)
		gm20b_j(&f.g, J_NOTE, 1000 + i, 0);
	gm20b_jflush(&f.g);                    /* wraps: two hal calls, 20 entries, in order */
	T_EQ(mock_jlen(f.m), GM20B_J_RING - 8 + 20);
	T_EQ(mock_jlog(f.m)[GM20B_J_RING - 8].a, 1000);
	T_EQ(mock_jlog(f.m)[GM20B_J_RING - 8 + 19].a, 1019);
	T_EQ(f.g.j.lost, 0);
	fx_done(&f);
}

/* ---- bounded waits ------------------------------------------------------- */
static u32 hook_set_after(struct mock *m, void *user, enum gm20b_space sp, u32 off, u32 cur)
{
	u64 t = *(u64 *)user;

	(void)sp; (void)off;
	return mock_now(m) >= t ? (cur | 1u) : cur;
}

TEST(poll_succeeds_when_bit_appears)
{
	struct fx f;
	u64 thr = 30 * 1000;                   /* bit appears after 30 us of virtual time */

	fx_init(&f, 0, 0);
	mock_hook(f.m, GM20B_SP_BAR0, 0x400, 0x400, hook_set_after, NULL, &thr);
	T_EQ(gm20b_poll(&f.g, GM20B_SP_BAR0, 0x400, 1, 1, 100), GM20B_OK);
	T_ASSERT(mock_now(f.m) >= thr);
	T_ASSERT(mock_now(f.m) < 100 * 1000);
	fx_done(&f);
}

TEST(poll_times_out_in_bounded_time)
{
	struct fx f;
	u64 elapsed;

	fx_init(&f, 0, 0);
	T_EQ(gm20b_poll(&f.g, GM20B_SP_BAR0, 0x400, 1, 1, 100), GM20B_E_TIMEOUT);
	elapsed = mock_now(f.m);
	T_ASSERT(elapsed >= 100 * 1000);
	T_ASSERT(elapsed < 200 * 1000);
	gm20b_jflush(&f.g);
	T_ASSERT(mock_jfind(f.m, J_WAIT_TIMEOUT, GM20B_JADDR(GM20B_SP_BAR0, 0x400), 0) >= 0);
	fx_done(&f);
}

static u64 frozen_now(void *c)
{
	(void)c;
	return 0;
}

static void nop_udelay(void *c, u32 us)
{
	(void)c; (void)us;
}

TEST(poll_has_iteration_cap_when_clock_is_frozen)
{
	struct fx f;

	fx_init(&f, 0, 0);
	f.g.hal.now_ns = frozen_now;
	f.g.hal.udelay = nop_udelay;
	T_EQ(gm20b_poll(&f.g, GM20B_SP_BAR0, 0x400, 1, 1, 1000), GM20B_E_TIMEOUT);
	T_EQ(mock_ntrace(f.m), 1000 + 64);
	fx_done(&f);
}

/* ---- register helpers ---------------------------------------------------- */
TEST(rmw_and_trace_flag)
{
	struct fx f;

	fx_init(&f, 0, GM20B_CF_TRACE_REGS);
	mock_set(f.m, GM20B_SP_BAR0, 0x500, 0xf0f0f0f0u);
	GRMW(&f.g, 0x500, 0x0000ff00u, 0x00001200u);
	T_EQ(mock_get(f.m, GM20B_SP_BAR0, 0x500), 0xf0f012f0u);
	gm20b_jflush(&f.g);
	T_ASSERT(mock_jfind(f.m, J_RD, GM20B_JADDR(GM20B_SP_BAR0, 0x500), 0) >= 0);
	T_ASSERT(mock_jfind(f.m, J_WR, GM20B_JADDR(GM20B_SP_BAR0, 0x500), 0) >= 0);
	fx_done(&f);
}

/* ---- stage runner -------------------------------------------------------- */
static int ev[64];
static int nev;
static int fail_at;

#define STAGE_FNS(n)                                                            \
	static int init_##n(struct gm20b *g)                                    \
	{                                                                       \
		(void)g;                                                        \
		ev[nev++] = 10 + (n);                                           \
		return fail_at == (n) ? GM20B_E_HW : 0;                         \
	}                                                                       \
	static void fini_##n(struct gm20b *g)                                   \
	{                                                                       \
		(void)g;                                                        \
		ev[nev++] = 20 + (n);                                           \
	}                                                                       \
	static const struct gm20b_stage_ops ops_##n = { "T" #n, init_##n, fini_##n };

STAGE_FNS(1) STAGE_FNS(2) STAGE_FNS(3) STAGE_FNS(4)
STAGE_FNS(5) STAGE_FNS(6) STAGE_FNS(7) STAGE_FNS(8)

static void install_overrides(void)
{
	gm20b_stage_override[1] = &ops_1; gm20b_stage_override[2] = &ops_2;
	gm20b_stage_override[3] = &ops_3; gm20b_stage_override[4] = &ops_4;
	gm20b_stage_override[5] = &ops_5; gm20b_stage_override[6] = &ops_6;
	gm20b_stage_override[7] = &ops_7; gm20b_stage_override[8] = &ops_8;
	nev = 0;
	fail_at = 0;
}

TEST(runner_stage0_only_touches_no_hardware)
{
	struct fx f;

	clear_overrides();
	fx_init(&f, 0, 0);
	T_EQ(gm20b_init(&f.g), GM20B_OK);
	T_EQ(f.g.stage_done, 1);
	T_EQ(mock_ntrace(f.m), 0);
	T_ASSERT(mock_jfind(f.m, J_STAGE_OK, 0, 0) >= 0);
	T_ASSERT(mock_jfind(f.m, J_STAGE_SKIP, 1, 0) >= 0);
	T_EQ(mock_jfind(f.m, J_STAGE_BEGIN, 1, 0), -1);
	gm20b_fini(&f.g);
	T_EQ(f.g.stage_done, 0);
	fx_done(&f);
}

TEST(runner_rejects_incomplete_hal)
{
	struct fx f;

	clear_overrides();
	fx_init(&f, 0, 0);
	f.g.hal.udelay = NULL;
	T_EQ(gm20b_init(&f.g), GM20B_E_INVAL);
	T_EQ(f.g.fail_stage, 0);
	T_EQ(f.g.stage_done, 0);
	fx_done(&f);
}

TEST(runner_failure_unwinds_in_reverse)
{
	struct fx f;
	static const int want[] = { 11, 12, 13, 23, 22, 21 };
	u32 i;

	clear_overrides();
	install_overrides();
	fail_at = 3;
	fx_init(&f, 8, 0);
	T_EQ(gm20b_init(&f.g), GM20B_E_HW);
	T_EQ(nev, (int)GM20B_ARRAY_SIZE(want));
	for (i = 0; i < GM20B_ARRAY_SIZE(want); i++)
		T_EQ(ev[i], want[i]);
	T_EQ(f.g.stage_done, 0);
	T_EQ(f.g.last_err, GM20B_E_HW);
	T_EQ(f.g.fail_stage, 3);
	T_ASSERT(mock_jfind(f.m, J_STAGE_FAIL, 3, 0) >= 0);
	T_EQ(mock_jlog(f.m)[mock_jfind(f.m, J_STAGE_FAIL, 3, 0)].b, (u32)GM20B_E_HW);
	fx_done(&f);
	clear_overrides();
}

TEST(runner_intent_is_flushed_before_each_stage)
{
	struct fx f;
	int b, i;

	clear_overrides();
	install_overrides();
	fx_init(&f, 2, 0);
	T_EQ(gm20b_init(&f.g), GM20B_OK);
	/* every J_STAGE_BEGIN must already be in the flushed log when the stage body runs;
	 * the log order proves BEGIN(n) precedes OK(n) and OK(n) precedes BEGIN(n+1). */
	for (i = 0; i <= 2; i++) {
		b = mock_jfind(f.m, J_STAGE_BEGIN, (u32)i, 0);
		T_ASSERT(b >= 0);
		T_ASSERT(mock_jfind(f.m, J_STAGE_OK, (u32)i, (u32)b) > b);
	}
	fx_done(&f);
	clear_overrides();
}

TEST(runner_advance_runs_only_new_stages_and_fini_is_reverse)
{
	struct fx f;
	static const int want[] = { 11, 12, 13, 14, 15, 25, 24, 23, 22, 21 };
	u32 i;

	clear_overrides();
	install_overrides();
	fx_init(&f, 2, 0);
	T_EQ(gm20b_init(&f.g), GM20B_OK);
	T_EQ(f.g.stage_done, 3);
	T_EQ(nev, 2);
	T_EQ(gm20b_advance(&f.g, 5), GM20B_OK);
	T_EQ(f.g.stage_done, 6);
	T_EQ(nev, 5);
	gm20b_fini(&f.g);
	T_EQ(nev, (int)GM20B_ARRAY_SIZE(want));
	for (i = 0; i < GM20B_ARRAY_SIZE(want); i++)
		T_EQ(ev[i], want[i]);
	T_EQ(f.g.stage_done, 0);
	fx_done(&f);
	clear_overrides();
}

TEST(runner_clamps_max_stage)
{
	struct fx f;

	clear_overrides();
	install_overrides();
	fx_init(&f, 99, 0);
	T_EQ(f.g.cfg.max_stage, GM20B_STAGE_COUNT - 1);
	T_EQ(gm20b_init(&f.g), GM20B_OK);
	T_EQ(f.g.stage_done, GM20B_STAGE_COUNT);
	gm20b_fini(&f.g);
	fx_done(&f);
	clear_overrides();
}

/* ---- DMA / names --------------------------------------------------------- */
TEST(dma_alloc_helpers_and_failure_paths)
{
	struct fx f;
	struct gm20b_dma d, e;

	fx_init(&f, 0, 0);
	T_EQ(gm20b_dma_alloc(&f.g, &d, 4096, 4096), GM20B_OK);
	T_EQ(d.phys & 4095, 0);
	T_ASSERT(d.cpu != NULL);
	gm20b_dma_wr64(&d, 8, 0x1122334455667788ull);
	T_EQ(gm20b_dma_rd32(&d, 8), 0x55667788u);
	T_EQ(gm20b_dma_rd32(&d, 12), 0x11223344u);
	T_EQ(gm20b_dma_rd64(&d, 8), 0x1122334455667788ull);
	T_EQ(gm20b_dma_alloc(&f.g, &e, 0, 4096), GM20B_E_INVAL);
	T_EQ(gm20b_dma_alloc(&f.g, &e, 64, 3), GM20B_E_INVAL);
	mock_dma_fail_after(f.m, 0);
	T_EQ(gm20b_dma_alloc(&f.g, &e, 64, 64), GM20B_E_NOMEM);
	T_ASSERT(e.cpu == NULL);
	T_EQ(mock_dma_live(f.m), 1);
	gm20b_dma_free(&f.g, &d);
	gm20b_dma_free(&f.g, &d);              /* idempotent */
	T_EQ(mock_dma_live(f.m), 0);
	T_EQ(mock_nviolations(f.m), 0);
	fx_done(&f);
}

TEST(firmware_helpers)
{
	struct fx f;
	struct gm20b_fw fw;
	static const u8 blob[4] = { 1, 2, 3, 4 };

	fx_init(&f, 0, 0);
	mock_fw_add(f.m, "nvidia/gm20b/test.bin", blob, sizeof(blob));
	T_EQ(gm20b_fw_get(&f.g, "nvidia/gm20b/test.bin", &fw), GM20B_OK);
	T_EQ(fw.size, 4);
	T_EQ(((const u8 *)fw.data)[2], 3);
	gm20b_fw_put(&f.g, &fw);
	T_ASSERT(fw.data == NULL);
	T_EQ(gm20b_fw_get(&f.g, "nvidia/gm20b/missing.bin", &fw), GM20B_E_FW);
	fx_done(&f);
}

TEST(names)
{
	T_ASSERT(!strcmp(gm20b_jcode_name(J_STAGE_FAIL), "J_STAGE_FAIL"));
	T_ASSERT(!strcmp(gm20b_jcode_name(0xdead), "J_?"));
	T_ASSERT(!strcmp(gm20b_stage_name(GM20B_STAGE_FIFO), "FIFO"));
	T_ASSERT(!strcmp(gm20b_err_name(GM20B_E_TIMEOUT), "TIMEOUT"));
}
