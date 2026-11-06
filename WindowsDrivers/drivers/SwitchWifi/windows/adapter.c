/* SPDX-License-Identifier: GPL-2.0-only */
#include "station_filter.h"

typedef struct SWIFI_STATION_BINDING {
    SWIFI_DEVICE *Device;
    NETADAPTER Adapter;
    WDI_PORT_ID Port;
} SWIFI_STATION_BINDING;

static EVT_WDF_OBJECT_CONTEXT_CLEANUP StationBindingCleanup;

static VOID StationBindingCleanup(WDFOBJECT Object)
{
    PAGED_CODE();
    SWIFI_STATION_BINDING *Binding = WdfMemoryGetBuffer((WDFMEMORY)Object, NULL);
    SWIFI_DEVICE *D = Binding->Device;
    NETADAPTER Adapter = Binding->Adapter;
    KIRQL Irql;
    KeAcquireSpinLock(&D->Hw.ProtocolLock, &Irql);
    if (D->Station != Adapter) {
        KeReleaseSpinLock(&D->Hw.ProtocolLock, Irql);
        return;
    }
    D->StationClosing = TRUE;
    D->Station = NULL;
    KeReleaseSpinLock(&D->Hw.ProtocolLock, Irql);
    SwifiWdiStation(D->Hw.Wdi, Binding->Port, FALSE);
    SwifiDataLink(D, NULL, FALSE, FALSE, FALSE);
    KeAcquireSpinLock(&D->Hw.ProtocolLock, &Irql);
    D->StationClosing = FALSE;
    KeReleaseSpinLock(&D->Hw.ProtocolLock, Irql);
}

NTSTATUS SwifiCreateAdapter(WDFDEVICE Device, NETADAPTER_INIT *Init)
{
    SWIFI_DEVICE *D = SwifiDeviceContext(Device);
    SWIFI_FIRMWARE_INFO *Info = &D->Hw.FirmwareInfo;
    NET_ADAPTER_DATAPATH_CALLBACKS Callbacks;
    NET_ADAPTER_LINK_LAYER_ADDRESS Mac;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES LinkCaps;
    NET_ADAPTER_LINK_STATE Link;
    NET_ADAPTER_TX_CAPABILITIES Tx;
    NET_ADAPTER_RX_CAPABILITIES Rx;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFMEMORY BindingMemory;
    SWIFI_STATION_BINDING *Binding;
    NETADAPTER Adapter;
    KIRQL Irql;
    if (WifiAdapterInitGetType(Init) != WIFI_ADAPTER_EXTENSIBLE_STATION) return STATUS_NOT_SUPPORTED;
    if (!Info->Bands) {
        SwifiReportStep(&D->Report, SwifiStationStart, STATUS_DEVICE_NOT_READY);
        return STATUS_DEVICE_NOT_READY;
    }
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&Callbacks, SwifiCreateTxQueue, SwifiCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(Init, &Callbacks);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWIFI_ADAPTER);
    NTSTATUS Status = NetAdapterCreate(Init, &Attributes, &Adapter);
    if (!NT_SUCCESS(Status)) {
        SwifiReportStep(&D->Report, SwifiStationCreate, Status);
        return Status;
    }
    SWIFI_ADAPTER *A = SwifiAdapterContext(Adapter);
    A->Device = D;
    Status = WifiAdapterInitialize(Adapter);
    if (!NT_SUCCESS(Status)) {
        SwifiReportStep(&D->Report, SwifiStationInitialize, Status);
        WdfObjectDelete(Adapter);
        return Status;
    }
    /* NetAdapterCx allocates the adapter context on an existing WDF object.
     * KMDF runs cleanup for a paged WDFMEMORY child at PASSIVE_LEVEL, so the
     * WDI port can drain its scan/join work and acquire its wait locks there. */
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Adapter;
    Attributes.EvtCleanupCallback = StationBindingCleanup;
    Status = WdfMemoryCreate(&Attributes, PagedPool, 'bFwS', sizeof(*Binding),
        &BindingMemory, (PVOID *)&Binding);
    if (!NT_SUCCESS(Status)) goto Failed;
    Binding->Device = D;
    Binding->Adapter = Adapter;
    Binding->Port = (WDI_PORT_ID)WifiAdapterGetPortId(Adapter);
    KeAcquireSpinLock(&D->Hw.ProtocolLock, &Irql);
    if (D->Station || D->StationClosing) {
        KeReleaseSpinLock(&D->Hw.ProtocolLock, Irql);
        Status = STATUS_DEVICE_BUSY;
        goto Failed;
    }
    D->Station = Adapter;
    RtlCopyMemory(D->Data.Address, Info->Address, 6);
    KeReleaseSpinLock(&D->Hw.ProtocolLock, Irql);

    /* NetAdapterCx expects maximum speeds, not a claim of current link rate. */
    ULONG64 PerStream = Info->Vht && (Info->Bandwidth[1] & 4) ? 433333333 :
        Info->Ht && ((Info->Bandwidth[0] | Info->Bandwidth[1]) & 2) ? 150000000 :
        Info->Ht ? 72222222 : 54000000;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&LinkCaps, PerStream * Info->TxStreams, PerStream * Info->RxStreams);
    NetAdapterSetLinkLayerCapabilities(Adapter, &LinkCaps);
    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&Mac, 6, Info->Address);
    NetAdapterSetPermanentLinkLayerAddress(Adapter, &Mac);
    NetAdapterSetCurrentLinkLayerAddress(Adapter, &Mac);
    NetAdapterSetLinkLayerMtuSize(Adapter, 1500);
    SwifiFilterRegister(Adapter);
    NET_ADAPTER_TX_CAPABILITIES_INIT(&Tx, 1);
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&Rx, SWIFI_PACKET_SIZE, 1);
    NetAdapterSetDataPathCapabilities(Adapter, &Tx, &Rx);
    NET_ADAPTER_LINK_STATE_INIT(&Link, 0, MediaConnectStateDisconnected, MediaDuplexStateUnknown,
        NetAdapterPauseFunctionTypeUnsupported, NetAdapterAutoNegotiationFlagNone);
    NetAdapterSetLinkState(Adapter, &Link);

    SwifiWdiStation(D->Hw.Wdi, Binding->Port, TRUE);
    Status = NetAdapterStart(Adapter);
    if (NT_SUCCESS(Status)) {
        SwifiReportStep(&D->Report, SwifiStationStart, Status);
        return Status;
    }
Failed:
    SwifiReportStep(&D->Report, SwifiStationStart, Status);
    WdfObjectDelete(Adapter);
    return Status;
}
