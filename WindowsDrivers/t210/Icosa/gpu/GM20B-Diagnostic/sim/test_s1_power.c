/* SPDX-License-Identifier: MIT */
/* Stage 1 (POWER) against the independent chip model in mock_s1_chip.c. */
#include "mock.h"
#include "mock_s1_chip.h"
#include "test.h"

#include <string.h>

#define GPU_BIT  0x01000000u
#define PLLG_BIT 0x20000000u

struct fx {
	struct mock *m;
	struct mock_chip chip;
	struct gm20b g;
};

static int rail_calls;
static u32 rail_uv_seen;
static u32 rail_writes_before;
static int rail_ret;

static int rail_hook(void *ctx, u32 uv)
{
	struct mock *m = ctx;
	u32 i;

	rail_calls++;
	rail_uv_seen = uv;
	rail_writes_before = 0;
	for (i = 0; i < mock_ntrace(m); i++)
		if (mock_trace(m, i)->is_wr)
			rail_writes_before++;
	return rail_ret;
}

static void fx_init(struct fx *f, bool cold, u32 max_stage, u32 flags, u32 rail_uv, bool with_hook)
{
	struct gm20b_hal hal;
	struct gm20b_cfg cfg;

	rail_calls = 0;
	rail_uv_seen = 0;
	rail_writes_before = 0;
	rail_ret = 0;
	f->m = mock_new();
	mock_chip_install(f->m, &f->chip, cold);
	mock_set(f->m, GM20B_SP_BAR0, 0x0, 0x12b000a1u);
	mock_hal(f->m, &hal);
	if (with_hook)
		hal.rail_gpu_set_uv = rail_hook;
	memset(&cfg, 0, sizeof(cfg));
	cfg.max_stage = max_stage;
	cfg.flags = flags;
	cfg.rail_uv = rail_uv;
	gm20b_ctx_init(&f->g, &hal, &cfg);
}

static void fx_done(struct fx *f)
{
	mock_free(f->m);
}

static u32 count_writes(struct mock *m, u32 from)
{
	u32 i, n = 0;

	for (i = from; i < mock_ntrace(m); i++)
		if (mock_trace(m, i)->is_wr)
			n++;
	return n;
}

static u32 count_space(struct mock *m, enum gm20b_space sp, u32 from)
{
	u32 i, n = 0;

	for (i = from; i < mock_ntrace(m); i++)
		if (mock_trace(m, i)->space == (u8)sp)
			n++;
	return n;
}

/* the STEP journal entry 'code' had already been flushed when access 'acc' happened */
static int step_before(struct mock *m, u32 code, int acc)
{
	int j = mock_jfind(m, code, 0xffffffffu, 0);

	return j >= 0 && acc >= 0 && (u32)j < mock_trace(m, (u32)acc)->jlen;
}

TEST(s1_power_up_order_and_intent_log)
{
	struct fx f;
	int clk, rst_a, pmc, rst_d;

	fx_init(&f, true, 0, 0, 0, false);
	T_EQ(gm20b_s1_init(&f.g), GM20B_OK);
	gm20b_jflush(&f.g);
	T_EQ(mock_nviolations(f.m), 0);

	clk   = mock_find_val(f.m, GM20B_SP_CAR, 0x284, 1, GPU_BIT, GPU_BIT, 0);
	rst_a = mock_find_val(f.m, GM20B_SP_CAR, 0x290, 1, GPU_BIT, GPU_BIT, 0);
	pmc   = mock_find_val(f.m, GM20B_SP_PMC, 0x2d4, 1, 0xffffffffu, 0, 0);
	rst_d = mock_find_val(f.m, GM20B_SP_CAR, 0x294, 1, GPU_BIT, GPU_BIT, 0);
	T_ASSERT(clk >= 0);
	T_ASSERT(rst_a > clk);
	T_ASSERT(pmc > rst_a);
	T_ASSERT(rst_d > pmc);
	T_EQ(mock_trace(f.m, (u32)clk)->val & (GPU_BIT | PLLG_BIT), GPU_BIT | PLLG_BIT);

	T_ASSERT(step_before(f.m, J_S1_CLK_ENABLE, clk));
	T_ASSERT(step_before(f.m, J_S1_RST_ASSERT, rst_a));
	T_ASSERT(step_before(f.m, J_S1_CLAMP_REMOVE, pmc));
	T_ASSERT(step_before(f.m, J_S1_RST_DEASSERT, rst_d));

	/* stage 1 never touches BAR0 and performs exactly these four writes */
	T_EQ(count_space(f.m, GM20B_SP_BAR0, 0), 0);
	T_EQ(count_writes(f.m, 0), 4);
	T_EQ(f.chip.n_alias_wr, 3);
	T_ASSERT(f.chip.clk_x & GPU_BIT);
	T_ASSERT(f.chip.clk_x & PLLG_BIT);
	T_EQ(f.chip.rst_x & GPU_BIT, 0);
	T_EQ(f.chip.rg & 1u, 0);
	T_ASSERT(mock_is_alive(f.m, GM20B_SP_BAR0));
	fx_done(&f);
}

