/* SPDX-License-Identifier: MIT */
/* Stage 3: GPC clock (GPCPLL).  State + entry points. */
#ifndef GM20B_STAGE3_H
#define GM20B_STAGE3_H
#include "gm20b_types.h"
#include "gm20b_hal.h"
struct gm20b;

struct gm20b_s3 {
	u32 reserved;   /* replace with the stage's real state */
};

int  gm20b_s3_init(struct gm20b *g);
void gm20b_s3_fini(struct gm20b *g);   /* must tolerate partial init */

#endif
