/*
 * TouchPanelDxe: STMicroelectronics FingerTip (FTS4 / "ftm4" in L4T) touch controller of the
 * Nintendo Switch, on I2C3 @ 0x49, exposed as EFI_ABSOLUTE_POINTER_PROTOCOL.
 *
 * Power-up / init sequence and event decoding follow hekate's touch.c (GPL-2.0) which is known to
 * work on retail hardware, and the Linux ftm4 driver command set.
 */
#include <PiDxe.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimplePointer.h>

#include <Foundation/Types.h>
#include <Library/I2cLib.h>
#include <Library/GpioLib.h>
#include <Library/ClockLib.h>
#include <Library/Max7762xLib.h>
#include <Protocol/Pmic.h>
#include <Device/TouchPanel.h>

#include "Fts4.h"

#define T210_HIDREV      0x70000804UL
#define TOUCH_IRQ_MUX    (PINMUX_AUX_BASE + 0x220)

typedef struct {
  UINT8   Raw[FTS4_EVENT_SIZE];
  UINT32  X, Y, Z;
  BOOLEAN Touch;
} TOUCH_EVENT;

STATIC PMIC_PROTOCOL                 *mPmic;
STATIC EFI_EVENT                      mPollTimer;
STATIC EFI_HANDLE                     mHandle;
STATIC BOOLEAN                        mClone;          // non-genuine IC: must use READ_ONE_EVENT
STATIC BOOLEAN                        mStateChanged;
STATIC EFI_ABSOLUTE_POINTER_STATE     mState;
STATIC EFI_SIMPLE_POINTER_STATE       mMouseState;
STATIC BOOLEAN                        mMouseStateChanged;
STATIC BOOLEAN                        mTouchActive;
STATIC UINT32                         mPreviousMouseX;
STATIC UINT32                         mPreviousMouseY;
STATIC EFI_EVENT                      mMouseWaitForInput;
STATIC TOUCH_DEVICE_PATH              mDevicePath = TOUCH_DEVICE_PATH_INIT;

STATIC EFI_ABSOLUTE_POINTER_MODE      mMode = {
  0, 0, 0,                                  // AbsoluteMin X/Y/Z
  TOUCH_RES_X - 1, TOUCH_RES_Y - 1, 0,      // AbsoluteMax X/Y/Z (no pressure axis)
  0                                         // Attributes
};

STATIC EFI_SIMPLE_POINTER_MODE        mMouseMode = {
  8, 8, 0, TRUE, FALSE
};

STATIC EFI_SIMPLE_POINTER_PROTOCOL    mMousePtr;

// ---------------------------------------------------------------------------------------------
// Low level I2C helpers. The I2cLib convention: 1 = success, 0 = failure.
// ---------------------------------------------------------------------------------------------
STATIC
BOOLEAN
TouchCmd (UINT8 Cmd, UINT8 *Buf, UINT32 Size)
{
  return i2c_send_buf_small (I2C_3, FTS4_I2C_ADDR, Cmd, Buf, Size) != 0;
}

STATIC
BOOLEAN
TouchReadReg (UINT8 *Cmd, UINT32 CSize, UINT8 *Buf, UINT32 Size)
{
  return i2c_xfer_packet (I2C_3, FTS4_I2C_ADDR, Cmd, CSize, Buf, Size) != 0;
}

STATIC
UINT32
TouchEventCount (VOID)
{
  UINT8 Cmd[3] = { FTS4_CMD_HW_REG_READ, 0, FTS4_HW_REG_EVENT_COUNT };
  UINT8 Buf[2];

  if (!TouchReadReg (Cmd, sizeof (Cmd), Buf, sizeof (Buf))) {
    return 0;
  }
  return Buf[1] >> 1;
}

