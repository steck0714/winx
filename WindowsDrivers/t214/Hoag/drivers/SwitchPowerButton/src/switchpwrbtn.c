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
 * A press reports System Power Down while the display is on, System Wake Up
 * while it is off. Optional firmware resources expose the B01 PMU_EXT wake
 * interrupt. Older firmware retains the 1.0.0.4 polling state machine.
 * This driver does not implement processor idle states or SC7 entry/resume.
 * Holding the button for several seconds still powers the PMIC off by itself.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>
#include <vhf.h>
#include "button_state.h"
#include "irq_route.h"

#define MAX77620_ONOFFIRQ    0x0B
#define MAX77620_ONOFFSTAT   0x15
#define ONOFFSTAT_EN0        0x04
#define ONOFFIRQ_EN0_F       0x04
#define ONOFFIRQ_EN0_R       0x08
#define POLL_MS              50
#define REPORT_ID_SYSTEM     1

/* System Control: Power Down (0x81), Wake Up (0x83), six padding bits. */
static UCHAR SwPwrReportDescriptor[] = {
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01,
    0x85, REPORT_ID_SYSTEM,
    0x09, 0x81, 0x09, 0x83, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x01,
    0xC0
};

typedef struct _SWPWR_DEVICE {
    LARGE_INTEGER I2cId;
    WDFIOTARGET I2c;
    WDFTIMER Timer;
    WDFINTERRUPT Interrupt;
    PULONG Pmc;
    ULONG OriginalPmc;
    BOOLEAN PmcChanged, VhfReady;
    struct swpwr_irq_route IrqRoute;
    NTSTATUS S0WakeStatus, SxWakeStatus;
    volatile LONG IrqFault, LastIrqStatus, LastIrqBits, Irqs, LastReportStatus;
    VHFHANDLE Vhf;
    volatile BOOLEAN Stopping;
    struct swpwr_button_state Button;
    KSPIN_LOCK TimerLock;
    PVOID DisplayCallback;
    volatile LONG DisplayOff;
    UCHAR Report[2];
    ULONG Failures;
    LONG Polls;
    LONG Presses;
    LONG Reports;
    LONG WakeReports, PowerReports;
} SWPWR_DEVICE, *PSWPWR_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWPWR_DEVICE, SwPwrGetDevice)

typedef struct _SWPWR_TIMER {
    WDFDEVICE Device;
} SWPWR_TIMER, *PSWPWR_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWPWR_TIMER, SwPwrGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwPwrEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwPwrEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwPwrEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY SwPwrEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT SwPwrEvtD0Exit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwPwrEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwPwrEvtSelfManagedIoCleanup;
EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND SwPwrEvtSuspend;
EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART SwPwrEvtRestart;
EVT_WDF_TIMER SwPwrEvtTimer;
EVT_WDF_INTERRUPT_ISR SwPwrEvtIsr;
EVT_WDF_INTERRUPT_WORKITEM SwPwrEvtInterruptWorkItem;

static WCHAR SwPwrParamsPath[256];
static const GUID SwPwrDisplayGuid =
    {0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};
static NTSTATUS SwPwrDisplayChanged(LPCGUID Setting, PVOID Value, ULONG Length, PVOID Context)
{
    PSWPWR_DEVICE Dev=Context;
    if (IsEqualGUID(Setting,&SwPwrDisplayGuid) && Length==sizeof(ULONG) && *(PULONG)Value<=2)
        InterlockedExchange(&Dev->DisplayOff,*(PULONG)Value==0);
    return STATUS_SUCCESS;
}

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

static NTSTATUS SwPwrWriteReg(WDFIOTARGET Target, UCHAR Register, UCHAR Value, UCHAR Mask)
{
    UCHAR Data[2] = {Register, Value}, Readback;
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Data, sizeof(Data));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    NTSTATUS Status = WdfIoTargetSendWriteSynchronously(Target, NULL, &Memory, NULL, &Options, &Bytes);
    if (!NT_SUCCESS(Status)) return Status;
    if (Bytes != sizeof(Data)) return STATUS_DEVICE_PROTOCOL_ERROR;
    Status = SwPwrReadReg(Target, Register, &Readback);
    if (NT_SUCCESS(Status) && ((Readback ^ Value) & Mask)) return STATUS_DEVICE_DATA_ERROR;
    return Status;
}

