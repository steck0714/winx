#ifndef __SHIM_TIMERLIB_H__
#define __SHIM_TIMERLIB_H__

#include <PiDxe.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/TimerLib.h>

#define udelay(x) gBS->Stall((x))
/* U-Boot get_timer() and the SD/MMC timeout constants use milliseconds.
 * Keep timer_get_us() separate for the clock/PLL microsecond deadlines. */
#define get_timer(x) ((GetTimeInNanoSecond(GetPerformanceCounter()) / 1000000ULL) - (x))

inline UINT64 timer_get_us()
{
    return GetTimeInNanoSecond(GetPerformanceCounter()) / 1000;
}

#endif
