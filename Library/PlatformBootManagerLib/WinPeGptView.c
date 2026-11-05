// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WinPeGptView.h"
#include "winarm-target.h"
#include <IndustryStandard/Mbr.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Uefi/UefiGpt.h>

STATIC EFI_BLOCK_IO_PROTOCOL *mDisk;
STATIC EFI_BLOCK_READ mReadBlocks;
STATIC UINT32 mMediaId;
STATIC MBR_PARTITION_RECORD mOriginal[MAX_MBR_PARTITIONS];
STATIC MBR_PARTITION_RECORD mProtective;

STATIC EFI_STATUS EFIAPI
ReadWindowsView (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buffer)
{
  EFI_STATUS Status = mReadBlocks (This, MediaId, Lba, Size, Buffer);
  if (!EFI_ERROR (Status) && This == mDisk && MediaId == mMediaId && Lba == 0 && Size >= MBR_SIZE) {
    MASTER_BOOT_RECORD *Mbr = Buffer;
    if (Mbr->Signature == MBR_SIGNATURE && CompareMem (Mbr->Partition, mOriginal, sizeof (mOriginal)) == 0) {
      ZeroMem (Mbr->Partition, sizeof (mOriginal));
      CopyMem (&Mbr->Partition[0], &mProtective, sizeof (mProtective));
    }
  }
  return Status;
}

// Resolve the parent of the selected HD() partition, not the first disk in the machine.
STATIC EFI_STATUS
ParentDisk (EFI_HANDLE Volume, EFI_BLOCK_IO_PROTOCOL **Disk)
{
  EFI_DEVICE_PATH_PROTOCOL *Path = DuplicateDevicePath (DevicePathFromHandle (Volume));
  EFI_STATUS Status = EFI_NOT_FOUND;
  EFI_HANDLE Parent;
  if (Path == NULL) return EFI_OUT_OF_RESOURCES;
  for (EFI_DEVICE_PATH_PROTOCOL *Node = Path; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if (DevicePathType (Node) == MEDIA_DEVICE_PATH && DevicePathSubType (Node) == MEDIA_HARDDRIVE_DP &&
        IsDevicePathEnd (NextDevicePathNode (Node))) {
      EFI_DEVICE_PATH_PROTOCOL *Remaining = Path;
      SetDevicePathEndNode (Node);
      Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &Remaining, &Parent);
      if (!EFI_ERROR (Status) && !IsDevicePathEnd (Remaining)) Status = EFI_NOT_FOUND;
      if (!EFI_ERROR (Status))
        Status = gBS->HandleProtocol (Parent, &gEfiBlockIoProtocolGuid, (VOID **)Disk);
      break;
    }
  }
  FreePool (Path);
  return Status;
}

