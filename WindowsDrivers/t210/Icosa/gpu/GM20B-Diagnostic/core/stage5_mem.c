/* SPDX-License-Identifier: MIT */
#include "gm20b.h"

/* STUB: replaced by the stage-5 implementation. Keep every prototype from stage5_mem.h. */
int gm20b_s5_init(struct gm20b *g) { (void)g; return GM20B_E_NOTIMPL; }
void gm20b_s5_fini(struct gm20b *g) { (void)g; }

int gm20b_vm_create(struct gm20b *g, struct gm20b_vm *vm, u64 va_start, u64 va_size)
{ (void)g; (void)vm; (void)va_start; (void)va_size; return GM20B_E_NOTIMPL; }
void gm20b_vm_destroy(struct gm20b *g, struct gm20b_vm *vm) { (void)g; (void)vm; }
int gm20b_vm_map(struct gm20b *g, struct gm20b_vm *vm, u64 gpu_va, const struct gm20b_dma *d,
		 size_t off, size_t len, u32 flags)
{ (void)g; (void)vm; (void)gpu_va; (void)d; (void)off; (void)len; (void)flags; return GM20B_E_NOTIMPL; }
int gm20b_vm_unmap(struct gm20b *g, struct gm20b_vm *vm, u64 gpu_va, size_t len)
{ (void)g; (void)vm; (void)gpu_va; (void)len; return GM20B_E_NOTIMPL; }
int gm20b_vm_flush(struct gm20b *g, struct gm20b_vm *vm) { (void)g; (void)vm; return GM20B_E_NOTIMPL; }
int gm20b_inst_create(struct gm20b *g, struct gm20b_inst *ib, struct gm20b_vm *vm)
{ (void)g; (void)ib; (void)vm; return GM20B_E_NOTIMPL; }
void gm20b_inst_destroy(struct gm20b *g, struct gm20b_inst *ib) { (void)g; (void)ib; }
int gm20b_fb_flush(struct gm20b *g) { (void)g; return GM20B_E_NOTIMPL; }
int gm20b_l2_flush(struct gm20b *g, bool invalidate) { (void)g; (void)invalidate; return GM20B_E_NOTIMPL; }
