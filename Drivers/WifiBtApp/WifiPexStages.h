/** @file
  WifiPexStages.h - shell-visible stages of the BCM4356 Wi-Fi bring-up over PCIe (uses the low level code in WifiPcie.h).

  Stages (each logs + flushes; the LAST log line names the step that failed):
    pex    : radio power, PMC/CAR/PLLE/UPHY, AFI, root port, link training, enumeration (BARs)     [PexUp + PexEnum]
    pchip  : backplane window over BAR0, chip id, EROM core discovery, ARM CR4 halt, TCM size                [PexChip]
    pfw    : firmware + NVRAM -> TCM, release CR4, wait for the shared-area pointer, dump it        [PexFw]
    pcon   : dump the firmware console ring (the firmware's own boot log)                          [PexConsole]
    poff   : everything down again                                                                 [PexOff]

  WifiMsgbuf.h and WifiScan.h add control-ring queries and passive scanning.
  Reaching "pfw: firmware is running" establishes chip boot only; association,
  packet transport and Windows adapter integration remain separate stages.

  DRAFT, UNTESTED. All device accesses are naturally aligned 32-bit MMIO (see Rd/Wr/TcmRd32/TcmWr32).
**/

#include "WifiErom.h"
#include "WifiPciLayout.h"

STATIC UINT32  mChipId     = 0;
STATIC UINT32  mArmBase    = 0;            // discovered from EROM
STATIC UINT32  mArmWrap    = 0;
STATIC UINT32  mD11Wrap    = 0;
STATIC UINT32  mPcieCore   = 0;
STATIC UINT32  mPcieRev    = 0;
STATIC UINT32  mChipOverrides = 0;         // explicit arm/wrap/d11/size options
STATIC UINT32  mRamBase    = BCM_RAMBASE_4356;
STATIC UINT32  mRamSize    = 0;
STATIC UINT32  mRamCapacity = 0;
STATIC UINT32  mSharedAddr = 0;
STATIC BOOLEAN mChipReady = FALSE;

#define AFI_PLLE_CONTROL     0x160
#define AFI_AXI_BAR1_SZ_X    0x04
#define PEX_CAR_ENB_V_SET    0x440

// ---------------------------------------------------------------------------------------------
STATIC
VOID
PexCfgDump (
  IN UINT32  Bus,
  IN UINT32  Dev,
  IN UINT32  Count
  )
{
  UINT32  I;

  for (I = 0; I < Count; I += 4) {
    Log (L"  cfg[%d:%d.0][%02x] = %08x %08x %08x %08x", Bus, Dev, I * 4,
         PexCfgRd (Bus, Dev, 0, I * 4), PexCfgRd (Bus, Dev, 0, I * 4 + 4),
         PexCfgRd (Bus, Dev, 0, I * 4 + 8), PexCfgRd (Bus, Dev, 0, I * 4 + 12));
  }
}

STATIC
VOID
PexAfiErr (
  IN CONST CHAR16  *Tag
  )
{
  Log (L"afi[%s]: INTR_CODE=%08x SIGNATURE=%08x (non-zero code = AFI saw a FPCI/AXI error)", Tag,
       Rd (PEX_AFI, AFI_INTR_CODE), Rd (PEX_AFI, AFI_INTR_SIGNATURE));
}