static int32_t SwPwrIrqRead(void *Context, uint8_t Register, uint8_t *Value)
{
    PSWPWR_DEVICE Dev = Context;
    return SwPwrReadReg(Dev->I2c, Register, Value);
}

static int32_t SwPwrIrqWrite(void *Context, uint8_t Register, uint8_t Value, uint8_t Mask)
{
    PSWPWR_DEVICE Dev = Context;
    return SwPwrWriteReg(Dev->I2c, Register, Value, Mask);
}

static NTSTATUS SwPwrSetIrq(PSWPWR_DEVICE Dev, BOOLEAN Enable)
{
    return swpwr_irq_set(&Dev->IrqRoute,Dev,SwPwrIrqRead,SwPwrIrqWrite,Enable != FALSE);
}

/* ---------------------------------------------------------------- button */

static VOID SwPwrReport(PSWPWR_DEVICE Dev, UCHAR Buttons)
{
    HID_XFER_PACKET Packet;
    NTSTATUS Status;

    Dev->Report[0] = REPORT_ID_SYSTEM;
    Dev->Report[1] = Buttons;
    Packet.reportBuffer = Dev->Report;
    Packet.reportBufferLen = sizeof(Dev->Report);
    Packet.reportId = REPORT_ID_SYSTEM;
    Status = VhfReadReportSubmit(Dev->Vhf, &Packet);
    if (NT_SUCCESS(Status)) {
        InterlockedIncrement(&Dev->Reports);
        if (Buttons==SWPWR_WAKE) InterlockedIncrement(&Dev->WakeReports);
        if (Buttons==SWPWR_POWER) InterlockedIncrement(&Dev->PowerReports);
    }
    InterlockedExchange(&Dev->LastReportStatus, Status);
}

static VOID SwPwrTraceReports(PSWPWR_DEVICE Dev)
{
    SwPwrTrace(L"Reports", (ULONG)InterlockedCompareExchange(&Dev->Reports,0,0));
    SwPwrTrace(L"PowerReports", (ULONG)InterlockedCompareExchange(&Dev->PowerReports,0,0));
    SwPwrTrace(L"WakeReports", (ULONG)InterlockedCompareExchange(&Dev->WakeReports,0,0));
    SwPwrTrace(L"LastReportError", (ULONG)InterlockedCompareExchange(&Dev->LastReportStatus,0,0));
}

BOOLEAN SwPwrEvtIsr(WDFINTERRUPT Interrupt, ULONG Message)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(WdfInterruptGetDevice(Interrupt));
    UCHAR Edge = 0;
    UNREFERENCED_PARAMETER(Message);
    if (InterlockedCompareExchange(&Dev->IrqFault,0,0)) return TRUE;
    NTSTATUS Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFIRQ, &Edge);
    if (NT_SUCCESS(Status) && !(Edge & 0x0c)) return FALSE; /* stale controller pending bit */
    InterlockedExchange(&Dev->LastIrqStatus, Status);
    InterlockedExchange(&Dev->LastIrqBits, Edge);
    InterlockedIncrement(&Dev->Irqs);
    if (!NT_SUCCESS(Status)) {
        UCHAR Top;
        /* Best effort to silence ONOFF before PnP disconnects a failed line.
         * An I2C failure is fatal to this interrupt path, never wake-ready.
         */
        if (NT_SUCCESS(SwPwrReadReg(Dev->I2c,SWPWR_TOP_MASK,&Top)))
            (VOID)SwPwrWriteReg(Dev->I2c,SWPWR_TOP_MASK,Top | SWPWR_TOP_ONOFF,SWPWR_TOP_ONOFF);
        InterlockedExchange(&Dev->IrqFault,1);
    } else if (Dev->VhfReady && (Edge & ONOFFIRQ_EN0_R)) {
        InterlockedIncrement(&Dev->Presses);
        SwPwrReport(Dev, InterlockedCompareExchange(&Dev->DisplayOff,0,0) ? SWPWR_WAKE : SWPWR_POWER);
        SwPwrReport(Dev,0); /* press-only IRQ; release never wakes the display */
    }
    /* No registry/hive I/O under the passive ISR's framework wait lock. */
    (VOID)WdfInterruptQueueWorkItemForIsr(Interrupt);
    return TRUE;
}

