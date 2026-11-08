/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * T210B01 UPHY setup, adapted from Switchroot Linux:
 * drivers/clk/tegra/{clk-tegra210b01.c,clk-tegra-periph.c}
 * drivers/phy/tegra/xusb-tegra210.c
 * Copyright (c) 2014-2019 NVIDIA CORPORATION. All rights reserved.
 * Copyright (C) 2015 Google, Inc.
 * Copyright (C) 2022-2023 CTCaer
 *
 * The shared PCIe code selects this helper only on B01. Rm/Wr/Rd/Us/Log
 * are provided by WifiPcie.h. This does not configure USB lanes.
 */
#ifndef WIFI_T214_UPHY_H
#define WIFI_T214_UPHY_H

STATIC
BOOLEAN
T214UphyManagementEnable (
  VOID
  )
{
  UINT32  Gates;
  UINT32  Divider;

  // pll_p -> pll_p_out_hsio -> pll_p_out_xusb -> pll_p_uphy_out.
  // The B01 divider is 7.1 format: 408 MHz * 2 / (6 + 2) = 102 MHz.
  // On Erista 0x6D0 has a different function and must not be written here.
  Rm (CAR_BASE, 0x680, 0, BIT29 | BIT28);
  Rm (CAR_BASE, 0x6D0, 0xFFU, BIT8 | 6U);
  Us (2);
  Gates = Rd (CAR_BASE, 0x680);
  Divider = Rd (CAR_BASE, 0x6D0);
  Log (L"uphy: B01 management clock PLLP_MISC1=%08x DIV=%08x", Gates, Divider);
  return (Gates & (BIT29 | BIT28)) == (BIT29 | BIT28) &&
         (Divider & 0x1FFU) == (BIT8 | 6U);
}

STATIC
VOID
T214PcieLaneDefaults (
  IN UINT32  Lane
  )
{
  // tegra210_pex_uphy_enable: lower the PCIe receiver idle threshold.
  Rm (PX_PADCTL, 0x460 + Lane * 0x40, 3U << 24, 1U << 24);
  // pcie_lane_data: CFG_RESET | CFG_WS | CFG_ADDR(0x97) | CFG_WDATA(0x80).
  Wr (PX_PADCTL, 0x47C + Lane * 0x40, BIT27 | BIT24 | (0x97U << 16) | 0x80U);
}

STATIC
VOID
T214UphyPllDefaults (
  VOID
  )
{
  STATIC CONST UINT32  PllData[] = {
    (0x02U << 16) | 0x0000U,
    (0x03U << 16) | 0x7051U,
    (0x25U << 16) | 0x0130U,
    (0x1EU << 16) | 0x0017U
  };
  UINTN  Index;

  // B01 uses usb3_pll_g1_init_data through CTL10, replacing the T210
  // direct CAL_CTRL=0x136 and DCO_CTRL=0x2A programming.
  for (Index = 0; Index < ARRAY_SIZE (PllData); Index++) {
    Wr (PX_PADCTL, 0x384, BIT27 | BIT24 | PllData[Index]);
  }
}

#endif
