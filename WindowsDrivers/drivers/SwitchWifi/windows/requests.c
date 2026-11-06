/* SPDX-License-Identifier: ISC */
#include "requests.h"
#include <TlvGeneratorParser.hpp>

struct SWIFI_WDI {
    WDFDEVICE Device;
    SWIFI_WIN_CONTROL *Control;
    SWIFI_FIRMWARE_INFO *Info;
    TLV_CONTEXT Tlv;
    WDFWORKITEM Worker;
    KSPIN_LOCK Lock;
    LIST_ENTRY Pending;
    BOOLEAN Scheduled;
    BOOLEAN Accepting;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWIFI_WDI, SwifiDeviceWdi);

typedef struct SWIFI_WDI_COMMAND {
    LIST_ENTRY Link;
    WIFIREQUEST Request;
} SWIFI_WDI_COMMAND;

static EVT_WIFI_DEVICE_SEND_COMMAND SendCommand;
static EVT_WIFI_DEVICE_CREATE_WIFIDIRECTDEVICE CreateDirectDevice;
static EVT_WDF_WORKITEM CommandWorker;

static NTSTATUS CreateDirectDevice(WDFDEVICE Device, WIFIDIRECT_DEVICE_INIT *Init)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Init);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS SwifiWdiCreate(WDFDEVICE Device, SWIFI_WIN_CONTROL *Control,
    SWIFI_FIRMWARE_INFO *Info, PFN_WIFI_DEVICE_CREATE_ADAPTER CreateAdapter,
    SWIFI_WDI **Wdi)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_WORKITEM_CONFIG WorkConfig;
    WIFI_DEVICE_CONFIG Config;
    SWIFI_WDI *State;
    NTSTATUS Status;
    *Wdi = NULL;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWIFI_WDI);
    Status = WdfObjectAllocateContext(Device, &Attributes, (PVOID *)&State);
    if (!NT_SUCCESS(Status)) return Status;
    State->Device = Device;
    State->Control = Control;
    State->Info = Info;
    KeInitializeSpinLock(&State->Lock);
    InitializeListHead(&State->Pending);
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    WDF_WORKITEM_CONFIG_INIT(&WorkConfig, CommandWorker);
    WorkConfig.AutomaticSerialization = FALSE;
    Status = WdfWorkItemCreate(&WorkConfig, &Attributes, &State->Worker);
    if (!NT_SUCCESS(Status)) return Status;
    WIFI_DEVICE_CONFIG_INIT(&Config, WDI_VERSION_LATEST, SendCommand, CreateAdapter, CreateDirectDevice);
    Status = WifiDeviceInitialize(Device, &Config);
    if (!NT_SUCCESS(Status)) return Status;
    State->Tlv.PeerVersion = WifiDeviceGetOsWdiVersion(Device);
    if (State->Tlv.PeerVersion < WDI_VERSION_MIN_SUPPORTED) return STATUS_REVISION_MISMATCH;
    *Wdi = State;
    return STATUS_SUCCESS;
}

VOID SwifiWdiStart(SWIFI_WDI *Wdi)
{
    KIRQL Irql;
    KeAcquireSpinLock(&Wdi->Lock, &Irql);
    Wdi->Accepting = TRUE;
    KeReleaseSpinLock(&Wdi->Lock, Irql);
}

VOID SwifiWdiRequestStop(SWIFI_WDI *Wdi)
{
    KIRQL Irql;
    if (!Wdi) return;
    KeAcquireSpinLock(&Wdi->Lock, &Irql);
    Wdi->Accepting = FALSE;
    KeReleaseSpinLock(&Wdi->Lock, Irql);
}

VOID SwifiWdiQuiesce(SWIFI_WDI *Wdi)
{
    if (Wdi) WdfWorkItemFlush(Wdi->Worker);
}

/* A successful M3 releases the WIFIREQUEST. The caller saves the input
 * header and preallocates M4 before reaching that ownership boundary. */
static VOID Complete(WIFIREQUEST Request, NTSTATUS Status)
{
    UINT Input, Output;
    WDI_MESSAGE_HEADER *Header = WifiRequestGetInOutBuffer(Request, &Input, &Output);
    if (!Header || Output < sizeof(*Header)) {
        WifiRequestSetBytesNeeded(Request, sizeof(*Header));
        WifiRequestComplete(Request, STATUS_BUFFER_OVERFLOW, 0);
    } else if (Input < sizeof(*Header)) {
        WifiRequestComplete(Request, STATUS_INVALID_PARAMETER, 0);
    } else {
        Header->Status = (NDIS_STATUS)Status;
        WifiRequestComplete(Request, Status, sizeof(*Header));
    }
}

static VOID SendCommand(WDFDEVICE Device, WIFIREQUEST Request)
{
    SWIFI_WDI *State = SwifiDeviceWdi(Device);
    SWIFI_WDI_COMMAND *Node;
    KIRQL Irql;
    Node = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Node), 'qFwS');
    if (!Node) {
        Complete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    Node->Request = Request;
    KeAcquireSpinLock(&State->Lock, &Irql);
    if (!State->Accepting) {
        KeReleaseSpinLock(&State->Lock, Irql);
        ExFreePoolWithTag(Node, 'qFwS');
        Complete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }
    InsertTailList(&State->Pending, &Node->Link);
    if (!State->Scheduled) {
        State->Scheduled = TRUE;
        WdfWorkItemEnqueue(State->Worker);
    }
    KeReleaseSpinLock(&State->Lock, Irql);
}

static NTSTATUS NewMessage(SWIFI_WDI *State, ULONG Length, WDFMEMORY *Memory,
    WDI_MESSAGE_HEADER **Buffer)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = State->Device;
    return WdfMemoryCreate(&Attributes, NonPagedPoolNx, 'mFwS', Length, Memory, (PVOID *)Buffer);
}

