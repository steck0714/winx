/*
 * Nintendo Switch power button driver, KMDF + Virtual HID Framework (VHF).
 *
 * ACPI device PWRB (ACPI\SWOA0601):
 *   I2cSerialBus 0x3C on I2C5: MAX77620 PMIC. The power button drives its
 *   EN0 input; there is no GPIO for it on the SoC.
 * Every 50 ms the driver reads (Hekate bdk/power/max77620.h):
 *   ONOFFSTAT 0x15 bit 2 (EN0): 1 while the button is held.
 *   ONOFFIRQ  0x0B (clear on read): bit 3 EN0 rising, bit 2 EN0 falling,
 *     so a press shorter than one poll is still seen.
 * Changes are reported as a HID System Control "System Power Down" button;
 * Windows then applies the power button action from Power Options.
 * Holding the button for several seconds still powers the PMIC off by itself.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>
#include <vhf.h>

#define MAX77620_ONOFFIRQ    0x0B
#define MAX77620_ONOFFSTAT   0x15
#define ONOFFSTAT_EN0        0x04
#define ONOFFIRQ_EN0_F       0x04
#define ONOFFIRQ_EN0_R       0x08
#define POLL_MS              50
#define REPORT_ID_SYSTEM     1

/* System Control: bit 0 System Power Down (0x81), 7 bits padding. */
static UCHAR SwPwrReportDescriptor[] = {
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01,
    0x85, REPORT_ID_SYSTEM,
    0x09, 0x81, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x95, 0x07, 0x81, 0x01,
    0xC0
};

typedef struct _SWPWR_DEVICE {
    LARGE_INTEGER I2cId;
    WDFIOTARGET I2c;
    WDFTIMER Timer;
    VHFHANDLE Vhf;
    volatile BOOLEAN Stopping;
    BOOLEAN Pressed;
    UCHAR Report[2];
    ULONG Failures;
    LONG Polls;
    LONG Presses;
    LONG Reports;
} SWPWR_DEVICE, *PSWPWR_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWPWR_DEVICE, SwPwrGetDevice)

typedef struct _SWPWR_TIMER {
    WDFDEVICE Device;
} SWPWR_TIMER, *PSWPWR_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWPWR_TIMER, SwPwrGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwPwrEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwPwrEvtPrepareHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwPwrEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwPwrEvtSelfManagedIoCleanup;
EVT_WDF_TIMER SwPwrEvtTimer;

static WCHAR SwPwrParamsPath[256];

static VOID SwPwrTrace(PCWSTR Name, ULONG Value)
{
    if (SwPwrParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwPwrParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

/* ------------------------------------------------------------------- I2C */

static NTSTATUS SwPwrOpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
{
    WCHAR PathBuffer[64];
    UNICODE_STRING Path;
    WDF_IO_TARGET_OPEN_PARAMS Open;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RtlUnicodeStringPrintf(&Path, L"\\Device\\RESOURCE_HUB\\%016I64x", (ULONG64)Id.QuadPart);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfIoTargetCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, Target);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Open, &Path, GENERIC_READ | GENERIC_WRITE);
    Status = WdfIoTargetOpen(*Target, &Open);
    if (!NT_SUCCESS(Status)) {
        WdfObjectDelete(*Target);
        *Target = NULL;
    }
    return Status;
}

static NTSTATUS SwPwrReadReg(WDFIOTARGET Target, UCHAR Register, PUCHAR Value)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    PSPB_TRANSFER_LIST_ENTRY Entries = Sequence.List.Transfers;
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Entries[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &Register, 1);
    Entries[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Value, 1);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Sequence, sizeof(Sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 2) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

/* ---------------------------------------------------------------- button */

static VOID SwPwrReport(PSWPWR_DEVICE Dev, BOOLEAN Pressed)
{
    HID_XFER_PACKET Packet;
    NTSTATUS Status;

    Dev->Report[0] = REPORT_ID_SYSTEM;
    Dev->Report[1] = Pressed ? 0x01 : 0x00;
    Packet.reportBuffer = Dev->Report;
    Packet.reportBufferLen = sizeof(Dev->Report);
    Packet.reportId = REPORT_ID_SYSTEM;
    Status = VhfReadReportSubmit(Dev->Vhf, &Packet);
    if (NT_SUCCESS(Status)) {
        SwPwrTrace(L"Reports", (ULONG)InterlockedIncrement(&Dev->Reports));
    } else {
        SwPwrTrace(L"LastReportError", (ULONG)Status);
    }
}