// ---------------------------------------------------------------------------------------------
// pex: power + controller + link
// ---------------------------------------------------------------------------------------------
STATIC
BOOLEAN
PexUp (
  VOID
  )
{
  UINT32  V;

  Log (L"pex: ---- bring-up, root port %d (pci@2,0), lane pcie-0, rst-run-level=%d gen2=%d ----", PEX_PORT, mPexRstRun, mPexGen2);
  PexDumpCar (L"start");
  mPexUp   = FALSE;
  mPexEnum = FALSE;
  mChipReady = FALSE;
  mSharedAddr = 0;
  mBpWin   = 0xFFFFFFFF;

  Log (L"pex[1] hold Wi-Fi reset low, configure pins and 32 kHz");
  // Wi-Fi bring-up must leave the already configured Bluetooth/UART pins alone.
  PinsWifi ();
  Step32k (TRUE);
  GpioH (H0 | H1, 0, TRUE);
  Ms (10);

  Log (L"pex[2] assert resets pex/afi/pcie_x, enable mselect clock (afi parent)");
  Wr (CAR_BASE, PX_CAR_RST_U_SET, PX_U_PEX | PX_U_AFI | PX_U_PCIEX);
  Wr (CAR_BASE, PEX_CAR_ENB_V_SET, BIT3);

  Log (L"pex[3] PMC power partition PCIE (id %d) + IO pads", PX_PG_PCIE);
  if (!PexPgOn (PX_PG_PCIE)) {
    return FALSE;
  }
  // Switchroot tegra210b01_pcie_data omits config_pex_io_dpd. Erista
  // retains that sequence; B01 uses UPHY preparation without these pad writes.
  if (!PexIsB01 () && !PexIoPadsOn ()) return FALSE;
  Log (L"pex: SoC profile %s, crossbar x4+x1", PexIsB01 () ? L"T214/B01" : L"T210");

  Log (L"pex[4] clocks: afi, pex; PLLE + cml0");
  Wr (CAR_BASE, PX_CAR_ENB_U_SET, PX_U_AFI | PX_U_PEX);
  if (!PlleEnable ()) {
    return FALSE;
  }
  Rm (CAR_BASE, PX_CAR_PLLE_AUX, 0, BIT0);                      // cml0 gate (clk-tegra210.c: PLLE_AUX bit0)
  Us (20);
  PexDumpCar (L"clocks");

  Log (L"pex[5] release AFI reset, controller setup");
  Wr (CAR_BASE, PX_CAR_RST_U_CLR, PX_U_AFI);
  Us (20);
  V = Rd (PEX_AFI, AFI_PLLE_CONTROL);
  V &= ~BIT9;                                                   // BYPASS_PADS2PLLE_CONTROL
  V |= BIT1;                                                    // PADS2PLLE_CONTROL_EN
  Wr (PEX_AFI, AFI_PLLE_CONTROL, V);
  Wr (PEX_AFI, AFI_PEXBIAS_CTRL_0, 0);
  V = Rd (PEX_AFI, AFI_PCIE_CONFIG);
  V &= ~(0xFU << 20);
  V |= 0xEU | (1U << 20) | (7U << 29);                          // DISABLE_ALL | XBAR x4+x1 | CLKREQ_GPIO_ALL
  V &= ~(1U << (PEX_PORT + 1));                                 // enable our port
  V &= ~(1U << (PEX_PORT + 29));
  Wr (PEX_AFI, AFI_PCIE_CONFIG, V);
  Rm (PEX_AFI, AFI_FUSE, BIT2, 0);                              // Gen2 not disabled
  Rm (PEX_AFI, AFI_CONFIGURATION, 0, BIT0 | BIT31);             // EN_FPCI | CLKEN_OVERRIDE
  Wr (PEX_AFI, AFI_FPCI_ERR_MASKS, 0);                          // no exceptions: a bad access must not abort the CPU
  Wr (PEX_AFI, AFI_INTR_MASK, 0);                               // no interrupts: we poll
  Wr (PEX_AFI, AFI_AFI_INTR_ENABLE, 0);
  Wr (PEX_AFI, AFI_SM_INTR_ENABLE, 0);

  Log (L"pex[6] address translations (cfg window %08x, mem window %08x+%08x)", PEX_CS, PEX_MEM_BASE, PEX_MEM_SIZE);
  Wr (PEX_AFI, AFI_AXI_BAR0_START, PEX_CS);
  Wr (PEX_AFI, AFI_AXI_BAR0_SZ, 0x10000000 >> 12);
  Wr (PEX_AFI, AFI_AXI_BAR3_START, PEX_MEM_BASE);
  Wr (PEX_AFI, AFI_AXI_BAR3_SZ, PEX_MEM_SIZE >> 12);
  Wr (PEX_AFI, AFI_FPCI_BAR3, (((PEX_MEM_BASE >> 12) & 0x0FFFFFFF) << 4) | 1);
  Wr (PEX_AFI, AFI_AXI_BAR4_START, 0);
  Wr (PEX_AFI, AFI_AXI_BAR4_SZ, 0);
  Wr (PEX_AFI, AFI_FPCI_BAR4, 0);
  Wr (PEX_AFI, AFI_AXI_BAR5_START, 0);
  Wr (PEX_AFI, AFI_AXI_BAR5_SZ, 0);
  Wr (PEX_AFI, AFI_FPCI_BAR5, 0);
  Wr (PEX_AFI, AFI_MSI_FPCI_BAR_ST, 0);
  Wr (PEX_AFI, AFI_MSI_BAR_SZ, 0);
  Wr (PEX_AFI, AFI_MSI_AXI_BAR_ST, 0);
  PexDumpAfi (L"translations");

  Log (L"pex[7] release PEX reset, UPHY PLL + pcie lane");
  Wr (CAR_BASE, PX_CAR_RST_U_CLR, PX_U_PEX);
  Us (20);
  if (!UphyEnable ()) {
    PexDumpCar (L"uphy-fail");
    return FALSE;
  }
  Wr (PEX_PADS, PADS_REFCLK_CFG0, 0x90B890B8);

  // Vali's boot-default-output-low identifies PH0 as RF_DISABLE, not an
  // active-high power enable. PH1 is high in the running Linux profile.
  Log (L"pex[8] Wi-Fi PH0=%d, PH1 reset -> run level %d", PexIsB01 () ? 0 : 1, mPexRstRun);
  GpioH (H0, PexIsB01 () ? 0 : 1, TRUE);
  Ms (20);
  GpioH (H1, mPexRstRun, TRUE);
  GpioH (H2, 0, FALSE);
  Ms (100);

  Log (L"pex[9] root port init + LTSSM start");
  PexRpInit ();
  Wr (CAR_BASE, PX_CAR_RST_U_CLR, PX_U_PCIEX);
  Ms (5);
  PexAfiErr (L"before-link");
  if (!PexCheckLink ()) {
    Log (L"pex: LINK DOWN. Things to try: `wifibt pex rst=%d` (opposite WIFI_RST polarity), longer power-up delay, 32 kHz clock, PLLE/UPHY lock, lane mapping", mPexRstRun ? 0 : 1);
    PexDumpAfi (L"link-down");
    PexDumpCar (L"link-down");
    Log (L"pex: root port cfg dump:");
    PexCfgDump (0, PEX_PORT + 1, 16);
    return FALSE;
  }
  if (mPexGen2) {
    Log (L"pex: retraining to Gen2");
    Rm (PEX_RP_BASE + PEX_PORT * 0x1000, RP_LINK_CTL_STATUS2, 0xF, 2);
    Rm (PEX_RP_BASE + PEX_PORT * 0x1000, RP_LINK_CTL_STATUS, 0, BIT5);
    Ms (50);
  }
  mPexUp = TRUE;
  Log (L"pex: link status reg %08x", Rd (PEX_RP_BASE + PEX_PORT * 0x1000, RP_LINK_CTL_STATUS));
  return TRUE;
}

