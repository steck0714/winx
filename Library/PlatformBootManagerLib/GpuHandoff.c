// SPDX-License-Identifier: GPL-2.0-only
/* EDK II side of the GM20B hand-off (Erista / Lite / OLED).
 *
 * PlatformGpuHandoffBeforeBoot () is called right before Windows Boot Manager
 * is started. It makes sure the GPU is in reset and clamped (see GpuHandoff.h),
 * prints the result, and arms an ExitBootServices notification that re-checks
 * the same contract at the last moment, because anything that ran between the
 * menu and the OS loader (option ROM style drivers, the boot manager itself,
 * the display path) may have clocked or unclamped the GPU again.
 *
 * It never powers the GPU and never blocks the boot: every failure is reported
 * and Windows starts anyway (the driver then reports BUSY / a probe error
 * instead of the machine hanging in firmware).
 *
 * Built only with -DSWITCH_ERISTA_GPU=1, -DSWITCH_LITE_GPU=1 or
 * -DSWITCH_OLED_GPU=1. Not run on hardware yet.
 */
#if defined(SWITCH_ERISTA_GPU) || defined(SWITCH_LITE_GPU) || defined(SWITCH_OLED_GPU)
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "GpuHandoff.h"

#if defined(SWITCH_ERISTA_GPU)
#define WINX_GPU_HIDREV   GH_HIDREV_T210
#define WINX_GPU_BOARD    L"Erista"
#else
/* Lite (HDH-001) and OLED (HEG-001) are both T210B01. */
#define WINX_GPU_HIDREV   GH_HIDREV_T210B01
#if defined(SWITCH_OLED_GPU)
#define WINX_GPU_BOARD    L"OLED"
#else
#define WINX_GPU_BOARD    L"Lite"
#endif
#endif

STATIC EFI_EVENT                   mGpuHandoffEbsEvent;
STATIC enum gpu_handoff_result     mGpuHandoffLast = GPU_HANDOFF_BAD_ARGUMENT;
STATIC struct gpu_handoff_report   mGpuHandoffReport;

STATIC UINT32 GhRead32 (VOID *Context, UINT32 Address)
{
  (VOID)Context;
  return MmioRead32 (Address);
}

STATIC VOID GhWrite32 (VOID *Context, UINT32 Address, UINT32 Value)
{
  (VOID)Context;
  MmioWrite32 (Address, Value);
  MemoryFence ();
}

STATIC VOID GhDelay (VOID *Context, UINT32 Microseconds)
{
  (VOID)Context;
  gBS->Stall (Microseconds);
}

STATIC CONST struct gpu_handoff_io mGpuHandoffIo = { NULL, GhRead32, GhWrite32, GhDelay };

STATIC CONST CHAR16 *
GpuHandoffText (enum gpu_handoff_result Result)
{
  switch (Result) {
    case GPU_HANDOFF_COLD:          return L"cold (in reset, clamped)";
    case GPU_HANDOFF_RESTORED:      return L"was running; isolated again";
    case GPU_HANDOFF_WRONG_SOC:     return L"not this SoC; untouched";
    case GPU_HANDOFF_BUS_ERROR:     return L"bus error; untouched";
    case GPU_HANDOFF_DRAIN_TIMEOUT: return L"MC drain timeout; untouched";
    case GPU_HANDOFF_NOT_ISOLATED:  return L"NOT isolated after restore";
    default:                        return L"bad argument";
  }
}

/* Runs at ExitBootServices: no console, no allocation, no Print. */
STATIC VOID EFIAPI
GpuHandoffOnExitBootServices (IN EFI_EVENT Event, IN VOID *Context)
{
  (VOID)Event;
  (VOID)Context;
  mGpuHandoffLast = gpu_handoff_prepare (&mGpuHandoffIo, WINX_GPU_HIDREV, &mGpuHandoffReport);
}

VOID
PlatformGpuHandoffBeforeBoot (VOID)
{
  mGpuHandoffLast = gpu_handoff_prepare (&mGpuHandoffIo, WINX_GPU_HIDREV, &mGpuHandoffReport);
  Print (L"GPU hand-off (%s): %s\r\n", WINX_GPU_BOARD, GpuHandoffText (mGpuHandoffLast));
  Print (L"  RESET_X %08x -> %08x  CLAMP %08x -> %08x  CLOCK_X %08x -> %08x\r\n",
         mGpuHandoffReport.reset_before, mGpuHandoffReport.reset_after,
         mGpuHandoffReport.clamp_before, mGpuHandoffReport.clamp_after,
         mGpuHandoffReport.clocks_before, mGpuHandoffReport.clocks_after);
  if (mGpuHandoffEbsEvent == NULL) {
    if (EFI_ERROR (gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY,
                                     GpuHandoffOnExitBootServices, NULL, &mGpuHandoffEbsEvent))) {
      mGpuHandoffEbsEvent = NULL;
      Print (L"GPU hand-off: ExitBootServices re-check not armed.\r\n");
    }
  }
}
#endif
