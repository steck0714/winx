/** @file

  Copyright (c) 2008 - 2009, Apple Inc. All rights reserved.<BR>
  Copyright (c) 2016, Linaro, Ltd. All rights reserved.<BR>
  Copyright (c) 2019, Bingxing Wang. All rights reserved.<BR>
  Portions ported from hekate (https://github.com/CTCaer/hekate), Copyright (c) 2018-2024 CTCaer,
  GPL-2.0-only: PLLU/UTMIPLL/UTMIP PHY bring-up, bm92t36 I2C packet transfer, VBUS handling.
  Because of those portions this file as a whole is distributed under GPL-2.0-only (see ../../LICENSE
  and ../../NOTICE.winx.md). Modified for winx, 2026.<BR>

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <PiDxe.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/UBootClockManagement.h>
#include <Protocol/Utc/Clock.h>
#include <Protocol/Utc/ClkRst.h>
#include <Protocol/Utc/ErrNo.h>
#include <Protocol/Utc/Usb.h>
#include <Protocol/Utc/Ehci.h>
#include <Protocol/Utc/Tegra.h>

#include <Foundation/Types.h>
#include <Device/T210.h>
#include <Device/Pmc.h>
#include <Library/GpioLib.h>
#include <Library/I2cLib.h>
#include <Guid/EventGroup.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/Cpu.h>
#include <Protocol/DebugSupport.h>
#include <Protocol/Pmic.h>
#include <Shim/DebugLib.h>
#include <Shim/UBootIo.h>
#include <Shim/TimerLib.h>
#include <Shim/BitOps.h>

#include "TegraUsb.h"

#define HOST_CONTROLLER_OPERATION_REG_SIZE  0x44
TEGRA210_UBOOT_CLOCK_MANAGEMENT_PROTOCOL* mClkProtocol;
FDT_USB mPriv;

/*
 * Tegra210 USB1 (USBD / UTMIP) bring-up.
 *
 * The register sequence below is ported from hekate (CTCaer, GPLv2):
 * bdk/usb/usbd.c _usb_init_phy() and bdk/soc/clock.c clock_enable_pllu() /
 * clock_enable_utmipll(). It is Erista-only (38.4 MHz oscillator).
 *
 * Why this replaces the former U-Boot derived sequence: that code touched the
 * USB controller registers *before* PLLU was running and never started UTMIPLL.
 * When booted through RCM + coreboot the bootrom had already left both PLLs
 * running, so it got away with it. hekate stops PLLU/UTMIPLL before it hands
 * over to BL33, and touching the USB1 block without PLLU stalls the CPU.
 * Every wait below is bounded.
 */
#define CLK_RST_BASE                    0x60006000UL
#define CLK_RST(o)                      (CLK_RST_BASE + (o))
#define CLK_RST_PLLU_BASE               0x0C0
#define CLK_RST_PLLU_MISC               0x0CC
#define CLK_RST_CLK_ENB_Y_SET           0x29C
#define CLK_RST_CLK_ENB_Y_CLR           0x2A0
#define CLK_RST_RST_DEV_L_SET           0x300
#define CLK_RST_RST_DEV_L_CLR           0x304
#define CLK_RST_CLK_ENB_L_SET           0x320
#define CLK_RST_CLK_ENB_H_SET           0x328
#define CLK_RST_RST_DEV_W_CLR           0x43C
#define CLK_RST_UTMIP_PLL_CFG0          0x480
#define CLK_RST_UTMIP_PLL_CFG1          0x484
#define CLK_RST_UTMIP_PLL_CFG2          0x488
#define CLK_RST_UTMIPLL_HW_PWRDN_CFG0   0x52C
#define CLK_RST_CLK_SOURCE_USB2_HSIC_TRK 0x6CC

#define PLL_BASE_LOCK                   BIT27
#define PLL_BASE_ENABLE                 BIT30
#define UTMIPLL_LOCK                    BIT31

#define CLK_H_FUSE                      7
#define CLK_L_USBD                      22
#define CLK_W_XUSB_PADCTL               14
#define CLK_Y_USB2_TRK                  18

#define USB1_BASE_ADDR                  0x7D000000UL
#define USB1_REG(o)                     (USB1_BASE_ADDR + (o))
#define USB1_USBCMD                     0x130
#define USB1_HOSTPC1_DEVLC              0x1B4
#define USB1_USBMODE                    0x1F8
#define USB1_SUSP_CTRL                  0x400
#define USB1_PHY_VBUS_SENSORS           0x404
#define USB1_UTMIP_XCVR_CFG0            0x808
#define USB1_UTMIP_BIAS_CFG0            0x80C
#define USB1_UTMIP_HSRX_CFG0            0x810
#define USB1_UTMIP_HSRX_CFG1            0x814
#define USB1_UTMIP_TX_CFG0              0x820
#define USB1_UTMIP_MISC_CFG1            0x828
#define USB1_UTMIP_DEBOUNCE_CFG0        0x82C
#define USB1_UTMIP_SPARE_CFG0           0x834
#define USB1_UTMIP_XCVR_CFG1            0x838
#define USB1_UTMIP_BIAS_CFG1            0x83C
#define USB1_UTMIP_BIAS_CFG2            0x850
#define USB1_UTMIP_XCVR_CFG2            0x854
#define USB1_UTMIP_XCVR_CFG3            0x858

