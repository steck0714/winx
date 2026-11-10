/* SPDX-License-Identifier: ISC */
/* KMDF runtime transport for BCM4356 msgbuf v5. Register sequencing follows
 * Switchroot brcmfmac/pcie.c; see ../core/NOTICE. No periodic polling timer. */
#include "transport.h"

#define SWIFI_MAILBOX_STATUS 0x48u
#define SWIFI_MAILBOX_MASK   0x4cu
#define SWIFI_DOORBELL      0x140u
#define SWIFI_RING_CAUSES   0x00ff0000u
#define SWIFI_MAIL_CAUSES   0x00000300u
#define SWIFI_IRQ_MASK      (SWIFI_RING_CAUSES | SWIFI_MAIL_CAUSES)
#define SWIFI_DPC_PER_RING  32u

typedef struct SWIFI_INTERRUPT_CONTEXT {
    SWIFI_WIN_TRANSPORT *Transport;
} SWIFI_INTERRUPT_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWIFI_INTERRUPT_CONTEXT, SwifiInterruptContext);

static EVT_WDF_INTERRUPT_ISR SwifiIsr;
static EVT_WDF_DPC SwifiDpc;
static EVT_WDF_INTERRUPT_ENABLE SwifiEnable;
static EVT_WDF_INTERRUPT_DISABLE SwifiDisable;

static ULONG SwifiIndexRead(const SWIFI_WIN_INDEX *Index)
{
    if (Index->Mmio) {
        /* TCM reserves four bytes per index but brcmfmac updates only the
         * low 16 bits. Do not overwrite adjacent/reserved firmware bits. */
        return READ_REGISTER_USHORT((PUSHORT)Index->Address);
    }
    /* brcmfmac uses a u16 counter even in a four-byte DMA-index slot. */
    return *(volatile USHORT *)Index->Address;
}

static uint32_t SwifiReadPeer(PVOID Context)
{
    SWIFI_WIN_RING *Ring = Context;
    return SwifiIndexRead(&Ring->Peer);
}

static VOID SwifiWriteHost(PVOID Context, uint16_t Value)
{
    SWIFI_WIN_RING *Ring = Context;
    const SWIFI_WIN_INDEX *Index = &Ring->Host;
    if (Index->Mmio)
        WRITE_REGISTER_USHORT((PUSHORT)Index->Address, Value);
    else
        *(volatile USHORT *)Index->Address = Value;
    KeMemoryBarrier();
}

static VOID SwifiBarrier(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    KeMemoryBarrier();
}

NTSTATUS SwifiWinCreateDma(WDFDEVICE Device, WDFDMAENABLER *Dma)
{
    WDF_DMA_ENABLER_CONFIG Config;
    WdfDeviceSetAlignmentRequirement(Device, 63);
    WDF_DMA_ENABLER_CONFIG_INIT(&Config, WdfDmaProfileScatterGatherDuplex,
        SWIFI_PACKET_SIZE);
    /* Use HAL's logical addresses/IOMMU mapping, not MmGetPhysicalAddress
     * or the UEFI probe's AFI-ASID bypass. The bus must expose real DMA
     * capabilities and noncoherent ARM64 (_CCA=0) resource ownership. */
    Config.WdmDmaVersionOverride = 3;
    return WdfDmaEnablerCreate(Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, Dma);
}

static BOOLEAN SwifiValidIndex(SWIFI_WIN_INDEX Index)
{
    return Index.Address && (Index.Width == 2 || Index.Width == 4) &&
        !((ULONG_PTR)Index.Address & (Index.Width - 1));
}

