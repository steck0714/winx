/*
 * UsbPowerAuditDxe - Tegra210 board bring-up for Windows on the Nintendo Switch.
 *
 * Source: hayato41200/Windows-for-Switch (https://github.com/hayato41200/Windows-for-Switch),
 * GPL-2.0. Ported unchanged into winx (SWITCH_WOA build); register sequences follow hekate
 * (CTCaer, GPL-2.0) and Linux tegra210 drivers. Credit: hayato41200.
 */
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/IoLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/PrintLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/I2cLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/DevicePath.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Protocol/PciIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/UsbIo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/Utc/Tegra210/PinMux.h>

#define BASE_ADDR       0x7000C000UL
#define CNFG            0x00
#define TX_FIFO         0x14
#define RX_FIFO         0x15
#define FIFO_CONTROL    0x17
#define FIFO_STATUS     0x18
#define INT_MASK_REG    0x19
#define INT_STATUS_REG  0x1A
#define INT_EN_REG      0x1B
#define BUS_CLEAR_CONFIG 0x21
#define CONFIG_LOAD     0x23
#define ERR_MASK        0x3C
#define ALL_COMPLETE    0x40
#define HDR0            0x10010
#define REPEAT_START    0x10000
#define READ_FLAG       0x80000
#define BM_ADDR         0x18
#define BM_STATUS1      0x03
#define BM_STATUS2      0x04
#define BM_CONFIG1      0x06
#define BM_DP_STATUS    0x18
#define BM_VENDOR_CONFIG 0x1A
#define BM_FW_TYPE      0x4B
#define BM_FW_REV       0x4C
#define BQ_ADDR         0x6B
#define BQ_PWR_ON       0x01
#define BQ_SYS_STAT     0x08
#define BQ_REVISION     0x0A
#define BQ_TERM_TIMER   0x05
#define BQ_WATCHDOG_MASK (3U << 4)
#define BM_SPDSRC_MASK  (3U << 14)
#define BM_OCP_DISABLE  (1U << 2)
#define BM_INSERT       (1U << 7)
#define BM_SRC_MODE     (1U << 12)
#define BM_OTG_INSERT   (1U << 13)
#define BQ_CHARGE_MODE_MASK (3U << 4)
#define BQ_VBUS_MODE    (2U << 4)
// R363: USB-C hub + charger support (BM92T in sink mode, BQ2419x charging).
// Register/bit names follow the L4T bm92txx driver.
#define BM_COMMAND      0x05
#define BM_READ_PDOS_SRC 0x08
#define BM_CURRENT_PDO  0x28
#define BM_CURRENT_RDO  0x2B
#define BM_VSAFE        (1U << 10)
#define BM_CMD_BUSY     (1U << 13)
#define BM_SPDSNK       (1U << 14)
#define BM_DR_SHIFT     8
#define BM_DR_MASK      (3U << BM_DR_SHIFT)
#define BM_DR_UFP       1U
#define BM_DR_DFP       2U
#define BM_DR_SWAP_CMD  0x1818U
#define BM_PDO_DRD      (1U << 25)
#define BQ_INPUT_SRC    0x00
#define BQ_FAULT        0x09
#define BQ_HIZ          (1U << 7)
#define BQ_IINLIM_MASK  0x07U
#define BQ_CHARGE_MODE  (1U << 4)
#define BQ_RESET_BITS   (3U << 6)
#define MAX17050_ADDR   0x36
#define MAX17050_REPSOC 0x06
#define MAX17050_VCELL  0x09
#define MAX17050_CURRENT 0x0A
#define R363_SINK_CONTRACT_POLLS 10
// R364: CPU clock. coreboot leaves PLLX at 38.4 MHz x 18 = 691.2 MHz with the
// MAX77621 VDD_CPU at 950 mV (VOUT 0xB7). This unit is CPU speedo ID 3
// (SKU 0x83, Linux TEGRA210_CPU_CVB_TABLE_ODN): 1020 MHz needs 985 mV under
// DFLL. PLLX is open loop, so R364 adds margin and uses 1025 mV.
#define TMP451_ADDR     0x4C
#define MAX77621_CPU_ADDR 0x1B
#define MAX77621_VOUT   0x00
#define MAX77621_VOUT_DVC 0x01
#define MAX77621_VOUT_ENABLE 0x80U
#define MAX77621_MIN_UV 606250U
#define MAX77621_STEP_UV 6250U
#define CAR_CCLK_BURST_POLICY 0x020U
#define CAR_SUPER_CCLK_DIVIDER 0x024U
#define CAR_PLLX_BASE   0x0E0U
#define CAR_PLLX_MISC   0x0E4U
#define PLLX_ENABLE     (1U << 30)
#define PLLX_BYPASS     (1U << 31)
#define PLLX_LOCK       (1U << 27)
#define PLLX_LOCK_ENABLE (1U << 18)
#define CCLK_ON_PLLX    0x20008888U
// R365: R364 hung after parking CCLK on PLLP (0x20004444): on T210 the PLLP
// branch to the CPU (CLK_ENB_Y bit 31, PLLP_OUT_CPU) is off after a cold
// coreboot boot (coreboot enables it only on LP0 resume). Park on CLK_M
// (38.4 MHz oscillator, source 0) instead, which needs no gate.
#define CCLK_ON_CLKM    0x20000000U
#define CAR_CLK_OUT_ENB_Y 0x298U
#define CAR_CCLKG_BURST_POLICY 0x368U
#define R364_OSC_KHZ    38400U
#define R364_TARGET_M   2U
#define R364_TARGET_N   53U         // 38.4 / 2 x 53 = 1017.6 MHz
#define R364_TARGET_MV  1025U
#define R364_MAX_MV     1227U       // ODN table max_millivolts
// R366: fan, following Hekate bdk/thermal/fan.c and regulator_5v.c. PWM1
// drives the fan through pin LCD_GPIO2 (GPIO PV4) with inverted polarity
// (register duty = 236 - duty, 0x100 = off). The fan runs from the 5V
// regulator enabled by GPIO PA5 (pin SATA_LED_ACTIVE). Tachometer on PS7
// (pin CAM1_PWDN), 2 pulses per revolution.
#define PWM_BASE        0x7000A000UL
#define PWM_CSR_1       0x10U
#define PWM_CSR_EN      (1U << 31)
#define PINMUX_SATA_LED_ACTIVE 0x04CU
#define PINMUX_USB_VBUS_EN0 0x1A8U
#define PINMUX_CAM1_PWDN 0x1ECU
#define PINMUX_LCD_GPIO2 0x20CU
#define PINMUX_PULL_UP_BITS (2U << 2)
#define PINMUX_TRISTATE_BIT (1U << 4)
#define PINMUX_INPUT_EN_BIT (1U << 6)
#define GPIO_PA_OFFSET  0x000U      // port A: bank 0, port 0
#define GPIO_PS_OFFSET  0x408U      // port S: bank 4, port 2
#define GPIO_PV_OFFSET  0x504U      // port V: bank 5, port 1
#define GPIO_PCC_OFFSET 0x700U      // port CC: bank 7, port 0
#define GPIO_CNF        0x00U
#define GPIO_OE         0x10U
#define GPIO_OUT        0x20U
#define GPIO_IN         0x30U
#define CAR_RST_DEVICES_L 0x004U
#define CAR_CLK_OUT_ENB_L 0x010U
#define CAR_CLK_SOURCE_PWM 0x110U
#define CAR_RST_DEV_L_SET 0x300U
#define CAR_RST_DEV_L_CLR 0x304U
#define CAR_CLK_ENB_L_SET 0x320U
#define CLK_L_PWM_BIT   (1U << 17)
#define R366_FAN_DUTY   76U         // of 236 (about 32%); Hekate uses 76 at 47-52 C
#define R366_FAN_MAX    236U
#define R290_PADCTL_CLK_MASK (1U << 14)
#define R290_CLK_ENB_W_SET_REG 0x448U
#define R290_CLK_ENB_W_CLR_REG 0x44CU
#define R290_USB2_VBUS_ID_REG 0xC60U
#define R290_ID_OVERRIDE_MASK (0xFU << 18)
#define R290_ID_OVERRIDE_GROUNDED 0U
#define R288_HOST_POLLS 10       // R378: was 30; hot-plug is handled in Windows now
#define R288_HOST_STREAK 3
#define XUSB_HCD_BASE   0x70090000UL
#define XUSB_FPCI_BASE  0x70098000UL
#define XUSB_IPFS_BASE  0x70099000UL
#define XUSB_PAD_BASE   0x7009F000UL
#define CAR_BASE        0x60006000UL
#define PMC_BASE        0x7000E400UL
#define PINMUX_BASE     0x70003000UL
#define GPIO_BASE       0x6000D000UL
#define I2C2_BASE       0x7000C500UL

STATIC EFI_EVENT mStartEvent;
STATIC UINT32 mOldCnfg, mOldIntMask, mOldFifo, mOldIntEn, mOldBusClear;
STATIC BOOLEAN mControllerSaved;
STATIC UINT16 mOldConfig1;
STATIC BOOLEAN mBmChanged;
STATIC UINT32 mOldPlluBase;
STATIC BOOLEAN mPllu480Changed;
STATIC EFI_PHYSICAL_ADDRESS mFirmwareAddress;
STATIC UINTN mFirmwarePages;
STATIC UINTN mFirmwareSize;
STATIC EFI_FILE_PROTOCOL *mLogFile;
STATIC BOOLEAN mR300RegistrationAttempted;
STATIC BOOLEAN mR300RegistrationPassed;
STATIC BOOLEAN mR300CsbAttempted;
STATIC BOOLEAN mR300CsbPassed;
STATIC BOOLEAN mR300TransferAttempted;
STATIC BOOLEAN mR300TransferPassed;
STATIC BOOLEAN mR300FalconAttempted;
STATIC BOOLEAN mR300FalconPassed;
STATIC BOOLEAN mR300OperationalAttempted;
STATIC BOOLEAN mR300OperationalPassed;
STATIC BOOLEAN mR300RunStopAttempted;
STATIC BOOLEAN mR300RunStopPassed;
STATIC BOOLEAN mR304ProbeAttempted;
STATIC BOOLEAN mR304NdSupportedPassed;
STATIC BOOLEAN mR304NdStartPassed;
STATIC BOOLEAN mR304PciIoObserved;
STATIC BOOLEAN mR304XhciSupportedPassed;
STATIC BOOLEAN mR304NdStopPassed;
STATIC BOOLEAN mR304PciIoRemoved;
STATIC BOOLEAN mR304RegistrationCleanupPassed;
STATIC BOOLEAN mR309XhciStartAttempted;
STATIC BOOLEAN mR309XhciStartPassed;
STATIC BOOLEAN mR309XhciStopAttempted;
STATIC BOOLEAN mR309XhciStopPassed;
STATIC BOOLEAN mR309Usb2HcRemoved;
STATIC BOOLEAN mR309XhciRunVerified;
STATIC BOOLEAN mR309Usb2HcInstalled;
// R362 production mode: bring XUSB up with the R243-R260 sequence, apply the
// R357-proven PADCTL host settings, register the controller and connect the
// stock XhciDxe/UsbBus/UsbMassStorage stack, and keep everything running (no
// restore, no dead-loop). BM92T/BQ VBUS stay in host/OTG mode.
STATIC BOOLEAN mR362Production = TRUE;
STATIC BOOLEAN mR362Attached;
STATIC EFI_EVENT mR362StatusEvent;
STATIC UINTN mR362StatusRound;
// R363: TRUE when the partner sources power (USB-C hub with a charger). The
// Switch then sinks power and charges, and XUSB is still brought up as host.
STATIC BOOLEAN mR363SinkMode;
// R378: no partner at boot. XUSB is still brought up (without VBUS) so that
// switchbattery.sys v4 can switch VBUS / the charge path when something is
// plugged in under Windows.
STATIC BOOLEAN mR378NoPartner;

typedef enum {
  R300_GATE_NOT_ENTERED = 0,
  R300_GATE_PATH_SETUP,
  R300_GATE_FW_FILE,
  R300_GATE_R243_BASELINE,
  R300_GATE_CLOCK_POWER,
  R300_GATE_FPCI_MAP,
  R300_GATE_CSB,
  R300_GATE_FIRMWARE,
  R300_GATE_FALCON,
  R300_GATE_OPERATIONAL,
  R300_GATE_RUNSTOP,
  R300_GATE_REGISTRATION,
  R300_GATE_DONE
} R300_GATE;

STATIC R300_GATE mR300Gate = R300_GATE_NOT_ENTERED;

STATIC CONST CHAR16 *R300GateName (R300_GATE Gate) {
  switch (Gate) {
    case R300_GATE_PATH_SETUP:    return L"PATH-SETUP";
    case R300_GATE_FW_FILE:      return L"FW-FILE";
    case R300_GATE_R243_BASELINE: return L"R243-BASELINE";
    case R300_GATE_CLOCK_POWER:   return L"CLOCK/POWER";
    case R300_GATE_FPCI_MAP:      return L"FPCI/MMIO";
    case R300_GATE_CSB:           return L"CSB";
    case R300_GATE_FIRMWARE:      return L"FW-TRANSFER";
    case R300_GATE_FALCON:        return L"FALCON";
    case R300_GATE_OPERATIONAL:  return L"HCD-OP";
    case R300_GATE_RUNSTOP:      return L"RUN/STOP";
    case R300_GATE_REGISTRATION: return L"ND-REGISTER";
    case R300_GATE_DONE:         return L"DONE";
    default:                     return L"NOT-ENTERED";
  }
}

STATIC CONST CHAR16 *R300StageStatus (BOOLEAN Attempted, BOOLEAN Passed) {
  if (!Attempted) return L"SKIP";
  return Passed ? L"PASS" : L"FAIL";
}

typedef enum {
  R260_PADCTL_PENDING = 0,
  R260_PADCTL_OK,
  R260_PADCTL_CAR_UNREADABLE,
  R260_PADCTL_RESET_ASSERTED,
  R260_PADCTL_CLOCK_VERIFY_FAILED,
  R260_PADCTL_REGS_INVALID,
  R260_PADCTL_RESTORE_FAILED
} R260_PADCTL_STATUS;

typedef struct {
  UINT32 ClockW;
  UINT32 ResetW;
  UINT32 Mux;
  UINT32 PortCap;
  UINT32 Pad0Ctl0;
  UINT32 Pad0Ctl1;
  UINT32 BiasCtl0;
  UINT32 BiasCtl1;
  UINT32 VbusId;
  BOOLEAN GateRead;
  BOOLEAN PadRead;
  BOOLEAN PadValid;
  BOOLEAN ClockAttempted;
  BOOLEAN ClockVerified;
  BOOLEAN RestoreAttempted;
  BOOLEAN RestoreOk;
  R260_PADCTL_STATUS Status;
} R260_PADCTL_SAMPLE;

STATIC R260_PADCTL_SAMPLE mR260EarlyPadctl;
STATIC R260_PADCTL_SAMPLE mR260RunPadctl;
STATIC UINT32 mR290OriginalPadctlClockW;
STATIC UINT32 mR290OriginalVbusId;
STATIC BOOLEAN mR290IdOverrideAttempted;
STATIC BOOLEAN mR290IdOverrideReady;
STATIC BOOLEAN mR290IdOverrideChanged;
STATIC BOOLEAN mR290PadctlClockDeferred;
STATIC BOOLEAN mR290IdOverrideRestoreOk = TRUE;
// R291 keeps a compact audit trail for Linux-vs-ROM comparison.
STATIC UINT32 mR291ClockPreW, mR291ResetPreW;
STATIC UINT32 mR291ClockPostSetW, mR291ResetPostSetW;
STATIC UINT32 mR291ClockPostRestoreW, mR291ResetPostRestoreW;
STATIC BOOLEAN mR291PreValid, mR291PostSetValid, mR291PostRestoreValid;
STATIC BOOLEAN mR291SetAttempted;
// 0=not run, 1=unchanged/not needed, 2=deferred, 3=PASS, 4=FAIL.
STATIC UINT8 mR291RestoreState;
// R292 brackets firmware-file discovery/read with read-only CAR snapshots.
STATIC UINT32 mR292FilePreClockW, mR292FilePreResetW, mR292FilePrePlluBase;
STATIC UINT32 mR292FilePostClockW, mR292FilePostResetW, mR292FilePostPlluBase;
STATIC BOOLEAN mR292FilePreValid, mR292FilePostValid;
// R293: compare raw CAR clock/reset banks at bounded lifecycle stages.
typedef struct {
  UINT32 ClockW, ClockY, ResetW, ResetY;
  BOOLEAN Valid;
} R293_CAR_SNAPSHOT;
enum { R293_EARLY, R293_FILE_PRE, R293_FILE_POST, R293_XHCI_RUN, R293_FINAL, R293_STAGE_COUNT };
STATIC R293_CAR_SNAPSHOT mR293Car[R293_STAGE_COUNT];
STATIC CONST CHAR16 *mR293StageName[R293_STAGE_COUNT] = {
  L"EARLY", L"FILE_PRE", L"FILE_POST", L"XHCI_RUN", L"FINAL"
};
// R294 compares Tegra210 USB2 UTMI pad state at the live xHCI window and
// after cleanup. These are strictly read-only samples; no Linux PHY writes.
typedef struct {
  UINT32 PadMux, PortCap, Pad0Ctl0, Pad0Ctl1;
  UINT32 BiasCtl0, BiasCtl1, VregCtl1, VbusId;
  BOOLEAN Valid;
} R294_USB2_SNAPSHOT;
enum { R294_USB2_RUN, R294_USB2_FINAL, R294_USB2_STAGE_COUNT };
STATIC R294_USB2_SNAPSHOT mR294Usb2[R294_USB2_STAGE_COUNT];
STATIC CONST CHAR16 *mR294Usb2StageName[R294_USB2_STAGE_COUNT] = {
  L"XHCI_RUN", L"FINAL"
};
// R295 temporarily matches only the three Linux-confirmed USB2 path fields.
// It deliberately does not copy UTMI/BIAS/VREG calibration register values.
typedef struct {
  UINT32 ClockY, PadMux, PortCap;
  UINT32 TrackLive, MuxLive, CapLive;
  UINT32 TrackFinal, MuxFinal, CapFinal;
  BOOLEAN Valid, Attempted, Applied, Restored;
} R295_USB2_PATH_STATE;
STATIC R295_USB2_PATH_STATE mR295Path;
STATIC UINT32 mR260Usb2PortSc[5];
STATIC UINT32 mR260Usb2PortValidMask;
STATIC UINTN mR260P5Samples;
STATIC UINT32 mR260P5First;
STATIC UINT32 mR260P5Last;
STATIC UINT32 mR260P5Changes;
STATIC UINT32 mR260P5ConnectedSamples;
STATIC BOOLEAN mR260P5SampleValid;

STATIC UINT32 ReadLe32 (CONST UINT8 *P) {
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC VOID CaptureR293Car (UINTN Stage) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  R293_CAR_SNAPSHOT *S;
  if (Stage >= R293_STAGE_COUNT) return;
  S = &mR293Car[Stage];
  S->ClockW = Car[0x364 / 4]; // CLK_ENB_W: bit15 is XUSB_GATE; bit14 is not assumed.
  S->ClockY = Car[0x298 / 4]; // CLK_ENB_Y: Linux shows USB2_TRK bit18, HSIC_TRK bit17.
  S->ResetW = Car[0x35C / 4]; // RST_DEVICES_W, raw bank only.
  S->ResetY = Car[0x2A4 / 4]; // RST_DEVICES_Y, raw bank only.
  S->Valid = S->ClockW != 0xFFFFFFFFU && S->ClockY != 0xFFFFFFFFU &&
             S->ResetW != 0xFFFFFFFFU && S->ResetY != 0xFFFFFFFFU;
}

STATIC VOID PrintR293CarSnapshots (VOID) {
  UINTN I;
  for (I = 0; I < R293_STAGE_COUNT; ++I) {
    R293_CAR_SNAPSHOT *S = &mR293Car[I];
    Print (L"R293 CAR %s valid=%u CLK_W/Y=%08x/%08x RST_W/Y=%08x/%08x\r\n",
           mR293StageName[I], S->Valid, S->ClockW, S->ClockY,
           S->ResetW, S->ResetY);
  }
  Print (L"R293 decode only: CLK_W bit15=XUSB_GATE; CLK_Y bit18=USB2_TRK bit17=HSIC_TRK; other fields raw.\r\n");
  Print (L"CAR snapshots are reads only; no guessed W bit14 or PADCTL ID_OVERRIDE write.\r\n");
}

STATIC VOID CaptureR294Usb2 (UINTN Stage) {
  volatile UINT32 *P = (volatile UINT32 *)XUSB_PAD_BASE;
  R294_USB2_SNAPSHOT *S;
  if (Stage >= R294_USB2_STAGE_COUNT) return;
  S = &mR294Usb2[Stage];
  S->PadMux = P[0x004 / 4];
  S->PortCap = P[0x008 / 4];
  S->VregCtl1 = P[0x084 / 4];
  S->Pad0Ctl0 = P[0x088 / 4];
  S->Pad0Ctl1 = P[0x08C / 4];
  S->BiasCtl0 = P[0x284 / 4];
  S->BiasCtl1 = P[0x288 / 4];
  S->VbusId = P[0xC60 / 4];
  S->Valid = S->PadMux != 0xFFFFFFFFU && S->PortCap != 0xFFFFFFFFU &&
             S->VregCtl1 != 0xFFFFFFFFU && S->Pad0Ctl0 != 0xFFFFFFFFU &&
             S->Pad0Ctl1 != 0xFFFFFFFFU && S->BiasCtl0 != 0xFFFFFFFFU &&
             S->BiasCtl1 != 0xFFFFFFFFU && S->VbusId != 0xFFFFFFFFU;
}

STATIC VOID PrintR294Usb2Snapshots (VOID) {
  UINTN I;
  for (I = 0; I < R294_USB2_STAGE_COUNT; ++I) {
    R294_USB2_SNAPSHOT *S = &mR294Usb2[I];
    Print (L"R294 PHY %s valid=%u MUX=%08x CAP=%08x VREG=%08x P0=%08x/%08x\r\n",
           mR294Usb2StageName[I], S->Valid, S->PadMux, S->PortCap,
           S->VregCtl1, S->Pad0Ctl0, S->Pad0Ctl1);
    Print (L"R294 PHY %s BIAS0/1=%08x/%08x VBUS_ID=%08x\r\n",
           mR294Usb2StageName[I], S->BiasCtl0, S->BiasCtl1, S->VbusId);
  }
  Print (L"R294 PHY snapshots are read-only observations; R295 separately tests three Linux-matched fields.\r\n");
}

STATIC BOOLEAN R295BeginLinuxMatchedPath (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Pad = (volatile UINT32 *)XUSB_PAD_BASE;
  CONST UINT32 TrackMask = (1U << 18);
  CONST UINT32 BiasMuxMask = (3U << 18);
  CONST UINT32 OtgCapMask = 3U;
  UINT32 Value;

  ZeroMem (&mR295Path, sizeof (mR295Path));
  mR295Path.ClockY = Car[0x298 / 4];
  mR295Path.PadMux = Pad[0x004 / 4];
  mR295Path.PortCap = Pad[0x008 / 4];
  mR295Path.Valid = mR295Path.ClockY != 0xFFFFFFFFU &&
                    mR295Path.PadMux != 0xFFFFFFFFU &&
                    mR295Path.PortCap != 0xFFFFFFFFU;
  mR295Path.TrackLive = mR295Path.TrackFinal = mR295Path.ClockY;
  mR295Path.MuxLive = mR295Path.MuxFinal = mR295Path.PadMux;
  mR295Path.CapLive = mR295Path.CapFinal = mR295Path.PortCap;
  Print (L"R295 saved CLK_ENB_Y/MUX/CAP=%08x/%08x/%08x valid=%u\r\n",
         mR295Path.ClockY, mR295Path.PadMux, mR295Path.PortCap,
         mR295Path.Valid);
  if (!mR295Path.Valid) {
    Print (L"R295 STOP: baseline read invalid; no Linux-matched writes.\r\n");
    return FALSE;
  }

  mR295Path.Attempted = TRUE;
  if ((mR295Path.ClockY & TrackMask) == 0) {
    Car[0x29C / 4] = TrackMask; // CAR CLK_ENB_Y SET alias.
    gBS->Stall (10);
  }
  Value = Car[0x298 / 4];
  mR295Path.TrackLive = Value;
  Print (L"R295 USB2_TRK readback=%08x bit18=%u\r\n", Value,
         (Value >> 18) & 1U);
  if (Value == 0xFFFFFFFFU || (Value & TrackMask) == 0) goto Restore;

  Value = (mR295Path.PadMux & ~BiasMuxMask) | (1U << 18);
  Pad[0x004 / 4] = Value;
  gBS->Stall (10);
  Value = Pad[0x004 / 4];
  mR295Path.MuxLive = Value;
  Print (L"R295 PAD_MUX readback=%08x bias_sel=%u (XUSB=1)\r\n",
         Value, (Value >> 18) & 3U);
  if (Value == 0xFFFFFFFFU || ((Value >> 18) & 3U) != 1U) goto Restore;

  Value = (mR295Path.PortCap & ~OtgCapMask) | 3U;
  Pad[0x008 / 4] = Value;
  gBS->Stall (10);
  Value = Pad[0x008 / 4];
  mR295Path.CapLive = Value;
  Print (L"R295 PORT_CAP readback=%08x port0=%u (OTG=3)\r\n",
         Value, Value & 3U);
  if (Value == 0xFFFFFFFFU || (Value & OtgCapMask) != 3U) goto Restore;

  mR295Path.Applied = TRUE;
  Print (L"R295 Linux-matched path: PASS; UTMI/BIAS/VREG calibration untouched.\r\n");
  return TRUE;

Restore:
  // Roll back even a partial setup before declining the xHCI window.
  Pad[0x008 / 4] = mR295Path.PortCap;
  Pad[0x004 / 4] = mR295Path.PadMux;
  if ((mR295Path.ClockY & TrackMask) == 0) Car[0x2A0 / 4] = TrackMask;
  gBS->Stall (10);
  mR295Path.TrackFinal = Car[0x298 / 4];
  mR295Path.MuxFinal = Pad[0x004 / 4];
  mR295Path.CapFinal = Pad[0x008 / 4];
  mR295Path.Restored = Pad[0x008 / 4] == mR295Path.PortCap &&
                       Pad[0x004 / 4] == mR295Path.PadMux &&
                       Car[0x298 / 4] == mR295Path.ClockY;
  Print (mR295Path.Restored ? L"R295 setup rollback: PASS\r\n" :
                              L"R295 setup rollback: FAIL; hold POWER 12-15s.\r\n");
  return FALSE;
}

STATIC BOOLEAN R295RestoreLinuxMatchedPath (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Pad = (volatile UINT32 *)XUSB_PAD_BASE;
  CONST UINT32 TrackMask = (1U << 18);

  if (!mR295Path.Valid || !mR295Path.Attempted) return TRUE;
  Pad[0x008 / 4] = mR295Path.PortCap;
  Pad[0x004 / 4] = mR295Path.PadMux;
  if ((mR295Path.ClockY & TrackMask) != 0) Car[0x29C / 4] = TrackMask;
  else Car[0x2A0 / 4] = TrackMask;
  gBS->Stall (10);
  mR295Path.TrackFinal = Car[0x298 / 4];
  mR295Path.MuxFinal = Pad[0x004 / 4];
  mR295Path.CapFinal = Pad[0x008 / 4];
  mR295Path.Restored = Pad[0x008 / 4] == mR295Path.PortCap &&
                       Pad[0x004 / 4] == mR295Path.PadMux &&
                       Car[0x298 / 4] == mR295Path.ClockY;
  Print (mR295Path.Restored ? L"R295 USB2 path exact restore: PASS\r\n" :
                              L"R295 USB2 path exact restore: FAIL; hold POWER 12-15s.\r\n");
  return mR295Path.Restored;
}

STATIC VOID LogText (IN CONST CHAR16 *Text) {
  UINTN Size;
  if (mLogFile == NULL) return;
  Size = StrSize (Text);
  mLogFile->Write (mLogFile, &Size, (VOID *)Text);
}

STATIC VOID CloseResultLog (VOID) {
  if (mLogFile != NULL) { mLogFile->Close (mLogFile); mLogFile = NULL; }
}

STATIC VOID CaptureR292CarFileSnapshot (BOOLEAN AfterFileRead) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  UINT32 ClockW = Car[0x364 / 4];
  UINT32 ResetW = Car[0x35C / 4];
  UINT32 PlluBase = Car[0x0C0 / 4];
  BOOLEAN Valid = (ClockW != 0xFFFFFFFFU && ResetW != 0xFFFFFFFFU &&
                   PlluBase != 0xFFFFFFFFU);

  if (AfterFileRead) {
    CaptureR293Car (R293_FILE_POST);
    mR292FilePostClockW = ClockW;
    mR292FilePostResetW = ResetW;
    mR292FilePostPlluBase = PlluBase;
    mR292FilePostValid = Valid;
  } else {
    CaptureR293Car (R293_FILE_PRE);
    mR292FilePreClockW = ClockW;
    mR292FilePreResetW = ResetW;
    mR292FilePrePlluBase = PlluBase;
    mR292FilePreValid = Valid;
  }
}

// R362: tegra21x_xusb_firmware embedded in the ROM as a FREEFORM file with a
// single RAW section (see NintendoSwitch.fdf), so no SD card is needed.
STATIC CONST EFI_GUID mR362XusbFirmwareFileGuid = {
  0x5a0f6a3e, 0x2d4c, 0x4c1b, { 0x9e, 0x51, 0x7b, 0x3f, 0x16, 0x8a, 0x0c, 0x42 }
};

STATIC BOOLEAN R362LoadEmbeddedFirmware (VOID) {
  VOID *Data = NULL;
  UINTN Size = 0;
  EFI_STATUS Status;
  EFI_PHYSICAL_ADDRESS Address = 0xFFFFFFFFULL;
  UINTN Pages;
  UINT8 *Firmware;
  UINT32 Crc32 = 0;
  UINT32 CodeTag, CodeSize, ImageLength;

  Status = GetSectionFromAnyFv (&mR362XusbFirmwareFileGuid, EFI_SECTION_RAW, 0, &Data, &Size);
  if (EFI_ERROR (Status) || Data == NULL) {
    Print (L"R362 embedded XUSB firmware: not found (%r); trying FAT volumes\r\n", Status);
    return FALSE;
  }
  CodeTag = (Size >= 256) ? ReadLe32 ((UINT8 *)Data + 8) : 0;
  CodeSize = (Size >= 256) ? ReadLe32 ((UINT8 *)Data + 12) : 0;
  ImageLength = (Size >= 256) ? ReadLe32 ((UINT8 *)Data + 100) : 0;
  Print (L"R362 embedded XUSB firmware: size=%u fwimg_len=%u boot_codetag=%08x boot_codesize=%u\r\n",
         (UINT32)Size, ImageLength, CodeTag, CodeSize);
  if (Size != 124416 || ImageLength != Size || CodeTag >= ImageLength || CodeSize == 0 ||
      CodeTag + CodeSize > ImageLength) {
    Print (L"R362 embedded XUSB firmware header validation: FAIL; trying FAT volumes\r\n");
    gBS->FreePool (Data);
    return FALSE;
  }
  Pages = EFI_SIZE_TO_PAGES (Size);
  // Same rule as the SD path: below 4 GiB, reserved while the Falcon runs.
  Status = gBS->AllocatePages (AllocateMaxAddress,
                               mR362Production ? EfiReservedMemoryType : EfiBootServicesData,
                               Pages, &Address);
  if (EFI_ERROR (Status)) {
    Print (L"R362 embedded XUSB firmware buffer allocation below 4GB: FAIL %r\r\n", Status);
    gBS->FreePool (Data);
    return FALSE;
  }
  Firmware = (UINT8 *)(UINTN)Address;
  CopyMem (Firmware, Data, Size);
  gBS->FreePool (Data);
  gBS->CalculateCrc32 (Firmware, Size, &Crc32);
  mFirmwareAddress = Address;
  mFirmwarePages = Pages;
  mFirmwareSize = Size;
  Print (L"R362 embedded XUSB firmware: PASS buffer=%016lx pages=%u CRC32=%08x first=%08x last=%08x\r\n",
         Address, (UINT32)Pages, Crc32, ReadLe32 (Firmware), ReadLe32 (&Firmware[Size - 4]));
  return TRUE;
}

STATIC BOOLEAN AuditXusbFirmwareFile (VOID) {
  EFI_HANDLE *Handles = NULL;
  UINTN HandleCount = 0, Index;
  EFI_STATUS Status;
  BOOLEAN Found = FALSE;

  // Capture immediately before filesystem discovery/open/read. These are
  // read-only CAR reads; they do not touch SDMMC, XUSB, or PLL controls.
  CaptureR292CarFileSnapshot (FALSE);

  if (R362LoadEmbeddedFirmware ()) {
    CaptureR292CarFileSnapshot (TRUE);
    return TRUE;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                    NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    Print (L"No readable FAT filesystem handles: %r\r\n", Status);
    return FALSE;
  }

  for (Index = 0; Index < HandleCount && !Found; Index++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
    EFI_FILE_PROTOCOL *Root = NULL, *File = NULL;
    UINT8 Header[256];
    UINTN ReadSize = sizeof (Header);
    UINT64 FileSize = 0;
    UINT32 CodeTag, CodeSize, Created, Version, ImageLength;
    // Let UEFI choose a free page range below 4 GiB. The fixed 2 GiB
    // address is not guaranteed to be available in the Switch memory map.
    EFI_PHYSICAL_ADDRESS Address = 0xFFFFFFFFULL;
    UINTN Pages = 0, FullReadSize = 0, CompareIndex;
    UINT8 *Firmware = NULL;
    UINT32 Crc32 = 0;
    BOOLEAN FullReadOk = FALSE;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid,
                                  (VOID **)&Fs);
    if (EFI_ERROR (Status)) continue;
    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) continue;
    Status = Root->Open (Root, &File, L"\\tegra21x_xusb_firmware",
                         EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR (Status)) { Root->Close (Root); continue; }

    Found = TRUE;
    Status = File->SetPosition (File, MAX_UINT64);
    if (!EFI_ERROR (Status)) Status = File->GetPosition (File, &FileSize);
    if (!EFI_ERROR (Status)) Status = File->SetPosition (File, 0);
    if (!EFI_ERROR (Status)) Status = File->Read (File, &ReadSize, Header);

    if (EFI_ERROR (Status) || ReadSize != sizeof (Header)) {
      Print (L"Firmware header read failed: %r bytes=%d\r\n", Status, (UINT32)ReadSize);
      File->Close (File);
      Root->Close (Root);
      break;
    }

    CodeTag = ReadLe32 (&Header[8]);
    CodeSize = ReadLe32 (&Header[12]);
    Created = ReadLe32 (&Header[44]);
    Version = ReadLe32 (&Header[72]);
    ImageLength = ReadLe32 (&Header[100]);
    Print (L"Found \\tegra21x_xusb_firmware on FAT volume %d\r\n", (UINT32)Index);
    Print (L"File size=%ld expected=124416 %s\r\n", FileSize,
           FileSize == 124416ULL ? L"MATCH" : L"DIFF");
    Print (L"Header: fwimg_len=%u boot_codetag=%08x boot_codesize=%u\r\n",
           ImageLength, CodeTag, CodeSize);
    Print (L"Header: created=%u version_id=%08x magic=%c%c%c%c%c%c%c%c\r\n",
           Created, Version,
           (CHAR16)Header[104], (CHAR16)Header[105], (CHAR16)Header[106], (CHAR16)Header[107],
           (CHAR16)Header[108], (CHAR16)Header[109], (CHAR16)Header[110], (CHAR16)Header[111]);
    if (FileSize == 124416ULL && ImageLength == FileSize &&
        CodeTag < ImageLength && CodeSize != 0 && CodeTag + CodeSize <= ImageLength) {
      Print (L"R77 firmware header validation: PASS\r\n");
    } else {
      Print (L"R77 firmware header validation: FAIL\r\n");
      File->Close (File);
      Root->Close (Root);
      break;
    }

    Pages = EFI_SIZE_TO_PAGES ((UINTN)FileSize);
    // R362: the Falcon keeps fetching firmware pages from this buffer
    // (ILOAD_BASE) while the controller runs, including after
    // ExitBootServices, so reserve it instead of using boot-services data.
    Status = gBS->AllocatePages (AllocateMaxAddress,
                                 mR362Production ? EfiReservedMemoryType : EfiBootServicesData,
                                 Pages, &Address);
    if (EFI_ERROR (Status)) {
      Print (L"DMA-buffer allocation below 4GB: FAIL %r\r\n", Status);
    } else {
      Firmware = (UINT8 *)(UINTN)Address;
      FullReadSize = (UINTN)FileSize;
      Status = File->SetPosition (File, 0);
      if (!EFI_ERROR (Status)) Status = File->Read (File, &FullReadSize, Firmware);
      FullReadOk = !EFI_ERROR (Status) && FullReadSize == (UINTN)FileSize;
      for (CompareIndex = 0; FullReadOk && CompareIndex < sizeof (Header); CompareIndex++) {
        if (Firmware[CompareIndex] != Header[CompareIndex]) FullReadOk = FALSE;
      }
      if (FullReadOk) Status = gBS->CalculateCrc32 (Firmware, FullReadSize, &Crc32);
      if (EFI_ERROR (Status)) FullReadOk = FALSE;
      Print (L"DMA buffer=%016lx pages=%u bytes=%u under-4G=%s\r\n",
             Address, (UINT32)Pages, (UINT32)FullReadSize,
             (Address + FileSize <= 0x100000000ULL) ? L"YES" : L"NO");
      if (FullReadOk) {
        Print (L"Full-file CRC32=%08x first=%08x last=%08x\r\n",
               Crc32, ReadLe32 (Firmware), ReadLe32 (&Firmware[FullReadSize - 4]));
        mFirmwareAddress = Address;
        mFirmwarePages = Pages;
        mFirmwareSize = FullReadSize;
        Print (L"R77 full firmware DMA-buffer audit: PASS; buffer held\r\n");
      } else {
        Print (L"R67 full firmware DMA-buffer audit: FAIL status=%r bytes=%u\r\n",
               Status, (UINT32)FullReadSize);
        gBS->FreePages (Address, Pages);
        Print (L"DMA buffer released after failure.\r\n");
      }
    }
    File->Close (File);
    Root->Close (Root);
  }

  gBS->FreePool (Handles);
  // Capture after file reads, closes, and filesystem-handle release.
  CaptureR292CarFileSnapshot (TRUE);
  if (!Found) Print (L"Firmware file was not found at FAT root.\r\n");
  return Found;
}