static NTSTATUS RadioStatus(SWIFI_WDI *State, ULONG Disabled)
{
    WDI_INDICATION_RADIO_STATUS_PARAMETERS Radio = {0};
    WDI_MESSAGE_HEADER *Buffer;
    WDFMEMORY Memory;
    ULONG Length = 0;
    UINT8 *Generated = NULL;
    NTSTATUS Status;
    Radio.RadioState.HardwareState = !(Disabled & 2);
    Radio.RadioState.SoftwareState = !(Disabled & 1);
    Status = (NTSTATUS)GenerateWdiIndicationRadioStatusFromIhv(&Radio,
        sizeof(*Buffer), &State->Tlv, &Length, &Generated);
    if (NT_SUCCESS(Status)) {
        Status = Length >= sizeof(*Buffer) && Generated ?
            NewMessage(State, Length, &Memory, &Buffer) : STATUS_DEVICE_PROTOCOL_ERROR;
        if (NT_SUCCESS(Status)) {
            RtlCopyMemory(Buffer, Generated, Length);
            RtlZeroMemory(Buffer, sizeof(*Buffer));
            Buffer->PortId = WDI_PORT_ID_ADAPTER;
            Buffer->TransactionId = WDI_TRANSACTION_ID_UNSOLICIT;
            Buffer->Status = NDIS_STATUS_SUCCESS;
            WifiDeviceReceiveIndication(State->Device, WDI_INDICATION_RADIO_STATUS, Memory);
            WdfObjectDelete(Memory);
        }
    }
    if (Generated) FreeGenerated(Generated);
    return Status;
}

static NTSTATUS SetSoftwareRadio(SWIFI_WDI *State, BOOLEAN Enabled)
{
    /* WLC_SET_RADIO changes only the low-half flags selected by the
     * high-half mask. Setting/clearing software disable leaves RFKILL and
     * Bluetooth alone. Read back actual hardware/software states. */
    ULONG Value = (1u << 16) | (Enabled ? 0u : 1u);
    NTSTATUS Status = SwifiFirmwareCommand(State->Control, 0, 38, &Value, sizeof(Value), NULL, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    Value = 0;
    USHORT Returned;
    Status = SwifiFirmwareCommand(State->Control, 0, 37, &Value, sizeof(Value), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned < sizeof(Value)) return STATUS_DEVICE_PROTOCOL_ERROR;
    State->Info->RadioDisabled = Value;
    Status = RadioStatus(State, Value);
    if (NT_SUCCESS(Status) && (!(Value & 1u)) != Enabled) return STATUS_DEVICE_PROTOCOL_ERROR;
    return Status;
}

static VOID Handle(SWIFI_WDI *State, WIFIREQUEST Request)
{
    UINT Input, Output;
    UINT16 Id = WifiRequestGetMessageId(Request);
    WDI_MESSAGE_HEADER *Header = WifiRequestGetInOutBuffer(Request, &Input, &Output);
    WDI_SET_RADIO_STATE_PARAMETERS Parameters = {0};
    WDI_MESSAGE_HEADER *Done;
    WDFMEMORY Memory;
    NTSTATUS Status;
    BOOLEAN Enable;
    if (!Header || Input < sizeof(*Header) || Output < sizeof(*Header)) {
        Complete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    if (Id != WDI_TASK_SET_RADIO_STATE) {
        Complete(Request, STATUS_NOT_SUPPORTED);
        return;
    }
    if (Header->PortId != WDI_PORT_ID_ADAPTER || !Header->TransactionId) {
        Complete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    Status = (NTSTATUS)ParseWdiTaskSetRadioStateToIhv(Input - sizeof(*Header),
        (PUCHAR)(Header + 1), &State->Tlv, &Parameters);
    Enable = Parameters.SoftwareRadioState != 0;
    if (NT_SUCCESS(Status) && Parameters.SoftwareRadioState > 1) Status = STATUS_INVALID_PARAMETER;
    CleanupParsedWdiTaskSetRadioStateToIhv(&Parameters);
    if (!NT_SUCCESS(Status)) {
        Complete(Request, Status);
        return;
    }
    Status = NewMessage(State, sizeof(*Done), &Memory, &Done);
    if (!NT_SUCCESS(Status)) {
        Complete(Request, Status);
        return;
    }
    *Done = *Header;
    Complete(Request, STATUS_SUCCESS);
    Done->Status = (NDIS_STATUS)SetSoftwareRadio(State, Enable);
    /* WiFiCx takes the M1 command ID for an M4 completion. */
    WifiDeviceReceiveIndication(State->Device, Id, Memory);
    WdfObjectDelete(Memory);
}

static VOID CommandWorker(WDFWORKITEM Work)
{
    WDFDEVICE Device = (WDFDEVICE)WdfWorkItemGetParentObject(Work);
    SWIFI_WDI *State = SwifiDeviceWdi(Device);
    for (;;) {
        KIRQL Irql;
        PLIST_ENTRY Link;
        BOOLEAN Online;
        KeAcquireSpinLock(&State->Lock, &Irql);
        if (IsListEmpty(&State->Pending)) {
            State->Scheduled = FALSE;
            KeReleaseSpinLock(&State->Lock, Irql);
            return;
        }
        Link = RemoveHeadList(&State->Pending);
        Online = State->Accepting;
        KeReleaseSpinLock(&State->Lock, Irql);
        SWIFI_WDI_COMMAND *Node = CONTAINING_RECORD(Link, SWIFI_WDI_COMMAND, Link);
        if (Online) Handle(State, Node->Request);
        else Complete(Node->Request, STATUS_CANCELLED);
        ExFreePoolWithTag(Node, 'qFwS');
    }
}
