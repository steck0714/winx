// SPDX-License-Identifier: GPL-2.0-only
#ifdef SWITCH_LITE_GPU
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/I2cLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

// Compile the canonical backend source from the driver submodule, not a copy.
#include "../../WindowsDrivers/drivers/TegraGpu/platform/t214_power.c"
#include "LiteGpuI2c.h"

STATIC uint32_t GpuRead32 (void *Context, uint32_t Address)
{
  (VOID)Context;
  return MmioRead32 (Address);
}

STATIC void GpuWrite32 (void *Context, uint32_t Address, uint32_t Value)
{
  (VOID)Context;
  MmioWrite32 (Address, Value);
  MemoryFence ();
}

STATIC void GpuDelay (void *Context, uint32_t Microseconds)
{
  (VOID)Context;
  gBS->Stall (Microseconds);
}

VOID LiteGpuPowerCheck (VOID)
{
  struct tg_gpu_power State;
  struct tg_gpu_power_io Io = { NULL, GpuRead32, GpuWrite32, GpuReadPmic, GpuWritePmic, GpuDelay };
  enum tg_gpu_power_result Result, Restore;
  UINT8 Value, Pmic = 0;
  BOOLEAN Probe31, Probe33;
  UINT32 Boot0, Boot0Again;

  ZeroMem (&State, sizeof (State));
  Print (L"T214 GPU POWER CHECK R3 / NO DMA\r\n");
  Print (L"Windows does not start during this check.\r\n");
  Print (L"HIDREV %08x  RESET_X %08x  CLAMP %08x\r\n",
         MmioRead32 (0x70000804), MmioRead32 (0x6000628C), MmioRead32 (0x7000E6D4));
  i2c_init (I2C_5);
  Probe31 = GpuReadPmic (NULL, 0x31, 0x26, &Value);
  Probe33 = GpuReadPmic (NULL, 0x33, 0x26, &Value);
  if (Probe31 != Probe33) Pmic = Probe31 ? 0x31 : 0x33;
  Result = tg_gpu_power_up (&Io, Pmic, &State);
  Print (L"PMIC %02x  power result %u  stage %u\r\n", Pmic, Result, State.stage);
  Print (L"Saved M1 VOUT %02x  EN_CTRL %02x  PLLP_OUT5 %08x\r\n",
         State.original_voltage, State.original_enable, State.original_pllp_out5);
  Print (L"Current PLLP_OUT5 %08x  CLOCK_X %08x\r\n",
         MmioRead32 (0x6000667C), MmioRead32 (0x60006280));
  if (Result == TG_GPU_POWER_OK) {
    Boot0 = MmioRead32 (0x57000000);
    gBS->Stall (10);
    Boot0Again = MmioRead32 (0x57000000);
    Print (L"GPU BOOT0 %08x  repeat %08x\r\n", Boot0, Boot0Again);
    if (Boot0 == Boot0Again && ((Boot0 >> 20) & 0x1FF) == 0x12B)
      Print (L"GM20B IDENTIFIED; GPU execution has not been tested.\r\n");
    else
      Print (L"GPU identity not confirmed.\r\n");
  }
  Restore = tg_gpu_power_down (&Io, &State);
  Print (L"Restore result %u  resources still owned %u\r\n", Restore, State.owned);
  Print (L"Photograph this result. Powering off in 30 seconds.\r\n");
  for (UINTN I = 0; I < 30; ++I) gBS->Stall (1000000);
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();
}
#endif