STATIC VOID RestorePllu480 (VOID) {
  if (mPllu480Changed) {
    ((volatile UINT32 *)CAR_BASE)[0x0C0 / 4] = mOldPlluBase;
    gBS->Stall (10);
    mPllu480Changed = FALSE;
  }
}

STATIC BOOLEAN SetPowerPartition (UINT32 Id, BOOLEAN On) {
  volatile UINT32 *P = (volatile UINT32 *)PMC_BASE;
  UINT32 Mask = (UINT32)(1U << Id);
  UINT32 Toggle;
  UINTN I;

  if (((P[0x038 / 4] & Mask) != 0) == On) return TRUE;
  Toggle = P[0x030 / 4];
  Toggle = (Toggle & ~0x1FU) | (Id & 0x1FU) | (1U << 8);
  P[0x030 / 4] = Toggle;
  for (I = 0; I < 10000; I++) {
    if ((P[0x030 / 4] & (1U << 8)) == 0) break;
    gBS->Stall (10);
  }
  if (I == 10000) return FALSE;
  for (I = 0; I < 10000; I++) {
    if (((P[0x038 / 4] & Mask) != 0) == On) return TRUE;
    gBS->Stall (10);
  }
  return FALSE;
}

STATIC BOOLEAN RemovePartitionClamp (UINT32 Id) {
  volatile UINT32 *P = (volatile UINT32 *)PMC_BASE;
  UINT32 Mask = (UINT32)(1U << Id);
  UINTN I;

  if ((P[0x02C / 4] & Mask) == 0) return TRUE;
  P[0x034 / 4] = Mask;
  for (I = 0; I < 10000; I++) {
    if ((P[0x02C / 4] & Mask) == 0) return TRUE;
    gBS->Stall (10);
  }
  return FALSE;
}

STATIC VOID DumpXusb (CONST CHAR16 *Phase) {
  volatile UINT32 *H = (volatile UINT32 *)XUSB_HCD_BASE;
  volatile UINT32 *F = (volatile UINT32 *)XUSB_FPCI_BASE;
  volatile UINT32 *I = (volatile UINT32 *)XUSB_IPFS_BASE;
  volatile UINT32 *P = (volatile UINT32 *)XUSB_PAD_BASE;

  Print (L"XUSB %s: HCD000=%08x FPCI000=%08x IPFS000=%08x PAD004=%08x\r\n",
         Phase, H[0x000 / 4], F[0x000 / 4], I[0x000 / 4], P[0x004 / 4]);
}

STATIC VOID DumpXusbPowerState (CONST CHAR16 *Phase) {
  volatile UINT32 *C = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *P = (volatile UINT32 *)PMC_BASE;
  UINT32 Pg = P[0x038 / 4];
  UINT32 Ru = C[0x00C / 4], Eu = C[0x018 / 4];
  UINT32 Rw = C[0x35C / 4], Ew = C[0x364 / 4];

  Print (L"PMC %s: PWRGATE=%08x XUSBA=%d XUSBB=%d XUSBC=%d CLAMP=%08x\r\n",
         Phase, Pg, (Pg >> 20) & 1, (Pg >> 21) & 1, (Pg >> 22) & 1,
         P[0x02C / 4]);
  Print (L"CAR %s U: RESET=%08x HOST=%d DEV=%d CLOCK=%08x HOST=%d DEV=%d\r\n",
         Phase, Ru, (Ru >> 25) & 1, (Ru >> 31) & 1,
         Eu, (Eu >> 25) & 1, (Eu >> 31) & 1);
  Print (L"CAR %s W: RESET=%08x PAD=%d CORE=%d SS=%d CLOCK=%08x PAD=%d CORE=%d SS=%d\r\n",
         Phase, Rw, (Rw >> 14) & 1, (Rw >> 15) & 1, (Rw >> 28) & 1,
         Ew, (Ew >> 14) & 1, (Ew >> 15) & 1, (Ew >> 28) & 1);
  Print (L"CAR %s SRC: HOST=%08x FALCON=%08x FS=%08x DEV=%08x SS=%08x\r\n",
         Phase, C[0x600 / 4], C[0x604 / 4], C[0x608 / 4],
         C[0x60C / 4], C[0x610 / 4]);
}

// These are read-only Tegra210 USB2 PadCTL observations.  Do not use this
// helper to program the pad: R127 deliberately performs no new writes.
STATIC VOID DumpXusbUsb2State (VOID) {
  volatile UINT32 *P = (volatile UINT32 *)XUSB_PAD_BASE;
  UINT32 Cap = P[0x008 / 4];
  UINT32 BiasCtl0 = P[0x284 / 4];
  UINT32 BiasCtl1 = P[0x288 / 4];

  Print (L"R127 USB2 mux=%08x CAP=%08x P0=%d P1=%d P2=%d\r\n",
         P[0x004 / 4], Cap, Cap & 3, (Cap >> 4) & 3, (Cap >> 8) & 3);
  Print (L"R127 USB2 pad0: C0=%08x C1=%08x\r\n", P[0x088 / 4], P[0x08C / 4]);
  Print (L"R127 USB2 pad1: C0=%08x C1=%08x\r\n", P[0x0C8 / 4], P[0x0CC / 4]);
  Print (L"R127 USB2 pad2: C0=%08x C1=%08x\r\n", P[0x108 / 4], P[0x10C / 4]);
  Print (L"R127 USB2 bias: C0=%08x pd=%d C1=%08x pdtrk=%d\r\n",
         BiasCtl0, (BiasCtl0 >> 11) & 1, BiasCtl1, (BiasCtl1 >> 26) & 1);
  Print (L"R127 USB2 VBUS_ID=%08x (read-only)\r\n", P[0xC60 / 4]);
}

// R128 is deliberately limited to the Switch USB-C USB2 companion (usb2-0).
// It does not enable VBUS, touch USB-C/charger I2C, issue an XUSB command, or
// start the Falcon.  Every changed PADCTL register is read first and restored
// exactly before returning to the normal R120 restore path.
STATIC BOOLEAN RunR128Usb2PadWakeTest (VOID) {
  volatile UINT32 *P = (volatile UINT32 *)XUSB_PAD_BASE;
  UINT32 OldCap = P[0x008 / 4];
  UINT32 OldPad0Ctl0 = P[0x088 / 4];
  UINT32 OldPad0Ctl1 = P[0x08C / 4];
  UINT32 OldBiasCtl0 = P[0x284 / 4];
  UINT32 OldBiasCtl1 = P[0x288 / 4];
  UINT32 Value;
  BOOLEAN RestoreOk;
  BOOLEAN WakeOk;

  Print (L"R128 USB2-0 reversible PadCTL wake test; no VBUS or XUSB command.\r\n");
  Print (L"R128 saved CAP=%08x P0C0=%08x P0C1=%08x BIAS0=%08x BIAS1=%08x\r\n",
         OldCap, OldPad0Ctl0, OldPad0Ctl1, OldBiasCtl0, OldBiasCtl1);

  // The upstream Tegra210 PHY sequence programs tracking timers, clears the
  // shared bias PD/PD_TRK bits, and clears the USB2 lane power-down bits. Keep
  // existing trim/calibration fields intact and use OTG mode, never host mode.
  Value = OldBiasCtl1 & ~((0x7FU << 12) | (0x7FU << 19));
  Value |= (0x1EU << 12) | (0x0AU << 19);
  P[0x288 / 4] = Value;
  P[0x284 / 4] = OldBiasCtl0 & ~(1U << 11);
  gBS->Stall (1);
  P[0x288 / 4] = P[0x288 / 4] & ~(1U << 26);
  gBS->Stall (50);

  P[0x088 / 4] = OldPad0Ctl0 & ~((1U << 29) | (1U << 27) | (1U << 26));
  P[0x08C / 4] = OldPad0Ctl1 & ~((1U << 2) | (1U << 1) | (1U << 0));
  P[0x008 / 4] = (OldCap & ~0x3U) | 0x3U;
  gBS->Stall (2000);

  Print (L"R128 live CAP=%08x P0C0=%08x P0C1=%08x BIAS0=%08x BIAS1=%08x\r\n",
         P[0x008 / 4], P[0x088 / 4], P[0x08C / 4],
         P[0x284 / 4], P[0x288 / 4]);
  Print (L"R128 live: USB2-0 OTG=%d pad_pd=%d/%d/%d bias_pd=%d trk_pd=%d\r\n",
         P[0x008 / 4] & 3, (P[0x088 / 4] >> 29) & 1,
         (P[0x088 / 4] >> 27) & 1, (P[0x088 / 4] >> 26) & 1,
         (P[0x284 / 4] >> 11) & 1, (P[0x288 / 4] >> 26) & 1);
  WakeOk = (P[0x008 / 4] & 3U) == 3U &&
           (P[0x088 / 4] & ((1U << 29) | (1U << 27) | (1U << 26))) == 0 &&
           (P[0x284 / 4] & (1U << 11)) == 0 &&
           (P[0x288 / 4] & (1U << 26)) == 0;
  Print (L"R128 hold 2 seconds; no attach/enumeration or VBUS operation...\r\n");
  gBS->Stall (2000000);

  P[0x008 / 4] = OldCap;
  P[0x08C / 4] = OldPad0Ctl1;
  P[0x088 / 4] = OldPad0Ctl0;
  P[0x284 / 4] = OldBiasCtl0;
  P[0x288 / 4] = OldBiasCtl1;
  gBS->Stall (100);
  RestoreOk = P[0x008 / 4] == OldCap && P[0x088 / 4] == OldPad0Ctl0 &&
              P[0x08C / 4] == OldPad0Ctl1 && P[0x284 / 4] == OldBiasCtl0 &&
              P[0x288 / 4] == OldBiasCtl1;
  Print (RestoreOk ? L"R128 PADCTL exact restore: PASS\r\n" :
                     L"R128 PADCTL exact restore: FAIL; hold POWER now.\r\n");
  Print (L"R128 %s: USB2-0 PADCTL wake only; VBUS, USB-C, I2C, XUSB command, CSB, DMA, and Falcon untouched.\r\n",
         RestoreOk && WakeOk ? L"PASS" : L"FAIL");
  return RestoreOk && WakeOk;
}

STATIC VOID RunR131Usb2TrackingMuxTest (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Padctl = (volatile UINT32 *)XUSB_PAD_BASE;
  CONST UINT32 TrackMask = (1U << 18); // TEGRA210_CLK_USB2_TRK, CAR Y bank
  UINT32 OldClockY = Car[0x298 / 4];
  UINT32 OldPadMux = Padctl[0x004 / 4];
  BOOLEAN ClockChanged = FALSE;
  BOOLEAN MuxChanged = FALSE;
  BOOLEAN WakeOk;
  BOOLEAN RestoreOk;

  Print (L"R131 USB2_TRK+mux test: saved CLK_ENB_Y=%08x bit18=%d MUX=%08x\r\n",
         OldClockY, (OldClockY >> 18) & 1, OldPadMux);
  if ((OldClockY & TrackMask) == 0) {
    Car[0x29C / 4] = TrackMask;
    gBS->Stall (10);
    ClockChanged = TRUE;
  }
  if ((Car[0x298 / 4] & TrackMask) == 0) {
    Print (L"R131 STOP: USB2_TRK clock did not enable; no PADCTL wake attempted.\r\n");
    goto RestoreClock;
  }
  Print (L"R131 USB2_TRK clock live: CLK_ENB_Y=%08x bit18=1\r\n", Car[0x298 / 4]);
  Padctl[0x004 / 4] = (OldPadMux & ~(3U << 18)) | (1U << 18);
  gBS->Stall (1);
  MuxChanged = TRUE;
  if (((Padctl[0x004 / 4] >> 18) & 3U) != 1U) {
    Print (L"R131 STOP: USB2 bias mux did not select XUSB; no PADCTL wake attempted.\r\n");
    goto RestoreClock;
  }
  Print (L"R131 USB2 bias mux live: MUX=%08x bias_sel=1(XUSB)\r\n", Padctl[0x004 / 4]);
  WakeOk = RunR128Usb2PadWakeTest ();
  Print (WakeOk ? L"R131 USB2 tracking wake: PASS\r\n" :
                  L"R131 USB2 tracking wake: FAIL; evidence retained.\r\n");

RestoreClock:
  if (MuxChanged) Padctl[0x004 / 4] = OldPadMux;
  if (ClockChanged) Car[0x2A0 / 4] = TrackMask;
  gBS->Stall (10);
  RestoreOk = Car[0x298 / 4] == OldClockY && Padctl[0x004 / 4] == OldPadMux;
  Print (RestoreOk ? L"R131 USB2_TRK+mux exact restore: PASS\r\n" :
                     L"R131 USB2_TRK+mux exact restore: FAIL; hold POWER now.\r\n");
}

// R132 adds only the USB2-0 on-chip VREG level used by the upstream Tegra210
// OTG path. It does not control VBUS or communicate with the USB-C/charger IC.
STATIC VOID RunR132Usb2VregLevelTest (VOID) {
  volatile UINT32 *Padctl = (volatile UINT32 *)XUSB_PAD_BASE;
  UINT32 OldVreg = Padctl[0x084 / 4];
  UINT32 LiveVreg;
  BOOLEAN RestoreOk;

  Print (L"R132 USB2-0 VREG-level test: saved BAT_CHRG_CTL1=%08x\r\n", OldVreg);
  Padctl[0x084 / 4] = (OldVreg & ~(3U << 7)) | (1U << 7);
  gBS->Stall (1);
  LiveVreg = Padctl[0x084 / 4];
  Print (L"R132 USB2-0 VREG-level live: BAT_CHRG_CTL1=%08x level=%d\r\n",
         LiveVreg, (LiveVreg >> 7) & 3);
  if (((LiveVreg >> 7) & 3U) != 1U) {
    Print (L"R132 STOP: USB2-0 VREG level did not select 1; no PadCTL wake attempted.\r\n");
  } else {
    RunR131Usb2TrackingMuxTest ();
  }

  Padctl[0x084 / 4] = OldVreg;
  gBS->Stall (10);
  RestoreOk = Padctl[0x084 / 4] == OldVreg;
  Print (RestoreOk ? L"R132 USB2-0 VREG exact restore: PASS\r\n" :
                     L"R132 USB2-0 VREG exact restore: FAIL; hold POWER now.\r\n");
}

// R133 temporarily enables only the CAR W-bank PadCTL clock. The associated
// PadCTL reset is already deasserted; no reset, controller, or VBUS operation
// is performed here.
STATIC VOID RunR133PadctlClockTest (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  CONST UINT32 PadctlClockMask = (1U << 14);
  UINT32 OldClockW = Car[0x364 / 4];
  BOOLEAN ClockChanged = FALSE;
  BOOLEAN RestoreOk;

  Print (L"R133 PadCTL clock test: saved CLK_ENB_W=%08x bit14=%d\r\n",
         OldClockW, (OldClockW >> 14) & 1);
  if ((OldClockW & PadctlClockMask) == 0) {
    Car[0x364 / 4] = OldClockW | PadctlClockMask;
    gBS->Stall (10);
    ClockChanged = TRUE;
  }
  if ((Car[0x364 / 4] & PadctlClockMask) == 0) {
    Print (L"R133 STOP: PadCTL clock did not enable; no USB2 wake attempted.\r\n");
    goto RestoreClock;
  }
  Print (L"R133 PadCTL clock live: CLK_ENB_W=%08x bit14=1\r\n", Car[0x364 / 4]);
  RunR132Usb2VregLevelTest ();

RestoreClock:
  if (ClockChanged) Car[0x364 / 4] = OldClockW;
  gBS->Stall (10);
  RestoreOk = Car[0x364 / 4] == OldClockW;
  Print (RestoreOk ? L"R133 PadCTL clock exact restore: PASS\r\n" :
                     L"R133 PadCTL clock exact restore: FAIL; hold POWER now.\r\n");
}

// R129 is the SD-independent form of the USB2-0 PadCTL test.  Unlike the
// older diagnostic path it neither opens a filesystem nor allocates/reads the
// XUSB firmware buffer.  The prerequisite ELPG and UPHY-reset changes are
// also restored exactly here.
STATIC VOID RunR129NoSdUsb2Test (VOID) {
  volatile UINT32 *Padctl = (volatile UINT32 *)XUSB_PAD_BASE;
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  UINT32 OldElpg = Padctl[0x024 / 4];
  UINT32 OldUpHyReset = Car[0x2A4 / 4];
  BOOLEAN RestoreOk;

  Print (L"R129 SD-independent USB2 test: no filesystem or firmware buffer.\r\n");
  Print (L"R129 saved ELPG=%08x PEX_USB_UPHY_RESET=%08x\r\n", OldElpg, OldUpHyReset);
  Padctl[0x024 / 4] = OldElpg & ~(1U << 29);
  gBS->Stall (100);
  Padctl[0x024 / 4] &= ~(1U << 30);
  gBS->Stall (100);
  Padctl[0x024 / 4] &= ~(1U << 31);
  gBS->Stall (100);
  Car[0x2A4 / 4] = OldUpHyReset & ~(1U << 13);
  gBS->Stall (100);
  Print (L"R129 prerequisite live ELPG=%08x UPHY_RESET=%08x\r\n",
         Padctl[0x024 / 4], Car[0x2A4 / 4]);
  DumpXusbUsb2State ();
  RunR133PadctlClockTest ();

  Car[0x2A4 / 4] = OldUpHyReset;
  Padctl[0x024 / 4] = OldElpg;
  gBS->Stall (100);
  RestoreOk = Car[0x2A4 / 4] == OldUpHyReset && Padctl[0x024 / 4] == OldElpg;
  Print (RestoreOk ? L"R129 prerequisite exact restore: PASS\r\n" :
                     L"R129 prerequisite exact restore: FAIL; hold POWER now.\r\n");
  Print (L"R129 %s: no SD, filesystem, firmware buffer, VBUS, USB-C, I2C, XUSB command, CSB, DMA, or Falcon operation.\r\n",
         RestoreOk ? L"PASS" : L"FAIL");
}

STATIC BOOLEAN LoadConfig (VOID) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINTN I;
  B[CONFIG_LOAD] = 0x25;
  for (I = 0; I < 10000; I++) {
    if ((B[CONFIG_LOAD] & 1) == 0) return TRUE;
    gBS->Stall (10);
  }
  return FALSE;
}

STATIC BOOLEAN EnterPacketMode (VOID) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  B[CNFG] = mOldCnfg | 0x400;
  return LoadConfig ();
}

STATIC BOOLEAN WaitTransfer (UINT32 *FinalStatus) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINTN I;
  UINT32 S = 0, L2Result = 0;
  for (I = 0; I < 10000; I++) {
    S = B[INT_STATUS_REG];
    if ((S & (ERR_MASK | ALL_COMPLETE)) != 0) break;
    gBS->Stall (10);
  }
  *FinalStatus = S;
  return (S & ERR_MASK) == 0 && (S & ALL_COMPLETE) != 0;
}

STATIC BOOLEAN BeginTransfer (VOID) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINTN I;
  B[INT_MASK_REG] = 0;
  B[INT_STATUS_REG] = 0xFFFFFFFF;
  B[FIFO_CONTROL] = B[FIFO_CONTROL] | 3;
  for (I = 0; I < 10000; I++) {
    if ((B[FIFO_CONTROL] & 3) == 0) return TRUE;
    gBS->Stall (10);
  }
  return FALSE;
}

STATIC BOOLEAN ReadReg16 (UINT8 Reg, UINT16 *Value, UINT32 *Status) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINT32 Word;
  if (!BeginTransfer ()) return FALSE;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 0;
  B[TX_FIFO] = REPEAT_START | (BM_ADDR << 1); B[TX_FIFO] = Reg;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 1;
  B[TX_FIFO] = READ_FLAG | (BM_ADDR << 1);
  if (!WaitTransfer (Status) || (B[FIFO_STATUS] & 0xF) == 0) return FALSE;
  Word = B[RX_FIFO];
  *Value = (UINT16)(Word & 0xFFFF);
  B[INT_STATUS_REG] = *Status;
  return TRUE;
}

STATIC BOOLEAN WriteReg16 (UINT8 Reg, UINT16 Value, UINT32 *Status) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINT32 Payload = Reg | ((UINT32)(Value & 0xFF) << 8) |
                   ((UINT32)(Value >> 8) << 16);
  if (!BeginTransfer ()) return FALSE;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 2;
  B[TX_FIFO] = (BM_ADDR << 1); B[TX_FIFO] = Payload;
  if (!WaitTransfer (Status)) return FALSE;
  B[INT_STATUS_REG] = *Status;
  return TRUE;
}

STATIC BOOLEAN ReadReg8At (UINT8 Address, UINT8 Reg, UINT8 *Value, UINT32 *Status) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINT32 Word;
  if (!BeginTransfer ()) return FALSE;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 0;
  B[TX_FIFO] = REPEAT_START | (Address << 1); B[TX_FIFO] = Reg;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 0;
  B[TX_FIFO] = READ_FLAG | (Address << 1);
  if (!WaitTransfer (Status) || (B[FIFO_STATUS] & 0xF) == 0) return FALSE;
  Word = B[RX_FIFO];
  *Value = (UINT8)(Word & 0xFF);
  B[INT_STATUS_REG] = *Status;
  return TRUE;
}

STATIC BOOLEAN WriteReg8At (UINT8 Address, UINT8 Reg, UINT8 Value, UINT32 *Status) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINT32 Payload = Reg | ((UINT32)Value << 8);
  if (!BeginTransfer ()) return FALSE;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 1;
  B[TX_FIFO] = (Address << 1); B[TX_FIFO] = Payload;
  if (!WaitTransfer (Status)) return FALSE;
  B[INT_STATUS_REG] = *Status;
  return TRUE;
}

STATIC VOID RestoreController (VOID) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  B[INT_MASK_REG] = mOldIntMask;
  B[FIFO_CONTROL] = mOldFifo;
  B[CNFG] = mOldCnfg;
  LoadConfig ();
}

// R135 is a single-boot, read-only inventory of the internal Raydium touch
// path used by Switchroot Linux: SPI4, the TOUCH clock/reset/interrupt pins,
// and GPIO state.  R134 established that a direct SPI4 register read stalls
// while SPI4 is clock-gated and held in reset, so R135 deliberately does not
// access the SPI4 register aperture.  It never clocks SPI4, changes GPIO
// direction or level, accesses the PMIC over I2C, or transfers data over SPI.
STATIC UINT32 ReadGpioPortRegister (UINT32 Port, UINT32 RegisterOffset) {
  UINT32 Bank = Port / 4;
  UINT32 PortInBank = Port % 4;
  volatile UINT32 *Gpio = (volatile UINT32 *)GPIO_BASE;
  return Gpio[((Bank * 0x80) + RegisterOffset + (PortInBank * 4)) / 4];
}

STATIC VOID RunR135TouchReadOnlyAudit (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Pinmux = (volatile UINT32 *)PINMUX_BASE;
  UINT32 Pv6Config, Pv6Dir, Pv6Out, Pv6In;
  UINT32 Px1Config, Px1Dir, Px1In;
  UINT32 IntInitial, IntLast, IntChanges = 0;
  UINTN I;

  Print (L"R135: Switch internal-touch read-only audit (one boot).\r\n");
  Print (L"R135: no SPI aperture access/transfer, PMIC/I2C access, GPIO write, clock/reset change, or USB action.\r\n");
  Print (L"R135 SPI4: CLK_U=%08x clk_bit4=%d RST_U=%08x rst_bit4=%d SRC=%08x\r\n",
         Car[0x018 / 4], (Car[0x018 / 4] >> 4) & 1,
         Car[0x00C / 4], (Car[0x00C / 4] >> 4) & 1,
         Car[0x1B4 / 4]);
  Print (L"R135 SPI4 pins: SCK=%08x CS0=%08x MOSI=%08x MISO=%08x\r\n",
         Pinmux[PMUX_PINGRP_SPI4_SCK_PC5], Pinmux[PMUX_PINGRP_SPI4_CS0_PC6],
         Pinmux[PMUX_PINGRP_SPI4_MOSI_PC7], Pinmux[PMUX_PINGRP_SPI4_MISO_PD0]);
  Print (L"R135 touch pins: CLK=%08x RST=%08x INT=%08x\r\n",
         Pinmux[PMUX_PINGRP_TOUCH_CLK_PV7], Pinmux[PMUX_PINGRP_TOUCH_RST_PV6],
         Pinmux[PMUX_PINGRP_TOUCH_INT_PX1]);

  // GPIO ports V (21) and X (23); each value is only sampled, never changed.
  Pv6Config = ReadGpioPortRegister (21, 0x00);
  Pv6Dir = ReadGpioPortRegister (21, 0x10);
  Pv6Out = ReadGpioPortRegister (21, 0x20);
  Pv6In = ReadGpioPortRegister (21, 0x30);
  Px1Config = ReadGpioPortRegister (23, 0x00);
  Px1Dir = ReadGpioPortRegister (23, 0x10);
  Px1In = ReadGpioPortRegister (23, 0x30);
  Print (L"R135 GPIO: PV6 cfg=%d dir=%d out=%d in=%d | PX1 cfg=%d dir=%d in=%d\r\n",
         (Pv6Config >> 6) & 1, (Pv6Dir >> 6) & 1, (Pv6Out >> 6) & 1, (Pv6In >> 6) & 1,
         (Px1Config >> 1) & 1, (Px1Dir >> 1) & 1, (Px1In >> 1) & 1);

  Print (L"R135 tap the screen repeatedly now; sampling TOUCH_INT for 1 second...\r\n");
  IntInitial = (Px1In >> 1) & 1;
  IntLast = IntInitial;
  for (I = 0; I < 100; I++) {
    UINT32 IntNow = (ReadGpioPortRegister (23, 0x30) >> 1) & 1;
    if (IntNow != IntLast) { IntChanges++; IntLast = IntNow; }
    gBS->Stall (10000);
  }
  Print (L"R135 TOUCH_INT: initial=%d final=%d transitions=%u\r\n",
         IntInitial, IntLast, (UINT32)IntChanges);
  Print (L"R135 PASS: internal touch path inventory complete; no device command was issued.\r\n");
}

// R136 changes only the GPIO-selection bits for the two Linux touch GPIOs.
// PV6 remains an input and its output latch is left untouched, so this does
// not assert/deassert reset.  PX1 likewise remains an input.  Both complete
// port configuration registers are restored before returning.
STATIC VOID RunR136TouchGpioSelectTest (VOID) {
  UINT32 Pv6Old, Px1Old, Pv6Live, Px1Live;
  BOOLEAN SelectOk, RestoreOk;

  Print (L"R136: reversible touch GPIO-select test only. No SPI4, PMIC/I2C, clock, reset, or USB action.\r\n");
  Pv6Old = ReadGpioPortRegister (21, 0x00);
  Px1Old = ReadGpioPortRegister (23, 0x00);
  Print (L"R136 saved GPIO_CFG: PV6-port=%08x PX1-port=%08x\r\n", Pv6Old, Px1Old);

  // GPIO config is the only mutable state in R136.  Do not alter DIR or OUT.
  ((volatile UINT32 *)GPIO_BASE)[((5 * 0x80) + 0x00 + (1 * 4)) / 4] = Pv6Old | (1U << 6);
  ((volatile UINT32 *)GPIO_BASE)[((5 * 0x80) + 0x00 + (3 * 4)) / 4] = Px1Old | (1U << 1);
  Pv6Live = ReadGpioPortRegister (21, 0x00);
  Px1Live = ReadGpioPortRegister (23, 0x00);
  SelectOk = ((Pv6Live & (1U << 6)) != 0) && ((Px1Live & (1U << 1)) != 0);
  Print (L"R136 live GPIO_CFG: PV6 cfg=%d PX1 cfg=%d %s\r\n",
         (Pv6Live >> 6) & 1, (Px1Live >> 1) & 1, SelectOk ? L"PASS" : L"FAIL");
  Print (L"R136 live DIR/OUT unchanged: PV6 dir=%d out=%d | PX1 dir=%d\r\n",
         (ReadGpioPortRegister (21, 0x10) >> 6) & 1,
         (ReadGpioPortRegister (21, 0x20) >> 6) & 1,
         (ReadGpioPortRegister (23, 0x10) >> 1) & 1);

  ((volatile UINT32 *)GPIO_BASE)[((5 * 0x80) + 0x00 + (1 * 4)) / 4] = Pv6Old;
  ((volatile UINT32 *)GPIO_BASE)[((5 * 0x80) + 0x00 + (3 * 4)) / 4] = Px1Old;
  RestoreOk = ReadGpioPortRegister (21, 0x00) == Pv6Old &&
              ReadGpioPortRegister (23, 0x00) == Px1Old;
  Print (L"R136 GPIO_CFG exact restore: %s\r\n", RestoreOk ? L"PASS" : L"FAIL; hold POWER now");
  Print (L"R136 %s: GPIO-selection proof complete; touch device remained uncommanded.\r\n",
         (SelectOk && RestoreOk) ? L"PASS" : L"FAIL");
}

// R137 is based on the live Switchroot Kubuntu snapshot.  The internal panel
// is an STM FTM4 at I2C2 (0x7000c500), address 0x49 -- not an SPI4 device.
// This stage is intentionally a register/pinmux inventory only: it does not
// enable I2C2, release reset, send an I2C address, modify GPIO, or access the
// FTM4 controller.  A later stage may use this evidence for a reversible read.
STATIC VOID RunR137Ftm4I2cPreflight (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Pinmux = (volatile UINT32 *)PINMUX_BASE;
  volatile UINT32 *I2c2 = (volatile UINT32 *)I2C2_BASE;
  UINT32 ClkH = Car[0x014 / 4];
  UINT32 RstH = Car[0x008 / 4];

  Print (L"R137: Kubuntu-proven FTM4 touch I2C2 preflight (read-only).\r\n");
  Print (L"R137: target=stm,ftm4_fts i2c=7000c500 addr=49 irq=185 coords=1280x720.\r\n");
  Print (L"R137: no I2C transfer/address phase, clock/reset/GPIO write, PMIC, SPI, USB, or SD action.\r\n");
  Print (L"R137 I2C2: CLK_H=%08x bit22=%d RST_H=%08x bit22=%d SRC=%08x\r\n",
         ClkH, (ClkH >> 22) & 1, RstH, (RstH >> 22) & 1, Car[0x198 / 4]);
  Print (L"R137 I2C2 pins: SCL_PJ2=%08x SDA_PJ3=%08x\r\n",
         Pinmux[PMUX_PINGRP_GEN2_I2C_SCL_PJ2], Pinmux[PMUX_PINGRP_GEN2_I2C_SDA_PJ3]);
  Print (L"R137 I2C2 regs: CNFG=%08x FIFO_STATUS=%08x INT_MASK=%08x INT_STATUS=%08x\r\n",
         I2c2[CNFG], I2c2[FIFO_STATUS], I2c2[INT_MASK_REG], I2c2[INT_STATUS_REG]);
  Print (L"R137 PASS: FTM4/I2C2 baseline captured; controller remained uncommanded.\r\n");
}

// R236 resumes the R232 XUSB bring-up at its unresolved MMIO boundary.  The
// values below are a read-only parity reference captured from the working
// Switchroot Kubuntu tegra-xusb driver.  Do not write CAR, PMC, FPCI, IPFS,
// HCD, PadCtl, CSB, or DMA state in this stage.
STATIC VOID RunR236XusbWindowAudit (VOID) {
  volatile UINT32 *Hcd = (volatile UINT32 *)XUSB_HCD_BASE;
  volatile UINT32 *Fpci = (volatile UINT32 *)XUSB_FPCI_BASE;
  volatile UINT32 *Ipfs = (volatile UINT32 *)XUSB_IPFS_BASE;
  UINT32 FpciId;
  UINT32 HcdCap;
  UINT32 IpfsId;

  Print (L"R236 step 1: CAR/PMC read-only power/clock snapshot\r\n");
  DumpXusbPowerState (L"R236 initial");

  Print (L"R236 step 2: FPCI ID read at 70098000\r\n");
  FpciId = Fpci[0];
  Print (L"R236 FPCI_ID=%08x expected=0FAC10DE %s\r\n",
         FpciId, FpciId == 0x0FAC10DEU ? L"MATCH" : L"DIFF");

  Print (L"R236 step 3: xHCI capability read at 70090000\r\n");
  HcdCap = Hcd[0];
  Print (L"R236 HCD_CAP=%08x expected=01000020 %s\r\n",
         HcdCap, HcdCap == 0x01000020U ? L"MATCH" : L"DIFF");

  Print (L"R236 step 4: IPFS ID read at 70099000\r\n");
  IpfsId = Ipfs[0];
  Print (L"R236 IPFS_ID=%08x expected=00000008 %s\r\n",
         IpfsId, IpfsId == 0x00000008U ? L"MATCH" : L"DIFF");

  if (FpciId == 0x0FAC10DEU && HcdCap == 0x01000020U && IpfsId == 0x00000008U) {
    Print (L"R236 PASS: all three Linux-known MMIO windows respond.\r\n");
    Print (L"Next boundary: CSB-window access remains disabled pending review.\r\n");
  } else {
    Print (L"R236 STOP: MMIO parity failed; no CSB access or controller write.\r\n");
  }
  Print (L"R236 safety: read-only; no power, clock, reset, BAR, CSB, Falcon, or USB write.\r\n");
}

// R237 isolates the Linux clock-before-power ordering at the host-only MMIO
// boundary. It enables the known Kubuntu HOST clock source, enables only the
// HOST clock, powers only XUSBC, releases only HOST reset, samples three
// identification/capability registers, then restores the cold baseline.
// No SS/device domain, PHY, Falcon, firmware, CSB, DMA, BAR, or USB transfer.
STATIC VOID RunR237XusbHostWindowAudit (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Pmc = (volatile UINT32 *)PMC_BASE;
  volatile UINT32 *Hcd = (volatile UINT32 *)XUSB_HCD_BASE;
  volatile UINT32 *Fpci = (volatile UINT32 *)XUSB_FPCI_BASE;
  volatile UINT32 *Ipfs = (volatile UINT32 *)XUSB_IPFS_BASE;
  CONST UINT32 HostBit = 1U << 25;
  CONST UINT32 DomainMask = (1U << 20) | (1U << 21) | (1U << 22);
  CONST UINT32 HostSourceLinux = 0x20000006U;
  UINT32 OldPllu = Car[0x0C0 / 4];
  UINT32 OldHostSource = Car[0x600 / 4];
  UINT32 OldClockU = Car[0x018 / 4];
  UINT32 OldResetU = Car[0x00C / 4];
  UINT32 OldPower = Pmc[0x038 / 4];
  UINT32 OldClamp = Pmc[0x02C / 4];
  UINT32 FpciId = 0xFFFFFFFFU, HcdCap = 0xFFFFFFFFU, IpfsId = 0xFFFFFFFFU;
  BOOLEAN SourceChanged = FALSE, ReadPass = FALSE, RestorePass;

  Print (L"R237: Linux-order HOST-only MMIO boundary test; no USB transaction.\r\n");
  Print (L"R237 baseline PLLU=%08x HOST_SRC=%08x CLK_U=%08x RST_U=%08x\r\n",
         OldPllu, OldHostSource, OldClockU, OldResetU);
  Print (L"R237 baseline PMC_PWRGATE=%08x CLAMP=%08x\r\n", OldPower, OldClamp);
  if ((OldPllu & ((1U << 30) | (1U << 27))) != ((1U << 30) | (1U << 27))) {
    Print (L"R237 STOP: PLLU is not enabled+locked; no XUSB state changed.\r\n");
    return;
  }
  if ((OldPower & DomainMask) != 0 ||
      (OldClockU & HostBit) != 0 || (OldResetU & HostBit) == 0) {
    Print (L"R237 STOP: XUSB is not in the expected cold baseline; no state changed.\r\n");
    return;
  }
  Print (L"R237 step 1: set Linux HOST_SRC while HOST clock is gated... ");
  Car[0x600 / 4] = HostSourceLinux;
  gBS->Stall (10);
  if (Car[0x600 / 4] != HostSourceLinux) {
    Print (L"FAIL; restoring source only.\r\n");
    Car[0x600 / 4] = OldHostSource;
    Print (Car[0x600 / 4] == OldHostSource ? L"R237 restore: PASS\r\n" : L"R237 restore: FAIL; hold POWER.\r\n");
    return;
  }
  SourceChanged = TRUE;
  Print (L"PASS (%08x)\r\n", Car[0x600 / 4]);
  Print (L"R237 step 2: enable HOST clock before XUSBC power... ");
  Car[0x330 / 4] = HostBit;
  gBS->Stall (10);
  if ((Car[0x018 / 4] & HostBit) == 0) {
    Print (L"FAIL\r\n");
    goto Restore;
  }
  Print (L"PASS CLK_ENB_U=%08x\r\n", Car[0x018 / 4]);
  Print (L"R237 step 3: power XUSBC host domain only... ");
  if (!SetPowerPartition (22, TRUE)) {
    Print (L"FAIL/TIMEOUT\r\n");
    goto Restore;
  }
  Print (L"PASS PMC_PWRGATE=%08x\r\n", Pmc[0x038 / 4]);
  Print (L"R237 step 4: remove XUSBC clamp... ");
  if (!RemovePartitionClamp (22)) {
    Print (L"FAIL/TIMEOUT\r\n");
    goto Restore;
  }
  Print (L"PASS PMC_CLAMP=%08x\r\n", Pmc[0x02C / 4]);
  Print (L"R237 step 5: release HOST reset only... ");
  Car[0x314 / 4] = HostBit;
  gBS->Stall (10);
  if ((Car[0x00C / 4] & HostBit) != 0) {
    Print (L"FAIL\r\n");
    goto Restore;
  }
  Print (L"PASS RST_DEV_U=%08x\r\n", Car[0x00C / 4]);
  Print (L"R237 step 6: FPCI/HCD/IPFS ID reads (read-only)...\r\n");
  FpciId = Fpci[0];
  Print (L"R237 FPCI_ID=%08x expected=0FAC10DE %s\r\n",
         FpciId, FpciId == 0x0FAC10DEU ? L"MATCH" : L"DIFF");
  HcdCap = Hcd[0];
  Print (L"R237 HCD_CAP=%08x expected=01000020 %s\r\n",
         HcdCap, HcdCap == 0x01000020U ? L"MATCH" : L"DIFF");
  IpfsId = Ipfs[0];
  Print (L"R237 IPFS_ID=%08x expected=00000008 %s\r\n",
         IpfsId, IpfsId == 0x00000008U ? L"MATCH" : L"DIFF");
  ReadPass = FpciId == 0x0FAC10DEU && HcdCap == 0x01000020U && IpfsId == 0x00000008U;
Restore:
  Print (L"R237 restore: assert HOST reset... ");
  if ((Car[0x00C / 4] & HostBit) == 0) Car[0x310 / 4] = HostBit;
  gBS->Stall (10);
  Print ((Car[0x00C / 4] & HostBit) != 0 ? L"PASS\r\n" : L"FAIL\r\n");
  Print (L"R237 restore: disable HOST clock... ");
  if ((Car[0x018 / 4] & HostBit) != 0) Car[0x334 / 4] = HostBit;
  gBS->Stall (10);
  Print ((Car[0x018 / 4] & HostBit) == 0 ? L"PASS\r\n" : L"FAIL\r\n");
  Print (L"R237 restore: power XUSBC OFF... ");
  if (((Pmc[0x038 / 4] & (1U << 22)) != 0) && !SetPowerPartition (22, FALSE)) Print (L"FAIL/TIMEOUT; hold POWER now.\r\n");
  else Print (L"PASS\r\n");
  Print (L"R237 restore: HOST_SRC exact baseline... ");
  if (SourceChanged) Car[0x600 / 4] = OldHostSource;
  gBS->Stall (10);
  Print (Car[0x600 / 4] == OldHostSource ? L"PASS\r\n" : L"FAIL; hold POWER now.\r\n");
  Print (L"R237 final PMC_PWRGATE=%08x CLAMP=%08x CLK_U=%08x RST_U=%08x\r\n",
         Pmc[0x038 / 4], Pmc[0x02C / 4], Car[0x018 / 4], Car[0x00C / 4]);
  RestorePass = ((Pmc[0x038 / 4] & (1U << 22)) == (OldPower & (1U << 22))) &&
                ((Pmc[0x02C / 4] & (1U << 22)) == (OldClamp & (1U << 22))) &&
                ((Car[0x018 / 4] & HostBit) == (OldClockU & HostBit)) &&
                ((Car[0x00C / 4] & HostBit) == (OldResetU & HostBit)) &&
                Car[0x600 / 4] == OldHostSource;
  Print (RestorePass ? L"R237 restore baseline: PASS.\r\n" :
                       L"R237 restore baseline: FAIL; hold POWER now.\r\n");
  Print (ReadPass ? L"R237 MMIO boundary: PASS; inspect restore values above.\r\n" :
                    L"R237 MMIO boundary: STOP; no CSB/Falcon/USB action.\r\n");
  Print (L"R237 safety: HOST-only; XUSBA/B, SS/DEV, PadCTL, PHY, CSB, DMA, firmware and USB transfer untouched.\r\n");
}

