/*
 * Nintendo Switch fan control driver, KMDF.
 *
 * ACPI device FAN0 (ACPI\SWOA0401):
 *   Memory32Fixed 0x7000A000: PWM controller. Only PWM_CSR_1 (+0x10) is
 *     touched; the fan uses PWM1 with inverted polarity (register duty =
 *     236 - duty, bit 24 = absolute off), as Hekate bdk/thermal/fan.c.
 *   I2cSerialBus 0x4C on I2C1: TMP451 (reg 0x00 board, 0x01 SoC/remote, C).
 * UEFI R366+ already powers the fan (5V EN on PA5), clocks PWM and routes
 * LCD_GPIO2 to PWM1, and starts it at duty 76/236. This driver only changes
 * the duty from the temperature every 2 s.
 *
 * Steps (Hekate fan_set_from_temp, plus two higher steps for overclocking):
 *   >= 68 C: 236 (100%)   >= 60 C: 153 (65%)   >= 52 C: 102 (43%)
 *   >= 47 C:  76 (32%)    >= 42 C:  51 (22%)   <= 39 C:   0 (off)
 *   40-41 C keeps the previous duty (hysteresis).
 * If the sensor cannot be read 3 times in a row, the fan goes to 153.
 *
 * v2: the same PWM block also drives the panel backlight (PWM_CSR_0, duty
 * 0..255, not inverted). UEFI R376+ routes LCD_BL_PWM (PV0) to PWM0 at duty
 * 255; this driver restores the saved brightness, follows the console display
 * state (off = 0, dimmed = 1/3) and takes brightness changes from the tray app
 * through \\.\SwitchBacklight. Without R376 the CSR_0 writes do nothing.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>

#define PWM_CSR_0        0x00
#define BACKLIGHT_MIN    8u
#define BACKLIGHT_MAX    255u
#define IOCTL_SWBL_GET   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_READ_ACCESS)    /* 0x00226040 */
#define IOCTL_SWBL_SET   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_WRITE_ACCESS)   /* 0x0022A044 */

/* GUID_CONSOLE_DISPLAY_STATE: 0 off, 1 on, 2 dimmed. */
static const GUID SwFanDisplayStateGuid =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

typedef struct _SWBL_STATUS {
    ULONG Version;          /* 2 */
    ULONG Brightness;       /* user level, 8..255 */
    ULONG Duty;             /* what CSR_0 holds now */
    ULONG DisplayState;     /* 0 off, 1 on, 2 dimmed */
} SWBL_STATUS;

#define PWM_CSR_1        0x10
#define PWM_CSR_EN       (1u << 31)
#define PWM_DUTY_MAX     236u
#define PWM_ABSOLUTE_OFF (0x100u << 16)
#define TMP451_LOCAL     0x00
#define TMP451_REMOTE    0x01
#define POLL_MS          2000
#define FAILSAFE_DUTY    153u

typedef struct _SWFAN_DEVICE {
    volatile UCHAR *Pwm;
    ULONG PwmLength;
    LARGE_INTEGER SensorId;
    WDFIOTARGET Sensor;
    WDFTIMER Timer;
    volatile BOOLEAN Stopping;
    ULONG Duty;
    ULONG Failures;
    LONG Runs;
    WDFWAITLOCK BacklightLock;
    PVOID DisplayCallback;
    ULONG Brightness;
    ULONG DisplayState;
} SWFAN_DEVICE, *PSWFAN_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWFAN_DEVICE, SwFanGetDevice)

typedef struct _SWFAN_TIMER {
    WDFDEVICE Device;
} SWFAN_TIMER, *PSWFAN_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWFAN_TIMER, SwFanGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwFanEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwFanEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwFanEvtReleaseHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwFanEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwFanEvtSelfManagedIoCleanup;
EVT_WDF_TIMER SwFanEvtTimer;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SwFanEvtIoDeviceControl;
POWER_SETTING_CALLBACK SwFanDisplayStateCallback;

static WCHAR SwFanParamsPath[256];

