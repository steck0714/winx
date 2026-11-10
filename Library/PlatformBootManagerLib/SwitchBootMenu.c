// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PlatformBm.h"
#include "SwitchBootMenu.h"
#include "SwitchBootEntries.h"
#include "SwitchScreenshot.h"
#include "SwitchVisuals.h"
#include "../../Platforms/Switch/SwitchBoardDetect.h"
#include "../../Drivers/MouseCursorDxe/SwitchMouseCursor.h"

#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimplePointer.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/WinXJoyConInput.h>

typedef enum {
  WinxMenuLoadOption,
  WinxMenuCustomLoadOption,
  WinxMenuExtraEntry,
  WinxMenuBootManager,
  WinxMenuScreenshot,
  WinxMenuDiagnostics,
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
STATIC BOOLEAN  mTouchActivationLatch;
STATIC BOOLEAN  mScreenshotStatusValid;
STATIC EFI_STATUS mScreenshotStatus;
STATIC CHAR16   mScreenshotStatusPath[128];
STATIC BOOLEAN  mBootStatusValid;
STATIC EFI_STATUS mLastBootStatus;
STATIC UINT32   mLastJoyConButtons;

#define WINX_MENU_LIST_FIRST_ROW  6
#define WINX_MENU_TAB_ROW         3

STATIC CONST CHAR16 *mWinxCategoryNames[WinxBootCategoryMax] = {
  L"All", L"Windows", L"Linux", L"Android", L"Other"
};

STATIC
BOOLEAN
ContainsAsciiNoCase (
  IN CONST CHAR16  *Text,
  IN CONST CHAR16  *Needle
  )
{
  CONST CHAR16  *Start;
  CONST CHAR16  *Left;
  CONST CHAR16  *Right;

  if ((Text == NULL) || (Needle == NULL) || (Needle[0] == L'\0')) {
    return FALSE;
  }

  for (Start = Text; *Start != L'\0'; ++Start) {
    Left = Start;
    Right = Needle;
    while ((*Left != L'\0') && (*Right != L'\0')) {
      CHAR16  A;
      CHAR16  B;

      A = *Left;
      B = *Right;
      if ((A >= L'A') && (A <= L'Z')) A = (CHAR16)(A + (L'a' - L'A'));
      if ((B >= L'A') && (B <= L'Z')) B = (CHAR16)(B + (L'a' - L'A'));
      if (A != B) break;
      ++Left;
      ++Right;
    }
    if (*Right == L'\0') return TRUE;
  }

  return FALSE;
}

STATIC
WINX_BOOT_CATEGORY
ClassifyBootOption (
  IN CONST EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  )
{
  CONST CHAR16  *Description;

  Description = (Option != NULL) ? Option->Description : NULL;
  if (ContainsAsciiNoCase (Description, L"windows") ||
      ContainsAsciiNoCase (Description, L"winarm") ||
      ContainsAsciiNoCase (Description, L"winpe"))
  {
    return WinxBootCategoryWindows;
  }

  if (ContainsAsciiNoCase (Description, L"android") ||
      ContainsAsciiNoCase (Description, L"lineage") ||
      ContainsAsciiNoCase (Description, L"bliss"))
  {
    return WinxBootCategoryAndroid;
  }

  if (ContainsAsciiNoCase (Description, L"linux") ||
      ContainsAsciiNoCase (Description, L"grub") ||
      ContainsAsciiNoCase (Description, L"ubuntu") ||
      ContainsAsciiNoCase (Description, L"debian") ||
      ContainsAsciiNoCase (Description, L"fedora") ||
      ContainsAsciiNoCase (Description, L"arch") ||
      ContainsAsciiNoCase (Description, L"l4t"))
  {
    return WinxBootCategoryLinux;
  }

  return WinxBootCategoryOther;
}

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

STATIC
VOID
WriteTextAt (
  IN UINTN        ScreenColumns,
  IN UINTN        Column,
  IN UINTN        Row,
  IN UINTN        Width,
  IN UINTN        Attribute,
  IN CONST CHAR16 *Text
  )
{
  CHAR16  Line[256];
  UINTN   Index;

  if ((gST->ConOut == NULL) || (Column >= ScreenColumns) || (Width == 0)) {
    return;
  }

  if (Width > ARRAY_SIZE (Line) - 1) Width = ARRAY_SIZE (Line) - 1;
  if (Width > ScreenColumns - Column) Width = ScreenColumns - Column;
  Index = 0;
  if (Text != NULL) {
    while ((Index < Width) && (Text[Index] != L'\0')) {
      Line[Index] = Text[Index];
      ++Index;
    }
  }
  while (Index < Width) {
    Line[Index++] = L' ';
  }
  Line[Width] = L'\0';

  gST->ConOut->SetAttribute (gST->ConOut, Attribute);
  gST->ConOut->SetCursorPosition (gST->ConOut, Column, Row);
  gST->ConOut->OutputString (gST->ConOut, Line);
}

STATIC
VOID
GetConsoleDimensions (
  OUT UINTN  *Columns,
  OUT UINTN  *Rows
  )
{
  *Columns = 80;
  *Rows = 25;
  if ((gST->ConOut != NULL) && (gST->ConOut->Mode != NULL)) {
    gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, Columns, Rows);
  }
  if (*Columns == 0) *Columns = 80;
  if (*Rows == 0) *Rows = 25;
}

