/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared snapshot used by the Switch model-specific Wi-Fi power policies. */
#ifndef WIFI_POWER_STATE_H
#define WIFI_POWER_STATE_H

typedef struct {
  UINT8   Config;
  UINT8   Fps;
  UINT8   EnabledConfig;
  UINT8   PowerStatus;
  BOOLEAN Changed;
} WIFI_PCIE_POWER_STATE;

typedef BOOLEAN (*WIFI_PCIE_POWER_RESTORE)(VOID *State);

#endif
