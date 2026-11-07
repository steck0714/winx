/* Switch Lite SIO gamepad and accelerometer on UART-C, ACPI\SWOA0902. */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <acpiioct.h>
#include <gpio.h>
#include <vhf.h>
#include "sensor_status.h"
#include "sio_protocol.h"

enum { Data = 0, Ier, Fifo, Lcr, Mcr, Lsr, Msr, Scratch, Irda };
enum { RxCapacity = 1024, PollMs = 8, ResponseMs = 300 };
#define MS_TO_100NS(ms) ((ULONGLONG)(ms) * 10000)

static UCHAR InputDescriptor[] = {
    0x05,0x01, 0x09,0x05, 0xA1,0x01, 0x85,0x01, /* Game Pad, report 1 */
    0x05,0x09, 0x19,0x01, 0x29,0x0E, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x0E, 0x81,0x02, 0x95,0x02, 0x81,0x03,
    0x05,0x01, 0x09,0x39, 0x15,0x00, 0x25,0x07,
    0x75,0x04, 0x95,0x01, 0x81,0x42, 0x81,0x03, /* hat with null */
    0x09,0x30, 0x09,0x31, 0x09,0x33, 0x09,0x34,
    0x16,0x01,0x80, 0x26,0xFF,0x7F, 0x75,0x10, 0x95,0x04, 0x81,0x02, 0xC0,

    /* A separate mouse collection lets mouhid consume relative reports. */
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,0x02, /* Mouse, report 2 */
    0x09,0x01, 0xA1,0x00,                        /* Pointer */
    0x05,0x09, 0x19,0x01, 0x29,0x02, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x02, 0x81,0x02,             /* left and right buttons */
    0x75,0x06, 0x95,0x01, 0x81,0x03,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7F,
    0x75,0x08, 0x95,0x02, 0x81,0x06, 0xC0,0xC0   /* signed relative X/Y */
};
#pragma pack(push, 1)
typedef struct { UCHAR Id; USHORT Buttons; UCHAR Hat; SHORT Axis[4]; } PAD_REPORT;
#pragma pack(pop)
C_ASSERT(sizeof(PAD_REPORT) == 12);
C_ASSERT(sizeof(SIO_MOUSE_REPORT) == 4);

typedef struct {
    WDFDEVICE Device;
    WDFINTERRUPT Interrupt;
    WDFTIMER Timer;
    WDFIOTARGET Home;
    WDFSPINLOCK SnapshotLock;
    KSPIN_LOCK TimerLock;
    volatile ULONG *Uart;
    VHFHANDLE Vhf;
    SWSENS_STATUS Sensor;
    UCHAR Rx[RxCapacity];
    ULONG RxHead, RxCount;  /* interrupt lock */
    BOOLEAN RxLost, Powered, Running;
    SIO_STREAM Stream;      /* passive timer, stopped before D0 exit */
    UCHAR Command;
    ULONGLONG LastReply, NextSend, NextVhf;
    BOOLEAN HomeDown, Delivered;
    PAD_REPORT LastPad;
    ULONGLONG LastMouseTime;
    SIO_POINTER_STATE Pointer;
    UCHAR MouseButtons;  /* last state accepted by VHF */
    ULONG ReportedState;
} SIO_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SIO_DEVICE, SioDevice)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD Add;
static EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
static EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
static EVT_WDF_DEVICE_D0_ENTRY EnterD0;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED Start;
static EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED Stop;
static EVT_WDF_DEVICE_D0_EXIT ExitD0;
static EVT_WDF_INTERRUPT_ISR Receive;
static EVT_WDF_INTERRUPT_ENABLE Enable;
static EVT_WDF_INTERRUPT_DISABLE Disable;
static EVT_WDF_TIMER Poll;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL ReadSensor;

