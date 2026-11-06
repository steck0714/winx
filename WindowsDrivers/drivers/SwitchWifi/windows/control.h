/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_WINDOWS_CONTROL_H
#define SWIFI_WINDOWS_CONTROL_H

#include "shared.h"

typedef struct SWIFI_CONTROL_CALLBACKS {
    /* PASSIVE_LEVEL, no protocol lock. Data is borrowed only until return.
     * Copy any event/task result that must outlive the callback. */
    VOID (*Event)(PVOID Context, const swifi_event_completion *Event);
    /* <=DISPATCH_LEVEL, no locks. Queue owner recovery; never perform reset
     * inline or wait for this component's worker from inside this callback. */
    VOID (*ScheduleRecovery)(PVOID Context, NTSTATUS Reason);
} SWIFI_CONTROL_CALLBACKS;

typedef struct SWIFI_WIN_CONTROL {
    SWIFI_WIN_TRANSPORT *Transport;
    SWIFI_SHARED *Shared;
    swifi_control Core;
    SWIFI_CONTROL_CALLBACKS Callbacks;
    PVOID Context;
    WDFWAITLOCK CommandLock;
    WDFWORKITEM EventWork;
    WDFWORKITEM MailboxWork;
    KEVENT ReplyReady;
    KEVENT RingProgress;
    KEVENT Stop;
    swifi_command_completion Reply;
    UCHAR ReplyData[SWIFI_CONTROL_BUFFER_SIZE];
    swifi_event_completion Events[SWIFI_EVENT_COUNT];
    UCHAR EventHead;
    UCHAR EventTail;
    UCHAR EventCount;
    BOOLEAN EventQueued;
    BOOLEAN MailboxQueued;
    BOOLEAN Bound;
    ULONG MailboxPending;
    ULONG LastMailbox;
    volatile LONG Running;
    volatile LONG FaultPending;
    volatile LONG FaultReported;
} SWIFI_WIN_CONTROL;

/* Create resident state/work items once during device setup, after the
 * transport's DataLock exists. Parent callbacks must not automatically
 * serialize these workers with callbacks that wait for them. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiControlCreate(WDFDEVICE Device, SWIFI_WIN_CONTROL *Control,
    SWIFI_WIN_TRANSPORT *Transport, const SWIFI_CONTROL_CALLBACKS *Callbacks,
    PVOID Context);

/* Prepare a fresh powered session after SwifiSharedPrepare, before IRQ enable.
 * On resume, quiesce/reset/unbind the old session before replacing buffers. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiControlBind(SWIFI_WIN_CONTROL *Control, SWIFI_SHARED *Shared);

/* D0EntryPostInterruptsEnabled: prime events after transport Ready/Armed.
 * Capability queries may then call Execute from the same passive context. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiControlStart(SWIFI_WIN_CONTROL *Control);

/* Passive command worker API. Serializes one firmware IOCTL, with a two-second
 * deadline after acquiring CommandLock. Input/output may be pageable: only
 * resident staging buffers are touched by the DPC. ReturnedLength is firmware
 * length before truncation; at most OutputLength bytes are copied. Firmware
 * errors return STATUS_UNSUCCESSFUL and retain the signed Broadcom status.
 * A WiFiCx SendCommand callback must queue passive work before calling this. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiControlExecute(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex, ULONG Command,
    const VOID *Input, USHORT InputLength, VOID *Output, USHORT OutputLength,
    PUSHORT ReturnedLength, PSHORT FirmwareStatus);

/* Transport hooks. Dispatch/Mailbox run under Transport.DataLock. Dispatch
 * returns UNHANDLED for packet/flow completions; pass those to the data core.
 * Notify runs outside the lock, before the owner's OS packet notifications. */
_IRQL_requires_(DISPATCH_LEVEL)
swifi_result SwifiControlDispatch(SWIFI_WIN_CONTROL *Control, const uint8_t *Item, size_t Length);
_IRQL_requires_(DISPATCH_LEVEL)
swifi_result SwifiControlMailbox(SWIFI_WIN_CONTROL *Control);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID SwifiControlNotify(SWIFI_WIN_CONTROL *Control);

/* Stop order: RequestStop, SwifiWinQuiesceInterrupts, Quiesce, chip reset,
 * StoppedAfterReset. Keep shared DMA mapped throughout. Quiesce joins command
 * waiters/workers but does not claim that firmware has released DMA. */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID SwifiControlRequestStop(SWIFI_WIN_CONTROL *Control);
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiControlQuiesce(SWIFI_WIN_CONTROL *Control);
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiControlStoppedAfterReset(SWIFI_WIN_CONTROL *Control);

/* ReleaseHardware, after the complete stop order and before transport deletion. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiControlDelete(SWIFI_WIN_CONTROL *Control);

#endif
