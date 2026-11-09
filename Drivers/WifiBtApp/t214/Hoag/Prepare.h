/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIFI_T214_PREPARE_H
#define WIFI_T214_PREPARE_H
#include "power.h"

STATIC BOOLEAN
T214RestoreAdapter (VOID *State)
{
  return T214PcieSupplyRestore ((WIFI_PCIE_POWER_STATE *)State);
}

EFI_STATUS EFIAPI
WifiPrepareMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  WIFI_PCIE_POWER_STATE Supply;
  (VOID)ImageHandle;
  (VOID)SystemTable;
  ZeroMem (&Supply, sizeof (Supply));
  i2c_init (I2C_5);
  if (!T214PcieSupplyEnable (&Supply)) {
    T214PcieSupplyRestore (&Supply);
    return EFI_DEVICE_ERROR;
  }
  return WifiPrepareRun (&Supply, T214RestoreAdapter);
}
#endif
