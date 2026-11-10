/* SPDX-License-Identifier: MIT */
#include "work_queue.h"

static VOID free_entry(TG_GPU_QUEUE_ENTRY *entry)
{
    TgGpuFreeCommandSnapshot(entry->Snapshot);
    ExFreePoolWithTag(entry,TG_GPU_QUEUE_TAG);
}

NTSTATUS TgGpuInitializeWorkQueue(TG_GPU_WORK_QUEUE *q,TG_GPU_CONTEXT_ENGINE *e)
{
    KIRQL irql;
    if(!q || !e || !e->Commands.Handle) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping || e->QueueOwner || e->Active || e->Notification) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_BUSY;
    }
    RtlZeroMemory(q,sizeof(*q));q->Engine=e;
    KeInitializeSpinLock(&q->Lock);InitializeListHead(&q->Pending);
    KeInitializeMutex(&q->DispatchGate,0);
    KeInitializeEvent(&q->Wake,SynchronizationEvent,FALSE);e->QueueOwner=q;
    KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
}
static NTSTATUS paging_range(TG_GPU_CONTEXT_ENGINE *engine,const DXGKARG_SUBMITCOMMANDVIRTUAL *a,
    UINT *offset,HANDLE *system_context)
{
    const TG_GPU_QUEUE_ENTRY *entries=a->pDmaBufferPrivateData;
    const TG_GPU_PAGING_BUFFER *first=&entries->Paging;
    UINT count=a->DmaBufferPrivateDataSize/sizeof(*entries);
    UINT end=first->WriteOffset;

    if(a->NodeOrdinal || a->EngineOrdinal || !count || count>TG_GPU_PAGING_PRIVATE_ENTRIES ||
       a->DmaBufferPrivateDataSize!=count*sizeof(*entries) || !first->CpuAddress ||
       ((ULONG_PTR)first->CpuAddress&7) || first->Capacity>TG_GPU_DMA_BUFFER_BYTES ||
       a->DmaBufferVirtualAddress<first->GpuAddress ||
       a->DmaBufferVirtualAddress-first->GpuAddress!=end)
        return STATUS_INVALID_PARAMETER;

    *offset=end;
    *system_context=NULL;
    for(UINT i=0;i<count;++i) {
        const TG_GPU_PAGING_BUFFER *p=&entries[i].Paging;
        if(p->Magic!=TG_GPU_PAGING_BUFFER_MAGIC || p->Engine!=engine ||
           p->CpuAddress!=first->CpuAddress || p->GpuAddress!=first->GpuAddress ||
           p->Capacity!=first->Capacity || p->WriteOffset!=end ||
           p->BuiltBytes<=end || p->BuiltBytes>p->Capacity || ((end|p->BuiltBytes)&7))
            return STATUS_INVALID_PARAMETER;
        if(p->SystemContext) {
            if(*system_context && *system_context!=p->SystemContext)
                return STATUS_INVALID_PARAMETER;
            *system_context=p->SystemContext;
        }
        end=p->BuiltBytes;
    }
    return end-*offset==a->DmaBufferSize?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
}

