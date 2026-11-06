/* SPDX-License-Identifier: ISC */
#include "scan.h"
#include "scan_firmware.h"
#include <TlvGeneratorParser.hpp>

enum { Batch = 3, BatchMs = 500, AbortMs = 2000, DefaultScanMs = 30000 };
#define SCAN_MS(n) ((ULONGLONG)(n) * 10000)

typedef struct SCAN_ENTRY {
    LIST_ENTRY Link;
    WDI_BSS_ENTRY_CONTAINER Wdi;
    ULONG Size;
    UCHAR Body[1];
} SCAN_ENTRY;

struct SWIFI_SCAN {
    WDFDEVICE Device;
    SWIFI_WIN_CONTROL *Control;
    SWIFI_FIRMWARE_INFO *Info;
    SWIFI_SCAN_RECOVER Recover;
    PVOID Context;
    TLV_CONTEXT Tlv;
    WDFWAITLOCK Lock;
    WDFTIMER Timer;
    BOOLEAN Online, Poisoned, Draining, Live;
    WDI_PORT_ID Port;
    USHORT Sync;
    ULONG Repeat;
    NTSTATUS DrainStatus;
    ULONGLONG Deadline, MaximumEnd, BatchEnd;
    WDFMEMORY DoneMemory;
    WDI_MESSAGE_HEADER *Done;
    SWIFI_SCAN_SPEC Spec;
    SWIFI_SCAN_POLICY Policy;
    LIST_ENTRY Entries;
    ULONG Count, Bytes;
};

typedef struct SCAN_TIMER_CONTEXT { SWIFI_SCAN *Scan; } SCAN_TIMER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SCAN_TIMER_CONTEXT, ScanTimerContext);
static EVT_WDF_TIMER ScanTimer;

NTSTATUS SwifiScanCreate(WDFDEVICE Device, SWIFI_WIN_CONTROL *Control, SWIFI_FIRMWARE_INFO *Info,
    ULONG PeerVersion, SWIFI_SCAN_RECOVER Recover, PVOID Context, SWIFI_SCAN **Scan)
{
    WDF_OBJECT_ATTRIBUTES A;
    WDF_TIMER_CONFIG T;
    WDFMEMORY Memory;
    SWIFI_SCAN *S;
    WDF_OBJECT_ATTRIBUTES_INIT(&A);
    A.ParentObject = Device;
    NTSTATUS Status = WdfMemoryCreate(&A, NonPagedPoolNx, 'sFwS', sizeof(*S), &Memory, (PVOID *)&S);
    if (!NT_SUCCESS(Status)) return Status;
    RtlZeroMemory(S, sizeof(*S));
    S->Device = Device;
    S->Control = Control;
    S->Info = Info;
    S->Recover = Recover;
    S->Context = Context;
    S->Tlv.PeerVersion = PeerVersion;
    S->Port = WDI_PORT_ID_ADAPTER;
    InitializeListHead(&S->Entries);
    Status = WdfWaitLockCreate(&A, &S->Lock);
    if (!NT_SUCCESS(Status)) return Status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A, SCAN_TIMER_CONTEXT);
    A.ParentObject = Device;
    A.ExecutionLevel = WdfExecutionLevelPassive;
    WDF_TIMER_CONFIG_INIT(&T, ScanTimer);
    T.AutomaticSerialization = FALSE;
    Status = WdfTimerCreate(&T, &A, &S->Timer);
    if (!NT_SUCCESS(Status)) return Status;
    ScanTimerContext(S->Timer)->Scan = S;
    *Scan = S;
    return STATUS_SUCCESS;
}

static void ArmTimer(SWIFI_SCAN *S)
{
    if (!S->DoneMemory || !S->Online) return;
    ULONGLONG Next = S->Deadline, Now = KeQueryInterruptTime();
    if (S->BatchEnd && !S->Draining && S->BatchEnd < Next) Next = S->BatchEnd;
    WdfTimerStart(S->Timer, Next <= Now ? -1 : -(LONGLONG)(Next - Now));
}

static void Discard(SWIFI_SCAN *S)
{
    while (!IsListEmpty(&S->Entries)) {
        PLIST_ENTRY Link = RemoveHeadList(&S->Entries);
        ExFreePoolWithTag(CONTAINING_RECORD(Link, SCAN_ENTRY, Link), 'bFwS');
    }
    S->Count = S->Bytes = 0;
    S->BatchEnd = 0;
}

