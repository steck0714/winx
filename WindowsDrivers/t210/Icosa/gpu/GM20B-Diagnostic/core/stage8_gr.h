/* SPDX-License-Identifier: MIT */
/* Stage 8: graphics/compute engine (GR) init and smoke test.  State + entry points. */
#ifndef GM20B_STAGE8_H
#define GM20B_STAGE8_H
#include "gm20b_types.h"
#include "gm20b_hal.h"
struct gm20b;

struct gm20b_s8 {
	u32 reserved;   /* replace with the stage's real state */
};

int  gm20b_s8_init(struct gm20b *g);
void gm20b_s8_fini(struct gm20b *g);   /* must tolerate partial init */

#endif
