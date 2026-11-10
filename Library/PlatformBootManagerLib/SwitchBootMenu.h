// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef WINX_SWITCH_BOOT_MENU_H
#define WINX_SWITCH_BOOT_MENU_H

#include <Uefi.h>

typedef EFI_STATUS (EFIAPI *WINX_SWITCH_MENU_ACTION)(IN VOID *Context);

EFI_STATUS
SwitchBootMenuRun (
  IN CONST CHAR16             *ExtraEntryName OPTIONAL,
  IN WINX_SWITCH_MENU_ACTION  ExtraEntryAction OPTIONAL,
  IN VOID                     *ExtraEntryContext OPTIONAL
  );

#endif
