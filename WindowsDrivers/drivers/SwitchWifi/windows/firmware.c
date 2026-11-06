/* SPDX-License-Identifier: ISC */
#include "firmware.h"

#define WLC_GET_VERSION 1u
#define WLC_GET_RADIO 37u
#define WLC_GET_BANDLIST 140u
#define WLC_GET_VALID_CHANNELS 217u
#define WLC_GET_VAR 262u
#define WLC_SET_VAR 263u
#define BCME_UNSUPPORTED (-23)
#define BCME_NOTFOUND (-30)
#define WLC_BAND_5G 1u
#define WLC_BAND_2G 2u

NTSTATUS SwifiFirmwareCommand(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex,
    ULONG Command, PVOID Buffer, USHORT Length, PUSHORT Returned, PSHORT FirmwareStatus)
{
    SHORT Error = 0;
    NTSTATUS Status = SwifiControlExecute(Control, IfIndex, Command,
        Buffer, Length, Buffer, Length, Returned, &Error);
    if (FirmwareStatus) *FirmwareStatus = Error;
    if (Status == STATUS_UNSUCCESSFUL) {
        if (Error == BCME_UNSUPPORTED) return STATUS_NOT_SUPPORTED;
        if (Error == BCME_NOTFOUND) return STATUS_NOT_FOUND;
    }
    return Status;
}

static NTSTATUS Iovar(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex, PCSTR Name,
    const VOID *Input, PVOID Output, USHORT Length, PUSHORT Returned, PSHORT FirmwareStatus)
{
    SIZE_T NameLength = 0, Total;
    USHORT ReplyLength = 0, Copied;
    PUCHAR Buffer;
    NTSTATUS Status;
    if (Returned) *Returned = 0;
    if (FirmwareStatus) *FirmwareStatus = 0;
    if (!Name || (Length && !Input)) return STATUS_INVALID_PARAMETER;
    while (NameLength < SWIFI_CONTROL_BUFFER_SIZE && Name[NameLength]) ++NameLength;
    Total = NameLength + 1 + Length;
    if (!NameLength || Total > SWIFI_CONTROL_BUFFER_SIZE)
        return STATUS_INVALID_PARAMETER;
    Buffer = ExAllocatePool2(POOL_FLAG_PAGED, Total, 'iFwS');
    if (!Buffer) return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(Buffer, Name, NameLength + 1);
    if (Length) RtlCopyMemory(Buffer + NameLength + 1, Input, Length);
    Status = SwifiFirmwareCommand(Control, IfIndex, Output ? WLC_GET_VAR : WLC_SET_VAR,
        Buffer, (USHORT)Total, &ReplyLength, FirmwareStatus);
    if (NT_SUCCESS(Status) && Output) {
        Copied = ReplyLength < Length ? ReplyLength : Length;
        if (Copied) RtlCopyMemory(Output, Buffer, Copied);
        if (Returned) *Returned = Copied;
    }
    ExFreePoolWithTag(Buffer, 'iFwS');
    return Status;
}

NTSTATUS SwifiFirmwareIovarGet(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex,
    PCSTR Name, PVOID Data, USHORT Length, PUSHORT Returned, PSHORT FirmwareStatus)
{
    if (!Data || !Length) return STATUS_INVALID_PARAMETER;
    return Iovar(Control, IfIndex, Name, Data, Data, Length, Returned, FirmwareStatus);
}

NTSTATUS SwifiFirmwareIovarSet(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex,
    PCSTR Name, const VOID *Data, USHORT Length, PSHORT FirmwareStatus)
{
    return Iovar(Control, IfIndex, Name, Data, NULL, Length, NULL, FirmwareStatus);
}

NTSTATUS SwifiFirmwareGetU32(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex,
    PCSTR Name, PULONG Value)
{
    USHORT Returned;
    ULONG Reply;
    NTSTATUS Status;
    if (!Value) return STATUS_INVALID_PARAMETER;
    /* Windows ARM64 and this firmware both use little-endian words. */
    Reply = *Value;
    Status = SwifiFirmwareIovarGet(Control, IfIndex, Name, &Reply, sizeof(Reply), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned < sizeof(Reply)) return STATUS_DEVICE_PROTOCOL_ERROR;
    *Value = Reply;
    return STATUS_SUCCESS;
}

