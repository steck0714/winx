/*
 * Nintendo Switch touch screen driver, KMDF + Virtual HID Framework (VHF).
 *
 * ACPI device TCH0 (ACPI\SWOA0501):
 *   I2cSerialBus 0x49 on I2C3: STM FTM4 (chip ID 0x3670).
 *   GpioInt PX1 (GPIO 185), level low.
 * UEFI R373+ powers the panel (LDO6 2.9 V, reset PJ7 high), resets it and
 * turns finger sensing on. This driver clears the event stack, enables the
 * controller interrupt, reads 8-byte events and reports them to Windows as
 * a HID multi-touch screen (parallel mode, 10 contacts) through vhf.sys.
 *
 * Events (Hekate bdk/input/touch.c):
 *   count = HW reg 0x0023 byte 1 >> 1; one event = cmd 0x85, 8 bytes.
 *   raw[0] low nibble: 3 enter, 4 leave, 5 motion; high nibble: touch id.
 *   x = raw[1] << 4 | raw[3] >> 4, y = raw[2] << 4 | (raw[3] & 0xF)
 *   (landscape, 15..1264 x 15..704, scaled to 1280 x 720).
 * The display is 720 x 1280 portrait, so HID X comes from the landscape y
 * and HID Y from the landscape x; Parameters\CfgInvertX / CfgInvertY flip
 * them (Touch.ps1 -Mode Calibrate sets them from one corner touch).
 *
 * The interrupt is the main path. A one-shot passive timer also polls: every
 * 12 ms while no interrupt has been seen, every 250 ms once they work.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>
#include <vhf.h>

#define TOUCH_MAX_CONTACTS   10
#define TOUCH_WIDTH          720      /* HID X: portrait */
#define TOUCH_HEIGHT         1280     /* HID Y: portrait */
#define X_REAL_MAX           1264
#define Y_REAL_MAX           704
#define EDGE_OFFSET          15
#define EV_ENTER             0x03
#define EV_LEAVE             0x04
#define EV_MOTION            0x05
#define CMD_READ_ONE_EVENT   0x85
#define CMD_CLEAR_STACK      0xA1
#define DEFAULT_INT_REG      0x2C     /* FTM4 HW reg: interrupt enable */
#define INT_ENABLE_VALUE     0x41
#define POLL_FAST_MS         12
#define JITTER_UNITS         12       /* ~1.2 mm on the 720 x 1280 grid */
#define ABS_DIFF(a, b)       ((a) > (b) ? (ULONG)((a) - (b)) : (ULONG)((b) - (a)))
#define POLL_SLOW_MS         250
#define REPORT_ID_TOUCH      1
#define REPORT_ID_MAX_COUNT  2

#pragma pack(push, 1)
typedef struct _TOUCH_CONTACT_REPORT {
    UCHAR Flags;            /* bit 0 tip switch, bit 1 in range */
    UCHAR Id;
    USHORT X;
    USHORT Y;
} TOUCH_CONTACT_REPORT;

typedef struct _TOUCH_REPORT {
    UCHAR ReportId;
    TOUCH_CONTACT_REPORT Contacts[TOUCH_MAX_CONTACTS];
    UCHAR ContactCount;
} TOUCH_REPORT;
#pragma pack(pop)

/* One finger: tip + in range (2 bits, 6 pad), id (8), X (16), Y (16). */
#define FINGER_COLLECTION                                                    \
    0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,                                      \
    0x09, 0x42, 0x09, 0x32, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02,  \
    0x81, 0x02, 0x95, 0x06, 0x81, 0x03,                                      \
    0x09, 0x51, 0x25, 0x0F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,              \
    0x05, 0x01, 0x55, 0x0E, 0x65, 0x11, 0x75, 0x10, 0x95, 0x01,              \
    0x09, 0x30, 0x26, 0xCF, 0x02, 0x46, 0x02, 0x03, 0x81, 0x02,              \
    0x09, 0x31, 0x26, 0xFF, 0x04, 0x46, 0x5A, 0x05, 0x81, 0x02,              \
    0x45, 0x00, 0x55, 0x00, 0x65, 0x00,                                      \
    0xC0

/* Touch screen, 0..719 x 0..1279, 77 x 137 mm (6.2" 16:9). */
static UCHAR SwTouchReportDescriptor[] = {
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01,
    0x85, REPORT_ID_TOUCH,
    FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION,
    FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION,
    0x05, 0x0D, 0x09, 0x54, 0x15, 0x00, 0x25, TOUCH_MAX_CONTACTS, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0x85, REPORT_ID_MAX_COUNT,
    0x09, 0x55, 0x25, TOUCH_MAX_CONTACTS, 0xB1, 0x02,
    0xC0
};

