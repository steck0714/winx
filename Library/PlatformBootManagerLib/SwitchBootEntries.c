// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>

#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Protocol/SimpleFileSystem.h>

#include "SwitchBootEntries.h"

#define WINX_ENTRY_MANIFEST       L"\\bootloader\\res\\winx\\entries.ini"
#define WINX_ENTRY_MAX_COUNT      64
#define WINX_ENTRY_MAX_FILE_SIZE  (64U * 1024U)
#define WINX_ENTRY_LINE_CHARS     512

STATIC
CHAR16
AsciiLower (
  IN CHAR16  Character
  )
{
  if ((Character >= L'A') && (Character <= L'Z')) {
    return (CHAR16)(Character + (L'a' - L'A'));
  }

  return Character;
}

STATIC
BOOLEAN
AsciiEqualsNoCase (
  IN CONST CHAR16  *Left,
  IN CONST CHAR16  *Right
  )
{
  if ((Left == NULL) || (Right == NULL)) {
    return FALSE;
  }

  while ((*Left != L'\0') && (*Right != L'\0')) {
    if (AsciiLower (*Left) != AsciiLower (*Right)) {
      return FALSE;
    }

    ++Left;
    ++Right;
  }

  return (BOOLEAN)(*Left == *Right);
}

STATIC
VOID
TrimField (
  IN OUT CHAR16  *Field
  )
{
  UINTN  Length;
  UINTN  Start;

  if (Field == NULL) {
    return;
  }

  Length = StrLen (Field);
  while ((Length > 0) && ((Field[Length - 1] == L' ') || (Field[Length - 1] == L'\t'))) {
    Field[--Length] = L'\0';
  }

  Start = 0;
  while ((Field[Start] == L' ') || (Field[Start] == L'\t')) {
    ++Start;
  }

  if (Start != 0) {
    CopyMem (Field, Field + Start, (Length - Start + 1) * sizeof (*Field));
  }
}

STATIC
BOOLEAN
IsEfiPath (
  IN CONST CHAR16  *Path
  )
{
  UINTN  Length;
  UINTN  Index;
  UINTN  ComponentStart;
  UINTN  ComponentLength;

  if ((Path == NULL) || (Path[0] != L'\\')) {
    return FALSE;
  }

  Length = StrLen (Path);
  if ((Length < 5) || (Length >= 260)) {
    return FALSE;
  }

  ComponentStart = 1;
  for (Index = 1; Index <= Length; ++Index) {
    if ((Path[Index] == L'/') || (Path[Index] == L':') || (Path[Index] == L'|')) {
      return FALSE;
    }

    if ((Path[Index] == L'\\') || (Path[Index] == L'\0')) {
      ComponentLength = Index - ComponentStart;
      if ((ComponentLength == 0) ||
          ((ComponentLength == 1) && (Path[ComponentStart] == L'.')) ||
          ((ComponentLength == 2) && (Path[ComponentStart] == L'.') && (Path[ComponentStart + 1] == L'.')))
      {
        return FALSE;
      }
      ComponentStart = Index + 1;
    }
  }

  return AsciiEqualsNoCase ((CONST CHAR16 *)(Path + Length - 4), L".efi");
}

STATIC
BOOLEAN
ParseCategory (
  IN  CONST CHAR16       *Name,
  OUT WINX_BOOT_CATEGORY *Category
  )
{
  if (Category == NULL) {
    return FALSE;
  }

  if (AsciiEqualsNoCase (Name, L"windows")) {
    *Category = WinxBootCategoryWindows;
    return TRUE;
  }

  if (AsciiEqualsNoCase (Name, L"linux")) {
    *Category = WinxBootCategoryLinux;
    return TRUE;
  }

  if (AsciiEqualsNoCase (Name, L"android")) {
    *Category = WinxBootCategoryAndroid;
    return TRUE;
  }

  if (AsciiEqualsNoCase (Name, L"other")) {
    *Category = WinxBootCategoryOther;
    return TRUE;
  }

  return FALSE;
}

