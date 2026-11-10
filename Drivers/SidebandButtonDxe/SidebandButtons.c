// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Device/StaticDevices.h>
#include <Foundation/Types.h>
#include <Library/GpioLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Pmic.h>

#include "SwitchSidebandButtons.h"

#define BUTTON_POLL_INTERVAL_100NS  (20 * 10000) // 20 ms
#define BUTTON_DEBOUNCE_SAMPLES     2

STATIC EFI_EVENT                    mPollTimer;
STATIC EFI_EVENT                    mButtonChangeEvent;
STATIC EFI_HANDLE                   mProtocolHandle;
STATIC PMIC_PROTOCOL                *mPmicProtocol;
STATIC WINX_SWITCH_BUTTON_PROTOCOL  mButtonProtocol;
STATIC UINT32                       mButtonState;
STATIC UINT8                        mCandidate[3];
STATIC UINT8                        mStable[3];
STATIC UINT8                        mSampleCount[3];

STATIC
EFI_STATUS
EFIAPI
ReadButtonState (
  IN  WINX_SWITCH_BUTTON_PROTOCOL  *This,
  OUT UINT32                       *Pressed
  )
{
  if (This != &mButtonProtocol || Pressed == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Pressed = mButtonState;
  return EFI_SUCCESS;
}

STATIC
VOID
UpdateButton (
  IN UINTN   Index,
  IN BOOLEAN Pressed,
  IN UINT32  Mask
  )
{
  if ((UINT8)Pressed != mCandidate[Index]) {
    mCandidate[Index] = (UINT8)Pressed;
    mSampleCount[Index] = 1;
    return;
  }

  if (mSampleCount[Index] < BUTTON_DEBOUNCE_SAMPLES) {
    ++mSampleCount[Index];
  }

  if (mSampleCount[Index] >= BUTTON_DEBOUNCE_SAMPLES &&
      mStable[Index] != mCandidate[Index]) {
    mStable[Index] = mCandidate[Index];
    if (mStable[Index]) {
      mButtonState |= Mask;
    } else {
      mButtonState &= ~Mask;
    }
  }
}

STATIC
VOID
EFIAPI
SidebandButtonsPoll (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  UINT32 Previous;

  (VOID)Event;
  (VOID)Context;
  Previous = mButtonState;
  UpdateButton (0, !gpio_read (GPIO_BY_NAME (BUTTON_VOL_DOWN)), WINX_SWITCH_BUTTON_VOLUME_DOWN);
  UpdateButton (1, !gpio_read (GPIO_BY_NAME (BUTTON_VOL_UP)), WINX_SWITCH_BUTTON_VOLUME_UP);
  UpdateButton (2, mPmicProtocol->QueryPowerButton (), WINX_SWITCH_BUTTON_POWER);

  if (Previous != mButtonState && mButtonChangeEvent != NULL) {
    gBS->SignalEvent (mButtonChangeEvent);
  }
}

EFI_STATUS
EFIAPI
SidebandButtonDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  Status = gBS->LocateProtocol (&gPmicProtocolGuid, NULL, (VOID **)&mPmicProtocol);
  if (EFI_ERROR (Status) || mPmicProtocol == NULL || mPmicProtocol->QueryPowerButton == NULL) {
    return EFI_NOT_FOUND;
  }

  mButtonProtocol.ReadState = ReadButtonState;
  Status = gBS->CreateEvent (0, TPL_APPLICATION, NULL, NULL, &mButtonChangeEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mButtonProtocol.WaitForChange = mButtonChangeEvent;
  Status = gBS->InstallProtocolInterface (&mProtocolHandle,
                                          &gWinXSidebandButtonProtocolGuid,
                                          EFI_NATIVE_INTERFACE,
                                          &mButtonProtocol);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mButtonChangeEvent);
    return Status;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             SidebandButtonsPoll, NULL, &mPollTimer);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (mPollTimer, TimerPeriodic, BUTTON_POLL_INTERVAL_100NS);
  }

  if (EFI_ERROR (Status)) {
    if (mPollTimer != NULL) {
      gBS->CloseEvent (mPollTimer);
      mPollTimer = NULL;
    }
    gBS->UninstallProtocolInterface (mProtocolHandle,
                                     &gWinXSidebandButtonProtocolGuid,
                                     &mButtonProtocol);
    gBS->CloseEvent (mButtonChangeEvent);
  }

  return Status;
}