VOID SwPwrEvtInterruptWorkItem(WDFINTERRUPT Interrupt, WDFOBJECT Object)
{
    WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    UNREFERENCED_PARAMETER(Object);
    if (InterlockedCompareExchange(&Dev->IrqFault,0,0)) {
        /* An unacknowledged exclusive level interrupt must not keep firing.
         * PnP disconnects it before the power callbacks restore the masks.
         * Do not change mask ownership concurrently with those callbacks.
         */
        WdfDeviceSetFailed(Device,WdfDeviceFailedNoRestart);
    }
    SwPwrTrace(L"WakeIrqReady", Dev->IrqRoute.armed && !Dev->IrqFault);
    SwPwrTrace(L"WakeIrqError", (ULONG)InterlockedCompareExchange(&Dev->LastIrqStatus,0,0));
    SwPwrTrace(L"LastOnOffIrq", (ULONG)InterlockedCompareExchange(&Dev->LastIrqBits,0,0));
    SwPwrTrace(L"Irqs", (ULONG)InterlockedCompareExchange(&Dev->Irqs,0,0));
    SwPwrTrace(L"Presses", (ULONG)InterlockedCompareExchange(&Dev->Presses,0,0));
    SwPwrTraceReports(Dev);
}

VOID SwPwrEvtTimer(WDFTIMER Timer)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(SwPwrGetTimer(Timer)->Device);
    UCHAR Irq = 0, Stat = 0;
    BOOLEAN Held;
    KIRQL Irql;
    NTSTATUS Status;

    if (Dev->Stopping || Dev->Interrupt) return;
    Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFIRQ, &Irq);
    if (NT_SUCCESS(Status)) {
        Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFSTAT, &Stat);
    }
    if (NT_SUCCESS(Status)) {
        Dev->Failures = 0;
        Held = (Stat & ONOFFSTAT_EN0) != 0;
        struct swpwr_button_event Event=swpwr_button_update(&Dev->Button,Held,
            (Irq&ONOFFIRQ_EN0_R)!=0,InterlockedCompareExchange(&Dev->DisplayOff,0,0)!=0);
        if (Event.emit) {
            if (Event.buttons) SwPwrTrace(L"Presses",(ULONG)InterlockedIncrement(&Dev->Presses));
            SwPwrReport(Dev,Event.buttons);
            if (Event.pulse) SwPwrReport(Dev,0);
            SwPwrTraceReports(Dev);
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
    KeAcquireSpinLock(&Dev->TimerLock,&Irql);
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));    /* one-shot passive timer, re-armed */
    }
    KeReleaseSpinLock(&Dev->TimerLock,Irql);
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwPwrEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Pmc = NULL, Irq = NULL, IrqRaw = NULL;
    NTSTATUS Status;
    if (Dev->IrqRoute.saved) return STATUS_INVALID_DEVICE_STATE; /* failed teardown */
    InterlockedExchange(&Dev->IrqFault,0);
    Dev->I2cId.QuadPart = 0;

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeConnection &&
            Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            Dev->I2cId.LowPart = Desc->u.Connection.IdLowPart;
            Dev->I2cId.HighPart = (LONG)Desc->u.Connection.IdHighPart;
        }
        if (Desc != NULL && Desc->Type == CmResourceTypeMemory &&
            Desc->u.Memory.Start.QuadPart == 0x7000e400 && Desc->u.Memory.Length == 4) Pmc = Desc;
        if (Desc != NULL && Desc->Type == CmResourceTypeInterrupt) {
            if (Irq != NULL) return STATUS_DEVICE_CONFIGURATION_ERROR;
            Irq = Desc; IrqRaw = WdfCmResourceListGetDescriptor(Raw,Index);
        }
    }
    if (!Dev->I2cId.QuadPart) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!Pmc && !Irq) return STATUS_SUCCESS;
    if (!Pmc || !Irq || !IrqRaw || IrqRaw->Type != CmResourceTypeInterrupt ||
        IrqRaw->u.Interrupt.Vector != 118 || (Irq->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ||
        Irq->ShareDisposition != CmResourceShareDeviceExclusive) return STATUS_DEVICE_CONFIGURATION_ERROR;
    Dev->Pmc = MmMapIoSpaceEx(Pmc->u.Memory.Start,sizeof(ULONG),PAGE_READWRITE | PAGE_NOCACHE);
    if (!Dev->Pmc) return STATUS_INSUFFICIENT_RESOURCES;
    Dev->OriginalPmc = READ_REGISTER_ULONG(Dev->Pmc);
    if (Dev->OriginalPmc == MAXULONG) { Status = STATUS_DEVICE_DATA_ERROR; goto Fail; }
    /* B01 PMU_EXT is SPI 86 / GSI 118, level high after PMC inversion. */
    Dev->PmcChanged = !(Dev->OriginalPmc & (1u << 17));
    WRITE_REGISTER_ULONG(Dev->Pmc,Dev->OriginalPmc | (1u << 17));
    if (!(READ_REGISTER_ULONG(Dev->Pmc) & (1u << 17))) { Status = STATUS_DEVICE_DATA_ERROR; goto Fail; }
    WDF_INTERRUPT_CONFIG Config;
    WDF_INTERRUPT_CONFIG_INIT(&Config,SwPwrEvtIsr,NULL);
    Config.PassiveHandling = TRUE; Config.CanWakeDevice = TRUE;
    Config.AutomaticSerialization = FALSE;
    Config.EvtInterruptWorkItem = SwPwrEvtInterruptWorkItem;
    Config.InterruptRaw = IrqRaw; Config.InterruptTranslated = Irq;
    Status = WdfInterruptCreate(Device,&Config,WDF_NO_OBJECT_ATTRIBUTES,&Dev->Interrupt);
    if (!NT_SUCCESS(Status)) goto Fail;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS Idle;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&Idle,IdleCanWakeFromS0);
    Idle.DxState = PowerDeviceD3; Idle.IdleTimeout = 1000;
    Idle.IdleTimeoutType = SystemManagedIdleTimeoutWithHint;
    Idle.ExcludeD3Cold = WdfTrue; Idle.Enabled = WdfTrue;
    Idle.UserControlOfIdleSettings = IdleDoNotAllowUserControl;
    Dev->S0WakeStatus = Status = WdfDeviceAssignS0IdleSettings(Device,&Idle);
    if (!NT_SUCCESS(Status)) goto Fail;
    WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS Wake;
    WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS_INIT(&Wake);
    Wake.DxState = PowerDeviceD3; Wake.UserControlOfWakeSettings = WakeDoNotAllowUserControl;
    Wake.Enabled = WdfTrue;
    Dev->SxWakeStatus = Status = WdfDeviceAssignSxWakeSettings(Device,&Wake);
    if (!NT_SUCCESS(Status) && Status != STATUS_POWER_STATE_INVALID) goto Fail;
    return STATUS_SUCCESS;
