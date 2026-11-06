/* SPDX-License-Identifier: GPL-2.0-only */
#include "join.h"
#include "join_firmware.h"
#include "station_keys.h"
#include "station_filter.h"

typedef struct JOIN_CANDIDATE {
    LIST_ENTRY Link;
    swifi_bss_profile Profile;
    UCHAR Peer[6], Channel, Pmkid[16];
    BOOLEAN HavePmkid, ExcludeUnencrypted, Reported;
    ULONG AllowedPhys;
    WDI_PHY_TYPE Phy;
    USHORT BeaconLength, VendorLength, VendorAdded, VendorRemoved;
    UCHAR Bytes[1]; /* Actual beacon/probe body followed by association vendor IEs. */
} JOIN_CANDIDATE;

typedef struct JOIN_TASK {
    WDFMEMORY Memory;
    WDI_MESSAGE_HEADER *Header;
    UINT16 Id;
} JOIN_TASK;

struct SWIFI_JOIN {
    SWIFI_DEVICE *Owner;
    SWIFI_SCAN *Scan;
    SWIFI_WIN_CONTROL *Control;
    SWIFI_FIRMWARE_INFO *Info;
    TLV_CONTEXT Tlv;
    WDFWAITLOCK Lock;
    WDFTIMER Timer;
    KEVENT Left;
    ULONGLONG Deadline, ConnectEnd;
    WDI_PORT_ID Port, PeerPort, ResetPort;
    UCHAR ResetPeer[6];
    BOOLEAN ResetNotice;
    ULONG DisallowedBands;
    BOOLEAN Online, Poisoned, Associated, Leaving, Announced, RetryNext;
    JOIN_TASK Connect, Leave;
    LIST_ENTRY Pending;
    JOIN_CANDIDATE *Active;
    SWIFI_STATION_KEYS Keys;
    SWIFI_ASSOC_FRAMES Frames;
    ULONG TxKbps, RxKbps, IndicatedTx, IndicatedRx;
    LONG IndicatedQuality;
    WDFWORKITEM LinkWorker;
};
typedef struct LINK_WORK { SWIFI_JOIN *Join; } LINK_WORK;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LINK_WORK, LinkWorkContext);
typedef struct JOIN_TIMER_CONTEXT { SWIFI_JOIN *Join; } JOIN_TIMER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(JOIN_TIMER_CONTEXT, JoinTimerContext);
static EVT_WDF_TIMER JoinTimeout;

static VOID DropPending(SWIFI_JOIN *J)
{
    while (!IsListEmpty(&J->Pending)) {
        JOIN_CANDIDATE *C = CONTAINING_RECORD(RemoveHeadList(&J->Pending), JOIN_CANDIDATE, Link);
        ExFreePoolWithTag(C, 'jFwS');
    }
}

static NTSTATUS NewTask(SWIFI_JOIN *J, JOIN_TASK *T, UINT16 Id, const WDI_MESSAGE_HEADER *H)
{
    if (T->Memory) return STATUS_DEVICE_BUSY;
    NTSTATUS Status = SwifiAllocWdiMessage(J->Owner->Hw.Device, sizeof(*H), &T->Memory, &T->Header);
    if (NT_SUCCESS(Status)) { *T->Header = *H; T->Id = Id; }
    return Status;
}

static VOID Finish(SWIFI_JOIN *J, JOIN_TASK *T, NTSTATUS Status)
{
    if (T->Memory) {
        if (T == &J->Connect) SwifiReportJoin(&J->Owner->Report, Status);
        T->Header->Status = (NDIS_STATUS)Status;
        WifiDeviceReceiveIndication(J->Owner->Hw.Device, T->Id, T->Memory);
        WdfObjectDelete(T->Memory);
        RtlZeroMemory(T, sizeof(*T));
    }
}

static VOID Arm(SWIFI_JOIN *J, ULONG Milliseconds)
{
    J->Deadline = KeQueryInterruptTime() + (ULONGLONG)Milliseconds * 10000;
    WdfTimerStart(J->Timer, WDF_REL_TIMEOUT_IN_MS(Milliseconds));
}

static NTSTATUS Indicate(SWIFI_JOIN *J, UINT16 Id, UINT8 *Generated, ULONG Length)
{
    if (!Generated || Length < sizeof(WDI_MESSAGE_HEADER)) return STATUS_DEVICE_PROTOCOL_ERROR;
    WDFMEMORY Memory;
    WDI_MESSAGE_HEADER *Header;
    NTSTATUS Status = SwifiAllocWdiMessage(J->Owner->Hw.Device, Length, &Memory, &Header);
    if (NT_SUCCESS(Status)) {
        RtlCopyMemory(Header, Generated, Length);
        RtlZeroMemory(Header, sizeof(*Header));
        Header->PortId = J->Port;
        Header->Status = NDIS_STATUS_SUCCESS;
        Header->TransactionId = WDI_TRANSACTION_ID_UNSOLICIT;
        WifiDeviceReceiveIndication(J->Owner->Hw.Device, Id, Memory);
        WdfObjectDelete(Memory);
    }
    return Status;
}

static WDI_PHY_TYPE NegotiatedPhy(const SWIFI_ASSOC_FRAMES *Frames, UCHAR Channel)
{
    WDI_PHY_TYPE Type = Channel > 14 ? WDI_PHY_TYPE_OFDM : WDI_PHY_TYPE_UNKNOWN;
    for (ULONG I = 6; I < Frames->RespLen; I += 2 + Frames->Resp[I+1]) {
        const UCHAR *Ie = Frames->Resp+I;
        if (Ie[0] == 191 && Ie[1] >= 12) return WDI_PHY_TYPE_VHT;
        if (Ie[0] == 45 && Ie[1] >= 26) Type = WDI_PHY_TYPE_HT;
        if (Type == WDI_PHY_TYPE_HT || Channel > 14 || (Ie[0] != 1 && Ie[0] != 50)) continue;
        for (ULONG K = 0; K < Ie[1]; ++K) {
            UCHAR Rate = Ie[2+K] & 0x7f;
            if (Rate == 12 || Rate == 18 || Rate == 24 || Rate == 36 || Rate == 48 ||
                Rate == 72 || Rate == 96 || Rate == 108) Type = WDI_PHY_TYPE_ERP;
            else if (Type == WDI_PHY_TYPE_UNKNOWN && (Rate == 2 || Rate == 4 || Rate == 11 || Rate == 22))
                Type = WDI_PHY_TYPE_HRDSSS;
        }
    }
    return Type;
}

