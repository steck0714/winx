/* SPDX-License-Identifier: MIT */
/*
 * Stage 6: PFIFO / PBDMA / runlists / channels, and a host-only smoke test.
 *
 * FROZEN API: stages 7 and 8 code against the prototypes below (same rule as
 * stage5_mem.h: you may add, you may not change or remove).
 *
 * A channel = instance block + GPFIFO ring + USERD page, living in a VM from
 * stage 5. Host methods (e.g. SEMAPHORE_*) run on the PBDMA without any engine
 * firmware, which is what makes the stage-6 smoke test possible before ACR/GR.
 */
#ifndef GM20B_STAGE6_H
#define GM20B_STAGE6_H

#include "gm20b_types.h"
#include "gm20b_hal.h"
#include "stage5_mem.h"

struct gm20b;

enum gm20b_engine {
	GM20B_ENG_GR  = 0,   /* graphics/compute (needs stage 7+8) */
	GM20B_ENG_CE2 = 1,   /* copy engine 2 */
	GM20B_ENG_NONE = 2,  /* host-only channel (smoke tests) */
};

struct gm20b_chan {
	struct gm20b_vm  *vm;
	struct gm20b_inst inst;
	struct gm20b_dma  gpfifo;     /* GPFIFO ring (8 bytes/entry) */
	struct gm20b_dma  userd;      /* USERD page */
	u32  chid;
	u32  engine;
	u32  gp_entries;              /* power of two */
	u32  gp_put;
	u32  impl[16];                /* stage-6 private */
};

/* Create a channel in 'vm' with a GPFIFO of gp_entries entries (power of two). */
int  gm20b_chan_create(struct gm20b *g, struct gm20b_chan *ch, struct gm20b_vm *vm,
		       u32 engine, u32 gp_entries);
void gm20b_chan_destroy(struct gm20b *g, struct gm20b_chan *ch);

/* Queue one pushbuffer segment (GPU VA, length in dwords) and kick the channel. */
int  gm20b_chan_push(struct gm20b *g, struct gm20b_chan *ch, u64 pb_gpu_va, u32 pb_dwords);

/* Wait until the PBDMA consumed everything pushed so far (GP_GET == GP_PUT). */
int  gm20b_chan_wait_idle(struct gm20b *g, struct gm20b_chan *ch, u32 timeout_us);

/*
 * Host-only smoke test: build a VM + channel, push a SEMAPHORE release into a
 * system-memory word, poll for the payload. Returns GM20B_OK on success.
 */
int  gm20b_chan_selftest_sem(struct gm20b *g);

struct gm20b_s6 {
	u32 ready;
	u32 selftest_result;   /* GM20B_OK / error from the last smoke test, or 1 = not run */
	u32 impl[32];          /* stage-6 private */
};

int  gm20b_s6_init(struct gm20b *g);
void gm20b_s6_fini(struct gm20b *g);   /* must tolerate partial init */

#endif
