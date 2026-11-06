/* SPDX-License-Identifier: GPL-2.0-only */
#include "station_data.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD DeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
static EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
static EVT_WDF_DEVICE_D0_ENTRY EnterD0;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED Online;
static EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED Stop;

/* WdfDeviceSetFailed queues PnP teardown. A cold preparation is required
 * after a held platform reset; never restart a failed endpoint in a loop. */
VOID SwifiPnpFailure(PVOID Context, NTSTATUS Reason)
{
    SWIFI_DEVICE *D = Context;
    UNREFERENCED_PARAMETER(Reason);
    if (!InterlockedExchange(&D->Failing, 1)) {
        SwifiDataRequestStop(D);
        WdfDeviceSetFailed(D->Hw.Device, WdfDeviceFailedNoRestart);
    }
}

static VOID OtherEvent(PVOID Context, const swifi_event_completion *Event)
{
    /* Only the WDI association task may establish a Windows link. A known
     * link-down event on interface 0 always invalidates outstanding frames. */
    SWIFI_DEVICE *D = Context;
    const UCHAR *P = Event->data;
    if (!Event->firmware_status && !Event->ifidx && Event->length >= 72 &&
        P[12] == 0x88 && P[13] == 0x6c && !P[70] && !P[19] && P[20] == 0x10 && P[21] == 0x18 && !P[22] && P[23] == 1 &&
        P[28] == 0 && P[29] == 0 && P[30] == 0 && P[31] == 16 && !(P[27] & 1))
        SwifiDataLink(D, NULL, FALSE);
}

static NTSTATUS Prepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    return SwifiHardwarePrepare(&SwifiDeviceContext(Device)->Hw, Raw, Translated);
}

static NTSTATUS Release(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    SWIFI_DEVICE *D = SwifiDeviceContext(Device);
    UNREFERENCED_PARAMETER(Translated);
    SwifiDataRequestStop(D);
    return SwifiHardwareRelease(&D->Hw);
}

static NTSTATUS EnterD0(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    SWIFI_DEVICE *D = SwifiDeviceContext(Device);
    UNREFERENCED_PARAMETER(Previous);
    NTSTATUS Status = SwifiHardwareEnterD0(&D->Hw);
    if (NT_SUCCESS(Status)) Status = SwifiDataBind(D);
    if (!NT_SUCCESS(Status)) {
        NTSTATUS StopStatus = SwifiHardwareStop(&D->Hw);
        if (!NT_SUCCESS(StopStatus)) return StopStatus;
    }
    return Status;
}

static NTSTATUS Online(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    SWIFI_DEVICE *D = SwifiDeviceContext(Device);
    UNREFERENCED_PARAMETER(Previous);
    NTSTATUS Status = SwifiHardwareOnline(&D->Hw);
    if (!NT_SUCCESS(Status)) {
        SwifiDataRequestStop(D);
        NTSTATUS StopStatus = SwifiHardwareStop(&D->Hw);
        if (!NT_SUCCESS(StopStatus)) return StopStatus;
    }
    return Status;
}

static NTSTATUS Stop(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    SWIFI_DEVICE *D = SwifiDeviceContext(Device);
    UNREFERENCED_PARAMETER(Target);
    SwifiDataRequestStop(D);
    return SwifiHardwareStop(&D->Hw);
}

static NTSTATUS DeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    static const SWIFI_HARDWARE_CALLBACKS Callbacks = {
        SwifiCreateAdapter, SwifiDataDispatch, SwifiDataNotify, SwifiDataReset, SwifiDataRelease,
        OtherEvent, SwifiPnpFailure};
    WDF_PNPPOWER_EVENT_CALLBACKS Power;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device;
    UNREFERENCED_PARAMETER(Driver);
    NTSTATUS Status = NetDeviceInitConfig(Init);
    if (NT_SUCCESS(Status)) Status = WifiDeviceInitConfig(Init);
    if (!NT_SUCCESS(Status)) return Status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Power);
    Power.EvtDevicePrepareHardware = Prepare;
    Power.EvtDeviceReleaseHardware = Release;
    Power.EvtDeviceD0Entry = EnterD0;
    Power.EvtDeviceD0EntryPostInterruptsEnabled = Online;
    Power.EvtDeviceD0ExitPreInterruptsDisabled = Stop;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Power);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWIFI_DEVICE);
    Status = WdfDeviceCreate(&Init, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) return Status;
    SWIFI_DEVICE *D = SwifiDeviceContext(Device);
    D->Data.Hardware = &D->Hw;
    return SwifiHardwareCreate(Device, &D->Hw, &Callbacks, D);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDF_DRIVER_CONFIG_INIT(&Config, DeviceAdd);
    return WdfDriverCreate(Driver, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
