/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_WINDOWS_TRANSPORT_H
#define SWIFI_WINDOWS_TRANSPORT_H

#include <ntddk.h>
#include <wdf.h>
#include "../core/msgbuf_ring.h"
#include "resources.h"

typedef struct SWIFI_WIN_INDEX {
    PVOID Address;
    UCHAR Width; /* 2/4-byte stride; the actual v5 counter is always 16-bit. */
    BOOLEAN Mmio;
} SWIFI_WIN_INDEX;

typedef struct SWIFI_WIN_RING {
    swifi_ring Core;
    SWIFI_WIN_INDEX Host;
    SWIFI_WIN_INDEX Peer;
    WDFCOMMONBUFFER Buffer;
    PHYSICAL_ADDRESS LogicalAddress;
} SWIFI_WIN_RING;

typedef struct SWIFI_WIN_TRANSPORT SWIFI_WIN_TRANSPORT;

typedef struct SWIFI_WIN_CALLBACKS {
    /* DISPATCH_LEVEL, under DataLock. Update internal queues only; never
     * call back into NDIS/NetAdapterCx/WiFiCx while holding this lock. */
    swifi_ring_handler Dispatch;
    swifi_result (*Mailbox)(PVOID Context);
    /* DISPATCH_LEVEL, outside DataLock. Return completed packets/commands
     * to the OS and notify queues after the protocol has released its lock. */
    VOID (*Notify)(PVOID Context);
    /* DISPATCH_LEVEL, no locks. Queue passive recovery; do not destroy or
     * unmap the transport from this callback, which runs inside its DPC. */
    VOID (*ScheduleRecovery)(PVOID Context, swifi_result Reason);
} SWIFI_WIN_CALLBACKS;

struct SWIFI_WIN_TRANSPORT {
    WDFINTERRUPT Interrupt;
    WDFDPC Dpc;
    PKSPIN_LOCK DataLock; /* Device-lifetime lock shared with packet queues. */
    /* BAR0 pinned to the PCIe core before enabling interrupts. No window
     * changes until quiescence. Covers offsets 0x48, 0x4c, 0x140 and 0x144. */
    PUCHAR Registers;
    SWIFI_PCIE_RESOURCES *Bus;
    SWIFI_WIN_RING *H2d;
    USHORT H2dCount;
    SWIFI_WIN_RING *D2h; /* Control, TX status, RX completion (three rings). */
    SWIFI_WIN_CALLBACKS Callbacks;
    PVOID Context;
    volatile LONG Ready;
    volatile LONG Armed;
    volatile LONG Pending;
    volatile LONG Faulted;
    volatile LONG HostFault;
    volatile LONG SawInterrupt, SawRing; /* Per D0; software rearm probes are excluded. */
    ULONG LastAfiCode;
    ULONG LastAfiSignature;
    UCHAR NextD2h;
};

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiWinCreateDma(WDFDEVICE Device, WDFDMAENABLER *Dma);

/* Allocate before publishing ring descriptors to firmware. Index memory is
 * supplied by the platform's TCM/host-index mapping and outlives the ring. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiWinCreateRing(WDFDMAENABLER Dma, SWIFI_WIN_RING *Ring,
    SWIFI_WIN_INDEX Host, SWIFI_WIN_INDEX Peer, USHORT Depth,
    USHORT ItemSize, BOOLEAN HostProduces, UCHAR CompletionPrefix);

/* Only after interrupts are disabled/flushed and endpoint DMA is stopped. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiWinDeleteRingAfterReset(SWIFI_WIN_RING *Ring);

/* Register the assigned interrupt during PrepareHardware, before touching
 * the endpoint. Bind registers and rings before D0Entry marks it ready. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiWinCreateTransport(WDFDEVICE Device, SWIFI_WIN_TRANSPORT *Transport,
    PKSPIN_LOCK DataLock, SWIFI_PCIE_RESOURCES *Bus, const SWIFI_WIN_CALLBACKS *Callbacks,
    PVOID Context, WDFCMRESLIST Raw, WDFCMRESLIST Translated);

/* swifi_data_ops.submit implementation; caller holds DataLock. */
_IRQL_requires_(DISPATCH_LEVEL)
swifi_result SwifiWinSubmit(PVOID Context, uint16_t Ring,
    const uint8_t *Item, size_t Length);

/* swifi_control_ops.submit implementation; one publication and doorbell for
 * the whole command batch. Caller holds DataLock, like SwifiWinSubmit. */
_IRQL_requires_(DISPATCH_LEVEL)
swifi_result SwifiWinSubmitControl(PVOID Context, const uint8_t *Items, uint16_t Count);

/* First phase of D0Exit/recovery. Masks interrupts and waits for all DPCs;
 * next reset/quiesce hardware, cancel owned packets, then release buffers. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiWinQuiesceInterrupts(SWIFI_WIN_TRANSPORT *Transport);

/* ReleaseHardware, after quiesce and hardware reset; before unmapping BARs.
 * DMA rings have a separate lifetime and must also be released after reset. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiWinDeleteTransportAfterReset(SWIFI_WIN_TRANSPORT *Transport);

#endif