// R243 follows the validated Linux host setup through IPFS, BAR0 and MEM_SPACE,
// reads xHCI capability registers, then preflights the Linux CSB window using
// read-only XUSB registers. It does not load firmware or start the Falcon.
// A stalled MMIO read cannot be timed out by firmware.
STATIC BOOLEAN RunR246FirmwareTransfer (volatile UINT32 *Fpci, UINT8 *Firmware,
                                        UINTN FirmwareSize,
                                        EFI_PHYSICAL_ADDRESS FirmwareAddress,
                                        BOOLEAN StartFalcon) {
  UINT32 CodeTag = ReadLe32 (&Firmware[8]);
  UINT32 CodeSize = ReadLe32 (&Firmware[12]);
  UINT32 TagBlocks = (CodeTag + 255U) / 256U;
  UINT32 SizeBlocks = (CodeSize + 255U) / 256U;
  UINT32 CodeBlocks = TagBlocks + SizeBlocks;
  UINT32 LoadBaseLo = (UINT32)(FirmwareAddress + 256U);
  UINT32 ImfillRange = (TagBlocks & 0xFFFFU) | ((CodeBlocks & 0xFFFFU) << 16);
  UINT32 L2Size = ((TagBlocks & 0x3FFU) << 8) | ((SizeBlocks & 0xFFU) << 24);
  UINT32 Result = 0;
  UINT32 Poll;
  UINT32 SavedRange = Fpci[0x41C / 4];
  BOOLEAN Ok = FALSE;

  if (FirmwareSize != 124416U || FirmwareAddress + FirmwareSize > 0x100000000ULL ||
      ReadLe32 (&Firmware[100]) != FirmwareSize || CodeTag == 0 || CodeSize == 0 ||
      (UINT64)CodeTag + CodeSize > FirmwareSize || (LoadBaseLo & 0xFFU) != 0) {
    Print (L"R246 STOP: transfer inputs failed validation; no firmware write.\r\n");
    return FALSE;
  }

  Print (StartFalcon ? L"R249 firmware transfer stage: Linux xhci-tegra order.\r\n" :
                       L"R246 transfer-only: Linux xhci-tegra order; stop before BOOTVEC/CPUCTL.\r\n");
  Print (L"R246 transfer inputs: ILOAD_ATTR=%u ILOAD_BASE=%08x:%08x tag=%08x size=%u\r\n",
         (UINT32)FirmwareSize, (UINT32)((FirmwareAddress + 256U) >> 32), LoadBaseLo,
         CodeTag, CodeSize);

  // Match dma_alloc_coherent(): make the firmware bytes visible to XUSB DMA.
  WriteBackDataCacheRange (Firmware, FirmwareSize);
  ArmDataSynchronizationBarrier ();
  Print (L"R246 host firmware cache clean + DSB: PASS\r\n");

  // Linux tegra_xusb_load_firmware(): provide system-memory image to Falcon boot ROM.
  Fpci[0x41C / 4] = 0x0000080DU;
  Fpci[(0x800 + 0x000) / 4] = (UINT32)FirmwareSize;        // ILOAD_ATTR
  Fpci[(0x800 + 0x008) / 4] = (UINT32)((FirmwareAddress + 256U) >> 32); // ILOAD_BASE_HI
  Fpci[0x41C / 4] = 0x0000080DU;
  Fpci[(0x800 + 0x004) / 4] = LoadBaseLo;                 // ILOAD_BASE_LO
  Fpci[0x41C / 4] = 0x0000080CU;
  Fpci[(0x800 + 0x01C) / 4] = 0x80000000U;                // APMAP_BOOTPATH
  Print (StartFalcon ? L"R249 steps 1-3: ILOAD_ATTR/BASE and APMAP_BOOTPATH programmed.\r\n" :
                       L"R246 steps 1-3: ILOAD_ATTR/BASE and APMAP_BOOTPATH programmed.\r\n");

  Fpci[0x41C / 4] = 0x0000080DU;
  Fpci[(0x800 + 0x014) / 4] = 0x40000000U;               // L2IMEM invalidate all
  Fpci[(0x800 + 0x010) / 4] = L2Size;                    // L2IMEMOP_SIZE
  Fpci[(0x800 + 0x014) / 4] = 0x11000000U;               // load locked result
  Fpci[0x41C / 4] = 0x00000000U;
  Fpci[(0x800 + 0x158) / 4] = SizeBlocks;                // IMFILLCTL
  Fpci[(0x800 + 0x154) / 4] = ImfillRange;               // IMFILLRNG1
  Fpci[(0x800 + 0x10C) / 4] = 0;                         // XUSB_FALC_DMACTL
  Print (StartFalcon ? L"R249 steps 4-8: L2IMEM load requested; polling result (max 10s)...\r\n" :
                       L"R246 steps 4-8: L2IMEM load requested; polling result (max 10s)...\r\n");

  for (Poll = 0; Poll < 100; ++Poll) {
    Fpci[0x41C / 4] = 0x0000080DU;
    Result = Fpci[(0x800 + 0x018) / 4];                  // L2IMEMOP_RESULT
    if (Result != 0xFFFFFFFFU && (Result & 0x80000000U) != 0) {
      Ok = TRUE;
      break;
    }
    gBS->Stall (100000);
  }
  Fpci[0x41C / 4] = SavedRange;
  Print (StartFalcon ? L"R249 L2IMEMOP_RESULT=%08x polls=%u %s\r\n" :
                       L"R246 L2IMEMOP_RESULT=%08x polls=%u %s\r\n", Result, Poll + 1,
         Ok ? L"TRANSFER PASS" : L"TIMEOUT");
  if (!StartFalcon) {
    Print (L"R246 stop point: no BOOTVEC write and no Falcon CPUCTL start.\r\n");
  }
  return Ok;
}

STATIC BOOLEAN RunR249FalconStart (volatile UINT32 *Fpci, volatile UINT32 *Hcd,
                                   UINT32 CodeTag) {
  UINT32 SavedRange = Fpci[0x41C / 4];
  UINT32 CpuCtl, UsbSts = 0xFFFFFFFFU, Poll;
  BOOLEAN Ready = FALSE;

  Fpci[0x41C / 4] = 0;
  gBS->Stall (10);
  CpuCtl = Fpci[(0x800 + 0x100) / 4];
  Print (L"R249 FALC_CPUCTL pre-start=%08x\r\n", CpuCtl);
  if (CpuCtl == 0xFFFFFFFFU || (CpuCtl & 0x2U) != 0) {
    Fpci[0x41C / 4] = SavedRange;
    Print (L"R249 STOP: Falcon CPUCTL state invalid/already started.\r\n");
    return FALSE;
  }

  Fpci[(0x800 + 0x104) / 4] = CodeTag; // XUSB_FALC_BOOTVEC
  Fpci[(0x800 + 0x100) / 4] = 0x00000002U; // CPUCTL_STARTCPU
  Print (L"R249 BOOTVEC=%08x and CPUCTL_STARTCPU issued.\r\n", CodeTag);
  Print (L"R249 poll USBSTS.CNR (HCD+0x24), max 200 ms; no xHCI command.\r\n");
  for (Poll = 0; Poll < 200; ++Poll) {
    UsbSts = Hcd[0x024 / sizeof (UINT32)];
    if (UsbSts == 0xFFFFFFFFU || (UsbSts & (1U << 12)) != 0) break;
    if ((UsbSts & (1U << 11)) == 0) {
      Ready = TRUE;
      break;
    }
    gBS->Stall (1000);
  }
  Fpci[0x41C / 4] = 0;
  gBS->Stall (10);
  CpuCtl = Fpci[(0x800 + 0x100) / 4];
  Print (L"R249 FALC_CPUCTL post-start=%08x\r\n", CpuCtl);
  Fpci[0x41C / 4] = SavedRange;
  Print (L"R249 USBSTS=%08x polls=%u CNR_CLEAR_NO_HCE=%s\r\n", UsbSts, Poll + 1,
         Ready ? L"PASS" : L"NO");
  return Ready;
}

// R249 observes the xHCI operational register block only after CNR clears.
// It deliberately does not set USBCMD.RS, issue commands, or write PORTSC.
STATIC BOOLEAN RunR249XhciOperationalAudit (volatile UINT32 *Hcd,
                                            UINT32 HcParams1) {
  UINT32 CapBase = Hcd[0];
  UINT32 CapLength = CapBase & 0xFFU;
  UINT32 PortCount = (HcParams1 >> 24) & 0x7FU;
  UINT32 OpBase;
  UINT32 UsbCmd, UsbSts, Config, PageSize;
  UINT32 Port, PortSc;
  BOOLEAN Ok = TRUE;

  Print (L"R249 step 11: read xHCI operational registers only; no controller writes.\r\n");
  Print (L"R249 HC_CAPBASE=%08x CAPLENGTH=%02x ports=%u\r\n",
         CapBase, CapLength, PortCount);
  if (CapBase != 0x01000020U || CapLength != 0x20U || PortCount != 9U) {
    Print (L"R249 STOP: capability/port count unexpected; operational reads skipped.\r\n");
    return FALSE;
  }

  OpBase = CapLength / sizeof (UINT32);
  Print (L"R249 read USBCMD (read-only)...\r\n");
  UsbCmd = Hcd[OpBase + 0x00 / 4];
  Print (L"R249 USBCMD=%08x RS=%u HCRST=%u\r\n", UsbCmd,
         UsbCmd & 1U, (UsbCmd >> 1) & 1U);
  Print (L"R249 read USBSTS (read-only)...\r\n");
  UsbSts = Hcd[OpBase + 0x04 / 4];
  Print (L"R249 USBSTS=%08x HCHalted=%u HSE=%u CNR=%u\r\n", UsbSts,
         UsbSts & 1U, (UsbSts >> 2) & 1U, (UsbSts >> 11) & 1U);
  Print (L"R249 read PAGESIZE/CONFIG (read-only)...\r\n");
  PageSize = Hcd[OpBase + 0x08 / 4];
  Config = Hcd[OpBase + 0x38 / 4];
  Print (L"R249 PAGESIZE=%08x CONFIG=%08x\r\n", PageSize, Config);

  if (UsbCmd == 0xFFFFFFFFU || UsbSts == 0xFFFFFFFFU ||
      PageSize == 0xFFFFFFFFU || Config == 0xFFFFFFFFU ||
      (UsbCmd & 0x3U) != 0 || (UsbSts & ((1U << 2) | (1U << 11) | (1U << 12))) != 0 ||
      (UsbSts & 1U) == 0 || PageSize == 0) {
    Ok = FALSE;
  }

  for (Port = 0; Port < PortCount; ++Port) {
    UINTN PortOffset = OpBase + 0x400 / sizeof (UINT32) + Port * 4;
    Print (L"R249 read PORTSC[%u] (HCD+%03x, read-only)...\r\n",
           Port + 1, (UINT32)(PortOffset * sizeof (UINT32)));
    PortSc = Hcd[PortOffset];
    Print (L"R249 PORTSC[%u]=%08x CCS=%u PED=%u PLS=%u Speed=%u Changes=%02x\r\n",
           Port + 1, PortSc, PortSc & 1U, (PortSc >> 1) & 1U,
           (PortSc >> 5) & 0xFU, (PortSc >> 10) & 0xFU,
           (PortSc >> 17) & 0x7FU);
    if (PortSc == 0xFFFFFFFFU) Ok = FALSE;
  }

  Print (Ok ? L"R249 operational/PORTSC read-only audit: PASS; xHCI remains halted, no USB command.\r\n" :
              L"R249 STOP: operational read validation failed; no xHCI command or PORTSC write.\r\n");
  return Ok;
}

// R260 captures USB2 PADCTL state after temporarily enabling only the PADCTL
// clock when its reset is already released. All PADCTL data registers remain
// read-only and the original CAR clock word is restored exactly.
STATIC VOID CaptureR260PadctlGateState (R260_PADCTL_SAMPLE *Sample) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  Sample->ClockW = Car[0x364 / 4];
  Sample->ResetW = Car[0x35C / 4];
  Sample->GateRead = (Sample->ClockW != 0xFFFFFFFFU &&
                      Sample->ResetW != 0xFFFFFFFFU);
}

// Restore the Linux-style OTG ID override only after xHCI has halted. If the
// ID field cannot be restored, leave the PADCTL clock available and request a
// full power-off rather than continuing to tear down live USB state.
STATIC BOOLEAN R290RestoreHostIdOverride (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Pad = (volatile UINT32 *)XUSB_PAD_BASE;
  UINT32 Readback;
  BOOLEAN IdOk = TRUE;
  BOOLEAN ClockOk = TRUE;

  if (mR290IdOverrideChanged) {
    Print (L"R290 restoring USB2_VBUS_ID=%08x... ", mR290OriginalVbusId);
    Pad[R290_USB2_VBUS_ID_REG / 4] = mR290OriginalVbusId;
    gBS->Stall (10);
    Readback = Pad[R290_USB2_VBUS_ID_REG / 4];
    IdOk = (Readback == mR290OriginalVbusId);
    Print (IdOk ? L"PASS\r\n" : L"FAIL read=%08x\r\n", Readback);
    if (!IdOk) {
      mR290IdOverrideRestoreOk = FALSE;
      Print (L"R290 STOP: ID restore failed; leave PADCTL clock on and hold POWER 12-15s.\r\n");
      return FALSE;
    }
    mR290IdOverrideChanged = FALSE;
  }

  if (mR290PadctlClockDeferred) {
    UINT32 CurrentClock = Car[0x364 / 4];
    Print (L"R290 restoring PADCTL CLK_ENB_W bit14 via CLR alias (saved=%08x)... ",
           mR290OriginalPadctlClockW);
    if ((mR290OriginalPadctlClockW & R290_PADCTL_CLK_MASK) == 0 &&
        (CurrentClock & R290_PADCTL_CLK_MASK) != 0) {
      Car[R290_CLK_ENB_W_CLR_REG / 4] = R290_PADCTL_CLK_MASK;
      gBS->Stall (10);
    }
    ClockOk = ((Car[0x364 / 4] & R290_PADCTL_CLK_MASK) ==
               (mR290OriginalPadctlClockW & R290_PADCTL_CLK_MASK) &&
               (Car[0x35C / 4] & R290_PADCTL_CLK_MASK) == 0);
    Print (ClockOk ? L"PASS state=%08x\r\n" : L"FAIL state=%08x reset=%08x\r\n",
           Car[0x364 / 4], Car[0x35C / 4]);
    mR291ClockPostRestoreW = Car[0x364 / 4];
    mR291ResetPostRestoreW = Car[0x35C / 4];
    mR291PostRestoreValid = (mR291ClockPostRestoreW != 0xFFFFFFFFU &&
                              mR291ResetPostRestoreW != 0xFFFFFFFFU);
    mR291RestoreState = ClockOk ? 3 : 4;
    mR290PadctlClockDeferred = FALSE;
    if (!ClockOk) {
      mR290IdOverrideRestoreOk = FALSE;
      Print (L"R290 STOP: PADCTL clock restore failed; hold POWER 12-15s.\r\n");
      return FALSE;
    }
  }

  mR290IdOverrideRestoreOk = TRUE;
  mR260RunPadctl.ClockW = Car[0x364 / 4];
  mR260RunPadctl.ResetW = Car[0x35C / 4];
  mR260RunPadctl.RestoreOk = TRUE;
  if (mR260RunPadctl.PadValid) mR260RunPadctl.Status = R260_PADCTL_OK;
  return TRUE;
}

STATIC BOOLEAN RunR260PadctlClockReadWindow (VOID) {
  R260_PADCTL_SAMPLE *Sample = &mR260RunPadctl;
  ZeroMem (Sample, sizeof (*Sample));
  // R293 deliberately replaces the old, incorrectly identified W bit14
  // clock toggle and PADCTL ID_OVERRIDE write with a CAR-only snapshot.
  // The caller uses RestoreOk to decide whether to continue read-only PORTSC
  // observation, so keep it true when the CAR snapshot itself is readable.
  mR290IdOverrideAttempted = FALSE;
  mR290IdOverrideReady = FALSE;
  mR290IdOverrideChanged = FALSE;
  mR290PadctlClockDeferred = FALSE;
  mR290IdOverrideRestoreOk = TRUE;
  CaptureR293Car (R293_XHCI_RUN);
  CaptureR294Usb2 (R294_USB2_RUN);
  Sample->Status = mR293Car[R293_XHCI_RUN].Valid ? R260_PADCTL_OK : R260_PADCTL_CAR_UNREADABLE;
  Sample->RestoreOk = TRUE;
  Sample->ClockW = mR293Car[R293_XHCI_RUN].ClockW;
  Sample->ResetW = mR293Car[R293_XHCI_RUN].ResetW;
  Sample->GateRead = mR293Car[R293_XHCI_RUN].Valid;
  mR291PreValid = Sample->GateRead;
  mR291ClockPreW = Sample->ClockW;
  mR291ResetPreW = Sample->ResetW;
  mR291ClockPostSetW = Sample->ClockW;
  mR291ResetPostSetW = Sample->ResetW;
  mR291ClockPostRestoreW = Sample->ClockW;
  mR291ResetPostRestoreW = Sample->ResetW;
  mR291PreValid = mR291PostSetValid = mR291PostRestoreValid = Sample->GateRead;
  mR291SetAttempted = FALSE;
  mR291RestoreState = 1;
  Print (L"R293 xHCI RUN: CAR snapshot only; skipped PADCTL clock/ID_OVERRIDE writes.\r\n");
  Print (L"R293 CLK_W/Y=%08x/%08x RST_W/Y=%08x/%08x\r\n",
         mR293Car[R293_XHCI_RUN].ClockW, mR293Car[R293_XHCI_RUN].ClockY,
         mR293Car[R293_XHCI_RUN].ResetW, mR293Car[R293_XHCI_RUN].ResetY);
  return Sample->GateRead;
}

STATIC CONST CHAR16 *R260PadctlStatusText (R260_PADCTL_STATUS Status) {
  switch (Status) {
    case R260_PADCTL_OK: return L"PASS";
    case R260_PADCTL_CAR_UNREADABLE: return L"CAR-READ";
    case R260_PADCTL_RESET_ASSERTED: return L"RESET-ON";
    case R260_PADCTL_CLOCK_VERIFY_FAILED: return L"CLK-VERIFY";
    case R260_PADCTL_REGS_INVALID: return L"PAD-READ";
    case R260_PADCTL_RESTORE_FAILED: return L"RESTORE";
    default: return L"NOT-RUN";
  }
}

STATIC VOID PrintR260EndSummary (VOID) {
  UINTN Index;
  Print (L"R260 SUMMARY: Linux DT usb2-0 mode=OTG lane=XUSB; protocol USB2 range=5..9.\r\n");
  Print (L"R260 PADCTL diagnostic=%s gate=%u reset14=%u clk14=%u en=%u read=%u restore=%u\r\n",
         R260PadctlStatusText (mR260RunPadctl.Status),
         mR260RunPadctl.GateRead,
         (mR260RunPadctl.ResetW >> 14) & 1U,
         (mR260RunPadctl.ClockW >> 14) & 1U,
         mR260RunPadctl.ClockVerified,
         mR260RunPadctl.PadValid,
         mR260RunPadctl.RestoreOk);
  Print (L"R291 PRE valid=%u CLK/RST=%08x/%08x b14=%u/%u\r\n",
         mR291PreValid, mR291ClockPreW, mR291ResetPreW,
         (mR291ClockPreW >> 14) & 1U, (mR291ResetPreW >> 14) & 1U);
  Print (L"R291 SET try=%u valid=%u CLK/RST=%08x/%08x b14=%u/%u\r\n",
         mR291SetAttempted, mR291PostSetValid,
         mR291ClockPostSetW, mR291ResetPostSetW,
         (mR291ClockPostSetW >> 14) & 1U,
         (mR291ResetPostSetW >> 14) & 1U);
  Print (L"R291 REST st=%u valid=%u CLK/RST=%08x/%08x b14=%u/%u\r\n",
         mR291RestoreState, mR291PostRestoreValid,
         mR291ClockPostRestoreW, mR291ResetPostRestoreW,
         (mR291ClockPostRestoreW >> 14) & 1U,
         (mR291ResetPostRestoreW >> 14) & 1U);
  if (mR260EarlyPadctl.GateRead) {
    Print (L"R260 EARLY PadCTL CLK/RST_W=%08x/%08x bit14=%u/%u\r\n",
           mR260EarlyPadctl.ClockW, mR260EarlyPadctl.ResetW,
           (mR260EarlyPadctl.ClockW >> 14) & 1U,
           (mR260EarlyPadctl.ResetW >> 14) & 1U);
  } else {
    Print (L"R260 EARLY PadCTL gate/reset unreadable.\r\n");
  }
  if (mR260RunPadctl.GateRead) {
    Print (L"R260 RUN PadCTL CLK/RST_W=%08x/%08x bit14=%u/%u\r\n",
           mR260RunPadctl.ClockW, mR260RunPadctl.ResetW,
           (mR260RunPadctl.ClockW >> 14) & 1U,
           (mR260RunPadctl.ResetW >> 14) & 1U);
  }
  if (mR260RunPadctl.PadValid) {
    Print (L"R260 PAD0 MUX=%08x PORT_CAP=%08x CTL0/1=%08x/%08x\r\n",
           mR260RunPadctl.Mux, mR260RunPadctl.PortCap,
           mR260RunPadctl.Pad0Ctl0, mR260RunPadctl.Pad0Ctl1);
    Print (L"R260 BIAS0/1=%08x/%08x VBUS_ID=%08x\r\n",
           mR260RunPadctl.BiasCtl0, mR260RunPadctl.BiasCtl1,
           mR260RunPadctl.VbusId);
  } else {
    Print (L"R260 PAD0 register values unavailable; see clock/restore status above.\r\n");
  }
  Print (L"R260 USB2 PORTSC P5..P9:");
  for (Index = 0; Index < 5; ++Index) {
    if ((mR260Usb2PortValidMask & (1U << Index)) != 0) {
      Print (L" P%u=%08x/CCS%u", (UINT32)Index + 5U,
             mR260Usb2PortSc[Index], mR260Usb2PortSc[Index] & 1U);
    } else {
      Print (L" P%u=NA", (UINT32)Index + 5U);
    }
  }
  if (mR260P5SampleValid) {
    Print (L"\r\nR260 P5 2s RO n=%u %08x>%08x chg=%u CCS=%u\r\n",
           (UINT32)mR260P5Samples, mR260P5First, mR260P5Last,
           mR260P5Changes, mR260P5ConnectedSamples);
  } else {
    Print (L"\r\nR260 P5 2s RO: no valid samples\r\n");
  }
  Print (L"R290 Linux host ID_OVERRIDE=%s restore=%s (no port power/reset/enum).\r\n",
         !mR290IdOverrideAttempted ? L"NOT-ATTEMPTED" :
         (mR290IdOverrideReady ? L"PASS" : L"FAIL"),
         !mR290IdOverrideAttempted ? L"N/A" :
         (mR290IdOverrideRestoreOk ? L"PASS" : L"FAIL"));
  Print (L"R290 boundary: only CLK_ENB_W bit14 uses SET/CLR aliases and ID_OVERRIDE is masked/restored; no other PADCTL data/reset, PORTSC, port power/reset, USB command, enumeration, or transfer.\r\n");
}

// Walk only the xHCI extended-capability linked list, bounded to 8 headers.
// Supported Protocol entries (ID 2) expose each root hub's port range.
STATIC VOID DumpR260ProtocolPortMap (volatile UINT32 *Hcd, UINT32 HccParams1,
                                     UINT32 MaxPorts) {
  UINT32 Offset = ((HccParams1 >> 16) & 0xFFFFU) * 4U;
  UINT32 Seen = 0;
  UINTN Index;

  if (Offset == 0 || (Offset & 3U) != 0 || Offset >= 0x1000U) {
    Print (L"R260 xHCI protocol map SKIP: xECP=%03x invalid.\r\n", Offset);
    return;
  }
  for (Index = 0; Index < 8 && Offset != 0; ++Index) {
    UINT32 Header, Id, Next;
    if (Offset >= 0x1000U || (Offset & 3U) != 0) break;
    Header = Hcd[Offset / 4];
    if (Header == 0xFFFFFFFFU || Header == 0) break;
    Id = Header & 0xFFU;
    Next = (Header >> 8) & 0xFFU;
    if (Id == 2U) {
      UINT32 Name, Ports, Major, Minor, PortOffset, Count;
      if (Offset > 0xFF0U) break;
      Name = Hcd[(Offset + 4U) / 4];
      Ports = Hcd[(Offset + 8U) / 4];
      // The protocol revision is encoded in DWORD0 (the capability header);
      // DWORD2 is port offset/count plus protocol-specific fields.
      Major = (Header >> 24) & 0xFFU;
      Minor = (Header >> 16) & 0xFFU;
      PortOffset = Ports & 0xFFU;
      Count = (Ports >> 8) & 0xFFU;
      if (Name == 0x20425355U && PortOffset != 0 && Count != 0 &&
          PortOffset + Count - 1U <= MaxPorts) {
        Print (L"R260 xHCI map USB %u.%u root-ports=%u..%u\r\n",
               Major, Minor, PortOffset, PortOffset + Count - (Count != 0));
        Seen++;
      } else {
        Print (L"R260 xHCI protocol cap invalid name=%08x range=%u+%u\r\n",
               Name, PortOffset, Count);
      }
    }
    if (Next == 0) break;
    Offset += Next * 4U;
  }
  Print (Seen != 0 ? L"R260 xHCI USB protocol map: read-only PASS\r\n" :
                     L"R260 xHCI USB protocol map: no Supported Protocol entry found.\r\n");
}

STATIC BOOLEAN RunR260XhciRunPortReadStop (volatile UINT32 *Hcd,
                                          UINT32 HcParams1,
                                          BOOLEAN *ControllerHalted) {
  UINT32 CapLength = Hcd[0] & 0xFFU;
  UINT32 PortCount = (HcParams1 >> 24) & 0x7FU;
  UINT32 HccParams1 = Hcd[0x10 / 4];
  UINT32 OpBase;
  UINT32 UsbCmd;
  UINT32 UsbSts;
  UINT32 Port;
  UINTN Poll;
  UINT32 CcsBitmap = 0;
  UINTN Samples = 0;
  UINT32 P5First = 0;
  UINT32 P5Last = 0;
  UINT32 P5Changes = 0;
  UINT32 P5ConnectedSamples = 0;
  BOOLEAN P5ReadOk = TRUE;
  BOOLEAN RunSeen = FALSE;
  BOOLEAN HaltSeen = FALSE;
  BOOLEAN PortReadOk = TRUE;
  BOOLEAN PadctlSafe = TRUE;
  BOOLEAN PortAuditDone = FALSE;

  *ControllerHalted = FALSE;
  if (CapLength != 0x20U) {
    Print (L"R260 STOP: CAPLENGTH=%02x unexpected; no RS write.\r\n", CapLength);
    return FALSE;
  }
  if (PortCount != 9U) {
    Print (L"R260 STOP: HCP1 ports=%u unexpected; no RS write.\r\n", PortCount);
    return FALSE;
  }
  OpBase = CapLength / sizeof (UINT32);
  UsbCmd = Hcd[OpBase + 0x00 / 4];
  UsbSts = Hcd[OpBase + 0x04 / 4];
  Print (L"R260 precondition USBCMD=%08x USBSTS=%08x ports=%u\r\n",
         UsbCmd, UsbSts, PortCount);

  // Only proceed from the verified stopped state. HCRST must be clear,
  // RS must be clear, and the controller must report halted without HSE/CNR.
  if (UsbCmd == 0xFFFFFFFFU || UsbSts == 0xFFFFFFFFU ||
      (UsbCmd & 0x3U) != 0 || (UsbSts & 1U) == 0 ||
      (UsbSts & ((1U << 2) | (1U << 11) | (1U << 12))) != 0) {
    Print (L"R260 STOP: initial halted/CNR/HSE/command state invalid; no RS write.\r\n");
    return FALSE;
  }
  *ControllerHalted = TRUE;

  Print (L"R260 step 12: set USBCMD.RS only; no rings or USB command...\r\n");
  *ControllerHalted = FALSE;
  Hcd[OpBase + 0x00 / 4] = UsbCmd | 1U;
  gBS->Stall (10);
  if ((Hcd[OpBase + 0x00 / 4] & 0x3U) != 1U) {
    Print (L"R260 STOP: RS readback did not match; attempting bounded halt.\r\n");
  } else {
    Print (L"R260 USBCMD.RS readback: PASS\r\n");
  }

  for (Poll = 0; Poll < 100; ++Poll) {
    UsbSts = Hcd[OpBase + 0x04 / 4];
    if (UsbSts == 0xFFFFFFFFU ||
        (UsbSts & ((1U << 2) | (1U << 11) | (1U << 12))) != 0) {
      break;
    }
    if ((UsbSts & 1U) == 0) {
      RunSeen = TRUE;
      break;
    }
    gBS->Stall (1000);
  }
  Print (L"R260 run poll USBSTS=%08x polls=%u HCHalted_clear=%s\r\n",
         UsbSts, (UINT32)((Poll < 100) ? Poll + 1 : 100), RunSeen ? L"YES" : L"NO");

  if (RunSeen) {
    // Linux Tegra210 maps USB2 logical port 1 to xHCI port 5 (offset 4).
    // Observe that candidate for 2 seconds, read-only, before the final map.
    // Do not acknowledge change bits, reset/power ports, or access a device.
    Print (L"R260 step 13: temporary PadCTL clock/read/restore; then observe USB2 P5...\r\n");
    // PADCTL diagnostics and xHCI PORTSC observation are independent. A
    // PADCTL read failure must be reported, but must not truncate safe,
    // read-only PORTSC sampling or masquerade as a port-audit failure.
    RunR260PadctlClockReadWindow ();
    PadctlSafe = mR260RunPadctl.RestoreOk;
    if (PadctlSafe) {
    DumpR260ProtocolPortMap (Hcd, HccParams1, PortCount);
    for (Poll = 0; Poll < 20; ++Poll) {
      Samples++;
      {
        UINT32 PortSc = Hcd[OpBase + 0x400 / sizeof (UINT32) + 4U * 4U];
        if (PortSc == 0xFFFFFFFFU) {
          P5ReadOk = FALSE;
          PortReadOk = FALSE;
          break;
        }
        if (Poll == 0) P5First = PortSc;
        if (Poll != 0 && PortSc != P5Last) P5Changes++;
        if ((PortSc & 1U) != 0) P5ConnectedSamples++;
        P5Last = PortSc;
      }
      if (!PortReadOk) break;
      gBS->Stall (100000);
    }
    mR260P5Samples = Samples;
    mR260P5First = P5First;
    mR260P5Last = P5Last;
    mR260P5Changes = P5Changes;
    mR260P5ConnectedSamples = P5ConnectedSamples;
    mR260P5SampleValid = (Samples != 0) && P5ReadOk;
    Print (L"R260 P5 read-only samples=%u first=%08x last=%08x changes=%u CCS_samples=%u\r\n",
           (UINT32)Samples, P5First, P5Last, P5Changes, P5ConnectedSamples);
    Print (L"R260 final PORTSC snapshot (raw; CCS=bit0):\r\n");
    for (Port = 0; Port < PortCount; Port += 3) {
      UINT32 PortSc[3];
      UINTN SubPort;
      for (SubPort = 0; SubPort < 3; ++SubPort) {
        UINTN PortOffset = OpBase + 0x400 / sizeof (UINT32) + (Port + SubPort) * 4;
        PortSc[SubPort] = Hcd[PortOffset];
        if (PortSc[SubPort] == 0xFFFFFFFFU) PortReadOk = FALSE;
        if ((PortSc[SubPort] & 1U) != 0) CcsBitmap |= 1U << (Port + SubPort);
        if (Port + SubPort >= 4 && Port + SubPort < 9 &&
            PortSc[SubPort] != 0xFFFFFFFFU) {
          UINTN Usb2Index = Port + SubPort - 4;
          mR260Usb2PortSc[Usb2Index] = PortSc[SubPort];
          mR260Usb2PortValidMask |= 1U << Usb2Index;
        }
      }
      Print (L"P%u=%08x P%u=%08x P%u=%08x\r\n",
             Port + 1, PortSc[0], Port + 2, PortSc[1], Port + 3, PortSc[2]);
    }
    Print (L"CCS bitmap=%03x (bit0=P1)\r\n", CcsBitmap);
    UsbSts = Hcd[OpBase + 0x04 / 4];
    Print (L"R260 post-PORTSC USBSTS=%08x HCHalted=%u HSE=%u CNR=%u\r\n",
           UsbSts, UsbSts & 1U, (UsbSts >> 2) & 1U, (UsbSts >> 11) & 1U);
    if (UsbSts == 0xFFFFFFFFU || (UsbSts & 1U) != 0 ||
        (UsbSts & ((1U << 2) | (1U << 11) | (1U << 12))) != 0) {
      PortReadOk = FALSE;
    }
    Print (PortReadOk ?
           L"R260 PORTSC read-only audit: PASS; no PORTSC write, reset, ring, or USB transfer.\r\n" :
           L"R260 PORTSC read-only audit: STOP; status unexpected; halt still requested.\r\n");
      PortAuditDone = TRUE;
    } else {
      Print (L"R260 PORTSC audit SKIP: PadCTL clock restore unverified; halting xHCI.\r\n");
    }
  }

  // Always request stop after the short observation window, then verify the
  // controller is halted before restoring clocks, power, BAR, and IPFS state.
  Print (L"R260 step 14: clear USBCMD.RS only and verify halt (max 100 ms)...\r\n");
  UsbCmd = Hcd[OpBase + 0x00 / 4];
  if (UsbCmd == 0xFFFFFFFFU) {
    Print (L"R260 STOP: USBCMD unreadable; controller state is unsafe to restore.\r\n");
    return FALSE;
  }
  Hcd[OpBase + 0x00 / 4] = UsbCmd & ~1U;
  gBS->Stall (10);
  for (Poll = 0; Poll < 100; ++Poll) {
    UsbSts = Hcd[OpBase + 0x04 / 4];
    if (UsbSts == 0xFFFFFFFFU ||
        (UsbSts & ((1U << 2) | (1U << 11) | (1U << 12))) != 0) {
      break;
    }
    if ((UsbSts & 1U) != 0) {
      HaltSeen = TRUE;
      break;
    }
    gBS->Stall (1000);
  }
  Print (L"R260 stop poll USBSTS=%08x polls=%u HCHalted=%s\r\n",
         UsbSts, (UINT32)((Poll < 100) ? Poll + 1 : 100), HaltSeen ? L"YES" : L"NO");
  *ControllerHalted = HaltSeen;
  if (!HaltSeen) {
    Print (L"R260 STOP: halt not confirmed; leave XUSB power/clocks intact and hold POWER 12-15s.\r\n");
    return FALSE;
  }
  if ((mR290IdOverrideChanged || mR290PadctlClockDeferred) &&
      !R290RestoreHostIdOverride ()) {
    *ControllerHalted = FALSE;
    Print (L"R290 SAFETY STOP: role/clock restore unverified; skip further XUSB teardown and hold POWER 12-15s.\r\n");
    return FALSE;
  }
  Print ((RunSeen && PortReadOk && PortAuditDone) ?
         L"R260 PORTSC Run/Stop test: PASS; halted restored; no USB command/transfer.\r\n" :
         L"R260 STOP: port observation or RUN failed; halted restored; no USB command/transfer.\r\n");
  return RunSeen && PortReadOk && PortAuditDone;
}

STATIC EFI_HANDLE R304FindDriverImage (CONST CHAR16 *ExpectedName) {
  EFI_HANDLE *Handles = NULL;
  EFI_HANDLE Image = NULL;
  UINTN Count = 0;
  UINTN Index;
  EFI_STATUS Status;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiDriverBindingProtocolGuid,
                                   NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) return NULL;
  for (Index = 0; Index < Count; ++Index) {
    EFI_DRIVER_BINDING_PROTOCOL *Binding = NULL;
    EFI_COMPONENT_NAME2_PROTOCOL *ComponentName = NULL;
    CHAR16 *Name = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index],
                                       &gEfiDriverBindingProtocolGuid,
                                       (VOID **)&Binding)) || Binding == NULL)
      continue;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index],
                                       &gEfiComponentName2ProtocolGuid,
                                       (VOID **)&ComponentName)) || ComponentName == NULL)
      continue;
    if (!EFI_ERROR (ComponentName->GetDriverName (ComponentName, "en", &Name)) &&
        Name != NULL && StrCmp (Name, ExpectedName) == 0) {
      Image = Binding->ImageHandle;
      break;
    }
  }
  if (Handles != NULL) gBS->FreePool (Handles);
  return Image;
}