#define SUSP_CTRL_USB_PHY_CLK_VALID     BIT7
#define SUSP_CTRL_UTMIP_RESET           BIT11
#define SUSP_CTRL_UTMIP_PHY_ENB         BIT12

#define USBCMD_RUN                      BIT0
#define USBCMD_RESET                    BIT1
#define USBMODE_CM_MASK                 0x3
#define USBMODE_CM_HOST                 0x3

#define FUSE_BASE_ADDR                  0x7000F800UL
#define FUSE_USB_CALIB                  0x1F0
#define FUSE_USB_CALIB_EXT              0x350
#define PMC_BASE_ADDR                   0x7000E400UL
#define PMC_USB_AO                      0xF0

#define USB_POLL_RETRIES                100000

STATIC
UINT32
ClkRd (UINTN Off)
{
  return MmioRead32 (CLK_RST (Off));
}

STATIC
VOID
ClkWr (UINTN Off, UINT32 Val)
{
  MmioWrite32 (CLK_RST (Off), Val);
}

STATIC
UINT32
UsbRd (UINTN Off)
{
  return MmioRead32 (USB1_REG (Off));
}

STATIC
VOID
UsbWr (UINTN Off, UINT32 Val)
{
  MmioWrite32 (USB1_REG (Off), Val);
}

STATIC
VOID
UsbAnd (UINTN Off, UINT32 Mask)
{
  UsbWr (Off, UsbRd (Off) & Mask);
}

STATIC
VOID
UsbOr (UINTN Off, UINT32 Bits)
{
  UsbWr (Off, UsbRd (Off) | Bits);
}

//
// hekate clock_enable_pllu(): 38.4 MHz / 2 * 25 = 480 MHz. Bounded lock wait.
// Does not touch the USB block, so it is safe with PLLU stopped.
//
STATIC
VOID
UsbEnablePllu (VOID)
{
  UINT32 Cfg;
  UINT32 Retries;

  ClkWr (CLK_RST_PLLU_MISC, ClkRd (CLK_RST_PLLU_MISC) | BIT29);
  Cfg = (ClkRd (CLK_RST_PLLU_BASE) & 0xFFE00000) | BIT24 | (1 << 16) | (0x19 << 8) | 2;
  ClkWr (CLK_RST_PLLU_BASE, Cfg);
  ClkWr (CLK_RST_PLLU_BASE, Cfg | PLL_BASE_ENABLE);

  for (Retries = 1000; Retries != 0; Retries--) {
    if ((ClkRd (CLK_RST_PLLU_BASE) & PLL_BASE_LOCK) != 0) {
      break;
    }

    udelay (1);
  }

  if (Retries == 0) {
    DEBUG ((DEBUG_ERROR, "USB: PLLU did not lock\n"));
  }

  udelay (8);

  // Enable PLLU USB/HSIC/ICUSB/48M outputs.
  ClkWr (CLK_RST_PLLU_BASE, ClkRd (CLK_RST_PLLU_BASE) | 0x2E00000);
}

//
// hekate clock_enable_utmipll(): 38.4 MHz * 25 / 1 = 960 MHz.
//
STATIC
VOID
UsbEnableUtmipll (VOID)
{
  UINT32 Retries;

  ClkWr (CLK_RST_UTMIP_PLL_CFG0, (ClkRd (CLK_RST_UTMIP_PLL_CFG0) & 0xFF0000FF) | (25 << 16) | (1 << 8));
  ClkWr (CLK_RST_UTMIP_PLL_CFG2, (ClkRd (CLK_RST_UTMIP_PLL_CFG2) & 0xFF00003F) | (24 << 18));
  ClkWr (CLK_RST_UTMIP_PLL_CFG1, (ClkRd (CLK_RST_UTMIP_PLL_CFG1) & 0x7FFA000) | (1 << 15) | 375);

  for (Retries = 100; Retries != 0; Retries--) {
    if ((ClkRd (CLK_RST_UTMIPLL_HW_PWRDN_CFG0) & UTMIPLL_LOCK) != 0) {
      break;
    }

    udelay (1);
  }

  if (Retries == 0) {
    DEBUG ((DEBUG_ERROR, "USB: UTMIPLL did not lock\n"));
  }
}

