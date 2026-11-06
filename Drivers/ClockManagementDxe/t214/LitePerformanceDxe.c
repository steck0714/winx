// SPDX-License-Identifier: GPL-2.0-only
// Hekate BL33 environment and Tegra210B01 BPMP EMC handoff.
// BPMP protocol: Switchroot/TF-A bpmp.c and NVIDIA clk-nv-bpmp.c.
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/I2cLib.h>
#include <Tegra214Performance.h>

#define ENV_ADDRESS 0xA9FC0000UL
#define ENV_SIZE    0x40000U
#define SEMAPHORE   0x60001000UL
#define ATOMICS_TRIGGER 0x70016000UL
#define ATOMICS_RESULT0 0x70016C00UL
#define BPMP_IRQ   0x60004018UL
#define MRQ_CLK    22U
#define MRQ_EMC_DVFS_TABLE 32U

STATIC UINTN mChannel;
STATIC INT32 mLastBpmp;

STATIC BOOLEAN WaitChannel (UINT32 State)
{
  UINTN Retry;
  for (Retry = 0; Retry < 50000; ++Retry) {
    if ((MmioRead32 (SEMAPHORE) & 3U) == State) return TRUE;
    gBS->Stall (10);
  }
  return FALSE;
}

STATIC EFI_STATUS Bpmp (UINT32 Mrq, CONST VOID *Input, UINTN InSize, VOID *Output, UINTN OutSize)
{
  UINTN Index;
  BOOLEAN IrqState;
  EFI_STATUS Status = EFI_TIMEOUT;
  if (mChannel == 0 || InSize > 120 || OutSize > 120) return EFI_INVALID_PARAMETER;
  IrqState = SaveAndDisableInterrupts ();
  if (!WaitChannel (2)) goto Done;
  MmioWrite32 (mChannel, Mrq);
  MmioWrite32 (mChannel + 4, 1); // DO_ACK
  for (Index = 0; Index < InSize; ++Index)
    MmioWrite8 (mChannel + 8 + Index, ((CONST UINT8 *)Input)[Index]);
  MemoryFence ();
  MmioWrite32 (SEMAPHORE + 8, 3); // signal BPMP on atomic channel 0
  MmioWrite32 (BPMP_IRQ, BIT6);
  if (!WaitChannel (3)) goto Done;
  MemoryFence ();
  mLastBpmp = (INT32)MmioRead32 (mChannel);
  for (Index = 0; Index < OutSize; ++Index)
    ((UINT8 *)Output)[Index] = MmioRead8 (mChannel + 8 + Index);
  MmioWrite32 (SEMAPHORE + 8, 1); // acknowledged -> free
  Status = mLastBpmp == 0 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
Done:
  SetInterruptState (IrqState);
  if (Status == EFI_TIMEOUT) mLastBpmp = -110;
  return Status;
}

STATIC UINT32 EnvNumber (CONST CHAR8 *Key, UINT32 Default)
{
  CONST CHAR8 *Text = (CONST CHAR8 *)ENV_ADDRESS;
  UINTN Pos = 0, Length = AsciiStrLen (Key), I;
  UINT32 Value;
  while (Pos < ENV_SIZE && Text[Pos] != 0) {
    UINTN End = Pos;
    while (End < ENV_SIZE && Text[End] != 0 && Text[End] != '\n') ++End;
    if (End > Pos + Length && Text[Pos + Length] == '=' &&
        CompareMem (Text + Pos, Key, Length) == 0) {
      Value = 0;
      for (I = Pos + Length + 1; I < End; ++I) {
        if (Text[I] < '0' || Text[I] > '9' || Value > 10000000U) return MAX_UINT32;
        Value = Value * 10U + (UINT32)(Text[I] - '0');
      }
      return Value;
    }
    Pos = End + 1;
  }
  return Default;
}

STATIC EFI_STATUS ReadRate (UINT64 *Rate)
{
  UINT32 Request = 1U << 24; // CLK_GET_RATE, clock ID 0 (validated as emc below)
  return Bpmp (MRQ_CLK, &Request, sizeof (Request), Rate, sizeof (*Rate));
}

