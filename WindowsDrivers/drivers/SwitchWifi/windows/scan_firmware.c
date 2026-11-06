/* SPDX-License-Identifier: ISC */
#include "scan_firmware.h"

/* Bsscfg 0 uses the ordinary iovar form. WPA and WME probe IEs are handled
 * by the firmware, as in Switchroot's brcmf_vif_set_mgmt_ie. */
static NTSTATUS VendorIe(SWIFI_WIN_CONTROL *Control, const UCHAR *Ie, USHORT Size, BOOLEAN Add)
{
    UCHAR Command[12 + 257] = {0};
    if (Ie[2] == 0 && Ie[3] == 0x50 && Ie[4] == 0xf2 && (Ie[5] == 1 || Ie[5] == 2))
        return STATUS_SUCCESS;
    /* A station scan must not send Wi-Fi Direct IEs. */
    if (Ie[2] == 0x50 && Ie[3] == 0x6f && Ie[4] == 0x9a && Ie[5] == 9)
        return STATUS_SUCCESS;
    RtlCopyMemory(Command, Add ? "add" : "del", 3);
    SwifiScanPut32(Command + 4, 1);
    SwifiScanPut32(Command + 8, 0x10); /* Probe requests. */
    RtlCopyMemory(Command + 12, Ie, Size);
    return SwifiFirmwareIovarSet(Control, 0, "vndr_ie", Command, (USHORT)(12 + Size), NULL);
}