static NTSTATUS AssocResult(SWIFI_JOIN *J, WDI_ASSOC_STATUS Result, ULONG FirmwareReason)
{
    JOIN_CANDIDATE *C = J->Active;
    WDI_INDICATION_ASSOCIATION_RESULT_LIST List = {0};
    WDI_ASSOCIATION_RESULT_CONTAINER Entry = {0};
    WDI_ASSOCIATION_RESULT_PARAMETERS *P = &Entry.AssociationResultParameters;
    BOOLEAN Secure = C->Profile.auth == SWIFI_AUTH_PSK_CCMP;
    WDI_PHY_TYPE Phy = WDI_PHY_TYPE_UNKNOWN;
    List.AssociationResults.ElementCount = 1;
    List.AssociationResults.pElements = &Entry;
    RtlCopyMemory(Entry.BSSID.Address, C->Peer, 6);
    P->AssociationStatus = Result;
    P->IHVAssociationStatus = FirmwareReason;
    P->AuthAlgorithm = Secure ? WDI_AUTH_ALGO_RSNA_PSK : WDI_AUTH_ALGO_80211_OPEN;
    P->UnicastCipherAlgorithm = P->MulticastDataCipherAlgorithm = Secure ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
    P->MulticastMgmtCipherAlgorithm = WDI_CIPHER_ALGO_NONE;
    P->BandID = C->Channel <= 14 ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
    P->DSInfo = WDI_DS_UNKNOWN;
    if (Result == WDI_ASSOC_STATUS_SUCCESS) {
        P->PortAuthorized = !Secure;
        Entry.Optional.AssociationRequestFrame_IsPresent = TRUE;
        Entry.AssociationRequestFrame.pElements = J->Frames.Req;
        Entry.AssociationRequestFrame.ElementCount = J->Frames.ReqLen;
        Entry.Optional.AssociationResponseFrame_IsPresent = TRUE;
        Entry.AssociationResponseFrame.pElements = J->Frames.Resp;
        Entry.AssociationResponseFrame.ElementCount = J->Frames.RespLen;
        /* Use negotiated response IEs, rather than the AP's advertised maximum. */
        Phy = C->Phy;
        for (ULONG I = 6; I < J->Frames.RespLen; I += 2 + J->Frames.Resp[I+1]) {
            UCHAR *Ie = J->Frames.Resp+I;
            if (Ie[0] == 221 && Ie[1] >= 6 && !memcmp(Ie+2,"\x00\x50\xf2\x02",4))
                P->WMMQoSEnabled = TRUE;
        }
    }
    Entry.ActivePhyTypeList.pElements = &Phy;
    Entry.ActivePhyTypeList.ElementCount = 1;
    Entry.Optional.BeaconProbeResponse_IsPresent = TRUE;
    Entry.BeaconProbeResponse.pElements = C->Bytes;
    Entry.BeaconProbeResponse.ElementCount = C->BeaconLength;
    UINT8 *Generated = NULL;
    ULONG Length;
    NTSTATUS Status = (NTSTATUS)GenerateWdiIndicationAssociationResultFromIhv(&List,
        sizeof(WDI_MESSAGE_HEADER), &J->Tlv, &Length, &Generated);
    if (NT_SUCCESS(Status)) Status = Indicate(J, WDI_INDICATION_ASSOCIATION_RESULT, Generated, Length);
    if (Generated) FreeGenerated(Generated);
    if (NT_SUCCESS(Status)) C->Reported = TRUE;
    return Status;
}

static NTSTATUS Disassociated(SWIFI_JOIN *J, const UCHAR Peer[6], WDI_ASSOC_STATUS Reason)
{
    WDI_INDICATION_DISASSOCIATION_PARAMETERS P = {0};
    RtlCopyMemory(P.DisconnectIndicationParameters.MacAddress.Address, Peer, 6);
    P.DisconnectIndicationParameters.DisassociationWABIReason = Reason;
    UINT8 *Generated = NULL;
    ULONG Length;
    NTSTATUS Status = (NTSTATUS)GenerateWdiIndicationDisassociationFromIhv(&P, sizeof(WDI_MESSAGE_HEADER),
        &J->Tlv, &Length, &Generated);
    if (NT_SUCCESS(Status)) Status = Indicate(J, WDI_INDICATION_DISASSOCIATION, Generated, Length);
    if (Generated) FreeGenerated(Generated);
    return Status;
}

static VOID MediaState(SWIFI_JOIN *J, BOOLEAN Connected)
{
    SWIFI_DEVICE *D = J->Owner;
    KIRQL Irql;
    KeAcquireSpinLock(&D->Hw.ProtocolLock, &Irql);
    NETADAPTER Adapter = D->Station;
    if (Adapter) WdfObjectReference(Adapter);
    KeReleaseSpinLock(&D->Hw.ProtocolLock, Irql);
    if (!Adapter) return;
    NET_ADAPTER_LINK_STATE State;
    NET_ADAPTER_LINK_STATE_INIT(&State, NET_ADAPTER_LINK_SPEED_UNKNOWN,
        Connected ? MediaConnectStateConnected : MediaConnectStateDisconnected, MediaDuplexStateUnknown,
        NetAdapterPauseFunctionTypeUnsupported, NetAdapterAutoNegotiationFlagNone);
    if (Connected && J->TxKbps) State.TxLinkSpeed = (ULONG64)J->TxKbps * 1000;
    if (Connected && J->RxKbps) State.RxLinkSpeed = (ULONG64)J->RxKbps * 1000;
    NetAdapterSetLinkState(Adapter, &State);
    WdfObjectDereference(Adapter);
}

static VOID ReadLink(WDFWORKITEM Work)
{
    SWIFI_JOIN *J = LinkWorkContext(Work)->Join;
    WdfWaitLockAcquire(J->Lock, NULL);
    if (!J->Online || J->Poisoned || J->Leaving || !J->Announced || !J->Active ||
        J->Port != J->PeerPort || !J->Control->Running) goto Done;

    SWIFI_DEVICE *D = J->Owner;
    KIRQL Irql;
    KeAcquireSpinLock(&D->Hw.ProtocolLock, &Irql);
    BOOLEAN Connected = D->Data.Enabled && D->Data.Associated && D->Station != NULL &&
        RtlEqualMemory(D->Data.Peer, J->Active->Peer, 6);
    LONG Dbm = D->Data.SampleValid && KeQueryInterruptTime() - D->Data.SampleTime < 50000000 ?
        D->Data.SampleDbm : 0;
    KeReleaseSpinLock(&D->Hw.ProtocolLock, Irql);
    if (!Connected) goto Done;

    /* sta_info is peer-specific. The rates describe the last successful
     * frame in each direction. GET_RATE supplies only the station TX rate. */
    UCHAR Sta[320] = {0};
    USHORT Returned = 0;
    uint32_t Tx = 0, Rx = 0;
    RtlCopyMemory(Sta, J->Active->Peer, 6);
    if (NT_SUCCESS(SwifiFirmwareIovarGet(J->Control, 0, "sta_info", Sta, sizeof(Sta), &Returned, NULL)))
        (VOID)swifi_station_rates(Sta, Returned, J->Active->Peer, &Tx, &Rx);
    if (!Tx) {
        ULONG Rate = 0;
        if (NT_SUCCESS(SwifiFirmwareCommand(J->Control, 0, 12, &Rate, sizeof(Rate), &Returned, NULL)) &&
            Returned >= sizeof(Rate) && Rate <= 2000) Tx = Rate * 500;
    }
    if (!Dbm) {
        /* Switchroot issues WLC_GET_RSSI for the associated station with a
         * zero-filled brcmf_scb_val_le; the returned first word is signed. */
        LONG Scb[3] = {0};
        if (NT_SUCCESS(SwifiFirmwareCommand(J->Control, 0, 127, Scb, sizeof(Scb), &Returned, NULL)) &&
            Returned >= sizeof(LONG) && Scb[0] >= -127 && Scb[0] < 0) Dbm = Scb[0];
    }
    /* Keep the last measured rate in this association if a later query
     * omits a direction. Never substitute the TX rate for the RX rate. */
    if ((Tx && J->TxKbps != Tx) || (Rx && J->RxKbps != Rx)) {
        if (Tx) J->TxKbps = Tx;
        if (Rx) J->RxKbps = Rx;
        MediaState(J, TRUE);
    }
    if (Dbm && J->TxKbps && J->RxKbps) {
        LONG Quality = swifi_signal_quality(Dbm);
        LONG Delta = Quality - J->IndicatedQuality;
        if (J->IndicatedTx != J->TxKbps || J->IndicatedRx != J->RxKbps || Delta > 5 || Delta < -5) {
            WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS P = {0};
            RtlCopyMemory(P.LinkStateChangeParameters.PeerMACAddress.Address, J->Active->Peer, 6);
            P.LinkStateChangeParameters.TxLinkSpeed = J->TxKbps;
            P.LinkStateChangeParameters.RxLinkSpeed = J->RxKbps;
            P.LinkStateChangeParameters.LinkQuality = (UCHAR)Quality;
            UINT8 *Generated = NULL;
            ULONG Length;
            NTSTATUS Status = (NTSTATUS)GenerateWdiIndicationLinkStateChangeFromIhv(&P,
                sizeof(WDI_MESSAGE_HEADER), &J->Tlv, &Length, &Generated);
            if (NT_SUCCESS(Status)) Status = Indicate(J, WDI_INDICATION_LINK_STATE_CHANGE, Generated, Length);
            if (Generated) FreeGenerated(Generated);
            if (NT_SUCCESS(Status)) {
                J->IndicatedTx = J->TxKbps;
                J->IndicatedRx = J->RxKbps;
                J->IndicatedQuality = Quality;
            }
        }
    }
Done:
    WdfWaitLockRelease(J->Lock);
}

