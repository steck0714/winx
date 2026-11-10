/* SPDX-License-Identifier: MIT
 * WDDM allocation/handle ownership follows Microsoft's graphics-driver-samples
 * at de4a2161 and the documented Dxgkrnl contracts. See README for references.
 */
#include "allocation.h"
#include "adapter_abi.h"

#define TAG 0x41674754u
typedef struct _TG_GPU_RESOURCE {
    TG_GPU_CONTEXT_ENGINE *Engine;
    UINT Allocations,Builders;
} TG_GPU_RESOURCE;
typedef struct _TG_GPU_ALLOCATION {
    TG_GPU_CONTEXT_ENGINE *Engine;
    TG_GPU_RESOURCE *Resource;
    struct tg_gpu_allocation_request Request;
    SIZE_T Bytes;
    UINT Opens;
} TG_GPU_ALLOCATION;
typedef struct _TG_GPU_OPEN_ALLOCATION {
    TG_GPU_DEVICE *Device;
    TG_GPU_ALLOCATION *Allocation;
    BOOLEAN ReadOnly;
} TG_GPU_OPEN_ALLOCATION;

static NTSTATUS request(const DXGK_ALLOCATIONINFO *info,struct tg_gpu_allocation_request *r)
{
    if(!info->pPrivateDriverData || info->PrivateDriverDataSize!=sizeof(*r))
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    RtlCopyMemory(r,info->pPrivateDriverData,sizeof(*r));
    if(info->FlagsWddm2.CreateInVpr) return STATUS_INVALID_PARAMETER;
    if(r->version!=TG_GPU_ALLOCATION_ABI) return STATUS_GRAPHICS_DRIVER_MISMATCH;
    if((r->flags&~TG_GPU_ALLOCATION_CPU_VISIBLE) || !r->bytes || r->bytes>TG_GPU_VA_LIMIT)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}
static UINT64 allocation_size(UINT64 bytes)
{
    return (bytes+PAGE_SIZE-1)&~(UINT64)(PAGE_SIZE-1);
}

