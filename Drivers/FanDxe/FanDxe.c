/** @file
  FanDxe - fixed-duty fan setup for the Nintendo Switch (Erista, Tegra210 / "T210").

  STATUS: NOT TESTED ON HARDWARE.
    The register sequence below is a port of hekate's fan driver (bdk/thermal/fan.c,
    bdk/power/regulator_5v.c, bdk/soc/gpio.c; CTCaer, GPLv2, hekate master 45cd81e / v6.5.4).
    hekate's own sequence is known to work on Erista. That this port writes the same registers in
    the same order is checked on the host by Tools/test-fan-trace/: it runs hekate's real sources
    and this file against one fake register file (39 scenarios) and compares; 19 deliberately
    injected bugs are caught, 1 is an equivalent change. That says nothing about the board itself.

  WHY A FIXED DUTY
    Boot Services timers stop at ExitBootServices() and Windows has no driver for this fan, so a
    firmware-side control loop cannot outlive the boot. The only thing firmware can do is leave
    the fan running at a constant duty. Nothing here reads a temperature; pick a value that is
    safe at your worst-case load. The fan is off (hekate's main bootloader never starts it) when
    this module is not built in, and that is the default.

  DUTY SCALE (hekate fan.h): 1 = ~960 RPM ... 235 = ~11000 RPM. For reference hekate's own
    temperature steps are 51 (>=42C), 76 (>=47C) and 102 (>=52C).

  DELIBERATE DIFFERENCES FROM HEKATE
    * The tachometer pad (CAM1_PWDN / GPIO S7) is not configured: nothing reads it.
    * The USB_VBUS_EN0 pad (GPIO CC4) is NOT touched. hekate repurposes it as a plain GPIO; in
      this package PinMuxDxe leaves it on the USB function, which is what makes VBUS available
      to a dock / OTG device under Windows. The 5V source selection is automatic in hardware
      (hekate's comment), so the fan needs only the battery 5V regulator enable (GPIO A5).
    * The two PMC writes are skipped when the bit already has the wanted value.
    * T210 (Erista) only. On anything else (GP_HIDREV major != 1) the module does nothing.

  KNOWN LIMITS
    * No sleep/resume support: PWM and GPIO state would have to be re-applied after resume.
    * The pad/GPIO identities were cross-checked against this package's coreboot-derived pad
      table (Include/Library/PinmuxLib.h): LCD_GPIO2 = V4 with PWM1 as function 1,
      SATA_LED_ACTIVE = A5. The register offsets are hekate's.

  Copyright (c) 2018-2024 CTCaer (hekate fan/regulator/gpio drivers this is ported from)
  Copyright (c) 2018 naehrwert (GpioLib)

  This program is free software; you can redistribute it and/or modify it under the terms and
  conditions of the GNU General Public License, version 2, as published by the Free Software
  Foundation. It is distributed in the hope it will be useful, but WITHOUT ANY WARRANTY.
**/

#include <PiDxe.h>
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Foundation/Types.h>
#include <Device/T210.h>
#include <Library/GpioLib.h>

// ---- registers (hekate bdk/soc/t210.h, pinmux.h, pmc_t210.h, clock.h) ---------------------

#define PWM_BASE                       0x7000A000
#define PWM(off)                       _REG (PWM_BASE, off)
#define PWM_CONTROLLER_PWM_CSR_1       0x10
#define PWM_CSR_EN                     (1U << 31)
#define PWM_CSR_PULSE_SHIFT            16

#define APB_MISC_GP_HIDREV             0x804
#define GP_HIDREV_MAJOR_T210           0x1

#define PINMUX_AUX_SATA_LED_ACTIVE     0x4C     // pad of GPIO A5: battery 5V regulator enable
#define PINMUX_AUX_LCD_GPIO2           0x20C    // pad of GPIO V4: fan PWM (function 1 = PWM1)

#define APBDEV_PMC_NO_IOPOWER          0x44
#define APBDEV_PMC_PWR_DET_VAL         0xE4
#define PMC_GPIO_BIT                   (1U << 21)

#define CLK_RST_RST_DEVICES_L          0x004
#define CLK_RST_CLK_OUT_ENB_L          0x010
#define CLK_RST_SOURCE_PWM             0x110
#define CLK_RST_RST_DEV_L_SET          0x300
#define CLK_RST_RST_DEV_L_CLR          0x304
#define CLK_RST_CLK_ENB_L_SET          0x320
#define CLK_RST_CLK_ENB_L_CLR          0x324
#define CLK_L_PWM_BIT                  (1U << 17)
#define PWM_CLK_SOURCE_CLK_M_DIV3      ((6U << 29) | 4U)   // hekate: src 6 (CLK_M), CLK_SRC_DIV(3)

// ---- fan constants (hekate fan.c) ---------------------------------------------------------

#define FAN_DUTY_SCALE                 236      // inverted polarity: pulse = 236 - duty
#define FAN_DUTY_MAX                   235
#define FAN_PWM_PARK                   0x100    // "absolute 0%": the fan-off level

/**
  Is the PWM block clocked and out of reset? hekate's display code enables it for the backlight
  (display_backlight_pwm_init -> clock_enable_pwm), so this is normally already true. The
  backlight PWM (CSR_0) shares the block: it must never be reset while it is running.
**/
STATIC
BOOLEAN
FanPwmClockRunning (
  VOID
  )
{
  return ((CLOCK (CLK_RST_CLK_OUT_ENB_L) & CLK_L_PWM_BIT) != 0) &&
         ((CLOCK (CLK_RST_RST_DEVICES_L) & CLK_L_PWM_BIT) == 0);
}

