/*
 * Nintendo Switch light sensor and accelerometer driver, KMDF.
 *
 * ACPI device SENS (ACPI\SWOA0901), UEFI R379+:
 *   Memory32Fixed 0x7000DA00: SPI4 controller. The IMU (LSM6DS3H, L4T DT
 *     imu@0) is on CS0, SPI mode 3. UEFI R379 clocks SPI4 (PLLP / 100 =
 *     4.08 MHz) and sets its pins; this driver runs the controller in PIO
 *     mode, one full-duplex block per register access with the hardware CS.
 *   I2cSerialBus 0x29 on I2C2: BH1730FVC ambient light sensor.
 * Either sensor may fail on its own; the other keeps working.
 *
 * Every 200 ms the driver reads the light (with automatic gain) and the
 * acceleration, and returns the last values through \\.\SwitchSensor. The
 * control center turns them into the screen brightness and orientation.
 *
 * Lux: BH1730 datasheet formula with the Switch window coefficients from the
 * L4T DT (rohm,opt-win-coeff: ratio x1000, visible, IR). Integration time is
 * fixed at 38 cycles (102.6 ms, the datasheet's reference time), so
 * lux = (cv * DATA0 - ci * DATA1) / 1000 / gain.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>

#define IOCTL_SWSENS_GET  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_READ_ACCESS)   /* 0x00226080 */
#define POLL_MS           200
#define TRACE_EVERY       50            /* registry trace every 10 s */

#define SENS_ALS_VALID    1u
#define SENS_IMU_VALID    2u

typedef struct _SWSENS_STATUS {
    ULONG Version;          /* 1 */
    ULONG Flags;            /* SENS_ALS_VALID, SENS_IMU_VALID */
    ULONG LuxMilli;         /* lux x 1000 */
    ULONG Data0;            /* visible + IR */
    ULONG Data1;            /* IR */
    ULONG Gain;             /* 1, 2, 64, 128 */
    LONG AccelX;            /* mg, chip axes */
    LONG AccelY;
    LONG AccelZ;
    ULONG ImuId;            /* WHO_AM_I */
    ULONG Samples;
} SWSENS_STATUS;

/* BH1730: command byte 0x80 | register. */
#define ALS_CMD           0x80
#define ALS_CONTROL       0x00
#define ALS_TIMING        0x01
#define ALS_GAIN          0x07
#define ALS_PART_ID       0x12
#define ALS_DATA0         0x14
#define ALS_DATA1         0x16
#define ALS_POWER_ADC     0x03
#define ALS_ITIME_38      0xDA          /* 256 - 38 */
#define ALS_HIGH          0xF000u       /* lower the gain above this */
#define ALS_LOW           0x0100u       /* raise the gain below this */

static const ULONG AlsGainMult[4] = { 1, 2, 64, 128 };
static const ULONG AlsCoeff[5][3] = {   /* L4T DT rohm,opt-win-coeff */
    { 500, 5002, 7502 }, { 754, 2250, 2000 }, { 1029, 1999, 1667 }, { 1373, 884, 583 }, { 1879, 309, 165 },
};

/* Tegra SPI (tegra114 layout), in ULONG units. */
#define SPI_COMMAND1      (0x000 / 4)
#define SPI_TRANS_STATUS  (0x010 / 4)
#define SPI_FIFO_STATUS   (0x014 / 4)
#define SPI_DMA_BLK       (0x024 / 4)
#define SPI_TX_FIFO       (0x108 / 4)
#define SPI_RX_FIFO       (0x188 / 4)
#define SPI_CMD1_IMU      ((1u << 30) | (3u << 28) | (1u << 22) | (1u << 12) | (1u << 11) | 7u)  /* master, mode 3, CS0 idle high, RX+TX, 8 bit */
#define SPI_PIO           (1u << 31)
#define SPI_RDY           (1u << 30)
#define SPI_FIFO_FLUSH    ((1u << 15) | (1u << 14))
#define SPI_FIFO_ERRORS   0xF0u

/* LSM6DS3H / LSM6DSO: read = register | 0x80. */
#define IMU_READ          0x80
#define IMU_WHO_AM_I      0x0F
#define IMU_CTRL1_XL      0x10
#define IMU_CTRL3_C       0x12
#define IMU_OUTX_L_XL     0x28
#define IMU_XL_104HZ_2G   0x40
#define IMU_BDU_IF_INC    0x44

