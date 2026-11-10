/* SPDX-License-Identifier: GPL-2.0-only */
#include "station_filter.h"

C_ASSERT(SWIFI_DIRECTED == NetPacketFilterFlagDirected && SWIFI_MULTICAST == NetPacketFilterFlagMulticast);
C_ASSERT(SWIFI_ALL_MULTICAST == NetPacketFilterFlagAllMulticast && SWIFI_BROADCAST == NetPacketFilterFlagBroadcast);
C_ASSERT(sizeof(WDI_MAC_ADDRESS) == 6);

enum { FilterPending, FilterAddressList, FilterWideFallback, FilterAllGroups, FilterNoGroups };

static BOOLEAN Unsupported(NTSTATUS Status)
{
    return Status == STATUS_NOT_SUPPORTED || Status == STATUS_NOT_FOUND;
}

/* Called at PASSIVE_LEVEL under FilterLock with the station interface up.
 * If firmware cannot install the requested list, receive all multicast in
 * firmware and enforce the exact Windows policy on the RX path. */
static NTSTATUS Program(SWIFI_DEVICE *D, const swifi_receive_filter *Filter, ULONG *Mode)
{
    SWIFI_WIN_CONTROL *C = &D->Hw.Control;
    if (Filter->flags & SWIFI_ALL_MULTICAST) {
        *Mode = FilterAllGroups;
        return SwifiFirmwareSetU32(C, 0, "allmulti", 1);
    }
    UCHAR Wire[4 + SWIFI_MAX_MULTICAST * 6];
    USHORT Size = (USHORT)swifi_filter_wire(Filter, Wire, sizeof(Wire));
    if (!Size) return STATUS_INVALID_PARAMETER;
    NTSTATUS Status = SwifiFirmwareIovarSet(C, 0, "mcast_list", Wire, Size, NULL);
    if (!NT_SUCCESS(Status) && Size > 4) {
        *Mode = FilterWideFallback;
        if (!C->Running) return Status;
        return SwifiFirmwareSetU32(C, 0, "allmulti", 1);
    }
    if (!NT_SUCCESS(Status) && !Unsupported(Status)) return Status;
    *Mode = Size > 4 ? FilterAddressList : FilterNoGroups;
    Status = SwifiFirmwareSetU32(C, 0, "allmulti", 0);
    /* With a programmed list, either narrow or all-multicast firmware
     * admits each requested address. Software always enforces the mask. */
    return Unsupported(Status) ? STATUS_SUCCESS : Status;
}

static VOID Publish(SWIFI_DEVICE *D, const swifi_receive_filter *Filter)
{
    KIRQL Irql;
    KeAcquireSpinLock(&D->Hw.ProtocolLock, &Irql);
    D->Filter = *Filter;
    KeReleaseSpinLock(&D->Hw.ProtocolLock, Irql);
}

/* A published policy survives D0 until WLC_UP programs this station anew. */
static NTSTATUS Change(SWIFI_DEVICE *D, const swifi_receive_filter *Next)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Mode = FilterPending;
    if (D->FilterLive && D->Hw.Control.Running) {
        Status = Program(D, Next, &Mode);
        if (!NT_SUCCESS(Status) && D->Hw.Control.Running) {
            ULONG PreviousMode;
            NTSTATUS Restore = Program(D, &D->Filter, &PreviousMode);
            if (!NT_SUCCESS(Restore) && D->Hw.Control.Running) SwifiPnpFailure(D, Restore);
        }
    }
    if (NT_SUCCESS(Status)) Publish(D, Next);
    SwifiReportFilter(&D->Report, Next->flags, Next->count, Mode, Status);
    return Status;
}

NTSTATUS SwifiFilterCreate(WDFDEVICE Device, SWIFI_DEVICE *D)
{
    WDF_OBJECT_ATTRIBUTES A;
    WDF_OBJECT_ATTRIBUTES_INIT(&A);
    A.ParentObject = Device;
    D->Filter.flags = SWIFI_DIRECTED | SWIFI_BROADCAST;
    return WdfWaitLockCreate(&A, &D->FilterLock);
}

NTSTATUS SwifiFilterStart(SWIFI_DEVICE *D)
{
    ULONG Mode = FilterPending;
    WdfWaitLockAcquire(D->FilterLock, NULL);
    NTSTATUS Status = Program(D, &D->Filter, &Mode);
    D->FilterLive = NT_SUCCESS(Status);
    SwifiReportFilter(&D->Report, D->Filter.flags, D->Filter.count, Mode, Status);
    WdfWaitLockRelease(D->FilterLock);
    return Status;
}

