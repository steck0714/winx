#include <PiDxe.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/DevicePath.h>
#include <Device/StaticDevices.h>
#include <Foundation/Types.h>

#include "Include/SdMmc.h"
#include "Include/HostOp.h"
#include "Include/EfiProto.h"
#include <winarm-target.h>

STATIC BIO_INSTANCE mBioTemplate = {
    BIO_INSTANCE_SIGNATURE,
    NULL, // Handle
    { 
        // BlockIo
        EFI_BLOCK_IO_INTERFACE_REVISION,   // Revision
        NULL,                              // *Media
        MMCHSReset,                        // Reset
        MMCHSReadBlocks,                   // ReadBlocks
        MMCHSWriteBlocks,                  // WriteBlocks
        MMCHSFlushBlocks                   // FlushBlocks
    },
    { 
        // BlockMedia
        BIO_INSTANCE_SIGNATURE,                   // MediaId
        TRUE,                                     // RemovableMedia
        TRUE,                                     // MediaPresent
        FALSE,                                    // LogicalPartition
        TRUE,                                     // Diagnostic volume is read-only
        FALSE,                                    // WriteCaching
        0,                                        // BlockSize
        4,                                        // IoAlign
        0,                                        // Pad
        0                                         // LastBlock
    },
    {   // DevicePath
        {
            {
                HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
                { (UINT8) (sizeof(VENDOR_DEVICE_PATH)), (UINT8) ((sizeof(VENDOR_DEVICE_PATH)) >> 8) },
            },
            // Hardware Device Path for Bio
            EFI_CALLER_ID_GUID // Use the driver's GUID
        },
        {
            END_DEVICE_PATH_TYPE,
            END_ENTIRE_DEVICE_PATH_SUBTYPE,
            { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
        }
    }
};

EFI_STATUS
EFIAPI
MMCHSReset(
    IN EFI_BLOCK_IO_PROTOCOL          *This,
    IN BOOLEAN                        ExtendedVerification
)
{
    return EFI_SUCCESS;
}

/*
 * Function: MmcReadInternal
 * Arg     : Data address on card, o/p buffer & data length
 * Return  : 0 on Success, non zero on failure
 * Flow    : Read data from the card to out
 */
STATIC UINT32 MmcReadInternal
(
    BIO_INSTANCE *Instance, 
    UINT64 DataAddr, 
    UINT32 *Buf, 
    UINT32 DataLen
)
{
    UINT32 Ret = 0;
    UINT32 BlockSize = Instance->BlockMedia.BlockSize;
    /* Operation block size. Will decrement until reaches 1 */
#ifdef SWITCH_SD_FAST
    UINT32 OpBlkSize = 256;     // 128 KiB per request (was 32 KiB): fewer command round trips
#else
    UINT32 OpBlkSize = 64;
#endif
    UINT32 ReadSize;
    UINT8 *Sptr = (UINT8 *) Buf;

    ASSERT(!(DataAddr % BlockSize));
    ASSERT(!(DataLen % BlockSize));

    /*
    * DMA onto write back memory is unsafe/nonportable,
    * but callers to this routine normally provide
    * write back buffers. Invalidate cache
    * before read data from MMC.
    */
    WriteBackInvalidateDataCacheRange(Buf, DataLen);

    /* TODO: This function is aware of max data that can be
    * tranferred using sdhci adma mode, need to have a cleaner
    * implementation to keep this function independent of sdhci
    * limitations
    */

    /* Set size */
    ReadSize = BlockSize * OpBlkSize;

read:
    while (DataLen > ReadSize) 
    {
        Ret = mmc_bread((DataAddr / BlockSize), (ReadSize / BlockSize), (VOID *) Sptr);
        if (Ret == 0)
        {
            // Attempt to reduce the block size
            if (OpBlkSize > 1)
            {
                // Decrement op size
                OpBlkSize = OpBlkSize / 2;
                ReadSize = BlockSize * OpBlkSize;
                goto read;
            }
            else
            {
                DEBUG((EFI_D_ERROR, "Failed Reading block @ %x\n",(UINTN) (DataAddr / BlockSize)));
                goto fail;
            }
        }
        Sptr += ReadSize;
        DataAddr += ReadSize;
        DataLen -= ReadSize;
    }

    if (DataLen)
    {
        Ret = mmc_bread((DataAddr / BlockSize), (DataLen / BlockSize), (VOID *) Sptr);
        if (Ret == 0)
        {
            // Attempt to reduce the block size
            if (OpBlkSize > 1)
            {
                // Decrement op size
                OpBlkSize = OpBlkSize / 2;
                ReadSize = BlockSize * OpBlkSize;
                goto read;
            }
            else
            {
                DEBUG((EFI_D_ERROR, "Failed Reading block @ %x\n",(UINTN) (DataAddr / BlockSize)));
                goto fail;
            }
        }
    }

    return 1;
fail:
    return 0;
}

// Validate the physical disk before publishing a single-partition, read-only MBR
// view. Windows sees this same signature and offset through its native SD driver.
EFI_STATUS
LitePrepareWindowsDisk (BIO_INSTANCE *Instance)
{
    EFI_STATUS Status = EFI_COMPROMISED_DATA;
    if (Instance->BlockMedia.BlockSize != 512 ||
        Instance->BlockMedia.LastBlock != LITE_CARD_BYTES / 512 - 1)
        return EFI_UNSUPPORTED;

    UINT8 *Data = AllocatePages (1);
    if (Data == NULL) return EFI_OUT_OF_RESOURCES;
    // LBA 0, the GPT header and the entire 11-entry GPT array fit in one page.
    if (!MmcReadInternal (Instance, 0, (UINT32 *)Data, 5 * 512)) {
        Status = EFI_DEVICE_ERROR;
        goto Exit;
    }
    UINT8 *Header = Data + 512;
    UINT8 *Entries = Data + 1024;
    if (!LiteSelectWindowsPartition (Data) || !LiteIsHeader (Header) ||
        !LiteIsPartition (Entries + 1024)) goto Exit;
    UINT32 HeaderBytes = LiteU32 (Header + 12);
    UINT32 HeaderCrc = LiteU32 (Header + 16);
    UINT32 ActualCrc;
    if (HeaderBytes < 92 || HeaderBytes > 512) goto Exit;
    ZeroMem (Header + 16, 4);
    if (EFI_ERROR (gBS->CalculateCrc32 (Header, HeaderBytes, &ActualCrc)) || ActualCrc != HeaderCrc ||
        EFI_ERROR (gBS->CalculateCrc32 (Entries, 11 * 128, &ActualCrc)) || ActualCrc != LiteU32 (Header + 88))
        goto Exit;
    CopyMem (Instance->WindowsMbr, Data, 512);
    if (!MmcReadInternal (Instance, (UINT64)WINARM_FIRST * 512, (UINT32 *)Data, 512)) {
        Status = EFI_DEVICE_ERROR;
        goto Exit;
    }
    if (LiteIsBootSector (Data)) Status = EFI_SUCCESS;
Exit:
    FreePages (Data, 1);
    return Status;
}

EFI_STATUS
EFIAPI
MMCHSReadBlocks (
    IN EFI_BLOCK_IO_PROTOCOL *This,
    IN UINT32 MediaId,
    IN EFI_LBA Lba,
    IN UINTN BufferSize,
    OUT VOID *Buffer
)
{
    BIO_INSTANCE *Instance = BIO_INSTANCE_FROM_BLOCKIO_THIS (This);
    EFI_BLOCK_IO_MEDIA *Media = &Instance->BlockMedia;
    if (MediaId != Media->MediaId) return EFI_MEDIA_CHANGED;
    if (BufferSize % 512 != 0) return EFI_BAD_BUFFER_SIZE;
    if (BufferSize == 0) return EFI_SUCCESS;
    UINTN Blocks = BufferSize / 512;
    if (Buffer == NULL || Lba > Media->LastBlock || Blocks - 1 > Media->LastBlock - Lba)
        return EFI_INVALID_PARAMETER;

    EFI_LBA End = Lba + Blocks;
    EFI_LBA First = MAX (Lba, (EFI_LBA)WINARM_FIRST);
    EFI_LBA Last = MIN (End, (EFI_LBA)WINARM_FIRST + WINARM_COUNT);
    if (First != Lba || Last != End) {
        ZeroMem (Buffer, BufferSize);
        if (Lba == 0) CopyMem (Buffer, Instance->WindowsMbr, 512);
    }
    if (First >= Last) return EFI_SUCCESS;
    UINT8 *Out = (UINT8 *)Buffer + (UINTN)(First - Lba) * 512;
    while (First < Last) {
        UINT32 Count = (UINT32)MIN (Last - First, 2048ULL);
        if (!MmcReadInternal (Instance, First * 512, (UINT32 *)Out, Count * 512)) return EFI_DEVICE_ERROR;
        Out += Count * 512;
        First += Count;
    }
    return EFI_SUCCESS;
}

//
EFI_STATUS
EFIAPI
MMCHSWriteBlocks(
    IN EFI_BLOCK_IO_PROTOCOL          *This,
    IN UINT32                         MediaId,
    IN EFI_LBA                        Lba,
    IN UINTN                          BufferSize,
    IN VOID                           *Buffer
)
{
    return EFI_WRITE_PROTECTED;
}

EFI_STATUS
EFIAPI
MMCHSFlushBlocks(
    IN EFI_BLOCK_IO_PROTOCOL  *This
)
{
    return EFI_SUCCESS;
}

EFI_STATUS
BioInstanceContructor(
    OUT BIO_INSTANCE** NewInstance
)
{
    BIO_INSTANCE* Instance;

    Instance = AllocateCopyPool(sizeof(BIO_INSTANCE), &mBioTemplate);
    if (Instance == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    Instance->BlockIo.Media = &Instance->BlockMedia;

    *NewInstance = Instance;
    return EFI_SUCCESS;
}
