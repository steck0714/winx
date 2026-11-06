/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_WINDOWS_SCAN_H
#define SWIFI_WINDOWS_SCAN_H

#include "requests.h"

typedef struct SWIFI_SCAN SWIFI_SCAN;
typedef VOID (*SWIFI_SCAN_RECOVER)(PVOID Context, NTSTATUS Reason);

NTSTATUS SwifiScanCreate(WDFDEVICE Device, SWIFI_WIN_CONTROL *Control, SWIFI_FIRMWARE_INFO *Info,
    ULONG PeerVersion, SWIFI_SCAN_RECOVER Recover, PVOID Context, SWIFI_SCAN **Scan);
NTSTATUS SwifiScanSetDisallowedBands(SWIFI_SCAN *Scan, ULONG Bands);
VOID SwifiScanStartSession(SWIFI_SCAN *Scan);
VOID SwifiScanEndSession(SWIFI_SCAN *Scan);
VOID SwifiScanStation(SWIFI_SCAN *Scan, WDI_PORT_ID Port, BOOLEAN Present);
VOID SwifiScanRequest(SWIFI_SCAN *Scan, WIFIREQUEST Request, BOOLEAN Abort);
BOOLEAN SwifiScanFirmwareEvent(SWIFI_SCAN *Scan, const swifi_event_completion *Event);
NTSTATUS SwifiScanCancelForRadio(SWIFI_SCAN *Scan);

#endif
