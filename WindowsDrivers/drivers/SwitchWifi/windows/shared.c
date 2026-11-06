/* SPDX-License-Identifier: ISC */
/* Firmware shared-v5 setup from Switchroot brcmfmac/pcie.c; see core/NOTICE. */
#include "shared.h"

#define SWIFI_SHARED_TAG 'iFwS'
#define SHARED_HOST_INDICES 0x00010000u
#define SHARED_TWO_BYTE_INDICES 0x00100000u
#define SHARED_HOST_READY 0x10000000u

NTSTATUS SwifiSharedCreateBuffer(WDFDMAENABLER Dma, SIZE_T Length, SWIFI_SHARED_BUFFER *Buffer)
{
    WDF_COMMON_BUFFER_CONFIG Config;
    NTSTATUS Status;
    if (!Buffer || Buffer->Handle || !Length) return STATUS_INVALID_PARAMETER;
    WDF_COMMON_BUFFER_CONFIG_INIT(&Config, 63);
    Status = WdfCommonBufferCreateWithConfig(Dma, Length, &Config,
        WDF_NO_OBJECT_ATTRIBUTES, &Buffer->Handle);
    if (!NT_SUCCESS(Status))
        return Status;
    Buffer->Memory = WdfCommonBufferGetAlignedVirtualAddress(Buffer->Handle);
    Buffer->Logical = WdfCommonBufferGetAlignedLogicalAddress(Buffer->Handle);
    Buffer->Length = Length;
    if (Buffer->Logical.QuadPart < 0 ||
        (ULONGLONG)Buffer->Logical.QuadPart > MAXULONG ||
        Length - 1 > MAXULONG - (ULONGLONG)Buffer->Logical.QuadPart) {
        SwifiSharedDeleteBuffer(Buffer);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    RtlZeroMemory(Buffer->Memory, Length);
    return STATUS_SUCCESS;
}

VOID SwifiSharedDeleteBuffer(SWIFI_SHARED_BUFFER *Buffer)
{
    if (Buffer->Handle)
        WdfObjectDelete(Buffer->Handle);
    RtlZeroMemory(Buffer, sizeof(*Buffer));
}

static SWIFI_WIN_INDEX GetIndex(SWIFI_SHARED *S, ULONG Kind, USHORT Ring)
{
    SWIFI_WIN_INDEX Index;
    ULONG Offset = S->IndexBase[Kind] + Ring * S->IndexWidth;
    Index.Width = S->IndexWidth;
    Index.Mmio = !S->HostIndices;
    Index.Address = S->HostIndices ? S->Indices.Memory + Offset : S->Chip->Bar2 + Offset;
    return Index;
}

static VOID ZeroFlowIndex(SWIFI_WIN_INDEX Index)
{
    /* A deleted/never-created flow has no firmware writer. Keep reserved TCM
     * high bits intact, matching the transport's low-16-bit index accesses. */
    if (Index.Mmio)
        WRITE_REGISTER_USHORT((PUSHORT)Index.Address, 0);
    else
        *(volatile USHORT *)Index.Address = 0;
}

static NTSTATUS PublishAddress(SWIFI_CHIP *Chip, ULONG Target, ULONGLONG Logical)
{
    SwifiChipWriteTcm(Chip, Target, (ULONG)Logical);
    SwifiChipWriteTcm(Chip, Target + 4, (ULONG)(Logical >> 32));
    return SwifiChipReadTcm(Chip, Target) == (ULONG)Logical &&
        SwifiChipReadTcm(Chip, Target + 4) == (ULONG)(Logical >> 32) ?
        STATUS_SUCCESS : STATUS_DEVICE_PROTOCOL_ERROR;
}

NTSTATUS SwifiSharedPrepare(SWIFI_SHARED *S, SWIFI_CHIP *C, WDFDMAENABLER Dma)
{
    static const USHORT Depth[5] = {64, 512, 64, 1024, 512};
    static const USHORT Size[5] = {40, 32, 24, 16, 32};
    NTSTATUS Status;
    ULONG I, Count, IndexBytes, Descriptor;
    SWIFI_WIN_RING *Ring;
    if (!S || !C || !Dma || S->H2d || S->Published ||
        C->Stage != SwifiChipFirmwareReady || !C->RuntimePinned)
        return STATUS_INVALID_DEVICE_STATE;
    /* The fixed v5 TX completion is 16 bytes, all of which carry fields.
     * Do not silently truncate it if a different firmware adds a prefix.
     * Such firmware needs its actual descriptor-size negotiation first. */
    if (C->RxDataOffset != 0)
        return STATUS_NOT_SUPPORTED;
    Status = SwifiChipSetBusMaster(C, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(S, sizeof(*S));
    S->Chip = C;
    S->Dma = Dma;
    S->MaxSubmit = (USHORT)SwifiChipReadTcm(C, C->RingInfo + 52);
    S->RingDescriptors = SwifiChipReadTcm(C, C->RingInfo);
    S->H2dMailbox = SwifiChipReadTcm(C, C->SharedAddress + 40);
    S->D2hMailbox = SwifiChipReadTcm(C, C->SharedAddress + 44);
    if (S->MaxSubmit < 3 || (S->RingDescriptors & 3) ||
        !SwifiChipTcmRange(C, S->RingDescriptors, 5 * 16) ||
        (S->H2dMailbox & 3) || !SwifiChipTcmRange(C, S->H2dMailbox, 4) ||
        (S->D2hMailbox & 3) || !SwifiChipTcmRange(C, S->D2hMailbox, 4)) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
        goto Failed;
    }
    S->HostIndices = (C->SharedFlags & SHARED_HOST_INDICES) != 0;
    S->IndexWidth = S->HostIndices && (C->SharedFlags & SHARED_TWO_BYTE_INDICES) ? 2 : 4;
    if (S->HostIndices) {
        S->IndexBase[0] = 0;
        S->IndexBase[1] = S->MaxSubmit * S->IndexWidth;
        S->IndexBase[2] = 2 * S->MaxSubmit * S->IndexWidth;
        S->IndexBase[3] = (2 * S->MaxSubmit + 3) * S->IndexWidth;
        IndexBytes = (2 * S->MaxSubmit + 6) * S->IndexWidth;
        Status = SwifiSharedCreateBuffer(Dma, IndexBytes, &S->Indices);
        if (!NT_SUCCESS(Status))
            goto Failed;
    } else {
        for (I = 0; I < 4; ++I) {
            S->IndexBase[I] = SwifiChipReadTcm(C, C->RingInfo + 4 + I * 4);
            Count = I < 2 ? S->MaxSubmit : 3;
            if ((S->IndexBase[I] & 3) || !SwifiChipTcmRange(C, S->IndexBase[I], Count * 4)) {
                Status = STATUS_DEVICE_PROTOCOL_ERROR;
                goto Failed;
            }
        }
    }
    S->H2d = ExAllocatePool2(POOL_FLAG_NON_PAGED,
        (SIZE_T)S->MaxSubmit * sizeof(*S->H2d), SWIFI_SHARED_TAG);
    if (!S->H2d) { Status = STATUS_INSUFFICIENT_RESOURCES; goto Failed; }
    for (I = 0; I < 5; ++I) {
        BOOLEAN Produces = I < 2;
        USHORT Id = (USHORT)(Produces ? I : I - 2);
        SWIFI_WIN_INDEX Host = GetIndex(S, Produces ? 0 : 3, Id);
        SWIFI_WIN_INDEX Peer = GetIndex(S, Produces ? 1 : 2, Id);
        Ring = Produces ? &S->H2d[Id] : &S->D2h[Id];
        Status = SwifiWinCreateRing(Dma, Ring, Host, Peer, Depth[I], Size[I], Produces, 0);
        if (!NT_SUCCESS(Status))
            goto Failed;
    }
    Status = SwifiSharedCreateBuffer(Dma, 8, &S->Scratch);
    if (!NT_SUCCESS(Status)) goto Failed;
    Status = SwifiSharedCreateBuffer(Dma, 1024, &S->Update);
    if (!NT_SUCCESS(Status)) goto Failed;
    Status = SwifiSharedCreateBuffer(Dma, (2 + SWIFI_EVENT_COUNT) * SWIFI_CONTROL_BUFFER_SIZE, &S->Control);
    if (!NT_SUCCESS(Status)) goto Failed;
    S->Input.memory = S->Control.Memory;
    S->Input.dma = (ULONGLONG)S->Control.Logical.QuadPart;
    S->Response.memory = S->Input.memory + SWIFI_CONTROL_BUFFER_SIZE;
    S->Response.dma = S->Input.dma + SWIFI_CONTROL_BUFFER_SIZE;
    for (I = 0; I < SWIFI_EVENT_COUNT; ++I) {
        S->Events[I].memory = S->Control.Memory + (I + 2) * SWIFI_CONTROL_BUFFER_SIZE;
        S->Events[I].dma = (ULONGLONG)S->Control.Logical.QuadPart + (I + 2) * SWIFI_CONTROL_BUFFER_SIZE;
    }

    /* Every allocation and initial common-ring index is valid before the
     * first device-visible pointer. From here, reset before cleanup even
     * when a readback fails; callers must not enable DMA on partial state. */
    KeMemoryBarrier();
    S->Published = TRUE;
    if (S->HostIndices) {
        for (I = 0; I < 4; ++I) {
            Status = PublishAddress(C, C->RingInfo + 20 + I * 8,
                (ULONGLONG)S->Indices.Logical.QuadPart + S->IndexBase[I]);
            if (!NT_SUCCESS(Status)) goto Failed;
        }
    }
    for (I = 0; I < 5; ++I) {
        ULONG Dimensions = Depth[I] | (ULONG)Size[I] << 16;
        Ring = I < 2 ? &S->H2d[I] : &S->D2h[I - 2];
        Descriptor = S->RingDescriptors + I * 16;
        SwifiChipWriteTcm(C, Descriptor + 4, Dimensions);
        Status = PublishAddress(C, Descriptor + 8, (ULONGLONG)Ring->LogicalAddress.QuadPart);
        if (!NT_SUCCESS(Status)) goto Failed;
        if (SwifiChipReadTcm(C, Descriptor + 4) != Dimensions) {
            Status = STATUS_DEVICE_PROTOCOL_ERROR;
            goto Failed;
        }
    }
    SwifiChipWriteTcm(C, C->SharedAddress + 52, 8);
    Status = PublishAddress(C, C->SharedAddress + 56, (ULONGLONG)S->Scratch.Logical.QuadPart);
    if (!NT_SUCCESS(Status)) goto Failed;
    SwifiChipWriteTcm(C, C->SharedAddress + 64, 1024);
    Status = PublishAddress(C, C->SharedAddress + 68, (ULONGLONG)S->Update.Logical.QuadPart);
    if (!NT_SUCCESS(Status)) goto Failed;
    if (SwifiChipReadTcm(C, C->SharedAddress + 52) != 8 ||
        SwifiChipReadTcm(C, C->SharedAddress + 64) != 1024) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
        goto Failed;
    }
    KeMemoryBarrier();
    S->Prepared = TRUE;
    return STATUS_SUCCESS;
Failed:
    if (!S->Published)
        SwifiSharedDeleteAfterReset(S); /* No pointer was exposed to firmware. */
    return Status;
}