static NTSTATUS IndicateBatch(SWIFI_SCAN *S)
{
    WDI_BSS_ENTRY_CONTAINER Entries[Batch];
    WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS P = {0};
    WDI_MESSAGE_HEADER *Message;
    WDFMEMORY Memory;
    ULONG Length = 0, Count = 0;
    UINT8 *Generated = NULL;
    for (PLIST_ENTRY Link = S->Entries.Flink; Link != &S->Entries && Count < Batch; Link = Link->Flink)
        Entries[Count++] = CONTAINING_RECORD(Link, SCAN_ENTRY, Link)->Wdi;
    if (!Count) return STATUS_SUCCESS;
    P.Optional.DeviceDescriptor_IsPresent = TRUE;
    P.DeviceDescriptor.ElementCount = Count;
    P.DeviceDescriptor.pElements = Entries;
    NTSTATUS Status = (NTSTATUS)GenerateWdiIndicationBssEntryListFromIhv(&P, sizeof(*Message),
        &S->Tlv, &Length, &Generated);
    if (NT_SUCCESS(Status)) {
        Status = Generated && Length >= sizeof(*Message) ?
            SwifiAllocWdiMessage(S->Device, Length, &Memory, &Message) : STATUS_DEVICE_PROTOCOL_ERROR;
        if (NT_SUCCESS(Status)) {
            RtlCopyMemory(Message, Generated, Length);
            RtlZeroMemory(Message, sizeof(*Message));
            Message->PortId = S->Done->PortId;
            Message->TransactionId = WDI_TRANSACTION_ID_UNSOLICIT;
            Message->Status = NDIS_STATUS_SUCCESS;
            WifiDeviceReceiveIndication(S->Device, WDI_INDICATION_BSS_ENTRY_LIST, Memory);
            WdfObjectDelete(Memory);
            for (ULONG I = 0; I < Count; ++I) {
                PLIST_ENTRY Link = RemoveHeadList(&S->Entries);
                SCAN_ENTRY *Old = CONTAINING_RECORD(Link, SCAN_ENTRY, Link);
                S->Bytes -= Old->Size;
                ExFreePoolWithTag(Old, 'bFwS');
            }
            S->Count -= Count;
            S->BatchEnd = S->Count && S->Live ? KeQueryInterruptTime() + SCAN_MS(BatchMs) : 0;
        }
    }
    if (Generated) FreeGenerated(Generated);
    return Status;
}

