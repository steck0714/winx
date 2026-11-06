// SPDX-License-Identifier: GPL-2.0-only
// Voltage data: Copyright (c) 2017 NVIDIA CORPORATION; 2022-2023 CTCaer.
// VDD_CORE ceilings from Switchroot tegra210b01[-slt]-core-dvfs.c.
// Use the highest requirement across the process bins; do not change the rail.
#ifndef LITE_EMC_POLICY_H
#define LITE_EMC_POLICY_H

STATIC UINT32 LiteEmcRequiredMv (UINT32 RequestedKhz, UINT64 RoundedHz)
{
  STATIC CONST struct { UINT32 Khz, Mv; } Bands[] = {
    { 204000, 637 }, { 408000, 637 }, { 800000, 637 }, { 1065600, 637 },
    { 1331200, 650 }, { 1600000, 675 }, { 1866000, 700 }, { 2133000, 725 }
  };
  UINTN Index;
  if (RoundedHz == 0 || RoundedHz > (UINT64)RequestedKhz * 1000) return 0;
  for (Index = 0; Index < sizeof (Bands) / sizeof (Bands[0]); ++Index)
    if (RoundedHz <= (UINT64)Bands[Index].Khz * 1000) return Bands[Index].Mv;
  return 0;
}

#endif
