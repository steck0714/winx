/** @file
  WifiPcie.h - low level part of the BCM4356 Wi-Fi bring-up over PCIe (Tegra210 PCIe controller, port index 1 / "pci@2,0", lane pcie-0).

  The Switch's Wi-Fi is NOT SDIO: switch-linux's defconfig has CONFIG_BRCMFMAC_PCIE=y, CONFIG_BRCMFMAC_SDIO unset, and the firmware names
  brcm/brcmfmac4356-pcie.{bin,txt}; its DTS enables the x1 root port (pci@2,0 with phy pcie-0) next to an (empty) x4 port.

  Everything here is ported from Linux (GPL-2.0): drivers/pci/controller/pci-tegra.c, drivers/phy/tegra/xusb-tegra210.c (UPHY PLL, pcie lanes),
  drivers/clk/tegra/{clk-pll.c,clk-tegra210.c} (PLLE + HW sequencer), drivers/soc/tegra/pmc.c (power gate, IO pad DPD) and
  drivers/net/wireless/broadcom/brcm80211/brcmfmac/{pcie.c,chip.c,firmware.c}.

  DRAFT, UNTESTED.  ALL accesses are naturally aligned 32-bit MMIO.  This file is #included by WifiBt.c (single translation unit) after Log/Rd/Wr/Us/Ms.
**/

// ---------------------------------------------------------------------------------------------
// Register map
// ---------------------------------------------------------------------------------------------
#define PEX_PADS          0x01003000
#define PEX_AFI           0x01003800
#define PEX_CS            0x02000000          // configuration space window (4 KiB used)
#define PEX_RP_BASE       0x01000000          // root port N config space = base + N * 0x1000
#define PEX_PORT          1                   // port index 1 == "pci@2,0" == x1 lane pcie-0 (Wi-Fi)
#define PEX_MEM_BASE      0x13000000          // non-prefetchable window (AXI), identity mapped to FPCI
#define PEX_MEM_SIZE      0x0D000000

#define AFI_AXI_BAR0_SZ     0x00
#define AFI_AXI_BAR3_SZ     0x0C
#define AFI_AXI_BAR4_SZ     0x10
#define AFI_AXI_BAR5_SZ     0x14
#define AFI_AXI_BAR0_START  0x18
#define AFI_AXI_BAR3_START  0x24
#define AFI_AXI_BAR4_START  0x28
#define AFI_AXI_BAR5_START  0x2C
#define AFI_FPCI_BAR0       0x30
#define AFI_FPCI_BAR3       0x3C
#define AFI_FPCI_BAR4       0x40
#define AFI_FPCI_BAR5       0x44
#define AFI_MSI_BAR_SZ      0x60
#define AFI_MSI_FPCI_BAR_ST 0x64
#define AFI_MSI_AXI_BAR_ST  0x68
#define AFI_CONFIGURATION   0x0AC
#define AFI_FPCI_ERR_MASKS  0x0B0
#define AFI_INTR_MASK       0x0B4
#define AFI_INTR_CODE       0x0B8
#define AFI_INTR_SIGNATURE  0x0BC
#define AFI_SM_INTR_ENABLE  0x0C4
#define AFI_AFI_INTR_ENABLE 0x0C8
#define AFI_PCIE_CONFIG     0x0F8
#define AFI_FUSE            0x104
#define AFI_PEX0_CTRL       0x110
#define AFI_PEX1_CTRL       0x118
#define AFI_PEXBIAS_CTRL_0  0x168

#define PADS_REFCLK_CFG0    0x0C8

// Root port (offsets inside the port's 4 KiB config space)
#define RP_ECTL_2_R1        0xE84
#define RP_ECTL_4_R1        0xE8C
#define RP_ECTL_5_R1        0xE90
#define RP_ECTL_6_R1        0xE94
#define RP_ECTL_2_R2        0xEA4
#define RP_ECTL_4_R2        0xEAC
#define RP_ECTL_5_R2        0xEB0
#define RP_ECTL_6_R2        0xEB4
#define RP_VEND_XP          0xF00
#define RP_VEND_CTL0        0xF44
#define RP_VEND_CTL1        0xF48
#define RP_VEND_XP_BIST     0xF4C
#define RP_VEND_CTL2        0xFA8
#define RP_PRIV_MISC        0xFE0
#define RP_LINK_CTL_STATUS  0x090
#define RP_LINK_CTL_STATUS2 0x0B0