STATIC
BOOLEAN
UsbWaitPhyClock (VOID)
{
  UINT32 Retries;

  for (Retries = USB_POLL_RETRIES; Retries != 0; Retries--) {
    if ((UsbRd (USB1_SUSP_CTRL) & SUSP_CTRL_USB_PHY_CLK_VALID) != 0) {
      return TRUE;
    }

    udelay (1);
  }

  return FALSE;
}

//
// hekate _usb_init_phy().
//
STATIC
VOID
UsbInitPhy (VOID)
{
  UINT32 SuspCtrl;
  UINT32 FuseCalib;

  // PLLU first: the USB1 register block is not accessible without it.
  UsbEnablePllu ();

  // FUSE clock (needed to read the USB calibration fuses below).
  ClkWr (CLK_RST_CLK_ENB_H_SET, 1U << CLK_H_FUSE);

  // Enable USBD clock and pulse its reset.
  ClkWr (CLK_RST_CLK_ENB_L_SET, 1U << CLK_L_USBD);
  udelay (2);
  ClkWr (CLK_RST_RST_DEV_L_SET, 1U << CLK_L_USBD);
  udelay (2);
  ClkWr (CLK_RST_RST_DEV_L_CLR, 1U << CLK_L_USBD);
  udelay (2);

  // Clear XUSB_PADCTL reset.
  ClkWr (CLK_RST_RST_DEV_W_CLR, 1U << CLK_W_XUSB_PADCTL);

  // Enable USB PHY and reset for programming.
  SuspCtrl = UsbRd (USB1_SUSP_CTRL);
  UsbWr (USB1_SUSP_CTRL, SuspCtrl | SUSP_CTRL_UTMIP_RESET);
  UsbWr (USB1_SUSP_CTRL, SuspCtrl | SUSP_CTRL_UTMIP_PHY_ENB | SUSP_CTRL_UTMIP_RESET);

  // Enable IDDQ control by software and disable UTMIPLL IDDQ.
  ClkWr (CLK_RST_UTMIPLL_HW_PWRDN_CFG0, (ClkRd (CLK_RST_UTMIPLL_HW_PWRDN_CFG0) & 0xFFFFFFFC) | 1);
  udelay (10);

  // Disable crystal clock.
  UsbAnd (USB1_UTMIP_MISC_CFG1, 0xBFFFFFFF);
  ClkWr (CLK_RST_UTMIP_PLL_CFG2, ClkRd (CLK_RST_UTMIP_PLL_CFG2) & 0xBFFFFFFF);

  // Set B_SESS_VLD.
  UsbOr (USB1_PHY_VBUS_SENSORS, 0x1000);
  UsbOr (USB1_PHY_VBUS_SENSORS, 0x800);

  UsbEnableUtmipll ();

  // Configure UTMIP transceiver cells from the fuses.
  FuseCalib = MmioRead32 (FUSE_BASE_ADDR + FUSE_USB_CALIB);
  UsbWr (USB1_UTMIP_XCVR_CFG0,
         (((UsbRd (USB1_UTMIP_XCVR_CFG0) & 0xFFFFFFF0) | (FuseCalib & 0xF)) & 0xFE3FFFFF) |
         ((((FuseCalib & 0x3F) << 25) >> 29) << 22));
  UsbWr (USB1_UTMIP_XCVR_CFG1,
         (UsbRd (USB1_UTMIP_XCVR_CFG1) & 0xFFC3FFFF) | ((((FuseCalib << 21) >> 28)) << 18));
  UsbWr (USB1_UTMIP_XCVR_CFG3,
         (UsbRd (USB1_UTMIP_XCVR_CFG3) & 0xFFFFC1FF) |
         ((MmioRead32 (FUSE_BASE_ADDR + FUSE_USB_CALIB_EXT) & 0x1F) << 9));
  UsbAnd (USB1_UTMIP_XCVR_CFG0, 0xFFDFFFFF);
  UsbWr (USB1_UTMIP_XCVR_CFG2, (UsbRd (USB1_UTMIP_XCVR_CFG2) & 0xFFFFF1FF) | 0x400);
  udelay (1);

  // Misc UTMIP.
  UsbWr (USB1_UTMIP_DEBOUNCE_CFG0, (UsbRd (USB1_UTMIP_DEBOUNCE_CFG0) & 0xFFFF0000) | 0xBB80);
  UsbWr (USB1_UTMIP_BIAS_CFG1, (UsbRd (USB1_UTMIP_BIAS_CFG1) & 0xFFFFC0FF) | 0x100);

  UsbOr (USB1_UTMIP_BIAS_CFG2, 2);
  UsbAnd (USB1_UTMIP_SPARE_CFG0, 0xFFFFFE67);
  UsbOr (USB1_UTMIP_TX_CFG0, 0x80000);

  UsbWr (USB1_UTMIP_HSRX_CFG0, (UsbRd (USB1_UTMIP_HSRX_CFG0) & 0xF0F003FF) | 0x88000 | 0x4000);
  UsbAnd (USB1_UTMIP_BIAS_CFG2, 0xFFFFFFF8);

  UsbWr (USB1_UTMIP_HSRX_CFG1, (UsbRd (USB1_UTMIP_HSRX_CFG1) & 0xFFFFFFC1) | 0x12);
  UsbOr (USB1_UTMIP_MISC_CFG1, 0x40000000);

  // Enable crystal clock.
  ClkWr (CLK_RST_UTMIP_PLL_CFG2, ClkRd (CLK_RST_UTMIP_PLL_CFG2) | 0x40000000);

  // Enable USB2 tracking clock.
  ClkWr (CLK_RST_CLK_ENB_Y_SET, 1U << CLK_Y_USB2_TRK);
  ClkWr (CLK_RST_CLK_SOURCE_USB2_HSIC_TRK, (ClkRd (CLK_RST_CLK_SOURCE_USB2_HSIC_TRK) & 0xFFFFFF00) | 6);

  UsbWr (USB1_UTMIP_BIAS_CFG1, (UsbRd (USB1_UTMIP_BIAS_CFG1) & 0xFFC03F07) | 0x78000 | 0x50);
  UsbAnd (USB1_UTMIP_BIAS_CFG0, 0xFFFFFBFF);
  udelay (1);

  // Force PDTRK input into power up.
  UsbWr (USB1_UTMIP_BIAS_CFG1, (UsbRd (USB1_UTMIP_BIAS_CFG1) & 0xFFFFFFFE) | 2);
  udelay (100);

  // TRK cycle done. Force PDTRK input into power down.
  UsbWr (USB1_UTMIP_BIAS_CFG1, (UsbRd (USB1_UTMIP_BIAS_CFG1) & 0xFF7FFFFF) | 1);
  udelay (3);

  // Force PDTRK input into power up.
  UsbAnd (USB1_UTMIP_BIAS_CFG1, 0xFFFFFFFE);
  udelay (100);

  // TRK cycle done. Force PDTRK input into power down.
  UsbWr (USB1_UTMIP_BIAS_CFG1, (UsbRd (USB1_UTMIP_BIAS_CFG1) & 0xFF7FFFFF) | 1);

  // Disable USB2 tracking clock and configure UTMIP misc.
  ClkWr (CLK_RST_CLK_ENB_Y_CLR, 1U << CLK_Y_USB2_TRK);
  ClkWr (CLK_RST_UTMIP_PLL_CFG2, (ClkRd (CLK_RST_UTMIP_PLL_CFG2) & 0xFEFFFFEA) | 0x2000000 | 0x28 | 2);
  udelay (1);

  UsbAnd (USB1_UTMIP_BIAS_CFG0, 0xFF3FF7FF);
  udelay (1);

  // Clear power downs on UTMIP ID and VBUS wake up, PD, PD2, PDZI, PDCHRP, PDDR.
  MmioAnd32 (PMC_BASE_ADDR + PMC_USB_AO, 0xFFFFFFF3);
  udelay (1);
  UsbAnd (USB1_UTMIP_XCVR_CFG0, 0xFFFFBFFF);
  udelay (1);
  UsbAnd (USB1_UTMIP_XCVR_CFG0, 0xFFFEFFFF);
  udelay (1);
  UsbAnd (USB1_UTMIP_XCVR_CFG0, 0xFFFBFFFF);
  udelay (1);
  UsbAnd (USB1_UTMIP_XCVR_CFG1, 0xFFFFFFFB);
  udelay (1);
  UsbAnd (USB1_UTMIP_XCVR_CFG1, 0xFFFFFFEF);
  udelay (1);
}

