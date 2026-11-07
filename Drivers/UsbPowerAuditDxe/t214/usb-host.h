/* SPDX-License-Identifier: GPL-2.0-only
 * Vali XUSB host handoff. The shared Falcon loader is in UsbPowerAudit.c.
 * The image lives in reserved system memory for the lifetime of the Falcon. */
#include <Protocol/LoadedImage.h>
#include "../../XusbInitApp/XusbPort.h"

#define LITE_XUSB_FILE L"\\SwitchWOA\\Usb\\tegra210b01_xusb_firmware"
#define XUSB_LIC1 0x60004100U
#define XUSB_HOST_ASID 0x70019288U
#define XUSB_IRQS (BIT7 | BIT8) /* SPI 39: xHCI, SPI 40: Falcon mailbox */

STATIC EFI_STATUS LiteReadXusb (EFI_HANDLE Image, EFI_PHYSICAL_ADDRESS *Address, UINTN *Size)
{
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root, *File;
  UINT64 Length;
  EFI_STATUS Status = gBS->HandleProtocol (Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) return Status;
  Status = gBS->HandleProtocol (Loaded->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) return Status;
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) return Status;
  Status = Root->Open (Root, &File, LITE_XUSB_FILE, EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (EFI_ERROR (Status)) return Status;
  Status = File->SetPosition (File, MAX_UINT64);
  if (!EFI_ERROR (Status)) Status = File->GetPosition (File, &Length);
  if (!EFI_ERROR (Status) && !XUSB_FW_SIZE_OK (Length)) Status = EFI_BAD_BUFFER_SIZE;
  if (!EFI_ERROR (Status)) Status = File->SetPosition (File, 0);
  if (!EFI_ERROR (Status)) {
    *Address = MAX_UINT32;
    Status = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType,
                                 EFI_SIZE_TO_PAGES ((UINTN)Length), Address);
    if (!EFI_ERROR (Status)) {
      UINT8 *Data = (UINT8 *)(UINTN)*Address;
      *Size = (UINTN)Length;
      Status = File->Read (File, Size, Data);
      if (!EFI_ERROR (Status) && (*Size != Length || ReadLe32 (Data + 100) != Length ||
          CompareMem (Data + 104, "XUSBFW", 6) || (ReadLe32 (Data + 72) >> 24) != 0x51 ||
          !ReadLe32 (Data + 12) || (UINT64)ReadLe32 (Data + 8) + ReadLe32 (Data + 12) > Length - 256))
        Status = EFI_COMPROMISED_DATA;
      if (EFI_ERROR (Status)) gBS->FreePages (*Address, EFI_SIZE_TO_PAGES ((UINTN)Length));
    }
  }
  File->Close (File);
  return Status;
}

STATIC BOOLEAN LiteUsbWait (UINTN Reg, UINT32 Mask, UINT32 Expected)
{
  for (UINTN I = 0; I < 1000; I++) {
    if ((MmioRead32 (Reg) & Mask) == Expected) return TRUE;
    gBS->Stall (2);
  }
  return FALSE;
}

STATIC BOOLEAN LiteUsbPlls (VOID)
{
  /* On this B01 the crystal is 38.4 MHz. Retain an already running PLL. */
  if ((MmioRead32 (CAR_BASE + 0x50) >> 28) != 5) return FALSE;
  if (!(MmioRead32 (CAR_BASE + 0xC0) & BIT27)) {
    MmioOr32 (CAR_BASE + 0xCC, BIT29);
    MmioAndThenOr32 (CAR_BASE + 0xC0, 0xFFE00000, BIT24 | 0x11902);
    MmioOr32 (CAR_BASE + 0xC0, BIT30);
    if (!LiteUsbWait (CAR_BASE + 0xC0, BIT27, BIT27)) return FALSE;
  }
  MmioOr32 (CAR_BASE + 0xC0, 0x02E00000);
  MmioOr32 (CAR_BASE + 0xC4, BIT0);
  if (!(MmioRead32 (CAR_BASE + 0x52C) & BIT31)) {
    MmioAndThenOr32 (CAR_BASE + 0x480, 0xFF0000FF, (25U << 16) | BIT8);
    MmioAndThenOr32 (CAR_BASE + 0x488, 0xFF00003F, 24U << 18);
    MmioAndThenOr32 (CAR_BASE + 0x484, 0x07FFA000, BIT15 | 375U);
    if (!LiteUsbWait (CAR_BASE + 0x52C, BIT31, BIT31)) return FALSE;
  }
  return TRUE;
}