TEST(s1_pre_state_is_probed_before_the_first_write)
{
	struct fx f;
	int first_wr, p;

	fx_init(&f, true, 0, 0, 0, false);
	T_EQ(gm20b_s1_init(&f.g), GM20B_OK);
	gm20b_jflush(&f.g);
	first_wr = mock_find(f.m, GM20B_SP_CAR, 0x284, 1, 0);
	T_ASSERT(first_wr >= 0);
	/* every pre-state register was read before that write */
	p = mock_find(f.m, GM20B_SP_CAR, 0x280, 0, 0);
	T_ASSERT(p >= 0 && p < first_wr);
	p = mock_find(f.m, GM20B_SP_CAR, 0x28c, 0, 0);
	T_ASSERT(p >= 0 && p < first_wr);
	p = mock_find(f.m, GM20B_SP_PMC, 0x2d4, 0, 0);
	T_ASSERT(p >= 0 && p < first_wr);
	p = mock_find(f.m, GM20B_SP_CAR, 0x67c, 0, 0);   /* PLLP_OUTC: read-only look */
	T_ASSERT(p >= 0 && p < first_wr);
	T_ASSERT(mock_jfind(f.m, J_S1_PRE_CLK, 0xffffffffu, 0) >= 0);
	T_ASSERT(mock_jfind(f.m, J_S1_PRE_RST, 0xffffffffu, 0) >= 0);
	T_ASSERT(mock_jfind(f.m, J_S1_PRE_CLAMP, 0xffffffffu, 0) >= 0);
	fx_done(&f);
}

/* Each of the five pre-state reads has its own flushed STEP: if the bus hangs on one of them the last journal line
 * names that register (the journal otherwise could only say "somewhere in the probe"). */
TEST(s1_every_probe_read_is_announced_first)
{
	static const struct { enum gm20b_space sp; u32 off; } rd[5] = {
		{ GM20B_SP_CAR, 0x280 }, { GM20B_SP_CAR, 0x28c }, { GM20B_SP_PMC, 0x2d4 },
		{ GM20B_SP_CAR, 0x0a0 }, { GM20B_SP_CAR, 0x67c },
	};
	struct fx f;
	int i;

	fx_init(&f, true, 0, 0, 0, false);
	T_EQ(gm20b_s1_init(&f.g), GM20B_OK);
	gm20b_jflush(&f.g);
	for (i = 0; i < 5; i++) {
		int acc = mock_find(f.m, rd[i].sp, rd[i].off, 0, 0);
		int j = mock_jfind(f.m, J_S1_PROBE_RD, GM20B_JADDR(rd[i].sp, rd[i].off), 0);

		T_ASSERT(acc >= 0);
		T_ASSERT(j >= 0);
		T_ASSERT((u32)j < mock_trace(f.m, (u32)acc)->jlen);   /* durable before the first read of that register */
	}
	fx_done(&f);
}