NTSTATUS APIENTRY TgGpuDdiCreateAllocation(HANDLE adapter,DXGKARG_CREATEALLOCATION *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;TG_GPU_RESOURCE *r=NULL;
    struct tg_gpu_allocation_request desc;TG_GPU_ALLOCATION *allocation;
    NTSTATUS status;KIRQL irql;UINT i,created=0;BOOLEAN new_resource=FALSE;
    UINT64 bytes=0;
    if(!e || !a || !e->Tables || e->Tables->SegmentId!=TG_GPU_EXPLICIT_TABLE_SEGMENT ||
        a->PrivateDriverDataSize || a->Flags.Reserved || (!a->Flags.Resource && a->hResource) ||
        (a->NumAllocations && !a->pAllocationInfo) || (!a->NumAllocations && !a->Flags.Resource))
        return STATUS_INVALID_PARAMETER;
    /* Validate all user descriptors before allocating or changing a resource. */
    for(i=0;i<a->NumAllocations;++i) a->pAllocationInfo[i].hAllocation=NULL;
    for(i=0;i<a->NumAllocations;++i) {
        status=request(&a->pAllocationInfo[i],&desc);
        if(!NT_SUCCESS(status)) return status;
        UINT64 size=allocation_size(desc.bytes);
        if(size>TG_GPU_MEMORY_BUDGET_BYTES-bytes) return STATUS_GRAPHICS_NO_VIDEO_MEMORY;
        bytes+=size;
    }
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY; }
    if(a->Flags.Resource && a->hResource) {
        r=a->hResource;
        if(r->Engine!=e) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER; }
    }
    /* Charge the batch before releasing the lock so concurrent devices
     * compete for the same UMA ceiling, including in-flight creates. */
    if(bytes>TG_GPU_MEMORY_BUDGET_BYTES-e->AllocationBytes) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_GRAPHICS_NO_VIDEO_MEMORY;
    }
    e->AllocationBytes+=bytes;
    if(r) ++r->Builders;
    ++e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    status=STATUS_NO_MEMORY;
    if(a->Flags.Resource && !r) {
        r=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*r),TAG);
        if(!r) goto fail;
        r->Engine=e;new_resource=TRUE;
    }
    for(i=0;i<a->NumAllocations;++i) {
        allocation=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*allocation),TAG);
        if(!allocation) { status=STATUS_NO_MEMORY;goto fail; }
        /* Dxgkrnl provides a stable kernel copy of the UMD data for this call. */
        RtlCopyMemory(&allocation->Request,a->pAllocationInfo[i].pPrivateDriverData,sizeof(desc));
        allocation->Bytes=(SIZE_T)allocation_size(allocation->Request.bytes);
        allocation->Engine=e;allocation->Resource=r;
        a->pAllocationInfo[i].hAllocation=allocation;++created;
    }
    for(i=0;i<a->NumAllocations;++i) {
        DXGK_ALLOCATIONINFO *info=&a->pAllocationInfo[i];
        PVOID private_data=info->pPrivateDriverData;UINT private_bytes=info->PrivateDriverDataSize;
        /* FlagsWddm2 is output except OS-reserved input bits in the WDK. */
        UINT os_flags=info->FlagsWddm2.Value&0xfff83800u;
        allocation=info->hAllocation;
        RtlZeroMemory(info,sizeof(*info));
        info->pPrivateDriverData=private_data;info->PrivateDriverDataSize=private_bytes;
        info->hAllocation=allocation;info->Size=allocation->Bytes;info->Alignment=PAGE_SIZE;
        /* VidMm redirects GpuMmu allocations from the aperture to implicit
         * system memory when AccessedPhysically is clear. */
        info->PreferredSegment.SegmentId0=TG_GPU_PAGING_APERTURE_SEGMENT;
        info->SupportedReadSegmentSet=info->SupportedWriteSegmentSet=
            1u<<(TG_GPU_PAGING_APERTURE_SEGMENT-1);
        info->FlagsWddm2.Value=os_flags;
        info->FlagsWddm2.CpuVisible=(allocation->Request.flags&TG_GPU_ALLOCATION_CPU_VISIBLE)!=0;
        info->FlagsWddm2.DisableLargePageMapping=1;
        info->AllocationPriority=D3DDDI_ALLOCATIONPRIORITY_NORMAL;
        /* No AccessedPhysically, cache-coherent or scanout claim. Pages may be
         * scattered; the ordinary GpuMmu PTE path maps VidMm-owned backing.
         */
    }
    KeAcquireSpinLock(&e->Lock,&irql);
    e->AllocationCount+=created;
    if(r) {
        r->Allocations+=created;
        if(new_resource) ++e->ResourceCount;else --r->Builders;
        a->hResource=r;
    }
    --e->Creating;KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