// CAR
#define PX_CAR_PLLE_BASE    0x0E8
#define PX_CAR_PLLE_MISC    0x0EC
#define PX_CAR_PLLE_SS      0x068
#define PX_CAR_PLLE_AUX     0x48C
#define PX_CAR_XUSBIO_PLL   0x51C
#define PX_CAR_OSC_CTRL     0x050
#define PX_CAR_RST_U_SET    0x310
#define PX_CAR_RST_U_CLR    0x314
#define PX_CAR_ENB_U_SET    0x330
#define PX_CAR_ENB_U_CLR    0x334
#define PX_CAR_RST_W        0x35C
#define PX_CAR_ENB_W        0x364
#define PX_CAR_RST_W_SET    0x438
#define PX_CAR_RST_W_CLR    0x43C
#define PX_CAR_ENB_W_SET    0x448
#define PX_CAR_RST_Y        0x2A4
#define PX_CAR_RST_Y_CLR    0x2AC
#define PX_U_PEX            BIT6                // clk 70 "pcie" / reset 70
#define PX_U_AFI            BIT8                // clk 72 "afi"  / reset 72
#define PX_U_PCIEX          BIT10               // reset 74 "pcie_x"
#define PX_W_PADCTL         BIT14               // reset 142
#define PX_W_XUSB           BIT15               // clk/reset 143 (xusb gate)
#define PX_Y_UPHY           BIT13               // reset 205 PEX_USB_UPHY

// PMC
#define PX_PMC              0x7000E400
#define PX_PMC_DPD_SAMPLE   0x020
#define PX_PMC_PG_TOGGLE    0x030
#define PX_PMC_PG_CLAMP     0x034
#define PX_PMC_PG_STATUS    0x038
#define PX_PMC_IO_DPD_REQ   0x1B8
#define PX_PMC_IO_DPD_STS   0x1BC
#define PX_PMC_SEL_DPD_TIM  0x1C8
#define PX_PG_PCIE          3

// XUSB padctl (UPHY lives here)
#define PX_PADCTL           0x7009F000
#define PAD_ELPG_PROGRAM1   0x024
#define PAD_USB3_PAD_MUX    0x028
#define PAD_UPHY_PLL_P0_1   0x360
#define PAD_UPHY_PLL_P0_2   0x364
#define PAD_UPHY_PLL_P0_4   0x36C
#define PAD_UPHY_PLL_P0_5   0x370
#define PAD_UPHY_PLL_P0_8   0x37C

// BCM4356 / brcmfmac PCIe
#define BCM_BAR0_WINDOW     0x80                // config register: backplane window for the 4 KiB at BAR0+0
#define BCM_CC_BASE         0x18000000
#define BCM_ARMCR4_ID       0x83E
#define BCM_RAMBASE_4356    0x180000
#define BRCMF_SHARED_CONSOLE_ADDR_OFFSET 20
#define BRCMF_SHARED_MAX_RXBUFPOST_OFFSET 34
#define BRCMF_SHARED_RX_DATAOFFSET_OFFSET 36
#define BRCMF_SHARED_HTOD_MB_OFFSET       40
#define BRCMF_SHARED_DTOH_MB_OFFSET       44
#define BRCMF_SHARED_RING_INFO_OFFSET     48

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
STATIC BOOLEAN  mPcieMode  = FALSE;   // Sb* helpers go through PCIe BAR0 window instead of SDIO
STATIC BOOLEAN  mPexUp     = FALSE;   // controller + link up
STATIC BOOLEAN  mPexEnum   = FALSE;   // endpoint BARs assigned
STATIC UINTN    mBar0      = 0;
STATIC UINTN    mBar2      = 0;
STATIC UINT32   mBar0Size  = 0;
STATIC UINT32   mBar2Size  = 0;
STATIC UINT32   mBpWin     = 0xFFFFFFFF;
STATIC UINT32   mPexRstRun = 1;       // level of WIFI_RST (PH1) that means "chip running"
STATIC UINT32   mPexGen2   = 0;       // retrain to Gen2 after link-up (default: stay Gen1)
STATIC UINT32   mUphyLanes = BIT0;    // PEX lanes whose IDDQ is released (lane0 = Wi-Fi). USB3 will need lanes 5/6.