static NTSTATUS KeepBss(SWIFI_SCAN *S, const SWIFI_SCAN_BSS *B)
{
    ULONG I, Offset;
    BOOLEAN HasSsid = FALSE;
    if (!B->Channel || B->Channel > SwifiScanChannels || (B->Bssid[0] & 1)) return STATUS_SUCCESS;
    if (!(S->Spec.Bssid[0] & 1) && !RtlEqualMemory(S->Spec.Bssid, B->Bssid, 6)) return STATUS_SUCCESS;
    for (I = 0; I < S->Spec.SsidCount; ++I) {
        const SWIFI_SCAN_SSID *Wanted = &S->Spec.Ssids[I];
        if (!Wanted->Length || (Wanted->Length == B->SsidLength &&
            RtlEqualMemory(Wanted->Bytes, B->Ssid, Wanted->Length))) break;
    }
    if (S->Spec.SsidCount && I == S->Spec.SsidCount) return STATUS_SUCCESS;
    for (I = 0; I < S->Spec.ChannelCount && S->Spec.Channels[I] != B->Channel; ++I) {}
    if (I == S->Spec.ChannelCount) return STATUS_SUCCESS;
    for (Offset = 0; Offset < B->IeLength;) {
        if (B->IeLength - Offset < 2 || 2u + B->Ies[Offset + 1] > B->IeLength - Offset)
            return STATUS_SUCCESS; /* Skip a malformed over-the-air record. */
        if (B->Ies[Offset] == 0) HasSsid = TRUE;
        Offset += 2 + B->Ies[Offset + 1];
    }
    ULONG Extra = HasSsid ? 0 : 2 + B->SsidLength;
    ULONG Body = 12 + Extra + B->IeLength;
    ULONG Size = FIELD_OFFSET(SCAN_ENTRY, Body) + Body;
    if (S->Bytes + Size > 1024 * 1024) return STATUS_INSUFFICIENT_RESOURCES;
    SCAN_ENTRY *Entry = ExAllocatePool2(POOL_FLAG_PAGED, Size, 'bFwS');
    if (!Entry) return STATUS_INSUFFICIENT_RESOURCES;
    Entry->Size = Size;
    RtlCopyMemory(Entry->Wdi.BSSID.Address, B->Bssid, 6);
    Entry->Wdi.ChannelInfo.ChannelNumber = B->Channel;
    Entry->Wdi.ChannelInfo.BandId = B->Channel <= 14 ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
    Entry->Wdi.SignalInfo.RSSI = B->Rssi;
    Entry->Wdi.SignalInfo.LinkQuality = B->Rssi <= -100 ? 0 : B->Rssi >= -50 ? 100 : 2 * (B->Rssi + 100);
    /* WDI wants the management frame body, without the MAC header. BSS
     * v109 reports interval/capability/IEs but no received TSF. Leave that
     * field zero, rather than substituting the host clock for the AP's TSF. */
    SwifiScanPut16(Entry->Body + 8, B->Interval);
    SwifiScanPut16(Entry->Body + 10, B->Capability);
    if (Extra) {
        Entry->Body[12] = 0;
        Entry->Body[13] = (UCHAR)B->SsidLength;
        RtlCopyMemory(Entry->Body + 14, B->Ssid, B->SsidLength);
    }
    if (B->IeLength) RtlCopyMemory(Entry->Body + 12 + Extra, B->Ies, B->IeLength);
    WDI_BYTE_BLOB *Frame;
    if (B->Flags & 1) {
        Entry->Wdi.Optional.BeaconFrame_IsPresent = TRUE;
        Frame = &Entry->Wdi.BeaconFrame;
    } else {
        Entry->Wdi.Optional.ProbeResponseFrame_IsPresent = TRUE;
        Frame = &Entry->Wdi.ProbeResponseFrame;
    }
    Frame->ElementCount = Body;
    Frame->pElements = Entry->Body;
    for (PLIST_ENTRY Link = S->Entries.Flink; Link != &S->Entries; Link = Link->Flink) {
        SCAN_ENTRY *Old = CONTAINING_RECORD(Link, SCAN_ENTRY, Link);
        if (Old->Wdi.ChannelInfo.ChannelNumber == B->Channel &&
            RtlEqualMemory(Old->Wdi.BSSID.Address, B->Bssid, 6)) {
            RemoveEntryList(Link);
            S->Bytes -= Old->Size;
            --S->Count;
            ExFreePoolWithTag(Old, 'bFwS');
            break;
        }
    }
    InsertTailList(&S->Entries, &Entry->Link);
    S->Bytes += Size;
    ++S->Count;
    if (S->Live && S->Count >= Batch) return IndicateBatch(S);
    if (S->Live && !S->BatchEnd) S->BatchEnd = KeQueryInterruptTime() + SCAN_MS(BatchMs);
    return STATUS_SUCCESS;
}

/* All mutable task state, including delivery of the final M4, belongs to
 * the passive wait lock. Firmware IOCTL completion runs independently in DPC. */
static VOID Poison(SWIFI_SCAN *S, NTSTATUS Status)
{
    if (!S->Poisoned) {
        S->Poisoned = TRUE;
        if (S->Online && S->Control->Running) S->Recover(S->Context, Status);
    }
}

static VOID Finish(SWIFI_SCAN *S, NTSTATUS Status, BOOLEAN Restore)
{
    WdfTimerStop(S->Timer, FALSE);
    if (Restore && S->Control->Running) {
        NTSTATUS R = SwifiScanRestore(S->Control, &S->Policy);
        if (!NT_SUCCESS(R)) { Poison(S, R); Status = R; }
    }
    while (NT_SUCCESS(Status) && S->Count) Status = IndicateBatch(S);
    Discard(S);
    S->Draining = FALSE;
    if (S->DoneMemory) {
        S->Done->Status = (NDIS_STATUS)Status;
        WifiDeviceReceiveIndication(S->Device, WDI_TASK_SCAN, S->DoneMemory);
        WdfObjectDelete(S->DoneMemory);
        S->DoneMemory = NULL;
        S->Done = NULL;
    }
}

static NTSTATUS AbortLocked(SWIFI_SCAN *S, NTSTATUS Reason)
{
    if (!S->DoneMemory || S->Draining) return STATUS_SUCCESS;
    NTSTATUS Status = SwifiScanAbortFirmware(S->Control);
    if (!NT_SUCCESS(Status)) {
        Poison(S, Status);
        Finish(S, Status, FALSE);
        return Status;
    }
    S->Draining = TRUE;
    S->DrainStatus = Reason;
    S->BatchEnd = 0;
    S->Deadline = KeQueryInterruptTime() + SCAN_MS(AbortMs);
    ArmTimer(S);
    return STATUS_SUCCESS;
}

