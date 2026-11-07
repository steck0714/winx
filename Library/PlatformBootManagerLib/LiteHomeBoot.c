// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PlatformBm.h"
#ifdef SWITCH_LITE_BOOT_SD
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/I2cLib.h>
#include <Library/TimerLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <LiteSdTarget.h>
#ifdef SWITCH_WIFI_RUNTIME
#include <Guid/Acpi.h>
#include <IndustryStandard/Acpi.h>
#endif

#define WINDOWS_LOADER L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"
STATIC BOOLEAN IsWindowsPartition (EFI_HANDLE Handle);

#if defined(SWITCH_LITE_TOUCH) && defined(SWITCH_LITE_PERFORMANCE)
#include "../../Drivers/WifiBtApp/t214/power.h"
/* The Hekate BL33 environment is reserved by the performance profile.
 * Configuration avoids VOL- being consumed by Hekate before BL33 starts. */
STATIC UINT32
BootCheckMode (CONST CHAR8 *Key)
{
  CONST CHAR8 *Text = (CONST CHAR8 *)0xA9FC0000UL;
  UINTN KeyLength = AsciiStrLen (Key);
  UINTN Pos = 0;
  if (MmioRead32 (0xA9FBFFFCUL) != 0x33334C42U) return 0;
  while (Pos < 0x40000U && Text[Pos] != 0) {
    UINTN End = Pos;
    while (End < 0x40000U && Text[End] != 0 && Text[End] != '\n') End++;
    if (End - Pos == KeyLength + 1 && CompareMem (Text + Pos, Key, KeyLength) == 0) {
      CHAR8 Value = Text[End - 1];
      return Value == '1' || Value == '2' ? (UINT32)(Value - '0') : 0;
    }
    Pos = End + 1;
  }
  return 0;
}

STATIC VOID
CheckPowerBeforeWindows (VOID)
{
  UINT32 Mode = BootCheckMode ("power_check=");
  UINTN I;
  EFI_RESET_TYPE Action;
  if (!Mode || (MmioRead32 (0x70000804) & 0xFFF0) != 0x2120) return;
  Action = Mode == 1 ? EfiResetCold : EfiResetShutdown;
  Print (L"FIRMWARE %s CHECK / CONFIG %u\r\n",
         Action == EfiResetCold ? L"RESTART" : L"POWER-OFF", Mode);
  Print (L"Windows has not started. Do not hold volume keys.\r\n");
  Print (L"Calling the firmware power path in 5 seconds.\r\n");
  for (I = 5; I > 0; I--) { Print (L"%u ", I); gBS->Stall (1000000); }
  Print (L"\r\nResetSystem called now.\r\n");
  gRT->ResetSystem (Action, EFI_SUCCESS, 0, NULL);
  Print (L"FAILED: ResetSystem returned.\r\n");
  for (;;) __asm__ volatile ("wfi");
}

STATIC VOID
ReplayWifiOutput (VOID);

STATIC EFI_TEXT_STRING mWifiOutput;
STATIC CHAR16 mWifiTrace[8192];
STATIC UINTN mWifiTraceLength;

STATIC EFI_STATUS EFIAPI
CaptureWifiOutput (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Text)
{
  UINTN Length = StrLen (Text);
  UINTN Capacity = ARRAY_SIZE (mWifiTrace) - 1;
  if (Length >= Capacity) {
    CopyMem (mWifiTrace, Text + Length - Capacity, Capacity * sizeof (CHAR16));
    mWifiTraceLength = Capacity;
  } else {
    if (mWifiTraceLength + Length > Capacity) {
      UINTN Drop = mWifiTraceLength + Length - Capacity;
      CopyMem (mWifiTrace, mWifiTrace + Drop, (mWifiTraceLength - Drop) * sizeof (CHAR16));
      mWifiTraceLength -= Drop;
    }
    CopyMem (mWifiTrace + mWifiTraceLength, Text, Length * sizeof (CHAR16));
    mWifiTraceLength += Length;
  }
  mWifiTrace[mWifiTraceLength] = 0;
  return mWifiOutput (This, Text);
}