STATIC
UINT32
PexBarSize (
  IN UINT32  Where
  )
{
  UINT32  Old = PexCfgRd (1, 0, 0, Where);
  UINT32  Sz;

  PexCfgWr (1, 0, 0, Where, 0xFFFFFFFF);
  Sz = PexCfgRd (1, 0, 0, Where);
  PexCfgWr (1, 0, 0, Where, Old);
  return Sz;
}

STATIC
BOOLEAN
PexEnum (
  VOID
  )
{
  UINT32  Id;
  UINT32  T;
  UINT32  S0;
  UINT32  S2;
  UINT32  Sz0;
  UINT32  Sz2;
  UINT32  A0;
  UINT32  A2;
  UINT32  Cmd;
  UINT32  Probe;
  UINT32  BridgeWindow;

  if (!mPexUp) {
    Log (L"enum: run `pex` first");
    return FALSE;
  }
  Id = PexCfgRd (0, PEX_PORT + 1, 0, 0);
  Log (L"enum: root port id=%08x class=%08x", Id, PexCfgRd (0, PEX_PORT + 1, 0, 8));
  Log (L"enum: root port bus numbers pri0 sec1 sub1; size BARs before opening memory window");
  PexCfgWr (0, PEX_PORT + 1, 0, 0x18, 0x00010100);
  PexCfgWr (0, PEX_PORT + 1, 0, 0x20, 0x0000FFF0);
  PexCfgWr (0, PEX_PORT + 1, 0, 0x24, 0x0000FFF0);               // prefetch window closed (base > limit)
  Cmd = PexCfgRd (0, PEX_PORT + 1, 0, 4) & 0xFFFF;
  PexCfgWr (0, PEX_PORT + 1, 0, 4, Cmd | 0x6);

  Id = 0xFFFFFFFF;
  for (T = 0; T < 200; T++) {
    Id = PexCfgRd (1, 0, 0, 0);
    if (Id != 0xFFFFFFFF && Id != 0xFFFF0001 && (Id & 0xFFFF) != 0xFFFF && Id != 0) {
      break;
    }
    Ms (10);
  }
  Log (L"enum: endpoint 1:0.0 id=%08x (expect 43ec14e4 for BCM4356)", Id);
  PexAfiErr (L"after-ep-probe");
  if (Id == 0xFFFFFFFF || (Id & 0xFFFF) == 0xFFFF || Id == 0xFFFF0001 || Id == 0) {
    Log (L"enum: no endpoint answers (link up but config reads fail: check cfg window mapping / PEX_CS)");
    return FALSE;
  }
  if ((Id & 0xFFFF) != 0x14E4) {
    Log (L"enum: WARNING vendor is not Broadcom");
  }
  PexCfgDump (1, 0, 16);

  Cmd = PexCfgRd (1, 0, 0, 4) & 0xFFFF;
  PexCfgWr (1, 0, 0, 4, Cmd & ~6U);
  Sz0 = PexBarSize (0x10);
  Sz2 = PexBarSize (0x18);
  Log (L"enum: BAR0 sizing readback=%08x  BAR2 sizing readback=%08x (bit2:1 = 10b means 64-bit BAR)", Sz0, Sz2);
  S0 = ~(Sz0 & ~0xFU) + 1;
  S2 = ~(Sz2 & ~0xFU) + 1;
  if (S0 == 0 || S2 == 0 || S0 > 0x100000 || S2 > 0x1000000) {
    Log (L"enum: implausible BAR sizes %08x / %08x", S0, S2);
    return FALSE;
  }
  if ((Sz0 & 1) || (Sz2 & 1) ||
      !PexMemoryLayout (PEX_MEM_BASE, PEX_MEM_SIZE, S0, S2, &A0, &A2, &BridgeWindow)) {
    Log (L"enum: BARs do not fit the outbound memory aperture");
    return FALSE;
  }
  Log (L"enum: BAR0 size %08x -> %08x, BAR2 size %08x -> %08x", S0, A0, S2, A2);
  PexCfgWr (1, 0, 0, 0x10, A0 | (Sz0 & 0xF));
  if ((Sz0 & 6) == 4) {
    PexCfgWr (1, 0, 0, 0x14, 0);
  }
  PexCfgWr (1, 0, 0, 0x18, A2 | (Sz2 & 0xF));
  if ((Sz2 & 6) == 4) {
    PexCfgWr (1, 0, 0, 0x1C, 0);
  }
  PexCfgWr (0, PEX_PORT + 1, 0, 0x20, BridgeWindow);
  if ((PexCfgRd (1, 0, 0, 0x10) & ~0xFU) != A0 ||
      (PexCfgRd (1, 0, 0, 0x18) & ~0xFU) != A2 ||
      (PexCfgRd (0, PEX_PORT + 1, 0, 0x20) & 0xFFF0FFF0U) != BridgeWindow) {
    Log (L"enum: BAR or bridge memory-window readback mismatch");
    return FALSE;
  }
  // Enumeration only needs memory decoding. MsgInitialize or the Windows
  // driver grants bus mastering after its own DMA addresses are published.
  PexCfgWr (1, 0, 0, 4, (Cmd | 2U) & ~4U);
  if ((PexCfgRd (1, 0, 0, 4) & 6U) != 2U) {
    Log (L"enum: endpoint did not accept memory decoding with DMA stopped");
    return FALSE;
  }
  Log (L"enum: bridge memory base/limit=%08x, covering BAR2 through %08x",
       BridgeWindow, A2 + S2 - 1U);
  Log (L"enum: BAR0=%08x BAR2=%08x cmd=%08x", PexCfgRd (1, 0, 0, 0x10), PexCfgRd (1, 0, 0, 0x18), PexCfgRd (1, 0, 0, 4));

  mBar0     = A0;
  mBar2     = A2;
  mBar0Size = S0;
  mBar2Size = S2;
  Probe = Rd (mBar0, 0);
  Log (L"enum: first dword of BAR0 window = %08x (chipcommon chipid register if window is default)", Probe);
  PexAfiErr (L"after-bar-read");
  mPexEnum = TRUE;
  return TRUE;
}