VOID SwPwrEvtTimer(WDFTIMER Timer)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(SwPwrGetTimer(Timer)->Device);
    UCHAR Irq = 0, Stat = 0;
    BOOLEAN Held;
    NTSTATUS Status;

    Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFIRQ, &Irq);
    if (NT_SUCCESS(Status)) {
        Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFSTAT, &Stat);
    }
    if (NT_SUCCESS(Status)) {
        Dev->Failures = 0;
        Held = (Stat & ONOFFSTAT_EN0) != 0;
        if (Held != Dev->Pressed) {
            if (Held) {
                SwPwrTrace(L"Presses", (ULONG)InterlockedIncrement(&Dev->Presses));
            }
            Dev->Pressed = Held;
            SwPwrReport(Dev, Held);
        } else if (!Held && (Irq & ONOFFIRQ_EN0_R) != 0) {
            /* pressed and released between two polls */
            SwPwrTrace(L"Presses", (ULONG)InterlockedIncrement(&Dev->Presses));
            SwPwrReport(Dev, TRUE);
            SwPwrReport(Dev, FALSE);
        }
        if (Irq != 0) {
            SwPwrTrace(L"LastOnOffIrq", Irq);
        }
    } else if (++Dev->Failures <= 3 || (Dev->Failures % 100) == 0) {
        SwPwrTrace(L"LastI2cError", (ULONG)Status);
        SwPwrTrace(L"Failures", Dev->Failures);
    }
    if ((InterlockedIncrement(&Dev->Polls) % 200) == 0) {
        SwPwrTrace(L"Polls", (ULONG)Dev->Polls);
        SwPwrTrace(L"LastOnOffStat", Stat);
    }
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));    /* one-shot passive timer, re-armed */
    }
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwPwrEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeConnection &&
            Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            Dev->I2cId.LowPart = Desc->u.Connection.IdLowPart;
            Dev->I2cId.HighPart = (LONG)Desc->u.Connection.IdHighPart;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

NTSTATUS SwPwrEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    VHF_CONFIG Config;
    UCHAR Stat = 0, Irq = 0;
    NTSTATUS Status;

    if (Dev->I2c == NULL) {
        Status = SwPwrOpenTarget(Device, Dev->I2cId, &Dev->I2c);
        SwPwrTrace(L"OpenI2cStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFSTAT, &Stat);
    SwPwrTrace(L"FirstReadStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    (VOID)SwPwrReadReg(Dev->I2c, MAX77620_ONOFFIRQ, &Irq);    /* drop edges latched before Windows */
    SwPwrTrace(L"OnOffStatAtStart", Stat);
    SwPwrTrace(L"OnOffIrqAtStart", Irq);
    Dev->Pressed = (Stat & ONOFFSTAT_EN0) != 0;

    if (Dev->Vhf == NULL) {
        VHF_CONFIG_INIT(&Config, WdfDeviceWdmGetDeviceObject(Device), sizeof(SwPwrReportDescriptor),
                        SwPwrReportDescriptor);
        Config.VhfClientContext = Dev;
        Config.VendorID = 0x057E;       /* Nintendo */
        Config.ProductID = 0x5601;
        Config.VersionNumber = 0x0001;
        Status = VhfCreate(&Config, &Dev->Vhf);
        SwPwrTrace(L"VhfCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Vhf = NULL;
            return Status;
        }
        Status = VhfStart(Dev->Vhf);
        SwPwrTrace(L"VhfStartStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            VhfDelete(Dev->Vhf, TRUE);
            Dev->Vhf = NULL;
            return Status;
        }
    }
    if (Dev->Timer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES Attributes;
        WDF_TIMER_CONFIG_INIT(&TimerConfig, SwPwrEvtTimer);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWPWR_TIMER);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->Timer);
        SwPwrTrace(L"TimerCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Timer = NULL;
            return Status;
        }
        SwPwrGetTimer(Dev->Timer)->Device = Device;
    }
    Dev->Stopping = FALSE;
    WdfTimerStart(Dev->Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));
    return STATUS_SUCCESS;
}

VOID SwPwrEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    Dev->Stopping = TRUE;
    if (Dev->Timer != NULL) {
        WdfTimerStop(Dev->Timer, TRUE);
    }
    if (Dev->Vhf != NULL) {
        VhfDelete(Dev->Vhf, TRUE);
        Dev->Vhf = NULL;
    }
    if (Dev->I2c != NULL) {
        WdfIoTargetClose(Dev->I2c);
        WdfObjectDelete(Dev->I2c);
        Dev->I2c = NULL;
    }
}

NTSTATUS SwPwrEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwPwrEvtPrepareHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwPwrEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwPwrEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWPWR_DEVICE);
    return WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwPwrParamsPath)) {
        RtlCopyMemory(SwPwrParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwPwrParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwPwrParamsPath);
    }
    SwPwrTrace(L"DriverVersion", 1);
    WDF_DRIVER_CONFIG_INIT(&Config, SwPwrEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