STATIC
VOID
TryAddManifestLine (
  IN EFI_HANDLE                  Volume,
  IN EFI_FILE_PROTOCOL           *Root,
  IN OUT WINX_CUSTOM_BOOT_ENTRY  *Entries,
  IN OUT UINTN                   *EntryCount,
  IN OUT CHAR16                  *Line
  )
{
  CHAR16                    *Fields[3];
  UINTN                     FieldCount;
  UINTN                     Index;
  EFI_FILE_PROTOCOL         *Loader;
  EFI_FILE_INFO             *LoaderInfo;
  UINTN                     LoaderInfoSize;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  WINX_BOOT_CATEGORY        Category;
  EFI_STATUS                Status;

  TrimField (Line);
  if ((Line[0] == L'\0') || (Line[0] == L'#') || (*EntryCount >= WINX_ENTRY_MAX_COUNT)) {
    return;
  }

  FieldCount = 1;
  Fields[0] = Line;
  for (Index = 0; (Line[Index] != L'\0') && (FieldCount < ARRAY_SIZE (Fields)); ++Index) {
    if (Line[Index] == L'|') {
      Line[Index] = L'\0';
      Fields[FieldCount++] = &Line[Index + 1];
    }
  }

  if (FieldCount != ARRAY_SIZE (Fields)) {
    return;
  }

  for (Index = 0; Index < ARRAY_SIZE (Fields); ++Index) {
    TrimField (Fields[Index]);
  }

  if ((Fields[0][0] == L'\0') || (Fields[1][0] == L'\0') ||
      !ParseCategory (Fields[1], &Category) || !IsEfiPath (Fields[2]))
  {
    return;
  }

  Loader = NULL;
  Status = Root->Open (Root, &Loader, Fields[2], EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || (Loader == NULL)) {
    return;
  }

  LoaderInfo = NULL;
  LoaderInfoSize = 0;
  Status = Loader->GetInfo (Loader, &gEfiFileInfoGuid, &LoaderInfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL && LoaderInfoSize != 0) {
    LoaderInfo = AllocatePool (LoaderInfoSize);
    if (LoaderInfo != NULL) {
      Status = Loader->GetInfo (Loader, &gEfiFileInfoGuid, &LoaderInfoSize, LoaderInfo);
    }
  }
  if (EFI_ERROR (Status) || (LoaderInfo == NULL) ||
      ((LoaderInfo->Attribute & EFI_FILE_DIRECTORY) != 0))
  {
    if (LoaderInfo != NULL) {
      FreePool (LoaderInfo);
    }
    Loader->Close (Loader);
    return;
  }
  FreePool (LoaderInfo);
  Loader->Close (Loader);
  DevicePath = FileDevicePath (Volume, Fields[2]);
  if (DevicePath == NULL) {
    return;
  }

  Status = EfiBootManagerInitializeLoadOption (
             &Entries[*EntryCount].Option,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE,
             Fields[0],
             DevicePath,
             NULL,
             0
             );
  FreePool (DevicePath);
  if (EFI_ERROR (Status)) {
    EfiBootManagerFreeLoadOption (&Entries[*EntryCount].Option);
    return;
  }

  Entries[*EntryCount].Category = Category;
  ++*EntryCount;
}

