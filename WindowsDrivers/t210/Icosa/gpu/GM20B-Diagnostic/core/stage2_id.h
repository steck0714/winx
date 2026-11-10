/* SPDX-License-Identifier: MIT */
/* Stage 2: chip identity (BOOT0), PMC enable (read only), interrupt masks.  State + entry points. */
#ifndef GM20B_STAGE2_H
#define GM20B_STAGE2_H
#include "gm20b_types.h"
#include "gm20b_hal.h"
struct gm20b;

/*
 * Private cfg flag (stages 1/2 own bits 16..19): read BOOT0 even though CAR/PMC say the GPU is not
 * powered.  May hang the bus: only for the case where OUR reading of CAR/PMC turns out to be wrong.
 */
#define GM20B_CF_FORCE_BAR0   GM20B_BIT(16)

#define GM20B_CHIPSET_GM20B   0x12bu   /* [nvgpu hw_mc BOOT0 arch 0x12 | impl 0xb; nouveau chipset 0x12b] [A] */

struct gm20b_s2 {
	u32 boot0;          /* as read */
	u32 pmc_enable;     /* mc_enable as read (S2 enables nothing) */
	u32 gated_mask;     /* power check: 1 clock off, 2 in reset, 4 clamped (0 = powered) */
};

int  gm20b_s2_init(struct gm20b *g);
void gm20b_s2_fini(struct gm20b *g);   /* nothing to undo */

#endif
