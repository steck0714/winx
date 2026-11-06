/* SPDX-License-Identifier: GPL-2.0-only */
#include "station_filter.h"

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
    if (!InterlockedExchange(&D->Failing, 1)) {
        SwifiReportIo(&D->Report, D->Hw.Transport.SawInterrupt, D->Hw.Transport.SawRing);
        SwifiReportStep(&D->Report, SwifiRuntimeFailure, Reason);
        SwifiDataRequestStop(D);
        WdfDeviceSetFailed(D->Hw.Device, WdfDeviceFailedNoRestart);
    }
}

static NTSTATUS Prepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    SWIFI_DEVICE *D = SwifiDeviceContext(Device);
    SwifiReportStep(&D->Report, SwifiPrepare, STATUS_PENDING);
    NTSTATUS Status = SwifiHardwarePrepare(&D->Hw, Raw, Translated);
    SwifiReportStep(&D->Report, SwifiPrepare, Status);
    if (!NT_SUCCESS(Status)) SwifiReportFlush(&D->Report);
    return Status;
}

static NTSTATUS Release(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    SWIFI_DEVICE *D = SwifiDeviceContext(Device);
    UNREFERENCED_PARAMETER(Translated);
    SwifiDataRequestStop(D);
    NTSTATUS Status = SwifiHardwareRelease(&D->Hw);
    if (!NT_SUCCESS(Status)) SwifiReportStep(&D->Report, SwifiQuiesce, Status);
    SwifiReportFlush(&D->Report);
    return Status;
}

static NTSTATUS EnterD0(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    SWIFI_DEVICE *D = SwifiDeviceContext(Device);
    UNREFERENCED_PARAMETER(Previous);
    NTSTATUS Status = SwifiHardwareEnterD0(&D->Hw);
    if (NT_SUCCESS(Status)) Status = SwifiDataBind(D);
    SwifiReportStep(&D->Report, SwifiEnterD0, Status);
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
    SwifiReportStep(&D->Report, SwifiRadioOnline, Status);
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
    NTSTATUS Status = SwifiHardwareStop(&D->Hw);
    if (!NT_SUCCESS(Status)) SwifiReportStep(&D->Report, SwifiQuiesce, Status);
    SwifiReportFlush(&D->Report);
    return Status;
}

static NTSTATUS DeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    static const SWIFI_HARDWARE_CALLBACKS Callbacks = {
        SwifiCreateAdapter, SwifiDataDispatch, SwifiDataNotify, SwifiDataReset, SwifiDataRelease,
        SwifiPnpFailure};
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
    KeInitializeEvent(&D->Data.EapolIdle, NotificationEvent, TRUE);
    SwifiReportOpen(Device, &D->Report);
    Status = SwifiFilterCreate(Device, D);
    if (NT_SUCCESS(Status)) Status = SwifiHardwareCreate(Device, &D->Hw, &Callbacks, D, &D->Report);
    SwifiReportStep(&D->Report, SwifiDeviceAdd, Status);
    if (!NT_SUCCESS(Status)) SwifiReportFlush(&D->Report);
    return Status;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDF_DRIVER_CONFIG_INIT(&Config, DeviceAdd);
    return WdfDriverCreate(Driver, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