STATIC
VOID
Rm (
  IN UINTN   B,
  IN UINTN   O,
  IN UINT32  Clr,
  IN UINT32  Set
  )
{
  Wr (B, O, (Rd (B, O) & ~Clr) | Set);
}

STATIC
BOOLEAN
WaitBits (
  IN UINTN   B,
  IN UINTN   O,
  IN UINT32  Mask,
  IN UINT32  Val,
  IN UINTN   TimeoutUs
  )
{
  UINTN  T;

  for (T = 0; T < TimeoutUs; T += 10) {
    if ((Rd (B, O) & Mask) == Val) {
      return TRUE;
    }
    Us (10);
  }
  return (Rd (B, O) & Mask) == Val;
}

// ---------------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------------
STATIC
VOID
PexDumpAfi (
  IN CONST CHAR16  *Tag
  )
{
  Log (L"pex[%s]: AFI_CONFIG=%08x PCIE_CONFIG=%08x PEX1_CTRL=%08x INTR_CODE=%08x SIG=%08x FPCI_BAR0=%08x",
       Tag, Rd (PEX_AFI, AFI_CONFIGURATION), Rd (PEX_AFI, AFI_PCIE_CONFIG), Rd (PEX_AFI, AFI_PEX1_CTRL),
       Rd (PEX_AFI, AFI_INTR_CODE), Rd (PEX_AFI, AFI_INTR_SIGNATURE), Rd (PEX_AFI, AFI_FPCI_BAR0));
}

STATIC
VOID
PexDumpCar (
  IN CONST CHAR16  *Tag
  )
{
  Log (L"car[%s]: ENB_U=%08x RST_U=%08x ENB_W=%08x RST_W=%08x RST_Y=%08x PLLE_BASE=%08x MISC=%08x AUX=%08x XUSBIO=%08x OSC=%08x",
       Tag, Rd (CAR_BASE, CAR_ENB_U), Rd (CAR_BASE, CAR_RST_U), Rd (CAR_BASE, PX_CAR_ENB_W), Rd (CAR_BASE, PX_CAR_RST_W),
       Rd (CAR_BASE, PX_CAR_RST_Y), Rd (CAR_BASE, PX_CAR_PLLE_BASE), Rd (CAR_BASE, PX_CAR_PLLE_MISC), Rd (CAR_BASE, PX_CAR_PLLE_AUX),
       Rd (CAR_BASE, PX_CAR_XUSBIO_PLL), Rd (CAR_BASE, PX_CAR_OSC_CTRL));
}

// ---------------------------------------------------------------------------------------------
// PMC: power gate + IO pad DPD
// ---------------------------------------------------------------------------------------------
STATIC
BOOLEAN
PexPgOn (
  IN UINT32  Id
  )
{
  UINTN  I;

  if (Rd (PX_PMC, PX_PMC_PG_STATUS) & (1U << Id)) {
    Log (L"pg: partition %d already on", Id);
  } else {
    Wr (PX_PMC, PX_PMC_PG_TOGGLE, BIT8 | Id);
    for (I = 0; I < 200; I++) {
      if (Rd (PX_PMC, PX_PMC_PG_STATUS) & (1U << Id)) {
        break;
      }
      Us (100);
    }
    if (!(Rd (PX_PMC, PX_PMC_PG_STATUS) & (1U << Id))) {
      Log (L"pg: partition %d did not power on (STATUS=%08x)", Id, Rd (PX_PMC, PX_PMC_PG_STATUS));
      return FALSE;
    }
    Us (10);
  }
  Wr (PX_PMC, PX_PMC_PG_CLAMP, 1U << Id);
  Us (10);
  Log (L"pg: partition %d on, STATUS=%08x", Id, Rd (PX_PMC, PX_PMC_PG_STATUS));
  return TRUE;
}

