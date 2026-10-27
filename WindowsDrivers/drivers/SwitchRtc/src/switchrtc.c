/*
 * Nintendo Switch real-time clock, KMDF.
 *
 * ACPI device RTC0 (ACPI\SWOA0801):
 *   I2cSerialBus 0x68 on I2C5: the MAX77620 RTC block (Hekate bdk/rtc).
 *   UPDATE0 0x04 = 0x10 latches the time into the read buffer; then CONTROL
 *   0x03 and SEC..DAY at 0x07..0x0D. CONTROL bit 0 set means BCD (Linux
 *   rtc-max77686 BCD_EN; Hekate calls it BIN_FORMAT, which reads backwards),
 *   bit 1 set means 24 h. On this unit CONTROL = 0x02: binary, 24 h.
 * The UEFI RTC is virtual (it counts from a fixed date), so Windows starts
 * with a wrong clock. Horizon keeps its own time in this RTC as raw value +
 * a settings offset, so this driver never writes the RTC either. It keeps
 * its own offset instead:
 *   - start:  system time = RTC + Parameters\OffsetSeconds (if learned)
 *   - \Callback\SetSystemTime (NTP sync, manual change): OffsetSeconds =
 *     system time - RTC, so the next boot starts from the right time.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>

#define RTC_CONTROL      0x03
#define RTC_UPDATE0      0x04
#define RTC_READ_UPDATE  0x10
#define RTC_SEC          0x07
#define RTC_BCD          0x01
#define OFFSET_FORMAT    2      /* v1 learned offsets from a misread (BCD) RTC: ignore them */
#define RTC_24H          0x02
#define RTC_HOUR_PM      0x40
#define TICKS_PER_SECOND 10000000LL

NTSYSAPI NTSTATUS NTAPI ZwSetSystemTime(PLARGE_INTEGER SystemTime, PLARGE_INTEGER PreviousTime);

typedef struct _SWRTC_DEVICE {
    LARGE_INTEGER I2cId;
    WDFIOTARGET I2c;
    WDFWORKITEM LearnWork;
    PCALLBACK_OBJECT TimeCallbackObject;
    PVOID TimeCallback;
    LONG Learns;
} SWRTC_DEVICE, *PSWRTC_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWRTC_DEVICE, SwRtcGetDevice)

typedef struct _SWRTC_WORK {
    WDFDEVICE Device;
} SWRTC_WORK, *PSWRTC_WORK;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWRTC_WORK, SwRtcGetWork)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwRtcEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwRtcEvtPrepareHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwRtcEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwRtcEvtSelfManagedIoCleanup;
EVT_WDF_WORKITEM SwRtcEvtLearnWork;
CALLBACK_FUNCTION SwRtcTimeChanged;

static WCHAR SwRtcParamsPath[256];

