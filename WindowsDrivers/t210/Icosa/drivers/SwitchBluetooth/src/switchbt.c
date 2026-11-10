/*
 * Nintendo Switch Bluetooth driver, KMDF. Stage 2: Windows Bluetooth over BthMini.
 *
 * ACPI device BTH0 (ACPI\SWOA0B01), UEFI R390+:
 *   Memory32Fixed 0x70006300: UART-D (Tegra HSUART, 16550 layout, registers
 *     4 bytes apart). The BCM4356A3 Bluetooth core is on it (H4 transport).
 *   Interrupt GSI 122: UART-D.
 * UEFI R390 sets the pins, clocks UART-D from PLLP (408 MHz) and powers the
 * chip (PH4). The Broadcom ROM listens at 115200 8N1 after power-up.
 *
 * At each power-up (D0) the driver polls the chip once with interrupts off:
 * HCI_Reset, Read_Local_Version, Write_BD_ADDR (the ROM answers with the
 * placeholder AA:AA:AA:AA:AA:AA, so the driver makes one address per Switch
 * and keeps it in the registry), Read_BD_ADDR and Read_Local_Name. The
 * answers go to Services\switchbt\Parameters.
 *
 * Then it works as an extensible transport (bthxddi.h) under the inbox
 * BthMini: the driver creates one child device with the compatible ID
 * MS_BTHX_BTHMINI, so bth.inf puts BthMini and bthport on it. The child
 * forwards IOCTL_BTHX_* to this device:
 *   WRITE_HCI: one command or ACL packet; the ISR sends it from TxBuffer
 *     (16 bytes per transmitter-empty interrupt), the DPC completes it.
 *   READ_HCI: waits in the event or ACL queue; the ISR moves received bytes
 *     to a ring, the DPC cuts them into H4 packets and completes the oldest
 *     waiting read of that type (or keeps the packet until one comes).
 *
 * 1.0.0.4: requests are completed only after Lock is released (SWBT_DONE).
 * BthMini sends the next READ_HCI from its completion routine, which came
 * back into SwBtEvtIoDeviceControl on the same CPU and spun on Lock forever:
 * Windows hung at the boot logo.
 *
 * 1.0.0.5: Windows still hung at boot with R390. Safety and a record:
 *   - BootPending is set (and flushed to disk) when the device is added and
 *     cleared after 60 s of running. If it is still set at the next boot,
 *     that boot did not get there: the driver stays idle (SafeMode=1, no
 *     interrupt, no probe, no child) until Bluetooth.ps1 -Mode Retry or
 *     Install clears it.
 *   - Stage and the traffic counters are flushed to disk as they change, so
 *     the values of a boot that hung can be read after the next one.
 *
 * 1.0.0.6: the record of a boot that stuck at the logo: the driver ran for
 * over 60 s (stage 5) but no BTHX request ever reached it. So the boot waited
 * on the Bluetooth stack above the child, not on this driver. The child is
 * now added 20 s after the device starts (so Windows can finish booting
 * whatever the stack does), its queue is not power managed, and the child
 * side is recorded too: D0 entries, requests seen, failed forwards.
 *
 * 1.0.0.7: Windows booted; BthMini sent GET_VERSION, SET_VERSION and
 * QUERY_CAPABILITIES, then failed to start (problem 10). The capabilities
 * said ScoSupportNone; Microsoft's serialhcibus sample notes HCI bypass is
 * the only option, so they now match the sample (HCI bypass, one SCO channel,
 * idle capable). The three requests are recorded: the version BthMini asked
 * for and what each request was completed with.
 *
 * 1.0.0.8 (stage 3): the Bluetooth toggle works but scans found nothing.
 *   - UART clock: UEFI R391 feeds UART-D 48 MHz (R390: 408 MHz). The probe
 *     tries 115200 with the 48 MHz divisor first, then the 408 MHz one.
 *   - Firmware patch (Broadcom patchram, as Linux btbcm): when
 *     %SystemRoot%\System32\drivers\SwitchWOA\BCM4356A3.hcd exists (the
 *     user copies it in; it is Broadcom's file and is not distributed here):
 *     HCI_Reset, Download_Minidriver (FC2E), every record of the file as an
 *     HCI command, 250 ms, HCI_Reset again at the ROM speed.
 *   - 3 Mbaud with 48 MHz: Broadcom Update_UART_Baud_Rate (FC18), then the
 *     host follows (divisor 1) with hardware flow control; Read_Local_Version
 *     must answer, else without flow control, else the chip is given up for
 *     this boot (HighSpeedResult). Parameters\HighSpeed = 0 keeps 115200.
 *
 * 1.0.0.9: bit 24 of CLK_SOURCE_UARTD is not a finer divider: it makes the
 * CAR clock itself the 16x baud clock and the UART's DLL/DLM no longer
 * count (coreboot: "We override the 16.0 UART divider with the 15.1
 * CLK_SOURCE divider"). So UEFI R391 (48 MHz with bit 24) ran the UART at
 * 3 Mbaud and its 115200 test got no answer, and 1.0.0.8's first try with
 * the "48 MHz divisor" sent bytes at the wrong speed and confused the chip.
 * The driver now sets the clock itself, as Linux does: PLLP 408 MHz without
 * the override and divisor 221 for 115200 at start (whatever UEFI left), and
 * the override with N = 15 (48 MHz = 16 x 3 Mbaud) for 3 Mbaud. UEFI R390
 * is enough; R391 is not needed.
 *
 * 1.0.0.10: a keyboard works; audio broke up every ~0.7 s. For streaming:
 * the transmitter gets 32 bytes per empty-FIFO interrupt (the whole FIFO),
 * the receiver interrupts at 8 bytes (the RX timeout catches the rest)
 * instead of every byte, and once the boot is known good (60 s) the
 * counters are still written every 10 s but no longer forced to disk.
 * At 3 Mbaud audio mostly stopped (a keyboard was fine): lost receive bytes
 * would lose Number_Of_Completed_Packets events and starve the ACL credits.
 * Line errors are counted now (LSR overrun, parity, framing, break) and so
 * are the H4 bytes the parser had to skip to find the next packet.
 *
 * 1.0.0.11: with 32-byte bursts the chip stopped answering after the
 * 244-byte Write_Extended_Inquiry_Response (0C52) and BthMini gave up
 * (problem 43): THRE does not mean the whole 32-byte FIFO is free, so bytes
 * were lost. Back to 16 bytes per burst; the 8-byte RX trigger stays (no
 * overruns, events of 7 bytes came through on the RX timeout).
 *
 * 1.0.0.12: after a session at 3 Mbaud the chip did not answer at 115200 on
 * the next boot, even after UEFI's 20 ms power cycle: it was still at
 * 3 Mbaud with the patch loaded. The probe now also tries 3 Mbaud and keeps
 * using it when the chip answers there (FoundAt3M). When the device powers
 * down (shutdown, restart, driver update) the driver moves the chip and the
 * UART back to 115200, so the next start finds it at the ROM speed.
 *
 * 1.0.0.13: the safety net was too eager and too sticky.
 *   - BootPending is cleared 10 s after the child is added (about 30 s after
 *     start) instead of after 60 s: a restart soon after logging on no longer
 *     counts as a hung boot.
 *   - Safe mode no longer skips setting up the device. It only holds back the
 *     probe and the child, and it is checked again every 2 s and at every D0
 *     entry: after -Mode Retry the driver starts right away, also when
 *     Windows "shut down" with fast startup (a hibernate: the driver is not
 *     loaded again, so the old decision used to stay).
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <initguid.h>
#include <bthxddi.h>

NTSYSAPI ULONG NTAPI RtlRandomEx(PULONG Seed);     /* ntoskrnl export, declared in ntifs.h */

#define UART_LENGTH       0x40

/* Tegra HSUART, in ULONG units. */
#define UART_THR          (0x00 / 4)
#define UART_IER          (0x04 / 4)
#define UART_IIR          (0x08 / 4)
#define UART_FCR          (0x08 / 4)
#define UART_LCR          (0x0C / 4)
#define UART_MCR          (0x10 / 4)
#define UART_LSR          (0x14 / 4)
#define UART_DLL          (0x00 / 4)
#define UART_DLM          (0x04 / 4)
#define UART_LSR_DR       0x01u
#define UART_LSR_THRE     0x20u
#define UART_LSR_OE       0x02u         /* overrun: a byte was lost in the FIFO */
#define UART_LSR_ERRORS   0x1Eu         /* overrun, parity, framing, break */
#define UART_IIR_NO_INT   0x01u
#define UART_IER_RX       0x15u         /* received data, line status, RX timeout */
#define UART_IER_TX       0x02u         /* transmitter empty */
#define UART_DIV_115200   221u          /* 408 MHz / 16 / 221 = 115384 baud (UEFI R390) */
#define UART_DIV_3M       1u            /* not counted while the CAR override is on */
#define CAR_UARTD_SOURCE  0x600061C0u   /* CLK_SOURCE_UARTD */
#define CAR_UART_PLLP     0x00000000u   /* PLLP 408 MHz, the UART divisor counts */
#define CAR_UART_3M       ((1u << 24) | 15u)   /* override: 2 x 408 / (15 + 2) = 48 MHz = 16 x 3 Mbaud */
#define UART_MCR_RTS      0x02u
#define UART_MCR_FLOW     0x62u         /* RTS on, CTS_EN and RTS_EN: hardware flow control */
#define BCM_DOWNLOAD_MINI 0xFC2E
#define BCM_UPDATE_BAUD   0xFC18
#define BCM_HIGH_BAUD     3000000u
#define FIRMWARE_PATH     L"\\SystemRoot\\System32\\drivers\\SwitchWOA\\BCM4356A3.hcd"
#define FIRMWARE_MAX      (256u * 1024u)
#define UART_TX_BURST     16u           /* bytes per transmitter-empty interrupt (32 lost bytes, 1.0.0.11) */