// pex-bias (bit4), pex-clk1 (bit5), pex-clk2 (bit6): leave deep power down.
STATIC
BOOLEAN
PexIoPadsOn (
  VOID
  )
{
  UINT32  Mask = BIT4 | BIT5 | BIT6;
  BOOLEAN Ok;

  Wr (PX_PMC, PX_PMC_DPD_SAMPLE, 1);
  Wr (PX_PMC, PX_PMC_SEL_DPD_TIM, 0x7F);
  Wr (PX_PMC, PX_PMC_IO_DPD_REQ, BIT30 | Mask);          // CODE_OFF = power the pads
  Ok = WaitBits (PX_PMC, PX_PMC_IO_DPD_STS, Mask, 0, 250000);
  Wr (PX_PMC, PX_PMC_DPD_SAMPLE, 0);
  Log (L"pmc: IO pads pex-bias/clk1/clk2 %s (DPD_STS=%08x)", Ok ? L"on" : L"STILL IN DPD", Rd (PX_PMC, PX_PMC_IO_DPD_STS));
  return Ok;
}

// ---------------------------------------------------------------------------------------------
// PLLE (software enable) + hardware sequencer  (clk_plle_tegra210_enable / tegra210_plle_hw_sequence_start)
// ---------------------------------------------------------------------------------------------
STATIC
BOOLEAN
PlleEnable (
  VOID
  )
{
  UINT32  V;

  if (Rd (CAR_BASE, PX_CAR_PLLE_AUX) & BIT24) {
    Log (L"plle: HW sequencer already running (AUX=%08x)", Rd (CAR_BASE, PX_CAR_PLLE_AUX));
    return TRUE;
  }
  if (Rd (CAR_BASE, PX_CAR_PLLE_BASE) & BIT31) {
    Log (L"plle: already enabled (BASE=%08x MISC=%08x)", Rd (CAR_BASE, PX_CAR_PLLE_BASE), Rd (CAR_BASE, PX_CAR_PLLE_MISC));
    return TRUE;
  }
  Log (L"plle: enabling from OSC_CTRL=%08x (table assumes 38.4 MHz pll_ref: M=2 N=125 CML=14)", Rd (CAR_BASE, PX_CAR_OSC_CTRL));
  Rm (CAR_BASE, PX_CAR_PLLE_BASE, BIT30, 0);                                   // no lock override
  V = Rd (CAR_BASE, PX_CAR_PLLE_MISC);
  V |= BIT9 | BIT14 | BIT8;                                                    // LOCK_ENABLE | IDDQ_SW_CTRL | PLLE_PTS
  V &= ~(BIT13 | (3U << 4) | (2U << 2));                                       // IDDQ_SW_VALUE, VREG_BG_CTRL, VREG_CTRL
  Wr (CAR_BASE, PX_CAR_PLLE_MISC, V);
  Us (5);
  Rm (CAR_BASE, PX_CAR_PLLE_SS, 0, BIT10 | BIT11 | BIT12);                     // SS disabled while locking
  Rm (CAR_BASE, PX_CAR_PLLE_BASE, 0xFF | (0xFFU << 8) | (0x1FU << 24), 2 | (125U << 8) | (14U << 24));
  Us (1);
  Rm (CAR_BASE, PX_CAR_PLLE_BASE, 0, BIT31);                                   // enable
  if (!WaitBits (CAR_BASE, PX_CAR_PLLE_MISC, BIT11, BIT11, 20000)) {
    Log (L"plle: NO LOCK (BASE=%08x MISC=%08x)", Rd (CAR_BASE, PX_CAR_PLLE_BASE), Rd (CAR_BASE, PX_CAR_PLLE_MISC));
    return FALSE;
  }
  V = Rd (CAR_BASE, PX_CAR_PLLE_SS);
  V &= ~(BIT14 | BIT15 | 0x1FF | (0xFFU << 16) | (0x3FU << 24));
  V |= 0x21 | (1U << 16) | (0x23U << 24);
  Wr (CAR_BASE, PX_CAR_PLLE_SS, V);
  V &= ~(BIT12 | BIT10);
  Wr (CAR_BASE, PX_CAR_PLLE_SS, V);
  Us (1);
  V &= ~BIT11;
  Wr (CAR_BASE, PX_CAR_PLLE_SS, V);
  Us (1);
  Log (L"plle: locked, BASE=%08x MISC=%08x SS=%08x", Rd (CAR_BASE, PX_CAR_PLLE_BASE), Rd (CAR_BASE, PX_CAR_PLLE_MISC), Rd (CAR_BASE, PX_CAR_PLLE_SS));
  return TRUE;
}

