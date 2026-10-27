/** @file
  Software mouse cursor.

  UsbMouseDxe publishes EFI_SIMPLE_POINTER_PROTOCOL but nothing in this firmware draws a pointer, so a USB
  mouse looked dead. This driver polls every SimplePointer instance every 20 ms, integrates the relative
  movement (with pointer acceleration and sub-pixel remainders) and draws an arrow on the GOP framebuffer.
  Left button = red arrow, right button = blue arrow (visual feedback for testing). The cursor is hidden until
  the first mouse event arrives.

  Events are NOT stolen from other consumers (Shell edit/hexedit via ConSplitter, HII UI): each instance's
  GetState() is replaced by a wrapper that hands out the movement accumulated by our poll. If nobody polls for
  500 ms the accumulated movement is dropped (buttons are kept), so a consumer that starts later is not hit
  with one huge jump.

  Text drawn by the console under the cursor is preserved: before restoring the saved background we compare
  the framebuffer with what we drew, pixel by pixel, and keep any pixel that changed in the meantime. A GOP
  mode change (size) hides the old cursor state and re-clamps the position.

  Copyright (c) winx contributors. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimplePointer.h>

#define CUR_W          12
#define CUR_H          19
#define POLL_100NS     (20 * 10000)      // 20 ms
#define PIXELS_PER_RES 8                 // pixels = RelativeMovement * 8 / ResolutionX (1 count = 1 px for the usual Res = 8)
#define MAX_MICE       8
#define IDLE_TICKS     25                // 500 ms without a consumer poll: drop accumulated movement
#define ACCEL_MAX      24                // |dx|+|dy| per tick at which acceleration saturates
#define ACCEL_STEP     16                // gain = 1.0 + 16/256 per count of speed, saturating at 2.5x

STATIC CONST CHAR8 *mArrow[CUR_H] = {
  "X...........",
  "XX..........",
  "XoX.........",
  "XooX........",
  "XoooX.......",
  "XooooX......",
  "XoooooX.....",
  "XooooooX....",
  "XoooooooX...",
  "XooooooooX..",
  "XoooooXXXXX.",
  "XooXooX.....",
  "XoX.XooX....",
  "XX..XooX....",
  "X....XooX...",
  ".....XooX...",
  "......XooX..",
  "......XooX..",
  ".......XX..."
};

typedef EFI_GRAPHICS_OUTPUT_BLT_PIXEL  PIX;

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop;
STATIC EFI_EVENT                     mTimer;
STATIC INT32                         mX, mY;
STATIC INT32                         mDrawX, mDrawY;
STATIC BOOLEAN                       mActive;     // saw at least one event
STATIC BOOLEAN                       mDrawn;
STATIC UINT32                        mDrawnW, mDrawnH;
STATIC PIX                           mSaved[CUR_W * CUR_H];   // screen content before the cursor was drawn
STATIC PIX                           mComp[CUR_W * CUR_H];    // what we put on the screen
STATIC BOOLEAN                       mLeft, mRight;
STATIC INT32                         mFracX, mFracY;          // sub-pixel remainder, 1/256 pixel
STATIC UINT32                        mModeW, mModeH;
STATIC UINT32                        mTick;

// One record per hooked EFI_SIMPLE_POINTER_PROTOCOL instance.
typedef EFI_STATUS (EFIAPI *GET_STATE)(EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *State);

typedef struct {
  EFI_SIMPLE_POINTER_PROTOCOL  *Sp;
  GET_STATE                    Orig;
  INT32                        PendX, PendY;     // movement not yet handed to a consumer, raw counts
  BOOLEAN                      Left, Right;
  BOOLEAN                      Pending;
  BOOLEAN                      Seen;
  UINT32                       LastConsumerTick;
} MOUSE_REC;

STATIC MOUSE_REC  mRec[MAX_MICE];

STATIC
MOUSE_REC *
RecFind (
  IN EFI_SIMPLE_POINTER_PROTOCOL  *Sp
  )
{
  UINTN  I;

  for (I = 0; I < MAX_MICE; I++) {
    if (mRec[I].Sp == Sp) {
      return &mRec[I];
    }
  }

  return NULL;
}

// Replacement GetState(): hands the accumulated movement to whoever polls (shell editor, HII, ...).
STATIC
EFI_STATUS
EFIAPI
WrapGetState (
  IN  EFI_SIMPLE_POINTER_PROTOCOL  *This,
  OUT EFI_SIMPLE_POINTER_STATE     *State
  )
{
  MOUSE_REC  *R;
  EFI_TPL    Old;

  R = (This != NULL) ? RecFind (This) : NULL;
  if (R == NULL || State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Old = gBS->RaiseTPL (TPL_NOTIFY);
  R->LastConsumerTick = mTick;
  if (!R->Pending) {
    gBS->RestoreTPL (Old);
    return EFI_NOT_READY;
  }

  State->RelativeMovementX = R->PendX;
  State->RelativeMovementY = R->PendY;
  State->RelativeMovementZ = 0;
  State->LeftButton        = R->Left;
  State->RightButton       = R->Right;
  R->PendX    = 0;
  R->PendY    = 0;
  R->Pending  = FALSE;
  gBS->RestoreTPL (Old);
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
SamePix (
  IN CONST PIX  *A,
  IN CONST PIX  *B
  )
{
  return A->Red == B->Red && A->Green == B->Green && A->Blue == B->Blue;
}

// Remove the cursor, but keep pixels that somebody else painted since we drew it.
STATIC
VOID
CursorErase (
  VOID
  )
{
  PIX     Cur[CUR_W * CUR_H];
  UINT32  N, I;

  if (!mDrawn) {
    return;
  }

  N = mDrawnW * mDrawnH;
  if (EFI_ERROR (mGop->Blt (mGop, Cur, EfiBltVideoToBltBuffer, mDrawX, mDrawY, 0, 0, mDrawnW, mDrawnH, 0))) {
    mDrawn = FALSE;
    return;
  }

  for (I = 0; I < N; I++) {
    if (SamePix (&Cur[I], &mComp[I])) {
      Cur[I] = mSaved[I];
    }
  }

  mGop->Blt (mGop, Cur, EfiBltBufferToVideo, 0, 0, mDrawX, mDrawY, mDrawnW, mDrawnH, 0);
  mDrawn = FALSE;
}

STATIC
VOID
CursorDraw (
  VOID
  )
{
  UINT32  W, H, X, Y;
  PIX     Fill;

  W = mGop->Mode->Info->HorizontalResolution;
  H = mGop->Mode->Info->VerticalResolution;
  if (mX < 0) {
    mX = 0;
  }

  if (mY < 0) {
    mY = 0;
  }

  if (mX >= (INT32)W) {
    mX = (INT32)W - 1;
  }

  if (mY >= (INT32)H) {
    mY = (INT32)H - 1;
  }

  mDrawX  = mX;
  mDrawY  = mY;
  mDrawnW = (UINT32)mX + CUR_W <= W ? CUR_W : W - (UINT32)mX;
  mDrawnH = (UINT32)mY + CUR_H <= H ? CUR_H : H - (UINT32)mY;

  if (EFI_ERROR (mGop->Blt (mGop, mSaved, EfiBltVideoToBltBuffer, mDrawX, mDrawY, 0, 0, mDrawnW, mDrawnH, 0))) {
    return;
  }

  // Pitch of mSaved/mComp is mDrawnW (Delta = 0 -> tightly packed), so index with mDrawnW.
  CopyMem (mComp, mSaved, mDrawnW * mDrawnH * sizeof (PIX));

  SetMem (&Fill, sizeof (Fill), 0);
  if (mLeft && mRight) {
    Fill.Red = 255; Fill.Blue = 255;                    // both: magenta
  } else if (mLeft) {
    Fill.Red = 255;                                     // left: red
  } else if (mRight) {
    Fill.Blue = 255;                                    // right: blue
  } else {
    Fill.Red = 255; Fill.Green = 255; Fill.Blue = 255;  // idle: white
  }

  for (Y = 0; Y < mDrawnH; Y++) {
    for (X = 0; X < mDrawnW; X++) {
      CHAR8  C = mArrow[Y][X];
      PIX    *P = &mComp[Y * mDrawnW + X];
      if (C == 'X') {
        P->Red = P->Green = P->Blue = 0;
      } else if (C == 'o') {
        *P = Fill;
        P->Reserved = 0;
      }
    }
  }

  if (!EFI_ERROR (mGop->Blt (mGop, mComp, EfiBltBufferToVideo, 0, 0, mDrawX, mDrawY, mDrawnW, mDrawnH, 0))) {
    mDrawn = TRUE;
  }
}

// Pointer acceleration: 1.0x at rest, rising to 2.5x for fast flicks. Returns the movement in 1/256 pixel.
STATIC
INT32
Accel (
  IN INT32   Counts,
  IN UINT32  Speed,
  IN UINT64  Res
  )
{
  INT64  Gain, V;

  if (Speed > ACCEL_MAX) {
    Speed = ACCEL_MAX;
  }

  Gain = 256 + (INT64)Speed * ACCEL_STEP;
  V    = (INT64)Counts * PIXELS_PER_RES * Gain / (INT64)Res;     // counts -> pixels * 256
  return (INT32)V;
}

STATIC
VOID
EFIAPI
PollMouse (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_HANDLE                   *Handles;
  UINTN                        Count, I;
  EFI_SIMPLE_POINTER_PROTOCOL  *Sp;
  EFI_SIMPLE_POINTER_STATE     St;
  MOUSE_REC                    *R;
  BOOLEAN                      Changed;
  BOOLEAN                      L, R2;
  EFI_TPL                      Old;
  INT32                        Dx, Dy;
  UINT32                       Speed;
  UINT64                       ResX, ResY;

  mTick++;

  if (mGop == NULL) {
    if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop)) ||
        mGop->Mode == NULL || mGop->Mode->Info == NULL) {
      mGop = NULL;
      return;
    }
  }

  // Video mode changed (or first call): forget what we drew, the framebuffer content is gone anyway.
  if (mModeW != mGop->Mode->Info->HorizontalResolution || mModeH != mGop->Mode->Info->VerticalResolution) {
    mModeW = mGop->Mode->Info->HorizontalResolution;
    mModeH = mGop->Mode->Info->VerticalResolution;
    mDrawn = FALSE;
    if (mActive) {
      mX = (INT32)(mModeW / 2);
      mY = (INT32)(mModeH / 2);
    }
  }

  Handles = NULL;
  Count   = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimplePointerProtocolGuid, NULL, &Count, &Handles))) {
    Count = 0;
  }

  // Hook new instances, forget the ones that went away.
  for (I = 0; I < MAX_MICE; I++) {
    mRec[I].Seen = FALSE;
  }

  for (I = 0; I < Count; I++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiSimplePointerProtocolGuid, (VOID **)&Sp))) {
      continue;
    }

    R = RecFind (Sp);
    if (R == NULL) {
      R = RecFind (NULL);
      if (R == NULL) {
        continue;                                 // more than MAX_MICE: leave the rest alone
      }

      if (Sp->GetState == WrapGetState) {
        continue;                                 // already hooked by us but the record was lost: cannot recover Orig
      }

      ZeroMem (R, sizeof (*R));
      R->Sp               = Sp;
      R->Orig             = Sp->GetState;
      R->LastConsumerTick = mTick;
      Sp->GetState        = WrapGetState;
    }

    R->Seen = TRUE;
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  for (I = 0; I < MAX_MICE; I++) {
    if (mRec[I].Sp != NULL && !mRec[I].Seen) {
      ZeroMem (&mRec[I], sizeof (mRec[I]));       // device gone; its protocol memory may already be freed
    }
  }

  Changed = FALSE;
  L       = mLeft;
  R2      = mRight;
  for (I = 0; I < MAX_MICE; I++) {
    R = &mRec[I];
    if (R->Sp == NULL) {
      continue;
    }

    if (EFI_ERROR (R->Orig (R->Sp, &St))) {
      // EFI_NOT_READY: nothing new. Drop stale movement nobody asked for.
      if (R->Pending && (mTick - R->LastConsumerTick) > IDLE_TICKS) {
        Old = gBS->RaiseTPL (TPL_NOTIFY);
        R->PendX = 0;
        R->PendY = 0;
        gBS->RestoreTPL (Old);
      }

      continue;
    }

    // Share the event with consumers (accumulate; buttons are levels, movement is a sum).
    Old = gBS->RaiseTPL (TPL_NOTIFY);
    if ((mTick - R->LastConsumerTick) > IDLE_TICKS) {
      R->PendX = 0;                               // nobody is listening: do not build up a huge jump
      R->PendY = 0;
    }

    R->PendX  += St.RelativeMovementX;
    R->PendY  += St.RelativeMovementY;
    R->Left    = St.LeftButton;
    R->Right   = St.RightButton;
    R->Pending = TRUE;
    gBS->RestoreTPL (Old);

    ResX = (R->Sp->Mode != NULL && R->Sp->Mode->ResolutionX != 0) ? R->Sp->Mode->ResolutionX : PIXELS_PER_RES;
    ResY = (R->Sp->Mode != NULL && R->Sp->Mode->ResolutionY != 0) ? R->Sp->Mode->ResolutionY : PIXELS_PER_RES;

    if (!mActive) {
      // First event: start in the middle of the screen.
      mX = (INT32)(mModeW / 2);
      mY = (INT32)(mModeH / 2);
    }

    Speed = (UINT32)((St.RelativeMovementX < 0 ? -St.RelativeMovementX : St.RelativeMovementX) +
                     (St.RelativeMovementY < 0 ? -St.RelativeMovementY : St.RelativeMovementY));
    Dx = Accel (St.RelativeMovementX, Speed, ResX) + mFracX;
    Dy = Accel (St.RelativeMovementY, Speed, ResY) + mFracY;

    mActive = TRUE;
    mX     += Dx / 256;                           // C division truncates toward zero, the remainder keeps its sign
    mY     += Dy / 256;
    mFracX  = Dx % 256;
    mFracY  = Dy % 256;
    L       = St.LeftButton;
    R2      = St.RightButton;
    Changed = TRUE;
  }

  if (!mActive) {
    return;
  }

  if (Changed) {
    mLeft  = L;
    mRight = R2;
    CursorErase ();
    CursorDraw ();
    // Position clamped at an edge: do not keep integrating beyond it.
    if (mX <= 0 || mX >= (INT32)mModeW - 1) {
      mFracX = 0;
    }

    if (mY <= 0 || mY >= (INT32)mModeH - 1) {
      mFracY = 0;
    }
  }
}

EFI_STATUS
EFIAPI
MouseCursorEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, PollMouse, NULL, &mTimer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gBS->SetTimer (mTimer, TimerPeriodic, POLL_100NS);
}