static NTSTATUS queue_submit(TG_GPU_WORK_QUEUE *q,const DXGKARG_SUBMITCOMMANDVIRTUAL *a,
    TG_GPU_QUEUE_ENTRY *reserved,TG_GPU_COMMAND_SNAPSHOT *snapshot,BOOLEAN *used)
{
    TG_GPU_QUEUE_ENTRY *entry;LIST_ENTRY *l;KIRQL irql,engine_irql;NTSTATUS status;
    if(used)*used=FALSE;
    if(!q || !q->Engine || !a)return STATUS_INVALID_PARAMETER;
    entry=a->pDmaBufferPrivateData;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(q->Paused || !NT_SUCCESS(q->Failure)) {
        KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_NOT_READY;
    }
    if(a->Flags.Value&~0xc9u){status=STATUS_NOT_SUPPORTED;goto done;}
    if(reserved && !a->Flags.ContextSwitch && !a->Flags.Paging &&
       !a->Flags.NullRendering && a->DmaBufferSize) {
        if(!snapshot || snapshot->Bytes!=a->DmaBufferSize ||
           snapshot->Bytes>TG_GPU_COMMAND_CAPTURE_BYTES) {status=STATUS_INVALID_PARAMETER;goto rejected;}
        entry=reserved;
    }
    if(reserved && !a->DmaBufferUmdPrivateDataSize &&
       (a->Flags.ContextSwitch || (!a->Flags.Paging && (a->Flags.NullRendering || !a->DmaBufferSize))) &&
       (!entry || a->DmaBufferPrivateDataSize<sizeof(*entry)))entry=reserved;
    if(!entry ||
        ((ULONG_PTR)entry&(TYPE_ALIGNMENT(TG_GPU_QUEUE_ENTRY)-1)) ||
        (entry!=reserved && a->DmaBufferPrivateDataSize<sizeof(*entry)) ||
        (a->DmaBufferUmdPrivateDataSize && !snapshot)) {
        status=STATUS_INVALID_PARAMETER;goto rejected;
    }
    if(q->CurrentPrivate==entry) { KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY; }
    for(l=q->Pending.Flink;l!=&q->Pending;l=l->Flink)
        if(CONTAINING_RECORD(l,TG_GPU_QUEUE_ENTRY,Link)==entry) {
            KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY;
        }
    if(a->Flags.ContextSwitch) {
        RtlZeroMemory(entry,sizeof(*entry));
        status=TgGpuCaptureSoftwareWork(q->Engine,a,&entry->Work);
    } else if(a->Flags.Paging) {
        UINT offset;HANDLE system_context;BOOLEAN requires_context;
        status=paging_range(q->Engine,a,&offset,&system_context);
        if(!NT_SUCCESS(status))goto rejected;
        status=TgGpuValidatePagingBuffer(q->Engine->Tables,
            (UCHAR *)entry->Paging.CpuAddress+offset,a->DmaBufferSize,&requires_context);
        if(NT_SUCCESS(status)) {
            KeAcquireSpinLock(&q->Engine->Lock,&engine_irql);
            status=q->Engine->Stopping?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS;
            if(NT_SUCCESS(status)) {
                RtlZeroMemory(&entry->Work,sizeof(entry->Work));
                entry->Work.Engine=q->Engine;entry->Work.Paging=TRUE;
                entry->Work.SubmissionFence=entry->Work.CompletionFence=a->SubmissionFenceId;
                entry->Work.PagingOffset=offset;entry->Work.PagingBytes=a->DmaBufferSize;
            }
            KeReleaseSpinLock(&q->Engine->Lock,engine_irql);
            if (NT_SUCCESS(status) && requires_context) {
                if (!system_context || (a->hContext && a->hContext!=system_context))
                    status=STATUS_INVALID_PARAMETER;
                else
                    status=TgGpuCapturePagingContext(q->Engine,system_context,&entry->Work);
            }
        }
    } else {
        RtlZeroMemory(entry,sizeof(*entry));
        status=(a->Flags.ContextSwitch || a->Flags.NullRendering || !a->DmaBufferSize)?
            TgGpuCaptureSoftwareWork(q->Engine,a,&entry->Work):
            TgGpuCaptureContextWork(q->Engine,a,&entry->Work);
        if(NT_SUCCESS(status) && snapshot) {
            entry->Snapshot=snapshot;
            entry->Work.DmaAddress=entry->Work.InternalVa+TG_GPU_COMMAND_CAPTURE;
        }
    }
    /* A paging buffer is generated by this KMD, so corruption is an internal
     * recovery condition rather than an application-format rejection. */
rejected:
    if(reserved && status==STATUS_INVALID_PARAMETER && a->Flags.Paging)status=STATUS_DEVICE_DATA_ERROR;
    if(status==STATUS_INVALID_PARAMETER && reserved) {
        RtlZeroMemory(reserved,sizeof(*reserved));reserved->Owned=TRUE;
        reserved->Work.Engine=q->Engine;reserved->Work.Software=reserved->Work.Rejected=TRUE;
        reserved->Work.SubmissionFence=reserved->Work.CompletionFence=a->SubmissionFenceId;
        InsertTailList(&q->Pending,&reserved->Link);*used=TRUE;
    }
    if(NT_SUCCESS(status)) {
        if(entry==reserved){entry->Owned=TRUE;*used=TRUE;}
        InsertTailList(&q->Pending,&entry->Link);
    }
done:
    KeReleaseSpinLock(&q->Lock,irql);
    if(NT_SUCCESS(status) || (used && *used)) (VOID)KeSetEvent(&q->Wake,IO_NO_INCREMENT,FALSE);
    return status;
}
NTSTATUS TgGpuQueueSubmit(TG_GPU_WORK_QUEUE *q,const DXGKARG_SUBMITCOMMANDVIRTUAL *a)
{return queue_submit(q,a,NULL,NULL,NULL);}
NTSTATUS TgGpuQueuePreempt(TG_GPU_WORK_QUEUE *q,const DXGKARG_PREEMPTCOMMAND *a,
    TG_GPU_QUEUE_ENTRY *entry)
{
    KIRQL irql,engine_irql;NTSTATUS status=STATUS_SUCCESS;
    if(!q || !a || !entry || a->NodeOrdinal || a->EngineOrdinal || a->Flags.Value)
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(!q->Engine || q->Paused || !NT_SUCCESS(q->Failure))status=STATUS_DEVICE_NOT_READY;
    else {
        KeAcquireSpinLock(&q->Engine->Lock,&engine_irql);
        if(q->Engine->Stopping || q->Engine->RecoveryPending)status=STATUS_DEVICE_NOT_READY;
        KeReleaseSpinLock(&q->Engine->Lock,engine_irql);
        if(NT_SUCCESS(status)) {
            RtlZeroMemory(entry,sizeof(*entry));entry->Owned=TRUE;
            entry->Work.Engine=q->Engine;entry->Work.Preemption=TRUE;
            entry->Work.SubmissionFence=a->PreemptionFenceId;
            InsertTailList(&q->Pending,&entry->Link);++q->Preemptions;
        }
    }
    KeReleaseSpinLock(&q->Lock,irql);
    if(NT_SUCCESS(status))(VOID)KeSetEvent(&q->Wake,IO_NO_INCREMENT,FALSE);
    return status;
}
NTSTATUS TgGpuQueueSubmitOrReject(TG_GPU_WORK_QUEUE *q,const DXGKARG_SUBMITCOMMANDVIRTUAL *a,
    TG_GPU_QUEUE_ENTRY *reserved,TG_GPU_COMMAND_SNAPSHOT *snapshot,BOOLEAN *used)
{
    if(!reserved || !used)return STATUS_INVALID_PARAMETER;
    return queue_submit(q,a,reserved,snapshot,used);
}
/* DispatchGate held, Current empty. This never touches hardware:
 * any running DMA buffer must first complete/retire through the normal gate.
 * Drop only the prefix before the first preemption marker, not newer work.
 */
