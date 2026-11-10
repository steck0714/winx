/* SPDX-License-Identifier: ISC */
#include "control.h"

#define SWIFI_COMMAND_TIMEOUT_100NS (2ull * 10000000ull)

typedef struct SWIFI_CONTROL_WORK_CONTEXT {
    SWIFI_WIN_CONTROL *Control;
} SWIFI_CONTROL_WORK_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWIFI_CONTROL_WORK_CONTEXT, SwifiControlWorkContext);

static EVT_WDF_WORKITEM EventWorker;
static EVT_WDF_WORKITEM MailboxWorker;

static NTSTATUS ProtocolStatus(swifi_result Result)
{
    switch (Result) {
    case SWIFI_OK: return STATUS_SUCCESS;
    case SWIFI_RETRY: return STATUS_DEVICE_BUSY;
    case SWIFI_STOPPED: return STATUS_CANCELLED;
    case SWIFI_INVALID: return STATUS_INVALID_PARAMETER;
    default: return STATUS_DEVICE_PROTOCOL_ERROR;
    }
}

static VOID ReportFault(SWIFI_WIN_CONTROL *C, NTSTATUS Status)
{
    LONG WasRunning = InterlockedExchange(&C->Running, 0);
    KeSetEvent(&C->Stop, IO_NO_INCREMENT, FALSE);
    /* A concurrent normal D0 stop already owns teardown; do not restart it. */
    if (WasRunning && !InterlockedCompareExchange(&C->FaultReported, 1, 0))
        C->Callbacks.ScheduleRecovery(C->Context, Status);
}

static swifi_result Submit(PVOID Context, const uint8_t *Items, uint16_t Count)
{
    SWIFI_WIN_CONTROL *C = Context;
    return SwifiWinSubmitControl(C->Transport, Items, Count);
}

static VOID CommandComplete(PVOID Context, const swifi_command_completion *Reply)
{
    SWIFI_WIN_CONTROL *C = Context;
    C->Reply = *Reply;
    if (Reply->length)
        RtlCopyMemory(C->ReplyData, Reply->response, Reply->length);
    C->Reply.response = Reply->length ? C->ReplyData : NULL;
    if ((Reply->reason == SWIFI_COMMAND_COMPLETED || Reply->reason == SWIFI_COMMAND_TRUNCATED ||
         Reply->reason == SWIFI_COMMAND_FIRMWARE_ERROR) && Reply->command == 263 &&
        RtlEqualMemory(C->Core.input.memory, "wsec_key", sizeof("wsec_key"))) {
        /* Only a matched completion returns the common buffers to the host. */
        RtlSecureZeroMemory(C->Core.input.memory, SWIFI_CONTROL_BUFFER_SIZE);
        RtlSecureZeroMemory(C->Core.response.memory, SWIFI_CONTROL_BUFFER_SIZE);
    }
    KeSetEvent(&C->ReplyReady, IO_NO_INCREMENT, FALSE);
}

static VOID EventComplete(PVOID Context, const swifi_event_completion *Event)
{
    SWIFI_WIN_CONTROL *C = Context;
    /* Each retained slot can appear once; no heap allocation occurs in DPC. */
    if (C->EventCount >= SWIFI_EVENT_COUNT) {
        InterlockedExchange(&C->FaultPending, STATUS_DEVICE_PROTOCOL_ERROR);
        return;
    }
    C->Events[C->EventTail] = *Event;
    C->EventTail = (UCHAR)((C->EventTail + 1) % SWIFI_EVENT_COUNT);
    ++C->EventCount;
}

static NTSTATUS CreateWorker(WDFDEVICE Device, SWIFI_WIN_CONTROL *C,
    PFN_WDF_WORKITEM Callback, WDFWORKITEM *Work)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_WORKITEM_CONFIG Config;
    NTSTATUS Status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWIFI_CONTROL_WORK_CONTEXT);
    Attributes.ParentObject = Device;
    WDF_WORKITEM_CONFIG_INIT(&Config, Callback);
    Config.AutomaticSerialization = FALSE;
    Status = WdfWorkItemCreate(&Config, &Attributes, Work);
    if (NT_SUCCESS(Status))
        SwifiControlWorkContext(*Work)->Control = C;
    return Status;
}