STATIC VOID
ReplayWifiOutput (VOID)
{
  if (mWifiTraceLength == 0) {
    Print (L"Probe returned without console output.\r\n");
    return;
  }
  // Child-image exit can restore/clear the console. Reprint its last lines
  // from the parent so the actual failing stage remains visible afterwards.
  UINTN Start = mWifiTraceLength;
  UINTN Lines = 0;
  while (Start > 0) {
    --Start;
    if (mWifiTrace[Start] == '\n' && ++Lines == 12) { ++Start; break; }
  }
  Print (L"Wi-Fi probe output (last lines):\r\n");
  // Print() formats at most PcdUefiLibMaxPrintBufferSize characters (320 in
  // Home). Send the already-formatted trace directly so its end is retained.
  gST->ConOut->OutputString (gST->ConOut, mWifiTrace + Start);
  gST->ConOut->OutputString (gST->ConOut, L"\r\n");
}

STATIC VOID
CheckWifiBeforeWindows (VOID)
{
  STATIC CHAR16 ProbePath[] = L"\\SwitchWOA\\WifiProbe\\WifiProbe.efi";
  EFI_HANDLE *Volumes = NULL, Image;
  UINTN Count, I;
  EFI_STATUS Status;
  if (BootCheckMode ("wifi_check=") != 1) return;
  Print (L"WIFI CHECK / CONFIG 1 / LOADER R5\r\nWindows will not start during this check.\r\n");
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Volumes);
  if (!EFI_ERROR (Status)) {
    Status = EFI_NOT_FOUND;
    for (I = 0; I < Count; I++) {
      EFI_DEVICE_PATH_PROTOCOL *Path;
      if (!IsWindowsPartition (Volumes[I])) continue;
      Print (L"Loading Wi-Fi probe from WINARM\r\n");
      Path = FileDevicePath (Volumes[I], ProbePath);
      if (Path == NULL) { Status = EFI_OUT_OF_RESOURCES; break; }
      Status = gBS->LoadImage (FALSE, gImageHandle, Path, NULL, 0, &Image);
      FreePool (Path);
      if (EFI_ERROR (Status)) {
        Print (L"LoadImage %s: %r\r\n", ProbePath, Status);
        break;
      }
      EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Console = gST->ConOut;
      T214_PCIE_SUPPLY Supply;
      ZeroMem (&Supply, sizeof (Supply));
      i2c_init (I2C_5);
      if ((MmioRead32 (0x70000804) & 0xFFF0) != 0x2120 || !T214PcieSupplyEnable (&Supply)) {
        Print (L"PCIe supply preparation failed.\r\n");
        T214PcieSupplyRestore (&Supply);
        gBS->UnloadImage (Image);
        Status = EFI_DEVICE_ERROR;
        break;
      }
      mWifiTraceLength = 0;
      mWifiTrace[0] = 0;
      mWifiOutput = Console->OutputString;
      Console->OutputString = CaptureWifiOutput;
      Status = gBS->StartImage (Image, NULL, NULL);
      Console->OutputString = mWifiOutput;
      gBS->UnloadImage (Image);
      ReplayWifiOutput ();
      Print (L"PCIe supply: before=%02x enabled=%02x POK=%02x\r\n",
             Supply.Config, Supply.EnabledConfig, Supply.PowerStatus);
      if (Supply.Changed) {
        // Also cover an early PHY/link failure before the child marks PEX up.
        MmioWrite32 (0x60006310, BIT10); // PCIEX reset, CAR RST_DEV_U_SET
        gBS->Stall (20);
      }
      if (!T214PcieSupplyRestore (&Supply)) Print (L"PCIe supply restore failed.\r\n");
      break;
    }
    FreePool (Volumes);
  }
  Print (L"Wi-Fi check returned: %r\r\n", Status);
  Print (L"Photograph the last stage/result. The boot disk is read-only.\r\n");
  Print (L"Powering off in 30 seconds.\r\n");
  for (I = 30; I > 0; I--) gBS->Stall (1000000);
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  for (;;) __asm__ volatile ("wfi");
}
#endif

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

