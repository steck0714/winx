/* SPDX-License-Identifier: GPL-2.0-only */
#include "join_firmware.h"
#include "station_filter.h"

NTSTATUS SwifiJoinFirmware(SWIFI_DEVICE *D, const SWIFI_FIRMWARE_INFO *Info,
    const swifi_bss_profile *Profile, const UCHAR Peer[6], UCHAR Channel, const UCHAR *Pmkid)
{
    SWIFI_WIN_CONTROL *C = &D->Hw.Control;
    UCHAR Join[SWIFI_JOIN_LEN], Rsn[SWIFI_RSN_MAX], Events[18] = {0};
    const BOOLEAN Secure = Profile->auth == SWIFI_AUTH_PSK_CCMP;
    USHORT Returned;
    ULONG One = 1;
    if (!swifi_join_request(Join, Profile, Peer, SwifiScanChanspec(Info->IoctlVersion, Channel)))
        return STATUS_INVALID_PARAMETER;
    ULONG ChannelFlags = SwifiScanChanspec(Info->IoctlVersion, Channel);
    NTSTATUS Status = SwifiFirmwareGetU32(C, 0, "per_chan_info", &ChannelFlags);
    if (!NT_SUCCESS(Status)) return Status;
    if ((ChannelFlags & 3) != 3 || (ChannelFlags & 0x10)) return STATUS_NOT_SUPPORTED;
    if (ChannelFlags & 0x68) Join[36] = 1; /* passive join scan on restricted channels */
    Status = SwifiFirmwareIovarGet(C, 0, "event_msgs", Events, sizeof(Events), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned < 3) return STATUS_DEVICE_PROTOCOL_ERROR;
    static const UCHAR Required[] = {0, 5, 6, 11, 12, 16, 19};
    for (ULONG I = 0; I < RTL_NUMBER_OF(Required); ++I) Events[Required[I]/8] |= 1u << (Required[I]%8);
    Status = SwifiFirmwareIovarSet(C, 0, "event_msgs", Events, Returned, NULL);
    if (NT_SUCCESS(Status)) Status = SwifiFirmwareSetU32(C, 0, "roam_off", 1);
    if (NT_SUCCESS(Status)) Status = SwifiFirmwareCommand(C, 0, 20, &One, sizeof(One), NULL, NULL);
    if (NT_SUCCESS(Status)) {
        Status = SwifiFirmwareSetU32(C, 0, "sup_wpa", 0); /* Windows owns the four-way handshake. */
        if (Status == STATUS_NOT_SUPPORTED || Status == STATUS_NOT_FOUND) Status = STATUS_SUCCESS;
    }
    if (NT_SUCCESS(Status)) Status = SwifiFirmwareSetU32(C, 0, "auth", 0);
    if (NT_SUCCESS(Status)) Status = SwifiFirmwareSetU32(C, 0, "wsec", Secure ? 4 : 0);
    if (NT_SUCCESS(Status)) Status = SwifiFirmwareSetU32(C, 0, "wpa_auth", Secure ? 0x80 : 0);
    if (NT_SUCCESS(Status)) {
        Status = SwifiFirmwareSetU32(C, 0, "wsec_restrict", Secure);
        /* The RX completion contains decrypted Ethernet, without the original
         * protection bit. Require firmware cleartext rejection on a secure BSS. */
        if (NT_SUCCESS(Status)) {
            ULONG Actual = 0;
            Status = SwifiFirmwareGetU32(C, 0, "wsec_restrict", &Actual);
            if (NT_SUCCESS(Status) && (Actual != 0) != Secure) Status = STATUS_DEVICE_PROTOCOL_ERROR;
        }
        if (!Secure && (Status == STATUS_NOT_SUPPORTED || Status == STATUS_NOT_FOUND)) Status = STATUS_SUCCESS;
    }
    if (NT_SUCCESS(Status)) {
        USHORT Length = Secure ? (USHORT)swifi_rsn_request(Rsn, Pmkid) : 0;
        Status = SwifiFirmwareIovarSet(C, 0, "wpaie", Rsn, Length, NULL);
    }
    if (NT_SUCCESS(Status)) {
        One = 1;
        Status = SwifiFirmwareCommand(C, 0, 2, &One, sizeof(One), NULL, NULL);
    }
    if (!NT_SUCCESS(Status)) return Status;
    Status = SwifiFilterStart(D);
    if (!NT_SUCCESS(Status)) return Status;
    Status = SwifiFirmwareIovarSet(C, 0, "join", Join, sizeof(Join), NULL);
    if ((Status == STATUS_NOT_SUPPORTED || Status == STATUS_NOT_FOUND) && Join[36] != 1) {
        UCHAR Legacy[52] = {0}; /* SSID followed immediately by assoc_params. */
        RtlCopyMemory(Legacy, Join, 36);
        RtlCopyMemory(Legacy+36, Join+56, 14);
        Status = SwifiFirmwareCommand(C, 0, 26, Legacy, sizeof(Legacy), NULL, NULL);
    }
    return Status;
}