typedef struct _SWSENS_DEVICE {
    volatile ULONG *Spi;
    ULONG SpiLength;
    LARGE_INTEGER AlsId;
    BOOLEAN HaveAls;
    WDFIOTARGET Als;
    WDFTIMER Timer;
    volatile BOOLEAN Stopping;
    BOOLEAN AlsOk;
    BOOLEAN ImuOk;
    ULONG GainIndex;
    BOOLEAN SkipSample;     /* the first sample after a gain change is mixed */
    ULONG AlsFailures;
    WDFSPINLOCK Lock;
    SWSENS_STATUS Last;
} SWSENS_DEVICE, *PSWSENS_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWSENS_DEVICE, SwSensGetDevice)

typedef struct _SWSENS_TIMER {
    WDFDEVICE Device;
} SWSENS_TIMER, *PSWSENS_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWSENS_TIMER, SwSensGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwSensEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwSensEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwSensEvtReleaseHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwSensEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwSensEvtSelfManagedIoCleanup;
EVT_WDF_TIMER SwSensEvtTimer;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SwSensEvtIoDeviceControl;

static WCHAR SwSensParamsPath[256];

static VOID SwSensTrace(PCWSTR Name, ULONG Value)
{
    if (SwSensParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwSensParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

/* ------------------------------------------------------------------- I2C */

static NTSTATUS SwSensOpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
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

static NTSTATUS SwSensAlsRead(PSWSENS_DEVICE Dev, UCHAR Register, PUCHAR Value, ULONG Length)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    PSPB_TRANSFER_LIST_ENTRY Entries = Sequence.List.Transfers;
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_REQUEST_SEND_OPTIONS Options;
    UCHAR Command = (UCHAR)(ALS_CMD | Register);
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Dev->Als == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Entries[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &Command, 1);
    Entries[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Value, Length);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Sequence, sizeof(Sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
    Status = WdfIoTargetSendIoctlSynchronously(Dev->Als, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 1 + Length) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

static NTSTATUS SwSensAlsWrite(PSWSENS_DEVICE Dev, UCHAR Register, UCHAR Value)
{
    UCHAR Buffer[2] = { (UCHAR)(ALS_CMD | Register), Value };
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Dev->Als == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, Buffer, sizeof(Buffer));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
    Status = WdfIoTargetSendWriteSynchronously(Dev->Als, NULL, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != sizeof(Buffer)) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

static NTSTATUS SwSensAlsStart(PSWSENS_DEVICE Dev)
{
    UCHAR Part = 0;
    NTSTATUS Status = SwSensAlsRead(Dev, ALS_PART_ID, &Part, 1);
    SwSensTrace(L"AlsPartIdStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    SwSensTrace(L"AlsPartId", Part);
    if ((Part & 0xF0) != 0x70) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Dev->GainIndex = 2;                 /* x64: indoor light lands mid-range */
    Status = SwSensAlsWrite(Dev, ALS_TIMING, ALS_ITIME_38);
    if (NT_SUCCESS(Status)) {
        Status = SwSensAlsWrite(Dev, ALS_GAIN, (UCHAR)Dev->GainIndex);
    }
    if (NT_SUCCESS(Status)) {
        Status = SwSensAlsWrite(Dev, ALS_CONTROL, ALS_POWER_ADC);
    }
    SwSensTrace(L"AlsStartStatus", (ULONG)Status);
    Dev->SkipSample = TRUE;
    return Status;
}

static ULONG SwSensLuxMilli(ULONG Data0, ULONG Data1, ULONG Gain)
{
    ULONG Index;
    LONGLONG Value;

    if (Data0 == 0) {
        return 0;
    }
    for (Index = 0; Index < RTL_NUMBER_OF(AlsCoeff); ++Index) {
        if (Data1 * 1000 / Data0 < AlsCoeff[Index][0]) {
            break;
        }
    }
    if (Index == RTL_NUMBER_OF(AlsCoeff)) {
        return 0;                       /* almost only IR */
    }
    Value = (LONGLONG)AlsCoeff[Index][1] * Data0 - (LONGLONG)AlsCoeff[Index][2] * Data1;
    return (Value <= 0) ? 0 : (ULONG)(Value / Gain);    /* coefficients are x1000: this is lux x 1000 */
}

/* One light sample; returns FALSE when there is no new value this time. */
static BOOLEAN SwSensAlsSample(PSWSENS_DEVICE Dev, PULONG Data0, PULONG Data1, PULONG LuxMilli)
{
    UCHAR Raw[2];
    ULONG Gain;

    if (!NT_SUCCESS(SwSensAlsRead(Dev, ALS_DATA0, Raw, 2))) {
        return FALSE;
    }
    *Data0 = Raw[0] | ((ULONG)Raw[1] << 8);
    if (!NT_SUCCESS(SwSensAlsRead(Dev, ALS_DATA1, Raw, 2))) {
        return FALSE;
    }
    *Data1 = Raw[0] | ((ULONG)Raw[1] << 8);
    if (Dev->SkipSample) {
        Dev->SkipSample = FALSE;
        return FALSE;
    }
    Gain = AlsGainMult[Dev->GainIndex];
    *LuxMilli = SwSensLuxMilli(*Data0, *Data1, Gain);
    if ((*Data0 >= ALS_HIGH && Dev->GainIndex > 0) || (*Data0 < ALS_LOW && Dev->GainIndex < 3)) {
        ULONG Next = (*Data0 >= ALS_HIGH) ? Dev->GainIndex - 1 : Dev->GainIndex + 1;
        if (NT_SUCCESS(SwSensAlsWrite(Dev, ALS_GAIN, (UCHAR)Next))) {
            Dev->GainIndex = Next;
            Dev->SkipSample = TRUE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------- SPI */

/* One full-duplex block of Length (<= 16) bytes on CS0. */
static BOOLEAN SwSensSpiXfer(PSWSENS_DEVICE Dev, const UCHAR *Tx, PUCHAR Rx, ULONG Length)
{
    volatile ULONG *S = Dev->Spi;
    ULONG Index;

    WRITE_REGISTER_ULONG((PULONG)&S[SPI_FIFO_STATUS], SPI_FIFO_FLUSH | SPI_FIFO_ERRORS);
    for (Index = 0; Index < 1000 && (READ_REGISTER_ULONG((PULONG)&S[SPI_FIFO_STATUS]) & SPI_FIFO_FLUSH) != 0; ++Index) {
        KeStallExecutionProcessor(1);
    }
    WRITE_REGISTER_ULONG((PULONG)&S[SPI_TRANS_STATUS], SPI_RDY);
    WRITE_REGISTER_ULONG((PULONG)&S[SPI_COMMAND1], SPI_CMD1_IMU);
    WRITE_REGISTER_ULONG((PULONG)&S[SPI_DMA_BLK], Length - 1);
    for (Index = 0; Index < Length; ++Index) {
        WRITE_REGISTER_ULONG((PULONG)&S[SPI_TX_FIFO], Tx[Index]);
    }
    WRITE_REGISTER_ULONG((PULONG)&S[SPI_COMMAND1], SPI_CMD1_IMU | SPI_PIO);
    for (Index = 0; Index < 1000 && (READ_REGISTER_ULONG((PULONG)&S[SPI_TRANS_STATUS]) & SPI_RDY) == 0; ++Index) {
        KeStallExecutionProcessor(1);
    }
    if ((READ_REGISTER_ULONG((PULONG)&S[SPI_TRANS_STATUS]) & SPI_RDY) == 0) {
        return FALSE;
    }
    for (Index = 0; Index < Length; ++Index) {
        Rx[Index] = (UCHAR)READ_REGISTER_ULONG((PULONG)&S[SPI_RX_FIFO]);
    }
    WRITE_REGISTER_ULONG((PULONG)&S[SPI_TRANS_STATUS], SPI_RDY);
    return TRUE;
}

static BOOLEAN SwSensImuWrite(PSWSENS_DEVICE Dev, UCHAR Register, UCHAR Value)
{
    UCHAR Tx[2] = { Register, Value }, Rx[2];
    return SwSensSpiXfer(Dev, Tx, Rx, 2);
}

static NTSTATUS SwSensImuStart(PSWSENS_DEVICE Dev)
{
    UCHAR Tx[2] = { IMU_READ | IMU_WHO_AM_I, 0 }, Rx[2] = { 0, 0 };

    WRITE_REGISTER_ULONG((PULONG)&Dev->Spi[SPI_COMMAND1], SPI_CMD1_IMU);
    KeStallExecutionProcessor(10);
    if (!SwSensSpiXfer(Dev, Tx, Rx, 2)) {
        SwSensTrace(L"ImuSpiTimeout", 1);
        return STATUS_IO_TIMEOUT;
    }
    Dev->Last.ImuId = Rx[1];
    SwSensTrace(L"ImuWhoAmI", Rx[1]);
    if (Rx[1] != 0x69 && Rx[1] != 0x6A && Rx[1] != 0x6C) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;   /* not an ST LSM6DS* (ICM-40607 is not handled yet) */
    }
    if (!SwSensImuWrite(Dev, IMU_CTRL3_C, IMU_BDU_IF_INC) || !SwSensImuWrite(Dev, IMU_CTRL1_XL, IMU_XL_104HZ_2G)) {
        return STATUS_IO_TIMEOUT;
    }
    return STATUS_SUCCESS;
}

/* Acceleration in mg (+-2 g range: 0.061 mg per LSB). */
static BOOLEAN SwSensImuSample(PSWSENS_DEVICE Dev, PLONG X, PLONG Y, PLONG Z)
{
    UCHAR Tx[7] = { IMU_READ | IMU_OUTX_L_XL, 0, 0, 0, 0, 0, 0 }, Rx[7];
    if (!SwSensSpiXfer(Dev, Tx, Rx, 7)) {
        return FALSE;
    }
    *X = (LONG)(SHORT)(Rx[1] | (Rx[2] << 8)) * 61 / 1000;
    *Y = (LONG)(SHORT)(Rx[3] | (Rx[4] << 8)) * 61 / 1000;
    *Z = (LONG)(SHORT)(Rx[5] | (Rx[6] << 8)) * 61 / 1000;
    return TRUE;
}

/* ----------------------------------------------------------------- poll */

VOID SwSensEvtTimer(WDFTIMER Timer)
{
    PSWSENS_DEVICE Dev = SwSensGetDevice(SwSensGetTimer(Timer)->Device);
    ULONG Data0 = 0, Data1 = 0, Lux = 0, Samples;
    LONG X = 0, Y = 0, Z = 0;
    BOOLEAN NewLight = FALSE, NewAccel = FALSE;

    if (Dev->AlsOk) {
        NewLight = SwSensAlsSample(Dev, &Data0, &Data1, &Lux);
        if (NewLight) {
            Dev->AlsFailures = 0;
        } else if (!Dev->SkipSample && ++Dev->AlsFailures >= 10) {
            Dev->AlsOk = FALSE;         /* stop asking a sensor that does not answer */
            SwSensTrace(L"AlsStopped", 1);
        }
    }
    if (Dev->ImuOk) {
        NewAccel = SwSensImuSample(Dev, &X, &Y, &Z);
    }

    WdfSpinLockAcquire(Dev->Lock);
    if (NewLight) {
        Dev->Last.Data0 = Data0;
        Dev->Last.Data1 = Data1;
        Dev->Last.LuxMilli = Lux;
        Dev->Last.Gain = AlsGainMult[Dev->GainIndex];
        Dev->Last.Flags |= SENS_ALS_VALID;
    } else if (!Dev->AlsOk) {
        Dev->Last.Flags &= ~SENS_ALS_VALID;
    }
    if (NewAccel) {
        Dev->Last.AccelX = X;
        Dev->Last.AccelY = Y;
        Dev->Last.AccelZ = Z;
        Dev->Last.Flags |= SENS_IMU_VALID;
    }
    Samples = ++Dev->Last.Samples;
    WdfSpinLockRelease(Dev->Lock);

    if (Samples % TRACE_EVERY == 1) {
        SwSensTrace(L"Samples", Samples);
        SwSensTrace(L"LuxMilli", Dev->Last.LuxMilli);
        SwSensTrace(L"AlsData0", Dev->Last.Data0);
        SwSensTrace(L"AlsData1", Dev->Last.Data1);
        SwSensTrace(L"AlsGain", Dev->Last.Gain);
        SwSensTrace(L"AccelX", (ULONG)Dev->Last.AccelX);
        SwSensTrace(L"AccelY", (ULONG)Dev->Last.AccelY);
        SwSensTrace(L"AccelZ", (ULONG)Dev->Last.AccelZ);
    }
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));    /* one-shot passive timer, re-armed */
    }
}

VOID SwSensEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    PSWSENS_DEVICE Dev = SwSensGetDevice(WdfIoQueueGetDevice(Queue));
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Info = 0;
    UNREFERENCED_PARAMETER(OutLength);
    UNREFERENCED_PARAMETER(InLength);

    if (Code == IOCTL_SWSENS_GET) {
        SWSENS_STATUS *Out;
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID *)&Out, NULL);
        if (NT_SUCCESS(Status)) {
            WdfSpinLockAcquire(Dev->Lock);
            *Out = Dev->Last;
            WdfSpinLockRelease(Dev->Lock);
            Out->Version = 1;
            Info = sizeof(*Out);
        }
    }
    WdfRequestCompleteWithInformation(Request, Status, Info);
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwSensEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWSENS_DEVICE Dev = SwSensGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc == NULL) {
            continue;
        }
        if (Desc->Type == CmResourceTypeMemory && Dev->Spi == NULL) {
            if (Desc->u.Memory.Length < 0x200) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            Dev->Spi = (volatile ULONG *)MmMapIoSpaceEx(Desc->u.Memory.Start, Desc->u.Memory.Length,
                                                        PAGE_READWRITE | PAGE_NOCACHE);
            if (Dev->Spi == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            Dev->SpiLength = Desc->u.Memory.Length;
        } else if (Desc->Type == CmResourceTypeConnection && !Dev->HaveAls &&
                   Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                   Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            Dev->AlsId.LowPart = Desc->u.Connection.IdLowPart;
            Dev->AlsId.HighPart = (LONG)Desc->u.Connection.IdHighPart;
            Dev->HaveAls = TRUE;
        }
    }
    SwSensTrace(L"HaveSpi", Dev->Spi != NULL);
    SwSensTrace(L"HaveAls", Dev->HaveAls);
    return (Dev->Spi == NULL && !Dev->HaveAls) ? STATUS_DEVICE_CONFIGURATION_ERROR : STATUS_SUCCESS;
}