//
// hekate _usbd_reset_usb_otg_phy_device_mode(), but selecting host mode.
//
STATIC
EFI_STATUS
UsbResetControllerHostMode (VOID)
{
  UINT32 Retries;

  // Clear UTMIP reset and wait for the PHY clock.
  UsbAnd (USB1_SUSP_CTRL, ~SUSP_CTRL_UTMIP_RESET);
  if (!UsbWaitPhyClock ()) {
    return EFI_TIMEOUT;
  }

  // Stop the controller, idle mode, reset.
  UsbAnd (USB1_USBCMD, ~USBCMD_RUN);
  UsbAnd (USB1_USBMODE, ~USBMODE_CM_MASK);
  UsbOr (USB1_USBCMD, USBCMD_RESET);

  for (Retries = USB_POLL_RETRIES; Retries != 0; Retries--) {
    if ((UsbRd (USB1_USBCMD) & USBCMD_RESET) == 0) {
      break;
    }

    udelay (1);
  }

  if (Retries == 0) {
    return EFI_TIMEOUT;
  }

  if (!UsbWaitPhyClock ()) {
    return EFI_TIMEOUT;
  }

  // Host mode.
  UsbWr (USB1_USBMODE, (UsbRd (USB1_USBMODE) & ~USBMODE_CM_MASK) | USBMODE_CM_HOST);
  for (Retries = USB_POLL_RETRIES; Retries != 0; Retries--) {
    if ((UsbRd (USB1_USBMODE) & USBMODE_CM_MASK) == USBMODE_CM_HOST) {
      return EFI_SUCCESS;
    }

    udelay (1);
  }

  return EFI_TIMEOUT;
}