static ULONG ReadReg(SIO_DEVICE *D, ULONG Reg) { return READ_REGISTER_ULONG(D->Uart + Reg); }
static VOID WriteReg(SIO_DEVICE *D, ULONG Reg, ULONG Value) { WRITE_REGISTER_ULONG(D->Uart + Reg, Value); }

static VOID TraceState(SIO_DEVICE *D, ULONG State)
{
    WDFKEY Key;
    DECLARE_CONST_UNICODE_STRING(Name, L"SioState");
    if (State == D->ReportedState) return;
    D->ReportedState = State;
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(D->Device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE,
                                           WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
        (VOID)WdfRegistryAssignULong(Key, &Name, State);
        WdfRegistryClose(Key);
    }
}

static NTSTATUS BoardPower(WDFDEVICE Device, ULONG On)
{
    ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER Input = {0};
    ACPI_EVAL_OUTPUT_BUFFER Reply = {0};
    WDF_MEMORY_DESCRIPTOR In, Out;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE;
    RtlCopyMemory(Input.MethodName, "SPWR", 4);
    Input.IntegerArgument = On;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&In, &Input, sizeof(Input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Out, &Reply, sizeof(Reply));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT | WDF_REQUEST_SEND_OPTION_IGNORE_TARGET_STATE);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options, WDF_REL_TIMEOUT_IN_MS(1500));
    NTSTATUS Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device), NULL,
        IOCTL_ACPI_EVAL_METHOD, &In, &Out, &Options, &Bytes);
    if (!NT_SUCCESS(Status)) return Status;
    if (Bytes < sizeof(Reply) || Reply.Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE ||
        Reply.Count != 1 || Reply.Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER ||
        Reply.Argument[0].DataLength != sizeof(ULONG) || Reply.Argument[0].Argument != 1)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS OpenHome(SIO_DEVICE *D, LARGE_INTEGER Id)
{
    WCHAR Text[64];
    UNICODE_STRING Path;
    WDF_IO_TARGET_OPEN_PARAMS Open;
    RtlInitEmptyUnicodeString(&Path, Text, sizeof(Text));
    NTSTATUS Status = RtlUnicodeStringPrintf(&Path, L"\\Device\\RESOURCE_HUB\\%016I64x", (ULONG64)Id.QuadPart);
    if (!NT_SUCCESS(Status)) return Status;
    Status = WdfIoTargetCreate(D->Device, WDF_NO_OBJECT_ATTRIBUTES, &D->Home);
    if (!NT_SUCCESS(Status)) return Status;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Open, &Path, GENERIC_READ);
    return WdfIoTargetOpen(D->Home, &Open);
}

static VOID SampleHome(SIO_DEVICE *D)
{
    UCHAR Pins = 0;
    ULONG_PTR Bytes = 0;
    WDF_MEMORY_DESCRIPTOR Out;
    WDF_REQUEST_SEND_OPTIONS Options;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Out, &Pins, sizeof(Pins));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options, WDF_REL_TIMEOUT_IN_MS(30));
    if (NT_SUCCESS(WdfIoTargetSendIoctlSynchronously(D->Home, NULL, IOCTL_GPIO_READ_PINS,
        NULL, &Out, &Options, &Bytes)) && Bytes == sizeof(Pins))
        D->HomeDown = !(Pins & 1);
}

static NTSTATUS StartHid(SIO_DEVICE *D)
{
    VHF_CONFIG C;
    VHF_CONFIG_INIT(&C, WdfDeviceWdmGetDeviceObject(D->Device), sizeof(InputDescriptor), InputDescriptor);
    NTSTATUS Status = VhfCreate(&C, &D->Vhf);
    if (NT_SUCCESS(Status)) {
        Status = VhfStart(D->Vhf);
        if (!NT_SUCCESS(Status)) {
            VhfDelete(D->Vhf, TRUE);
            D->Vhf = NULL;
        }
    } else D->Vhf = NULL;
    return Status;
}

