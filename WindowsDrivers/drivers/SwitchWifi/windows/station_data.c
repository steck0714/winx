/* SPDX-License-Identifier: GPL-2.0-only */
#include "station_data.h"
#include <net/wifi/exemptionaction.h>
#include "../core/station_frame.h"

static swifi_result Submit(void *Context, uint16_t Ring, const uint8_t *Item, size_t Length)
{
    SWIFI_STATION_DATA *D = Context;
    return SwifiWinSubmit(&D->Hardware->Transport, Ring, Item, Length);
}

static VOID UpdateEapolEvent(SWIFI_STATION_DATA *D)
{
    if (D->EapolPending || D->HeadEapol) KeClearEvent(&D->EapolIdle);
    else KeSetEvent(&D->EapolIdle, IO_NO_INCREMENT, FALSE);
}

static BOOLEAN IsEapol(const UCHAR *Frame)
{
    return Frame[12] == 0x88 && Frame[13] == 0x8e;
}

NTSTATUS SwifiDataDrainEapol(SWIFI_DEVICE *Device)
{
    LARGE_INTEGER Timeout;
    Timeout.QuadPart = -10 * 1000 * 1000; /* One second, as in brcmfmac. */
    NTSTATUS Status = KeWaitForSingleObject(&Device->Data.EapolIdle, Executive, KernelMode, FALSE, &Timeout);
    return Status == STATUS_TIMEOUT ? STATUS_IO_TIMEOUT : Status;
}

VOID SwifiDataKeyUpdate(SWIFI_DEVICE *D, BOOLEAN Updating)
{
    KIRQL Irql;
    KeAcquireSpinLock(&D->Hw.ProtocolLock, &Irql);
    if (Updating && !D->Data.UpdatingKeys) ++D->Data.KeyEpoch;
    D->Data.UpdatingKeys = Updating;
    ++D->Data.TxProgress;
    KeReleaseSpinLock(&D->Hw.ProtocolLock, Irql);
    if (!Updating) SwifiDataNotify(D);
}

static VOID TxComplete(void *Context, const swifi_packet_completion *Completion)
{
    SWIFI_STATION_DATA *D = Context;
    SWIFI_TX_BUFFER *Slot = Completion->cookie;
    if (Completion->reason != SWIFI_COMPLETED || Completion->tx_status) ++D->TxErrors;
    if (Slot->Record) Slot->Record->Done = TRUE;
    if (Slot->Eapol) {
        --D->EapolPending;
        Slot->Eapol = FALSE;
        UpdateEapolEvent(D);
    }
    Slot->Record = NULL;
    Slot->Owned = FALSE;
    ++D->TxProgress;
}

/* RX runs under ProtocolLock. Only the passive worker talks to the firmware.
 * A datapath slot and its optional metadata stay owned together until completion. */
static VOID ReadMetadata(SWIFI_STATION_DATA *D, SWIFI_RX_BUFFER *Slot, USHORT Length)
{
    SWIFI_REPORT *Report = &SwifiDeviceContext(D->Hardware->Device)->Report;
    if (Length) {
        swifi_rx_metadata Fields;
        UCHAR *Bytes = D->RxMeta.Memory + (Slot - D->Rx) * SWIFI_RX_METADATA_SIZE;
        if (!swifi_parse_rx_metadata(Bytes, Length, &Fields)) {
            if (!D->BadMetadataSeen) {
                D->BadMetadataSeen = TRUE;
                SwifiReportRx(Report, SwifiRxMetaInvalid);
            }
        } else {
            if (!D->MetadataSeen) {
                D->MetadataSeen = TRUE;
                SwifiReportRx(Report, SwifiRxMetaSeen);
            }
            if (Fields.have_rssi && D->Associated && Slot->Epoch == D->Epoch) {
                if (!D->RssiSeen) {
                    D->RssiSeen = TRUE;
                    SwifiReportRx(Report, SwifiRxRssiSeen);
                }
                D->SampleDbm = Fields.rssi_dbm;
                D->SampleTime = KeQueryInterruptTime();
                D->SampleValid = TRUE;
            }
        }
    }
    if (D->Associated && Slot->Epoch == D->Epoch && D->LinkWorker) {
        ULONGLONG Now = KeQueryInterruptTime();
        if (Now >= D->NextLinkCheck) {
            D->NextLinkCheck = Now + 50000000; /* Five seconds of active traffic. */
            WdfWorkItemEnqueue(D->LinkWorker);
        }
    }
}