Fail:
    (VOID)SwPwrEvtReleaseHardware(Device,Translated);
    return Status;
}

NTSTATUS SwPwrEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    NTSTATUS Status = SwPwrSetIrq(Dev,FALSE);
    UNREFERENCED_PARAMETER(Translated);
    if (Dev->Interrupt) { WdfObjectDelete(Dev->Interrupt); Dev->Interrupt = NULL; }
    if (Dev->Pmc) {
        if (Dev->PmcChanged) WRITE_REGISTER_ULONG(Dev->Pmc,
            (READ_REGISTER_ULONG(Dev->Pmc) & ~(1u << 17)) | (Dev->OriginalPmc & (1u << 17)));
        MmUnmapIoSpace(Dev->Pmc,sizeof(ULONG)); Dev->Pmc = NULL; Dev->PmcChanged = FALSE;
    }
    if (Dev->I2c) { WdfIoTargetClose(Dev->I2c); WdfObjectDelete(Dev->I2c); Dev->I2c = NULL; }
    return Status;
}

NTSTATUS SwPwrEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    UNREFERENCED_PARAMETER(Previous);
    NTSTATUS Status = STATUS_SUCCESS;
    if (!Dev->I2c) Status = SwPwrOpenTarget(Device,Dev->I2cId,&Dev->I2c);
    if (!NT_SUCCESS(Status) || !Dev->Interrupt) return Status;
    if (Dev->IrqFault) return (NTSTATUS)Dev->LastIrqStatus;
    BOOLEAN Fresh = !Dev->IrqRoute.saved;
    Status = swpwr_irq_snapshot(&Dev->IrqRoute,Dev,SwPwrIrqRead,SwPwrIrqWrite);
    if (!NT_SUCCESS(Status)) return Status;
    if (Fresh && Dev->VhfReady) {
        UCHAR OldEdges;
        Status = SwPwrReadReg(Dev->I2c,MAX77620_ONOFFIRQ,&OldEdges);
        if (!NT_SUCCESS(Status)) return Status;
    }
    /* Wake D0 entry retains the latched edge for the ISR. */
    return Dev->VhfReady ? SwPwrSetIrq(Dev,TRUE) : STATUS_SUCCESS;
}