STATIC
VOID
PlleHwSequence (
  VOID
  )
{
  UINT32  V;

  if (Rd (CAR_BASE, PX_CAR_PLLE_AUX) & BIT24) {
    return;
  }
  if (!(Rd (CAR_BASE, PX_CAR_PLLE_MISC) & BIT11)) {
    Log (L"plle: hw sequence skipped, PLLE not locked");
    return;
  }
  Rm (CAR_BASE, PX_CAR_PLLE_MISC, BIT14, 0);                                   // IDDQ_SW_CTRL off
  V = Rd (CAR_BASE, PX_CAR_PLLE_AUX);
  V |= BIT3 | BIT31;                                                           // USE_LOCKDET | SS_SEQ_INCLUDE
  V &= ~(BIT4 | BIT6);                                                         // ENABLE_SWCTL | SS_SWCTL
  Wr (CAR_BASE, PX_CAR_PLLE_AUX, V);
  Us (1);
  V |= BIT24;                                                                  // SEQ_ENABLE
  Wr (CAR_BASE, PX_CAR_PLLE_AUX, V);
  Us (1);
  Log (L"plle: HW sequencer started, AUX=%08x", Rd (CAR_BASE, PX_CAR_PLLE_AUX));
}

// ---------------------------------------------------------------------------------------------
// XUSB padctl access + UPHY PLL  (tegra210_pex_uphy_enable)
// ---------------------------------------------------------------------------------------------
STATIC
BOOLEAN
PadctlUp (
  VOID
  )
{
  if ((Rd (CAR_BASE, PX_CAR_ENB_W) & PX_W_XUSB) != 0 && (Rd (CAR_BASE, PX_CAR_RST_W) & (PX_W_XUSB | PX_W_PADCTL)) == 0) {
    return TRUE;
  }
  Log (L"padctl: enabling clock + releasing reset (xusb gate id143, padctl reset id142)");
  Wr (CAR_BASE, PX_CAR_ENB_W_SET, PX_W_XUSB);
  Wr (CAR_BASE, PX_CAR_RST_W_SET, PX_W_XUSB);
  Us (2);
  Wr (CAR_BASE, PX_CAR_RST_W_CLR, PX_W_XUSB);
  Wr (CAR_BASE, PX_CAR_RST_W_CLR, PX_W_PADCTL);
  Us (2);
  return (Rd (CAR_BASE, PX_CAR_RST_W) & (PX_W_XUSB | PX_W_PADCTL)) == 0;
}

STATIC
BOOLEAN
UphyWait (
  IN UINTN        Off,
  IN UINT32       Mask,
  IN UINT32       Val,
  IN CONST CHAR16 *What
  )
{
  if (!WaitBits (PX_PADCTL, Off, Mask, Val, 100000)) {
    Log (L"uphy: TIMEOUT waiting for %s (reg %03x = %08x)", What, Off, Rd (PX_PADCTL, Off));
    return FALSE;
  }
  return TRUE;
}