static VOID SwRtcTrace(PCWSTR Name, ULONG Value)
{
    if (SwRtcParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwRtcParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

static VOID SwRtcTrace64(PCWSTR Name, LONGLONG Value)
{
    if (SwRtcParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwRtcParamsPath, Name, REG_QWORD, &Value, sizeof(Value));
    }
}

/* Parameters value Name of Type and exactly Size bytes. */
static BOOLEAN SwRtcReadValue(PCWSTR ValueName, ULONG Type, PVOID Data, ULONG Size)
{
    UNICODE_STRING Path, Name;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key;
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(LONGLONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    ULONG Length = 0;
    NTSTATUS Status;

    RtlInitUnicodeString(&Path, SwRtcParamsPath);
    RtlInitUnicodeString(&Name, ValueName);
    InitializeObjectAttributes(&Attributes, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (Size > sizeof(LONGLONG) || !NT_SUCCESS(ZwOpenKey(&Key, KEY_READ, &Attributes))) {
        return FALSE;
    }
    Status = ZwQueryValueKey(Key, &Name, KeyValuePartialInformation, Info, sizeof(Buffer), &Length);
    ZwClose(Key);
    if (!NT_SUCCESS(Status) || Info->Type != Type || Info->DataLength != Size) {
        return FALSE;
    }
    RtlCopyMemory(Data, Info->Data, Size);
    return TRUE;
}

static BOOLEAN SwRtcReadOffset(LONGLONG *Offset)
{
    ULONG Format = 0;
    return SwRtcReadValue(L"OffsetFormat", REG_DWORD, &Format, sizeof(Format)) && Format == OFFSET_FORMAT &&
           SwRtcReadValue(L"OffsetSeconds", REG_QWORD, Offset, sizeof(*Offset));
}

/* ------------------------------------------------------------------- I2C */

static NTSTATUS SwRtcOpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
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

static NTSTATUS SwRtcRead(WDFIOTARGET Target, UCHAR Register, PUCHAR Data, ULONG Length)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    PSPB_TRANSFER_LIST_ENTRY Entries = Sequence.List.Transfers;
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Entries[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &Register, 1);
    Entries[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Data, Length);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Sequence, sizeof(Sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 1 + Length) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

static NTSTATUS SwRtcWrite(WDFIOTARGET Target, UCHAR Register, UCHAR Value)
{
    UCHAR Buffer[2] = { Register, Value };
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, sizeof(Buffer));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    Status = WdfIoTargetSendWriteSynchronously(Target, NULL, &Memory, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != sizeof(Buffer)) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

static UCHAR SwRtcBcd(UCHAR Value, BOOLEAN Binary)
{
    return Binary ? Value : (UCHAR)((Value >> 4) * 10 + (Value & 0x0F));
}

/* RTC time as Windows system time (100 ns since 1601, the RTC's own zone). */
static NTSTATUS SwRtcReadTime(PSWRTC_DEVICE Dev, PLARGE_INTEGER Time)
{
    UCHAR Control = 0, R[7];
    TIME_FIELDS Fields;
    LARGE_INTEGER Delay;
    BOOLEAN Binary;
    CSHORT Hour;
    NTSTATUS Status;

    Status = SwRtcWrite(Dev->I2c, RTC_UPDATE0, RTC_READ_UPDATE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Delay.QuadPart = -16 * 10000;       /* Hekate waits 16 ms after an update */
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    Status = SwRtcRead(Dev->I2c, RTC_CONTROL, &Control, 1);
    if (NT_SUCCESS(Status)) {
        Status = SwRtcRead(Dev->I2c, RTC_SEC, R, sizeof(R));
    }
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    SwRtcTrace(L"RtcControl", Control);
    SwRtcTrace(L"RtcRaw0", R[0] | (R[1] << 8) | (R[2] << 16) | ((ULONG)R[3] << 24));
    SwRtcTrace(L"RtcRaw1", R[4] | (R[5] << 8) | (R[6] << 16));
    Binary = (Control & RTC_BCD) == 0;
    RtlZeroMemory(&Fields, sizeof(Fields));
    Fields.Second = SwRtcBcd(R[0] & 0x7F, Binary);
    Fields.Minute = SwRtcBcd(R[1] & 0x7F, Binary);
    if (Control & RTC_24H) {
        Hour = SwRtcBcd(R[2] & 0x3F, Binary);
    } else {
        Hour = SwRtcBcd(R[2] & 0x1F, Binary) % 12;
        if (R[2] & RTC_HOUR_PM) {
            Hour += 12;
        }
    }
    Fields.Hour = Hour;
    Fields.Month = SwRtcBcd(R[4] & 0x1F, Binary);
    Fields.Year = (CSHORT)(SwRtcBcd(R[5] & 0x7F, Binary) + 2000);
    Fields.Day = SwRtcBcd(R[6] & 0x3F, Binary);
    if (!RtlTimeFieldsToTime(&Fields, Time)) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------- offset handling */

/* Learn OffsetSeconds = system time - RTC after the system time was set. */
VOID SwRtcEvtLearnWork(WDFWORKITEM WorkItem)
{
    PSWRTC_DEVICE Dev = SwRtcGetDevice(SwRtcGetWork(WorkItem)->Device);
    LARGE_INTEGER Rtc, Now;
    LONGLONG Offset;
    NTSTATUS Status;

    Status = SwRtcReadTime(Dev, &Rtc);
    SwRtcTrace(L"LastLearnStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return;
    }
    KeQuerySystemTime(&Now);
    Offset = (Now.QuadPart - Rtc.QuadPart + TICKS_PER_SECOND / 2) / TICKS_PER_SECOND;
    SwRtcTrace64(L"OffsetSeconds", Offset);
    SwRtcTrace(L"OffsetFormat", OFFSET_FORMAT);
    SwRtcTrace(L"Learns", (ULONG)InterlockedIncrement(&Dev->Learns));
}

/* \Callback\SetSystemTime: any IRQL the caller had, so only queue the work. */
VOID SwRtcTimeChanged(PVOID Context, PVOID Argument1, PVOID Argument2)
{
    PSWRTC_DEVICE Dev = (PSWRTC_DEVICE)Context;
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);
    if (Dev->LearnWork != NULL) {
        WdfWorkItemEnqueue(Dev->LearnWork);
    }
}

/* At start: system time = RTC + OffsetSeconds, when an offset was learned. */
static VOID SwRtcRestoreTime(PSWRTC_DEVICE Dev)
{
    LARGE_INTEGER Rtc, Now, NewTime;
    LONGLONG Offset, Diff;
    NTSTATUS Status;

    Status = SwRtcReadTime(Dev, &Rtc);
    SwRtcTrace(L"FirstReadStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return;
    }
    if (!SwRtcReadOffset(&Offset)) {
        SwRtcTrace(L"RestoreResult", 1);    /* no offset yet: wait for the first time sync */
        return;
    }
    KeQuerySystemTime(&Now);
    NewTime.QuadPart = Rtc.QuadPart + Offset * TICKS_PER_SECOND;
    Diff = (NewTime.QuadPart - Now.QuadPart) / TICKS_PER_SECOND;
    SwRtcTrace64(L"BootCorrectionSeconds", Diff);
    if (Diff >= -2 && Diff <= 2) {
        SwRtcTrace(L"RestoreResult", 2);    /* already right */
        return;
    }
    Status = ZwSetSystemTime(&NewTime, NULL);
    SwRtcTrace(L"RestoreResult", NT_SUCCESS(Status) ? 3 : (ULONG)Status);
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwRtcEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWRTC_DEVICE Dev = SwRtcGetDevice(Device);
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

NTSTATUS SwRtcEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWRTC_DEVICE Dev = SwRtcGetDevice(Device);
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    if (Dev->I2c == NULL) {
        Status = SwRtcOpenTarget(Device, Dev->I2cId, &Dev->I2c);
        SwRtcTrace(L"OpenI2cStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    SwRtcRestoreTime(Dev);

    if (Dev->TimeCallback == NULL) {
        RtlInitUnicodeString(&Name, L"\\Callback\\SetSystemTime");
        InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        Status = ExCreateCallback(&Dev->TimeCallbackObject, &Attributes, FALSE, TRUE);
        SwRtcTrace(L"CallbackOpenStatus", (ULONG)Status);
        if (NT_SUCCESS(Status)) {
            Dev->TimeCallback = ExRegisterCallback(Dev->TimeCallbackObject, SwRtcTimeChanged, Dev);
            if (Dev->TimeCallback == NULL) {
                ObDereferenceObject(Dev->TimeCallbackObject);
                Dev->TimeCallbackObject = NULL;
            }
        }
        SwRtcTrace(L"CallbackRegistered", Dev->TimeCallback != NULL);
    }
    return STATUS_SUCCESS;
}

VOID SwRtcEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWRTC_DEVICE Dev = SwRtcGetDevice(Device);
    if (Dev->TimeCallback != NULL) {
        ExUnregisterCallback(Dev->TimeCallback);
        Dev->TimeCallback = NULL;
    }
    if (Dev->TimeCallbackObject != NULL) {
        ObDereferenceObject(Dev->TimeCallbackObject);
        Dev->TimeCallbackObject = NULL;
    }
    if (Dev->LearnWork != NULL) {
        WdfWorkItemFlush(Dev->LearnWork);
    }
    if (Dev->I2c != NULL) {
        WdfIoTargetClose(Dev->I2c);
        WdfObjectDelete(Dev->I2c);
        Dev->I2c = NULL;
    }
}

NTSTATUS SwRtcEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_WORKITEM_CONFIG WorkConfig;
    WDFDEVICE Device;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwRtcEvtPrepareHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwRtcEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwRtcEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWRTC_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_WORKITEM_CONFIG_INIT(&WorkConfig, SwRtcEvtLearnWork);
    WorkConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWRTC_WORK);
    Attributes.ParentObject = Device;
    Status = WdfWorkItemCreate(&WorkConfig, &Attributes, &SwRtcGetDevice(Device)->LearnWork);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    SwRtcGetWork(SwRtcGetDevice(Device)->LearnWork)->Device = Device;
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwRtcParamsPath)) {
        RtlCopyMemory(SwRtcParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwRtcParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwRtcParamsPath);
    }
    SwRtcTrace(L"DriverVersion", 2);
    WDF_DRIVER_CONFIG_INIT(&Config, SwRtcEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
