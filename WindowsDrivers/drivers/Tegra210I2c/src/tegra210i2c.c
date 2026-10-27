/*
 * Tegra210 I2C controller driver for Windows on the Nintendo Switch.
 * SpbCx (simple peripheral bus) controller, KMDF.
 *
 * Version 1 transfers by polling in packet mode, the same sequence the UEFI
 * UsbPowerAudit driver uses for BM92T/BQ24193/MAX17050 (R288 helpers) and
 * Linux i2c-tegra uses per message:
 *   header 0: PROTOCOL_I2C (1 << 4) | PACKET_ID 1 (1 << 16)
 *   header 1: payload size - 1
 *   header 2: (addr << 1) | READ (bit 19) | REPEAT_START (bit 16, not last)
 *   then payload words through TX_FIFO / RX_FIFO, wait PACKET_XFER_COMPLETE.
 * The bus clock divisor is the one the firmware programmed; the controller
 * clock/reset are expected to be on (UEFI uses I2C1 and I2C5).
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spbcx.h>

#define I2C_CNFG            0x000
#define I2C_STATUS          0x01C
#define I2C_TX_FIFO         0x050
#define I2C_RX_FIFO         0x054
#define I2C_FIFO_CONTROL    0x05C
#define I2C_FIFO_STATUS     0x060
#define I2C_INT_MASK        0x064
#define I2C_INT_STATUS      0x068
#define I2C_CLK_DIVISOR     0x06C
#define I2C_BUS_CLEAR_CONFIG 0x084
#define I2C_BUS_CLEAR_STATUS 0x088
#define I2C_CONFIG_LOAD     0x08C
#define I2C_MMIO_SIZE       0x100

#define CNFG_PACKET_MODE    (1u << 10)
#define FIFO_FLUSH_TX_RX    0x3u
#define INT_ARB_LOST        (1u << 2)
#define INT_NO_ACK          (1u << 3)
#define INT_ERR_MASK        0x3Cu
#define INT_ALL_PACKETS     (1u << 6)
#define INT_PACKET_DONE     (1u << 7)
#define INT_BUS_CLEAR_DONE  (1u << 11)
#define HDR0_I2C_PACKET1    0x10010u
#define HDR_REPEAT_START    (1u << 16)
#define HDR_READ            (1u << 19)
#define CONFIG_LOAD_ALL     0x25u
#define I2C_MAX_TRANSFER    4096

#include <pshpack1.h>
typedef struct _PNP_SERIAL_BUS_DESCRIPTOR {
    UCHAR Tag;
    USHORT Length;
    UCHAR RevisionId;
    UCHAR ResourceSourceIndex;
    UCHAR SerialBusType;
    UCHAR GeneralFlags;
    USHORT TypeSpecificFlags;
    UCHAR TypeSpecificRevisionId;
    USHORT TypeDataLength;
} PNP_SERIAL_BUS_DESCRIPTOR, *PPNP_SERIAL_BUS_DESCRIPTOR;

typedef struct _PNP_I2C_SERIAL_BUS_DESCRIPTOR {
    PNP_SERIAL_BUS_DESCRIPTOR SerialBusDescriptor;
    ULONG ConnectionSpeed;
    USHORT SlaveAddress;
} PNP_I2C_SERIAL_BUS_DESCRIPTOR, *PPNP_I2C_SERIAL_BUS_DESCRIPTOR;
#include <poppack.h>

typedef struct _RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER {
    ULONG PropertiesLength;
    UCHAR ConnectionProperties[ANYSIZE_ARRAY];
} RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER, *PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER;

#define I2C_SERIAL_BUS_TYPE 0x01
#define I2C_10BIT_ADDRESS_FLAG 0x0001

typedef struct _TEGRA_I2C_DEVICE {
    volatile UCHAR *Base;
    ULONG Length;
    ULONG PhysLow;              /* low 32 bits of the MMIO base, for logging */
    WDFTIMER TraceTimer;
    volatile LONG Transfers, Errors, Nacks, Timeouts, LastError, LastAddress;
} TEGRA_I2C_DEVICE, *PTEGRA_I2C_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TEGRA_I2C_DEVICE, TegraI2cGetDevice)

typedef struct _TEGRA_I2C_TARGET {
    USHORT Address;
    ULONG Speed;
} TEGRA_I2C_TARGET, *PTEGRA_I2C_TARGET;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TEGRA_I2C_TARGET, TegraI2cGetTarget)

