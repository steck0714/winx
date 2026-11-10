/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_ROOT_TABLE_H
#define TEGRA_GPU_ROOT_TABLE_H
#include "table_segment.h"
#include "../platform/t214_mmu.h"

typedef struct _TG_GPU_ROOT_TABLE {
    D3DGPU_PHYSICAL_ADDRESS Address;
    UINT Entries;
    SIZE_T Bytes;
    UINT64 DeviceAddress;
    UINT64 VaLimit;
    BOOLEAN Valid;
} TG_GPU_ROOT_TABLE;

/* Helpers for the WDDM two-level root-table DDIs. The adapter owns process
 * and context handle validation; these helpers do not register an adapter.
 * A root move is resolved before publishing the new binding. On failure the
 * context must remain unschedulable (Root.Valid is cleared), not reuse its
 * old binding after VidMm may have moved/freed the old allocation.
 */
SIZE_T TgGpuRootTableSize(DXGKARG_GETROOTPAGETABLESIZE *Args);
NTSTATUS TgGpuResolveRootTable(const TG_GPU_TABLE_SEGMENT *Segment,
    const DXGKARG_SETROOTPAGETABLE *Args, TG_GPU_ROOT_TABLE *Root);

/* Caller serializes this with submission and holds the context idle until
 * instance invalidation/rebind is complete. Instance is an owned uncached
 * private allocation; only RAMIN PDB/VA-limit words are changed here.
 */
NTSTATUS TgGpuWriteRootInstance(const TG_GPU_ROOT_TABLE *Root,
    const TG_GPU_PRIVATE_MEMORY *Instance, BOOLEAN ContextIdle);

/* Execute at the paging fence's ordered submission point, not while merely
 * building a future paging buffer. Keep old table/data pages resident until
 * this returns success or a successful hardware drain/reset retires them.
 * Caller owns GPU power and serializes the invalidate registers.
 */
NTSTATUS TgGpuFlushRootTable(const TG_GPU_ROOT_TABLE *Root,
    const DXGK_BUILDPAGINGBUFFER_FLUSHTLB *Args,
    const struct tg_gpu_power_io *Io, const struct tg_gpu_power *Power);
#endif
