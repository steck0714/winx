/*
 * Nintendo Switch CPU clock control, KMDF.
 *
 * ACPI device CPUC (ACPI\SWOA0701):
 *   Memory32Fixed 0x60006000: CAR. Only CCLK_BURST_POLICY (0x020), PLLX_BASE
 *     (0x0E0) and PLLX_MISC (0x0E4) are touched.
 *   I2cSerialBus 0x1B on I2C5: MAX77621 VDD_CPU (VOUT 0x00, VOUT_DVC 0x01;
 *     606.25 mV + 6.25 mV x code, bit 7 = enable).
 *   I2cSerialBus 0x4C on I2C1: TMP451 (0x00 board, 0x01 SoC), thermal guard.
 * UEFI R365+ boots the CPU at PLLX 38.4 / 2 x 53 = 1017.6 MHz, 1025 mV.
 *
 * A clock change follows UEFI R365: voltage up first; CCLK parked on CLK_M
 * (never PLLP: PLLP_OUT_CPU is gated after a cold boot); PLLX re-locked with
 * the new N; CCLK back on PLLX; voltage down last. Only the fixed profile
 * table below can be selected (speedo ID 3, Linux CVB table ODN + 40 mV for
 * open-loop PLLX, never above 1227 mV).
 *
 * Thermal guard (1 s): SoC >= 80 C, or 3 failed sensor reads, drops to the
 * default profile; below 70 C the requested profile comes back.
 * User interface: \\.\SwitchCpu, see switchcpu_ioctl.h.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>
#include "switchcpu_ioctl.h"

#define CAR_CCLK_BURST_POLICY 0x020
#define CAR_PLLX_BASE         0x0E0
#define CAR_PLLX_MISC         0x0E4
#define PLLX_ENABLE           (1u << 30)
#define PLLX_BYPASS           (1u << 31)
#define PLLX_LOCK             (1u << 27)
#define PLLX_LOCK_ENABLE      (1u << 18)
#define PLLX_DIV_MASK         (0xFFu | (0xFFu << 8) | (0x1Fu << 20))
#define PLLX_M                2u
#define OSC_KHZ               38400u
#define CCLK_ON_PLLX          0x20008888u
#define CCLK_ON_CLKM          0x20000000u
#define MAX77621_VOUT         0x00
#define MAX77621_VOUT_DVC     0x01
#define MAX77621_ENABLE       0x80u
#define MAX77621_MIN_UV       606250u
#define MAX77621_STEP_UV      6250u
#define VDD_CPU_MAX_MV        1227u
#define TMP451_LOCAL          0x00
#define TMP451_REMOTE         0x01
#define GUARD_MS              1000
#define THROTTLE_C            80
#define RESUME_C              70
#define DEFAULT_PROFILE       1

typedef struct _CPU_PROFILE {
    ULONG N;
    ULONG Mv;
    ULONG Flags;
} CPU_PROFILE;

static const CPU_PROFILE SwCpuProfiles[] = {
    { 32,  893, 0 },                            /*  614.4 MHz, ODN  853 mV */
    { 53, 1025, SWCPU_PROFILE_DEFAULT },        /* 1017.6 MHz, ODN  985 mV */
    { 63, 1091, 0 },                            /* 1209.6 MHz, ODN 1051 mV */
    { 74, 1157, 0 },                            /* 1420.8 MHz, ODN 1117 mV */
    { 82, 1170, 0 },                            /* 1574.4 MHz, ODN 1130 mV */
    { 87, 1208, SWCPU_PROFILE_EXPERIMENTAL },   /* 1670.4 MHz, ODN 1168 mV */
    { 92, 1227, SWCPU_PROFILE_EXPERIMENTAL },   /* 1766.4 MHz, ODN 1228 mV: no margin */
};
#define PROFILE_COUNT RTL_NUMBER_OF(SwCpuProfiles)

typedef struct _SWCPU_DEVICE {
    volatile ULONG *Car;
    ULONG CarLength;
    LARGE_INTEGER PmicId;
    LARGE_INTEGER SensorId;
    WDFIOTARGET Pmic;
    WDFIOTARGET Sensor;
    WDFWAITLOCK Lock;
    WDFTIMER Timer;
    volatile BOOLEAN Stopping;
    LONG Requested;
    BOOLEAN Throttled;
    ULONG SensorFailures;
    ULONG TempSoC;
    ULONG TempBoard;
    BOOLEAN TempValid;
    ULONG Switches;
    NTSTATUS LastError;
    ULONG FanRuns;          /* fallback temperature source, see SwCpuReadFanTemps */
    ULONG FanStale;
} SWCPU_DEVICE, *PSWCPU_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWCPU_DEVICE, SwCpuGetDevice)

