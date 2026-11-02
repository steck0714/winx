/** @file
  SdAutoDxe - choose the SD host ACPI description for the Windows that is installed on the card.

  The DSDT (SWITCH_ACPI_SD_AUTO) has  Name (SDMD, 0x10)  (AML: 08 'S' 'D' 'M' 'D' 0A 10) and builds the _HID / _CID /
  _CCA of SDM1 from it. Just before the boot option starts (ReadyToBoot) this driver connects the block devices,
  looks for \Windows\System32\drivers\nvtsdhc.sys on every file system and patches SDMD in the DSDT in memory,
  then fixes the DSDT checksum (same method as hayato41200's R389 XSTA patch):
    nvtsdhc.sys found                 -> 0x02
    a Windows without nvtsdhc.sys     -> 0x01
    no Windows found / nothing read   -> unchanged (0x10): the DSDT then uses _OSI ("Windows 2021").
  Reading NTFS needs the EfiFs driver (-D SWITCH_FSDRV=1). DRAFT, UNTESTED on hardware.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/Acpi.h>
#include <Guid/EventGroup.h>
#include <IndustryStandard/Acpi.h>

#define SDMD_UNDECIDED  0x10
#define SDMD_SDBUS      0x01
#define SDMD_NVTSDHC    0x02

STATIC BOOLEAN    mDone;
STATIC EFI_EVENT  mReadyEvent;

STATIC
BOOLEAN
FileExists (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR16       *Path
  )
{
  EFI_FILE_PROTOCOL  *File;

  if (EFI_ERROR (Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0))) {
    return FALSE;
  }

  File->Close (File);
  return TRUE;
}

STATIC
VOID
Scan (
  OUT BOOLEAN  *HaveWindows,
  OUT BOOLEAN  *HaveNvtsdhc
  )
{
  EFI_HANDLE                       *Handles;
  UINTN                            Count, I;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;

  *HaveWindows = FALSE;
  *HaveNvtsdhc = FALSE;

  // Make sure every partition has its file system (the Windows NTFS volume may not be connected yet).
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles))) {
    for (I = 0; I < Count; I++) {
      gBS->ConnectController (Handles[I], NULL, NULL, TRUE);
    }

    gBS->FreePool (Handles);
  }

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  for (I = 0; I < Count; I++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
        EFI_ERROR (Fs->OpenVolume (Fs, &Root)))
    {
      continue;
    }

    if (FileExists (Root, L"\\Windows\\System32\\config\\SYSTEM")) {
      *HaveWindows = TRUE;
      if (FileExists (Root, L"\\Windows\\System32\\drivers\\nvtsdhc.sys")) {
        *HaveNvtsdhc = TRUE;
      }
    }

    Root->Close (Root);
  }

  gBS->FreePool (Handles);
}

STATIC
BOOLEAN
PatchDsdt (
  IN UINT8  Mode
  )
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp = NULL;
  EFI_ACPI_DESCRIPTION_HEADER                   *Xsdt, *Table, *Dsdt = NULL;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE     *Fadt;
  CONST UINT8                                   Pattern[8] = { 0x08, 'S', 'D', 'M', 'D', 0x0A, SDMD_UNDECIDED, 0 };
  UINT8                                         *Aml, Sum = 0;
  UINTN                                         Entries, I, J;

  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, (VOID **)&Rsdp)) || (Rsdp == NULL) ||
      (Rsdp->XsdtAddress == 0))
  {
    return FALSE;
  }

  Xsdt    = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
  Entries = (Xsdt->Length - sizeof (*Xsdt)) / sizeof (UINT64);
  for (I = 0; (I < Entries) && (Dsdt == NULL); ++I) {
    Table = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)ReadUnaligned64 ((UINT64 *)((UINT8 *)Xsdt + sizeof (*Xsdt) + I * 8));
    if (Table->Signature == EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE) {
      Fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)Table;
      Dsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)(Fadt->XDsdt != 0 ? Fadt->XDsdt : Fadt->Dsdt);
    }
  }

  if (Dsdt == NULL) {
    return FALSE;
  }

  Aml = (UINT8 *)Dsdt;
  for (I = sizeof (*Dsdt); I + 7 <= Dsdt->Length; ++I) {
    if (CompareMem (&Aml[I], Pattern, 7) == 0) {
      Aml[I + 6]      = Mode;
      Dsdt->Checksum = 0;
      for (J = 0; J < Dsdt->Length; ++J) {
        Sum = (UINT8)(Sum + Aml[J]);
      }

      Dsdt->Checksum = (UINT8)(0 - Sum);
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  BOOLEAN  HaveWindows, HaveNvtsdhc;

  if (mDone) {
    return;
  }

  mDone = TRUE;
  Scan (&HaveWindows, &HaveNvtsdhc);
  if (HaveNvtsdhc) {
    Print (L"SdAuto: nvtsdhc.sys found -> SD host described for nvtsdhc (%s)\r\n",
           PatchDsdt (SDMD_NVTSDHC) ? L"DSDT patched" : L"SDMD not found in the DSDT");
  } else if (HaveWindows) {
    Print (L"SdAuto: Windows without nvtsdhc.sys -> SD host described for sdbus (%s)\r\n",
           PatchDsdt (SDMD_SDBUS) ? L"DSDT patched" : L"SDMD not found in the DSDT");
  } else {
    Print (L"SdAuto: no Windows volume read -> DSDT decides with _OSI\r\n");
  }
}

EFI_STATUS
EFIAPI
SdAutoEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                OnReadyToBoot,
                NULL,
                &gEfiEventReadyToBootGuid,
                &mReadyEvent
                );
}