NTSTATUS SwifiWinCreateRing(WDFDMAENABLER Dma, SWIFI_WIN_RING *Ring,
    SWIFI_WIN_INDEX Host, SWIFI_WIN_INDEX Peer, USHORT Depth,
    USHORT ItemSize, BOOLEAN HostProduces, UCHAR CompletionPrefix)
{
    static const swifi_ring_ops Ops = {SwifiReadPeer, SwifiWriteHost, SwifiBarrier};
    NTSTATUS Status;
    SIZE_T Length = (SIZE_T)Depth * ItemSize;
    PVOID Memory;
    WDF_COMMON_BUFFER_CONFIG Config;
    if (!SwifiValidIndex(Host) || !SwifiValidIndex(Peer) || Depth < 2 ||
        ItemSize < 8 || ItemSize > SWIFI_TX_ITEM_SIZE ||
        CompletionPrefix > ItemSize - 8 || (HostProduces && CompletionPrefix))
        return STATUS_INVALID_PARAMETER;
    if (SwifiIndexRead(&Host) || SwifiIndexRead(&Peer))
        return STATUS_DEVICE_PROTOCOL_ERROR;
    RtlZeroMemory(Ring, sizeof(*Ring));
    Ring->Host = Host;
    Ring->Peer = Peer;
    WDF_COMMON_BUFFER_CONFIG_INIT(&Config, 63);
    Status = WdfCommonBufferCreateWithConfig(Dma, Length, &Config,
        WDF_NO_OBJECT_ATTRIBUTES, &Ring->Buffer);
    if (!NT_SUCCESS(Status))
        return Status;
    Memory = WdfCommonBufferGetAlignedVirtualAddress(Ring->Buffer);
    Ring->LogicalAddress = WdfCommonBufferGetAlignedLogicalAddress(Ring->Buffer);
    if (Ring->LogicalAddress.QuadPart < 0 ||
        (ULONGLONG)Ring->LogicalAddress.QuadPart > MAXULONG ||
        Length - 1 > MAXULONG - (ULONGLONG)Ring->LogicalAddress.QuadPart) {
        SwifiWinDeleteRingAfterReset(Ring); /* Not published yet. */
        return STATUS_CONFLICTING_ADDRESSES;
    }
    RtlZeroMemory(Memory, Length);
    if (swifi_ring_init(&Ring->Core, &Ops, Ring, Memory, Length,
        Depth, ItemSize, HostProduces, CompletionPrefix) != SWIFI_OK) {
        SwifiWinDeleteRingAfterReset(Ring);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return STATUS_SUCCESS;
}

VOID SwifiWinDeleteRingAfterReset(SWIFI_WIN_RING *Ring)
{
    if (Ring->Buffer)
        WdfObjectDelete(Ring->Buffer);
    RtlZeroMemory(Ring, sizeof(*Ring));
}

NTSTATUS SwifiWinCreateTransport(WDFDEVICE Device, SWIFI_WIN_TRANSPORT *T,
    PKSPIN_LOCK DataLock, SWIFI_PCIE_RESOURCES *Bus, const SWIFI_WIN_CALLBACKS *Callbacks,
    PVOID Context, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    WDF_INTERRUPT_CONFIG Config;
    WDF_DPC_CONFIG DpcConfig;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR RawInterrupt = NULL, TranslatedInterrupt = NULL;
    ULONG Count = WdfCmResourceListGetCount(Translated);
    if (!DataLock || !Bus || !Callbacks || !Callbacks->Dispatch || !Callbacks->Mailbox ||
        !Callbacks->Notify || !Callbacks->ScheduleRecovery) return STATUS_INVALID_PARAMETER;
    if (WdfCmResourceListGetCount(Raw) != Count) return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (ULONG I = 0; I < Count; ++I) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR R = WdfCmResourceListGetDescriptor(Raw, I);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR X = WdfCmResourceListGetDescriptor(Translated, I);
        if (!R || !X || R->Type != X->Type) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (X->Type != CmResourceTypeInterrupt) continue;
        if (RawInterrupt || ((X->Flags | R->Flags) &
            (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED)))
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        /* KMDF locates its own resource object from these exact pointers. */
        RawInterrupt = R;
        TranslatedInterrupt = X;
    }
    if (!RawInterrupt) return STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlZeroMemory(T, sizeof(*T));
    T->DataLock = DataLock;
    T->Bus = Bus;
    T->Callbacks = *Callbacks;
    T->Context = Context;
    WDF_DPC_CONFIG_INIT(&DpcConfig, SwifiDpc);
    DpcConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWIFI_INTERRUPT_CONTEXT);
    Attributes.ParentObject = Device;
    NTSTATUS Status = WdfDpcCreate(&DpcConfig, &Attributes, &T->Dpc);
    if (!NT_SUCCESS(Status))
        return Status;
    SwifiInterruptContext(T->Dpc)->Transport = T;
    WDF_INTERRUPT_CONFIG_INIT(&Config, SwifiIsr, NULL);
    Config.EvtInterruptEnable = SwifiEnable;
    Config.EvtInterruptDisable = SwifiDisable;
    Config.AutomaticSerialization = FALSE;
    Config.InterruptRaw = RawInterrupt;
    Config.InterruptTranslated = TranslatedInterrupt;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWIFI_INTERRUPT_CONTEXT);
    Status = WdfInterruptCreate(Device, &Config, &Attributes, &T->Interrupt);
    if (NT_SUCCESS(Status)) {
        SwifiInterruptContext(T->Interrupt)->Transport = T;
    } else {
        WdfObjectDelete(T->Dpc);
        T->Dpc = NULL;
    }
    return Status;
}