STATIC
BOOLEAN
TouchWaitEvent (UINT8 Event, UINT8 Status, UINT32 TimeoutMs, UINT8 *Out)
{
  UINT32 Loops = TimeoutMs * 2;   // 500 us per iteration
  UINT8  Tmp[FTS4_EVENT_SIZE];

  for (;;) {
    if (TouchEventCount () &&
        i2c_recv_buf_big (Tmp, FTS4_EVENT_SIZE, I2C_3, FTS4_I2C_ADDR, FTS4_CMD_READ_ONE_EVENT) &&
        Tmp[0] == Event && Tmp[1] == Status) {
      if (Out != NULL) {
        CopyMem (Out, &Tmp[2], 6);
      }
      return TRUE;
    }
    if (Loops-- == 0) {
      return FALSE;
    }
    gBS->Stall (500);
  }
}

// ---------------------------------------------------------------------------------------------
// Controller bring-up
// ---------------------------------------------------------------------------------------------
STATIC
BOOLEAN
TouchGetChipInfo (UINT16 *ChipId)
{
  UINT8 Cmd[3] = { FTS4_CMD_HW_REG_READ, 0, FTS4_HW_REG_CHIP_ID_INFO };
  UINT8 Buf[7] = { 0 };

  if (!TouchReadReg (Cmd, sizeof (Cmd), Buf, sizeof (Buf))) {
    return FALSE;
  }
  *ChipId = (UINT16)((Buf[1] << 8) | Buf[2]);

  // Genuine ICs mirror the chip id at register 2; clones do not.
  Cmd[2] = 2;
  TouchReadReg (Cmd, sizeof (Cmd), Buf, sizeof (Buf));
  mClone = (BOOLEAN)(*ChipId != (UINT16)((Buf[3] << 8) | Buf[4]));
  return TRUE;
}

