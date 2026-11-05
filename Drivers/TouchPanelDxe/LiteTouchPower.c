// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Foundation/Types.h>
#include <Library/GpioLib.h>
#include <Library/ClockLib.h>
#include <Library/I2cLib.h>
#include <Protocol/Pmic.h>
#include "Fts4.h"

// Hoag's touch_int_px1 is an active-low GPIO input with a pull-up.
#define TOUCH_IRQ_MUX  (PINMUX_AUX_BASE + 0x220)
#define T210_HIDREV    0x70000804UL

EFI_STATUS EFIAPI
LiteTouchPowerInitialize (IN EFI_HANDLE Image, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  PMIC_PROTOCOL *Pmic;

  if ((MmioRead32 (T210_HIDREV) & 0xFFF0) != 0x2120) {
    return EFI_UNSUPPORTED;
  }
  Status = gBS->LocateProtocol (&gPmicProtocolGuid, NULL, (VOID **)&Pmic);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (TOUCH_RST_PINMUX_REG, TOUCH_RST_PINMUX_VAL);
  gpio_write (TOUCH_RST_GPIO_PORT, TOUCH_RST_GPIO_PIN, GPIO_LOW);
  gpio_config (TOUCH_RST_GPIO_PORT, TOUCH_RST_GPIO_PIN, GPIO_MODE_GPIO);
  gpio_output_enable (TOUCH_RST_GPIO_PORT, TOUCH_RST_GPIO_PIN, GPIO_OUTPUT_ENABLE);
  gBS->Stall (20);
  Status = Pmic->SetRegulatorVoltage (REGULATOR_LDO6, 2900000);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Pmic->EnableRegulator (REGULATOR_LDO6);
  MmioWrite32 (PINMUX_AUX_I2C_SCL (I2C_3), PINMUX_INPUT_ENABLE);
  MmioWrite32 (PINMUX_AUX_I2C_SDA (I2C_3), PINMUX_INPUT_ENABLE);
  MmioOr32 (PINMUX_AUX_I2C_SCL (I2C_4), 1);
  MmioOr32 (PINMUX_AUX_I2C_SDA (I2C_4), 1);
  clock_enable_i2c (I2C_3);
  i2c_init (I2C_3);
  MmioWrite32 (TOUCH_IRQ_MUX, PINMUX_INPUT_ENABLE | BIT4 | (2U << 2));
  gpio_output_enable (GPIO_PORT_X, GPIO_PIN_1, GPIO_OUTPUT_DISABLE);
  gpio_config (GPIO_PORT_X, GPIO_PIN_1, GPIO_MODE_GPIO);
  gBS->Stall (1000);
  gpio_write (TOUCH_RST_GPIO_PORT, TOUCH_RST_GPIO_PIN, GPIO_HIGH);
  gBS->Stall (10000);
  // Windows owns the controller FIFO, interrupt and HID reports from this point.
  return EFI_SUCCESS;
}
