/* SPDX-License-Identifier: ISC */
/* BCM4356 CR4/TCM firmware startup. Sequence shared with the accepted UEFI
 * path and derived from Broadcom brcmfmac/chip.c and pcie.c; see core/NOTICE. */
#include "chip.h"
#include <stdint.h>
#include <string.h>

#define SWIFI_POOL_TAG 0x69576653u
#define SWIFI_BCM_ID 0x43ec14e4u
#define SWIFI_CHIPCOMMON 0x18000000u
#define SWIFI_RAM_BASE 0x00180000u

/* Compile the unchanged shared format parsers with kernel allocation. These
 * aliases are local to this translation unit, not part of the Windows API. */
#define STATIC static
#define UINT8 uint8_t
#define UINT32 uint32_t
#define UINTN size_t
#define CHAR8 char
#define AllocateZeroPool(n) ExAllocatePool2(POOL_FLAG_NON_PAGED, (n), SWIFI_POOL_TAG)
#define FreePool(p) ExFreePoolWithTag((p), SWIFI_POOL_TAG)
#define CopyMem memcpy
#define CompareMem memcmp
#include "../core/WifiErom.h"
#include "../core/WifiNvram.h"
#undef STATIC
#undef UINT8
#undef UINT32
#undef UINTN
#undef CHAR8
#undef AllocateZeroPool
#undef FreePool
#undef CopyMem
#undef CompareMem

static ULONG ReadLe32(const UCHAR *Bytes)
{
    return (ULONG)Bytes[0] | ((ULONG)Bytes[1] << 8) |
        ((ULONG)Bytes[2] << 16) | ((ULONG)Bytes[3] << 24);
}