// ---------------------------------------------------------------------------------------------
// pchip: backplane
// ---------------------------------------------------------------------------------------------
STATIC
BOOLEAN
PexChip (
  VOID
  )
{
  UINT32    V;
  UINT32    Cap;
  UINT32    Banks;
  UINT32    I;
  UINT32    Info;
  UINT32    Size = 0;
  UINT32    Erom;
  UINT32    *Words;
  PEX_CORE  Cores[32];
  UINTN     Count;
  UINT32    Found = 0;
  BOOLEAN   Parsed;

  mChipReady = FALSE;
  if (!mPexEnum) {
    Log (L"chip: run `pex` first");
    return FALSE;
  }
  mPcieMode = TRUE;
  mBpWin = 0xFFFFFFFFU;
  if (!BpRd32 (BCM_CC_BASE, &V)) return FALSE;
  mChipId = V;
  Log (L"chip: chipid=%08x chip=%04x rev=%d pkg=%d", V, V & 0xFFFF, (V >> 16) & 0xF, (V >> 20) & 0xF);
  if ((V & 0xFFFF) != 0x4356) {
    Log (L"chip: this firmware path requires the Switch BCM4356");
    return FALSE;
  }
  if (!BpRd32 (BCM_CC_BASE + 0xFC, &Erom) || (Erom & 0xFFF) ||
      Erom < 0x18000000U || Erom >= 0x19000000U) {
    Log (L"chip: invalid EROM pointer");
    return FALSE;
  }
  Words = AllocatePool (4096);
  if (Words == NULL) return FALSE;
  // One EROM page; the parser consumes extension words and the end marker.
  for (I = 0; I < 1024; I++) {
    if (!BpRd32 (Erom + I * 4, &Words[I])) {
      FreePool (Words);
      return FALSE;
    }
  }
  Parsed = PexEromParse (Words, 1024, Cores, ARRAY_SIZE (Cores), &Count);
  FreePool (Words);
  if (!Parsed) {
    Log (L"chip: invalid or truncated EROM table at %08x", Erom);
    return FALSE;
  }
  for (I = 0; I < Count; I++) {
    Log (L"chip: core %03x rev=%d base=%08x wrapper=%08x", Cores[I].Id,
         Cores[I].Revision, Cores[I].Base, Cores[I].Wrapper);
    if (Cores[I].Id == 0x83E && Cores[I].Wrapper != 0 && !(Found & BIT0)) {
      if (!(mChipOverrides & BIT0)) mArmBase = Cores[I].Base;
      if (!(mChipOverrides & BIT1)) mArmWrap = Cores[I].Wrapper;
      Found |= BIT0;
    } else if (Cores[I].Id == 0x812 && Cores[I].Wrapper != 0 && !(Found & BIT1)) {
      if (!(mChipOverrides & BIT2)) mD11Wrap = Cores[I].Wrapper;
      Found |= BIT1;
    } else if (Cores[I].Id == 0x83C && !(Found & BIT2)) {
      mPcieCore = Cores[I].Base;
      mPcieRev = Cores[I].Revision;
      Found |= BIT2;
    }
  }
  if (Found != (BIT0 | BIT1 | BIT2) || !mArmBase || !mArmWrap || !mD11Wrap ||
      ((mArmBase | mArmWrap | mD11Wrap) & 0xFFF)) {
    Log (L"chip: missing CR4/D11/PCIe core or invalid explicit override");
    return FALSE;
  }
  Log (L"chip: CR4=%08x wrapper=%08x D11 wrapper=%08x PCIe=%08x rev=%d overrides=%x",
       mArmBase, mArmWrap, mD11Wrap, mPcieCore, mPcieRev, mChipOverrides);
  mSharedAddr = 0;
  if (!CoreDisable (mD11Wrap, 0x0C, 0x04) || !CoreReset (mArmWrap, 0x20, 0x20, 0x20)) {
    Log (L"chip: could not halt D11/CR4");
    return FALSE;
  }
  if (!BpRd32 (mArmBase + 0x04, &Cap) || Cap == 0xFFFFFFFFU) return FALSE;
  Banks = (Cap & 0xF) + ((Cap >> 4) & 0xF);
  if (Banks == 0) {
    Log (L"chip: CR4 reports no TCM banks");
    return FALSE;
  }
  for (I = 0; I < Banks; I++) {
    if (!BpWr32 (mArmBase + 0x40, I) || !BpRd32 (mArmBase + 0x44, &Info) ||
        Info == 0xFFFFFFFFU) return FALSE;
    // BCM4356 CR4 BANKINFO: six-bit size in 8 KiB units (brcmfmac/chip.c).
    Size += ((Info & 0x3FU) + 1) * 8192;
  }
  if (!(mChipOverrides & BIT3)) mRamSize = Size;
  if (!mRamSize || mRamSize > Size || mRamBase > mBar2Size ||
      mRamSize > mBar2Size - mRamBase) {
    Log (L"chip: TCM size %08x / requested %08x does not fit BAR2 %08x", Size, mRamSize, mBar2Size);
    return FALSE;
  }
  Log (L"chip: %d TCM banks, size=%08x, using %08x bytes at %08x", Banks, Size, mRamSize, mRamBase);
  mRamCapacity = mRamSize;
  mChipReady = TRUE;
  return TRUE;
}