STATIC
VOID
ReadManifestFromVolume (
  IN EFI_HANDLE                       Volume,
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem,
  IN OUT WINX_CUSTOM_BOOT_ENTRY       *Entries,
  IN OUT UINTN                        *EntryCount
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *Manifest;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINTN              ReadSize;
  UINT8              *Buffer;
  UINTN              Offset;
  UINTN              LineLength;
  BOOLEAN            TooLong;
  CHAR16             Line[WINX_ENTRY_LINE_CHARS];

  Root = NULL;
  Manifest = NULL;
  Info = NULL;
  Buffer = NULL;
  Status = FileSystem->OpenVolume (FileSystem, &Root);
  if (EFI_ERROR (Status) || (Root == NULL)) {
    if (Root != NULL) {
      Root->Close (Root);
    }
    return;
  }

  Status = Root->Open (Root, &Manifest, WINX_ENTRY_MANIFEST, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || (Manifest == NULL)) {
    goto Done;
  }

  InfoSize = 0;
  Status = Manifest->GetInfo (Manifest, &gEfiFileInfoGuid, &InfoSize, NULL);
  if ((Status != EFI_BUFFER_TOO_SMALL) || (InfoSize == 0)) {
    goto Done;
  }

  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    goto Done;
  }

  Status = Manifest->GetInfo (Manifest, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status) || ((Info->Attribute & EFI_FILE_DIRECTORY) != 0) ||
      (Info->FileSize == 0) || (Info->FileSize > WINX_ENTRY_MAX_FILE_SIZE))
  {
    goto Done;
  }

  Buffer = AllocatePool ((UINTN)Info->FileSize);
  if (Buffer == NULL) {
    goto Done;
  }

  ReadSize = (UINTN)Info->FileSize;
  Status = Manifest->Read (Manifest, &ReadSize, Buffer);
  if (EFI_ERROR (Status) || (ReadSize != (UINTN)Info->FileSize)) {
    goto Done;
  }

  Offset = 0;
  // Accept an optional UTF-8 BOM, then parse the documented ASCII format.
  if ((ReadSize >= 3) && (Buffer[0] == 0xEF) && (Buffer[1] == 0xBB) && (Buffer[2] == 0xBF)) {
    Offset = 3;
  }

  LineLength = 0;
  TooLong = FALSE;
  for (; Offset <= ReadSize; ++Offset) {
    UINT8  Character;

    Character = (Offset == ReadSize) ? '\n' : Buffer[Offset];
    if ((Character == '\r') && (Offset < ReadSize)) {
      continue;
    }

    if (Character == '\n') {
      if (!TooLong) {
        Line[LineLength] = L'\0';
        TryAddManifestLine (Volume, Root, Entries, EntryCount, Line);
      }
      LineLength = 0;
      TooLong = FALSE;
      if (*EntryCount >= WINX_ENTRY_MAX_COUNT) {
        break;
      }
      continue;
    }

    if ((Character < 0x20) || (Character > 0x7E)) {
      TooLong = TRUE;
      continue;
    }

    if (LineLength + 1 < ARRAY_SIZE (Line)) {
      Line[LineLength++] = (CHAR16)Character;
    } else {
      TooLong = TRUE;
    }
  }

Done:
  if (Buffer != NULL) {
    FreePool (Buffer);
  }
  if (Info != NULL) {
    FreePool (Info);
  }
  if (Manifest != NULL) {
    Manifest->Close (Manifest);
  }
  Root->Close (Root);
}

EFI_STATUS
WinxBootEntriesLoad (
  OUT WINX_CUSTOM_BOOT_ENTRY  **Entries,
  OUT UINTN                   *EntryCount
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  WINX_CUSTOM_BOOT_ENTRY           *LoadedEntries;
  UINTN                            LoadedCount;

  if ((Entries == NULL) || (EntryCount == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Entries = NULL;
  *EntryCount = 0;
  Handles = NULL;
  HandleCount = 0;
  LoadedEntries = AllocateZeroPool (sizeof (*LoadedEntries) * WINX_ENTRY_MAX_COUNT);
  if (LoadedEntries == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->LocateHandleBuffer (
                 ByProtocol,
                 &gEfiSimpleFileSystemProtocolGuid,
                 NULL,
                 &HandleCount,
                 &Handles
                 );
  if (EFI_ERROR (Status)) {
    if (Handles != NULL) {
      FreePool (Handles);
    }
    FreePool (LoadedEntries);
    return Status;
  }

  LoadedCount = 0;
  for (Index = 0; Index < HandleCount; ++Index) {
    FileSystem = NULL;
    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid,
                                         (VOID **)&FileSystem)) && (FileSystem != NULL))
    {
      ReadManifestFromVolume (Handles[Index], FileSystem, LoadedEntries, &LoadedCount);
    }

    if (LoadedCount >= WINX_ENTRY_MAX_COUNT) {
      break;
    }
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  if (LoadedCount == 0) {
    FreePool (LoadedEntries);
    return EFI_NOT_FOUND;
  }

  *Entries = LoadedEntries;
  *EntryCount = LoadedCount;
  return EFI_SUCCESS;
}

VOID
WinxBootEntriesFree (
  IN WINX_CUSTOM_BOOT_ENTRY  *Entries,
  IN UINTN                   EntryCount
  )
{
  UINTN  Index;

  if (Entries == NULL) {
    return;
  }

  for (Index = 0; Index < EntryCount; ++Index) {
    EfiBootManagerFreeLoadOption (&Entries[Index].Option);
  }

  FreePool (Entries);
}
