/* SPDX-License-Identifier: MIT */
/*
 * Stage 5: instance memory, BAR1, GMMU (GPU virtual memory), FB/L2 flush.
 *
 * FROZEN API: other stages (6, 7, 8) code against the prototypes below.
 * The implementer may add fields to the structs and add new functions, but must
 * not change or remove an existing prototype / its meaning. Struct *internals*
 * are private to stage 5; other stages must only use the functions.
 *
 * Model: GPU physical == CPU physical (SMMU bypass). Buffers come from
 * hal.dma_alloc (uncached, contiguous). A VM maps GPU virtual addresses to such
 * buffers with 4 KiB pages. All sizes/addresses below are bytes.
 */
#ifndef GM20B_STAGE5_H
#define GM20B_STAGE5_H

#include "gm20b_types.h"
#include "gm20b_hal.h"

struct gm20b;

#define GM20B_PAGE_SHIFT 12u
#define GM20B_PAGE_SIZE  (1u << GM20B_PAGE_SHIFT)

/* gm20b_vm_map flags */
#define GM20B_VMAP_RO       GM20B_BIT(0)  /* read-only for the GPU */
#define GM20B_VMAP_VOL      GM20B_BIT(1)  /* volatile (bypass GPU L2) */
#define GM20B_VMAP_SYS_COH  GM20B_BIT(2)  /* coherent system-memory aperture (default: non-coherent) */

#define GM20B_VM_MAX_PT     64u           /* page tables one VM may own (fixed pool, no malloc) */

struct gm20b_vm {
	struct gm20b_dma pd;                     /* top-level directory (HW format) */
	u64  va_start;
	u64  va_size;
	u32  nr_pt;
	struct gm20b_dma pt[GM20B_VM_MAX_PT];    /* leaf/mid tables owned by this VM */
	u32  impl[16];                           /* stage-5 private */
};

struct gm20b_inst {
	struct gm20b_dma blk;                    /* 4 KiB instance block (RAMIN image) */
	u64 pd_phys;
};

/* Create/destroy a GPU VM covering [va_start, va_start+va_size). */
int  gm20b_vm_create(struct gm20b *g, struct gm20b_vm *vm, u64 va_start, u64 va_size);
void gm20b_vm_destroy(struct gm20b *g, struct gm20b_vm *vm);

/* Map [off, off+len) of 'd' at gpu_va (page aligned). */
int  gm20b_vm_map(struct gm20b *g, struct gm20b_vm *vm, u64 gpu_va,
		  const struct gm20b_dma *d, size_t off, size_t len, u32 flags);
int  gm20b_vm_unmap(struct gm20b *g, struct gm20b_vm *vm, u64 gpu_va, size_t len);

/* Invalidate the GMMU TLB for 'vm'. Call after map/unmap before the GPU uses it. */
int  gm20b_vm_flush(struct gm20b *g, struct gm20b_vm *vm);

/* Build a 4 KiB instance block that points at 'vm' (PDB, address limit). */
int  gm20b_inst_create(struct gm20b *g, struct gm20b_inst *ib, struct gm20b_vm *vm);
void gm20b_inst_destroy(struct gm20b *g, struct gm20b_inst *ib);

/* Make CPU writes visible to the GPU / GPU writes visible to the CPU. */
int  gm20b_fb_flush(struct gm20b *g);
int  gm20b_l2_flush(struct gm20b *g, bool invalidate);

struct gm20b_s5 {
	u32 ready;       /* 1 once S5 init completed */
	u32 impl[32];    /* stage-5 private */
};

int  gm20b_s5_init(struct gm20b *g);
void gm20b_s5_fini(struct gm20b *g);   /* must tolerate partial init */

#endif