static VOID RxComplete(void *Context, const swifi_packet_completion *Completion)
{
    SWIFI_STATION_DATA *D = Context;
    SWIFI_RX_BUFFER *Slot = Completion->cookie;
    Slot->State = SwifiRxFree;
    if (Completion->reason != SWIFI_COMPLETED || Completion->ifidx || !D->Enabled ||
        (D->RxQueue && D->RxQueue->Cancelling) || (!D->Associated && !D->Joining) ||
        Slot->QueueEpoch != D->RxQueueEpoch) {
        if (Completion->reason != SWIFI_CANCELLED) ++D->RxDrops;
        return;
    }
    UCHAR *Frame = D->RxMemory.Memory + (Slot - D->Rx) * SWIFI_PACKET_SIZE + Completion->offset;
    USHORT Length = Completion->length;
    BOOLEAN Native = (Completion->rx_flags & 2) != 0;
    SWIFI_REPORT *Report = &SwifiDeviceContext(D->Hardware->Device)->Report;
    if (Native) {
        if ((Completion->rx_flags & 1) != 0) { ++D->RxDrops; return; }
        if (Length >= 24 && (Frame[0] == 8 || Frame[0] == 0x88) &&
            (Frame[1] & 0x43) == 0x42 && RtlEqualMemory(Frame + 10, D->Peer, 6) &&
            ((Frame[4] & 1) || RtlEqualMemory(Frame + 4, D->Address, 6)) &&
            !D->NativeProtectedSeen) {
            /* A protected data header from this AP. Its inner type is unknown. */
            D->NativeProtectedSeen = TRUE;
            SwifiReportRx(Report, SwifiRxNativeProtected);
        }
        Length = (USHORT)swifi_clear_eapol(Frame, Length, D->Address, D->Peer);
        if (!Length) { ++D->RxDrops; return; }
    }
    BOOLEAN Eapol = IsEapol(Frame), Group = (Frame[0] & 1) != 0;
    if (Eapol && (!RtlEqualMemory(Frame + 6, D->Peer, 6) || Length < 18 ||
        (((USHORT)Frame[16] << 8) | Frame[17]) > Length - 18)) { ++D->RxDrops; return; }
    if ((!D->Associated || Slot->Epoch != D->Epoch) && (!Eapol || !RtlEqualMemory(Frame+6,D->Peer,6))) {
        ++D->RxDrops;
        return;
    }
    ReadMetadata(D, Slot, Completion->metadata_length);
    Slot->Protected = FALSE;
    Slot->DerivedClear = D->Secure && Eapol && !Native;
    if (D->Secure) {
        if (Eapol) {
            UCHAR Rule = D->EapolExemption[Group];
            /* An Ethernet completion loses the original Protected bit. After
             * a receive key is live we can accept verified clear native EAPOL,
             * but cannot label an ambiguous Ethernet EAPOL as clear. */
            BOOLEAN PreKey = swifi_prekey_ethernet(Slot->KeyEpoch, D->KeyEpoch,
                Slot->KeyInFlight, D->Pairwise || D->Group || D->UpdatingKeys);
            SwifiReportRx(Report, Native ? SwifiRxNativeClear :
                (PreKey ? SwifiRxEtherBeforeKey : SwifiRxEtherUncertain));
            if ((!Native && !PreKey) ||
                (Rule == WDI_EXEMPT_ON_KEY_MAPPING_KEY_UNAVAILABLE && D->Pairwise) ||
                (!Rule && D->ExcludeUnencrypted)) { ++D->RxDrops; return; }
        } else {
            /* Secure join required wsec_restrict readback before submitting. */
            if (D->UpdatingKeys || !D->Pairwise || (Group && !D->Group)) { ++D->RxDrops; return; }
            Slot->Protected = TRUE;
        }
    }
    Slot->Epoch = D->Epoch;
    Slot->State = SwifiRxReady;
    Slot->Offset = Completion->offset;
    Slot->Length = Length;
    InsertTailList(&D->ReadyRx, &Slot->Link);
}

static VOID FlowChanged(void *Context, uint16_t Ring, swifi_flow_state State, int16_t Status)
{
    SWIFI_STATION_DATA *D = Context;
    if (Status && D->Enabled) D->FlowFault = TRUE;
    if (State == SWIFI_FLOW_CLOSED && !Status && D->Enabled) {
        SwifiSharedRearmClosedFlow(&D->Hardware->Shared.H2d[Ring]);
        D->FlowEpoch[Ring - SWIFI_FIRST_FLOW_RING] = 0;
    }
    ++D->TxProgress;
}

swifi_result SwifiDataDispatch(PVOID Context, const uint8_t *Item, size_t Length)
{
    SWIFI_DEVICE *Device = Context;
    return swifi_data_complete(&Device->Data.Core, Item, Length);
}