static NTSTATUS Vendors(SWIFI_JOIN *J, BOOLEAN Add)
{
    JOIN_CANDIDATE *C = J->Active;
    USHORT *Position = Add ? &C->VendorAdded : &C->VendorRemoved;
    ULONG End = Add ? C->VendorLength : C->VendorAdded;
    while (*Position < End) {
        UCHAR *Ie = C->Bytes+C->BeaconLength+*Position;
        USHORT Size = (USHORT)(2+Ie[1]);
        NTSTATUS Status = SwifiSetVendorIe(J->Control, Ie, Size, Add, 0x20);
        if (!NT_SUCCESS(Status)) return Status;
        *Position += Size;
    }
    return STATUS_SUCCESS;
}

static VOID FreeActive(SWIFI_JOIN *J)
{
    if (J->Active) ExFreePoolWithTag(J->Active, 'jFwS');
    J->Active = NULL;
}

static VOID Poison(SWIFI_JOIN *J, NTSTATUS Reason)
{
    J->Poisoned = TRUE;
    SwifiFilterStop(J->Owner, FALSE);
    J->Deadline = 0;
    SwifiDataLink(J->Owner, NULL, FALSE, FALSE, FALSE);
    DropPending(J);
    Finish(J, &J->Connect, Reason);
    Finish(J, &J->Leave, Reason);
    KeSetEvent(&J->Left, IO_NO_INCREMENT, FALSE);
    SwifiPnpFailure(J->Owner, Reason);
}

static BOOLEAN Listed(const WDI_ADDRESS_LIST_CONTAINER *List, const UCHAR *Mac)
{
    for (ULONG I = 0; I < List->ElementCount; ++I)
        if (RtlEqualMemory(List->pElements[I].Address, Mac, 6)) return TRUE;
    return FALSE;
}

static unsigned AuthMask(const WDI_CONNECT_PARAMETERS_CONTAINER *C)
{
    BOOLEAN Open = FALSE, Psk = FALSE, PairNone = FALSE, GroupNone = FALSE, PairCcmp = FALSE, GroupCcmp = FALSE;
    for (ULONG I = 0; I < C->AuthenticationAlgorithms.ElementCount; ++I) {
        WDI_AUTH_ALGORITHM A = C->AuthenticationAlgorithms.pElements[I];
        Open |= A == WDI_AUTH_ALGO_80211_OPEN;
        Psk |= A == WDI_AUTH_ALGO_RSNA_PSK;
    }
    for (ULONG I = 0; I < C->UnicastCipherAlgorithms.ElementCount; ++I) {
        WDI_CIPHER_ALGORITHM A = C->UnicastCipherAlgorithms.pElements[I];
        PairCcmp |= A == WDI_CIPHER_ALGO_CCMP;
        PairNone |= A == WDI_CIPHER_ALGO_NONE;
    }
    for (ULONG I = 0; I < C->MulticastCipherAlgorithms.ElementCount; ++I) {
        WDI_CIPHER_ALGORITHM A = C->MulticastCipherAlgorithms.pElements[I];
        GroupCcmp |= A == WDI_CIPHER_ALGO_CCMP;
        GroupNone |= A == WDI_CIPHER_ALGO_NONE;
    }
    if (C->Optional.RsnaAkmCipherSuites_IsPresent && C->RsnaAkmCipherSuites.ElementCount) {
        BOOLEAN Allowed = FALSE;
        for (ULONG I = 0; I < C->RsnaAkmCipherSuites.ElementCount; ++I) {
            const RSNA_AKM_CIPHER_PAIR *Pair = &C->RsnaAkmCipherSuites.pElements[I];
            if (Pair->akm == rsna_akm_psk && Pair->cipher == rsna_cipher_ccmp_128) Allowed = TRUE;
        }
        Psk &= Allowed;
    }
    if (C->Optional.RsnaAkmSuites_IsPresent && C->RsnaAkmSuites.ElementCount) {
        BOOLEAN Allowed = FALSE;
        for (ULONG I = 0; I < C->RsnaAkmSuites.ElementCount; ++I)
            if (C->RsnaAkmSuites.pElements[I] == rsna_akm_psk) Allowed = TRUE;
        Psk &= Allowed;
    }
    return (Psk && PairCcmp && GroupCcmp ? SWIFI_AUTH_PSK_CCMP : 0) |
        (Open && PairNone && GroupNone && !C->ConnectionSettings.ExcludeUnencrypted ? SWIFI_AUTH_OPEN : 0);
}

static BOOLEAN VendorBlob(const WDI_BYTE_BLOB *Blob)
{
    if (Blob->ElementCount > SwifiScanVendorMax || !swifi_valid_ies(Blob->pElements, Blob->ElementCount)) return FALSE;
    for (ULONG I = 0; I < Blob->ElementCount; I += 2 + Blob->pElements[I+1])
        if (Blob->pElements[I] != 221 || Blob->pElements[I+1] < 4) return FALSE;
    return TRUE;
}

