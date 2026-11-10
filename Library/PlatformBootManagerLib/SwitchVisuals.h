// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_VISUALS_H_
#define SWITCH_VISUALS_H_

#include <Uefi.h>

EFI_STATUS
WinxVisualsShowFirmwareSplash (
  VOID
  );

EFI_STATUS
WinxVisualsDrawMenuBackground (
  VOID
  );

VOID
WinxVisualsInvalidateBootLogo (
  VOID
  );

VOID
WinxVisualsShowBootSplash (
  IN CONST CHAR16  *Description
  );

VOID
WinxVisualsPrepareShell (
  VOID
  );

#endif
