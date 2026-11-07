// SPDX-License-Identifier: GPL-2.0-only
// BootROM reset commands from Switchroot's tegra210b01-nx-common.dtsi.
// Encoding matches drivers/soc/tegra/pmc.c in the Linux 5.1.2 release.
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>

#define T210_HIDREV       0x70000804UL
#define PMC_BR_COMMAND    0x7000ED08UL

// Linux's parser leaves controller_id at zero for the T210 BootROM engine.
// Keep the generated words, including per-block byte checksums, identical.
STATIC CONST UINT32 mResetCommands[] = {
  0x00005252, // 2 retries, 10 delay, 2 blocks, 10 wait-before-bus-clear
  0x80B80568, // RTC address 0x68, 5 byte writes, reset controller, checksum B8
  0x001A0013, // Clear RTC reboot reason slots
  0x00180011, // Clear RTC reboot reason magic
  0x00000104, // Latch RTC writes
  0x806F033C, // PMIC address 0x3C, 3 byte writes, checksum 6F
  0x99422016, // After reset: SD0=0x20, ONOFFCNFG2=0x99
  0x00008041  // After reset: ONOFFCNFG1.SFT_RST
};

EFI_STATUS EFIAPI
LiteResetInitialize (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  UINTN Index;
  if ((MmioRead32 (T210_HIDREV) & 0xFFF0) != 0x2120) {
    return EFI_UNSUPPORTED;
  }

  // These are retained BootROM commands, not live I2C/voltage writes.
  // Commit the global header after both complete command blocks.
  for (Index = 1; Index < ARRAY_SIZE (mResetCommands); ++Index) {
    MmioWrite32 (PMC_BR_COMMAND + Index * sizeof (UINT32), mResetCommands[Index]);
  }
  MmioWrite32 (PMC_BR_COMMAND, mResetCommands[0]);
  for (Index = 0; Index < ARRAY_SIZE (mResetCommands); ++Index) {
    if (MmioRead32 (PMC_BR_COMMAND + Index * sizeof (UINT32)) != mResetCommands[Index]) {
      DEBUG ((DEBUG_ERROR, "LiteReset: BootROM command %u readback failed\n", Index));
      return EFI_DEVICE_ERROR;
    }
  }
  DEBUG ((DEBUG_INFO, "LiteReset: Mariko BootROM reset commands installed\n"));
  return EFI_SUCCESS;
}