static NTSTATUS Candidates(SWIFI_JOIN *J, const WDI_TASK_CONNECT_PARAMETERS *P)
{
    const WDI_CONNECT_PARAMETERS_CONTAINER *C = &P->ConnectParameters;
    ULONG Mask = AuthMask(C), Blocked = J->DisallowedBands;
    if (!Mask || C->SSIDList.ElementCount != 1 || C->ConnectionSettings.HostFIPSModeEnabled ||
        C->ConnectionSettings.IsFIPSConnection || C->ConnectionSettings.MFPEnabled ||
        C->ConnectionSettings.RoamRequest || C->Optional.HESSIDInfo_IsPresent ||
        C->Optional.OWEDHIE_IsPresent || C->ConnectionSettings.MloConnectionSupported)
        return STATUS_NOT_SUPPORTED;
    const WDI_SSID *Ssid = &C->SSIDList.pElements[0];
    if (!Ssid->ElementCount || Ssid->ElementCount > 32) return STATUS_INVALID_PARAMETER;
    if (C->Optional.UnavailableBandList_IsPresent) {
        for (ULONG I = 0; I < C->UnavailableBandList.ElementCount; ++I) {
            if (C->UnavailableBandList.pElements[I] == WDI_BAND_ID_2400) Blocked |= SWIFI_BAND_2GHZ;
            if (C->UnavailableBandList.pElements[I] == WDI_BAND_ID_5000) Blocked |= SWIFI_BAND_5GHZ;
        }
    }
    ULONG Phys = MAXULONG;
    if (C->Optional.ActivePhyTypeList_IsPresent && C->ActivePhyTypeList.ElementCount) {
        Phys = 0;
        for (ULONG I = 0; I < C->ActivePhyTypeList.ElementCount; ++I) {
            ULONG Type = (ULONG)C->ActivePhyTypeList.pElements[I];
            if (Type == WDI_PHY_TYPE_ANY) { Phys = MAXULONG; break; }
            if (Type < 32) Phys |= 1u << Type;
        }
        if (!Phys) return STATUS_NOT_SUPPORTED;
    }
    WDI_BYTE_BLOB Empty = {0};
    const WDI_BYTE_BLOB *Global = C->Optional.AssociationRequestVendorIE_IsPresent ? &C->AssociationRequestVendorIE : &Empty;
    if (!VendorBlob(Global)) return STATUS_INVALID_PARAMETER;
    for (ULONG I = 0; I < P->PreferredBSSEntryList.ElementCount; ++I) {
        const WDI_CONNECT_BSS_ENTRY_CONTAINER *B = &P->PreferredBSSEntryList.pElements[I];
        const UCHAR *Peer = B->BSSID.Address;
        if ((Peer[0] & 1) || !memcmp(Peer,"\0\0\0\0\0\0",6) || Listed(&C->DisallowedBSSIDs, Peer) ||
            (C->AllowedBSSIDs.ElementCount && !Listed(&C->AllowedBSSIDs, Peer)) ||
            B->Optional.FTInitialAssocParameters_IsPresent || B->Optional.FTReAssocParameters_IsPresent) continue;
        ULONG Ch = B->ChannelInfo.ChannelNumber;
        if (!Ch || Ch > 196 || B->ChannelInfo.BandId != (Ch <= 14 ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000) ||
            (Blocked & (Ch <= 14 ? SWIFI_BAND_2GHZ : SWIFI_BAND_5GHZ))) continue;
        ULONG K = 0;
        while (K < J->Info->ChannelCount && J->Info->Channels[K] != Ch) ++K;
        if (K == J->Info->ChannelCount) continue;
        const WDI_BYTE_BLOB *Body = NULL;
        WDI_BYTE_BLOB Cached = {0};
        swifi_bss_profile Profile;
        const WDI_BYTE_BLOB *Frames[] = {&B->ProbeResponseFrame, &B->BeaconFrame};
        for (ULONG K = 0; K < RTL_NUMBER_OF(Frames); ++K) {
            const WDI_BYTE_BLOB *Frame = Frames[K];
            if (Frame->ElementCount && Frame->ElementCount <= SwifiAssocBodyMax &&
                swifi_match_bss(Frame->pElements, Frame->ElementCount, Ssid->pElements, Ssid->ElementCount,
                    C->ConnectionSettings.HiddenNetwork, Mask, &Profile)) { Body = Frame; break; }
            /* A supplied security mismatch must not be overridden by an older cache entry. */
            if (Frame->ElementCount && K == 0) break;
        }
        if (!Body && !B->ProbeResponseFrame.ElementCount && !B->BeaconFrame.ElementCount) {
            Cached.pElements = J->Frames.Req; /* scratch until firmware reports the association */
            for (ULONG K = 0; K < 2; ++K) {
                Cached.ElementCount = SwifiScanLookup(J->Scan, J->Port, Peer, Ch, K == 0,
                    Cached.pElements, sizeof(J->Frames.Req));
                if (!Cached.ElementCount) continue;
                if (swifi_match_bss(Cached.pElements, Cached.ElementCount, Ssid->pElements,
                    Ssid->ElementCount, C->ConnectionSettings.HiddenNetwork, Mask, &Profile)) Body = &Cached;
                break;
            }
        }
        if (!Body) continue;
        if (B->Optional.PMKID_IsPresent && (B->PMKID.ElementCount != 16 || Profile.auth != SWIFI_AUTH_PSK_CCMP))
            continue;
        const WDI_BYTE_BLOB *Local = B->Optional.AssociationRequestVendorIE_IsPresent ? &B->AssociationRequestVendorIE : &Empty;
        if (!VendorBlob(Local) || Global->ElementCount + Local->ElementCount > SwifiScanVendorMax) continue;
        JOIN_CANDIDATE *New = ExAllocatePool2(POOL_FLAG_NON_PAGED, FIELD_OFFSET(JOIN_CANDIDATE, Bytes) +
            Body->ElementCount + Global->ElementCount + Local->ElementCount, 'jFwS');
        if (!New) return STATUS_INSUFFICIENT_RESOURCES;
        New->Profile = Profile;
        New->ExcludeUnencrypted = C->ConnectionSettings.ExcludeUnencrypted;
        New->Channel = (UCHAR)Ch;
        New->AllowedPhys = Phys;
        RtlCopyMemory(New->Peer, Peer, 6);
        New->HavePmkid = B->Optional.PMKID_IsPresent;
        if (New->HavePmkid) RtlCopyMemory(New->Pmkid, B->PMKID.pElements, 16);
        New->BeaconLength = (USHORT)Body->ElementCount;
        New->VendorLength = (USHORT)(Global->ElementCount+Local->ElementCount);
        RtlCopyMemory(New->Bytes, Body->pElements, Body->ElementCount);
        if (Global->ElementCount) RtlCopyMemory(New->Bytes+New->BeaconLength, Global->pElements, Global->ElementCount);
        if (Local->ElementCount) RtlCopyMemory(New->Bytes+New->BeaconLength+Global->ElementCount,
            Local->pElements, Local->ElementCount);
        InsertTailList(&J->Pending, &New->Link);
    }
    return IsListEmpty(&J->Pending) ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
}

