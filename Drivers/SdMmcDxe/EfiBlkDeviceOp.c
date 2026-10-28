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
        FALSE,                                    // ReadOnly (writes gated to FAT partitions, see SdWriteAllowed)
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

EFI_STATUS
EFIAPI
MMCHSReadBlocks(
    IN EFI_BLOCK_IO_PROTOCOL          *This,
    IN UINT32                         MediaId,
    IN EFI_LBA                        Lba,
    IN UINTN                          BufferSize,
    OUT VOID                          *Buffer
)
{
    BIO_INSTANCE              *Instance;
    EFI_BLOCK_IO_MEDIA        *Media;
    UINTN                     BlockSize;
    UINTN                     rc;

    Instance  = BIO_INSTANCE_FROM_BLOCKIO_THIS(This);
    Media     = &Instance->BlockMedia;
    BlockSize = Media->BlockSize;

    if (MediaId != Media->MediaId) 
    {
        return EFI_MEDIA_CHANGED;
    }

    if (Lba > Media->LastBlock) 
    {
        return EFI_INVALID_PARAMETER;
    }

    if ((Lba + (BufferSize / BlockSize) - 1) > Media->LastBlock) 
    {
        return EFI_INVALID_PARAMETER;
    }

    if (BufferSize % BlockSize != 0) 
    {
        return EFI_BAD_BUFFER_SIZE;
    }

    if (Buffer == NULL) 
    {
        return EFI_INVALID_PARAMETER;
    }

    if (BufferSize == 0) 
    {
        return EFI_SUCCESS;
    }

    rc = MmcReadInternal(Instance, (UINT64) Lba * BlockSize, Buffer, BufferSize);
    if (rc == 1)
        return EFI_SUCCESS;
    else
        return EFI_DEVICE_ERROR;
}

//
// ---- Write gate: allow writes only inside partitions whose first sector is a FAT boot sector ----
//
#define SD_MAX_ALLOWED  16

typedef struct { UINT64 First; UINT64 Last; } SD_RANGE;

STATIC SD_RANGE  mAllowed[SD_MAX_ALLOWED];
STATIC UINTN     mNAllowed;
STATIC BOOLEAN   mAllowedInit;

STATIC
UINT32
Le32 (CONST UINT8 *P)
{
    return (UINT32) P[0] | ((UINT32) P[1] << 8) | ((UINT32) P[2] << 16) | ((UINT32) P[3] << 24);
}

STATIC
UINT64
Le64 (CONST UINT8 *P)
{
    return (UINT64) Le32 (P) | ((UINT64) Le32 (P + 4) << 32);
}

STATIC
BOOLEAN
SdReadSector (
    IN  BIO_INSTANCE *Instance,
    IN  UINT64       Lba,
    OUT UINT8        *Buf
)
{
    return MmcReadInternal (Instance, Lba * 512, (UINT32 *) Buf, 512) == 1;
}

// FAT12/16: "FAT1x   " at 0x36, FAT32: "FAT32   " at 0x52, both with the 0x55AA signature.
// exFAT ("EXFAT   " at 3) and NTFS ("NTFS    " at 3) do not match.
STATIC
BOOLEAN
SdIsFatBootSector (
    IN CONST UINT8 *S
)
{
    if (S[510] != 0x55 || S[511] != 0xAA) return FALSE;
    return CompareMem (S + 0x52, "FAT32   ", 8) == 0 || CompareMem (S + 0x36, "FAT", 3) == 0;
}

STATIC
VOID
SdAddIfFat (
    IN BIO_INSTANCE *Instance,
    IN UINT64       First,
    IN UINT64       Last,
    IN UINT8        *Buf
)
{
    if (mNAllowed >= SD_MAX_ALLOWED || Last < First || Last > Instance->BlockMedia.LastBlock) return;
    if (!SdReadSector (Instance, First, Buf) || !SdIsFatBootSector (Buf)) return;
    mAllowed[mNAllowed].First = First;
    mAllowed[mNAllowed].Last  = Last;
    mNAllowed++;
    DEBUG ((DEBUG_INFO, "SD: FAT range lba %lx-%lx writable\n", First, Last));
}