NTSTATUS SwifiDataBind(SWIFI_DEVICE *Device)
{
    static const swifi_data_ops Ops = {Submit, TxComplete, RxComplete, FlowChanged};
    SWIFI_STATION_DATA *D = &Device->Data;
    SWIFI_HARDWARE *H = &Device->Hw;
    USHORT Flows = H->Shared.MaxSubmit >= 2 + SwifiNumFifo ? SwifiNumFifo : 1;
    USHORT RxCount = min(H->Chip.MaxRxBuffers, SwifiNumRx);
    KIRQL Irql;
    if (H->DataBound || !H->Shared.Prepared) return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS Status = SwifiSharedCreateBuffer(H->Dma, SwifiNumTx * SWIFI_PACKET_SIZE, &D->TxMemory);
    if (NT_SUCCESS(Status))
        Status = SwifiSharedCreateBuffer(H->Dma, RxCount * SWIFI_PACKET_SIZE, &D->RxMemory);
    if (NT_SUCCESS(Status))
        Status = SwifiSharedCreateBuffer(H->Dma, RxCount * SWIFI_RX_METADATA_SIZE, &D->RxMeta);
    for (USHORT I = 0; NT_SUCCESS(Status) && I < Flows; ++I)
        Status = SwifiSharedAllocateFlow(&H->Shared, (USHORT)(2 + I));
    if (!NT_SUCCESS(Status)) {
        /* No packet or flow address has been published. Shared owns flow allocations. */
        SwifiDataRelease(Device);
        return Status;
    }
    KeAcquireSpinLock(&H->ProtocolLock, &Irql);
    D->Hardware = H;
    InitializeListHead(&D->ReadyRx);
    RtlZeroMemory(D->Tx, sizeof(D->Tx));
    RtlZeroMemory(D->Rx, sizeof(D->Rx));
    RtlZeroMemory(D->FlowEpoch, sizeof(D->FlowEpoch));
    D->FlowFault = D->HeadEapol = D->UpdatingKeys = FALSE;
    D->EapolPending = 0;
    D->MetadataSeen = D->RssiSeen = D->BadMetadataSeen = D->NativeProtectedSeen = FALSE;
    D->SampleValid = FALSE;
    D->NextLinkCheck = 0;
    KeInitializeEvent(&D->EapolIdle, NotificationEvent, TRUE);
    if (swifi_data_init(&D->Core, &Ops, D, D->Flows, Flows, D->TxIds, SwifiNumTx,
        D->RxIds, RxCount, (USHORT)H->Chip.RxDataOffset) == SWIFI_OK) {
        H->DataBound = TRUE;
        D->Enabled = TRUE;
    } else Status = STATUS_DEVICE_PROTOCOL_ERROR;
    KeReleaseSpinLock(&H->ProtocolLock, Irql);
    if (!NT_SUCCESS(Status)) SwifiDataRelease(Device);
    return Status;
}

VOID SwifiDataRelease(PVOID Context)
{
    SWIFI_STATION_DATA *D = &((SWIFI_DEVICE *)Context)->Data;
    SwifiSharedDeleteBuffer(&D->RxMeta);
    SwifiSharedDeleteBuffer(&D->RxMemory);
    SwifiSharedDeleteBuffer(&D->TxMemory);
}

static VOID DropReady(SWIFI_STATION_DATA *D)
{
    while (!IsListEmpty(&D->ReadyRx)) {
        SWIFI_RX_BUFFER *Slot = CONTAINING_RECORD(RemoveHeadList(&D->ReadyRx), SWIFI_RX_BUFFER, Link);
        Slot->State = SwifiRxFree;
    }
}

static BOOLEAN CanSubmit(SWIFI_STATION_DATA *D)
{
    return D->Enabled && !D->FlowFault && D->Hardware->Transport.Armed &&
        D->Hardware->Transport.Ready && !D->Hardware->Transport.Faulted;
}

/* Caller holds the protocol lock. RETRY on the submission ring is retried
 * after the next completion. Nothing is posted while disconnected. */