static NTSTATUS NextScan(SWIFI_SCAN *S)
{
    ULONGLONG Now = KeQueryInterruptTime();
    S->Deadline = Now + SCAN_MS(DefaultScanMs);
    if (S->MaximumEnd) {
        if (S->MaximumEnd <= Now) return STATUS_IO_TIMEOUT;
        if (S->MaximumEnd < S->Deadline) S->Deadline = S->MaximumEnd;
    }
    if (!++S->Sync) ++S->Sync;
    return SwifiScanIssue(S->Control, S->Info, &S->Spec, S->Sync);
}

static NTSTATUS AddChannel(SWIFI_SCAN *S, ULONG Channel, WDI_BAND_ID Band)
{
    if (!Channel || Channel > SwifiScanChannels ||
        (Channel <= 14 ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000) != Band)
        return STATUS_INVALID_PARAMETER;
    for (ULONG J = 0; J < S->Spec.ChannelCount; ++J)
        if (S->Spec.Channels[J] == Channel) return STATUS_SUCCESS;
    if (S->Spec.ChannelCount >= SwifiScanChannels) return STATUS_INVALID_PARAMETER;
    S->Spec.Channels[S->Spec.ChannelCount++] = (UCHAR)Channel;
    return STATUS_SUCCESS;
}

static NTSTATUS ParseSpec(SWIFI_SCAN *S, const WDI_SCAN_PARAMETERS *P)
{
    RtlZeroMemory(&S->Spec, sizeof(S->Spec));
    if (P->SSIDList.ElementCount > SwifiScanSsids) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(S->Spec.Bssid, P->BSSID.Address, 6);
    static const UCHAR Zero[6] = {0};
    static const UCHAR Broadcast[6] = {255,255,255,255,255,255};
    if (RtlEqualMemory(S->Spec.Bssid, Zero, 6)) RtlCopyMemory(S->Spec.Bssid, Broadcast, 6);
    if ((S->Spec.Bssid[0] & 1) && !RtlEqualMemory(S->Spec.Bssid, Broadcast, 6))
        return STATUS_INVALID_PARAMETER;
    S->Spec.SsidCount = P->SSIDList.ElementCount;
    for (ULONG I = 0; I < S->Spec.SsidCount; ++I) {
        const WDI_SSID *Ssid = P->SSIDList.pElements + I;
        if (Ssid->ElementCount > 32) return STATUS_INVALID_PARAMETER;
        S->Spec.Ssids[I].Length = (UCHAR)Ssid->ElementCount;
        if (Ssid->ElementCount) RtlCopyMemory(S->Spec.Ssids[I].Bytes, Ssid->pElements, Ssid->ElementCount);
    }
    switch (P->ScanModeParameters.AllowedScanType) {
    case WDI_SCAN_TYPE_PASSIVE_ONLY: S->Spec.Passive = TRUE; break;
    case WDI_SCAN_TYPE_ACTIVE_ONLY: break;
    case WDI_SCAN_TYPE_AUTO:
        S->Spec.Passive = !S->Spec.SsidCount && RtlEqualMemory(S->Spec.Bssid, Broadcast, 6);
        break;
    default: return STATUS_INVALID_PARAMETER;
    }
    if (!S->Spec.Passive && !S->Spec.SsidCount) S->Spec.SsidCount = 1; /* Broadcast probe. */
    if (P->Optional.BandChannelList_IsPresent) {
        for (ULONG I = 0; I < P->BandChannelList.ElementCount; ++I) {
            const WDI_SCAN_BAND_CHANNEL_LIST_CONTAINER *B = &P->BandChannelList.pElements[I];
            if (B->BandID != WDI_BAND_ID_2400 && B->BandID != WDI_BAND_ID_5000) continue;
            if (!B->ChannelList.ElementCount) {
                for (ULONG J = 0; J < S->Info->ChannelCount; ++J) {
                    UCHAR Ch = S->Info->Channels[J];
                    if ((Ch <= 14) != (B->BandID == WDI_BAND_ID_2400)) continue;
                    NTSTATUS Status = AddChannel(S, Ch, B->BandID);
                    if (!NT_SUCCESS(Status)) return Status;
                }
            } else {
                for (ULONG J = 0; J < B->ChannelList.ElementCount; ++J) {
                    NTSTATUS Status = AddChannel(S, B->ChannelList.pElements[J], B->BandID);
                    if (!NT_SUCCESS(Status)) return Status;
                }
            }
        }
        if (P->BandChannelList.ElementCount && !S->Spec.ChannelCount) return STATUS_NOT_SUPPORTED;
    }
    if (P->Optional.SixGHzBandChannelList_IsPresent && P->SixGHzBandChannelList.ElementCount &&
        !S->Spec.ChannelCount) return STATUS_NOT_SUPPORTED;
    S->Spec.ActiveMs = P->DwellTime.ActiveChannelDwellTime;
    S->Spec.PassiveMs = P->DwellTime.PassiveChannelDwellTime;
    S->MaximumEnd = P->DwellTime.MaximumScanTime ?
        KeQueryInterruptTime() + SCAN_MS(P->DwellTime.MaximumScanTime) : 0;
    S->Repeat = P->ScanModeParameters.ScanRepeatCount;
    S->Live = P->ScanModeParameters.LiveUpdatesNeeded;
    return STATUS_SUCCESS;
}