typedef struct _SWCPU_TIMER {
    WDFDEVICE Device;
} SWCPU_TIMER, *PSWCPU_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWCPU_TIMER, SwCpuGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwCpuEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwCpuEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwCpuEvtReleaseHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwCpuEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwCpuEvtSelfManagedIoCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SwCpuEvtIoDeviceControl;
EVT_WDF_TIMER SwCpuEvtTimer;

static WCHAR SwCpuParamsPath[256];

static VOID SwCpuTrace(PCWSTR Name, ULONG Value)
{
    if (SwCpuParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwCpuParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

/* ------------------------------------------------------------------- I2C */

static NTSTATUS SwCpuOpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
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

static NTSTATUS SwCpuReadReg(WDFIOTARGET Target, UCHAR Register, PUCHAR Value)
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

static NTSTATUS SwCpuWriteReg(WDFIOTARGET Target, UCHAR Register, UCHAR Value)
{
    UCHAR Buffer[2] = { Register, Value };
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, sizeof(Buffer));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    Status = WdfIoTargetSendWriteSynchronously(Target, NULL, &Memory, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != sizeof(Buffer)) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

/* ------------------------------------------------------------- regulator */

static ULONG SwCpuCodeMv(UCHAR Code)
{
    return (MAX77621_MIN_UV + (ULONG)(Code & 0x7F) * MAX77621_STEP_UV) / 1000;
}

/* Smallest code at or above Mv, but never above VDD_CPU_MAX_MV. */
static UCHAR SwCpuMvCode(ULONG Mv)
{
    ULONG Code = (Mv * 1000 - MAX77621_MIN_UV + MAX77621_STEP_UV - 1) / MAX77621_STEP_UV;
    while (Code > 0 && SwCpuCodeMv((UCHAR)Code) > VDD_CPU_MAX_MV) {
        --Code;
    }
    return (UCHAR)(MAX77621_ENABLE | Code);
}

static NTSTATUS SwCpuSetVoltage(PSWCPU_DEVICE Dev, ULONG Mv)
{
    UCHAR Code = SwCpuMvCode(Mv), A = 0, B = 0;
    NTSTATUS Status;
    LARGE_INTEGER Settle;

    Status = SwCpuWriteReg(Dev->Pmic, MAX77621_VOUT_DVC, Code);
    if (NT_SUCCESS(Status)) {
        Status = SwCpuWriteReg(Dev->Pmic, MAX77621_VOUT, Code);
    }
    if (NT_SUCCESS(Status)) {
        Status = SwCpuReadReg(Dev->Pmic, MAX77621_VOUT, &A);
    }
    if (NT_SUCCESS(Status)) {
        Status = SwCpuReadReg(Dev->Pmic, MAX77621_VOUT_DVC, &B);
    }
    if (NT_SUCCESS(Status) && (A != Code || B != Code)) {
        Status = STATUS_DEVICE_DATA_ERROR;
    }
    Settle.QuadPart = -2 * 10000;      /* 2 ms for the rail to slew */
    KeDelayExecutionThread(KernelMode, FALSE, &Settle);
    return Status;
}

static ULONG SwCpuReadMv(PSWCPU_DEVICE Dev)
{
    UCHAR Code = 0;
    return NT_SUCCESS(SwCpuReadReg(Dev->Pmic, MAX77621_VOUT, &Code)) ? SwCpuCodeMv(Code) : 0;
}

/* ------------------------------------------------------------------ PLLX */

static ULONG SwCpuReadCar(PSWCPU_DEVICE Dev, ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)&Dev->Car[Offset / 4]);
}

static VOID SwCpuWriteCar(PSWCPU_DEVICE Dev, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)&Dev->Car[Offset / 4], Value);
}

static ULONG SwCpuPllxKhz(ULONG Base)
{
    static const ULONG PDiv[] = { 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 12, 16, 20, 24, 32, 32 };
    ULONG M = Base & 0xFF, N = (Base >> 8) & 0xFF, P = (Base >> 20) & 0x1F;
    if (M == 0 || P >= RTL_NUMBER_OF(PDiv)) {
        return 0;
    }
    return (OSC_KHZ * N) / (M * PDiv[P]);
}