static swifi_result SwifiSubmitBatch(PVOID Context, uint16_t Ring,
    const uint8_t *Items, size_t Length, uint16_t Count)
{
    SWIFI_WIN_TRANSPORT *T = Context;
    swifi_result Result;
    if (!T->Ready || T->Faulted)
        return SWIFI_STOPPED;
    if (Ring >= T->H2dCount || !T->H2d[Ring].Buffer)
        return SWIFI_INVALID;
    Result = swifi_ring_submit_batch(&T->H2d[Ring].Core, Items, Length, Count);
    if (Result == SWIFI_OK) {
        /* Publication already transferred ownership. Never report RETRY or
         * return the packet on a subsequent link/doorbell failure. */
        WRITE_REGISTER_ULONG((PULONG)(T->Registers + SWIFI_DOORBELL), 1);
    }
    return Result;
}

swifi_result SwifiWinSubmit(PVOID Context, uint16_t Ring,
    const uint8_t *Item, size_t Length)
{
    return SwifiSubmitBatch(Context, Ring, Item, Length, 1);
}

swifi_result SwifiWinSubmitControl(PVOID Context, const uint8_t *Items, uint16_t Count)
{
    return SwifiSubmitBatch(Context, SWIFI_CONTROL_RING, Items,
        (size_t)Count * SWIFI_CONTROL_ITEM_SIZE, Count);
}

static BOOLEAN ServiceInterrupt(SWIFI_WIN_TRANSPORT *T)
{
    ULONG Status, HostCode, Signature;
    if (!T->Armed)
        return FALSE;
    HostCode = SwifiAfiAcknowledge(T->Bus, &Signature);
    if (HostCode) {
        T->LastAfiCode = HostCode;
        T->LastAfiSignature = Signature;
    }
    if (HostCode && HostCode != 6 && HostCode != 10) {
        /* Do not access a potentially unreachable endpoint after an AFI
         * abort/timeout. Quiesce the host source and recover passively. */
        SwifiAfiInterruptMask(T->Bus, FALSE);
        InterlockedExchange(&T->HostFault, 1);
        InterlockedExchange(&T->Faulted, 1);
        WdfDpcEnqueue(T->Dpc);
        return TRUE;
    }
    Status = READ_REGISTER_ULONG((PULONG)(T->Registers + SWIFI_MAILBOX_STATUS));
    if (!(Status & SWIFI_IRQ_MASK))
        return HostCode != 0;
    SwifiAfiInterruptMask(T->Bus, FALSE);
    if (Status == MAXULONG) {
        InterlockedExchange(&T->HostFault, 1); /* Endpoint MMIO is unreachable. */
        InterlockedExchange(&T->Faulted, 1);
    } else {
        WRITE_REGISTER_ULONG((PULONG)(T->Registers + SWIFI_MAILBOX_MASK), 0);
        Status &= SWIFI_IRQ_MASK;
        WRITE_REGISTER_ULONG((PULONG)(T->Registers + SWIFI_MAILBOX_STATUS), Status);
        InterlockedOr(&T->Pending, (LONG)Status);
    }
    WdfDpcEnqueue(T->Dpc);
    return TRUE;
}

static BOOLEAN SwifiIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    SWIFI_WIN_TRANSPORT *T = SwifiInterruptContext(Interrupt)->Transport;
    UNREFERENCED_PARAMETER(MessageId);
    if (!ServiceInterrupt(T)) return FALSE;
    T->SawInterrupt = 1;
    return TRUE;
}