static NTSTATUS preempt_prefix(TG_GPU_WORK_QUEUE *q)
{
    TG_GPU_QUEUE_ENTRY *entry;TG_GPU_CONTEXT_ENGINE *e=q->Engine;
    NTSTATUS status;KIRQL irql;BOOLEAN aborted=FALSE,owned;
    KeAcquireSpinLock(&e->Lock,&irql);
    status=e->Active || e->Notification || e->Channel.referenced?STATUS_PENDING:STATUS_SUCCESS;
    KeReleaseSpinLock(&e->Lock,irql);if(status!=STATUS_SUCCESS)return status;
    for(;;) {
        KeAcquireSpinLock(&q->Lock,&irql);
        if(IsListEmpty(&q->Pending)) {
            KeReleaseSpinLock(&q->Lock,irql);return STATUS_INVALID_DEVICE_STATE;
        }
        entry=CONTAINING_RECORD(q->Pending.Flink,TG_GPU_QUEUE_ENTRY,Link);
        if(entry->Work.Preemption) {
            KeReleaseSpinLock(&q->Lock,irql);return STATUS_SUCCESS;
        }
        RemoveEntryList(&entry->Link);KeReleaseSpinLock(&q->Lock,irql);
        if(entry->Work.Rejected && !aborted) {
            KeAcquireSpinLock(&e->Lock,&irql);e->LastCompletedFence=entry->Work.CompletionFence;
            KeReleaseSpinLock(&e->Lock,irql);
        } else if(!entry->Work.Rejected)aborted=TRUE;
        status=entry->Work.Context?TgGpuCancelCapturedWork(e,&entry->Work):STATUS_SUCCESS;
        if(!NT_SUCCESS(status)) {
            KeAcquireSpinLock(&q->Lock,&irql);InsertHeadList(&q->Pending,&entry->Link);
            KeReleaseSpinLock(&q->Lock,irql);return status;
        }
        owned=entry->Owned;
        /* VidSch can resubmit an aborted paging packet. Preserve the KMD's
         * immutable paging descriptor in OS private storage until OS reuse. */
        RtlZeroMemory(&entry->Work,sizeof(entry->Work));InitializeListHead(&entry->Link);
        if(owned)free_entry(entry);
    }
}
NTSTATUS TgGpuServiceWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power_io *io,
                             const struct tg_gpu_power *power)
{
    TG_GPU_QUEUE_ENTRY *entry;TG_GPU_CONTEXT_WORK *w;KIRQL irql;NTSTATUS status;
    BOOLEAN wake=FALSE;
    if(!q || !q->Engine) return STATUS_INVALID_PARAMETER;
    status=KeWaitForSingleObject(&q->DispatchGate,Executive,KernelMode,FALSE,NULL);
    if(status!=STATUS_SUCCESS)return NT_SUCCESS(status)?STATUS_DEVICE_BUSY:status;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(q->Servicing) {
        KeReleaseSpinLock(&q->Lock,irql);(VOID)KeReleaseMutex(&q->DispatchGate,FALSE);return STATUS_DEVICE_BUSY;
    }
    if(q->Paused || !NT_SUCCESS(q->Failure)) {
        status=q->Paused ? STATUS_DEVICE_NOT_READY : q->Failure;
        KeReleaseSpinLock(&q->Lock,irql);(VOID)KeReleaseMutex(&q->DispatchGate,FALSE);return status;
    }
    q->Servicing=TRUE;w=&q->Current;
    if(!w->Engine && q->Preemptions) {
        /* Cancel/free one detached record at a time at PASSIVE, rather than
         * holding a spin lock across an arbitrarily long admitted prefix. */
        KeReleaseSpinLock(&q->Lock,irql);
        status=preempt_prefix(q);
        if(status!=STATUS_SUCCESS)goto done;
        KeAcquireSpinLock(&q->Lock,&irql);
    }
    if(!w->Engine && !IsListEmpty(&q->Pending)) {
        entry=CONTAINING_RECORD(RemoveHeadList(&q->Pending),TG_GPU_QUEUE_ENTRY,Link);
        *w=entry->Work;q->CurrentPrivate=entry;q->CurrentPaging=entry->Paging;
        q->CurrentOwned=entry->Owned?entry:NULL;
        if(w->Preemption){--q->Preemptions;q->CurrentPrivate=NULL;}
        /* Transfer the retained reference to queue-owned storage. Dxgkrnl may
         * reuse its original buffer immediately from the completion callback.
         */
        entry->Work.Context=NULL;
    }
    KeReleaseSpinLock(&q->Lock,irql);
    status=STATUS_SUCCESS;
    if(!w->Engine) goto done;
    if(w->Software || w->Preemption) {
        status=TgGpuArmSoftwareCompletion(q->Engine,w);
        if(status!=STATUS_SUCCESS)goto done;
        goto notify;
    }
    if(w->Paging) {
        status=TgGpuExecutePagingWork(q->Engine,w,
            (UCHAR *)q->CurrentPaging.CpuAddress+w->PagingOffset,w->PagingBytes,io,power);
        if(status!=STATUS_SUCCESS)goto done;
        goto notify;
    }
    if(w->CompletionReady)goto notify;
    if(!w->Expected) {
        status=TgGpuActivateContextWork(q->Engine,w);
        if(status==STATUS_DEVICE_BUSY) { status=STATUS_PENDING;goto done; }
        if(!NT_SUCCESS(status)) goto done;
    }
    if(!w->Prepared) {
        status=TgGpuPrepareContextWork(q->Engine,w);
        if(!NT_SUCCESS(status)) goto done;
    }
    if(!w->Attempted) {
        if(q->CurrentOwned && q->CurrentOwned->Snapshot) {
            const TG_GPU_COMMAND_SNAPSHOT *s=q->CurrentOwned->Snapshot;
            if(s->Bytes!=w->DmaBytes || s->Bytes>TG_GPU_COMMAND_CAPTURE_BYTES ||
               w->DmaAddress!=w->InternalVa+TG_GPU_COMMAND_CAPTURE ||
               q->Engine->Commands.Bytes!=TG_GPU_COMMAND_BYTES) {
                status=STATUS_INVALID_PARAMETER;goto done;
            }
            /* Prepare cleared control pages, not this region. Only the
             * serialized execution slot can publish captured USER words. */
            RtlZeroMemory((UCHAR *)q->Engine->Commands.CpuAddress+TG_GPU_COMMAND_CAPTURE,TG_GPU_COMMAND_CAPTURE_BYTES);
            RtlCopyMemory((UCHAR *)q->Engine->Commands.CpuAddress+TG_GPU_COMMAND_CAPTURE,s->Words,s->Bytes);
            KeMemoryBarrier();++q->Engine->SnapshotUploads;q->Engine->SnapshotBytes+=s->Bytes;
        }
        status=TgGpuSubmitContextWork(q->Engine,w,io,power);
        if(!NT_SUCCESS(status)) goto done;
    }
    status=TgGpuObserveContextWork(q->Engine,w,io,power);
    if(status!=STATUS_SUCCESS) goto done;
    if(!w->Retired) {
        status=TgGpuQuiesceContextWork(q->Engine,w,io,power);
        if(!NT_SUCCESS(status)) goto done;
    }
    status=TgGpuObserveContextWork(q->Engine,w,io,power);
    if(status!=STATUS_SUCCESS)goto done;
notify:
    /* Admission of rejected records uses this same PASSIVE gate. Consume
     * adjacent rejection markers before the real completion callback, so a
     * concurrent QueryCurrentFence never regresses behind those packets. */
    if(!w->Preemption)for(;;) {
        KeAcquireSpinLock(&q->Lock,&irql);
        entry=IsListEmpty(&q->Pending)?NULL:CONTAINING_RECORD(q->Pending.Flink,TG_GPU_QUEUE_ENTRY,Link);
        if(!entry || !entry->Work.Rejected) {KeReleaseSpinLock(&q->Lock,irql);break;}
        RemoveEntryList(&entry->Link);w->CompletionFence=entry->Work.CompletionFence;
        KeReleaseSpinLock(&q->Lock,irql);
        free_entry(entry);
    }
    KeAcquireSpinLock(&q->Lock,&irql);q->CurrentPrivate=NULL;KeReleaseSpinLock(&q->Lock,irql);
    status=TgGpuSignalContextWork(q->Engine,w);
    if(status!=STATUS_SUCCESS) goto done;
    RtlZeroMemory(w,sizeof(*w));RtlZeroMemory(&q->CurrentPaging,sizeof(q->CurrentPaging));
    if(q->CurrentOwned) {
        free_entry(q->CurrentOwned);q->CurrentOwned=NULL;
    }
    KeAcquireSpinLock(&q->Lock,&irql);wake=!IsListEmpty(&q->Pending);KeReleaseSpinLock(&q->Lock,irql);
done:
    KeAcquireSpinLock(&q->Lock,&irql);
    if(!NT_SUCCESS(status)) q->Failure=status;
    q->Servicing=FALSE;KeReleaseSpinLock(&q->Lock,irql);
    if(wake) (VOID)KeSetEvent(&q->Wake,IO_NO_INCREMENT,FALSE);
    (VOID)KeReleaseMutex(&q->DispatchGate,FALSE);
    return status;
}
NTSTATUS TgGpuDrainRejectedIdle(TG_GPU_WORK_QUEUE *q)
{
    KIRQL irql;BOOLEAN idle;
    if(!q || !q->Engine)return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);
    idle=!q->Servicing && !q->Current.Engine && !IsListEmpty(&q->Pending) &&
        CONTAINING_RECORD(q->Pending.Flink,TG_GPU_QUEUE_ENTRY,Link)->Work.Rejected;
    KeReleaseSpinLock(&q->Lock,irql);
    /* Kernel dispatcher mutexes are recursive for the owning thread. This
     * step therefore shares the exact worker completion/release path. */
    return idle?TgGpuServiceWorkQueue(q,NULL,NULL):STATUS_SUCCESS;
}
NTSTATUS TgGpuResetWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power_io *io,
                           struct tg_gpu_power *power,struct tg_gpu_clock *clock)
{
    TG_GPU_QUEUE_ENTRY *entry;TG_GPU_CONTEXT_WORK *w;KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    if(!q || !q->Engine) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);q->Paused=TRUE;
    if(q->Servicing) { KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY; }
    q->Servicing=TRUE;w=&q->Current;KeReleaseSpinLock(&q->Lock,irql);
    if(w->CompletionReady && !w->Notified) {
        status=TgGpuCancelContextNotification(q->Engine,w);
        if(!NT_SUCCESS(status)) goto done;
        RtlZeroMemory(w,sizeof(*w));q->CurrentPrivate=NULL;
    }
    if(w->PagingStarted && !w->CompletionReady) {
        status=TgGpuResetContextWork(q->Engine,w,io,power,clock);
        if(!NT_SUCCESS(status))goto done;
    } else if(w->Context) {
        if(!w->Expected) status=TgGpuCancelCapturedWork(q->Engine,w);
        else if(!w->Attempted) status=TgGpuRetireContextWork(q->Engine,w,NULL,NULL);
        else status=TgGpuResetContextWork(q->Engine,w,io,power,clock);
        if(!NT_SUCCESS(status)) goto done;
    }
    RtlZeroMemory(w,sizeof(*w));q->CurrentPrivate=NULL;
    RtlZeroMemory(&q->CurrentPaging,sizeof(q->CurrentPaging));
    if(q->CurrentOwned) {
        free_entry(q->CurrentOwned);q->CurrentOwned=NULL;
    }
    /* Admission is paused and this worker is exclusive. Clear each borrowed
     * queue entry before the adapter reports reset/preemption to Dxgkrnl.
     */
    while(!IsListEmpty(&q->Pending)) {
        entry=CONTAINING_RECORD(q->Pending.Flink,TG_GPU_QUEUE_ENTRY,Link);
        status=entry->Work.Context?TgGpuCancelCapturedWork(q->Engine,&entry->Work):STATUS_SUCCESS;
        if(!NT_SUCCESS(status)) goto done;
        BOOLEAN owned=entry->Owned;
        if(entry->Work.Preemption)--q->Preemptions;
        RemoveEntryList(&entry->Link);
        if(owned)free_entry(entry);else RtlZeroMemory(entry,sizeof(*entry));
    }