STATIC
BOOLEAN
TouchSysReset (VOID)
{
  UINT8 Cmd[3] = { 0, FTS4_HW_REG_SYS_RESET, 0x80 };
  UINTN Retry;

  for (Retry = 0; Retry < 3; Retry++) {
    if (!TouchCmd (FTS4_CMD_HW_REG_WRITE, Cmd, 3)) {
      gBS->Stall (10000);
      continue;
    }
    gBS->Stall (10000);
    if (TouchWaitEvent (FTS4_EV_CONTROLLER_READY, 0, 20, NULL)) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC
BOOLEAN
TouchSenseEnable (VOID)
{
  UINT8 Mode = FTS4_FINGER_MODE;

  return TouchCmd (FTS4_CMD_SWITCH_SENSE_MODE, &Mode, 1) &&
         TouchCmd (FTS4_CMD_MS_MT_SENSE_ON, NULL, 0)     &&
         TouchCmd (FTS4_CMD_CLEAR_EVENT_STACK, NULL, 0);
}

STATIC
BOOLEAN
TouchInitChip (VOID)
{
  UINT16 ChipId = 0;

  if (!TouchGetChipInfo (&ChipId) || ChipId != FTS4_I2C_CHIP_ID) {
    DEBUG ((DEBUG_WARN, "Touch: unexpected chip id 0x%04x\n", ChipId));
    return FALSE;
  }
  return TouchSysReset () && TouchSenseEnable ();
}

STATIC
EFI_STATUS
TouchPowerOn (VOID)
{
  UINTN Retry;

  // Touch reset line: low while powering up.
  MmioWrite32 (TOUCH_RST_PINMUX_REG, TOUCH_RST_PINMUX_VAL);
  gpio_config (TOUCH_RST_GPIO_PORT, TOUCH_RST_GPIO_PIN, GPIO_MODE_GPIO);
  gpio_output_enable (TOUCH_RST_GPIO_PORT, TOUCH_RST_GPIO_PIN, GPIO_OUTPUT_ENABLE);
  gpio_write (TOUCH_RST_GPIO_PORT, TOUCH_RST_GPIO_PIN, GPIO_LOW);
  gBS->Stall (20);

  // LDO6 = touch AVDD/DVDD (2.9 V).
  mPmic->SetRegulatorVoltage (REGULATOR_LDO6, 2900000);
  mPmic->EnableRegulator (REGULATOR_LDO6);

  // I2C3 pads (and detach the I2C4 pads from the I2C3 function, as hekate does).
  MmioWrite32 (PINMUX_AUX_I2C_SCL (I2C_3), PINMUX_INPUT_ENABLE);
  MmioWrite32 (PINMUX_AUX_I2C_SDA (I2C_3), PINMUX_INPUT_ENABLE);
  MmioOr32    (PINMUX_AUX_I2C_SCL (I2C_4), 1);
  MmioOr32    (PINMUX_AUX_I2C_SDA (I2C_4), 1);
  clock_enable_i2c (I2C_3);
  i2c_init (I2C_3);
  gBS->Stall (1000);

#ifdef SWITCH_LITE_TOUCH
  // Hoag uses PX1 as the active-low FTM4 interrupt input. Keep the pin ready for
  // the Windows HID driver after ExitBootServices; firmware polls the FIFO below.
  MmioWrite32 (TOUCH_IRQ_MUX, PINMUX_INPUT_ENABLE | BIT4 | (2U << 2));
  gpio_output_enable (GPIO_PORT_X, GPIO_PIN_1, GPIO_OUTPUT_DISABLE);
  gpio_config (GPIO_PORT_X, GPIO_PIN_1, GPIO_MODE_GPIO);
#endif

  // Release reset and wait for the controller.
  gpio_write (TOUCH_RST_GPIO_PORT, TOUCH_RST_GPIO_PIN, GPIO_HIGH);
  gBS->Stall (10000);
  TouchWaitEvent (FTS4_EV_CONTROLLER_READY, 0, 20, NULL);

  for (Retry = 0; Retry < 3; Retry++) {
    if (TouchInitChip ()) {
      return EFI_SUCCESS;
    }
  }

  gpio_write (TOUCH_RST_GPIO_PORT, TOUCH_RST_GPIO_PIN, GPIO_LOW);
  mPmic->DisableRegulator (REGULATOR_LDO6);
  return EFI_NOT_FOUND;
}

// ---------------------------------------------------------------------------------------------
// Event decoding (raw FTS4 panel coordinates -> 1280x720)
// ---------------------------------------------------------------------------------------------
STATIC
UINT32
TouchScale (UINT32 Raw, UINT32 RawMax, UINT32 Res)
{
  if (Raw < FTS4_EDGE_OFFSET) Raw = FTS4_EDGE_OFFSET;
  if (Raw > RawMax)           Raw = RawMax;
  Raw -= FTS4_EDGE_OFFSET;
  return (Raw * ((Res * 1000) / (RawMax - FTS4_EDGE_OFFSET))) / 1000;
}

STATIC
BOOLEAN
TouchParseEvent (TOUCH_EVENT *Ev)
{
  UINT32 Tmp;

  switch (Ev->Raw[0] & FTS4_MASK_EVENT_ID) {
  case FTS4_EV_MULTI_TOUCH_ENTER:
  case FTS4_EV_MULTI_TOUCH_MOTION:
    Ev->X = TouchScale ((Ev->Raw[1] << 4) | ((Ev->Raw[3] & FTS4_MASK_Y_LSB) >> 4),
                        FTS4_X_REAL_MAX, TOUCH_RES_X);
    Ev->Y = TouchScale ((Ev->Raw[2] << 4) | (Ev->Raw[3] & FTS4_MASK_X_MSB),
                        FTS4_Y_REAL_MAX, TOUCH_RES_Y);

    // Contact "size" -> palm rejection (same heuristic as hekate).
    Ev->Z = (Ev->Raw[4] | (Ev->Raw[5] << 8)) << 6;
    Tmp   = 0x40;
    if ((Ev->Raw[6] & 0x3F) != 1 && (Ev->Raw[6] & 0x3F) != 0x3F) {
      Tmp = Ev->Raw[6] & 0x3F;
    }
    Ev->Z /= Tmp + 0x40;
    Ev->Touch = (BOOLEAN)(Ev->Z < 500);
    return TRUE;

  case FTS4_EV_MULTI_TOUCH_LEAVE:
    Ev->Touch = FALSE;       // keep last X/Y
    return TRUE;

  default:
    return FALSE;
  }
}

STATIC
VOID
EFIAPI
TouchPoll (IN EFI_EVENT Event, IN VOID *Context)
{
  TOUCH_EVENT Ev;
  UINT8       Cmd = mClone ? FTS4_CMD_READ_ONE_EVENT : FTS4_CMD_LATEST_EVENT;

  ZeroMem (&Ev, sizeof (Ev));
  if (!i2c_recv_buf_big (Ev.Raw, FTS4_EVENT_SIZE, I2C_3, FTS4_I2C_ADDR, Cmd)) {
    return;
  }
  if (!TouchParseEvent (&Ev)) {
    return;
  }

  if (!Ev.Touch) {
    Ev.X = mState.CurrentX;
    Ev.Y = mState.CurrentY;
  }
  if (Ev.Touch) {
    mState.CurrentX = Ev.X;
    mState.CurrentY = Ev.Y;
  }
  mState.CurrentZ      = 0;
  mState.ActiveButtons = Ev.Touch ? 1 : 0;
  mStateChanged        = TRUE;

  if (mTouchActive != Ev.Touch) {
    mTouchActive = Ev.Touch;
    gBS->SetTimer (
      mPollTimer,
      TimerPeriodic,
      mTouchActive ? TOUCH_POLL_INTERVAL_100NS : (2 * TOUCH_POLL_INTERVAL_100NS)
      );
  }

  // Publish a relative pointer view as well as the absolute touchscreen. This lets EFI
  // applications which only understand SimplePointer use the panel like a one-button mouse.
  {
    INT32  DeltaX;
    INT32  DeltaY;

    DeltaX = (INT32)Ev.X - (INT32)mPreviousMouseX;
    DeltaY = (INT32)Ev.Y - (INT32)mPreviousMouseY;
    if (Ev.Touch || mMouseState.LeftButton) {
      if ((DeltaX > 0) && (mMouseState.RelativeMovementX > MAX_INT32 - DeltaX)) {
        mMouseState.RelativeMovementX = MAX_INT32;
      } else if ((DeltaX < 0) && (mMouseState.RelativeMovementX < MIN_INT32 - DeltaX)) {
        mMouseState.RelativeMovementX = MIN_INT32;
      } else {
        mMouseState.RelativeMovementX += DeltaX;
      }
      if ((DeltaY > 0) && (mMouseState.RelativeMovementY > MAX_INT32 - DeltaY)) {
        mMouseState.RelativeMovementY = MAX_INT32;
      } else if ((DeltaY < 0) && (mMouseState.RelativeMovementY < MIN_INT32 - DeltaY)) {
        mMouseState.RelativeMovementY = MIN_INT32;
      } else {
        mMouseState.RelativeMovementY += DeltaY;
      }
    }
    mPreviousMouseX = Ev.X;
    mPreviousMouseY = Ev.Y;
  }
  mMouseState.LeftButton = Ev.Touch;
  mMouseStateChanged = TRUE;
  if (mMouseWaitForInput != NULL) {
    gBS->SignalEvent (mMouseWaitForInput);
  }
}

// ---------------------------------------------------------------------------------------------
// EFI_ABSOLUTE_POINTER_PROTOCOL
// ---------------------------------------------------------------------------------------------
STATIC
EFI_STATUS
EFIAPI
AbsPtrReset (IN EFI_ABSOLUTE_POINTER_PROTOCOL *This, IN BOOLEAN ExtendedVerification)
{
  ZeroMem (&mState, sizeof (mState));
  mStateChanged = FALSE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MousePtrReset (
  IN EFI_SIMPLE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                      ExtendedVerification
  )
{
  ZeroMem (&mMouseState, sizeof (mMouseState));
  mMouseStateChanged = FALSE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MousePtrGetState (
  IN  EFI_SIMPLE_POINTER_PROTOCOL  *This,
  OUT EFI_SIMPLE_POINTER_STATE     *State
  )
{
  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (!mMouseStateChanged) {
    return EFI_NOT_READY;
  }

  CopyMem (State, &mMouseState, sizeof (*State));
  mMouseState.RelativeMovementX = 0;
  mMouseState.RelativeMovementY = 0;
  mMouseState.RelativeMovementZ = 0;
  mMouseStateChanged = FALSE;
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
MousePtrWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mMouseStateChanged) {
    gBS->SignalEvent (Event);
  }
}

STATIC EFI_SIMPLE_POINTER_PROTOCOL mMousePtr = {
  MousePtrReset,
  MousePtrGetState,
  NULL,
  &mMouseMode
};

STATIC
EFI_STATUS
EFIAPI
AbsPtrGetState (IN EFI_ABSOLUTE_POINTER_PROTOCOL *This, IN OUT EFI_ABSOLUTE_POINTER_STATE *State)
{
  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (!mStateChanged) {
    return EFI_NOT_READY;
  }
  CopyMem (State, &mState, sizeof (*State));
  mStateChanged = FALSE;
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
AbsPtrWaitForInput (IN EFI_EVENT Event, IN VOID *Context)
{
  if (mStateChanged) {
    gBS->SignalEvent (Event);
  }
}

STATIC EFI_ABSOLUTE_POINTER_PROTOCOL mAbsPtr = {
  AbsPtrReset,
  AbsPtrGetState,
  NULL,          // WaitForInput, created in entry point
  &mMode
};

EFI_STATUS
EFIAPI
TouchPanelDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

#ifdef SWITCH_LITE_TOUCH
  if ((MmioRead32 (T210_HIDREV) & 0xFFF0) != 0x2120) {
    return EFI_UNSUPPORTED;
  }
#endif
  Status = gBS->LocateProtocol (&gPmicProtocolGuid, NULL, (VOID **)&mPmic);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = TouchPowerOn ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Touch: controller not found/initialised (%r)\n", Status));
    return Status;
  }

  // MouseCursorDxe starts at the display center when it first receives input.
  // The touch panel and Switch display are both 1280x720 on this profile.
  mPreviousMouseX = TOUCH_RES_X / 2;
  mPreviousMouseY = TOUCH_RES_Y / 2;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, AbsPtrWaitForInput, NULL,
                             &mAbsPtr.WaitForInput);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, MousePtrWaitForInput, NULL,
                             &mMouseWaitForInput);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mAbsPtr.WaitForInput);
    mAbsPtr.WaitForInput = NULL;
    return Status;
  }
  mMousePtr.WaitForInput = mMouseWaitForInput;

  Status = gBS->InstallMultipleProtocolInterfaces (&mHandle,
                  &gEfiDevicePathProtocolGuid,      &mDevicePath,
                  &gEfiAbsolutePointerProtocolGuid, &mAbsPtr,
                  &gEfiSimplePointerProtocolGuid,   &mMousePtr,
                  NULL);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mMouseWaitForInput);
    mMouseWaitForInput = NULL;
    mMousePtr.WaitForInput = NULL;
    gBS->CloseEvent (mAbsPtr.WaitForInput);
    mAbsPtr.WaitForInput = NULL;
    return Status;
  }

  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL | EVT_TIMER, TPL_CALLBACK, TouchPoll, NULL,
                             &mPollTimer);
  if (!EFI_ERROR (Status)) {
    // FTM4 is polled at 20 ms while idle and 10 ms during contact. This keeps
    // touch responsive while halving idle I2C traffic relative to the active rate.
    Status = gBS->SetTimer (mPollTimer, TimerPeriodic, 2 * TOUCH_POLL_INTERVAL_100NS);
  }
  return Status;
}
