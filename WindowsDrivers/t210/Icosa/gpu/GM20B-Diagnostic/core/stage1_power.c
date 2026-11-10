/* SPDX-License-Identifier: MIT */
/*
 * Stage 1: power up the GM20B through the Tegra210 CAR and PMC.  BAR0 is never touched here.
 *
 * Order (nouveau nvkm/engine/device/tegra.c, nvkm_device_tegra_power_up(), MIT) [B]:
 *   regulator -> clk "gpu" + clk "ref" + clk "pwr"(204 MHz) -> 10us -> reset assert -> 10us
 *   -> remove clamps -> 10us -> reset deassert -> 10us
 * Our situation differs from Linux in three deliberate ways:
 *   - no clock/reset/regulator framework: CAR is written ONLY through its write-1-to-set /
 *     write-1-to-clear alias registers (CAR is shared with another Windows device, "CPUC");
 *   - the 'pwr' clock (PLLP_OUT5, register shared with CPUC) is only READ and journaled;
 *   - the rail (MAX77621) is not touched unless the platform shell explicitly allows it.
 * Everything is journaled; each state-changing write is preceded by an already-flushed STEP.
 * Provenance + the list of touched registers: docs/provenance/s1_power.md
 */
#include "gm20b.h"
#include "tegra_car_pmc.h"

#define GPU_BIT       GM20B_BIT(CAR_X_GPU_BIT)
#define PLLG_BIT      GM20B_BIT(CAR_X_PLLG_REF_BIT)
#define CLK_BITS      (GPU_BIT | PLLG_BIT)

static u32 car_rd(struct gm20b *g, u32 off)
{
	return gm20b_rd(g, GM20B_SP_CAR, off);
}

static void car_wr(struct gm20b *g, u32 off, u32 v)
{
	gm20b_wr(g, GM20B_SP_CAR, off, v);
}

/* One durable intent line per register: if the bus hangs, the last line names the register that did it. */
static u32 probe_rd(struct gm20b *g, enum gm20b_space sp, u32 off)
{
	GM20B_STEP(g, J_S1_PROBE_RD, GM20B_JADDR(sp, off), 0);
	return gm20b_rd(g, sp, off);
}

/* read-only look at everything we depend on, before any write */
static void probe_pre_state(struct gm20b *g)
{
	struct gm20b_s1 *s = &g->s1;

	GM20B_STEP(g, J_S1_PROBE_BEGIN, 0, 0);
	s->pre_clk = probe_rd(g, GM20B_SP_CAR, CAR_CLK_OUT_ENB_X);
	gm20b_j(g, J_S1_PRE_CLK, CAR_CLK_OUT_ENB_X, s->pre_clk);
	s->pre_rst = probe_rd(g, GM20B_SP_CAR, CAR_RST_DEVICES_X);
	gm20b_j(g, J_S1_PRE_RST, CAR_RST_DEVICES_X, s->pre_rst);
	s->pre_rg = probe_rd(g, GM20B_SP_PMC, PMC_GPU_RG_CNTRL);
	gm20b_j(g, J_S1_PRE_CLAMP, PMC_GPU_RG_CNTRL, s->pre_rg);
	s->pre_pllp_base = probe_rd(g, GM20B_SP_CAR, CAR_PLLP_BASE);
	gm20b_j(g, J_S1_PRE_PLLP_BASE, CAR_PLLP_BASE, s->pre_pllp_base);
	s->pre_pllp_outc = probe_rd(g, GM20B_SP_CAR, CAR_PLLP_OUTC);
	gm20b_j(g, J_S1_PRE_PLLP_OUTC, CAR_PLLP_OUTC, s->pre_pllp_outc);
	gm20b_jflush(g);
}

/*
 * Informational: is PLLP_OUT5 (the GPU 'pwr' clock) running at what nouveau programs (204 MHz)?
 * Never written (no alias exists and the register is shared).  Stages 1-6 do not need the PMU;
 * stage 7 does and will insist.  Decode is confidence C: verify against the raw words.
 */
static void check_pwr_clock(struct gm20b *g)
{
	u32 base = g->s1.pre_pllp_base;
	u32 outc = g->s1.pre_pllp_outc;
	u32 ratio = (outc >> CAR_PLLP_OUT5_RATIO_SHIFT) & 0xffu;
	bool running = (base & GM20B_BIT(CAR_PLLP_BASE_ENABLE_BIT)) &&
		       (outc & GM20B_BIT(CAR_PLLP_OUT5_RSTN_BIT)) &&
		       (outc & GM20B_BIT(CAR_PLLP_OUT5_CLKEN_BIT));

	g->s1.pwr_khz = running ? (CAR_PLLP_ASSUMED_KHZ * 2u) / (ratio + 2u) : 0u;
	gm20b_j(g, J_S1_PWR_CLK, running ? 1u : 0u, g->s1.pwr_khz);
}