STATIC
BOOLEAN
UphyEnable (
  VOID
  )
{
  UINT32  I;

  if (!PadctlUp ()) {
    Log (L"uphy: padctl not accessible");
    return FALSE;
  }
  Log (L"uphy: PLL_P0 CTL1=%08x CTL2=%08x CTL4=%08x CTL5=%08x CTL8=%08x PAD_MUX=%08x XUSBIO=%08x",
       Rd (PX_PADCTL, PAD_UPHY_PLL_P0_1), Rd (PX_PADCTL, PAD_UPHY_PLL_P0_2), Rd (PX_PADCTL, PAD_UPHY_PLL_P0_4),
       Rd (PX_PADCTL, PAD_UPHY_PLL_P0_5), Rd (PX_PADCTL, PAD_UPHY_PLL_P0_8), Rd (PX_PADCTL, PAD_USB3_PAD_MUX), Rd (CAR_BASE, PX_CAR_XUSBIO_PLL));

  if (Rd (CAR_BASE, PX_CAR_XUSBIO_PLL) & BIT24) {
    Log (L"uphy: XUSBIO PLL HW sequencer already running, skipping PLL calibration");
  } else {
    Wr (CAR_BASE, PX_CAR_RST_Y_CLR, PX_Y_UPHY);                                  // release PEX_USB_UPHY reset (id 205)
    Us (10);
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_2, 0xFFFFFFU << 4, 0x136U << 4);              // CAL_CTRL
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_5, 0xFFU << 16, 0x2AU << 16);                 // DCO_CTRL
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_1, 0, BIT4);                                  // PWR_OVRD
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_2, 0, BIT2);                                  // CAL_OVRD
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_8, 0, BIT15);                                 // RCAL_OVRD
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_4, (3U << 12) | (0xFU << 4), (2U << 12) | BIT15);   // TXCLKREF_SEL=USB, TXCLKREF_EN, REFCLK_SEL=0
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_1, (3U << 16) | (0xFFU << 20), 0x19U << 20);  // FREQ_MDIV=0, FREQ_NDIV=0x19
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_1, BIT0, 0);                                  // IDDQ off
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_1, 3U << 1, 0);                               // SLEEP = 0
    Us (20);
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_4, 0, BIT8);                                  // REFCLKBUF_EN
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_2, 0, BIT0);                                  // CAL_EN
    if (!UphyWait (PAD_UPHY_PLL_P0_2, BIT1, BIT1, L"CAL_DONE")) {
      return FALSE;
    }
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_2, BIT0, 0);
    if (!UphyWait (PAD_UPHY_PLL_P0_2, BIT1, 0, L"CAL_DONE clear")) {
      return FALSE;
    }
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_1, 0, BIT3);                                  // ENABLE
    if (!UphyWait (PAD_UPHY_PLL_P0_1, BIT15, BIT15, L"PLL LOCKDET")) {
      return FALSE;
    }
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_8, 0, BIT12 | BIT13);                         // RCAL_EN | RCAL_CLK_EN
    if (!UphyWait (PAD_UPHY_PLL_P0_8, BIT31, BIT31, L"RCAL_DONE")) {
      return FALSE;
    }
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_8, BIT12, 0);
    if (!UphyWait (PAD_UPHY_PLL_P0_8, BIT31, 0, L"RCAL_DONE clear")) {
      return FALSE;
    }
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_8, BIT13, 0);
    // tegra210_xusb_pll_hw_control_enable()
    Rm (CAR_BASE, PX_CAR_XUSBIO_PLL, BIT2 | BIT0, BIT6 | BIT13);                 // clear CLK_ENABLE_SWCTL|PADPLL_RESET_SWCTL, set USE_LOCKDET|SLEEP_IDDQ
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_1, BIT4, 0);
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_2, BIT2, 0);
    Rm (PX_PADCTL, PAD_UPHY_PLL_P0_8, BIT15, 0);
    Us (20);
    Rm (CAR_BASE, PX_CAR_XUSBIO_PLL, 0, BIT24);                                  // tegra210_xusb_pll_hw_sequence_start()
    Log (L"uphy: PLL calibrated + locked, XUSBIO=%08x CTL1=%08x", Rd (CAR_BASE, PX_CAR_XUSBIO_PLL), Rd (PX_PADCTL, PAD_UPHY_PLL_P0_1));
  }

  // lane muxing: pcie-0 = function "pcie-x1" (0) in USB3_PAD_MUX[13:12]; release IDDQ for the lanes we use
  Rm (PX_PADCTL, PAD_USB3_PAD_MUX, 3U << 12, 0);
  for (I = 0; I < 7; I++) {
    if (mUphyLanes & (1U << I)) {
      Rm (PX_PADCTL, PAD_USB3_PAD_MUX, 0, 1U << (1 + I));
    }
  }
  PlleHwSequence ();

  // tegra210_aux_mux_lp0_clamp_disable()
  Rm (PX_PADCTL, PAD_ELPG_PROGRAM1, BIT29, 0);
  Us (150);
  Rm (PX_PADCTL, PAD_ELPG_PROGRAM1, BIT30, 0);
  Us (150);
  Rm (PX_PADCTL, PAD_ELPG_PROGRAM1, BIT31, 0);
  Log (L"uphy: done. PAD_MUX=%08x ELPG1=%08x", Rd (PX_PADCTL, PAD_USB3_PAD_MUX), Rd (PX_PADCTL, PAD_ELPG_PROGRAM1));
  return TRUE;
}