/*
 * bm92t36 (ROHM USB-C PD controller, I2C1 0x18). Its registers need a combined write+read with a
 * repeated START, which the legacy-mode I2cLib cannot do, so port hekate's packet-mode transfer.
 */
#define I2C1_BASE        0x7000C000
#define BM92T_ADDR       0x18

STATIC
VOID
I2cLoadCfgWait (
    volatile UINT32 *B
)
{
    UINT32 I;

    B[0x8C / 4] = (1 << 5) | (1 << 2) | 1;
    for (I = 0; I < 20; I++) {
        if (!(B[0x8C / 4] & 1)) break;
        gBS->Stall (1);
    }
}

// Write one register byte, repeated START, read RxSize (<= 8) bytes. Returns 0 on success.
STATIC
UINT32
I2c1WriteRead (
    IN  UINT32 Dev,
    IN  UINT8  Reg,
    OUT UINT8  *Rx,
    IN  UINT32 RxSize
)
{
    volatile UINT32 *B = (volatile UINT32 *)(UINTN)I2C1_BASE;
    UINT32 Res = 0;
    UINT32 Left = RxSize;
    UINT32 T;

    B[0x04 / 4] = (Dev << 1) | 1;
    B[0x00 / 4] = (2 << 12) | (1 << 11) | (1 << 6);          // debounce, new master FSM, read
    B[0x5C / 4] = 3;                                          // flush RX/TX FIFO
    I2cLoadCfgWait (B);
    B[0x00 / 4] = (B[0x00 / 4] & ~(1 << 9)) | (1 << 10);     // packet mode go

    B[0x50 / 4] = (1 << 4);                                   // protocol I2C
    B[0x50 / 4] = 0;                                          // 1 byte
    B[0x50 / 4] = (1 << 16) | (Dev << 1);                     // repeated start
    B[0x50 / 4] = Reg;

    for (T = 0; T < 200; T++) {
        if (((B[0x58 / 4] >> 4) & 0xFFF) == 0) break;
        gBS->Stall (1000);
    }

    B[0x50 / 4] = (1 << 4);
    B[0x50 / 4] = RxSize - 1;
    B[0x50 / 4] = (1 << 19) | (Dev << 1);                     // read

    T = 0;
    while (Left) {
        if (B[0x60 / 4] & 0xF) {
            UINT32 Len = Left < 4 ? Left : 4;
            UINT32 W = B[0x54 / 4];
            UINT32 K;
            for (K = 0; K < Len; K++) {
                Rx[(RxSize - Left) + K] = (UINT8)(W >> (8 * K));
            }
            Left -= Len;
        } else {
            gBS->Stall (1000);
            if (++T > 200) { Res = 1; break; }
        }
    }

    if (B[0x1C / 4] & 0xF) Res = 1;                           // NACK
    gBS->Stall (20);
    B[0x00 / 4] &= ~((1 << 9) | (1 << 10));
    return Res;
}

STATIC
UINT32
Bm92tRead16 (
    IN  UINT8  Reg,
    OUT UINT32 *Val
)
{
    UINT8 D[2] = { 0xEE, 0xEE };
    UINT32 R = I2c1WriteRead (BM92T_ADDR, Reg, D, 2);

    *Val = D[0] | (D[1] << 8);
    return R;
}

STATIC UINT32 mBmResets;

// Same as the Linux extcon driver: SPDSRC1/2 power path ON (CONFIG1[15:14]=0), OCP enabled (VENDOR bit2=0).
STATIC
VOID
Bm92tReapply (
    VOID
)
{
    UINT32 V;
    UINT8  W[2];

    if (!Bm92tRead16 (0x06, &V) && (V & (3 << 14))) {
        V &= ~(3 << 14);
        W[0] = V & 0xFF; W[1] = (V >> 8) & 0xFF;
        i2c_send_buf_small (I2C_1, BM92T_ADDR, 0x06, W, 2);
    }
    if (!Bm92tRead16 (0x1A, &V) && (V & (1 << 2))) {
        V &= ~(1 << 2);
        W[0] = V & 0xFF; W[1] = (V >> 8) & 0xFF;
        i2c_send_buf_small (I2C_1, BM92T_ADDR, 0x1A, W, 2);
    }
}