EFI_STATUS EFIAPI
LitePerformanceInitialize (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  LITE_PERFORMANCE *Config = (LITE_PERFORMANCE *)LITE_PERF_ADDRESS;
  struct { UINT32 Count; struct { UINT32 Khz, Mv; } Pair[14]; } Timings;
  struct { UINT32 Command, Unused; UINT64 Rate; } SetRate;
  UINT8 Info[113], CoreReg, Register, Probe31, Probe33;
  UINT32 Request, RequiredMv = 0;
  UINT64 Rate = 0;
  UINTN Index;
  EFI_STATUS Status;

  if ((MmioRead32 (0x70000804) & 0xFFF0) != 0x2120) return EFI_UNSUPPORTED;
  ZeroMem (Config, sizeof (*Config));
  Config->Version = LITE_PERF_VERSION;
  // Identify the installed MAX77812 phase variant with read-only VOUT probes.
  // This avoids depending on non-secure access to the ODM28 fuse group.
  Register = 0x26;
  Probe31 = i2c_xfer_packet (I2C_5, 0x31, &Register, 1, &CoreReg, 1) != 0;
  Probe33 = i2c_xfer_packet (I2C_5, 0x33, &Register, 1, &CoreReg, 1) != 0;
  Config->PmicAddress = Probe31 != Probe33 ? (Probe31 ? 0x31 : 0x33) : 0;
  Config->BootPllx = MmioRead32 (0x600060E0);
  Config->BootPolicy = MmioRead32 (0x60006020);
  Config->Status = LitePerfBadConfig;
  if (MmioRead32 (ENV_ADDRESS - 4) != 0x33334C42) goto Done;
  Config->CpuKhz = EnvNumber ("cpu_oc", 0);
  Config->RamKhz = EnvNumber ("ram_oc", 1600000);
  if ((Config->CpuKhz != 0 && Config->CpuKhz != LITE_CPU_KHZ) ||
      (Config->RamKhz != 1600000 && Config->RamKhz != LITE_RAM_KHZ)) goto Done;
  Config->Status = LitePerfNoBpmp;
  if (!WaitChannel (2)) { mLastBpmp = -110; goto Done; }
  // TRIGGER0 accepts commands; RESULT0 is read-only command output.
  // Match Switchroot mail_t210.c rather than writing the result register.
  MmioWrite32 (ATOMICS_TRIGGER, 4); // ATOMIC_CMD_GET for channel 0
  mChannel = MmioRead32 (ATOMICS_RESULT0);
  Config->BpmpChannel = (UINT32)mChannel;
  // Hekate's Mariko BPMP runs in the non-secure-accessible SECFW carveout.
  // Its channel buffers can be in DRAM, unlike the older IRAM-only path.
  if ((mChannel & 3) != 0 ||
      !((mChannel >= 0x40000000 && mChannel <= 0x4003FF80) ||
        (mChannel >= 0xFFE00000 && mChannel <= 0xFFEFFF80))) {
    mLastBpmp = -22;
    goto Done;
  }

  Config->Status = LitePerfNoEmc;
  Request = 14U << 24; // CLK_GET_ALL_INFO, ID 0
  Status = Bpmp (MRQ_CLK, &Request, sizeof (Request), Info, sizeof (Info));
  // flags + parent + sixteen parents + num_parents, then clock name.
  if (EFI_ERROR (Status) || CompareMem (Info + 73, "emc", 4) != 0) goto Done;
  if (EFI_ERROR (ReadRate (&Rate))) goto Done;
  Config->RamActualKhz = (UINT32)(Rate / 1000);

  Config->Status = LitePerfNoTiming;
  if (EFI_ERROR (Bpmp (MRQ_EMC_DVFS_TABLE, NULL, 0, &Timings, sizeof (Timings))) || Timings.Count > 14) goto Done;
  for (Index = 0; Index < Timings.Count; ++Index)
    if (Timings.Pair[Index].Khz == Config->RamKhz) RequiredMv = Timings.Pair[Index].Mv;
  if (RequiredMv == 0) goto Done;

  Config->Status = LitePerfCoreVoltage;
  Register = 0x16;
  if (!i2c_xfer_packet (I2C_5, 0x3C, &Register, 1, &CoreReg, 1)) goto Done;
  Config->CoreMv = 600 + (CoreReg & 0x3F) * 125 / 10;
  // Hekate leaves Mariko VDD_CORE at 1050 mV. Do not invent a different rail setting.
  if (Config->CoreMv < RequiredMv) goto Done;

  Config->Status = LitePerfClockError;
  if (Config->RamActualKhz != Config->RamKhz) {
    SetRate.Command = 2U << 24; // CLK_SET_RATE, ID 0
    SetRate.Unused = 0;
    SetRate.Rate = (UINT64)Config->RamKhz * 1000;
    if (EFI_ERROR (Bpmp (MRQ_CLK, &SetRate, sizeof (SetRate), &Rate, sizeof (Rate)))) goto Done;
  }
  if (EFI_ERROR (ReadRate (&Rate))) goto Done;
  Config->RamActualKhz = (UINT32)(Rate / 1000);
  Config->Status = Config->RamActualKhz == Config->RamKhz ? LitePerfReady : LitePerfRateMismatch;
Done:
  Config->BpmpError = (UINT32)mLastBpmp;
  Config->Magic = LITE_PERF_MAGIC;
  WriteBackDataCacheRange (Config, sizeof (*Config));
  return EFI_SUCCESS; // diagnostics travel to Windows; boot itself remains available
}