/* HCI (H4). */
#define HCI_COMMAND_PKT   0x01
#define HCI_ACL_PKT       0x02
#define HCI_SCO_PKT       0x03
#define HCI_EVENT_PKT     0x04
#define HCI_EV_COMPLETE   0x0E
#define HCI_RESET         0x0C03
#define HCI_READ_NAME     0x0C14
#define HCI_READ_VERSION  0x1001
#define HCI_READ_BD_ADDR  0x1009
#define BCM_WRITE_BD_ADDR 0xFC01
#define HCI_ACL_MAX_DATA  1021
#define HCI_MAX_PACKET    (4 + HCI_ACL_MAX_DATA)
#define NINTENDO_OUI      0x98B6E9u     /* the address is 98:B6:E9 + 3 bytes made once per Switch */

#define RX_RING_SIZE      8192u         /* power of two */
#define MAX_KEPT_PACKETS  64u
#define POOL_TAG          'tBwS'

/* Container ID for a device built into the PC (GUID_CONTAINERID_INTERNALLY_CONNECTED_DEVICE). */
DEFINE_GUID(SWBT_CONTAINER_INTERNAL, 0x00000000, 0x0000, 0x0000, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);

typedef struct _SWBT_PACKET {
    LIST_ENTRY Link;
    UCHAR Type;
    ULONG Length;
    UCHAR Data[1];
} SWBT_PACKET, *PSWBT_PACKET;

/* Requests finished under Lock, completed after it is released. */
#define SWBT_DONE_MAX 32
typedef struct _SWBT_DONE {
    ULONG Count;
    WDFREQUEST Request[SWBT_DONE_MAX];
    NTSTATUS Status[SWBT_DONE_MAX];
    ULONG_PTR Information[SWBT_DONE_MAX];
} SWBT_DONE, *PSWBT_DONE;

static BOOLEAN SwBtDoneFull(PSWBT_DONE Done)
{
    return Done->Count >= SWBT_DONE_MAX;
}

static VOID SwBtDoneAdd(PSWBT_DONE Done, WDFREQUEST Request, NTSTATUS Status, ULONG_PTR Information)
{
    if (Done->Count < SWBT_DONE_MAX) {
        Done->Request[Done->Count] = Request;
        Done->Status[Done->Count] = Status;
        Done->Information[Done->Count] = Information;
        Done->Count++;
    } else {
        WdfRequestCompleteWithInformation(Request, Status, Information);   /* not reached: callers check */
    }
}

/* Lock must not be held. */
static VOID SwBtDoneFlush(PSWBT_DONE Done)
{
    ULONG Index;
    for (Index = 0; Index < Done->Count; ++Index) {
        WdfRequestCompleteWithInformation(Done->Request[Index], Done->Status[Index], Done->Information[Index]);
    }
    Done->Count = 0;
}

typedef struct _SWBT_DEVICE {
    volatile ULONG *Uart;
    volatile ULONG *CarUartd;       /* CLK_SOURCE_UARTD, so the driver sets the UART clock */
    WDFINTERRUPT Interrupt;
    BOOLEAN ChipOk;
    BOOLEAN SafeMode;               /* the last boot did not finish: stay idle */
    BOOLEAN HighSpeedNow;           /* the chip and the UART run at 3 Mbaud */
    ULONG ChildTick;                /* timer tick at which the child is added */
    UCHAR BdAddr[6];                /* little endian, as HCI */

    /* Shared with the ISR (interrupt lock). */
    UCHAR RxRing[RX_RING_SIZE];
    ULONG RxHead;                   /* written by the ISR */
    ULONG RxTail;                   /* written by the DPC */
    ULONG RxOverflows;
    ULONG LineOverruns;             /* LSR OE seen by the ISR */
    ULONG LineErrors;               /* LSR PE/FE/BI seen by the ISR */
    ULONG ParserSkipped;            /* bytes skipped while looking for an H4 packet type */
    UCHAR TxBuffer[1 + HCI_MAX_PACKET + 8];
    ULONG TxLength;
    ULONG TxSent;
    BOOLEAN TxBusy;
    BOOLEAN TxDone;
    ULONG Ier;

    /* DPC and I/O (Lock). */
    WDFSPINLOCK Lock;
    WDFQUEUE TxQueue;
    WDFQUEUE EventQueue;
    WDFQUEUE AclQueue;
    WDFREQUEST TxRequest;
    ULONG TxRequestLength;
    LIST_ENTRY KeptEvents;
    LIST_ENTRY KeptAcl;
    ULONG KeptCount;
    ULONG Dropped;

    /* H4 parser (DPC only). */
    UCHAR RxPacket[1 + HCI_MAX_PACKET + 8];
    ULONG RxGot;
    ULONG RxNeed;
    BTHX_CAPABILITIES Caps;

    /* Diagnostics, written to the registry every few seconds by Timer. */
    WDFTIMER Timer;
    ULONG Ticks;
    volatile LONG IsrCalls;
    ULONG IirSeen;                  /* bit (IIR >> 1) & 7 for each interrupt source seen */
    ULONG RxBytes;
    ULONG TxBytes;
    ULONG CmdsOut;
    ULONG AclOut;
    ULONG EventsIn;
    ULONG AclIn;
    volatile LONG ReadEventRequests;
    volatile LONG ReadAclRequests;
    volatile LONG WriteRequests;
    volatile LONG OtherIoctls;
    ULONG LastOtherIoctl;
    ULONG CmdLog[8];                /* last commands: opcode */
    ULONG EventLog[8];              /* last events: code << 16 | opcode (complete / status) */
    ULONG CmdLogNext;
    ULONG EventLogNext;

    /* 1.0.0.6: the child, added late, and what reaches it. */
    BOOLEAN ChildAdded;
    volatile LONG ChildD0;
    volatile LONG ChildRequests;
    volatile LONG ChildForwardFailures;
    ULONG LastChildIoctl;
    ULONG LastForwardStatus;

    /* 1.0.0.7: the setup requests from BthMini and their results. */
    volatile LONG GetVersionRequests;
    volatile LONG SetVersionRequests;
    volatile LONG CapsRequests;
    ULONG SetVersionValue;
    ULONG LastSetupStatus;          /* IOCTL function << 24 | low bits of the status */
} SWBT_DEVICE, *PSWBT_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWBT_DEVICE, SwBtGetDevice)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwBtEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwBtEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwBtEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY SwBtEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT SwBtEvtD0Exit;
EVT_WDF_INTERRUPT_ISR SwBtEvtIsr;
EVT_WDF_INTERRUPT_DPC SwBtEvtDpc;
EVT_WDF_INTERRUPT_ENABLE SwBtEvtInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE SwBtEvtInterruptDisable;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SwBtEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SwBtEvtChildIoDeviceControl;
EVT_WDF_TIMER SwBtEvtTimer;
EVT_WDF_DEVICE_D0_ENTRY SwBtEvtChildD0Entry;
static NTSTATUS SwBtCreateChild(WDFDEVICE Device);
static BOOLEAN SwBtSafeModeCleared(PSWBT_DEVICE Dev);
static VOID SwBtStart(PSWBT_DEVICE Dev);
#define CHILD_DELAY_TICKS 10            /* timer ticks of 2 s: the child comes 20 s after start */

static WCHAR SwBtParamsPath[256];

/* Writes the Parameters key to disk now, so it survives a hang. PASSIVE only. */
static VOID SwBtFlush(VOID)
{
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key;
    if (SwBtParamsPath[0] == L'\0' || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    RtlInitUnicodeString(&Path, SwBtParamsPath);
    InitializeObjectAttributes(&Attributes, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&Key, KEY_WRITE, &Attributes))) {
        (VOID)ZwFlushKey(Key);
        ZwClose(Key);
    }
}

static VOID SwBtTrace(PCWSTR Name, ULONG Value)
{
    if (SwBtParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwBtParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

static VOID SwBtTraceString(PCWSTR Name, PCWSTR Value)
{
    if (SwBtParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwBtParamsPath, Name, REG_SZ, (PVOID)Value,
                                    (ULONG)((wcslen(Value) + 1) * sizeof(WCHAR)));
    }
}

/* How far this boot got, on disk at once. */
static VOID SwBtStage(ULONG Stage)
{
    SwBtTrace(L"Stage", Stage);
    SwBtFlush();
}

static BOOLEAN SwBtReadParam(PCWSTR Name, PULONG Value)
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Result = 0;
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Result;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    if (SwBtParamsPath[0] == L'\0' ||
        !NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, SwBtParamsPath, Table, NULL, NULL))) {
        return FALSE;
    }
    *Value = Result;
    return TRUE;
}

/* ------------------------------------------------------------------ UART */

static ULONG SwBtRead(PSWBT_DEVICE Dev, ULONG Reg)
{
    return READ_REGISTER_ULONG((PULONG)&Dev->Uart[Reg]);
}

