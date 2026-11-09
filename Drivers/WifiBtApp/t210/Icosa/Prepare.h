/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIFI_ERISTA_PREPARE_H
#define WIFI_ERISTA_PREPARE_H
#include "power.h"

STATIC BOOLEAN
EristaRestoreAdapter (VOID *State)
{
  return EristaPcieSupplyRestore (State);
}

EFI_STATUS EFIAPI
WifiPrepareMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  WIFI_PCIE_POWER_STATE Supply;
  (VOID)ImageHandle;
  (VOID)SystemTable;
  ZeroMem (&Supply, sizeof (Supply));
  i2c_init (I2C_5);
  // Power rails are only observed on Erista until the board sequence is proven.
  if (!EristaPcieSupplyObserve (&Supply)) return EFI_UNSUPPORTED;
  return WifiPrepareRun (&Supply, EristaRestoreAdapter);
}
#endif
