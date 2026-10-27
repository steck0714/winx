/** @file
  FbText.c - minimal text console on the panel framebuffer for the exception handler.

  Used when the CPU takes an unexpected exception (e.g. an alignment fault): the normal UEFI console may be dead, the serial
  port is not wired on a retail Switch, so the register dump is drawn straight into the framebuffer that hekate/SimpleFbDxe
  use. Pure CPU stores into the framebuffer, no allocation, no protocol use. Font: 5x12 (Android/lk fbcon, BSD).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>
#include <Library/CacheMaintenanceLib.h>

#include <Resources/font5x12.h>

#ifndef EXC_FB_SCALE
#define EXC_FB_SCALE  1
#endif

#define CELL_W  ((FONT_WIDTH + 1) * EXC_FB_SCALE)
#define CELL_H  ((FONT_HEIGHT + 1) * EXC_FB_SCALE)

STATIC BOOLEAN  mInit;
STATIC UINT32   mCol;
STATIC UINT32   mRow;
STATIC UINT32   mLogW;      // logical (rotated) width/height in pixels
STATIC UINT32   mLogH;

#define BG  0xFF000080U     // dark blue
#define FG  0xFFFFFFFFU

STATIC
volatile UINT32 *
PixelAddr (
  IN UINT32  Lx,
  IN UINT32  Ly
  )
{
  UINT32  W = FixedPcdGet32 (PcdMipiFrameBufferWidth);       // stride in pixels
  UINT32  VisW = FixedPcdGet32 (PcdMipiFrameBufferVisibleWidth);
  UINT32  H = FixedPcdGet32 (PcdMipiFrameBufferHeight);
  UINT32  X;
  UINT32  Y;

#if defined (EXC_FB_ROTATE)
 #if (EXC_FB_ROTATE == 2)
  X = Ly;
  Y = (H - 1) - Lx;
 #else
  X = (VisW - 1) - Ly;
  Y = Lx;
 #endif
#else
  X = Lx;
  Y = Ly;
#endif
  return (volatile UINT32 *)(UINTN)FixedPcdGet32 (PcdMipiFrameBufferAddress) + (UINTN)Y * W + X;
}

STATIC
VOID
Flush (
  VOID
  )
{
  WriteBackDataCacheRange (
    (VOID *)(UINTN)FixedPcdGet32 (PcdMipiFrameBufferAddress),
    (UINTN)FixedPcdGet32 (PcdMipiFrameBufferWidth) * FixedPcdGet32 (PcdMipiFrameBufferHeight) * 4
    );
}

STATIC
VOID
Init (
  VOID
  )
{
  UINT32  X;
  UINT32  Y;

  mInit = TRUE;
#if defined (EXC_FB_ROTATE)
  mLogW = FixedPcdGet32 (PcdMipiFrameBufferHeight);
  mLogH = FixedPcdGet32 (PcdMipiFrameBufferVisibleWidth);
#else
  mLogW = FixedPcdGet32 (PcdMipiFrameBufferVisibleWidth);
  mLogH = FixedPcdGet32 (PcdMipiFrameBufferHeight);
#endif
  for (Y = 0; Y < mLogH; Y++) {
    for (X = 0; X < mLogW; X++) {
      *PixelAddr (X, Y) = BG;
    }
  }
  mCol = 0;
  mRow = 0;
}

STATIC
VOID
PutChar (
  IN CHAR8  C
  )
{
  UINT32    Gx;
  UINT32    Gy;
  UINT32    X0;
  UINT32    Y0;
  UINT32    Row;
  UINT32    Col;
  UINT32    Bits;
  UINT32    Sx;
  UINT32    Sy;
  UINT32    *Glyph;

  if (C == '\r') {
    return;
  }
  if (C == '\n') {
    mCol = 0;
    mRow++;
    return;
  }
  if (C < 32 || C > 126) {
    C = '?';
  }
  if ((mCol + 1) * CELL_W > mLogW) {
    mCol = 0;
    mRow++;
  }
  if ((mRow + 1) * CELL_H > mLogH) {
    mRow = 0;                                         // wrap; keep the first lines readable by overwriting from the top
    mCol = 0;
  }
  Glyph = &font5x12[(C - 32) * 2];
  X0    = mCol * CELL_W;
  Y0    = mRow * CELL_H;
  for (Row = 0; Row < FONT_HEIGHT; Row++) {
    Bits = Glyph[Row < FONT_HEIGHT / 2 ? 0 : 1] >> ((Row % (FONT_HEIGHT / 2)) * FONT_WIDTH);
    for (Col = 0; Col < FONT_WIDTH; Col++) {
      for (Sy = 0; Sy < EXC_FB_SCALE; Sy++) {
        for (Sx = 0; Sx < EXC_FB_SCALE; Sx++) {
          *PixelAddr (X0 + Col * EXC_FB_SCALE + Sx, Y0 + Row * EXC_FB_SCALE + Sy) = (Bits & 1) ? FG : BG;
        }
      }
      Bits >>= 1;
    }
  }
  (VOID)Gx;
  (VOID)Gy;
  mCol++;
}

UINTN
EFIAPI
ExcWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  I;

  if (!mInit) {
    Init ();
  }
  for (I = 0; I < NumberOfBytes; I++) {
    PutChar ((CHAR8)Buffer[I]);
  }
  Flush ();
  return SerialPortWrite (Buffer, NumberOfBytes);
}

VOID
EFIAPI
ExcDebug (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Buf[256];
  VA_LIST  Ap;
  UINTN    N;

  VA_START (Ap, Format);
  N = AsciiVSPrint (Buf, sizeof (Buf), Format, Ap);
  VA_END (Ap);
  ExcWrite ((UINT8 *)Buf, N);
}