static VOID Service(SWIFI_STATION_DATA *D)
{
    if (!CanSubmit(D)) return;
    for (USHORT I = 0; I < D->Core.flow_count; ++I) {
        swifi_flow *Flow = &D->Flows[I];
        if (D->FlowEpoch[I] != D->Epoch && Flow->state == SWIFI_FLOW_OPEN && !Flow->outstanding) {
            swifi_result R = swifi_data_delete_flow(&D->Core, (USHORT)(2 + I));
            if (R != SWIFI_OK && R != SWIFI_RETRY) D->FlowFault = TRUE;
        }
    }
    if ((!D->Associated && !D->Joining) || (D->RxQueue && D->RxQueue->Cancelling) || D->FlowFault) return;
    for (USHORT I = 0; I < D->Core.rx_count; ++I) {
        SWIFI_RX_BUFFER *Slot = &D->Rx[I];
        if (Slot->State != SwifiRxFree) continue;
        ULONGLONG Address = D->RxMemory.Logical.QuadPart + (SIZE_T)I * SWIFI_PACKET_SIZE;
        SIZE_T MetaOffset = (SIZE_T)I * SWIFI_RX_METADATA_SIZE;
        RtlZeroMemory(D->RxMeta.Memory + MetaOffset, SWIFI_RX_METADATA_SIZE);
        swifi_result R = swifi_data_post_rx_meta(&D->Core, Address, SWIFI_PACKET_SIZE,
            D->RxMeta.Logical.QuadPart + MetaOffset, SWIFI_RX_METADATA_SIZE, Slot);
        if (R != SWIFI_OK) {
            if (R != SWIFI_RETRY) D->FlowFault = TRUE;
            break;
        }
        Slot->Epoch = D->Epoch;
        Slot->QueueEpoch = D->RxQueueEpoch;
        Slot->KeyEpoch = D->KeyEpoch;
        Slot->KeyInFlight = D->Pairwise || D->Group || D->UpdatingKeys;
        Slot->State = SwifiRxPosted;
    }
}

VOID SwifiDataReset(PVOID Context)
{
    SWIFI_STATION_DATA *D = &((SWIFI_DEVICE *)Context)->Data;
    D->Enabled = D->Joining = D->Associated = D->Pairwise = D->Group = D->HeadEapol = FALSE;
    D->SampleValid = FALSE;
    SwifiReportKeys(&((SWIFI_DEVICE *)Context)->Report, 0, 0, 0);
    swifi_data_stopped_after_reset(&D->Core);
    DropReady(D);
}

VOID SwifiDataRequestStop(SWIFI_DEVICE *Device)
{
    KIRQL Irql;
    KeAcquireSpinLock(&Device->Hw.ProtocolLock, &Irql);
    Device->Data.Enabled = Device->Data.Joining = Device->Data.Associated = FALSE;
    Device->Data.Pairwise = Device->Data.Group = FALSE;
    ++Device->Data.Epoch;
    ++Device->Data.TxProgress;
    KeReleaseSpinLock(&Device->Hw.ProtocolLock, Irql);
}

VOID SwifiDataBeginJoin(SWIFI_DEVICE *Device, const UCHAR Peer[6], BOOLEAN Secure, BOOLEAN ExcludeUnencrypted)
{
    SWIFI_STATION_DATA *D = &Device->Data;
    KIRQL Irql;
    KeAcquireSpinLock(&Device->Hw.ProtocolLock, &Irql);
    if (D->Enabled) {
        ++D->Epoch;
        D->Joining = TRUE;
        D->SampleValid = FALSE;
        D->NextLinkCheck = 0;
        D->Associated = D->Pairwise = D->Group = FALSE;
        D->Secure = Secure;
        D->ExcludeUnencrypted = ExcludeUnencrypted;
        D->Sequence = 0;
        DropReady(D);
        RtlCopyMemory(D->Peer, Peer, 6);
        Service(D);
    }
    KeReleaseSpinLock(&Device->Hw.ProtocolLock, Irql);
}

VOID SwifiDataLink(SWIFI_DEVICE *Device, const UCHAR *Peer, BOOLEAN Secure, BOOLEAN Pairwise, BOOLEAN Group)
{
    SWIFI_STATION_DATA *D = &Device->Data;
    KIRQL Irql;
    KeAcquireSpinLock(&Device->Hw.ProtocolLock, &Irql);
    if (!D->Enabled) {
        KeReleaseSpinLock(&Device->Hw.ProtocolLock, Irql);
        return;
    }
    if ((!D->Associated && !D->Joining) || !Peer || !RtlEqualMemory(Peer, D->Peer, 6)) {
        ++D->Epoch;
        D->SampleValid = FALSE;
        D->NextLinkCheck = 0;
        DropReady(D);
        D->Sequence = 0;
    }
    D->Joining = FALSE;
    D->Associated = Peer != NULL;
    D->Secure = Peer && Secure;
    D->Pairwise = Peer && Pairwise;
    D->Group = Peer && Group;
    if (Peer) RtlCopyMemory(D->Peer, Peer, 6);
    ++D->TxProgress;
    KeReleaseSpinLock(&Device->Hw.ProtocolLock, Irql);
    SwifiDataNotify(Device);
}