typedef struct _TOUCH_CONTACT {
    BOOLEAN Active;
    BOOLEAN Released;       /* report once with tip = 0 */
    BOOLEAN Moving;         /* left the dead zone since touch-down */
    USHORT X;
    USHORT Y;
} TOUCH_CONTACT;

typedef struct _SWTOUCH_DEVICE {
    LARGE_INTEGER I2cId;
    WDFIOTARGET I2c;
    WDFINTERRUPT Interrupt;
    WDFWAITLOCK Lock;
    WDFTIMER Timer;
    VHFHANDLE Vhf;
    volatile BOOLEAN VhfReady;
    volatile BOOLEAN Stopping;
    TOUCH_CONTACT Contacts[TOUCH_MAX_CONTACTS];
    TOUCH_REPORT Report;
    ULONG InvertX;
    ULONG InvertY;
    ULONG IntReg;
    LONG Irqs;
    LONG IrqsAtLastPoll;
    LONG Polls;
    LONG Events;
    LONG Reports;
    LONG Errors;
    LONG Held;              /* motion events swallowed by the jitter dead zone */
} SWTOUCH_DEVICE, *PSWTOUCH_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWTOUCH_DEVICE, SwTouchGetDevice)

typedef struct _SWTOUCH_TIMER {
    WDFDEVICE Device;
} SWTOUCH_TIMER, *PSWTOUCH_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWTOUCH_TIMER, SwTouchGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwTouchEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwTouchEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwTouchEvtReleaseHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwTouchEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwTouchEvtSelfManagedIoCleanup;
EVT_WDF_INTERRUPT_ISR SwTouchEvtIsr;
EVT_WDF_TIMER SwTouchEvtTimer;
EVT_VHF_ASYNC_OPERATION SwTouchEvtGetFeature;

static WCHAR SwTouchParamsPath[256];

static VOID SwTouchTrace(PCWSTR Name, ULONG Value)
{
    if (SwTouchParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwTouchParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

static ULONG SwTouchConfig(PCWSTR Name, ULONG Default)
{
    ULONG Value = Default;
    RTL_QUERY_REGISTRY_TABLE Table[2];

    if (SwTouchParamsPath[0] == L'\0') {
        return Default;
    }
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, SwTouchParamsPath, Table, NULL, NULL))) {
        Value = Default;
    }
    return Value;
}

/* ------------------------------------------------------------------- I2C */

static NTSTATUS SwTouchOpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
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

/* Write Cmd, then (if Length != 0) repeated-start read Length bytes. */
static NTSTATUS SwTouchXfer(PSWTOUCH_DEVICE Dev, PUCHAR Cmd, ULONG CmdLength, PUCHAR Data, ULONG Length)
{
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Dev->I2c == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    if (Length == 0) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Cmd, CmdLength);
        Status = WdfIoTargetSendWriteSynchronously(Dev->I2c, NULL, &Memory, NULL, &Options, &Bytes);
        if (NT_SUCCESS(Status) && Bytes != CmdLength) {
            Status = STATUS_DEVICE_PROTOCOL_ERROR;
        }
    } else {
        SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
        PSPB_TRANSFER_LIST_ENTRY Entries = Sequence.List.Transfers;
        SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
        Entries[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, Cmd, CmdLength);
        Entries[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Data, Length);
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, &Sequence, sizeof(Sequence));
        Status = WdfIoTargetSendIoctlSynchronously(Dev->I2c, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Memory, NULL, &Options, &Bytes);
        if (NT_SUCCESS(Status) && Bytes != CmdLength + Length) {
            Status = STATUS_DEVICE_PROTOCOL_ERROR;
        }
    }
    return Status;
}

/* ----------------------------------------------------------------- touch */

static USHORT SwTouchScale(ULONG Raw, ULONG RealMax, ULONG Size)
{
    if (Raw < EDGE_OFFSET) Raw = EDGE_OFFSET;
    if (Raw > RealMax) Raw = RealMax;
    Raw = (Raw - EDGE_OFFSET) * Size / (RealMax - EDGE_OFFSET);
    return (USHORT)((Raw >= Size) ? Size - 1 : Raw);
}