NTSTATUS SwifiLeaveFirmware(SWIFI_WIN_CONTROL *C, const UCHAR Peer[6], USHORT Reason)
{
    UCHAR Scb[12] = {0};
    SwifiScanPut32(Scb, Reason);
    RtlCopyMemory(Scb+4, Peer, 6);
    return SwifiFirmwareCommand(C, 0, 52, Scb, sizeof(Scb), NULL, NULL); /* WLC_DISASSOC */
}

NTSTATUS SwifiJoinReadFrames(SWIFI_WIN_CONTROL *C, const UCHAR Peer[6],
    const swifi_bss_profile *Profile, const UCHAR *Pmkid, SWIFI_ASSOC_FRAMES *Frames)
{
    UCHAR Actual[6] = {0}, Info[28] = {0};
    USHORT Returned;
    NTSTATUS Status = SwifiFirmwareCommand(C, 0, 23, Actual, sizeof(Actual), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned < 6 || !RtlEqualMemory(Actual, Peer, 6)) return STATUS_DEVICE_PROTOCOL_ERROR;
    UCHAR Ssid[36] = {0};
    Status = SwifiFirmwareCommand(C, 0, 25, Ssid, sizeof(Ssid), &Returned, NULL); /* WLC_GET_SSID */
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned < 4u+Profile->ssid_len || SwifiScanLe32(Ssid) != Profile->ssid_len ||
        !RtlEqualMemory(Ssid+4, Profile->ssid, Profile->ssid_len)) return STATUS_DEVICE_PROTOCOL_ERROR;
    Status = SwifiFirmwareIovarGet(C, 0, "assoc_info", Info, sizeof(Info), &Returned, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (Returned < sizeof(Info) || (SwifiScanLe32(Info+8) & 1)) return STATUS_DEVICE_PROTOCOL_ERROR;
    /* wl_assoc_info_t contains the fixed request header at 12, reassoc
     * address at 16 and response header at 22. req/resp lengths include
     * those fixed bytes. The two separate *_ies iovars contain only IEs. */
    ULONG Req = SwifiScanLe32(Info), Resp = SwifiScanLe32(Info+4);
    if (Req < 4 || Req > sizeof(Frames->Req) || Resp < 6 || Resp > sizeof(Frames->Resp) ||
        SwifiScanLe16(Info+24) != 0) return STATUS_DEVICE_PROTOCOL_ERROR;
    RtlCopyMemory(Frames->Req, Info+12, 4);
    RtlCopyMemory(Frames->Resp, Info+22, 6);
    if (Req > 4) {
        Status = SwifiFirmwareIovarGet(C, 0, "assoc_req_ies", Frames->Req+4, (USHORT)(Req-4), &Returned, NULL);
        if (!NT_SUCCESS(Status)) return Status;
        if (Returned != Req-4 || !swifi_valid_ies(Frames->Req+4, Returned)) return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    if (Resp > 6) {
        Status = SwifiFirmwareIovarGet(C, 0, "assoc_resp_ies", Frames->Resp+6, (USHORT)(Resp-6), &Returned, NULL);
        if (!NT_SUCCESS(Status)) return Status;
        if (Returned != Resp-6 || !swifi_valid_ies(Frames->Resp+6, Returned)) return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    if (!swifi_assoc_ies_match(Frames->Req+4, Req-4, Profile, Pmkid)) return STATUS_DEVICE_PROTOCOL_ERROR;
    Frames->ReqLen = (USHORT)Req; Frames->RespLen = (USHORT)Resp;
    return STATUS_SUCCESS;
}

NTSTATUS SwifiJoinKey(SWIFI_WIN_CONTROL *C, UCHAR Id, const UCHAR *Peer, const UCHAR *Key, const UCHAR *Pn)
{
    UCHAR Wire[SWIFI_KEY_LEN];
    if (!swifi_ccmp_key(Wire, Id, Peer, Key, Pn)) return STATUS_INVALID_PARAMETER;
    NTSTATUS Status = SwifiFirmwareIovarSet(C, 0, "wsec_key", Wire, sizeof(Wire), NULL);
    RtlSecureZeroMemory(Wire, sizeof(Wire));
    return Status;
}
