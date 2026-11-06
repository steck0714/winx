/* SPDX-License-Identifier: ISC */
/*
 * Broadcom AI EROM component/address decoding, adapted from brcmfmac/chip.c.
 * Copyright (c) 2010 Broadcom Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef WIFI_EROM_H
#define WIFI_EROM_H

typedef struct {
  UINT32  Id;
  UINT32  Revision;
  UINT32  Base;
  UINT32  Wrapper;
} PEX_CORE;

STATIC
BOOLEAN
PexEromParse (
  IN  CONST UINT32  *Words,
  IN  UINTN         WordCount,
  OUT PEX_CORE      *Cores,
  IN  UINTN         Capacity,
  OUT UINTN        *CoreCount
  )
{
  UINTN     I = 0;
  UINT32    A;
  UINT32    B;
  UINT32    V;
  UINT32    Type;
  UINT32    Upper;
  UINT32    Size;
  UINT32    SlaveType;
  UINT32    WrapperType;
  PEX_CORE  Core;

  *CoreCount = 0;
  while (I < WordCount) {
    A = Words[I++];
    if (A == 0xFFFFFFFFU) return FALSE;
    if ((A & 0xF) == 0xF) return TRUE;
    if ((A & 0xF) != 1) continue;
    if (I == WordCount || (Words[I] & 0xF) != 1) return FALSE;
    B = Words[I++];
    Core.Id = (A >> 8) & 0xFFF;
    Core.Revision = B >> 24;
    Core.Base = Core.Wrapper = 0;
    WrapperType = 0;

    while (I < WordCount) {
      V = Words[I];
      if (V == 0xFFFFFFFFU) return FALSE;
      Type = V & 0xF;
      if (Type == 1 || Type == 0xF) break;
      I++;
      if (Type == 3) {
        WrapperType = 3; // master-port descriptors select a master wrapper
        continue;
      }
      if ((Type & ~8U) != 5) continue;
      if (WrapperType == 0) WrapperType = 2; // address first: slave wrapper
      Upper = 0;
      if (V & 8) {
        if (I == WordCount) return FALSE;
        Upper = Words[I++];
      }
      Size = (V >> 4) & 3;
      if (Size == 3) {
        if (I == WordCount) return FALSE;
        if (Words[I++] & 8) {
          if (I == WordCount) return FALSE;
          I++;
        }
      }
      // BCM4356 register/wrapper windows are 4 KiB and below 4 GiB.
      // Consume larger descriptors, but do not mistake their payload for cores.
      if (Size != 0 || Upper != 0) continue;
      SlaveType = (V >> 6) & 3;
      if (SlaveType == 0 && Core.Base == 0) Core.Base = V & 0xFFFFF000U;
      if (SlaveType == WrapperType && Core.Wrapper == 0) Core.Wrapper = V & 0xFFFFF000U;
    }
    if (Core.Base != 0 && (((B >> 14) & 0x1F) + ((B >> 19) & 0x1F)) != 0) {
      if (*CoreCount == Capacity) return FALSE;
      Cores[(*CoreCount)++] = Core;
    }
  }
  return FALSE; // truncated table, without an end descriptor
}

#endif
