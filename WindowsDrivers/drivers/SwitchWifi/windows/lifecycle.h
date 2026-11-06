/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWIFI_WINDOWS_LIFECYCLE_H
#define SWIFI_WINDOWS_LIFECYCLE_H

#include "firmware.h"

typedef struct SWIFI_HARDWARE_CALLBACKS {
    /* DISPATCH_LEVEL under Transport.DataLock. Packet/flow completions only. */
    swifi_ring_handler DispatchData;
    /* <=DISPATCH_LEVEL outside the protocol lock. Complete OS packets here. */
    VOID (*NotifyData)(PVOID Context);
    /* DISPATCH_LEVEL under DataLock, after confirmed hardware stop/reset.
     * Cancel packet ownership into completion lists; do not call the OS yet. */
    VOID (*CancelDataAfterReset)(PVOID Context);
    /* PASSIVE_LEVEL; copy any event data that must outlive the callback. */
    VOID (*Event)(PVOID Context, const swifi_event_completion *Event);
    /* <=DISPATCH_LEVEL, nonblocking. Queue owner recovery/PnP failure work.
     * That owner uses the same stop sequence as the power callbacks. */
    VOID (*ScheduleRecovery)(PVOID Context, NTSTATUS Reason);
} SWIFI_HARDWARE_CALLBACKS;

typedef struct SWIFI_HARDWARE {
    WDFDEVICE Device;
    WDFWORKITEM InitializeWork;
    WDFDMAENABLER Dma;
    SWIFI_PCIE_RESOURCES Bus;
    SWIFI_CHIP Chip;
    SWIFI_SHARED Shared;
    SWIFI_WIN_TRANSPORT Transport;
    SWIFI_WIN_CONTROL Control;
    SWIFI_FIRMWARE_INFO FirmwareInfo;
    SWIFI_HARDWARE_CALLBACKS Callbacks;
    PVOID Context;
    NTSTATUS InitializeStatus;
    NTSTATUS LastStopStatus;
    BOOLEAN Prepared;
    BOOLEAN ControlCreated;
    BOOLEAN DataBound;
    BOOLEAN RequiresColdBoot;
} SWIFI_HARDWARE;

/* DeviceAdd: creates the system-context initialization worker. H is resident
 * device-lifetime storage. All lifecycle calls are serialized by the owning
 * PnP/power callbacks, independently of any work item they wait for. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiHardwareCreate(WDFDEVICE Device, SWIFI_HARDWARE *H,
    const SWIFI_HARDWARE_CALLBACKS *Callbacks, PVOID Context);

/* Call directly from EvtDevicePrepareHardware (not a worker): interrupt-object
 * creation uses that KMDF phase and the actual assigned resource descriptors.
 * Chip/file/shared-buffer preparation itself runs in a system worker. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiHardwarePrepare(SWIFI_HARDWARE *H, WDFCMRESLIST Raw, WDFCMRESLIST Translated);

/* D0Entry: reload a stopped session if necessary, rebind queues/control to
 * the existing interrupt object and enable DMA. IRQs are not armed yet.
 * Initialize/rebind the owner's data core from H.Shared before returning
 * from D0Entry, then set H.DataBound=TRUE. No packet buffer is posted here. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiHardwareEnterD0(SWIFI_HARDWARE *H);

/* D0EntryPostInterruptsEnabled: prime events, query firmware identity/radio
 * capabilities. WiFiCx capability publication follows this successful return. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiHardwareOnline(SWIFI_HARDWARE *H);

/* Wake command waiters, stop upper OS queues/tasks, then call Stop before
 * KMDF disables interrupts. This also unwinds a failed start. Stop performs
 * no OS task/queue management; caller prevents new submissions first. */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID SwifiHardwareRequestStop(SWIFI_HARDWARE *H);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiHardwareStop(SWIFI_HARDWARE *H);

/* ReleaseHardware: only frees/unmaps after a confirmed stop. On failure,
 * retain this context and its DMA objects; the caller must resolve hardware
 * isolation before allowing parent-object deletion. No failure is treated
 * as successful DMA quiescence. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiHardwareRelease(SWIFI_HARDWARE *H);

/* Device teardown after a successful Release. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiHardwareDelete(SWIFI_HARDWARE *H);

#endif