typedef struct _TEGRA_I2C_TIMER {
    WDFDEVICE Device;
} TEGRA_I2C_TIMER, *PTEGRA_I2C_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TEGRA_I2C_TIMER, TegraI2cGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD TegraI2cEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE TegraI2cEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE TegraI2cEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY TegraI2cEvtD0Entry;
EVT_SPB_TARGET_CONNECT TegraI2cEvtTargetConnect;
EVT_SPB_CONTROLLER_READ TegraI2cEvtRead;
EVT_SPB_CONTROLLER_WRITE TegraI2cEvtWrite;
EVT_SPB_CONTROLLER_SEQUENCE TegraI2cEvtSequence;
EVT_WDF_TIMER TegraI2cEvtTraceTimer;

/* ------------------------------------------------------------ diagnostics */

static WCHAR TegraI2cParamsPath[256];

/* DWORD "I2C_<base>_<Name>" under Services\tegra210i2c\Parameters (PASSIVE only). */
static VOID TegraI2cTrace(PTEGRA_I2C_DEVICE Dev, PCWSTR Name, ULONG Value)
{
    WCHAR Full[64];
    UNICODE_STRING Str;
    if (TegraI2cParamsPath[0] == L'\0' || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    RtlInitEmptyUnicodeString(&Str, Full, sizeof(Full));
    if (NT_SUCCESS(RtlUnicodeStringPrintf(&Str, L"I2C_%04X_%s", (Dev != NULL) ? (Dev->PhysLow & 0xFFFF) : 0, Name))) {
        Full[Str.Length / sizeof(WCHAR)] = L'\0';
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, TegraI2cParamsPath, Full, REG_DWORD, &Value, sizeof(Value));
    }
}

/* -------------------------------------------------------------- hardware */

static ULONG RdReg(PTEGRA_I2C_DEVICE Dev, ULONG Off)
{
    return READ_REGISTER_ULONG((PULONG)(Dev->Base + Off));
}

static VOID WrReg(PTEGRA_I2C_DEVICE Dev, ULONG Off, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Dev->Base + Off), Value);
}

static BOOLEAN TegraI2cWaitClear(PTEGRA_I2C_DEVICE Dev, ULONG Off, ULONG Mask, ULONG Microseconds)
{
    ULONG Waited;
    for (Waited = 0; Waited < Microseconds; Waited += 5) {
        if ((RdReg(Dev, Off) & Mask) == 0) {
            return TRUE;
        }
        KeStallExecutionProcessor(5);
    }
    return FALSE;
}

static VOID TegraI2cLoadConfig(PTEGRA_I2C_DEVICE Dev)
{
    WrReg(Dev, I2C_CONFIG_LOAD, CONFIG_LOAD_ALL);
    (VOID)TegraI2cWaitClear(Dev, I2C_CONFIG_LOAD, 1, 10000);
}

static VOID TegraI2cResetFifos(PTEGRA_I2C_DEVICE Dev)
{
    WrReg(Dev, I2C_INT_STATUS, 0xFFFFFFFF);
    WrReg(Dev, I2C_FIFO_CONTROL, RdReg(Dev, I2C_FIFO_CONTROL) | FIFO_FLUSH_TX_RX);
    (VOID)TegraI2cWaitClear(Dev, I2C_FIFO_CONTROL, FIFO_FLUSH_TX_RX, 10000);
}

/* After an arbitration loss or timeout: 9 clock pulses to free a stuck slave. */
static VOID TegraI2cBusClear(PTEGRA_I2C_DEVICE Dev)
{
    ULONG Waited;
    WrReg(Dev, I2C_BUS_CLEAR_CONFIG, (9u << 16) | (1u << 2) | 1u);  /* threshold 9, STOP, enable */
    TegraI2cLoadConfig(Dev);
    for (Waited = 0; Waited < 10000; Waited += 5) {
        if ((RdReg(Dev, I2C_INT_STATUS) & INT_BUS_CLEAR_DONE) != 0) {
            break;
        }
        KeStallExecutionProcessor(5);
    }
    WrReg(Dev, I2C_INT_STATUS, 0xFFFFFFFF);
}