STATIC
EFI_BOOT_MANAGER_LOAD_OPTION *
GetItemOption (
  IN WINX_MENU_ITEM             *Item,
  IN EFI_BOOT_MANAGER_LOAD_OPTION *Options,
  IN WINX_CUSTOM_BOOT_ENTRY     *CustomEntries
  )
{
  if (Item->Type == WinxMenuLoadOption) {
    return &Options[Item->OptionIndex];
  }
  if (Item->Type == WinxMenuCustomLoadOption) {
    return &CustomEntries[Item->OptionIndex].Option;
  }
  return NULL;
}

STATIC
VOID
FormatItemName (
  IN  WINX_MENU_ITEM               *Item,
  IN  EFI_BOOT_MANAGER_LOAD_OPTION *Options,
  IN  WINX_CUSTOM_BOOT_ENTRY       *CustomEntries,
  IN  CONST CHAR16                 *ExtraEntryName,
  OUT CHAR16                       *Buffer,
  IN  UINTN                        BufferSize
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *Option;

  Option = GetItemOption (Item, Options, CustomEntries);
  if (Option != NULL) {
    if (Item->Type == WinxMenuCustomLoadOption) {
      UnicodeSPrint (Buffer, BufferSize, L"%s   Custom",
                     Option->Description != NULL ? Option->Description : L"Unnamed EFI app");
    } else {
      UnicodeSPrint (Buffer, BufferSize, L"%s   Boot %04x",
                     Option->Description != NULL ? Option->Description : L"Unnamed boot option",
                     Option->OptionNumber & 0xFFFF);
    }
    return;
  }

  switch (Item->Type) {
    case WinxMenuExtraEntry:      UnicodeSPrint (Buffer, BufferSize, L"%s", ExtraEntryName != NULL ? ExtraEntryName : L"WinX utility"); break;
    case WinxMenuBootManager:    UnicodeSPrint (Buffer, BufferSize, L"Firmware boot options"); break;
    case WinxMenuScreenshot:     UnicodeSPrint (Buffer, BufferSize, L"Save screenshot"); break;
    case WinxMenuDiagnostics:    UnicodeSPrint (Buffer, BufferSize, L"Diagnostics"); break;
    default:                     UnicodeSPrint (Buffer, BufferSize, L"Restart"); break;
  }
}

STATIC
UINTN
CountProtocolHandles (
  IN CONST EFI_GUID  *Protocol
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;

  Handles = NULL;
  Count = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, (EFI_GUID *)Protocol, NULL, &Count, &Handles))) {
    return 0;
  }
  if (Handles != NULL) FreePool (Handles);
  return Count;
}