STATIC BOOLEAN LiteUsbClocks (VOID)
{
  MmioWrite32 (CAR_BASE + 0x310, BIT25);
  MmioWrite32 (CAR_BASE + 0x438, BIT28);
  if (!SetPowerPartition (20, TRUE) || !SetPowerPartition (22, TRUE)) return FALSE;
  MmioWrite32 (CAR_BASE + 0x330, BIT25);
  MmioWrite32 (CAR_BASE + 0x448, BIT15 | BIT28);
  MmioAndThenOr32 (CAR_BASE + 0x600, ~0xE00000FFU, BIT29 | 6U); /* PLLP / 4 */
  MmioAndThenOr32 (CAR_BASE + 0x604, ~0xE00000FFU, BIT29 | 2U); /* PLLP / 2 */
  MmioAndThenOr32 (CAR_BASE + 0x608, ~0xE00000FFU, 2U << 29);
  MmioAndThenOr32 (CAR_BASE + 0x610, ~0xE70000FFU, (3U << 29) | 6U);
  gBS->Stall (20);
  if (!RemovePartitionClamp (20) || !RemovePartitionClamp (22)) return FALSE;
  MmioWrite32 (CAR_BASE + 0x43C, BIT28 | BIT15);
  MmioWrite32 (CAR_BASE + 0x314, BIT25);
  gBS->Stall (30);
  return !(MmioRead32 (CAR_BASE + 0x00C) & BIT25) &&
         !(MmioRead32 (CAR_BASE + 0x35C) & (BIT28 | BIT15 | BIT14));
}

STATIC BOOLEAN LiteUsbPads (VOID)
{
  UINTN Pad = XUSB_PAD_BASE;
  UINT32 Cal, Ext, Hs;
  /* UPHY PLL P0 and PLLE supplied by the B01 preparation shared with PCIe.
   * Never pulse padctl/UPHY reset or reprogram lane 0 while PCIe owns it. */
  if (!(MmioRead32 (Pad + 0x360) & BIT15) || !(MmioRead32 (CAR_BASE + 0xEC) & BIT11))
    return FALSE;
  MmioWrite32 (CAR_BASE + 0x328, BIT7); /* fuse clock */
  gBS->Stall (5);
  Cal = MmioRead32 (R362_FUSE_BASE + R362_FUSE_USB_CALIB);
  Ext = MmioRead32 (R362_FUSE_BASE + R362_FUSE_USB_CALIB_EXT);
  if (Cal == MAX_UINT32 || Ext == MAX_UINT32 || !Cal) return FALSE;
  Hs = ((Cal & 0x3F) + 6U) & 0x3F;
  MmioAndThenOr32 (Pad + 0x004, ~(3U | (3U << 18)), 1U | BIT18);
  MmioAndThenOr32 (Pad + 0x008, ~3U, 3); /* Vali port 0 OTG */
  MmioAndThenOr32 (Pad + 0x088, ~(0x3FU | BIT26 | BIT29), Hs);
  MmioAndThenOr32 (Pad + 0x08C, ~(BIT2 | (0xFU << 3) | (0x1FU << 26)),
                   ((Cal >> 7) & 0xF) << 3 | ((Ext & 0x1F) << 26));
  MmioAndThenOr32 (Pad + 0x084, ~(7U << 6), BIT7);
  MmioAnd32 (Pad + 0x080, ~BIT0);
  MmioAndThenOr32 (CAR_BASE + 0x6CC, ~0xE00000FFU, 6);
  MmioWrite32 (CAR_BASE + 0x29C, BIT18);
  MmioAndThenOr32 (Pad + 0x288, ~((0x7FU << 12) | (0x7FU << 19)),
                   (0x1EU << 12) | (0xAU << 19));
  /* Switchroot keeps tracking active while the UTMI port is powered. */
  MmioAndThenOr32 (Pad + 0x284, ~(BIT11 | 0x3FU), (7U << 3) | ((Cal >> 29) & 7U));
  gBS->Stall (1);
  MmioAnd32 (Pad + 0x288, ~BIT26);
  gBS->Stall (100);

  /* Select pcie-5 (= USB3-0 on B01); the Wi-Fi lane is pcie-0. */
  MmioAndThenOr32 (Pad + 0x028, ~(3U << 22), (1U << 22) | BIT6);
  MmioAnd32 (Pad + 0x024, ~BIT29); gBS->Stall (150);
  MmioAnd32 (Pad + 0x024, ~BIT30); gBS->Stall (150);
  MmioAnd32 (Pad + 0x024, ~BIT31);
  XusbEnableSsPort0 (Pad);
  MmioAndThenOr32 (Pad + 0xC60, ~(BIT14 | (3U << 16) | (0xFU << 18)), BIT16);
  return (MmioRead32 (Pad + 0x088) & 0x3F) == Hs &&
         (MmioRead32 (Pad + 0x028) & ((3U << 22) | BIT6)) == ((1U << 22) | BIT6);
}

