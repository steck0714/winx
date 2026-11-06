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

STATIC UINT32 ScanBe32 (CONST UINT8 *P)
{
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) | ((UINT32)P[2] << 8) | P[3];
}

/* Ethernet (14), Broadcom (10), and event-message (48) headers precede the
 * data. BSS info v109 has natural 32-bit alignment on the wire. */
STATIC BOOLEAN ScanEvent (VOID *Context, CONST UINT8 *Data, UINT16 Length)
{
  PEX_SCAN *Scan = Context;
  CONST UINT8 *Result, *Bss;
  UINT32 Event, Status, Size, BssLength, IeOffset, IeLength;
  UINTN I;
  CHAR8 Ssid[33];
  if (Length < 72 || Data[12] != 0x88 || Data[13] != 0x6c ||
      Data[19] != 0 || Data[20] != 0x10 || Data[21] != 0x18 ||
      Data[22] != 0 || Data[23] != 1) return FALSE;
  Event = ScanBe32 (Data + 28);
  Size = ScanBe32 (Data + 44);
  if (Size > (UINT32)Length - 72) return FALSE;
  if (Event != 69 || Data[70] != 0) return TRUE;
  Status = ScanBe32 (Data + 32);
  if (Status != 8) {
    Scan->Done = TRUE;
    Scan->Failed = Status != 0;
    Log (L"pscan: terminal status=%d records=%d", Status, Scan->Records);
    return TRUE;
  }
  if (Size < 12 + 128) return FALSE;
  Result = Data + 72;
  if (MsgLe16 (Result + 8) != Scan->SyncId) return TRUE;
  if (Le32 (Result) < 140 || Le32 (Result) > Size || MsgLe16 (Result + 10) != 1) return FALSE;
  Bss = Result + 12;
  BssLength = Le32 (Bss + 4);
  if (Le32 (Bss) != 109 || BssLength != Le32 (Result) - 12 || Bss[18] > 32) return FALSE;
  IeOffset = MsgLe16 (Bss + 116);
  IeLength = Le32 (Bss + 120);
  if (IeLength && (IeOffset < 128 || IeOffset > BssLength || IeLength > BssLength - IeOffset)) return FALSE;
  for (I = 0; I < Bss[18]; I++) {
    UINT8 Ch = Bss[19 + I];
    Ssid[I] = Ch >= 0x20 && Ch < 0x7f ? (CHAR8)Ch : '.';
  }
  Ssid[I] = 0;
  Scan->Records++;
  Log (L"pscan: %02x:%02x:%02x:%02x:%02x:%02x ch=%d rssi=%d cap=%04x ssid=%a",
       Bss[8], Bss[9], Bss[10], Bss[11], Bss[12], Bss[13], Bss[88],
       (INT16)MsgLe16 (Bss + 78), MsgLe16 (Bss + 16), Ssid);
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
  ZeroMem (Params, sizeof (Params));
  MsgPut32 (Params, 1); MsgPut16 (Params + 4, 1); MsgPut16 (Params + 6, Scan.SyncId);
  for (I = 44; I < 50; I++) Params[I] = 0xff;
  Params[50] = 2; // DOT11_BSSTYPE_ANY
  Params[51] = 1; // BRCMF_SCANTYPE_PASSIVE
  for (I = 52; I < 68; I += 4) MsgPut32 (Params + I, 0xffffffffU);
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
