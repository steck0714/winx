// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PlatformBm.h"
#include "SwitchBootMenu.h"
#include "../../Platforms/Switch/SwitchBoardDetect.h"
#include "../../Drivers/MouseCursorDxe/SwitchMouseCursor.h"
#include "../../Drivers/SidebandButtonDxe/SwitchSidebandButtons.h"

#include <Library/IoLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/SimpleTextIn.h>

typedef enum {
  WinxMenuLoadOption,
  WinxMenuExtraEntry,
  WinxMenuBootManager,
  WinxMenuReset
} WINX_MENU_ITEM_TYPE;

typedef struct {
  WINX_MENU_ITEM_TYPE Type;
  UINTN               OptionIndex;
} WINX_MENU_ITEM;

STATIC BOOLEAN
IsShellOption (
  IN CONST EFI_BOOT_MANAGER_LOAD_OPTION *Option
  )
{
  return (BOOLEAN)(Option->Description != NULL && StrCmp (Option->Description, L"UEFI Shell") == 0);
}

STATIC BOOLEAN  mMouseLeftDown;

STATIC EFI_STATUS
BootManagerMenu (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION Option;
  EFI_STATUS                   Status;

  Status = EfiBootManagerGetBootManagerMenu (&Option);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  EfiBootManagerBoot (&Option);
  Status = Option.Status;
  EfiBootManagerFreeLoadOption (&Option);
  return Status;
}

STATIC VOID
WaitForTouchRelease (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL *Pointer
  )
{
  EFI_ABSOLUTE_POINTER_STATE State;
  UINTN                      Attempt;

  // Avoid reopening a returned Shell/loader while the user is still touching
  // the panel. TouchPanelDxe publishes a new state at its periodic poll rate.
  for (Attempt = 0; Attempt < 250; ++Attempt) {
    gBS->Stall (20000);
    if (!EFI_ERROR (Pointer->GetState (Pointer, &State)) && State.ActiveButtons == 0) {
      return;
    }
  }
}