static BOOLEAN SwTouchParse(PSWTOUCH_DEVICE Dev, const UCHAR *Ev)
{
    ULONG Type = Ev[0] & 0x0F, Id = Ev[0] >> 4;
    USHORT Lx, Ly, X, Y;
    TOUCH_CONTACT *C;

    if ((Type != EV_ENTER && Type != EV_MOTION && Type != EV_LEAVE) || Id >= TOUCH_MAX_CONTACTS) {
        return FALSE;
    }
    C = &Dev->Contacts[Id];
    if (Type == EV_LEAVE) {
        if (C->Active) {
            C->Active = FALSE;
            C->Released = TRUE;
        }
        return TRUE;
    }
    Lx = SwTouchScale(((ULONG)Ev[1] << 4) | (Ev[3] >> 4), X_REAL_MAX, TOUCH_HEIGHT);
    Ly = SwTouchScale(((ULONG)Ev[2] << 4) | (Ev[3] & 0x0F), Y_REAL_MAX, TOUCH_WIDTH);
    X = Dev->InvertX ? (USHORT)(TOUCH_WIDTH - 1 - Ly) : Ly;
    Y = Dev->InvertY ? (USHORT)(TOUCH_HEIGHT - 1 - Lx) : Lx;
    /* A resting finger still jitters, and Windows cancels press-and-hold on
     * any movement: keep the touch-down point until the finger really moves. */
    if (C->Active && !C->Moving) {
        if (ABS_DIFF(X, C->X) <= JITTER_UNITS && ABS_DIFF(Y, C->Y) <= JITTER_UNITS) {
            InterlockedIncrement(&Dev->Held);
            return FALSE;
        }
        C->Moving = TRUE;
    }
    if (!C->Active || Type == EV_ENTER) {
        C->Moving = FALSE;
    }
    C->X = X;
    C->Y = Y;
    C->Active = TRUE;
    C->Released = FALSE;
    SwTouchTrace(L"LastLandscapeX", Lx);
    SwTouchTrace(L"LastLandscapeY", Ly);
    return TRUE;
}

static VOID SwTouchSendReport(PSWTOUCH_DEVICE Dev)
{
    HID_XFER_PACKET Packet;
    ULONG Id, Count = 0;
    NTSTATUS Status;

    RtlZeroMemory(&Dev->Report, sizeof(Dev->Report));
    Dev->Report.ReportId = REPORT_ID_TOUCH;
    for (Id = 0; Id < TOUCH_MAX_CONTACTS; ++Id) {
        TOUCH_CONTACT *C = &Dev->Contacts[Id];
        if (!C->Active && !C->Released) {
            continue;
        }
        Dev->Report.Contacts[Count].Flags = C->Active ? 0x03 : 0x00;
        Dev->Report.Contacts[Count].Id = (UCHAR)Id;
        Dev->Report.Contacts[Count].X = C->X;
        Dev->Report.Contacts[Count].Y = C->Y;
        C->Released = FALSE;
        ++Count;
    }
    if (Count == 0 || !Dev->VhfReady) {
        return;
    }
    Dev->Report.ContactCount = (UCHAR)Count;
    Packet.reportBuffer = (PUCHAR)&Dev->Report;
    Packet.reportBufferLen = sizeof(Dev->Report);
    Packet.reportId = REPORT_ID_TOUCH;
    Status = VhfReadReportSubmit(Dev->Vhf, &Packet);
    if (NT_SUCCESS(Status)) {
        InterlockedIncrement(&Dev->Reports);
    } else {
        SwTouchTrace(L"LastReportError", (ULONG)Status);
    }
}

/* Drain the controller's event stack and report what changed. PASSIVE_LEVEL. */
static VOID SwTouchService(PSWTOUCH_DEVICE Dev)
{
    UCHAR CountCmd[3] = { 0xB6, 0x00, 0x23 }, ReadCmd = CMD_READ_ONE_EVENT;
    UCHAR CountBuf[2] = { 0, 0 }, Ev[8];
    ULONG Count, Index;
    BOOLEAN Changed = FALSE;
    NTSTATUS Status;

    WdfWaitLockAcquire(Dev->Lock, NULL);
    Status = SwTouchXfer(Dev, CountCmd, sizeof(CountCmd), CountBuf, sizeof(CountBuf));
    if (!NT_SUCCESS(Status)) {
        InterlockedIncrement(&Dev->Errors);
        SwTouchTrace(L"LastI2cError", (ULONG)Status);
        goto Done;
    }
    Count = CountBuf[1] >> 1;
    if (Count > 32) {
        Count = 32;
    }
    for (Index = 0; Index < Count; ++Index) {
        Status = SwTouchXfer(Dev, &ReadCmd, 1, Ev, sizeof(Ev));
        if (!NT_SUCCESS(Status)) {
            InterlockedIncrement(&Dev->Errors);
            SwTouchTrace(L"LastI2cError", (ULONG)Status);
            break;
        }
        InterlockedIncrement(&Dev->Events);
        if (SwTouchParse(Dev, Ev)) {
            Changed = TRUE;
        }
    }
    if (Changed) {
        SwTouchSendReport(Dev);
    }
Done:
    WdfWaitLockRelease(Dev->Lock);
}

