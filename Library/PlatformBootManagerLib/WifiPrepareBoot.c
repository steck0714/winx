// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Erista (T210) Wi-Fi hand-off for an explicitly selected Windows path.
 * This function is intentionally not called from generic BDS startup.
 * Compiles only when the library is selected; physical execution is untested.
 * A profile build does not verify the external EFI app, PMIC hand-off or DSDT update.
 *
 * Same job as PrepareWindowsDevice() in LiteHomeBoot.c, but for the normal
 * (non-Lite) boot chain: run \SwitchWOA\Wifi\WifiPrepare.efi from whichever
 * volume holds it, and only if it succeeds flip the WFI0 readiness byte
 * (WSTA 0x10 -> 0x0F) in the installed DSDT. On any failure WFI0 stays hidden
 * and the normal boot continues.
 */
#include "PlatformBm.h"
#ifdef SWITCH_WIFI_ERISTA
#include <Guid/Acpi.h>
#include <IndustryStandard/Acpi.h>
#include <Protocol/LoadedImage.h>

#define WIFI_PREPARE_FILE_GUID \
  { 0x2700B6BD, 0x46A1, 0x49B5, { 0x8D, 0xDA, 0x13, 0x7E, 0x44, 0x77, 0xDE, 0x11 } }

STATIC EFI_GUID mWifiPrepareFileGuid = WIFI_PREPARE_FILE_GUID;

STATIC EFI_DEVICE_PATH_PROTOCOL *
WifiPrepareFilePath (VOID)
{
  EFI_STATUS                         Status;
  EFI_LOADED_IMAGE_PROTOCOL          *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL           *BasePath;
  EFI_DEVICE_PATH_PROTOCOL           *FilePath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;

  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  if (EFI_ERROR (Status) || LoadedImage == NULL ||
      (LoadedImage->DeviceHandle == NULL)) {
    return NULL;
  }

  BasePath = DevicePathFromHandle (LoadedImage->DeviceHandle);
  if (BasePath == NULL) {
    return NULL;
  }
  EfiInitializeFwVolDevicepathNode (&FileNode, &mWifiPrepareFileGuid);
  FilePath = AppendDevicePathNode (BasePath, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
  FreePool (BasePath);
  return FilePath;
}

VOID
PlatformWifiPrepareBeforeBoot (VOID)
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;
  EFI_ACPI_DESCRIPTION_HEADER                   *Xsdt, *Table, *Dsdt = NULL;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE     *Fadt;
  // AML: NameOp 'WSTA' BytePrefix 0x10 (the "hidden" marker in WFI0).
  UINT8                                         Pattern[] = {0x08, 'W', 'S', 'T', 'A', 0x0A, 0x10};
  UINT8                                         *Ready = NULL, *Aml, Sum = 0;
  UINTN                                         Entries, I, Matches = 0;
  EFI_DEVICE_PATH_PROTOCOL                      *Path;
  EFI_HANDLE                                    Image;
  EFI_STATUS                                    Status;

  Status = EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, (VOID **)&Rsdp);
  if (EFI_ERROR (Status) || Rsdp == NULL || Rsdp->XsdtAddress == 0) goto Hidden;
  Xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
  if (Xsdt->Length < sizeof (*Xsdt)) goto Hidden;
  Entries = (Xsdt->Length - sizeof (*Xsdt)) / sizeof (UINT64);
  for (I = 0; I < Entries && Dsdt == NULL; ++I) {
    Table = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)ReadUnaligned64 (
              (UINT64 *)((UINT8 *)Xsdt + sizeof (*Xsdt) + I * 8));
    if (Table->Signature != EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE) continue;
    if (Table->Length < sizeof (*Fadt)) goto Hidden;
    Fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)Table;
    Dsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)(Fadt->XDsdt ? Fadt->XDsdt : Fadt->Dsdt);
  }
  if (Dsdt == NULL || Dsdt->Length < sizeof (*Dsdt)) goto Hidden;
  Aml = (UINT8 *)Dsdt;
  for (I = sizeof (*Dsdt); I + sizeof (Pattern) <= Dsdt->Length; ++I) {
    if (CompareMem (Aml + I, Pattern, sizeof (Pattern)) == 0) {
      Ready = Aml + I + 6;
      Matches++;
    }
  }
  if (Matches != 1) goto Hidden;   // DSDT built without SWITCH_WIFI_RUNTIME

  Path = WifiPrepareFilePath ();
  if (Path == NULL) goto Hidden;
  Status = gBS->LoadImage (FALSE, gImageHandle, Path, NULL, 0, &Image);
  FreePool (Path);
  if (EFI_ERROR (Status)) goto Hidden;
  Status = gBS->StartImage (Image, NULL, NULL);
  gBS->UnloadImage (Image);
  if (EFI_ERROR (Status)) {
    Print (L"Wi-Fi preparation: %r; device hidden, continuing boot.\r\n", Status);
    return;
  }

  *Ready = 0x0F;
  Dsdt->Checksum = 0;
  for (I = 0; I < Dsdt->Length; ++I) Sum = (UINT8)(Sum + Aml[I]);
  Dsdt->Checksum = (UINT8)(0 - Sum);
  MemoryFence ();
  Print (L"Wi-Fi preparation done; WFI0 exposed.\r\n");
  return;

Hidden:
  Print (L"Wi-Fi preparation skipped (ACPI/DSDT not ready); device hidden.\r\n");
}
#endif /* SWITCH_WIFI_ERISTA */