static NTSTATUS Begin(SWIFI_SCAN *S, const WDI_MESSAGE_HEADER *Header, UINT Input)
{
    WDI_SCAN_PARAMETERS P = {0};
    WDI_MESSAGE_HEADER *Done = NULL;
    WDFMEMORY Completion = NULL;
    if (!S->Online || S->Poisoned || S->Port == WDI_PORT_ID_ADAPTER) return STATUS_DEVICE_NOT_READY;
    if (Header->PortId != S->Port || !Header->TransactionId) return STATUS_INVALID_PARAMETER;
    if (S->DoneMemory) return STATUS_DEVICE_BUSY;
    NTSTATUS Status = (NTSTATUS)ParseWdiTaskScanToIhv(Input - sizeof(*Header),
        (const UINT8 *)(Header + 1), &S->Tlv, &P);
    if (NT_SUCCESS(Status)) Status = ParseSpec(S, &P);
    if (NT_SUCCESS(Status)) Status = SwifiAllocWdiMessage(S->Device, sizeof(*Done), &Completion, &Done);
    if (NT_SUCCESS(Status)) Status = SwifiScanPrepare(S->Control, S->Info, &S->Spec,
        P.ScanModeParameters.AllowedScanType == WDI_SCAN_TYPE_ACTIVE_ONLY,
        P.Optional.VendorIEs_IsPresent && !S->Spec.Passive ? P.VendorIEs.pElements : NULL,
        P.Optional.VendorIEs_IsPresent && !S->Spec.Passive ? P.VendorIEs.ElementCount : 0,
        S->MaximumEnd, &S->Policy);
    if (NT_SUCCESS(Status)) Status = NextScan(S);
    CleanupParsedWdiTaskScanToIhv(&P);
    if (!NT_SUCCESS(Status)) {
        if (Completion) WdfObjectDelete(Completion);
        if (S->Control->Running) {
            NTSTATUS R = SwifiScanRestore(S->Control, &S->Policy);
            if (!NT_SUCCESS(R)) Poison(S, R);
        }
        return Status;
    }
    *Done = *Header;
    S->DoneMemory = Completion;
    S->Done = Done;
    return STATUS_SUCCESS;
}