// R309 starts the EDK2 xHCI binding after the R304 preflight, then immediately
// calls its Stop() path. It never calls ConnectController()/UsbBusDxe and does
// not request port reset, enumeration, descriptors, or transfers.
STATIC BOOLEAN RunR309XhciStartStopProbe (volatile UINT32 *Hcd,
                                         UINT32 OpBase,
                                         BOOLEAN *ControllerSafeToRestore) {
  EFI_STATUS Status;
  EFI_HANDLE Controller = NULL;
  EFI_HANDLE NdImage;
  EFI_HANDLE XhciImage;
  EFI_DRIVER_BINDING_PROTOCOL *NdBinding = NULL;
  EFI_DRIVER_BINDING_PROTOCOL *XhciBinding = NULL;
  EFI_PCI_IO_PROTOCOL *PciIo = NULL;
  NON_DISCOVERABLE_DEVICE *NdDevice = NULL;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath = NULL;
  BOOLEAN NdSupportedPassed = FALSE;
  BOOLEAN NdStartPassed = FALSE;
  BOOLEAN PciIoObserved = FALSE;
  BOOLEAN XhciSupportedPassed = FALSE;
  BOOLEAN NdStopPassed = FALSE;
  BOOLEAN PciIoRemoved = FALSE;
  BOOLEAN CleanupPassed = FALSE;
  BOOLEAN XhciStartPassed = FALSE;
  BOOLEAN XhciStopPassed = FALSE;
  BOOLEAN Usb2HcRemoved = FALSE;
  BOOLEAN XhciRunVerified = FALSE;
  BOOLEAN Usb2HcInstalled = FALSE;

  mR304ProbeAttempted = TRUE;
  mR304NdSupportedPassed = FALSE;
  mR304NdStartPassed = FALSE;
  mR304PciIoObserved = FALSE;
  mR304XhciSupportedPassed = FALSE;
  mR304NdStopPassed = FALSE;
  mR304PciIoRemoved = FALSE;
  mR304RegistrationCleanupPassed = FALSE;
  mR309XhciStartAttempted = FALSE;
  mR309XhciStartPassed = FALSE;
  mR309XhciStopAttempted = FALSE;
  mR309XhciStopPassed = FALSE;
  mR309Usb2HcRemoved = FALSE;
  mR309XhciRunVerified = FALSE;
  mR309Usb2HcInstalled = FALSE;
  mR300RegistrationAttempted = TRUE;
  mR300RegistrationPassed = FALSE;
  mR300Gate = R300_GATE_REGISTRATION;
  *ControllerSafeToRestore = TRUE;
  Print (L"R309 xHCI Start/Stop probe: USBSTS=%08x\r\n",
         Hcd[OpBase + 0x04 / 4]);
  if (Hcd[0] != 0x01000020U ||
      (Hcd[OpBase + 0x00 / 4] & 1U) != 0 ||
      (Hcd[OpBase + 0x04 / 4] & 1U) == 0 ||
      (Hcd[OpBase + 0x04 / 4] & ((1U << 2) | (1U << 11) | (1U << 12))) != 0) {
    Print (L"R309 STOP: accessible, halted xHCI baseline not met.\r\n");
    return FALSE;
  }
  if (mR260Usb2PortValidMask != 0x1FU ||
      (mR260Usb2PortSc[0] & 1U) || (mR260Usb2PortSc[1] & 1U) ||
      (mR260Usb2PortSc[2] & 1U) || (mR260Usb2PortSc[3] & 1U) ||
      (mR260Usb2PortSc[4] & 1U)) {
    Print (L"R309 STOP: require valid P5-P9 snapshot with CCS=0 on every port.\r\n");
    return FALSE;
  }
  Print (L"R309 no-device gate: P5-P9 valid; CCS=0 on all ports.\r\n");

  NdImage = R304FindDriverImage (L"PCI I/O protocol emulation driver for non-discoverable devices");
  XhciImage = R304FindDriverImage (L"Usb Xhci Driver");
  if (NdImage == NULL || XhciImage == NULL ||
      EFI_ERROR (gBS->HandleProtocol (NdImage, &gEfiDriverBindingProtocolGuid,
                                      (VOID **)&NdBinding)) || NdBinding == NULL ||
      EFI_ERROR (gBS->HandleProtocol (XhciImage, &gEfiDriverBindingProtocolGuid,
                                      (VOID **)&XhciBinding)) || XhciBinding == NULL) {
    Print (L"R309 STOP: required ND/xHCI DriverBinding missing (ND=%u XHCI=%u).\r\n",
           NdImage != NULL, XhciImage != NULL);
    return FALSE;
  }
  Print (L"R309 driver images found: ND=%u xHCI=%u version=%u\r\n",
         NdImage != NULL, XhciImage != NULL, XhciBinding->Version);

  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL, &Controller, 1,
             (UINTN)XUSB_HCD_BASE, (UINTN)0x10000);
  if (EFI_ERROR (Status) || Controller == NULL) {
    Print (L"R309 temporary XUSB registration: FAIL status=%r\r\n", Status);
    return FALSE;
  }
  mR300RegistrationPassed = TRUE;
  Print (L"R309 temporary XUSB non-discoverable registration: PASS\r\n");

  Print (L"R309 calling ND Supported() directly...\r\n");
  Status = NdBinding->Supported (NdBinding, Controller, NULL);
  NdSupportedPassed = !EFI_ERROR (Status);
  Print (L"R309 ND Supported(): %s status=%r\r\n",
         NdSupportedPassed ? L"PASS" : L"FAIL/UNSUPPORTED", Status);

  if (NdSupportedPassed) {
    Print (L"R309 calling ND Start() directly...\r\n");
    Status = NdBinding->Start (NdBinding, Controller, NULL);
    NdStartPassed = !EFI_ERROR (Status);
    Print (L"R309 ND Start(): %s status=%r\r\n",
           NdStartPassed ? L"PASS" : L"FAIL", Status);
    Status = gBS->HandleProtocol (Controller, &gEfiPciIoProtocolGuid,
                                 (VOID **)&PciIo);
    PciIoObserved = !EFI_ERROR (Status) && PciIo != NULL;
    Print (L"R309 PCI-I/O install readback: %s status=%r\r\n",
           PciIoObserved ? L"PASS" : L"NOT-FOUND", Status);
    if (NdStartPassed && PciIoObserved) {
      Print (L"R309 calling XhciDxe Supported()...\r\n");
      Status = XhciBinding->Supported (XhciBinding, Controller, NULL);
      XhciSupportedPassed = !EFI_ERROR (Status);
      Print (L"R309 XhciDxe Supported(): %s status=%r\r\n",
             XhciSupportedPassed ? L"PASS" : L"NO-MATCH/FAIL", Status);
      if (XhciSupportedPassed) {
        mR309XhciStartAttempted = TRUE;
        Print (L"R309 calling XhciDxe Start(); no ConnectController...\r\n");
        Status = XhciBinding->Start (XhciBinding, Controller, NULL);
        XhciStartPassed = !EFI_ERROR (Status);
        mR309XhciStartPassed = XhciStartPassed;
        Print (L"R309 XhciDxe Start(): %s status=%r\r\n",
               XhciStartPassed ? L"PASS" : L"FAIL", Status);
        if (XhciStartPassed) {
          EFI_USB2_HC_PROTOCOL *Usb2Hc = NULL;
          UINT32 UsbCmd;
          UINT32 UsbSts;
          UsbCmd = Hcd[OpBase + 0x00 / 4];
          UsbSts = Hcd[OpBase + 0x04 / 4];
          XhciRunVerified = ((UsbCmd & 1U) != 0) && ((UsbSts & 1U) == 0) &&
                            ((UsbSts & (1U << 11)) == 0);
          mR309XhciRunVerified = XhciRunVerified;
          Print (L"R309 post-Start USBCMD=%08x USBSTS=%08x RUN-verified=%s\r\n",
                 UsbCmd, UsbSts, XhciRunVerified ? L"YES" : L"NO");
          Status = gBS->HandleProtocol (Controller, &gEfiUsb2HcProtocolGuid,
                                        (VOID **)&Usb2Hc);
          Usb2HcInstalled = !EFI_ERROR (Status) && Usb2Hc != NULL;
          mR309Usb2HcInstalled = Usb2HcInstalled;
          Print (L"R309 USB2_HC install readback: %s status=%r\r\n",
                 Usb2HcInstalled ? L"PASS" : L"FAIL", Status);
          mR309XhciStopAttempted = TRUE;
          Print (L"R309 immediately calling XhciDxe Stop()...\r\n");
          Status = XhciBinding->Stop (XhciBinding, Controller, 0, NULL);
          XhciStopPassed = !EFI_ERROR (Status);
          mR309XhciStopPassed = XhciStopPassed;
          Print (L"R309 XhciDxe Stop(): %s status=%r\r\n",
                 XhciStopPassed ? L"PASS" : L"FAIL", Status);
          Usb2Hc = NULL;
          Status = gBS->HandleProtocol (Controller, &gEfiUsb2HcProtocolGuid,
                                        (VOID **)&Usb2Hc);
          Usb2HcRemoved = EFI_ERROR (Status);
          mR309Usb2HcRemoved = Usb2HcRemoved;
          Print (L"R309 USB2_HC removal readback: %s status=%r\r\n",
                 Usb2HcRemoved ? L"PASS" : L"STILL-PRESENT", Status);
          if (!XhciStopPassed || !Usb2HcRemoved ||
              (Hcd[OpBase + 0x04 / 4] & 1U) == 0) {
            Print (L"R309 SAFETY STOP: xHCI Stop/halt not verified; skip XUSB restore; hold POWER.\r\n");
            *ControllerSafeToRestore = FALSE;
            return FALSE;
          }
          Print (L"R309 xHCI halted after Stop; USB command/transfer=NONE.\r\n");
        } else {
          // The stock EDK2 Start() has failure exits that do not all prove the
          // controller halted. Do not tear down ND PCI-I/O or power/clock state.
          Print (L"R309 SAFETY STOP: Start failed; controller state uncertain; hold POWER.\r\n");
          *ControllerSafeToRestore = FALSE;
          return FALSE;
        }
      }
    }
  }

  if (NdStartPassed || PciIoObserved) {
    Print (L"R309 calling ND Stop() to remove PCI-I/O...\r\n");
    Status = NdBinding->Stop (NdBinding, Controller, 0, NULL);
    NdStopPassed = !EFI_ERROR (Status);
    Print (L"R309 ND Stop(): %s status=%r\r\n",
           NdStopPassed ? L"PASS" : L"FAIL", Status);
    if (!NdStopPassed) {
      Print (L"R309 SAFETY STOP: ND Stop failed; do not restore XUSB power/clock; hold POWER.\r\n");
      *ControllerSafeToRestore = FALSE;
      return FALSE;
    }
    PciIo = NULL;
    Status = gBS->HandleProtocol (Controller, &gEfiPciIoProtocolGuid,
                                  (VOID **)&PciIo);
    PciIoRemoved = EFI_ERROR (Status);
    Print (L"R309 PCI-I/O removal readback: %s status=%r\r\n",
           PciIoRemoved ? L"PASS" : L"STILL-PRESENT", Status);
    if (!PciIoRemoved) {
      Print (L"R309 SAFETY STOP: PCI-I/O remains installed; hold POWER.\r\n");
      *ControllerSafeToRestore = FALSE;
      return FALSE;
    }
  }

  Status = gBS->HandleProtocol (Controller,
                                &gEdkiiNonDiscoverableDeviceProtocolGuid,
                                (VOID **)&NdDevice);
  if (!EFI_ERROR (Status) && NdDevice != NULL) {
    Status = gBS->HandleProtocol (Controller, &gEfiDevicePathProtocolGuid,
                                  (VOID **)&DevicePath);
  }
  if (!EFI_ERROR (Status) && NdDevice != NULL && DevicePath != NULL) {
    Status = gBS->UninstallMultipleProtocolInterfaces (
               Controller,
               &gEdkiiNonDiscoverableDeviceProtocolGuid, NdDevice,
               &gEfiDevicePathProtocolGuid, DevicePath,
               NULL);
    if (!EFI_ERROR (Status)) {
      gBS->FreePool (NdDevice);
      gBS->FreePool (DevicePath);
      CleanupPassed = TRUE;
    } else {
      Print (L"R309 registration cleanup: FAIL status=%r\r\n", Status);
    }
  }
  mR304NdSupportedPassed = NdSupportedPassed;
  mR304NdStartPassed = NdStartPassed;
  mR304PciIoObserved = PciIoObserved;
  mR304XhciSupportedPassed = XhciSupportedPassed;
  mR304NdStopPassed = NdStopPassed;
  mR304PciIoRemoved = PciIoRemoved;
  mR304RegistrationCleanupPassed = CleanupPassed;
  mR300RegistrationPassed = mR300RegistrationPassed && CleanupPassed;
  if (!CleanupPassed) *ControllerSafeToRestore = FALSE;
  Print (L"R309 registration cleanup: %s\r\n",
         CleanupPassed ? L"PASS" : L"FAIL/SKIP");
  Print (L"R309 ConnectController=NOT-ATTEMPTED; USB I/O=NO\r\n");
  if (mR300RegistrationPassed) mR300Gate = R300_GATE_DONE;
  return mR304NdSupportedPassed && mR304NdStartPassed && mR304PciIoObserved &&
         mR304XhciSupportedPassed && XhciStartPassed && XhciRunVerified &&
         Usb2HcInstalled && XhciStopPassed &&
         Usb2HcRemoved && mR304NdStopPassed && mR304PciIoRemoved &&
         mR304RegistrationCleanupPassed;
}

// R362: fuse-derived UTMI pad0 calibration and the FS/LS driver level, as in
// Hekate bdk/usb/xusbd.c _xusb_init_phy() and Linux tegra210_usb2_phy_power_on:
//   OTG_PAD0_CTL0 (0x088) [5:0]   HS_CURR_LEVEL  = FUSE_USB_CALIB[5:0]
//   OTG_PAD0_CTL1 (0x08C) [6:3]   TERM_RANGE_ADJ = FUSE_USB_CALIB[10:7]
//                         [30:26] RPD_CTRL       = FUSE_USB_CALIB_EXT[4:0]
//   BATTERY_CHRG_OTGPAD0_CTL1 (0x084) [8:6] = 010b (VREG_FIX18=0, VREG_LEV=1)
// R360 and earlier left 0x084 at 0 (VREG_LEV unset); full-speed devices then
// connected/enabled but never enumerated and stayed suspended (~1.4 mA).
// Fuses are read only when the FUSE clock (CAR CLK_OUT_ENB_H bit 7, Linux
// TEGRA210_CLK_FUSE=39) is on, to avoid a gated APB access. Otherwise, or if
// the read looks blank, this unit's values from the Linux dmesg snapshot are
// used (TEGRA_FUSE_SKU_CALIB_0=0x09248c11, TEGRA_FUSE_USB_CALIB_EXT_0=0x4).
#define R362_FUSE_BASE             0x7000F800UL
#define R362_FUSE_USB_CALIB        0x1F0U
#define R362_FUSE_USB_CALIB_EXT    0x350U
#define R362_LINUX_USB_CALIB       0x09248C11U
#define R362_LINUX_USB_CALIB_EXT   0x00000004U

STATIC BOOLEAN R362ApplyFsPhyCalibration (VOID) {
  volatile UINT32 *Pad = (volatile UINT32 *)XUSB_PAD_BASE;
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Fuse = (volatile UINT32 *)R362_FUSE_BASE;
  UINT32 ClockH, Calib, CalibExt, Ctl0, Ctl1, Bc1;
  CONST CHAR16 *Source;
  BOOLEAN Ok;

  ClockH = Car[0x014 / 4];
  if ((ClockH & (1U << 7)) != 0) {
    Calib = Fuse[R362_FUSE_USB_CALIB / 4];
    CalibExt = Fuse[R362_FUSE_USB_CALIB_EXT / 4];
    Source = L"FUSE";
    if (Calib == 0 || Calib == 0xFFFFFFFFU) {
      Calib = R362_LINUX_USB_CALIB;
      CalibExt = R362_LINUX_USB_CALIB_EXT;
      Source = L"LINUX-SNAPSHOT (fuse read blank)";
    }
  } else {
    Calib = R362_LINUX_USB_CALIB;
    CalibExt = R362_LINUX_USB_CALIB_EXT;
    Source = L"LINUX-SNAPSHOT (FUSE clock off; fuses not read)";
  }
  Print (L"R362 FUSE: CLK_OUT_ENB_H=%08x source=%s USB_CALIB=%08x USB_CALIB_EXT=%08x linux=%08x/%08x %s\r\n",
         ClockH, Source, Calib, CalibExt, R362_LINUX_USB_CALIB, R362_LINUX_USB_CALIB_EXT,
         (Calib == R362_LINUX_USB_CALIB && (CalibExt & 0x1FU) == R362_LINUX_USB_CALIB_EXT) ?
         L"MATCH" : L"DIFF");
  Print (L"R362 FS PHY before: BC1=%08x OTG0=%08x OTG1=%08x (HS_CURR=%u TERM=%u RPD=%u VREG[8:6]=%u)\r\n",
         Pad[0x084 / 4], Pad[0x088 / 4], Pad[0x08C / 4],
         Pad[0x088 / 4] & 0x3FU, (Pad[0x08C / 4] >> 3) & 0xFU,
         (Pad[0x08C / 4] >> 26) & 0x1FU, (Pad[0x084 / 4] >> 6) & 0x7U);

  Pad[0x088 / 4] = (Pad[0x088 / 4] & 0xFFFFFFC0U) | (Calib & 0x3FU);
  Pad[0x08C / 4] = (Pad[0x08C / 4] & 0x83FFFF87U) | ((Calib & 0x780U) >> 4) |
                   ((UINT32)(CalibExt << 27) >> 1);
  Pad[0x084 / 4] = (Pad[0x084 / 4] & 0xFFFFFE3FU) | 0x80U;
  (VOID)Pad[0x08C / 4];
  gBS->Stall (10);

  Ctl0 = Pad[0x088 / 4];
  Ctl1 = Pad[0x08C / 4];
  Bc1 = Pad[0x084 / 4];
  Ok = ((Ctl0 & 0x3FU) == (Calib & 0x3FU)) &&
       (((Ctl1 >> 3) & 0xFU) == ((Calib >> 7) & 0xFU)) &&
       (((Ctl1 >> 26) & 0x1FU) == (CalibExt & 0x1FU)) &&
       (((Bc1 >> 6) & 0x7U) == 0x2U);
  Print (L"R362 FS PHY after:  BC1=%08x OTG0=%08x OTG1=%08x (HS_CURR=%u TERM=%u RPD=%u VREG[8:6]=%u) %s\r\n",
         Bc1, Ctl0, Ctl1, Ctl0 & 0x3FU, (Ctl1 >> 3) & 0xFU, (Ctl1 >> 26) & 0x1FU,
         (Bc1 >> 6) & 0x7U, Ok ? L"PASS" : L"FAIL");
  return Ok;
}

// R362: USB2 OTG pad0 host settings proven by R353/R354/R357, applied for
// keeps. Offsets and bits follow Hekate bdk/usb/usb_t210.h and the
// _xusb_init_phy() power-down masks. Only the bits listed are written.
STATIC BOOLEAN R362ApplyHostPadctl (VOID) {
  volatile UINT32 *Pad = (volatile UINT32 *)XUSB_PAD_BASE;
  UINT32 Value;
  BOOLEAN PowerOk, IdOk, MuxOk;

  Print (L"R362 PADCTL before: MUX=%08x CAP=%08x BC0=%08x OTG0=%08x OTG1=%08x BIAS0=%08x VBUS_ID=%08x\r\n",
         Pad[0x004 / 4], Pad[0x008 / 4], Pad[0x080 / 4], Pad[0x088 / 4],
         Pad[0x08C / 4], Pad[0x284 / 4], Pad[0xC60 / 4]);

  // Hekate order: calibration and VREG level first, then the power-down bits.
  if (!R362ApplyFsPhyCalibration ()) {
    Print (L"R362 WARNING: FS PHY calibration readback mismatch; continuing with power/ID/MUX.\r\n");
  }

  // Pad0/bias power: OTG_PAD0_CTL0 PD(26)/PD_ZI(29), OTG_PAD0_CTL1 PD_DR(2),
  // BATTERY_CHRG_OTGPAD0_CTL0 PD_CHG(0), USB2_BIAS_PAD_CTL0 PD(11).
  Pad[0x088 / 4] = Pad[0x088 / 4] & ~((1U << 26) | (1U << 29));
  Pad[0x08C / 4] = Pad[0x08C / 4] & ~(1U << 2);
  Pad[0x080 / 4] = Pad[0x080 / 4] & ~1U;
  Pad[0x284 / 4] = Pad[0x284 / 4] & ~(1U << 11);
  (VOID)Pad[0x08C / 4];
  gBS->Stall (30);

  // Host role: USB2_VBUS_ID VBUS_ON(14)=0, ID source(17:16)=override,
  // ID override(21:18)=GND. Lower bits are status and are not compared.
  Value = Pad[0xC60 / 4];
  if ((Value & (1U << 14)) != 0) {
    Value &= ~(1U << 14);
    Pad[0xC60 / 4] = Value;
    gBS->Stall (1000);
  }
  Value = (Value & ~((3U << 16) | (0xFU << 18))) | (1U << 16);
  Pad[0xC60 / 4] = Value;
  gBS->Stall (1000);

  // OTG pad0 to XUSB instead of the legacy USB2 controller: PAD_MUX[1:0]=01.
  Pad[0x004 / 4] = (Pad[0x004 / 4] & ~3U) | 1U;
  gBS->Stall (10);

  PowerOk = ((Pad[0x088 / 4] & ((1U << 26) | (1U << 29))) == 0) &&
            ((Pad[0x08C / 4] & (1U << 2)) == 0) &&
            ((Pad[0x080 / 4] & 1U) == 0) &&
            ((Pad[0x284 / 4] & (1U << 11)) == 0);
  IdOk = ((Pad[0xC60 / 4] & 0x003F7000U) == (Value & 0x003F7000U));
  MuxOk = ((Pad[0x004 / 4] & 3U) == 1U) && (((Pad[0x004 / 4] >> 18) & 3U) == 1U) &&
          ((Pad[0x008 / 4] & 3U) == 3U);
  Print (L"R362 PADCTL after:  MUX=%08x CAP=%08x BC0=%08x OTG0=%08x OTG1=%08x BIAS0=%08x VBUS_ID=%08x\r\n",
         Pad[0x004 / 4], Pad[0x008 / 4], Pad[0x080 / 4], Pad[0x088 / 4],
         Pad[0x08C / 4], Pad[0x284 / 4], Pad[0xC60 / 4]);
  Print (L"R362 PADCTL host setup: pad-power=%s id-gnd=%s mux/bias/cap=%s\r\n",
         PowerOk ? L"PASS" : L"FAIL", IdOk ? L"PASS" : L"FAIL", MuxOk ? L"PASS" : L"FAIL");
  return PowerOk && IdOk && MuxOk;
}

STATIC VOID R363PrintPower (CONST CHAR16 *Tag);
STATIC VOID R366PrintFan (CONST CHAR16 *Tag, UINT32 Seconds);
STATIC VOID R373PrintTouch (VOID);

// R362: periodic read-only report of what the stock USB stack found. Runs a
// few times after ConnectController so UsbBus can enumerate in between.
STATIC VOID EFIAPI R362UsbStatus (EFI_EVENT Event, VOID *Context) {
  volatile UINT32 *Hcd = (volatile UINT32 *)XUSB_HCD_BASE;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;
  EFI_USB_IO_PROTOCOL *UsbIo;
  EFI_USB_DEVICE_DESCRIPTOR Desc;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINTN FsCount;

  mR362StatusRound++;
  Print (L"\r\nR362 USB STATUS #%u: USBCMD=%08x USBSTS=%08x P5=%08x\r\n",
         (UINT32)mR362StatusRound, Hcd[0x20 / 4], Hcd[0x24 / 4],
         Hcd[(0x20 + 0x400 + 0x10 * 4) / 4]);

  Handles = NULL;
  Count = 0;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbIoProtocolGuid, NULL,
                                           &Count, &Handles))) {
    for (Index = 0; Index < Count; ++Index) {
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiUsbIoProtocolGuid,
                                           (VOID **)&UsbIo)) &&
          !EFI_ERROR (UsbIo->UsbGetDeviceDescriptor (UsbIo, &Desc))) {
        Print (L"R362 UsbIo[%u]: VID:PID=%04x:%04x class=%02x bcdUSB=%x\r\n",
               (UINT32)Index, Desc.IdVendor, Desc.IdProduct, Desc.DeviceClass, Desc.BcdUSB);
      }
    }
    gBS->FreePool (Handles);
  }
  Print (L"R362 UsbIo handles=%u\r\n", (UINT32)Count);

  Handles = NULL;
  Count = 0;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL,
                                           &Count, &Handles))) {
    for (Index = 0; Index < Count; ++Index) {
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid,
                                           (VOID **)&BlockIo)) &&
          BlockIo->Media->RemovableMedia) {
        Print (L"R362 removable BlockIo[%u]: present=%u partition=%u block=%u last=%lx\r\n",
               (UINT32)Index, BlockIo->Media->MediaPresent, BlockIo->Media->LogicalPartition,
               BlockIo->Media->BlockSize, BlockIo->Media->LastBlock);
      }
    }
    gBS->FreePool (Handles);
  }
  FsCount = 0;
  Handles = NULL;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL,
                                           &FsCount, &Handles))) {
    gBS->FreePool (Handles);
  }
  Print (L"R362 BlockIo handles=%u SimpleFileSystem handles=%u; in the Shell run: map -r\r\n",
         (UINT32)Count, (UINT32)FsCount);
  R363PrintPower (mR363SinkMode ? L"status(sink)" : mR378NoPartner ? L"status(idle)" : L"status(otg)");
  R366PrintFan (L"status", 1);
  R373PrintTouch ();
  {
    // R368: button levels (GPIO port X = bank 5 port 3; active low, 0 = pressed).
    volatile UINT32 *Px = (volatile UINT32 *)(GPIO_BASE + 0x500U + 3U * 4U);
    UINT32 In = Px[GPIO_IN / 4], Cnf = Px[GPIO_CNF / 4];
    Print (L"R369 buttons: PX5(power?)=%u PX6(vol+)=%u PX7(vol-)=%u (0 = pressed) CNF=%02x OE=%02x\r\n",
           (In >> 5) & 1U, (In >> 6) & 1U, (In >> 7) & 1U, Cnf & 0xFFU, Px[GPIO_OE / 4] & 0xFFU);
  }

  if (mR362StatusRound >= 4) {
    gBS->SetTimer (Event, TimerCancel, 0);
    gBS->CloseEvent (Event);
    mR362StatusEvent = NULL;
  }
}

// R362: the remaining parts of Linux tegra_xusb_ipfs_config() that R243 left
// out. Without IPFS_XUSB_HOST_INTR_MASK_0.IP_INT_MASK the xHCI interrupt never
// leaves the IPFS, which UEFI's polling XhciDxe tolerates but Windows'
// interrupt-driven USBXHCI does not (R359: "Port Reset Failed").
//   IPFS+0x188 IPFS_XUSB_HOST_INTR_MASK_0           |= BIT16 (IP_INT_MASK)
//   IPFS+0x1BC IPFS_XUSB_HOST_CLKGATE_HYSTERESIS_0   = 0x80
//   FPCI+0x004 XUSB_CFG_1 command                   |= IO(0) | MEM(1) | BUS_MASTER(2)
// The command register is written as 16 bits so the W1C status half of
// XUSB_CFG_1 is not touched.
STATIC BOOLEAN R362ConfigureIpfsFpci (VOID) {
  volatile UINT32 *Ipfs = (volatile UINT32 *)XUSB_IPFS_BASE;
  volatile UINT16 *FpciCommand = (volatile UINT16 *)(XUSB_FPCI_BASE + 0x004);
  UINT32 IntrMask, Hysteresis;
  UINT16 Command;
  BOOLEAN Ok;

  IntrMask = Ipfs[0x188 / 4];
  Hysteresis = Ipfs[0x1BC / 4];
  Command = *FpciCommand;
  Print (L"R362 IPFS/FPCI before: INTR_MASK=%08x CLKGATE_HYSTERESIS=%08x FPCI_CMD=%04x\r\n",
         IntrMask, Hysteresis, Command);

  *FpciCommand = (UINT16)(Command | (1U << 0) | (1U << 1) | (1U << 2));
  gBS->Stall (10);
  Ipfs[0x188 / 4] = IntrMask | (1U << 16);
  Ipfs[0x1BC / 4] = 0x80U;
  gBS->Stall (10);

  IntrMask = Ipfs[0x188 / 4];
  Hysteresis = Ipfs[0x1BC / 4];
  Command = *FpciCommand;
  Ok = ((IntrMask & (1U << 16)) != 0) && (Hysteresis == 0x80U) &&
       ((Command & 0x7U) == 0x7U);
  Print (L"R362 IPFS/FPCI after:  INTR_MASK=%08x (IP_INT_MASK=%u) CLKGATE_HYSTERESIS=%08x FPCI_CMD=%04x (IO/MEM/BM=%u/%u/%u) %s\r\n",
         IntrMask, (IntrMask >> 16) & 1U, Hysteresis, Command,
         Command & 1U, (Command >> 1) & 1U, (Command >> 2) & 1U, Ok ? L"PASS" : L"FAIL");
  return Ok;
}

// R362: XUSB full-speed logic clock, as in Hekate _xusbd_init_device_clocks()
// (Linux xusb uses xusb_fs_src as well). R243 only programs the host core
// (CAR+0x600) and SS (CAR+0x610) sources. Each write happens only when the
// field differs, and every value is printed before and after:
//   CAR+0x0C0 PLLU_BASE              bit25 (48 MHz output) set if clear
//   CAR+0x0C4 PLLU_OUTA              bit0  (PLLU_OUT1 out of reset) set if clear
//   CAR+0x608 CLK_SOURCE_XUSB_FS     [31:29] = 2 (FO 48 MHz)
// CLK_SOURCE_XUSB_FALCON (0x604) and XUSB_SS (0x610) are printed only.
STATIC BOOLEAN R362ConfigureFsClocks (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  UINT32 Pllu, PlluOutA, FsSrc;
  BOOLEAN Ok;

  Pllu = Car[0x0C0 / 4];
  PlluOutA = Car[0x0C4 / 4];
  FsSrc = Car[0x608 / 4];
  Print (L"R362 CLK before: PLLU_BASE=%08x (EN=%u LOCK=%u USB480=%u USB60=%u USB48=%u) PLLU_OUTA=%08x FALCON_SRC=%08x FS_SRC=%08x SS_SRC=%08x\r\n",
         Pllu, (Pllu >> 30) & 1U, (Pllu >> 27) & 1U, (Pllu >> 22) & 1U, (Pllu >> 23) & 1U,
         (Pllu >> 25) & 1U, PlluOutA, Car[0x604 / 4], FsSrc, Car[0x610 / 4]);

  if ((Pllu & (1U << 25)) == 0) {
    Car[0x0C0 / 4] = Pllu | (1U << 25);
    gBS->Stall (10);
  }
  if ((PlluOutA & 1U) == 0) {
    Car[0x0C4 / 4] = PlluOutA | 1U;
    gBS->Stall (2);
  }
  if ((FsSrc >> 29) != 2U) {
    Car[0x608 / 4] = (FsSrc & 0x1FFFFFFFU) | (2U << 29);
    gBS->Stall (2);
  }

  Pllu = Car[0x0C0 / 4];
  PlluOutA = Car[0x0C4 / 4];
  FsSrc = Car[0x608 / 4];
  Ok = ((Pllu & (1U << 25)) != 0) && ((PlluOutA & 1U) != 0) && ((FsSrc >> 29) == 2U) &&
       ((Pllu & ((1U << 30) | (1U << 27))) == ((1U << 30) | (1U << 27)));
  Print (L"R362 CLK after:  PLLU_BASE=%08x (EN=%u LOCK=%u USB48=%u) PLLU_OUTA=%08x FS_SRC=%08x (src=%u) %s\r\n",
         Pllu, (Pllu >> 30) & 1U, (Pllu >> 27) & 1U, (Pllu >> 25) & 1U, PlluOutA,
         FsSrc, FsSrc >> 29, Ok ? L"PASS" : L"FAIL");
  return Ok;
}

// R362: register the halted, firmware-running XUSB host as a non-discoverable
// xHCI and connect the stock driver stack recursively. On success the
// controller is owned by XhciDxe and must not be restored.
STATIC BOOLEAN R362ProductionAttach (volatile UINT32 *Hcd, UINT32 OpBase,
                                     BOOLEAN *ControllerSafeToRestore) {
  EFI_STATUS Status;
  EFI_HANDLE Controller = NULL;
  EFI_USB2_HC_PROTOCOL *Usb2Hc = NULL;

  *ControllerSafeToRestore = TRUE;
  mR300Gate = R300_GATE_REGISTRATION;
  mR300RegistrationAttempted = TRUE;
  Print (L"\r\nR362 PRODUCTION ATTACH: USBCMD=%08x USBSTS=%08x\r\n",
         Hcd[OpBase + 0x00 / 4], Hcd[OpBase + 0x04 / 4]);
  if (Hcd[0] != 0x01000020U ||
      (Hcd[OpBase + 0x00 / 4] & 1U) != 0 ||
      (Hcd[OpBase + 0x04 / 4] & 1U) == 0 ||
      (Hcd[OpBase + 0x04 / 4] & ((1U << 2) | (1U << 11) | (1U << 12))) != 0) {
    Print (L"R362 STOP: halted xHCI baseline not met; XUSB will be restored.\r\n");
    return FALSE;
  }
  if (!R362ConfigureFsClocks ()) {
    Print (L"R362 WARNING: FS clock readback mismatch; continuing (HS does not need it).\r\n");
  }
  if (!R362ApplyHostPadctl ()) {
    Print (L"R362 STOP: PADCTL host setup readback failed; XUSB will be restored.\r\n");
    return FALSE;
  }
  if (!R362ConfigureIpfsFpci ()) {
    // Not fatal for UEFI (polling); report it and continue so the UEFI USB
    // result stays comparable with R359.
    Print (L"R362 WARNING: IPFS/FPCI readback mismatch; Windows interrupts may still fail.\r\n");
  }

  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL, &Controller, 1,
             (UINTN)XUSB_HCD_BASE, (UINTN)0x10000);
  if (EFI_ERROR (Status) || Controller == NULL) {
    Print (L"R362 STOP: XUSB non-discoverable registration status=%r; XUSB will be restored.\r\n",
           Status);
    return FALSE;
  }
  mR300RegistrationPassed = TRUE;
  // From here on the controller belongs to the driver stack.
  *ControllerSafeToRestore = FALSE;

  Print (L"R362 ConnectController (recursive): ND -> XhciDxe -> UsbBus -> UsbMassStorage...\r\n");
  Status = gBS->ConnectController (Controller, NULL, NULL, TRUE);
  gBS->HandleProtocol (Controller, &gEfiUsb2HcProtocolGuid, (VOID **)&Usb2Hc);
  Print (L"R362 ConnectController status=%r USB2_HC=%s USBCMD=%08x USBSTS=%08x P5=%08x\r\n",
         Status, Usb2Hc != NULL ? L"INSTALLED" : L"MISSING",
         Hcd[OpBase + 0x00 / 4], Hcd[OpBase + 0x04 / 4],
         Hcd[OpBase + (0x400 + 0x10 * 4) / 4]);
  mR362Attached = (Usb2Hc != NULL);
  if (mR362Attached) {
    mR300Gate = R300_GATE_DONE;
    mR362StatusRound = 0;
    if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                      R362UsbStatus, NULL, &mR362StatusEvent))) {
      gBS->SetTimer (mR362StatusEvent, TimerPeriodic, 5ULL * 1000ULL * 1000ULL * 10ULL);
    }
  }
  return mR362Attached;
}

