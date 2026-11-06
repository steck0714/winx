/* SPDX-License-Identifier: ISC */
#include "requests.h"
#include "scan.h"
#include <TlvGeneratorParser.hpp>

struct SWIFI_WDI {
    WDFDEVICE Device;
    SWIFI_WIN_CONTROL *Control;
    SWIFI_FIRMWARE_INFO *Info;
    TLV_CONTEXT Tlv;
    SWIFI_SCAN *Scan;
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
    VOID (*Recover)(PVOID, NTSTATUS), PVOID RecoveryContext, SWIFI_WDI **Wdi)
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
    Status = SwifiScanCreate(Device, Control, Info, State->Tlv.PeerVersion, Recover,
        RecoveryContext, &State->Scan);
    if (!NT_SUCCESS(Status)) return Status;
    *Wdi = State;
    return STATUS_SUCCESS;
}

VOID SwifiWdiStart(SWIFI_WDI *Wdi)
{
    KIRQL Irql;
    SwifiScanStartSession(Wdi->Scan);
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
    if (Wdi) {
        WdfWorkItemFlush(Wdi->Worker);
        SwifiScanEndSession(Wdi->Scan);
    }
}

VOID SwifiWdiStation(SWIFI_WDI *Wdi, WDI_PORT_ID Port, BOOLEAN Present)
{
    SwifiScanStation(Wdi->Scan, Port, Present);
}

BOOLEAN SwifiWdiFirmwareEvent(SWIFI_WDI *Wdi, const swifi_event_completion *Event)
{
    return Wdi && SwifiScanFirmwareEvent(Wdi->Scan, Event);
}

/* A successful M3 releases the WIFIREQUEST. The caller saves the input
 * header and preallocates M4 before reaching that ownership boundary. */
VOID SwifiCompleteWdiRequest(WIFIREQUEST Request, NTSTATUS Status)
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
        SwifiCompleteWdiRequest(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    Node->Request = Request;
    KeAcquireSpinLock(&State->Lock, &Irql);
    if (!State->Accepting) {
        KeReleaseSpinLock(&State->Lock, Irql);
        ExFreePoolWithTag(Node, 'qFwS');
        SwifiCompleteWdiRequest(Request, STATUS_DEVICE_NOT_READY);
        return;
    }
    InsertTailList(&State->Pending, &Node->Link);
    if (!State->Scheduled) {
        State->Scheduled = TRUE;
        WdfWorkItemEnqueue(State->Worker);
    }
    KeReleaseSpinLock(&State->Lock, Irql);
}

NTSTATUS SwifiAllocWdiMessage(WDFDEVICE Device, ULONG Length, WDFMEMORY *Memory,
    WDI_MESSAGE_HEADER **Buffer)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
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
            SwifiAllocWdiMessage(State->Device, Length, &Memory, &Buffer) : STATUS_DEVICE_PROTOCOL_ERROR;
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
    NTSTATUS Status;
    if (!Enabled) {
        Status = SwifiScanCancelForRadio(State->Scan);
        if (!NT_SUCCESS(Status)) return Status;
    }
    Status = SwifiFirmwareCommand(State->Control, 0, 38, &Value, sizeof(Value), NULL, NULL);
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

static NTSTATUS ConfigureAdapter(SWIFI_WDI *State, WDI_MESSAGE_HEADER *Header, ULONG Length)
{
    WDI_SET_FIRMWARE_CONFIGURATION_PARAMETERS P = {0};
    if (Header->PortId != WDI_PORT_ID_ADAPTER) return STATUS_INVALID_PARAMETER;
    NTSTATUS Status = (NTSTATUS)ParseWdiSetAdapterConfigurationToIhv(Length,
        (PUCHAR)(Header + 1), &State->Tlv, &P);
    ULONG Disallowed = 0;
    if (NT_SUCCESS(Status) && P.Optional.MacAddress_IsPresent &&
        !RtlEqualMemory(P.MacAddress.Address, State->Info->Address, 6))
        Status = STATUS_NOT_SUPPORTED; /* Do not silently use a different identity. */
    if (NT_SUCCESS(Status) && P.Optional.DisallowedBandList_IsPresent) {
        for (ULONG I = 0; I < P.DisallowedBandList.ElementCount; ++I) {
            WDI_BAND_ID Band = P.DisallowedBandList.pElements[I];
            if (Band == WDI_BAND_ID_2400) Disallowed |= SWIFI_BAND_2GHZ;
            if (Band == WDI_BAND_ID_5000) Disallowed |= SWIFI_BAND_5GHZ;
        }
    }
    if (NT_SUCCESS(Status)) Status = SwifiScanSetDisallowedBands(State->Scan, Disallowed);
    CleanupParsedWdiSetAdapterConfigurationToIhv(&P);
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
        SwifiCompleteWdiRequest(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    if (Id == WDI_SET_ADAPTER_CONFIGURATION) {
        SwifiCompleteWdiRequest(Request, ConfigureAdapter(State, Header, Input - sizeof(*Header)));
        return;
    }
    if (Id == WDI_TASK_SCAN || Id == WDI_ABORT_TASK) {
        SwifiScanRequest(State->Scan, Request, Id == WDI_ABORT_TASK);
        return;
    }
    if (Id != WDI_TASK_SET_RADIO_STATE) {
        SwifiCompleteWdiRequest(Request, STATUS_NOT_SUPPORTED);
        return;
    }
    if (Header->PortId != WDI_PORT_ID_ADAPTER || !Header->TransactionId) {
        SwifiCompleteWdiRequest(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    Status = (NTSTATUS)ParseWdiTaskSetRadioStateToIhv(Input - sizeof(*Header),
        (PUCHAR)(Header + 1), &State->Tlv, &Parameters);
    Enable = Parameters.SoftwareRadioState != 0;
    if (NT_SUCCESS(Status) && Parameters.SoftwareRadioState > 1) Status = STATUS_INVALID_PARAMETER;
    CleanupParsedWdiTaskSetRadioStateToIhv(&Parameters);
    if (!NT_SUCCESS(Status)) {
        SwifiCompleteWdiRequest(Request, Status);
        return;
    }
    Status = SwifiAllocWdiMessage(State->Device, sizeof(*Done), &Memory, &Done);
    if (!NT_SUCCESS(Status)) {
        SwifiCompleteWdiRequest(Request, Status);
        return;
    }
    *Done = *Header;
    SwifiCompleteWdiRequest(Request, STATUS_SUCCESS);
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
        else SwifiCompleteWdiRequest(Node->Request, STATUS_CANCELLED);
        ExFreePoolWithTag(Node, 'qFwS');
    }
}