VOID SwifiFilterStop(SWIFI_DEVICE *D, BOOLEAN RemovePort)
{
    WdfWaitLockAcquire(D->FilterLock, NULL);
    D->FilterLive = FALSE;
    if (RemovePort) {
        swifi_receive_filter Empty = {0};
        Empty.flags = SWIFI_DIRECTED | SWIFI_BROADCAST;
        Publish(D, &Empty);
    }
    WdfWaitLockRelease(D->FilterLock);
}

NTSTATUS SwifiFilterResetList(SWIFI_DEVICE *D)
{
    WdfWaitLockAcquire(D->FilterLock, NULL);
    swifi_receive_filter Next = D->Filter;
    Next.count = 0;
    NTSTATUS Status = Change(D, &Next);
    WdfWaitLockRelease(D->FilterLock);
    return Status;
}

static EVT_NET_ADAPTER_SET_RECEIVE_FILTER SetReceiveFilter;

static VOID SetReceiveFilter(NETADAPTER Adapter, NETRECEIVEFILTER Request)
{
    SWIFI_DEVICE *D = SwifiAdapterContext(Adapter)->Device;
    SIZE_T Count = NetReceiveFilterGetMulticastAddressCount(Request);
    const NET_ADAPTER_LINK_LAYER_ADDRESS *List = NetReceiveFilterGetMulticastAddressList(Request);
    swifi_receive_filter Next = {0};
    Next.flags = NetReceiveFilterGetPacketFilter(Request) & SWIFI_STATION_FILTERS;
    if (Count > SWIFI_MAX_MULTICAST || (Count && !List)) goto Invalid;
    for (SIZE_T I = 0; I < Count; ++I)
        if (List[I].Length != 6) goto Invalid;
    if (!swifi_filter_list(&Next, Count ? List[0].Address : NULL, Count, sizeof(*List))) goto Invalid;
    WdfWaitLockAcquire(D->FilterLock, NULL);
    NTSTATUS Status = Change(D, &Next);
    WdfWaitLockRelease(D->FilterLock);
    /* NetAdapterCx's powered callback has no status result. Stop a live
     * endpoint that could not accept an OS filter instead of losing traffic. */
    if (!NT_SUCCESS(Status) && D->Hw.Control.Running) SwifiPnpFailure(D, Status);
    return;
Invalid:
    SwifiPnpFailure(D, STATUS_INVALID_PARAMETER);
}

VOID SwifiFilterRegister(NETADAPTER Adapter)
{
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES Caps;
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&Caps, SetReceiveFilter);
    Caps.SupportedPacketFilters = (NET_PACKET_FILTER_FLAGS)SWIFI_STATION_FILTERS;
    Caps.MaximumMulticastAddresses = SWIFI_MAX_MULTICAST;
    NetAdapterSetReceiveFilterCapabilities(Adapter, &Caps);
}

NTSTATUS SwifiFilterWdi(SWIFI_DEVICE *D, UINT16 Id, TLV_CONTEXT *Tlv, WDI_MESSAGE_HEADER *H, ULONG Bytes)
{
    WdfWaitLockAcquire(D->FilterLock, NULL);
    swifi_receive_filter Next = D->Filter;
    NTSTATUS Status;
    if (Id == WDI_SET_RECEIVE_PACKET_FILTER) {
        WDI_SET_RECEIVE_PACKET_FILTER_PARAMETERS P = {0};
        Status = (NTSTATUS)ParseWdiSetReceivePacketFilterToIhv(Bytes, (PUCHAR)(H+1), Tlv, &P);
        if (NT_SUCCESS(Status)) {
            if (P.PacketFilterType & ~(ULONG)SWIFI_STATION_FILTERS) Status = STATUS_NOT_SUPPORTED;
            else Next.flags = P.PacketFilterType;
        }
        CleanupParsedWdiSetReceivePacketFilterToIhv(&P);
    } else {
        WDI_SET_MULTICAST_LIST_PARAMETERS P = {0};
        Status = (NTSTATUS)ParseWdiSetMulticastListToIhv(Bytes, (PUCHAR)(H+1), Tlv, &P);
        if (NT_SUCCESS(Status)) {
            SIZE_T Count = P.Optional.MulticastList_IsPresent ? P.MulticastList.ElementCount : 0;
            const UCHAR *Addresses = (const UCHAR *)P.MulticastList.pElements;
            if (!swifi_filter_list(&Next, Addresses, Count, sizeof(WDI_MAC_ADDRESS)))
                Status = STATUS_INVALID_PARAMETER;
        }
        CleanupParsedWdiSetMulticastListToIhv(&P);
    }
    if (NT_SUCCESS(Status)) Status = Change(D, &Next);
    WdfWaitLockRelease(D->FilterLock);
    return Status;
}
