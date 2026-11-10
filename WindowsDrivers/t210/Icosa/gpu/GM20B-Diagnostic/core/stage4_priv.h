/* SPDX-License-Identifier: MIT */
/* Stage 4: priv ring, timer, fuses/floorsweeping, FB, L2, top-table.  State + entry points. */
#ifndef GM20B_STAGE4_H
#define GM20B_STAGE4_H
#include "gm20b_types.h"
#include "gm20b_hal.h"
struct gm20b;

struct gm20b_s4 {
	u32 reserved;   /* replace with the stage's real state */
};

int  gm20b_s4_init(struct gm20b *g);
void gm20b_s4_fini(struct gm20b *g);   /* must tolerate partial init */

#endif
