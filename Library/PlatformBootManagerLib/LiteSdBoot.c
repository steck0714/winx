// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PlatformBm.h"
#include "WinPeGptView.h"
#include <Guid/EventGroup.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Protocol/SimpleFileSystem.h>

#define LOADER_PATH L"\\EFI\\LiteNV\\bootmgfw.efi"
#define MARKER_PATH L"\\switchroot\\woa-lite-winpe\\MEDIA.ID"

STATIC VOID EFIAPI
MarkExitBootServices (EFI_EVENT Event, VOID *Context)
{
  // Hekate's live 720 x 1280 framebuffer. The first 16 scanlines are the
  // right edge in handheld orientation. Only RAM and CPU cache operations here.
  volatile UINT32 *Fb = (volatile UINT32 *)(UINTN)0xf5a00000;
  for (UINTN I = 0; I < 16 * 720; ++I) {
    Fb[I] = 0xffffaa00;
  }
  WriteBackDataCacheRange ((VOID *)(UINTN)Fb, 16 * 720 * sizeof (*Fb));
}

STATIC EFI_STATUS
CheckMarker (EFI_FILE_PROTOCOL *Root)
{
  STATIC CONST CHAR8 Expected[] = "SWITCH-LITE-WINPE-22621-R1\n";
  CHAR8 Data[sizeof (Expected)];
  UINTN Size = sizeof (Data);
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status = Root->Open (Root, &File, MARKER_PATH, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) return Status;
  Status = File->Read (File, &Size, Data);
  File->Close (File);
  if (EFI_ERROR (Status)) return Status;
  if (Size != sizeof (Expected) - 1 || CompareMem (Data, Expected, Size) != 0)
    return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
CheckFile (EFI_FILE_PROTOCOL *Root, CHAR16 *Name, CONST CHAR8 *Magic, UINTN MagicSize, BOOLEAN CheckTail)
{
  EFI_FILE_PROTOCOL *File;
  UINT8 Buffer[512];
  UINTN Size = MagicSize;
  UINT64 Length = 0;
  EFI_STATUS Status = Root->Open (Root, &File, Name, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    Status = File->Read (File, &Size, Buffer);
    if (!EFI_ERROR (Status) && (Size != MagicSize || CompareMem (Buffer, Magic, MagicSize) != 0))
      Status = EFI_COMPROMISED_DATA;
    if (!EFI_ERROR (Status)) Status = File->SetPosition (File, MAX_UINT64);
    if (!EFI_ERROR (Status)) Status = File->GetPosition (File, &Length);
    if (!EFI_ERROR (Status) && CheckTail) {
      if (Length < sizeof (Buffer)) {
        Status = EFI_BAD_BUFFER_SIZE;
      } else {
        Status = File->SetPosition (File, Length - sizeof (Buffer));
        if (!EFI_ERROR (Status)) {
          Size = sizeof (Buffer);
          Status = File->Read (File, &Size, Buffer);
          if (!EFI_ERROR (Status) && Size != sizeof (Buffer)) Status = EFI_END_OF_FILE;
        }
      }
    }
    File->Close (File);
  }
  Print (L"%s: %r (%Lu bytes)\r\n", Name, Status, Length);
  return Status;
}

STATIC EFI_STATUS
BootPe (EFI_HANDLE Volume, EFI_FILE_PROTOCOL *Root)
{
  EFI_STATUS Status;
  EFI_EVENT ExitEvent;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_BOOT_MANAGER_LOAD_OPTION Option;

  Status = CheckFile (Root, L"\\EFI\\LiteNV\\BCD", "regf", 4, FALSE);
  if (!EFI_ERROR (Status)) Status = CheckFile (Root, L"\\boot\\boot.sdi", "$SDI0001", 8, FALSE);
  if (!EFI_ERROR (Status)) Status = CheckFile (Root, L"\\sources\\nvsd.wim", "MSWIM\0\0\0", 8, TRUE);
  if (!EFI_ERROR (Status)) Status = CheckFile (Root, LOADER_PATH, "MZ", 2, FALSE);
  Root->Close (Root);
  if (EFI_ERROR (Status)) return Status;

  Path = FileDevicePath (Volume, LOADER_PATH);
  if (Path == NULL) return EFI_OUT_OF_RESOURCES;
  Status = EfiBootManagerInitializeLoadOption (&Option, LoadOptionNumberUnassigned,
             LoadOptionTypeBoot, LOAD_OPTION_ACTIVE, L"Lite Tegra SD test", Path, NULL, 0);
  FreePool (Path);
  if (EFI_ERROR (Status)) return Status;

  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, MarkExitBootServices,
                               NULL, &gEfiEventExitBootServicesGuid, &ExitEvent);
  if (!EFI_ERROR (Status)) {
    Status = BeginWinPeGptView (Volume);
    if (!EFI_ERROR (Status)) {
      Print (L"STARTING WINDOWS BOOT MANAGER\r\n");
      gBS->Stall (1200000);
      EfiBootManagerBoot (&Option);
      Status = Option.Status;
      EndWinPeGptView ();
      Print (L"Boot Manager returned: %r\r\n", Status);
    } else {
      Print (L"Preparing Windows SD view: %r\r\n", Status);
    }
    gBS->CloseEvent (ExitEvent);
  }
  EfiBootManagerFreeLoadOption (&Option);
  return Status;
}

VOID EFIAPI
PlatformBootManagerAfterConsole (VOID)
{
  EFI_HANDLE *Volumes = NULL;
  UINTN Count = 0;
  BOOLEAN Found = FALSE;
  EFI_STATUS Status;

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"LITE / WINDOWS PE / TEGRA SD\r\n");
  EfiBootManagerConnectAll ();
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                    NULL, &Count, &Volumes);
  Print (L"File systems: %u (%r)\r\n", (UINT32)Count, Status);
  if (!EFI_ERROR (Status)) {
    for (UINTN Index = 0; Index < Count; ++Index) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
      EFI_FILE_PROTOCOL *Root;
      Status = gBS->HandleProtocol (Volumes[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
      if (EFI_ERROR (Status)) continue;
      Status = Fs->OpenVolume (Fs, &Root);
      if (EFI_ERROR (Status)) continue;
      Status = CheckMarker (Root);
      if (EFI_ERROR (Status)) {
        Root->Close (Root);
        if (Status != EFI_NOT_FOUND) Print (L"FS %u MEDIA.ID: %r\r\n", (UINT32)Index, Status);
        continue;
      }
      Found = TRUE;
      Print (L"Using SD volume %u\r\n", (UINT32)Index);
      Status = BootPe (Volumes[Index], Root);
      Print (L"PE launch ended: %r\r\n", Status);
      break;
    }
    FreePool (Volumes);
  }
  if (!Found) Print (L"WinPE media marker not found.\r\n");
  Print (L"Hold POWER 12s to turn off.\r\n");
  for (;;) { __asm__ volatile ("wfi"); }
}
