/* SPDX-License-Identifier: MIT */
/* Stage 7: falcons: PMU, ACR secure boot, FECS/GPCCS.  State + entry points. */
#ifndef GM20B_STAGE7_H
#define GM20B_STAGE7_H
#include "gm20b_types.h"
#include "gm20b_hal.h"
struct gm20b;

struct gm20b_s7 {
	u32 reserved;   /* replace with the stage's real state */
};

int  gm20b_s7_init(struct gm20b *g);
void gm20b_s7_fini(struct gm20b *g);   /* must tolerate partial init */

#endif