static VOID SwBtWrite(PSWBT_DEVICE Dev, ULONG Reg, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)&Dev->Uart[Reg], Value);
}

static VOID SwBtUartInit(PSWBT_DEVICE Dev, ULONG Divisor, ULONG Mcr)
{
    SwBtWrite(Dev, UART_IER, 0);
    SwBtWrite(Dev, UART_LCR, 0x80);                 /* divisor latch */
    SwBtWrite(Dev, UART_DLL, Divisor & 0xFF);
    SwBtWrite(Dev, UART_DLM, (Divisor >> 8) & 0xFF);
    SwBtWrite(Dev, UART_LCR, 0x03);                 /* 8N1 */
    SwBtWrite(Dev, UART_FCR, 0x87);                 /* FIFOs on and cleared, RX trigger 8 bytes (+ RX timeout) */
    SwBtWrite(Dev, UART_MCR, Mcr);
}

static VOID SwBtSleepMs(ULONG Ms)
{
    LARGE_INTEGER Delay;
    Delay.QuadPart = -(LONGLONG)Ms * 10000;
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &Delay);
}

static ULONGLONG SwBtNowMs(VOID)
{
    return KeQueryInterruptTime() / 10000;
}

/* ------------------------------------------------- polled HCI (D0 entry) */

static BOOLEAN SwBtPollSend(PSWBT_DEVICE Dev, const UCHAR *Buffer, ULONG Length)
{
    ULONG Index;
    for (Index = 0; Index < Length; ++Index) {
        ULONGLONG End = SwBtNowMs() + 50;
        while ((SwBtRead(Dev, UART_LSR) & UART_LSR_THRE) == 0) {
            if (SwBtNowMs() > End) {
                return FALSE;
            }
            KeStallExecutionProcessor(10);
        }
        SwBtWrite(Dev, UART_THR, Buffer[Index]);
    }
    return TRUE;
}

/*
 * Reads one HCI event into Buffer (04 code len params) within TimeoutMs.
 * The loop polls faster than 115200 baud fills the FIFO, so nothing is lost.
 * The deadline holds even when the line never stops sending.
 */
static ULONG SwBtPollEvent(PSWBT_DEVICE Dev, UCHAR *Buffer, ULONG Max, ULONG TimeoutMs)
{
    ULONGLONG End = SwBtNowMs() + TimeoutMs;
    ULONG Got = 0, Need = 3;
    while (Got < Need) {
        UCHAR Byte;
        if (SwBtNowMs() > End) {
            return 0;
        }
        if ((SwBtRead(Dev, UART_LSR) & UART_LSR_DR) == 0) {
            KeStallExecutionProcessor(5);
            continue;
        }
        Byte = (UCHAR)SwBtRead(Dev, UART_THR);
        if (Got == 0 && Byte != HCI_EVENT_PKT) {
            continue;                   /* skip anything before an event */
        }
        if (Got < Max) {
            Buffer[Got] = Byte;
        }
        if (++Got == 3) {
            Need = 3 + Byte;
        }
    }
    return Got;
}

/* Sends a command; returns the length of its Command Complete event. */
static ULONG SwBtPollCommand(PSWBT_DEVICE Dev, USHORT Opcode, const UCHAR *Params, UCHAR ParamLength,
                             UCHAR *Event, ULONG Max)
{
    UCHAR Cmd[4 + 255];
    ULONG Tries, Length;
    Cmd[0] = HCI_COMMAND_PKT;
    Cmd[1] = (UCHAR)Opcode;
    Cmd[2] = (UCHAR)(Opcode >> 8);
    Cmd[3] = ParamLength;
    if (ParamLength != 0) {
        RtlCopyMemory(&Cmd[4], Params, ParamLength);
    }
    if (!SwBtPollSend(Dev, Cmd, 4u + ParamLength)) {
        return 0;
    }
    for (Tries = 0; Tries < 4; ++Tries) {
        Length = SwBtPollEvent(Dev, Event, Max, 1000);
        if (Length == 0) {
            return 0;
        }
        /* 04 0E len ncmd opcode(2) status ... */
        if (Length >= 7 && Event[1] == HCI_EV_COMPLETE && Event[4] == (UCHAR)Opcode &&
            Event[5] == (UCHAR)(Opcode >> 8)) {
            return Length;
        }
    }
    return 0;
}

/* One address per Switch: 98:B6:E9 and 3 random bytes, made once and kept. */
static VOID SwBtChooseAddress(PSWBT_DEVICE Dev)
{
    ULONG Suffix;
    if (!SwBtReadParam(L"AddressSuffix", &Suffix)) {
        LARGE_INTEGER Pc = KeQueryPerformanceCounter(NULL);
        ULONG Seed = Pc.LowPart ^ (ULONG)KeQueryInterruptTime() ^ (ULONG)(ULONG_PTR)Dev;
        Suffix = RtlRandomEx(&Seed) & 0xFFFFFF;
        if (Suffix == 0 || Suffix == 0xFFFFFF) {
            Suffix = 0x5A5A5A;
        }
        SwBtTrace(L"AddressSuffix", Suffix);
    }
    Suffix &= 0xFFFFFF;
    Dev->BdAddr[0] = (UCHAR)Suffix;
    Dev->BdAddr[1] = (UCHAR)(Suffix >> 8);
    Dev->BdAddr[2] = (UCHAR)(Suffix >> 16);
    Dev->BdAddr[3] = (UCHAR)NINTENDO_OUI;
    Dev->BdAddr[4] = (UCHAR)(NINTENDO_OUI >> 8);
    Dev->BdAddr[5] = (UCHAR)(NINTENDO_OUI >> 16);
}

static VOID SwBtFlushRx(PSWBT_DEVICE Dev)
{
    ULONG Index;
    for (Index = 0; Index < 4096 && (SwBtRead(Dev, UART_LSR) & UART_LSR_DR) != 0; ++Index) {
        (VOID)SwBtRead(Dev, UART_THR);
    }
}

/* HCI_Reset at the current speed; TRUE when it is answered with status 0. */
static BOOLEAN SwBtReset(PSWBT_DEVICE Dev, UCHAR *Event, ULONG Max)
{
    ULONG Length;
    SwBtFlushRx(Dev);
    Length = SwBtPollCommand(Dev, HCI_RESET, NULL, 0, Event, Max);
    return Length >= 7 && Event[6] == 0;
}

static VOID SwBtTraceVersion(PSWBT_DEVICE Dev, UCHAR *Event, ULONG Max)
{
    ULONG Length = SwBtPollCommand(Dev, HCI_READ_VERSION, NULL, 0, Event, Max);
    if (Length >= 15 && Event[6] == 0) {
        SwBtTrace(L"HciVersion", Event[7]);
        SwBtTrace(L"HciRevision", Event[8] | (Event[9] << 8));
        SwBtTrace(L"LmpVersion", Event[10]);
        SwBtTrace(L"Manufacturer", Event[11] | (Event[12] << 8));
        SwBtTrace(L"LmpSubversion", Event[13] | (Event[14] << 8));
    }
}