STATIC EFI_STATUS
ReadMenuInput (
  IN  UINTN                    ItemCount,
  IN  UINTN                    FirstItemRow,
  IN  UINTN                    *Selected,
  OUT BOOLEAN                  *Activate,
  OUT BOOLEAN                  *OpenBootManager,
  IN  BOOLEAN                  AllowBootManager
  )
{
  EFI_HANDLE                       *Handles = NULL;
  EFI_ABSOLUTE_POINTER_PROTOCOL    **Pointers = NULL;
  EFI_EVENT                        *Events = NULL;
  EFI_EVENT                        MouseChangeEvent = NULL;
  EFI_EVENT                        SidebandChangeEvent = NULL;
  WINX_MOUSE_CURSOR_PROTOCOL       *MouseCursor = NULL;
  WINX_SWITCH_BUTTON_PROTOCOL      *SidebandButtons = NULL;
  WINX_MOUSE_CURSOR_STATE          MouseState;
  UINTN                            PointerCount = 0;
  UINTN                            ValidPointerCount = 0;
  UINTN                            EventCount;
  UINTN                            EventIndex;
  UINTN                            SidebandEventIndex = MAX_UINTN;
  UINTN                            Index;
  EFI_STATUS                       Status;

  *Activate = FALSE;
  *OpenBootManager = FALSE;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiAbsolutePointerProtocolGuid,
                                    NULL, &PointerCount, &Handles);
  if (EFI_ERROR (Status)) {
    PointerCount = 0;
  }
  Pointers = AllocateZeroPool (sizeof (*Pointers) * PointerCount);
  Events = AllocatePool (sizeof (*Events) * (PointerCount + 3));
  if (Events == NULL || (PointerCount != 0 && Pointers == NULL)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Events[0] = gST->ConIn->WaitForKey;
  EventCount = 1;
  for (Index = 0; Index < PointerCount; ++Index) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiAbsolutePointerProtocolGuid,
                                       (VOID **)&Pointers[Index])) ||
        Pointers[Index] == NULL || Pointers[Index]->WaitForInput == NULL) {
      continue;
    }
    Pointers[ValidPointerCount] = Pointers[Index];
    Events[EventCount++] = Pointers[ValidPointerCount]->WaitForInput;
    ++ValidPointerCount;
  }

  if (!EFI_ERROR (gBS->LocateProtocol (&gWinXMouseCursorProtocolGuid, NULL, (VOID **)&MouseCursor)) &&
      MouseCursor != NULL && MouseCursor->WaitForChange != NULL) {
    MouseChangeEvent = MouseCursor->WaitForChange;
    Events[EventCount++] = MouseChangeEvent;
  }

  if (!EFI_ERROR (gBS->LocateProtocol (&gWinXSidebandButtonProtocolGuid, NULL,
                                        (VOID **)&SidebandButtons)) &&
      SidebandButtons != NULL && SidebandButtons->WaitForChange != NULL) {
    SidebandChangeEvent = SidebandButtons->WaitForChange;
    SidebandEventIndex = EventCount;
    Events[EventCount++] = SidebandChangeEvent;
  }

  for (;;) {
    Status = gBS->WaitForEvent (EventCount, Events, &EventIndex);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    if (EventIndex == 0) {
      EFI_INPUT_KEY  Key;

      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (EFI_ERROR (Status)) {
        goto Done;
      }
      if (Key.ScanCode == SCAN_UP || Key.UnicodeChar == L'k' || Key.UnicodeChar == L'K') {
        *Selected = (*Selected == 0) ? ItemCount - 1 : *Selected - 1;
      } else if (Key.ScanCode == SCAN_DOWN || Key.UnicodeChar == L'j' || Key.UnicodeChar == L'J') {
        *Selected = (*Selected + 1) % ItemCount;
      } else if (Key.ScanCode == SCAN_ESC) {
        *OpenBootManager = AllowBootManager;
      } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
        *Activate = TRUE;
      }
      break;
    }

    if (EventIndex <= ValidPointerCount) {
      EFI_ABSOLUTE_POINTER_PROTOCOL  *Pointer;
      EFI_ABSOLUTE_POINTER_STATE    State;
      UINTN                         Columns;
      UINTN                         Rows;
      UINT64                        Span;

      Pointer = Pointers[EventIndex - 1];
      if (Pointer != NULL && !EFI_ERROR (Pointer->GetState (Pointer, &State)) &&
          (State.ActiveButtons & 1) != 0 && Pointer->Mode != NULL &&
          Pointer->Mode->AbsoluteMaxY >= Pointer->Mode->AbsoluteMinY &&
          State.CurrentY >= Pointer->Mode->AbsoluteMinY &&
          State.CurrentY <= Pointer->Mode->AbsoluteMaxY) {
        Span = (UINT64)Pointer->Mode->AbsoluteMaxY - Pointer->Mode->AbsoluteMinY + 1;
        if (gST->ConOut == NULL || gST->ConOut->Mode == NULL ||
            EFI_ERROR (gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, &Columns, &Rows))) {
          Rows = 25;
        }
        {
          UINTN  Row;

          Row = (UINTN)(((UINT64)(State.CurrentY - Pointer->Mode->AbsoluteMinY) * Rows) / Span);
          if (Row >= FirstItemRow && Row - FirstItemRow < ItemCount) {
            *Selected = Row - FirstItemRow;
            *Activate = TRUE;
            WaitForTouchRelease (Pointer);
          }
        }
      }
      break;
    }

    if (MouseChangeEvent != NULL && EventIndex == ValidPointerCount + 1 &&
        !EFI_ERROR (MouseCursor->GetState (MouseCursor, &MouseState))) {
      BOOLEAN  WasDown;
      UINTN    Columns;
      UINTN    Rows;
      UINTN    Row;

      WasDown = mMouseLeftDown;
      mMouseLeftDown = MouseState.LeftButton;
      if (!MouseState.Visible || MouseState.ScreenHeight == 0 ||
          gST->ConOut == NULL || gST->ConOut->Mode == NULL ||
          EFI_ERROR (gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, &Columns, &Rows))) {
        continue;
      }

      Row = (UINTN)(((UINT64)MouseState.Y * Rows) / MouseState.ScreenHeight);
      if (Row >= FirstItemRow && Row - FirstItemRow < ItemCount) {
        UINTN  MouseSelection;

        MouseSelection = Row - FirstItemRow;
        if (MouseSelection != *Selected) {
          *Selected = MouseSelection;
          if (MouseState.LeftButton && !WasDown) {
            *Activate = TRUE;
          }
          break;
        }
        if (MouseState.LeftButton && !WasDown) {
          *Activate = TRUE;
          break;
        }
      }
    }

    if (SidebandChangeEvent != NULL && EventIndex == SidebandEventIndex) {
      UINT32  Pressed;

      Status = SidebandButtons->ReadState (SidebandButtons, &Pressed);
      if (EFI_ERROR (Status)) {
        goto Done;
      }
      if (Pressed == 0) {
        continue;
      }
      if ((Pressed & WINX_SWITCH_BUTTON_VOLUME_UP) != 0) {
        *Selected = (*Selected == 0) ? ItemCount - 1 : *Selected - 1;
      } else if ((Pressed & WINX_SWITCH_BUTTON_VOLUME_DOWN) != 0) {
        *Selected = (*Selected + 1) % ItemCount;
      }
      if ((Pressed & WINX_SWITCH_BUTTON_POWER) != 0) {
        *Activate = TRUE;
      }
      break;
    }
  }