NTSTATUS SwifiControlCreate(WDFDEVICE Device, SWIFI_WIN_CONTROL *C,
    SWIFI_WIN_TRANSPORT *Transport, const SWIFI_CONTROL_CALLBACKS *Callbacks,
    PVOID Context)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;
    if (!Device || !C || !Transport || !Callbacks ||
        !Callbacks->Event || !Callbacks->ScheduleRecovery)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(C, sizeof(*C));
    C->Transport = Transport;
    C->Callbacks = *Callbacks;
    C->Context = Context;
    KeInitializeEvent(&C->ReplyReady, NotificationEvent, FALSE);
    KeInitializeEvent(&C->RingProgress, SynchronizationEvent, FALSE);
    KeInitializeEvent(&C->Stop, NotificationEvent, TRUE);
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfWaitLockCreate(&Attributes, &C->CommandLock);
    if (!NT_SUCCESS(Status)) goto Failed;
    Status = CreateWorker(Device, C, EventWorker, &C->EventWork);
    if (!NT_SUCCESS(Status)) goto Failed;
    Status = CreateWorker(Device, C, MailboxWorker, &C->MailboxWork);
    if (!NT_SUCCESS(Status)) goto Failed;
    return STATUS_SUCCESS;
Failed:
    SwifiControlDelete(C); /* Nothing is bound or running yet. */
    return Status;
}

