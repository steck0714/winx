/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_WINDOWS_SHARED_H
#define SWIFI_WINDOWS_SHARED_H

#include "chip.h"
#include "transport.h"
#include "../core/msgbuf_control.h"

typedef struct SWIFI_SHARED_BUFFER {
    WDFCOMMONBUFFER Handle;
    PUCHAR Memory;
    PHYSICAL_ADDRESS Logical;
    SIZE_T Length;
} SWIFI_SHARED_BUFFER;

typedef struct SWIFI_SHARED {
    SWIFI_CHIP *Chip;
    WDFDMAENABLER Dma;
    SWIFI_WIN_RING *H2d; /* Common submission rings followed by dynamic flows. */
    SWIFI_WIN_RING D2h[3];
    SWIFI_SHARED_BUFFER Indices;
    SWIFI_SHARED_BUFFER Scratch;
    SWIFI_SHARED_BUFFER Update;
    SWIFI_SHARED_BUFFER Control;
    swifi_control_buffer Input;
    swifi_control_buffer Response;
    swifi_control_buffer Events[SWIFI_EVENT_COUNT];
    ULONG IndexBase[4]; /* Host-memory offsets or TCM addresses. */
    ULONG RingDescriptors;
    ULONG H2dMailbox;
    ULONG D2hMailbox;
    USHORT MaxSubmit;
    UCHAR IndexWidth;
    BOOLEAN HostIndices;
    BOOLEAN Published;
    BOOLEAN Prepared;
    BOOLEAN DmaExposed;
} SWIFI_SHARED;

/* After firmware startup, while bus mastering and interrupts are disabled.
 * Shared must be zero-initialized. Allocates all common rings, indices,
 * scratch/update areas and command/event buffers, then publishes their HAL
 * logical addresses. Does not enable DMA, create an interrupt or start queues.
 * On a post-publication failure, retain the object and reset hardware before
 * deletion. A pre-publication allocation failure is cleaned up internally. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiSharedPrepare(SWIFI_SHARED *Shared, SWIFI_CHIP *Chip, WDFDMAENABLER Dma);

/* Call after transport/callback initialization, before KMDF interrupt enable.
 * DMA exposure is recorded even if the PCI command readback fails. Neither
 * failure nor success here permits freeing buffers without hardware reset. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiSharedEnableDma(SWIFI_SHARED *Shared);

/* DPC side: read/clear device mailbox flags, retaining them for the owner's
 * state machine. 0x2 requests deep-sleep ACK, 0x4 reports wake, 0x1 is D3 ACK
 * and 0x10000000 is firmware halt. Queue responses to a passive
 * worker; no wait or configuration-window write occurs in this function. */
_IRQL_requires_(DISPATCH_LEVEL)
NTSTATUS SwifiSharedReadMailbox(SWIFI_SHARED *Shared, PULONG Flags);

/* Serialized passive worker: wait at most one second for the previous host
 * mailbox word to be consumed, then notify through PCI config SBMBX (twice
 * on PCIe core revision <=13, as in brcmfmac). The owner must drain/cancel
 * this worker before quiescing/resetting the chip. This is not a periodic
 * runtime poll; use it only for an explicit firmware mailbox transaction. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiSharedSendMailbox(SWIFI_SHARED *Shared, ULONG Flags);

/* PASSIVE_LEVEL, serialized with flow lifecycle. Only for a CLOSED flow with
 * no in-flight callbacks. After this allocation, submit the ordinary async
 * flow-create command with this ring's logical address/depth. No descriptor
 * table entry exists for a dynamic flow in the five-ring common table. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiSharedAllocateFlow(SWIFI_SHARED *Shared, USHORT RingId);

/* Matching successful delete completion (or endpoint reset) has stopped this
 * flow's DMA and all packets have been returned. It is now safe to free it. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiSharedDeleteFlowAfterStop(SWIFI_SHARED *Shared, USHORT RingId);

/* Quiesce IRQ/DPC and callers, stop/reset endpoint, cancel command/packet
 * ownership and release retained events BEFORE this call. This routine only
 * frees allocations; it does not stop hardware or own the DMA enabler. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiSharedDeleteAfterReset(SWIFI_SHARED *Shared);

#endif
