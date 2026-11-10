/* SPDX-License-Identifier: GPL-2.0-only */
/* T210B01 PCIe supply policy from Switchroot tegra210b01-nx-pmic.dtsi:
 * LDO1 supplies the PCIe analog/digital PLL rails at 1.0 V; shared SD3 is
 * already 1.8 V. MAX77620 register fields follow the upstream regulator driver.
 * Copyright (c) 2016 NVIDIA CORPORATION. All rights reserved.
 */
#ifndef WIFI_T214_POWER_H
#define WIFI_T214_POWER_H

#include "../../PowerState.h"

STATIC BOOLEAN
T214PciePmicRead (UINT32 Register, UINT8 *Value)
{
  return i2c_recv_buf_small (Value, 1, I2C_5, 0x3C, Register) != 0;
}

STATIC BOOLEAN
T214PcieSupplyEnable (WIFI_PCIE_POWER_STATE *State)
{
  UINT8 Sd3;
  UINTN Attempt;
  if (!T214PciePmicRead (0x25, &State->Config) ||
      !T214PciePmicRead (0x47, &State->Fps) ||
      !T214PciePmicRead (0x26, &State->PowerStatus) ||
      !T214PciePmicRead (0x19, &Sd3)) return FALSE;
  State->EnabledConfig = State->Config;
  Print (L"PCIe supply: LDO1=%02x FPS=%02x POK=%02x SD3=%02x\r\n",
         State->Config, State->Fps, State->PowerStatus, Sd3);
  // SD3 also supplies audio and other I/O. It is observed, never reprogrammed.
  if (Sd3 != 0x60) return FALSE;
  if (State->PowerStatus & 8) {
    UINT8 Selector = State->Config & 0x3F;
    // Preserve an already-powered 1.0-1.05 V shared XUSB/PCIe supply.
    return Selector >= 8 && Selector <= 10;
  }
  State->Changed = TRUE;
  if (!i2c_send_byte (I2C_5, 0x3C, 0x25, 0x08) ||
      !i2c_send_byte (I2C_5, 0x3C, 0x47, (UINT8)(State->Fps | 0xC0)) ||
      !i2c_send_byte (I2C_5, 0x3C, 0x25, 0xC8)) return FALSE;
  // 50 us minimum board ramp; allow the analog supply to settle before UPHY.
  MicroSecondDelay (1000);
  for (Attempt = 0; Attempt < 20; Attempt++) {
    if (!T214PciePmicRead (0x26, &State->PowerStatus)) return FALSE;
    if (State->PowerStatus & 8) break;
    MicroSecondDelay (100);
  }
  if (!T214PciePmicRead (0x25, &State->EnabledConfig)) return FALSE;
  Print (L"PCIe supply enabled: LDO1=%02x POK=%02x\r\n",
         State->EnabledConfig, State->PowerStatus);
  return State->EnabledConfig == 0xC8 && (State->PowerStatus & 8) != 0;
}

STATIC BOOLEAN
T214PcieSupplyRestore (WIFI_PCIE_POWER_STATE *State)
{
  BOOLEAN ConfigOk, FpsOk;
  if (!State->Changed) return TRUE;
  ConfigOk = i2c_send_byte (I2C_5, 0x3C, 0x25, State->Config) != 0;
  FpsOk = i2c_send_byte (I2C_5, 0x3C, 0x47, State->Fps) != 0;
  if (ConfigOk && FpsOk) State->Changed = FALSE;
  return ConfigOk && FpsOk;
}
#endif