NTSTATUS SwifiControlBind(SWIFI_WIN_CONTROL *C, SWIFI_SHARED *Shared)
{
    static const swifi_control_ops Ops = {Submit, CommandComplete, EventComplete};
    swifi_result Result;
    if (!C->CommandLock || !Shared || !Shared->Prepared || C->Bound || C->Running ||
        C->EventQueued || C->MailboxQueued || C->Transport->Ready || C->Transport->Armed)
        return STATUS_INVALID_DEVICE_STATE;
    Result = swifi_control_init(&C->Core, &Ops, C, Shared->Input, Shared->Response,
        Shared->Events, (USHORT)Shared->Chip->RxDataOffset);
    if (Result != SWIFI_OK)
        return ProtocolStatus(Result);
    C->Shared = Shared;
    C->EventHead = C->EventTail = C->EventCount = 0;
    C->MailboxPending = C->LastMailbox = 0;
    C->FaultPending = C->FaultReported = 0;
    RtlZeroMemory(&C->Reply, sizeof(C->Reply));
    KeClearEvent(&C->ReplyReady);
    KeClearEvent(&C->RingProgress);
    C->Bound = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS SwifiControlStart(SWIFI_WIN_CONTROL *C)
{
    KIRQL Irql;
    swifi_result Result;
    if (!C->Bound || !C->Transport->Ready || !C->Transport->Armed || C->Running)
        return STATUS_INVALID_DEVICE_STATE;
    KeAcquireSpinLock(C->Transport->DataLock, &Irql);
    KeClearEvent(&C->Stop);
    InterlockedExchange(&C->Running, 1);
    Result = swifi_control_refill_events(&C->Core);
    KeReleaseSpinLock(C->Transport->DataLock, Irql);
    if (Result != SWIFI_OK && Result != SWIFI_RETRY) {
        ReportFault(C, ProtocolStatus(Result));
        return ProtocolStatus(Result);
    }
    SwifiControlNotify(C);
    return STATUS_SUCCESS;
}

static NTSTATUS WaitFor(SWIFI_WIN_CONTROL *C, PKEVENT Event, ULONGLONG Deadline)
{
    PVOID Objects[2] = {&C->Stop, Event};
    ULONGLONG Now = KeQueryInterruptTime();
    LARGE_INTEGER Timeout;
    if (KeReadStateEvent(&C->Stop))
        return STATUS_WAIT_0;
    if (Now >= Deadline)
        return STATUS_TIMEOUT;
    Timeout.QuadPart = -(LONGLONG)(Deadline - Now);
    return KeWaitForMultipleObjects(2, Objects, WaitAny, Executive, KernelMode,
        FALSE, &Timeout, NULL);
}

NTSTATUS SwifiControlExecute(SWIFI_WIN_CONTROL *C, UCHAR IfIndex, ULONG Command,
    const VOID *Input, USHORT InputLength, VOID *Output, USHORT OutputLength,
    PUSHORT ReturnedLength, PSHORT FirmwareStatus)
{
    KIRQL Irql;
    ULONGLONG Deadline;
    NTSTATUS Status, Fault = STATUS_SUCCESS;
    swifi_result Result;
    uint32_t RequestId = 0;
    PUCHAR InputCopy = NULL;
    if (ReturnedLength) *ReturnedLength = 0;
    if (FirmwareStatus) *FirmwareStatus = 0;
    if ((InputLength && !Input) || (OutputLength && !Output) ||
        InputLength > SWIFI_CONTROL_BUFFER_SIZE || OutputLength > SWIFI_CONTROL_BUFFER_SIZE)
        return STATUS_INVALID_PARAMETER;
    /* Caller input may be pageable. Copy before raising IRQL for submission. */
    if (InputLength) {
        InputCopy = ExAllocatePool2(POOL_FLAG_NON_PAGED, InputLength, 'cFwS');
        if (!InputCopy)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlCopyMemory(InputCopy, Input, InputLength);
    }
    Status = WdfWaitLockAcquire(C->CommandLock, NULL);
    if (!NT_SUCCESS(Status)) goto Done;
    if (!C->Bound || !C->Running) { Status = STATUS_CANCELLED; goto Unlock; }
    Deadline = KeQueryInterruptTime() + SWIFI_COMMAND_TIMEOUT_100NS;
    for (;;) {
        KeAcquireSpinLock(C->Transport->DataLock, &Irql);
        KeClearEvent(&C->RingProgress);
        KeClearEvent(&C->ReplyReady);
        if (!C->Running) {
            Result = SWIFI_STOPPED;
        } else {
            Result = swifi_control_command(&C->Core, IfIndex, Command, InputCopy,
                InputLength, OutputLength, C, &RequestId);
        }
        KeReleaseSpinLock(C->Transport->DataLock, Irql);
        if (Result != SWIFI_RETRY)
            break;
        Status = WaitFor(C, &C->RingProgress, Deadline);
        if (Status == STATUS_WAIT_0) { Status = STATUS_CANCELLED; goto Unlock; }
        if (Status == STATUS_TIMEOUT) { Fault = Status = STATUS_IO_TIMEOUT; goto Unlock; }
        if (Status != STATUS_WAIT_0 + 1) goto Unlock;
    }
    if (Result != SWIFI_OK) {
        Status = ProtocolStatus(Result);
        if (Result == SWIFI_PROTOCOL_ERROR) Fault = Status;
        goto Unlock;
    }
    Status = WaitFor(C, &C->ReplyReady, Deadline);
    if (Status == STATUS_WAIT_0) { Status = STATUS_CANCELLED; goto Unlock; }
    if (Status == STATUS_TIMEOUT) {
        KeAcquireSpinLock(C->Transport->DataLock, &Irql);
        Result = swifi_control_timeout(&C->Core, RequestId);
        KeReleaseSpinLock(C->Transport->DataLock, Irql);
        if (Result == SWIFI_OK) { Fault = Status = STATUS_IO_TIMEOUT; goto Unlock; }
        /* Completion won the race with the deadline; consume that result. */
    } else if (Status != STATUS_WAIT_0 + 1) {
        Fault = Status;
        goto Unlock;
    }
    if (ReturnedLength) *ReturnedLength = C->Reply.returned_length;
    if (FirmwareStatus) *FirmwareStatus = C->Reply.firmware_status;
    if (C->Reply.length)
        RtlCopyMemory(Output, C->ReplyData, C->Reply.length);
    switch (C->Reply.reason) {
    case SWIFI_COMMAND_COMPLETED: Status = STATUS_SUCCESS; break;
    case SWIFI_COMMAND_TRUNCATED: Status = STATUS_BUFFER_OVERFLOW; break;
    case SWIFI_COMMAND_FIRMWARE_ERROR: Status = STATUS_UNSUCCESSFUL; break;
    case SWIFI_COMMAND_TIMED_OUT: Status = STATUS_IO_TIMEOUT; break;
    default: Status = STATUS_CANCELLED; break;
    }
Unlock:
    if (Command == 263) {
        KeAcquireSpinLock(C->Transport->DataLock, &Irql);
        RtlSecureZeroMemory(C->ReplyData, sizeof(C->ReplyData));
        KeReleaseSpinLock(C->Transport->DataLock, Irql);
    }
    WdfWaitLockRelease(C->CommandLock);
Done:
    if (InputCopy) {
        RtlSecureZeroMemory(InputCopy, InputLength);
        ExFreePoolWithTag(InputCopy, 'cFwS');
    }
    if (!NT_SUCCESS(Fault)) ReportFault(C, Fault);
    return Status;
}

swifi_result SwifiControlDispatch(SWIFI_WIN_CONTROL *C, const uint8_t *Item, size_t Length)
{
    return C->Bound ? swifi_control_complete(&C->Core, Item, Length) : SWIFI_STOPPED;
}

swifi_result SwifiControlMailbox(SWIFI_WIN_CONTROL *C)
{
    ULONG Flags;
    NTSTATUS Status;
    if (!C->Bound)
        return SWIFI_STOPPED;
    Status = SwifiSharedReadMailbox(C->Shared, &Flags);
    C->LastMailbox = Flags;
    if (!NT_SUCCESS(Status))
        return SWIFI_PROTOCOL_ERROR;
    if (Flags & 2u)
        C->MailboxPending |= 2u; /* Host deep-sleep ACK. */
    return SWIFI_OK;
}

VOID SwifiControlNotify(SWIFI_WIN_CONTROL *C)
{
    KIRQL Irql;
    BOOLEAN QueueEvent = FALSE, QueueMailbox = FALSE;
    swifi_result Result = SWIFI_OK;
    NTSTATUS Fault;
    KeAcquireSpinLock(C->Transport->DataLock, &Irql);
    if (C->Bound && C->Running) {
        Result = swifi_control_refill_events(&C->Core);
        if (C->EventCount && !C->EventQueued) {
            C->EventQueued = TRUE;
            QueueEvent = TRUE;
        }
        if (C->MailboxPending && !C->MailboxQueued) {
            C->MailboxQueued = TRUE;
            QueueMailbox = TRUE;
        }
    }
    Fault = (NTSTATUS)InterlockedExchange(&C->FaultPending, 0);
    KeReleaseSpinLock(C->Transport->DataLock, Irql);
    KeSetEvent(&C->RingProgress, IO_NO_INCREMENT, FALSE);
    if (QueueEvent) WdfWorkItemEnqueue(C->EventWork);
    if (QueueMailbox) WdfWorkItemEnqueue(C->MailboxWork);
    if (!NT_SUCCESS(Fault))
        ReportFault(C, Fault);
    else if (Result != SWIFI_OK && Result != SWIFI_RETRY && Result != SWIFI_STOPPED)
        ReportFault(C, ProtocolStatus(Result));
}

static VOID EventWorker(WDFWORKITEM Work)
{
    SWIFI_WIN_CONTROL *C = SwifiControlWorkContext(Work)->Control;
    KIRQL Irql;
    for (;;) {
        swifi_event_completion Event;
        swifi_result Result = SWIFI_OK;
        BOOLEAN Running;
        KeAcquireSpinLock(C->Transport->DataLock, &Irql);
        if (!C->EventCount) {
            C->EventQueued = FALSE;
            KeReleaseSpinLock(C->Transport->DataLock, Irql);
            return;
        }
        Event = C->Events[C->EventHead];
        C->EventHead = (UCHAR)((C->EventHead + 1) % SWIFI_EVENT_COUNT);
        --C->EventCount;
        Running = C->Running != 0;
        KeReleaseSpinLock(C->Transport->DataLock, Irql);
        if (Running)
            C->Callbacks.Event(C->Context, &Event);
        KeAcquireSpinLock(C->Transport->DataLock, &Irql);
        Result = swifi_control_release_event(&C->Core, Event.request_id);
        if (Result == SWIFI_OK && C->Running)
            Result = swifi_control_refill_events(&C->Core);
        KeReleaseSpinLock(C->Transport->DataLock, Irql);
        if (Result != SWIFI_OK && Result != SWIFI_RETRY && Result != SWIFI_STOPPED)
            ReportFault(C, ProtocolStatus(Result));
    }
}

static VOID MailboxWorker(WDFWORKITEM Work)
{
    SWIFI_WIN_CONTROL *C = SwifiControlWorkContext(Work)->Control;
    KIRQL Irql;
    for (;;) {
        ULONG Flags;
        NTSTATUS Status;
        KeAcquireSpinLock(C->Transport->DataLock, &Irql);
        Flags = C->MailboxPending;
        C->MailboxPending = 0;
        if (!Flags || !C->Running) {
            C->MailboxQueued = FALSE;
            KeReleaseSpinLock(C->Transport->DataLock, Irql);
            return;
        }
        KeReleaseSpinLock(C->Transport->DataLock, Irql);
        Status = SwifiSharedSendMailbox(C->Shared, Flags);
        if (!NT_SUCCESS(Status)) ReportFault(C, Status);
    }
}

VOID SwifiControlRequestStop(SWIFI_WIN_CONTROL *C)
{
    InterlockedExchange(&C->Running, 0);
    KeSetEvent(&C->Stop, IO_NO_INCREMENT, FALSE);
}

VOID SwifiControlQuiesce(SWIFI_WIN_CONTROL *C)
{
    SwifiControlRequestStop(C);
    /* Owner already joined the transport DPC, so no enqueue can race Flush. */
    if (C->EventWork) WdfWorkItemFlush(C->EventWork);
    if (C->MailboxWork) WdfWorkItemFlush(C->MailboxWork);
    if (C->CommandLock) {
        WdfWaitLockAcquire(C->CommandLock, NULL);
        WdfWaitLockRelease(C->CommandLock);
    }
}

VOID SwifiControlStoppedAfterReset(SWIFI_WIN_CONTROL *C)
{
    KIRQL Irql;
    ULONG I;
    if (!C->Bound)
        return;
    NT_ASSERT(!C->Running && !C->EventQueued && !C->MailboxQueued);
    KeAcquireSpinLock(C->Transport->DataLock, &Irql);
    swifi_control_stopped_after_reset(&C->Core);
    RtlSecureZeroMemory(C->Core.input.memory, SWIFI_CONTROL_BUFFER_SIZE);
    RtlSecureZeroMemory(C->Core.response.memory, SWIFI_CONTROL_BUFFER_SIZE);
    RtlSecureZeroMemory(C->ReplyData, sizeof(C->ReplyData));
    /* Workers are flushed and their callback contract never lends a pointer
     * past return. This also releases a retained slot after a queue fault. */
    for (I = 0; I < SWIFI_EVENT_COUNT; ++I)
        if (C->Core.events[I].state == SWIFI_EVENT_DELIVERED)
            (VOID)swifi_control_release_event(&C->Core, C->Core.events[I].request_id);
    C->Bound = FALSE;
    C->Shared = NULL;
    KeReleaseSpinLock(C->Transport->DataLock, Irql);
}

VOID SwifiControlDelete(SWIFI_WIN_CONTROL *C)
{
    NT_ASSERT(!C->Running && !C->Bound);
    if (C->EventWork) WdfObjectDelete(C->EventWork);
    if (C->MailboxWork) WdfObjectDelete(C->MailboxWork);
    if (C->CommandLock) WdfObjectDelete(C->CommandLock);
    RtlZeroMemory(C, sizeof(*C));
}
