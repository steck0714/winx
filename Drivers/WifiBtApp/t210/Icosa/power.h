/* SPDX-License-Identifier: GPL-2.0-only */
/* Erista (T210) Wi-Fi power policy. Observe-only until board rails are verified. */
#ifndef WIFI_ERISTA_POWER_H
#define WIFI_ERISTA_POWER_H

#include "../../PowerState.h"

STATIC BOOLEAN
EristaPciePmicRead (UINT32 Register, UINT8 *Value)
{
  return i2c_recv_buf_small (Value, 1, I2C_5, 0x3C, Register) != 0;
}

STATIC BOOLEAN
EristaPcieSupplyObserve (WIFI_PCIE_POWER_STATE *State)
{
  UINT8 Value;
  UINTN Index;

  State->Config = 0;
  State->Fps = 0;
  State->EnabledConfig = 0;
  State->PowerStatus = 0;
  State->Changed = FALSE;

  Print (L"PCIe supply (Erista/T210, observe-only):\r\n");
  // MAX77620 SD0..SD3 voltage registers.
  for (Index = 0; Index < 4; Index++) {
    if (!EristaPciePmicRead (0x16 + (UINT32)Index, &Value)) {
      Print (L"  SD%u: read failed\r\n", (UINT32)Index);
      return FALSE;
    }
    Print (L"  SD%u=%02x\r\n", (UINT32)Index, Value);
  }

  // LDO0..LDO8 CNFG/CNFG2 pairs from the MAX77620 register map.
  for (Index = 0; Index < 9; Index++) {
    UINT8 Cnfg2;
    if (!EristaPciePmicRead (0x23 + 2 * (UINT32)Index, &Value) ||
        !EristaPciePmicRead (0x24 + 2 * (UINT32)Index, &Cnfg2)) {
      Print (L"  LDO%u: read failed\r\n", (UINT32)Index);
      return FALSE;
    }
    Print (L"  LDO%u=%02x CNFG2=%02x\r\n", (UINT32)Index, Value, Cnfg2);
  }
  return TRUE;
}

STATIC BOOLEAN
EristaPcieSupplyRestore (VOID *State)
{
  // Erista policy does not change PMIC state.
  (VOID)State;
  return TRUE;
}

#endif