BOOLEAN SwTouchEvtIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    PSWTOUCH_DEVICE Dev = SwTouchGetDevice(WdfInterruptGetDevice(Interrupt));
    UNREFERENCED_PARAMETER(MessageId);
    InterlockedIncrement(&Dev->Irqs);
    SwTouchService(Dev);
    return TRUE;
}

VOID SwTouchEvtTimer(WDFTIMER Timer)
{
    PSWTOUCH_DEVICE Dev = SwTouchGetDevice(SwTouchGetTimer(Timer)->Device);
    LONG Irqs = Dev->Irqs, Polls;
    BOOLEAN IrqWorks = (Irqs != Dev->IrqsAtLastPoll), Touching = FALSE;
    ULONG Id;

    Dev->IrqsAtLastPoll = Irqs;
    SwTouchService(Dev);
    /* The FTM4 sends nothing while a finger rests still, but Windows needs a
     * steady report stream to see press-and-hold (right click): repeat the
     * current contacts every POLL_FAST_MS while any finger is down. */
    WdfWaitLockAcquire(Dev->Lock, NULL);
    for (Id = 0; Id < TOUCH_MAX_CONTACTS; ++Id) {
        Touching = Touching || Dev->Contacts[Id].Active;
    }
    if (Touching) {
        SwTouchSendReport(Dev);
    }
    WdfWaitLockRelease(Dev->Lock);
    Polls = InterlockedIncrement(&Dev->Polls);
    if ((Polls % 64) == 0 || IrqWorks) {
        SwTouchTrace(L"Irqs", (ULONG)Irqs);
        SwTouchTrace(L"Polls", (ULONG)Polls);
        SwTouchTrace(L"Events", (ULONG)Dev->Events);
        SwTouchTrace(L"Reports", (ULONG)Dev->Reports);
        SwTouchTrace(L"Errors", (ULONG)Dev->Errors);
        SwTouchTrace(L"Held", (ULONG)Dev->Held);
    }
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS((Irqs != 0 && !Touching) ? POLL_SLOW_MS : POLL_FAST_MS));
    }
}

VOID SwTouchEvtGetFeature(PVOID Context, VHFOPERATIONHANDLE Operation, PVOID OperationContext, PHID_XFER_PACKET Packet)
{
    NTSTATUS Status = STATUS_NOT_SUPPORTED;
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(OperationContext);
    if (Packet->reportId == REPORT_ID_MAX_COUNT && Packet->reportBufferLen >= 2) {
        Packet->reportBuffer[0] = REPORT_ID_MAX_COUNT;
        Packet->reportBuffer[1] = TOUCH_MAX_CONTACTS;
        Status = STATUS_SUCCESS;
    }
    VhfAsyncOperationComplete(Operation, Status);
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwTouchEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWTOUCH_DEVICE Dev = SwTouchGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    BOOLEAN HaveI2c = FALSE;
    UCHAR ChipCmd[3] = { 0xB6, 0x00, 0x04 }, Info[7] = { 0 }, Clear = CMD_CLEAR_STACK, IntCmd[4];
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeConnection && !HaveI2c &&
            Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            Dev->I2cId.LowPart = Desc->u.Connection.IdLowPart;
            Dev->I2cId.HighPart = (LONG)Desc->u.Connection.IdHighPart;
            HaveI2c = TRUE;
        }
    }
    if (!HaveI2c) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Dev->InvertX = SwTouchConfig(L"CfgInvertX", 0);
    Dev->InvertY = SwTouchConfig(L"CfgInvertY", 1);   /* confirmed on hardware: top/bottom flipped otherwise */
    Dev->IntReg = SwTouchConfig(L"CfgIntReg", DEFAULT_INT_REG);

    /* Opened here so the (level) interrupt can always drain the controller. */
    Status = SwTouchOpenTarget(Device, Dev->I2cId, &Dev->I2c);
    SwTouchTrace(L"OpenI2cStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = SwTouchXfer(Dev, ChipCmd, sizeof(ChipCmd), Info, sizeof(Info));
    SwTouchTrace(L"ChipInfoStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;          /* panel not powered: boot with UEFI R373+ */
    }
    SwTouchTrace(L"ChipId", ((ULONG)Info[1] << 8) | Info[2]);
    SwTouchTrace(L"FwVersion", ((ULONG)Info[3] << 8) | Info[4]);
    if (Info[1] != 0x36 || Info[2] != 0x70) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    SwTouchTrace(L"ClearStackStatus", (ULONG)SwTouchXfer(Dev, &Clear, 1, NULL, 0));
    if (Dev->IntReg <= 0xFF) {
        IntCmd[0] = 0xB6;
        IntCmd[1] = 0x00;
        IntCmd[2] = (UCHAR)Dev->IntReg;
        IntCmd[3] = INT_ENABLE_VALUE;
        SwTouchTrace(L"IntEnableStatus", (ULONG)SwTouchXfer(Dev, IntCmd, sizeof(IntCmd), NULL, 0));
    }
    SwTouchTrace(L"CfgInvertX", Dev->InvertX);
    SwTouchTrace(L"CfgInvertY", Dev->InvertY);
    return STATUS_SUCCESS;
}