/* Reads the .hcd the user put in place; NULL when there is none. PASSIVE. */
static PUCHAR SwBtLoadFirmware(PULONG Length)
{
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Io;
    FILE_STANDARD_INFORMATION Info;
    HANDLE File;
    PUCHAR Data = NULL;
    NTSTATUS Status;

    *Length = 0;
    RtlInitUnicodeString(&Path, FIRMWARE_PATH);
    InitializeObjectAttributes(&Attributes, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Attributes, &Io, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    SwBtTrace(L"FirmwareOpen", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return NULL;
    }
    Status = ZwQueryInformationFile(File, &Io, &Info, sizeof(Info), FileStandardInformation);
    if (NT_SUCCESS(Status) && Info.EndOfFile.QuadPart > 0 && Info.EndOfFile.QuadPart <= FIRMWARE_MAX) {
        Data = (PUCHAR)ExAllocatePool2(POOL_FLAG_PAGED, (SIZE_T)Info.EndOfFile.QuadPart, POOL_TAG);
        if (Data != NULL) {
            Status = ZwReadFile(File, NULL, NULL, NULL, &Io, Data, (ULONG)Info.EndOfFile.QuadPart, NULL, NULL);
            if (NT_SUCCESS(Status) && Io.Information == (ULONG_PTR)Info.EndOfFile.QuadPart) {
                *Length = (ULONG)Io.Information;
            } else {
                ExFreePoolWithTag(Data, POOL_TAG);
                Data = NULL;
            }
        }
    }
    ZwClose(File);
    SwBtTrace(L"FirmwareBytes", *Length);
    return Data;
}

/* Broadcom patchram (Linux btbcm_patchram). Returns the records sent, 0 on failure. */
static ULONG SwBtPatchram(PSWBT_DEVICE Dev, const UCHAR *Fw, ULONG FwLength, UCHAR *Event, ULONG Max)
{
    ULONG Offset = 0, Records = 0;
    if (SwBtPollCommand(Dev, BCM_DOWNLOAD_MINI, NULL, 0, Event, Max) < 7 || Event[6] != 0) {
        SwBtTrace(L"FirmwareResult", 0xF0000001u);
        return 0;
    }
    SwBtSleepMs(50);
    while (Offset + 3 <= FwLength) {
        USHORT Opcode = (USHORT)(Fw[Offset] | (Fw[Offset + 1] << 8));
        UCHAR ParamLength = Fw[Offset + 2];
        if (Offset + 3 + ParamLength > FwLength) {
            SwBtTrace(L"FirmwareResult", 0xF0000002u);       /* truncated file */
            return 0;
        }
        if (SwBtPollCommand(Dev, Opcode, &Fw[Offset + 3], ParamLength, Event, Max) < 7) {
            SwBtTrace(L"FirmwareResult", 0xF0000003u);
            SwBtTrace(L"FirmwareFailedRecord", Records);
            return 0;
        }
        Offset += 3u + ParamLength;
        ++Records;
    }
    SwBtSleepMs(250);                                       /* the chip boots the patched code */
    return Records;
}

/* Moves the chip and the host to 3 Mbaud. FALSE: the chip no longer answers. */
static BOOLEAN SwBtHighSpeed(PSWBT_DEVICE Dev, UCHAR *Event, ULONG Max)
{
    UCHAR Params[6] = { 0, 0, (UCHAR)BCM_HIGH_BAUD, (UCHAR)(BCM_HIGH_BAUD >> 8),
                        (UCHAR)(BCM_HIGH_BAUD >> 16), (UCHAR)(BCM_HIGH_BAUD >> 24) };
    ULONG Length = SwBtPollCommand(Dev, BCM_UPDATE_BAUD, Params, sizeof(Params), Event, Max);
    if (Length < 7 || Event[6] != 0) {
        SwBtTrace(L"HighSpeedResult", 1);                   /* refused: still 115200 */
        return TRUE;
    }
    WRITE_REGISTER_ULONG((PULONG)Dev->CarUartd, CAR_UART_3M);
    KeStallExecutionProcessor(10);
    SwBtUartInit(Dev, UART_DIV_3M, UART_MCR_FLOW);
    SwBtSleepMs(10);
    SwBtFlushRx(Dev);
    Length = SwBtPollCommand(Dev, HCI_READ_VERSION, NULL, 0, Event, Max);
    if (Length >= 15 && Event[6] == 0) {
        SwBtTrace(L"HighSpeedResult", 2);                   /* 3 Mbaud, flow control */
        Dev->HighSpeedNow = TRUE;
        return TRUE;
    }
    SwBtUartInit(Dev, UART_DIV_3M, UART_MCR_RTS);
    SwBtSleepMs(10);
    SwBtFlushRx(Dev);
    Length = SwBtPollCommand(Dev, HCI_READ_VERSION, NULL, 0, Event, Max);
    if (Length >= 15 && Event[6] == 0) {
        SwBtTrace(L"HighSpeedResult", 3);                   /* 3 Mbaud, no flow control */
        Dev->HighSpeedNow = TRUE;
        return TRUE;
    }
    SwBtTrace(L"HighSpeedResult", 4);                       /* lost: the next boot powers the chip again */
    return FALSE;
}

static BOOLEAN SwBtProbe(PSWBT_DEVICE Dev)
{
    UCHAR Event[260];
    WCHAR Text[256];
    ULONG Length, Index, CanSwitchClock, FwLength = 0, HighSpeed = 1;
    PUCHAR Fw;

    /* 115200 from PLLP 408 MHz, whatever clock UEFI left (R391 left the 3 Mbaud override). */
    CanSwitchClock = Dev->CarUartd != NULL;
    if (Dev->CarUartd != NULL) {
        SwBtTrace(L"CarUartdAtStart", READ_REGISTER_ULONG((PULONG)Dev->CarUartd));
        WRITE_REGISTER_ULONG((PULONG)Dev->CarUartd, CAR_UART_PLLP);
        KeStallExecutionProcessor(10);
    }
    SwBtUartInit(Dev, UART_DIV_115200, UART_MCR_RTS);
    KeStallExecutionProcessor(100);
    Dev->HighSpeedNow = FALSE;
    SwBtTrace(L"FoundAt3M", 0);
    if (!SwBtReset(Dev, Event, sizeof(Event)) && !SwBtReset(Dev, Event, sizeof(Event))) {
        /* Still at 3 Mbaud from the last session (the power cycle did not reset it)? */
        if (Dev->CarUartd != NULL) {
            WRITE_REGISTER_ULONG((PULONG)Dev->CarUartd, CAR_UART_3M);
            KeStallExecutionProcessor(10);
            SwBtUartInit(Dev, UART_DIV_3M, UART_MCR_FLOW);
            SwBtSleepMs(10);
            if (SwBtReset(Dev, Event, sizeof(Event)) || SwBtReset(Dev, Event, sizeof(Event))) {
                Dev->HighSpeedNow = TRUE;
                SwBtTrace(L"FoundAt3M", 1);
            } else {
                WRITE_REGISTER_ULONG((PULONG)Dev->CarUartd, CAR_UART_PLLP);
                SwBtUartInit(Dev, UART_DIV_115200, UART_MCR_RTS);
            }
        }
        if (!Dev->HighSpeedNow) {
            SwBtTrace(L"HciReset", 0);
            SwBtTraceString(L"Result", L"no answer to HCI_Reset (115200 or 3 Mbaud)");
            return FALSE;
        }
    }
    SwBtTrace(L"HciReset", 0x100);
    SwBtTrace(L"UartClockMHz", Dev->HighSpeedNow ? 48 : 408);
    SwBtTraceVersion(Dev, Event, sizeof(Event));

    /* Firmware patch, if the user put the file in place (not when the chip already runs it at 3 Mbaud). */
    Fw = Dev->HighSpeedNow ? NULL : SwBtLoadFirmware(&FwLength);
    if (Dev->HighSpeedNow) {
        SwBtTrace(L"FirmwareResult", 2);                    /* kept from the last session */
    } else if (Fw != NULL) {
        ULONG Records = SwBtPatchram(Dev, Fw, FwLength, Event, sizeof(Event));
        ExFreePoolWithTag(Fw, POOL_TAG);
        SwBtTrace(L"FirmwareRecords", Records);
        if (Records != 0) {
            if (SwBtReset(Dev, Event, sizeof(Event))) {
                SwBtTrace(L"FirmwareResult", 1);            /* patched and answering */
                SwBtTraceVersion(Dev, Event, sizeof(Event));
            } else {
                SwBtTrace(L"FirmwareResult", 0xF0000004u);
                SwBtTraceString(L"Result", L"no answer after the firmware patch");
                return FALSE;
            }
        }
    } else {
        SwBtTrace(L"FirmwareResult", 0);                    /* no file: ROM code */
    }

    SwBtChooseAddress(Dev);
    Length = SwBtPollCommand(Dev, BCM_WRITE_BD_ADDR, Dev->BdAddr, 6, Event, sizeof(Event));
    SwBtTrace(L"WriteBdAddr", Length >= 7 ? Event[6] + 0x100u : 0);

    Length = SwBtPollCommand(Dev, HCI_READ_BD_ADDR, NULL, 0, Event, sizeof(Event));
    if (Length >= 13 && Event[6] == 0) {
        (VOID)RtlStringCchPrintfW(Text, RTL_NUMBER_OF(Text), L"%02X:%02X:%02X:%02X:%02X:%02X",
                                  Event[12], Event[11], Event[10], Event[9], Event[8], Event[7]);
        SwBtTraceString(L"BdAddr", Text);
    }

    Length = SwBtPollCommand(Dev, HCI_READ_NAME, NULL, 0, Event, sizeof(Event));
    if (Length >= 8 && Event[6] == 0) {
        for (Index = 0; Index + 7 < Length && Index < RTL_NUMBER_OF(Text) - 1 && Event[7 + Index] != 0; ++Index) {
            UCHAR C = Event[7 + Index];
            Text[Index] = (C >= 0x20 && C < 0x7F) ? (WCHAR)C : L'?';
        }
        Text[Index] = L'\0';
        SwBtTraceString(L"LocalName", Text);
    }

    (VOID)SwBtReadParam(L"HighSpeed", &HighSpeed);
    if (Dev->HighSpeedNow) {
        SwBtTrace(L"HighSpeedResult", 5);                   /* already at 3 Mbaud */
    } else if (CanSwitchClock && HighSpeed != 0) {
        if (!SwBtHighSpeed(Dev, Event, sizeof(Event))) {
            SwBtTraceString(L"Result", L"lost the chip at 3 Mbaud");
            return FALSE;
        }
    } else {
        SwBtTrace(L"HighSpeedResult", 0);                   /* 115200 (R390 clock, or turned off) */
    }
    SwBtTraceString(L"Result", L"ok");
    return TRUE;
}

/* ---------------------------------------------------------- interrupt */

/*
 * Puts up to one burst into the empty transmit FIFO. Returns TRUE when the
 * packet is all out (the DPC then completes the request). Interrupt lock held.
 */
static BOOLEAN SwBtTxBurst(PSWBT_DEVICE Dev)
{
    ULONG Count;
    for (Count = 0; Count < UART_TX_BURST && Dev->TxSent < Dev->TxLength; ++Count) {
        SwBtWrite(Dev, UART_THR, Dev->TxBuffer[Dev->TxSent++]);
        Dev->TxBytes++;
    }
    if (Dev->TxSent < Dev->TxLength) {
        return FALSE;
    }
    Dev->TxBusy = FALSE;
    Dev->TxDone = TRUE;
    Dev->Ier &= ~UART_IER_TX;
    SwBtWrite(Dev, UART_IER, Dev->Ier);
    return TRUE;
}

BOOLEAN SwBtEvtIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(WdfInterruptGetDevice(Interrupt));
    BOOLEAN Mine = FALSE, Wake = FALSE;
    ULONG Loops;
    UNREFERENCED_PARAMETER(MessageId);

    for (Loops = 0; Loops < 64; ++Loops) {
        ULONG Iir = SwBtRead(Dev, UART_IIR), Lsr, Count;
        if ((Iir & UART_IIR_NO_INT) != 0) {
            break;
        }
        Mine = TRUE;
        InterlockedIncrement(&Dev->IsrCalls);
        Dev->IirSeen |= 1u << ((Iir >> 1) & 7);
        Lsr = SwBtRead(Dev, UART_LSR);
        if ((Lsr & UART_LSR_ERRORS) != 0) {
            if ((Lsr & UART_LSR_OE) != 0) Dev->LineOverruns++;
            if ((Lsr & (UART_LSR_ERRORS & ~UART_LSR_OE)) != 0) Dev->LineErrors++;
        }
        for (Count = 0; Count < 64 && (Lsr & UART_LSR_DR) != 0; ++Count) {
            UCHAR Byte = (UCHAR)SwBtRead(Dev, UART_THR);
            Dev->RxBytes++;
            if (Dev->RxHead - Dev->RxTail < RX_RING_SIZE) {
                Dev->RxRing[Dev->RxHead & (RX_RING_SIZE - 1)] = Byte;
                Dev->RxHead++;
            } else {
                Dev->RxOverflows++;
            }
            Wake = TRUE;
            Lsr = SwBtRead(Dev, UART_LSR);
            if ((Lsr & UART_LSR_ERRORS) != 0) {
                if ((Lsr & UART_LSR_OE) != 0) Dev->LineOverruns++;
                if ((Lsr & (UART_LSR_ERRORS & ~UART_LSR_OE)) != 0) Dev->LineErrors++;
            }
        }
        if (Dev->TxBusy && (Lsr & UART_LSR_THRE) != 0) {
            Wake |= SwBtTxBurst(Dev);
        } else if (!Dev->TxBusy && (Dev->Ier & UART_IER_TX) != 0) {
            Dev->Ier &= ~UART_IER_TX;
            SwBtWrite(Dev, UART_IER, Dev->Ier);
        }
    }
    if (Wake) {
        WdfInterruptQueueDpcForIsr(Interrupt);
    }
    return Mine;
}

