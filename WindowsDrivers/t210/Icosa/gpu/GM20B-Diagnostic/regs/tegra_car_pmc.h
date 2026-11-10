/* SPDX-License-Identifier: MIT */
/*
 * Tegra210 CAR / PMC registers needed to power up the GPU (stage 1) and to check
 * that it is powered (stage 2).  Offsets are relative to the base of the HAL space
 * (CAR 0x60006000, PMC 0x7000e400).
 *
 * Provenance: the Linux clock/PMC drivers are GPL-2.0 and were used ONLY to confirm
 * addresses and bit positions (facts); no code was copied.  nouveau's
 * nvkm_device_tegra_power_up() (MIT) fixes the ORDER of the sequence.
 * Confidence labels as in docs/DESIGN.md.  Table: docs/provenance/s1_power.md.
 */
#ifndef GM20B_REGS_TEGRA_CAR_PMC_H
#define GM20B_REGS_TEGRA_CAR_PMC_H

/* ---- CAR bank "X" (clock/reset ids 160..191; bit = id - 160) ---------------- */
#define CAR_CLK_OUT_ENB_X       0x280u /* [linux drivers/clk/tegra/clk.h:18] [B] clock gates      */
#define CAR_CLK_OUT_ENB_SET_X   0x284u /* [clk.h:31] [B] write-1-to-set alias (write-only)        */
#define CAR_CLK_OUT_ENB_CLR_X   0x288u /* [clk.h:32] [B] write-1-to-clear alias (write-only)      */
#define CAR_RST_DEVICES_X       0x28cu /* [clk.h:41] [B] 1 = module held in reset                 */
#define CAR_RST_DEVICES_SET_X   0x290u /* [clk.h:53] [B] assert reset (write-only)                */
#define CAR_RST_DEVICES_CLR_X   0x294u /* [clk.h:54] [B] deassert reset (write-only)              */

/* [linux clk-tegra-periph.c:806 GATE("gpu","pll_ref",184,..); include/dt-bindings TEGRA210_CLK_GPU=184;
 *  tegra210.dtsi gpu node: clocks[0]=TEGRA210_CLK_GPU, resets=<&tegra_car 184>] [B]  184-160 = 24 */
#define CAR_X_GPU_BIT           24u
/* [clk-tegra-periph.c:807 GATE("pllg_ref","pll_ref",189,..); dtsi gpu clocks[2]=PLL_G_REF] [B]  189-160 = 29 */
#define CAR_X_PLLG_REF_BIT      29u

/* ---- CAR: the 'pwr' clock = PLLP_OUT5 (204 MHz in nouveau). READ-ONLY here: shared with CPUC. */
#define CAR_PLLP_BASE           0x0a0u /* [clk-tegra210.c:64] [B] */
#define CAR_PLLP_BASE_ENABLE_BIT 30u   /* [C] generic Tegra PLL base layout (informational decode only) */
#define CAR_PLLP_OUTC           0x67cu /* [clk-tegra-periph.c:257] [B] */
/* PLL_OUT(5, PLLP_OUTC, div_shift=24, 0, rst_shift=16) [clk-tegra-periph.c:857] [B];
 * the enable bit is rst_shift+1 in every Tegra PLL_OUT half-word [C]. */
#define CAR_PLLP_OUT5_RSTN_BIT   16u
#define CAR_PLLP_OUT5_CLKEN_BIT  17u
#define CAR_PLLP_OUT5_RATIO_SHIFT 24u  /* 7.1 fixed-point divider: rate = parent*2/(ratio+2) [C] */
#define CAR_PLLP_ASSUMED_KHZ    408000u /* [C] Tegra210 PLLP default 408 MHz; informational */
#define CAR_PWR_CLK_EXPECTED_KHZ 204000u /* [nouveau nvkm/engine/device/tegra.c: clk_set_rate(clk_pwr, 204000000)] [B] */

/* ---- PMC ------------------------------------------------------------------- */
/* tegra_pmc_powergate_remove_clamping(3D) on SoCs with has_gpu_clamps (Tegra210: true) writes 0 here.
 * [linux drivers/soc/tegra/pmc.c:176 (offset), :726-727 (write 0), :3891 (tegra210_pmc_soc.has_gpu_clamps)] [B] */
#define PMC_GPU_RG_CNTRL        0x2d4u
/* "powered" <=> (GPU_RG_CNTRL & 1) == 0  [pmc.c:594] [B] */
#define PMC_GPU_RG_CNTRL_GATED_BIT 0u

#endif /* GM20B_REGS_TEGRA_CAR_PMC_H */