TEST(s1_rail_policy)
{
	struct fx f;

	/* flag unset: the hook is never used, the rail stays alone */
	fx_init(&f, true, 0, 0, 900000, true);
	T_EQ(gm20b_s1_init(&f.g), GM20B_OK);
	T_EQ(rail_calls, 0);
	fx_done(&f);

	/* ALLOW_RAIL without a hook: refused before ANY access */
	fx_init(&f, true, 0, GM20B_CF_ALLOW_RAIL, 900000, false);
	T_EQ(gm20b_s1_init(&f.g), GM20B_E_INVAL);
	T_EQ(mock_ntrace(f.m), 0);
	fx_done(&f);

	/* out of range, low and high: refused, hook unused, nothing touched */
	fx_init(&f, true, 0, GM20B_CF_ALLOW_RAIL, GM20B_RAIL_MIN_UV - 1, true);
	T_EQ(gm20b_s1_init(&f.g), GM20B_E_INVAL);
	T_EQ(rail_calls, 0);
	T_EQ(mock_ntrace(f.m), 0);
	fx_done(&f);
	fx_init(&f, true, 0, GM20B_CF_ALLOW_RAIL, GM20B_RAIL_MAX_UV + 1, true);
	T_EQ(gm20b_s1_init(&f.g), GM20B_E_INVAL);
	T_EQ(rail_calls, 0);
	T_EQ(mock_ntrace(f.m), 0);
	fx_done(&f);

	/* in range: the hook runs once, BEFORE the first write, with the requested value */
	fx_init(&f, true, 0, GM20B_CF_ALLOW_RAIL, 900000, true);
	T_EQ(gm20b_s1_init(&f.g), GM20B_OK);
	T_EQ(rail_calls, 1);
	T_EQ(rail_uv_seen, 900000);
	T_EQ(rail_writes_before, 0);
	fx_done(&f);

	/* the hook fails: E_IO and no clock/reset/clamp write at all */
	fx_init(&f, true, 0, GM20B_CF_ALLOW_RAIL, 900000, true);
	rail_ret = -5;
	T_EQ(gm20b_s1_init(&f.g), GM20B_E_IO);
	T_EQ(count_writes(f.m, 0), 0);
	gm20b_s1_fini(&f.g);
	T_EQ(count_writes(f.m, 0), 0);
	fx_done(&f);
}

TEST(s1_diag_only_writes_nothing)
{
	struct fx f;

	fx_init(&f, true, 0, GM20B_CF_DIAG_ONLY, 0, false);
	T_EQ(gm20b_s1_init(&f.g), GM20B_OK);
	T_EQ(count_writes(f.m, 0), 0);
	T_ASSERT(!mock_is_alive(f.m, GM20B_SP_BAR0));
	T_EQ(f.g.s1.did, 0);
	gm20b_s1_fini(&f.g);
	T_EQ(count_writes(f.m, 0), 0);
	fx_done(&f);
}

TEST(s1_clamp_write_dropped_fails_verification_and_fini_holds_reset)
{
	struct fx f;

	fx_init(&f, true, 0, 0, 0, false);
	f.chip.pmc_wr_ignored = true;               /* models a secure-only PMC */
	T_EQ(gm20b_s1_init(&f.g), GM20B_E_HW);
	gm20b_jflush(&f.g);
	T_ASSERT(mock_jfind(f.m, J_S1_VERIFY_FAIL, 4, 0) >= 0);   /* 4 = still clamped */
	T_ASSERT(!mock_is_alive(f.m, GM20B_SP_BAR0));
	gm20b_s1_fini(&f.g);
	T_ASSERT(f.chip.rst_x & GPU_BIT);
	T_EQ(f.chip.clk_x & (GPU_BIT | PLLG_BIT), 0);
	T_EQ(count_space(f.m, GM20B_SP_BAR0, 0), 0);
	T_EQ(mock_nviolations(f.m), 0);
	fx_done(&f);
}

TEST(s1_fini_order_and_scope)
{
	struct fx f;
	u32 n;
	int a, c;

	fx_init(&f, true, 0, 0, 0, false);
	f.chip.clk_x = PLLG_BIT;                    /* the ref clock gate was somebody else's */
	mock_chip_update(&f.chip);
	T_EQ(gm20b_s1_init(&f.g), GM20B_OK);
	n = mock_ntrace(f.m);
	gm20b_s1_fini(&f.g);
	gm20b_jflush(&f.g);

	a = mock_find_val(f.m, GM20B_SP_CAR, 0x290, 1, GPU_BIT, GPU_BIT, n);   /* reset assert */
	c = mock_find(f.m, GM20B_SP_CAR, 0x288, 1, n);                         /* clock gate off */
	T_ASSERT(a >= 0);
	T_ASSERT(c > a);
	T_EQ(mock_trace(f.m, (u32)c)->val, GPU_BIT);      /* only what we enabled */
	T_ASSERT(step_before(f.m, J_S1_FINI_RST, a));
	T_ASSERT(step_before(f.m, J_S1_FINI_CLK, c));
	T_ASSERT(f.chip.rst_x & GPU_BIT);
	T_EQ(f.chip.clk_x, PLLG_BIT);
	T_EQ(f.chip.rg, 0);                               /* not re-clamped */
	T_EQ(mock_find(f.m, GM20B_SP_PMC, 0x2d4, 1, n), -1);
	T_EQ(count_space(f.m, GM20B_SP_BAR0, n), 0);      /* fini never touches BAR0 */
	T_EQ(mock_nviolations(f.m), 0);

	n = mock_ntrace(f.m);                             /* idempotent */
	gm20b_s1_fini(&f.g);
	T_EQ(mock_ntrace(f.m), n);
	fx_done(&f);
}

