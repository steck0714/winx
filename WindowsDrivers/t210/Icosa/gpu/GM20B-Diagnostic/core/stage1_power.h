/* SPDX-License-Identifier: MIT */
/* Stage 1: power, clocks, reset (CAR + PMC only; BAR0 is never touched).  State + entry points. */
#ifndef GM20B_STAGE1_H
#define GM20B_STAGE1_H
#include "gm20b_types.h"
#include "gm20b_hal.h"
struct gm20b;

/* what this stage changed in hardware (so fini can undo exactly that) */
#define GM20B_S1_DID_RAIL    GM20B_BIT(0)
#define GM20B_S1_DID_CLK     GM20B_BIT(1)
#define GM20B_S1_DID_RST     GM20B_BIT(2)
#define GM20B_S1_DID_CLAMP   GM20B_BIT(3)
#define GM20B_S1_DID_UNRST   GM20B_BIT(4)

struct gm20b_s1 {
	u32 did;             /* GM20B_S1_DID_* */
	u32 clk_set_by_us;   /* CLK_OUT_ENB_X bits that were off before and that we turned on */
	u32 pre_clk, pre_rst, pre_rg, pre_pllp_base, pre_pllp_outc;
	u32 post_clk, post_rst, post_rg;
	u32 pwr_khz;         /* derived PLLP_OUT5 rate, 0 = not running (informational) */
};

int  gm20b_s1_init(struct gm20b *g);
void gm20b_s1_fini(struct gm20b *g);   /* must tolerate partial init */

#endif