static LONG SwCpuActiveProfile(PSWCPU_DEVICE Dev)
{
    ULONG Base = SwCpuReadCar(Dev, CAR_PLLX_BASE), Index;
    if ((Base & 0xFF) != PLLX_M || ((Base >> 20) & 0x1F) != 0) {
        return -1;
    }
    for (Index = 0; Index < PROFILE_COUNT; ++Index) {
        if (((Base >> 8) & 0xFF) == SwCpuProfiles[Index].N) {
            return (LONG)Index;
        }
    }
    return -1;
}

/* Re-lock PLLX at N with CCLK parked on CLK_M. DISPATCH_LEVEL, a few hundred us. */
static NTSTATUS SwCpuRelockPllx(PSWCPU_DEVICE Dev, ULONG N)
{
    ULONG OldBase = SwCpuReadCar(Dev, CAR_PLLX_BASE);
    ULONG OldPolicy = SwCpuReadCar(Dev, CAR_CCLK_BURST_POLICY);
    ULONG NewBase = (OldBase & ~PLLX_DIV_MASK & ~PLLX_LOCK) | PLLX_M | (N << 8);
    ULONG Spin;
    BOOLEAN Locked = FALSE;
    KIRQL OldIrql;

    if ((OldBase & (PLLX_ENABLE | PLLX_BYPASS)) != PLLX_ENABLE || OldPolicy != CCLK_ON_PLLX) {
        SwCpuTrace(L"UnexpectedPllxBase", OldBase);
        SwCpuTrace(L"UnexpectedCclkPolicy", OldPolicy);
        return STATUS_INVALID_DEVICE_STATE;
    }
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    SwCpuWriteCar(Dev, CAR_CCLK_BURST_POLICY, CCLK_ON_CLKM);
    KeStallExecutionProcessor(10);
    SwCpuWriteCar(Dev, CAR_PLLX_MISC, SwCpuReadCar(Dev, CAR_PLLX_MISC) | PLLX_LOCK_ENABLE);
    SwCpuWriteCar(Dev, CAR_PLLX_BASE, NewBase & ~PLLX_ENABLE);
    KeStallExecutionProcessor(10);
    SwCpuWriteCar(Dev, CAR_PLLX_BASE, NewBase | PLLX_ENABLE);
    for (Spin = 0; Spin < 1000; ++Spin) {
        KeStallExecutionProcessor(10);
        if ((SwCpuReadCar(Dev, CAR_PLLX_BASE) & PLLX_LOCK) != 0) {
            Locked = TRUE;
            break;
        }
    }
    if (!Locked) {
        SwCpuWriteCar(Dev, CAR_PLLX_BASE, OldBase & ~PLLX_LOCK & ~PLLX_ENABLE);
        KeStallExecutionProcessor(10);
        SwCpuWriteCar(Dev, CAR_PLLX_BASE, (OldBase & ~PLLX_LOCK) | PLLX_ENABLE);
        for (Spin = 0; Spin < 1000 && (SwCpuReadCar(Dev, CAR_PLLX_BASE) & PLLX_LOCK) == 0; ++Spin) {
            KeStallExecutionProcessor(10);
        }
    }
    KeStallExecutionProcessor(50);
    SwCpuWriteCar(Dev, CAR_CCLK_BURST_POLICY, OldPolicy);
    KeStallExecutionProcessor(10);
    KeLowerIrql(OldIrql);
    SwCpuTrace(L"LastLockSpins", Spin);
    return Locked ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

/* Switch to a table profile. Caller holds Dev->Lock, PASSIVE_LEVEL. */
static NTSTATUS SwCpuApply(PSWCPU_DEVICE Dev, ULONG Index)
{
    const CPU_PROFILE *Target = &SwCpuProfiles[Index];
    LONG Active = SwCpuActiveProfile(Dev);
    ULONG OldMv = SwCpuReadMv(Dev);
    NTSTATUS Status;

    if (Active == (LONG)Index && OldMv >= SwCpuCodeMv(SwCpuMvCode(Target->Mv))) {
        return STATUS_SUCCESS;
    }
    if (OldMv == 0) {
        Status = STATUS_DEVICE_NOT_READY;
        goto Fail;
    }
    if (Target->Mv > OldMv) {
        Status = SwCpuSetVoltage(Dev, Target->Mv);
        if (!NT_SUCCESS(Status)) {
            (VOID)SwCpuSetVoltage(Dev, OldMv);
            goto Fail;
        }
    }
    Status = SwCpuRelockPllx(Dev, Target->N);
    if (!NT_SUCCESS(Status)) {
        (VOID)SwCpuSetVoltage(Dev, OldMv);      /* the old PLLX setting is back */
        goto Fail;
    }
    if (Target->Mv < OldMv) {
        (VOID)SwCpuSetVoltage(Dev, Target->Mv);
    }
    ++Dev->Switches;
    SwCpuTrace(L"Switches", Dev->Switches);
    SwCpuTrace(L"ActiveProfile", Index);
    SwCpuTrace(L"CurrentKhz", SwCpuPllxKhz(SwCpuReadCar(Dev, CAR_PLLX_BASE)));
    SwCpuTrace(L"CurrentMv", SwCpuReadMv(Dev));
    return STATUS_SUCCESS;

Fail:
    Dev->LastError = Status;
    SwCpuTrace(L"LastError", (ULONG)Status);
    return Status;
}

/* --------------------------------------------------------- thermal guard */

/* The fan driver already owns the TMP451 (opening it again fails with
 * STATUS_DEVICE_BUSY), so its trace is the fallback source: TempSoC and
 * TempBoard every 2 s, with Runs counting up. A Runs value that stops for
 * 5 s counts as a failed read. */
static NTSTATUS SwCpuReadFanTemps(PSWCPU_DEVICE Dev, PUCHAR Remote, PUCHAR Local)
{
    ULONG Soc = 0xFFFFFFFF, Board = 0xFFFFFFFF, Runs = 0;
    RTL_QUERY_REGISTRY_TABLE Table[4];
    NTSTATUS Status;

    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = L"TempSoC";
    Table[0].EntryContext = &Soc;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    Table[1] = Table[0];
    Table[1].Name = L"TempBoard";
    Table[1].EntryContext = &Board;
    Table[2] = Table[0];
    Table[2].Name = L"Runs";
    Table[2].EntryContext = &Runs;
    Status = RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, L"switchfan\\Parameters", Table, NULL, NULL);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (Runs != Dev->FanRuns) {
        Dev->FanRuns = Runs;
        Dev->FanStale = 0;
    } else if (++Dev->FanStale >= 5) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (Soc > 150 || Board > 150) {
        return STATUS_DEVICE_DATA_ERROR;
    }
    *Remote = (UCHAR)Soc;
    *Local = (UCHAR)Board;
    return STATUS_SUCCESS;
}