// ---------------------------------------------------------------------------------------------
// PCIe configuration space access (Tegra "ECAM-like" window)
// ---------------------------------------------------------------------------------------------
STATIC
UINT32
PexCfgRd (
  IN UINT32  Bus,
  IN UINT32  Dev,
  IN UINT32  Fn,
  IN UINT32  Where
  )
{
  UINT32  Off;

  if (Bus == 0) {
    if (Dev != (PEX_PORT + 1U) || Fn != 0) {
      return 0xFFFFFFFF;
    }
    return Rd (PEX_RP_BASE + PEX_PORT * 0x1000, Where & 0xFFC);
  }
  Off = ((Where & 0xF00U) << 16) | (Bus << 16) | (Dev << 11) | (Fn << 8) | (Where & 0xFF);
  Wr (PEX_AFI, AFI_FPCI_BAR0, 0xFE100000U + ((Off & ~0xFFFU) >> 8));
  (VOID)Rd (PEX_AFI, AFI_FPCI_BAR0);
  return Rd (PEX_CS, Off & 0xFFC);
}

STATIC
VOID
PexCfgWr (
  IN UINT32  Bus,
  IN UINT32  Dev,
  IN UINT32  Fn,
  IN UINT32  Where,
  IN UINT32  V
  )
{
  UINT32  Off;

  if (Bus == 0) {
    if (Dev == (PEX_PORT + 1U) && Fn == 0) {
      Wr (PEX_RP_BASE + PEX_PORT * 0x1000, Where & 0xFFC, V);
    }
    return;
  }
  Off = ((Where & 0xF00U) << 16) | (Bus << 16) | (Dev << 11) | (Fn << 8) | (Where & 0xFF);
  Wr (PEX_AFI, AFI_FPCI_BAR0, 0xFE100000U + ((Off & ~0xFFFU) >> 8));
  (VOID)Rd (PEX_AFI, AFI_FPCI_BAR0);
  Wr (PEX_CS, Off & 0xFFC, V);
}

// ---------------------------------------------------------------------------------------------
// Root port (tegra_pcie_port_enable / tegra_pcie_port_check_link)
// ---------------------------------------------------------------------------------------------
STATIC
VOID
PexPortReset (
  VOID
  )
{
  // PERST# from the SoC: RST bit low = asserted
  Rm (PEX_AFI, AFI_PEX1_CTRL, BIT0, 0);
  Ms (2);
  Rm (PEX_AFI, AFI_PEX1_CTRL, 0, BIT0);
}

STATIC
VOID
PexRpInit (
  VOID
  )
{
  UINTN  Rp = PEX_RP_BASE + PEX_PORT * 0x1000;

  Rm (PEX_AFI, AFI_PEX1_CTRL, 0, BIT3 | BIT1 | BIT4);       // REFCLK_EN | CLKREQ_EN | OVERRIDE_EN
  PexPortReset ();
  Rm (Rp, RP_VEND_CTL2, 0, BIT7);                           // PCA_ENABLE (T210 force_pca_enable)
  Rm (Rp, RP_VEND_CTL1, 0, BIT13);                          // AER
  Rm (Rp, RP_VEND_XP, 0, BIT27 | BIT28);                    // opportunistic ACK / UPDATEFC
  Rm (Rp, RP_VEND_XP_BIST, 0, BIT28);
  Rm (Rp, RP_PRIV_MISC, (0x7FU << 16) | (0x7FU << 24), BIT23 | BIT31 | (0xFU << 16) | (0xFU << 24));
  // T210 ectl
  Rm (Rp, RP_ECTL_2_R1, 0xFFFF, 0x0000000F);
  Rm (Rp, RP_ECTL_4_R1, 0xFFFFU << 16, 0x67U << 16);
  Wr (Rp, RP_ECTL_5_R1, 0x55010000);
  Wr (Rp, RP_ECTL_6_R1, 0x00000001);
  Rm (Rp, RP_ECTL_2_R2, 0xFFFF, 0x0000008F);
  Rm (Rp, RP_ECTL_4_R2, 0xFFFFU << 16, 0xC7U << 16);
  Wr (Rp, RP_ECTL_5_R2, 0x55010000);
  Wr (Rp, RP_ECTL_6_R2, 0x00000001);
  // sw fixups: deskew pulse width, FC threshold, advertise Gen1 only first
  Rm (Rp, RP_VEND_CTL0, 0xFU << 12, 0x9U << 12);
  Rm (Rp, RP_VEND_XP, 0xFFU << 18, 0x01800000);
  Rm (Rp, RP_LINK_CTL_STATUS2, 0xF, 1);
}