// STATUS1[1:0] != 0 is the PD IC's "internal error" state. The Linux driver answers it with a full
// SYS_RESET (command 0x0D0D) and re-applies its setup. At most 3 times, never with a cable inserted.
STATIC
VOID
Bm92tCheck (
    VOID
)
{
    UINT32 S1, Dummy;
    UINT8  Cmd[2] = { 0x0D, 0x0D };

    if (Bm92tRead16 (0x03, &S1)) return;
    if ((S1 & 3) && !(S1 & 0x80) && mBmResets < 3) {
        mBmResets++;
        i2c_send_buf_small (I2C_1, BM92T_ADDR, 0x05, Cmd, 2);
        gBS->Stall (33000);
        Bm92tRead16 (0x02, &Dummy);                      // clear the alert after the reset
        gBS->Stall (100000);
        Bm92tReapply ();
    }
}

STATIC
VOID
EFIAPI
Bm92tPollTimer (
    IN EFI_EVENT  Event,
    IN VOID       *Context
)
{
    Bm92tCheck ();
}

STATIC
VOID
Bm92tEnableSource (
    VOID
)
{
    UINT32 Man, Dev;

    if (Bm92tRead16 (0x4D, &Man) || Bm92tRead16 (0x4E, &Dev) || Man != 0x4B5 || Dev != 0x3B0) {
        DEBUG((DEBUG_ERROR, "USB: bm92t36 not found (man=%x dev=%x)\n", Man, Dev));
        return;
    }
    Bm92tCheck ();
    Bm92tReapply ();
    DEBUG((DEBUG_INFO, "USB: bm92t36 source path enabled\n"));
}

/* Set up VBUS for host/device mode */
static void set_up_vbus(
    struct fdt_usb *config, 
    enum usb_init_type init
)
{
	/*
	 * If we are an OTG port initializing in host mode,
	 * check if remote host is driving VBus and bail out in this case.
	 */
	if (init == USB_INIT_HOST &&
	    config->dr_mode == DR_MODE_OTG &&
	    (readl(&config->reg->phy_vbus_sensors) & VBUS_VLD_STS)) {
		printf("tegrausb: VBUS input active; not enabling as host\n");
		return;
	}

	if (config->vbus_gpio_valid)
    {
        UINT8 Reg;

        /*
         * Erista: VBUS to the USB-C port is sourced by the bq24193 OTG boost
         * and gated by GPIO CC4 (USB_VBUS_EN0). hekate leaves both off, and
         * PinMuxDxe routes the pad to the USB function, so GPIO CC4 never
         * reaches the pad. Fix all three.
         */

        // Pad: not the USB function, so the GPIO output drives it (as hekate does).
        MmioWrite32 (0x70003000 + 0x1A8, (1 << 8) | 1);

        // bq24193 (I2C1 0x6B): only touch it if the vendor/part ID matches.
        if (i2c_recv_byte (I2C_1, 0x6B, 0x0A) == 0x2F) {
            // REG05: disable the I2C watchdog, or it resets OTG back to charge.
            Reg = i2c_recv_byte (I2C_1, 0x6B, 0x05);
            Reg &= ~(3 << 4);
            i2c_send_byte (I2C_1, 0x6B, 0x05, Reg);

            // REG01 CHG_CONFIG[5:4] = 10b: OTG boost on.
            Reg = i2c_recv_byte (I2C_1, 0x6B, 0x01);
            Reg = (Reg & ~(3 << 4)) | (2 << 4);
            i2c_send_byte (I2C_1, 0x6B, 0x01, Reg);

            DEBUG((DEBUG_INFO, "USB: bq24193 OTG on, REG01=%02x\n",
                   i2c_recv_byte (I2C_1, 0x6B, 0x01)));
        } else {
            DEBUG((DEBUG_ERROR, "USB: bq24193 not found, no OTG boost\n"));
        }

        Bm92tEnableSource ();

        // nvidia,vbus-gpio = <&gpio TEGRA_GPIO(CC, 4) GPIO_ACTIVE_HIGH>
        gpio_config(GPIO_PORT_CC, GPIO_PIN_4, GPIO_MODE_GPIO);
        gpio_write(GPIO_PORT_CC, GPIO_PIN_4, GPIO_HIGH);
        gpio_output_enable(GPIO_PORT_CC, GPIO_PIN_4, GPIO_OUTPUT_ENABLE);
        DEBUG((DEBUG_INFO, "USB: VBUS enabled\n"));
	}
}

/*
 * On-screen USB/VBUS diagnostics. There is no keyboard to type commands with, so print the relevant
 * registers once the console is up (ReadyToBoot) and hold the screen for a while.
 */
