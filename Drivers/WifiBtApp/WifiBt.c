/** @file
  wifibt - Nintendo Switch (Erista) Broadcom BCM4356 Bluetooth + Wi-Fi bring-up experiment.  DRAFT, UNTESTED.

  The Switch has ONE combo chip, BCM4356 (Wi-Fi: SDIO, Bluetooth: HCI over UART).  A real 802.11 / Bluetooth
  stack does not exist in EDK2, so this tool only does the part that can be done at shell level and that you
  need to find out first on real hardware:

    wifibt info                      GPIO H / pinmux / UART-D / SDMMC3 / PMIC 32 kHz state (read only)
    wifibt pwr                       PMIC 32 kHz out (GPIO4 alt function) + pinmux for BT/WiFi control pins
    wifibt bt [fw.hcd] [baud=N] [flow=0|1]
                                     power BT, UART-D @115200, HCI Reset / Read Version / Read BD_ADDR,
                                     optional Broadcom patchram (.hcd), optional baud switch (e.g. 3000000)
    wifibt wifi [base=0x700B0400]    power Wi-Fi, SDHCI bring-up, SDIO CMD5/3/7, CCCR dump, chip id, EROM dump
    wifibt wfw fw.bin nvram.txt [ram=0x180000] [size=0xC0000] [arm=0x18002000] [wrap=0x18102000]
                                     (after `wifi`) put the ARM CR4 in reset, download firmware + NVRAM, start it
    wifibt all                       pwr, bt, wifi
    wifibt off                       power both radios down

  Every step is logged to the screen AND to wifibt.log (flushed per line) BEFORE the risky access, so the last
  line of the log names the step that faulted (the idea: fix alignment/abort faults one by one on real HW).
  All register accesses are 32-bit aligned MMIO; SDIO data uses PIO (no DMA), so no cache/alignment surprises
  except the ones the SoC itself raises.

  Sources: fail0verflow/switch-linux tegra210-nintendo-switch.dts (BT on UART-D 0x70006300, GPIO H3/H4,
  pinmux), Linux drivers/net/wireless/broadcom/brcm80211/brcmfmac (chip.c, sdio.c), hekate (bdk uart/sdmmc/gpio).
  Firmware (brcmfmac4356-sdio.bin/.txt, BCM4356A2.hcd) is NOT included; copy it from your own Switch Linux.
  UNVERIFIED assumptions: Wi-Fi SDIO is on SDMMC3 (0x700B0400); GPIO H0=WIFI_EN H1=WIFI_RST (high = on);
  BCM4356 RAM base 0x180000 size 0xC0000; ARM CR4 core 0x18002000/wrapper 0x18102000.  `wifi` dumps the EROM so
  the real core layout can be read.
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
#include <Library/TimerLib.h>
#include <Library/I2cLib.h>
#include <Protocol/ShellParameters.h>
#include <Protocol/ShellDynamicCommand.h>

// ---------------------------------------------------------------------------------------------
#define CAR_BASE        0x60006000
#define CAR_RST_U       0x00C
#define CAR_ENB_U       0x018
#define CAR_RST_U_SET   0x310
#define CAR_RST_U_CLR   0x314
#define CAR_ENB_U_SET   0x330
#define CAR_SRC_UARTD   0x1C0
#define CAR_SRC_SDMMC3  0x1BC
#define CLK_U_UARTD     (1U << 1)
#define CLK_U_SDMMC3    (1U << 5)

#define PINMUX_BASE     0x70003000
#define PM_SDMMC3_CLK   0x1C
#define PM_SDMMC3_CMD   0x20
#define PM_SDMMC3_DAT0  0x24
#define PM_UART4_TX     0x114
#define PM_UART4_RX     0x118
#define PM_UART4_RTS    0x11C
#define PM_UART4_CTS    0x120
#define PM_WIFI_EN      0x1B4
#define PM_WIFI_RST     0x1B8
#define PM_WIFI_WAKE    0x1BC
#define PM_AP_WAKE_BT   0x1C0
#define PM_BT_RST       0x1C4
#define PM_BT_WAKE_AP   0x1C8
#define PM_INPUT        0x40
#define PM_TRISTATE     0x10
#define PM_PULLDOWN     0x04
#define PM_PULLUP       0x08

// GPIO port H = bank 1, port 3
#define GPIO_BASE       0x6000D000
#define GPIO_H_CNF      0x10C
#define GPIO_H_OE       0x11C
#define GPIO_H_OUT      0x12C
#define GPIO_H_IN       0x13C
#define H0              0x01      // WIFI_EN
#define H1              0x02      // WIFI_RST
#define H2              0x04      // WIFI_WAKE_AP (in)
#define H3              0x08      // AP_WAKE_BT (BT_DEV_WAKE, active low)
#define H4              0x10      // BT_RST (BT_REG_ON, high = on)
#define H5              0x20      // BT_WAKE_AP (in)

#define UARTD_BASE      0x70006300
#define U_THR           0x00
#define U_IER           0x04
#define U_FCR           0x08
#define U_LCR           0x0C
#define U_MCR           0x10
#define U_LSR           0x14
#define U_SPR           0x1C

#define PMIC_ADDR       0x3C
#define PMIC_CNFG1_32K  0x03
#define PMIC_GPIO4      0x3A
#define PMIC_AME_GPIO   0x40

// SDHCI (standard + Tegra vendor)
#define SD_BLKSZ        0x04
#define SD_BLKCNT       0x06
#define SD_ARG          0x08
#define SD_XFER         0x0C
#define SD_CMD          0x0E
#define SD_RESP         0x10
#define SD_DATA         0x20
#define SD_PRES         0x24
#define SD_HOSTCTL      0x28
#define SD_PWR          0x29
#define SD_CLK          0x2C
#define SD_TMO          0x2E
#define SD_SWRST        0x2F
#define SD_INTSTA       0x30
#define SD_INTSTAEN     0x34
#define SD_INTSIGEN     0x38
#define SD_HOSTCTL2     0x3C
#define SD_CAP          0x40
#define SD_VEN_CLK      0x100
#define SD_VEN_TRIM     0x1AC
#define SD_VEN_PAD      0x1E0
#define SD_VEN_ACAL     0x1E4
#define SD_VEN_ACSTA    0x1EC
#define SD_VEN_IOSPARE  0x1F0

// ---------------------------------------------------------------------------------------------
STATIC SHELL_FILE_HANDLE  mLog = NULL;
STATIC UINTN              mSd   = 0x700B0400;
STATIC BOOLEAN            mSdUp = FALSE;
STATIC UINT32             mFlow = 0;

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
  Print (L"%s\n", Buf);
  if (mLog != NULL) {
    Len = AsciiSPrint (Out, sizeof (Out), "%s\r\n", Buf);
    ShellWriteFile (mLog, &Len, Out);
    ShellFlushFile (mLog);
  }
}

STATIC UINT32 Rd (UINTN B, UINTN O) { return MmioRead32 (B + O); }
STATIC VOID   Wr (UINTN B, UINTN O, UINT32 V) { MmioWrite32 (B + O, V); }
STATIC VOID   Us (UINTN N) { MicroSecondDelay (N); }
STATIC VOID   Ms (UINTN N) { MicroSecondDelay (N * 1000); }

// 8/16-bit SDHCI registers: always access through the containing aligned 32-bit word.
STATIC
UINT32
SdR8 (UINTN O)
{
  return (Rd (mSd, O & ~3U) >> ((O & 3) * 8)) & 0xFF;
}

STATIC
UINT32
SdR16 (UINTN O)
{
  return (Rd (mSd, O & ~3U) >> ((O & 2) * 8)) & 0xFFFF;
}

STATIC
VOID
SdW8 (UINTN O, UINT32 V)
{
  UINT32  Sh = (O & 3) * 8;
  UINT32  W  = Rd (mSd, O & ~3U);

  Wr (mSd, O & ~3U, (W & ~(0xFFU << Sh)) | ((V & 0xFF) << Sh));
}

STATIC
VOID
SdW16 (UINTN O, UINT32 V)
{
  UINT32  Sh = (O & 2) * 8;
  UINT32  W  = Rd (mSd, O & ~3U);

  Wr (mSd, O & ~3U, (W & ~(0xFFFFU << Sh)) | ((V & 0xFFFF) << Sh));
}

// ---------------------------------------------------------------------------------------------
// GPIO / pinmux / PMIC
// ---------------------------------------------------------------------------------------------
STATIC
VOID
GpioH (
  IN UINT32  Pins,
  IN UINT32  Out,      // value
  IN BOOLEAN Drive     // output?
  )
{
  Wr (GPIO_BASE, GPIO_H_CNF, Rd (GPIO_BASE, GPIO_H_CNF) | Pins);
  if (Drive) {
    Wr (GPIO_BASE, GPIO_H_OUT, (Rd (GPIO_BASE, GPIO_H_OUT) & ~Pins) | (Out ? Pins : 0));
    Wr (GPIO_BASE, GPIO_H_OE, Rd (GPIO_BASE, GPIO_H_OE) | Pins);
  } else {
    Wr (GPIO_BASE, GPIO_H_OE, Rd (GPIO_BASE, GPIO_H_OE) & ~Pins);
  }
  (VOID)Rd (GPIO_BASE, GPIO_H_CNF);
}

STATIC
VOID
PinsRadio (
  VOID
  )
{
  Wr (PINMUX_BASE, PM_WIFI_EN,   0);
  Wr (PINMUX_BASE, PM_WIFI_RST,  0);
  Wr (PINMUX_BASE, PM_WIFI_WAKE, PM_INPUT | PM_TRISTATE | PM_PULLDOWN);
  Wr (PINMUX_BASE, PM_AP_WAKE_BT, 0);
  Wr (PINMUX_BASE, PM_BT_RST,     0);
  Wr (PINMUX_BASE, PM_BT_WAKE_AP, PM_INPUT | PM_TRISTATE);
  // UART-D (func 0 = UARTD on uart4_*): TX/RTS outputs, RX/CTS inputs
  Wr (PINMUX_BASE, PM_UART4_TX,  0);
  Wr (PINMUX_BASE, PM_UART4_RX,  PM_INPUT | PM_TRISTATE);
  Wr (PINMUX_BASE, PM_UART4_RTS, 0);
  Wr (PINMUX_BASE, PM_UART4_CTS, PM_INPUT | PM_TRISTATE);
  (VOID)Rd (PINMUX_BASE, PM_UART4_CTS);
}

STATIC
VOID
Step32k (
  IN BOOLEAN  Set
  )
{
  UINT8  Ame;
  UINT8  Cfg;
  UINT8  G4;

  Log (L"pmic: reading MAX77620 (I2C5 0x3C)");
  i2c_init (I2C_5);
  Ame = i2c_recv_byte (I2C_5, PMIC_ADDR, PMIC_AME_GPIO);
  G4  = i2c_recv_byte (I2C_5, PMIC_ADDR, PMIC_GPIO4);
  Cfg = i2c_recv_byte (I2C_5, PMIC_ADDR, PMIC_CNFG1_32K);
  Log (L"pmic: AME_GPIO=%02x GPIO4=%02x CNFG1_32K=%02x (32k-out1 = GPIO4 alt fn = AME bit4)", Ame, G4, Cfg);
  if (Set && !(Ame & 0x10)) {
    Log (L"pmic: setting AME_GPIO bit4 (32 kHz out1 -> BCM4356 LPO)");
    i2c_send_byte (I2C_5, PMIC_ADDR, PMIC_AME_GPIO, Ame | 0x10);
    Log (L"pmic: AME_GPIO=%02x", i2c_recv_byte (I2C_5, PMIC_ADDR, PMIC_AME_GPIO));
  }
}

STATIC
VOID
StepInfo (
  VOID
  )
{
  Log (L"info: GPIO H CNF=%02x OE=%02x OUT=%02x IN=%02x", Rd (GPIO_BASE, GPIO_H_CNF) & 0xFF, Rd (GPIO_BASE, GPIO_H_OE) & 0xFF,
       Rd (GPIO_BASE, GPIO_H_OUT) & 0xFF, Rd (GPIO_BASE, GPIO_H_IN) & 0xFF);
  Log (L"info: pinmux WIFI_EN=%02x WIFI_RST=%02x WIFI_WAKE=%02x BT_DEVWAKE=%02x BT_RST=%02x BT_HOSTWAKE=%02x",
       Rd (PINMUX_BASE, PM_WIFI_EN), Rd (PINMUX_BASE, PM_WIFI_RST), Rd (PINMUX_BASE, PM_WIFI_WAKE),
       Rd (PINMUX_BASE, PM_AP_WAKE_BT), Rd (PINMUX_BASE, PM_BT_RST), Rd (PINMUX_BASE, PM_BT_WAKE_AP));
  Log (L"info: pinmux UART4 tx=%02x rx=%02x rts=%02x cts=%02x", Rd (PINMUX_BASE, PM_UART4_TX), Rd (PINMUX_BASE, PM_UART4_RX),
       Rd (PINMUX_BASE, PM_UART4_RTS), Rd (PINMUX_BASE, PM_UART4_CTS));
  Log (L"info: CAR ENB_U=%08x RST_U=%08x (UARTD bit1, SDMMC3 bit5) SRC_UARTD=%08x SRC_SDMMC3=%08x",
       Rd (CAR_BASE, CAR_ENB_U), Rd (CAR_BASE, CAR_RST_U), Rd (CAR_BASE, CAR_SRC_UARTD), Rd (CAR_BASE, CAR_SRC_SDMMC3));
  Step32k (FALSE);
}

STATIC
VOID
StepPwr (
  VOID
  )
{
  Log (L"pwr: pinmux");
  PinsRadio ();
  Step32k (TRUE);
  Log (L"pwr: done");
}

// ---------------------------------------------------------------------------------------------
// UART-D + HCI
// ---------------------------------------------------------------------------------------------
STATIC
VOID
UartInit (
  IN UINT32  Baud,
  IN UINT32  Flow
  )
{
  UINT32  Div;
  UINT32  Src;

  // clock: enable + release reset
  Wr (CAR_BASE, CAR_ENB_U_SET, CLK_U_UARTD);
  Wr (CAR_BASE, CAR_RST_U_SET, CLK_U_UARTD);
  Us (10);
  Src = Rd (CAR_BASE, CAR_SRC_UARTD) & 0xE0000000;
  if (Baud == 3000000) {
    Wr (CAR_BASE, CAR_SRC_UARTD, Src | (1U << 24) | 15);      // 408 / 8.5 = 48 MHz
    Div = 1;
  } else if (Baud == 1000000) {
    Wr (CAR_BASE, CAR_SRC_UARTD, Src | (1U << 24) | 49);      // 408 / 25.5 = 16 MHz
    Div = 1;
  } else {
    Wr (CAR_BASE, CAR_SRC_UARTD, Src | 2);
    Div = (8 * Baud + 408000000) / (16 * Baud);
  }
  Wr (CAR_BASE, CAR_RST_U_CLR, CLK_U_UARTD);
  Us (10);
  Wr (UARTD_BASE, U_IER, 0);
  Wr (UARTD_BASE, U_LCR, 0x83);
  Wr (UARTD_BASE, U_THR, Div & 0xFF);
  Wr (UARTD_BASE, U_IER, (Div >> 8) & 0xFF);
  Wr (UARTD_BASE, U_LCR, (Baud > 1000000 ? 0x4 : 0) | 0x3);    // 8N1 (2 stop > 1 Mbaud, like hekate; Linux DTS says 1 stop)
  (VOID)Rd (UARTD_BASE, U_SPR);
  Wr (UARTD_BASE, U_FCR, 1);
  Us (20);
  Wr (UARTD_BASE, U_MCR, 0);
  Us (100);
  Wr (UARTD_BASE, U_FCR, 7);
  Wr (UARTD_BASE, U_MCR, Flow ? 0x60 : 0);                      // RTS_EN | CTS_EN
  Us (100);
  Log (L"uart: %d baud div=%d flow=%d LSR=%02x", Baud, Div, Flow, Rd (UARTD_BASE, U_LSR) & 0xFF);
}

STATIC
VOID
UartTx (
  IN CONST UINT8  *B,
  IN UINTN        N
  )
{
  UINTN  I;
  UINTN  T;

  for (I = 0; I < N; I++) {
    for (T = 0; T < 200000 && !(Rd (UARTD_BASE, U_LSR) & 0x20); T++) {
      Us (10);
    }
    Wr (UARTD_BASE, U_THR, B[I]);
  }
  for (T = 0; T < 200000 && !(Rd (UARTD_BASE, U_LSR) & 0x40); T++) {
    Us (10);
  }
}

// returns TRUE and a byte, FALSE on timeout (microseconds)
STATIC
BOOLEAN
UartRx (
  OUT UINT8  *B,
  IN  UINTN  TmoUs
  )
{
  UINTN  T;

  for (T = 0; T <= TmoUs; T += 20) {
    if (Rd (UARTD_BASE, U_LSR) & 1) {
      *B = (UINT8)Rd (UARTD_BASE, U_THR);
      return TRUE;
    }
    Us (20);
  }
  return FALSE;
}

//
// Send one HCI command, wait for its Command Complete. Out = event parameters after the 3-byte
// (num packets, opcode) header. Returns number of bytes in Out, or -1 on timeout/garbage.
//
STATIC
INTN
HciCmd (
  IN  UINT16       Op,
  IN  CONST UINT8  *P,
  IN  UINT8        PLen,
  OUT UINT8        *Out,
  IN  UINTN        OutMax
  )
{
  UINT8  Pkt[4 + 255];
  UINT8  B;
  UINT8  Ev[260];
  UINT8  Code;
  UINT8  Len;
  UINTN  I;
  UINTN  Round;
  UINTN  Dummy;

  Pkt[0] = 1;
  Pkt[1] = (UINT8)Op;
  Pkt[2] = (UINT8)(Op >> 8);
  Pkt[3] = PLen;
  if (PLen) {
    CopyMem (&Pkt[4], P, PLen);
  }
  UartTx (Pkt, 4 + (UINTN)PLen);

  for (Round = 0; Round < 8; Round++) {
    if (!UartRx (&B, 1000000)) {
      return -1;
    }
    if (B != 4) {
      Log (L"hci: stray byte %02x", B);
      continue;
    }
    if (!UartRx (&Code, 100000) || !UartRx (&Len, 100000)) {
      return -1;
    }
    for (I = 0; I < Len; I++) {
      if (!UartRx (&Ev[I], 100000)) {
        return -1;
      }
    }
    if (Code == 0x0E && Len >= 3 && (Ev[1] | (Ev[2] << 8)) == Op) {
      Dummy = Len - 3;
      if (Dummy > OutMax) {
        Dummy = OutMax;
      }
      CopyMem (Out, &Ev[3], Dummy);
      return (INTN)(Len - 3);
    }
    if (Code == 0x0F && Len >= 4 && (Ev[2] | (Ev[3] << 8)) == Op && Ev[0] != 0) {
      Log (L"hci: command status error %02x", Ev[0]);
      return -1;
    }
    // other events: ignore
  }
  return -1;
}

STATIC
EFI_STATUS
ReadFile (
  IN  CONST CHAR16  *Name,
  OUT UINT8         **Buf,
  OUT UINTN         *Size
  )
{
  SHELL_FILE_HANDLE  H;
  UINT64             Sz = 0;
  EFI_STATUS         S;
  UINTN              N;

  S = ShellOpenFileByName (Name, &H, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (S)) {
    Log (L"file: cannot open %s (%r)", Name, S);
    return S;
  }
  ShellGetFileSize (H, &Sz);
  *Buf = AllocatePool ((UINTN)Sz + 4);
  if (*Buf == NULL) {
    ShellCloseFile (&H);
    return EFI_OUT_OF_RESOURCES;
  }
  N = (UINTN)Sz;
  S = ShellReadFile (H, &N, *Buf);
  ShellCloseFile (&H);
  *Size = N;
  Log (L"file: %s %d bytes", Name, N);
  return S;
}

STATIC
VOID
StepBt (
  IN CONST CHAR16  *Hcd,
  IN UINT32        NewBaud
  )
{
  UINT8   R[64];
  INTN    N;
  UINT8   Fw[8];
  UINT8   *H = NULL;
  UINTN   HSize = 0;
  UINTN   Off;
  UINTN   Cnt = 0;
  UINT16  Op;
  UINT8   L;

  Log (L"bt: pinmux + GPIO (H4=BT_REG_ON low->high, H3 dev-wake low)");
  PinsRadio ();
  Step32k (TRUE);
  GpioH (H4, 0, TRUE);
  GpioH (H3, 0, TRUE);
  Ms (50);
  GpioH (H4, 1, TRUE);
  Ms (150);
  Log (L"bt: UART-D");
  UartInit (115200, mFlow);

  Log (L"bt: HCI Reset");
  N = HciCmd (0x0C03, NULL, 0, R, sizeof (R));
  Log (L"bt: reset -> %d status=%02x", N, N > 0 ? R[0] : 0xFF);
  if (N < 0) {
    Log (L"bt: no answer. Try `wifibt bt flow=1`, check H4/H3 polarity (info), or that the 32 kHz clock runs.");
    return;
  }
  N = HciCmd (0x1001, NULL, 0, R, sizeof (R));
  if (N >= 9) {
    Log (L"bt: version: status=%02x hci=%d rev=%04x lmp=%d mfr=%04x(0x000F=Broadcom) lmpsub=%04x", R[0], R[1], R[2] | (R[3] << 8), R[4],
         R[5] | (R[6] << 8), R[7] | (R[8] << 8));
  }
  N = HciCmd (0x1009, NULL, 0, R, sizeof (R));
  if (N >= 7) {
    Log (L"bt: BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x", R[6], R[5], R[4], R[3], R[2], R[1]);
  }
  N = HciCmd (0x0C14, NULL, 0, R, sizeof (R));
  if (N >= 2) {
    R[N < 63 ? N : 63] = 0;
    Log (L"bt: local name: %a", (CHAR8 *)&R[1]);
  }

  if (Hcd != NULL && !EFI_ERROR (ReadFile (Hcd, &H, &HSize))) {
    Log (L"bt: patchram: download minidriver");
    N = HciCmd (0xFC2E, NULL, 0, R, sizeof (R));
    Log (L"bt: minidriver -> %d", N);
    Ms (50);
    Off = 0;
    while (Off + 3 <= HSize) {
      Op = (UINT16)(H[Off] | (H[Off + 1] << 8));
      L  = H[Off + 2];
      if (Off + 3 + L > HSize) {
        break;
      }
      N = HciCmd (Op, &H[Off + 3], L, R, sizeof (R));
      if (N < 0 || (N > 0 && R[0] != 0)) {
        Log (L"bt: patch record %d (op %04x len %d) failed (%d)", Cnt, Op, L, N);
        break;
      }
      Cnt++;
      Off += 3 + L;
    }
    Log (L"bt: patchram sent %d records (%d/%d bytes)", Cnt, Off, HSize);
    FreePool (H);
    Ms (250);
    N = HciCmd (0x0C03, NULL, 0, R, sizeof (R));
    Log (L"bt: reset after patch -> %d", N);
    N = HciCmd (0x1001, NULL, 0, R, sizeof (R));
    if (N >= 9) {
      Log (L"bt: version now: rev=%04x mfr=%04x lmpsub=%04x", R[2] | (R[3] << 8), R[5] | (R[6] << 8), R[7] | (R[8] << 8));
    }
  }

  if (NewBaud != 0) {
    Fw[0] = 0;
    Fw[1] = 0;
    Fw[2] = (UINT8)NewBaud;
    Fw[3] = (UINT8)(NewBaud >> 8);
    Fw[4] = (UINT8)(NewBaud >> 16);
    Fw[5] = (UINT8)(NewBaud >> 24);
    N = HciCmd (0xFC18, Fw, 6, R, sizeof (R));
    Log (L"bt: set baud %d -> %d", NewBaud, N);
    Ms (50);
    UartInit (NewBaud, mFlow);
    N = HciCmd (0x0C03, NULL, 0, R, sizeof (R));
    Log (L"bt: reset @%d -> %d", NewBaud, N);
  }
  Log (L"bt: done (no HCI transport/stack is registered with UEFI; this only proves the link)");
}


// ---------------------------------------------------------------------------------------------
// Bluetooth scan: classic Inquiry (with RSSI/EIR) + LE active scan. Proves the controller works
// end to end (RF, firmware, HCI); pairing/ACL are NOT implemented.
// ---------------------------------------------------------------------------------------------
STATIC
BOOLEAN
HciReadEv (
  OUT UINT8  *Code,
  OUT UINT8  *Ev,
  OUT UINT8  *Len,
  IN  UINTN  TmoUs
  )
{
  UINT8  B;
  UINTN  I;

  if (!UartRx (&B, TmoUs)) {
    return FALSE;
  }
  if (B != 4) {
    return FALSE;                                   // stray byte: caller just retries
  }
  if (!UartRx (Code, 100000) || !UartRx (Len, 100000)) {
    return FALSE;
  }
  for (I = 0; I < *Len; I++) {
    if (!UartRx (&Ev[I], 100000)) {
      return FALSE;
    }
  }
  return TRUE;
}

#define BT_SEEN_MAX  48
STATIC UINT8   mSeen[BT_SEEN_MAX][7];
STATIC UINTN   mSeenN = 0;

STATIC
BOOLEAN
BtSeen (
  IN CONST UINT8  *Addr,
  IN UINT8        Kind
  )
{
  UINTN  I;

  for (I = 0; I < mSeenN; I++) {
    if (CompareMem (mSeen[I], Addr, 6) == 0 && mSeen[I][6] == Kind) {
      return TRUE;
    }
  }
  if (mSeenN < BT_SEEN_MAX) {
    CopyMem (mSeen[mSeenN], Addr, 6);
    mSeen[mSeenN][6] = Kind;
    mSeenN++;
  }
  return FALSE;
}

// EIR / advertising data: find the (shortened or complete) local name, copy as ASCII.
STATIC
VOID
BtName (
  IN  CONST UINT8  *D,
  IN  UINTN        N,
  OUT CHAR8        *Out,
  IN  UINTN        OutMax
  )
{
  UINTN  I = 0;
  UINTN  L;
  UINTN  K;

  Out[0] = 0;
  while (I + 1 < N) {
    L = D[I];
    if (L == 0 || I + 1 + L > N) {
      break;
    }
    if (D[I + 1] == 0x08 || D[I + 1] == 0x09) {
      for (K = 0; K + 1 < L && K + 1 < OutMax; K++) {
        Out[K] = (D[I + 2 + K] >= 0x20 && D[I + 2 + K] < 0x7F) ? (CHAR8)D[I + 2 + K] : '?';
      }
      Out[K] = 0;
      return;
    }
    I += 1 + L;
  }
}

STATIC
VOID
BtReport (
  IN CONST CHAR16  *Kind,
  IN CONST UINT8   *Addr,        // little endian as on the wire
  IN UINT8         AddrKind,
  IN INT8          Rssi,
  IN UINT32        Cod,
  IN CONST UINT8   *Data,
  IN UINTN         DataLen
  )
{
  CHAR8  Name[40];

  if (BtSeen (Addr, AddrKind)) {
    return;
  }
  BtName (Data, DataLen, Name, sizeof (Name));
  Log (L"scan: %s %02x:%02x:%02x:%02x:%02x:%02x type=%d rssi=%d cod=%06x name='%a'", Kind, Addr[5], Addr[4], Addr[3], Addr[2], Addr[1], Addr[0],
       AddrKind, (INTN)Rssi, Cod, Name);
}

STATIC
VOID
BtScan (
  IN UINT32  Sec,
  IN UINT32  Baud
  )
{
  UINT8    R[64];
  UINT8    Ev[260];
  UINT8    Code;
  UINT8    Len;
  UINT8    P[8];
  UINTN    T;
  UINTN    I;
  UINTN    Off;
  UINTN    Ticks;
  INTN     N;
  UINT32   Found;

  if (Sec == 0 || Sec > 60) {
    Sec = 10;
  }
  mSeenN = 0;
  Log (L"scan: UART-D @%d, reset controller (run `wifibt bt [file.hcd]` before so the patchram is loaded)", Baud ? Baud : 115200);
  UartInit (Baud ? Baud : 115200, mFlow);
  N = HciCmd (0x0C03, NULL, 0, R, sizeof (R));
  if (N < 0) {
    Log (L"scan: no HCI answer - run `wifibt bt` first (power-up + patchram)");
    return;
  }
  P[0] = 2;                                                  // inquiry mode: extended (EIR)
  N = HciCmd (0x0C45, P, 1, R, sizeof (R));
  Log (L"scan: write inquiry mode -> %d status=%02x", N, N > 0 ? R[0] : 0xFF);
  P[0] = 0xFF; P[1] = 0xFF; P[2] = 0xFF; P[3] = 0xFF; P[4] = 0xFF; P[5] = 0xFF; P[6] = 0xFF; P[7] = 0x3F;   // default event mask (+LE meta)
  HciCmd (0x0C01, P, 8, R, sizeof (R));
  P[0] = 0x01; P[1] = 0x00; P[2] = 0x00; P[3] = 0x00; P[4] = 0x00; P[5] = 0x00; P[6] = 0x00; P[7] = 0x20;   // LE events (le meta bit 61)
  HciCmd (0x2001, P, 8, R, sizeof (R));

  // ---- classic inquiry (GIAC 9E8B33), length in 1.28 s units
  P[0] = 0x33; P[1] = 0x8B; P[2] = 0x9E;
  P[3] = (UINT8)((Sec * 100 + 127) / 128);
  if (P[3] > 0x30) {
    P[3] = 0x30;
  }
  P[4] = 0;
  Log (L"scan: classic inquiry for %d s (put a controller in pairing mode: Joy-Con SYNC / Pro Controller SYNC)", Sec);
  UartTx ((UINT8[]){ 1, 0x01, 0x04, 5, P[0], P[1], P[2], P[3], P[4] }, 9);
  Ticks = (UINTN)Sec * 1000 + 3000;
  Found = 0;
  for (T = 0; T < Ticks;) {
    if (!HciReadEv (&Code, Ev, &Len, 100000)) {
      T += 100;
      continue;
    }
    if (Code == 0x0F && Len >= 4) {
      Log (L"scan: inquiry status=%02x", Ev[0]);
      if (Ev[0] != 0) {
        break;
      }
    } else if (Code == 0x01) {
      Log (L"scan: inquiry complete");
      break;
    } else if (Code == 0x02 || Code == 0x22) {
      for (I = 0; I < Ev[0] && 1 + (I + 1) * 14 <= Len; I++) {
        Off = 1 + I * 14;
        BtReport (L"classic", &Ev[Off], 0, Code == 0x22 ? (INT8)Ev[Off + 13] : 0, Ev[Off + 9] | (Ev[Off + 10] << 8) | (Ev[Off + 11] << 16), NULL, 0);
        Found++;
      }
    } else if (Code == 0x2F && Len >= 15 && Ev[0] >= 1) {
      BtReport (L"classic", &Ev[1], 0, (INT8)Ev[14], Ev[10] | (Ev[11] << 8) | (Ev[12] << 16), &Ev[15], (UINTN)Len - 15);
      Found++;
    }
  }
  Log (L"scan: classic done (%d responses)", Found);

  // ---- LE active scan
  P[0] = 1; P[1] = 0x60; P[2] = 0x00; P[3] = 0x30; P[4] = 0x00; P[5] = 0; P[6] = 0;
  N = HciCmd (0x200B, P, 7, R, sizeof (R));
  Log (L"scan: LE set scan parameters -> %d status=%02x", N, N > 0 ? R[0] : 0xFF);
  P[0] = 1; P[1] = 0;
  N = HciCmd (0x200C, P, 2, R, sizeof (R));
  Log (L"scan: LE scan enable -> %d status=%02x, scanning %d s", N, N > 0 ? R[0] : 0xFF, Sec);
  Found = 0;
  for (T = 0; T < (UINTN)Sec * 1000;) {
    if (!HciReadEv (&Code, Ev, &Len, 100000)) {
      T += 100;
      continue;
    }
    if (Code == 0x3E && Len >= 12 && Ev[0] == 0x02) {
      UINTN  DL = Ev[9];

      if (10 + DL + 1 <= Len) {
        BtReport (L"LE", &Ev[4], Ev[3], (INT8)Ev[10 + DL], 0, &Ev[10], DL);
        Found++;
      }
    }
  }
  P[0] = 0; P[1] = 0;
  HciCmd (0x200C, P, 2, R, sizeof (R));
  Log (L"scan: LE done (%d reports). Total distinct devices %d", Found, mSeenN);
}

// ---------------------------------------------------------------------------------------------
// SDHCI PIO driver (SDMMC3)
// ---------------------------------------------------------------------------------------------
STATIC
BOOLEAN
SdWaitInt (
  IN UINT32  Mask,
  OUT UINT32 *Sta
  )
{
  UINTN   T;
  UINT32  S;

  for (T = 0; T < 500000; T++) {
    S = Rd (mSd, SD_INTSTA);
    if (S & 0x8000) {            // error summary
      *Sta = S;
      return FALSE;
    }
    if (S & Mask) {
      *Sta = S;
      return TRUE;
    }
    Us (10);
  }
  *Sta = Rd (mSd, SD_INTSTA);
  return FALSE;
}

STATIC
VOID
SdResetCmdData (
  VOID
  )
{
  SdW8 (SD_SWRST, 0x06);
  Us (100);
  Wr (mSd, SD_INTSTA, 0xFFFFFFFF);
}

#define RSP_NONE  0
#define RSP_R2    1
#define RSP_R1    2
#define RSP_R1B   3
#define RSP_R3    4

STATIC
BOOLEAN
SdCmd (
  IN  UINT32  Idx,
  IN  UINT32  Arg,
  IN  UINT32  Rsp,
  OUT UINT32  *Resp,
  IN  UINT32  DataLen,        // 0 = no data
  IN  BOOLEAN DataRead,
  IN OUT UINT8 *Data
  )
{
  UINT32  Flags = 0;
  UINT32  Xfer  = 0;
  UINT32  Sta;
  UINT32  I;
  UINT32  W;
  UINTN   T;

  for (T = 0; T < 100000 && (Rd (mSd, SD_PRES) & 3); T++) {
    Us (10);
  }
  if (Rd (mSd, SD_PRES) & 3) {
    Log (L"sd: CMD%d: inhibit stuck PRES=%08x", Idx, Rd (mSd, SD_PRES));
    SdResetCmdData ();
    return FALSE;
  }
  Wr (mSd, SD_INTSTA, 0xFFFFFFFF);
  switch (Rsp) {
    case RSP_R2:  Flags = 0x01 | 0x08;        break;
    case RSP_R1:  Flags = 0x02 | 0x08 | 0x10; break;
    case RSP_R1B: Flags = 0x03 | 0x08 | 0x10; break;
    case RSP_R3:  Flags = 0x02;               break;
    default:      Flags = 0;                  break;
  }
  if (DataLen) {
    Flags |= 0x20;
    Xfer   = DataRead ? 0x10 : 0;
    Wr (mSd, SD_BLKSZ, DataLen | (1U << 16));        // BLKSZ[15:0] + BLKCNT[31:16] = 1 (single block, byte mode)
    Xfer |= 0;                                       // no block-count enable: single transfer of DataLen bytes
  }
  SdW8 (SD_TMO, 0x0E);
  Wr (mSd, SD_ARG, Arg);
  Wr (mSd, SD_XFER, Xfer | (((Idx << 8) | Flags) << 16));
  if (!SdWaitInt (0x1, &Sta)) {
    Log (L"sd: CMD%d: no response INT=%08x PRES=%08x", Idx, Sta, Rd (mSd, SD_PRES));
    SdResetCmdData ();
    return FALSE;
  }
  Wr (mSd, SD_INTSTA, 0x1);
  for (I = 0; I < 4; I++) {
    Resp[I] = Rd (mSd, SD_RESP + I * 4);
  }
  if (DataLen) {
    for (I = 0; I < DataLen; I += 4) {
      if (!SdWaitInt (DataRead ? 0x20 : 0x10, &Sta)) {
        Log (L"sd: CMD%d: data wait failed INT=%08x at %d", Idx, Sta, I);
        SdResetCmdData ();
        return FALSE;
      }
      if (I % 512 == 0 || 1) {
        Wr (mSd, SD_INTSTA, DataRead ? 0x20 : 0x10);
      }
      if (DataRead) {
        W = Rd (mSd, SD_DATA);
        CopyMem (&Data[I], &W, (DataLen - I) >= 4 ? 4 : (DataLen - I));
      } else {
        W = 0;
        CopyMem (&W, &Data[I], (DataLen - I) >= 4 ? 4 : (DataLen - I));
        Wr (mSd, SD_DATA, W);
      }
    }
    if (!SdWaitInt (0x2, &Sta)) {
      Log (L"sd: CMD%d: transfer complete missing INT=%08x", Idx, Sta);
      SdResetCmdData ();
      return FALSE;
    }
    Wr (mSd, SD_INTSTA, 0x2);
  } else if (Rsp == RSP_R1B) {
    SdWaitInt (0x2, &Sta);
    Wr (mSd, SD_INTSTA, 0x2);
  }
  return TRUE;
}

STATIC
BOOLEAN
Cmd52 (
  IN  UINT32  Fn,
  IN  UINT32  Addr,
  IN  BOOLEAN Write,
  IN  UINT8   Val,
  OUT UINT8   *Out
  )
{
  UINT32  R[4];
  UINT32  Arg = ((Write ? 1U : 0) << 31) | (Fn << 28) | ((Addr & 0x1FFFF) << 9) | Val;

  if (!SdCmd (52, Arg, RSP_R1, R, 0, FALSE, NULL)) {
    return FALSE;
  }
  if (R[0] & 0xCB00) {
    Log (L"sdio: CMD52 fn%d @%05x flags=%02x", Fn, Addr, (R[0] >> 8) & 0xFF);
  }
  if (Out != NULL) {
    *Out = (UINT8)R[0];
  }
  return TRUE;
}

STATIC
BOOLEAN
Cmd53 (
  IN UINT32     Fn,
  IN UINT32     Addr,
  IN BOOLEAN    Write,
  IN OUT UINT8  *Buf,
  IN UINT32     Len        // 1..512, byte mode, incrementing (FIFO if Fifo)
  )
{
  UINT32  R[4];
  UINT32  Arg = ((Write ? 1U : 0) << 31) | (Fn << 28) | (1U << 26) | ((Addr & 0x1FFFF) << 9) | (Len & 0x1FF);

  return SdCmd (53, Arg, RSP_R1, R, Len, !Write, Buf);
}

STATIC
BOOLEAN
SdInit (
  VOID
  )
{
  UINTN   T;
  UINT32  R[4];
  UINT32  Ocr;

  Log (L"sd: SDMMC3 @%08x pinmux", mSd);
  Wr (PINMUX_BASE, PM_SDMMC3_CLK, PM_INPUT);
  Wr (PINMUX_BASE, PM_SDMMC3_CMD, PM_INPUT | PM_PULLUP);
  Wr (PINMUX_BASE, PM_SDMMC3_DAT0 + 0, PM_INPUT | PM_PULLUP);
  Wr (PINMUX_BASE, PM_SDMMC3_DAT0 + 4, PM_INPUT | PM_PULLUP);
  Wr (PINMUX_BASE, PM_SDMMC3_DAT0 + 8, PM_INPUT | PM_PULLUP);
  Wr (PINMUX_BASE, PM_SDMMC3_DAT0 + 12, PM_INPUT | PM_PULLUP);

  Log (L"sd: CAR clock (PLLP/16 = 25.5 MHz), reset");
  Wr (CAR_BASE, CAR_SRC_SDMMC3, 30);
  Wr (CAR_BASE, CAR_RST_U_SET, CLK_U_SDMMC3);
  Wr (CAR_BASE, CAR_ENB_U_SET, CLK_U_SDMMC3);
  Us (100);
  Wr (CAR_BASE, CAR_RST_U_CLR, CLK_U_SDMMC3);
  Us (100);
  if (!(Rd (CAR_BASE, CAR_ENB_U) & CLK_U_SDMMC3)) {
    Log (L"sd: SDMMC3 clock did not come up (ENB_U=%08x)", Rd (CAR_BASE, CAR_ENB_U));
    return FALSE;
  }

  Log (L"sd: controller reset (HW touch: CAP=%08x)", Rd (mSd, SD_CAP));
  SdW8 (SD_SWRST, 0x01);
  for (T = 0; T < 1000 && (SdR8 (SD_SWRST) & 1); T++) {
    Us (10);
  }
  Wr (mSd, SD_VEN_IOSPARE, Rd (mSd, SD_VEN_IOSPARE) | (1U << 19));
  Wr (mSd, SD_VEN_TRIM, Rd (mSd, SD_VEN_TRIM) & ~(1U << 2));
  Wr (mSd, SD_VEN_CLK, (Rd (mSd, SD_VEN_CLK) & 0xE0FFFFFB) | (3U << 24));
  Wr (mSd, SD_VEN_PAD, (Rd (mSd, SD_VEN_PAD) & ~0xFU) | 7);
  Wr (mSd, SD_VEN_ACAL, (Rd (mSd, SD_VEN_ACAL) & 0xFFFF8080) | (0x7D << 8));      // like SDMMC1 3.3V: pd -3, pu 0
  Log (L"sd: pad auto-calibration");
  Wr (mSd, SD_VEN_PAD, Rd (mSd, SD_VEN_PAD) | 0x80000000);
  Wr (mSd, SD_VEN_ACAL, Rd (mSd, SD_VEN_ACAL) | 0xA0000000);
  for (T = 0; T < 1000 && (Rd (mSd, SD_VEN_ACSTA) & 0x80000000); T++) {
    Us (10);
  }
  if (Rd (mSd, SD_VEN_ACSTA) & 0x80000000) {
    Log (L"sd: autocal timeout (continuing)");
    Wr (mSd, SD_VEN_ACAL, Rd (mSd, SD_VEN_ACAL) & ~0x20000000U);
  }
  Wr (mSd, SD_VEN_PAD, Rd (mSd, SD_VEN_PAD) & 0x7FFFFFFF);

  // internal clock, power 3.3V, card clock 400 kHz (25.5 MHz / 64)
  SdW16 (SD_CLK, 0x0001);
  for (T = 0; T < 2000 && !(SdR16 (SD_CLK) & 2); T++) {
    Us (10);
  }
  if (!(SdR16 (SD_CLK) & 2)) {
    Log (L"sd: internal clock not stable");
    return FALSE;
  }
  SdW8 (SD_PWR, 0x0F);
  SdW8 (SD_HOSTCTL, 0);
  Wr (mSd, SD_INTSTAEN, 0x01FF01FF);
  Wr (mSd, SD_INTSIGEN, 0);
  SdW16 (SD_CLK, 0x0001 | (32 << 8) | 0x0004 | 0x0002);       // div 64 -> ~398 kHz, SD clock on
  Ms (10);

  Log (L"sdio: CMD0");
  SdCmd (0, 0, RSP_NONE, R, 0, FALSE, NULL);
  Ms (5);
  Log (L"sdio: CMD5");
  if (!SdCmd (5, 0, RSP_R3, R, 0, FALSE, NULL)) {
    Log (L"sdio: no SDIO card answered CMD5 (wrong controller? power? 32 kHz? WIFI_EN/RST polarity?)");
    return FALSE;
  }
  Ocr = R[0];
  Log (L"sdio: OCR=%08x functions=%d memory=%d", Ocr, (Ocr >> 28) & 7, (Ocr >> 27) & 1);
  for (T = 0; T < 100; T++) {
    if (!SdCmd (5, Ocr & 0x00FFFF00, RSP_R3, R, 0, FALSE, NULL)) {
      return FALSE;
    }
    if (R[0] & 0x80000000) {
      break;
    }
    Ms (10);
  }
  Log (L"sdio: ready OCR=%08x", R[0]);
  if (!SdCmd (3, 0, RSP_R1, R, 0, FALSE, NULL)) {
    return FALSE;
  }
  Log (L"sdio: RCA=%04x", R[0] >> 16);
  if (!SdCmd (7, R[0] & 0xFFFF0000, RSP_R1B, R, 0, FALSE, NULL)) {
    return FALSE;
  }
  mSdUp = TRUE;
  return TRUE;
}

#include "WifiPcie.h"

STATIC UINT32 mWin = 0xFFFFFFFF;

STATIC
BOOLEAN
SbWindow (
  IN UINT32  Addr
  )
{
  UINT32  Base = Addr & ~0x7FFFU;

  if (Base == mWin) {
    return TRUE;
  }
  if (!Cmd52 (1, 0x1000A, TRUE, (UINT8)((Base >> 8) & 0x80), NULL) ||
      !Cmd52 (1, 0x1000B, TRUE, (UINT8)(Base >> 16), NULL) ||
      !Cmd52 (1, 0x1000C, TRUE, (UINT8)(Base >> 24), NULL))
  {
    return FALSE;
  }
  mWin = Base;
  return TRUE;
}

STATIC
BOOLEAN
SbRead32 (
  IN  UINT32  Addr,
  OUT UINT32  *V
  )
{
  if (mPcieMode) {
    return BpRd32 (Addr, V);
  }
  if (!SbWindow (Addr)) {
    return FALSE;
  }
  return Cmd53 (1, (Addr & 0x7FFF) | 0x8000, FALSE, (UINT8 *)V, 4);
}

STATIC
BOOLEAN
SbWrite32 (
  IN UINT32  Addr,
  IN UINT32  V
  )
{
  if (mPcieMode) {
    return BpWr32 (Addr, V);
  }
  if (!SbWindow (Addr)) {
    return FALSE;
  }
  return Cmd53 (1, (Addr & 0x7FFF) | 0x8000, TRUE, (UINT8 *)&V, 4);
}

STATIC
BOOLEAN
SbWriteBlock (
  IN UINT32        Addr,
  IN CONST UINT8   *Data,
  IN UINTN         Len
  )
{
  UINT8   Tmp[512];
  UINTN   Chunk;
  UINTN   InWin;

  while (Len) {
    if (!SbWindow (Addr)) {
      return FALSE;
    }
    InWin = 0x8000 - (Addr & 0x7FFF);
    Chunk = Len < 512 ? Len : 512;
    if (Chunk > InWin) {
      Chunk = InWin;
    }
    CopyMem (Tmp, Data, Chunk);
    if (!Cmd53 (1, Addr & 0x7FFF, TRUE, Tmp, (UINT32)Chunk)) {
      return FALSE;
    }
    Addr += (UINT32)Chunk;
    Data += Chunk;
    Len  -= Chunk;
  }
  return TRUE;
}

STATIC
BOOLEAN
SdioClk (
  IN UINT8   Req,
  IN UINT8   Avail
  )
{
  UINTN  T;
  UINT8  V = 0;

  Cmd52 (1, 0x1000E, TRUE, Req, NULL);
  for (T = 0; T < 500; T++) {
    Cmd52 (1, 0x1000E, FALSE, 0, &V);
    if (V & Avail) {
      return TRUE;
    }
    Ms (1);
  }
  Log (L"sdio: CHIPCLKCSR=%02x (wanted %02x)", V, Avail);
  return FALSE;
}

STATIC
VOID
StepWifi (
  VOID
  )
{
  UINT8   V;
  UINT8   Cccr[0x18];
  UINT32  I;
  UINT32  Id = 0;
  UINT32  Erom = 0;
  UINT32  W;

  Log (L"wifi: pinmux, 32 kHz, WIFI_EN/WIFI_RST");
  PinsRadio ();
  Step32k (TRUE);
  GpioH (H0 | H1, 0, TRUE);
  Ms (20);
  GpioH (H0, 1, TRUE);
  Ms (20);
  GpioH (H1, 1, TRUE);
  GpioH (H2, 0, FALSE);
  Ms (150);

  if (!SdInit ()) {
    Log (L"wifi: SDIO init failed");
    return;
  }
  for (I = 0; I < sizeof (Cccr); I++) {
    Cmd52 (0, I, FALSE, 0, &Cccr[I]);
  }
  Log (L"sdio: CCCR rev=%02x sdspec=%02x ioen=%02x ioready=%02x cap=%02x bus=%02x", Cccr[0], Cccr[1], Cccr[2], Cccr[3], Cccr[8], Cccr[7]);
  // 4-bit bus
  Cmd52 (0, 0x07, TRUE, 0x82, NULL);
  SdW8 (SD_HOSTCTL, SdR8 (SD_HOSTCTL) | 2);
  // function 1 enable + block size
  Cmd52 (0, 0x02, TRUE, 0x02, NULL);
  for (I = 0; I < 100; I++) {
    Cmd52 (0, 0x03, FALSE, 0, &V);
    if (V & 2) {
      break;
    }
    Ms (5);
  }
  Log (L"sdio: function 1 ioready=%02x", V);
  Cmd52 (0, 0x110, TRUE, 64, NULL);
  Cmd52 (0, 0x111, TRUE, 0, NULL);
  // faster card clock: 25.5 MHz / 2
  SdW16 (SD_CLK, 0x0001 | (1 << 8));                         // div 2 -> 12.75 MHz
  for (I = 0; I < 1000 && !(SdR16 (SD_CLK) & 2); I++) {
    Us (10);
  }
  SdW16 (SD_CLK, SdR16 (SD_CLK) | 0x0004);
  mWin = 0xFFFFFFFF;
  if (!SdioClk (0x08, 0x40)) {
    Log (L"wifi: ALP clock not available");
  }
  if (SbRead32 (0x18000000, &Id)) {
    Log (L"wifi: chipcommon chipid reg=%08x -> chip %04x rev %d (BCM4356 = 4356)", Id, Id & 0xFFFF, (Id >> 16) & 0xF);
  } else {
    Log (L"wifi: chip id read failed");
    return;
  }
  if (SbRead32 (0x180000FC, &Erom)) {
    Log (L"wifi: EROM pointer %08x; dump:", Erom);
    for (I = 0; I < 64; I++) {
      if (!SbRead32 (Erom + I * 4, &W)) {
        break;
      }
      if (W == 0xF) {
        break;
      }
      Log (L"  erom[%02d]=%08x", I, W);
    }
  }
  Log (L"wifi: done. Next: wfw fw.bin nvram.txt (needs the real brcmfmac4356-sdio.bin / .txt)");
}

STATIC
UINT32
Hex (
  IN CONST CHAR16  *S
  )
{
  return (UINT32)StrHexToUintn (S);
}

STATIC
VOID
CoreDisable (
  IN UINT32  Wrap,
  IN UINT32  Pre,
  IN UINT32  Reset
  )
{
  UINT32  V = 0;
  UINTN   T;

  SbRead32 (Wrap + 0x800, &V);
  if (!(V & 1)) {
    SbWrite32 (Wrap + 0x408, Pre | 2 | 1);
    SbRead32 (Wrap + 0x408, &V);
    SbWrite32 (Wrap + 0x800, 1);
    Us (20);
    for (T = 0; T < 300; T++) {
      SbRead32 (Wrap + 0x800, &V);
      if (V == 1) {
        break;
      }
      Us (1);
    }
  }
  SbWrite32 (Wrap + 0x408, Reset | 2 | 1);
  SbRead32 (Wrap + 0x408, &V);
}

STATIC
VOID
CoreReset (
  IN UINT32  Wrap,
  IN UINT32  Pre,
  IN UINT32  Reset,
  IN UINT32  Post
  )
{
  UINT32  V = 0;
  UINTN   T;

  CoreDisable (Wrap, Pre, Reset);
  for (T = 0; T < 50; T++) {
    SbRead32 (Wrap + 0x800, &V);
    if (!(V & 1)) {
      break;
    }
    SbWrite32 (Wrap + 0x800, 0);
    Us (60);
  }
  SbWrite32 (Wrap + 0x408, Post | 1);
  SbRead32 (Wrap + 0x408, &V);
}

STATIC
VOID
StepWfw (
  IN CONST CHAR16  *FwName,
  IN CONST CHAR16  *NvName,
  IN UINT32        Ram,
  IN UINT32        Size,
  IN UINT32        Arm,
  IN UINT32        Wrap
  )
{
  UINT8   *Fw = NULL;
  UINT8   *Nv = NULL;
  UINT8   *Clean;
  UINTN   FwSz = 0;
  UINTN   NvSz = 0;
  UINTN   I;
  UINTN   O;
  UINT32  V;
  UINT32  Tok;
  UINT32  Rst;
  UINT8   C;
  BOOLEAN Comment;
  BOOLEAN Start;

  if (!mSdUp) {
    Log (L"wfw: run `wifibt wifi` first in the same boot");
    return;
  }
  if (EFI_ERROR (ReadFile (FwName, &Fw, &FwSz)) || FwSz < 4) {
    return;
  }
  Log (L"wfw: ARM CR4 core=%08x wrap=%08x, ram=%08x size=%08x", Arm, Wrap, Ram, Size);
  CopyMem (&Rst, Fw, 4);
  Log (L"wfw: firmware reset vector word = %08x", Rst);
  Log (L"wfw: disabling ARM CR4");
  CoreDisable (Wrap, 0x20, 0x20);                            // CPUHALT
  Log (L"wfw: writing %d bytes of firmware to %08x", FwSz, Ram);
  if (!SbWriteBlock (Ram, Fw, FwSz)) {
    Log (L"wfw: firmware write failed");
    return;
  }
  Log (L"wfw: verify first word");
  if (SbRead32 (Ram, &V)) {
    Log (L"wfw: ram[0]=%08x (fw word0 %08x)", V, Rst);
  }
  if (NvName != NULL && !EFI_ERROR (ReadFile (NvName, &Nv, &NvSz))) {
    Clean = AllocateZeroPool (NvSz + 8);
    if (Clean == NULL) {
      return;
    }
    O = 0;
    Comment = FALSE;
    Start = TRUE;
    for (I = 0; I < NvSz; I++) {
      C = Nv[I];
      if (C == '\r') {
        continue;
      }
      if (Start && C == '#') {
        Comment = TRUE;
      }
      if (C == '\n') {
        if (!Comment && !Start) {
          Clean[O++] = 0;
        }
        Comment = FALSE;
        Start   = TRUE;
        continue;
      }
      if (Start && (C == ' ' || C == '\t') ) {
        continue;
      }
      Start = FALSE;
      if (!Comment) {
        Clean[O++] = C;
      }
    }
    Clean[O++] = 0;
    Clean[O++] = 0;
    O = (O + 3) & ~3U;
    Tok = (UINT32)(O / 4);
    Tok = ((~Tok & 0xFFFF) << 16) | (Tok & 0xFFFF);
    Log (L"wfw: nvram %d bytes -> %d, to %08x, token %08x", NvSz, O, Ram + Size - 4 - (UINT32)O, Tok);
    if (!SbWriteBlock (Ram + Size - 4 - (UINT32)O, Clean, O) || !SbWrite32 (Ram + Size - 4, Tok)) {
      Log (L"wfw: nvram write failed");
      return;
    }
    FreePool (Clean);
    FreePool (Nv);
  } else {
    Log (L"wfw: no nvram given");
  }
  Log (L"wfw: set reset vector + release ARM CR4");
  SbWrite32 (0, Rst);
  CoreReset (Wrap, 0x20, 0, 0);
  if (SdioClk (0x10, 0x80)) {
    Log (L"wfw: HT clock available -> firmware appears to be RUNNING");
  } else {
    Log (L"wfw: HT clock never came up (firmware did not start)");
  }
  FreePool (Fw);
}

STATIC VOID PexOff (VOID);

STATIC
VOID
StepOff (
  VOID
  )
{
  Log (L"off: BT_REG_ON low, WIFI_EN/RST low");
  GpioH (H4, 0, TRUE);
  GpioH (H0 | H1, 0, TRUE);
  mSdUp = FALSE;
  PexOff ();
}

#include "WifiPexStages.h"

// ---------------------------------------------------------------------------------------------
STATIC
EFI_STATUS
RunWifiBt (
  IN UINTN    Argc,
  IN CHAR16   **Argv
  )
{
  CONST CHAR16  *Cmd;
  CONST CHAR16  *Pos[2] = { NULL, NULL };
  UINTN         NPos = 0;
  UINTN         I;
  UINT32        Baud = 0;
  UINT32        Ram  = 0x180000;
  UINT32        Size = 0;
  UINT32        Arm  = 0x18002000;
  UINT32        Wrap = 0x18102000;

  if (Argc < 2) {
    Print (L"usage: wifibt info|pwr|bt [fw.hcd] [baud=N] [flow=1]|btscan [sec] [baud=N]|pex [rst=0|1] [gen2=1]|pchip [arm= wrap= d11= size= ram=]|pfw fw.bin nvram.txt|pcon|pall fw nvram|poff|wifi (OLD SDIO, wrong for Switch)|wfw|all|off\n");
    return EFI_SUCCESS;
  }
  Cmd = Argv[1];
  for (I = 2; I < Argc; I++) {
    if (StrnCmp (Argv[I], L"baud=", 5) == 0) {
      Baud = (UINT32)StrDecimalToUintn (Argv[I] + 5);
    } else if (StrnCmp (Argv[I], L"flow=", 5) == 0) {
      mFlow = (UINT32)StrDecimalToUintn (Argv[I] + 5);
    } else if (StrnCmp (Argv[I], L"base=", 5) == 0) {
      mSd = Hex (Argv[I] + 5);
    } else if (StrnCmp (Argv[I], L"ram=", 4) == 0) {
      Ram = Hex (Argv[I] + 4);
      mRamBase = Ram;
    } else if (StrnCmp (Argv[I], L"size=", 5) == 0) {
      Size = Hex (Argv[I] + 5);
      mRamSize = Size;
    } else if (StrnCmp (Argv[I], L"arm=", 4) == 0) {
      Arm = Hex (Argv[I] + 4);
      mArmBase = Arm;
    } else if (StrnCmp (Argv[I], L"wrap=", 5) == 0) {
      Wrap = Hex (Argv[I] + 5);
      mArmWrap = Wrap;
    } else if (StrnCmp (Argv[I], L"d11=", 4) == 0) {
      mD11Wrap = Hex (Argv[I] + 4);
    } else if (StrnCmp (Argv[I], L"rst=", 4) == 0) {
      mPexRstRun = (UINT32)StrDecimalToUintn (Argv[I] + 4);
    } else if (StrnCmp (Argv[I], L"gen2=", 5) == 0) {
      mPexGen2 = (UINT32)StrDecimalToUintn (Argv[I] + 5);
    } else if (NPos < 2) {
      Pos[NPos++] = Argv[I];
    }
  }

  if (EFI_ERROR (ShellOpenFileByName (L"wifibt.log", &mLog, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
    mLog = NULL;
  } else {
    UINT64  End = 0;
    ShellGetFileSize (mLog, &End);
    ShellSetFilePosition (mLog, End);
  }
  Log (L"wifibt %s (draft)", Cmd);

  if (StrCmp (Cmd, L"info") == 0) {
    StepInfo ();
  } else if (StrCmp (Cmd, L"pwr") == 0) {
    StepPwr ();
  } else if (StrCmp (Cmd, L"bt") == 0) {
    StepBt (Pos[0], Baud);
  } else if (StrCmp (Cmd, L"btscan") == 0) {
    StepPwr ();
    BtScan (Pos[0] != NULL ? (UINT32)StrDecimalToUintn (Pos[0]) : 10, Baud);
  } else if (StrCmp (Cmd, L"wifi") == 0) {
    StepWifi ();
  } else if (StrCmp (Cmd, L"wfw") == 0) {
    if (Pos[0] == NULL) {
      Log (L"wfw: need firmware file");
    } else {
      StepWfw (Pos[0], Pos[1], Ram, Size, Arm, Wrap);
    }
  } else if (StrCmp (Cmd, L"pex") == 0) {
    if (PexUp ()) {
      PexEnum ();
    }
  } else if (StrCmp (Cmd, L"pchip") == 0) {
    PexChip ();
  } else if (StrCmp (Cmd, L"pfw") == 0) {
    if (Pos[0] == NULL) {
      Log (L"pfw: need firmware file (brcmfmac4356-pcie.bin) and nvram (brcmfmac4356-pcie.txt)");
    } else {
      PexFw (Pos[0], Pos[1]);
    }
  } else if (StrCmp (Cmd, L"pcon") == 0) {
    PexConsole ();
  } else if (StrCmp (Cmd, L"poff") == 0) {
    PexOff ();
  } else if (StrCmp (Cmd, L"pall") == 0) {
    // radio over PCIe in one go: pall fw.bin nvram.txt
    StepPwr ();
    if (PexUp () && PexEnum () && PexChip () && Pos[0] != NULL) {
      if (PexFw (Pos[0], Pos[1])) {
        Ms (500);
      }
      PexConsole ();
    }
  } else if (StrCmp (Cmd, L"all") == 0) {
    StepPwr ();
    StepBt (NULL, 0);
    if (PexUp () && PexEnum ()) {
      PexChip ();
    }
  } else if (StrCmp (Cmd, L"off") == 0) {
    StepOff ();
  } else {
    Log (L"unknown command %s", Cmd);
  }

  if (mLog != NULL) {
    ShellCloseFile (&mLog);
  }
  return EFI_SUCCESS;
}

STATIC
SHELL_STATUS
EFIAPI
WifiBtCmdHandler (
  IN EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL  *This,
  IN EFI_SYSTEM_TABLE                    *SystemTable,
  IN EFI_SHELL_PARAMETERS_PROTOCOL       *ShellParameters,
  IN EFI_SHELL_PROTOCOL                  *Shell
  )
{
  gEfiShellParametersProtocol = ShellParameters;
  gEfiShellProtocol           = Shell;
  return (SHELL_STATUS)RunWifiBt (ShellParameters->Argc, ShellParameters->Argv);
}

STATIC
CHAR16 *
EFIAPI
WifiBtCmdGetHelp (
  IN EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL  *This,
  IN CONST CHAR8                         *Language
  )
{
  STATIC CONST CHAR16  Help[] =
    L"wifibt info|pwr|bt [fw.hcd] [baud=N] [flow=1]|btscan [sec]|pex|pchip|pfw fw nvram|pcon|pall|poff|all|off  (BCM4356 Bluetooth/Wi-Fi bring-up, DRAFT; log: wifibt.log)\r\n";

  return AllocateCopyPool (sizeof (Help), Help);
}

STATIC EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL  mWifiBtCommand = {
  L"wifibt",
  WifiBtCmdHandler,
  WifiBtCmdGetHelp
};

EFI_STATUS
EFIAPI
WifiBtCmdInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return gBS->InstallProtocolInterface (&ImageHandle, &gEfiShellDynamicCommandProtocolGuid, EFI_NATIVE_INTERFACE, &mWifiBtCommand);
}
