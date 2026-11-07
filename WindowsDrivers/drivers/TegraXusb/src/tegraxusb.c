/* SPDX-License-Identifier: GPL-2.0-only
 * Tegra210B01 XUSB Falcon sideband. Windows USBXHCI owns the host aperture. */
#include <ntddk.h>
#include <wdf.h>
#include "mailbox.h"

enum {
    MailCmd = 0xe4,
    MailIn = 0xe8,
    MailOut = 0xec,
    MailOwner = 0xf0,
    SmiIntr = 0x428
};
#define TO_FALCON ((ULONG)(1u << 27))
#define TO_HOST ((ULONG)(1u << 29))
#define MAIL_IRQ ((ULONG)(1u << 31))
typedef struct {
    PUCHAR Fpci;
    PULONG Lane;
    WDFINTERRUPT Interrupt;
    ULONG Messages;
    BOOLEAN Enabled, Notified, Hung;
} SWUSB_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWUSB_DEVICE, SwUsbDevice)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD Add;
static EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
static EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED Start;
static EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED Stop;
static EVT_WDF_INTERRUPT_ISR Isr;
static EVT_WDF_INTERRUPT_ENABLE Enable;
static EVT_WDF_INTERRUPT_DISABLE Disable;

static ULONG Read(SWUSB_DEVICE *d, ULONG reg)
{
    return READ_REGISTER_ULONG((PULONG)(d->Fpci + reg));
}
static void Write(SWUSB_DEVICE *d, ULONG reg, ULONG value)
{
    WRITE_REGISTER_ULONG((PULONG)(d->Fpci + reg), value);
}

static void Trace(WDFDEVICE device, PCUNICODE_STRING name, ULONG value)
{
    WDFKEY key;
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (void)WdfRegistryAssignULong(key, name, value);
        WdfRegistryClose(key);
    }
}