STATIC VOID RunR242XhciCapabilityAudit (BOOLEAN TransferOnly, BOOLEAN StartFalcon,
                                        BOOLEAN RunStop, BOOLEAN UefiBinding) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Pmc = (volatile UINT32 *)PMC_BASE;
  volatile UINT32 *Ipfs = (volatile UINT32 *)XUSB_IPFS_BASE;
  volatile UINT32 *Fpci = (volatile UINT32 *)XUSB_FPCI_BASE;
  volatile UINT32 *Hcd = (volatile UINT32 *)XUSB_HCD_BASE;
  volatile UINT16 *FpciCommand = (volatile UINT16 *)(XUSB_FPCI_BASE + 0x004);
  CONST UINT32 HostBit = 1U << 25;
  CONST UINT32 SsBit = 1U << 28;
  CONST UINT32 XusbGateBit = 1U << 15;
  CONST UINT32 DomainMask = (1U << 20) | (1U << 21) | (1U << 22);
  UINT32 OldHostSource = Car[0x600 / 4];
  UINT32 OldSsSource = Car[0x610 / 4];
  UINT32 OldClockU = Car[0x018 / 4];
  UINT32 OldClockW = Car[0x364 / 4];
  UINT32 OldResetU = Car[0x00C / 4];
  UINT32 OldResetW = Car[0x35C / 4];
  UINT32 OldPower = Pmc[0x038 / 4];
  UINT32 OldClamp = Pmc[0x02C / 4];
  UINT32 OldIpfsConfig = 0;
  UINT32 OldBar0 = 0, TestBar0 = 0;
  UINT32 OldCsbRange = 0, CsbValue1 = 0xFFFFFFFFU, CsbValue2 = 0xFFFFFFFFU;
  UINT32 Apmap1 = 0xFFFFFFFFU, Apmap2 = 0xFFFFFFFFU;
  UINT16 OldCommand = 0, TestCommand = 0;
  BOOLEAN IpfsRead = FALSE, IpfsChanged = FALSE;
  BOOLEAN Bar0Changed = FALSE, CommandChanged = FALSE;
  BOOLEAN CsbRangeRead = FALSE, CsbRangeChanged = FALSE, CsbReadsOk = FALSE;
  BOOLEAN AOn = FALSE, COn = FALSE, SsClockOn = FALSE, HostClockOn = FALSE;
  BOOLEAN SsResetReleased = FALSE, HostResetReleased = FALSE;
  BOOLEAN RestoreOk = TRUE, FpciMatch = FALSE, HcdMatch = FALSE;
  BOOLEAN RunStopSafeToRestore = TRUE;
  BOOLEAN BindingSafeToRestore = TRUE;
  BOOLEAN TransferOk = !TransferOnly, FalconOk = !StartFalcon;
  UINT32 FpciId = 0xFFFFFFFFU, HcdCap = 0xFFFFFFFFU;
  UINT32 HcdRegs[6] = { 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
                        0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU };
  CONST UINTN HcdOffsets[6] = { 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18 };
  UINTN Index;
  BOOLEAN CapReadsOk = TRUE;

  mR300Gate = R300_GATE_R243_BASELINE;
  Print (L"R243: xHCI capability + CSB-window read preflight; no firmware/USB.\r\n");
  Print (L"R243 baseline HOST_SRC=%08x SS_SRC=%08x CLK_U=%08x CLK_W=%08x\r\n",
         OldHostSource, OldSsSource, OldClockU, OldClockW);
  Print (L"R243 baseline RST_U=%08x RST_W=%08x PMC_PWRGATE=%08x CLAMP=%08x\r\n",
         OldResetU, OldResetW, OldPower, OldClamp);
  if ((OldPower & DomainMask) != 0 ||
      (OldClockU & HostBit) != 0 || (OldClockW & (SsBit | XusbGateBit)) != 0 ||
      (OldResetU & HostBit) == 0 || (OldResetW & SsBit) == 0 ||
      (Car[0x0C0 / 4] & ((1U << 30) | (1U << 27))) != ((1U << 30) | (1U << 27))) {
    Print (L"R243 STOP: baseline/PLLU mismatch; no XUSB write made.\r\n");
    return;
  }

  mR300Gate = R300_GATE_CLOCK_POWER;
  Print (L"R243 step 1: set HOST_SRC and SS_SRC to Linux references... ");
  Car[0x600 / 4] = 0x20000006U;
  Car[0x610 / 4] = 0x64000006U;
  gBS->Stall (10);
  if (Car[0x600 / 4] != 0x20000006U || Car[0x610 / 4] != 0x64000006U) {
    Print (L"FAIL\r\n");
    goto Restore;
  }
  Print (L"PASS\r\n");

  Print (L"R243 step 2: power XUSBA prerequisite domain... ");
  AOn = TRUE;
  if (!SetPowerPartition (20, TRUE)) { Print (L"FAIL/TIMEOUT\r\n"); goto Restore; }
  Print (L"PASS\r\n");
  gBS->Stall (500000);
  Print (L"R243 step 3: enable XUSB_GATE and SS clocks... ");
  SsClockOn = TRUE;
  Car[0x448 / 4] = XusbGateBit | SsBit;
  gBS->Stall (500);
  if ((Car[0x364 / 4] & (XusbGateBit | SsBit)) != (XusbGateBit | SsBit)) {
    Print (L"FAIL\r\n"); goto Restore;
  }
  Print (L"PASS\r\n");
  Print (L"R243 step 4: unclamp XUSBA and release SS reset... ");
  if (!RemovePartitionClamp (20)) { Print (L"CLAMP FAIL/TIMEOUT\r\n"); goto Restore; }
  SsResetReleased = TRUE;
  Car[0x43C / 4] = SsBit;
  gBS->Stall (500);
  if ((Car[0x35C / 4] & SsBit) != 0) { Print (L"RESET FAIL\r\n"); goto Restore; }
  Print (L"PASS\r\n");

  Print (L"R243 step 5: power XUSBC host domain... ");
  COn = TRUE;
  if (!SetPowerPartition (22, TRUE)) { Print (L"FAIL/TIMEOUT\r\n"); goto Restore; }
  Print (L"PASS\r\n");
  gBS->Stall (500000);
  Print (L"R243 step 6: enable HOST clock... ");
  HostClockOn = TRUE;
  Car[0x330 / 4] = HostBit;
  gBS->Stall (500);
  if ((Car[0x018 / 4] & HostBit) == 0) { Print (L"FAIL\r\n"); goto Restore; }
  Print (L"PASS\r\n");
  Print (L"R243 step 7: unclamp XUSBC and release HOST reset... ");
  if (!RemovePartitionClamp (22)) { Print (L"CLAMP FAIL/TIMEOUT\r\n"); goto Restore; }
  HostResetReleased = TRUE;
  Car[0x314 / 4] = HostBit;
  gBS->Stall (500);
  if ((Car[0x00C / 4] & HostBit) != 0) { Print (L"RESET FAIL\r\n"); goto Restore; }
  Print (L"PASS\r\n");

  mR300Gate = R300_GATE_FPCI_MAP;
  Print (L"R243 step 8: read IPFS XUSB_HOST_CONFIGURATION_0 only...\r\n");
  OldIpfsConfig = Ipfs[0x180 / 4];
  IpfsRead = TRUE;
  Print (L"R243 IPFS_CONFIG=%08x EN_FPCI=%u\r\n", OldIpfsConfig, OldIpfsConfig & 1U);
  Print (L"R243 step 9: set IPFS_EN_FPCI bit 0 and verify...\r\n");
  Ipfs[0x180 / 4] = OldIpfsConfig | 1U;
  IpfsChanged = TRUE;
  gBS->Stall (10);
  if (Ipfs[0x180 / 4] != (OldIpfsConfig | 1U)) {
    Print (L"R243 STOP: IPFS_EN_FPCI readback mismatch.\r\n");
    goto Restore;
  }
  Print (L"R243 IPFS_EN_FPCI: PASS.\r\n");
  Print (L"R243 step 10: read FPCI ID once; if frozen, photo then hold POWER 12-15s.\r\n");
  FpciId = Fpci[0];
  FpciMatch = (FpciId == 0x0FAC10DEU);
  Print (L"R243 FPCI_ID=%08x expected=0FAC10DE %s\r\n", FpciId,
         FpciMatch ? L"MATCH" : L"DIFF");
  if (!FpciMatch) goto Restore;

  // Linux maps the HCD aperture through FPCI CFG4 before xHCI registers are
  // accessed. Keep CFG1's upper status half untouched: it may contain W1C bits.
  OldBar0 = Fpci[0x010 / 4];
  TestBar0 = (OldBar0 & ~0xFFFF8000U) | (XUSB_HCD_BASE & 0xFFFF8000U);
  Print (L"R243 step 11: program FPCI BAR0 CFG4=%08x -> %08x (HCD=70090000)...\r\n",
         OldBar0, TestBar0);
  Fpci[0x010 / 4] = TestBar0;
  Bar0Changed = TRUE;
  gBS->Stall (150);
  if (Fpci[0x010 / 4] != TestBar0) {
    Print (L"R243 STOP: BAR0 readback=%08x; HCD not read.\r\n", Fpci[0x010 / 4]);
    goto Restore;
  }
  Print (L"R243 BAR0 readback: PASS\r\n");

  OldCommand = *FpciCommand;
  TestCommand = (UINT16)(OldCommand | (1U << 1)); // PCI MEMORY_SPACE only.
  Print (L"R243 step 12: enable FPCI MEM_SPACE only CMD=%04x -> %04x; BUS_MASTER stays off...\r\n",
         OldCommand, TestCommand);
  *FpciCommand = TestCommand;
  CommandChanged = TRUE;
  gBS->Stall (10);
  if (*FpciCommand != TestCommand) {
    Print (L"R243 STOP: command readback=%04x; HCD not read.\r\n", *FpciCommand);
    goto Restore;
  }
  Print (L"R243 MEM_SPACE readback: PASS\r\n");

  Print (L"R243 step 13: read HC_CAPBASE; require v1.0/CAPLENGTH=0x20 before continuing.\r\n");
  HcdCap = Hcd[0];
  HcdMatch = (HcdCap == 0x01000020U);
  Print (L"R243 HC_CAPBASE=%08x expected=01000020 %s\r\n", HcdCap,
         HcdMatch ? L"MATCH" : L"DIFF");
  if (!HcdMatch) goto Restore;

  mR300Gate = R300_GATE_CSB;
  Print (L"R243 step 14: read capability offsets 04,08,0C,10,14,18 only; freeze point shown before each read.\r\n");
  for (Index = 0; Index < 6; ++Index) {
    Print (L"R243 read HCD+%02x (read-only)...\r\n", HcdOffsets[Index]);
    HcdRegs[Index] = Hcd[HcdOffsets[Index] / sizeof (UINT32)];
    Print (L"R243 HCD+%02x=%08x\r\n", HcdOffsets[Index], HcdRegs[Index]);
    if (HcdRegs[Index] == 0xFFFFFFFFU) CapReadsOk = FALSE;
  }
  Print (L"R243 HCSPARAMS1=%08x HCSPARAMS2=%08x HCSPARAMS3=%08x\r\n",
         HcdRegs[0], HcdRegs[1], HcdRegs[2]);
  Print (L"R243 HCCPARAMS1=%08x DBOFF=%08x RTSOFF=%08x\r\n",
         HcdRegs[3], HcdRegs[4], HcdRegs[5]);
  Print (L"R243 decode: slots=%u interrupters=%u ports=%u DBOFF_ALIGN=%s RTSOFF_ALIGN=%s\r\n",
         HcdRegs[0] & 0xFFU, (HcdRegs[0] >> 8) & 0x7FFU,
         (HcdRegs[0] >> 24) & 0x7FU,
         ((HcdRegs[4] & 0x3U) == 0) ? L"OK" : L"CHECK",
         ((HcdRegs[5] & 0x1FU) == 0) ? L"OK" : L"CHECK");

  // The Linux Tegra driver selects CSB page (addr >> 9) via FPCI+0x41c,
  // then accesses FPCI+0x800+(addr & 0x1ff). Test only stable read targets.
  // CSBRANGE is restored before any exit; no CSB data register is written.
  OldCsbRange = Fpci[0x41C / 4];
  if (OldCsbRange == 0xFFFFFFFFU) {
    Print (L"R243 STOP: CSBRANGE baseline unreadable; no selector write made.\r\n");
    goto Restore;
  }
  CsbRangeRead = TRUE;
  Print (L"R243 step 15: CSB window preflight; only CSBRANGE selector writes.\r\n");
  Print (L"R243 CSBRANGE baseline=%08x; ILOAD_BASE_LO page=0000080d ofs=00000004\r\n",
         OldCsbRange);
  Fpci[0x41C / 4] = 0x0000080DU;
  CsbRangeChanged = TRUE;
  gBS->Stall (10);
  if (Fpci[0x41C / 4] != 0x0000080DU) {
    Print (L"R243 STOP: CSBRANGE selector readback=%08x; no CSB data read.\r\n",
           Fpci[0x41C / 4]);
    goto Restore;
  }
  Print (L"R243 read ILOAD_BASE_LO (read-only)...\r\n");
  CsbValue1 = Fpci[(0x800 + 0x004) / 4];
  Print (L"R243 ILOAD_BASE_LO first=%08x\r\n", CsbValue1);
  Print (L"R243 read ILOAD_BASE_LO again (read-only)...\r\n");
  CsbValue2 = Fpci[(0x800 + 0x004) / 4];
  Print (L"R243 ILOAD_BASE_LO second=%08x expected=00000000\r\n", CsbValue2);
  if (CsbValue1 != 0 || CsbValue2 != 0 || CsbValue1 != CsbValue2) {
    Print (L"R243 STOP: cold-state ILOAD_BASE_LO mismatch; firmware not loaded.\r\n");
    goto Restore;
  }

  Print (L"R243 CSBRANGE select APMAP page=0000080c ofs=0000001c...\r\n");
  Fpci[0x41C / 4] = 0x0000080CU;
  gBS->Stall (10);
  if (Fpci[0x41C / 4] != 0x0000080CU) {
    Print (L"R243 STOP: APMAP page selector readback=%08x.\r\n", Fpci[0x41C / 4]);
    goto Restore;
  }
  Print (L"R243 read APMAP (read-only)...\r\n");
  Apmap1 = Fpci[(0x800 + 0x01C) / 4];
  Print (L"R243 APMAP first=%08x\r\n", Apmap1);
  Print (L"R243 read APMAP again (read-only)...\r\n");
  Apmap2 = Fpci[(0x800 + 0x01C) / 4];
  Print (L"R243 APMAP second=%08x\r\n", Apmap2);
  CsbReadsOk = (Apmap1 != 0xFFFFFFFFU && Apmap1 == Apmap2);
  mR300CsbAttempted = TRUE;
  mR300CsbPassed = CsbReadsOk;
  Print (CsbReadsOk ? L"R243 CSB window read-only preflight: PASS\r\n" :
                      L"R243 STOP: APMAP read invalid/unstable; no firmware load.\r\n");
  if (CsbReadsOk && TransferOnly) {
    mR300Gate = R300_GATE_FIRMWARE;
    mR300TransferAttempted = TRUE;
    TransferOk = RunR246FirmwareTransfer (Fpci, (UINT8 *)(UINTN)mFirmwareAddress,
                                         mFirmwareSize, mFirmwareAddress, StartFalcon);
    mR300TransferPassed = TransferOk;
    if (TransferOk && StartFalcon) {
      mR300Gate = R300_GATE_FALCON;
      mR300FalconAttempted = TRUE;
      FalconOk = RunR249FalconStart (Fpci, Hcd, ReadLe32 ((UINT8 *)(UINTN)mFirmwareAddress + 8));
      mR300FalconPassed = FalconOk;
      if (FalconOk) {
        mR300Gate = R300_GATE_OPERATIONAL;
        mR300OperationalAttempted = TRUE;
        FalconOk = RunR249XhciOperationalAudit (Hcd, HcdRegs[0]);
        mR300OperationalPassed = FalconOk;
        if (FalconOk && RunStop) {
          mR300Gate = R300_GATE_RUNSTOP;
          mR300RunStopAttempted = TRUE;
          FalconOk = RunR260XhciRunPortReadStop (Hcd, HcdRegs[0],
                                                 &RunStopSafeToRestore);
          mR300RunStopPassed = FalconOk;
        }
        if (FalconOk && UefiBinding && mR362Production) {
          // R362: hand the running XUSB to the stock xHCI/USB stack and keep
          // it. Clocks, power, IPFS, FPCI BAR/command and firmware stay as
          // programmed above; the R243 restore below is skipped once attached.
          FalconOk = R362ProductionAttach (Hcd,
                                           Hcd[0] & 0xFFU ?
                                           (Hcd[0] & 0xFFU) / 4U : 0,
                                           &BindingSafeToRestore);
          if (!BindingSafeToRestore) {
            Print (L"R362 XUSB left running for XhciDxe; R243 restore skipped.\r\n");
            return;
          }
        } else if (FalconOk && UefiBinding) {
          FalconOk = RunR309XhciStartStopProbe (Hcd,
                                               Hcd[0] & 0xFFU ?
                                               (Hcd[0] & 0xFFU) / 4U : 0,
                                               &BindingSafeToRestore);
          if (!BindingSafeToRestore) RunStopSafeToRestore = FALSE;
        }
      }
    }
  }

Restore:
  if (RunStop && !RunStopSafeToRestore) {
    Print (L"R260 SAFETY STOP: xHCI halt is unconfirmed; skip BAR/IPFS/clock/power restore.\r\n");
    Print (L"Hold POWER for 12-15 seconds; do not continue running the console.\r\n");
    return;
  }
  if (CsbRangeChanged && CsbRangeRead) {
    Print (L"R243 restore: CSBRANGE=%08x... ", OldCsbRange);
    Fpci[0x41C / 4] = OldCsbRange;
    gBS->Stall (10);
    if (Fpci[0x41C / 4] != OldCsbRange) {
      Print (L"FAIL readback=%08x\r\n", Fpci[0x41C / 4]);
      RestoreOk = FALSE;
    } else {
      Print (L"PASS\r\n");
    }
  }
  if (CommandChanged) {
    Print (L"R243 restore: PCI command=%04x... ", OldCommand);
    *FpciCommand = OldCommand;
    gBS->Stall (10);
    if (*FpciCommand != OldCommand) { Print (L"FAIL readback=%04x\r\n", *FpciCommand); RestoreOk = FALSE; }
    else Print (L"PASS\r\n");
  }
  if (Bar0Changed) {
    Print (L"R243 restore: FPCI BAR0 CFG4=%08x... ", OldBar0);
    Fpci[0x010 / 4] = OldBar0;
    gBS->Stall (150);
    if (Fpci[0x010 / 4] != OldBar0) { Print (L"FAIL readback=%08x\r\n", Fpci[0x010 / 4]); RestoreOk = FALSE; }
    else Print (L"PASS\r\n");
  }
  if (IpfsChanged && IpfsRead) {
    Print (L"R243 restore: IPFS configuration... ");
    Ipfs[0x180 / 4] = OldIpfsConfig;
    gBS->Stall (10);
    if (Ipfs[0x180 / 4] != OldIpfsConfig) { Print (L"FAIL\r\n"); RestoreOk = FALSE; }
    else Print (L"PASS\r\n");
  }
  if (HostResetReleased) Car[0x310 / 4] = HostBit;
  if (SsResetReleased) Car[0x438 / 4] = SsBit;
  gBS->Stall (10);
  if (HostClockOn) Car[0x334 / 4] = HostBit;
  if (SsClockOn) Car[0x44C / 4] = XusbGateBit | SsBit;
  gBS->Stall (10);
  if (COn && !SetPowerPartition (22, FALSE)) RestoreOk = FALSE;
  if (AOn && !SetPowerPartition (20, FALSE)) RestoreOk = FALSE;
  Car[0x600 / 4] = OldHostSource;
  Car[0x610 / 4] = OldSsSource;
  gBS->Stall (10);
  if (Pmc[0x038 / 4] != OldPower || Pmc[0x02C / 4] != OldClamp ||
      Car[0x018 / 4] != OldClockU || Car[0x364 / 4] != OldClockW ||
      Car[0x00C / 4] != OldResetU || Car[0x35C / 4] != OldResetW ||
      Car[0x600 / 4] != OldHostSource || Car[0x610 / 4] != OldSsSource) {
    RestoreOk = FALSE;
  }
  Print (RestoreOk ? L"R243 restore baseline: PASS\r\n" :
                     L"R243 restore baseline: FAIL; hold POWER for 12-15 seconds.\r\n");
  PrintR260EndSummary ();
  if (StartFalcon) {
    if (FpciMatch && HcdMatch && CapReadsOk && CsbReadsOk && RestoreOk && TransferOk && FalconOk) {
      Print (RunStop ?
             L"R260 boundary: xHCI RUN observed then halted; baseline restored; no USB command/transfer.\r\n" :
             L"R249 boundary: Falcon started; CNR cleared; operational/PORTSC reads only; no xHCI command or USB transfer.\r\n");
    } else {
      Print (RunStop ?
             L"R260 boundary: STOP; USB command and transfer not attempted.\r\n" :
             L"R249 boundary: STOP; no xHCI command or USB transfer attempted.\r\n");
    }
  } else if (TransferOnly) {
    Print ((FpciMatch && HcdMatch && CapReadsOk && CsbReadsOk && RestoreOk && TransferOk) ?
           L"R246 boundary: firmware copied to L2IMEM; no Falcon start or USB transfer.\r\n" :
           L"R246 boundary: STOP; Falcon start and USB transfer not attempted.\r\n");
  } else {
    Print ((FpciMatch && HcdMatch && CapReadsOk && CsbReadsOk && RestoreOk) ?
           L"R243 boundary: CSB reads verified; no firmware, Falcon start, operational registers or USB transfer.\r\n" :
           L"R243 boundary: STOP; no firmware load or Falcon start.\r\n");
  }
}

// R245 validates the firmware image and computes the Linux Tegra load plan in
// host RAM only, then repeats the R243 read-only controller/CSB preflight.
// No firmware bytes are DMA-transferred to XUSB and Falcon is never started.
STATIC VOID RunR244FirmwareLoadPlanAudit (UINT32 Stage) {
  UINT8 *Firmware;
  UINT32 CodeTag, CodeSize, ImageLength;
  UINT32 CodeTagBlocks, CodeSizeBlocks, CodeBlocks;
  UINT32 L2ImemSize, ImfillRange;
  EFI_PHYSICAL_ADDRESS LoadAddress;
  BOOLEAN PlanOk;

  ZeroMem (&mR260EarlyPadctl, sizeof (mR260EarlyPadctl));
  ZeroMem (&mR260RunPadctl, sizeof (mR260RunPadctl));
  ZeroMem (mR260Usb2PortSc, sizeof (mR260Usb2PortSc));
  mR260Usb2PortValidMask = 0;
  CaptureR260PadctlGateState (&mR260EarlyPadctl);

  mR300Gate = R300_GATE_FW_FILE;
  Print ((Stage >= 4) ? L"R300: bounded firmware/Falcon gates + registration-only diagnostic.\r\n" :
         (Stage >= 3) ? L"R260: firmware transfer + Falcon start; read PORTSC while xHCI RUN; no USB command/transfer.\r\n" :
         (Stage >= 2) ? L"R249: firmware transfer + Falcon start; read operational/PORTSC only; no xHCI commands.\r\n" :
         (Stage == 1) ? L"R246: firmware transfer test; stop before Falcon start.\r\n" :
                        L"R245: Linux firmware-load plan in system RAM; no XUSB transfer or Falcon start.\r\n");
  if (!AuditXusbFirmwareFile () || mFirmwarePages == 0) {
    Print (L"R244 STOP: verified firmware DMA buffer unavailable; no XUSB change.\r\n");
    return;
  }

  Firmware = (UINT8 *)(UINTN)mFirmwareAddress;
  CodeTag = ReadLe32 (&Firmware[8]);
  CodeSize = ReadLe32 (&Firmware[12]);
  ImageLength = ReadLe32 (&Firmware[100]);
  CodeTagBlocks = (CodeTag + 255U) / 256U;
  CodeSizeBlocks = (CodeSize + 255U) / 256U;
  CodeBlocks = CodeTagBlocks + CodeSizeBlocks;
  L2ImemSize = ((CodeTagBlocks & 0x3FFU) << 8) |
               ((CodeSizeBlocks & 0xFFU) << 24);
  ImfillRange = (CodeTagBlocks & 0xFFFFU) |
                ((CodeBlocks & 0xFFFFU) << 16);
  LoadAddress = mFirmwareAddress + 256U;
  PlanOk = (mFirmwareAddress != 0 &&
            mFirmwareAddress + mFirmwareSize <= 0x100000000ULL &&
            mFirmwareSize == 124416U && ImageLength == mFirmwareSize &&
            CodeTag != 0 && CodeSize != 0 &&
            (UINT64)CodeTag + (UINT64)CodeSize <= ImageLength &&
            (LoadAddress & 0xFFU) == 0 && (CodeTag & 0xFFU) == 0 &&
            LoadAddress < 0x100000000ULL);

  Print ((Stage >= 3) ? L"R260 load plan: fw_size=%u ILOAD_ATTR=%u\r\n" :
         (Stage >= 2) ? L"R249 load plan: fw_size=%u ILOAD_ATTR=%u\r\n" :
         (Stage == 1) ? L"R246 load plan: fw_size=%u ILOAD_ATTR=%u\r\n" :
                        L"R245 load plan: fw_size=%u ILOAD_ATTR=%u\r\n",
         (UINT32)mFirmwareSize, (UINT32)mFirmwareSize);
  Print ((Stage >= 3) ? L"R260 load plan: DMA=%016lx + header=256 -> ILOAD_BASE_HI=%08x LO=%08x\r\n" :
         (Stage >= 2) ? L"R249 load plan: DMA=%016lx + header=256 -> ILOAD_BASE_HI=%08x LO=%08x\r\n" :
         (Stage == 1) ? L"R246 load plan: DMA=%016lx + header=256 -> ILOAD_BASE_HI=%08x LO=%08x\r\n" :
                        L"R245 load plan: DMA=%016lx + header=256 -> ILOAD_BASE_HI=%08x LO=%08x\r\n",
         mFirmwareAddress, (UINT32)(LoadAddress >> 32), (UINT32)LoadAddress);
  Print ((Stage >= 3) ? L"R260 load plan: APMAP_BOOTPATH=%08x boot_tag=%08x boot_size=%u\r\n" :
         (Stage >= 2) ? L"R249 load plan: APMAP_BOOTPATH=%08x boot_tag=%08x boot_size=%u\r\n" :
         (Stage == 1) ? L"R246 load plan: APMAP_BOOTPATH=%08x boot_tag=%08x boot_size=%u\r\n" :
                        L"R245 load plan: APMAP_BOOTPATH=%08x boot_tag=%08x boot_size=%u\r\n",
         0x80000000U, CodeTag, CodeSize);
  Print ((Stage >= 3) ? L"R260 load plan: tag_blocks=%u size_blocks=%u L2IMEM_SIZE=%08x IMFILLCTL=%u IMFILLRNG1=%08x\r\n" :
         (Stage >= 2) ? L"R249 load plan: tag_blocks=%u size_blocks=%u L2IMEM_SIZE=%08x IMFILLCTL=%u IMFILLRNG1=%08x\r\n" :
         (Stage == 1) ? L"R246 load plan: tag_blocks=%u size_blocks=%u L2IMEM_SIZE=%08x IMFILLCTL=%u IMFILLRNG1=%08x\r\n" :
                        L"R245 load plan: tag_blocks=%u size_blocks=%u L2IMEM_SIZE=%08x IMFILLCTL=%u IMFILLRNG1=%08x\r\n",
         CodeTagBlocks, CodeSizeBlocks, L2ImemSize, CodeSizeBlocks, ImfillRange);
  Print ((Stage >= 3) ?
         (PlanOk ? L"R260 firmware load-plan audit: PASS\r\n" : L"R260 STOP: firmware load-plan validation failed.\r\n") :
         (Stage >= 2) ?
         (PlanOk ? L"R249 firmware load-plan audit: PASS\r\n" : L"R249 STOP: firmware load-plan validation failed.\r\n") :
         (Stage == 1) ?
         (PlanOk ? L"R246 firmware load-plan audit: PASS\r\n" : L"R246 STOP: firmware load-plan validation failed.\r\n") :
         (PlanOk ? L"R245 firmware load-plan audit: PASS (calculation only)\r\n" : L"R245 STOP: firmware load-plan validation failed.\r\n"));
  if (!PlanOk) goto ReleaseFirmware;

  Print ((Stage >= 4) ? L"R300 next: halted baseline, temporary protocol registration/readback/cleanup; no driver connect.\r\n" :
         (Stage >= 3) ? L"R260 next: start Falcon, validate halted state, read PORTSC while RUN, then halt.\r\n" :
         (Stage >= 2) ? L"R249 next: start Falcon, then read operational/PORTSC only.\r\n" :
         (Stage == 1) ? L"R246 next: transfer bootcode into L2IMEM only; do not start Falcon.\r\n" :
                        L"R245 next: repeat R243 read-only xHCI/CSB preflight.\r\n");
  RunR242XhciCapabilityAudit (Stage >= 1, Stage >= 2, Stage >= 3, Stage >= 4);

ReleaseFirmware:
  if (mR362Attached && mFirmwarePages != 0) {
    Print (L"R362 firmware buffer kept (reserved memory) at %016lx pages=%u for the running Falcon.\r\n",
           mFirmwareAddress, (UINT32)mFirmwarePages);
  } else if (mFirmwarePages != 0) {
    if (EFI_ERROR (gBS->FreePages (mFirmwareAddress, mFirmwarePages))) {
      PlanOk = FALSE;
      Print ((Stage >= 3) ? L"R260 firmware buffer release: FAIL.\r\n" :
             (Stage >= 2) ? L"R249 firmware buffer release: FAIL.\r\n" :
             (Stage == 1) ? L"R246 firmware buffer release: FAIL.\r\n" : L"R245 firmware buffer release: FAIL.\r\n");
    } else {
      Print ((Stage >= 3) ? L"R260 firmware buffer release: PASS.\r\n" :
             (Stage >= 2) ? L"R249 firmware buffer release: PASS.\r\n" :
             (Stage == 1) ? L"R246 firmware buffer release: PASS.\r\n" : L"R245 firmware buffer release: PASS.\r\n");
    }
    mFirmwareAddress = 0;
    mFirmwarePages = 0;
    mFirmwareSize = 0;
  }
  Print ((Stage >= 3) ?
         L"R260 boundary: bounded Run/Stop attempted; see status above; no USB command, enumeration, or transfer.\r\n" :
         (Stage >= 2) ?
         L"R249 boundary: Falcon start + operational/PORTSC read-only audit; no xHCI command, enumeration, or USB transfer.\r\n" :
         (Stage == 1) ?
         L"R246 boundary: L2IMEM transfer only; no BOOTVEC/CPUCTL start, xHCI operation, or USB transfer.\r\n" :
         L"R245 boundary: host-RAM load plan + R243 read-only checks only; no CSB data write, DMA, L2IMEM action, Falcon start, or USB transfer.\r\n");
}

STATIC BOOLEAN R288LoadI2cConfig (VOID) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINTN I;
  B[CONFIG_LOAD] = 0x25;
  for (I = 0; I < 10000; ++I) {
    if ((B[CONFIG_LOAD] & 1U) == 0) return TRUE;
    gBS->Stall (10);
  }
  return FALSE;
}

STATIC VOID R288SaveI2cController (volatile UINT32 *B) {
  mOldCnfg = B[CNFG];
  mOldIntMask = B[INT_MASK_REG];
  mOldFifo = B[FIFO_CONTROL];
  mOldIntEn = B[INT_EN_REG];
  mOldBusClear = B[BUS_CLEAR_CONFIG];
  mControllerSaved = TRUE;
}

STATIC VOID R288RestoreI2cController (VOID) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  if (!mControllerSaved) return;
  B[INT_EN_REG] = mOldIntEn;
  B[BUS_CLEAR_CONFIG] = mOldBusClear;
  B[INT_MASK_REG] = mOldIntMask;
  B[FIFO_CONTROL] = mOldFifo;
  B[CNFG] = mOldCnfg;
  R288LoadI2cConfig ();
  mControllerSaved = FALSE;
}

STATIC BOOLEAN R288BeginI2c (VOID) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINTN I;
  B[INT_MASK_REG] = 0;
  B[INT_STATUS_REG] = 0xFFFFFFFFU;
  B[FIFO_CONTROL] = B[FIFO_CONTROL] | 3U;
  for (I = 0; I < 10000; ++I) {
    if ((B[FIFO_CONTROL] & 3U) == 0) return TRUE;
    gBS->Stall (10);
  }
  return FALSE;
}

STATIC BOOLEAN R288WaitI2c (UINT32 *FinalStatus) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINTN I;
  UINT32 S = 0;
  for (I = 0; I < 10000; ++I) {
    S = B[INT_STATUS_REG];
    if ((S & (ERR_MASK | ALL_COMPLETE)) != 0) break;
    gBS->Stall (10);
  }
  *FinalStatus = S;
  return (S & ERR_MASK) == 0 && (S & ALL_COMPLETE) != 0;
}

STATIC BOOLEAN R288Read16 (UINT8 Reg, UINT16 *Value, UINT32 *Status) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  if (!R288BeginI2c ()) return FALSE;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 0;
  B[TX_FIFO] = REPEAT_START | (BM_ADDR << 1); B[TX_FIFO] = Reg;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 1;
  B[TX_FIFO] = READ_FLAG | (BM_ADDR << 1);
  if (!R288WaitI2c (Status) || (B[FIFO_STATUS] & 0xFU) == 0) return FALSE;
  *Value = (UINT16)(B[RX_FIFO] & 0xFFFFU);
  B[INT_STATUS_REG] = *Status;
  return TRUE;
}

STATIC BOOLEAN R288Write16 (UINT8 Reg, UINT16 Value, UINT32 *Status) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINT32 Payload = Reg | ((UINT32)(Value & 0xFFU) << 8) |
                   ((UINT32)(Value >> 8) << 16);
  if (!R288BeginI2c ()) return FALSE;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 2;
  B[TX_FIFO] = BM_ADDR << 1; B[TX_FIFO] = Payload;
  if (!R288WaitI2c (Status)) return FALSE;
  B[INT_STATUS_REG] = *Status;
  return TRUE;
}

STATIC BOOLEAN R288Read8 (UINT8 Address, UINT8 Reg, UINT8 *Value, UINT32 *Status) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  if (!R288BeginI2c ()) return FALSE;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 0;
  B[TX_FIFO] = REPEAT_START | (Address << 1); B[TX_FIFO] = Reg;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 0;
  B[TX_FIFO] = READ_FLAG | (Address << 1);
  if (!R288WaitI2c (Status) || (B[FIFO_STATUS] & 0xFU) == 0) return FALSE;
  *Value = (UINT8)(B[RX_FIFO] & 0xFFU);
  B[INT_STATUS_REG] = *Status;
  return TRUE;
}

STATIC BOOLEAN R288Write8 (UINT8 Address, UINT8 Reg, UINT8 Value, UINT32 *Status) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINT32 Payload = Reg | ((UINT32)Value << 8);
  if (!R288BeginI2c ()) return FALSE;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 1;
  B[TX_FIFO] = Address << 1; B[TX_FIFO] = Payload;
  if (!R288WaitI2c (Status)) return FALSE;
  B[INT_STATUS_REG] = *Status;
  return TRUE;
}

// R363: read Len (1..32) bytes starting at Reg. The Tegra I2C RX FIFO holds 8
// words, so one packet is enough for the BM92T PDO block (29 bytes).
STATIC BOOLEAN R363ReadBlock (UINT8 Address, UINT8 Reg, UINT8 *Buf, UINTN Len, UINT32 *Status) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINTN Words = (Len + 3) / 4;
  UINTN I;
  UINT32 W = 0;
  if (Len == 0 || Len > 32 || !R288BeginI2c ()) return FALSE;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = 0;
  B[TX_FIFO] = REPEAT_START | (Address << 1); B[TX_FIFO] = Reg;
  B[TX_FIFO] = HDR0; B[TX_FIFO] = (UINT32)(Len - 1);
  B[TX_FIFO] = READ_FLAG | (Address << 1);
  if (!R288WaitI2c (Status) || (B[FIFO_STATUS] & 0xFU) < Words) return FALSE;
  for (I = 0; I < Len; ++I) {
    if ((I % 4) == 0) W = B[RX_FIFO];
    Buf[I] = (UINT8)(W >> (8 * (I % 4)));
  }
  B[INT_STATUS_REG] = *Status;
  return TRUE;
}

STATIC BOOLEAN R363Read16At (UINT8 Address, UINT8 Reg, UINT16 *Value, UINT32 *Status) {
  UINT8 Buf[2];
  if (!R363ReadBlock (Address, Reg, Buf, 2, Status)) return FALSE;
  *Value = (UINT16)(Buf[0] | (Buf[1] << 8));
  return TRUE;
}

STATIC UINT32 R363Le32 (CONST UINT8 *P) {
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

// Fixed-supply PDO: bits 9:0 current (10 mA), 19:10 voltage (50 mV), 31:30 type.
STATIC VOID R363PrintPdo (CONST CHAR16 *Name, UINTN Index, UINT32 Pdo) {
  if ((Pdo >> 30) == 0) {
    Print (L"R363 %s %u: %umV %umA%s raw=%08x\r\n", Name, (UINT32)Index,
           ((Pdo >> 10) & 0x3FFU) * 50U, (Pdo & 0x3FFU) * 10U,
           (Pdo & BM_PDO_DRD) ? L" DRD" : L"", Pdo);
  } else {
    Print (L"R363 %s %u: non-fixed type=%u raw=%08x\r\n", Name, (UINT32)Index, Pdo >> 30, Pdo);
  }
}

// R363: one-line charger/battery summary. Assumes the I2C controller is in
// packet mode. MAX17050 current uses the Switch's 5 mOhm sense resistor
// (1 LSB = 1.5625 uV / 5 mOhm = 0.3125 mA; positive = charging).
STATIC VOID R363PrintPowerNow (CONST CHAR16 *Tag) {
  UINT32 S = 0;
  UINT8 Sys = 0, Fault = 0, In = 0, Pwr = 0;
  UINT16 St1 = 0, Soc = 0, Vcell = 0, Cur = 0;
  BOOLEAN BqOk, BmOk, FgOk;
  INT32 CurMa;
  CONST CHAR16 *ChgNames[4] = { L"not-charging", L"pre-charge", L"fast-charge", L"done" };
  CONST CHAR16 *VbusNames[4] = { L"none", L"usb-host", L"adapter", L"otg" };

  BqOk = R288Read8 (BQ_ADDR, BQ_SYS_STAT, &Sys, &S) && R288Read8 (BQ_ADDR, BQ_FAULT, &Fault, &S) &&
         R288Read8 (BQ_ADDR, BQ_INPUT_SRC, &In, &S) && R288Read8 (BQ_ADDR, BQ_PWR_ON, &Pwr, &S);
  BmOk = R288Read16 (BM_STATUS1, &St1, &S);
  FgOk = R363Read16At (MAX17050_ADDR, MAX17050_REPSOC, &Soc, &S) &&
         R363Read16At (MAX17050_ADDR, MAX17050_VCELL, &Vcell, &S) &&
         R363Read16At (MAX17050_ADDR, MAX17050_CURRENT, &Cur, &S);
  CurMa = ((INT32)(INT16)Cur * 5) / 16;
  if (BqOk) {
    Print (L"R363 %s BQ: %s vbus=%s REG00=%02x REG01=%02x SYS=%02x FAULT=%02x\r\n", Tag,
           ChgNames[(Sys >> 4) & 3U], VbusNames[(Sys >> 6) & 3U], In, Pwr, Sys, Fault);
  } else {
    Print (L"R363 %s BQ read FAIL I2C=%08x\r\n", Tag, S);
  }
  if (FgOk) {
    Print (L"R363 %s battery: %u%% %umV current=%d mA (raw %04x, + = charging) BM92T S1=%04x%s\r\n", Tag,
           (UINT32)(Soc >> 8), ((UINT32)(Vcell >> 3) * 625U) / 1000U, CurMa, Cur, St1,
           BmOk ? L"" : L"(read FAIL)");
  } else {
    Print (L"R363 %s MAX17050 read FAIL I2C=%08x\r\n", Tag, S);
  }
  {
    UINT8 Local = 0, Remote = 0;
    if (R288Read8 (TMP451_ADDR, 0x00, &Local, &S) && R288Read8 (TMP451_ADDR, 0x01, &Remote, &S))
      Print (L"R364 %s temp: SoC=%u C board=%u C (TMP451)\r\n", Tag, Remote, Local);
    else
      Print (L"R364 %s TMP451 read FAIL I2C=%08x\r\n", Tag, S);
  }
}

// R364: CPU clock helpers.
STATIC UINT64 R364Counter (VOID) {
  UINT64 V;
  __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r" (V));
  return V;
}

STATIC UINT64 R364CounterFreq (VOID) {
  UINT64 V;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (V));
  return V;
}