/**
  hekate clock_enable(&_clock_pwm): reset, disable, source CLK_M/3 (6.4 MHz), enable, 2 us, un-reset.
  Only reached when the PWM block is not running at all, so no backlight PWM can be disturbed.
**/
STATIC
VOID
FanPwmClockEnable (
  VOID
  )
{
  CLOCK (CLK_RST_RST_DEV_L_SET) = CLK_L_PWM_BIT;
  CLOCK (CLK_RST_CLK_ENB_L_CLR) = CLK_L_PWM_BIT;
  CLOCK (CLK_RST_SOURCE_PWM)    = PWM_CLK_SOURCE_CLK_M_DIV3;
  CLOCK (CLK_RST_CLK_ENB_L_SET) = CLK_L_PWM_BIT;
  gBS->Stall (2);
  CLOCK (CLK_RST_RST_DEV_L_CLR) = CLK_L_PWM_BIT;
  (VOID) CLOCK (CLK_RST_RST_DEV_L_SET);
}

/**
  The part of hekate regulator_5v_enable() that applies to a battery-powered Erista: enable the
  battery 5V regulator (GPIO A5), then make sure the GPIO pads have IO power at 1.8 V.
**/
STATIC
VOID
FanPower5vOn (
  VOID
  )
{
  UINT32  Value;

  PINMUX_AUX (PINMUX_AUX_SATA_LED_ACTIVE) = 1;

  // gpio_direction_output (A5, HIGH): function first, then the level, then the driver.
  gpio_config        (GPIO_PORT_A, GPIO_PIN_5, GPIO_MODE_GPIO);
  gpio_write         (GPIO_PORT_A, GPIO_PIN_5, GPIO_HIGH);
  gpio_output_enable (GPIO_PORT_A, GPIO_PIN_5, GPIO_OUTPUT_ENABLE);

  // Make sure GPIO IO power is enabled.
  Value = PMC (APBDEV_PMC_NO_IOPOWER);
  if ((Value & PMC_GPIO_BIT) != 0) {
    PMC (APBDEV_PMC_NO_IOPOWER) = Value & ~PMC_GPIO_BIT;
    (VOID) PMC (APBDEV_PMC_NO_IOPOWER);
  }

  // Tell the GPIO IO pads the rail is 1.8 V.
  Value = PMC (APBDEV_PMC_PWR_DET_VAL);
  if ((Value & PMC_GPIO_BIT) != 0) {
    PMC (APBDEV_PMC_PWR_DET_VAL) = Value & ~PMC_GPIO_BIT;
    (VOID) PMC (APBDEV_PMC_PWR_DET_VAL);
  }
}

/**
  hekate fan_set_duty (Duty) for Duty != 0, first call: initialise the PWM output, then power
  the fan and set the duty. Duty must be 1..FAN_DUTY_MAX.
**/
STATIC
VOID
FanApplyDuty (
  IN UINT32  Duty
  )
{
  UINT32  InvDuty;

  InvDuty = FAN_DUTY_SCALE - Duty;                      // inverted polarity

  if (!FanPwmClockRunning ()) {
    FanPwmClockEnable ();
  }

  // Park the PWM at the fan-off level before routing the pad to it.
  PWM (PWM_CONTROLLER_PWM_CSR_1) = PWM_CSR_EN | (FAN_PWM_PARK << PWM_CSR_PULSE_SHIFT);

  PINMUX_AUX (PINMUX_AUX_LCD_GPIO2) = 1;                // source = PWM1
  gpio_config (GPIO_PORT_V, GPIO_PIN_4, GPIO_MODE_SPIO); // fan power mode: pad driven by the PWM block

  FanPower5vOn ();

  PWM (PWM_CONTROLLER_PWM_CSR_1) = PWM_CSR_EN | (InvDuty << PWM_CSR_PULSE_SHIFT);
  PINMUX_AUX (PINMUX_AUX_LCD_GPIO2) = 1;                // enable fan
}

EFI_STATUS
EFIAPI
FanDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Duty;
  UINT32  HidRev;
  UINT32  Csr;

  Duty = FixedPcdGet8 (PcdSwitchFanDuty);
  if (Duty == 0) {
    return EFI_SUCCESS;                                 // not requested
  }
  if (Duty > FAN_DUTY_MAX) {
    Duty = FAN_DUTY_MAX;
  }

  HidRev = APB_MISC (APB_MISC_GP_HIDREV);
  if (((HidRev >> 4) & 0xF) != GP_HIDREV_MAJOR_T210) {
    DEBUG ((DEBUG_WARN, "FanDxe: GP_HIDREV 0x%08x is not Tegra210 (Erista); fan left alone\n", HidRev));
    return EFI_UNSUPPORTED;
  }

  FanApplyDuty (Duty);

  Csr = PWM (PWM_CONTROLLER_PWM_CSR_1);
  DEBUG ((
    DEBUG_INFO,
    "FanDxe: fixed duty %u/%u (~%u%%), PWM_CSR_1=0x%08x (UNTESTED on hardware)\n",
    Duty,
    FAN_DUTY_SCALE,
    (Duty * 100) / FAN_DUTY_SCALE,
    Csr
    ));

  return EFI_SUCCESS;
}