VOID SwCpuEvtTimer(WDFTIMER Timer)
{
    PSWCPU_DEVICE Dev = SwCpuGetDevice(SwCpuGetTimer(Timer)->Device);
    UCHAR Local = 0, Remote = 0;
    NTSTATUS Status;

    WdfWaitLockAcquire(Dev->Lock, NULL);
    if (Dev->Sensor != NULL) {
        Status = SwCpuReadReg(Dev->Sensor, TMP451_REMOTE, &Remote);
        if (NT_SUCCESS(Status)) {
            Status = SwCpuReadReg(Dev->Sensor, TMP451_LOCAL, &Local);
        }
    } else {
        Status = SwCpuReadFanTemps(Dev, &Remote, &Local);
    }
    if (NT_SUCCESS(Status)) {
        Dev->SensorFailures = 0;
        Dev->TempSoC = Remote;
        Dev->TempBoard = Local;
        Dev->TempValid = TRUE;
    } else {
        Dev->TempValid = FALSE;
        ++Dev->SensorFailures;
    }
    if (!Dev->Throttled && (Dev->SensorFailures >= 3 || (Dev->TempValid && Dev->TempSoC >= THROTTLE_C)) &&
        SwCpuActiveProfile(Dev) > DEFAULT_PROFILE) {
        Dev->Throttled = TRUE;
        SwCpuTrace(L"ThrottleTemp", Dev->TempSoC);
        (VOID)SwCpuApply(Dev, DEFAULT_PROFILE);
    } else if (Dev->Throttled && Dev->TempValid && Dev->TempSoC < RESUME_C) {
        Dev->Throttled = FALSE;
        (VOID)SwCpuApply(Dev, (ULONG)Dev->Requested);
    }
    SwCpuTrace(L"Throttled", Dev->Throttled);
    WdfWaitLockRelease(Dev->Lock);
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(GUARD_MS));    /* one-shot passive timer, re-armed */
    }
}

