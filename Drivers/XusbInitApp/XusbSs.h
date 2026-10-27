/** @file
  Tegra210 XUSB SuperSpeed (USB3) bring-up: PLLE + UPHY PLL + lane "pcie-6" (usb3-ss) + SS port 0.

  DRAFT, UNTESTED ON HARDWARE. Staged so every step logs and can be run on its own (`xusbinit ss`).

  Ported from Linux (GPL-2.0-only), drivers/phy/tegra/xusb-tegra210.c (tegra210_pex_uphy_enable,
  tegra210_uphy_init, tegra210_usb3_phy_power_on) and drivers/clk/tegra/clk-tegra210.c (PLLE).
  Switch wiring from fail0verflow switch-linux tegra210-nintendo-switch.dts:
    usb3-0 -> lane "pcie-6" (function usb3-ss), companion usb2-0, vbus = USB-C VBUS supply.

  Included from XusbInit.c, which provides Rd/Wr/Rmw/Us/Log/PadctlAlive and the SS_/PAD_ base macros.
**/

#define SS_CAR_PLLE_BASE    0x0E8
#define SS_CAR_PLLE_MISC    0x0EC
#define SS_CAR_PLLE_SS      0x068
#define SS_CAR_PLLE_AUX     0x48C
#define SS_CAR_XUSBIO_PLL   0x51C
#define SS_CAR_OSC_CTRL     0x050
#define SS_CAR_RST_Y_CLR    0x2AC
#define SS_Y_UPHY           BIT13               // reset 205 PEX_USB_UPHY

#define SS_PAD_ELPG1        0x024
#define SS_PAD_USB3_MUX     0x028
#define SS_PAD_SS_PORT_MAP  0x014
#define SS_PAD_PLL_P0_1     0x360
#define SS_PAD_PLL_P0_2     0x364
#define SS_PAD_PLL_P0_4     0x36C
#define SS_PAD_PLL_P0_5     0x370
#define SS_PAD_PLL_P0_8     0x37C
#define SS_PAD_ECTL1(x)     (0xA60 + (x) * 0x40)
#define SS_PAD_ECTL2(x)     (0xA64 + (x) * 0x40)
#define SS_PAD_ECTL3(x)     (0xA68 + (x) * 0x40)
#define SS_PAD_ECTL4(x)     (0xA6C + (x) * 0x40)
#define SS_PAD_ECTL6(x)     (0xA74 + (x) * 0x40)

#define SS_LANE_USB3        6                   // "pcie-6" carries USB3 port 0 (tegra210_usb3_map: {0, "pcie", 6})
#define SS_FUNC_USB3        1                   // lane function "usb3-ss" (index in tegra210_pcie_functions)

STATIC
BOOLEAN
SsWait (
  IN UINTN   Off,
  IN UINT32  Mask,
  IN UINT32  Val,
  IN CONST CHAR16  *What
  )
{
  UINTN  T;

  for (T = 0; T < 100000; T += 10) {
    if ((Rd (XUSB_PADCTL, Off) & Mask) == Val) {
      return TRUE;
    }
    Us (10);
  }
  Log (L"ss: TIMEOUT waiting for %s (reg %03x = %08x)", What, Off, Rd (XUSB_PADCTL, Off));
  return FALSE;
}

// clk_plle_tegra210_enable() + tegra210_plle_hw_sequence_start()
STATIC
BOOLEAN
SsPlle (
  VOID
  )
{
  UINT32  V;
  UINTN   T;

  if (Rd (CAR_BASE, SS_CAR_PLLE_AUX) & BIT24) {
    Log (L"ss: PLLE HW sequencer already running (AUX=%08x)", Rd (CAR_BASE, SS_CAR_PLLE_AUX));
    return TRUE;
  }
  if (!(Rd (CAR_BASE, SS_CAR_PLLE_BASE) & BIT31)) {
    Log (L"ss: enabling PLLE (OSC_CTRL=%08x, table for 38.4 MHz pll_ref: M=2 N=125 CML=14)", Rd (CAR_BASE, SS_CAR_OSC_CTRL));
    Rmw (CAR_BASE, SS_CAR_PLLE_BASE, BIT30, 0);
    V = Rd (CAR_BASE, SS_CAR_PLLE_MISC);
    V |= BIT9 | BIT14 | BIT8;
    V &= ~(BIT13 | (3U << 4) | (2U << 2));
    Wr (CAR_BASE, SS_CAR_PLLE_MISC, V);
    Us (5);
    Rmw (CAR_BASE, SS_CAR_PLLE_SS, 0, BIT10 | BIT11 | BIT12);
    Rmw (CAR_BASE, SS_CAR_PLLE_BASE, 0xFF | (0xFFU << 8) | (0x1FU << 24), 2 | (125U << 8) | (14U << 24));
    Us (1);
    Rmw (CAR_BASE, SS_CAR_PLLE_BASE, 0, BIT31);
    for (T = 0; T < 20000 && !(Rd (CAR_BASE, SS_CAR_PLLE_MISC) & BIT11); T += 10) {
      Us (10);
    }
    if (!(Rd (CAR_BASE, SS_CAR_PLLE_MISC) & BIT11)) {
      Log (L"ss: PLLE NO LOCK (BASE=%08x MISC=%08x)", Rd (CAR_BASE, SS_CAR_PLLE_BASE), Rd (CAR_BASE, SS_CAR_PLLE_MISC));
      return FALSE;
    }
    V = Rd (CAR_BASE, SS_CAR_PLLE_SS);
    V &= ~(BIT14 | BIT15 | 0x1FF | (0xFFU << 16) | (0x3FU << 24));
    V |= 0x21 | (1U << 16) | (0x23U << 24);
    Wr (CAR_BASE, SS_CAR_PLLE_SS, V);
    V &= ~(BIT12 | BIT10);
    Wr (CAR_BASE, SS_CAR_PLLE_SS, V);
    Us (1);
    V &= ~BIT11;
    Wr (CAR_BASE, SS_CAR_PLLE_SS, V);
    Us (1);
    Log (L"ss: PLLE locked BASE=%08x MISC=%08x SS=%08x", Rd (CAR_BASE, SS_CAR_PLLE_BASE), Rd (CAR_BASE, SS_CAR_PLLE_MISC), Rd (CAR_BASE, SS_CAR_PLLE_SS));
  }
  return TRUE;
}

