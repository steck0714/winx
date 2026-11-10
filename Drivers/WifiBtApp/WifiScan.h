/* SPDX-License-Identifier: ISC
 * BCM4356 passive escan using the Switchroot brcmfmac event/scan wire format.
 * Copyright (c) 2010-2014 Broadcom Corporation
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

typedef struct {
  UINT16 SyncId;
  UINT32 Records;
  BOOLEAN Done;
  BOOLEAN Failed;
} PEX_SCAN;

#include "../../WindowsDrivers/t214/Hoag/drivers/SwitchWifi/core/WifiEscan.h"

STATIC BOOLEAN ScanEvent (VOID *Context, CONST UINT8 *Data, UINT16 Length)
{
  PEX_SCAN *Scan = Context;
  SWIFI_SCAN_BSS Bss;
  unsigned Status;
  enum swifi_scan_event Kind = SwifiDecodeScan (Data, Length, 0, Scan->SyncId, &Bss, &Status);
  if (Kind == SwifiScanInvalid) return FALSE;
  if (Kind == SwifiScanOther) return TRUE;
  if (Kind == SwifiScanTerminal) {
    Scan->Done = TRUE;
    Scan->Failed = Status != 0;
    Log (L"pscan: terminal status=%d records=%d", Status, Scan->Records);
    return TRUE;
  }
  CHAR8 Ssid[33];
  for (unsigned I = 0; I < Bss.SsidLength; ++I) {
    UINT8 Ch = Bss.Ssid[I];
    Ssid[I] = Ch >= 0x20 && Ch < 0x7f ? (CHAR8)Ch : '.';
  }
  Ssid[Bss.SsidLength] = 0;
  Scan->Records++;
  Log (L"pscan: %02x:%02x:%02x:%02x:%02x:%02x ch=%d rssi=%d cap=%04x ssid=%a",
       Bss.Bssid[0], Bss.Bssid[1], Bss.Bssid[2], Bss.Bssid[3], Bss.Bssid[4], Bss.Bssid[5],
       Bss.Channel, Bss.Rssi, Bss.Capability, Ssid);
  return TRUE;
}

STATIC BOOLEAN PexScanProbe (CONST UINT8 *Clm, UINTN ClmLength)
{
  PEX_MSGBUF M;
  PEX_SCAN Scan;
  UINT8 Params[72], Mask[18], Value[4];
  UINT16 Returned;
  UINTN I;
  BOOLEAN Result = FALSE;
  ZeroMem (&M, sizeof (M)); ZeroMem (&Scan, sizeof (Scan));
  Scan.SyncId = 0x1234;
  if (!MsgInitialize (&M)) goto Done;
  if (Clm != NULL && !MsgLoadClm (&M, Clm, ClmLength)) goto Done;
  if (!MsgStartEvents (&M, ScanEvent, &Scan)) goto Done;
  ZeroMem (Mask, sizeof (Mask)); Mask[69 / 8] = 1U << (69 % 8);
  if (!MsgSetIovar (&M, "event_msgs", Mask, sizeof (Mask))) goto Done;
  MsgPut32 (Value, 0);
  if (!MsgSetIovar (&M, "mpc", Value, sizeof (Value))) goto Done;
  MsgPut32 (Value, 1);
  if (!MsgIoctl (&M, 2, Value, sizeof (Value), &Returned)) goto Done;
  MsgPut32 (Value, 1);
  if (!MsgIoctl (&M, 49, Value, sizeof (Value), &Returned)) goto Done;
  SWIFI_SCAN_SPEC Spec;
  ZeroMem (&Spec, sizeof (Spec));
  Spec.Passive = 1;
  for (I = 0; I < 6; ++I) Spec.Bssid[I] = 0xff;
  // Wildcard probe: no channel list, so no version-specific chanspecs.
  if (SwifiEncodeScan (Params, sizeof (Params), 2, Scan.SyncId, &Spec) != sizeof (Params)) goto Done;
  Log (L"pscan: starting passive scan, up to 20 seconds");
  if (!MsgSetIovar (&M, "escan", Params, sizeof (Params))) goto Done;
  for (I = 0; I < 20000 && !Scan.Done; I++) {
    if (MsgPollControl (&M, 0, NULL, 0, &Returned) < 0) goto Done;
    Ms (1);
  }
  Result = Scan.Done && !Scan.Failed;
Done:
  MsgRelease (&M);
  Log (L"pscan: %s, records=%d; Wi-Fi reset and DMA released",
       Result ? L"SCAN COMPLETE" : L"SCAN FAILED", Scan.Records);
  return Result;
}
#endif