#ifdef SWITCH_WIFI_RUNTIME
STATIC EFI_STATUS
PrepareWindowsDevice (EFI_HANDLE Volume, CHAR16 *FileName, CONST CHAR8 Name[4])
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *Rsdp;
  EFI_ACPI_DESCRIPTION_HEADER *Xsdt, *Table, *Dsdt = NULL;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *Fadt;
  UINT8 Pattern[] = {0x08, 0, 0, 0, 0, 0x0A, 0x10};
  UINT8 *Ready = NULL, *Aml, Sum = 0;
  UINTN Entries, I, Matches = 0;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_HANDLE Image;
  EFI_STATUS Status;

  CopyMem (Pattern + 1, Name, 4);
  Status = EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, (VOID **)&Rsdp);
  if (EFI_ERROR (Status) || Rsdp == NULL || Rsdp->XsdtAddress == 0) return EFI_NOT_FOUND;
  Xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
  if (Xsdt->Length < sizeof (*Xsdt)) return EFI_COMPROMISED_DATA;
  Entries = (Xsdt->Length - sizeof (*Xsdt)) / sizeof (UINT64);
  for (I = 0; I < Entries && Dsdt == NULL; ++I) {
    Table = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)ReadUnaligned64 (
        (UINT64 *)((UINT8 *)Xsdt + sizeof (*Xsdt) + I * 8));
    if (Table->Signature != EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE) continue;
    if (Table->Length < sizeof (*Fadt)) return EFI_COMPROMISED_DATA;
    Fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)Table;
    Dsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)(Fadt->XDsdt ? Fadt->XDsdt : Fadt->Dsdt);
  }
  if (Dsdt == NULL || Dsdt->Length < sizeof (*Dsdt)) return EFI_NOT_FOUND;
  Aml = (UINT8 *)Dsdt;
  for (I = sizeof (*Dsdt); I + sizeof (Pattern) <= Dsdt->Length; ++I) {
    if (CompareMem (Aml + I, Pattern, sizeof (Pattern)) == 0) {
      Ready = Aml + I + 6;
      Matches++;
    }
  }
  if (Matches != 1) return EFI_NOT_FOUND;
  Path = FileDevicePath (Volume, FileName);
  if (Path == NULL) return EFI_OUT_OF_RESOURCES;
  Status = gBS->LoadImage (FALSE, gImageHandle, Path, NULL, 0, &Image);
  FreePool (Path);
  if (EFI_ERROR (Status)) return Status;
  Status = gBS->StartImage (Image, NULL, NULL);
  gBS->UnloadImage (Image);
  if (EFI_ERROR (Status)) return Status;
  *Ready = 0x0F;
  Dsdt->Checksum = 0;
  for (I = 0; I < Dsdt->Length; ++I) Sum = (UINT8)(Sum + Aml[I]);
  Dsdt->Checksum = (UINT8)(0 - Sum);
  MemoryFence ();
  return EFI_SUCCESS;
}
#endif

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

#ifdef SWITCH_WIFI_RUNTIME
  Status = PrepareWindowsDevice (Volume, L"\\SwitchWOA\\Wifi\\WifiPrepare.efi", "WSTA");
  if (EFI_ERROR (Status))
    Print (L"Wi-Fi preparation: %r; device hidden, continuing Windows boot.\r\n", Status);

#ifdef SWITCH_LITE_XUSB
  if (!EFI_ERROR (Status)) {
    Status = PrepareWindowsDevice (Volume, L"\\SwitchWOA\\Usb\\UsbPrepare.efi", "XSTA");
    if (EFI_ERROR (Status)) Print (L"USB host preparation returned %r.\r\n", Status);
  }
#endif
#endif
  Path = FileDevicePath (Volume, WINDOWS_LOADER);
  if (Path == NULL) return EFI_OUT_OF_RESOURCES;
  Status = EfiBootManagerInitializeLoadOption (&Option, LoadOptionNumberUnassigned,
      LoadOptionTypeBoot, LOAD_OPTION_ACTIVE, L"Windows 11 Lite", Path, NULL, 0);
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
  Print (L"SWITCH LITE / WINDOWS 11 LITE\r\n");
  EfiBootManagerConnectAll ();
#if defined(SWITCH_LITE_TOUCH) && defined(SWITCH_LITE_PERFORMANCE)
  CheckPowerBeforeWindows ();
  CheckWifiBeforeWindows ();
#endif
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

#endif /* SWITCH_LITE_BOOT_SD */