// Loop iterations per millisecond; only the before/after ratio matters.
STATIC UINT64 R364LoopRate (VOID) {
  volatile UINT32 Spin;
  UINT64 Start, Ticks, Freq = R364CounterFreq ();
  UINT32 I;
  Start = R364Counter ();
  for (I = 0, Spin = 0; I < 20000000U; ++I) Spin++;
  Ticks = R364Counter () - Start;
  if (Ticks == 0 || Freq == 0) return 0;
  return (20000000ULL * Freq) / (Ticks * 1000ULL);
}

STATIC UINT32 R364PllxKhz (UINT32 Base) {
  CONST UINT32 PDiv[] = { 1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 15, 16, 18, 20, 24, 30, 32 };
  UINT32 M = Base & 0xFFU, N = (Base >> 8) & 0xFFU, P = (Base >> 20) & 0x1FU;
  if (M == 0 || P >= ARRAY_SIZE (PDiv)) return 0;
  return (R364_OSC_KHZ * N) / (M * PDiv[P]);
}

STATIC BOOLEAN R364ReadCpuVout (UINT8 Reg, UINT8 *Value) {
  return i2c_recv_buf_small (Value, 1, I2C_5, MAX77621_CPU_ADDR, Reg) == 1;
}

STATIC BOOLEAN R364SetCpuVout (UINT8 Code) {
  UINT8 A = 0, B = 0;
  if (i2c_send_byte (I2C_5, MAX77621_CPU_ADDR, MAX77621_VOUT_DVC, Code) != 1 ||
      i2c_send_byte (I2C_5, MAX77621_CPU_ADDR, MAX77621_VOUT, Code) != 1) return FALSE;
  gBS->Stall (2000);
  return R364ReadCpuVout (MAX77621_VOUT, &A) && R364ReadCpuVout (MAX77621_VOUT_DVC, &B) &&
         A == Code && B == Code;
}

STATIC UINT32 R364VoutMv (UINT8 Code) {
  return (MAX77621_MIN_UV + (Code & 0x7FU) * MAX77621_STEP_UV) / 1000U;
}

// R366: fan helpers.
STATIC VOID R366GpioSet (UINT32 PortOffset, UINT32 Reg, UINT32 Bit, BOOLEAN On) {
  volatile UINT32 *G = (volatile UINT32 *)(GPIO_BASE + PortOffset + Reg);
  if (On) *G |= (1U << Bit); else *G &= ~(1U << Bit);
  (VOID)*G;
}

STATIC UINT32 R366GpioBit (UINT32 PortOffset, UINT32 Reg, UINT32 Bit) {
  return (*(volatile UINT32 *)(GPIO_BASE + PortOffset + Reg) >> Bit) & 1U;
}

// Count tachometer pulses on PS7 (2 per revolution, as Hekate).
STATIC UINT32 R366FanRpm (UINT32 Seconds) {
  UINT64 End = R364Counter () + R364CounterFreq () * Seconds;
  UINT32 Edges = 0, Last = R366GpioBit (GPIO_PS_OFFSET, GPIO_IN, 7), Now;
  while (R364Counter () < End) {
    Now = R366GpioBit (GPIO_PS_OFFSET, GPIO_IN, 7);
    if (Now && !Last) Edges++;
    Last = Now;
  }
  return Seconds ? (Edges * 60U) / (2U * Seconds) : 0;
}

STATIC VOID R366PrintFan (CONST CHAR16 *Tag, UINT32 Seconds) {
  volatile UINT32 *Pwm = (volatile UINT32 *)PWM_BASE;
  UINT32 Csr = Pwm[PWM_CSR_1 / 4];
  UINT32 Duty = (Csr & PWM_CSR_EN) ? R366_FAN_MAX - ((Csr >> 16) & 0xFFU) : 0;
  if (((Csr >> 16) & 0x100U) != 0) Duty = 0;
  Print (L"R366 %s fan: duty=%u/236 (%u%%) CSR1=%08x rpm=%u\r\n", Tag, Duty, (Duty * 100U) / R366_FAN_MAX,
         Csr, R366FanRpm (Seconds));
}

// Turn the fan on at a fixed duty. Windows has no fan/thermal driver yet, so
// this setting stays for the whole session.
STATIC VOID R366StartFan (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Pin = (volatile UINT32 *)PINMUX_BASE;
  volatile UINT32 *Pwm = (volatile UINT32 *)PWM_BASE;
  BOOLEAN ClkOn = (Car[CAR_CLK_OUT_ENB_L / 4] & CLK_L_PWM_BIT) != 0;
  BOOLEAN InReset = (Car[CAR_RST_DEVICES_L / 4] & CLK_L_PWM_BIT) != 0;

  Print (L"R366 fan before: PWM clk=%u reset=%u SRC=%08x CSR1=%08x pinmux LCD_GPIO2=%08x CAM1_PWDN=%08x SATA_LED=%08x VBUS_EN0=%08x\r\n",
         ClkOn, InReset, Car[CAR_CLK_SOURCE_PWM / 4], (ClkOn && !InReset) ? Pwm[PWM_CSR_1 / 4] : 0,
         Pin[PINMUX_LCD_GPIO2 / 4], Pin[PINMUX_CAM1_PWDN / 4], Pin[PINMUX_SATA_LED_ACTIVE / 4],
         Pin[PINMUX_USB_VBUS_EN0 / 4]);
  Print (L"R366 GPIO before: PA5 cnf/oe/out=%u/%u/%u PV4 cnf=%u PS7 cnf/oe/in=%u/%u/%u PCC4 cnf/oe/out=%u/%u/%u\r\n",
         R366GpioBit (GPIO_PA_OFFSET, GPIO_CNF, 5), R366GpioBit (GPIO_PA_OFFSET, GPIO_OE, 5),
         R366GpioBit (GPIO_PA_OFFSET, GPIO_OUT, 5), R366GpioBit (GPIO_PV_OFFSET, GPIO_CNF, 4),
         R366GpioBit (GPIO_PS_OFFSET, GPIO_CNF, 7), R366GpioBit (GPIO_PS_OFFSET, GPIO_OE, 7),
         R366GpioBit (GPIO_PS_OFFSET, GPIO_IN, 7), R366GpioBit (GPIO_PCC_OFFSET, GPIO_CNF, 4),
         R366GpioBit (GPIO_PCC_OFFSET, GPIO_OE, 4), R366GpioBit (GPIO_PCC_OFFSET, GPIO_OUT, 4));

  // PWM controller clock (Hekate _clock_pwm: CLK_M, divisor 3). Only touched
  // when it is off, so a running backlight PWM is never reset.
  if (!ClkOn || InReset) {
    Car[CAR_RST_DEV_L_SET / 4] = CLK_L_PWM_BIT;
    Car[CAR_CLK_SOURCE_PWM / 4] = (6U << 29) | 4U;
    Car[CAR_CLK_ENB_L_SET / 4] = CLK_L_PWM_BIT;
    gBS->Stall (10);
    Car[CAR_RST_DEV_L_CLR / 4] = CLK_L_PWM_BIT;
    gBS->Stall (10);
    Print (L"R366 PWM clock enabled: clk=%u reset=%u SRC=%08x\r\n",
           (Car[CAR_CLK_OUT_ENB_L / 4] & CLK_L_PWM_BIT) != 0,
           (Car[CAR_RST_DEVICES_L / 4] & CLK_L_PWM_BIT) != 0, Car[CAR_CLK_SOURCE_PWM / 4]);
  }

  // Tachometer input: PS7 as GPIO input with pull-up.
  Pin[PINMUX_CAM1_PWDN / 4] = PINMUX_TRISTATE_BIT | PINMUX_INPUT_EN_BIT | PINMUX_PULL_UP_BITS | 1U;
  R366GpioSet (GPIO_PS_OFFSET, GPIO_OE, 7, FALSE);
  R366GpioSet (GPIO_PS_OFFSET, GPIO_CNF, 7, TRUE);

  // Fan 5V supply from the battery 5V regulator (EN = PA5 high).
  Pin[PINMUX_SATA_LED_ACTIVE / 4] = 1U;
  R366GpioSet (GPIO_PA_OFFSET, GPIO_OUT, 5, TRUE);
  R366GpioSet (GPIO_PA_OFFSET, GPIO_OE, 5, TRUE);
  R366GpioSet (GPIO_PA_OFFSET, GPIO_CNF, 5, TRUE);

  // PWM1 duty, then route LCD_GPIO2 (PV4) to PWM1.
  Pwm[PWM_CSR_1 / 4] = PWM_CSR_EN | ((R366_FAN_MAX - R366_FAN_DUTY) << 16);
  R366GpioSet (GPIO_PV_OFFSET, GPIO_CNF, 4, FALSE);
  Pin[PINMUX_LCD_GPIO2 / 4] = 1U;
  gBS->Stall (1000);
  Print (L"R366 fan set: CSR1=%08x LCD_GPIO2=%08x PA5 out=%u PV4 cnf=%u\r\n",
         Pwm[PWM_CSR_1 / 4], Pin[PINMUX_LCD_GPIO2 / 4],
         R366GpioBit (GPIO_PA_OFFSET, GPIO_OUT, 5), R366GpioBit (GPIO_PV_OFFSET, GPIO_CNF, 4));
  gBS->Stall (1000000);     // let the fan spin up before measuring
  R366PrintFan (L"after start", 2);
}

// R376: backlight on PWM0 instead of a plain GPIO. coreboot drives the WLED
// PWM input LCD_BL_PWM (GPIO PV0) high as a GPIO, i.e. full brightness with no
// control. Hekate display_backlight_pwm_init(): PWM_CSR_0 = EN | duty << 16
// (duty 0..255, not inverted), pin function PWM0, PV0 as SPIO. The duty is set
// first so the switch-over keeps the panel at the same (full) brightness.
// switchfan.sys v2 then changes CSR_0 from Windows.
#define PWM_CSR_0             0x00U
#define PINMUX_LCD_BL_PWM     0x1FCU
#define R376_BACKLIGHT_DUTY   255U

STATIC VOID R376BacklightPwm (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Pin = (volatile UINT32 *)PINMUX_BASE;
  volatile UINT32 *Pwm = (volatile UINT32 *)PWM_BASE;

  Print (L"R376 backlight before: CSR0=%08x LCD_BL_PWM=%08x PV0 cnf/oe/out=%u/%u/%u\r\n",
         Pwm[PWM_CSR_0 / 4], Pin[PINMUX_LCD_BL_PWM / 4], R366GpioBit (GPIO_PV_OFFSET, GPIO_CNF, 0),
         R366GpioBit (GPIO_PV_OFFSET, GPIO_OE, 0), R366GpioBit (GPIO_PV_OFFSET, GPIO_OUT, 0));
  if ((Car[CAR_CLK_OUT_ENB_L / 4] & CLK_L_PWM_BIT) == 0 || (Car[CAR_RST_DEVICES_L / 4] & CLK_L_PWM_BIT) != 0) {
    Print (L"R376 SKIP: PWM clock is off; backlight stays on the GPIO.\r\n");
    return;
  }
  Pwm[PWM_CSR_0 / 4] = PWM_CSR_EN | (R376_BACKLIGHT_DUTY << 16);
  Pin[PINMUX_LCD_BL_PWM / 4] = (Pin[PINMUX_LCD_BL_PWM / 4] & ~3U) | 1U;   // function PWM0
  gBS->Stall (2);
  R366GpioSet (GPIO_PV_OFFSET, GPIO_CNF, 0, FALSE);                         // PV0 -> SPIO (PWM0)
  Print (L"R376 backlight after: CSR0=%08x LCD_BL_PWM=%08x PV0 cnf=%u (PWM0, duty %u/255)\r\n",
         Pwm[PWM_CSR_0 / 4], Pin[PINMUX_LCD_BL_PWM / 4], R366GpioBit (GPIO_PV_OFFSET, GPIO_CNF, 0),
         R376_BACKLIGHT_DUTY);
}

// R373: touch panel power-on, following Hekate bdk/input/touch.c
// touch_power_on() and the L4T DT (fts@49 on i2c@7000c500, IRQ PX1 low,
// reset PJ7). STM FTM4: chip ID 0x3670 at HW reg 0x0004.
#define I2C3_BASE            0x7000C500UL
#define TOUCH_ADDR           0x49
#define MAX77620_ADDR        0x3C
#define MAX77620_LDO6_CFG    0x2F
#define PINMUX_DAP4_SCLK     0x150U
#define PINMUX_GEN3_I2C_SCL  0x0CCU
#define PINMUX_GEN3_I2C_SDA  0x0D0U
#define PINMUX_CAM_I2C_SCL   0x0D4U
#define PINMUX_CAM_I2C_SDA   0x0D8U
#define PINMUX_TOUCH_INT     0x220U
#define GPIO_PJ_OFFSET       0x204U      // port J: bank 2, port 1
#define GPIO_PX_OFFSET       0x50CU      // port X: bank 5, port 3
#define CAR_RST_DEVICES_U    0x00CU
#define CAR_CLK_OUT_ENB_U    0x018U
#define CAR_CLK_SOURCE_I2C3  0x1B8U
#define CAR_RST_DEV_U_SET    0x310U
#define CAR_RST_DEV_U_CLR    0x314U
#define CAR_CLK_ENB_U_SET    0x330U
#define CLK_U_I2C3_BIT       (1U << 3)

STATIC BOOLEAN mR373TouchReady;

// Packet-mode transfer on any Tegra I2C controller: optional write of WLen
// (<= 8) bytes, then optional repeated-start read of RLen (<= 32) bytes.
STATIC BOOLEAN R373I2cXfer (volatile UINT32 *B, UINT8 Addr, CONST UINT8 *W, UINTN WLen,
                            UINT8 *R, UINTN RLen, UINT32 *Status) {
  UINTN I, Words;
  UINT32 Word = 0, S = 0;
  B[INT_STATUS_REG] = 0xFFFFFFFFU;
  B[FIFO_CONTROL] = B[FIFO_CONTROL] | 3U;
  for (I = 0; I < 10000 && (B[FIFO_CONTROL] & 3U) != 0; ++I) gBS->Stall (10);
  if (WLen != 0) {
    B[TX_FIFO] = HDR0; B[TX_FIFO] = (UINT32)(WLen - 1);
    B[TX_FIFO] = ((UINT32)Addr << 1) | (RLen != 0 ? REPEAT_START : 0);
    for (I = 0; I < WLen; I += 4) {
      Word = 0;
      for (Words = 0; Words < 4 && I + Words < WLen; ++Words) Word |= (UINT32)W[I + Words] << (8 * Words);
      B[TX_FIFO] = Word;
    }
  }
  if (RLen != 0) {
    B[TX_FIFO] = HDR0; B[TX_FIFO] = (UINT32)(RLen - 1);
    B[TX_FIFO] = READ_FLAG | ((UINT32)Addr << 1);
  }
  for (I = 0; I < 20000; ++I) {
    S = B[INT_STATUS_REG];
    if ((S & (ERR_MASK | ALL_COMPLETE)) != 0) break;
    gBS->Stall (10);
  }
  *Status = S;
  if ((S & ERR_MASK) != 0 || (S & ALL_COMPLETE) == 0) {
    B[INT_STATUS_REG] = 0xFFFFFFFFU;
    return FALSE;
  }
  Words = (RLen + 3) / 4;
  if (RLen != 0 && (B[FIFO_STATUS] & 0xFU) < Words) return FALSE;
  for (I = 0; I < RLen; ++I) {
    if ((I % 4) == 0) Word = B[RX_FIFO];
    R[I] = (UINT8)(Word >> (8 * (I % 4)));
  }
  B[INT_STATUS_REG] = S;
  return TRUE;
}

// Pending event count (HW reg 0x0023, value >> 1).
STATIC UINTN R373TouchEventCount (volatile UINT32 *B) {
  CONST UINT8 Cmd[3] = { 0xB6, 0x00, 0x23 };
  UINT8 Buf[2] = { 0, 0 };
  UINT32 S;
  return R373I2cXfer (B, TOUCH_ADDR, Cmd, 3, Buf, 2, &S) ? (Buf[1] >> 1) : 0;
}

// Read one 8-byte event (cmd 0x85).
STATIC BOOLEAN R373TouchReadEvent (volatile UINT32 *B, UINT8 *Ev) {
  CONST UINT8 Cmd = 0x85;
  UINT32 S;
  return R373I2cXfer (B, TOUCH_ADDR, &Cmd, 1, Ev, 8, &S);
}

STATIC BOOLEAN R373TouchWaitReady (volatile UINT32 *B, UINTN Ms) {
  UINT8 Ev[8];
  UINTN T;
  for (T = 0; T < Ms * 2; ++T) {
    if (R373TouchEventCount (B) != 0 && R373TouchReadEvent (B, Ev) && Ev[0] == 0x10) return TRUE;
    gBS->Stall (500);
  }
  return FALSE;
}

STATIC BOOLEAN R373TouchCommand (volatile UINT32 *B, CONST UINT8 *Bytes, UINTN Len) {
  UINT32 S;
  return R373I2cXfer (B, TOUCH_ADDR, Bytes, Len, NULL, 0, &S);
}

STATIC VOID R373TouchPowerOn (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  volatile UINT32 *Pin = (volatile UINT32 *)PINMUX_BASE;
  volatile UINT32 *B = (volatile UINT32 *)I2C3_BASE;
  UINT8 Ldo = 0, LdoNew, LdoRead = 0, Info[7] = { 0 };
  CONST UINT8 ChipCmd[3] = { 0xB6, 0x00, 0x04 };
  CONST UINT8 ResetCmd[4] = { 0xB6, 0x00, 0x28, 0x80 };
  CONST UINT8 SenseMode[2] = { 0xC3, 0x01 }, SenseOn[1] = { 0x93 }, ClearStack[1] = { 0xA1 };
  BOOLEAN ClkOn, InReset, Ready, ResetOk = FALSE, SenseOk = FALSE;
  UINT32 S = 0;
  UINTN Retry;

  // 1. Reset pin PJ7 (pin DAP4_SCLK) low.
  Pin[PINMUX_DAP4_SCLK / 4] = (1U << 2) | 1U;              // pull-down, function 1
  R366GpioSet (GPIO_PJ_OFFSET, GPIO_OUT, 7, FALSE);
  R366GpioSet (GPIO_PJ_OFFSET, GPIO_OE, 7, TRUE);
  R366GpioSet (GPIO_PJ_OFFSET, GPIO_CNF, 7, TRUE);
  gBS->Stall (20);

  // 2. MAX77620 LDO6 = 2.9 V (800 mV + 42 x 50 mV), power mode normal.
  if (i2c_recv_buf_small (&Ldo, 1, I2C_5, MAX77620_ADDR, MAX77620_LDO6_CFG) != 1) {
    Print (L"R373 touch: LDO6 read FAIL; touch not powered.\r\n");
    return;
  }
  LdoNew = (UINT8)((Ldo & 0xC0U) | 42U);
  i2c_send_byte (I2C_5, MAX77620_ADDR, MAX77620_LDO6_CFG, LdoNew);
  gBS->Stall (1000);
  LdoNew = (UINT8)((LdoNew & 0x3FU) | (3U << 6));
  i2c_send_byte (I2C_5, MAX77620_ADDR, MAX77620_LDO6_CFG, LdoNew);
  gBS->Stall (1000);
  i2c_recv_buf_small (&LdoRead, 1, I2C_5, MAX77620_ADDR, MAX77620_LDO6_CFG);
  Print (L"R373 touch: LDO6_CFG %02x -> %02x read=%02x (2.9 V, on)\r\n", Ldo, LdoNew, LdoRead);

  // 3. I2C3 pins (GEN3_I2C), detach CAM_I2C (I2C4) from the I2C3 function; touch IRQ PX1 input pull-up.
  Pin[PINMUX_GEN3_I2C_SCL / 4] = PINMUX_INPUT_EN_BIT;
  Pin[PINMUX_GEN3_I2C_SDA / 4] = PINMUX_INPUT_EN_BIT;
  Pin[PINMUX_CAM_I2C_SCL / 4] |= 1U;
  Pin[PINMUX_CAM_I2C_SDA / 4] |= 1U;
  Pin[PINMUX_TOUCH_INT / 4] = PINMUX_INPUT_EN_BIT | PINMUX_TRISTATE_BIT | PINMUX_PULL_UP_BITS;
  R366GpioSet (GPIO_PX_OFFSET, GPIO_OE, 1, FALSE);
  R366GpioSet (GPIO_PX_OFFSET, GPIO_CNF, 1, TRUE);

  // 4. I2C3 clock (CLK_M source, divisor 0 = 19.2 MHz) and reset.
  ClkOn = (Car[CAR_CLK_OUT_ENB_U / 4] & CLK_U_I2C3_BIT) != 0;
  InReset = (Car[CAR_RST_DEVICES_U / 4] & CLK_U_I2C3_BIT) != 0;
  if (!ClkOn || InReset) {
    Car[CAR_RST_DEV_U_SET / 4] = CLK_U_I2C3_BIT;
    Car[CAR_CLK_SOURCE_I2C3 / 4] = 6U << 29;
    Car[CAR_CLK_ENB_U_SET / 4] = CLK_U_I2C3_BIT;
    gBS->Stall (10);
    Car[CAR_RST_DEV_U_CLR / 4] = CLK_U_I2C3_BIT;
    gBS->Stall (10);
  }
  Print (L"R373 touch: I2C3 clk was %u reset was %u -> clk=%u reset=%u SRC=%08x\r\n", ClkOn, InReset,
         (Car[CAR_CLK_OUT_ENB_U / 4] & CLK_U_I2C3_BIT) != 0, (Car[CAR_RST_DEVICES_U / 4] & CLK_U_I2C3_BIT) != 0,
         Car[CAR_CLK_SOURCE_I2C3 / 4]);
  i2c_init (I2C_3);
  B[CNFG] |= 0x400U;                                          // packet mode
  B[CONFIG_LOAD] = 0x25;
  for (Retry = 0; Retry < 10000 && (B[CONFIG_LOAD] & 1U) != 0; ++Retry) gBS->Stall (10);
  gBS->Stall (1000);

  // 5. Release reset, wait for the controller.
  R366GpioSet (GPIO_PJ_OFFSET, GPIO_OUT, 7, TRUE);
  gBS->Stall (10000);
  Ready = R373TouchWaitReady (B, 20);

  // 6. Chip ID.
  if (!R373I2cXfer (B, TOUCH_ADDR, ChipCmd, 3, Info, 7, &S)) {
    Print (L"R373 touch: chip info read FAIL I2C=%08x (ready=%u)\r\n", S, Ready);
    return;
  }
  Print (L"R373 touch: ready=%u chip=%02x%02x fw=%02x%02x config=%02x/%02x (expect chip 3670)\r\n",
         Ready, Info[1], Info[2], Info[3], Info[4], Info[5], Info[6]);
  if (Info[1] != 0x36 || Info[2] != 0x70) {
    Print (L"R373 touch: unexpected chip ID; not initialised.\r\n");
    return;
  }

  // 7. System reset, then finger sense on (Hekate touch_init / touch_sense_enable).
  for (Retry = 0; Retry < 3 && !ResetOk; ++Retry) {
    if (R373TouchCommand (B, ResetCmd, 4)) {
      gBS->Stall (10000);
      ResetOk = R373TouchWaitReady (B, 20);
    } else {
      gBS->Stall (10000);
    }
  }
  SenseOk = R373TouchCommand (B, SenseMode, 2) && R373TouchCommand (B, SenseOn, 1) &&
            R373TouchCommand (B, ClearStack, 1);
  mR373TouchReady = ResetOk && SenseOk;
  Print (L"R373 touch: reset=%s sense-on=%s. Touch the screen during USB STATUS to see events.\r\n",
         ResetOk ? L"PASS" : L"FAIL", SenseOk ? L"PASS" : L"FAIL");
}

// Status report: pending events and up to 3 decoded ones (x, y per Hekate).
STATIC VOID R373PrintTouch (VOID) {
  volatile UINT32 *B = (volatile UINT32 *)I2C3_BASE;
  UINTN Count, I;
  UINT8 Ev[8];
  if (!mR373TouchReady) {
    Print (L"R373 touch: not initialised\r\n");
    return;
  }
  Count = R373TouchEventCount (B);
  Print (L"R373 touch: events=%u IRQ(PX1)=%u (0 = pending)\r\n", (UINT32)Count,
         R366GpioBit (GPIO_PX_OFFSET, GPIO_IN, 1));
  for (I = 0; I < Count && I < 3; ++I) {
    if (!R373TouchReadEvent (B, Ev)) break;
    Print (L"R373 touch: ev %02x id=%u x=%u y=%u raw=%02x %02x %02x %02x %02x %02x %02x %02x\r\n",
           Ev[0] & 0x0FU, (Ev[0] >> 4) + 1, ((UINT32)Ev[1] << 4) | ((Ev[3] & 0xF0U) >> 4),
           ((UINT32)Ev[2] << 4) | (Ev[3] & 0x0FU), Ev[0], Ev[1], Ev[2], Ev[3], Ev[4], Ev[5], Ev[6], Ev[7]);
  }
}

// Raise VDD_CPU first, park CCLK on PLLP (408 MHz), reprogram and relock
// PLLX, then return CCLK to PLLX. Any failure puts the old PLLX/voltage back.
STATIC VOID R364RaiseCpuClock (VOID) {
  volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
  UINT32 OldBase, OldPolicy, NewBase, I;
  UINT64 RateBefore, RateAfter;
  UINT8 OldVout = 0, OldDvc = 0, NewCode;
  BOOLEAN Locked = FALSE;

  OldBase = Car[CAR_PLLX_BASE / 4];
  OldPolicy = Car[CAR_CCLK_BURST_POLICY / 4];
  Print (L"R364 CPU before: PLLX_BASE=%08x (%u kHz) MISC=%08x CCLK_POLICY=%08x SUPER_DIV=%08x\r\n",
         OldBase, R364PllxKhz (OldBase), Car[CAR_PLLX_MISC / 4], OldPolicy,
         Car[CAR_SUPER_CCLK_DIVIDER / 4]);
  Print (L"R365 CCLKG_POLICY(0x368)=%08x CLK_OUT_ENB_Y=%08x (bit31 PLLP_OUT_CPU=%u)\r\n",
         Car[CAR_CCLKG_BURST_POLICY / 4], Car[CAR_CLK_OUT_ENB_Y / 4],
         (Car[CAR_CLK_OUT_ENB_Y / 4] >> 31) & 1U);
  if (!R364ReadCpuVout (MAX77621_VOUT, &OldVout) || !R364ReadCpuVout (MAX77621_VOUT_DVC, &OldDvc)) {
    Print (L"R364 SKIP: MAX77621 (I2C5 0x1B) read FAIL; CPU clock unchanged.\r\n");
    return;
  }
  Print (L"R364 VDD_CPU before: VOUT=%02x (%u mV) VOUT_DVC=%02x (%u mV)\r\n",
         OldVout, R364VoutMv (OldVout), OldDvc, R364VoutMv (OldDvc));
  if ((OldVout & MAX77621_VOUT_ENABLE) == 0 || (OldBase & (PLLX_ENABLE | PLLX_BYPASS)) != PLLX_ENABLE ||
      (OldBase & PLLX_LOCK) == 0 || OldPolicy != CCLK_ON_PLLX) {
    Print (L"R364 SKIP: unexpected CPU clock/regulator state; CPU clock unchanged.\r\n");
    return;
  }
  NewCode = (UINT8)(MAX77621_VOUT_ENABLE |
                    ((R364_TARGET_MV * 1000U - MAX77621_MIN_UV + MAX77621_STEP_UV - 1) / MAX77621_STEP_UV));
  if (R364VoutMv (NewCode) > R364_MAX_MV || R364VoutMv (NewCode) < R364VoutMv (OldVout)) {
    Print (L"R364 SKIP: target voltage %u mV out of range.\r\n", R364VoutMv (NewCode));
    return;
  }
  RateBefore = R364LoopRate ();

  Print (L"R364 step 1: VDD_CPU %u -> %u mV (code %02x)... ", R364VoutMv (OldVout), R364VoutMv (NewCode), NewCode);
  if (!R364SetCpuVout (NewCode)) {
    Print (L"FAIL; restoring %02x.\r\n", OldVout);
    R364SetCpuVout (OldVout);
    return;
  }
  Print (L"PASS\r\n");

  NewBase = (OldBase & ~(0xFFU | (0xFFU << 8) | (0x1FU << 20))) |
            R364_TARGET_M | (R364_TARGET_N << 8);
  Print (L"R365 step 2: CCLK -> CLK_M (38.4 MHz), PLLX %08x -> %08x (%u kHz)... ",
         OldBase, NewBase & ~PLLX_LOCK, R364PllxKhz (NewBase));
  Car[CAR_CCLK_BURST_POLICY / 4] = CCLK_ON_CLKM;
  gBS->Stall (10);
  Car[CAR_PLLX_MISC / 4] |= PLLX_LOCK_ENABLE;
  Car[CAR_PLLX_BASE / 4] = (NewBase & ~(PLLX_ENABLE | PLLX_LOCK));
  gBS->Stall (10);
  Car[CAR_PLLX_BASE / 4] = (NewBase & ~PLLX_LOCK) | PLLX_ENABLE;
  for (I = 0; I < 1000; ++I) {
    gBS->Stall (10);
    if ((Car[CAR_PLLX_BASE / 4] & PLLX_LOCK) != 0) { Locked = TRUE; break; }
  }
  if (!Locked) {
    Car[CAR_PLLX_BASE / 4] = (OldBase & ~(PLLX_ENABLE | PLLX_LOCK));
    gBS->Stall (10);
    Car[CAR_PLLX_BASE / 4] = (OldBase & ~PLLX_LOCK) | PLLX_ENABLE;
    for (I = 0; I < 1000 && (Car[CAR_PLLX_BASE / 4] & PLLX_LOCK) == 0; ++I) gBS->Stall (10);
    Car[CAR_CCLK_BURST_POLICY / 4] = OldPolicy;
    R364SetCpuVout (OldVout);
    Print (L"FAIL (no lock); old PLLX and voltage restored, BASE=%08x.\r\n", Car[CAR_PLLX_BASE / 4]);
    return;
  }
  gBS->Stall (50);
  Car[CAR_CCLK_BURST_POLICY / 4] = OldPolicy;
  gBS->Stall (10);
  Print (L"PASS (lock after %u0 us)\r\n", I + 1);

  RateAfter = R364LoopRate ();
  Print (L"R364 CPU after: PLLX_BASE=%08x (%u kHz) CCLK_POLICY=%08x VOUT=%u mV\r\n",
         Car[CAR_PLLX_BASE / 4], R364PllxKhz (Car[CAR_PLLX_BASE / 4]),
         Car[CAR_CCLK_BURST_POLICY / 4], R364VoutMv (NewCode));
  Print (L"R364 loop rate: before=%lu/ms after=%lu/ms ratio=%lu.%02lu (expect %u.%02u) => about %lu MHz\r\n",
         RateBefore, RateAfter,
         RateBefore ? RateAfter / RateBefore : 0, RateBefore ? ((RateAfter * 100) / RateBefore) % 100 : 0,
         R364PllxKhz (NewBase) / R364PllxKhz (OldBase),
         ((R364PllxKhz (NewBase) * 100) / R364PllxKhz (OldBase)) % 100,
         RateBefore ? (RateAfter * R364PllxKhz (OldBase)) / (RateBefore * 1000) : 0);
}

// R363: periodic status variant; borrows I2C1 like RunTest and restores it.
STATIC VOID R363PrintPower (CONST CHAR16 *Tag) {
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  R288SaveI2cController (B);
  i2c_init (I2C_1);
  B[CNFG] |= 0x400U;
  if (R288LoadI2cConfig ()) R363PrintPowerNow (Tag);
  else Print (L"R363 %s I2C packet mode FAIL\r\n", Tag);
  R288RestoreI2cController ();
}

STATIC VOID R363DisableBqWatchdog (VOID) {
  UINT32 S = 0;
  UINT8 BqTimer = 0, BqTimerTarget = 0, BqTimerVerify = 0;
  // Nobody services the BQ2419x watchdog after this driver (or after
  // ExitBootServices). Linux sets ti,watchdog-timeout=0; do the same so the
  // charger setting is not reverted to defaults on watchdog expiry.
  if (R288Read8 (BQ_ADDR, BQ_TERM_TIMER, &BqTimer, &S)) {
    BqTimerTarget = (UINT8)(BqTimer & ~BQ_WATCHDOG_MASK);
    if (BqTimerTarget != BqTimer) {
      if (!R288Write8 (BQ_ADDR, BQ_TERM_TIMER, BqTimerTarget, &S) ||
          !R288Read8 (BQ_ADDR, BQ_TERM_TIMER, &BqTimerVerify, &S)) {
        BqTimerVerify = 0xFF;
      }
    } else {
      BqTimerVerify = BqTimer;
    }
    Print (L"R362 BQ REG05 watchdog: %02x -> %02x read=%02x (%s)\r\n",
           BqTimer, BqTimerTarget, BqTimerVerify,
           (BqTimerVerify == BqTimerTarget) ? L"watchdog disabled" : L"FAIL");
  } else {
    Print (L"R362 BQ REG05 read FAIL I2C=%08x; watchdog unchanged\r\n", S);
  }
}

// R363: the partner sources power (USB-C hub with a charger). The BM92T
// firmware negotiates the sink contract by itself (Hekate shows 15 V with the
// stock settings), so only read it, try a data-role swap like L4T when the
// source offers DRD, then put the BQ2419x in charge mode with an input limit
// matching the contract (L4T table: 5/9 V 2000 mA, 12 V 1500 mA, 15 V 1200 mA).
// VBUS/OTG is never enabled on this path.
STATIC BOOLEAN R363ConfigureSink (VOID) {
  UINT32 S = 0;
  UINT8 Pdos[29], Cur[5];
  UINT16 Status1 = 0;
  UINT32 Pdo1 = 0, ContractPdo = 0, ContractMv = 0, ContractMa = 0, LimitMa;
  UINTN I, Count;
  BOOLEAN Contract = FALSE;
  UINT8 Rev = 0, In = 0, Pwr = 0, InTarget, PwrTarget, InVerify = 0, PwrVerify = 0, Code;
  CONST UINT32 Limits[8] = { 100, 150, 500, 900, 1200, 1500, 2000, 3000 };

  // Leave OTG first: an earlier R362 session (watchdog disabled) may have left
  // the BQ2419x boosting VBUS, which must not stay on with a source attached.
  if (!R288Read8 (BQ_ADDR, BQ_REVISION, &Rev, &S) || Rev != 0x2FU ||
      !R288Read8 (BQ_ADDR, BQ_PWR_ON, &Pwr, &S)) {
    Print (L"R363 BQ2419x identity/state read FAIL REV=%02x; no BQ write\r\n", Rev);
    return FALSE;
  }
  PwrTarget = (UINT8)((Pwr & ~(BQ_RESET_BITS | BQ_CHARGE_MODE_MASK)) | BQ_CHARGE_MODE);
  if (!R288Write8 (BQ_ADDR, BQ_PWR_ON, PwrTarget, &S) ||
      !R288Read8 (BQ_ADDR, BQ_PWR_ON, &PwrVerify, &S) ||
      (PwrVerify & BQ_CHARGE_MODE_MASK) != BQ_CHARGE_MODE) {
    Print (L"R363 BQ REG01 charge-mode write FAIL %02x -> %02x read=%02x I2C=%08x\r\n",
           Pwr, PwrTarget, PwrVerify, S);
    return FALSE;
  }
  Print (L"R363 BQ REG01 %02x -> %02x read=%02x (charge mode, OTG off) PASS\r\n",
         Pwr, PwrTarget, PwrVerify);

  if (R363ReadBlock (BM_ADDR, BM_READ_PDOS_SRC, Pdos, sizeof (Pdos), &S)) {
    Count = Pdos[0] / 4;
    if (Count > 7) Count = 7;
    Print (L"R363 source PDOs: %u\r\n", (UINT32)Count);
    for (I = 0; I < Count; ++I) {
      if (I == 0) Pdo1 = R363Le32 (&Pdos[1]);
      R363PrintPdo (L"PDO", I + 1, R363Le32 (&Pdos[1 + I * 4]));
    }
  } else {
    Print (L"R363 PDO read FAIL I2C=%08x\r\n", S);
  }

  for (I = 0; I < R363_SINK_CONTRACT_POLLS; ++I) {
    if (R288Read16 (BM_STATUS1, &Status1, &S) && (Status1 & BM_SPDSNK) != 0 &&
        R363ReadBlock (BM_ADDR, BM_CURRENT_PDO, Cur, sizeof (Cur), &S)) {
      ContractPdo = R363Le32 (&Cur[1]);
      if ((ContractPdo >> 30) == 0 && ContractPdo != 0) {
        ContractMv = ((ContractPdo >> 10) & 0x3FFU) * 50U;
        ContractMa = (ContractPdo & 0x3FFU) * 10U;
        Contract = TRUE;
        break;
      }
    }
    gBS->Stall (500000);
  }
  if (Contract) {
    R363PrintPdo (L"contract PDO", 0, ContractPdo);
    if (R363ReadBlock (BM_ADDR, BM_CURRENT_RDO, Cur, sizeof (Cur), &S))
      Print (L"R363 contract RDO raw=%08x\r\n", R363Le32 (&Cur[1]));
  } else {
    Print (L"R363 no PD contract after %u s (S1=%04x); treating as 5 V / 500 mA\r\n",
           (UINT32)(R363_SINK_CONTRACT_POLLS / 2), Status1);
  }

  // Data role: the Switch should be DFP (USB host). L4T swaps only when the
  // source advertises DRD; otherwise it keeps UFP and still runs as host.
  if (R288Read16 (BM_STATUS1, &Status1, &S)) {
    Print (L"R363 data role=%u (1=UFP 2=DFP) source DRD=%u\r\n",
           (Status1 & BM_DR_MASK) >> BM_DR_SHIFT, (Pdo1 & BM_PDO_DRD) != 0);
    if (((Status1 & BM_DR_MASK) >> BM_DR_SHIFT) == BM_DR_UFP && (Pdo1 & BM_PDO_DRD) != 0) {
      if (R288Write16 (BM_COMMAND, BM_DR_SWAP_CMD, &S)) {
        for (I = 0; I < 20; ++I) {
          gBS->Stall (100000);
          if (R288Read16 (BM_STATUS1, &Status1, &S) && (Status1 & BM_CMD_BUSY) == 0 &&
              ((Status1 & BM_DR_MASK) >> BM_DR_SHIFT) == BM_DR_DFP) break;
        }
        Print (L"R363 DR_SWAP: S1=%04x role=%u lastcmd=%u (%s)\r\n", Status1,
               (Status1 & BM_DR_MASK) >> BM_DR_SHIFT, (Status1 >> 4) & 7U,
               (((Status1 & BM_DR_MASK) >> BM_DR_SHIFT) == BM_DR_DFP) ? L"DFP" : L"still UFP; continue as host");
      } else {
        Print (L"R363 DR_SWAP command FAIL I2C=%08x; continue as host\r\n", S);
      }
    }
  }

  if (!R288Read8 (BQ_ADDR, BQ_INPUT_SRC, &In, &S)) {
    Print (L"R363 BQ REG00 read FAIL I2C=%08x; input limit unchanged\r\n", S);
    return FALSE;
  }
  if (!Contract) {
    LimitMa = 500;
  } else if (ContractMv <= 9500U) {
    LimitMa = 2000;
  } else if (ContractMv <= 12500U) {
    LimitMa = 1500;
  } else if (ContractMv <= 15500U) {
    LimitMa = 1200;
  } else {
    LimitMa = 500;
  }
  if (Contract && ContractMa != 0 && ContractMa < LimitMa) LimitMa = ContractMa;
  for (Code = 7; Code > 0 && Limits[Code] > LimitMa; --Code) {}
  InTarget = (UINT8)((In & ~(BQ_HIZ | BQ_IINLIM_MASK)) | Code);
  Print (L"R363 BQ REG00 %02x -> %02x (input limit %u mA, HIZ off)\r\n", In, InTarget, Limits[Code]);
  if (!R288Write8 (BQ_ADDR, BQ_INPUT_SRC, InTarget, &S) ||
      !R288Read8 (BQ_ADDR, BQ_INPUT_SRC, &InVerify, &S) || InVerify != InTarget) {
    Print (L"R363 BQ REG00 readback FAIL read=%02x I2C=%08x\r\n", InVerify, S);
    return FALSE;
  }
  Print (L"R363 BQ input limit readback PASS\r\n");
  R363DisableBqWatchdog ();
  gBS->Stall (500000);
  R363PrintPowerNow (L"after charge setup");
  return TRUE;
}