NTSTATUS SwBtEvtInterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(AssociatedDevice);
    UNREFERENCED_PARAMETER(Interrupt);
    Dev->RxHead = Dev->RxTail = 0;
    Dev->TxBusy = Dev->TxDone = FALSE;
    Dev->Ier = Dev->SafeMode ? 0 : UART_IER_RX;
    SwBtWrite(Dev, UART_IER, Dev->Ier);
    return STATUS_SUCCESS;
}

NTSTATUS SwBtEvtInterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(AssociatedDevice);
    UNREFERENCED_PARAMETER(Interrupt);
    Dev->Ier = 0;
    SwBtWrite(Dev, UART_IER, 0);
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------- packets (DPC) */

/* Fills a READ_HCI with one packet; it is completed after Lock. Lock held. */
static VOID SwBtCompleteRead(PSWBT_DONE Done, WDFREQUEST Request, UCHAR Type, const UCHAR *Data, ULONG Length)
{
    WDFMEMORY Memory;
    size_t Size = 0;
    PBTHX_HCI_READ_WRITE_CONTEXT Ctx;
    ULONG Bytes = FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + Length;
    NTSTATUS Status = WdfRequestRetrieveOutputMemory(Request, &Memory);
    if (!NT_SUCCESS(Status)) {
        SwBtDoneAdd(Done, Request, Status, 0);
        return;
    }
    Ctx = (PBTHX_HCI_READ_WRITE_CONTEXT)WdfMemoryGetBuffer(Memory, &Size);
    if (Size < Bytes) {
        SwBtDoneAdd(Done, Request, STATUS_BUFFER_TOO_SMALL, 0);
        return;
    }
    Ctx->Type = Type;
    Ctx->DataLen = Length;
    RtlCopyMemory(Ctx->Data, Data, Length);
    SwBtDoneAdd(Done, Request, STATUS_SUCCESS, Bytes);
}

/* A whole H4 packet from the chip (without the type byte). Lock held. */
static VOID SwBtDeliver(PSWBT_DEVICE Dev, PSWBT_DONE Done, UCHAR Type, const UCHAR *Data, ULONG Length)
{
    WDFQUEUE Queue = Type == HCI_EVENT_PKT ? Dev->EventQueue : Dev->AclQueue;
    PLIST_ENTRY Kept = Type == HCI_EVENT_PKT ? &Dev->KeptEvents : &Dev->KeptAcl;
    WDFREQUEST Request;
    PSWBT_PACKET Packet;

    if (IsListEmpty(Kept) && !SwBtDoneFull(Done) && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Queue, &Request))) {
        SwBtCompleteRead(Done, Request, Type, Data, Length);
        return;
    }
    if (Dev->KeptCount >= MAX_KEPT_PACKETS) {
        Dev->Dropped++;
        return;
    }
    Packet = (PSWBT_PACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, FIELD_OFFSET(SWBT_PACKET, Data) + Length, POOL_TAG);
    if (Packet == NULL) {
        Dev->Dropped++;
        return;
    }
    Packet->Type = Type;
    Packet->Length = Length;
    RtlCopyMemory(Packet->Data, Data, Length);
    InsertTailList(Kept, &Packet->Link);
    Dev->KeptCount++;
}

/* Hands kept packets to reads that came in since. Lock held. */
static VOID SwBtDrainKept(PSWBT_DEVICE Dev, PSWBT_DONE Done, PLIST_ENTRY Kept, WDFQUEUE Queue)
{
    WDFREQUEST Request;
    while (!IsListEmpty(Kept) && !SwBtDoneFull(Done) && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Queue, &Request))) {
        PSWBT_PACKET Packet = CONTAINING_RECORD(RemoveHeadList(Kept), SWBT_PACKET, Link);
        Dev->KeptCount--;
        SwBtCompleteRead(Done, Request, Packet->Type, Packet->Data, Packet->Length);
        ExFreePoolWithTag(Packet, POOL_TAG);
    }
}

static VOID SwBtFreeKept(PSWBT_DEVICE Dev)
{
    while (!IsListEmpty(&Dev->KeptEvents)) {
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(&Dev->KeptEvents), SWBT_PACKET, Link), POOL_TAG);
    }
    while (!IsListEmpty(&Dev->KeptAcl)) {
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(&Dev->KeptAcl), SWBT_PACKET, Link), POOL_TAG);
    }
    Dev->KeptCount = 0;
}

/* H4 parser: one received byte. Lock held. */
static VOID SwBtParse(PSWBT_DEVICE Dev, PSWBT_DONE Done, UCHAR Byte)
{
    UCHAR Type;
    if (Dev->RxGot == 0) {
        switch (Byte) {
        case HCI_EVENT_PKT: Dev->RxNeed = 1 + 2; break;      /* code, length */
        case HCI_ACL_PKT:   Dev->RxNeed = 1 + 4; break;      /* handle(2), length(2) */
        case HCI_SCO_PKT:   Dev->RxNeed = 1 + 3; break;      /* handle(2), length */
        default:            Dev->ParserSkipped++; return;    /* out of step: skip */
        }
    }
    Dev->RxPacket[Dev->RxGot++] = Byte;
    if (Dev->RxGot < Dev->RxNeed) {
        return;
    }
    /* At the end of the header, add the payload length. */
    Type = Dev->RxPacket[0];
    if (Type == HCI_EVENT_PKT && Dev->RxGot == 3) {
        Dev->RxNeed += Dev->RxPacket[2];
    } else if (Type == HCI_ACL_PKT && Dev->RxGot == 5) {
        ULONG Data = Dev->RxPacket[3] | (Dev->RxPacket[4] << 8);
        if (Data > HCI_ACL_MAX_DATA) {
            Dev->RxGot = 0;                                  /* corrupt: resynchronise */
            Dev->Dropped++;
            return;
        }
        Dev->RxNeed += Data;
    } else if (Type == HCI_SCO_PKT && Dev->RxGot == 4) {
        Dev->RxNeed += Dev->RxPacket[3];
    }
    if (Dev->RxGot < Dev->RxNeed) {
        return;
    }
    if (Type == HCI_EVENT_PKT) {
        ULONG Opcode = 0;
        if (Dev->RxPacket[1] == HCI_EV_COMPLETE && Dev->RxGot >= 6) {
            Opcode = Dev->RxPacket[4] | (Dev->RxPacket[5] << 8);
        } else if (Dev->RxPacket[1] == 0x0F && Dev->RxGot >= 7) {   /* Command Status */
            Opcode = Dev->RxPacket[5] | (Dev->RxPacket[6] << 8);
        }
        Dev->EventsIn++;
        Dev->EventLog[Dev->EventLogNext++ % RTL_NUMBER_OF(Dev->EventLog)] = ((ULONG)Dev->RxPacket[1] << 16) | Opcode;
    } else if (Type == HCI_ACL_PKT) {
        Dev->AclIn++;
    }
    if (Type != HCI_SCO_PKT) {                               /* SCO over HCI is not offered */
        SwBtDeliver(Dev, Done, Type, &Dev->RxPacket[1], Dev->RxGot - 1);
    }
    Dev->RxGot = 0;
}

