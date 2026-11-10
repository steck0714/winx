/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared PCIe-to-Windows handoff; board power policy is model-specific. */
#ifndef WIFI_PREPARE_COMMON_H
#define WIFI_PREPARE_COMMON_H

#include "PowerState.h"
#include "../../WindowsDrivers/t214/Hoag/drivers/SwitchWifi/platform/tegra_resources.h"

STATIC EFI_STATUS
WifiPrepareRun (WIFI_PCIE_POWER_STATE *Supply,
                WIFI_PCIE_POWER_RESTORE RestoreSupply)
{
  UINT32 AfiAsid = 0, LicClass = 0, LicEnabled = 0;
  BOOLEAN AfiChanged = FALSE, LicChanged = FALSE, Started = FALSE;

  Started = TRUE;
  if (!PexUp () || !PexEnum ()) goto Failed;
  // Keep endpoint bus mastering disabled until Windows takes ownership.
  if (!PexChip () || !BpWr32 (mPcieCore + 0x4C, 0)) goto Failed;

  // Establish a direct AFI DMA domain while endpoint bus mastering is off.
  AfiAsid = MmioRead32 (0x70019238UL);
  AfiChanged = TRUE;
  MmioWrite32 (0x70019238UL, 0);
  if (MmioRead32 (0x70019238UL) != 0) goto Failed;

  LicClass = MmioRead32 (SWIFI_LIC_BASE + 0x2C);
  LicEnabled = MmioRead32 (SWIFI_LIC_BASE + 0x20);
  LicChanged = TRUE;
  MmioWrite32 (SWIFI_LIC_BASE + 0x2C, LicClass & ~SWIFI_LIC_MASK);
  MmioWrite32 (SWIFI_LIC_BASE + 0x1C, SWIFI_LIC_MASK);
  MmioWrite32 (SWIFI_LIC_BASE + 0x24, SWIFI_LIC_MASK);
  if (!(MmioRead32 (SWIFI_LIC_BASE + 0x20) & SWIFI_LIC_MASK) ||
      (MmioRead32 (SWIFI_LIC_BASE + 0x2C) & SWIFI_LIC_MASK)) goto Failed;

  MemoryFence ();
  Log (L"Windows Wi-Fi handoff: PCIe ready; DMA/interrupts held for Windows");
  Log (L"BAR0=%08x size=%08x BAR2=%08x size=%08x",
       (UINT32)mBar0, mBar0Size, (UINT32)mBar2, mBar2Size);
  return EFI_SUCCESS;

Failed:
  if (Started) PexOff ();
  if (LicChanged) {
    UINT32 Class = MmioRead32 (SWIFI_LIC_BASE + 0x2C);
    MmioWrite32 (SWIFI_LIC_BASE + 0x2C,
      (Class & ~SWIFI_LIC_MASK) | (LicClass & SWIFI_LIC_MASK));
    MmioWrite32 (SWIFI_LIC_BASE +
      ((LicEnabled & SWIFI_LIC_MASK) ? 0x24 : 0x28), SWIFI_LIC_MASK);
  }
  if (AfiChanged) {
    MmioWrite32 (0x70019238UL, AfiAsid);
    MemoryFence ();
  }
  if (Supply != NULL && RestoreSupply != NULL && !RestoreSupply (Supply))
    Log (L"Wi-Fi preparation: model-specific supply restore failed");
  return EFI_DEVICE_ERROR;
}

#endif