/* read back and verify; the GPU clock gate, reset release and clamp removal are essential */
static int verify_powered(struct gm20b *g)
{
	struct gm20b_s1 *s = &g->s1;
	u32 bad = 0;

	s->post_clk = car_rd(g, CAR_CLK_OUT_ENB_X);
	gm20b_j(g, J_S1_POST_CLK, CAR_CLK_OUT_ENB_X, s->post_clk);
	s->post_rst = car_rd(g, CAR_RST_DEVICES_X);
	gm20b_j(g, J_S1_POST_RST, CAR_RST_DEVICES_X, s->post_rst);
	s->post_rg = gm20b_rd(g, GM20B_SP_PMC, PMC_GPU_RG_CNTRL);
	gm20b_j(g, J_S1_POST_CLAMP, PMC_GPU_RG_CNTRL, s->post_rg);
	gm20b_j(g, J_S1_PLLG_REF, (s->post_clk & PLLG_BIT) ? 1u : 0u, 0);

	if (!(s->post_clk & GPU_BIT))
		bad |= 1u;
	if (s->post_rst & GPU_BIT)
		bad |= 2u;
	if (s->post_rg & GM20B_BIT(PMC_GPU_RG_CNTRL_GATED_BIT))
		bad |= 4u;
	if (bad) {
		GM20B_STEP(g, J_S1_VERIFY_FAIL, bad, 0);
		return GM20B_E_HW;
	}
	return GM20B_OK;
}

int gm20b_s1_init(struct gm20b *g)
{
	struct gm20b_s1 *s = &g->s1;
	int err;

	/* 1. refuse a bad rail request before anything at all is touched */
	if ((g->cfg.flags & GM20B_CF_ALLOW_RAIL) &&
	    (!g->hal.rail_gpu_set_uv || g->cfg.rail_uv < GM20B_RAIL_MIN_UV ||
	     g->cfg.rail_uv > GM20B_RAIL_MAX_UV)) {
		gm20b_j(g, J_S1_RAIL_REFUSED, g->cfg.rail_uv, g->hal.rail_gpu_set_uv ? 1u : 0u);
		return GM20B_E_INVAL;
	}

	/* 2. read-only pre-state */
	probe_pre_state(g);
	check_pwr_clock(g);
	if (g->cfg.flags & GM20B_CF_DIAG_ONLY) {
		gm20b_j(g, J_S1_DIAG_ONLY, 0, 0);
		return GM20B_OK;
	}

	/* 3. rail (optional platform service; default: untouched) */
	if (g->cfg.flags & GM20B_CF_ALLOW_RAIL) {
		GM20B_STEP(g, J_S1_RAIL_SET, g->cfg.rail_uv, 0);
		err = g->hal.rail_gpu_set_uv(g->hal.ctx, g->cfg.rail_uv);
		gm20b_j(g, J_S1_RAIL_DONE, g->cfg.rail_uv, (u32)err);
		if (err)
			return GM20B_E_IO;
		s->did |= GM20B_S1_DID_RAIL;
		gm20b_msleep(g, 2);                         /* rail ramp: [C] measure */
	} else {
		gm20b_j(g, J_S1_RAIL_SKIP, 0, 0);
	}

	/* 4. clocks "gpu" + "ref" (SET alias only); remember which ones were ours */
	s->clk_set_by_us = CLK_BITS & ~s->pre_clk;
	GM20B_STEP(g, J_S1_CLK_ENABLE, CAR_CLK_OUT_ENB_SET_X, CLK_BITS);
	car_wr(g, CAR_CLK_OUT_ENB_SET_X, CLK_BITS);
	s->did |= GM20B_S1_DID_CLK;
	gm20b_udelay(g, 10);                                /* [nouveau tegra.c] [B] */

	/* 5. reset assert -> clamp removal -> reset deassert, 10us apart */
	GM20B_STEP(g, J_S1_RST_ASSERT, CAR_RST_DEVICES_SET_X, GPU_BIT);
	car_wr(g, CAR_RST_DEVICES_SET_X, GPU_BIT);
	s->did |= GM20B_S1_DID_RST;
	gm20b_udelay(g, 10);

	GM20B_STEP(g, J_S1_CLAMP_REMOVE, PMC_GPU_RG_CNTRL, 0);
	gm20b_wr(g, GM20B_SP_PMC, PMC_GPU_RG_CNTRL, 0);     /* [pmc.c:727] [B] */
	s->did |= GM20B_S1_DID_CLAMP;
	gm20b_udelay(g, 10);

	GM20B_STEP(g, J_S1_RST_DEASSERT, CAR_RST_DEVICES_CLR_X, GPU_BIT);
	car_wr(g, CAR_RST_DEVICES_CLR_X, GPU_BIT);
	s->did |= GM20B_S1_DID_UNRST;
	gm20b_udelay(g, 10);

	/* 6. verify */
	return verify_powered(g);
}

/*
 * Hold the GPU in reset again and gate the clocks we turned on.  Never touches BAR0, never
 * re-clamps (nouveau's power_down does not either), never touches the rail or PLLP.
 */
void gm20b_s1_fini(struct gm20b *g)
{
	struct gm20b_s1 *s = &g->s1;

	if (!(s->did & (GM20B_S1_DID_CLK | GM20B_S1_DID_RST | GM20B_S1_DID_UNRST)))
		return;

	GM20B_STEP(g, J_S1_FINI_RST, CAR_RST_DEVICES_SET_X, GPU_BIT);
	car_wr(g, CAR_RST_DEVICES_SET_X, GPU_BIT);
	gm20b_udelay(g, 10);
	if (s->clk_set_by_us) {
		GM20B_STEP(g, J_S1_FINI_CLK, CAR_CLK_OUT_ENB_CLR_X, s->clk_set_by_us);
		car_wr(g, CAR_CLK_OUT_ENB_CLR_X, s->clk_set_by_us);
	}
	gm20b_j(g, J_S1_FINI_POST, car_rd(g, CAR_RST_DEVICES_X), car_rd(g, CAR_CLK_OUT_ENB_X));
	s->did = 0;
	s->clk_set_by_us = 0;
}
