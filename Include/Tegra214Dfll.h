// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef LITE_DFLL_HANDOFF_H
#define LITE_DFLL_HANDOFF_H
// Separate versioned record; retain the existing LITE_PERFORMANCE v1 ABI.
#define LITE_DFLL_HANDOFF_ADDRESS 0xA9FB0100UL
#define LITE_DFLL_HANDOFF_MAGIC   0x4C4C4644UL
#define LITE_DFLL_HANDOFF_VERSION 2u
typedef struct {
  unsigned int Magic, Version, Size, Hidrev;
  unsigned int Sku, CpuSpeedo, CpuProcessId, PmicAddress;
  unsigned int ClockMKhz; /* OSC_CTRL and SPARE_REG0 readback; fixed after firmware handoff */
} LITE_DFLL_HANDOFF;
/* T210/B01 OSC_FREQ=5 denotes the 38.4 MHz crystal. CLK_M has its own
 * divider in SPARE_REG0[3:2]; it is not necessarily PLLP_OUT0/anything.
 */
static inline unsigned int LiteDfllClockMKhz(unsigned int osc_ctrl, unsigned int spare)
{
  return (osc_ctrl >> 28) == 5u ? 38400u / (((spare >> 2) & 3u) + 1u) : 0u;
}
#endif