/* Starts the next WRITE_HCI if the transmitter is free. Lock held. */
static VOID SwBtStartTx(PSWBT_DEVICE Dev, PSWBT_DONE Done)
{
    WDFREQUEST Request;
    WDFMEMORY Memory;
    size_t Size = 0;
    PBTHX_HCI_READ_WRITE_CONTEXT Ctx = NULL;

    while (Dev->TxRequest == NULL && !SwBtDoneFull(Done) &&
           NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Dev->TxQueue, &Request))) {
        NTSTATUS Status = WdfRequestRetrieveInputMemory(Request, &Memory);
        if (NT_SUCCESS(Status)) {
            Ctx = (PBTHX_HCI_READ_WRITE_CONTEXT)WdfMemoryGetBuffer(Memory, &Size);
            if (Size < FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) ||
                Ctx->DataLen > Size - FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) ||
                Ctx->DataLen > HCI_MAX_PACKET || Ctx->DataLen == 0 ||
                (Ctx->Type != HciPacketCommand && Ctx->Type != HciPacketAclData)) {
                Status = STATUS_INVALID_PARAMETER;
            }
        }
        if (!NT_SUCCESS(Status)) {
            SwBtDoneAdd(Done, Request, Status, 0);
            continue;
        }
        Dev->TxRequest = Request;
        Dev->TxRequestLength = Ctx->DataLen;
        if (Ctx->Type == HciPacketCommand) {
            Dev->CmdsOut++;
            Dev->CmdLog[Dev->CmdLogNext++ % RTL_NUMBER_OF(Dev->CmdLog)] =
                Ctx->DataLen >= 2 ? (ULONG)(Ctx->Data[0] | (Ctx->Data[1] << 8)) : 0xFFFF;
        } else {
            Dev->AclOut++;
        }
        WdfInterruptAcquireLock(Dev->Interrupt);
        Dev->TxBuffer[0] = Ctx->Type;
        RtlCopyMemory(&Dev->TxBuffer[1], Ctx->Data, Ctx->DataLen);
        Dev->TxLength = 1 + Ctx->DataLen;
        Dev->TxSent = 0;
        Dev->TxDone = FALSE;
        Dev->TxBusy = TRUE;
        /* Fill the FIFO now; the transmitter-empty interrupt sends the rest. */
        if ((SwBtRead(Dev, UART_LSR) & UART_LSR_THRE) != 0 && SwBtTxBurst(Dev)) {
            WdfInterruptQueueDpcForIsr(Dev->Interrupt);
        } else {
            Dev->Ier |= UART_IER_TX;
            SwBtWrite(Dev, UART_IER, Dev->Ier);
        }
        WdfInterruptReleaseLock(Dev->Interrupt);
    }
}

/* The ISR sent the whole packet. Lock held. */
static VOID SwBtCompleteTx(PSWBT_DEVICE Dev, PSWBT_DONE Done)
{
    WDFREQUEST Request = Dev->TxRequest;
    WDFMEMORY Memory;
    size_t Size = 0;
    ULONG_PTR Info = 0;
    Dev->TxRequest = NULL;
    if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &Memory))) {
        PULONG Out = (PULONG)WdfMemoryGetBuffer(Memory, &Size);
        if (Size >= sizeof(ULONG)) {
            *Out = Dev->TxRequestLength;                     /* bytes written, as the sample */
            Info = sizeof(ULONG);
        }
    }
    SwBtDoneAdd(Done, Request, STATUS_SUCCESS, Info);
}

VOID SwBtEvtDpc(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(WdfInterruptGetDevice(Interrupt));
    UCHAR Chunk[64];                /* at most 21 packets, so Done never fills */
    SWBT_DONE Done;
    BOOLEAN TxDone;
    UNREFERENCED_PARAMETER(AssociatedObject);

    Done.Count = 0;
    for (;;) {
        ULONG Count = 0, Index;
        WdfSpinLockAcquire(Dev->Lock);
        WdfInterruptAcquireLock(Interrupt);
        while (Count < sizeof(Chunk) && Dev->RxTail != Dev->RxHead) {
            Chunk[Count++] = Dev->RxRing[Dev->RxTail & (RX_RING_SIZE - 1)];
            Dev->RxTail++;
        }
        TxDone = Dev->TxDone;
        Dev->TxDone = FALSE;
        WdfInterruptReleaseLock(Interrupt);
        for (Index = 0; Index < Count; ++Index) {
            SwBtParse(Dev, &Done, Chunk[Index]);
        }
        if (TxDone && Dev->TxRequest != NULL) {
            SwBtCompleteTx(Dev, &Done);
            SwBtStartTx(Dev, &Done);
        }
        WdfSpinLockRelease(Dev->Lock);
        SwBtDoneFlush(&Done);
        if (Count == 0 && !TxDone) {
            break;
        }
    }
}

/* ------------------------------------------------------------------- I/O */

VOID SwBtEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(WdfIoQueueGetDevice(Queue));
    WDFMEMORY Memory;
    PVOID Buffer;
    size_t Size = 0;
    SWBT_DONE Done;
    NTSTATUS Status;

    switch (Code) {
    case IOCTL_BTHX_GET_VERSION:
        InterlockedIncrement(&Dev->GetVersionRequests);
        if (OutLength < sizeof(BTHX_VERSION) || !NT_SUCCESS(Status = WdfRequestRetrieveOutputMemory(Request, &Memory))) {
            Dev->LastSetupStatus = 0x01000000u | (OutLength < sizeof(BTHX_VERSION) ? 0xFFFFFFu : ((ULONG)Status & 0xFFFFFFu));
            break;
        }
        Buffer = WdfMemoryGetBuffer(Memory, &Size);
        RtlCopyMemory(Buffer, &Microsoft_BTHX_DDI_Version, sizeof(BTHX_VERSION));
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(BTHX_VERSION));
        return;

    case IOCTL_BTHX_SET_VERSION:
        InterlockedIncrement(&Dev->SetVersionRequests);
        if (InLength < sizeof(BTHX_VERSION)) {
            Dev->LastSetupStatus = 0x02FFFFFFu;
            break;
        }
        if (NT_SUCCESS(WdfRequestRetrieveInputMemory(Request, &Memory))) {
            Buffer = WdfMemoryGetBuffer(Memory, &Size);
            if (Size >= sizeof(BTHX_VERSION)) {
                Dev->SetVersionValue = ((PBTHX_VERSION)Buffer)->Version;
            }
        }
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    case IOCTL_BTHX_QUERY_CAPABILITIES:
        InterlockedIncrement(&Dev->CapsRequests);
        if (OutLength < sizeof(BTHX_CAPABILITIES) || !NT_SUCCESS(Status = WdfRequestRetrieveOutputMemory(Request, &Memory))) {
            Dev->LastSetupStatus = 0x03000000u | (OutLength < sizeof(BTHX_CAPABILITIES) ? (ULONG)OutLength : ((ULONG)Status & 0xFFFFFFu));
            break;
        }
        Buffer = WdfMemoryGetBuffer(Memory, &Size);
        RtlCopyMemory(Buffer, &Dev->Caps, sizeof(BTHX_CAPABILITIES));
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(BTHX_CAPABILITIES));
        return;

    case IOCTL_BTHX_WRITE_HCI:
        InterlockedIncrement(&Dev->WriteRequests);
        if (!Dev->ChipOk) {
            WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
            return;
        }
        Done.Count = 0;
        WdfSpinLockAcquire(Dev->Lock);
        Status = WdfRequestForwardToIoQueue(Request, Dev->TxQueue);
        if (NT_SUCCESS(Status)) {
            SwBtStartTx(Dev, &Done);
        }
        WdfSpinLockRelease(Dev->Lock);
        SwBtDoneFlush(&Done);
        if (!NT_SUCCESS(Status)) {
            WdfRequestComplete(Request, Status);
        }
        return;

    case IOCTL_BTHX_READ_HCI: {
        ULONG Type = 0;
        if (InLength < sizeof(UCHAR) || OutLength < FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) ||
            !NT_SUCCESS(WdfRequestRetrieveInputMemory(Request, &Memory))) {
            break;
        }
        Buffer = WdfMemoryGetBuffer(Memory, &Size);
        RtlCopyMemory(&Type, Buffer, min(Size, sizeof(Type)));
        if ((Type != HciPacketEvent && Type != HciPacketAclData) || Dev->EventQueue == NULL) {
            break;
        }
        InterlockedIncrement(Type == HciPacketEvent ? &Dev->ReadEventRequests : &Dev->ReadAclRequests);
        Done.Count = 0;
        WdfSpinLockAcquire(Dev->Lock);
        Status = WdfRequestForwardToIoQueue(Request, Type == HciPacketEvent ? Dev->EventQueue : Dev->AclQueue);
        if (NT_SUCCESS(Status)) {
            if (Type == HciPacketEvent) {
                SwBtDrainKept(Dev, &Done, &Dev->KeptEvents, Dev->EventQueue);
            } else {
                SwBtDrainKept(Dev, &Done, &Dev->KeptAcl, Dev->AclQueue);
            }
        }
        WdfSpinLockRelease(Dev->Lock);
        SwBtDoneFlush(&Done);
        if (!NT_SUCCESS(Status)) {
            WdfRequestComplete(Request, Status);
        }
        return;
    }

    default:
        InterlockedIncrement(&Dev->OtherIoctls);
        Dev->LastOtherIoctl = Code;
        break;
    }
    WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
}

