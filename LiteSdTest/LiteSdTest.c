// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

EFI_STATUS EFIAPI
UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  CONST CHAR8 Expected[] = "SWITCH-LITE-SD2\n";
  CHAR8 Data[sizeof (Expected)];
  UINTN Size = sizeof (Data);  // Read one extra byte to detect the wrong file.
  EFI_LOADED_IMAGE_PROTOCOL *Image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_FILE_PROTOCOL *Root, *File;
  EFI_STATUS Status;

  Print (L"EFI APP START\r\n");
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
  if (EFI_ERROR (Status)) return Status;
  Status = gBS->HandleProtocol (Image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) return Status;
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) return Status;
  Status = Root->Open (Root, &File, L"\\switchroot\\woa-lite-uefi\\SDCHECK.TXT", EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    Status = File->Read (File, &Size, Data);
    File->Close (File);
    if (!EFI_ERROR (Status) &&
        ((Size != sizeof (Expected) - 1) || CompareMem (Data, Expected, Size) != 0)) {
      Status = EFI_COMPROMISED_DATA;
    }
  }
  Root->Close (Root);
  Print (L"SDCHECK.TXT: %r\r\n", Status);
  if (EFI_ERROR (Status)) return Status;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status)) return Status;
  Print (L"GOP  %u x %u\r\n", Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution);
  Print (L"UEFI SD OK\r\n");
  return EFI_SUCCESS;
}