static NTSTATUS PrepareChannels(SWIFI_WIN_CONTROL *Control, const SWIFI_FIRMWARE_INFO *Info,
    SWIFI_SCAN_SPEC *Spec, BOOLEAN ActiveOnly, ULONGLONG Deadline)
{
    ULONG Keep = 0;
    if (!Spec->ChannelCount) {
        RtlCopyMemory(Spec->Channels, Info->Channels, Info->ChannelCount);
        Spec->ChannelCount = Info->ChannelCount;
    }
    if (Spec->ChannelCount > SwifiScanChannels) return STATUS_INVALID_PARAMETER;
    for (ULONG I = 0; I < Spec->ChannelCount; ++I) {
        ULONG Ch = Spec->Channels[I], J;
        for (J = 0; J < Info->ChannelCount && Info->Channels[J] != Ch; ++J) {}
        if (J == Info->ChannelCount) return STATUS_INVALID_PARAMETER;
        if (ActiveOnly) {
            if (Deadline && KeQueryInterruptTime() >= Deadline) return STATUS_IO_TIMEOUT;
            ULONG Flags = SwifiScanChanspec(Info->IoctlVersion, Ch);
            NTSTATUS Status = SwifiFirmwareGetU32(Control, 0, "per_chan_info", &Flags);
            if (!NT_SUCCESS(Status)) return Status;
            /* Both hardware and country must allow this channel. Do not
             * actively probe radar, inactive, passive or restricted channels. */
            if ((Flags & 3) != 3 || (Flags & 0x78)) continue;
        }
        Spec->Channels[Keep++] = (UCHAR)Ch;
    }
    Spec->ChannelCount = Keep;
    return Keep ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

NTSTATUS SwifiScanPrepare(SWIFI_WIN_CONTROL *Control, const SWIFI_FIRMWARE_INFO *Info,
    SWIFI_SCAN_SPEC *Spec, BOOLEAN ActiveOnly, const UCHAR *Vendor, ULONG VendorLength,
    ULONGLONG Deadline, SWIFI_SCAN_POLICY *Policy)
{
    UCHAR Events[18] = {0};
    ULONG Radio = 0, Up = 1;
    USHORT Returned;
    NTSTATUS Status;
    if (Policy->MpcChanged || Policy->Added != Policy->Removed) return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(Policy, sizeof(*Policy));
    if (VendorLength > sizeof(Policy->Vendor) || (VendorLength && !Vendor))
        return STATUS_INVALID_PARAMETER;
    for (ULONG Offset = 0; Offset < VendorLength;) {
        if (VendorLength - Offset < 2 || Vendor[Offset] != 221 || Vendor[Offset + 1] < 4 ||
            (ULONG)Vendor[Offset + 1] + 2 > VendorLength - Offset) return STATUS_INVALID_PARAMETER;
        Offset += 2 + Vendor[Offset + 1];
    }
    Status = SwifiFirmwareCommand(Control, 0, 37, &Radio, sizeof(Radio), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned < sizeof(Radio)) return STATUS_DEVICE_PROTOCOL_ERROR;
    if (Radio & 3) return STATUS_DEVICE_POWERED_OFF;
    Status = SwifiFirmwareIovarGet(Control, 0, "event_msgs", Events, sizeof(Events), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned <= 69 / 8) return STATUS_DEVICE_PROTOCOL_ERROR;
    Events[69 / 8] |= 1u << (69 % 8);
    Status = SwifiFirmwareIovarSet(Control, 0, "event_msgs", Events, Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    Status = SwifiFirmwareGetU32(Control, 0, "mpc", &Policy->OldMpc);
    if (!NT_SUCCESS(Status)) return Status;
    if (Policy->OldMpc) {
        Status = SwifiFirmwareSetU32(Control, 0, "mpc", 0);
        if (!NT_SUCCESS(Status)) return Status;
        Policy->MpcChanged = TRUE;
    }
    Status = SwifiFirmwareCommand(Control, 0, 2, &Up, sizeof(Up), NULL, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    Status = PrepareChannels(Control, Info, Spec, ActiveOnly, Deadline);
    if (!NT_SUCCESS(Status)) return Status;
    if (VendorLength) RtlCopyMemory(Policy->Vendor, Vendor, VendorLength);
    while (Policy->Added < VendorLength) {
        UCHAR *Ie = Policy->Vendor + Policy->Added;
        USHORT Length = (USHORT)(2 + Ie[1]);
        Status = VendorIe(Control, Ie, Length, TRUE);
        if (!NT_SUCCESS(Status)) return Status;
        Policy->Added += Length;
    }
    return STATUS_SUCCESS;
}

NTSTATUS SwifiScanIssue(SWIFI_WIN_CONTROL *Control, const SWIFI_FIRMWARE_INFO *Info,
    const SWIFI_SCAN_SPEC *Spec, USHORT SyncId)
{
    UCHAR Params[SwifiScanWireMax];
    unsigned Length = SwifiEncodeScan(Params, sizeof(Params), Info->IoctlVersion, SyncId, Spec);
    if (!Length) return STATUS_INVALID_PARAMETER;
    return SwifiFirmwareIovarSet(Control, 0, "escan", Params, (USHORT)Length, NULL);
}

NTSTATUS SwifiScanAbortFirmware(SWIFI_WIN_CONTROL *Control)
{
    UCHAR Params[68];
    SwifiEncodeScanAbort(Params);
    return SwifiFirmwareCommand(Control, 0, 50, Params, sizeof(Params), NULL, NULL);
}

NTSTATUS SwifiScanRestore(SWIFI_WIN_CONTROL *Control, SWIFI_SCAN_POLICY *Policy)
{
    NTSTATUS Status;
    while (Policy->Removed < Policy->Added) {
        UCHAR *Ie = Policy->Vendor + Policy->Removed;
        USHORT Length = (USHORT)(2 + Ie[1]);
        Status = VendorIe(Control, Ie, Length, FALSE);
        if (!NT_SUCCESS(Status)) return Status;
        Policy->Removed += Length;
    }
    if (Policy->MpcChanged) {
        Status = SwifiFirmwareSetU32(Control, 0, "mpc", Policy->OldMpc);
        if (!NT_SUCCESS(Status)) return Status;
        Policy->MpcChanged = FALSE;
    }
    return STATUS_SUCCESS;
}
