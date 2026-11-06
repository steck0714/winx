/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_WINDOWS_SCAN_FIRMWARE_H
#define SWIFI_WINDOWS_SCAN_FIRMWARE_H

#include "firmware.h"
#include "../core/WifiEscan.h"

enum { SwifiScanVendorMax = 1024 };
typedef struct SWIFI_SCAN_POLICY {
    ULONG OldMpc;
    BOOLEAN MpcChanged;
    UCHAR Vendor[SwifiScanVendorMax];
    USHORT Added;
    USHORT Removed;
} SWIFI_SCAN_POLICY;

/* All calls are serialized at PASSIVE_LEVEL by the scan task. Prepare may
 * partially apply policy on error. Always Restore or reset, then allow reuse. */
NTSTATUS SwifiScanPrepare(SWIFI_WIN_CONTROL *Control, const SWIFI_FIRMWARE_INFO *Info,
    SWIFI_SCAN_SPEC *Spec, BOOLEAN ActiveOnly, const UCHAR *Vendor, ULONG VendorLength,
    ULONGLONG Deadline, SWIFI_SCAN_POLICY *Policy);
NTSTATUS SwifiScanIssue(SWIFI_WIN_CONTROL *Control, const SWIFI_FIRMWARE_INFO *Info,
    const SWIFI_SCAN_SPEC *Spec, USHORT SyncId);
NTSTATUS SwifiScanAbortFirmware(SWIFI_WIN_CONTROL *Control);
NTSTATUS SwifiScanRestore(SWIFI_WIN_CONTROL *Control, SWIFI_SCAN_POLICY *Policy);

#endif