// ---------------------------------------------------------------------------------------------
// pfw: firmware download + boot
// ---------------------------------------------------------------------------------------------
#include "WifiNvram.h"

STATIC
BOOLEAN
PexTcmContains (
  IN UINT32  Address,
  IN UINT32  Length
  )
{
  return Address >= mRamBase && Length <= mRamSize &&
         Address - mRamBase <= mRamSize - Length &&
         Address <= mBar2Size && Length <= mBar2Size - Address;
}

STATIC
BOOLEAN
PexFw (
  IN CONST CHAR16  *FwName,
  IN CONST CHAR16  *NvName
  )
{
  UINT8    *Fw   = NULL;
  UINT8    *Nv   = NULL;
  UINT8    *NvB  = NULL;
  UINTN    FwSz  = 0;
  UINTN    NvSz  = 0;
  UINT32   NvLen = 0;
  UINT32   Rst;
  UINT32   Mark;
  UINT32   Cur;
  UINT32   I;
  UINT32   V;

  UINT32   FirmwareRamSize;

  if (!mPexEnum || !mChipReady || mRamSize < 4 || (mRamBase & 3) || (mRamSize & 3) ||
      !PexTcmContains (mRamBase, mRamSize)) {
    Log (L"pfw: run `pex` and `pchip` first; RAM must fit the BAR2 aperture");
    return FALSE;
  }
  if (EFI_ERROR (ReadFile (FwName, &Fw, &FwSz)) ||
      !PexFirmwareRamSize (Fw, FwSz, mRamCapacity, &FirmwareRamSize)) {
    Log (L"pfw: invalid firmware file or RAM size");
    goto Failed;
  }
  if (NvName == NULL || EFI_ERROR (ReadFile (NvName, &Nv, &NvSz)) ||
      !PexNvramBuild (Nv, NvSz, &NvB, &NvLen)) {
    Log (L"pfw: a valid single-board Switch NVRAM file is required");
    goto Failed;
  }
  FreePool (Nv);
  Nv = NULL;
  if (NvLen > FirmwareRamSize || ((FwSz + 3) & ~(UINTN)3) > FirmwareRamSize - NvLen) {
    Log (L"pfw: firmware (%d) overlaps NVRAM (%d) in RAM (%08x)", FwSz, NvLen, FirmwareRamSize);
    goto Failed;
  }
  Rst = Le32 (Fw);
  Log (L"pfw: firmware %d bytes, first word (reset vector) %08x", FwSz, Rst);
  Log (L"pfw: halting CR4");
  mSharedAddr = 0;
  if (!CoreReset (mArmWrap, 0x20, 0x20, 0x20)) goto Failed;
  mRamSize = FirmwareRamSize;
  Log (L"pfw: firmware RAM size=%08x, discovered capacity=%08x", mRamSize, mRamCapacity);

  Log (L"pfw: writing firmware to TCM offset %08x (32-bit stores)", mRamBase);
  TcmWriteBuf (mRamBase, Fw, FwSz);
  V = TcmRd32 (mRamBase);
  if (V != Rst) {
    Log (L"pfw: firmware word0 mismatch %08x != %08x; CR4 remains halted", V, Rst);
    goto Failed;
  }
  V = TcmRd32 (mRamBase + (UINT32)(FwSz & ~3U) - 4);
  if (V != Le32 (Fw + (FwSz & ~3U) - 4)) {
    Log (L"pfw: final firmware word mismatch; CR4 remains halted");
    goto Failed;
  }
  FreePool (Fw);
  Fw = NULL;

  TcmWr32 (mRamBase + mRamSize - 4, 0);
  Log (L"pfw: nvram %d bytes -> %d (incl. token) at TCM %08x", NvSz, NvLen, mRamBase + mRamSize - NvLen);
  TcmWriteBuf (mRamBase + mRamSize - NvLen, NvB, NvLen);
  V = TcmRd32 (mRamBase + mRamSize - 4);
  if (V != Le32 (NvB + NvLen - 4)) {
    Log (L"pfw: NVRAM token readback mismatch; CR4 remains halted");
    goto Failed;
  }
  FreePool (NvB);
  NvB = NULL;

  Mark = TcmRd32 (mRamBase + mRamSize - 4);
  Log (L"pfw: marker at RAM end = %08x (firmware overwrites it with the shared-area pointer)", Mark);
  Log (L"pfw: write reset vector to TCM[0], release CR4");
  TcmWr32 (0, Rst);
  if (!CoreReset (mArmWrap, 0x20, 0, 0)) goto Failed;
  BpRd32 (mArmWrap + 0x408, &V);
  Log (L"pfw: CR4 IOCTL=%08x RESET_CTL follows", V);
  BpRd32 (mArmWrap + 0x800, &V);
  Log (L"pfw: CR4 RESET_CTL=%08x", V);

  Cur = Mark;
  for (I = 0; I < 100 && Cur == Mark; I++) {
    Ms (50);
    Cur = TcmRd32 (mRamBase + mRamSize - 4);
  }
  if (Cur == Mark) {
    Log (L"pfw: FIRMWARE DID NOT START within 5 s (marker unchanged %08x)", Cur);
    Log (L"pfw: try: other nvram file, `ram=`/`size=`, check chip halted state, then `pcon` anyway");
    return FALSE;
  }
  if ((Cur & 3) || !PexTcmContains (Cur, 80)) {
    Log (L"pfw: marker changed to %08x but is outside RAM [%08x,%08x)", Cur, mRamBase, mRamBase + mRamSize);
    return FALSE;
  }
  mSharedAddr = Cur;
  Log (L"pfw: FIRMWARE IS RUNNING after %d ms; shared area at %08x", (I) * 50, Cur);
  Log (L"pfw: shared: flags=%08x (version %d) console=%08x w32(maxrxbuf@34)=%08x rxdataoff=%08x htod_mb=%08x dtoh_mb=%08x ringinfo=%08x",
       TcmRd32 (Cur + 0), TcmRd32 (Cur + 0) & 0xFF, TcmRd32 (Cur + BRCMF_SHARED_CONSOLE_ADDR_OFFSET),
       TcmRd32 (Cur + 32), TcmRd32 (Cur + 36), TcmRd32 (Cur + BRCMF_SHARED_HTOD_MB_OFFSET),
       TcmRd32 (Cur + BRCMF_SHARED_DTOH_MB_OFFSET), TcmRd32 (Cur + BRCMF_SHARED_RING_INFO_OFFSET));
  return TRUE;
Failed:
  if (Fw != NULL) FreePool (Fw);
  if (Nv != NULL) FreePool (Nv);
  if (NvB != NULL) FreePool (NvB);
  return FALSE;
}