/* Which loaded image contains Addr? Prints GUID / base / offset so a crash PC can be mapped to a module. */
STATIC
VOID
UsbDiagFindImage (
    IN UINT64  Addr
)
{
    CHAR16                      Line[160];
    UINTN                       Count, I;
    EFI_HANDLE                  *Handles;
    EFI_LOADED_IMAGE_PROTOCOL   *Li;
    UINT8                       *Node;
    UINTN                       Guard;

    if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &Count, &Handles))) {
        return;
    }
    for (I = 0; I < Count; I++) {
        if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiLoadedImageProtocolGuid, (VOID **)&Li)) || Li == NULL) continue;
        if (Addr < (UINT64)(UINTN)Li->ImageBase || Addr >= (UINT64)(UINTN)Li->ImageBase + Li->ImageSize) continue;

        UnicodeSPrint (Line, sizeof (Line), L"[USBDIAG] %lx is in image base=%lx +%lx",
                       Addr, (UINT64)(UINTN)Li->ImageBase, Addr - (UINT64)(UINTN)Li->ImageBase);
        gST->ConOut->OutputString (gST->ConOut, Line);
        // FV file path node: type 4 (MEDIA), subtype 6 (PIWG file), GUID at +4.
        Node = (UINT8 *)Li->FilePath;
        for (Guard = 0; Node != NULL && Guard < 8 && Node[0] != 0x7F; Guard++) {
            if (Node[0] == 0x04 && Node[1] == 0x06) {
                UnicodeSPrint (Line, sizeof (Line), L" file=%g", (EFI_GUID *)(Node + 4));
                gST->ConOut->OutputString (gST->ConOut, Line);
                break;
            }
            Node += Node[2] | (Node[3] << 8);
        }
        gST->ConOut->OutputString (gST->ConOut, L"\r\n");
    }
    gBS->FreePool (Handles);
}

/*
 * Synchronous exception handler: print the fault state and which image the PC / LR are in, then hang.
 * (The default handler only shows one line on screen.)
 */
STATIC
VOID
EFIAPI
UsbDiagExceptionHandler (
    IN EFI_EXCEPTION_TYPE  Type,
    IN EFI_SYSTEM_CONTEXT  Ctx
)
{
    CHAR16                      Line[160];
    EFI_SYSTEM_CONTEXT_AARCH64  *C = Ctx.SystemContextAArch64;

    if (gST->ConOut != NULL) {
        UnicodeSPrint (Line, sizeof (Line), L"\r\n[EXC] ELR=%lx ESR=%lx FAR=%lx SPSR=%lx\r\n", C->ELR, C->ESR, C->FAR, C->SPSR);
        gST->ConOut->OutputString (gST->ConOut, Line);
        UnicodeSPrint (Line, sizeof (Line), L"[EXC] LR=%lx SP=%lx X0=%lx X1=%lx X2=%lx X3=%lx\r\n", C->LR, C->SP, C->X0, C->X1, C->X2, C->X3);
        gST->ConOut->OutputString (gST->ConOut, Line);
        UsbDiagFindImage (C->ELR);
        UsbDiagFindImage (C->LR);
    }
    for (;;) { }
}