static VOID NextCandidate(SWIFI_JOIN *J)
{
    while (!IsListEmpty(&J->Pending)) {
        if (KeQueryInterruptTime() >= J->ConnectEnd) {
            DropPending(J);
            J->Deadline = 0;
            Finish(J, &J->Connect, STATUS_IO_TIMEOUT);
            return;
        }
        J->Active = CONTAINING_RECORD(RemoveHeadList(&J->Pending), JOIN_CANDIDATE, Link);
        JOIN_CANDIDATE *C = J->Active;
        SwifiDataBeginJoin(J->Owner, C->Peer, C->Profile.auth == SWIFI_AUTH_PSK_CCMP, C->ExcludeUnencrypted);
        NTSTATUS Status = Vendors(J, TRUE);
        if (NT_SUCCESS(Status)) Status = SwifiJoinFirmware(J->Owner, J->Info, &C->Profile,
            C->Peer, C->Channel, C->HavePmkid ? C->Pmkid : NULL);
        if (NT_SUCCESS(Status)) {
            ULONGLONG Now = KeQueryInterruptTime();
            ULONG RemainingMs = J->ConnectEnd > Now ? (ULONG)((J->ConnectEnd-Now)/10000) : 1;
            ULONG AttemptMs = IsListEmpty(&J->Pending) ? RemainingMs : min(RemainingMs, 3500u);
            Arm(J, max(AttemptMs,1u));
            return;
        }
        SwifiFilterStop(J->Owner, FALSE);
        SwifiDataLink(J->Owner, NULL, FALSE, FALSE, FALSE);
        if (!NT_SUCCESS(Vendors(J, FALSE)) || !NT_SUCCESS(AssocResult(J, WDI_ASSOC_STATUS_FAILURE, (ULONG)Status))) {
            Poison(J, STATUS_DEVICE_PROTOCOL_ERROR);
            return;
        }
        FreeActive(J);
        if (!J->Control->Running) { Poison(J, Status); return; }
    }
    J->Deadline = 0;
    Finish(J, &J->Connect, STATUS_NETWORK_UNREACHABLE);
}

static NTSTATUS LinkIsDown(SWIFI_JOIN *J, BOOLEAN *Confirmed)
{
    UCHAR Current[6] = {0};
    SHORT Firmware = 0;
    USHORT Returned = 0;
    NTSTATUS Status = SwifiFirmwareCommand(J->Control, 0, 23, Current, sizeof(Current), &Returned, &Firmware);
    *Confirmed = FALSE;
    if (Firmware == -17) { *Confirmed = TRUE; return STATUS_SUCCESS; } /* BCME_NOTASSOCIATED */
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned != 6) return STATUS_DEVICE_PROTOCOL_ERROR;
    *Confirmed = !memcmp(Current,"\0\0\0\0\0\0",6);
    if (!*Confirmed && !RtlEqualMemory(Current,J->Active->Peer,6)) return STATUS_DEVICE_PROTOCOL_ERROR;
    return STATUS_SUCCESS;
}

static VOID Down(SWIFI_JOIN *J, WDI_ASSOC_STATUS Reason)
{
    NTSTATUS Status = STATUS_SUCCESS;
    SwifiFilterStop(J->Owner, FALSE);
    SwifiDataLink(J->Owner, NULL, FALSE, FALSE, FALSE);
    if (J->Active) {
        Status = SwifiKeysClear(&J->Keys, J->Owner, J->Active->Peer);
        if (NT_SUCCESS(Status)) Status = Vendors(J, FALSE);
        if (!NT_SUCCESS(Status)) { Poison(J, Status); return; }
    }
    if (J->Announced && J->Port == J->PeerPort) {
        Status = Disassociated(J, J->Active->Peer, Reason);
        MediaState(J, FALSE);
    }
    J->Associated = J->Announced = J->Leaving = FALSE;
    FreeActive(J);
    J->Deadline = 0;
    if (J->RetryNext && J->Connect.Memory && !J->Leave.Memory && NT_SUCCESS(Status)) {
        J->RetryNext = FALSE;
        NextCandidate(J);
    } else {
        J->RetryNext = FALSE;
        DropPending(J);
    }
    Finish(J, &J->Leave, Status);
    KeSetEvent(&J->Left, IO_NO_INCREMENT, FALSE);
    if (!NT_SUCCESS(Status)) Poison(J, Status);
}

/* A BCM event has no host transaction tag. After abort/timeout keep this
 * attempt fenced until firmware confirms the disconnect. A timer expiring
 * while fenced requires endpoint recovery before another join may start. */
static NTSTATUS BeginLeave(SWIFI_JOIN *J, USHORT Reason, NTSTATUS ConnectStatus, BOOLEAN TryNext)
{
    if (!J->Active) return STATUS_SUCCESS;
    J->RetryNext = TryNext && J->Connect.Memory && !J->Leave.Memory && !IsListEmpty(&J->Pending) &&
        KeQueryInterruptTime() < J->ConnectEnd;
    if (J->Connect.Memory) {
        if (!J->Active->Reported) {
            NTSTATUS Report = AssocResult(J, WDI_ASSOC_STATUS_FAILURE, (ULONG)ConnectStatus);
            if (!NT_SUCCESS(Report)) { Poison(J, Report); return Report; }
        }
        if (!J->RetryNext) Finish(J, &J->Connect, ConnectStatus);
    }
    if (!J->RetryNext) DropPending(J);
    SwifiDataLink(J->Owner, NULL, FALSE, FALSE, FALSE);
    if (!J->Leaving) {
        KeClearEvent(&J->Left);
        NTSTATUS Status = SwifiLeaveFirmware(J->Control, J->Active->Peer, Reason);
        if (!NT_SUCCESS(Status)) { Poison(J, Status); return Status; }
        J->Leaving = TRUE;
        Arm(J, 2000);
    }
    return STATUS_SUCCESS;
}

static VOID JoinTimeout(WDFTIMER Timer)
{
    SWIFI_JOIN *J = JoinTimerContext(Timer)->Join;
    WdfWaitLockAcquire(J->Lock, NULL);
    if (J->Online && J->Deadline && KeQueryInterruptTime() >= J->Deadline) {
        if (J->Leaving) Poison(J, STATUS_IO_TIMEOUT);
        else (VOID)BeginLeave(J, 3, STATUS_IO_TIMEOUT, TRUE);
    }
    WdfWaitLockRelease(J->Lock);
}

NTSTATUS SwifiJoinCreate(WDFDEVICE Device, SWIFI_WIN_CONTROL *Control, SWIFI_FIRMWARE_INFO *Info,
    ULONG Version, SWIFI_SCAN *Scan, SWIFI_JOIN **Join)
{
    WDF_OBJECT_ATTRIBUTES A;
    WDF_TIMER_CONFIG Config;
    WDFMEMORY Memory;
    SWIFI_JOIN *J;
    WDF_OBJECT_ATTRIBUTES_INIT(&A);
    A.ParentObject = Device;
    NTSTATUS Status = WdfMemoryCreate(&A, NonPagedPoolNx, 'jFwS', sizeof(*J), &Memory, (PVOID *)&J);
    if (!NT_SUCCESS(Status)) return Status;
    RtlZeroMemory(J, sizeof(*J));
    InitializeListHead(&J->Pending);
    KeInitializeEvent(&J->Left, NotificationEvent, TRUE);
    J->Owner = SwifiDeviceContext(Device);
    J->Scan = Scan;
    J->Control = Control;
    J->Info = Info;
    J->Tlv.PeerVersion = Version;
    J->Port = WDI_PORT_ID_ADAPTER;
    Status = WdfWaitLockCreate(&A, &J->Lock);
    if (!NT_SUCCESS(Status)) return Status;
    WDF_TIMER_CONFIG_INIT(&Config, JoinTimeout);
    Config.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A, JOIN_TIMER_CONTEXT);
    A.ParentObject = Device;
    A.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfTimerCreate(&Config, &A, &J->Timer);
    if (!NT_SUCCESS(Status)) return Status;
    JoinTimerContext(J->Timer)->Join = J;
    WDF_WORKITEM_CONFIG LinkConfig;
    WDF_WORKITEM_CONFIG_INIT(&LinkConfig, ReadLink);
    LinkConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A, LINK_WORK);
    A.ParentObject = Device;
    Status = WdfWorkItemCreate(&LinkConfig, &A, &J->LinkWorker);
    if (!NT_SUCCESS(Status)) return Status;
    LinkWorkContext(J->LinkWorker)->Join = J;
    J->Owner->Data.LinkWorker = J->LinkWorker;
    *Join = J;
    return STATUS_SUCCESS;
}

