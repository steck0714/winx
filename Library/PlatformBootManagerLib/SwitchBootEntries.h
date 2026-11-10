// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef WINX_SWITCH_BOOT_ENTRIES_H_
#define WINX_SWITCH_BOOT_ENTRIES_H_

#include <Uefi.h>
#include <Library/UefiBootManagerLib.h>

typedef enum {
  WinxBootCategoryAll = 0,
  WinxBootCategoryWindows,
  WinxBootCategoryLinux,
  WinxBootCategoryAndroid,
  WinxBootCategoryOther,
  WinxBootCategoryMax
} WINX_BOOT_CATEGORY;

typedef struct {
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;
  WINX_BOOT_CATEGORY            Category;
} WINX_CUSTOM_BOOT_ENTRY;

EFI_STATUS
WinxBootEntriesLoad (
  OUT WINX_CUSTOM_BOOT_ENTRY  **Entries,
  OUT UINTN                   *EntryCount
  );

VOID
WinxBootEntriesFree (
  IN WINX_CUSTOM_BOOT_ENTRY  *Entries,
  IN UINTN                   EntryCount
  );

#endif