STATIC VOID EFIAPI RunTest (EFI_EVENT Event, VOID *Context) {
  EFI_SYSTEM_TABLE *St = (EFI_SYSTEM_TABLE *)Context;
  volatile UINT32 *B = (volatile UINT32 *)BASE_ADDR;
  UINT32 S = 0;
  UINT16 FwType = 0, FwRev = 0, Config1 = 0, Vendor = 0, Verify = 0, Target = 0;
  UINT16 Status1 = 0, Status2 = 0, Dp = 0, TargetConfig1, TargetVendor;
  UINT8 BqRevision = 0, BqOriginal = 0, BqTarget = 0, BqVerify = 0;
  UINTN I, HostStreak = 0, SinkStreak = 0;
  BOOLEAN SinkGatePassed = FALSE;
  BOOLEAN IdentityOk = FALSE, ConfigSaved = FALSE, VendorSaved = FALSE;
  BOOLEAN ConfigWriteAttempted = FALSE, VendorWriteAttempted = FALSE;
  BOOLEAN ConfigRestoreOk = TRUE, VendorRestoreOk = TRUE;
  BOOLEAN BqWriteAttempted = FALSE, BqEnableOk = FALSE, BqRestoreOk = TRUE;
  BOOLEAN HostGatePassed = FALSE;
  BOOLEAN XhciAttempted = FALSE;
  BOOLEAN Detected = FALSE, BqChanged = FALSE;
  UINT8 BqSys = 0;

  gBS->CloseEvent (Event);
  St->ConOut->SetAttribute (St->ConOut, EFI_WHITE | EFI_BACKGROUND_BLUE);
  St->ConOut->ClearScreen (St->ConOut);
  Print (L"============================================================\r\n");
  Print (L" Nintendo Switch UEFI XUSB bring-up R378 (ACPI GPIO/buttons/I2C/battery/fan/touch/power button/CPU clock/RTC + USB hot-plug + backlight PWM + touch power + fan + CPU 1020 MHz + USB + hub charging)\r\n");
  Print (L" host or sink gate -> VBUS/charge -> path -> firmware/Falcon -> PADCTL -> XhciDxe\r\n");
  Print (L"============================================================\r\n\r\n");
  Print (L"OTG: USB device directly (Switch supplies VBUS), or a USB-C hub with its\r\n");
  Print (L"charger already connected (Switch charges, still USB host). No PC.\r\n");
  Print (L"XUSB, PADCTL and power settings are left running for the stock USB stack.\r\n\r\n");
  ZeroMem (mR293Car, sizeof (mR293Car));
  CaptureR293Car (R293_EARLY);

  R288SaveI2cController ((volatile UINT32 *)BASE_ADDR);
  Print (L"I2C controller init... ");
  i2c_init (I2C_1);
  if (((volatile UINT32 *)BASE_ADDR)[CNFG] == 0xFFFFFFFFU) {
    Print (L"FAIL; no BM92T/BQ writes.\r\n");
    goto R288Cleanup;
  }
  ((volatile UINT32 *)BASE_ADDR)[CNFG] |= 0x400U;
  if (!R288LoadI2cConfig ()) {
    Print (L"FAIL (packet mode); no BM92T/BQ writes.\r\n");
    goto R288Cleanup;
  }
  Print (L"PASS\r\n");
  R363PrintPowerNow (L"before CPU");
  R364RaiseCpuClock ();
  R366StartFan ();
  R376BacklightPwm ();
  R373TouchPowerOn ();

  if (!R288Read16 (BM_FW_TYPE, &FwType, &S) ||
      !R288Read16 (BM_FW_REV, &FwRev, &S) ||
      !R288Read16 (BM_CONFIG1, &Config1, &S) ||
      !R288Read16 (BM_VENDOR_CONFIG, &Vendor, &S)) {
    Print (L"BM92T read FAIL I2C=%08x; no source-mode update.\r\n", S);
    goto R288Cleanup;
  }
  IdentityOk = (FwType == 0x0603U && FwRev == 0x0D7CU);
  ConfigSaved = VendorSaved = TRUE;
  Print (L"BM92T FW=%04x/%04x CONFIG1=%04x VENDOR=%04x (%s)\r\n",
         FwType, FwRev, Config1, Vendor, IdentityOk ? L"match" : L"unexpected");
  if (!IdentityOk) {
    Print (L"SAFE STOP: unexpected BM92T identity; no BM/BQ changes.\r\n");
    goto R288Cleanup;
  }

  TargetVendor = (UINT16)(Vendor & ~BM_OCP_DISABLE);
  if (TargetVendor != Vendor) {
    VendorWriteAttempted = TRUE;
    if (!R288Write16 (BM_VENDOR_CONFIG, TargetVendor, &S) ||
        !R288Read16 (BM_VENDOR_CONFIG, &Verify, &S) || Verify != TargetVendor) {
      Print (L"BM92T OCP-enable write/readback FAIL; no VBUS enable.\r\n");
      goto R288Cleanup;
    }
    Print (L"BM92T OCP-enable readback PASS.\r\n");
  } else Print (L"BM92T OCP already enabled; unchanged.\r\n");

  TargetConfig1 = (UINT16)(Config1 & ~BM_SPDSRC_MASK);
  if (TargetConfig1 != Config1) {
    ConfigWriteAttempted = TRUE;
    if (!R288Write16 (BM_CONFIG1, TargetConfig1, &S) ||
        !R288Read16 (BM_CONFIG1, &Verify, &S) || Verify != TargetConfig1) {
      Print (L"BM92T source-mode update FAIL; no VBUS enable.\r\n");
      goto R288Cleanup;
    }
  }
  Print (L"BM92T source mode CONFIG1=%04x readback PASS.\r\n", TargetConfig1);
  Print (L"Waiting up to 30s: host = INSERT+SRC_MODE+OTG_INSERT, sink = INSERT+VSAFE+!SRC_MODE+!DFP (3 samples)...\r\n");
  for (I = 0; I < R288_HOST_POLLS; ++I) {
    if (!R288Read16 (BM_STATUS1, &Status1, &S) ||
        !R288Read16 (BM_STATUS2, &Status2, &S) ||
        !R288Read16 (BM_DP_STATUS, &Dp, &S)) {
      HostStreak = 0;
      SinkStreak = 0;
      Print (L"t=%02us BM92T read FAIL I2C=%08x\r\n", I + 1, S);
    } else {
      if (((Status1 & (BM_INSERT | BM_SRC_MODE)) == (BM_INSERT | BM_SRC_MODE)) &&
          (Status2 & BM_OTG_INSERT) != 0) HostStreak++;
      else HostStreak = 0;
      // R363: a power source (hub + charger) puts the BM92T in sink mode with
      // VBUS present. Never treat a DFP role as a source partner.
      if ((Status1 & (BM_INSERT | BM_VSAFE)) == (BM_INSERT | BM_VSAFE) &&
          (Status1 & BM_SRC_MODE) == 0 &&
          ((Status1 & BM_DR_MASK) >> BM_DR_SHIFT) != BM_DR_DFP) SinkStreak++;
      else SinkStreak = 0;
      if (I == 0 || HostStreak != 0 || SinkStreak != 0 || ((I + 1) % 5) == 0)
        Print (L"t=%02us S1=%04x S2=%04x DP=%04x INS=%u SRC=%u OTG=%u VSAFE=%u SNK=%u DR=%u\r\n",
               I + 1, Status1, Status2, Dp,
               (Status1 & BM_INSERT) != 0,
               (Status1 & BM_SRC_MODE) != 0,
               (Status2 & BM_OTG_INSERT) != 0,
               (Status1 & BM_VSAFE) != 0,
               (Status1 & BM_SPDSNK) != 0,
               (Status1 & BM_DR_MASK) >> BM_DR_SHIFT);
      if (HostStreak >= R288_HOST_STREAK) {
        HostGatePassed = TRUE;
        break;
      }
      if (SinkStreak >= R288_HOST_STREAK) {
        SinkGatePassed = TRUE;
        break;
      }
    }
    gBS->Stall (1000000);
  }
  if (SinkGatePassed) {
    Print (L"Stable BM92T sink state (power source attached): charge path, no VBUS output.\r\n");
    if (!R363ConfigureSink ()) {
      Print (L"R363 SAFE STOP: charger setup failed; xHCI skipped.\r\n");
      goto R288Cleanup;
    }
    mR363SinkMode = TRUE;
    goto R363PathSetup;
  }
  if (!HostGatePassed) {
    // R378: nothing attached. Bring XUSB up anyway, with VBUS off (BQ stays in
    // charge mode) and the BM92T source-mode setting kept, so Windows can
    // enable VBUS or the charge path on a later plug-in.
    Print (L"R378 no partner after %u s: XUSB host without VBUS; Windows handles hot-plug.\r\n",
           (UINT32)R288_HOST_POLLS);
    mR378NoPartner = TRUE;
    R363DisableBqWatchdog ();
    goto R363PathSetup;
  }
  Print (L"Stable BM92T host state: PASS.\r\n");

  if (!R288Read8 (BQ_ADDR, BQ_REVISION, &BqRevision, &S) || BqRevision != 0x2FU ||
      !R288Read8 (BQ_ADDR, BQ_PWR_ON, &BqOriginal, &S)) {
    Print (L"BQ2419x identity/state read FAIL REV=%02x; no BQ write.\r\n", BqRevision);
    goto R288Cleanup;
  }
  BqTarget = (UINT8)((BqOriginal & ~BQ_CHARGE_MODE_MASK) | BQ_VBUS_MODE);
  Print (L"BQ2419x REV=%02x PWR_ON %02x -> %02x (masked update).\r\n",
         BqRevision, BqOriginal, BqTarget);
  BqWriteAttempted = TRUE;
  if (!R288Write8 (BQ_ADDR, BQ_PWR_ON, BqTarget, &S) ||
      !R288Read8 (BQ_ADDR, BQ_PWR_ON, &BqVerify, &S) || BqVerify != BqTarget) {
    Print (L"BQ VBUS mode readback FAIL; skip xHCI.\r\n");
    goto R288Cleanup;
  }
  BqEnableOk = TRUE;
  Print (L"VBUS enabled/readback PASS. Apply Linux-matched USB2 path fields.\r\n");
  if (mR362Production) {
    R363DisableBqWatchdog ();
  } else {
    Print (L"No PORTSC writes, port reset, command ring, enumeration, or transfer.\r\n\r\n");
  }
R363PathSetup:
  mR300Gate = R300_GATE_PATH_SETUP;
  if (R295BeginLinuxMatchedPath ()) {
    XhciAttempted = TRUE;
    mR300Gate = R300_GATE_FW_FILE;
    RunR244FirmwareLoadPlanAudit (4);
    if (!mR362Attached) {
      R295RestoreLinuxMatchedPath ();
    }
  } else {
    Print (L"R295 SAFE STOP: path setup failed; xHCI window skipped.\r\n");
  }

R288Cleanup:
  if (mR362Attached) {
    // R362: XUSB is owned by XhciDxe now. Keep VBUS (BQ OTG mode) or, in R363
    // sink mode, the BQ charge mode; keep the BM92T configuration and the USB2
    // path fields; restore only the I2C controller registers this driver borrowed.
    R288RestoreI2cController ();
    Print (L"\r\n============================================================\r\n");
    Print (L" R363 RESULT: XUSB running, stock XhciDxe/UsbBus/UsbMassStorage connected\r\n");
    if (mR363SinkMode) {
      Print (L" power=HUB/CHARGER (sink, charging, kept) path=PASS PADCTL host=PASS (kept)\r\n");
    } else if (mR378NoPartner) {
      Print (L" nothing attached: VBUS off, path=PASS PADCTL host=PASS (kept); plug in under Windows\r\n");
    } else {
      Print (L" host-gate=PASS VBUS=PASS (kept) path=PASS (kept) PADCTL host=PASS (kept)\r\n");
    }
    Print (L" Status reports follow every 5 s (4 times). In the UEFI Shell: map -r, then ls fsN:\r\n");
    Print (L"============================================================\r\n");
    return;
  }
  if (BqWriteAttempted) {
    Print (L"Restoring BQ PWR_ON=%02x... ", BqOriginal);
    BqRestoreOk = R288Write8 (BQ_ADDR, BQ_PWR_ON, BqOriginal, &S) &&
                  R288Read8 (BQ_ADDR, BQ_PWR_ON, &BqVerify, &S) &&
                  BqVerify == BqOriginal;
    Print (BqRestoreOk ? L"PASS\r\n" : L"FAIL read=%02x I2C=%08x\r\n", BqVerify, S);
  }
  if (ConfigSaved && ConfigWriteAttempted) {
    ConfigRestoreOk = R288Write16 (BM_CONFIG1, Config1, &S) &&
                      R288Read16 (BM_CONFIG1, &Verify, &S) && Verify == Config1;
    Print (ConfigRestoreOk ? L"Restoring BM92T CONFIG1: PASS\r\n" :
                             L"Restoring BM92T CONFIG1: FAIL\r\n");
  }
  if (VendorSaved && VendorWriteAttempted) {
    VendorRestoreOk = R288Write16 (BM_VENDOR_CONFIG, Vendor, &S) &&
                      R288Read16 (BM_VENDOR_CONFIG, &Verify, &S) && Verify == Vendor;
    Print (VendorRestoreOk ? L"Restoring BM92T VENDOR_CONFIG: PASS\r\n" :
                             L"Restoring BM92T VENDOR_CONFIG: FAIL\r\n");
  }
  R288RestoreI2cController ();
  Print (L"I2C controller original state restore requested; W1C flags may not restore.\r\n");
  CaptureR293Car (R293_FINAL);
  CaptureR294Usb2 (R294_USB2_FINAL);
  Print (L"R295 RESULT: host-gate=%s VBUS=%s path=%s path-restore=%s BQ-restore=%s config-restore=%s OCP-restore=%s xHCI=%s\r\n",
         HostGatePassed ? L"PASS" : L"NO",
         BqEnableOk ? L"PASS" : L"NO",
         mR295Path.Applied ? L"PASS" : (mR295Path.Attempted ? L"FAIL" : L"SKIPPED"),
         mR295Path.Attempted ? (mR295Path.Restored ? L"PASS" : L"FAIL") : L"N/A",
         BqWriteAttempted ? (BqRestoreOk ? L"PASS" : L"FAIL") : L"N/A",
         ConfigSaved ? (ConfigWriteAttempted ? (ConfigRestoreOk ? L"PASS" : L"FAIL") : L"UNCHANGED") : L"N/A",
         VendorSaved ? (VendorWriteAttempted ? (VendorRestoreOk ? L"PASS" : L"FAIL") : L"UNCHANGED") : L"N/A",
         XhciAttempted ? L"ATTEMPTED; see PORTSC above" : L"SKIPPED");
  // R291 legacy fields are no longer interpreted: its bit14 SET attempt was
  // based on an incorrect clock-ID assumption and is disabled in this build.
  Print (L"R292 FILE PRE valid=%u CLK/RST_W/PLLU=%08x/%08x/%08x\r\n",
         mR292FilePreValid, mR292FilePreClockW,
         mR292FilePreResetW, mR292FilePrePlluBase);
  Print (L"R292 FILE POST valid=%u CLK/RST_W/PLLU=%08x/%08x/%08x\r\n",
         mR292FilePostValid, mR292FilePostClockW,
         mR292FilePostResetW, mR292FilePostPlluBase);
  PrintR293CarSnapshots ();
  PrintR294Usb2Snapshots ();
  Print (L"R292 file snapshots bracket FAT firmware-file access; PLLU raw only.\r\n");
  Print (L"R295 changed only USB2_TRK bit18, PAD_MUX bias field, and PORT0 CAP; UTMI/BIAS/VREG untouched.\r\n");
  if (!BqRestoreOk || !ConfigRestoreOk || !VendorRestoreOk ||
      (mR295Path.Attempted && !mR295Path.Restored))
    Print (L"STOP: restore failed; disconnect USB memory after photo; hold POWER 12-15s.\r\n");
  Print (L"CCS=1 means connected only, not USB enumeration. Take photo, then hold POWER 12-15s.\r\n");

  // The verbose audit scrolls on the Switch display. Replace it with a stable
  // compact result page so the user can photograph every decisive status.
  St->ConOut->ClearScreen (St->ConOut);
  St->ConOut->SetAttribute (St->ConOut, EFI_WHITE | EFI_BACKGROUND_BLUE);
  Print (L"============================================================\r\n");
  Print (L" R309 FINAL SUMMARY (xHCI Start/Stop probe)\r\n");
  Print (L"============================================================\r\n");
  Print (L"Host=%s VBUS=%s Path=%s Restore=%s\r\n",
         HostGatePassed ? L"PASS" : L"NO",
         BqEnableOk ? L"PASS" : L"NO",
         mR295Path.Applied ? L"PASS" : (mR295Path.Attempted ? L"FAIL" : L"SKIP"),
         mR295Path.Attempted ? (mR295Path.Restored ? L"PASS" : L"FAIL") : L"N/A");
  Print (L"USB2_TRK bit18: %u>%u>%u (saved>live>final)\r\n",
         (mR295Path.ClockY >> 18) & 1U, (mR295Path.TrackLive >> 18) & 1U,
         (mR295Path.TrackFinal >> 18) & 1U);
  Print (L"PAD_MUX bias[19:18]: %u>%u>%u\r\n",
         (mR295Path.PadMux >> 18) & 3U, (mR295Path.MuxLive >> 18) & 3U,
         (mR295Path.MuxFinal >> 18) & 3U);
  Print (L"PORT_CAP port0: %u>%u>%u (OTG=3)\r\n",
         mR295Path.PortCap & 3U, mR295Path.CapLive & 3U,
         mR295Path.CapFinal & 3U);
  Print (L"CCS P5-P9=%u%u%u%u%u valid=%02x (1=connected)\r\n",
         mR260Usb2PortSc[0] & 1U, mR260Usb2PortSc[1] & 1U,
         mR260Usb2PortSc[2] & 1U, mR260Usb2PortSc[3] & 1U,
         mR260Usb2PortSc[4] & 1U, mR260Usb2PortValidMask);
  if (!HostGatePassed) {
    Print (L"STOP_POINT=BM92T_HOST_GATE; later stages were not reached.\r\n");
  } else if (!BqEnableOk) {
    Print (L"STOP_POINT=VBUS_SETUP; inspect BQ identity/readback above.\r\n");
  } else if (!mR295Path.Applied) {
    Print (L"STOP_POINT=USB2_PATH_SETUP; inspect path readback above.\r\n");
  } else if (!mR300TransferPassed || !mR300FalconPassed ||
             !mR300OperationalPassed || !mR300RunStopPassed) {
    Print (L"STOP_POINT=XUSB_FIRMWARE_OR_CONTROLLER; Gate=%s; see stage results.\r\n",
           R300GateName (mR300Gate));
  } else if (mR260Usb2PortValidMask != 0x1FU) {
    Print (L"STOP_POINT=USB2_PORTSC_READ; valid=%02x expected=1f.\r\n",
           mR260Usb2PortValidMask);
  } else if (((mR260Usb2PortSc[0] | mR260Usb2PortSc[1] |
               mR260Usb2PortSc[2] | mR260Usb2PortSc[3] |
               mR260Usb2PortSc[4]) & 1U) == 0) {
    Print (L"STOP_POINT=ROOT_PORT_CCS; no USB2 port reports connection.\r\n");
    Print (L"NEXT_GATE=single-port reset/enumeration remains blocked until CCS=1.\r\n");
  } else {
    Print (L"STOP_POINT=ROOT_PORT_CCS; connected port present; review gated next stage.\r\n");
  }
  Print (L"Restore: BQ=%s BM-config=%s BM-OCP=%s; xHCI=%s\r\n",
         BqWriteAttempted ? (BqRestoreOk ? L"PASS" : L"FAIL") : L"N/A",
         ConfigSaved ? (ConfigWriteAttempted ? (ConfigRestoreOk ? L"PASS" : L"FAIL") : L"UNCHANGED") : L"N/A",
         VendorSaved ? (VendorWriteAttempted ? (VendorRestoreOk ? L"PASS" : L"FAIL") : L"UNCHANGED") : L"N/A",
         XhciAttempted ? L"ATTEMPTED" : L"SKIPPED");
  Print (L"Gate=%s\r\n", R300GateName (mR300Gate));
  Print (L"Stages CSB=%s FW=%s FLN=%s HCD=%s RUN=%s\r\n",
         R300StageStatus (mR300CsbAttempted, mR300CsbPassed),
         R300StageStatus (mR300TransferAttempted, mR300TransferPassed),
         R300StageStatus (mR300FalconAttempted, mR300FalconPassed),
         R300StageStatus (mR300OperationalAttempted, mR300OperationalPassed),
         R300StageStatus (mR300RunStopAttempted, mR300RunStopPassed));
  Print (L"ND register+cleanup=%s; registration cleanup=%s\r\n",
         R300StageStatus (mR300RegistrationAttempted, mR300RegistrationPassed),
         mR304RegistrationCleanupPassed ? L"PASS" : L"FAIL/SKIP");
  Print (L"ND Supported=%s Start=%s PCI-I/O=%s\r\n",
         !mR304ProbeAttempted ? L"SKIP" : (mR304NdSupportedPassed ? L"PASS" : L"FAIL"),
         !mR304ProbeAttempted ? L"SKIP" : (mR304NdStartPassed ? L"PASS" : L"FAIL"),
         !mR304ProbeAttempted ? L"SKIP" : (mR304PciIoObserved ? L"PASS" : L"NO"));
  Print (L"xHCI Supported=%s; ND Stop=%s; PCI-I/O removed=%s\r\n",
         !mR304ProbeAttempted ? L"SKIP" : (mR304XhciSupportedPassed ? L"PASS" : L"NO-MATCH/FAIL"),
         !mR304ProbeAttempted ? L"SKIP" : (mR304NdStopPassed ? L"PASS" : L"FAIL/SKIP"),
         !mR304ProbeAttempted ? L"SKIP" : (mR304PciIoRemoved ? L"PASS" : L"FAIL/SKIP"));
  Print (L"xHCI Start=%s RUN=%s HC-proto=%s\r\n",
         !mR309XhciStartAttempted ? L"SKIP" : (mR309XhciStartPassed ? L"PASS" : L"FAIL"),
         !mR309XhciStartAttempted ? L"SKIP" : (mR309XhciRunVerified ? L"PASS" : L"FAIL"),
         !mR309XhciStartAttempted ? L"SKIP" : (mR309Usb2HcInstalled ? L"PASS" : L"FAIL"));
  Print (L"xHCI Stop=%s HC-proto-removed=%s\r\n",
         !mR309XhciStopAttempted ? L"SKIP" : (mR309XhciStopPassed ? L"PASS" : L"FAIL"),
         !mR309XhciStopAttempted ? L"SKIP" : (mR309Usb2HcRemoved ? L"PASS" : L"FAIL"));
  Print (L"ConnectController=NOT-ATTEMPTED; USB I/O=NO.\r\n");
  Print (L"No USB device enumeration, descriptor request, or data transfer.\r\n");
  if (!BqRestoreOk || !ConfigRestoreOk || !VendorRestoreOk ||
      (mR295Path.Attempted && !mR295Path.Restored))
    Print (L"RESTORE FAIL: do not retry; hold POWER 12-15s.\r\n");
  Print (L"Take a photo now, then hold POWER 12-15 seconds to shut down.\r\n");
  return;

  /* Historical diagnostics below remain compiled but are not dispatched. */
  // Keep historical diagnostic chains compiled, but never dispatch them here.
  if (FALSE) RunR237XusbHostWindowAudit ();
  if (FALSE) RunR137Ftm4I2cPreflight ();
  if (FALSE) RunR236XusbWindowAudit ();
  if (FALSE) RunR129NoSdUsb2Test ();
  if (FALSE) RunR135TouchReadOnlyAudit ();
  if (FALSE) RunR136TouchGpioSelectTest ();
  Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
  return;

  if (!AuditXusbFirmwareFile () || mFirmwarePages == 0) {
    LogText (L"R82 STOP: firmware buffer was not prepared. No XUSB change.\r\n");
    CloseResultLog ();
    Print (L"R82 STOP: firmware buffer was not prepared. No XUSB change.\r\n");
    Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
    return;
  }

  goto R41Lifecycle;

  {
    volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
    volatile UINT32 *Pmc = (volatile UINT32 *)PMC_BASE;
    UINT32 OldW = Car[0x364 / 4];
    UINT32 OldResetW = Car[0x35C / 4];
    UINT32 GateMask = (1U << 15);
    BOOLEAN RestoreOk;

    Print (L"Linux xusb_gate/xusb_src reset = ID 143 = CAR W bit 15.\r\n");
    Print (L"Before CLK_ENB_W=%08x RST_DEV_W=%08x SRC_RESET=%d\r\n",
           OldW, OldResetW, (OldResetW >> 15) & 1);
    if ((OldW & GateMask) != 0 || (OldResetW & GateMask) == 0 ||
        (Pmc[0x038 / 4] & ((1U << 20) | (1U << 22))) != 0) {
      Print (L"STOP: gate/reset/domain state is not the verified baseline. No write made.\r\n");
      Print (L"Hold POWER for 12-15 seconds.\r\n");
      return;
    }

    Print (L"Enabling XUSB_GATE with XUSB_SRC reset asserted... ");
    Car[0x448 / 4] = GateMask;
    gBS->Stall (10);
    if ((Car[0x364 / 4] & GateMask) == 0) {
      Print (L"FAIL\r\n"); goto R40Restore;
    }
    Print (L"PASS\r\n");

    Print (L"Releasing XUSB_SRC reset only... ");
    Car[0x43C / 4] = GateMask;
    gBS->Stall (10);
    if ((Car[0x35C / 4] & GateMask) == 0) Print (L"PASS\r\n");
    else Print (L"FAIL\r\n");
    Print (L"Test CLK_ENB_W=%08x RST_DEV_W=%08x SRC_RESET=%d\r\n",
           Car[0x364 / 4], Car[0x35C / 4], (Car[0x35C / 4] >> 15) & 1);

R40Restore:
    Print (L"Re-asserting XUSB_SRC reset and restoring gate OFF... ");
    Car[0x438 / 4] = GateMask;
    gBS->Stall (10);
    Car[0x44C / 4] = GateMask;
    gBS->Stall (10);
    RestoreOk = (Car[0x364 / 4] == OldW && Car[0x35C / 4] == OldResetW);
    Print (RestoreOk ? L"PASS\r\n" : L"FAIL; hold POWER now.\r\n");
    Print (L"Restored CLK_ENB_W=%08x RST_DEV_W=%08x\r\n",
           Car[0x364 / 4], Car[0x35C / 4]);
    Print (L"No XUSB MMIO, PLL, power-domain, I2C, USB-C, SD, or Windows write.\r\n");
    Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
    return;
  }

R41Lifecycle:
  {
    volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
    volatile UINT32 *Pmc = (volatile UINT32 *)PMC_BASE;
    CONST UINTN SrcOffset[5] = { 0x600 / 4, 0x604 / 4, 0x608 / 4, 0x60C / 4, 0x610 / 4 };
    CONST UINT32 LinuxSrc[5] = {
      0x20000006U, 0xA0000002U, 0x40000000U, 0x20000006U, 0x64000006U
    };
    UINT32 OldSrc[5];
    UINT32 OldPllu = Car[0x0C0 / 4];
    UINT32 OldPlleBase = Car[0x0E8 / 4];
    UINT32 OldPlleMisc = Car[0x0EC / 4];
    UINT32 OldPlleAux = Car[0x48C / 4];
    UINT32 OldMbist = Car[0x3A0 / 4];
    UINT32 Value;
    BOOLEAN PlleLocked = FALSE;
    BOOLEAN CPower = FALSE, APower = FALSE, Gates = FALSE, ResetsReleased = FALSE;
    BOOLEAN RestoreOk = TRUE;

    for (I = 0; I < 5; I++) OldSrc[I] = Car[SrcOffset[I]];
    Print (L"PLLE=%08x/%08x/%08x PLLU=%08x\r\n",
           OldPlleBase, OldPlleMisc, OldPlleAux, OldPllu);
    Print (L"SRC before HOST=%08x FALCON=%08x FS=%08x DEV=%08x SS=%08x\r\n",
           OldSrc[0], OldSrc[1], OldSrc[2], OldSrc[3], OldSrc[4]);

    if ((Pmc[0x038 / 4] & ((1U << 20) | (1U << 22))) != 0 ||
        (Car[0x018 / 4] & ((1U << 25) | (1U << 31))) != 0 ||
        (Car[0x364 / 4] & ((1U << 15) | (1U << 28))) != 0 ||
        (Car[0x00C / 4] & (1U << 25)) == 0 ||
        (Car[0x35C / 4] & (1U << 28)) == 0 ||
        (OldPllu & ((1U << 30) | (1U << 27))) != ((1U << 30) | (1U << 27)) ||
        (OldPlleBase & (1U << 31)) != 0) {
      Print (L"STOP: initial power/clock/reset/PLL state is not the verified baseline.\r\n");
      gBS->FreePages (mFirmwareAddress, mFirmwarePages);
      mFirmwarePages = 0;
      Print (L"DMA firmware buffer released before stop.\r\n");
      Print (L"No write made. Hold POWER for 12-15 seconds.\r\n");
      return;
    }

    Print (L"Starting PLLE 100 MHz... ");
    Car[0x48C / 4] = OldPlleAux & ~(1U << 28);
    Car[0x0EC / 4] = OldPlleMisc & ~(1U << 13);
    gBS->Stall (5);
    Value = OldPlleBase & ~((0x1FU << 24) | (0xFFU << 8) | 0xFFU);
    Value |= (0x0EU << 24) | (0x7DU << 8) | 0x02U;
    Car[0x0E8 / 4] = Value;
    Value = Car[0x0EC / 4];
    Value |= (1U << 8);
    Value &= ~((3U << 6) | (3U << 2) | 1U);
    Car[0x0EC / 4] = Value;
    Car[0x0E8 / 4] = Car[0x0E8 / 4] | (1U << 31);
    for (I = 0; I < 250; I++) {
      if ((Car[0x0EC / 4] & (1U << 11)) != 0) { PlleLocked = TRUE; break; }
      gBS->Stall (1);
    }
    if (!PlleLocked) { Print (L"FAIL/TIMEOUT\r\n"); goto R35Restore; }
    Print (L"PASS (%d us)\r\n", (UINT32)I);

    Print (L"Enabling PLLU 480+48 MHz outputs... ");
    Car[0x0C0 / 4] = OldPllu | (1U << 22) | (1U << 25);
    gBS->Stall (10);
    if ((Car[0x0C0 / 4] & ((1U << 22) | (1U << 25))) != ((1U << 22) | (1U << 25))) {
      Print (L"FAIL\r\n"); goto R35Restore;
    }
    Print (L"PASS\r\n");

    Print (L"Programming five Ubuntu-reference XUSB sources... ");
    for (I = 0; I < 5; I++) Car[SrcOffset[I]] = LinuxSrc[I];
    gBS->Stall (10);
    for (I = 0; I < 5; I++) if (Car[SrcOffset[I]] != LinuxSrc[I]) {
      Print (L"FAIL at source %d\r\n", (UINT32)I); goto R35Restore;
    }
    Print (L"PASS\r\n");

    Print (L"Linux sequence 1/8: powering XUSBA SuperSpeed domain... ");
    if (!SetPowerPartition (20, TRUE)) { Print (L"FAIL/TIMEOUT\r\n"); goto R35Restore; }
    APower = TRUE; Print (L"PASS\r\n"); gBS->Stall (500000);
    Print (L"Linux sequence 2/8: enabling XUSB_GATE and SS clocks... ");
    Gates = TRUE;
    Car[0x448 / 4] = (1U << 15) | (1U << 28);
    gBS->Stall (500);
    if ((Car[0x364 / 4] & ((1U << 15) | (1U << 28))) != ((1U << 15) | (1U << 28))) {
      Print (L"FAIL\r\n"); goto R35Restore;
    }
    Print (L"PASS\r\n");
    Print (L"Linux sequence 3/8: removing XUSBA clamp... ");
    if (!RemovePartitionClamp (20)) { Print (L"FAIL/TIMEOUT\r\n"); goto R35Restore; }
    Print (L"PASS\r\n"); gBS->Stall (500000);
    Print (L"Linux sequence 4/8: releasing SS reset... ");
    ResetsReleased = TRUE;
    Car[0x43C / 4] = (1U << 28);
    gBS->Stall (500);
    if ((Car[0x35C / 4] & (1U << 28)) != 0) { Print (L"FAIL\r\n"); goto R35Restore; }
    Print (L"PASS\r\n");
    Print (L"Linux sequence 5/8: powering XUSBC host domain... ");
    if (!SetPowerPartition (22, TRUE)) { Print (L"FAIL/TIMEOUT\r\n"); goto R35Restore; }
    CPower = TRUE; Print (L"PASS\r\n"); gBS->Stall (500000);
    Print (L"Linux sequence 6/8: enabling HOST clock; DEV stays OFF... ");
    Car[0x330 / 4] = (1U << 25);
    gBS->Stall (500);
    if ((Car[0x018 / 4] & (1U << 25)) == 0 || (Car[0x018 / 4] & (1U << 31)) != 0) {
      Print (L"FAIL\r\n"); goto R35Restore;
    }
    Print (L"PASS\r\n");
    Print (L"Linux sequence 7/8: removing XUSBC clamp... ");
    if (!RemovePartitionClamp (22)) { Print (L"FAIL/TIMEOUT\r\n"); goto R35Restore; }
    Print (L"PASS\r\n"); gBS->Stall (500000);
    Print (L"Linux sequence 8/8: releasing HOST reset... ");
    Car[0x314 / 4] = (1U << 25);
    gBS->Stall (500);
    if ((Car[0x00C / 4] & (1U << 25)) != 0) { Print (L"FAIL\r\n"); goto R35Restore; }
    Print (L"PASS\r\n");
    {
      UINT32 PwrGate = Pmc[0x038 / 4];
      Print (L"R77 domain state: PMC PWRGATE=%08x XUSBA=%d XUSBC=%d\r\n",
             PwrGate, (PwrGate >> 20) & 1U, (PwrGate >> 22) & 1U);
      Print (L"R77 XUSBA/XUSBC domain readback: recorded\r\n");
    }

    Print (L"Applying reversible Tegra210 XUSB MBIST workaround... ");
    Car[0x3A0 / 4] = OldMbist | (1U << 30) | (1U << 31);
    gBS->Stall (1); Car[0x3A0 / 4] = OldMbist; gBS->Stall (1);
    if (Car[0x3A0 / 4] == OldMbist) Print (L"PASS\r\n");
    else { Print (L"FAIL\r\n"); RestoreOk = FALSE; }
    Print (L"ABOUT TO READ IPFS CONFIG offset 0x180...\r\n");
    gBS->Stall (500000);
    {
      volatile UINT32 *Ipfs = (volatile UINT32 *)XUSB_IPFS_BASE;
      UINT32 OldIpfsConfig = Ipfs[0x180 / 4];
      UINT32 TestIpfsConfig = OldIpfsConfig | 1U;
      Print (L"IPFS CONFIG before=%08x EN_FPCI=%d\r\n",
             OldIpfsConfig, OldIpfsConfig & 1U);
      Print (L"Setting IPFS_EN_FPCI bit 0 only... ");
      Ipfs[0x180 / 4] = TestIpfsConfig;
      gBS->Stall (10);
      if (Ipfs[0x180 / 4] == TestIpfsConfig) Print (L"PASS\r\n");
      else Print (L"FAIL readback=%08x\r\n", Ipfs[0x180 / 4]);
      Print (L"ABOUT TO READ FPCI offset 0x000...\r\n");
      gBS->Stall (500000);
      {
        volatile UINT32 *Fpci = (volatile UINT32 *)XUSB_FPCI_BASE;
        UINT32 Fpci000 = Fpci[0x000 / 4];
        UINT32 OldCfg1;
        UINT32 TestCfg1;
        UINT32 OldCfg4;
        UINT32 TestCfg4;
        Print (L"FPCI000=%08x expected=0FAC10DE %s\r\n",
               Fpci000, Fpci000 == 0x0FAC10DEU ? L"MATCH" : L"DIFF");
        Print (L"R77 FPCI context reads: CFG16=%08x CFG24=%08x AXI=%08x\r\n",
               Fpci[0x040 / 4], Fpci[0x060 / 4], Fpci[0x0F8 / 4]);
        Print (L"R77 ARU context: CTX=%08x HSPLS=%08x FSPLS=%08x SPD=%08x PP=%08x\r\n",
               Fpci[0x43C / 4], Fpci[0x478 / 4], Fpci[0x47C / 4],
               Fpci[0x480 / 4], Fpci[0x484 / 4]);
        Print (L"R77 FPCI context audit: PASS (no CSB access)\r\n");
        OldCfg4 = Fpci[0x010 / 4];
        TestCfg4 = (OldCfg4 & ~0xFFFF8000U) | (0x70090000U & 0xFFFF8000U);
        Print (L"CFG4 before=%08x target=%08x (BAR0=70090000)\r\n", OldCfg4, TestCfg4);
        Print (L"Writing CFG4 BAR0 field only... ");
        Fpci[0x010 / 4] = TestCfg4;
        gBS->Stall (10);
        if (Fpci[0x010 / 4] == TestCfg4) Print (L"PASS\r\n");
        else { Print (L"FAIL readback=%08x\r\n", Fpci[0x010 / 4]); RestoreOk = FALSE; goto R51RestoreCfg4; }
        OldCfg1 = Fpci[0x004 / 4];
        TestCfg1 = OldCfg1 | 7U;
        Print (L"CFG1 before=%08x target=%08x (IO+MEM+MASTER)\r\n", OldCfg1, TestCfg1);
        Print (L"Enabling CFG1 bits 0..2 only... ");
        Fpci[0x004 / 4] = TestCfg1;
        gBS->Stall (10);
        if (Fpci[0x004 / 4] == TestCfg1) Print (L"PASS\r\n");
        else { Print (L"FAIL readback=%08x\r\n", Fpci[0x004 / 4]); RestoreOk = FALSE; goto R52RestoreCfg1; }
        Print (L"ABOUT TO READ HCD 0x70090000 offset 0x000 ONCE...\r\n");
        gBS->Stall (500000);
        {
          volatile UINT32 *Hcd = (volatile UINT32 *)XUSB_HCD_BASE;
          UINT32 Hcd000 = Hcd[0x000 / 4];
          UINT32 OldIntrMask;
          UINT32 OldHysteresis;
          UINT32 CodeTag;
          UINT32 CodeSize;
          UINT32 CodeTagBlocks;
          UINT32 CodeSizeBlocks;
          UINT32 CodeBlocks;
          UINT32 L2ImemSize;
          UINT32 ImfillRange;
          UINT32 L2Result = 0;
          UINTN  Poll;
          volatile UINTN Delay;
          BOOLEAN L2Loaded = FALSE;
          Print (L"HCD READ RETURNED: HCD000=%08x expected=01000020 %s\r\n",
                 Hcd000, Hcd000 == 0x01000020U ? L"MATCH" : L"DIFF");
          OldIntrMask = Ipfs[0x188 / 4];
          OldHysteresis = Ipfs[0x1BC / 4];
          Print (L"Applying verified IPFS mask+hysteresis... ");
          Ipfs[0x188 / 4] = OldIntrMask | (1U << 16);
          Ipfs[0x1BC / 4] = 0x80U;
          gBS->Stall (10);
          if (Ipfs[0x188 / 4] == (OldIntrMask | (1U << 16)) &&
              Ipfs[0x1BC / 4] == 0x80U) Print (L"PASS\r\n");
          else { Print (L"FAIL\r\n"); RestoreOk = FALSE; goto R58RestoreIpfs; }
          Print (L"R82 sequential Linux power/reset audit: PASS\r\n");
          LogText (L"R82 staged Linux power/clock/reset sequence: PASS.\r\n");
          Print (L"R82 mailbox: CMD=%08x IN=%08x OUT=%08x OWNER=%08x SMI=%08x\r\n",
                 Fpci[0x0E4 / 4], Fpci[0x0E8 / 4], Fpci[0x0EC / 4],
                 Fpci[0x0F0 / 4], Fpci[0x428 / 4]);
          {
            CHAR16 LogBuffer[256];
            UnicodeSPrint (LogBuffer, sizeof (LogBuffer),
              L"Mailbox CMD=%08x IN=%08x OUT=%08x OWNER=%08x SMI=%08x\r\n",
              Fpci[0x0E4 / 4], Fpci[0x0E8 / 4], Fpci[0x0EC / 4], Fpci[0x0F0 / 4], Fpci[0x428 / 4]);
            LogText (LogBuffer);
          }
          Print (L"R82 Falcon source=%08x expected=A0000002 gate=%d src_reset=%d\r\n",
                 Car[0x604 / 4], (Car[0x364 / 4] >> 15) & 1U,
                 (Car[0x35C / 4] >> 15) & 1U);
          if (Car[0x604 / 4] == 0xA0000002U &&
              (Car[0x364 / 4] & (1U << 15)) != 0 &&
              (Car[0x35C / 4] & (1U << 15)) == 0) {
            Print (L"R82 Falcon clock-route audit: PASS (read only; no CSB access)\r\n");
            LogText (L"R82 Falcon clock-route audit: PASS.\r\n");
          } else {
            Print (L"R82 Falcon clock-route audit: FAIL; no CSB access made\r\n");
            LogText (L"R82 Falcon clock-route audit: FAIL.\r\n");
            RestoreOk = FALSE;
          }
          Print (L"R83 CSB write-return probe: page=0 FALC_DMACTL=0...\r\n");
          Fpci[0x41C / 4] = 0;
          gBS->Stall (10);
          Fpci[(0x800 + 0x10C) / 4] = 0;
          gBS->Stall (10);
          Print (L"R83 CSB FALC_DMACTL zero write returned: PASS\r\n");
          CodeTag = ReadLe32 ((UINT8 *)(UINTN)mFirmwareAddress + 8);
          CodeSize = ReadLe32 ((UINT8 *)(UINTN)mFirmwareAddress + 12);
          CodeTagBlocks = (CodeTag + 255U) / 256U;
          CodeSizeBlocks = (CodeSize + 255U) / 256U;
          CodeBlocks = CodeTagBlocks + CodeSizeBlocks;
          L2ImemSize = ((CodeTagBlocks & 0x3FFU) << 8) |
                       ((CodeSizeBlocks & 0xFFU) << 24);
          ImfillRange = (CodeTagBlocks & 0xFFFFU) |
                        ((CodeBlocks & 0xFFFFU) << 16);
          Print (L"R120 calculated Linux firmware load plan:\r\n");
          Print (L"CAR live U: CLK=%08x RESET=%08x HOST=%d DEV=%d\r\n",
                 Car[0x018 / 4], Car[0x00C / 4],
                 (Car[0x018 / 4] >> 25) & 1U, (Car[0x018 / 4] >> 31) & 1U);
          Print (L"CAR live W: CLK=%08x RESET=%08x GATE=%d SS=%d SRC_RST=%d\r\n",
                 Car[0x364 / 4], Car[0x35C / 4],
                 (Car[0x364 / 4] >> 15) & 1U, (Car[0x364 / 4] >> 28) & 1U,
                 (Car[0x35C / 4] >> 15) & 1U);
          Print (L"Linux refs U=03F04352 W=582080FD (compare relevant bits only)\r\n");
          Print (L"IPFS MSI: SIZE=%08x AXI=%08x FPCI=%08x\r\n",
                 Ipfs[0x0C0 / 4], Ipfs[0x0C4 / 4], Ipfs[0x0C8 / 4]);
          Print (L"IPFS MSI: VEC0=%08x ENVEC0=%08x\r\n",
                 Ipfs[0x100 / 4], Ipfs[0x140 / 4]);
          Print (L"IPFS CTRL: ERRMASK=%08x INTEN=%08x UFPCI=%08x FIFO=%08x\r\n",
                 Ipfs[0x184 / 4], Ipfs[0x198 / 4],
                 Ipfs[0x19C / 4], Ipfs[0x1DC / 4]);
          Print (L" ILOAD_ATTR=%08x ILOAD_BASE=%016lx\r\n",
                 (UINT32)mFirmwareSize, mFirmwareAddress + 256U);
          Print (L" L2IMEM_SIZE=%08x IMFILLCTL=%08x IMFILLRNG1=%08x\r\n",
                 L2ImemSize, CodeSizeBlocks, ImfillRange);
          Print (L" BOOTVEC=%08x STARTCPU=00000002\r\n", CodeTag);
          if ((((UINTN)mFirmwareAddress + 256U) & 0xFFU) == 0 &&
              (CodeTag & 0xFFU) == 0 && CodeSize != 0) {
            Print (L"R77 DMA/boot-vector alignment check: PASS\r\n");
          } else {
            Print (L"R77 DMA/boot-vector alignment check: FAIL\r\n");
            RestoreOk = FALSE;
          }
          Print (L"R127 preparing existing PadCTL ELPG + PEX_USB_UPHY release.\r\n");
          if (RestoreOk) {
            St->ConOut->ClearScreen (St->ConOut);
            volatile UINT32 *Padctl = (volatile UINT32 *)XUSB_PAD_BASE;
            UINT32 Elpg = Padctl[0x024 / 4];
            Print (L"R120 PADCTL base=%08x padctl+4=%08x ELPG=%08x clamp=%d early=%d vcore=%d\r\n",
                   ((volatile UINT32 *)XUSB_PAD_BASE)[0],
                   ((volatile UINT32 *)XUSB_PAD_BASE)[1],
                   Elpg, (Elpg >> 29) & 1, (Elpg >> 30) & 1, (Elpg >> 31) & 1);
            Padctl[0x024 / 4] = Elpg & ~(1U << 29);
            gBS->Stall (100);
            Padctl[0x024 / 4] &= ~(1U << 30);
            gBS->Stall (100);
            Padctl[0x024 / 4] &= ~(1U << 31);
            gBS->Stall (100);
            Print (L"R120 PadCTL ELPG released: %08x\r\n", Padctl[0x024 / 4]);
            volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
            UINT32 UpHyReset = Car[0x2A4 / 4];
            Print (L"R127 PEX_USB_UPHY reset Y=%08x bit13=%d\r\n", UpHyReset, (UpHyReset >> 13) & 1);
            Car[0x2A4 / 4] = UpHyReset & ~(1U << 13);
            gBS->Stall (100);
            Print (L"R127 PEX_USB_UPHY reset released Y=%08x bit13=%d\r\n",
                   Car[0x2A4 / 4], (Car[0x2A4 / 4] >> 13) & 1);
            Print (L"R127 read-only Tegra210 USB2 audit; no CSB read or USB command.\r\n");
            DumpXusbPowerState (L"R127 pre-USB2");
            Print (L"R127 FPCI CFG1=%08x CFG4=%08x IPFS_CFG=%08x MASK=%08x HYST=%08x\r\n",
                   Fpci[0x004 / 4], Fpci[0x010 / 4], Ipfs[0x180 / 4],
                   Ipfs[0x188 / 4], Ipfs[0x1BC / 4]);
            DumpXusbUsb2State ();
            RunR128Usb2PadWakeTest ();
          }
R58RestoreIpfs:
          Print (L"Restoring exact IPFS mask+hysteresis... ");
          Ipfs[0x1BC / 4] = OldHysteresis;
          Ipfs[0x188 / 4] = OldIntrMask;
          gBS->Stall (10);
          if (Ipfs[0x188 / 4] == OldIntrMask && Ipfs[0x1BC / 4] == OldHysteresis) Print (L"PASS\r\n");
          else { Print (L"FAIL; hold POWER now.\r\n"); RestoreOk = FALSE; }
        }
R52RestoreCfg1:
        Print (L"Restoring exact CFG1=%08x... ", OldCfg1);
        Fpci[0x004 / 4] = OldCfg1;
        gBS->Stall (10);
        if (Fpci[0x004 / 4] == OldCfg1) Print (L"PASS\r\n");
        else { Print (L"FAIL; hold POWER now.\r\n"); RestoreOk = FALSE; }
R51RestoreCfg4:
        Print (L"Restoring exact CFG4=%08x... ", OldCfg4);
        Fpci[0x010 / 4] = OldCfg4;
        gBS->Stall (10);
        if (Fpci[0x010 / 4] == OldCfg4) Print (L"PASS\r\n");
        else { Print (L"FAIL; hold POWER now.\r\n"); RestoreOk = FALSE; }
      }
      Print (L"Restoring exact IPFS CONFIG=%08x... ", OldIpfsConfig);
      Ipfs[0x180 / 4] = OldIpfsConfig;
      gBS->Stall (10);
      if (Ipfs[0x180 / 4] == OldIpfsConfig) Print (L"PASS\r\n");
      else { Print (L"FAIL; hold POWER now.\r\n"); RestoreOk = FALSE; }
    }
    Print (L"Holding live state for 2 seconds; no XUSB controller command...\r\n");
    gBS->Stall (2000000);

R35Restore:
    Print (L"Restoring in reverse order...\r\n");
    if (ResetsReleased) {
      Car[0x310 / 4] = (1U << 25); Car[0x438 / 4] = (1U << 28); gBS->Stall (500);
      if ((Car[0x00C / 4] & (1U << 25)) == 0 || (Car[0x35C / 4] & (1U << 28)) == 0) RestoreOk = FALSE;
    }
    if (Gates) {
      Car[0x44C / 4] = (1U << 15) | (1U << 28);
      Car[0x334 / 4] = (1U << 25); gBS->Stall (500);
      if ((Car[0x018 / 4] & (1U << 25)) != 0 ||
          (Car[0x364 / 4] & ((1U << 15) | (1U << 28))) != 0) RestoreOk = FALSE;
    }
    if (CPower && !SetPowerPartition (22, FALSE)) RestoreOk = FALSE;
    if (APower && !SetPowerPartition (20, FALSE)) RestoreOk = FALSE;
    for (I = 0; I < 5; I++) Car[SrcOffset[I]] = OldSrc[I];
    Car[0x0C0 / 4] = OldPllu;
    Car[0x0E8 / 4] = OldPlleBase; gBS->Stall (5);
    Car[0x0EC / 4] = OldPlleMisc; Car[0x48C / 4] = OldPlleAux; gBS->Stall (10);
    for (I = 0; I < 5; I++) if (Car[SrcOffset[I]] != OldSrc[I]) RestoreOk = FALSE;
    if (Car[0x0C0 / 4] != OldPllu || Car[0x0E8 / 4] != OldPlleBase ||
        Car[0x0EC / 4] != OldPlleMisc || Car[0x48C / 4] != OldPlleAux) RestoreOk = FALSE;
    if (mFirmwarePages != 0) {
      if (EFI_ERROR (gBS->FreePages (mFirmwareAddress, mFirmwarePages))) RestoreOk = FALSE;
      else Print (L"DMA firmware buffer released: PASS\r\n");
      mFirmwarePages = 0;
    }
    Print (RestoreOk ? L"R120 automatic restore: PASS\r\n" : L"R120 automatic restore: FAIL; hold POWER now.\r\n");
    Print (L"Read-only FALC_CPUCTL precheck; no L2 result read, L2 action, IMFILL, DMACTL, BOOTVEC, Falcon CPU start, controller command, I2C, USB-C, SD, or Windows write.\r\n");
    Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
    return;
  }

  {
    volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
    CONST UINTN Offset[5] = { 0x600 / 4, 0x604 / 4, 0x608 / 4, 0x60C / 4, 0x610 / 4 };
    CONST UINT32 LinuxValue[5] = {
      0x20000006U, 0xA0000002U, 0x40000000U, 0x20000006U, 0x64000006U
    };
    UINT32 Old[5];
    BOOLEAN WriteOk = TRUE;
    BOOLEAN RestoreOk = TRUE;

    for (I = 0; I < 5; I++) Old[I] = Car[Offset[I]];
    Print (L"Before HOST=%08x FALCON=%08x FS=%08x DEV=%08x SS=%08x\r\n",
           Old[0], Old[1], Old[2], Old[3], Old[4]);
    if ((Car[0x018 / 4] & ((1U << 25) | (1U << 31))) != 0 ||
        (Car[0x364 / 4] & (1U << 28)) != 0 ||
        ((((volatile UINT32 *)PMC_BASE)[0x038 / 4] & ((1U << 20) | (1U << 22))) != 0)) {
      Print (L"STOP: an XUSB gate or power domain was already ON. No source write made.\r\n");
      Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
      return;
    }
    Print (L"Writing Ubuntu-reference source values with all XUSB gates OFF... ");
    for (I = 0; I < 5; I++) Car[Offset[I]] = LinuxValue[I];
    gBS->Stall (10);
    for (I = 0; I < 5; I++) if (Car[Offset[I]] != LinuxValue[I]) WriteOk = FALSE;
    Print (WriteOk ? L"PASS\r\n" : L"FAIL\r\n");
    Print (L"Test   HOST=%08x FALCON=%08x FS=%08x DEV=%08x SS=%08x\r\n",
           Car[Offset[0]], Car[Offset[1]], Car[Offset[2]], Car[Offset[3]], Car[Offset[4]]);
    Print (L"Restoring exact original source values... ");
    for (I = 0; I < 5; I++) Car[Offset[I]] = Old[I];
    gBS->Stall (10);
    for (I = 0; I < 5; I++) if (Car[Offset[I]] != Old[I]) RestoreOk = FALSE;
    Print (RestoreOk ? L"PASS\r\n" : L"FAIL; hold POWER now.\r\n");
    Print (L"Restored HOST=%08x FALCON=%08x FS=%08x DEV=%08x SS=%08x\r\n",
           Car[Offset[0]], Car[Offset[1]], Car[Offset[2]], Car[Offset[3]], Car[Offset[4]]);
    Print (L"No XUSB MMIO, PLL, power-domain, reset, I2C, USB-C, SD, or Windows write.\r\n");
    Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
    return;
  }

  {
    volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
    UINT32 OldBase = Car[0x0E8 / 4];
    UINT32 OldMisc = Car[0x0EC / 4];
    UINT32 OldAux  = Car[0x48C / 4];
    UINT32 Value;
    BOOLEAN Locked = FALSE;

    Print (L"PLLE before: BASE=%08x MISC=%08x AUX=%08x\r\n", OldBase, OldMisc, OldAux);
    if ((OldBase & (1U << 31)) != 0) {
      Print (L"STOP: PLLE was already enabled. No PLLE write made.\r\n");
      Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
      return;
    }

    Print (L"Selecting XTAL and leaving PLLE IDDQ... ");
    Car[0x48C / 4] = OldAux & ~(1U << 28);
    Car[0x0EC / 4] = OldMisc & ~(1U << 13);
    gBS->Stall (5);
    Print (L"PASS\r\n");

    Print (L"Programming PLLE 100 MHz and waiting up to 250 us for LOCK... ");
    Value = OldBase;
    Value &= ~((0x1FU << 24) | (0xFFU << 8) | 0xFFU);
    Value |= (0x0EU << 24) | (0x7DU << 8) | 0x02U;
    Car[0x0E8 / 4] = Value;

    Value = Car[0x0EC / 4];
    Value |= (1U << 8);
    Value &= ~((3U << 6) | (3U << 2) | 1U);
    Car[0x0EC / 4] = Value;
    Car[0x0E8 / 4] = Car[0x0E8 / 4] | (1U << 31);

    for (I = 0; I < 250; I++) {
      if ((Car[0x0EC / 4] & (1U << 11)) != 0) {
        Locked = TRUE;
        break;
      }
      gBS->Stall (1);
    }
    Print (Locked ? L"PASS after %d us\r\n" : L"FAIL/TIMEOUT after %d us\r\n", (UINT32)I);
    Print (L"PLLE test: BASE=%08x MISC=%08x AUX=%08x\r\n",
           Car[0x0E8 / 4], Car[0x0EC / 4], Car[0x48C / 4]);

    Print (L"Restoring exact PLLE registers... ");
    Car[0x0E8 / 4] = OldBase;
    gBS->Stall (5);
    Car[0x0EC / 4] = OldMisc;
    Car[0x48C / 4] = OldAux;
    gBS->Stall (10);
    if (Car[0x0E8 / 4] == OldBase && Car[0x0EC / 4] == OldMisc &&
        Car[0x48C / 4] == OldAux)
      Print (L"PASS\r\n");
    else
      Print (L"FAIL; hold POWER now.\r\n");
    Print (L"PLLE lock test complete. No XUSB register or power domain was touched.\r\n");
    Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
    return;
  }

  DumpXusbPowerState (L"before");
  DumpXusb (L"before");
  Print (L"\r\n");

  {
    volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
    UINT32 PlluBase = Car[0x0C0 / 4];
    UINT32 PlluMisc0 = Car[0x0CC / 4];
    UINT32 PlleBase = Car[0x0E8 / 4];
    UINT32 PlleMisc0 = Car[0x0EC / 4];

    Print (L"PLLU BASE=%08x ENABLE=%d LOCK=%d USB480=%d USB60=%d USB48=%d\r\n",
           PlluBase, (PlluBase >> 30) & 1, (PlluBase >> 27) & 1,
           (PlluBase >> 22) & 1, (PlluBase >> 23) & 1, (PlluBase >> 25) & 1);
    Print (L"PLLU OUTA=%08x MISC0=%08x IDDQ=%d LOCK_EN=%d\r\n",
           Car[0x0C4 / 4], PlluMisc0, (PlluMisc0 >> 31) & 1,
           (PlluMisc0 >> 29) & 1);
    Print (L"PLLE BASE=%08x ENABLE=%d MISC0=%08x LOCK=%d LOCK_EN=%d AUX=%08x\r\n",
           PlleBase, (PlleBase >> 30) & 1, PlleMisc0,
           (PlleMisc0 >> 11) & 1, (PlleMisc0 >> 9) & 1, Car[0x48C / 4]);
    Print (L"XUSBIO_PLL_CFG0=%08x UTMIPLL_HW=%08x PLLU_HW=%08x XUSB_PLL_CFG0=%08x\r\n",
           Car[0x51C / 4], Car[0x52C / 4], Car[0x530 / 4], Car[0x534 / 4]);
    Print (L"XUSB SRC HOST=%08x FALCON=%08x FS=%08x DEV=%08x SS=%08x\r\n",
           Car[0x600 / 4], Car[0x604 / 4], Car[0x608 / 4],
           Car[0x60C / 4], Car[0x610 / 4]);
    Print (L"XUSB GATES U=%08x W=%08x RESETS U=%08x W=%08x MBIST_OVRC=%08x\r\n",
           Car[0x018 / 4], Car[0x364 / 4], Car[0x00C / 4],
           Car[0x35C / 4], Car[0x3A0 / 4]);
  }
  {
    volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
    UINT32 OldPllu = Car[0x0C0 / 4];
    UINT32 TestPllu = OldPllu | (1U << 22);

    if (((OldPllu >> 30) & 1) == 0 || ((OldPllu >> 27) & 1) == 0) {
      Print (L"STOP: PLLU is not enabled and locked. No PLLU write made.\r\n");
      Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
      return;
    } else {
      Print (L"Enabling and holding PLLU 480 MHz output: %08x -> %08x... ", OldPllu, TestPllu);
      Car[0x0C0 / 4] = TestPllu;
      gBS->Stall (10);
      if ((Car[0x0C0 / 4] & (1U << 22)) != 0) {
        mOldPlluBase = OldPllu;
        mPllu480Changed = TRUE;
        Print (L"PASS\r\n");
      } else {
        Car[0x0C0 / 4] = OldPllu;
        gBS->Stall (10);
        Print (L"FAIL; original PLLU value restored. Hold POWER for 12-15 seconds now.\r\n");
        return;
      }
    }
  }
  Print (L"Starting Linux-order three-domain lifecycle (XUSBA/B/C) with PLLU480 held ON.\r\n");

  if ((((volatile UINT32 *)PMC_BASE)[0x038 / 4] & ((1U << 22) | (1U << 21) | (1U << 20))) != 0) {
    Print (L"STOP: XUSBA, XUSBB, or XUSBC was already ON. No power-state write made.\r\n");
    RestorePllu480 ();
    Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
    return;
  }
  Print (L"Requesting XUSBC host power ON only... ");
  if (!SetPowerPartition (22, TRUE)) {
    RestorePllu480 ();
    Print (L"FAIL/TIMEOUT. Hold POWER for 12-15 seconds now.\r\n");
    return;
  }
  Print (L"PASS\r\n");
  gBS->Stall (500000);
  Print (L"Requesting XUSBB device power ON only... ");
  if (!SetPowerPartition (21, TRUE)) {
    Print (L"FAIL/TIMEOUT; restoring XUSBC power OFF... ");
    if (SetPowerPartition (22, FALSE)) Print (L"PASS\r\n");
    else Print (L"FAIL/TIMEOUT\r\n");
    RestorePllu480 ();
    Print (L"Hold POWER for 12-15 seconds now.\r\n");
    return;
  }
  Print (L"PASS\r\n");
  gBS->Stall (500000);
  Print (L"Requesting XUSBA SuperSpeed power ON only... ");
  if (!SetPowerPartition (20, TRUE)) {
    Print (L"FAIL/TIMEOUT; restoring XUSBC power OFF... ");
    if (SetPowerPartition (22, FALSE)) Print (L"PASS\r\n");
    else Print (L"FAIL/TIMEOUT\r\n");
    RestorePllu480 ();
    Print (L"Hold POWER for 12-15 seconds now.\r\n");
    return;
  }
  Print (L"PASS\r\n");
  gBS->Stall (500000);
  Print (L"Enabling HOST, DEV, and SS clocks before unclamping... ");
  ((volatile UINT32 *)CAR_BASE)[0x330 / 4] = (1U << 25) | (1U << 31);
  ((volatile UINT32 *)CAR_BASE)[0x448 / 4] = (1U << 28);
  gBS->Stall (500);
  if (((((volatile UINT32 *)CAR_BASE)[0x018 / 4] & ((1U << 25) | (1U << 31))) !=
       ((1U << 25) | (1U << 31))) ||
      ((((volatile UINT32 *)CAR_BASE)[0x364 / 4] & (1U << 28)) == 0)) {
    Print (L"FAIL; restoring clocks and power...\r\n");
    ((volatile UINT32 *)CAR_BASE)[0x44C / 4] = (1U << 28);
    ((volatile UINT32 *)CAR_BASE)[0x334 / 4] = (1U << 25) | (1U << 31);
    SetPowerPartition (20, FALSE);
    SetPowerPartition (22, FALSE);
    RestorePllu480 ();
    Print (L"Hold POWER for 12-15 seconds now.\r\n");
    return;
  }
  Print (L"PASS\r\n");
  gBS->Stall (500000);
  Print (L"Removing XUSBC host clamp only... ");
  if (!RemovePartitionClamp (22)) {
    Print (L"FAIL/TIMEOUT; restoring both power domains OFF...\r\n");
    ((volatile UINT32 *)CAR_BASE)[0x44C / 4] = (1U << 28);
    ((volatile UINT32 *)CAR_BASE)[0x334 / 4] = (1U << 25) | (1U << 31);
    SetPowerPartition (20, FALSE);
    SetPowerPartition (22, FALSE);
    RestorePllu480 ();
    Print (L"Hold POWER for 12-15 seconds now.\r\n");
    return;
  }
  Print (L"PASS\r\n");
  gBS->Stall (500000);
  Print (L"Removing XUSBA SuperSpeed clamp only... ");
  if (!RemovePartitionClamp (20)) {
    Print (L"FAIL/TIMEOUT; restoring both power domains OFF...\r\n");
    ((volatile UINT32 *)CAR_BASE)[0x44C / 4] = (1U << 28);
    ((volatile UINT32 *)CAR_BASE)[0x334 / 4] = (1U << 25) | (1U << 31);
    SetPowerPartition (20, FALSE);
    SetPowerPartition (22, FALSE);
    RestorePllu480 ();
    Print (L"Hold POWER for 12-15 seconds now.\r\n");
    return;
  }
  Print (L"PASS\r\n");
  gBS->Stall (500000);
  Print (L"Releasing HOST and SS resets only... ");
  ((volatile UINT32 *)CAR_BASE)[0x314 / 4] = (1U << 25);
  ((volatile UINT32 *)CAR_BASE)[0x43C / 4] = (1U << 28);
  gBS->Stall (500);
  if (((((volatile UINT32 *)CAR_BASE)[0x00C / 4] & (1U << 25)) != 0) ||
      ((((volatile UINT32 *)CAR_BASE)[0x35C / 4] & (1U << 28)) != 0)) {
    Print (L"FAIL; re-asserting HOST reset and restoring...\r\n");
    ((volatile UINT32 *)CAR_BASE)[0x310 / 4] = (1U << 25);
    ((volatile UINT32 *)CAR_BASE)[0x438 / 4] = (1U << 28);
    ((volatile UINT32 *)CAR_BASE)[0x44C / 4] = (1U << 28);
    ((volatile UINT32 *)CAR_BASE)[0x334 / 4] = (1U << 25) | (1U << 31);
    SetPowerPartition (20, FALSE);
    SetPowerPartition (22, FALSE);
    RestorePllu480 ();
    Print (L"Hold POWER for 12-15 seconds now.\r\n");
    return;
  }
  Print (L"PASS\r\n");
  gBS->Stall (500000);
  DumpXusbPowerState (L"HOST+SS live");
  Print (L"Applying Tegra210 XUSB MBIST workaround; reversible CAR write... ");
  {
    volatile UINT32 *Car = (volatile UINT32 *)CAR_BASE;
    UINT32 OldOvrC = Car[0x3A0 / 4];
    Car[0x3A0 / 4] = OldOvrC | (1U << 30) | (1U << 31);
    gBS->Stall (1);
    Car[0x3A0 / 4] = OldOvrC;
    gBS->Stall (1);
    if (Car[0x3A0 / 4] == OldOvrC) Print (L"PASS\r\n");
    else Print (L"FAIL; hold POWER after remaining restore steps.\r\n");
  }
  Print (L"ABOUT TO READ IPFS 0x70099000 offset 0x000 WITH PLLU480...\r\n");
  gBS->Stall (500000);
  {
    volatile UINT32 *Ipfs = (volatile UINT32 *)XUSB_IPFS_BASE;
    UINT32 Ipfs000 = Ipfs[0x000 / 4];
    Print (L"IPFS READ RETURNED: IPFS000=%08x\r\n", Ipfs000);
  }
  Print (L"Holding both domains live for 2 seconds; no XUSB command...\r\n");
  gBS->Stall (2000000);
  Print (L"Re-asserting HOST and SS resets... ");
  ((volatile UINT32 *)CAR_BASE)[0x310 / 4] = (1U << 25);
  ((volatile UINT32 *)CAR_BASE)[0x438 / 4] = (1U << 28);
  gBS->Stall (500);
  if (((((volatile UINT32 *)CAR_BASE)[0x00C / 4] & (1U << 25)) != 0) &&
      ((((volatile UINT32 *)CAR_BASE)[0x35C / 4] & (1U << 28)) != 0))
    Print (L"PASS\r\n");
  else
    Print (L"FAIL; hold POWER after remaining restore steps.\r\n");
  Print (L"Restoring HOST, DEV, and SS clocks OFF... ");
  ((volatile UINT32 *)CAR_BASE)[0x44C / 4] = (1U << 28);
  ((volatile UINT32 *)CAR_BASE)[0x334 / 4] = (1U << 25) | (1U << 31);
  gBS->Stall (500);
  if (((((volatile UINT32 *)CAR_BASE)[0x018 / 4] & ((1U << 25) | (1U << 31))) == 0) &&
      ((((volatile UINT32 *)CAR_BASE)[0x364 / 4] & (1U << 28)) == 0))
    Print (L"PASS\r\n");
  else
    Print (L"FAIL; hold POWER after automatic power restore.\r\n");
  Print (L"Restoring XUSBA SuperSpeed power OFF... ");
  if (!SetPowerPartition (20, FALSE)) {
    RestorePllu480 ();
    Print (L"FAIL/TIMEOUT. Hold POWER for 12-15 seconds now.\r\n");
    return;
  }
  Print (L"PASS\r\n");
  Print (L"Restoring XUSBB device power OFF... ");
  if (!SetPowerPartition (21, FALSE)) {
    RestorePllu480 ();
    Print (L"FAIL/TIMEOUT. Hold POWER for 12-15 seconds now.\r\n");
    return;
  }
  Print (L"PASS\r\n");
  Print (L"Restoring XUSBC host power OFF... ");
  if (!SetPowerPartition (22, FALSE)) {
    RestorePllu480 ();
    Print (L"FAIL/TIMEOUT. Hold POWER for 12-15 seconds now.\r\n");
    return;
  }
  Print (L"PASS\r\n");
  gBS->Stall (500000);
  DumpXusbPowerState (L"restored");
  if ((((volatile UINT32 *)PMC_BASE)[0x02C / 4] & ((1U << 22) | (1U << 21) | (1U << 20))) ==
      ((1U << 22) | (1U << 21) | (1U << 20)))
    Print (L"XUSBA/B/C clamps re-asserted automatically: PASS\r\n");
  else
    Print (L"WARNING: clamp bit did not re-assert. Hold POWER now.\r\n");
  Print (L"Restoring exact PLLU BASE=%08x... ", mOldPlluBase);
  RestorePllu480 ();
  if (((volatile UINT32 *)CAR_BASE)[0x0C0 / 4] == mOldPlluBase)
    Print (L"PASS\r\n");
  else
    Print (L"FAIL; hold POWER now.\r\n");
  Print (L"Resets/clocks restored. No XUSB command, I2C, USB-C, SD, or Windows write.\r\n");
  Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
  return;

  mOldCnfg = B[CNFG]; mOldIntMask = B[INT_MASK_REG]; mOldFifo = B[FIFO_CONTROL];
  if (!EnterPacketMode () || !ReadReg16 (BM_CONFIG1, &mOldConfig1, &S)) {
    Print (L"FAIL: cannot read CONFIG1. No BM92T write made. INT=0x%08x\r\n", S);
    RestoreController (); return;
  }
  Target = (UINT16)(mOldConfig1 & 0x3FFF);
  Print (L"CONFIG1 before=0x%04x; SPDSRC1/2 ON target=0x%04x\r\n", mOldConfig1, Target);
  if (mOldConfig1 == Target) {
    Print (L"Source paths were already ON; no write needed.\r\n");
  } else if (!WriteReg16 (BM_CONFIG1, Target, &S) ||
             !ReadReg16 (BM_CONFIG1, &Verify, &S) || Verify != Target) {
    Print (L"FAIL: write/read-back failed (0x%04x), INT=0x%08x\r\n", Verify, S);
    RestoreController (); return;
  } else {
    mBmChanged = TRUE;
    Print (L"CONFIG1 write/read-back: PASS (0x%04x)\r\n", Verify);
  }

  Print (L"\r\nCONNECT the OTG adapter + keyboard NOW. Monitoring");
  for (I = 0; I < 60; I++) {
    if (ReadReg16 (BM_STATUS1, &Status1, &S) &&
        ReadReg16 (BM_STATUS2, &Status2, &S) &&
        (((Status1 >> 7) & 1) != 0 || ((Status2 >> 13) & 1) != 0)) {
      Detected = TRUE;
      break;
    }
    if ((I % 10) == 9) Print (L".");
    gBS->Stall (500000);
  }
  Print (L"\r\n%s after %d ms\r\n", Detected ? L"DETECTED" : L"NOT DETECTED",
         Detected ? (UINT32)(I * 500) : 30000);
  Print (L"STATUS1=0x%04x INSERT=%d SRC_MODE=%d VSAFE=%d\r\n",
         Status1, (Status1 >> 7) & 1, (Status1 >> 12) & 1, (Status1 >> 10) & 1);
  Print (L"STATUS2=0x%04x OTG_INSERT=%d\r\n",
         Status2, (Status2 >> 13) & 1);

  if (Detected && ((Status1 >> 12) & 1) != 0 && ((Status2 >> 13) & 1) != 0) {
    Print (L"\r\nSafe OTG condition matched. Auditing BQ2419x...\r\n");
    if (!ReadReg8At (BQ_ADDR, BQ_REVISION, &BqRevision, &S) || BqRevision != 0x2F ||
        !ReadReg8At (BQ_ADDR, BQ_PWR_ON, &BqOriginal, &S)) {
      Print (L"FAIL: BQ2419x identity/PWR_ON read. VBUS remains OFF.\r\n");
    } else {
      BqTarget = (UINT8)((BqOriginal & 0xCE) | 0x20);
      Print (L"BQ REV=0x%02x PWR_ON=0x%02x; enabling 500 mA as 0x%02x... ",
             BqRevision, BqOriginal, BqTarget);
      if (WriteReg8At (BQ_ADDR, BQ_PWR_ON, BqTarget, &S)) {
        if (ReadReg8At (BQ_ADDR, BQ_PWR_ON, &BqVerify, &S) && BqVerify == BqTarget) {
          Print (L"PASS\r\n");
          BqChanged = TRUE;
          gBS->Stall (2000000);
          if (ReadReg8At (BQ_ADDR, BQ_SYS_STAT, &BqSys, &S))
            Print (L"BQ SYS_STAT after 2 sec=0x%02x\r\n", BqSys);
          DumpXusbPowerState (L"VBUS ");
          DumpXusb (L"VBUS ");
          Print (L"Light-check window: 10 seconds. XUSB registers are read only.\r\n");
          gBS->Stall (10000000);
        } else {
          Print (L"READ-BACK FAIL (0x%02x)\r\n", BqVerify);
        }
      } else {
        Print (L"WRITE FAIL\r\n");
      }
    }
  } else {
    Print (L"\r\nOTG safety condition not matched. BQ2419x was not changed.\r\n");
  }

  if (BqChanged) {
    Print (L"Restoring BQ2419x PWR_ON=0x%02x... ", BqOriginal);
    if (WriteReg8At (BQ_ADDR, BQ_PWR_ON, BqOriginal, &S) &&
        ReadReg8At (BQ_ADDR, BQ_PWR_ON, &BqVerify, &S) && BqVerify == BqOriginal)
      Print (L"PASS\r\n");
    else
      Print (L"FAIL (read-back=0x%02x)\r\n", BqVerify);
  }
  if (mBmChanged) {
    Print (L"Restoring BM92T CONFIG1=0x%04x... ", mOldConfig1);
    if (WriteReg16 (BM_CONFIG1, mOldConfig1, &S) &&
        ReadReg16 (BM_CONFIG1, &Verify, &S) && Verify == mOldConfig1)
      Print (L"PASS\r\n");
    else
      Print (L"FAIL (read-back=0x%04x)\r\n", Verify);
  }
  RestoreController ();
  Print (L"I2C controller restored. No XUSB register was changed.\r\n");
  Print (L"Take a clear photo, then hold POWER for 12-15 seconds.\r\n");
}

EFI_STATUS EFIAPI UsbPowerAuditEntryPoint (
  IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS E = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                   RunTest, SystemTable, &mStartEvent);
  if (EFI_ERROR (E)) return E;
  E = gBS->SetTimer (mStartEvent, TimerRelative, 10ULL * 1000ULL * 1000ULL * 10ULL);
  if (EFI_ERROR (E)) gBS->CloseEvent (mStartEvent);
  return E;
}