/* Diagnostics: every 2 s for the first minute, then every 10 s. */
VOID SwBtEvtTimer(WDFTIMER Timer)
{
    PSWBT_DEVICE Dev = SwBtGetDevice((WDFDEVICE)WdfTimerGetParentObject(Timer));
    WCHAR Text[160];
    ULONG Index, Next;

    SwBtTrace(L"IsrCalls", (ULONG)Dev->IsrCalls);
    SwBtTrace(L"IirSeen", Dev->IirSeen);
    SwBtTrace(L"RxBytes", Dev->RxBytes);
    SwBtTrace(L"TxBytes", Dev->TxBytes);
    SwBtTrace(L"RxOverflows", Dev->RxOverflows);
    SwBtTrace(L"LineOverruns", Dev->LineOverruns);
    SwBtTrace(L"LineErrors", Dev->LineErrors);
    SwBtTrace(L"ParserSkipped", Dev->ParserSkipped);
    SwBtTrace(L"CmdsOut", Dev->CmdsOut);
    SwBtTrace(L"AclOut", Dev->AclOut);
    SwBtTrace(L"EventsIn", Dev->EventsIn);
    SwBtTrace(L"AclIn", Dev->AclIn);
    SwBtTrace(L"Dropped", Dev->Dropped);
    SwBtTrace(L"ReadEventRequests", (ULONG)Dev->ReadEventRequests);
    SwBtTrace(L"ReadAclRequests", (ULONG)Dev->ReadAclRequests);
    SwBtTrace(L"WriteRequests", (ULONG)Dev->WriteRequests);
    SwBtTrace(L"OtherIoctls", (ULONG)Dev->OtherIoctls);
    SwBtTrace(L"LastOtherIoctl", Dev->LastOtherIoctl);
    SwBtTrace(L"ChildD0", (ULONG)Dev->ChildD0);
    SwBtTrace(L"GetVersionRequests", (ULONG)Dev->GetVersionRequests);
    SwBtTrace(L"SetVersionRequests", (ULONG)Dev->SetVersionRequests);
    SwBtTrace(L"SetVersionValue", Dev->SetVersionValue);
    SwBtTrace(L"CapsRequests", (ULONG)Dev->CapsRequests);
    SwBtTrace(L"LastSetupStatus", Dev->LastSetupStatus);
    SwBtTrace(L"ChildRequests", (ULONG)Dev->ChildRequests);
    SwBtTrace(L"ChildForwardFailures", (ULONG)Dev->ChildForwardFailures);
    SwBtTrace(L"LastChildIoctl", Dev->LastChildIoctl);
    SwBtTrace(L"LastForwardStatus", Dev->LastForwardStatus);
    SwBtTrace(L"TxBusy", Dev->TxBusy | (Dev->TxRequest != NULL ? 2u : 0u));

    Text[0] = L'\0';
    Next = Dev->CmdLogNext;
    for (Index = Next > 8 ? Next - 8 : 0; Index < Next; ++Index) {
        WCHAR Item[12];
        (VOID)RtlStringCchPrintfW(Item, RTL_NUMBER_OF(Item), L"%04X ", Dev->CmdLog[Index % 8]);
        (VOID)RtlStringCchCatW(Text, RTL_NUMBER_OF(Text), Item);
    }
    SwBtTraceString(L"LastCommands", Text);
    Text[0] = L'\0';
    Next = Dev->EventLogNext;
    for (Index = Next > 8 ? Next - 8 : 0; Index < Next; ++Index) {
        WCHAR Item[16];
        (VOID)RtlStringCchPrintfW(Item, RTL_NUMBER_OF(Item), L"%02X:%04X ", Dev->EventLog[Index % 8] >> 16,
                                  Dev->EventLog[Index % 8] & 0xFFFF);
        (VOID)RtlStringCchCatW(Text, RTL_NUMBER_OF(Text), Item);
    }
    SwBtTraceString(L"LastEvents", Text);

    ++Dev->Ticks;
    if (Dev->SafeMode && SwBtSafeModeCleared(Dev)) {
        /* -Mode Retry while running: start now, with the UART interrupt held off. */
        WdfInterruptAcquireLock(Dev->Interrupt);
        Dev->Ier = 0;
        SwBtWrite(Dev, UART_IER, 0);
        WdfInterruptReleaseLock(Dev->Interrupt);
        SwBtStart(Dev);
        WdfInterruptAcquireLock(Dev->Interrupt);
        Dev->RxHead = Dev->RxTail = 0;
        Dev->TxBusy = Dev->TxDone = FALSE;
        Dev->Ier = UART_IER_RX;
        SwBtWrite(Dev, UART_IER, Dev->Ier);
        WdfInterruptReleaseLock(Dev->Interrupt);
    }
    if (!Dev->SafeMode && Dev->ChildAdded && Dev->Ticks == Dev->ChildTick + 5) {
        SwBtTrace(L"BootPending", 0);           /* 10 s after the child: this boot is fine */
        SwBtFlush();
    }
    if (!Dev->SafeMode && Dev->Ticks >= Dev->ChildTick && !Dev->ChildAdded && Dev->ChipOk) {
        WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
        NTSTATUS Status;
        WdfFdoLockStaticChildListForIteration(Device);
        Status = SwBtCreateChild(Device);
        WdfFdoUnlockStaticChildListFromIteration(Device);   /* PnP re-enumerates: the child appears */
        Dev->ChildAdded = NT_SUCCESS(Status);
        SwBtTrace(L"ChildStatus", (ULONG)Status);
        SwBtTrace(L"Stage", 6);
    }
    {
        BOOLEAN Settled = !Dev->SafeMode && Dev->ChildAdded && Dev->Ticks > Dev->ChildTick + 5;
        if (!Settled && !Dev->SafeMode) {
            SwBtFlush();                        /* only while a hang is still possible */
        }
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_SEC(Settled ? 10 : 2));
    }
}

/* The child (BthMini) hands every BTHX request to this device. */
NTSTATUS SwBtEvtChildD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    InterlockedIncrement(&SwBtGetDevice(WdfPdoGetParent(Device))->ChildD0);
    return STATUS_SUCCESS;
}

VOID SwBtEvtChildIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    WDF_REQUEST_FORWARD_OPTIONS Options;
    WDFDEVICE Parent = WdfPdoGetParent(WdfIoQueueGetDevice(Queue));
    PSWBT_DEVICE Dev = SwBtGetDevice(Parent);
    NTSTATUS Status = STATUS_INVALID_PARAMETER;
    UNREFERENCED_PARAMETER(OutLength);
    UNREFERENCED_PARAMETER(InLength);

    switch (Code) {
    case IOCTL_BTHX_GET_VERSION:
    case IOCTL_BTHX_SET_VERSION:
    case IOCTL_BTHX_QUERY_CAPABILITIES:
    case IOCTL_BTHX_WRITE_HCI:
    case IOCTL_BTHX_READ_HCI:
        WDF_REQUEST_FORWARD_OPTIONS_INIT(&Options);
        Options.Flags = WDF_REQUEST_FORWARD_OPTION_SEND_AND_FORGET;
        Status = WdfRequestForwardToParentDeviceIoQueue(Request, WdfDeviceGetDefaultQueue(Parent), &Options);
        break;
    default:
        break;
    }
    InterlockedIncrement(&Dev->ChildRequests);
    Dev->LastChildIoctl = Code;
    if (!NT_SUCCESS(Status)) {
        InterlockedIncrement(&Dev->ChildForwardFailures);
        Dev->LastForwardStatus = (ULONG)Status;
    }
    if (!NT_SUCCESS(Status)) {
        WdfRequestComplete(Request, Status);
    }
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwBtEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeMemory && Dev->Uart == NULL) {
            if (Desc->u.Memory.Length < UART_LENGTH) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            Dev->Uart = (volatile ULONG *)MmMapIoSpaceEx(Desc->u.Memory.Start, UART_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
            if (Dev->Uart == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
    }
    SwBtTrace(L"HaveUart", Dev->Uart != NULL);
    if (Dev->Uart != NULL && Dev->CarUartd == NULL) {
        PHYSICAL_ADDRESS Car;
        Car.QuadPart = CAR_UARTD_SOURCE;
        Dev->CarUartd = (volatile ULONG *)MmMapIoSpaceEx(Car, sizeof(ULONG), PAGE_READWRITE | PAGE_NOCACHE);
    }
    SwBtTrace(L"HaveCar", Dev->CarUartd != NULL);
    SwBtStage(3);
    return Dev->Uart == NULL ? STATUS_DEVICE_CONFIGURATION_ERROR : STATUS_SUCCESS;
}

NTSTATUS SwBtEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);

    WdfSpinLockAcquire(Dev->Lock);
    SwBtFreeKept(Dev);
    WdfSpinLockRelease(Dev->Lock);
    if (Dev->Uart != NULL) {
        MmUnmapIoSpace((PVOID)Dev->Uart, UART_LENGTH);
        Dev->Uart = NULL;
    }
    if (Dev->CarUartd != NULL) {
        MmUnmapIoSpace((PVOID)Dev->CarUartd, sizeof(ULONG));
        Dev->CarUartd = NULL;
    }
    return STATUS_SUCCESS;
}

