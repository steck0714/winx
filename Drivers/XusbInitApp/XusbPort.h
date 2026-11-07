/* SPDX-License-Identifier: GPL-2.0-only
 * Tegra210 USB3 port analog settings and clamp sequence from
 * Switchroot drivers/phy/tegra/xusb-tegra210.c. Port 0 is paired with USB2-0.
 * The caller selects the physical lane for its board and owns the pad clock. */
#ifndef TEGRA_XUSB_PORT_H
#define TEGRA_XUSB_PORT_H
STATIC VOID XusbEnableSsPort0 (UINTN Pad)
{
  MmioAndThenOr32 (Pad + 0x014, ~(BIT4 | 7U), 0);
  MmioAndThenOr32 (Pad + 0xA60, ~(3U << 16), 2U << 16);
  MmioAndThenOr32 (Pad + 0xA64, ~0xFFFFU, 0x00FC);
  MmioWrite32 (Pad + 0xA68, 0xC0077F1F);
  MmioAndThenOr32 (Pad + 0xA6C, ~(0xFFFFU << 16), 0x01C7U << 16);
  MmioWrite32 (Pad + 0xA74, 0xFCF01368);
  MmioAnd32 (Pad + 0x024, ~BIT2);
  gBS->Stall (150);
  MmioAnd32 (Pad + 0x024, ~BIT1);
  gBS->Stall (150);
  MmioAnd32 (Pad + 0x024, ~BIT0);
}
#endif
