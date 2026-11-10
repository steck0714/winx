/* SPDX-License-Identifier: ISC */
/*
 * Single-board Broadcom NVRAM text conversion for the Switch PCIe loader.
 * Based on brcmfmac/firmware.c's entry and length-token format.
 * Copyright (c) 2013 Broadcom Corporation
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
#ifndef WIFI_NVRAM_H
#define WIFI_NVRAM_H

#define PEX_NVRAM_MAX 64000U

/* brcmfmac/pcie.c RAMS header: the firmware can reserve part of CR4 RAM.
 * Keep the result inside the discovered aperture before placing the token. */
STATIC BOOLEAN PexFirmwareRamSize (CONST UINT8 *Fw, UINTN Length,
  UINT32 Capacity, UINT32 *Size)
{
  UINT32 Value = Capacity;
  *Size = 0;
  if (Fw == NULL || Length < 4 || Length > Capacity) return FALSE;
  if (Length >= 0x74 && Fw[0x6c] == 'R' && Fw[0x6d] == 'A' &&
      Fw[0x6e] == 'M' && Fw[0x6f] == 'S') {
    Value = Fw[0x70] | ((UINT32)Fw[0x71] << 8) |
            ((UINT32)Fw[0x72] << 16) | ((UINT32)Fw[0x73] << 24);
  }
  if (Value < Length || Value > Capacity || (Value & 3)) return FALSE;
  *Size = Value;
  return TRUE;
}

STATIC
BOOLEAN
PexNvramBuild (
  IN  CONST UINT8  *Nv,
  IN  UINTN        NvSz,
  OUT UINT8        **Out,
  OUT UINT32       *OutLen
  )
{
  enum { Idle, Key, Value, Comment } State = Idle;
  STATIC CONST CHAR8  DefaultBoardrev[] = "boardrev=0xff";
  UINT8    *Clean;
  UINT8    C;
  UINTN    I = 0;
  UINTN    Start = 0;
  UINTN    KeyLen = 0;
  UINTN    O = 0;
  UINTN    Length;
  UINTN    Capacity;
  UINT32   Token;
  BOOLEAN  Boardrev = FALSE;

  *Out = NULL;
  *OutLen = 0;
  if (Nv == NULL || NvSz == 0) return FALSE;
  Capacity = NvSz < PEX_NVRAM_MAX ? NvSz : PEX_NVRAM_MAX;
  // Last entry NUL, extra terminator, up to three pad bytes and length token.
  Clean = AllocateZeroPool (Capacity + sizeof (DefaultBoardrev) + 9);
  if (Clean == NULL) return FALSE;

  // A synthetic final NUL also completes a text file without a final newline.
  while (I <= NvSz) {
    C = I < NvSz ? Nv[I] : 0;
    switch (State) {
      case Idle:
        if (I == NvSz) goto Finished;
        if (C == '#') {
          State = Comment;
        } else if (C > 0x20 && C < 0x7F) {
          Start = I;
          State = Key;
        } else {
          I++;
        }
        break;
      case Key:
        if (C == '=' && I != Start) {
          KeyLen = I - Start;
          State = KeyLen >= 4 && CompareMem (Nv + Start, "RAW1", 4) == 0 ? Comment : Value;
          I++;
        } else if (C <= 0x20 || C >= 0x7F || C == '#' || C == '=') {
          State = Comment;
        } else {
          I++;
        }
        break;
      case Value:
        if (C >= 0x20 && C < 0x7F && C != '#') {
          I++;
          break;
        }
        // Switch board files contain direct keys. Multi-device router files
        // need a PCI-domain selector and must not be sent to the chip verbatim.
        if ((KeyLen >= 7 && CompareMem (Nv + Start, "devpath", 7) == 0) ||
            (KeyLen >= 5 && CompareMem (Nv + Start, "pcie/", 5) == 0)) goto Failed;
        Length = I - Start;
        if (Length + 1 > PEX_NVRAM_MAX - O) goto Failed;
        CopyMem (Clean + O, Nv + Start, Length);
        O += Length + 1;
        if (KeyLen == 8 && CompareMem (Nv + Start, "boardrev", 8) == 0) Boardrev = TRUE;
        State = Idle;
        break;
      case Comment:
        if (I == NvSz) goto Finished;
        if (C == '\n' || C == 0) State = Idle;
        I++;
        break;
    }
  }

Finished:
  if (O == 0) goto Failed;
  if (!Boardrev) {
    if (sizeof (DefaultBoardrev) > PEX_NVRAM_MAX - O) goto Failed;
    CopyMem (Clean + O, DefaultBoardrev, sizeof (DefaultBoardrev));
    O += sizeof (DefaultBoardrev);
  }
  O = (O + 1 + 3) & ~(UINTN)3; // extra terminator, padded to 32 bits
  Token = (UINT32)(O / 4);
  Token = ((~Token & 0xFFFFU) << 16) | Token;
  Clean[O]     = (UINT8)Token;
  Clean[O + 1] = (UINT8)(Token >> 8);
  Clean[O + 2] = (UINT8)(Token >> 16);
  Clean[O + 3] = (UINT8)(Token >> 24);
  *Out = Clean;
  *OutLen = (UINT32)O + 4;
  return TRUE;
Failed:
  FreePool (Clean);
  return FALSE;
}

#endif