STATIC
VOID
DrawDiagnostics (
  IN WINX_SWITCH_BOARD_INFO  Board,
  IN UINTN                   BootOptionCount,
  IN UINTN                   CustomOptionCount,
  IN BOOLEAN                 Pinned,
  IN BOOLEAN                 Matches
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  WINX_MOUSE_CURSOR_PROTOCOL    *MouseCursor;
  WINX_JOYCON_INPUT_PROTOCOL    *JoyCon;
  UINTN                         Columns;
  UINTN                         Rows;
  UINTN                         FileSystems;
  UINTN                         BlockDevices;
  UINTN                         AbsolutePointers;
  UINTN                         RelativePointers;
  UINTN                         Row;
  CHAR16                        Line[256];
  BOOLEAN                       HasGop;
  BOOLEAN                       HasMouseCursor;
  BOOLEAN                       HasJoyCon;

  GetConsoleDimensions (&Columns, &Rows);
  gST->ConOut->ClearScreen (gST->ConOut);
  WinxVisualsDrawMenuBackground ();
  WriteTextAt (Columns, 2, 1, Columns > 4 ? Columns - 4 : Columns, EFI_CYAN | EFI_BRIGHT | EFI_BACKGROUND_BLACK,
               L"WINX / READ-ONLY DIAGNOSTICS");
  WriteTextAt (Columns, 2, 2, Columns > 4 ? Columns - 4 : Columns, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK,
               L"Quick protocol and configuration checks; no stress test or hardware writes are run.");

  Gop = NULL;
  HasGop = (BOOLEAN)!EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) &&
           (Gop != NULL) && (Gop->Mode != NULL) && (Gop->Mode->Info != NULL);
  MouseCursor = NULL;
  HasMouseCursor = (BOOLEAN)!EFI_ERROR (gBS->LocateProtocol (&gWinXMouseCursorProtocolGuid, NULL, (VOID **)&MouseCursor)) &&
                   (MouseCursor != NULL);
  JoyCon = NULL;
  HasJoyCon = (BOOLEAN)!EFI_ERROR (gBS->LocateProtocol (&gWinXJoyConInputProtocolGuid, NULL, (VOID **)&JoyCon)) &&
              (JoyCon != NULL);
  AbsolutePointers = CountProtocolHandles (&gEfiAbsolutePointerProtocolGuid);
  RelativePointers = CountProtocolHandles (&gEfiSimplePointerProtocolGuid);
  FileSystems = CountProtocolHandles (&gEfiSimpleFileSystemProtocolGuid);
  BlockDevices = CountProtocolHandles (&gEfiBlockIoProtocolGuid);

  Row = 4;
  UnicodeSPrint (Line, sizeof (Line), L"BOARD  %s   HIDREV %08x   ODM4 %08x",
                 WinxSwitchBoardName (Board.Board), Board.HidRev, Board.Odm4);
  WriteTextAt (Columns, 2, Row++, Columns - 4, EFI_LIGHTGRAY | EFI_BRIGHT | EFI_BACKGROUND_BLACK, Line);
  UnicodeSPrint (Line, sizeof (Line), L"PROFILE  %s", !Pinned ? L"generic" : (Matches ? L"matches detected board" : L"MISMATCH"));
  WriteTextAt (Columns, 2, Row++, Columns - 4, (Pinned && !Matches) ? EFI_RED | EFI_BRIGHT | EFI_BACKGROUND_BLACK : EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);

  UnicodeSPrint (Line, sizeof (Line), L"GOP  %s%s", HasGop ? L"present  " : L"missing",
                 HasGop ? L"" : L"(display diagnostics unavailable)");
  if (HasGop) {
    UnicodeSPrint (Line, sizeof (Line), L"GOP  present  %ux%u  mode %u",
                   Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution,
                   Gop->Mode->Mode);
  }
  WriteTextAt (Columns, 2, Row++, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);
  UnicodeSPrint (Line, sizeof (Line), L"Console input  %s  (protocol presence only)",
                 (gST->ConIn != NULL && gST->ConIn->WaitForKey != NULL) ? L"present" : L"missing");
  WriteTextAt (Columns, 2, Row++, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);
  UnicodeSPrint (Line, sizeof (Line), L"Touch/absolute pointer handles  %u  (presence is not a touch test)", AbsolutePointers);
  WriteTextAt (Columns, 2, Row++, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);
  UnicodeSPrint (Line, sizeof (Line), L"Relative pointer handles  %u  | mouse cursor protocol  %s",
                 RelativePointers, HasMouseCursor ? L"present" : L"missing");
  WriteTextAt (Columns, 2, Row++, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);
  UnicodeSPrint (Line, sizeof (Line), L"Joy-Con input protocol  %s  (presence is not a hardware test)",
                 HasJoyCon ? L"present" : L"not installed");
  WriteTextAt (Columns, 2, Row++, Columns - 4, HasJoyCon ? EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK : EFI_BROWN | EFI_BRIGHT | EFI_BACKGROUND_BLACK, Line);
  UnicodeSPrint (Line, sizeof (Line), L"Simple file systems  %u  | block I/O handles  %u",
                 FileSystems, BlockDevices);
  WriteTextAt (Columns, 2, Row++, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);
  UnicodeSPrint (Line, sizeof (Line), L"Boot entries  %u firmware + %u configured custom EFI paths",
                 BootOptionCount, CustomOptionCount);
  WriteTextAt (Columns, 2, Row++, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);

  if (mBootStatusValid) {
    UnicodeSPrint (Line, sizeof (Line), L"Last returned boot status  %r", mLastBootStatus);
    WriteTextAt (Columns, 2, Row++, Columns - 4, EFI_BROWN | EFI_BRIGHT | EFI_BACKGROUND_BLACK, Line);
  }
  if (mScreenshotStatusValid) {
    UnicodeSPrint (Line, sizeof (Line), L"Last screenshot  %s%r",
                   EFI_ERROR (mScreenshotStatus) ? L"failed " : L"saved ", mScreenshotStatus);
    WriteTextAt (Columns, 2, Row++, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);
  }

  WriteTextAt (Columns, 2, Rows > 2 ? Rows - 2 : 0, Columns - 4, EFI_CYAN | EFI_BRIGHT | EFI_BACKGROUND_BLACK,
               L"ESC / B / any tap: return to boot menu");
  DEBUG ((DEBUG_INFO, "WinX diagnostics: board=%u gop=%u abs=%u rel=%u joycon=%u fs=%u block=%u boot=%u custom=%u\n",
          Board.Board, HasGop, AbsolutePointers, RelativePointers, HasJoyCon,
          FileSystems, BlockDevices, BootOptionCount, CustomOptionCount));
}