done:
    KeAcquireSpinLock(&q->Lock,&irql);q->Failure=status;q->Servicing=FALSE;KeReleaseSpinLock(&q->Lock,irql);
    return status;
}
NTSTATUS TgGpuResumeWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power *power)
{
    KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    if(!q || !q->Engine || !power) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(q->Servicing || q->Current.Engine || q->Preemptions || !IsListEmpty(&q->Pending) || !NT_SUCCESS(q->Failure))
        status=STATUS_DEVICE_BUSY;
    else if(!power->owned || !power->ready || !q->Engine->UserdBar1Ready || q->Engine->RecoveryPending) status=STATUS_DEVICE_NOT_READY;
    else q->Paused=FALSE;
    KeReleaseSpinLock(&q->Lock,irql);return status;
}
NTSTATUS TgGpuStopWorkQueue(TG_GPU_WORK_QUEUE *q)
{
    KIRQL irql,engine_irql;TG_GPU_CONTEXT_ENGINE *e;
    if(!q || !q->Engine) return STATUS_INVALID_PARAMETER;
    e=q->Engine;KeAcquireSpinLock(&q->Lock,&irql);q->Paused=TRUE;
    if(q->Servicing || q->Current.Engine || q->Preemptions || !IsListEmpty(&q->Pending)) {
        KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY;
    }
    KeAcquireSpinLock(&e->Lock,&engine_irql);e->QueueOwner=NULL;KeReleaseSpinLock(&e->Lock,engine_irql);
    q->Engine=NULL;KeReleaseSpinLock(&q->Lock,irql);return STATUS_SUCCESS;
}