static VOID SubmitPad(SIO_DEVICE *D, const SIO_PAD *Pad)
{
    PAD_REPORT Report;
    HID_XFER_PACKET Xfer;
    if (!D->Vhf) return;
    Report.Id = 1;
    Report.Buttons = Pad->buttons | (D->HomeDown ? (1u << 12) : 0);
    Report.Hat = Pad->hat;
    RtlCopyMemory(Report.Axis, Pad->axis, sizeof(Report.Axis));
    if (D->Delivered && RtlEqualMemory(&D->LastPad, &Report, sizeof(Report))) return;
    Xfer.reportId = Report.Id;
    Xfer.reportBuffer = (PUCHAR)&Report;
    Xfer.reportBufferLen = sizeof(Report);
    if (NT_SUCCESS(VhfReadReportSubmit(D->Vhf, &Xfer))) {
        D->LastPad = Report;
        D->Delivered = TRUE;
    }
}

static VOID SendMouseReport(SIO_DEVICE *D, SIO_MOUSE_REPORT *Report)
{
    HID_XFER_PACKET xfer;
    if (!D->Vhf || (!Report->dx && !Report->dy && Report->buttons == D->MouseButtons)) return;
    xfer.reportId = Report->id;
    xfer.reportBuffer = (PUCHAR)Report;
    xfer.reportBufferLen = sizeof(*Report);
    if (NT_SUCCESS(VhfReadReportSubmit(D->Vhf, &xfer)))
        D->MouseButtons = Report->buttons;
}

static VOID SubmitMouse(SIO_DEVICE *D, const SIO_PAD *Pad, ULONGLONG Now)
{
    ULONGLONG delta = D->LastMouseTime ? Now - D->LastMouseTime : 0;
    unsigned ms = (unsigned)(min(delta, MS_TO_100NS(32)) / 10000);
    SIO_MOUSE_REPORT report = SioPointerReport(&D->Pointer, Pad, ms);
    D->LastMouseTime = Now;
    SendMouseReport(D, &report);
}

static VOID Neutral(SIO_DEVICE *D)
{
    const SIO_PAD Pad = {0, SioNeutralHat, {0,0,0,0}};
    D->HomeDown = FALSE;
    SIO_MOUSE_REPORT mouse = {2, 0, 0, 0};
    D->LastMouseTime = 0;
    SioPointerResync(&D->Pointer);
    SendMouseReport(D, &mouse);
    SubmitPad(D, &Pad);
    WdfSpinLockAcquire(D->SnapshotLock);
    D->Sensor.Flags = 0;
    WdfSpinLockRelease(D->SnapshotLock);
}

static VOID Frame(SIO_DEVICE *D, ULONGLONG Now)
{
    const UCHAR *P = D->Stream.bytes;
    if (P[1] != (D->Command | SioAck) || P[4] != 0) return;
    if (D->Command != SioInput) {
        D->Command = D->Command == SioInit ? SioVersion : SioInput;
        D->LastReply = Now;
        D->NextSend = 0;
        TraceState(D, D->Command);
        return;
    }
    SIO_PAD Pad;
    SIO_MOTION Motion;
    if (!SioInputReport(P + SioHeaderBytes, D->Stream.total - SioHeaderBytes, &Pad, &Motion)) return;
    D->LastReply = Now;
    if (!D->Vhf && Now >= D->NextVhf) {
        NTSTATUS Status = StartHid(D);
        if (!NT_SUCCESS(Status)) TraceState(D, (ULONG)Status);
        D->NextVhf = Now + MS_TO_100NS(1000);
    }
    if (D->Vhf) {
        TraceState(D, SioInput | SioAck);
        SubmitMouse(D, &Pad, Now);
        SampleHome(D);
        SubmitPad(D, &Pad);
    }
    if (Motion.count) {
        WdfSpinLockAcquire(D->SnapshotLock);
        D->Sensor.Flags = SENS_IMU_VALID;
        D->Sensor.AccelX = Motion.accelMg[0];
        D->Sensor.AccelY = Motion.accelMg[1];
        D->Sensor.AccelZ = Motion.accelMg[2];
        D->Sensor.Samples += Motion.count;
        WdfSpinLockRelease(D->SnapshotLock);
    } else if (D->Stream.total >= SioHeaderBytes + 13 && (P[SioHeaderBytes + 12] >> 4) == 15) {
        WdfSpinLockAcquire(D->SnapshotLock);
        D->Sensor.Flags = 0;
        WdfSpinLockRelease(D->SnapshotLock);
    }
}

