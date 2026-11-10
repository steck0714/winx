/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_GM20B_MMU_H
#define TEGRA_GPU_GM20B_MMU_H

#include <stdbool.h>
#include <stdint.h>

/* GM20B's 64 KiB big-page layout, using only its 4 KiB small pages initially.
 * A directory entry covers 64 MiB; the directory and each SPT are 128 KiB.
 * Small-page leaves can use pitch-linear or uncompressed block-linear kinds.
 */
#define TG_GPU_PAGE_SHIFT       12u
#define TG_GPU_TABLE_BITS       14u
#define TG_GPU_DIRECTORY_SHIFT  26u
#define TG_GPU_TABLE_ENTRIES    16384u
#define TG_GPU_TABLE_BYTES      131072u
#define TG_GPU_VA_LIMIT         (UINT64_C(1) << 40)
#define TG_GPU_PTE_KIND_PITCH           0u
#define TG_GPU_PTE_KIND_GENERIC_16BX2   0xfeu

struct tg_gpu_va_indices {
    uint32_t directory;
    uint32_t small_page;
};

bool tg_gpu_va_indices(uint64_t va, struct tg_gpu_va_indices *indices);

/* gpu_address is a device-visible address supplied by the DMA/IOMMU owner,
 * not a CPU pointer and not an assumed identity mapping of Windows PFNs.
 * The caller owns backing-page lifetime and cache/TLB maintenance.
 */
bool tg_gpu_pte_4k(uint64_t gpu_address, bool read_only, bool gpu_uncached,
                   uint64_t *pte);
bool tg_gpu_pte_4k_kind(uint64_t gpu_address, bool read_only, bool gpu_uncached,
                        uint8_t kind, uint64_t *pte);
bool tg_gpu_pde_small(uint64_t table_gpu_address, uint64_t *pde);
bool tg_gpu_instance_pdb(uint64_t directory_gpu_address, uint64_t *pdb);

#endif