VOID SwifiJoinStartSession(SWIFI_JOIN *J)
{
    WdfWaitLockAcquire(J->Lock, NULL);
    J->Online = TRUE;
    J->Poisoned = FALSE;
    WdfWaitLockRelease(J->Lock);
}

VOID SwifiJoinEndSession(SWIFI_JOIN *J)
{
    WdfWaitLockAcquire(J->Lock, NULL);
    J->Online = FALSE;
    SwifiFilterStop(J->Owner, FALSE);
    J->Deadline = 0;
    Finish(J, &J->Connect, STATUS_CANCELLED);
    Finish(J, &J->Leave, STATUS_CANCELLED);
    if (J->Announced && J->Port == J->PeerPort) {
        /* Runtime DMA is not yet isolated here. Issue the reset indication
         * only after the device owner confirms the hardware stop. */
        RtlCopyMemory(J->ResetPeer, J->Active->Peer, 6);
        J->ResetPort = J->PeerPort;
        J->ResetNotice = TRUE;
    }
    J->Associated = J->Announced = J->Leaving = J->RetryNext = FALSE;
    SwifiDataLink(J->Owner, NULL, FALSE, FALSE, FALSE);
    RtlSecureZeroMemory(&J->Keys, sizeof(J->Keys));
    DropPending(J);
    FreeActive(J);
    WdfWaitLockRelease(J->Lock);
    KeSetEvent(&J->Left, IO_NO_INCREMENT, FALSE);
    WdfTimerStop(J->Timer, TRUE);
    WdfWorkItemFlush(J->LinkWorker);
}

VOID SwifiJoinAfterReset(SWIFI_JOIN *J)
{
    WdfWaitLockAcquire(J->Lock, NULL);
    if (J->ResetNotice && J->Port == J->ResetPort) {
        (VOID)Disassociated(J, J->ResetPeer, WDI_ASSOC_STATUS_DISASSOCIATE_BY_DEVICE_RESET);
        MediaState(J, FALSE);
    }
    J->ResetNotice = FALSE;
    WdfWaitLockRelease(J->Lock);
}

VOID SwifiJoinStation(SWIFI_JOIN *J, WDI_PORT_ID Port, BOOLEAN Present)
{
    WdfWaitLockAcquire(J->Lock, NULL);
    if (Present) J->Port = Port;
    else if (J->Port == Port) {
        if (J->Online && J->Control->Running && J->Active && !J->Poisoned) (VOID)BeginLeave(J, 3, STATUS_CANCELLED, FALSE);
        SwifiFilterStop(J->Owner, TRUE);
        J->Announced = J->ResetNotice = FALSE;
        J->Port = WDI_PORT_ID_ADAPTER;
    }
    WdfWaitLockRelease(J->Lock);
}

NTSTATUS SwifiJoinSetBands(SWIFI_JOIN *J, ULONG Disallowed)
{
    WdfWaitLockAcquire(J->Lock, NULL);
    NTSTATUS Status = J->Active && (Disallowed &
        (J->Active->Channel <= 14 ? SWIFI_BAND_2GHZ : SWIFI_BAND_5GHZ)) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    if (NT_SUCCESS(Status)) J->DisallowedBands = Disallowed;
    WdfWaitLockRelease(J->Lock);
    return Status;
}

BOOLEAN SwifiJoinMayScan(SWIFI_JOIN *J)
{
    WdfWaitLockAcquire(J->Lock, NULL);
    BOOLEAN Allowed = J->Online && !J->Poisoned && !J->Leaving && !J->Connect.Memory;
    WdfWaitLockRelease(J->Lock);
    return Allowed;
}

NTSTATUS SwifiJoinRadioOff(SWIFI_JOIN *J)
{
    WdfWaitLockAcquire(J->Lock, NULL);
    NTSTATUS Status = J->Active ? BeginLeave(J, 3, STATUS_CANCELLED, FALSE) : STATUS_SUCCESS;
    BOOLEAN Wait = NT_SUCCESS(Status) && J->Leaving;
    WdfWaitLockRelease(J->Lock);
    if (Wait) {
        LARGE_INTEGER Timeout;
        Timeout.QuadPart = -25 * 1000000; /* 2.5 seconds; the disconnect timer owns recovery. */
        Status = KeWaitForSingleObject(&J->Left, Executive, KernelMode, FALSE, &Timeout);
        if (Status == STATUS_TIMEOUT) return STATUS_IO_TIMEOUT;
        WdfWaitLockAcquire(J->Lock, NULL);
        if (J->Poisoned || !J->Online) Status = STATUS_DEVICE_NOT_READY;
        WdfWaitLockRelease(J->Lock);
    }
    return Status;
}

static NTSTATUS Connect(SWIFI_JOIN *J, WDI_MESSAGE_HEADER *H, ULONG Bytes)
{
    if (J->Active || J->Connect.Memory || J->Leave.Memory || J->Info->RadioDisabled)
        return STATUS_DEVICE_BUSY;
    WDI_TASK_CONNECT_PARAMETERS P = {0};
    NTSTATUS Status = (NTSTATUS)ParseWdiTaskConnectToIhv(Bytes, (PUCHAR)(H+1), &J->Tlv, &P);
    if (NT_SUCCESS(Status)) Status = Candidates(J, &P);
    CleanupParsedWdiTaskConnectToIhv(&P);
    if (NT_SUCCESS(Status)) {
        Status = NewTask(J, &J->Connect, WDI_TASK_CONNECT, H);
        if (NT_SUCCESS(Status)) {
            J->PeerPort = H->PortId;
            J->ConnectEnd = KeQueryInterruptTime() + 10000ULL * 10000;
        }
    }
    if (!NT_SUCCESS(Status)) DropPending(J);
    return Status;
}

static VOID SetEapolRules(SWIFI_JOIN *J, const UCHAR Rules[2]);