NTSTATUS SwifiSharedEnableDma(SWIFI_SHARED *S)
{
    NTSTATUS Status;
    if (!S->Prepared || !S->Published || S->DmaExposed)
        return STATUS_INVALID_DEVICE_STATE;
    KeMemoryBarrier();
    S->DmaExposed = TRUE;
    Status = SwifiChipSetBusMaster(S->Chip, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;
    if (S->Chip->SharedFlags & SHARED_HOST_READY)
        WRITE_REGISTER_ULONG((PULONG)(SwifiChipRuntimeRegisters(S->Chip) + 0x144), 1);
    return STATUS_SUCCESS;
}

NTSTATUS SwifiSharedReadMailbox(SWIFI_SHARED *S, PULONG Flags)
{
    *Flags = 0;
    if (!S->Prepared || !S->DmaExposed || !S->Chip->RuntimePinned)
        return STATUS_DEVICE_NOT_READY;
    *Flags = SwifiChipReadTcm(S->Chip, S->D2hMailbox);
    if (*Flags == MAXULONG)
        return STATUS_DEVICE_DOES_NOT_EXIST;
    if (*Flags) {
        SwifiChipWriteTcm(S->Chip, S->D2hMailbox, 0);
        KeMemoryBarrier();
    }
    return (*Flags & 0x10000000u) ? STATUS_DEVICE_HARDWARE_ERROR : STATUS_SUCCESS;
}

NTSTATUS SwifiSharedSendMailbox(SWIFI_SHARED *S, ULONG Flags)
{
    ULONG Attempt, Current;
    LARGE_INTEGER Delay;
    if (!Flags)
        return STATUS_INVALID_PARAMETER;
    if (!S->Prepared || !S->DmaExposed || !S->Chip->RuntimePinned)
        return STATUS_DEVICE_NOT_READY;
    Delay.QuadPart = -10 * 10000; /* A passive 10 ms sleep, not a spin wait. */
    for (Attempt = 0; ; ++Attempt) {
        Current = SwifiChipReadTcm(S->Chip, S->H2dMailbox);
        if (!Current)
            break;
        if (Current == MAXULONG)
            return STATUS_DEVICE_DOES_NOT_EXIST;
        if (Attempt == 100)
            return STATUS_IO_TIMEOUT;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    SwifiChipWriteTcm(S->Chip, S->H2dMailbox, Flags);
    KeMemoryBarrier();
    SwifiConfigWrite(S->Chip->Bus, 0x98, 1);
    if (S->Chip->PcieRevision <= 13)
        SwifiConfigWrite(S->Chip->Bus, 0x98, 1);
    return STATUS_SUCCESS;
}

NTSTATUS SwifiSharedAllocateFlow(SWIFI_SHARED *S, USHORT RingId)
{
    SWIFI_WIN_INDEX Host, Peer;
    if (!S->Prepared || RingId < 2 || RingId >= S->MaxSubmit || S->H2d[RingId].Buffer)
        return STATUS_INVALID_PARAMETER;
    Host = GetIndex(S, 0, RingId);
    Peer = GetIndex(S, 1, RingId);
    ZeroFlowIndex(Host);
    ZeroFlowIndex(Peer);
    KeMemoryBarrier();
    return SwifiWinCreateRing(S->Dma, &S->H2d[RingId], Host, Peer, 512, 48, TRUE, 0);
}

VOID SwifiSharedRearmClosedFlow(SWIFI_WIN_RING *Ring)
{
    swifi_ring *Core = &Ring->Core;
    RtlZeroMemory(Core->memory, (SIZE_T)Core->depth * Core->item_size);
    Core->host_index = 0;
    ZeroFlowIndex(Ring->Host);
    ZeroFlowIndex(Ring->Peer);
    KeMemoryBarrier();
}

VOID SwifiSharedDeleteFlowAfterStop(SWIFI_SHARED *S, USHORT RingId)
{
    if (S->H2d && RingId >= 2 && RingId < S->MaxSubmit)
        SwifiWinDeleteRingAfterReset(&S->H2d[RingId]);
}

VOID SwifiSharedDeleteAfterReset(SWIFI_SHARED *S)
{
    ULONG I;
    if (S->H2d) {
        for (I = 0; I < S->MaxSubmit; ++I)
            SwifiWinDeleteRingAfterReset(&S->H2d[I]);
        ExFreePoolWithTag(S->H2d, SWIFI_SHARED_TAG);
    }
    for (I = 0; I < 3; ++I)
        SwifiWinDeleteRingAfterReset(&S->D2h[I]);
    SwifiSharedDeleteBuffer(&S->Control);
    SwifiSharedDeleteBuffer(&S->Update);
    SwifiSharedDeleteBuffer(&S->Scratch);
    SwifiSharedDeleteBuffer(&S->Indices);
    RtlZeroMemory(S, sizeof(*S));
}