/* One I2C message as one packet. Buffer is a kernel buffer of Length bytes. */
static NTSTATUS TegraI2cTransfer(PTEGRA_I2C_DEVICE Dev, USHORT Address, BOOLEAN Read, PUCHAR Buffer,
                                 ULONG Length, BOOLEAN RepeatStart)
{
    ULONG Done = 0, Waited = 0, Timeout = 20000 + Length * 200, Status, Word, Index;

    if (Length == 0 || Length > I2C_MAX_TRANSFER) {
        return STATUS_INVALID_PARAMETER;
    }
    InterlockedIncrement(&Dev->Transfers);
    TegraI2cResetFifos(Dev);

    WrReg(Dev, I2C_TX_FIFO, HDR0_I2C_PACKET1);
    WrReg(Dev, I2C_TX_FIFO, Length - 1);
    WrReg(Dev, I2C_TX_FIFO, ((ULONG)(Address & 0x7F) << 1) | (Read ? HDR_READ : 0) | (RepeatStart ? HDR_REPEAT_START : 0));

    while (Done < Length) {
        Status = RdReg(Dev, I2C_INT_STATUS);
        if ((Status & INT_ERR_MASK) != 0) {
            goto Error;
        }
        if (Read) {
            ULONG Full = RdReg(Dev, I2C_FIFO_STATUS) & 0x0F;          /* RX words available */
            if (Full != 0) {
                Word = RdReg(Dev, I2C_RX_FIFO);
                for (Index = 0; Index < 4 && Done < Length; ++Index) {
                    Buffer[Done++] = (UCHAR)(Word >> (8 * Index));
                }
                continue;
            }
        } else {
            ULONG Empty = (RdReg(Dev, I2C_FIFO_STATUS) >> 4) & 0x0F;  /* TX words free */
            if (Empty != 0) {
                Word = 0;
                for (Index = 0; Index < 4 && Done < Length; ++Index) {
                    Word |= (ULONG)Buffer[Done++] << (8 * Index);
                }
                WrReg(Dev, I2C_TX_FIFO, Word);
                continue;
            }
        }
        if (Waited >= Timeout) {
            goto Timeout;
        }
        KeStallExecutionProcessor(5);
        Waited += 5;
    }

    for (;;) {
        Status = RdReg(Dev, I2C_INT_STATUS);
        if ((Status & INT_ERR_MASK) != 0) {
            goto Error;
        }
        if ((Status & (INT_PACKET_DONE | INT_ALL_PACKETS)) != 0) {
            WrReg(Dev, I2C_INT_STATUS, Status);
            return STATUS_SUCCESS;
        }
        if (Waited >= Timeout) {
            goto Timeout;
        }
        KeStallExecutionProcessor(5);
        Waited += 5;
    }

Error:
    InterlockedIncrement(&Dev->Errors);
    InterlockedExchange(&Dev->LastError, (LONG)Status);
    InterlockedExchange(&Dev->LastAddress, Address);
    if ((Status & INT_NO_ACK) != 0) {
        InterlockedIncrement(&Dev->Nacks);
        TegraI2cResetFifos(Dev);
        return STATUS_NO_SUCH_DEVICE;
    }
    TegraI2cResetFifos(Dev);
    TegraI2cBusClear(Dev);
    return STATUS_IO_DEVICE_ERROR;

Timeout:
    InterlockedIncrement(&Dev->Timeouts);
    InterlockedExchange(&Dev->LastAddress, Address);
    TegraI2cResetFifos(Dev);
    TegraI2cBusClear(Dev);
    return STATUS_IO_TIMEOUT;
}

/* Copy between an MDL chain and a flat buffer. */
static NTSTATUS TegraI2cMdlCopy(PMDL Mdl, PUCHAR Flat, SIZE_T Length, BOOLEAN ToFlat)
{
    SIZE_T Offset = 0;
    while (Mdl != NULL && Offset < Length) {
        PUCHAR Va = (PUCHAR)MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority | MdlMappingNoExecute);
        SIZE_T Chunk = MmGetMdlByteCount(Mdl);
        if (Va == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (Chunk > Length - Offset) {
            Chunk = Length - Offset;
        }
        if (ToFlat) {
            RtlCopyMemory(Flat + Offset, Va, Chunk);
        } else {
            RtlCopyMemory(Va, Flat + Offset, Chunk);
        }
        Offset += Chunk;
        Mdl = Mdl->Next;
    }
    return (Offset == Length) ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}