NTSTATUS SwSensEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWSENS_DEVICE Dev = SwSensGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (Dev->Spi != NULL) {
        MmUnmapIoSpace((PVOID)Dev->Spi, Dev->SpiLength);
        Dev->Spi = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS SwSensEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWSENS_DEVICE Dev = SwSensGetDevice(Device);
    NTSTATUS Status = STATUS_SUCCESS;

    Dev->AlsOk = FALSE;
    if (Dev->HaveAls) {
        if (Dev->Als == NULL) {
            Status = SwSensOpenTarget(Device, Dev->AlsId, &Dev->Als);
            SwSensTrace(L"OpenAlsStatus", (ULONG)Status);
        }
        Dev->AlsOk = NT_SUCCESS(SwSensAlsStart(Dev));
        Dev->AlsFailures = 0;
    }
    Dev->ImuOk = (Dev->Spi != NULL) && NT_SUCCESS(Status = SwSensImuStart(Dev));
    if (Dev->Spi != NULL) {
        SwSensTrace(L"ImuStartStatus", (ULONG)Status);
    }
    SwSensTrace(L"AlsOk", Dev->AlsOk);
    SwSensTrace(L"ImuOk", Dev->ImuOk);

    if (Dev->Timer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES Attributes;
        WDF_TIMER_CONFIG_INIT(&TimerConfig, SwSensEvtTimer);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWSENS_TIMER);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->Timer);
        SwSensTrace(L"TimerCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Timer = NULL;
            return Status;
        }
        SwSensGetTimer(Dev->Timer)->Device = Device;
    }
    Dev->Stopping = FALSE;
    WdfTimerStart(Dev->Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));
    return STATUS_SUCCESS;
}

