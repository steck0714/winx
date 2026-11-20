// SPDX-License-Identifier: GPL-2.0-only
/* GpuHandoff.efi - bring-up tool for the GM20B hand-off, run from the UEFI Shell.
 *
 *   fs0:\> GpuHandoff.efi          read-only: print the GPU isolation state
 *   fs0:\> GpuHandoff.efi -fix     apply gpu_handoff_prepare() and print before/after
 *
 * Use it while developing the Windows TegraGpu driver or the hand-off logic: no BL33
 * rebuild is needed, and it shows exactly what the Windows driver's tg_gpu_power_up()
 * will see (it needs RESET_X bit 24 and CLAMP bit 0 both set, otherwise it returns BUSY).
 * The SoC (Erista T210 / Lite and OLED T210B01) is detected from HIDREV.
 * It never powers the GPU. Not run on hardware yet.
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/ShellParameters.h>
#include "../../Library/PlatformBootManagerLib/GpuHandoff.h"

STATIC UINT32 AppRead32 (VOID *Context, UINT32 Address)
{
  (VOID)Context;
  return MmioRead32 (Address);
}

STATIC VOID AppWrite32 (VOID *Context, UINT32 Address, UINT32 Value)
{
  (VOID)Context;
  MmioWrite32 (Address, Value);
  MemoryFence ();
}

STATIC VOID AppDelay (VOID *Context, UINT32 Microseconds)
{
  (VOID)Context;
  gBS->Stall (Microseconds);
}

STATIC BOOLEAN
HasArgument (IN EFI_HANDLE ImageHandle, IN CONST CHAR16 *Name)
{
  EFI_SHELL_PARAMETERS_PROTOCOL  *Parameters = NULL;
  UINTN                          Index;

  if (EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiShellParametersProtocolGuid,
                                      (VOID **)&Parameters)) || Parameters == NULL) {
    return FALSE;
  }
  for (Index = 1; Index < Parameters->Argc; ++Index) {
    if (StrCmp (Parameters->Argv[Index], Name) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC VOID
PrintState (IN CONST CHAR16 *Title)
{
  UINT32  Reset = MmioRead32 (GH_CAR_RESET_X);
  UINT32  Clamp = MmioRead32 (GH_PMC_GPU_CLAMP);
  UINT32  Clock = MmioRead32 (GH_CAR_CLOCK_X);

  Print (L"%s: RESET_X %08x  CLAMP %08x  CLOCK_X %08x  -> %s\r\n", Title, Reset, Clamp, Clock,
         gh_is_cold (Reset, Clamp) ? L"cold (driver can take it)" : L"NOT cold (driver returns BUSY)");
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST struct gpu_handoff_io  Io = { NULL, AppRead32, AppWrite32, AppDelay };
  struct gpu_handoff_report    Report;
  UINT32                       Hid = MmioRead32 (GH_APB_MISC_HIDREV);
  UINT32                       Expected = Hid & GH_HIDREV_MASK;

  (VOID)SystemTable;
  Print (L"GpuHandoff: HIDREV %08x (%s)\r\n", Hid,
         Expected == GH_HIDREV_T210 ? L"T210 / Erista" :
         Expected == GH_HIDREV_T210B01 ? L"T210B01 / Lite, OLED" : L"unknown SoC");
  if (Expected != GH_HIDREV_T210 && Expected != GH_HIDREV_T210B01) {
    return EFI_SUCCESS;   /* unknown chip: read nothing else, write nothing */
  }
  PrintState (L"before");
  if (HasArgument (ImageHandle, L"-fix")) {
    enum gpu_handoff_result  Result = gpu_handoff_prepare (&Io, Expected, &Report);
    Print (L"gpu_handoff_prepare = %u (0 cold, 1 restored, 2 bad arg, 3 wrong SoC, 4 bus error, 5 MC drain timeout, 6 not isolated)\r\n",
           (UINT32)Result);
    PrintState (L"after ");
  }
  return EFI_SUCCESS;
}
