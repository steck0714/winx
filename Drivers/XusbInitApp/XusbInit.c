/** @file
  xusbinit - Tegra210 XUSB (xHCI) bring-up experiment for the UEFI Shell.  DRAFT, UNTESTED.

  Goal: get the Tegra210 XUSB host controller (USB 2.0 + later SuperSpeed) running so that EDK2's stock
  XhciDxe can drive it. This tool does it in small steps, each one logged (screen + xusb.log, flushed after
  every line) BEFORE the risky access, so the last line of the log names the step that faulted.

  The USB-C port's USB2 pad is shared with the ChipIdea/EHCI controller that the firmware uses for the keyboard.
  `pad` hands the pad to XUSB, which makes the EHCI keyboard disappear: run these from a startup.nsh (or
  `xusbinit all fw=xusb.bin` and wait), end with `restore`, then reset. See docs in README.winx.md.

    xusbinit info                  read-only CAR / controller state (reads XUSB regs only when clocks are on)
    xusbinit pmc                   PMC power-gate status of XUSBA/B/C (READS PMC: may fault from non-secure)
    xusbinit pg                    power up XUSBA + XUSBC partitions (Linux sequence; PMC access)
    xusbinit clk                   XUSB clocks + resets (hekate/Linux values)
    xusbinit pad                   USB2 OTG pad0 -> XUSB, host capability, ID grounded
    xusbinit fw [file]             load xusb.bin (default xusb.bin) into RAM, set up IPFS/FPCI, boot the falcon
    xusbinit svc [seconds]         answer firmware mailbox requests (clock ACKs) and print port status changes
    xusbinit ports                 dump xHCI port registers
    xusbinit restore               pad mux back to USB2 (UTMIP/EHCI)
    xusbinit all [file]            clk, pad, fw, svc 30
    xusbinit up [file]             clk, pad, ss, fw, ports, then the resident mailbox daemon (stays after the command)
    xusbinit daemon [stop]         start/stop the resident mailbox responder (10 ms timer, until ExitBootServices)
    xusbinit dlog                  show what the daemon logged

  Register sequences follow Linux (drivers/usb/host/xhci-tegra.c, drivers/phy/tegra/xusb-tegra210.c) and
  CTCaer's hekate (bdk/usb/xusbd.c); xusb.bin is NVIDIA's firmware (not included).
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/ShellLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/TimerLib.h>
#include <Protocol/ShellParameters.h>
#ifdef XUSB_DYNAMIC
#include <Protocol/ShellDynamicCommand.h>
#endif

// ---------------------------------------------------------------------------------------------
// Register map
// ---------------------------------------------------------------------------------------------
#define CAR_BASE            0x60006000
#define CAR_RST_U           0x00C
#define CAR_ENB_U           0x018
#define CAR_RST_W           0x35C
#define CAR_ENB_W           0x364
#define CAR_RST_U_SET       0x310
#define CAR_RST_U_CLR       0x314
#define CAR_ENB_U_SET       0x330
#define CAR_RST_W_SET       0x438
#define CAR_RST_W_CLR       0x43C
#define CAR_ENB_W_SET       0x448
#define CAR_PLLU_BASE       0x0C0
#define CAR_PLLU_OUTA       0x0C4
#define CAR_SRC_XUSB_HOST   0x600
#define CAR_SRC_XUSB_FALCON 0x604
#define CAR_SRC_XUSB_FS     0x608
#define CAR_SRC_XUSB_SS     0x610
#define CAR_ENB_Y_SET       0x29C      // CLK_OUT_ENB_SET_Y (0x2A8/0x2AC are RST_DEV_Y_SET/CLR!)
#define CAR_ENB_Y_CLR       0x2A0      // CLK_OUT_ENB_CLR_Y
#define CAR_SRC_USB2_TRK    0x6CC

#define XUSB_HOST_RST_U     BIT25      // RST_DEVICES_U / CLK_OUT_ENB_U
#define XUSB_PADCTL_RST_W   BIT14
#define XUSB_RST_W          BIT15      // "xusb" (src) reset + enable
#define XUSB_SS_W           BIT28
#define USB2_TRK_Y          BIT18

#define PMC_BASE            0x7000E400
#define PMC_PWRGATE_TOGGLE  0x30
#define  PWRGATE_START      BIT8
#define PMC_REMOVE_CLAMP    0x34
#define PMC_PWRGATE_STATUS  0x38
#define PMC_USB_AO          0xF0
#define PG_XUSBA            20
#define PG_XUSBB            21
#define PG_XUSBC            22

#define FUSE_BASE           0x7000F800
#define FUSE_USB_CALIB      0x1F0
#define FUSE_USB_CALIB_EXT  0x350

#define XUSB_HCD            0x70090000
#define XUSB_FPCI           0x70098000
#define XUSB_IPFS           0x70099000
#define XUSB_PADCTL         0x7009F000

#define PAD_USB2_PAD_MUX    0x004
#define PAD_USB2_PORT_CAP   0x008
#define PAD_SS_PORT_MAP     0x014
#define PAD_ELPG0           0x020
#define PAD_ELPG1           0x024
#define PAD_BATT_CTL0       0x080
#define PAD_BATT_CTL1       0x084
#define PAD_OTG0_CTL0       0x088
#define PAD_OTG0_CTL1       0x08C
#define PAD_BIAS_CTL0       0x284
#define PAD_BIAS_CTL1       0x288
#define PAD_VBUS_ID         0xC60

// FPCI
#define CFG_1               0x004
#define CFG_4               0x010
#define CFG_CSBRANGE        0x41C
#define CFG_CSB_BASE        0x800
#define CFG_MBOX_CMD        0x0E4
#define CFG_MBOX_DATA_IN    0x0E8
#define CFG_MBOX_DATA_OUT   0x0EC
#define CFG_MBOX_OWNER      0x0F0
#define CFG_SMI_INTR        0x428
#define  SMI_FW_HANG        BIT1
#define MBOX_DEST_FALC      BIT27
#define MBOX_DEST_SMI       BIT29
#define MBOX_INT_EN         BIT31
// IPFS
#define IPFS_CONFIG         0x180
#define IPFS_INTR_MASK      0x188
#define IPFS_CLKGATE_HYST   0x1BC
// falcon CSB
#define FALC_CPUCTL         0x100
#define FALC_BOOTVEC        0x104
#define FALC_DMACTL         0x10C
#define FALC_IMFILLRNG1     0x154
#define FALC_IMFILLCTL      0x158
#define CSB_ILOAD_ATTR      0x101A00
#define CSB_ILOAD_BASE_LO   0x101A04
#define CSB_ILOAD_BASE_HI   0x101A08
#define CSB_L2IMEMOP_SIZE   0x101A10
#define CSB_L2IMEMOP_TRIG   0x101A14
#define CSB_L2IMEMOP_RESULT 0x101A18
#define CSB_APMAP           0x10181C
#define IMEM_BLOCK          256
#define CMD_NONE            0
#define MBOX_CMD_MSG_ENABLED  1
#define MBOX_CMD_INC_FALC     2
#define MBOX_CMD_DEC_FALC     3
#define MBOX_CMD_INC_SSPI     4
#define MBOX_CMD_DEC_SSPI     5
#define MBOX_CMD_SET_BW       6
#define MBOX_CMD_SAVE_DFE_CTLE_CTX 9
#define MBOX_CMD_DISABLE_SS_LFPS_DETECTION 17
#define MBOX_CMD_ENABLE_SS_LFPS_DETECTION  18
#define MBOX_CMD_ACK          128
#define MBOX_CMD_NAK          129

#pragma pack(1)
typedef struct {
  UINT32 BootLoadAddrInImem;
  UINT32 BootCodeDfiOffset;
  UINT32 BootCodeTag;
  UINT32 BootCodeSize;
  UINT32 PhysMemAddr;
  UINT16 ReqPhysMemSize;
  UINT16 AllocPhysMemSize;
  UINT32 RodataImgOffset;
  UINT32 RodataSectionStart;
  UINT32 RodataSectionEnd;
  UINT32 MainFnAddr;
  UINT32 FwImgCksum;
  UINT32 FwImgCreatedTime;
  UINT32 ImemResidentStart;
  UINT32 ImemResidentEnd;
  UINT32 IdirectStart;
  UINT32 IdirectEnd;
  UINT32 L2ImemStart;
  UINT32 L2ImemEnd;
  UINT32 VersionId;
  UINT8  InitDdirect;
  UINT8  Reserved[3];
  UINT32 PhysAddrLogBuffer;
  UINT32 TotalLogEntries;
  UINT32 DequeuePtr;
  UINT32 DummyVar[2];
  UINT32 FwImgLen;
  UINT8  Magic[8];
  UINT32 SsLowPowerEntryTimeout;
  UINT8  NumHsicPort;
  UINT8  Padding[139];
} XUSB_FW_HEADER;
#pragma pack()

// ---------------------------------------------------------------------------------------------
// Logging (screen + file, flushed per line so the last line survives a fault)
// ---------------------------------------------------------------------------------------------
STATIC SHELL_FILE_HANDLE  mLog = NULL;
STATIC UINT32             mFwPhys = 0;

// Resident mailbox service: while it runs, Log() must not touch the console/files (it is called from a
// timer callback), so lines go to a ring buffer shown by `xusbinit dlog`.
STATIC BOOLEAN  mQuiet = FALSE;
STATIC CHAR16   mDLog[2048];
STATIC UINTN    mDLogLen = 0;

STATIC
VOID
Log (
  IN CONST CHAR16  *Fmt,
  ...
  )
{
  CHAR16   Buf[200];
  CHAR8    Out[220];
  VA_LIST  Ap;
  UINTN    Len;

  VA_START (Ap, Fmt);
  UnicodeVSPrint (Buf, sizeof (Buf), Fmt, Ap);
  VA_END (Ap);

  if (mQuiet) {
    UINTN  N = StrLen (Buf);
    if (mDLogLen + N + 2 < ARRAY_SIZE (mDLog)) {          // keep the first lines (the start-up is what matters)
      CopyMem (&mDLog[mDLogLen], Buf, N * sizeof (CHAR16));
      mDLogLen += N;
      mDLog[mDLogLen++] = L'\n';
      mDLog[mDLogLen]   = 0;
    }
    return;
  }

  Print (L"%s\n", Buf);
  if (mLog != NULL) {
    Len = AsciiSPrint (Out, sizeof (Out), "%s\r\n", Buf);
    ShellWriteFile (mLog, &Len, Out);
    ShellFlushFile (mLog);
  }
}

STATIC UINT32 Rd (UINTN B, UINTN O) { return MmioRead32 (B + O); }
STATIC VOID   Wr (UINTN B, UINTN O, UINT32 V) { MmioWrite32 (B + O, V); }
STATIC VOID   Rmw (UINTN B, UINTN O, UINT32 Clr, UINT32 Set) { MmioWrite32 (B + O, (MmioRead32 (B + O) & ~Clr) | Set); }

STATIC
VOID
Us (UINTN N)
{
  MicroSecondDelay (N);
}

// ---------------------------------------------------------------------------------------------
// Guards: never touch 0x7009xxxx unless clocks/resets say the block is alive (else the CPU stalls).
// ---------------------------------------------------------------------------------------------
STATIC
BOOLEAN
PadctlAlive (
  VOID
  )
{
  return (Rd (CAR_BASE, CAR_ENB_W) & XUSB_RST_W) != 0 &&
         (Rd (CAR_BASE, CAR_RST_W) & (XUSB_RST_W | XUSB_PADCTL_RST_W)) == 0;
}

STATIC
BOOLEAN
HostAlive (
  VOID
  )
{
  return PadctlAlive () &&
         (Rd (CAR_BASE, CAR_ENB_U) & XUSB_HOST_RST_U) != 0 &&
         (Rd (CAR_BASE, CAR_RST_U) & XUSB_HOST_RST_U) == 0 &&
         (Rd (CAR_BASE, CAR_ENB_W) & XUSB_SS_W) != 0 &&
         (Rd (CAR_BASE, CAR_RST_W) & XUSB_SS_W) == 0;
}

// ---------------------------------------------------------------------------------------------
// FPCI / CSB access
// ---------------------------------------------------------------------------------------------
STATIC
UINT32
CsbRd (
  IN UINT32  Off
  )
{
  Wr (XUSB_FPCI, CFG_CSBRANGE, (Off >> 9) & 0x7FFFFF);
  return Rd (XUSB_FPCI, CFG_CSB_BASE + (Off & 0x1FF));
}

STATIC
VOID
CsbWr (
  IN UINT32  Off,
  IN UINT32  V
  )
{
  Wr (XUSB_FPCI, CFG_CSBRANGE, (Off >> 9) & 0x7FFFFF);
  Wr (XUSB_FPCI, CFG_CSB_BASE + (Off & 0x1FF), V);
}

// ---------------------------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------------------------
STATIC
VOID
StepInfo (
  VOID
  )
{
  UINT32  Len;

  Log (L"info: CAR ENB_U=%08x RST_U=%08x ENB_W=%08x RST_W=%08x", Rd (CAR_BASE, CAR_ENB_U), Rd (CAR_BASE, CAR_RST_U),
       Rd (CAR_BASE, CAR_ENB_W), Rd (CAR_BASE, CAR_RST_W));
  Log (L"info: PLLU_BASE=%08x (lock=bit27) SRC host=%08x falcon=%08x fs=%08x ss=%08x", Rd (CAR_BASE, CAR_PLLU_BASE),
       Rd (CAR_BASE, CAR_SRC_XUSB_HOST), Rd (CAR_BASE, CAR_SRC_XUSB_FALCON), Rd (CAR_BASE, CAR_SRC_XUSB_FS), Rd (CAR_BASE, CAR_SRC_XUSB_SS));
  if (!PadctlAlive ()) {
    Log (L"info: XUSB pad clock/reset off -> padctl not read");
    return;
  }
  Log (L"info: PADCTL mux=%08x cap=%08x ssmap=%08x vbus_id=%08x", Rd (XUSB_PADCTL, PAD_USB2_PAD_MUX), Rd (XUSB_PADCTL, PAD_USB2_PORT_CAP),
       Rd (XUSB_PADCTL, PAD_SS_PORT_MAP), Rd (XUSB_PADCTL, PAD_VBUS_ID));
  if (!HostAlive ()) {
    Log (L"info: XUSB host/ss clock or reset off -> host regs not read");
    return;
  }
  Log (L"info: IPFS cfg=%08x FPCI cfg1=%08x cfg4=%08x", Rd (XUSB_IPFS, IPFS_CONFIG), Rd (XUSB_FPCI, CFG_1), Rd (XUSB_FPCI, CFG_4));
  Len = Rd (XUSB_HCD, 0) & 0xFF;
  Log (L"info: xHCI CAPLENGTH=%02x HCIVERSION=%04x HCSPARAMS1=%08x USBCMD=%08x USBSTS=%08x", Len, Rd (XUSB_HCD, 0) >> 16,
       Rd (XUSB_HCD, 4), Rd (XUSB_HCD, Len), Rd (XUSB_HCD, Len + 4));
}

STATIC
VOID
StepPmc (
  VOID
  )
{
  Log (L"pmc: PWRGATE_STATUS=%08x (XUSBA bit20, XUSBB bit21, XUSBC bit22)", Rd (PMC_BASE, PMC_PWRGATE_STATUS));
}

STATIC
BOOLEAN
PgOn (
  IN UINT32  Id
  )
{
  UINTN  I;

  if (Rd (PMC_BASE, PMC_PWRGATE_STATUS) & (1U << (Id))) {
    Log (L"pg: partition %d already on", Id);
    return TRUE;
  }
  Wr (PMC_BASE, PMC_PWRGATE_TOGGLE, PWRGATE_START | Id);
  for (I = 0; I < 100; I++) {
    if (Rd (PMC_BASE, PMC_PWRGATE_STATUS) & (1U << (Id))) {
      break;
    }
    Us (100);
  }
  if (!(Rd (PMC_BASE, PMC_PWRGATE_STATUS) & (1U << (Id)))) {
    Log (L"pg: partition %d did not power on (STATUS=%08x)", Id, Rd (PMC_BASE, PMC_PWRGATE_STATUS));
    return FALSE;
  }
  Us (10);
  Wr (PMC_BASE, PMC_REMOVE_CLAMP, (1U << (Id)));
  Us (10);
  Log (L"pg: partition %d on", Id);
  return TRUE;
}

STATIC
VOID
StepPg (
  VOID
  )
{
  // Linux: tegra_powergate_sequence_power_up(): clk on, reset asserted, power on, remove clamp, reset released.
  Log (L"pg: step start (PMC access)");
  // Clocks must run and the blocks be in reset while the partition powers up.
  Wr (CAR_BASE, CAR_ENB_U_SET, XUSB_HOST_RST_U);
  Wr (CAR_BASE, CAR_ENB_W_SET, XUSB_SS_W | XUSB_RST_W);
  Wr (CAR_BASE, CAR_RST_U_SET, XUSB_HOST_RST_U);
  Wr (CAR_BASE, CAR_RST_W_SET, XUSB_SS_W);
  Us (10);
  PgOn (PG_XUSBA);
  PgOn (PG_XUSBC);
  Log (L"pg: done, STATUS=%08x", Rd (PMC_BASE, PMC_PWRGATE_STATUS));
}

STATIC
VOID
StepClk (
  VOID
  )
{
  Log (L"clk: PLLU_BASE=%08x", Rd (CAR_BASE, CAR_PLLU_BASE));
  if (!(Rd (CAR_BASE, CAR_PLLU_BASE) & BIT27)) {
    Log (L"clk: PLLU not locked -> abort (EHCI/hekate should have enabled it)");
    return;
  }
  Wr (CAR_BASE, CAR_PLLU_OUTA, Rd (CAR_BASE, CAR_PLLU_OUTA) | 1);   // release PLLU_OUT1 reset (hekate)
  Us (2);

  // XUSB (src) + padctl: enable clock, pulse reset, release (hekate xusb_device_init).
  Wr (CAR_BASE, CAR_ENB_W_SET, XUSB_RST_W);
  Wr (CAR_BASE, CAR_RST_W_SET, XUSB_RST_W);
  Us (2);
  Wr (CAR_BASE, CAR_RST_W_CLR, XUSB_RST_W);
  Wr (CAR_BASE, CAR_RST_W_CLR, XUSB_PADCTL_RST_W);
  Us (2);
  Log (L"clk: xusb+padctl up, RST_W=%08x", Rd (CAR_BASE, CAR_RST_W));

  // Host core 102 MHz (PLLP/(6+2)*2), falcon 204 MHz, FS 48 MHz, SS 120 MHz (hekate/Linux values).
  Wr (CAR_BASE, CAR_ENB_U_SET, XUSB_HOST_RST_U);
  Rmw (CAR_BASE, CAR_SRC_XUSB_HOST,   0xE00000FF, (1 << 29) | 6);
  Rmw (CAR_BASE, CAR_SRC_XUSB_FALCON, 0xE00000FF, (1 << 29) | 2);
  Rmw (CAR_BASE, CAR_SRC_XUSB_FS,     0xE0000000, (2 << 29));
  Wr (CAR_BASE, CAR_ENB_W_SET, XUSB_SS_W);
  Rmw (CAR_BASE, CAR_SRC_XUSB_SS,     0xE00000FF, (3 << 29) | 6);
  Us (2);
  Wr (CAR_BASE, CAR_RST_W_CLR, XUSB_SS_W);
  Wr (CAR_BASE, CAR_RST_U_CLR, XUSB_HOST_RST_U);
  Us (10);
  Log (L"clk: host/ss up ENB_U=%08x RST_U=%08x ENB_W=%08x RST_W=%08x", Rd (CAR_BASE, CAR_ENB_U), Rd (CAR_BASE, CAR_RST_U),
       Rd (CAR_BASE, CAR_ENB_W), Rd (CAR_BASE, CAR_RST_W));
}

STATIC
VOID
StepPad (
  VOID
  )
{
  UINT32  Calib;
  UINT32  Ext;

  if (!PadctlAlive ()) {
    Log (L"pad: padctl clock/reset not up, run `clk` first");
    return;
  }
  Log (L"pad: before mux=%08x cap=%08x", Rd (XUSB_PADCTL, PAD_USB2_PAD_MUX), Rd (XUSB_PADCTL, PAD_USB2_PORT_CAP));

  // OTG pad0 and the bias pad belong to XUSB (EHCI/UTMIP loses the port from here on).
  Rmw (XUSB_PADCTL, PAD_USB2_PAD_MUX, (3 << 0) | (3 << 18), (1 << 0) | (1 << 18));
  Log (L"pad: mux -> XUSB");

  // hekate _xusb_init_phy(): calibration + power-up. PLLU/UTMIPLL were already enabled by the EHCI path.
  Calib = Rd (FUSE_BASE, FUSE_USB_CALIB);
  Ext   = Rd (FUSE_BASE, FUSE_USB_CALIB_EXT);
  Log (L"pad: fuse calib=%08x ext=%08x", Calib, Ext);
  Rmw (XUSB_PADCTL, PAD_OTG0_CTL0, 0x3F, Calib & 0x3F);
  Wr (XUSB_PADCTL, PAD_OTG0_CTL1, (Rd (XUSB_PADCTL, PAD_OTG0_CTL1) & 0x83FFFF87) | ((Calib & 0x780) >> 4) | ((Ext << 27) >> 1));
  Rmw (XUSB_PADCTL, PAD_BATT_CTL1, 0x1C0, 0x80);
  Rmw (XUSB_PADCTL, PAD_OTG0_CTL0, BIT26 | BIT29, 0);   // pad power down off (0xDBFFFFFF in hekate)
  Rmw (XUSB_PADCTL, PAD_OTG0_CTL1, BIT2, 0);
  Rmw (XUSB_PADCTL, PAD_BATT_CTL0, BIT0, 0);
  Rmw (XUSB_PADCTL, PAD_BIAS_CTL0, BIT11, 0);
  (VOID)Rd (XUSB_PADCTL, PAD_OTG0_CTL1);
  Log (L"pad: pads powered");

  // USB2 tracking
  Wr (CAR_BASE, CAR_ENB_Y_SET, USB2_TRK_Y);
  Rmw (CAR_BASE, CAR_SRC_USB2_TRK, 0xFF, 6);
  Wr (XUSB_PADCTL, PAD_BIAS_CTL1, 0x451E000);
  Wr (XUSB_PADCTL, PAD_BIAS_CTL1, 0x51E000);
  Us (100);
  Wr (XUSB_PADCTL, PAD_BIAS_CTL1, 0x451E000);
  Us (3);
  Wr (XUSB_PADCTL, PAD_BIAS_CTL1, 0x51E000);
  Us (100);
  Rmw (XUSB_PADCTL, PAD_BIAS_CTL1, 0, 0x4000000);
  Wr (CAR_BASE, CAR_ENB_Y_CLR, USB2_TRK_Y);
  Us (30);
  Log (L"pad: tracking done");

  // Host capability for USB2 port0; SS port0 mapped to USB2 port0; no wake events; ID grounded -> host.
  Rmw (XUSB_PADCTL, PAD_USB2_PORT_CAP, 3, 1);
  Rmw (XUSB_PADCTL, PAD_SS_PORT_MAP, 0xF, 0);
  Wr (XUSB_PADCTL, PAD_ELPG0, 0);
  Wr (XUSB_PADCTL, PAD_ELPG1, 0);
  Rmw (XUSB_PADCTL, PAD_VBUS_ID, (0xF << 18) | (3 << 16), (1 << 16));   // SRC_ID_OVR_EN, ID = GND (host)
  Rmw (PMC_BASE, PMC_USB_AO, BIT2 | BIT3, 0);                           // ID / VBUS wake-up power up (hekate)
  Log (L"pad: host cap set, mux=%08x cap=%08x vbus_id=%08x", Rd (XUSB_PADCTL, PAD_USB2_PAD_MUX), Rd (XUSB_PADCTL, PAD_USB2_PORT_CAP),
       Rd (XUSB_PADCTL, PAD_VBUS_ID));
}

#include "XusbSs.h"

STATIC
VOID
StepRestore (
  VOID
  )
{
  if (!PadctlAlive ()) {
    Log (L"restore: padctl off, nothing to do");
    return;
  }
  Rmw (XUSB_PADCTL, PAD_USB2_PAD_MUX, (3 << 0) | (3 << 18), 0);
  Log (L"restore: USB2 pad mux -> USB2 (EHCI), mux=%08x", Rd (XUSB_PADCTL, PAD_USB2_PAD_MUX));
}

STATIC
BOOLEAN
MboxSend (
  IN UINT32  Cmd,
  IN UINT32  Data
  )
{
  BOOLEAN  WaitIdle;
  UINTN    I;
  UINT32   V;

  WaitIdle = !(Cmd == MBOX_CMD_ACK || Cmd == MBOX_CMD_NAK);
  if (WaitIdle) {
    if (Rd (XUSB_FPCI, CFG_MBOX_OWNER) != 0) {
      Log (L"mbox: busy (owner=%d)", Rd (XUSB_FPCI, CFG_MBOX_OWNER));
      return FALSE;
    }
    Wr (XUSB_FPCI, CFG_MBOX_OWNER, 2);
    if (Rd (XUSB_FPCI, CFG_MBOX_OWNER) != 2) {
      Log (L"mbox: cannot acquire");
      return FALSE;
    }
  }
  Wr (XUSB_FPCI, CFG_MBOX_DATA_IN, ((Cmd & 0xFF) << 24) | (Data & 0xFFFFFF));
  Wr (XUSB_FPCI, CFG_MBOX_CMD, Rd (XUSB_FPCI, CFG_MBOX_CMD) | MBOX_INT_EN | MBOX_DEST_FALC);
  if (WaitIdle) {
    for (I = 0; I < 25000; I++) {
      V = Rd (XUSB_FPCI, CFG_MBOX_OWNER);
      if (V == 0) {
        return TRUE;
      }
      Us (10);
    }
    Log (L"mbox: timeout waiting for firmware (owner=%d)", Rd (XUSB_FPCI, CFG_MBOX_OWNER));
    return FALSE;
  }
  return TRUE;
}

STATIC
VOID
StepFw (
  IN CONST CHAR16  *Path
  )
{
  SHELL_FILE_HANDLE  F;
  UINT64             Size;
  UINTN              Sz;
  UINTN              Pages;
  EFI_PHYSICAL_ADDRESS  Addr;
  EFI_STATUS         Status;
  XUSB_FW_HEADER     *H;
  UINT32             V;
  UINT32             TagBlocks, SizeBlocks;
  UINT32             Len;
  UINTN              I;
  UINT64             Phys;

  if (!HostAlive ()) {
    Log (L"fw: host/ss clock or reset not up, run `clk` first");
    return;
  }

  Log (L"fw: opening %s", Path);
  Status = ShellOpenFileByName (Path, &F, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Log (L"fw: cannot open %s: %r", Path, Status);
    return;
  }
  ShellGetFileSize (F, &Size);
  if (Size < sizeof (XUSB_FW_HEADER) + 0x1000 || Size > 0x100000) {
    Log (L"fw: implausible size %ld", Size);
    ShellCloseFile (&F);
    return;
  }
  Pages = EFI_SIZE_TO_PAGES ((UINTN)Size);
  Addr  = 0xFFFFFFFF;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiRuntimeServicesData, Pages, &Addr);
  if (EFI_ERROR (Status)) {
    Log (L"fw: AllocatePages: %r", Status);
    ShellCloseFile (&F);
    return;
  }
  Sz = (UINTN)Size;
  Status = ShellReadFile (F, &Sz, (VOID *)(UINTN)Addr);
  ShellCloseFile (&F);
  if (EFI_ERROR (Status) || Sz != (UINTN)Size) {
    Log (L"fw: read: %r (%d of %ld)", Status, Sz, Size);
    return;
  }
  WriteBackDataCacheRange ((VOID *)(UINTN)Addr, (UINTN)Size);   // the XUSB falcon DMA reads this from DRAM
  mFwPhys = (UINT32)Addr;
  H = (XUSB_FW_HEADER *)(UINTN)Addr;
  Log (L"fw: %ld bytes at %08x, ver=%08x built=%08x imglen=%d magic=%c%c%c%c%c%c%c%c", Size, mFwPhys, H->VersionId, H->FwImgCreatedTime,
       H->FwImgLen, H->Magic[0], H->Magic[1], H->Magic[2], H->Magic[3], H->Magic[4], H->Magic[5], H->Magic[6], H->Magic[7]);
  Log (L"fw: bootcode tag=%x size=%x dfi_off=%x", H->BootCodeTag, H->BootCodeSize, H->BootCodeDfiOffset);
  if (H->BootCodeTag + H->BootCodeSize > Size) {
    Log (L"fw: header looks wrong (bootcode beyond file) -> abort");
    return;
  }

  // --- tegra_xusb_config(): IPFS enable, BAR0, bus master ---
  Rmw (XUSB_IPFS, IPFS_CONFIG, 0, 1);
  Us (20);
  Rmw (XUSB_FPCI, CFG_4, 0xFFFF8000, XUSB_HCD & 0xFFFF8000);
  Us (200);
  Rmw (XUSB_FPCI, CFG_1, 0, 7);
  Rmw (XUSB_IPFS, IPFS_INTR_MASK, 0, BIT16);
  Wr (XUSB_IPFS, IPFS_CLKGATE_HYST, 0x80);
  Log (L"fw: FPCI cfg1=%08x cfg4=%08x ipfs=%08x", Rd (XUSB_FPCI, CFG_1), Rd (XUSB_FPCI, CFG_4), Rd (XUSB_IPFS, IPFS_CONFIG));

  // --- tegra_xusb_load_firmware_rom() ---
  if (CsbRd (CSB_ILOAD_BASE_LO) != 0) {
    Log (L"fw: firmware already loaded, falcon cpuctl=%08x", CsbRd (FALC_CPUCTL));
  } else {
    Phys = (UINT64)Addr + sizeof (XUSB_FW_HEADER);
    CsbWr (CSB_ILOAD_ATTR, (UINT32)Size);
    CsbWr (CSB_ILOAD_BASE_HI, (UINT32)(Phys >> 32));
    CsbWr (CSB_ILOAD_BASE_LO, (UINT32)Phys);
    CsbWr (CSB_APMAP, BIT31);
    CsbWr (CSB_L2IMEMOP_TRIG, 0x40U << 24);                         // invalidate all
    TagBlocks  = (H->BootCodeTag + IMEM_BLOCK - 1) / IMEM_BLOCK;
    SizeBlocks = (H->BootCodeSize + IMEM_BLOCK - 1) / IMEM_BLOCK;
    CsbWr (CSB_L2IMEMOP_SIZE, ((TagBlocks & 0x3FF) << 8) | ((SizeBlocks & 0xFF) << 24));
    CsbWr (CSB_L2IMEMOP_TRIG, 0x11U << 24);                         // load locked + result
    CsbWr (FALC_IMFILLCTL, SizeBlocks);
    CsbWr (FALC_IMFILLRNG1, ((TagBlocks & 0xFFFF) << 0) | (((TagBlocks + SizeBlocks) & 0xFFFF) << 16));
    CsbWr (FALC_DMACTL, 0);
    Log (L"fw: L2IMEM load triggered");
    for (I = 0; I < 100; I++) {
      V = CsbRd (CSB_L2IMEMOP_RESULT);
      if (V & BIT31) {
        break;
      }
      Us (100);
    }
    if (!(V & BIT31)) {
      Log (L"fw: DMA controller not ready (%08x)", V);
      return;
    }
    CsbWr (FALC_BOOTVEC, H->BootCodeTag);
    CsbWr (FALC_CPUCTL, BIT1);                                      // start CPU
    Log (L"fw: falcon started, cpuctl=%08x", CsbRd (FALC_CPUCTL));
  }

  // wait for USBSTS.CNR == 0
  Len = Rd (XUSB_HCD, 0) & 0xFF;
  for (I = 0; I < 200; I++) {
    V = Rd (XUSB_HCD, Len + 4);
    if (!(V & BIT11)) {
      break;
    }
    Us (1000);
  }
  Log (L"fw: USBSTS=%08x (CNR=bit11) falcon cpuctl=%08x", V, CsbRd (FALC_CPUCTL));
  if (V & BIT11) {
    Log (L"fw: controller not ready");
    return;
  }

  if (MboxSend (MBOX_CMD_MSG_ENABLED, 0)) {
    Log (L"fw: firmware messages enabled");
  }
  Log (L"fw: HCIVERSION=%04x HCSPARAMS1=%08x (ports=bits31:24) USBCMD=%08x", Rd (XUSB_HCD, 0) >> 16, Rd (XUSB_HCD, 4), Rd (XUSB_HCD, Len));
}

STATIC
VOID
StepPorts (
  VOID
  )
{
  UINT32  Len;
  UINT32  N;
  UINT32  I;

  if (!HostAlive ()) {
    Log (L"ports: host not up");
    return;
  }
  Len = Rd (XUSB_HCD, 0) & 0xFF;
  N   = (Rd (XUSB_HCD, 4) >> 24) & 0xFF;
  for (I = 0; I < N && I < 16; I++) {
    Log (L"ports: P%d PORTSC=%08x", I + 1, Rd (XUSB_HCD, Len + 0x400 + I * 0x10));
  }
}

// Answer one firmware mailbox request, if any (clock ACKs etc.). Safe to call from a timer callback:
// ACK/NAK replies never wait (MboxSend only waits for requests that WE send).
STATIC
VOID
SvcHandleSmi (
  VOID
  )
{
  UINT32  Smi;
  UINT32  V;
  UINT32  Cmd, Data;

  Smi = Rd (XUSB_FPCI, CFG_SMI_INTR);
  if (Smi != 0) {
    Wr (XUSB_FPCI, CFG_SMI_INTR, Smi);
    if (Smi & SMI_FW_HANG) {
      Log (L"svc: firmware hang reported");
    }
    V    = Rd (XUSB_FPCI, CFG_MBOX_DATA_OUT);
    Cmd  = V >> 24;
    Data = V & 0xFFFFFF;
    Wr (XUSB_FPCI, CFG_MBOX_CMD, Rd (XUSB_FPCI, CFG_MBOX_CMD) & ~MBOX_DEST_SMI);
    if (Cmd != MBOX_CMD_ACK && Cmd != MBOX_CMD_NAK && Cmd != MBOX_CMD_SET_BW) {
      // requires an ACK/NAK; owner stays with firmware until we answer
    } else {
      Wr (XUSB_FPCI, CFG_MBOX_OWNER, 0);
    }
    Log (L"svc: fw msg cmd=%d data=%06x", Cmd, Data);
    switch (Cmd) {
      case MBOX_CMD_INC_FALC:
      case MBOX_CMD_DEC_FALC:
        MboxSend (Data == 204000 ? MBOX_CMD_ACK : MBOX_CMD_NAK, 204000);   // falcon clock is 204 MHz
        break;
      case MBOX_CMD_INC_SSPI:
      case MBOX_CMD_DEC_SSPI:
        MboxSend (MBOX_CMD_ACK, Data);                                   // SS clock is fixed (scale_ss_clock = false)
        break;
      case MBOX_CMD_SAVE_DFE_CTLE_CTX:
        MboxSend (MBOX_CMD_NAK, Data);                                   // Tegra210 has no usb3_save_context op (Linux: -ENOSYS -> NAK)
        break;
      case MBOX_CMD_DISABLE_SS_LFPS_DETECTION:
      case MBOX_CMD_ENABLE_SS_LFPS_DETECTION:
        MboxSend (SsLfps (Data, Cmd == MBOX_CMD_ENABLE_SS_LFPS_DETECTION) ? MBOX_CMD_ACK : MBOX_CMD_NAK, Data);
        break;
      default:
        if (Cmd != MBOX_CMD_SET_BW && Cmd != MBOX_CMD_ACK && Cmd != MBOX_CMD_NAK) {
          Log (L"svc: unhandled message %d (not answered)", Cmd);
        }
        break;
    }
  }
}

STATIC
VOID
StepSvc (
  IN UINTN  Seconds
  )
{
  UINTN    T;
  UINT32   Len;
  UINT32   Last[16];
  UINT32   N, I, Ps;

  if (!HostAlive ()) {
    Log (L"svc: host not up");
    return;
  }
  Len = Rd (XUSB_HCD, 0) & 0xFF;
  N   = (Rd (XUSB_HCD, 4) >> 24) & 0xFF;
  if (N > 16) {
    N = 16;
  }
  SetMem (Last, sizeof (Last), 0xFF);
  Log (L"svc: %d s (mailbox + port monitor)", Seconds);
  for (T = 0; T < Seconds * 100; T++) {
    SvcHandleSmi ();
    for (I = 0; I < N; I++) {
      Ps = Rd (XUSB_HCD, Len + 0x400 + I * 0x10);
      if (Ps != Last[I]) {
        Log (L"svc: P%d PORTSC=%08x (CCS=%d PED=%d PLS=%d speed=%d)", I + 1, Ps, Ps & 1, (Ps >> 1) & 1, (Ps >> 5) & 0xF, (Ps >> 10) & 0xF);
        Last[I] = Ps;
      }
    }
    Us (10000);
  }
  Log (L"svc: done");
}

// ---------------------------------------------------------------------------------------------
// Resident mailbox service. `svc` only answers the firmware while the shell command runs; the xHCI
// firmware asks for clock ACKs again whenever a SuperSpeed device is (un)plugged or the link changes
// power state, so without a resident responder USB3 stalls right after the command returns.
// This keeps answering from a periodic timer until ExitBootServices (after that nobody answers: an OS
// needs its own mailbox handler, see README.fsdrv).
// ---------------------------------------------------------------------------------------------
STATIC EFI_EVENT  mDaemonTimer = NULL;
STATIC EFI_EVENT  mDaemonExit  = NULL;
STATIC BOOLEAN    mDaemonOn    = FALSE;

STATIC
VOID
EFIAPI
DaemonTick (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (HostAlive ()) {                       // never touch 0x7009xxxx unless clocks/resets say it is alive
    SvcHandleSmi ();
  }
}

STATIC
VOID
DaemonStop (
  VOID
  )
{
  if (mDaemonTimer != NULL) {
    gBS->SetTimer (mDaemonTimer, TimerCancel, 0);
    gBS->CloseEvent (mDaemonTimer);
    mDaemonTimer = NULL;
  }
  if (mDaemonExit != NULL) {
    gBS->CloseEvent (mDaemonExit);
    mDaemonExit = NULL;
  }
  mDaemonOn = FALSE;
}

STATIC
VOID
EFIAPI
DaemonOnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mDaemonTimer != NULL) {
    gBS->SetTimer (mDaemonTimer, TimerCancel, 0);
  }
  mDaemonOn = FALSE;
}

STATIC
VOID
StepDaemon (
  IN BOOLEAN  Start
  )
{
  EFI_STATUS  Status;

  if (!Start) {
    DaemonStop ();
    mQuiet = FALSE;
    Log (L"daemon: stopped");
    return;
  }
  if (mDaemonOn) {
    Log (L"daemon: already running");
    return;
  }
  if (!HostAlive ()) {
    Log (L"daemon: host not up (run `xusbinit up` first)");
    return;
  }
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, DaemonTick, NULL, &mDaemonTimer);
  if (!EFI_ERROR (Status)) {
    Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, DaemonOnExitBootServices, NULL,
                                 &gEfiEventExitBootServicesGuid, &mDaemonExit);
  }
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (mDaemonTimer, TimerPeriodic, 100000);          // every 10 ms
  }
  if (EFI_ERROR (Status)) {
    Log (L"daemon: cannot start: %r", Status);
    DaemonStop ();
    return;
  }
  mDaemonOn = TRUE;
  Log (L"daemon: running (mailbox answered every 10 ms until ExitBootServices); log: `xusbinit dlog`");
  mQuiet = TRUE;                            // from now on Log() goes to the ring buffer
}

STATIC
VOID
StepDLog (
  VOID
  )
{
  if (mDLogLen == 0) {
    Print (L"dlog: empty\n");
  } else {
    Print (L"%s", mDLog);
  }
}

// ---------------------------------------------------------------------------------------------
// ACPI USB controller selection. With -D SWITCH_ACPI_XHCI=1 -D SWITCH_ACPI_EHCI=1 the DSDT describes both
// EHC0 (inbox usbehci, USB1) and XHC0 (usbxhci, XUSB) but only one may be visible: they drive the same
// USB-C port. The DSDT holds `Name (UBSL, Zero)`; _STA of EHC0 is "UBSL == 0", of XHC0 "UBSL == 1".
// The AML for that name is 08 'U' 'B' 'S' 'L' 00, so selecting XHC0 = patching the last byte to 01
// (OneOp) and fixing the table checksum. Builds with only one of the flags have no UBSL: nothing to do.
// `xusbinit acpi [xhci|ehci]` shows / forces the choice. Without a forced choice the firmware picks XHC0
// at ExitBootServices when the xHCI host is up (xusbinit up/boot/daemon ran), else EHC0.
// ---------------------------------------------------------------------------------------------
#define ACPI_SEL_NOTFOUND  (-1)

STATIC INTN     mAcpiForced = -1;           // -1 = automatic, 0 = EHC0, 1 = XHC0
#ifdef XUSB_DYNAMIC
STATIC EFI_EVENT mAcpiSelEvent = NULL;
#endif

STATIC
UINT32
AcpiRd32 (
  IN CONST UINT8  *P
  )
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC
UINT64
AcpiRd64 (
  IN CONST UINT8  *P
  )
{
  return (UINT64)AcpiRd32 (P) | ((UINT64)AcpiRd32 (P + 4) << 32);
}

// Find the loaded DSDT, locate UBSL and return a pointer to its value byte (NULL if not there).
STATIC
UINT8 *
AcpiFindUbsl (
  OUT UINT8   **DsdtOut,
  OUT UINT32  *DsdtLen
  )
{
  VOID    *Rsdp;
  UINT8   *R;
  UINT8   *X;
  UINT8   *F;
  UINT8   *D;
  UINT32  N;
  UINT32  I;
  UINT32  L;
  UINT64  Addr;

  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, &Rsdp)) || Rsdp == NULL) {
    return NULL;
  }
  R = (UINT8 *)Rsdp;
  if (CompareMem (R, "RSD PTR ", 8) != 0 || R[15] < 2) {
    return NULL;
  }
  X = (UINT8 *)(UINTN)AcpiRd64 (R + 24);
  if (X == NULL || CompareMem (X, "XSDT", 4) != 0) {
    return NULL;
  }
  N = (AcpiRd32 (X + 4) - 36) / 8;
  for (I = 0; I < N; I++) {
    F = (UINT8 *)(UINTN)AcpiRd64 (X + 36 + 8 * I);
    if (F == NULL || CompareMem (F, "FACP", 4) != 0) {
      continue;
    }
    L    = AcpiRd32 (F + 4);
    Addr = (L >= 148) ? AcpiRd64 (F + 140) : 0;          // X_DSDT (offset 140: not 8-byte aligned, read bytewise)
    if (Addr == 0) {
      Addr = AcpiRd32 (F + 40);                          // DSDT
    }
    D = (UINT8 *)(UINTN)Addr;
    if (D == NULL || CompareMem (D, "DSDT", 4) != 0) {
      return NULL;
    }
    L = AcpiRd32 (D + 4);
    for (I = 36; I + 6 <= L; I++) {
      if (D[I] == 0x08 && D[I + 1] == 'U' && D[I + 2] == 'B' && D[I + 3] == 'S' && D[I + 4] == 'L' &&
          (D[I + 5] == 0x00 || D[I + 5] == 0x01)) {
        *DsdtOut = D;
        *DsdtLen = L;
        return &D[I + 5];
      }
    }
    return NULL;
  }
  return NULL;
}

// Set UBSL (0 = EHC0, 1 = XHC0), fix the checksum. Returns the value now in the table or ACPI_SEL_NOTFOUND.
STATIC
INTN
AcpiSelect (
  IN INTN  Value                                         // -1: only read
  )
{
  UINT8   *D;
  UINT8   *V;
  UINT32  L;
  UINT32  I;
  UINT8   Sum;

  V = AcpiFindUbsl (&D, &L);
  if (V == NULL) {
    return ACPI_SEL_NOTFOUND;
  }
  if (Value >= 0 && *V != (UINT8)Value) {
    *V = (UINT8)Value;
    D[9] = 0;
    for (Sum = 0, I = 0; I < L; I++) {
      Sum = (UINT8)(Sum + D[I]);
    }
    D[9] = (UINT8)(0 - Sum);
  }
  return *V;
}

#ifdef XUSB_DYNAMIC
STATIC
VOID
EFIAPI
AcpiSelectOnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mAcpiForced >= 0) {
    AcpiSelect (mAcpiForced);
  } else {
    AcpiSelect (HostAlive () ? 1 : 0);
  }
}
#endif

STATIC
VOID
StepAcpi (
  IN CONST CHAR16  *Arg
  )
{
  INTN  R;

  if (Arg != NULL && StrCmp (Arg, L"xhci") == 0) {
    mAcpiForced = 1;
  } else if (Arg != NULL && StrCmp (Arg, L"ehci") == 0) {
    mAcpiForced = 0;
  } else if (Arg != NULL && StrCmp (Arg, L"auto") == 0) {
    mAcpiForced = -1;
  }
  R = AcpiSelect (mAcpiForced);                          // applies a forced choice now, else only reads
  if (R == ACPI_SEL_NOTFOUND) {
    Log (L"acpi: no UBSL in the DSDT (build with both -D SWITCH_ACPI_XHCI=1 and -D SWITCH_ACPI_EHCI=1)");
  } else {
    Log (L"acpi: DSDT selects %s (%s); xHCI host %s",
         R == 1 ? L"XHC0 (xHCI)" : L"EHC0 (EHCI)",
         mAcpiForced < 0 ? L"automatic: decided again at ExitBootServices" : L"forced",
         HostAlive () ? L"up" : L"down");
  }
}

// Registered once, from the resident (dynamic command) build only: an event handler in a shell
// application image would outlive the image.
STATIC
VOID
AcpiSelectArm (
  VOID
  )
{
#ifdef XUSB_DYNAMIC
  if (mAcpiSelEvent == NULL) {
    gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, AcpiSelectOnExitBootServices, NULL,
                        &gEfiEventExitBootServicesGuid, &mAcpiSelEvent);
  }
#endif
}

// ---------------------------------------------------------------------------------------------
STATIC
EFI_STATUS
RunXusb (
  IN UINTN    Argc,
  IN CHAR16   **Argv
  )
{
  CONST CHAR16  *Cmd;
  CONST CHAR16  *Arg;

  if (Argc < 2) {
    Print (L"usage: xusbinit info|pmc|pg|clk|pad|ss|fw [file]|svc [sec]|ports|restore|all [file]|up [file]|daemon [stop]|dlog|acpi [xhci|ehci|auto]\n");
    return EFI_SUCCESS;
  }
  Cmd = Argv[1];
  Arg = Argc > 2 ? Argv[2] : NULL;

  if (EFI_ERROR (ShellOpenFileByName (L"xusb.log", &mLog, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
    mLog = NULL;
  } else {
    UINT64  End = 0;
    ShellGetFileSize (mLog, &End);
    ShellSetFilePosition (mLog, End);
  }
  Log (L"xusbinit %s (draft)", Cmd);

  if (StrCmp (Cmd, L"info") == 0) {
    StepInfo ();
  } else if (StrCmp (Cmd, L"pmc") == 0) {
    StepPmc ();
  } else if (StrCmp (Cmd, L"pg") == 0) {
    StepPg ();
  } else if (StrCmp (Cmd, L"clk") == 0) {
    StepClk ();
  } else if (StrCmp (Cmd, L"pad") == 0) {
    StepPad ();
  } else if (StrCmp (Cmd, L"fw") == 0) {
    StepFw (Arg != NULL ? Arg : L"xusb.bin");
  } else if (StrCmp (Cmd, L"svc") == 0) {
    StepSvc (Arg != NULL ? StrDecimalToUintn (Arg) : 30);
  } else if (StrCmp (Cmd, L"ports") == 0) {
    StepPorts ();
  } else if (StrCmp (Cmd, L"ss") == 0) {
    StepSs ();
  } else if (StrCmp (Cmd, L"restore") == 0) {
    StepRestore ();
  } else if (StrCmp (Cmd, L"daemon") == 0) {
    StepDaemon (!(Arg != NULL && StrCmp (Arg, L"stop") == 0));
  } else if (StrCmp (Cmd, L"dlog") == 0) {
    StepDLog ();
  } else if (StrCmp (Cmd, L"acpi") == 0) {
    StepAcpi (Arg);
  } else if (StrCmp (Cmd, L"up") == 0) {
    // clk, pad, ss, firmware, then keep answering the firmware in the background
    StepClk ();
    StepPad ();
    StepSs ();
    StepFw (Arg != NULL ? Arg : L"xusb.bin");
    StepPorts ();
    StepDaemon (TRUE);
  } else if (StrCmp (Cmd, L"all") == 0) {
    StepClk ();
    StepPad ();
    StepSs ();
    StepFw (Arg != NULL ? Arg : L"xusb.bin");
    StepPorts ();
    StepSvc (30);
  } else {
    Log (L"unknown command %s", Cmd);
  }

  AcpiSelectArm ();                         // select XHC0/EHC0 in the DSDT at ExitBootServices

  if (mLog != NULL) {
    ShellCloseFile (&mLog);
  }
  return EFI_SUCCESS;
}

#ifndef XUSB_DYNAMIC

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_SHELL_PARAMETERS_PROTOCOL  *P;

  if (EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiShellParametersProtocolGuid, (VOID **)&P))) {
    Print (L"xusbinit: run from the UEFI Shell\n");
    return EFI_SUCCESS;
  }
  return RunXusb (P->Argc, P->Argv);
}

#else

//
// Built into the firmware volume as a shell dynamic command ("xusbinit"): no file needed on the SD card.
//
STATIC
SHELL_STATUS
EFIAPI
XusbCmdHandler (
  IN EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL  *This,
  IN EFI_SYSTEM_TABLE                    *SystemTable,
  IN EFI_SHELL_PARAMETERS_PROTOCOL       *ShellParameters,
  IN EFI_SHELL_PROTOCOL                  *Shell
  )
{
  gEfiShellParametersProtocol = ShellParameters;
  gEfiShellProtocol           = Shell;
  return (SHELL_STATUS)RunXusb (ShellParameters->Argc, ShellParameters->Argv);
}

STATIC
CHAR16 *
EFIAPI
XusbCmdGetHelp (
  IN EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL  *This,
  IN CONST CHAR8                         *Language
  )
{
  STATIC CONST CHAR16  Help[] =
    L"xusbinit info|pmc|pg|clk|pad|fw [file]|svc [sec]|ports|restore|all [file]|up [file]|daemon [stop]|dlog|acpi [xhci|ehci|auto]  (Tegra210 XUSB bring-up, DRAFT; log: xusb.log)\r\n";

  return AllocateCopyPool (sizeof (Help), Help);
}

STATIC EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL  mXusbCommand = {
  L"xusbinit",
  XusbCmdHandler,
  XusbCmdGetHelp
};

EFI_STATUS
EFIAPI
XusbCmdInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return gBS->InstallProtocolInterface (&ImageHandle, &gEfiShellDynamicCommandProtocolGuid, EFI_NATIVE_INTERFACE, &mXusbCommand);
}

#endif