/* ------------------------------------------------------------------- I/O */

VOID SwCpuEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    PSWCPU_DEVICE Dev = SwCpuGetDevice(WdfIoQueueGetDevice(Queue));
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Info = 0;
    UNREFERENCED_PARAMETER(OutLength);
    UNREFERENCED_PARAMETER(InLength);

    WdfWaitLockAcquire(Dev->Lock, NULL);
    switch (Code) {
    case IOCTL_SWCPU_GET_STATUS: {
        SWCPU_STATUS *Out;
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID *)&Out, NULL);
        if (NT_SUCCESS(Status)) {
            RtlZeroMemory(Out, sizeof(*Out));
            Out->Version = 1;
            Out->CurrentKhz = SwCpuPllxKhz(SwCpuReadCar(Dev, CAR_PLLX_BASE));
            Out->CurrentMv = SwCpuReadMv(Dev);
            Out->ActiveProfile = SwCpuActiveProfile(Dev);
            Out->RequestedProfile = Dev->Requested;
            Out->Throttled = Dev->Throttled;
            Out->TempSoC = Dev->TempSoC;
            Out->TempBoard = Dev->TempBoard;
            Out->TempValid = Dev->TempValid;
            Out->ProfileCount = PROFILE_COUNT;
            Out->Switches = Dev->Switches;
            Out->LastError = (ULONG)Dev->LastError;
            Info = sizeof(*Out);
        }
        break;
    }
    case IOCTL_SWCPU_GET_PROFILES: {
        SWCPU_PROFILES *Out;
        ULONG Index;
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID *)&Out, NULL);
        if (NT_SUCCESS(Status)) {
            RtlZeroMemory(Out, sizeof(*Out));
            Out->Count = PROFILE_COUNT;
            for (Index = 0; Index < PROFILE_COUNT; ++Index) {
                Out->Profiles[Index].Khz = OSC_KHZ * SwCpuProfiles[Index].N / PLLX_M;
                Out->Profiles[Index].Mv = SwCpuCodeMv(SwCpuMvCode(SwCpuProfiles[Index].Mv));
                Out->Profiles[Index].Flags = SwCpuProfiles[Index].Flags;
            }
            Info = sizeof(*Out);
        }
        break;
    }
    case IOCTL_SWCPU_SET_PROFILE: {
        SWCPU_SET_PROFILE_INPUT *In;
        Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*In), (PVOID *)&In, NULL);
        if (NT_SUCCESS(Status) && In->Profile >= PROFILE_COUNT) {
            Status = STATUS_INVALID_PARAMETER;
        }
        if (NT_SUCCESS(Status)) {
            Dev->Requested = (LONG)In->Profile;
            SwCpuTrace(L"RequestedProfile", In->Profile);
            /* While throttled, a faster profile waits for the guard to release it. */
            if (!Dev->Throttled || In->Profile <= DEFAULT_PROFILE) {
                Status = SwCpuApply(Dev, In->Profile);
            }
        }
        break;
    }
    default:
        break;
    }
    WdfWaitLockRelease(Dev->Lock);
    WdfRequestCompleteWithInformation(Request, Status, Info);
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwCpuEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWCPU_DEVICE Dev = SwCpuGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index, I2cSeen = 0;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc == NULL) {
            continue;
        }
        if (Desc->Type == CmResourceTypeMemory && Dev->Car == NULL) {
            if (Desc->u.Memory.Length < 0x100) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            Dev->Car = (volatile ULONG *)MmMapIoSpaceEx(Desc->u.Memory.Start, Desc->u.Memory.Length,
                                                        PAGE_READWRITE | PAGE_NOCACHE);
            if (Dev->Car == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            Dev->CarLength = Desc->u.Memory.Length;
        } else if (Desc->Type == CmResourceTypeConnection &&
                   Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                   Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            LARGE_INTEGER *Id = (I2cSeen == 0) ? &Dev->PmicId : &Dev->SensorId;   /* DSDT order */
            if (I2cSeen < 2) {
                Id->LowPart = Desc->u.Connection.IdLowPart;
                Id->HighPart = (LONG)Desc->u.Connection.IdHighPart;
            }
            ++I2cSeen;
        }
    }
    if (Dev->Car == NULL || I2cSeen < 2) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    SwCpuTrace(L"PllxBaseAtStart", SwCpuReadCar(Dev, CAR_PLLX_BASE));
    SwCpuTrace(L"CclkPolicyAtStart", SwCpuReadCar(Dev, CAR_CCLK_BURST_POLICY));
    return STATUS_SUCCESS;
}

