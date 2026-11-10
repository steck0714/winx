/* SPDX-License-Identifier: MIT */
#include "gm20b.h"

/* STUB: replaced by the stage-6 implementation. Keep every prototype from stage6_fifo.h. */
int gm20b_s6_init(struct gm20b *g) { (void)g; return GM20B_E_NOTIMPL; }
void gm20b_s6_fini(struct gm20b *g) { (void)g; }

int gm20b_chan_create(struct gm20b *g, struct gm20b_chan *ch, struct gm20b_vm *vm, u32 engine, u32 gp_entries)
{ (void)g; (void)ch; (void)vm; (void)engine; (void)gp_entries; return GM20B_E_NOTIMPL; }
void gm20b_chan_destroy(struct gm20b *g, struct gm20b_chan *ch) { (void)g; (void)ch; }
int gm20b_chan_push(struct gm20b *g, struct gm20b_chan *ch, u64 pb_gpu_va, u32 pb_dwords)
{ (void)g; (void)ch; (void)pb_gpu_va; (void)pb_dwords; return GM20B_E_NOTIMPL; }
int gm20b_chan_wait_idle(struct gm20b *g, struct gm20b_chan *ch, u32 timeout_us)
{ (void)g; (void)ch; (void)timeout_us; return GM20B_E_NOTIMPL; }
int gm20b_chan_selftest_sem(struct gm20b *g) { (void)g; return GM20B_E_NOTIMPL; }
