/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWIFI_WINDOWS_JOIN_H
#define SWIFI_WINDOWS_JOIN_H
#include "scan.h"
#include <TlvGeneratorParser.hpp>

NTSTATUS SwifiJoinCreate(WDFDEVICE Device, SWIFI_WIN_CONTROL *Control, SWIFI_FIRMWARE_INFO *Info,
    ULONG Version, SWIFI_SCAN *Scan, SWIFI_JOIN **Join);
VOID SwifiJoinStartSession(SWIFI_JOIN *Join);
VOID SwifiJoinEndSession(SWIFI_JOIN *Join);
VOID SwifiJoinAfterReset(SWIFI_JOIN *Join);
VOID SwifiJoinStation(SWIFI_JOIN *Join, WDI_PORT_ID Port, BOOLEAN Present);
NTSTATUS SwifiJoinSetBands(SWIFI_JOIN *Join, ULONG Disallowed);
BOOLEAN SwifiJoinCommand(SWIFI_JOIN *Join, WIFIREQUEST Request, UINT16 Id, WDI_MESSAGE_HEADER *Header, ULONG Bytes);
BOOLEAN SwifiJoinMayScan(SWIFI_JOIN *Join);
NTSTATUS SwifiJoinRadioOff(SWIFI_JOIN *Join);
#endif