NTSTATUS SwCpuEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWCPU_DEVICE Dev = SwCpuGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (Dev->Car != NULL) {
        MmUnmapIoSpace((PVOID)Dev->Car, Dev->CarLength);
        Dev->Car = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS SwCpuEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWCPU_DEVICE Dev = SwCpuGetDevice(Device);
    LONG Active;
    NTSTATUS Status;

    if (Dev->Pmic == NULL) {
        Status = SwCpuOpenTarget(Device, Dev->PmicId, &Dev->Pmic);
        SwCpuTrace(L"OpenPmicStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    if (Dev->Sensor == NULL) {
        Status = SwCpuOpenTarget(Device, Dev->SensorId, &Dev->Sensor);
        SwCpuTrace(L"OpenSensorStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Sensor = NULL;     /* thermal guard reads the fan driver's trace instead */
        }
    }
    SwCpuTrace(L"TempSource", (Dev->Sensor != NULL) ? 1 : 2);
    Active = SwCpuActiveProfile(Dev);
    Dev->Requested = (Active >= 0) ? Active : DEFAULT_PROFILE;
    SwCpuTrace(L"ActiveProfile", (ULONG)Active);
    SwCpuTrace(L"CurrentKhz", SwCpuPllxKhz(SwCpuReadCar(Dev, CAR_PLLX_BASE)));
    SwCpuTrace(L"CurrentMv", SwCpuReadMv(Dev));

    if (Dev->Timer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES Attributes;
        WDF_TIMER_CONFIG_INIT(&TimerConfig, SwCpuEvtTimer);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWCPU_TIMER);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->Timer);
        SwCpuTrace(L"TimerCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Timer = NULL;
            return Status;
        }
        SwCpuGetTimer(Dev->Timer)->Device = Device;
    }
    Dev->Stopping = FALSE;
    WdfTimerStart(Dev->Timer, WDF_REL_TIMEOUT_IN_MS(GUARD_MS));
    return STATUS_SUCCESS;
}

VOID SwCpuEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWCPU_DEVICE Dev = SwCpuGetDevice(Device);
    Dev->Stopping = TRUE;
    if (Dev->Timer != NULL) {
        WdfTimerStop(Dev->Timer, TRUE);
    }
    WdfWaitLockAcquire(Dev->Lock, NULL);
    if (Dev->Car != NULL && Dev->Pmic != NULL && SwCpuActiveProfile(Dev) != DEFAULT_PROFILE) {
        (VOID)SwCpuApply(Dev, DEFAULT_PROFILE);    /* leave the UEFI default behind */
    }
    WdfWaitLockRelease(Dev->Lock);
    if (Dev->Pmic != NULL) {
        WdfIoTargetClose(Dev->Pmic);
        WdfObjectDelete(Dev->Pmic);
        Dev->Pmic = NULL;
    }
    if (Dev->Sensor != NULL) {
        WdfIoTargetClose(Dev->Sensor);
        WdfObjectDelete(Dev->Sensor);
        Dev->Sensor = NULL;
    }
}

NTSTATUS SwCpuEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    DECLARE_CONST_UNICODE_STRING(DeviceName, SWCPU_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(LinkName, SWCPU_SYMLINK_NAME);
    PSWCPU_DEVICE Dev;
    WDFDEVICE Device;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwCpuEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwCpuEvtReleaseHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwCpuEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwCpuEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    Status = WdfDeviceInitAssignName(DeviceInit, &DeviceName);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWCPU_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Dev = SwCpuGetDevice(Device);
    Dev->Requested = DEFAULT_PROFILE;
    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &Dev->Lock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfDeviceCreateSymbolicLink(Device, &LinkName);
    SwCpuTrace(L"SymbolicLinkStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
    QueueConfig.EvtIoDeviceControl = SwCpuEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfIoQueueCreate(Device, &QueueConfig, &Attributes, WDF_NO_HANDLE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwCpuParamsPath)) {
        RtlCopyMemory(SwCpuParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwCpuParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwCpuParamsPath);
    }
    SwCpuTrace(L"DriverVersion", 2);
    WDF_DRIVER_CONFIG_INIT(&Config, SwCpuEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
