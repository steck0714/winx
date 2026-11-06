/* SPDX-License-Identifier: GPL-2.0-only */
#include "lifecycle.h"

static const WCHAR FirmwarePath[] = L"\\SystemRoot\\System32\\drivers\\SwitchWOA\\brcmfmac4356A3-pcie.bin";
static const WCHAR NvramPath[] = L"\\SystemRoot\\System32\\drivers\\SwitchWOA\\brcmfmac4356A3-pcie.txt";

typedef struct SWIFI_INITIALIZE_CONTEXT {
    SWIFI_HARDWARE *Hardware;
} SWIFI_INITIALIZE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWIFI_INITIALIZE_CONTEXT, SwifiInitializeContext);

static EVT_WDF_WORKITEM InitializeWorker;

static swifi_result Dispatch(PVOID Context, const uint8_t *Item, size_t Length)
{
    SWIFI_HARDWARE *H = Context;
    swifi_result Result = SwifiControlDispatch(&H->Control, Item, Length);
    if (Result == SWIFI_UNHANDLED && H->DataBound)
        Result = H->Callbacks.DispatchData(H->Context, Item, Length);
    return Result;
}

static swifi_result Mailbox(PVOID Context)
{
    return SwifiControlMailbox(&((SWIFI_HARDWARE *)Context)->Control);
}

static VOID Notify(PVOID Context)
{
    SWIFI_HARDWARE *H = Context;
    SwifiControlNotify(&H->Control);
    if (H->DataBound)
        H->Callbacks.NotifyData(H->Context);
}

static VOID RecoverTransport(PVOID Context, swifi_result Reason)
{
    SWIFI_HARDWARE *H = Context;
    UNREFERENCED_PARAMETER(Reason);
    SwifiControlRequestStop(&H->Control);
    H->Callbacks.ScheduleRecovery(H->Context, STATUS_DEVICE_PROTOCOL_ERROR);
}

static VOID RecoverControl(PVOID Context, NTSTATUS Reason)
{
    SWIFI_HARDWARE *H = Context;
    H->Callbacks.ScheduleRecovery(H->Context, Reason);
}

static VOID FirmwareEvent(PVOID Context, const swifi_event_completion *Event)
{
    SWIFI_HARDWARE *H = Context;
    H->Callbacks.Event(H->Context, Event);
}

NTSTATUS SwifiHardwareCreate(WDFDEVICE Device, SWIFI_HARDWARE *H,
    const SWIFI_HARDWARE_CALLBACKS *Callbacks, PVOID Context)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_WORKITEM_CONFIG Config;
    NTSTATUS Status;
    if (!H || !Callbacks || !Callbacks->DispatchData || !Callbacks->NotifyData ||
        !Callbacks->CancelDataAfterReset || !Callbacks->Event || !Callbacks->ScheduleRecovery)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(H, sizeof(*H));
    H->Device = Device;
    H->Callbacks = *Callbacks;
    H->Context = Context;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWIFI_INITIALIZE_CONTEXT);
    Attributes.ParentObject = Device;
    WDF_WORKITEM_CONFIG_INIT(&Config, InitializeWorker);
    Config.AutomaticSerialization = FALSE;
    Status = WdfWorkItemCreate(&Config, &Attributes, &H->InitializeWork);
    if (NT_SUCCESS(Status))
        SwifiInitializeContext(H->InitializeWork)->Hardware = H;
    return Status;
}

static VOID InitializeWorker(WDFWORKITEM Work)
{
    SWIFI_HARDWARE *H = SwifiInitializeContext(Work)->Hardware;
    NTSTATUS Status = STATUS_SUCCESS;
    if (!H->Chip.Bar0)
        Status = SwifiChipMapAndDiscover(&H->Chip, &H->Bus);
    if (NT_SUCCESS(Status))
        Status = SwifiChipLoadFirmware(&H->Chip, FirmwarePath, NvramPath);
    if (NT_SUCCESS(Status))
        Status = SwifiSharedPrepare(&H->Shared, &H->Chip, H->Dma);
    H->InitializeStatus = Status;
}

static NTSTATUS InitializeSession(SWIFI_HARDWARE *H)
{
    if (H->RequiresColdBoot)
        return STATUS_DEVICE_HARDWARE_ERROR;
    H->InitializeStatus = STATUS_PENDING;
    WdfWorkItemEnqueue(H->InitializeWork);
    WdfWorkItemFlush(H->InitializeWork);
    return H->InitializeStatus;
}

NTSTATUS SwifiHardwarePrepare(SWIFI_HARDWARE *H, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    static const SWIFI_WIN_CALLBACKS TransportCallbacks = {Dispatch, Mailbox, Notify, RecoverTransport};
    static const SWIFI_CONTROL_CALLBACKS ControlCallbacks = {FirmwareEvent, RecoverControl};
    NTSTATUS Status, CleanupStatus;
    if (H->Prepared || H->Bus.Host || !H->InitializeWork || H->RequiresColdBoot)
        return STATUS_INVALID_DEVICE_STATE;
    Status = SwifiMapResources(&H->Bus, Raw, Translated);
    if (!NT_SUCCESS(Status)) return Status;
    Status = SwifiWinCreateDma(H->Device, &H->Dma);
    if (!NT_SUCCESS(Status)) goto Failed;
    Status = InitializeSession(H);
    if (!NT_SUCCESS(Status)) goto Failed;
    /* This call remains in the PrepareHardware callback, not InitializeWork. */
    Status = SwifiWinCreateTransport(H->Device, &H->Transport, &H->Bus,
        SwifiChipRuntimeRegisters(&H->Chip), H->Chip.Bar0Length,
        H->Shared.H2d, H->Shared.MaxSubmit, H->Shared.D2h, &TransportCallbacks, H,
        &H->Bus.RawInterrupt, &H->Bus.TranslatedInterrupt);
    if (!NT_SUCCESS(Status)) goto Failed;
    Status = SwifiControlCreate(H->Device, &H->Control, &H->Transport, &ControlCallbacks, H);
    if (!NT_SUCCESS(Status)) goto Failed;
    H->ControlCreated = TRUE;
    Status = SwifiControlBind(&H->Control, &H->Shared);
    if (!NT_SUCCESS(Status)) goto Failed;
    H->Prepared = TRUE;
    return STATUS_SUCCESS;
Failed:
    CleanupStatus = SwifiHardwareRelease(H);
    return NT_SUCCESS(CleanupStatus) ? Status : CleanupStatus;
}