// ---------------------------------------------------------------------------------------------
// pcon: firmware console
// ---------------------------------------------------------------------------------------------
STATIC
VOID
PexConsole (
  VOID
  )
{
  UINT32  Con;
  UINT32  Buf;
  UINT32  Size;
  UINT32  Widx;
  UINT32  I;
  UINT32  W = 0;
  CHAR8   Line[160];
  UINTN   L = 0;
  UINT8   C;

  if (!mPexEnum || !mChipReady || mRamSize < 4 || !PexTcmContains (mRamBase, mRamSize)) {
    Log (L"pcon: run `pex` and `pchip` first");
    return;
  }
  if (mSharedAddr == 0) {
    Log (L"pcon: shared area unknown (pfw did not see the firmware start); trying to locate it at RAM end");
    mSharedAddr = TcmRd32 (mRamBase + mRamSize - 4);
    if ((mSharedAddr & 3) || !PexTcmContains (mSharedAddr, 24)) {
      Log (L"pcon: RAM end word=%08x is not a pointer, firmware console not available", mSharedAddr);
      mSharedAddr = 0;
      return;
    }
  }
  Con = TcmRd32 (mSharedAddr + BRCMF_SHARED_CONSOLE_ADDR_OFFSET);
  if ((Con & 3) || !PexTcmContains (Con, 20)) {
    Log (L"pcon: bad console address %08x", Con);
    return;
  }
  Buf  = TcmRd32 (Con + 8);
  Size = TcmRd32 (Con + 12);
  Widx = TcmRd32 (Con + 16);
  Log (L"pcon: console struct %08x buf=%08x size=%08x widx=%08x", Con, Buf, Size, Widx);
  if ((Buf & 3) || Size == 0 || Size > 0x10000 ||
      !PexTcmContains (Buf, (Size + 3) & ~3U)) {
    return;
  }
  if (Widx > Size) {
    Widx = Size;
  }
  for (I = 0; I < Widx; I++) {
    if ((I & 3) == 0) {
      W = TcmRd32 (Buf + I);
    }
    C = (UINT8)(W >> ((I & 3) * 8));
    if (C == '\n' || L >= sizeof (Line) - 2) {
      Line[L] = 0;
      Log (L"fw> %a", Line);
      L = 0;
      if (C == '\n') {
        continue;
      }
    }
    if (C >= 0x20 && C < 0x7F) {
      Line[L++] = (CHAR8)C;
    }
  }
  if (L) {
    Line[L] = 0;
    Log (L"fw> %a", Line);
  }
}

// ---------------------------------------------------------------------------------------------
STATIC
VOID
PexOff (
  VOID
  )
{
  Log (L"poff: halt CR4 if reachable, hold PCIe in reset, radio off");
  if (mPexEnum && mPcieMode && mChipReady) {
    CoreReset (mArmWrap, 0x20, 0x20, 0x20);
  }
  GpioH (H0 | H1, 0, TRUE);
  if (mPexUp) {
    Rm (PEX_AFI, AFI_PEX1_CTRL, BIT0, 0);
  }
  Wr (CAR_BASE, PX_CAR_RST_U_SET, PX_U_PCIEX);
  mPexUp      = FALSE;
  mPexEnum    = FALSE;
  mPcieMode   = FALSE;
  mChipReady  = FALSE;
  mSharedAddr = 0;
}