static VOID DelayMs(ULONG Milliseconds)
{
    LARGE_INTEGER Interval;
    Interval.QuadPart = -(LONGLONG)Milliseconds * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

static BOOLEAN SelectBackplane(SWIFI_CHIP *C, ULONG Address)
{
    ULONG Base = Address & ~0xfffu;
    ULONG Attempt;
    if (C->RuntimePinned)
        return FALSE;
    if (C->BackplaneWindow == Base)
        return TRUE;
    for (Attempt = 0; Attempt < 2; ++Attempt) {
        SwifiConfigWrite(C->Bus, 0x80, Base);
        if (SwifiConfigRead(C->Bus, 0x80) == Base) {
            C->BackplaneWindow = Base;
            return TRUE;
        }
    }
    C->BackplaneWindow = MAXULONG;
    return FALSE;
}

static BOOLEAN BpRead(SWIFI_CHIP *C, ULONG Address, ULONG *Value)
{
    if (!SelectBackplane(C, Address))
        return FALSE;
    *Value = READ_REGISTER_ULONG((PULONG)(C->Bar0 + (Address & 0xffc)));
    return *Value != MAXULONG;
}

static BOOLEAN BpWrite(SWIFI_CHIP *C, ULONG Address, ULONG Value)
{
    if (!SelectBackplane(C, Address))
        return FALSE;
    WRITE_REGISTER_ULONG((PULONG)(C->Bar0 + (Address & 0xffc)), Value);
    return TRUE;
}

static BOOLEAN CoreDisable(SWIFI_CHIP *C, ULONG Wrapper, ULONG Pre, ULONG Reset)
{
    ULONG Value, Attempt;
    if (!BpRead(C, Wrapper + 0x800, &Value))
        return FALSE;
    if (!(Value & 1)) {
        if (!BpWrite(C, Wrapper + 0x408, Pre | 3) ||
            !BpRead(C, Wrapper + 0x408, &Value) || !BpWrite(C, Wrapper + 0x800, 1))
            return FALSE;
        KeStallExecutionProcessor(20);
        for (Attempt = 0; Attempt < 300; ++Attempt) {
            if (!BpRead(C, Wrapper + 0x800, &Value))
                return FALSE;
            if (Value == 1)
                break;
            KeStallExecutionProcessor(1);
        }
        if (Attempt == 300)
            return FALSE;
    }
    return BpWrite(C, Wrapper + 0x408, Reset | 3) && BpRead(C, Wrapper + 0x408, &Value);
}

static BOOLEAN CoreReset(SWIFI_CHIP *C, ULONG Wrapper, ULONG Pre, ULONG Reset, ULONG Post)
{
    ULONG Value, Attempt;
    if (!CoreDisable(C, Wrapper, Pre, Reset))
        return FALSE;
    for (Attempt = 0; Attempt < 50; ++Attempt) {
        if (!BpRead(C, Wrapper + 0x800, &Value))
            return FALSE;
        if (!(Value & 1))
            break;
        if (!BpWrite(C, Wrapper + 0x800, 0))
            return FALSE;
        KeStallExecutionProcessor(60);
    }
    if (Attempt == 50)
        return FALSE;
    return BpWrite(C, Wrapper + 0x408, Post | 1) && BpRead(C, Wrapper + 0x408, &Value);
}

static NTSTATUS ProbeBar(SWIFI_PCIE_RESOURCES *Bus, USHORT Offset,
    ULONG *Address, ULONG *Length)
{
    ULONG Original = SwifiConfigRead(Bus, Offset);
    ULONG High = 0, Mask, HighMask = MAXULONG;
    BOOLEAN Wide = (Original & 6) == 4;
    if (Original == MAXULONG || (Original & 1) || ((Original & 6) != 0 && !Wide))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (Wide)
        High = SwifiConfigRead(Bus, Offset + 4);
    SwifiConfigWrite(Bus, Offset, MAXULONG);
    if (Wide)
        SwifiConfigWrite(Bus, Offset + 4, MAXULONG);
    Mask = SwifiConfigRead(Bus, Offset);
    if (Wide)
        HighMask = SwifiConfigRead(Bus, Offset + 4);
    /* Restore both halves with memory decoding disabled, including failure. */
    if (Wide)
        SwifiConfigWrite(Bus, Offset + 4, High);
    SwifiConfigWrite(Bus, Offset, Original);
    if (High || HighMask != MAXULONG || (Mask & 1))
        return STATUS_CONFLICTING_ADDRESSES;
    *Address = Original & ~0xfu;
    *Length = ~(Mask & ~0xfu) + 1;
    if (!*Length || (*Length & (*Length - 1)) || (*Address & (*Length - 1)))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    return STATUS_SUCCESS;
}

NTSTATUS SwifiChipMapAndDiscover(SWIFI_CHIP *C, SWIFI_PCIE_RESOURCES *Bus)
{
    ULONG Command, Bar0, Bar2, Erom, Value, Banks, Index;
    uint32_t *Words = NULL;
    PEX_CORE Cores[32];
    size_t Count = 0;
    NTSTATUS Status;
    RtlZeroMemory(C, sizeof(*C));
    C->Bus = Bus;
    C->BackplaneWindow = MAXULONG;
    C->RamBase = SWIFI_RAM_BASE;
    if (!(READ_REGISTER_ULONG((PULONG)(Bus->Host + SWIFI_ROOT_OFFSET + 0x90)) & 0x20000000))
        return STATUS_DEVICE_NOT_READY;
    if (SwifiConfigRead(Bus, 0) != SWIFI_BCM_ID)
        return STATUS_DEVICE_DOES_NOT_EXIST;
    Command = SwifiConfigRead(Bus, 4) & 0xffff;
    SwifiConfigWrite(Bus, 4, Command & ~6u);
    if (SwifiConfigRead(Bus, 4) & 6)
        return STATUS_DEVICE_PROTOCOL_ERROR;
    Status = ProbeBar(Bus, 0x10, &Bar0, &C->Bar0Length);
    if (NT_SUCCESS(Status))
        Status = ProbeBar(Bus, 0x18, &Bar2, &C->Bar2Length);
    SwifiConfigWrite(Bus, 4, (Command | 2) & ~4u);
    if (!NT_SUCCESS(Status))
        return Status;
    if (C->Bar0Length < 0x1000 || C->Bar0Length > 0x100000 ||
        C->Bar2Length < SWIFI_RAM_BASE + 4 || C->Bar2Length > 0x1000000 ||
        ((ULONGLONG)Bar0 < (ULONGLONG)Bar2 + C->Bar2Length &&
         (ULONGLONG)Bar2 < (ULONGLONG)Bar0 + C->Bar0Length))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    Status = SwifiMapBar(Bus, Bar0, C->Bar0Length, &C->Bar0);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = SwifiMapBar(Bus, Bar2, C->Bar2Length, &C->Bar2);
    if (!NT_SUCCESS(Status))
        goto Failed;
    C->Stage = SwifiChipBarsMapped;
    Status = STATUS_DEVICE_PROTOCOL_ERROR;
    if (!BpRead(C, SWIFI_CHIPCOMMON, &C->ChipId) || (C->ChipId & 0xffff) != 0x4356 ||
        !BpRead(C, SWIFI_CHIPCOMMON + 0xfc, &Erom) || (Erom & 0xfff) ||
        Erom < 0x18000000 || Erom >= 0x19000000)
        goto Failed;
    Words = ExAllocatePool2(POOL_FLAG_NON_PAGED, 4096, SWIFI_POOL_TAG);
    if (!Words) { Status = STATUS_INSUFFICIENT_RESOURCES; goto Failed; }
    for (Index = 0; Index < 1024; ++Index) {
        if (!BpRead(C, Erom + Index * 4, &Value))
            goto Failed;
        Words[Index] = Value;
    }
    if (!PexEromParse(Words, 1024, Cores, RTL_NUMBER_OF(Cores), &Count))
        goto Failed;
    ExFreePoolWithTag(Words, SWIFI_POOL_TAG);
    Words = NULL;
    for (Index = 0; Index < Count; ++Index) {
        if (Cores[Index].Id == 0x83e && !C->ArmBase && Cores[Index].Wrapper) {
            C->ArmBase = Cores[Index].Base;
            C->ArmWrapper = Cores[Index].Wrapper;
        } else if (Cores[Index].Id == 0x812 && !C->D11Wrapper) {
            C->D11Wrapper = Cores[Index].Wrapper;
        } else if (Cores[Index].Id == 0x83c && !C->PcieBase) {
            C->PcieBase = Cores[Index].Base;
            C->PcieRevision = Cores[Index].Revision;
        }
    }
    if (!C->ArmBase || !C->ArmWrapper || !C->D11Wrapper || !C->PcieBase ||
        !CoreDisable(C, C->D11Wrapper, 0x0c, 4) ||
        !CoreReset(C, C->ArmWrapper, 0x20, 0x20, 0x20) ||
        !BpRead(C, C->ArmBase + 4, &Value))
        goto Failed;
    Banks = (Value & 0xf) + ((Value >> 4) & 0xf);
    if (!Banks)
        goto Failed;
    for (Index = 0; Index < Banks; ++Index) {
        if (!BpWrite(C, C->ArmBase + 0x40, Index) ||
            !BpRead(C, C->ArmBase + 0x44, &Value))
            goto Failed;
        C->RamCapacity += ((Value & 0x3f) + 1) * 8192;
    }
    if (!C->RamCapacity || C->RamCapacity > C->Bar2Length - C->RamBase)
        goto Failed;
    C->RamSize = C->RamCapacity;
    C->Stage = SwifiChipDiscovered;
    return STATUS_SUCCESS;
Failed:
    if (Words)
        ExFreePoolWithTag(Words, SWIFI_POOL_TAG);
    /* Bus mastering is still disabled, and this call has published no DMA. */
    SwifiChipUnmapAfterReset(C);
    return Status;
}

BOOLEAN SwifiChipTcmRange(const SWIFI_CHIP *C, ULONG Address, ULONG Length)
{
    return Address >= C->RamBase && Address - C->RamBase <= C->RamSize &&
        Length <= C->RamSize - (Address - C->RamBase);
}

ULONG SwifiChipReadTcm(SWIFI_CHIP *C, ULONG Address)
{
    if ((Address & 3) || !SwifiChipTcmRange(C, Address, 4))
        return MAXULONG;
    return READ_REGISTER_ULONG((PULONG)(C->Bar2 + Address));
}

VOID SwifiChipWriteTcm(SWIFI_CHIP *C, ULONG Address, ULONG Value)
{
    NT_ASSERT(!(Address & 3) && SwifiChipTcmRange(C, Address, 4));
    if (!(Address & 3) && SwifiChipTcmRange(C, Address, 4))
        WRITE_REGISTER_ULONG((PULONG)(C->Bar2 + Address), Value);
}

static NTSTATUS ReadImage(PCWSTR Path, ULONG Maximum, PUCHAR *Buffer, PULONG Size)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Io;
    FILE_STANDARD_INFORMATION Info;
    LARGE_INTEGER Offset = {0};
    HANDLE Handle;
    NTSTATUS Status;
    *Buffer = NULL;
    *Size = 0;
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&Handle, FILE_READ_DATA | SYNCHRONIZE, &Attributes, &Io,
        NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_SEQUENTIAL_ONLY,
        NULL, 0);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = ZwQueryInformationFile(Handle, &Io, &Info, sizeof(Info), FileStandardInformation);
    if (!NT_SUCCESS(Status))
        goto Done;
    if (Info.EndOfFile.QuadPart <= 0 || Info.EndOfFile.QuadPart > Maximum) {
        Status = STATUS_INVALID_IMAGE_FORMAT;
        goto Done;
    }
    *Size = (ULONG)Info.EndOfFile.QuadPart;
    *Buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, *Size, SWIFI_POOL_TAG);
    if (!*Buffer) { Status = STATUS_INSUFFICIENT_RESOURCES; goto Done; }
    Status = ZwReadFile(Handle, NULL, NULL, NULL, &Io, *Buffer, *Size, &Offset, NULL);
    if (NT_SUCCESS(Status) && Io.Information != *Size)
        Status = STATUS_END_OF_FILE;