EFI_STATUS
BeginWinPeGptView (EFI_HANDLE Volume)
{
  EFI_BLOCK_IO_PROTOCOL *Disk;
  EFI_STATUS Status;
  UINTN Protective = MAX_MBR_PARTITIONS, Active = 0, Pages;
  UINT32 Crc, ExpectedCrc;
  VOID *Sector;

  if (mDisk != NULL) return EFI_ALREADY_STARTED;
  // A hybrid card may have either kind of HD() handle. Bind its parent by the on-disk identities below.
  Status = ParentDisk (Volume, &Disk);
  if (EFI_ERROR (Status)) return Status;
  EFI_BLOCK_IO_MEDIA *Media = Disk->Media;
  if (Media == NULL || Media->LogicalPartition || Media->BlockSize != 512 || Media->LastBlock != LITE_CARD_BYTES / 512 - 1)
    return EFI_UNSUPPORTED;
  Pages = EFI_SIZE_TO_PAGES ((UINTN)Media->BlockSize);
  Sector = AllocateAlignedPages (Pages, MAX ((UINTN)EFI_PAGE_SIZE, (UINTN)Media->IoAlign));
  if (Sector == NULL) return EFI_OUT_OF_RESOURCES;

  Status = Disk->ReadBlocks (Disk, Media->MediaId, 0, Media->BlockSize, Sector);
  if (EFI_ERROR (Status)) goto Done;
  MASTER_BOOT_RECORD *Mbr = Sector;
  if (Mbr->Signature != MBR_SIGNATURE) { Status = EFI_VOLUME_CORRUPTED; goto Done; }
  CopyMem (mOriginal, Mbr->Partition, sizeof (mOriginal));
  if (!LiteSelectWindowsPartition (Sector)) { Status = EFI_NOT_FOUND; goto Done; }
  Print (L"SD MBR types: %02x %02x %02x %02x\r\n", mOriginal[0].OSIndicator,
         mOriginal[1].OSIndicator, mOriginal[2].OSIndicator, mOriginal[3].OSIndicator);
  for (UINTN I = 0; I < MAX_MBR_PARTITIONS; ++I) {
    if (mOriginal[I].OSIndicator == 0) continue;
    ++Active;
    if (mOriginal[I].OSIndicator == PMBR_GPT_PARTITION) {
      if (Protective != MAX_MBR_PARTITIONS || ReadUnaligned32 ((CONST UINT32 *)mOriginal[I].StartingLBA) != 1) {
        Status = EFI_VOLUME_CORRUPTED;
        goto Done;
      }
      Protective = I;
    }
  }
  if (Protective == MAX_MBR_PARTITIONS || Active <= 1) {
    Print (L"SD view: native partition table\r\n");
    goto Done;
  }

  // PartitionDxe already enumerated this GPT volume. Verify the primary
  // header too: Windows will use it after seeing the protective MBR.
  Status = Disk->ReadBlocks (Disk, Media->MediaId, PRIMARY_PART_HEADER_LBA, Media->BlockSize, Sector);
  if (EFI_ERROR (Status)) goto Done;
  EFI_PARTITION_TABLE_HEADER *Gpt = Sector;
  if (Gpt->Header.Signature != EFI_PTAB_HEADER_ID || Gpt->MyLBA != PRIMARY_PART_HEADER_LBA ||
      Gpt->Header.HeaderSize < sizeof (*Gpt) || Gpt->Header.HeaderSize > Media->BlockSize ||
      Gpt->AlternateLBA > Media->LastBlock || Gpt->LastUsableLBA >= Gpt->AlternateLBA ||
      Gpt->FirstUsableLBA > Gpt->LastUsableLBA) {
    Status = EFI_VOLUME_CORRUPTED;
    goto Done;
  }
  if (!LiteIsHeader (Sector)) { Status = EFI_NOT_FOUND; goto Done; }
  UINT32 ArrayCrc = Gpt->PartitionEntryArrayCRC32;
  ExpectedCrc = Gpt->Header.CRC32;
  Gpt->Header.CRC32 = 0;
  Status = gBS->CalculateCrc32 (Gpt, Gpt->Header.HeaderSize, &Crc);
  if (EFI_ERROR (Status)) goto Done;
  if (Crc != ExpectedCrc) { Status = EFI_CRC_ERROR; goto Done; }

  // One allocated EFI page holds all three sectors of this card's GPT array.
  Status = Disk->ReadBlocks (Disk, Media->MediaId, 2, 3 * 512, Sector);
  if (EFI_ERROR (Status)) goto Done;
  Status = gBS->CalculateCrc32 (Sector, 11 * 128, &Crc);
  if (EFI_ERROR (Status)) goto Done;
  if (Crc != ArrayCrc || !LiteIsPartition ((UINT8 *)Sector + 2 * 512)) {
    Status = EFI_VOLUME_CORRUPTED; goto Done;
  }
  Status = Disk->ReadBlocks (Disk, Media->MediaId, WINARM_FIRST, 512, Sector);
  if (EFI_ERROR (Status)) goto Done;
  if (!LiteIsBootSector (Sector)) { Status = EFI_NOT_FOUND; goto Done; }
  Print (L"WINARM GPT and NTFS verified.\r\n");

  CopyMem (&mProtective, &mOriginal[Protective], sizeof (mProtective));
  mProtective.BootIndicator = 0;
  mMediaId = Media->MediaId;
  mReadBlocks = Disk->ReadBlocks;
  mDisk = Disk;
  Disk->ReadBlocks = ReadWindowsView;
  Print (L"SD view: GPT for Windows (read only)\r\n");
Done:
  FreeAlignedPages (Sector, Pages);
  return Status;
}

VOID
EndWinPeGptView (VOID)
{
  if (mDisk != NULL) {
    if (mDisk->ReadBlocks == ReadWindowsView) mDisk->ReadBlocks = mReadBlocks;
    mDisk = NULL;
  }
}
