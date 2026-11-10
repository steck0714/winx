/* SPDX-License-Identifier: MIT */
/* GM20B entry formats adapted from Linux nouveau v6.12:
 * vmmgf100.c, vmmgk20a.c, vmmgm200.c and vmmgm20b.c.
 * Copyright 2017 Red Hat Inc. See ../NOTICE for the upstream MIT notice.
 * Copyright 2026 ruje0504-2
 */
#include "gm20b_mmu.h"

static bool
page_address_valid(uint64_t address)
{
    return address < TG_GPU_VA_LIMIT && (address & 0xfffu) == 0;
}

bool
tg_gpu_va_indices(uint64_t va, struct tg_gpu_va_indices *indices)
{
    if (!indices || !page_address_valid(va))
        return false;
    indices->directory = (uint32_t)(va >> TG_GPU_DIRECTORY_SHIFT);
    indices->small_page = (uint32_t)((va >> TG_GPU_PAGE_SHIFT) & 0x3fffu);
    return true;
}

bool
tg_gpu_pte_4k_kind(uint64_t gpu_address, bool read_only, bool gpu_uncached,
                   uint8_t kind, uint64_t *pte)
{
    if (!pte || !page_address_valid(gpu_address) ||
        (kind != TG_GPU_PTE_KIND_PITCH && kind != TG_GPU_PTE_KIND_GENERIC_16BX2))
        return false;

    /* gk20a_vmm_aper(NCOH) returns 0 on GM20B. Using the discrete-GPU
     * SYS_NCOH aperture (3 << 33) here would address the wrong aperture.
     * Maxwell's kind occupies bits 43:36. VOL bypasses the GPU cache.
     */
    *pte = (gpu_address >> 8) | UINT64_C(1) |
           ((uint64_t)read_only << 2) | ((uint64_t)gpu_uncached << 32) |
           ((uint64_t)kind << 36);
    return true;
}

bool
tg_gpu_pte_4k(uint64_t gpu_address, bool read_only, bool gpu_uncached,
              uint64_t *pte)
{
    return tg_gpu_pte_4k_kind(gpu_address, read_only, gpu_uncached, TG_GPU_PTE_KIND_PITCH, pte);
}

bool
tg_gpu_pde_small(uint64_t table_gpu_address, uint64_t *pde)
{
    if (!pde || !page_address_valid(table_gpu_address))
        return false;

    /* Switchroot GM20B has honors_aperture=false: SoC DRAM uses the
     * VIDMEM selector (1 for a PDE), plus VOL (4). NCOH=3 is the discrete
     * GPU encoding, not the result of nvgpu_aperture_mask on this SoC.
     * The lower half remains absent so the SPT is selected.
     */
    *pde = (table_gpu_address << 24) | (UINT64_C(5) << 32);
    return true;
}

bool
tg_gpu_instance_pdb(uint64_t directory_gpu_address, uint64_t *pdb)
{
    if (!pdb || !page_address_valid(directory_gpu_address))
        return false;

    /* RAMIN +0x200: VIDMEM target 0, volatile PDB (4), 64 KiB big pages
     * (bit 11). Match Switchroot's resolved GM20B aperture policy.
     * RAMIN +0x208 must separately contain the inclusive VA limit.
     */
    *pdb = directory_gpu_address | UINT64_C(0x804);
    return true;
}