Done:
    ZwClose(Handle);
    if (!NT_SUCCESS(Status)) {
        if (*Buffer)
            ExFreePoolWithTag(*Buffer, SWIFI_POOL_TAG);
        *Buffer = NULL;
        *Size = 0;
    }
    return Status;
}

static VOID WriteTcmImage(SWIFI_CHIP *C, ULONG Address, const UCHAR *Data, ULONG Size)
{
    ULONG Offset;
    for (Offset = 0; Offset + 4 <= Size; Offset += 4)
        SwifiChipWriteTcm(C, Address + Offset, ReadLe32(Data + Offset));
    if (Offset != Size) {
        UCHAR Tail[4] = {0};
        RtlCopyMemory(Tail, Data + Offset, Size - Offset);
        SwifiChipWriteTcm(C, Address + Offset, ReadLe32(Tail));
    }
}

NTSTATUS SwifiChipLoadFirmware(SWIFI_CHIP *C, PCWSTR FirmwarePath, PCWSTR NvramPath)
{
    PUCHAR Firmware = NULL, Nvram = NULL, Packed = NULL;
    ULONG FirmwareLength, NvramLength, ResetVector, Marker, Pointer, Attempt;
    uint32_t RamSize, PackedLength;
    NTSTATUS Status;
    if (!C->Bar0 || !C->Bar2 ||
        (C->Stage != SwifiChipDiscovered && C->Stage != SwifiChipStopped) || C->RuntimePinned)
        return STATUS_INVALID_DEVICE_STATE;
    Status = ReadImage(FirmwarePath, C->RamCapacity, &Firmware, &FirmwareLength);
    if (!NT_SUCCESS(Status))
        goto Done;
    C->Stage = SwifiChipFirmwareRead;
    if (!PexFirmwareRamSize(Firmware, FirmwareLength, C->RamCapacity, &RamSize)) {
        Status = STATUS_INVALID_IMAGE_FORMAT;
        goto Done;
    }
    Status = ReadImage(NvramPath, 128 * 1024, &Nvram, &NvramLength);
    if (!NT_SUCCESS(Status))
        goto Done;
    C->Stage = SwifiChipNvramRead;
    if (!PexNvramBuild(Nvram, NvramLength, &Packed, &PackedLength) ||
        PackedLength > RamSize || ((FirmwareLength + 3u) & ~3u) > RamSize - PackedLength) {
        Status = STATUS_INVALID_IMAGE_FORMAT;
        goto Done;
    }
    C->RamSize = RamSize;
    C->Stage = SwifiChipUploading;
    Status = STATUS_DEVICE_PROTOCOL_ERROR;
    if (!CoreReset(C, C->ArmWrapper, 0x20, 0x20, 0x20))
        goto Done;
    ResetVector = ReadLe32(Firmware);
    WriteTcmImage(C, C->RamBase, Firmware, FirmwareLength);
    if (SwifiChipReadTcm(C, C->RamBase) != ResetVector ||
        SwifiChipReadTcm(C, C->RamBase + (FirmwareLength & ~3u) - 4) !=
            ReadLe32(Firmware + (FirmwareLength & ~3u) - 4))
        goto Done;
    WriteTcmImage(C, C->RamBase + C->RamSize - PackedLength, Packed, PackedLength);
    Marker = ReadLe32(Packed + PackedLength - 4);
    if (SwifiChipReadTcm(C, C->RamBase + C->RamSize - 4) != Marker)
        goto Done;
    /* CR4's reset vector is at BAR2 offset zero, outside its RAM-base range. */
    WRITE_REGISTER_ULONG((PULONG)C->Bar2, ResetVector);
    C->Stage = SwifiChipStarting;
    if (!CoreReset(C, C->ArmWrapper, 0x20, 0, 0))
        goto Done;
    Pointer = Marker;
    for (Attempt = 0; Attempt < 100 && Pointer == Marker; ++Attempt) {
        DelayMs(50);
        Pointer = SwifiChipReadTcm(C, C->RamBase + C->RamSize - 4);
    }
    if (Pointer == Marker) { Status = STATUS_IO_TIMEOUT; goto Done; }
    if ((Pointer & 3) || !SwifiChipTcmRange(C, Pointer, 80))
        goto Done;
    C->SharedAddress = Pointer;
    C->SharedFlags = SwifiChipReadTcm(C, Pointer);
    C->RxDataOffset = SwifiChipReadTcm(C, Pointer + 36);
    C->MaxRxBuffers = (USHORT)(SwifiChipReadTcm(C, Pointer + 32) >> 16);
    if (!C->MaxRxBuffers)
        C->MaxRxBuffers = 255; /* brcmfmac's v5 default. */
    C->RingInfo = SwifiChipReadTcm(C, Pointer + 48);
    if ((C->SharedFlags & 0xff) != 5) { Status = STATUS_NOT_SUPPORTED; goto Done; }
    if ((C->RingInfo & 3) || !SwifiChipTcmRange(C, C->RingInfo, 56))
        goto Done;
    if (!SelectBackplane(C, C->PcieBase))
        goto Done;
    C->RuntimePinned = TRUE;
    WRITE_REGISTER_ULONG((PULONG)(C->Bar0 + 0x4c), 0);
    C->Stage = SwifiChipFirmwareReady;
    Status = STATUS_SUCCESS;
Done:
    if (Firmware) ExFreePoolWithTag(Firmware, SWIFI_POOL_TAG);
    if (Nvram) ExFreePoolWithTag(Nvram, SWIFI_POOL_TAG);
    if (Packed) ExFreePoolWithTag(Packed, SWIFI_POOL_TAG);
    if (!NT_SUCCESS(Status) && C->Stage >= SwifiChipUploading) {
        /* Initialization owns no host DMA yet. Leave a failed start halted. */
        SWIFI_CHIP_STAGE FailedStage = C->Stage;
        (VOID)SwifiChipStopAfterQuiesce(C);
        C->Stage = FailedStage;
    }
    return Status;
}