static VOID SwifiDpc(WDFDPC Dpc)
{
    SWIFI_WIN_TRANSPORT *T = SwifiInterruptContext(Dpc)->Transport;
    WDFINTERRUPT Interrupt = T->Interrupt;
    swifi_result Result = SWIFI_OK;
    ULONG Pending;
    UCHAR Index, Step;
    BOOLEAN More = FALSE;

    KeAcquireSpinLockAtDpcLevel(T->DataLock);
    if (!T->Ready || !T->Armed) {
        KeReleaseSpinLockFromDpcLevel(T->DataLock);
        return;
    }
    Pending = (ULONG)InterlockedExchange(&T->Pending, 0);
    if (T->Faulted)
        Result = SWIFI_PROTOCOL_ERROR;
    if (Result == SWIFI_OK && (Pending & SWIFI_MAIL_CAUSES))
        Result = T->Callbacks.Mailbox(T->Context);
    for (Step = 0; Result == SWIFI_OK && Step < 3; ++Step) {
        uint16_t Processed = 0;
        int RingMore;
        Index = (UCHAR)((T->NextD2h + Step) % 3);
        Result = swifi_ring_drain(&T->D2h[Index].Core, SWIFI_DPC_PER_RING,
            T->Callbacks.Dispatch, T->Context, &Processed, &RingMore);
        if (Processed) T->SawRing = 1;
        if (RingMore)
            More = TRUE;
    }
    T->NextD2h = (UCHAR)((T->NextD2h + 1) % 3);
    if (Result != SWIFI_OK) {
        InterlockedExchange(&T->Faulted, 1);
        InterlockedExchange(&T->Ready, 0);
    }
    KeReleaseSpinLockFromDpcLevel(T->DataLock);

    /* Upper-stack completions may immediately submit another packet. */
    T->Callbacks.Notify(T->Context);

    WdfInterruptAcquireLock(Interrupt);
    if (Result != SWIFI_OK) {
        InterlockedExchange(&T->Ready, 0);
        InterlockedExchange(&T->Armed, 0);
        SwifiAfiInterruptMask(T->Bus, FALSE);
        if (!T->HostFault)
            WRITE_REGISTER_ULONG((PULONG)(T->Registers + SWIFI_MAILBOX_MASK), 0);
    } else if (T->Armed && T->Ready && !T->Faulted) {
        if (More || T->Pending) {
            WdfDpcEnqueue(T->Dpc);
        } else {
            WRITE_REGISTER_ULONG((PULONG)(T->Registers + SWIFI_MAILBOX_MASK), SWIFI_IRQ_MASK);
            SwifiAfiInterruptMask(T->Bus, TRUE);
            /* Close the masked-arrival/rearm race while holding ISR lock. */
            ServiceInterrupt(T);
        }
    }
    WdfInterruptReleaseLock(Interrupt);
    if (Result != SWIFI_OK)
        T->Callbacks.ScheduleRecovery(T->Context, Result);
}

static NTSTATUS SwifiEnable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    SWIFI_WIN_TRANSPORT *T = SwifiInterruptContext(Interrupt)->Transport;
    UNREFERENCED_PARAMETER(Device);
    if (!T->Ready || T->Faulted)
        return STATUS_DEVICE_NOT_READY;
    InterlockedExchange(&T->Armed, 1);
    WRITE_REGISTER_ULONG((PULONG)(T->Registers + SWIFI_MAILBOX_MASK), SWIFI_IRQ_MASK);
    SwifiAfiInterruptMask(T->Bus, TRUE);
    ServiceInterrupt(T);
    return STATUS_SUCCESS;
}

static NTSTATUS SwifiDisable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    SWIFI_WIN_TRANSPORT *T = SwifiInterruptContext(Interrupt)->Transport;
    UNREFERENCED_PARAMETER(Device);
    if (InterlockedExchange(&T->Armed, 0)) {
        SwifiAfiInterruptMask(T->Bus, FALSE);
        if (!T->HostFault)
            WRITE_REGISTER_ULONG((PULONG)(T->Registers + SWIFI_MAILBOX_MASK), 0);
    }
    return STATUS_SUCCESS;
}

VOID SwifiWinQuiesceInterrupts(SWIFI_WIN_TRANSPORT *T)
{
    KIRQL OldIrql;
    InterlockedExchange(&T->Ready, 0);
    /* PnP serializes this call with interrupt enable/disable. A failed
     * Prepare or a second Release may have no connected WDM interrupt. */
    if (T->Armed) {
        WdfInterruptAcquireLock(T->Interrupt);
        if (InterlockedExchange(&T->Armed, 0)) {
            SwifiAfiInterruptMask(T->Bus, FALSE);
            if (!T->HostFault)
                WRITE_REGISTER_ULONG((PULONG)(T->Registers + SWIFI_MAILBOX_MASK), 0);
        }
        WdfInterruptReleaseLock(T->Interrupt);
    }
    /* A TX callback may have passed Ready just before it was cleared. Join
     * that in-flight submission before the owner resets/frees DMA. Never
     * hold the ISR lock while waiting for the lower-IRQL protocol lock. */
    KeAcquireSpinLock(T->DataLock, &OldIrql);
    KeReleaseSpinLock(T->DataLock, OldIrql);
    WdfDpcCancel(T->Dpc, TRUE);
}

VOID SwifiWinDeleteTransportAfterReset(SWIFI_WIN_TRANSPORT *T)
{
    if (T->Interrupt) {
        /* Quiesce ran before reset; the BAR may no longer be accessible.
         * Deleting an already-disarmed interrupt must not touch hardware. */
        NT_ASSERT(!T->Ready && !T->Armed);
        WdfObjectDelete(T->Interrupt);
    }
    if (T->Dpc) {
        WdfDpcCancel(T->Dpc, TRUE);
        WdfObjectDelete(T->Dpc);
    }
    RtlZeroMemory(T, sizeof(*T));
}