static SWIFI_QUEUE *TakeNotify(SWIFI_QUEUE *Q, BOOLEAN Work)
{
    if (!Q || !Work || !Q->Armed || Q->Cancelling) return NULL;
    if (!ExAcquireRundownProtection(&Q->Notifications)) return NULL;
    Q->Armed = FALSE;
    return Q;
}

static BOOLEAN TxCanAdvance(SWIFI_STATION_DATA *D)
{
    SWIFI_QUEUE *Q = D->TxQueue;
    if (!Q) return FALSE;
    if (Q->SeenProgress != D->TxProgress) return TRUE;
    if (!CanSubmit(D) || Q->WaitRing == MAXUSHORT) return FALSE;
    swifi_ring *Ring = &D->Hardware->Shared.H2d[Q->WaitRing].Core;
    ULONG Peer = Ring->ops.read_peer(Ring->context);
    if (Peer >= Ring->depth) {
        D->FlowFault = TRUE;
        return FALSE;
    }
    return (Ring->host_index + 1u) % Ring->depth != Peer;
}

VOID SwifiDataNotify(PVOID Context)
{
    SWIFI_DEVICE *Device = Context;
    SWIFI_STATION_DATA *D = &Device->Data;
    SWIFI_QUEUE *Tx, *Rx;
    BOOLEAN Fault;
    KIRQL Irql;
    KeAcquireSpinLock(&Device->Hw.ProtocolLock, &Irql);
    Service(D);
    Tx = TakeNotify(D->TxQueue, TxCanAdvance(D));
    Rx = TakeNotify(D->RxQueue, D->Enabled && D->Associated && !IsListEmpty(&D->ReadyRx));
    Fault = D->Enabled && D->FlowFault;
    KeReleaseSpinLock(&Device->Hw.ProtocolLock, Irql);
    if (Tx) {
        NetTxQueueNotifyMoreCompletedPacketsAvailable(Tx->Handle);
        ExReleaseRundownProtection(&Tx->Notifications);
    }
    if (Rx) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(Rx->Handle);
        ExReleaseRundownProtection(&Rx->Notifications);
    }
    if (Fault) SwifiPnpFailure(Device, STATUS_DEVICE_PROTOCOL_ERROR);
}

static SIZE_T Gather(SWIFI_QUEUE *Q, NET_PACKET *Packet, PUCHAR Buffer)
{
    SIZE_T Total = 0;
    ULONG Index = Packet->FragmentIndex;
    for (ULONG I = 0; I < Packet->FragmentCount; ++I) {
        NET_FRAGMENT *F = NetRingGetFragmentAtIndex(Q->Fragments, Index);
        PUCHAR Address = NetExtensionGetFragmentVirtualAddress(&Q->Va, Index)->VirtualAddress;
        if (!Address || F->Offset > F->Capacity || F->ValidLength > F->Capacity - F->Offset ||
            F->ValidLength > SWIFI_PACKET_SIZE - Total) return 0;
        RtlCopyMemory(Buffer + Total, Address + F->Offset, (SIZE_T)F->ValidLength);
        Total += (SIZE_T)F->ValidLength;
        Index = NetRingIncrementIndex(Q->Fragments, Index);
    }
    return Total;
}

/* RETRY leaves the OS packet at NextIndex, including while opening a flow.
 * All other results consume that packet; a posted packet retains its record
 * until the subsequent matching firmware completion. */