STATIC
VOID
SdBuildAllowed (
    IN BIO_INSTANCE *Instance
)
{
    UINT8  *Buf;
    UINT8  *Hdr;
    UINT32 I;
    BOOLEAN Gpt = FALSE;

    mAllowedInit = TRUE;
    mNAllowed = 0;
    if (Instance->BlockMedia.BlockSize != 512) return;

    Buf = AllocateZeroPool (512);
    Hdr = AllocateZeroPool (512);
    if (Buf == NULL || Hdr == NULL) goto out;

    if (!SdReadSector (Instance, 0, Buf) || Buf[510] != 0x55 || Buf[511] != 0xAA) goto out;

    for (I = 0; I < 4; I++) {
        CONST UINT8 *E = Buf + 0x1BE + 16 * I;
        if (E[4] == 0xEE) { Gpt = TRUE; continue; }
        if (E[4] != 0 && Le32 (E + 12) != 0) {                     // extended partitions are not followed
            UINT64 First = Le32 (E + 8);
            SdAddIfFat (Instance, First, First + Le32 (E + 12) - 1, Hdr);
        }
    }

    if (Gpt && SdReadSector (Instance, 1, Hdr) && CompareMem (Hdr, "EFI PART", 8) == 0) {
        UINT64 EntLba  = Le64 (Hdr + 72);
        UINT32 NumEnt  = Le32 (Hdr + 80);
        UINT32 EntSize = Le32 (Hdr + 84);
        UINT32 Per     = (EntSize >= 128 && EntSize <= 512) ? 512 / EntSize : 0;

        if (Per != 0 && NumEnt <= 256) {
            UINT32 N;
            for (N = 0; N < NumEnt; N++) {
                UINT8 Z[16] = { 0 };
                if (N % Per == 0 && !SdReadSector (Instance, EntLba + N / Per, Buf)) break;
                {
                    CONST UINT8 *E = Buf + (N % Per) * EntSize;
                    if (CompareMem (E, Z, 16) == 0) continue;     // unused entry
                    SdAddIfFat (Instance, Le64 (E + 32), Le64 (E + 40), Hdr);
                }
            }
        }
    }

out:
    if (Buf != NULL) FreePool (Buf);
    if (Hdr != NULL) FreePool (Hdr);
}

STATIC
BOOLEAN
SdWriteAllowed (
    IN BIO_INSTANCE *Instance,
    IN UINT64       Lba,
    IN UINT64       Blocks
)
{
    UINTN I;

    if (!mAllowedInit) SdBuildAllowed (Instance);
    for (I = 0; I < mNAllowed; I++) {
        if (Lba >= mAllowed[I].First && (Lba + Blocks - 1) <= mAllowed[I].Last) return TRUE;
    }
    return FALSE;
}

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
    BIO_INSTANCE              *Instance;
    EFI_BLOCK_IO_MEDIA        *Media;
    UINTN                     BlockSize;
    UINT64                    Blocks;

    Instance  = BIO_INSTANCE_FROM_BLOCKIO_THIS(This);
    Media     = &Instance->BlockMedia;
    BlockSize = Media->BlockSize;

    if (MediaId != Media->MediaId) return EFI_MEDIA_CHANGED;
    if (Buffer == NULL) return EFI_INVALID_PARAMETER;
    if (BufferSize == 0) return EFI_SUCCESS;
    if (BufferSize % BlockSize != 0) return EFI_BAD_BUFFER_SIZE;

    Blocks = BufferSize / BlockSize;
    if (Lba > Media->LastBlock || (Lba + Blocks - 1) > Media->LastBlock) return EFI_INVALID_PARAMETER;

    // Safety gate: only FAT volumes are writable. The NTFS Windows partition and the partition
    // tables stay read-only, because this write path has not been verified on hardware yet.
    if (!SdWriteAllowed (Instance, Lba, Blocks)) {
        return EFI_WRITE_PROTECTED;
    }

    if (mmc_bwrite (Lba, Blocks, Buffer) != Blocks) {
        return EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS;
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