NTSTATUS SwTouchEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWTOUCH_DEVICE Dev = SwTouchGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (Dev->I2c != NULL) {
        WdfIoTargetClose(Dev->I2c);
        WdfObjectDelete(Dev->I2c);
        Dev->I2c = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS SwTouchEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWTOUCH_DEVICE Dev = SwTouchGetDevice(Device);
    VHF_CONFIG Config;
    NTSTATUS Status;

    if (Dev->Vhf == NULL) {
        VHF_CONFIG_INIT(&Config, WdfDeviceWdmGetDeviceObject(Device), sizeof(SwTouchReportDescriptor),
                        SwTouchReportDescriptor);
        Config.VhfClientContext = Dev;
        Config.VendorID = 0x057E;       /* Nintendo */
        Config.ProductID = 0x5501;
        Config.VersionNumber = 0x0001;
        Config.EvtVhfAsyncOperationGetFeature = SwTouchEvtGetFeature;
        Status = VhfCreate(&Config, &Dev->Vhf);
        SwTouchTrace(L"VhfCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Vhf = NULL;
            return Status;
        }
        Status = VhfStart(Dev->Vhf);
        SwTouchTrace(L"VhfStartStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            VhfDelete(Dev->Vhf, TRUE);
            Dev->Vhf = NULL;
            return Status;
        }
    }
    Dev->VhfReady = TRUE;

    if (Dev->Timer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES Attributes;
        WDF_TIMER_CONFIG_INIT(&TimerConfig, SwTouchEvtTimer);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWTOUCH_TIMER);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->Timer);
        SwTouchTrace(L"TimerCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Timer = NULL;
            return Status;
        }
        SwTouchGetTimer(Dev->Timer)->Device = Device;
    }
    Dev->Stopping = FALSE;
    WdfTimerStart(Dev->Timer, WDF_REL_TIMEOUT_IN_MS(100));
    return STATUS_SUCCESS;
}

VOID SwTouchEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWTOUCH_DEVICE Dev = SwTouchGetDevice(Device);
    Dev->Stopping = TRUE;
    if (Dev->Timer != NULL) {
        WdfTimerStop(Dev->Timer, TRUE);
    }
    Dev->VhfReady = FALSE;
    if (Dev->Vhf != NULL) {
        VhfDelete(Dev->Vhf, TRUE);
        Dev->Vhf = NULL;
    }
}

NTSTATUS SwTouchEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    PSWTOUCH_DEVICE Dev;
    WDFDEVICE Device;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwTouchEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwTouchEvtReleaseHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwTouchEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwTouchEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWTOUCH_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Dev = SwTouchGetDevice(Device);
    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &Dev->Lock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    /* GpioInt PX1: passive-level ISR, since it talks to the controller over I2C. */
    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, SwTouchEvtIsr, NULL);
    InterruptConfig.PassiveHandling = TRUE;
    Status = WdfInterruptCreate(Device, &InterruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &Dev->Interrupt);
    SwTouchTrace(L"InterruptCreateStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        Dev->Interrupt = NULL;      /* polling still works */
    }
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwTouchParamsPath)) {
        RtlCopyMemory(SwTouchParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwTouchParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwTouchParamsPath);
    }
    SwTouchTrace(L"DriverVersion", 4);
    WDF_DRIVER_CONFIG_INIT(&Config, SwTouchEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
