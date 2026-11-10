/* SPDX-License-Identifier: MIT */
#include "submit_ddi.h"
#include "work_queue.h"
#include "hardware.h"

NTSTATUS APIENTRY TgGpuDdiSubmitCommandVirtual(HANDLE adapter,const DXGKARG_SUBMITCOMMANDVIRTUAL *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;TG_GPU_QUEUE_ENTRY *reserved;TG_GPU_HARDWARE *h;
    TG_GPU_WORK_QUEUE *q;TG_GPU_COMMAND_SNAPSHOT *snapshot=NULL;
    NTSTATUS status;BOOLEAN used=FALSE;KIRQL irql;
    PAGED_CODE();
    if(!e || !a || !e->Dxgk.DeviceHandle)return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);++e->Creating;q=e->QueueOwner;KeReleaseSpinLock(&e->Lock,irql);
    if(q) {
        status=KeWaitForSingleObject(&q->DispatchGate,Executive,KernelMode,FALSE,NULL);
        if(status!=STATUS_SUCCESS) {
            (VOID)TgGpuRequestEngineRecovery(e,NT_SUCCESS(status)?STATUS_DEVICE_BUSY:status);
            KeAcquireSpinLock(&e->Lock,&irql);--e->Creating;KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
        }
    }
    /* Reserve before inspecting the submission. Allocation failure is an
     * internal admission failure, not a malformed-packet diagnosis. Accept
     * into recovery without touching commands or reporting completion. */
    reserved=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*reserved),TG_GPU_QUEUE_TAG);
    h=e->HardwareOwner;
    if(!reserved)status=STATUS_NO_MEMORY;
    else if(e->Resetting || e->RecoveryPending || !q || q->Engine!=e ||
       !h || h->Engine!=e || !h->Ready || !h->Power.owned || !h->Power.ready)
        status=STATUS_DEVICE_NOT_READY;
    else {
        BOOLEAN application=!a->Flags.Paging && !a->Flags.ContextSwitch &&
            !a->Flags.NullRendering && a->DmaBufferSize;
        status=application && !e->PagingOnly?TgGpuCaptureCommandSnapshot(a,&snapshot):STATUS_SUCCESS;
        if(status==STATUS_SUCCESS || status==STATUS_INVALID_PARAMETER)
            status=TgGpuQueueSubmitOrReject(q,a,reserved,snapshot,&used);
    }
    /* Only a successful application admission owns the captured bytes. An
     * ordered rejection marker can own Reserved without owning Snapshot. */
    if(snapshot && status!=STATUS_SUCCESS)TgGpuFreeCommandSnapshot(snapshot);
    if(reserved && !used)ExFreePoolWithTag(reserved,TG_GPU_QUEUE_TAG);
    if(status==STATUS_INVALID_PARAMETER && used) {
        NTSTATUS drain=TgGpuDrainRejectedIdle(q);
        if(!NT_SUCCESS(drain))(VOID)TgGpuRequestEngineRecovery(e,drain);
    } else if(status!=STATUS_SUCCESS)(VOID)TgGpuRequestEngineRecovery(e,status);
    if(q)(VOID)KeReleaseMutex(&q->DispatchGate,FALSE);
    KeAcquireSpinLock(&e->Lock,&irql);--e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    if(status==STATUS_INVALID_PARAMETER && used)return STATUS_INVALID_PARAMETER;
    /* Busy, OOM, lost root and transport failures must never escape this DDI:
     * Microsoft permits only SUCCESS or malformed INVALID_PARAMETER here. */
    return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiPreemptCommand(HANDLE adapter,const DXGKARG_PREEMPTCOMMAND *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;TG_GPU_WORK_QUEUE *q;TG_GPU_QUEUE_ENTRY *entry=NULL;
    KIRQL irql;NTSTATUS status;
    /* Nonpageable DISPATCH_LEVEL callback: no dispatcher mutex or GPU waits.
     * WDDM requires SUCCESS even on an internal failure; request recovery
     * instead of inventing a preempt completion or returning a bugcheck code. */
    if(!e)return STATUS_SUCCESS;
    KeAcquireSpinLock(&e->Lock,&irql);++e->Creating;q=e->QueueOwner;
    status=e->Stopping?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS;KeReleaseSpinLock(&e->Lock,irql);
    if(!a || a->NodeOrdinal || a->EngineOrdinal || a->Flags.Value)status=STATUS_INVALID_PARAMETER;
    if(NT_SUCCESS(status)) {
        entry=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*entry),TG_GPU_QUEUE_TAG);
        status=entry?TgGpuQueuePreempt(q,a,entry):STATUS_NO_MEMORY;
        if(entry && !NT_SUCCESS(status))ExFreePoolWithTag(entry,TG_GPU_QUEUE_TAG);
    }
    if(!NT_SUCCESS(status))(VOID)TgGpuRequestEngineRecovery(e,status);
    KeAcquireSpinLock(&e->Lock,&irql);--e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    return STATUS_SUCCESS;
}
void TgGpuRegisterSubmissionDdi(DRIVER_INITIALIZATION_DATA *d)
{
    d->DxgkDdiSubmitCommandVirtual=TgGpuDdiSubmitCommandVirtual;
    d->DxgkDdiPreemptCommand=TgGpuDdiPreemptCommand;
}