static BOOLEAN Receive(WDFINTERRUPT Interrupt, ULONG Message)
{
    SIO_DEVICE *D = SioDevice(WdfInterruptGetDevice(Interrupt));
    UNREFERENCED_PARAMETER(Message);
    if (!D->Powered || (ReadReg(D, Fifo) & 1)) return FALSE;
    for (ULONG i = 0; i < 128; ++i) {
        ULONG Status = ReadReg(D, Lsr);
        if (Status & 0x1E) D->RxLost = TRUE;
        if (!(Status & 1)) break;
        UCHAR Byte = (UCHAR)ReadReg(D, Data);
        if (D->RxCount < RxCapacity) {
            D->Rx[(D->RxHead + D->RxCount) % RxCapacity] = Byte;
            ++D->RxCount;
        } else D->RxLost = TRUE;
    }
    return TRUE;
}

static NTSTATUS Enable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Interrupt);
    WriteReg(SioDevice(Device), Ier, 0x15); /* Tegra PIO: RX data, timeout and line status */
    return STATUS_SUCCESS;
}

static NTSTATUS Disable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Interrupt);
    WriteReg(SioDevice(Device), Ier, 0);
    return STATUS_SUCCESS;
}

static VOID Poll(WDFTIMER Timer)
{
    SIO_DEVICE *D = SioDevice((WDFDEVICE)WdfTimerGetParentObject(Timer));
    UCHAR Bytes[256];
    ULONGLONG Now = KeQueryInterruptTime();
    ULONG Count;
    /* All parsing, GPIO I/O and HID submissions run after releasing the IRQ lock. */
    for (ULONG Batch = 0; Batch < RxCapacity / sizeof(Bytes); ++Batch) {
        WdfInterruptAcquireLock(D->Interrupt);
        if (D->RxLost) {
            D->RxLost = FALSE;
            D->RxCount = D->RxHead = 0;
            D->Stream.used = 0;
        }
        Count = min(D->RxCount, (ULONG)sizeof(Bytes));
        ULONG First = min(Count, RxCapacity - D->RxHead);
        RtlCopyMemory(Bytes, D->Rx + D->RxHead, First);
        RtlCopyMemory(Bytes + First, D->Rx, Count - First);
        D->RxHead = (D->RxHead + Count) % RxCapacity;
        D->RxCount -= Count;
        WdfInterruptReleaseLock(D->Interrupt);
        for (ULONG i = 0; i < Count; ++i)
            if (SioPush(&D->Stream, Bytes[i])) Frame(D, Now);
        if (Count < sizeof(Bytes)) break;
    }
    if (D->Command != SioInit && Now - D->LastReply >= MS_TO_100NS(ResponseMs)) {
        Neutral(D);
        D->Command = SioInit;
        D->NextSend = 0;
        D->Stream.used = 0;
        TraceState(D, SioInit);
    } else if (D->Command != SioInput && D->Vhf && D->MouseButtons) {
        Neutral(D); /* Retry a release that VHF could not accept before the link went down. */
    }
    if (Now >= D->NextSend && (ReadReg(D, Lsr) & 0x20)) {
        UCHAR Command[SioHeaderBytes];
        SioCommand(D->Command, Command);
        for (ULONG i = 0; i < sizeof(Command); ++i) WriteReg(D, Data, Command[i]);
        D->NextSend = Now + MS_TO_100NS(D->Command == SioInput ? PollMs : 100);
    }
    KIRQL Irql;
    KeAcquireSpinLock(&D->TimerLock, &Irql);
    if (D->Running) WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(D->Command == SioInput ? PollMs : 25));
    KeReleaseSpinLock(&D->TimerLock, Irql);
}