static VOID SwFanTrace(PCWSTR Name, ULONG Value)
{
    if (SwFanParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwFanParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

/* ------------------------------------------------------------------- I2C */

static NTSTATUS SwFanOpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
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

static NTSTATUS SwFanReadReg(WDFIOTARGET Target, UCHAR Register, PUCHAR Value)
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
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 2) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

/* ------------------------------------------------------------------- fan */

static VOID SwFanSetDuty(PSWFAN_DEVICE Dev, ULONG Duty)
{
    ULONG Csr;
    if (Duty > PWM_DUTY_MAX) {
        Duty = PWM_DUTY_MAX;
    }
    Csr = (Duty == 0) ? (PWM_CSR_EN | PWM_ABSOLUTE_OFF) : (PWM_CSR_EN | ((PWM_DUTY_MAX - Duty) << 16));
    WRITE_REGISTER_ULONG((PULONG)(Dev->Pwm + PWM_CSR_1), Csr);
    Dev->Duty = Duty;
}

static ULONG SwFanDutyForTemp(ULONG Temp, ULONG Previous)
{
    if (Temp >= 68) return 236;
    if (Temp >= 60) return 153;
    if (Temp >= 52) return 102;
    if (Temp >= 47) return 76;
    if (Temp >= 42) return 51;
    if (Temp <= 39) return 0;
    return (Previous > 51) ? 51 : Previous;    /* 40-41 C: hold (at most the 42 C step) */
}

VOID SwFanEvtTimer(WDFTIMER Timer)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(SwFanGetTimer(Timer)->Device);
    UCHAR Local = 0, Remote = 0;
    NTSTATUS Status;

    Status = SwFanReadReg(Dev->Sensor, TMP451_REMOTE, &Remote);
    if (NT_SUCCESS(Status)) {
        Status = SwFanReadReg(Dev->Sensor, TMP451_LOCAL, &Local);
    }
    if (NT_SUCCESS(Status)) {
        ULONG Temp = (Remote > Local) ? Remote : Local;
        Dev->Failures = 0;
        SwFanSetDuty(Dev, SwFanDutyForTemp(Temp, Dev->Duty));
        SwFanTrace(L"TempSoC", Remote);
        SwFanTrace(L"TempBoard", Local);
    } else {
        SwFanTrace(L"LastSensorError", (ULONG)Status);
        if (++Dev->Failures >= 3) {
            SwFanSetDuty(Dev, FAILSAFE_DUTY);
        }
    }
    SwFanTrace(L"Duty", Dev->Duty);
    SwFanTrace(L"Runs", (ULONG)InterlockedIncrement(&Dev->Runs));
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));    /* one-shot passive timer, re-armed */
    }
}

/* ------------------------------------------------------------- backlight */

static ULONG SwFanReadParam(PCWSTR Name, ULONG Default)
{
    ULONG Value = Default;
    RTL_QUERY_REGISTRY_TABLE Table[2];

    if (SwFanParamsPath[0] == L'\0') {
        return Default;
    }
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, SwFanParamsPath, Table, NULL, NULL))) {
        Value = Default;
    }
    return Value;
}

/* CSR_0 from the brightness and the display state. Caller holds BacklightLock. */
static ULONG SwFanApplyBacklight(PSWFAN_DEVICE Dev)
{
    ULONG Duty = Dev->Brightness;
    if (Dev->DisplayState == 0) {
        Duty = 0;
    } else if (Dev->DisplayState == 2) {
        Duty = (Duty / 3 < BACKLIGHT_MIN) ? BACKLIGHT_MIN : Duty / 3;
    }
    if (Dev->Pwm != NULL) {
        WRITE_REGISTER_ULONG((PULONG)(Dev->Pwm + PWM_CSR_0), PWM_CSR_EN | (Duty << 16));
    }
    SwFanTrace(L"BacklightDuty", Duty);
    return Duty;
}

NTSTATUS SwFanDisplayStateCallback(LPCGUID Setting, PVOID Value, ULONG Length, PVOID Context)
{
    PSWFAN_DEVICE Dev = (PSWFAN_DEVICE)Context;
    if (Length >= sizeof(ULONG) && IsEqualGUID(Setting, &SwFanDisplayStateGuid)) {
        WdfWaitLockAcquire(Dev->BacklightLock, NULL);
        Dev->DisplayState = *(PULONG)Value;
        SwFanTrace(L"DisplayState", Dev->DisplayState);
        (VOID)SwFanApplyBacklight(Dev);
        WdfWaitLockRelease(Dev->BacklightLock);
    }
    return STATUS_SUCCESS;
}

VOID SwFanEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(WdfIoQueueGetDevice(Queue));
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Info = 0;
    UNREFERENCED_PARAMETER(OutLength);
    UNREFERENCED_PARAMETER(InLength);

    WdfWaitLockAcquire(Dev->BacklightLock, NULL);
    if (Code == IOCTL_SWBL_GET) {
        SWBL_STATUS *Out;
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID *)&Out, NULL);
        if (NT_SUCCESS(Status)) {
            Out->Version = 2;
            Out->Brightness = Dev->Brightness;
            Out->Duty = (Dev->Pwm != NULL) ? (READ_REGISTER_ULONG((PULONG)(Dev->Pwm + PWM_CSR_0)) >> 16) & 0x7FFF : 0;
            Out->DisplayState = Dev->DisplayState;
            Info = sizeof(*Out);
        }
    } else if (Code == IOCTL_SWBL_SET) {
        PULONG In;
        Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), (PVOID *)&In, NULL);
        if (NT_SUCCESS(Status)) {
            ULONG Level = *In;
            Dev->Brightness = (Level < BACKLIGHT_MIN) ? BACKLIGHT_MIN : (Level > BACKLIGHT_MAX) ? BACKLIGHT_MAX : Level;
            SwFanTrace(L"Brightness", Dev->Brightness);     /* also what the next boot restores */
            (VOID)SwFanApplyBacklight(Dev);
        }
    }
    WdfWaitLockRelease(Dev->BacklightLock);
    WdfRequestCompleteWithInformation(Request, Status, Info);
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwFanEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    BOOLEAN HaveSensor = FALSE;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc == NULL) {
            continue;
        }
        if (Desc->Type == CmResourceTypeMemory && Dev->Pwm == NULL) {
            if (Desc->u.Memory.Length < 0x20) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            Dev->Pwm = (volatile UCHAR *)MmMapIoSpaceEx(Desc->u.Memory.Start, Desc->u.Memory.Length,
                                                        PAGE_READWRITE | PAGE_NOCACHE);
            if (Dev->Pwm == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            Dev->PwmLength = Desc->u.Memory.Length;
        } else if (Desc->Type == CmResourceTypeConnection && !HaveSensor &&
                   Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                   Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            Dev->SensorId.LowPart = Desc->u.Connection.IdLowPart;
            Dev->SensorId.HighPart = (LONG)Desc->u.Connection.IdHighPart;
            HaveSensor = TRUE;
        }
    }
    if (Dev->Pwm == NULL || !HaveSensor) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    SwFanTrace(L"CsrAtStart", READ_REGISTER_ULONG((PULONG)(Dev->Pwm + PWM_CSR_1)));
    return STATUS_SUCCESS;
}