static NTSTATUS LeaveTask(SWIFI_JOIN *J, UINT16 Id, WDI_MESSAGE_HEADER *H, ULONG Bytes, USHORT *Reason)
{
    NTSTATUS Status;
    BOOLEAN DefaultMib = FALSE;
    *Reason = 3;
    if (Id == WDI_TASK_DISCONNECT) {
        WDI_TASK_DISCONNECT_PARAMETERS P = {0};
        Status = (NTSTATUS)ParseWdiTaskDisconnectToIhv(Bytes, (PUCHAR)(H+1), &J->Tlv, &P);
        if (NT_SUCCESS(Status) && J->Active && !RtlEqualMemory(P.DisconnectParameters.MacAddress.Address,
            J->Active->Peer, 6)) Status = STATUS_INVALID_PARAMETER;
        *Reason = P.DisconnectParameters.Disassociation80211Reason;
        CleanupParsedWdiTaskDisconnectToIhv(&P);
    } else {
        WDI_TASK_DOT11_RESET_PARAMETERS P = {0};
        Status = (NTSTATUS)ParseWdiTaskDot11ResetToIhv(Bytes, (PUCHAR)(H+1), &J->Tlv, &P);
        if (NT_SUCCESS(Status) && P.Optional.ResetMACAddress_IsPresent &&
            !RtlEqualMemory(P.ResetMACAddress.Address, J->Info->Address, 6)) Status = STATUS_NOT_SUPPORTED;
        if (NT_SUCCESS(Status) && P.Optional.ResetMloLinkMACAddresses_IsPresent) Status = STATUS_NOT_SUPPORTED;
        if (NT_SUCCESS(Status)) DefaultMib = P.Dot11ResetParameters.SetDefaultMIB;
        CleanupParsedWdiTaskDot11ResetToIhv(&P);
    }
    if (NT_SUCCESS(Status)) Status = NewTask(J, &J->Leave, Id, H);
    if (NT_SUCCESS(Status) && Id == WDI_TASK_DOT11_RESET) {
        Status = SwifiFilterResetList(J->Owner);
        if (!NT_SUCCESS(Status)) {
            WdfObjectDelete(J->Leave.Memory);
            RtlZeroMemory(&J->Leave, sizeof(J->Leave));
        } else if (DefaultMib) {
            static const UCHAR Default[2] = {0};
            SetEapolRules(J, Default);
        }
    }
    return Status;
}

static NTSTATUS ChangeKeys(SWIFI_JOIN *J, UINT16 Id, WDI_MESSAGE_HEADER *H, ULONG Bytes)
{
    if (!J->Associated || J->Leaving || J->Active->Profile.auth != SWIFI_AUTH_PSK_CCMP)
        return STATUS_INVALID_DEVICE_STATE;
    BOOLEAN Uncertain;
    BOOLEAN OldPair = J->Keys.Slot[4].Present;
    SwifiDataKeyUpdate(J->Owner, TRUE);
    NTSTATUS Status = SwifiKeysChange(&J->Keys, J->Owner, J->Active->Peer, &J->Tlv,
        H, Bytes, Id == WDI_SET_ADD_CIPHER_KEYS, &Uncertain);
    if (NT_SUCCESS(Status) && OldPair != J->Keys.Slot[4].Present) {
        UCHAR Address[6];
        RtlCopyMemory(Address, J->Active->Peer, 6);
        Status = SwifiFirmwareCommand(J->Control, 0, J->Keys.Slot[4].Present ? 121 : 122,
            Address, 6, NULL, NULL); /* SCB authorize/deauthorize the same peer. */
        if (!NT_SUCCESS(Status)) Uncertain = TRUE;
    }
    if (NT_SUCCESS(Status)) {
        SwifiDataLink(J->Owner, J->Active->Peer, TRUE, J->Keys.Slot[4].Present, SwifiKeysHaveGroup(&J->Keys));
    } else if (Uncertain) {
        /* A failed batch may have changed dongle key state. Reset before accepting another peer. */
        Poison(J, Status);
    }
    SwifiDataKeyUpdate(J->Owner, FALSE);
    return Status;
}

static VOID SetEapolRules(SWIFI_JOIN *J, const UCHAR Rules[2])
{
    KIRQL Irql;
    KeAcquireSpinLock(&J->Owner->Hw.ProtocolLock, &Irql);
    RtlCopyMemory(J->Owner->Data.EapolExemption, Rules, 2);
    KeReleaseSpinLock(&J->Owner->Hw.ProtocolLock, Irql);
}

static NTSTATUS Privacy(SWIFI_JOIN *J, WDI_MESSAGE_HEADER *H, ULONG Bytes)
{
    WDI_SET_PRIVACY_EXEMPTION_LIST_PARAMETERS P = {0};
    UCHAR Rules[2] = {0}, Seen = 0;
    NTSTATUS Status = (NTSTATUS)ParseWdiSetPrivacyExemptionListToIhv(Bytes, (PUCHAR)(H+1), &J->Tlv, &P);
    if (NT_SUCCESS(Status) && P.PrivacyExemptionEntry.ElementCount > 2) Status = STATUS_NOT_SUPPORTED;
    for (ULONG I = 0; NT_SUCCESS(Status) && I < P.PrivacyExemptionEntry.ElementCount; ++I) {
        WDI_PRIVACY_EXEMPTION_LIST_CONTAINER *E = &P.PrivacyExemptionEntry.pElements[I];
        if (RtlUshortByteSwap(E->EtherType) != 0x888e ||
            (E->ExemptionActionType != WDI_EXEMPT_ALWAYS && E->ExemptionActionType != WDI_EXEMPT_ON_KEY_MAPPING_KEY_UNAVAILABLE) ||
            E->ExemptionPacketType < WDI_EXEMPT_PACKET_TYPE_UNICAST || E->ExemptionPacketType > WDI_EXEMPT_PACKET_TYPE_BOTH) {
            Status = STATUS_NOT_SUPPORTED;
            break;
        }
        UCHAR Types = (UCHAR)E->ExemptionPacketType;
        if (Types & Seen) { Status = STATUS_INVALID_PARAMETER; break; }
        Seen |= Types;
        if (Types & 1) Rules[0] = (UCHAR)E->ExemptionActionType;
        if (Types & 2) Rules[1] = (UCHAR)E->ExemptionActionType;
    }
    if (NT_SUCCESS(Status)) SetEapolRules(J, Rules);
    CleanupParsedWdiSetPrivacyExemptionListToIhv(&P);
    return Status;
}

static BOOLEAN AbortConnect(SWIFI_JOIN *J, WIFIREQUEST Request, WDI_MESSAGE_HEADER *H, ULONG Bytes)
{
    WDI_TASK_ABORT_PARAMETERS P = {0};
    NTSTATUS Status = (NTSTATUS)ParseWdiAbortTaskToIhv(Bytes, (PUCHAR)(H+1), &J->Tlv, &P);
    BOOLEAN Match = NT_SUCCESS(Status) && P.CancelParameters.OriginalTaskOID == OID_WDI_TASK_CONNECT;
    if (Match) {
        WdfWaitLockAcquire(J->Lock, NULL);
        if (!J->Connect.Header || P.CancelParameters.OriginalPortId != J->Connect.Header->PortId ||
            P.CancelParameters.OriginalTransactionId != J->Connect.Header->TransactionId) Status = STATUS_NOT_FOUND;
        else Status = BeginLeave(J, 3, STATUS_CANCELLED, FALSE);
        WdfWaitLockRelease(J->Lock);
        SwifiCompleteWdiRequest(Request, Status);
    }
    CleanupParsedWdiAbortTaskToIhv(&P);
    return Match;
}