STATIC
VOID
SsPlleHwSeq (
  VOID
  )
{
  UINT32  V;

  if (Rd (CAR_BASE, SS_CAR_PLLE_AUX) & BIT24) {
    return;
  }
  if (!(Rd (CAR_BASE, SS_CAR_PLLE_MISC) & BIT11)) {
    Log (L"ss: PLLE HW sequence skipped (not locked)");
    return;
  }
  Rmw (CAR_BASE, SS_CAR_PLLE_MISC, BIT14, 0);
  V = Rd (CAR_BASE, SS_CAR_PLLE_AUX);
  V |= BIT3 | BIT31;
  V &= ~(BIT4 | BIT6);
  Wr (CAR_BASE, SS_CAR_PLLE_AUX, V);
  Us (1);
  V |= BIT24;
  Wr (CAR_BASE, SS_CAR_PLLE_AUX, V);
  Us (1);
  Log (L"ss: PLLE HW sequencer started AUX=%08x", Rd (CAR_BASE, SS_CAR_PLLE_AUX));
}

// tegra210_pex_uphy_enable(), restricted to the USB3 lane
STATIC
BOOLEAN
SsUphy (
  VOID
  )
{
  if (Rd (CAR_BASE, SS_CAR_XUSBIO_PLL) & BIT24) {
    Log (L"ss: XUSBIO PLL HW sequencer already running (shared with PCIe), skipping calibration");
  } else {
    Wr (CAR_BASE, SS_CAR_RST_Y_CLR, SS_Y_UPHY);
    Us (10);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_2, 0xFFFFFFU << 4, 0x136U << 4);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_5, 0xFFU << 16, 0x2AU << 16);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_1, 0, BIT4);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_2, 0, BIT2);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_8, 0, BIT15);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_4, (3U << 12) | (0xFU << 4), (2U << 12) | BIT15);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_1, (3U << 16) | (0xFFU << 20), 0x19U << 20);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_1, BIT0, 0);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_1, 3U << 1, 0);
    Us (20);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_4, 0, BIT8);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_2, 0, BIT0);
    if (!SsWait (SS_PAD_PLL_P0_2, BIT1, BIT1, L"CAL_DONE")) {
      return FALSE;
    }
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_2, BIT0, 0);
    if (!SsWait (SS_PAD_PLL_P0_2, BIT1, 0, L"CAL_DONE clear")) {
      return FALSE;
    }
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_1, 0, BIT3);
    if (!SsWait (SS_PAD_PLL_P0_1, BIT15, BIT15, L"PLL LOCKDET")) {
      return FALSE;
    }
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_8, 0, BIT12 | BIT13);
    if (!SsWait (SS_PAD_PLL_P0_8, BIT31, BIT31, L"RCAL_DONE")) {
      return FALSE;
    }
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_8, BIT12, 0);
    if (!SsWait (SS_PAD_PLL_P0_8, BIT31, 0, L"RCAL_DONE clear")) {
      return FALSE;
    }
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_8, BIT13, 0);
    Rmw (CAR_BASE, SS_CAR_XUSBIO_PLL, BIT2 | BIT0, BIT6 | BIT13);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_1, BIT4, 0);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_2, BIT2, 0);
    Rmw (XUSB_PADCTL, SS_PAD_PLL_P0_8, BIT15, 0);
    Us (20);
    Rmw (CAR_BASE, SS_CAR_XUSBIO_PLL, 0, BIT24);
    Log (L"ss: UPHY PLL calibrated+locked XUSBIO=%08x CTL1=%08x", Rd (CAR_BASE, SS_CAR_XUSBIO_PLL), Rd (XUSB_PADCTL, SS_PAD_PLL_P0_1));
  }

  // lane 6 -> function usb3-ss (USB3_PAD_MUX[25:24]), release its IDDQ (bit 1 + lane)
  Rmw (XUSB_PADCTL, SS_PAD_USB3_MUX, 3U << (12 + 2 * (SS_LANE_USB3)), (UINT32)SS_FUNC_USB3 << (12 + 2 * (SS_LANE_USB3)));
  Rmw (XUSB_PADCTL, SS_PAD_USB3_MUX, 0, 1U << (1 + SS_LANE_USB3));
  SsPlleHwSeq ();

  // tegra210_aux_mux_lp0_clamp_disable()
  Rmw (XUSB_PADCTL, SS_PAD_ELPG1, BIT29, 0);
  Us (150);
  Rmw (XUSB_PADCTL, SS_PAD_ELPG1, BIT30, 0);
  Us (150);
  Rmw (XUSB_PADCTL, SS_PAD_ELPG1, BIT31, 0);
  Log (L"ss: uphy done PAD_MUX=%08x ELPG1=%08x", Rd (XUSB_PADCTL, SS_PAD_USB3_MUX), Rd (XUSB_PADCTL, SS_PAD_ELPG1));
  return TRUE;
}

