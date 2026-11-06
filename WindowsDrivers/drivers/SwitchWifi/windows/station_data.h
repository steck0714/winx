/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWIFI_STATION_DATA_H
#define SWIFI_STATION_DATA_H

#include "lifecycle.h"
#include "report.h"
#include "../core/rx_metadata.h"
#include "../core/receive_filter.h"
#include "../core/station_security.h"
#include <net/virtualaddress.h>

enum { SwifiNumTx = 24, SwifiNumRx = 511, SwifiNumFifo = 4 };
typedef struct SWIFI_QUEUE SWIFI_QUEUE;
typedef struct SWIFI_TX_RECORD {
    ULONG FragmentEnd;
    BOOLEAN Done;
} SWIFI_TX_RECORD;

typedef struct SWIFI_TX_BUFFER {
    BOOLEAN Owned, Eapol;
    SWIFI_TX_RECORD *Record; /* Detached during queue cancellation. */
} SWIFI_TX_BUFFER;

typedef struct SWIFI_RX_BUFFER {
    LIST_ENTRY Link;
    ULONG Epoch, KeyEpoch, AcceptedKeyEpoch;
    BOOLEAN KeyInFlight;
    USHORT Offset, Length;
    BOOLEAN Protected, DerivedClear;
    enum { SwifiRxFree, SwifiRxPosted, SwifiRxReady } State;
} SWIFI_RX_BUFFER;

typedef struct SWIFI_STATION_DATA {
    SWIFI_HARDWARE *Hardware;
    SWIFI_QUEUE *TxQueue, *RxQueue;
    SWIFI_SHARED_BUFFER TxMemory, RxMemory, RxMeta;
    swifi_data Core;
    swifi_flow Flows[SwifiNumFifo];
    ULONG FlowEpoch[SwifiNumFifo];
    swifi_packet_slot TxIds[SwifiNumTx], RxIds[SwifiNumRx];
    SWIFI_TX_BUFFER Tx[SwifiNumTx];
    SWIFI_RX_BUFFER Rx[SwifiNumRx];
    LIST_ENTRY FreeRx, ReadyRx;
    USHORT Sequence;
    UCHAR Address[6], Peer[6];
    BOOLEAN Enabled, Joining, Associated, Secure, Pairwise, Group, FlowFault;
    BOOLEAN ExcludeUnencrypted;
    UCHAR EapolExemption[2]; /* unicast, multicast */
    swifi_eapol_tx KeyTx;
    KEVENT EapolIdle;
    ULONG Epoch, KeyEpoch, TxProgress, PacketPath;
    BOOLEAN FirstJoin;
    BOOLEAN MetadataSeen, RssiSeen, BadMetadataSeen, NativeProtectedSeen;
    BOOLEAN SampleValid;
    LONG SampleDbm;
    ULONGLONG SampleTime, NextLinkCheck;
    WDFWORKITEM LinkWorker;
    ULONG64 TxErrors, RxDrops;
} SWIFI_STATION_DATA;

struct SWIFI_QUEUE {
    NETPACKETQUEUE Handle;
    SWIFI_STATION_DATA *Data;
    NET_RING *Packets, *Fragments;
    NET_EXTENSION Va, TxExemption;
    EX_RUNDOWN_REF Notifications;
    SWIFI_TX_RECORD *Records;
    ULONG SeenProgress;
    USHORT WaitRing; /* A full submission ring, or MAXUSHORT. */
    BOOLEAN IsTx, Armed, Cancelling;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWIFI_QUEUE, SwifiQueueContext);

typedef struct SWIFI_DEVICE {
    SWIFI_HARDWARE Hw;
    SWIFI_REPORT Report;
    SWIFI_STATION_DATA Data;
    NETADAPTER Station;
    WDFWAITLOCK FilterLock;
    /* Writers take FilterLock, RX reads the published policy under ProtocolLock. */
    swifi_receive_filter Filter;
    BOOLEAN FilterLive;
    BOOLEAN StationClosing; /* ProtocolLock guards station publication and teardown. */
    volatile LONG Failing;
} SWIFI_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWIFI_DEVICE, SwifiDeviceContext);

typedef struct SWIFI_ADAPTER {
    SWIFI_DEVICE *Device;
} SWIFI_ADAPTER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWIFI_ADAPTER, SwifiAdapterContext);

EVT_NET_ADAPTER_CREATE_TXQUEUE SwifiCreateTxQueue;
EVT_NET_ADAPTER_CREATE_RXQUEUE SwifiCreateRxQueue;
EVT_WIFI_DEVICE_CREATE_ADAPTER SwifiCreateAdapter;

VOID SwifiDataPrime(SWIFI_DEVICE *Device);
NTSTATUS SwifiDataBind(SWIFI_DEVICE *Device);
VOID SwifiDataNotify(PVOID Context);
swifi_result SwifiDataDispatch(PVOID Context, const uint8_t *Item, size_t Length);
VOID SwifiDataReset(PVOID Context);
VOID SwifiDataRelease(PVOID Context);
/* The association owner calls this only on confirmed firmware state changes.
 * A null peer disconnects. A key update for the same peer keeps its flows. */
VOID SwifiDataBeginJoin(SWIFI_DEVICE *Device, const UCHAR Peer[6], BOOLEAN Secure, BOOLEAN ExcludeUnencrypted);
VOID SwifiDataLink(SWIFI_DEVICE *Device, const UCHAR *Peer, BOOLEAN Secure, BOOLEAN Pairwise, BOOLEAN Group);
NTSTATUS SwifiDataBeginKeyUpdate(SWIFI_DEVICE *Device);
VOID SwifiDataEndKeyUpdate(SWIFI_DEVICE *Device);
VOID SwifiDataRequestStop(SWIFI_DEVICE *Device);
VOID SwifiPnpFailure(PVOID Context, NTSTATUS Reason);

#endif
