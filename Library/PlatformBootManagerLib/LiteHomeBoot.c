// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PlatformBm.h"
#include <Library/UefiBootManagerLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <winarm-target.h>

#define WINDOWS_LOADER L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"

STATIC BOOLEAN
IsWindowsPartition (EFI_HANDLE Handle)
{
  EFI_DEVICE_PATH_PROTOCOL *Node = DevicePathFromHandle (Handle);
  if (Node == NULL) return FALSE;
  for (; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if (DevicePathType (Node) == MEDIA_DEVICE_PATH &&
        DevicePathSubType (Node) == MEDIA_HARDDRIVE_DP &&
        DevicePathNodeLength (Node) == sizeof (HARDDRIVE_DEVICE_PATH) &&
        IsDevicePathEnd (NextDevicePathNode (Node))) {
      HARDDRIVE_DEVICE_PATH Hd;
      CopyMem (&Hd, Node, sizeof (Hd));
      return Hd.MBRType == MBR_TYPE_PCAT && Hd.SignatureType == SIGNATURE_TYPE_MBR &&
             Hd.PartitionNumber == 1 && Hd.PartitionStart == WINARM_FIRST &&
             Hd.PartitionSize == WINARM_COUNT && LiteU32 (Hd.Signature) == WINARM_MBR_SIGNATURE;
    }
  }
  return FALSE;
}

STATIC EFI_STATUS
CheckBootFile (EFI_FILE_PROTOCOL *Root, CHAR16 *Name, CONST CHAR8 *Magic, UINTN Length)
{
  EFI_FILE_PROTOCOL *File;
  CHAR8 Header[4];
  UINTN Size = Length;
  EFI_STATUS Status = Root->Open (Root, &File, Name, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    Status = File->Read (File, &Size, Header);
    if (!EFI_ERROR (Status) && (Size != Length || CompareMem (Header, Magic, Length) != 0))
      Status = EFI_COMPROMISED_DATA;
    File->Close (File);
  }
  if (EFI_ERROR (Status)) Print (L"%s: %r\r\n", Name, Status);
  return Status;
}

STATIC EFI_STATUS
BootWindows (EFI_HANDLE Volume, EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs)
{
  EFI_BOOT_MANAGER_LOAD_OPTION Option;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_FILE_PROTOCOL *Root;
  EFI_STATUS Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) return Status;
  Status = CheckBootFile (Root, WINDOWS_LOADER, "MZ", 2);
  if (!EFI_ERROR (Status)) Status = CheckBootFile (Root, L"\\EFI\\Microsoft\\Boot\\BCD", "regf", 4);
  if (!EFI_ERROR (Status)) Status = CheckBootFile (Root, L"\\Windows\\System32\\winload.efi", "MZ", 2);
  Root->Close (Root);
  if (EFI_ERROR (Status)) return Status;

  Path = FileDevicePath (Volume, WINDOWS_LOADER);
  if (Path == NULL) return EFI_OUT_OF_RESOURCES;
  Status = EfiBootManagerInitializeLoadOption (&Option, LoadOptionNumberUnassigned,
      LoadOptionTypeBoot, LOAD_OPTION_ACTIVE, L"Lite Windows 11 Home", Path, NULL, 0);
  FreePool (Path);
  if (EFI_ERROR (Status)) return Status;
  Print (L"Starting Windows Boot Manager from WINARM\r\n");
  EfiBootManagerBoot (&Option);
  Status = Option.Status;
  EfiBootManagerFreeLoadOption (&Option);
  return Status;
}

VOID EFIAPI
PlatformBootManagerAfterConsole (VOID)
{
  EFI_HANDLE *Volumes;
  UINTN Count;
  EFI_STATUS Status;
  BOOLEAN Found = FALSE;

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"SWITCH LITE / WINDOWS 11 HOME\r\n");
  EfiBootManagerConnectAll ();
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Volumes);
  if (!EFI_ERROR (Status)) {
    for (UINTN Index = 0; Index < Count; ++Index) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
      if (!IsWindowsPartition (Volumes[Index])) continue;
      Found = TRUE;
      Print (L"WINARM 80 GiB / NTFS\r\n");
      Status = gBS->HandleProtocol (Volumes[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
      if (!EFI_ERROR (Status)) Status = BootWindows (Volumes[Index], Fs);
      Print (L"Windows launch returned: %r\r\n", Status);
      break;
    }
    FreePool (Volumes);
  }
  if (!Found) Print (L"WINARM filesystem missing (%r)\r\n", Status);
  Print (L"Hold POWER 12s to turn off.\r\n");
  for (;;) __asm__ volatile ("wfi");
}
