/* SPDX-License-Identifier: MIT */
/* RAMIN PDB/limit layout follows nouveau v6.12 gm200_vmm_join_ and Switchroot
 * gk20a_init_inst_block. See NOTICE for the upstream license notices.
 */
#include "root_table.h"

SIZE_T TgGpuRootTableSize(DXGKARG_GETROOTPAGETABLESIZE *Args)
{
    UINT entries;
    const UINT per_page = PAGE_SIZE / sizeof(UINT64);
    if (!Args || Args->PhysicalAdapterIndex || Args->NumberOfPte > TG_GPU_TABLE_ENTRIES)
        return 0;
    entries = Args->NumberOfPte ? Args->NumberOfPte : 1;
    entries = (entries + per_page - 1) & ~(per_page - 1);
    Args->NumberOfPte = entries;
    return (SIZE_T)entries * sizeof(UINT64);
}

NTSTATUS TgGpuResolveRootTable(const TG_GPU_TABLE_SEGMENT *Segment,
    const DXGKARG_SETROOTPAGETABLE *Args, TG_GPU_ROOT_TABLE *Root)
{
    TG_GPU_ROOT_TABLE next = {0};
    DXGKARG_GETROOTPAGETABLESIZE size = {0};
    NTSTATUS status;
    if (!Root) return STATUS_INVALID_PARAMETER;
    Root->Valid = FALSE;
    if (!Args || !Args->NumEntries || Args->Address.Padding)
        return STATUS_INVALID_PARAMETER;
    size.NumberOfPte = Args->NumEntries;
    next.Bytes = TgGpuRootTableSize(&size);
    if (!next.Bytes) return STATUS_INVALID_PARAMETER;
    status = TgGpuResolveTableSegment((PVOID)Segment, Args->Address.SegmentId,
        Args->Address.SegmentOffset, next.Bytes, &next.DeviceAddress);
    if (!NT_SUCCESS(status)) return status;
    next.Address = Args->Address;
    next.Entries = Args->NumEntries;
    next.VaLimit = (UINT64)next.Entries << TG_GPU_DIRECTORY_SHIFT;
    next.Valid = TRUE;
    *Root = next;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuWriteRootInstance(const TG_GPU_ROOT_TABLE *Root,
    const TG_GPU_PRIVATE_MEMORY *Instance, BOOLEAN ContextIdle)
{
    UINT64 pdb, limit;
    volatile UINT32 *words;
    if (!Root || !Root->Valid || !Root->Entries || Root->Entries > TG_GPU_TABLE_ENTRIES ||
        Root->VaLimit != (UINT64)Root->Entries << TG_GPU_DIRECTORY_SHIFT ||
        !Instance || !Instance->Handle || !Instance->CpuAddress ||
        ((ULONG_PTR)Instance->CpuAddress & (PAGE_SIZE - 1)) || Instance->Bytes < PAGE_SIZE ||
        !tg_gpu_instance_pdb(Root->DeviceAddress, &pdb))
        return STATUS_INVALID_PARAMETER;
    if (!ContextIdle) return STATUS_GRAPHICS_ALLOCATION_BUSY;
    words = Instance->CpuAddress;
    limit = Root->VaLimit - 1;
    words[128] = (UINT32)pdb | 4; /* volatile PDB, 64 KiB big-page layout */
    words[129] = (UINT32)(pdb >> 32);
    words[130] = (UINT32)limit & ~0xfffu;
    words[131] = (UINT32)(limit >> 32);
    KeMemoryBarrier();
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuFlushRootTable(const TG_GPU_ROOT_TABLE *Root,
    const DXGK_BUILDPAGINGBUFFER_FLUSHTLB *Args,
    const struct tg_gpu_power_io *Io, const struct tg_gpu_power *Power)
{
    enum tg_gpu_tlb_result result;
    if (!Root || !Root->Valid || !Args || Args->RootPageTableAddress.Padding ||
        Args->RootPageTableAddress.SegmentId != Root->Address.SegmentId ||
        Args->RootPageTableAddress.SegmentOffset != Root->Address.SegmentOffset ||
        ((Args->StartVirtualAddress || Args->EndVirtualAddress) &&
         (Args->StartVirtualAddress >= Root->VaLimit ||
          Args->EndVirtualAddress < Args->StartVirtualAddress ||
          Args->EndVirtualAddress > Root->VaLimit)))
        return STATUS_INVALID_PARAMETER;
    /* Full-root invalidate covers the requested subrange too. Publish all
     * uncached CPU PTE writes before issuing the actual hardware operation.
     */
    KeMemoryBarrier();
    result = tg_gpu_tlb_invalidate(Io, Power, Root->DeviceAddress);
    switch (result) {
    case TG_GPU_TLB_OK: return STATUS_SUCCESS;
    case TG_GPU_TLB_NOT_READY: return STATUS_DEVICE_NOT_READY;
    case TG_GPU_TLB_NO_SPACE:
    case TG_GPU_TLB_TIMEOUT: return STATUS_IO_TIMEOUT;
    case TG_GPU_TLB_IO_ERROR: return STATUS_DEVICE_DATA_ERROR;
    default: return STATUS_INVALID_PARAMETER;
    }
}