/* Run SPB transfers [0, Count) of Request; repeated start between them. */
static VOID TegraI2cRunRequest(WDFDEVICE Controller, SPBTARGET Target, SPBREQUEST Request, ULONG Count)
{
    PTEGRA_I2C_DEVICE Dev = TegraI2cGetDevice(Controller);
    PTEGRA_I2C_TARGET Tgt = TegraI2cGetTarget(Target);
    NTSTATUS Status = STATUS_SUCCESS;
    SIZE_T Total = 0;
    ULONG Index;

    for (Index = 0; Index < Count && NT_SUCCESS(Status); ++Index) {
        SPB_TRANSFER_DESCRIPTOR Desc;
        PMDL Mdl = NULL;
        PUCHAR Bounce;
        BOOLEAN Read;

        SPB_TRANSFER_DESCRIPTOR_INIT(&Desc);
        SpbRequestGetTransferParameters(Request, Index, &Desc, &Mdl);
        Read = (Desc.Direction == SpbTransferDirectionFromDevice);
        if (Desc.TransferLength == 0 || Desc.TransferLength > I2C_MAX_TRANSFER || Mdl == NULL) {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (Desc.DelayInUs != 0) {
            KeStallExecutionProcessor(Desc.DelayInUs > 1000 ? 1000 : Desc.DelayInUs);
        }
        Bounce = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, Desc.TransferLength, 'c2iT');
        if (Bounce == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        if (!Read) {
            Status = TegraI2cMdlCopy(Mdl, Bounce, Desc.TransferLength, TRUE);
        }
        if (NT_SUCCESS(Status)) {
            Status = TegraI2cTransfer(Dev, Tgt->Address, Read, Bounce, (ULONG)Desc.TransferLength, (BOOLEAN)(Index + 1 < Count));
        }
        if (NT_SUCCESS(Status) && Read) {
            Status = TegraI2cMdlCopy(Mdl, Bounce, Desc.TransferLength, FALSE);
        }
        if (NT_SUCCESS(Status)) {
            Total += Desc.TransferLength;
        }
        ExFreePoolWithTag(Bounce, 'c2iT');
    }
    WdfRequestSetInformation((WDFREQUEST)Request, Total);
    SpbRequestComplete(Request, Status);
}

/* -------------------------------------------------------------------- SPB */

NTSTATUS TegraI2cEvtTargetConnect(WDFDEVICE Controller, SPBTARGET Target)
{
    SPB_CONNECTION_PARAMETERS Params;
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Conn;
    PPNP_I2C_SERIAL_BUS_DESCRIPTOR I2c;
    PTEGRA_I2C_TARGET Tgt = TegraI2cGetTarget(Target);
    PTEGRA_I2C_DEVICE Dev = TegraI2cGetDevice(Controller);
    static LONG ConnectCount;

    /* v2: record every connect attempt (the battery's open failed with
     * STATUS_INVALID_PARAMETER and this is the only place returning it). */
    TegraI2cTrace(Dev, L"ConnectCount", (ULONG)InterlockedIncrement(&ConnectCount));
    SPB_CONNECTION_PARAMETERS_INIT(&Params);
    SpbTargetGetConnectionParameters(Target, &Params);
    Conn = (PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER)Params.ConnectionParameters;
    TegraI2cTrace(Dev, L"ConnectHasParams", Conn != NULL);
    if (Conn == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    TegraI2cTrace(Dev, L"ConnectPropsLength", Conn->PropertiesLength);
    {
        /* v3: v2 saw PropertiesLength = 1 on this Windows build, so the
         * buffer layout is not the documented length + descriptor. Dump the
         * first 32 bytes and find the ACPI GenericSerialBus descriptor
         * (large item tag 0x8E, SerialBusType 1 = I2C) instead of trusting
         * the length field. */
        PUCHAR Raw = (PUCHAR)Conn;
        ULONG Offset;
        WCHAR Name[16] = L"ConnectRaw0";
        I2c = NULL;
        for (Offset = 0; Offset < 32; Offset += 4) {
            Name[10] = (WCHAR)(L'0' + Offset / 4);
            TegraI2cTrace(Dev, Name, *(ULONG UNALIGNED *)(Raw + Offset));
        }
        for (Offset = 0; Offset <= 16; ++Offset) {
            PPNP_I2C_SERIAL_BUS_DESCRIPTOR Candidate = (PPNP_I2C_SERIAL_BUS_DESCRIPTOR)(Raw + Offset);
            if (Candidate->SerialBusDescriptor.Tag == 0x8E &&
                Candidate->SerialBusDescriptor.SerialBusType == I2C_SERIAL_BUS_TYPE &&
                Candidate->SerialBusDescriptor.Length >= sizeof(PNP_I2C_SERIAL_BUS_DESCRIPTOR) - 3) {
                I2c = Candidate;
                TegraI2cTrace(Dev, L"ConnectDescriptorOffset", Offset);
                break;
            }
        }
        if (I2c == NULL) {
            TegraI2cTrace(Dev, L"ConnectDescriptorOffset", 0xFFFF);
            return STATUS_INVALID_PARAMETER;
        }
    }
    TegraI2cTrace(Dev, L"ConnectTypeFlags", I2c->SerialBusDescriptor.TypeSpecificFlags);
    TegraI2cTrace(Dev, L"ConnectTypeDataLength", I2c->SerialBusDescriptor.TypeDataLength);
    TegraI2cTrace(Dev, L"ConnectAddrRaw", I2c->SlaveAddress);
    if ((I2c->SerialBusDescriptor.TypeSpecificFlags & I2C_10BIT_ADDRESS_FLAG) != 0 || I2c->SlaveAddress > 0x7F) {
        return STATUS_NOT_SUPPORTED;
    }
    Tgt->Address = I2c->SlaveAddress;
    Tgt->Speed = I2c->ConnectionSpeed;
    TegraI2cTrace(Dev, L"LastConnectAddr", Tgt->Address);
    TegraI2cTrace(Dev, L"LastConnectSpeed", Tgt->Speed);
    return STATUS_SUCCESS;
}

VOID TegraI2cEvtRead(WDFDEVICE Controller, SPBTARGET Target, SPBREQUEST Request, size_t Length)
{
    UNREFERENCED_PARAMETER(Length);
    TegraI2cRunRequest(Controller, Target, Request, 1);
}

VOID TegraI2cEvtWrite(WDFDEVICE Controller, SPBTARGET Target, SPBREQUEST Request, size_t Length)
{
    UNREFERENCED_PARAMETER(Length);
    TegraI2cRunRequest(Controller, Target, Request, 1);
}

VOID TegraI2cEvtSequence(WDFDEVICE Controller, SPBTARGET Target, SPBREQUEST Request, ULONG TransferCount)
{
    TegraI2cRunRequest(Controller, Target, Request, TransferCount);
}

/* ------------------------------------------------------------------- PnP */

VOID TegraI2cEvtTraceTimer(WDFTIMER Timer)
{
    PTEGRA_I2C_DEVICE Dev = TegraI2cGetDevice(TegraI2cGetTimer(Timer)->Device);
    TegraI2cTrace(Dev, L"Transfers", (ULONG)Dev->Transfers);
    TegraI2cTrace(Dev, L"Errors", (ULONG)Dev->Errors);
    TegraI2cTrace(Dev, L"Nacks", (ULONG)Dev->Nacks);
    TegraI2cTrace(Dev, L"Timeouts", (ULONG)Dev->Timeouts);
    TegraI2cTrace(Dev, L"LastErrorStatus", (ULONG)Dev->LastError);
    TegraI2cTrace(Dev, L"LastErrorAddr", (ULONG)Dev->LastAddress);
}

NTSTATUS TegraI2cEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PTEGRA_I2C_DEVICE Dev = TegraI2cGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeMemory && Dev->Base == NULL) {
            if (Desc->u.Memory.Length < I2C_MMIO_SIZE) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            Dev->Base = (volatile UCHAR *)MmMapIoSpaceEx(Desc->u.Memory.Start, Desc->u.Memory.Length,
                                                         PAGE_READWRITE | PAGE_NOCACHE);
            if (Dev->Base == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            Dev->Length = Desc->u.Memory.Length;
            Dev->PhysLow = Desc->u.Memory.Start.LowPart;
        }
    }
    if (Dev->Base == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    TegraI2cTrace(Dev, L"ResourceCount", Count);
    TegraI2cTrace(Dev, L"CnfgBefore", RdReg(Dev, I2C_CNFG));
    TegraI2cTrace(Dev, L"ClkDivisor", RdReg(Dev, I2C_CLK_DIVISOR));

    /* Packet mode, interrupts masked (version 1 polls). */
    WrReg(Dev, I2C_INT_MASK, 0);
    WrReg(Dev, I2C_CNFG, RdReg(Dev, I2C_CNFG) | CNFG_PACKET_MODE);
    TegraI2cLoadConfig(Dev);
    TegraI2cResetFifos(Dev);
    TegraI2cTrace(Dev, L"CnfgAfter", RdReg(Dev, I2C_CNFG));

    if (Dev->TraceTimer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES Attributes;
        WDF_TIMER_CONFIG_INIT_PERIODIC(&TimerConfig, TegraI2cEvtTraceTimer, 2000);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, TEGRA_I2C_TIMER);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        if (NT_SUCCESS(WdfTimerCreate(&TimerConfig, &Attributes, &Dev->TraceTimer))) {
            TegraI2cGetTimer(Dev->TraceTimer)->Device = Device;
            WdfTimerStart(Dev->TraceTimer, WDF_REL_TIMEOUT_IN_MS(2000));
        } else {
            Dev->TraceTimer = NULL;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS TegraI2cEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PTEGRA_I2C_DEVICE Dev = TegraI2cGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (Dev->TraceTimer != NULL) {
        WdfTimerStop(Dev->TraceTimer, TRUE);
        WdfObjectDelete(Dev->TraceTimer);
        Dev->TraceTimer = NULL;
    }
    if (Dev->Base != NULL) {
        MmUnmapIoSpace((PVOID)Dev->Base, Dev->Length);
        Dev->Base = NULL;
    }
    return STATUS_SUCCESS;
}

/* Self-test: read one register from a known chip on this bus (read-only). */
static VOID TegraI2cSelfTest(PTEGRA_I2C_DEVICE Dev, USHORT Address, UCHAR Register, PCWSTR Name)
{
    UCHAR Value = 0;
    NTSTATUS Status = TegraI2cTransfer(Dev, Address, FALSE, &Register, 1, TRUE);
    if (NT_SUCCESS(Status)) {
        Status = TegraI2cTransfer(Dev, Address, TRUE, &Value, 1, FALSE);
    }
    TegraI2cTrace(Dev, Name, NT_SUCCESS(Status) ? (0x100u | Value) : (ULONG)Status);
}

NTSTATUS TegraI2cEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PTEGRA_I2C_DEVICE Dev = TegraI2cGetDevice(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    /* Value 0x1XX = success with register value XX, otherwise the NTSTATUS. */
    if (Dev->PhysLow == 0x7000C000) {
        TegraI2cSelfTest(Dev, 0x4C, 0x00, L"SelfTest_TMP451_Local");    /* board temperature, C */
        TegraI2cSelfTest(Dev, 0x4C, 0x01, L"SelfTest_TMP451_Remote");   /* SoC temperature, C */
        TegraI2cSelfTest(Dev, 0x6B, 0x0A, L"SelfTest_BQ24193_Rev");     /* expect 0x2F */
    } else if (Dev->PhysLow == 0x7000D000) {
        TegraI2cSelfTest(Dev, 0x3C, 0x15, L"SelfTest_MAX77620_OnOffStat");
        TegraI2cSelfTest(Dev, 0x1B, 0x00, L"SelfTest_MAX77621_Vout");   /* CPU rail, expect 0xC3 after R365 */
    }
    TegraI2cTrace(Dev, L"D0EntryDone", 1);
    return STATUS_SUCCESS;
}

NTSTATUS TegraI2cEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    SPB_CONTROLLER_CONFIG SpbConfig;
    WDFDEVICE Device;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    Status = SpbDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = TegraI2cEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = TegraI2cEvtReleaseHardware;
    Pnp.EvtDeviceD0Entry = TegraI2cEvtD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, TEGRA_I2C_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    SPB_CONTROLLER_CONFIG_INIT(&SpbConfig);
    SpbConfig.PowerManaged = WdfTrue;
    SpbConfig.EvtSpbTargetConnect = TegraI2cEvtTargetConnect;
    SpbConfig.EvtSpbIoRead = TegraI2cEvtRead;
    SpbConfig.EvtSpbIoWrite = TegraI2cEvtWrite;
    SpbConfig.EvtSpbIoSequence = TegraI2cEvtSequence;
    Status = SpbDeviceInitialize(Device, &SpbConfig);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, TEGRA_I2C_TARGET);
    SpbControllerSetTargetAttributes(Device, &Attributes);
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(TegraI2cParamsPath)) {
        RtlCopyMemory(TegraI2cParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&TegraI2cParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, TegraI2cParamsPath);
    }
    TegraI2cTrace(NULL, L"DriverVersion", 3);

    WDF_DRIVER_CONFIG_INIT(&Config, TegraI2cEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