STATIC
VOID
DrawBootMenu (
  IN  WINX_SWITCH_BOARD_INFO        Board,
  IN  EFI_BOOT_MANAGER_LOAD_OPTION *Options,
  IN  UINTN                         OptionCount,
  IN  WINX_CUSTOM_BOOT_ENTRY       *CustomEntries,
  IN  UINTN                         CustomEntryCount,
  IN  CONST CHAR16                 *ExtraEntryName,
  IN  WINX_MENU_ITEM               *Items,
  IN  UINTN                         ItemCount,
  IN  UINTN                         Selected,
  IN  WINX_BOOT_CATEGORY            Category,
  IN  BOOLEAN                       Pinned,
  IN  BOOLEAN                       Matches,
  OUT UINTN                        *ColumnsOut,
  OUT UINTN                        *RowsOut,
  OUT UINTN                        *VisibleStartOut,
  OUT UINTN                        *VisibleCountOut
  )
{
  UINTN       Columns;
  UINTN       Rows;
  UINTN       FooterRow;
  UINTN       DetailRow;
  UINTN       VisibleRows;
  UINTN       VisibleStart;
  UINTN       VisibleIndex;
  UINTN       Index;
  UINTN       CategoryCount;
  UINTN       TabWidth;
  UINTN       Row;
  CHAR16      Line[256];
  CHAR16      Name[224];
  CHAR16      *PathText;
  EFI_BOOT_MANAGER_LOAD_OPTION *Option;
  UINTN       TextAttribute;

  GetConsoleDimensions (&Columns, &Rows);
  FooterRow = (Rows > 4) ? Rows - 4 : Rows - 1;
  DetailRow = (FooterRow > 0) ? FooterRow - 1 : 0;
  VisibleRows = (DetailRow > WINX_MENU_LIST_FIRST_ROW) ? DetailRow - WINX_MENU_LIST_FIRST_ROW : 1;
  if (VisibleRows == 0) VisibleRows = 1;
  VisibleStart = (Selected / VisibleRows) * VisibleRows;

  *ColumnsOut = Columns;
  *RowsOut = Rows;
  *VisibleStartOut = VisibleStart;
  *VisibleCountOut = VisibleRows;

  WriteTextAt (Columns, 2, 0, Columns - 4, EFI_LIGHTGRAY | EFI_BRIGHT | EFI_BACKGROUND_BLACK,
               L"WinX");
  UnicodeSPrint (Line, sizeof (Line), L"%s   %u options",
                 WinxSwitchBoardName (Board.Board), OptionCount + CustomEntryCount);
  WriteTextAt (Columns, 2, 1, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);
  if (Pinned && !Matches) {
    WriteTextAt (Columns, 2, 2, Columns - 4, EFI_RED | EFI_BRIGHT | EFI_BACKGROUND_BLACK,
                 L"This build targets a different Switch model. Board-specific options are hidden.");
  } else {
    WriteTextAt (Columns, 2, 2, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, L"");
  }

  TabWidth = Columns / WinxBootCategoryMax;
  for (Index = 0; Index < WinxBootCategoryMax; ++Index) {
    CategoryCount = 0;
    if (Index == WinxBootCategoryAll) {
      UINTN  OptionIndex;

      for (OptionIndex = 0; OptionIndex < OptionCount; ++OptionIndex) {
        if (!Pinned || Matches || IsShellOption (&Options[OptionIndex])) ++CategoryCount;
      }
      if (!Pinned || Matches) CategoryCount += CustomEntryCount;
    } else {
      UINTN  OptionIndex;

      for (OptionIndex = 0; OptionIndex < OptionCount; ++OptionIndex) {
        if ((!Pinned || Matches || IsShellOption (&Options[OptionIndex])) &&
            (ClassifyBootOption (&Options[OptionIndex]) == (WINX_BOOT_CATEGORY)Index))
        {
          ++CategoryCount;
        }
      }
      if (!Pinned || Matches) {
        for (OptionIndex = 0; OptionIndex < CustomEntryCount; ++OptionIndex) {
          if (CustomEntries[OptionIndex].Category == (WINX_BOOT_CATEGORY)Index) ++CategoryCount;
        }
      }
    }
    UnicodeSPrint (Line, sizeof (Line), L"%s  (%u)", mWinxCategoryNames[Index], CategoryCount);
    WriteTextAt (Columns, Index * TabWidth, WINX_MENU_TAB_ROW, TabWidth,
                 (Category == (WINX_BOOT_CATEGORY)Index) ? EFI_LIGHTGRAY | EFI_BACKGROUND_BLUE : EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK,
                 Line);
  }

  WriteTextAt (Columns, 0, 4, Columns, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, L"");
  WriteTextAt (Columns, 2, 5, Columns - 4, EFI_LIGHTGRAY | EFI_BRIGHT | EFI_BACKGROUND_BLACK,
               L"Boot options");

  for (VisibleIndex = 0; VisibleIndex < VisibleRows; ++VisibleIndex) {
    Row = WINX_MENU_LIST_FIRST_ROW + VisibleIndex;
    Index = VisibleStart + VisibleIndex;
    if (Index < ItemCount) {
      FormatItemName (&Items[Index], Options, CustomEntries, ExtraEntryName, Name, sizeof (Name));
      TextAttribute = (Index == Selected) ? EFI_LIGHTGRAY | EFI_BACKGROUND_BLUE : EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK;
      UnicodeSPrint (Line, sizeof (Line), L"%c  %s", (Index == Selected) ? L'>' : L' ', Name);
      WriteTextAt (Columns, 1, Row, Columns - 2, TextAttribute, Line);
    } else {
      WriteTextAt (Columns, 1, Row, Columns - 2, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, L"");
    }
  }

  WriteTextAt (Columns, 2, DetailRow, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, L"");
  if (Selected < ItemCount) {
    Option = GetItemOption (&Items[Selected], Options, CustomEntries);
    if (Option != NULL) {
      PathText = ConvertDevicePathToText (Option->FilePath, TRUE, FALSE);
      UnicodeSPrint (Line, sizeof (Line), L"Path: %s", PathText != NULL ? PathText : L"unavailable");
      WriteTextAt (Columns, 2, DetailRow, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);
      if (PathText != NULL) FreePool (PathText);
    } else if (Items[Selected].Type == WinxMenuDiagnostics) {
      WriteTextAt (Columns, 2, DetailRow, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK,
                   L"Read-only firmware and device checks");
    } else if (Items[Selected].Type == WinxMenuScreenshot) {
      if (mScreenshotStatusValid) {
        if (mScreenshotStatusPath[0] != L'\0') {
          UnicodeSPrint (Line, sizeof (Line), L"Screenshot: %s", mScreenshotStatusPath);
          WriteTextAt (Columns, 2, DetailRow, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, Line);
        } else {
          UnicodeSPrint (Line, sizeof (Line), L"Screenshot: %r", mScreenshotStatus);
          WriteTextAt (Columns, 2, DetailRow, Columns - 4, EFI_BROWN | EFI_BRIGHT | EFI_BACKGROUND_BLACK, Line);
        }
      }
    }
  }

  WriteTextAt (Columns, 2, FooterRow, Columns - 4, EFI_LIGHTGRAY | EFI_BRIGHT | EFI_BACKGROUND_BLACK,
               L"Up / Down  Select     Left / Right  Filter     Enter / A  Start");
  WriteTextAt (Columns, 2, FooterRow + 1, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK,
               L"F10 / D  Diagnostics     F12 / Capture  Screenshot     Esc / B  Firmware");
  if (mBootStatusValid) {
    UnicodeSPrint (Line, sizeof (Line), L"Last boot returned: %r", mLastBootStatus);
    WriteTextAt (Columns, 2, Rows - 1, Columns - 4, EFI_BROWN | EFI_BRIGHT | EFI_BACKGROUND_BLACK, Line);
  } else {
    WriteTextAt (Columns, 2, Rows - 1, Columns - 4, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK, L"");
  }
}

