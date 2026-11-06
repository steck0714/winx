/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWIFI_STATION_DATA_H
#define SWIFI_STATION_DATA_H

#include "lifecycle.h"
#include <net/virtualaddress.h>

enum { SwifiNumTx = 24, SwifiNumRx = 32, SwifiNumFifo = 4 };
typedef struct SWIFI_QUEUE SWIFI_QUEUE;
typedef struct SWIFI_TX_RECORD {
    ULONG FragmentEnd;
    BOOLEAN Done;
} SWIFI_TX_RECORD;

typedef struct SWIFI_TX_BUFFER {
    BOOLEAN Owned;
    SWIFI_TX_RECORD *Record; /* Detached during queue cancellation. */
} SWIFI_TX_BUFFER;

typedef struct SWIFI_RX_BUFFER {
    LIST_ENTRY Link;
    ULONG Epoch, QueueEpoch;
    USHORT Offset, Length;
    enum { SwifiRxFree, SwifiRxPosted, SwifiRxReady } State;
} SWIFI_RX_BUFFER;

typedef struct SWIFI_STATION_DATA {
    SWIFI_HARDWARE *Hardware;
    SWIFI_QUEUE *TxQueue, *RxQueue;
    SWIFI_SHARED_BUFFER TxMemory, RxMemory;
    swifi_data Core;
    swifi_flow Flows[SwifiNumFifo];
    ULONG FlowEpoch[SwifiNumFifo];
    swifi_packet_slot TxIds[SwifiNumTx], RxIds[SwifiNumRx];
    SWIFI_TX_BUFFER Tx[SwifiNumTx];
    SWIFI_RX_BUFFER Rx[SwifiNumRx];
    LIST_ENTRY ReadyRx;
    USHORT Sequence;
    UCHAR Address[6], Peer[6];
    BOOLEAN Enabled, Associated, KeysInstalled, FlowFault;
    ULONG Epoch, RxQueueEpoch, TxProgress;
    ULONG64 TxErrors, RxDrops;
} SWIFI_STATION_DATA;

struct SWIFI_QUEUE {
    NETPACKETQUEUE Handle;
    SWIFI_STATION_DATA *Data;
    NET_RING *Packets, *Fragments;
    NET_EXTENSION Va;
    EX_RUNDOWN_REF Notifications;
    SWIFI_TX_RECORD *Records;
    ULONG SeenProgress;
    USHORT WaitRing; /* A full submission ring, or MAXUSHORT. */
    BOOLEAN IsTx, Armed, Cancelling;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWIFI_QUEUE, SwifiQueueContext);

typedef struct SWIFI_DEVICE {
    SWIFI_HARDWARE Hw;
    SWIFI_STATION_DATA Data;
    NETADAPTER Station;
    WDI_PORT_ID Port;
    volatile LONG Failing;
} SWIFI_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWIFI_DEVICE, SwifiDeviceContext);

typedef struct SWIFI_ADAPTER {
    SWIFI_DEVICE *Device;
    BOOLEAN Bound;
} SWIFI_ADAPTER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWIFI_ADAPTER, SwifiAdapterContext);

EVT_NET_ADAPTER_CREATE_TXQUEUE SwifiCreateTxQueue;
EVT_NET_ADAPTER_CREATE_RXQUEUE SwifiCreateRxQueue;
EVT_WIFI_DEVICE_CREATE_ADAPTER SwifiCreateAdapter;

NTSTATUS SwifiDataBind(SWIFI_DEVICE *Device);
VOID SwifiDataNotify(PVOID Context);
swifi_result SwifiDataDispatch(PVOID Context, const uint8_t *Item, size_t Length);
VOID SwifiDataReset(PVOID Context);
VOID SwifiDataRelease(PVOID Context);
/* The association owner calls this only on confirmed firmware state changes.
 * A null peer disconnects. A key update for the same peer keeps its flows. */
VOID SwifiDataLink(SWIFI_DEVICE *Device, const UCHAR *Peer, BOOLEAN KeysInstalled);
VOID SwifiDataRequestStop(SWIFI_DEVICE *Device);
VOID SwifiPnpFailure(PVOID Context, NTSTATUS Reason);

#endif