Done:
  if (Events != NULL) FreePool (Events);
  if (Pointers != NULL) FreePool (Pointers);
  if (Handles != NULL) FreePool (Handles);
  return Status;
}

EFI_STATUS
SwitchBootMenuRun (
  IN CONST CHAR16             *ExtraEntryName OPTIONAL,
  IN WINX_SWITCH_MENU_ACTION  ExtraEntryAction OPTIONAL,
  IN VOID                     *ExtraEntryContext OPTIONAL
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  WINX_MENU_ITEM                *Items;
  UINTN                         OptionCount;
  UINTN                         ItemCount;
  UINTN                         VisibleOptions;
  UINTN                         Selected = 0;
  UINTN                         FirstItemRow;
  UINTN                         Index;
  UINTN                         Item;
  UINTN                         VisibleIndex;
  UINTN                         *OptionMap;
  BOOLEAN                       Pinned;
  BOOLEAN                       Matches;
  BOOLEAN                       AllowBootManager;
  BOOLEAN                       Activate;
  BOOLEAN                       OpenBootManager;
  EFI_STATUS                    Status;
  WINX_SWITCH_BOARD_INFO        Board;

  if (gST->ConIn == NULL || gST->ConOut == NULL || gST->ConIn->WaitForKey == NULL) {
    return EFI_UNSUPPORTED;
  }

  Board = WinxSwitchDetectBoard ();
  Pinned = WinxSwitchBuildHasPinnedBoard ();
  Matches = WinxSwitchBuildMatchesBoard (Board.Board);
  AllowBootManager = (BOOLEAN)(!Pinned || Matches);
  if (ExtraEntryName == NULL || ExtraEntryAction == NULL) {
    ExtraEntryName = NULL;
    ExtraEntryAction = NULL;
  }

  for (;;) {
    EfiBootManagerRefreshAllBootOption ();
    Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
    VisibleOptions = 0;
    OptionMap = AllocatePool (sizeof (*OptionMap) * OptionCount);
    if (OptionCount != 0 && OptionMap == NULL) {
      EfiBootManagerFreeLoadOptions (Options, OptionCount);
      return EFI_OUT_OF_RESOURCES;
    }
    for (Index = 0; Index < OptionCount; ++Index) {
      if (!Pinned || Matches || IsShellOption (&Options[Index])) {
        OptionMap[VisibleOptions++] = Index;
      }
    }

    ItemCount = VisibleOptions + 1 + (AllowBootManager ? 1 : 0) +
                (ExtraEntryName != NULL ? 1 : 0);
    Items = AllocateZeroPool (sizeof (*Items) * ItemCount);
    if (Items == NULL) {
      if (OptionMap != NULL) FreePool (OptionMap);
      EfiBootManagerFreeLoadOptions (Options, OptionCount);
      return EFI_OUT_OF_RESOURCES;
    }
    Item = 0;
    if (ExtraEntryName != NULL) {
      Items[Item++].Type = WinxMenuExtraEntry;
    }
    for (VisibleIndex = 0; VisibleIndex < VisibleOptions; ++VisibleIndex) {
      Items[Item].Type = WinxMenuLoadOption;
      Items[Item++].OptionIndex = OptionMap[VisibleIndex];
    }
    if (AllowBootManager) {
      Items[Item++].Type = WinxMenuBootManager;
    }
    Items[Item].Type = WinxMenuReset;

    gST->ConOut->ClearScreen (gST->ConOut);
    Print (L"WinX UEFI  |  Nintendo Switch boot menu\r\n");
    Print (L"Hekate / BL33 hardware: %s  (HIDREV %08x, ODM4 %08x)\r\n",
           WinxSwitchBoardName (Board.Board), Board.HidRev, Board.Odm4);
    if (Pinned && !Matches) {
      Print (L"BUILD PROFILE MISMATCH: board-specific OS entries are hidden.\r\n");
    } else {
      Print (L"Arrows/volume buttons move; Enter/Power selects; tap selects; Esc opens firmware options.\r\n");
    }
    Print (L"\r\n");
    FirstItemRow = (gST->ConOut->Mode != NULL) ? gST->ConOut->Mode->CursorRow : 0;
    for (Item = 0; Item < ItemCount; ++Item) {
      CONST CHAR16 *Name;
      if (Item == 0 && ExtraEntryName != NULL) {
        Name = ExtraEntryName;
      } else if (Items[Item].Type == WinxMenuLoadOption) {
        Name = Options[Items[Item].OptionIndex].Description;
      } else if (Items[Item].Type == WinxMenuBootManager) {
        Name = L"UEFI firmware boot options";
      } else {
        Name = L"Restart Switch";
      }
      Print (L"%s %s\r\n", Item == Selected ? L">" : L" ", Name != NULL ? Name : L"Unnamed boot option");
    }
    Print (L"\r\nUEFI Shell stays available as a selectable boot entry.\r\n");

    Status = ReadMenuInput (ItemCount, FirstItemRow, &Selected, &Activate,
                            &OpenBootManager, AllowBootManager);
    if (EFI_ERROR (Status)) {
      FreePool (Items);
      if (OptionMap != NULL) FreePool (OptionMap);
      EfiBootManagerFreeLoadOptions (Options, OptionCount);
      return Status;
    }
    if (OpenBootManager) {
      Status = BootManagerMenu ();
    } else if (Activate) {
      if (Items[Selected].Type == WinxMenuExtraEntry) {
        Status = ExtraEntryAction (ExtraEntryContext);
      } else if (Items[Selected].Type == WinxMenuLoadOption) {
        EFI_BOOT_MANAGER_LOAD_OPTION *Option = &Options[Items[Selected].OptionIndex];
#ifdef SWITCH_WIFI_ERISTA
        if (Option->Description != NULL && StrStr (Option->Description, L"Windows") != NULL) {
          PlatformWifiPrepareBeforeBoot ();
        }
#endif
        EfiBootManagerBoot (Option);
        Status = Option->Status;
      } else if (Items[Selected].Type == WinxMenuBootManager) {
        Status = BootManagerMenu ();
      } else {
        gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
        Status = EFI_SUCCESS;
      }
      if (EFI_ERROR (Status)) {
        EFI_INPUT_KEY Key;
        Print (L"\r\nBoot returned: %r\r\nPress a key to return to the menu.\r\n", Status);
        gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
        gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      }
    }

    FreePool (Items);
    if (OptionMap != NULL) FreePool (OptionMap);
    EfiBootManagerFreeLoadOptions (Options, OptionCount);
  }
}