STATIC
BOOLEAN
PexCheckLink (
  VOID
  )
{
  UINTN    Rp = PEX_RP_BASE + PEX_PORT * 0x1000;
  UINT32   Try;

  Rm (Rp, RP_PRIV_MISC, 0xF, 0xE);                          // override presence detect: endpoint present
  for (Try = 0; Try < 3; Try++) {
    if (WaitBits (Rp, RP_VEND_XP, BIT30, BIT30, 250000)) {                       // DL_UP
      if (WaitBits (Rp, RP_LINK_CTL_STATUS, 0x20000000, 0x20000000, 250000)) {   // DL_LINK_ACTIVE
        Log (L"pex: link UP (try %d) VEND_XP=%08x LINK_CTL_STATUS=%08x", Try, Rd (Rp, RP_VEND_XP), Rd (Rp, RP_LINK_CTL_STATUS));
        return TRUE;
      }
    }
    Log (L"pex: link down (try %d) VEND_XP=%08x LINK_CTL_STATUS=%08x PRIV_MISC=%08x -> port reset", Try, Rd (Rp, RP_VEND_XP),
         Rd (Rp, RP_LINK_CTL_STATUS), Rd (Rp, RP_PRIV_MISC));
    PexPortReset ();
  }
  return FALSE;
}

// ---------------------------------------------------------------------------------------------
// Backplane window over BAR0 (brcmf_pcie_buscore_*)
// ---------------------------------------------------------------------------------------------
STATIC
VOID
BpSetWin (
  IN UINT32  Addr
  )
{
  UINT32  Base = Addr & ~0xFFFU;

  if (Base != mBpWin) {
    PexCfgWr (1, 0, 0, BCM_BAR0_WINDOW, Base);
    mBpWin = Base;
  }
}

STATIC
BOOLEAN
BpRd32 (
  IN  UINT32  Addr,
  OUT UINT32  *V
  )
{
  BpSetWin (Addr);
  *V = Rd (mBar0, Addr & 0xFFC);
  return TRUE;
}

STATIC
BOOLEAN
BpWr32 (
  IN UINT32  Addr,
  IN UINT32  V
  )
{
  BpSetWin (Addr);
  Wr (mBar0, Addr & 0xFFC, V);
  return TRUE;
}

// TCM (BAR2) accesses: 32-bit only.
STATIC UINT32 TcmRd32 (IN UINT32 Off) { return Rd (mBar2, Off & ~3U); }
STATIC VOID   TcmWr32 (IN UINT32 Off, IN UINT32 V) { Wr (mBar2, Off & ~3U, V); }

STATIC
UINT32
Le32 (
  IN CONST UINT8  *P
  )
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC
VOID
TcmWriteBuf (
  IN UINT32       Off,
  IN CONST UINT8  *Data,
  IN UINTN        Len
  )
{
  UINTN   I;
  UINT8   Tail[4];

  for (I = 0; I + 4 <= Len; I += 4) {
    TcmWr32 (Off + (UINT32)I, Le32 (Data + I));
  }
  if (I < Len) {
    Tail[0] = Tail[1] = Tail[2] = Tail[3] = 0;
    CopyMem (Tail, Data + I, Len - I);
    TcmWr32 (Off + (UINT32)I, Le32 (Tail));
  }
}