STATIC EFI_STATUS
ReadMenuInput (
  IN  UINTN                    ItemCount,
  IN  UINTN                    FirstItemRow,
  IN  UINTN                    VisibleStart,
  IN  UINTN                    VisibleCount,
  IN  UINTN                    Columns,
  IN  UINTN                    Rows,
  IN  UINTN                    *Selected,
  IN OUT WINX_BOOT_CATEGORY    *Category,
  OUT BOOLEAN                  *CategoryChanged,
  OUT BOOLEAN                  *Activate,
  OUT BOOLEAN                  *OpenBootManager,
  OUT BOOLEAN                  *RequestScreenshot,
  OUT BOOLEAN                  *OpenDiagnostics,
  IN  BOOLEAN                  AllowBootManager
  )
{
  EFI_HANDLE                       *Handles = NULL;
  EFI_ABSOLUTE_POINTER_PROTOCOL    **Pointers = NULL;
  EFI_EVENT                        *Events = NULL;
  EFI_EVENT                        MouseChangeEvent = NULL;
  EFI_EVENT                        JoyConEvent = NULL;
  WINX_MOUSE_CURSOR_PROTOCOL       *MouseCursor = NULL;
  WINX_JOYCON_INPUT_PROTOCOL       *JoyCon = NULL;
  WINX_MOUSE_CURSOR_STATE          MouseState;
  UINTN                            PointerCount = 0;
  UINTN                            ValidPointerCount = 0;
  UINTN                            EventCount;
  UINTN                            EventIndex;
  UINTN                            JoyConEventIndex = MAX_UINTN;
  UINTN                            Index;
  EFI_STATUS                       Status;

  *Activate = FALSE;
  *OpenBootManager = FALSE;
  *RequestScreenshot = FALSE;
  *OpenDiagnostics = FALSE;
  *CategoryChanged = FALSE;
  if (ItemCount == 0) {
    return EFI_INVALID_PARAMETER;
  }
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

  if (!EFI_ERROR (gBS->LocateProtocol (&gWinXJoyConInputProtocolGuid, NULL, (VOID **)&JoyCon)) &&
      JoyCon != NULL && JoyCon->WaitForInput != NULL && JoyCon->ReadState != NULL) {
    JoyConEvent = JoyCon->WaitForInput;
    JoyConEventIndex = EventCount;
    Events[EventCount++] = JoyConEvent;
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
      } else if (Key.ScanCode == SCAN_LEFT || Key.UnicodeChar == L'h' || Key.UnicodeChar == L'H') {
        *Category = (*Category == WinxBootCategoryAll) ? WinxBootCategoryOther : *Category - 1;
        *CategoryChanged = TRUE;
      } else if (Key.ScanCode == SCAN_RIGHT || Key.UnicodeChar == L'l' || Key.UnicodeChar == L'L') {
        *Category = (*Category + 1) % WinxBootCategoryMax;
        *CategoryChanged = TRUE;
      } else if (Key.ScanCode == SCAN_ESC) {
        *OpenBootManager = AllowBootManager;
      } else if (Key.ScanCode == SCAN_F12) {
        *RequestScreenshot = TRUE;
      } else if (Key.ScanCode == SCAN_F10 || Key.UnicodeChar == L'd' || Key.UnicodeChar == L'D') {
        *OpenDiagnostics = TRUE;
      } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
        *Activate = TRUE;
      }
      break;
    }

    if (EventIndex <= ValidPointerCount) {
      EFI_ABSOLUTE_POINTER_PROTOCOL  *Pointer;
      EFI_ABSOLUTE_POINTER_STATE    State;
      UINTN                         Column;
      UINTN                         Row;
      UINT64                        SpanX;
      UINT64                        SpanY;

      Pointer = Pointers[EventIndex - 1];
      if (Pointer != NULL && !EFI_ERROR (Pointer->GetState (Pointer, &State))) {
        if (mTouchActivationLatch) {
          if ((State.ActiveButtons & 1) == 0) {
            mTouchActivationLatch = FALSE;
          } else {
            continue;
          }
        }

        if ((State.ActiveButtons & 1) == 0 || Pointer->Mode == NULL ||
            Pointer->Mode->AbsoluteMaxX < Pointer->Mode->AbsoluteMinX ||
            Pointer->Mode->AbsoluteMaxY < Pointer->Mode->AbsoluteMinY ||
            State.CurrentX < Pointer->Mode->AbsoluteMinX ||
            State.CurrentX > Pointer->Mode->AbsoluteMaxX ||
            State.CurrentY < Pointer->Mode->AbsoluteMinY ||
            State.CurrentY > Pointer->Mode->AbsoluteMaxY) {
          break;
        }
        SpanX = (UINT64)Pointer->Mode->AbsoluteMaxX - Pointer->Mode->AbsoluteMinX + 1;
        SpanY = (UINT64)Pointer->Mode->AbsoluteMaxY - Pointer->Mode->AbsoluteMinY + 1;
        Column = (UINTN)(((UINT64)(State.CurrentX - Pointer->Mode->AbsoluteMinX) * Columns) / SpanX);
        Row = (UINTN)(((UINT64)(State.CurrentY - Pointer->Mode->AbsoluteMinY) * Rows) / SpanY);
        if ((Row == WINX_MENU_TAB_ROW) && (Columns != 0)) {
          UINTN  CategoryIndex;

          CategoryIndex = (UINTN)(((UINT64)Column * WinxBootCategoryMax) / Columns);
          if (CategoryIndex >= WinxBootCategoryMax) CategoryIndex = WinxBootCategoryMax - 1;
          if (*Category != (WINX_BOOT_CATEGORY)CategoryIndex) {
            *Category = (WINX_BOOT_CATEGORY)CategoryIndex;
            *CategoryChanged = TRUE;
          }
          mTouchActivationLatch = TRUE;
        } else if ((Row >= FirstItemRow) && (Row - FirstItemRow < VisibleCount)) {
          UINTN  Selection;

          Selection = VisibleStart + Row - FirstItemRow;
          if (Selection < ItemCount) {
            *Selected = Selection;
            *Activate = TRUE;
            mTouchActivationLatch = TRUE;
          }
        }
      }
      break;
    }

    if (MouseChangeEvent != NULL && EventIndex == ValidPointerCount + 1 &&
        !EFI_ERROR (MouseCursor->GetState (MouseCursor, &MouseState))) {
      BOOLEAN  WasDown;
      UINTN    Column;
      UINTN    Row;

      WasDown = mMouseLeftDown;
      mMouseLeftDown = MouseState.LeftButton;
      if (!MouseState.Visible || MouseState.ScreenHeight == 0 || MouseState.ScreenWidth == 0) {
        continue;
      }

      Row = (UINTN)(((UINT64)MouseState.Y * Rows) / MouseState.ScreenHeight);
      Column = (UINTN)(((UINT64)MouseState.X * Columns) / MouseState.ScreenWidth);
      if (MouseState.LeftButton && !WasDown && (Row == WINX_MENU_TAB_ROW) && (Columns != 0)) {
        UINTN  CategoryIndex;

        CategoryIndex = (UINTN)(((UINT64)Column * WinxBootCategoryMax) / Columns);
        if (CategoryIndex >= WinxBootCategoryMax) CategoryIndex = WinxBootCategoryMax - 1;
        if (*Category != (WINX_BOOT_CATEGORY)CategoryIndex) {
          *Category = (WINX_BOOT_CATEGORY)CategoryIndex;
          *CategoryChanged = TRUE;
        }
        break;
      }

      if ((Row >= FirstItemRow) && (Row - FirstItemRow < VisibleCount)) {
        UINTN  MouseSelection;

        MouseSelection = VisibleStart + Row - FirstItemRow;
        if (MouseSelection < ItemCount) {
          *Selected = MouseSelection;
          if (MouseState.LeftButton && !WasDown) {
            *Activate = TRUE;
            break;
          }
        }
      }
    }

    if (JoyConEvent != NULL && EventIndex == JoyConEventIndex) {
      WINX_JOYCON_STATE State;
      UINT32           Pressed;

      ZeroMem (&State, sizeof (State));
      Status = JoyCon->ReadState (JoyCon, &State);
      if (EFI_ERROR (Status)) continue;
      Pressed = State.Buttons & ~mLastJoyConButtons;
      mLastJoyConButtons = State.Buttons;
      if ((Pressed & WINX_JOYCON_BUTTON_CAPTURE) != 0) {
        *RequestScreenshot = TRUE;
      } else if ((Pressed & WINX_JOYCON_BUTTON_UP) != 0) {
        *Selected = (*Selected == 0) ? ItemCount - 1 : *Selected - 1;
      } else if ((Pressed & WINX_JOYCON_BUTTON_DOWN) != 0) {
        *Selected = (*Selected + 1) % ItemCount;
      } else if ((Pressed & WINX_JOYCON_BUTTON_LEFT) != 0) {
        *Category = (*Category == WinxBootCategoryAll) ? WinxBootCategoryOther : *Category - 1;
        *CategoryChanged = TRUE;
      } else if ((Pressed & WINX_JOYCON_BUTTON_RIGHT) != 0) {
        *Category = (*Category + 1) % WinxBootCategoryMax;
        *CategoryChanged = TRUE;
      } else if ((Pressed & WINX_JOYCON_BUTTON_A) != 0) {
        *Activate = TRUE;
      } else if ((Pressed & WINX_JOYCON_BUTTON_MINUS) != 0) {
        *OpenDiagnostics = TRUE;
      } else if ((Pressed & (WINX_JOYCON_BUTTON_B | WINX_JOYCON_BUTTON_X)) != 0) {
        *OpenBootManager = AllowBootManager;
      } else {
        continue;
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
  WINX_CUSTOM_BOOT_ENTRY        *CustomEntries;
  WINX_MENU_ITEM                *Items;
  UINTN                         *OptionMap;
  UINTN                         *CustomMap;
  UINTN                         OptionCount;
  UINTN                         CustomEntryCount;
  UINTN                         ItemCount;
  UINTN                         VisibleOptions;
  UINTN                         VisibleCustomOptions;
  UINTN                         Selected;
  UINTN                         Columns;
  UINTN                         Rows;
  UINTN                         VisibleStart;
  UINTN                         VisibleCount;
  UINTN                         Index;
  UINTN                         Item;
  UINTN                         VisibleIndex;
  UINTN                         DebugSelected;
  WINX_BOOT_CATEGORY            Category;
  WINX_BOOT_CATEGORY            DebugCategory;
  BOOLEAN                       RequestScreenshot;
  BOOLEAN                       Pinned;
  BOOLEAN                       Matches;
  BOOLEAN                       AllowBootManager;
  BOOLEAN                       Activate;
  BOOLEAN                       OpenBootManager;
  BOOLEAN                       OpenDiagnostics;
  BOOLEAN                       CategoryChanged;
  BOOLEAN                       NeedRefreshOptions;
  BOOLEAN                       NeedFullRedraw;
  EFI_STATUS                    Status;
  WINX_SWITCH_BOARD_INFO        Board;

  Options = NULL;
  CustomEntries = NULL;
  Items = NULL;
  OptionMap = NULL;
  CustomMap = NULL;
  OptionCount = 0;
  CustomEntryCount = 0;
  Status = EFI_SUCCESS;

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

  // The optional manifest is bounded and read once on menu entry. Missing or malformed
  // filesystems simply leave the firmware-registered entries available.
  if (EFI_ERROR (WinxBootEntriesLoad (&CustomEntries, &CustomEntryCount))) {
    CustomEntries = NULL;
    CustomEntryCount = 0;
  }

  Category = WinxBootCategoryAll;
  Selected = 0;
  NeedRefreshOptions = TRUE;
  NeedFullRedraw = TRUE;
  for (;;) {
    if (NeedRefreshOptions) {
      if (Options != NULL) {
        EfiBootManagerFreeLoadOptions (Options, OptionCount);
        Options = NULL;
      }
      // Read existing Boot#### entries only. Do not refresh discovery here because
      // EfiBootManagerRefreshAllBootOption may create or update NVRAM boot entries.
      OptionCount = 0;
      Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
      NeedRefreshOptions = FALSE;
    }

    OptionMap = AllocatePool (sizeof (*OptionMap) * ((OptionCount == 0) ? 1 : OptionCount));
    CustomMap = AllocatePool (sizeof (*CustomMap) * ((CustomEntryCount == 0) ? 1 : CustomEntryCount));
    if ((OptionMap == NULL) || (CustomMap == NULL)) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Done;
    }

    VisibleOptions = 0;
    for (Index = 0; Index < OptionCount; ++Index) {
      if ((Pinned && !Matches && !IsShellOption (&Options[Index])) ||
          ((Category != WinxBootCategoryAll) &&
           (ClassifyBootOption (&Options[Index]) != Category)))
      {
        continue;
      }
      OptionMap[VisibleOptions++] = Index;
    }

    VisibleCustomOptions = 0;
    if (!Pinned || Matches) {
      for (Index = 0; Index < CustomEntryCount; ++Index) {
        if ((Category == WinxBootCategoryAll) || (CustomEntries[Index].Category == Category)) {
          CustomMap[VisibleCustomOptions++] = Index;
        }
      }
    }

    ItemCount = VisibleOptions + VisibleCustomOptions + 3 +
                (AllowBootManager ? 1 : 0) + (ExtraEntryName != NULL ? 1 : 0);
    Items = AllocateZeroPool (sizeof (*Items) * ItemCount);
    if (Items == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Done;
    }

    Item = 0;
    if (ExtraEntryName != NULL) {
      Items[Item++].Type = WinxMenuExtraEntry;
    }
    for (VisibleIndex = 0; VisibleIndex < VisibleOptions; ++VisibleIndex) {
      Items[Item].Type = WinxMenuLoadOption;
      Items[Item++].OptionIndex = OptionMap[VisibleIndex];
    }
    for (VisibleIndex = 0; VisibleIndex < VisibleCustomOptions; ++VisibleIndex) {
      Items[Item].Type = WinxMenuCustomLoadOption;
      Items[Item++].OptionIndex = CustomMap[VisibleIndex];
    }
    if (AllowBootManager) {
      Items[Item++].Type = WinxMenuBootManager;
    }
    Items[Item++].Type = WinxMenuScreenshot;
    Items[Item++].Type = WinxMenuDiagnostics;
    Items[Item].Type = WinxMenuReset;

    if (Selected >= ItemCount) {
      Selected = 0;
    }

    if (NeedFullRedraw) {
      WinxVisualsInvalidateBootLogo ();
      gST->ConOut->ClearScreen (gST->ConOut);
      WinxVisualsDrawMenuBackground ();
      NeedFullRedraw = FALSE;
    }

    DrawBootMenu (Board, Options, OptionCount, CustomEntries, CustomEntryCount,
                  ExtraEntryName, Items, ItemCount, Selected, Category, Pinned, Matches,
                  &Columns, &Rows, &VisibleStart, &VisibleCount);

    Status = ReadMenuInput (ItemCount, WINX_MENU_LIST_FIRST_ROW, VisibleStart, VisibleCount,
                            Columns, Rows, &Selected, &Category, &CategoryChanged,
                            &Activate, &OpenBootManager, &RequestScreenshot,
                            &OpenDiagnostics, AllowBootManager);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    if (CategoryChanged) {
      Selected = 0;
    } else if (OpenDiagnostics ||
               (Activate && Items[Selected].Type == WinxMenuDiagnostics))
    {
      DrawDiagnostics (Board, OptionCount, CustomEntryCount, Pinned, Matches);
      GetConsoleDimensions (&Columns, &Rows);
      DebugSelected = 0;
      DebugCategory = Category;
      Status = ReadMenuInput (Rows, 0, 0, Rows, Columns, Rows, &DebugSelected,
                              &DebugCategory, &CategoryChanged, &Activate,
                              &OpenBootManager, &RequestScreenshot,
                              &OpenDiagnostics, FALSE);
      if (EFI_ERROR (Status)) {
        goto Done;
      }
      NeedFullRedraw = TRUE;
    } else if (RequestScreenshot ||
               (Activate && Items[Selected].Type == WinxMenuScreenshot))
    {
      CHAR16  SavedPath[128];

      ZeroMem (SavedPath, sizeof (SavedPath));
      Status = WinxCaptureBootMenuScreenshot (
                 SavedPath,
                 sizeof (SavedPath) / sizeof (SavedPath[0])
                 );
      mScreenshotStatus = Status;
      mScreenshotStatusValid = TRUE;
      mScreenshotStatusPath[0] = L'\0';
      if (!EFI_ERROR (Status)) {
        CopyMem (mScreenshotStatusPath, SavedPath, sizeof (mScreenshotStatusPath));
      }
      Status = EFI_SUCCESS;
    } else if (OpenBootManager ||
               (Activate && Items[Selected].Type == WinxMenuBootManager))
    {
      Status = BootManagerMenu ();
      if (EFI_ERROR (Status)) {
        mLastBootStatus = Status;
        mBootStatusValid = TRUE;
      }
      NeedRefreshOptions = TRUE;
      NeedFullRedraw = TRUE;
      Status = EFI_SUCCESS;
    } else if (Activate) {
      if (Items[Selected].Type == WinxMenuExtraEntry) {
        WinxVisualsShowBootSplash (ExtraEntryName);
        Status = ExtraEntryAction (ExtraEntryContext);
      } else if ((Items[Selected].Type == WinxMenuLoadOption) ||
                 (Items[Selected].Type == WinxMenuCustomLoadOption))
      {
        EFI_BOOT_MANAGER_LOAD_OPTION  *Option;

        Option = GetItemOption (&Items[Selected], Options, CustomEntries);
#ifdef SWITCH_WIFI_ERISTA
        if (Option->Description != NULL &&
            (ContainsAsciiNoCase (Option->Description, L"Windows") ||
             ContainsAsciiNoCase (Option->Description, L"WinARM"))) {
          PlatformWifiPrepareBeforeBoot ();
        }
#endif
        if (IsShellOption (Option)) {
          WinxVisualsPrepareShell ();
        } else {
          WinxVisualsShowBootSplash (Option->Description);
        }
        EfiBootManagerBoot (Option);
        Status = Option->Status;
      } else if (Items[Selected].Type == WinxMenuReset) {
        gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
        Status = EFI_SUCCESS;
      }

      if (Items[Selected].Type != WinxMenuReset) {
        if (EFI_ERROR (Status)) {
          mLastBootStatus = Status;
          mBootStatusValid = TRUE;
        } else {
          mBootStatusValid = FALSE;
        }
        Status = EFI_SUCCESS;
        NeedRefreshOptions = TRUE;
        NeedFullRedraw = TRUE;
      }
    }

    if (Items != NULL) {
      FreePool (Items);
      Items = NULL;
    }
    if (OptionMap != NULL) {
      FreePool (OptionMap);
      OptionMap = NULL;
    }
    if (CustomMap != NULL) {
      FreePool (CustomMap);
      CustomMap = NULL;
    }
  }

Done:
  if (Items != NULL) FreePool (Items);
  if (OptionMap != NULL) FreePool (OptionMap);
  if (CustomMap != NULL) FreePool (CustomMap);
  if (Options != NULL) EfiBootManagerFreeLoadOptions (Options, OptionCount);
  WinxBootEntriesFree (CustomEntries, CustomEntryCount);
  return Status;
}