NTSTATUS SwifiFirmwareSetU32(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex,
    PCSTR Name, ULONG Value)
{
    return SwifiFirmwareIovarSet(Control, IfIndex, Name, &Value, sizeof(Value), NULL);
}

static NTSTATUS OptionalU32(SWIFI_WIN_CONTROL *Control, PCSTR Name, PULONG Value)
{
    NTSTATUS Status = SwifiFirmwareGetU32(Control, 0, Name, Value);
    return Status == STATUS_NOT_SUPPORTED || Status == STATUS_NOT_FOUND ? STATUS_SUCCESS : Status;
}

static NTSTATUS StringQuery(SWIFI_WIN_CONTROL *Control, PCSTR Name, PCHAR Value, USHORT Length)
{
    USHORT Returned, I;
    NTSTATUS Status;
    RtlZeroMemory(Value, Length);
    Status = SwifiFirmwareIovarGet(Control, 0, Name, Value, Length, &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (!Returned) return STATUS_DEVICE_PROTOCOL_ERROR;
    for (I = 0; I < Returned; ++I)
        if (!Value[I] || Value[I] == '\n') break;
    Value[I < Length ? I : Length - 1] = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS GetBandwidth(SWIFI_WIN_CONTROL *Control, SWIFI_FIRMWARE_INFO *Info)
{
    ULONG Index, Value;
    BOOLEAN Legacy = FALSE;
    NTSTATUS Status;
    for (Index = 0; Index < 2; ++Index) {
        if (!(Info->Bands & (1u << Index))) continue;
        Value = Index ? WLC_BAND_5G : WLC_BAND_2G;
        Status = SwifiFirmwareGetU32(Control, 0, "bw_cap", &Value);
        if (Status == STATUS_NOT_SUPPORTED || Status == STATUS_NOT_FOUND) { Legacy = TRUE; break; }
        if (!NT_SUCCESS(Status)) return Status;
        if (!(Value & 1u)) return STATUS_DEVICE_PROTOCOL_ERROR;
        Info->Bandwidth[Index] = Value & 15u;
    }
    if (!Legacy) return STATUS_SUCCESS;
    Value = 0; /* Legacy default: 20 MHz in both bands. */
    Status = OptionalU32(Control, "mimo_bw_cap", &Value);
    if (!NT_SUCCESS(Status)) return Status;
    if (Value > 2) return STATUS_DEVICE_PROTOCOL_ERROR;
    if (Info->Bands & SWIFI_BAND_2GHZ) Info->Bandwidth[0] = Value == 1 ? 3 : 1;
    if (Info->Bands & SWIFI_BAND_5GHZ) Info->Bandwidth[1] = Value ? 3 : 1;
    return STATUS_SUCCESS;
}

ULONG SwifiChannelFrequencyMhz(UCHAR Channel)
{
    if (Channel >= 1 && Channel <= 13) return 2407 + 5u * Channel;
    if (Channel == 14) return 2484;
    if (Channel >= 182 && Channel <= 196) return 4000 + 5u * Channel;
    if (Channel > 14 && Channel < 182) return 5000 + 5u * Channel;
    return 0;
}

static NTSTATUS QueryInfo(SWIFI_WIN_CONTROL *Control, SWIFI_FIRMWARE_INFO *Info)
{
    ULONG Words[1 + SWIFI_MAX_CHANNELS] = {0};
    ULONG I, J, Value, Nonzero = 0;
    USHORT Returned;
    NTSTATUS Status;
    Status = SwifiFirmwareCommand(Control, 0, WLC_GET_VERSION,
        &Info->IoctlVersion, sizeof(ULONG), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned < sizeof(ULONG)) return STATUS_DEVICE_PROTOCOL_ERROR;
    if (Info->IoctlVersion != 1 && Info->IoctlVersion != 2) return STATUS_NOT_SUPPORTED;
    Status = SwifiFirmwareIovarGet(Control, 0, "cur_etheraddr", Info->Address,
        sizeof(Info->Address), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    for (I = 0; I < sizeof(Info->Address); ++I) Nonzero |= Info->Address[I];
    if (Returned < sizeof(Info->Address) || !Nonzero || (Info->Address[0] & 1))
        return STATUS_DEVICE_PROTOCOL_ERROR;
    Status = StringQuery(Control, "ver", Info->Version, sizeof(Info->Version));
    if (!NT_SUCCESS(Status)) return Status;
    Status = StringQuery(Control, "cap", Info->Features, sizeof(Info->Features));
    if (Status != STATUS_NOT_SUPPORTED && Status != STATUS_NOT_FOUND && !NT_SUCCESS(Status)) return Status;
    Status = SwifiFirmwareCommand(Control, 0, WLC_GET_RADIO,
        &Info->RadioDisabled, sizeof(ULONG), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned < sizeof(ULONG)) return STATUS_DEVICE_PROTOCOL_ERROR;
    Status = SwifiFirmwareCommand(Control, 0, WLC_GET_BANDLIST,
        Words, 3 * sizeof(ULONG), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned < sizeof(ULONG) || !Words[0] || Words[0] > 2 ||
        Returned < (Words[0] + 1) * sizeof(ULONG)) return STATUS_DEVICE_PROTOCOL_ERROR;
    for (I = 1; I <= Words[0]; ++I) {
        if (Words[I] == WLC_BAND_2G) Info->Bands |= SWIFI_BAND_2GHZ;
        else if (Words[I] == WLC_BAND_5G) Info->Bands |= SWIFI_BAND_5GHZ;
        else return STATUS_NOT_SUPPORTED;
    }
    RtlZeroMemory(Words, sizeof(Words));
    Words[0] = SWIFI_MAX_CHANNELS;
    Status = SwifiFirmwareCommand(Control, 0, WLC_GET_VALID_CHANNELS,
        Words, sizeof(Words), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned < sizeof(ULONG) || !Words[0] || Words[0] > SWIFI_MAX_CHANNELS ||
        Returned < (Words[0] + 1) * sizeof(ULONG)) return STATUS_DEVICE_PROTOCOL_ERROR;
    for (I = 1; I <= Words[0]; ++I) {
        Value = Words[I];
        if (!Value || Value > SWIFI_MAX_CHANNELS ||
            !(Info->Bands & (Value <= 14 ? SWIFI_BAND_2GHZ : SWIFI_BAND_5GHZ)))
            return STATUS_DEVICE_PROTOCOL_ERROR;
        for (J = 0; J < Info->ChannelCount; ++J)
            if (Info->Channels[J] == Value) break;
        if (J == Info->ChannelCount) Info->Channels[Info->ChannelCount++] = (UCHAR)Value;
    }
    Value = 0;
    Status = OptionalU32(Control, "nmode", &Value);
    if (!NT_SUCCESS(Status)) return Status;
    Info->Ht = Value != 0;
    Value = 0;
    Status = OptionalU32(Control, "vhtmode", &Value);
    if (!NT_SUCCESS(Status)) return Status;
    Info->Vht = Value != 0;
    for (I = 0; I < 2; ++I)
        if (Info->Bands & (1u << I)) Info->Bandwidth[I] = 1;
    if (Info->Ht || Info->Vht) {
        Status = GetBandwidth(Control, Info);
        if (!NT_SUCCESS(Status)) return Status;
    }
    Value = 1;
    Status = OptionalU32(Control, "rxchain", &Value);
    if (!NT_SUCCESS(Status)) return Status;
    for (; Value; Value &= Value - 1) ++Info->RxStreams;
    Value = 1;
    if (Info->Vht) {
        Status = OptionalU32(Control, "txstreams", &Value);
        if (!NT_SUCCESS(Status)) return Status;
    }
    if (!Info->RxStreams || Info->RxStreams > 8 || !Value || Value > 8)
        return STATUS_DEVICE_PROTOCOL_ERROR;
    Info->TxStreams = (UCHAR)Value;
    return STATUS_SUCCESS;
}

NTSTATUS SwifiFirmwareQueryInfo(SWIFI_WIN_CONTROL *Control, SWIFI_FIRMWARE_INFO *Info)
{
    NTSTATUS Status;
    if (!Info) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Info, sizeof(*Info));
    Status = QueryInfo(Control, Info);
    if (!NT_SUCCESS(Status)) RtlZeroMemory(Info, sizeof(*Info));
    return Status;
}
