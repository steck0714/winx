// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef WINX_SWITCH_SCREENSHOT_H
#define WINX_SWITCH_SCREENSHOT_H

#include <Uefi.h>

EFI_STATUS
WinxCaptureBootMenuScreenshot (
  OUT CHAR16  *SavedPath,
  IN  UINTN   SavedPathChars
  );

#endif