NTSTATUS SwFanEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (Dev->Pwm != NULL) {
        SwFanSetDuty(Dev, 76);      /* leave the UEFI default running when the driver goes away */
        MmUnmapIoSpace((PVOID)Dev->Pwm, Dev->PwmLength);
        Dev->Pwm = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS SwFanEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    NTSTATUS Status;

    /* Backlight first: it must not depend on the temperature sensor. */
    WdfWaitLockAcquire(Dev->BacklightLock, NULL);
    Dev->Brightness = SwFanReadParam(L"Brightness", BACKLIGHT_MAX);
    if (Dev->Brightness < BACKLIGHT_MIN || Dev->Brightness > BACKLIGHT_MAX) {
        Dev->Brightness = BACKLIGHT_MAX;
    }
    Dev->DisplayState = 1;
    SwFanTrace(L"BacklightCsrAtStart", READ_REGISTER_ULONG((PULONG)(Dev->Pwm + PWM_CSR_0)));
    (VOID)SwFanApplyBacklight(Dev);
    WdfWaitLockRelease(Dev->BacklightLock);
    if (Dev->DisplayCallback == NULL) {
        /* Called once right away with the current state, so not under BacklightLock. */
        Status = PoRegisterPowerSettingCallback(WdfDeviceWdmGetDeviceObject(Device), &SwFanDisplayStateGuid,
                                                SwFanDisplayStateCallback, Dev, &Dev->DisplayCallback);
        SwFanTrace(L"DisplayCallbackStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->DisplayCallback = NULL;
        }
    }

    Status = SwFanOpenTarget(Device, Dev->SensorId, &Dev->Sensor);
    SwFanTrace(L"OpenSensorStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        SwFanSetDuty(Dev, FAILSAFE_DUTY);
        return Status;
    }
    Dev->Duty = 76;                 /* what UEFI R366+ set */
    if (Dev->Timer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES Attributes;
        WDF_TIMER_CONFIG_INIT(&TimerConfig, SwFanEvtTimer);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWFAN_TIMER);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->Timer);
        SwFanTrace(L"TimerCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Timer = NULL;
            return Status;
        }
        SwFanGetTimer(Dev->Timer)->Device = Device;
    }
    Dev->Stopping = FALSE;
    WdfTimerStart(Dev->Timer, WDF_REL_TIMEOUT_IN_MS(500));
    return STATUS_SUCCESS;
}

VOID SwFanEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    Dev->Stopping = TRUE;
    if (Dev->Timer != NULL) {
        WdfTimerStop(Dev->Timer, TRUE);
    }
    if (Dev->DisplayCallback != NULL) {
        (VOID)PoUnregisterPowerSettingCallback(Dev->DisplayCallback);
        Dev->DisplayCallback = NULL;
    }
    WdfWaitLockAcquire(Dev->BacklightLock, NULL);
    Dev->DisplayState = 1;              /* never leave the panel dark behind */
    (VOID)SwFanApplyBacklight(Dev);
    WdfWaitLockRelease(Dev->BacklightLock);
}

NTSTATUS SwFanEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    DECLARE_CONST_UNICODE_STRING(DeviceName, L"\\Device\\SwitchBacklight");
    DECLARE_CONST_UNICODE_STRING(LinkName, L"\\DosDevices\\SwitchBacklight");
    WDFDEVICE Device;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwFanEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwFanEvtReleaseHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwFanEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwFanEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    Status = WdfDeviceInitAssignName(DeviceInit, &DeviceName);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWFAN_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &SwFanGetDevice(Device)->BacklightLock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfDeviceCreateSymbolicLink(Device, &LinkName);
    SwFanTrace(L"SymbolicLinkStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
    QueueConfig.EvtIoDeviceControl = SwFanEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfIoQueueCreate(Device, &QueueConfig, &Attributes, WDF_NO_HANDLE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwFanParamsPath)) {
        RtlCopyMemory(SwFanParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwFanParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwFanParamsPath);
    }
    SwFanTrace(L"DriverVersion", 2);
    WDF_DRIVER_CONFIG_INIT(&Config, SwFanEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
