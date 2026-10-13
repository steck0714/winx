#pragma once
#include <PiDxe.h>
extern UINT64 g_counter;
static inline UINT64 GetPerformanceCounter (void) { return g_counter; }
