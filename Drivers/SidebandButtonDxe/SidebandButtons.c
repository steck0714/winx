// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Device/T210.h>
#include <Foundation/Types.h>
#include <Library/GpioLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Pmic.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/Utc/Tegra210/PinMux.h>

#include "SwitchSidebandButtons.h"

#define BUTTON_POLL_INTERVAL_100NS  (20 * 10000) // 20 ms
#define BUTTON_DEBOUNCE_SAMPLES     2
#define BUTTON_KEY_QUEUE_SIZE      8
#define BUTTON_PINMUX_INPUT_ENABLE  BIT6
#define BUTTON_PINMUX_PULL_UP       (2U << 2)

STATIC EFI_EVENT                    mPollTimer;
STATIC EFI_EVENT                    mButtonChangeEvent;
STATIC EFI_HANDLE                   mProtocolHandle;
STATIC PMIC_PROTOCOL                *mPmicProtocol;
STATIC WINX_SWITCH_BUTTON_PROTOCOL  mButtonProtocol;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mKeyboard;
STATIC WINX_SWITCH_BUTTON_DEVICE_PATH mDevicePath = WINX_SWITCH_BUTTON_DEVICE_PATH_INIT;
STATIC EFI_INPUT_KEY                mKeyQueue[BUTTON_KEY_QUEUE_SIZE];
STATIC UINTN                        mKeyHead;
STATIC UINTN                        mKeyCount;
STATIC UINT32                       mButtonState;
STATIC UINT8                        mCandidate[3];
STATIC UINT8                        mStable[3];
STATIC UINT8                        mSampleCount[3];

STATIC
VOID
ConfigureVolumeButtonInputs (
  VOID
  )
{
  UINT32  PinmuxValue;

  // Volume keys are active-low PX6/PX7 on T210 and T210B01 Switch boards.
  // Configure these two pads locally so the keyboard bridge also works in WOA
  // and Lite profiles that do not load the full PinMuxDxe table.
  PinmuxValue = BUTTON_PINMUX_INPUT_ENABLE | BUTTON_PINMUX_PULL_UP;
  MmioWrite32 (
    PINMUX_AUX_BASE + (PMUX_PINGRP_BUTTON_VOL_UP_PX6 * sizeof (UINT32)),
    PinmuxValue
    );
  MmioWrite32 (
    PINMUX_AUX_BASE + (PMUX_PINGRP_BUTTON_VOL_DOWN_PX7 * sizeof (UINT32)),
    PinmuxValue
    );
  gpio_config (GPIO_PORT_X, GPIO_PIN_6 | GPIO_PIN_7, GPIO_MODE_GPIO);
  gpio_output_enable (GPIO_PORT_X, GPIO_PIN_6 | GPIO_PIN_7, GPIO_OUTPUT_DISABLE);
}

STATIC
VOID
QueueKey (
  IN UINT16  ScanCode,
  IN CHAR16  UnicodeChar
  )
{
  UINTN  Tail;

  if (mKeyCount == BUTTON_KEY_QUEUE_SIZE) {
    mKeyHead = (mKeyHead + 1) % BUTTON_KEY_QUEUE_SIZE;
    --mKeyCount;
  }

  Tail = (mKeyHead + mKeyCount) % BUTTON_KEY_QUEUE_SIZE;
  mKeyQueue[Tail].ScanCode    = ScanCode;
  mKeyQueue[Tail].UnicodeChar = UnicodeChar;
  ++mKeyCount;
  if (mKeyboard.WaitForKey != NULL) {
    gBS->SignalEvent (mKeyboard.WaitForKey);
  }
}

STATIC
EFI_STATUS
EFIAPI
KeyboardReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  if (This != &mKeyboard) {
    return EFI_INVALID_PARAMETER;
  }

  mKeyHead  = 0;
  mKeyCount = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeyboardReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  if ((This != &mKeyboard) || (Key == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (mKeyCount == 0) {
    return EFI_NOT_READY;
  }

  *Key = mKeyQueue[mKeyHead];
  mKeyHead = (mKeyHead + 1) % BUTTON_KEY_QUEUE_SIZE;
  --mKeyCount;
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
KeyboardWaitForKey (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mKeyCount != 0) {
    gBS->SignalEvent (Event);
  }
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mKeyboard = {
  KeyboardReset,
  KeyboardReadKeyStroke,
  NULL
};

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
  UINT32 NewPresses;

  (VOID)Event;
  (VOID)Context;
  Previous = mButtonState;
  UpdateButton (0, !gpio_read (GPIO_PORT_X, GPIO_PIN_7), WINX_SWITCH_BUTTON_VOLUME_DOWN);
  UpdateButton (1, !gpio_read (GPIO_PORT_X, GPIO_PIN_6), WINX_SWITCH_BUTTON_VOLUME_UP);
  UpdateButton (2, mPmicProtocol->QueryPowerButton (), WINX_SWITCH_BUTTON_POWER);

  if (Previous != mButtonState && mButtonChangeEvent != NULL) {
    gBS->SignalEvent (mButtonChangeEvent);
  }

  NewPresses = mButtonState & ~Previous;
  if ((NewPresses & WINX_SWITCH_BUTTON_VOLUME_UP) != 0) {
    QueueKey (SCAN_UP, 0);
  }
  if ((NewPresses & WINX_SWITCH_BUTTON_VOLUME_DOWN) != 0) {
    QueueKey (SCAN_DOWN, 0);
  }
  if ((NewPresses & WINX_SWITCH_BUTTON_POWER) != 0) {
    QueueKey (0, CHAR_CARRIAGE_RETURN);
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

  ConfigureVolumeButtonInputs ();
  mButtonProtocol.ReadState = ReadButtonState;
  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeyboardWaitForKey,
                             NULL, &mKeyboard.WaitForKey);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (0, TPL_APPLICATION, NULL, NULL, &mButtonChangeEvent);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mKeyboard.WaitForKey);
    mKeyboard.WaitForKey = NULL;
    return Status;
  }

  mButtonProtocol.WaitForChange = mButtonChangeEvent;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mProtocolHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiSimpleTextInProtocolGuid,
                  &mKeyboard,
                  &gWinXSidebandButtonProtocolGuid,
                  &mButtonProtocol,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mButtonChangeEvent);
    gBS->CloseEvent (mKeyboard.WaitForKey);
    mKeyboard.WaitForKey = NULL;
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
    gBS->UninstallMultipleProtocolInterfaces (
           mProtocolHandle,
           &gEfiDevicePathProtocolGuid,
           &mDevicePath,
           &gEfiSimpleTextInProtocolGuid,
           &mKeyboard,
           &gWinXSidebandButtonProtocolGuid,
           &mButtonProtocol,
           NULL
           );
    gBS->CloseEvent (mButtonChangeEvent);
    gBS->CloseEvent (mKeyboard.WaitForKey);
    mKeyboard.WaitForKey = NULL;
  }

  return Status;
}