EFI_STATUS EFIAPI LiteUsbPrepareMain (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_PHYSICAL_ADDRESS Address = 0;
  UINTN Size = 0;
  EFI_STATUS Status;
  UINT32 Asid = 0, LicClass, Id, Cap, Op;
  BOOLEAN Decode = FALSE, Dma = FALSE;
  CONST CHAR16 *Stage;
  (VOID)SystemTable;
  if ((MmioRead32 (0x70000804) & 0xFFF0) != 0x2120) return EFI_UNSUPPORTED;
  Status = LiteReadXusb (Image, &Address, &Size);
  if (EFI_ERROR (Status)) return Status;
  Status = EFI_DEVICE_ERROR;
  Stage = L"USB PLL lock";
  if (!LiteUsbPlls ()) goto Failed;
  Stage = L"host power and clocks";
  if (!LiteUsbClocks ()) goto Failed;
  Stage = L"UTMI and lane 5";
  if (!LiteUsbPads ()) goto Failed;
  Stage = L"XUSB register access";
  MmioOr32 (XUSB_IPFS_BASE + 0x180, BIT0);
  gBS->Stall (20);
  Id = MmioRead32 (XUSB_FPCI_BASE);
  if (Id != 0x0FAC10DE && Id != 0x0FAD10DE) goto Failed;
  MmioAnd16 (XUSB_FPCI_BASE + 4, (UINT16)~BIT2);
  if (MmioRead16 (XUSB_FPCI_BASE + 4) & BIT2) goto Failed;
  Asid = MmioRead32 (XUSB_HOST_ASID);
  MmioWrite32 (XUSB_HOST_ASID, 0);
  Decode = TRUE;
  if (MmioRead32 (XUSB_HOST_ASID) != 0) goto Failed;
  MmioAndThenOr32 (XUSB_FPCI_BASE + 0x10, 0x7FFFU, XUSB_HCD_BASE);
  gBS->Stall (200);
  MmioOr16 (XUSB_FPCI_BASE + 4, (UINT16)(BIT1 | BIT2));
  Dma = TRUE;
  MmioOr32 (XUSB_IPFS_BASE + 0x188, BIT16);
  MmioWrite32 (XUSB_IPFS_BASE + 0x1BC, 0x80);
  Stage = L"Falcon boot";
  Cap = MmioRead32 (XUSB_HCD_BASE);
  Op = Cap & 0xFF;
  if (Op != 0x20 || (Cap >> 16) != 0x0100 ||
      !RunR246FirmwareTransfer ((volatile UINT32 *)XUSB_FPCI_BASE, (UINT8 *)(UINTN)Address,
                              Size, Address, TRUE) ||
      !RunR249FalconStart ((volatile UINT32 *)XUSB_FPCI_BASE, (volatile UINT32 *)XUSB_HCD_BASE,
                         ReadLe32 ((UINT8 *)(UINTN)Address + 8))) goto Failed;
  if (!(MmioRead32 (XUSB_HCD_BASE + Op + 4) & BIT0) ||
      (MmioRead32 (XUSB_HCD_BASE + 4) >> 24) < 5) goto Failed;
  /* Windows takes the halted xHC. The KMDF companion starts Falcon messaging. */
  Stage = L"IRQ routing";
  LicClass = MmioRead32 (XUSB_LIC1 + 0x2C);
  MmioWrite32 (XUSB_LIC1 + 0x2C, LicClass & ~XUSB_IRQS);
  MmioWrite32 (XUSB_LIC1 + 0x1C, XUSB_IRQS);
  MmioWrite32 (XUSB_LIC1 + 0x24, XUSB_IRQS);
  if ((MmioRead32 (XUSB_LIC1 + 0x20) & XUSB_IRQS) != XUSB_IRQS) goto Failed;
  ArmDataSynchronizationBarrier ();
  return EFI_SUCCESS;

Failed:
  Print (L"XUSB preparation stopped at %s.\r\n", Stage);
  if (Dma) {
    MmioAnd16 (XUSB_FPCI_BASE + 4, (UINT16)~BIT2);
    MmioWrite32 (CAR_BASE + 0x310, BIT25);
    if (!LiteUsbWait (CAR_BASE + 0x00C, BIT25, BIT25)) return Status; /* DMA ownership uncertain */
  }
  if (Decode) MmioWrite32 (XUSB_HOST_ASID, Asid);
  gBS->FreePages (Address, EFI_SIZE_TO_PAGES (Size));
  return Status;
}