VOID SwifiScanRequest(SWIFI_SCAN *S, WIFIREQUEST Request, BOOLEAN Abort)
{
    UINT Input, Output;
    WDI_MESSAGE_HEADER *H = WifiRequestGetInOutBuffer(Request, &Input, &Output);
    NTSTATUS Status;
    if (!H || Input < sizeof(*H) || Output < sizeof(*H)) {
        SwifiCompleteWdiRequest(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    WdfWaitLockAcquire(S->Lock, NULL);
    if (Abort) {
        WDI_TASK_ABORT_PARAMETERS P = {0};
        Status = (NTSTATUS)ParseWdiAbortTaskToIhv(Input - sizeof(*H), (UCHAR *)(H + 1), &S->Tlv, &P);
        if (NT_SUCCESS(Status)) {
            if (P.CancelParameters.OriginalTaskOID != OID_WDI_TASK_SCAN) Status = STATUS_NOT_SUPPORTED;
            else if (!S->Done || P.CancelParameters.OriginalPortId != S->Done->PortId ||
                P.CancelParameters.OriginalTransactionId != S->Done->TransactionId) Status = STATUS_NOT_FOUND;
            else Status = AbortLocked(S, STATUS_CANCELLED);
        }
        CleanupParsedWdiAbortTaskToIhv(&P);
    } else Status = Begin(S, H, Input);
    /* An event can race the IOCTL reply. Complete M3 before releasing the
     * lock so an early BSS/terminal can never overtake it. */
    SwifiCompleteWdiRequest(Request, Status);
    if (NT_SUCCESS(Status)) ArmTimer(S);
    WdfWaitLockRelease(S->Lock);
}

BOOLEAN SwifiScanFirmwareEvent(SWIFI_SCAN *S, const swifi_event_completion *E)
{
    if (E->ifidx || E->length < 72 || !E->data || SwifiScanBe32(E->data + 28) != 69)
        return FALSE;
    WdfWaitLockAcquire(S->Lock, NULL);
    if (S->Online && S->DoneMemory && S->Control->Running) {
        SWIFI_SCAN_BSS Bss;
        unsigned EventStatus;
        enum swifi_scan_event Kind = SwifiDecodeScan(E->data, E->length, S->Info->IoctlVersion,
            S->Sync, &Bss, &EventStatus);
        if (Kind == SwifiScanPartial && !S->Draining) {
            NTSTATUS Status = KeepBss(S, &Bss);
            if (!NT_SUCCESS(Status)) AbortLocked(S, Status);
        } else if (Kind == SwifiScanTerminal) {
            if (S->Draining) Finish(S, S->DrainStatus, TRUE);
            else if (EventStatus) Finish(S, STATUS_UNSUCCESSFUL, TRUE);
            else if (S->Repeat == 1) Finish(S, STATUS_SUCCESS, TRUE);
            else {
                if (S->Repeat) --S->Repeat;
                NTSTATUS Status = NextScan(S);
                if (!NT_SUCCESS(Status)) Finish(S, Status, TRUE);
            }
        }
        ArmTimer(S);
    }
    WdfWaitLockRelease(S->Lock);
    return TRUE;
}

static VOID ScanTimer(WDFTIMER Timer)
{
    SWIFI_SCAN *S = ScanTimerContext(Timer)->Scan;
    WdfWaitLockAcquire(S->Lock, NULL);
    if (S->Online && S->DoneMemory && S->Control->Running) {
        ULONGLONG Now = KeQueryInterruptTime();
        if (Now >= S->Deadline) {
            if (S->Draining) {
                /* Some terminal events carry no sync ID. Only reset can now
                 * separate a future scan from this timed-out firmware scan. */
                Poison(S, STATUS_IO_TIMEOUT);
                Finish(S, STATUS_IO_TIMEOUT, FALSE);
            } else AbortLocked(S, STATUS_IO_TIMEOUT);
        } else if (S->BatchEnd && Now >= S->BatchEnd && !S->Draining) {
            NTSTATUS Status = IndicateBatch(S);
            if (!NT_SUCCESS(Status)) AbortLocked(S, Status);
        }
        ArmTimer(S);
    }
    WdfWaitLockRelease(S->Lock);
}

NTSTATUS SwifiScanCancelForRadio(SWIFI_SCAN *S)
{
    WdfWaitLockAcquire(S->Lock, NULL);
    NTSTATUS Status = AbortLocked(S, STATUS_CANCELLED);
    WdfWaitLockRelease(S->Lock);
    return Status;
}

VOID SwifiScanStation(SWIFI_SCAN *S, WDI_PORT_ID Port, BOOLEAN Present)
{
    WdfWaitLockAcquire(S->Lock, NULL);
    if (Present) S->Port = Port;
    else if (S->Port == Port) {
        if (S->Online && S->Control->Running) AbortLocked(S, STATUS_CANCELLED);
        S->Port = WDI_PORT_ID_ADAPTER;
    }
    WdfWaitLockRelease(S->Lock);
}

VOID SwifiScanStartSession(SWIFI_SCAN *S)
{
    WdfWaitLockAcquire(S->Lock, NULL);
    RtlZeroMemory(&S->Policy, sizeof(S->Policy));
    S->Online = TRUE;
    S->Poisoned = FALSE;
    WdfWaitLockRelease(S->Lock);
}

VOID SwifiScanEndSession(SWIFI_SCAN *S)
{
    WdfWaitLockAcquire(S->Lock, NULL);
    S->Online = FALSE;
    Finish(S, STATUS_CANCELLED, FALSE);
    WdfWaitLockRelease(S->Lock);
    WdfTimerStop(S->Timer, TRUE);
}