#if 0 // [USBDIAG] screen dump, disabled
STATIC
VOID
EFIAPI
UsbDiagOnReadyToBoot (
    IN EFI_EVENT  Event,
    IN VOID       *Context
)
{
    CHAR16 Line[160];
    UINT32 Ci;

    STATIC UINT32 Tick = 0;

    Tick++;
    if (Tick > 6) {
        gBS->CloseEvent (Event);
        return;
    }
    if (gST->ConOut == NULL) return;

    UnicodeSPrint (Line, sizeof (Line), L"\r\n[USBDIAG] tick %d/6", Tick);
    gST->ConOut->OutputString (gST->ConOut, Line);

    UnicodeSPrint (Line, sizeof (Line),
        L"\r\n[USBDIAG] GPIO CC cnf=%08x oe=%08x out=%08x in=%08x  (CC4=bit4)\r\n",
        MmioRead32 (0x6000D700), MmioRead32 (0x6000D710), MmioRead32 (0x6000D720), MmioRead32 (0x6000D730));
    gST->ConOut->OutputString (gST->ConOut, Line);

    UnicodeSPrint (Line, sizeof (Line),
        L"[USBDIAG] PINMUX VBUS_EN0=%08x\r\n", MmioRead32 (0x700031A8));
    gST->ConOut->OutputString (gST->ConOut, Line);

    UnicodeSPrint (Line, sizeof (Line),
        L"[USBDIAG] USB1 CMD=%08x STS=%08x MODE=%08x PORTSC=%08x HOSTPC1=%08x SUSP=%08x VBUS=%08x\r\n",
        MmioRead32 (0x7D000130), MmioRead32 (0x7D000134), MmioRead32 (0x7D0001F8),
        MmioRead32 (0x7D000184), MmioRead32 (0x7D0001B4), MmioRead32 (0x7D000400), MmioRead32 (0x7D000404));
    gST->ConOut->OutputString (gST->ConOut, Line);

    UnicodeSPrint (Line, sizeof (Line), L"[USBDIAG] bq24193 REG00-0A:");
    gST->ConOut->OutputString (gST->ConOut, Line);
    for (Ci = 0; Ci <= 0x0A; Ci++) {
        UnicodeSPrint (Line, sizeof (Line), L" %02x", i2c_recv_byte (I2C_1, 0x6B, Ci));
        gST->ConOut->OutputString (gST->ConOut, Line);
    }
    gST->ConOut->OutputString (gST->ConOut, L"\r\n");

    UnicodeSPrint (Line, sizeof (Line), L"[USBDIAG] EHCI HCSPARAMS=%08x HCCPARAMS=%08x\r\n",
        MmioRead32 (0x7D000104), MmioRead32 (0x7D000108));
    gST->ConOut->OutputString (gST->ConOut, Line);

    // bm92t36: 02=ALERT 03=STATUS1(b12 SRC_MODE) 04=STATUS2(b13 OTG_INSERT) 06=CONFIG1 1A=VENDOR
    //          4B=FW 4D=MAN(4b5) 4E=DEV(3b0)
    {
        STATIC CONST UINT8 BmRegs[] = { 0x02, 0x03, 0x04, 0x06, 0x1A, 0x4B, 0x4D, 0x4E };
        UINT32 V;
        UINTN Bi;

        gST->ConOut->OutputString (gST->ConOut, L"[USBDIAG] bm92t36:");
        for (Bi = 0; Bi < sizeof (BmRegs); Bi++) {
            UINT32 R = Bm92tRead16 (BmRegs[Bi], &V);
            UnicodeSPrint (Line, sizeof (Line), L" r%02x=%04x%s", BmRegs[Bi], V, R ? L"!" : L"");
            gST->ConOut->OutputString (gST->ConOut, Line);
        }
        UnicodeSPrint (Line, sizeof (Line), L" resets=%d", mBmResets);
        gST->ConOut->OutputString (gST->ConOut, Line);
        gST->ConOut->OutputString (gST->ConOut, L"\r\n");
    }

    UsbDiagFindImage (0x16F398C14ULL);                        // PC of the exception seen on hardware
}

#endif

EFI_STATUS
ConfigController(
    VOID
)
{
    EFI_STATUS Status;

    mPriv.reg = (VOID*) (UINTN) 0x7d000000;
    mPriv.enabled = 1;
    mPriv.utmi = 1;
    mPriv.ulpi = 0;
    mPriv.has_legacy_mode = 0;
    mPriv.dr_mode = DR_MODE_OTG;
    mPriv.type = USB_CTLR_T210;
    mPriv.init_type = USB_INIT_HOST;
    mPriv.periph_id = PERIPH_ID_USBD;
    mPriv.vbus_gpio_valid = true;
    mPriv.clk_done = false;

    UsbInitPhy();

    Status = UsbResetControllerHostMode();
    if (EFI_ERROR(Status))
    {
        DEBUG((DEBUG_ERROR, "USB: controller bring-up failed: %r\n", Status));
        return EFI_DEVICE_ERROR;
    }

    set_up_vbus(&mPriv, USB_INIT_HOST);
    return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ConfigureUSBHost(
    NON_DISCOVERABLE_DEVICE   *Device
)
{
    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
PciEmulationEntryPoint(
    IN EFI_HANDLE       ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
)
{
    EFI_STATUS Status;

    Status = gBS->LocateProtocol(
        &gTegraUBootClockManagementProtocolGuid,
        NULL,
        (VOID**) &mClkProtocol
    );

    if (EFI_ERROR(Status)) goto exit;

    Status = ConfigController();
    if (EFI_ERROR(Status)) goto exit;

    {
        EFI_CPU_ARCH_PROTOCOL *Cpu;
        if (!EFI_ERROR (gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu))) {
            Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, UsbDiagExceptionHandler);
        }
    }

    {
        EFI_EVENT BmEvent;
        if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                          Bm92tPollTimer, NULL, &BmEvent))) {
            gBS->SetTimer (BmEvent, TimerPeriodic, 2500 * 1000);   // every 250 ms
        }

        // On-screen [USBDIAG] output removed (USB keyboard works).
    }

    return RegisterNonDiscoverableMmioDevice(
        NonDiscoverableDeviceTypeEhci,
        NonDiscoverableDeviceDmaTypeNonCoherent,
        ConfigureUSBHost,
        NULL,
        1,
        &mPriv.reg->cap_length, 
        0x740
    );

exit:
    ASSERT_EFI_ERROR(Status);
    return Status;
}