NTSTATUS SwPwrEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    POWER_ACTION Action = WdfDeviceGetSystemPowerAction(Device);
    if (Target == WdfPowerDeviceD3Final || Action == PowerActionShutdown ||
        Action == PowerActionShutdownReset || Action == PowerActionShutdownOff)
        return SwPwrSetIrq(Dev,FALSE);
    return STATUS_SUCCESS; /* Keep the wake route armed during S0 idle. */
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
    Dev->Button.suppress_held = (Stat & ONOFFSTAT_EN0) != 0;

    if (Dev->Vhf == NULL) {
        VHF_CONFIG_INIT(&Config, WdfDeviceWdmGetDeviceObject(Device), sizeof(SwPwrReportDescriptor),
                        SwPwrReportDescriptor);
        Config.VhfClientContext = Dev;
        Config.VendorID = 0x057E;       /* Nintendo */
        Config.ProductID = 0x5601;
        Config.VersionNumber = 0x0005;
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
    Dev->VhfReady = TRUE;
    if (Dev->Timer == NULL && !Dev->Interrupt) {
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
    if (Dev->DisplayCallback==NULL) {
        Status=PoRegisterPowerSettingCallback(WdfDeviceWdmGetDeviceObject(Device),
            &SwPwrDisplayGuid,SwPwrDisplayChanged,Dev,&Dev->DisplayCallback);
        SwPwrTrace(L"DisplayCallbackStatus",(ULONG)Status);
        if (!NT_SUCCESS(Status)) { Dev->DisplayCallback=NULL; return Status; }
    }
    Status = SwPwrEvtRestart(Device);
    SwPwrTrace(L"WakeIrqMode",Dev->Interrupt != NULL);
    SwPwrTrace(L"WakeIrqReady",Dev->IrqRoute.armed && !Dev->IrqFault);
    SwPwrTrace(L"S0WakePolicyStatus",(ULONG)Dev->S0WakeStatus);
    SwPwrTrace(L"SxWakePolicyStatus",(ULONG)Dev->SxWakeStatus);
    return Status;
}

NTSTATUS SwPwrEvtSuspend(WDFDEVICE Device)
{
    PSWPWR_DEVICE Dev=SwPwrGetDevice(Device);
    KIRQL Irql;
    KeAcquireSpinLock(&Dev->TimerLock,&Irql);
    Dev->Stopping=TRUE;
    KeReleaseSpinLock(&Dev->TimerLock,Irql);
    if (Dev->Timer) WdfTimerStop(Dev->Timer,TRUE);
    if (!Dev->Interrupt && Dev->Button.held && Dev->Vhf) {
        SwPwrReport(Dev,0); SwPwrTraceReports(Dev);
    }
    Dev->Button.held=false;
    if (!Dev->Interrupt) SwPwrTrace(L"Suspended",1);
    return STATUS_SUCCESS;
}
NTSTATUS SwPwrEvtRestart(WDFDEVICE Device)
{
    PSWPWR_DEVICE Dev=SwPwrGetDevice(Device);
    UCHAR Irq=0,Stat=0;
    KIRQL Irql;
    if (Dev->Interrupt) return SwPwrSetIrq(Dev,TRUE);
    NTSTATUS Status=SwPwrReadReg(Dev->I2c,MAX77620_ONOFFIRQ,&Irq);
    if (NT_SUCCESS(Status)) Status=SwPwrReadReg(Dev->I2c,MAX77620_ONOFFSTAT,&Stat);
    SwPwrTrace(L"ResumeReadStatus",(ULONG)Status);
    if (!NT_SUCCESS(Status)) return Status;
    Dev->Button.held=false;
    Dev->Button.suppress_held=(Stat&ONOFFSTAT_EN0)!=0;
    SwPwrTrace(L"Suspended",0);
    KeAcquireSpinLock(&Dev->TimerLock,&Irql);
    Dev->Stopping=FALSE;
    WdfTimerStart(Dev->Timer,WDF_REL_TIMEOUT_IN_MS(POLL_MS));
    KeReleaseSpinLock(&Dev->TimerLock,Irql);
    return STATUS_SUCCESS;
}

VOID SwPwrEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    (VOID)SwPwrEvtSuspend(Device);
    if (Dev->Interrupt) WdfInterruptAcquireLock(Dev->Interrupt);
    Dev->VhfReady = FALSE;
    if (Dev->Interrupt) WdfInterruptReleaseLock(Dev->Interrupt);
    if (Dev->DisplayCallback) {
        PoUnregisterPowerSettingCallback(Dev->DisplayCallback);
        Dev->DisplayCallback=NULL;
    }
    if (Dev->Vhf != NULL) {
        VhfDelete(Dev->Vhf, TRUE);
        Dev->Vhf = NULL;
    }
    /* ReleaseHardware retains I2C until masks and the interrupt are retired. */
}

NTSTATUS SwPwrEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwPwrEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwPwrEvtReleaseHardware;
    Pnp.EvtDeviceD0Entry = SwPwrEvtD0Entry;
    Pnp.EvtDeviceD0Exit = SwPwrEvtD0Exit;
    Pnp.EvtDeviceSelfManagedIoInit = SwPwrEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwPwrEvtSelfManagedIoCleanup;
    Pnp.EvtDeviceSelfManagedIoSuspend = SwPwrEvtSuspend;
    Pnp.EvtDeviceSelfManagedIoRestart = SwPwrEvtRestart;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWPWR_DEVICE);
    Status=WdfDeviceCreate(&DeviceInit,&Attributes,&Device);
    if (NT_SUCCESS(Status)) {
        PSWPWR_DEVICE Dev=SwPwrGetDevice(Device);
        KeInitializeSpinLock(&Dev->TimerLock);
        Dev->Stopping=TRUE;
    }
    return Status;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwPwrParamsPath)) {
        RtlCopyMemory(SwPwrParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwPwrParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwPwrParamsPath);
    }
    SwPwrTrace(L"DriverVersion", 5);
    WDF_DRIVER_CONFIG_INIT(&Config, SwPwrEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