/*
 * Interrupts are already off here. At 3 Mbaud: ask the chip for 115200
 * (Update_UART_Baud_Rate) and follow, so the next start finds the ROM speed.
 */
NTSTATUS SwBtEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(Device);
    UCHAR Event[64];
    UCHAR Params[6] = { 0, 0, (UCHAR)115200u, (UCHAR)(115200u >> 8), (UCHAR)(115200u >> 16), 0 };
    ULONG Length;
    UNREFERENCED_PARAMETER(TargetState);

    if (Dev->SafeMode || Dev->Uart == NULL || Dev->CarUartd == NULL || !Dev->HighSpeedNow) {
        return STATUS_SUCCESS;
    }
    SwBtFlushRx(Dev);
    Length = SwBtPollCommand(Dev, BCM_UPDATE_BAUD, Params, sizeof(Params), Event, sizeof(Event));
    WRITE_REGISTER_ULONG((PULONG)Dev->CarUartd, CAR_UART_PLLP);
    KeStallExecutionProcessor(10);
    SwBtUartInit(Dev, UART_DIV_115200, UART_MCR_RTS);
    Dev->HighSpeedNow = FALSE;
    SwBtTrace(L"ExitTo115200", Length >= 7 ? Event[6] + 0x100u : 0);
    SwBtFlush();
    return STATUS_SUCCESS;
}

/* Safe mode ends when BootPending was cleared (-Mode Retry). PASSIVE. */
static BOOLEAN SwBtSafeModeCleared(PSWBT_DEVICE Dev)
{
    ULONG Pending = 1;
    if (!Dev->SafeMode || !SwBtReadParam(L"BootPending", &Pending) || Pending != 0) {
        return FALSE;
    }
    Dev->SafeMode = FALSE;
    SwBtTrace(L"SafeMode", 0);
    SwBtTrace(L"SafeModeLeftByRetry", 1);
    SwBtTrace(L"BootPending", 1);
    SwBtStage(1);
    return TRUE;
}

/* Probes the chip (polling, the UART interrupt off) and schedules the child. */
static VOID SwBtStart(PSWBT_DEVICE Dev)
{
    SwBtStage(4);
    Dev->ChipOk = SwBtProbe(Dev);
    Dev->RxGot = 0;
    WdfSpinLockAcquire(Dev->Lock);
    SwBtFreeKept(Dev);
    WdfSpinLockRelease(Dev->Lock);
    SwBtTrace(L"ChipOk", Dev->ChipOk);
    Dev->ChildTick = Dev->Ticks + CHILD_DELAY_TICKS;
    SwBtStage(5);
}

/* Interrupts are still off here: the probe polls. */
NTSTATUS SwBtEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(Device);
    UNREFERENCED_PARAMETER(PreviousState);

    if (Dev->SafeMode && !SwBtSafeModeCleared(Dev)) {
        return STATUS_SUCCESS;
    }
    SwBtStart(Dev);
    return STATUS_SUCCESS;
}

static NTSTATUS SwBtCreateChild(WDFDEVICE Device)
{
    DECLARE_CONST_UNICODE_STRING(HardwareId, L"SWOA\\BTH_UART_H4");
    DECLARE_CONST_UNICODE_STRING(CompatibleId, L"MS_BTHX_BTHMINI");
    DECLARE_CONST_UNICODE_STRING(InstanceId, L"0");
    DECLARE_CONST_UNICODE_STRING(Description, L"Nintendo Switch Bluetooth radio");
    DECLARE_CONST_UNICODE_STRING(Location, L"Nintendo Switch UART-D");
    PWDFDEVICE_INIT Init;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_DEVICE_PNP_CAPABILITIES Caps;
    WDFDEVICE Child;
    UNICODE_STRING Container;
    NTSTATUS Status;

    Init = WdfPdoInitAllocate(Device);
    if (Init == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    WdfDeviceInitSetDeviceType(Init, FILE_DEVICE_BUS_EXTENDER);
    Status = WdfPdoInitAssignDeviceID(Init, &HardwareId);
    if (NT_SUCCESS(Status)) Status = WdfPdoInitAddHardwareID(Init, &HardwareId);
    if (NT_SUCCESS(Status)) Status = WdfPdoInitAddCompatibleID(Init, &CompatibleId);
    if (NT_SUCCESS(Status)) Status = WdfPdoInitAssignInstanceID(Init, &InstanceId);
    if (NT_SUCCESS(Status)) Status = WdfPdoInitAddDeviceText(Init, &Description, &Location, 0x409);
    if (NT_SUCCESS(Status) && NT_SUCCESS(RtlStringFromGUID(&SWBT_CONTAINER_INTERNAL, &Container))) {
        Status = WdfPdoInitAssignContainerID(Init, &Container);
        RtlFreeUnicodeString(&Container);
    }
    if (!NT_SUCCESS(Status)) {
        WdfDeviceInitFree(Init);
        return Status;
    }
    WdfPdoInitSetDefaultLocale(Init, 0x409);
    WdfPdoInitAllowForwardingRequestToParent(Init);
    {
        WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
        WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
        Pnp.EvtDeviceD0Entry = SwBtEvtChildD0Entry;
        WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Pnp);
    }

    Status = WdfDeviceCreate(&Init, WDF_NO_OBJECT_ATTRIBUTES, &Child);
    if (!NT_SUCCESS(Status)) {
        if (Init != NULL) {
            WdfDeviceInitFree(Init);
        }
        return Status;
    }
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&Caps);
    Caps.Removable = WdfFalse;
    Caps.SurpriseRemovalOK = WdfTrue;
    Caps.UniqueID = WdfTrue;
    WdfDeviceSetPnpCapabilities(Child, &Caps);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoInternalDeviceControl = SwBtEvtChildIoDeviceControl;
    QueueConfig.EvtIoDeviceControl = SwBtEvtChildIoDeviceControl;
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(Child, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    return WdfFdoAddStaticChild(Device, Child);
}

static NTSTATUS SwBtCreateManualQueue(WDFDEVICE Device, WDFQUEUE *Queue)
{
    WDF_IO_QUEUE_CONFIG Config;
    WDF_IO_QUEUE_CONFIG_INIT(&Config, WdfIoQueueDispatchManual);
    Config.PowerManaged = WdfFalse;
    return WdfIoQueueCreate(Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, Queue);
}

NTSTATUS SwBtEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDFDEVICE Device;
    PSWBT_DEVICE Dev;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwBtEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwBtEvtReleaseHardware;
    Pnp.EvtDeviceD0Entry = SwBtEvtD0Entry;
    Pnp.EvtDeviceD0Exit = SwBtEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWBT_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Dev = SwBtGetDevice(Device);
    InitializeListHead(&Dev->KeptEvents);
    InitializeListHead(&Dev->KeptAcl);
    Dev->Caps.MaxAclTransferInSize = HCI_MAX_PACKET;
    Dev->Caps.ScoSupport = ScoSupportHCIBypass;     /* as the sample: the only option BthMini takes */
    Dev->Caps.MaxScoChannels = 1;
    Dev->Caps.IsDeviceIdleCapable = TRUE;
    Dev->Caps.IsDeviceWakeCapable = FALSE;

    Status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &Dev->Lock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    {
        ULONG Pending = 0;
        Dev->SafeMode = SwBtReadParam(L"BootPending", &Pending) && Pending != 0;
    }
    SwBtTrace(L"SafeMode", Dev->SafeMode);
    SwBtTrace(L"SafeModeLeftByRetry", 0);
    if (Dev->SafeMode) {
        SwBtFlush();                            /* idle until -Mode Retry clears BootPending */
    } else {
        SwBtTrace(L"BootPending", 1);
        SwBtStage(1);
    }
    Dev->ChildTick = CHILD_DELAY_TICKS;
    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, SwBtEvtIsr, SwBtEvtDpc);
    InterruptConfig.EvtInterruptEnable = SwBtEvtInterruptEnable;
    InterruptConfig.EvtInterruptDisable = SwBtEvtInterruptDisable;
    Status = WdfInterruptCreate(Device, &InterruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &Dev->Interrupt);
    SwBtTrace(L"InterruptStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoInternalDeviceControl = SwBtEvtIoDeviceControl;
    QueueConfig.EvtIoDeviceControl = SwBtEvtIoDeviceControl;
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (NT_SUCCESS(Status)) Status = SwBtCreateManualQueue(Device, &Dev->TxQueue);
    if (NT_SUCCESS(Status)) Status = SwBtCreateManualQueue(Device, &Dev->EventQueue);
    if (NT_SUCCESS(Status)) Status = SwBtCreateManualQueue(Device, &Dev->AclQueue);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_TIMER_CONFIG_INIT(&TimerConfig, SwBtEvtTimer);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->Timer);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        WdfTimerStart(Dev->Timer, WDF_REL_TIMEOUT_IN_SEC(2));
    }
    SwBtStage(2);                           /* the child comes from the timer */
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwBtParamsPath)) {
        RtlCopyMemory(SwBtParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwBtParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwBtParamsPath);
    }
    SwBtTrace(L"DriverVersion", 13);
    WDF_DRIVER_CONFIG_INIT(&Config, SwBtEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