PUCHAR SwifiChipRuntimeRegisters(SWIFI_CHIP *C)
{
    return C->RuntimePinned ? C->Bar0 : NULL;
}

NTSTATUS SwifiChipSetBusMaster(SWIFI_CHIP *C, BOOLEAN Enable)
{
    ULONG Command, Readback;
    if (Enable && (!C->RuntimePinned || C->Stage != SwifiChipFirmwareReady))
        return STATUS_INVALID_DEVICE_STATE;
    Command = SwifiConfigRead(C->Bus, 4);
    if (Command == MAXULONG)
        return STATUS_DEVICE_DOES_NOT_EXIST;
    Command &= 0xffff;
    Command = Enable ? (Command | 6u) : (Command & ~4u);
    SwifiConfigWrite(C->Bus, 4, Command);
    Readback = SwifiConfigRead(C->Bus, 4);
    if (Readback == MAXULONG || (Readback & 6u) != (Command & 6u))
        return STATUS_DEVICE_PROTOCOL_ERROR;
    return STATUS_SUCCESS;
}

NTSTATUS SwifiChipStopAfterQuiesce(SWIFI_CHIP *C)
{
    NTSTATUS Status;
    if (!C->Bar0 || !C->ArmWrapper || !C->D11Wrapper)
        return STATUS_INVALID_DEVICE_STATE;
    Status = SwifiChipSetBusMaster(C, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;
    C->RuntimePinned = FALSE;
    if (!CoreReset(C, C->ArmWrapper, 0x20, 0x20, 0x20))
        return STATUS_DEVICE_PROTOCOL_ERROR;
    if (!CoreDisable(C, C->D11Wrapper, 0x0c, 4))
        return STATUS_DEVICE_PROTOCOL_ERROR;
    DelayMs(20);
    C->SharedAddress = 0;
    C->Stage = SwifiChipStopped;
    return STATUS_SUCCESS;
}

VOID SwifiChipUnmapAfterReset(SWIFI_CHIP *C)
{
    if (C->Bar2) MmUnmapIoSpace(C->Bar2, C->Bar2Length);
    if (C->Bar0) MmUnmapIoSpace(C->Bar0, C->Bar0Length);
    RtlZeroMemory(C, sizeof(*C));
}
