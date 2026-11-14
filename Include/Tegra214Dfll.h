// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef LITE_DFLL_HANDOFF_H
#define LITE_DFLL_HANDOFF_H
// Separate versioned record; retain the existing LITE_PERFORMANCE v1 ABI.
#define LITE_DFLL_HANDOFF_ADDRESS 0xA9FB0100UL
#define LITE_DFLL_HANDOFF_MAGIC   0x4C4C4644UL
#define LITE_DFLL_HANDOFF_VERSION 1u
typedef struct {
  unsigned int Magic, Version, Size, Hidrev;
  unsigned int Sku, CpuSpeedo, CpuProcessId, PmicAddress;
} LITE_DFLL_HANDOFF;
#endif
