#pragma once
#include <PiDxe.h>
extern UINT64 g_freq;
static inline UINT64 ArmGenericTimerGetTimerFreq (void) { return g_freq; }