fail:
    for(i=0;i<created;++i) {
        ExFreePoolWithTag(a->pAllocationInfo[i].hAllocation,TAG);a->pAllocationInfo[i].hAllocation=NULL;
    }
    if(new_resource) ExFreePoolWithTag(r,TAG);
    KeAcquireSpinLock(&e->Lock,&irql);
    if(r && !new_resource) --r->Builders;
    e->AllocationBytes-=bytes;
    --e->Creating;KeReleaseSpinLock(&e->Lock,irql);return status;
}
NTSTATUS APIENTRY TgGpuDdiDestroyAllocation(HANDLE adapter,const DXGKARG_DESTROYALLOCATION *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;TG_GPU_RESOURCE *resource=NULL;
    TG_GPU_ALLOCATION *allocation;UINT i,j,resource_children=0;KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    if(!e || !a || a->Flags.Reserved || (a->NumAllocations && !a->pAllocationList))
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(a->Flags.DestroyResource) {
        resource=a->hResource;
        if(!resource || resource->Engine!=e) { status=STATUS_INVALID_PARAMETER;goto unlock; }
        if(resource->Builders) { status=STATUS_DEVICE_BUSY;goto unlock; }
    }
    /* Handles here are KMD objects supplied by Dxgkrnl. Do not create a second
     * global handle table or cast a UMD D3DKMT_HANDLE into one of these objects.
     */
    for(i=0;i<a->NumAllocations;++i) {
        allocation=a->pAllocationList[i];
        if(!allocation || allocation->Engine!=e) { status=STATUS_INVALID_PARAMETER;goto unlock; }
        if(allocation->Opens) { status=STATUS_DEVICE_BUSY;goto unlock; }
        for(j=0;j<i;++j) if(a->pAllocationList[j]==allocation) { status=STATUS_INVALID_PARAMETER;goto unlock; }
        if(resource && allocation->Resource==resource) ++resource_children;
    }
    if(resource && resource->Allocations!=resource_children) { status=STATUS_DEVICE_BUSY;goto unlock; }
    ++e->Creating; /* release callbacks/metadata frees must finish before stop */
    for(i=0;i<a->NumAllocations;++i) {
        allocation=a->pAllocationList[i];
        if(allocation->Resource) --allocation->Resource->Allocations;
        e->AllocationBytes-=allocation->Bytes;
        --e->AllocationCount;
    }
    if(resource) --e->ResourceCount;
unlock:
    KeReleaseSpinLock(&e->Lock,irql);
    if(!NT_SUCCESS(status)) return status;
    for(i=0;i<a->NumAllocations;++i) ExFreePoolWithTag(a->pAllocationList[i],TAG);
    if(resource) ExFreePoolWithTag(resource,TAG);
    KeAcquireSpinLock(&e->Lock,&irql);--e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    return STATUS_SUCCESS;
}
static void close_one(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_OPEN_ALLOCATION *open)
{
    KIRQL irql;
    KeAcquireSpinLock(&e->Lock,&irql);
    --open->Allocation->Opens;--open->Device->OpenAllocations;
    KeReleaseSpinLock(&e->Lock,irql);
    ExFreePoolWithTag(open,TAG);
}
NTSTATUS APIENTRY TgGpuDdiOpenAllocation(HANDLE device,const DXGKARG_OPENALLOCATION *a)
{
    TG_GPU_DEVICE *d=device;TG_GPU_CONTEXT_ENGINE *e;TG_GPU_OPEN_ALLOCATION *open;
    TG_GPU_ALLOCATION *allocation;KIRQL irql;NTSTATUS status=STATUS_SUCCESS;UINT i,created=0;
    DXGKARGCB_GETHANDLEDATA query={.Type=DXGK_HANDLE_ALLOCATION};
    if(!d || !a || !a->NumAllocations || !a->pOpenAllocation || a->PrivateDriverSize ||
        a->Flags.Reserved) return STATUS_INVALID_PARAMETER;
    e=d->Engine;
    if(!e->Dxgk.DxgkCbAcquireHandleData || !e->Dxgk.DxgkCbReleaseHandleData)
        return STATUS_INVALID_DEVICE_STATE;
    for(i=0;i<a->NumAllocations;++i) a->pOpenAllocation[i].hDeviceSpecificAllocation=NULL;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY; }
    ++d->OpenAllocations;++e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    for(i=0;i<a->NumAllocations;++i) {
        DXGKARGCB_RELEASEHANDLEDATA release={.Type=DXGK_HANDLE_ALLOCATION};
        open=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*open),TAG);
        if(!open) { status=STATUS_NO_MEMORY;break; }
        query.hObject=a->pOpenAllocation[i].hAllocation;
        allocation=e->Dxgk.DxgkCbAcquireHandleData(&query,&release.ReleaseHandle);
        if(allocation && release.ReleaseHandle && allocation->Engine==e) {
            open->Device=d;open->Allocation=allocation;open->ReadOnly=a->Flags.ReadOnly;
            KeAcquireSpinLock(&e->Lock,&irql);++allocation->Opens;++d->OpenAllocations;KeReleaseSpinLock(&e->Lock,irql);
            a->pOpenAllocation[i].hDeviceSpecificAllocation=open;++created;
        } else {
            ExFreePoolWithTag(open,TAG);
            status=STATUS_INVALID_PARAMETER;
        }
        /* The lookup reference is scoped to this DDI. Dxgkrnl owns the
         * binding and calls CloseAllocation before destroying its object. */
        if(release.ReleaseHandle) e->Dxgk.DxgkCbReleaseHandleData(release);
        if(!NT_SUCCESS(status))break;
    }
    if(!NT_SUCCESS(status)) for(i=0;i<created;++i) {
        close_one(e,a->pOpenAllocation[i].hDeviceSpecificAllocation);
        a->pOpenAllocation[i].hDeviceSpecificAllocation=NULL;
    }
    KeAcquireSpinLock(&e->Lock,&irql);--d->OpenAllocations;--e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    return status;
}
NTSTATUS APIENTRY TgGpuDdiCloseAllocation(HANDLE device,const DXGKARG_CLOSEALLOCATION *a)
{
    TG_GPU_DEVICE *d=device;TG_GPU_CONTEXT_ENGINE *e;TG_GPU_OPEN_ALLOCATION *open;
    UINT i,j;KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    if(!d || !a || (a->NumAllocations && !a->pOpenHandleList)) return STATUS_INVALID_PARAMETER;
    e=d->Engine;KeAcquireSpinLock(&e->Lock,&irql);
    for(i=0;i<a->NumAllocations;++i) {
        open=a->pOpenHandleList[i];
        if(!open || open->Device!=d || !open->Allocation || open->Allocation->Engine!=e) {
            status=STATUS_INVALID_PARAMETER;break;
        }
        for(j=0;j<i;++j) if(a->pOpenHandleList[j]==open) { status=STATUS_INVALID_PARAMETER;break; }
        if(!NT_SUCCESS(status)) break;
    }
    if(NT_SUCCESS(status)) { ++d->OpenAllocations;++e->Creating; }
    KeReleaseSpinLock(&e->Lock,irql);
    if(!NT_SUCCESS(status)) return status;
    for(i=0;i<a->NumAllocations;++i) close_one(e,a->pOpenHandleList[i]);
    KeAcquireSpinLock(&e->Lock,&irql);--d->OpenAllocations;--e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    return STATUS_SUCCESS;
}
NTSTATUS TgGpuValidateOpenAllocationRange(TG_GPU_DEVICE *d,HANDLE handle,
                                         UINT64 offset,UINT64 bytes,BOOLEAN write)
{
    TG_GPU_OPEN_ALLOCATION *open=handle;NTSTATUS status=STATUS_SUCCESS;KIRQL irql;
    if(!d || !open || !bytes) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&d->Engine->Lock,&irql);
    if(open->Device!=d || !open->Allocation || open->Allocation->Engine!=d->Engine)
        status=STATUS_INVALID_PARAMETER;
    else if(write && open->ReadOnly) status=STATUS_ACCESS_DENIED;
    else if(offset>open->Allocation->Bytes || bytes>open->Allocation->Bytes-offset)
        status=STATUS_INVALID_BUFFER_SIZE;
    KeReleaseSpinLock(&d->Engine->Lock,irql);return status;
}
void TgGpuRegisterAllocationDdis(DRIVER_INITIALIZATION_DATA *d)
{
    d->DxgkDdiCreateAllocation=TgGpuDdiCreateAllocation;
    d->DxgkDdiDestroyAllocation=TgGpuDdiDestroyAllocation;
    d->DxgkDdiOpenAllocation=TgGpuDdiOpenAllocation;
    d->DxgkDdiCloseAllocation=TgGpuDdiCloseAllocation;
}
NTSTATUS TgGpuValidatePagingAllocation(TG_GPU_CONTEXT_ENGINE *e,HANDLE handle,UINT64 offset,UINT64 bytes)
{
    TG_GPU_ALLOCATION *a=handle;NTSTATUS status=STATUS_SUCCESS;KIRQL irql;
    if(!e || !a)return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(a->Engine!=e)status=STATUS_INVALID_PARAMETER;
    else if(offset>a->Bytes || bytes>a->Bytes-offset)status=STATUS_INVALID_BUFFER_SIZE;
    KeReleaseSpinLock(&e->Lock,irql);return status;
}