static NTSTATUS EnterD0(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    SIO_DEVICE *D = SioDevice(Device);
    UNREFERENCED_PARAMETER(Previous);
    NTSTATUS Status = BoardPower(Device, 1);
    if (!NT_SUCCESS(Status)) {
        (VOID)BoardPower(Device, 0);
        return Status;
    }
    WriteReg(D, Lcr, 3);
    WriteReg(D, Ier, 0);
    WriteReg(D, Lcr, 0x83);
    WriteReg(D, Data, 1);
    WriteReg(D, Ier, 0);
    WriteReg(D, Lcr, 3); /* 16 MHz / 16, 8N1 */
    WriteReg(D, Irda, 0);
    WriteReg(D, Fifo, 0x47); /* clear both FIFOs, four-byte RX trigger */
    WriteReg(D, Mcr, 0x62); /* RTS, hardware CTS/RTS */
    D->Stream.used = 0;
    D->RxHead = D->RxCount = 0;
    D->RxLost = D->Delivered = D->HomeDown = FALSE;
    D->Command = SioInit;
    D->LastReply = D->NextSend = D->NextVhf = D->LastMouseTime = 0;
    SioPointerResync(&D->Pointer);
    D->MouseButtons = 0xff; /* Submit the first button state even after a resume. */
    D->Powered = TRUE;
    TraceState(D, SioInit);
    return STATUS_SUCCESS;
}

static NTSTATUS Start(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    SIO_DEVICE *D = SioDevice(Device);
    KIRQL Irql;
    UNREFERENCED_PARAMETER(Previous);
    KeAcquireSpinLock(&D->TimerLock, &Irql);
    D->Running = TRUE;
    WdfTimerStart(D->Timer, WDF_REL_TIMEOUT_IN_MS(1));
    KeReleaseSpinLock(&D->TimerLock, Irql);
    return STATUS_SUCCESS;
}

static NTSTATUS Stop(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    SIO_DEVICE *D = SioDevice(Device);
    KIRQL Irql;
    UNREFERENCED_PARAMETER(Target);
    KeAcquireSpinLock(&D->TimerLock, &Irql);
    D->Running = FALSE;
    KeReleaseSpinLock(&D->TimerLock, Irql);
    WdfTimerStop(D->Timer, TRUE);
    Neutral(D);
    return STATUS_SUCCESS;
}

static NTSTATUS ExitD0(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    SIO_DEVICE *D = SioDevice(Device);
    UNREFERENCED_PARAMETER(Target);
    D->Powered = FALSE;
    return BoardPower(Device, 0);
}

static NTSTATUS Release(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    SIO_DEVICE *D = SioDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (D->Vhf) { VhfDelete(D->Vhf, TRUE); D->Vhf = NULL; }
    if (D->Home) { WdfObjectDelete(D->Home); D->Home = NULL; }
    if (D->Uart) { MmUnmapIoSpace((PVOID)D->Uart, 0x40); D->Uart = NULL; }
    return STATUS_SUCCESS;
}

