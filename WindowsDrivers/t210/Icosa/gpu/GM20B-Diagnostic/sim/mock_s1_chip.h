/* SPDX-License-Identifier: MIT */
/*
 * Chip power model for stage 1/2 tests (CAR bank X, PMC GPU clamp, BAR0 gating).
 * Written from the register SEMANTICS (write-1-to-set/clear aliases, reset bit, clamp bit), with its
 * own literal offsets on purpose: it must NOT include regs/tegra_car_pmc.h, so a typo there is caught.
 *
 * Hook ranges: CAR [0x000..0xfff] (whole space), PMC [0x000..0x3ff] (whole space).  BAR0: none
 * (plain storage; BOOT0 is preset with mock_set()).  BAR0 is "alive" only when
 *   rail_on && clock gate bit24 set && reset bit24 clear && clamp bit0 clear.
 * Violations recorded: direct writes to the shared CLK_OUT_ENB_X / RST_DEVICES_X, writes to any other
 * CAR register (e.g. PLLP), writes to any PMC register but GPU_RG_CNTRL, reads of the write-only aliases.
 */
#ifndef SIM_MOCK_S1_CHIP_H
#define SIM_MOCK_S1_CHIP_H

#include "mock.h"

struct mock_chip {
	struct mock *m;
	u32  clk_x;           /* CAR CLK_OUT_ENB_X as the silicon holds it */
	u32  rst_x;           /* CAR RST_DEVICES_X */
	u32  rg;              /* PMC GPU_RG_CNTRL */
	bool rail_on;         /* VDD_GPU present */
	bool pmc_wr_ignored;  /* models a secure-only PMC: writes silently dropped */
	u32  n_alias_wr;      /* writes seen on the SET/CLR aliases */
};

/* cold: clocks off, reset asserted, clamped.  warm: powered and running. */
void mock_chip_install(struct mock *m, struct mock_chip *c, bool cold);
/* re-evaluate BAR0 gating after a test edited the chip fields directly */
void mock_chip_update(struct mock_chip *c);

#endif