BOOLEAN SwifiJoinCommand(SWIFI_JOIN *J, WIFIREQUEST Request, UINT16 Id, WDI_MESSAGE_HEADER *H, ULONG Bytes)
{
    if (Id == WDI_ABORT_TASK) return AbortConnect(J, Request, H, Bytes);
    BOOLEAN ConnectTask = Id == WDI_TASK_CONNECT;
    BOOLEAN Leave = Id == WDI_TASK_DISCONNECT || Id == WDI_TASK_DOT11_RESET;
    BOOLEAN Key = Id == WDI_SET_ADD_CIPHER_KEYS || Id == WDI_SET_DELETE_CIPHER_KEYS;
    BOOLEAN Exempt = Id == WDI_SET_PRIVACY_EXEMPTION_LIST;
    BOOLEAN Filter = Id == WDI_SET_RECEIVE_PACKET_FILTER || Id == WDI_SET_MULTICAST_LIST;
    if (!ConnectTask && !Leave && !Key && !Exempt && !Filter) return FALSE;
    USHORT Reason = 3;
    NTSTATUS Status;
    WdfWaitLockAcquire(J->Lock, NULL);
    if (!J->Online || J->Poisoned || J->Port == WDI_PORT_ID_ADAPTER) Status = STATUS_DEVICE_NOT_READY;
    else if (J->Port != H->PortId || ((ConnectTask || Leave) && !H->TransactionId)) Status = STATUS_INVALID_PARAMETER;
    else if (ConnectTask) Status = Connect(J, H, Bytes);
    else if (Leave) Status = LeaveTask(J, Id, H, Bytes, &Reason);
    else if (Exempt) Status = Privacy(J, H, Bytes);
    else if (Filter) Status = SwifiFilterWdi(J->Owner, Id, &J->Tlv, H, Bytes);
    else Status = ChangeKeys(J, Id, H, Bytes);
    if (ConnectTask && !NT_SUCCESS(Status)) SwifiReportJoin(&J->Owner->Report, Status);
    /* All input was copied or consumed. Publish M3 before a firmware event
     * or immediate refusal can complete the M4. H is no longer owned here. */
    SwifiCompleteWdiRequest(Request, Status);
    if (NT_SUCCESS(Status)) {
        if (ConnectTask) NextCandidate(J);
        else if (Leave) {
            if (J->Active) (VOID)BeginLeave(J, Reason, STATUS_CANCELLED, FALSE);
            else Finish(J, &J->Leave, STATUS_SUCCESS);
        }
    }
    WdfWaitLockRelease(J->Lock);
    return TRUE;
}

BOOLEAN SwifiJoinEvent(SWIFI_JOIN *J, const swifi_event_completion *E)
{
    const UCHAR *P = E->data;
    if (E->firmware_status || E->ifidx || !P || E->length < 72 || P[12] != 0x88 || P[13] != 0x6c ||
        P[19] || P[20] != 0x10 || P[21] != 0x18 || P[22] || P[23] != 1 || P[70] ||
        SwifiScanBe32(P+44) > E->length - 72u) return FALSE;
    ULONG Type = SwifiScanBe32(P+28), Status = SwifiScanBe32(P+32);
    BOOLEAN DownEvent = Type == 5 || Type == 6 || Type == 11 || Type == 12 || (Type == 16 && !(P[27] & 1));
    if (Type != 0 && !DownEvent && Type != 19) return FALSE;
    WdfWaitLockAcquire(J->Lock, NULL);
    if (!J->Online || !J->Active || J->Poisoned) goto Done;
    BOOLEAN Same = RtlEqualMemory(P+48, J->Active->Peer, 6);
    BOOLEAN Unknown = !memcmp(P+48,"\0\0\0\0\0\0",6);
    if (!Same && !Unknown) {
        /* Independent roaming was disabled. A successful unsolicited roam
         * to another peer makes the current security context unusable. */
        if (Type == 19 && !Status && J->Associated) Poison(J, STATUS_DEVICE_PROTOCOL_ERROR);
        goto Done;
    }
    if (J->Leaving) {
        if (DownEvent) {
            BOOLEAN Confirmed;
            NTSTATUS Check = LinkIsDown(J, &Confirmed);
            if (NT_SUCCESS(Check) && Confirmed)
                Down(J, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
            else if (!NT_SUCCESS(Check)) Poison(J, Check);
        }
        goto Done;
    }
    if (J->Associated) {
        if (DownEvent) {
            BOOLEAN Confirmed = Same;
            NTSTATUS Check = Same ? STATUS_SUCCESS : LinkIsDown(J, &Confirmed);
            if (!NT_SUCCESS(Check)) Poison(J, Check);
            else if (Confirmed) Down(J, Type == 5 || Type == 6 ?
                WDI_ASSOC_STATUS_PEER_DEAUTHENTICATED : WDI_ASSOC_STATUS_PEER_DISASSOCIATED);
        }
        goto Done;
    }
    if (!J->Connect.Memory || Type != 0) goto Done;
    if (Status) {
        NTSTATUS Report = AssocResult(J, WDI_ASSOC_STATUS_FAILURE, Status);
        if (NT_SUCCESS(Report)) Report = Vendors(J, FALSE);
        SwifiFilterStop(J->Owner, FALSE);
        SwifiDataLink(J->Owner, NULL, FALSE, FALSE, FALSE);
        if (!NT_SUCCESS(Report)) { Poison(J, Report); goto Done; }
        FreeActive(J);
        NextCandidate(J);
        goto Done;
    }
    /* SET_SSID success plus readback of the selected BSSID and association
     * frames are required before any upper-layer success or media notification. */
    NTSTATUS Result = SwifiJoinReadFrames(J->Control, J->Active->Peer, &J->Active->Profile,
        J->Active->HavePmkid ? J->Active->Pmkid : NULL, &J->Frames);
    if (NT_SUCCESS(Result)) {
        J->Active->Phy = NegotiatedPhy(&J->Frames, J->Active->Channel);
        if (!(J->Active->AllowedPhys & (1u << J->Active->Phy))) Result = STATUS_NOT_SUPPORTED;
    }
    if (!NT_SUCCESS(Result)) { (VOID)BeginLeave(J, 3, Result, TRUE); goto Done; }
    J->Associated = TRUE;
    J->Deadline = 0;
    BOOLEAN Secure = J->Active->Profile.auth == SWIFI_AUTH_PSK_CCMP;
    Result = Vendors(J, FALSE);
    if (NT_SUCCESS(Result)) Result = AssocResult(J, WDI_ASSOC_STATUS_SUCCESS, 0);
    if (!NT_SUCCESS(Result)) { Poison(J, Result); goto Done; }
    J->Announced = TRUE;
    J->TxKbps = J->RxKbps = J->IndicatedTx = J->IndicatedRx = 0;
    J->IndicatedQuality = -100;
    SwifiDataLink(J->Owner, J->Active->Peer, Secure, FALSE, FALSE);
    MediaState(J, TRUE);
    WdfWorkItemEnqueue(J->LinkWorker);
    DropPending(J);
    Finish(J, &J->Connect, STATUS_SUCCESS);
Done:
    WdfWaitLockRelease(J->Lock);
    return TRUE;
}
