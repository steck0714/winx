/* SPDX-License-Identifier: MIT */
/*
 * Stage 2: identify the chip and put the GPU interrupt masks into a known state.
 *
 * This is the FIRST BAR0 access of the whole bring-up, i.e. the place where a wrong theory about
 * clocks/reset/clamps shows up as a bus hang.  Therefore:
 *   - before touching BAR0 the CAR/PMC power state is re-read and checked (guard);  if it says
 *     "not powered" BAR0 is NOT touched and the stage fails with GM20B_E_STATE.  GM20B_CF_FORCE_BAR0
 *     bypasses the guard (for the case that our reading of CAR/PMC is what is wrong);
 *   - an intent line (STEP) is flushed to disk right before the BOOT0 read.
 * Register facts come from the vendored nvgpu header regs/nvgpu/hw_mc_gm20b.h.
 * Provenance: docs/provenance/s2_id.md
 */
#include "gm20b.h"
#include "tegra_car_pmc.h"
#include "nvgpu/hw_mc_gm20b.h"

/* returns a mask: 1 = GPU clock gate off, 2 = GPU in reset, 4 = clamped/rail-gated; 0 = powered */
static u32 power_check(struct gm20b *g)
{
	u32 clk = gm20b_rd(g, GM20B_SP_CAR, CAR_CLK_OUT_ENB_X);
	u32 rst = gm20b_rd(g, GM20B_SP_CAR, CAR_RST_DEVICES_X);
	u32 rg = gm20b_rd(g, GM20B_SP_PMC, PMC_GPU_RG_CNTRL);
	u32 bad = 0;

	gm20b_j(g, J_S2_GUARD_CLK, CAR_CLK_OUT_ENB_X, clk);
	gm20b_j(g, J_S2_GUARD_RST, CAR_RST_DEVICES_X, rst);
	gm20b_j(g, J_S2_GUARD_CLAMP, PMC_GPU_RG_CNTRL, rg);
	if (!(clk & GM20B_BIT(CAR_X_GPU_BIT)))
		bad |= 1u;
	if (rst & GM20B_BIT(CAR_X_GPU_BIT))
		bad |= 2u;
	if (rg & GM20B_BIT(PMC_GPU_RG_CNTRL_GATED_BIT))
		bad |= 4u;
	return bad;
}

/* journal the four MC interrupt registers; returns the OR of their values */
static u32 probe_intr(struct gm20b *g, u32 code)
{
	u32 off[4], acc = 0, v, i;

	off[0] = mc_intr_en_0_r();     /* 0x140 */
	off[1] = mc_intr_en_1_r();     /* 0x144 */
	off[2] = mc_intr_mask_0_r();   /* 0x640 */
	off[3] = mc_intr_mask_1_r();   /* 0x644 */
	for (i = 0; i < 4; i++) {
		v = GRD(g, off[i]);
		gm20b_j(g, code, off[i], v);
		acc |= v;
	}
	return acc;
}

int gm20b_s2_init(struct gm20b *g)
{
	struct gm20b_s2 *s = &g->s2;
	u32 bad, boot0, arch, impl, chipset, chipset_nv, rev, left;

	/* 1. guard: never touch BAR0 while CAR/PMC say the GPU is not powered */
	bad = power_check(g);
	s->gated_mask = bad;
	if (bad) {
		if (!(g->cfg.flags & GM20B_CF_FORCE_BAR0)) {
			gm20b_j(g, J_S2_GATED, bad, 0);
			return GM20B_E_STATE;
		}
		gm20b_j(g, J_S2_FORCED, bad, 0);
	}

	/* 2. BOOT0: the first BAR0 access.  Intent first, result right after (both flushed). */
	GM20B_STEP(g, J_S2_BOOT0_INTENT, GM20B_JADDR(GM20B_SP_BAR0, mc_boot_0_r()), 0);
	boot0 = GRD(g, mc_boot_0_r());
	GM20B_STEP(g, J_S2_BOOT0, boot0, 0);
	s->boot0 = boot0;
	g->info.boot0 = boot0;

	if (GM20B_IS_DEAD(boot0)) {                 /* 0xffffffff or 0xbadfXXXX: bus fault pattern */
		(void)power_check(g);               /* make the failed run self-explanatory */
		gm20b_j(g, J_S2_BAD_BOOT0, boot0, (u32)GM20B_E_IO);
		return GM20B_E_IO;
	}
	if (boot0 == 0) {
		(void)power_check(g);
		gm20b_j(g, J_S2_BAD_BOOT0, boot0, (u32)GM20B_E_NODEV);
		return GM20B_E_NODEV;
	}

	/* 3. decode: nvgpu fields [A] cross-checked against nouveau's mask [A] */
	arch = mc_boot_0_architecture_v(boot0);                   /* [28:24] */
	impl = mc_boot_0_implementation_v(boot0);                 /* [23:20] */
	chipset = (arch << 4) | impl;
	chipset_nv = (boot0 & 0x1ff00000u) >> 20;                 /* nvkm_device_ctor */
	rev = (mc_boot_0_major_revision_v(boot0) << 4) | mc_boot_0_minor_revision_v(boot0);
	gm20b_j(g, J_S2_DECODE, chipset, chipset_nv);
	g->info.chipset = chipset;
	g->info.chiprev = rev;
	if (chipset != chipset_nv || chipset != GM20B_CHIPSET_GM20B) {
		(void)power_check(g);
		gm20b_j(g, J_S2_WRONG_CHIP, chipset, GM20B_CHIPSET_GM20B);
		return GM20B_E_NODEV;
	}

	/* 4. PMC_ENABLE: look only.  nvgpu enables engines where they are initialised (fifo/gr/pwr/ce),
	 *    not in the power-on prologue, so S2 enables nothing. */
	s->pmc_enable = GRD(g, mc_enable_r());
	gm20b_j(g, J_S2_PMC_ENABLE, mc_enable_r(), s->pmc_enable);

	/* 5. interrupts: we poll, so every GPU interrupt source stays masked at the MC */
	(void)probe_intr(g, J_S2_INTR_PRE);
	if (g->cfg.flags & GM20B_CF_DIAG_ONLY) {
		gm20b_j(g, J_S2_DIAG_ONLY, 0, 0);
		return GM20B_OK;
	}
	gm20b_j(g, J_S2_INTR_MASK, 0, 0);
	GWR(g, mc_intr_en_0_r(), mc_intr_en_0_inta_disabled_f());
	GWR(g, mc_intr_en_1_r(), mc_intr_en_1_inta_disabled_f());
	GWR(g, mc_intr_mask_0_r(), 0);
	GWR(g, mc_intr_mask_1_r(), 0);
	left = probe_intr(g, J_S2_INTR_POST);
	if (left)
		gm20b_j(g, J_S2_INTR_NOT_MASKED, 0, left);        /* non-fatal: nobody services them anyway */
	return GM20B_OK;
}

void gm20b_s2_fini(struct gm20b *g)
{
	(void)g;   /* masked interrupts are the safe state; nothing to undo */
}