/* Passive interrupt handling: LFPS disable must settle before the reply. */
static BOOLEAN Isr(WDFINTERRUPT interrupt, ULONG messageId)
{
    SWUSB_DEVICE *d = SwUsbDevice(WdfInterruptGetDevice(interrupt));
    UNREFERENCED_PARAMETER(messageId);
    if (!d->Enabled)
        return FALSE;
    ULONG status = Read(d, SmiIntr), cmd = Read(d, MailCmd);
    if (status == MAXULONG || cmd == MAXULONG || (!status && !(cmd & TO_HOST)))
        return FALSE;
    if (status) {
        Write(d, SmiIntr, status);
        (void)Read(d, SmiIntr); /* Flush the W1C before releasing the level interrupt. */
    }
    if (status & 2u) {
        d->Hung = TRUE;
        return TRUE;
    }
    if (!(cmd & TO_HOST))
        return TRUE;
    ULONG msg = Read(d, MailOut);
    Write(d, MailCmd, cmd & ~TO_HOST);
    SWUSB_REPLY reply = SwUsbReply(msg);
    ++d->Messages;
    if (!reply.command) {
        Write(d, MailOwner, 0);
        return TRUE;
    }
    if (reply.lfps) {
        ULONG field = (3u << 20) | (1u << 18) | (1u << 13);
        ULONG reg = READ_REGISTER_ULONG(d->Lane) & ~field;
        if (reply.lfps < 0)
            reg |= (1u << 20) | (1u << 18) | (1u << 13);
        WRITE_REGISTER_ULONG(d->Lane, reg);
        (void)READ_REGISTER_ULONG(d->Lane);
        if (reply.lfps < 0) {
            LARGE_INTEGER interval;
            interval.QuadPart = -5000;
            (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
        }
    }
    Write(d, MailIn, (reply.command << 24) | reply.data);
    Write(d, MailCmd, Read(d, MailCmd) | MAIL_IRQ | TO_FALCON);
    return TRUE;
}

static NTSTATUS Enable(WDFINTERRUPT interrupt, WDFDEVICE device)
{
    UNREFERENCED_PARAMETER(interrupt);
    SwUsbDevice(device)->Enabled = TRUE;
    return STATUS_SUCCESS;
}
static NTSTATUS Disable(WDFINTERRUPT interrupt, WDFDEVICE device)
{
    UNREFERENCED_PARAMETER(interrupt);
    SwUsbDevice(device)->Enabled = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS Start(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    UNREFERENCED_PARAMETER(previous);
    SWUSB_DEVICE *d = SwUsbDevice(device);
    DECLARE_CONST_UNICODE_STRING(name, L"MailboxStartStatus");
    LARGE_INTEGER interval;
    interval.QuadPart = -10000;
    NTSTATUS status = STATUS_IO_TIMEOUT;
    BOOLEAN sent = FALSE;
    for (unsigned i = 0; i < 250; i++) {
        WdfInterruptAcquireLock(d->Interrupt);
        ULONG owner = Read(d, MailOwner);
        if (d->Hung || owner > 2)
            status = STATUS_DEVICE_HARDWARE_ERROR;
        else if (!owner && (sent || d->Notified)) {
            d->Notified = TRUE;
            status = STATUS_SUCCESS;
        } else if (!owner) {
            Write(d, MailOwner, 2);
            if (Read(d, MailOwner) == 2) {
                Write(d, MailIn, SwUsbEnableMessages << 24);
                Write(d, MailCmd, Read(d, MailCmd) | MAIL_IRQ | TO_FALCON);
                sent = TRUE;
            }
        }
        WdfInterruptReleaseLock(d->Interrupt);
        if (status != STATUS_IO_TIMEOUT)
            break;
        (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
    Trace(device, &name, (ULONG)status);
    return status;
}

static NTSTATUS Stop(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    SWUSB_DEVICE *d = SwUsbDevice(device);
    DECLARE_CONST_UNICODE_STRING(name, L"MailboxRequests");
    WdfInterruptAcquireLock(d->Interrupt);
    ULONG count = d->Messages;
    if (target == WdfPowerDeviceD3Final)
        d->Notified = FALSE;
    WdfInterruptReleaseLock(d->Interrupt);
    Trace(device, &name, count);
    return STATUS_SUCCESS;
}

static NTSTATUS Release(WDFDEVICE device, WDFCMRESLIST translated)
{
    SWUSB_DEVICE *d = SwUsbDevice(device);
    UNREFERENCED_PARAMETER(translated);
    if (d->Lane) {
        MmUnmapIoSpace(d->Lane, 4);
        d->Lane = NULL;
    }
    if (d->Fpci) {
        MmUnmapIoSpace(d->Fpci, 0x1000);
        d->Fpci = NULL;
    }
    d->Notified = d->Hung = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS Prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    SWUSB_DEVICE *d = SwUsbDevice(device);
    for (ULONG i = 0; i < WdfCmResourceListGetCount(translated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(raw, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR t = WdfCmResourceListGetDescriptor(translated, i);
        if (!r || !t || r->Type != CmResourceTypeMemory || t->Type != CmResourceTypeMemory)
            continue;
        if (r->u.Memory.Start.QuadPart == 0x70098000 && t->u.Memory.Length == 0x1000 && !d->Fpci)
            d->Fpci = MmMapIoSpaceEx(t->u.Memory.Start, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
        if (r->u.Memory.Start.QuadPart == 0x7009f5a0 && t->u.Memory.Length == 4 && !d->Lane)
            d->Lane = MmMapIoSpaceEx(t->u.Memory.Start, 4, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (d->Fpci && d->Lane)
        return STATUS_SUCCESS;
    Release(device, translated);
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

static NTSTATUS Add(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS cb;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_INTERRUPT_CONFIG irq;
    WDFDEVICE device;
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&cb);
    cb.EvtDevicePrepareHardware = Prepare;
    cb.EvtDeviceReleaseHardware = Release;
    cb.EvtDeviceD0EntryPostInterruptsEnabled = Start;
    cb.EvtDeviceD0ExitPreInterruptsDisabled = Stop;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &cb);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, SWUSB_DEVICE);
    NTSTATUS status = WdfDeviceCreate(&init, &attr, &device);
    if (!NT_SUCCESS(status))
        return status;
    WDF_INTERRUPT_CONFIG_INIT(&irq, Isr, NULL);
    irq.PassiveHandling = TRUE;
    irq.EvtInterruptEnable = Enable;
    irq.EvtInterruptDisable = Disable;
    return WdfInterruptCreate(device, &irq, WDF_NO_OBJECT_ATTRIBUTES, &SwUsbDevice(device)->Interrupt);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, Add);
    return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