static NTSTATUS Prepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    SIO_DEVICE *D = SioDevice(Device);
    LARGE_INTEGER Uart = {0}, Home = {0};
    BOOLEAN HaveUart = FALSE, HaveHome = FALSE;
    for (ULONG i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR T = WdfCmResourceListGetDescriptor(Translated, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR R = WdfCmResourceListGetDescriptor(Raw, i);
        if (!T || !R) continue;
        if (R->Type == CmResourceTypeMemory && T->Type == CmResourceTypeMemory &&
            R->u.Memory.Start.QuadPart == 0x70006200 && T->u.Memory.Length >= 0x40) {
            Uart = T->u.Memory.Start;
            HaveUart = TRUE;
        } else if (T->Type == CmResourceTypeConnection &&
            T->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
            T->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO) {
            Home.LowPart = T->u.Connection.IdLowPart;
            Home.HighPart = (LONG)T->u.Connection.IdHighPart;
            HaveHome = TRUE;
        }
    }
    if (!HaveUart || !HaveHome) return STATUS_DEVICE_CONFIGURATION_ERROR;
    D->Uart = MmMapIoSpaceEx(Uart, 0x40, PAGE_READWRITE | PAGE_NOCACHE);
    if (!D->Uart) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS Status = OpenHome(D, Home);
    if (!NT_SUCCESS(Status)) Release(Device, Translated);
    return Status;
}

static VOID ReadSensor(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    SIO_DEVICE *D = SioDevice(WdfIoQueueGetDevice(Queue));
    UNREFERENCED_PARAMETER(OutLength);
    UNREFERENCED_PARAMETER(InLength);
    SwSensReturnStatus(Request, Code, D->SnapshotLock, &D->Sensor);
}

static NTSTATUS Add(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_INTERRUPT_CONFIG Irq;
    WDF_TIMER_CONFIG Timer;
    WDF_IO_QUEUE_CONFIG Queue;
    WDF_OBJECT_ATTRIBUTES A;
    WDFDEVICE Device;
    DECLARE_CONST_UNICODE_STRING(Name, L"\\Device\\SwitchSensor");
    DECLARE_CONST_UNICODE_STRING(Link, L"\\DosDevices\\SwitchSensor");
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = Prepare;
    Pnp.EvtDeviceReleaseHardware = Release;
    Pnp.EvtDeviceD0Entry = EnterD0;
    Pnp.EvtDeviceD0EntryPostInterruptsEnabled = Start;
    Pnp.EvtDeviceD0ExitPreInterruptsDisabled = Stop;
    Pnp.EvtDeviceD0Exit = ExitD0;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Pnp);
    NTSTATUS Status = WdfDeviceInitAssignName(Init, &Name);
    if (!NT_SUCCESS(Status)) return Status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A, SIO_DEVICE);
    Status = WdfDeviceCreate(&Init, &A, &Device);
    if (!NT_SUCCESS(Status)) return Status;
    SIO_DEVICE *D = SioDevice(Device);
    D->Device = Device;
    KeInitializeSpinLock(&D->TimerLock);
    WDF_OBJECT_ATTRIBUTES_INIT(&A);
    A.ParentObject = Device;
    Status = WdfSpinLockCreate(&A, &D->SnapshotLock);
    if (!NT_SUCCESS(Status)) return Status;
    WDF_INTERRUPT_CONFIG_INIT(&Irq, Receive, NULL);
    Irq.EvtInterruptEnable = Enable;
    Irq.EvtInterruptDisable = Disable;
    Status = WdfInterruptCreate(Device, &Irq, WDF_NO_OBJECT_ATTRIBUTES, &D->Interrupt);
    if (!NT_SUCCESS(Status)) return Status;
    WDF_TIMER_CONFIG_INIT(&Timer, Poll);
    Timer.AutomaticSerialization = FALSE;
    A.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfTimerCreate(&Timer, &A, &D->Timer);
    if (!NT_SUCCESS(Status)) return Status;
    Status = WdfDeviceCreateSymbolicLink(Device, &Link);
    if (!NT_SUCCESS(Status)) return Status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Queue, WdfIoQueueDispatchParallel);
    Queue.EvtIoDeviceControl = ReadSensor;
    return WdfIoQueueCreate(Device, &Queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Object, PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG C;
    WDF_DRIVER_CONFIG_INIT(&C, Add);
    return WdfDriverCreate(Object, Path, WDF_NO_OBJECT_ATTRIBUTES, &C, WDF_NO_HANDLE);
}