static swifi_result Transmit(SWIFI_QUEUE *Q, NET_PACKET *Packet, SWIFI_TX_RECORD *Record)
{
    SWIFI_STATION_DATA *D = Q->Data;
    UCHAR Priority;
    if (!CanSubmit(D) || !D->Associated) return SWIFI_STOPPED;
    USHORT I;
    for (I = 0; I < SwifiNumTx && D->Tx[I].Owned; ++I) { }
    if (I == SwifiNumTx) return SWIFI_RETRY;
    SIZE_T Offset = (SIZE_T)I * SWIFI_PACKET_SIZE;
    PUCHAR Buffer = D->TxMemory.Memory + Offset;
    SIZE_T Length = Gather(Q, Packet, Buffer);
    Length = swifi_station_tx(Buffer, Length, D->Address, D->Peer,
        Buffer, SWIFI_PACKET_SIZE, &Priority);
    if (!Length) return SWIFI_INVALID;
    D->HeadEapol = IsEapol(Buffer);
    if (D->Secure && !D->HeadEapol) {
        if (D->UpdatingKeys) return SWIFI_RETRY;
        if (!D->Pairwise) return SWIFI_INVALID;
    }
    UCHAR Action = Q->TxExemption.Enabled ?
        WifiExtensionGetExemptionAction(&Q->TxExemption, Q->Packets->NextIndex)->ExemptionAction :
        (D->HeadEapol ? WDI_EXEMPT_ON_KEY_MAPPING_KEY_UNAVAILABLE : WDI_EXEMPT_NO_EXEMPTION);
    if (Action > WDI_EXEMPT_ON_KEY_MAPPING_KEY_UNAVAILABLE ||
        (D->Secure && !D->HeadEapol && Action == WDI_EXEMPT_ALWAYS) ||
        (D->Secure && !D->Pairwise && Action == WDI_EXEMPT_NO_EXEMPTION)) return SWIFI_INVALID;
    if (D->Core.flow_count == 1) Priority = 0;
    UCHAR Fifo = swifi_priority_fifo(Priority);
    USHORT Ring = (USHORT)(SWIFI_FIRST_FLOW_RING + Fifo);
    if (D->Flows[Fifo].state == SWIFI_FLOW_CLOSED) {
        SWIFI_WIN_RING *Flow = &D->Hardware->Shared.H2d[Ring];
        swifi_result R = swifi_data_create_flow(&D->Core, Ring, 0, Priority, D->Address,
            Buffer, Flow->LogicalAddress.QuadPart, Flow->Core.depth);
        if (R == SWIFI_OK) D->FlowEpoch[Fifo] = D->Epoch;
        else if (R == SWIFI_RETRY) Q->WaitRing = SWIFI_CONTROL_RING;
        return R == SWIFI_OK ? SWIFI_RETRY : R;
    }
    if (D->FlowEpoch[Fifo] != D->Epoch || D->Flows[Fifo].state != SWIFI_FLOW_OPEN)
        return SWIFI_RETRY;
    swifi_result R = swifi_data_tx_with_exemption(&D->Core, Ring, Priority, Action, Buffer,
        D->TxMemory.Logical.QuadPart + Offset + SWIFI_ETH_HEADER_SIZE,
        (USHORT)(Length - SWIFI_ETH_HEADER_SIZE), &D->Tx[I]);
    if (R == SWIFI_OK) {
        D->Tx[I].Owned = TRUE;
        D->Tx[I].Eapol = D->HeadEapol;
        if (D->HeadEapol) ++D->EapolPending;
        D->Tx[I].Record = Record;
    } else if (R == SWIFI_RETRY) Q->WaitRing = Ring;
    return R;
}

static VOID AdvanceTx(SWIFI_QUEUE *Q)
{
    NET_RING *Packets = Q->Packets, *Fragments = Q->Fragments;
    Q->WaitRing = MAXUSHORT;
    while (Packets->NextIndex != Packets->EndIndex) {
        ULONG Index = Packets->NextIndex;
        NET_PACKET *Packet = NetRingGetPacketAtIndex(Packets, Index);
        SWIFI_TX_RECORD *R = &Q->Records[Index];
        R->Done = TRUE;
        if (!Packet->Ignore) {
            swifi_result Result = Transmit(Q, Packet, R);
            if (Result == SWIFI_RETRY) break;
            Q->Data->HeadEapol = FALSE;
            R->Done = Result != SWIFI_OK;
            if (R->Done) ++Q->Data->TxErrors;
            Fragments->NextIndex = NetRingAdvanceIndex(Fragments, Packet->FragmentIndex, Packet->FragmentCount);
        }
        R->FragmentEnd = Fragments->NextIndex;
        Packets->NextIndex = NetRingIncrementIndex(Packets, Index);
    }
    while (Packets->BeginIndex != Packets->NextIndex) {
        SWIFI_TX_RECORD *R = &Q->Records[Packets->BeginIndex];
        if (!R->Done) break;
        Fragments->BeginIndex = R->FragmentEnd;
        Packets->BeginIndex = NetRingIncrementIndex(Packets, Packets->BeginIndex);
    }
    UpdateEapolEvent(Q->Data);
    Q->SeenProgress = Q->Data->TxProgress;
}