NTSTATUS SwifiHardwareEnterD0(SWIFI_HARDWARE *H)
{
    NTSTATUS Status;
    SWIFI_WIN_TRANSPORT *T = &H->Transport;
    if (!H->Prepared || H->RequiresColdBoot || T->Ready || T->Armed)
        return STATUS_INVALID_DEVICE_STATE;
    if (!H->Shared.Prepared) {
        Status = InitializeSession(H);
        if (!NT_SUCCESS(Status)) return Status;
        T->Registers = SwifiChipRuntimeRegisters(&H->Chip);
        T->H2d = H->Shared.H2d;
        T->H2dCount = H->Shared.MaxSubmit;
        T->D2h = H->Shared.D2h;
        Status = SwifiControlBind(&H->Control, &H->Shared);
        if (!NT_SUCCESS(Status)) return Status;
    }
    T->Pending = T->Faulted = T->HostFault = 0;
    T->LastAfiCode = T->LastAfiSignature = 0;
    T->NextD2h = 0;
    InterlockedExchange(&T->Ready, 1);
    Status = SwifiSharedEnableDma(&H->Shared);
    if (!NT_SUCCESS(Status))
        InterlockedExchange(&T->Ready, 0);
    return Status;
}

NTSTATUS SwifiHardwareOnline(SWIFI_HARDWARE *H)
{
    NTSTATUS Status;
    if (!H->Prepared) return STATUS_INVALID_DEVICE_STATE;
    Status = SwifiControlStart(&H->Control);
    if (NT_SUCCESS(Status))
        Status = SwifiFirmwareQueryInfo(&H->Control, &H->FirmwareInfo);
    if (!NT_SUCCESS(Status))
        SwifiHardwareRequestStop(H);
    return Status;
}

VOID SwifiHardwareRequestStop(SWIFI_HARDWARE *H)
{
    if (H->ControlCreated)
        SwifiControlRequestStop(&H->Control);
}

NTSTATUS SwifiHardwareStop(SWIFI_HARDWARE *H)
{
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL Irql;
    SwifiHardwareRequestStop(H);
    RtlZeroMemory(&H->FirmwareInfo, sizeof(H->FirmwareInfo));
    if (H->InitializeWork) WdfWorkItemFlush(H->InitializeWork);
    if (H->Transport.Interrupt)
        SwifiWinQuiesceInterrupts(&H->Transport);
    if (H->ControlCreated)
        SwifiControlQuiesce(&H->Control);
    if (H->Chip.Bar0 && H->Chip.Stage != SwifiChipStopped) {
        Status = SwifiChipStopAfterQuiesce(&H->Chip);
        if (!NT_SUCCESS(Status)) {
            Status = SwifiHoldEndpointReset(&H->Bus);
            if (NT_SUCCESS(Status)) {
                H->RequiresColdBoot = TRUE;
                H->Chip.RuntimePinned = FALSE;
                H->Chip.SharedAddress = 0;
                H->Chip.Stage = SwifiChipStopped;
            }
        }
    }
    H->LastStopStatus = Status;
    if (!NT_SUCCESS(Status))
        return Status; /* DMA/mappings remain owned; never free on uncertainty. */
    if (H->DataBound) {
        KeAcquireSpinLock(&H->Transport.DataLock, &Irql);
        H->Callbacks.CancelDataAfterReset(H->Context);
        H->DataBound = FALSE;
        KeReleaseSpinLock(&H->Transport.DataLock, Irql);
        H->Callbacks.NotifyData(H->Context);
    }
    if (H->ControlCreated)
        SwifiControlStoppedAfterReset(&H->Control);
    SwifiSharedDeleteAfterReset(&H->Shared);
    if (H->Transport.Interrupt) {
        H->Transport.H2d = NULL;
        H->Transport.H2dCount = 0;
        H->Transport.D2h = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS SwifiHardwareRelease(SWIFI_HARDWARE *H)
{
    NTSTATUS Status = SwifiHardwareStop(H);
    if (!NT_SUCCESS(Status)) return Status;
    if (H->ControlCreated) {
        SwifiControlDelete(&H->Control);
        H->ControlCreated = FALSE;
    }
    SwifiWinDeleteTransportAfterReset(&H->Transport);
    SwifiChipUnmapAfterReset(&H->Chip);
    if (H->Dma) {
        WdfObjectDelete(H->Dma);
        H->Dma = NULL;
    }
    SwifiUnmapResources(&H->Bus);
    H->Prepared = FALSE;
    return STATUS_SUCCESS;
}

VOID SwifiHardwareDelete(SWIFI_HARDWARE *H)
{
    NT_ASSERT(!H->Prepared && !H->Bus.Host && !H->Dma);
    if (H->InitializeWork) WdfObjectDelete(H->InitializeWork);
    H->InitializeWork = NULL;
}
