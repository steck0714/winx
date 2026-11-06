/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWIFI_JOIN_FIRMWARE_H
#define SWIFI_JOIN_FIRMWARE_H
#include "scan_firmware.h"
#include "../core/station_security.h"

enum { SwifiAssocBodyMax = 2304 };
typedef struct SWIFI_ASSOC_FRAMES {
    USHORT ReqLen, RespLen;
    UCHAR Req[SwifiAssocBodyMax], Resp[SwifiAssocBodyMax];
} SWIFI_ASSOC_FRAMES;

NTSTATUS SwifiJoinFirmware(SWIFI_WIN_CONTROL *Control, const SWIFI_FIRMWARE_INFO *Info,
    const swifi_bss_profile *Profile, const UCHAR Peer[6], UCHAR Channel, const UCHAR *Pmkid);
NTSTATUS SwifiLeaveFirmware(SWIFI_WIN_CONTROL *Control, const UCHAR Peer[6], USHORT Reason);
NTSTATUS SwifiJoinReadFrames(SWIFI_WIN_CONTROL *Control, const UCHAR Peer[6],
    const swifi_bss_profile *Profile, const UCHAR *Pmkid, SWIFI_ASSOC_FRAMES *Frames);
NTSTATUS SwifiJoinKey(SWIFI_WIN_CONTROL *Control, UCHAR Id, const UCHAR *Peer,
    const UCHAR *Key, const UCHAR *Pn);
#endif