static VOID AdvanceRx(SWIFI_QUEUE *Q)
{
    SWIFI_STATION_DATA *D = Q->Data;
    NET_RING *Packets = Q->Packets, *Fragments = Q->Fragments;
    while (D->Enabled && D->Associated && !IsListEmpty(&D->ReadyRx) && Packets->BeginIndex != Packets->EndIndex &&
        Fragments->BeginIndex != Fragments->EndIndex) {
        SWIFI_RX_BUFFER *Slot = CONTAINING_RECORD(RemoveHeadList(&D->ReadyRx), SWIFI_RX_BUFFER, Link);
        Slot->State = SwifiRxFree;
        if (Slot->DerivedClear && !swifi_prekey_ethernet(Slot->KeyEpoch, D->KeyEpoch,
            Slot->KeyInFlight, D->Pairwise || D->Group || D->UpdatingKeys)) {
            ++D->RxDrops;
            continue;
        }
        ULONG Id = Fragments->BeginIndex;
        NET_FRAGMENT *F = NetRingGetFragmentAtIndex(Fragments, Id);
        PUCHAR Target = NetExtensionGetFragmentVirtualAddress(&Q->Va, Id)->VirtualAddress;
        PUCHAR Frame = D->RxMemory.Memory + (Slot - D->Rx) * SWIFI_PACKET_SIZE + Slot->Offset;
        SIZE_T Length = Target && Slot->Epoch == D->Epoch && D->Associated ?
            swifi_station_rx(Frame, Slot->Length, D->Address, D->Peer, D->Sequence,
                Slot->Protected, Target, (SIZE_T)F->Capacity) : 0;
        if (!Length) { ++D->RxDrops; continue; }
        NET_PACKET *Packet = NetRingGetPacketAtIndex(Packets, Packets->BeginIndex);
        F->Offset = 0;
        F->ValidLength = Length;
        Packet->Ignore = FALSE;
        Packet->FragmentIndex = Id;
        Packet->FragmentCount = 1;
        RtlZeroMemory(&Packet->Layout, sizeof(Packet->Layout));
        Packet->Layout.Layer2Type = NetPacketLayer2TypeIeee80211;
        Packet->Layout.Layer2HeaderLength = (Frame[12] << 8 | Frame[13]) >= 1536 ? 32 : 24;
        ++D->Sequence;
        Fragments->BeginIndex = NetRingIncrementIndex(Fragments, Id);
        Packets->BeginIndex = NetRingIncrementIndex(Packets, Packets->BeginIndex);
    }
    Fragments->NextIndex = Fragments->BeginIndex;
    Packets->NextIndex = Packets->BeginIndex;
}

static VOID QueueAdvance(NETPACKETQUEUE Handle)
{
    SWIFI_QUEUE *Q = SwifiQueueContext(Handle);
    KIRQL Irql;
    KeAcquireSpinLock(&Q->Data->Hardware->ProtocolLock, &Irql);
    if (!Q->Cancelling) {
        if (Q->IsTx) AdvanceTx(Q);
        else AdvanceRx(Q);
        Service(Q->Data);
    }
    BOOLEAN Fault = Q->Data->Enabled && Q->Data->FlowFault;
    KeReleaseSpinLock(&Q->Data->Hardware->ProtocolLock, Irql);
    if (Fault) SwifiPnpFailure(SwifiDeviceContext(Q->Data->Hardware->Device), STATUS_DEVICE_PROTOCOL_ERROR);
}

static VOID QueueStart(NETPACKETQUEUE Handle)
{
    SWIFI_QUEUE *Q = SwifiQueueContext(Handle);
    SWIFI_STATION_DATA *D = Q->Data;
    KIRQL Irql;
    KeAcquireSpinLock(&D->Hardware->ProtocolLock, &Irql);
    Q->Cancelling = Q->Armed = FALSE;
    if (Q->IsTx) D->TxQueue = Q;
    else {
        D->RxQueue = Q;
    }
    KeReleaseSpinLock(&D->Hardware->ProtocolLock, Irql);
    SwifiDataNotify(SwifiDeviceContext(D->Hardware->Device));
}

static VOID QueueSetNotify(NETPACKETQUEUE Handle, BOOLEAN Enabled)
{
    SWIFI_QUEUE *Q = SwifiQueueContext(Handle);
    KIRQL Irql;
    KeAcquireSpinLock(&Q->Data->Hardware->ProtocolLock, &Irql);
    Q->Armed = Enabled && !Q->Cancelling;
    KeReleaseSpinLock(&Q->Data->Hardware->ProtocolLock, Irql);
    if (Enabled) SwifiDataNotify(SwifiDeviceContext(Q->Data->Hardware->Device));
    else {
        ExWaitForRundownProtectionRelease(&Q->Notifications);
        ExReInitializeRundownProtection(&Q->Notifications);
    }
}