TEST(s1_warm_start_is_accepted)
{
	struct fx f;

	fx_init(&f, false, 0, 0, 0, false);         /* the bootloader left the GPU running */
	T_EQ(gm20b_s1_init(&f.g), GM20B_OK);
	T_ASSERT(mock_is_alive(f.m, GM20B_SP_BAR0));
	T_ASSERT(mock_find(f.m, GM20B_SP_CAR, 0x290, 1, 0) >= 0);   /* still resets it */
	gm20b_s1_fini(&f.g);
	T_ASSERT(f.chip.rst_x & GPU_BIT);
	T_EQ(f.chip.clk_x, GPU_BIT | PLLG_BIT);           /* it was not our clock: left on */
	T_EQ(mock_nviolations(f.m), 0);
	fx_done(&f);
}

TEST(s1_fini_on_fresh_context_is_a_noop)
{
	struct fx f;

	fx_init(&f, true, 0, 0, 0, false);
	gm20b_s1_fini(&f.g);
	T_EQ(mock_ntrace(f.m), 0);
	fx_done(&f);
}

TEST(s1_pwr_clock_probe_is_informational_and_read_only)
{
	struct fx f;
	int idx;

	fx_init(&f, true, 0, 0, 0, false);
	mock_set(f.m, GM20B_SP_CAR, 0x0a0, 1u << 30);                                /* PLLP enabled */
	mock_set(f.m, GM20B_SP_CAR, 0x67c, (2u << 24) | (1u << 17) | (1u << 16));    /* OUT5: ratio 2 */
	T_EQ(gm20b_s1_init(&f.g), GM20B_OK);
	gm20b_jflush(&f.g);
	T_EQ(f.g.s1.pwr_khz, 204000);
	idx = mock_jfind(f.m, J_S1_PWR_CLK, 1, 0);
	T_ASSERT(idx >= 0);
	T_EQ(mock_jlog(f.m)[idx].b, 204000);
	T_EQ(mock_nviolations(f.m), 0);                   /* PLLP registers were never written */
	fx_done(&f);

	fx_init(&f, true, 0, 0, 0, false);                /* not running: still succeeds, reports 0 */
	T_EQ(gm20b_s1_init(&f.g), GM20B_OK);
	T_EQ(f.g.s1.pwr_khz, 0);
	fx_done(&f);
}

TEST(s1_runner_failure_unwinds_and_names_the_stage)
{
	struct fx f;

	fx_init(&f, true, 1, 0, 0, false);
	f.chip.pmc_wr_ignored = true;
	T_EQ(gm20b_init(&f.g), GM20B_E_HW);
	T_EQ(f.g.fail_stage, 1);
	T_EQ(f.g.last_err, GM20B_E_HW);
	T_ASSERT(f.chip.rst_x & GPU_BIT);                 /* held in reset again */
	T_EQ(count_space(f.m, GM20B_SP_BAR0, 0), 0);
	fx_done(&f);
}

TEST(s1_runner_success_flushes_intent_before_first_hardware_write)
{
	struct fx f;
	int first_wr, j;

	fx_init(&f, true, 1, 0, 0, false);
	T_EQ(gm20b_init(&f.g), GM20B_OK);
	T_ASSERT(mock_is_alive(f.m, GM20B_SP_BAR0));
	first_wr = mock_find(f.m, GM20B_SP_CAR, 0x284, 1, 0);
	T_ASSERT(first_wr >= 0);
	j = mock_jfind(f.m, J_STAGE_BEGIN, 1, 0);
	T_ASSERT(j >= 0);
	T_ASSERT((u32)j < mock_trace(f.m, (u32)first_wr)->jlen);
	gm20b_fini(&f.g);
	T_ASSERT(f.chip.rst_x & GPU_BIT);
	T_EQ(mock_nviolations(f.m), 0);
	fx_done(&f);
}
