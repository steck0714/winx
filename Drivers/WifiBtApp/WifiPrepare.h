/* SPDX-License-Identifier: GPL-2.0-only */
/* Included at the end of WifiBt.c: reuse the physically accepted PCIe path.
 * No host DMA allocation survives this application. Windows loads the radio
 * firmware after it owns the ACPI device, registers and interrupt. */
#include "t214/power.h"
#include "../../WindowsDrivers/drivers/SwitchWifi/platform/tegra_resources.h"

EFI_STATUS EFIAPI
WifiPrepareMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  T214_PCIE_SUPPLY Supply;
  UINT32 AfiAsid = 0, LicClass = 0, LicEnabled = 0;
  BOOLEAN AfiChanged = FALSE, LicChanged = FALSE, Started = FALSE;
  (VOID)ImageHandle;
  (VOID)SystemTable;
  ZeroMem (&Supply, sizeof (Supply));
  if (!PexIsB01 ()) return EFI_UNSUPPORTED;
  i2c_init (I2C_5);
  if (!T214PcieSupplyEnable (&Supply)) goto Failed;
  Started = TRUE;
  if (!PexUp () || !PexEnum ()) goto Failed;

  // PexEnum left memory decoding on and verified that endpoint DMA is off.
  if (!PexChip () || !BpWr32 (mPcieCore + 0x4C, 0)) goto Failed;

  // This platform has no Windows Tegra SMMU/IORT driver. Establish a direct
  // AFI DMA domain while bus mastering is off; Windows uses 32-bit HAL DMA
  // logical addresses and uncached/noncoherent common buffers (_CCA=0).
  // Other SMMU clients are untouched. Failure restores the inherited domain.
  AfiAsid = MmioRead32 (0x70019238UL);
  AfiChanged = TRUE;
  MmioWrite32 (0x70019238UL, 0);
  if (MmioRead32 (0x70019238UL) != 0) goto Failed;
  // Switchroot irq-tegra.c unmasks the legacy controller as well as GIC.
  // Windows owns the GIC; prepare only SPI 98's CPU route while AFI and the
  // endpoint remain masked. Preserve all other IRQ/FIQ routing bits.
  LicClass = MmioRead32 (SWIFI_LIC_BASE + 0x2C);
  LicEnabled = MmioRead32 (SWIFI_LIC_BASE + 0x20);
  LicChanged = TRUE;
  MmioWrite32 (SWIFI_LIC_BASE + 0x2C, LicClass & ~SWIFI_LIC_MASK);
  MmioWrite32 (SWIFI_LIC_BASE + 0x1C, SWIFI_LIC_MASK); // Clear stale forced IRQ.
  MmioWrite32 (SWIFI_LIC_BASE + 0x24, SWIFI_LIC_MASK); // CPU_IER_SET.
  if (!(MmioRead32 (SWIFI_LIC_BASE + 0x20) & SWIFI_LIC_MASK) ||
      (MmioRead32 (SWIFI_LIC_BASE + 0x2C) & SWIFI_LIC_MASK)) goto Failed;
  MemoryFence ();
  Log (L"Windows Wi-Fi handoff: PCIe ready, CR4 halted, bus mastering/interrupts off");
  Log (L"BAR0=%08x size=%08x BAR2=%08x size=%08x; Windows owns firmware and DMA",
       (UINT32)mBar0, mBar0Size, (UINT32)mBar2, mBar2Size);
  return EFI_SUCCESS; // Keep the LDO1/PHY/link prepared for the Windows owner.

Failed:
  if (Started) PexOff ();
  if (LicChanged) {
    UINT32 Class = MmioRead32 (SWIFI_LIC_BASE + 0x2C);
    MmioWrite32 (SWIFI_LIC_BASE + 0x2C, (Class & ~SWIFI_LIC_MASK) | (LicClass & SWIFI_LIC_MASK));
    MmioWrite32 (SWIFI_LIC_BASE + ((LicEnabled & SWIFI_LIC_MASK) ? 0x24 : 0x28), SWIFI_LIC_MASK);
  }
  if (AfiChanged) {
    MmioWrite32 (0x70019238UL, AfiAsid);
    MemoryFence ();
  }
  if (Supply.Changed) {
    MmioWrite32 (0x60006310UL, BIT10);
    MicroSecondDelay (20);
  }
  if (!T214PcieSupplyRestore (&Supply))
    Log (L"Windows Wi-Fi preparation: supply restore failed");
  return EFI_DEVICE_ERROR;
}