static VOID QueueCancel(NETPACKETQUEUE Handle)
{
    SWIFI_QUEUE *Q = SwifiQueueContext(Handle);
    SWIFI_STATION_DATA *D = Q->Data;
    KIRQL Irql;
    KeAcquireSpinLock(&D->Hardware->ProtocolLock, &Irql);
    Q->Cancelling = TRUE;
    Q->Armed = FALSE;
    if (Q->IsTx) {
        /* Only private common buffers remain device-owned. */
        for (ULONG I = 0; I < SwifiNumTx; ++I) D->Tx[I].Record = NULL;
        D->HeadEapol = FALSE;
        UpdateEapolEvent(D);
    } else {
        ++D->RxQueueEpoch;
        if (D->Enabled) DropReady(D);
        for (ULONG I = Q->Packets->BeginIndex; I != Q->Packets->EndIndex; I = NetRingIncrementIndex(Q->Packets, I))
            NetRingGetPacketAtIndex(Q->Packets, I)->Ignore = TRUE;
    }
    Q->Packets->BeginIndex = Q->Packets->NextIndex = Q->Packets->EndIndex;
    Q->Fragments->BeginIndex = Q->Fragments->NextIndex = Q->Fragments->EndIndex;
    KeReleaseSpinLock(&D->Hardware->ProtocolLock, Irql);
}

static VOID QueueStop(NETPACKETQUEUE Handle)
{
    SWIFI_QUEUE *Q = SwifiQueueContext(Handle);
    SWIFI_STATION_DATA *D = Q->Data;
    KIRQL Irql;
    KeAcquireSpinLock(&D->Hardware->ProtocolLock, &Irql);
    Q->Armed = FALSE;
    if (Q->IsTx) D->TxQueue = NULL;
    else D->RxQueue = NULL;
    KeReleaseSpinLock(&D->Hardware->ProtocolLock, Irql);
    ExWaitForRundownProtectionRelease(&Q->Notifications);
    ExReInitializeRundownProtection(&Q->Notifications);
}

static NTSTATUS CreateQueue(NETADAPTER Adapter, NETTXQUEUE_INIT *Tx, NETRXQUEUE_INIT *Rx)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    NET_PACKET_QUEUE_CONFIG Config;
    NETPACKETQUEUE Handle;
    NET_EXTENSION_QUERY Query;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWIFI_QUEUE);
    NET_PACKET_QUEUE_CONFIG_INIT(&Config, QueueAdvance, QueueSetNotify, QueueCancel);
    Config.EvtStart = QueueStart;
    Config.EvtStop = QueueStop;
    NTSTATUS Status = Tx ? NetTxQueueCreate(Tx, &Attributes, &Config, &Handle) :
        NetRxQueueCreate(Rx, &Attributes, &Config, &Handle);
    if (!NT_SUCCESS(Status)) return Status;
    SWIFI_QUEUE *Q = SwifiQueueContext(Handle);
    Q->Data = &SwifiAdapterContext(Adapter)->Device->Data;
    Q->Handle = Handle;
    Q->IsTx = Tx != NULL;
    Q->WaitRing = MAXUSHORT;
    ExInitializeRundownProtection(&Q->Notifications);
    NET_RING_COLLECTION const *Rings = Tx ? NetTxQueueGetRingCollection(Handle) : NetRxQueueGetRingCollection(Handle);
    Q->Packets = NetRingCollectionGetPacketRing(Rings);
    Q->Fragments = NetRingCollectionGetFragmentRing(Rings);
    NET_EXTENSION_QUERY_INIT(&Query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
        NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    if (Tx) NetTxQueueGetExtension(Handle, &Query, &Q->Va);
    else NetRxQueueGetExtension(Handle, &Query, &Q->Va);
    if (!Q->Va.Enabled) Status = STATUS_NOT_SUPPORTED;
    if (Tx) {
        NET_EXTENSION_QUERY_INIT(&Query, NET_PACKET_EXTENSION_WIFI_EXEMPTION_ACTION_NAME,
            NET_PACKET_EXTENSION_WIFI_EXEMPTION_ACTION_VERSION_1, NetExtensionTypePacket);
        NetTxQueueGetExtension(Handle, &Query, &Q->TxExemption);
    }
    if (NT_SUCCESS(Status) && Tx) {
        WDFMEMORY Memory;
        WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
        Attributes.ParentObject = Handle;
        Status = WdfMemoryCreate(&Attributes, NonPagedPoolNx, 'tFwS',
            (SIZE_T)Q->Packets->NumberOfElements * sizeof(*Q->Records), &Memory, (PVOID *)&Q->Records);
    }
    if (!NT_SUCCESS(Status)) WdfObjectDelete(Handle);
    return Status;
}

NTSTATUS SwifiCreateTxQueue(NETADAPTER Adapter, NETTXQUEUE_INIT *Init)
{
    return CreateQueue(Adapter, Init, NULL);
}

NTSTATUS SwifiCreateRxQueue(NETADAPTER Adapter, NETRXQUEUE_INIT *Init)
{
    return CreateQueue(Adapter, NULL, Init);
}