VOID SwSensEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWSENS_DEVICE Dev = SwSensGetDevice(Device);
    Dev->Stopping = TRUE;
    if (Dev->Timer != NULL) {
        WdfTimerStop(Dev->Timer, TRUE);
    }
    if (Dev->AlsOk) {
        (VOID)SwSensAlsWrite(Dev, ALS_CONTROL, 0);      /* power the light sensor down */
    }
    if (Dev->ImuOk) {
        (VOID)SwSensImuWrite(Dev, IMU_CTRL1_XL, 0);     /* accelerometer power-down */
    }
}

NTSTATUS SwSensEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    DECLARE_CONST_UNICODE_STRING(DeviceName, L"\\Device\\SwitchSensor");
    DECLARE_CONST_UNICODE_STRING(LinkName, L"\\DosDevices\\SwitchSensor");
    WDFDEVICE Device;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwSensEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwSensEvtReleaseHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwSensEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwSensEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    Status = WdfDeviceInitAssignName(DeviceInit, &DeviceName);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWSENS_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &SwSensGetDevice(Device)->Lock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfDeviceCreateSymbolicLink(Device, &LinkName);
    SwSensTrace(L"SymbolicLinkStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoDeviceControl = SwSensEvtIoDeviceControl;
    return WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwSensParamsPath)) {
        RtlCopyMemory(SwSensParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwSensParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwSensParamsPath);
    }
    SwSensTrace(L"DriverVersion", 1);
    WDF_DRIVER_CONFIG_INIT(&Config, SwSensEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