// tegra210_usb3_phy_power_on() for USB3 port 0 (companion USB2 port 0, not "internal")
STATIC
VOID
SsPortOn (
  VOID
  )
{
  UINT32  Idx = 0;

  Rmw (XUSB_PADCTL, SS_PAD_SS_PORT_MAP, (1U << (Idx * 5 + 4)) | (7U << (Idx * 5)), 0U << (Idx * 5));
  Rmw (XUSB_PADCTL, SS_PAD_ECTL1 (Idx), 3U << 16, 2U << 16);
  Rmw (XUSB_PADCTL, SS_PAD_ECTL2 (Idx), 0xFFFF, 0x00FC);
  Wr (XUSB_PADCTL, SS_PAD_ECTL3 (Idx), 0xC0077F1F);
  Rmw (XUSB_PADCTL, SS_PAD_ECTL4 (Idx), 0xFFFFU << 16, 0x01C7U << 16);
  Wr (XUSB_PADCTL, SS_PAD_ECTL6 (Idx), 0xFCF01368);
  Rmw (XUSB_PADCTL, SS_PAD_ELPG1, 1U << (2 + Idx * 3), 0);        // SSPX_ELPG_VCORE_DOWN
  Us (150);
  Rmw (XUSB_PADCTL, SS_PAD_ELPG1, 1U << (1 + Idx * 3), 0);        // SSPX_ELPG_CLAMP_EN_EARLY
  Us (150);
  Rmw (XUSB_PADCTL, SS_PAD_ELPG1, 1U << (Idx * 3), 0);            // SSPX_ELPG_CLAMP_EN
  Log (L"ss: SS port0 on MAP=%08x ECTL1=%08x ELPG1=%08x", Rd (XUSB_PADCTL, SS_PAD_SS_PORT_MAP), Rd (XUSB_PADCTL, SS_PAD_ECTL1 (Idx)),
       Rd (XUSB_PADCTL, SS_PAD_ELPG1));
}

STATIC
VOID
StepSs (
  VOID
  )
{
  if (!PadctlAlive ()) {
    Log (L"ss: padctl not up, run `clk` first");
    return;
  }
  Log (L"ss: start (needs `clk` and `pad` first; SS lane = pcie-%d)", SS_LANE_USB3);
  if (!SsPlle ()) {
    return;
  }
  if (!SsUphy ()) {
    return;
  }
  SsPortOn ();
  Log (L"ss: done; run `ports`/`svc` and plug a USB3 device (use a USB3 cable and the dock-less USB-C port)");
}

// MBOX_CMD_{DISABLE,ENABLE}_SS_LFPS_DETECTION (tegra210_usb3_set_lfps_detect). Data bit (1 + n) selects USB3 port n.
// Only USB3 port 0 exists on the Switch (lane pcie-6); any other selected port fails like Linux' -ENODEV.
STATIC
BOOLEAN
SsLfps (
  IN UINT32   Data,
  IN BOOLEAN  Enable
  )
{
  UINT32  Mask = (Data >> 1) & 0xF;
  UINT32  Off  = 0x460 + SS_LANE_USB3 * 0x40;      // UPHY_MISC_PAD_PX_CTL1(6)

  if (Mask & ~1U) {
    return FALSE;
  }
  if (Mask & 1U) {
    // AUX_RX_IDLE_MODE [21:20], AUX_RX_TERM_EN bit18, AUX_RX_MODE_OVRD bit13
    Rmw (XUSB_PADCTL, Off, (3U << 20) | BIT18 | BIT13, Enable ? 0 : ((1U << 20) | BIT18 | BIT13));
    if (!Enable) {
      Us (500);          // let the LFPS detector turn off before the ACK
    }
  }
  return TRUE;
}
