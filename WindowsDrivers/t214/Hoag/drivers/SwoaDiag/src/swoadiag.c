/*
 * SwoaDiag: read-only XUSB register snapshot for Windows on the Nintendo Switch.
 * A non-PnP KMDF driver (like SwoaHello). Each `sc start` reads a fixed list
 * of registers once and writes them to
 *   HKLM\SYSTEM\CurrentControlSet\Services\SwoaDiag\Parameters
 * then the script stops and deletes the service. Nothing is ever written to
 * the hardware; only these windows are mapped, all read-only by use:
 *   xHCI     0x70090000: USBCMD, USBSTS, PORTSC 1..MaxPorts, IMAN0
 *   FPCI cfg 0x70098000: command, ARU mailbox (CMD/DATA_IN/DATA_OUT/OWNER), SMI
 *   IPFS     0x70099000: host configuration, interrupt mask, clock-gate hysteresis
 *   PADCTL   0x7009F000: pad mux, port caps, ELPG, OTG pad 0, bias, VBUS/ID
 * Purpose: see why a USB device plugged in again under Windows is not seen
 * (e.g. a Falcon mailbox request nobody answers, linux tegra-xusb.c).
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD SwoaDiagUnload;

static WCHAR ParamsPath[256];

static VOID Put(PCWSTR Name, ULONG Value)
{
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, ParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
}

static volatile ULONG *MapWindow(ULONG64 Physical, SIZE_T Length)
{
    PHYSICAL_ADDRESS Address;
    Address.QuadPart = (LONGLONG)Physical;
    return (volatile ULONG *)MmMapIoSpaceEx(Address, Length, PAGE_READWRITE | PAGE_NOCACHE);
}

static ULONG Rd(volatile ULONG *Base, ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((PUCHAR)Base + Offset));
}

static VOID SnapshotXhci(VOID)
{
    volatile ULONG *X = MapWindow(0x70090000, 0x8000);
    ULONG CapLength, Params1, MaxPorts, RtsOff, Op, Port;
    WCHAR Name[32];

    if (X == NULL) {
        Put(L"XhciMapFailed", 1);
        return;
    }
    CapLength = Rd(X, 0x00) & 0xFF;
    Params1 = Rd(X, 0x04);
    RtsOff = Rd(X, 0x18) & ~0x1Fu;
    MaxPorts = Params1 >> 24;
    Op = CapLength;
    Put(L"XhciCapLength", CapLength);
    Put(L"XhciMaxPorts", MaxPorts);
    Put(L"XhciUSBCMD", Rd(X, Op + 0x00));
    Put(L"XhciUSBSTS", Rd(X, Op + 0x04));
    if (MaxPorts > 16) {
        MaxPorts = 16;
    }
    for (Port = 1; Port <= MaxPorts; ++Port) {
        if (Op + 0x400 + 0x10 * (Port - 1) + 4 > 0x8000) {
            break;
        }
        (VOID)RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"XhciPORTSC%02u", Port);
        Put(Name, Rd(X, Op + 0x400 + 0x10 * (Port - 1)));
    }
    if (RtsOff + 0x24 <= 0x8000) {
        Put(L"XhciIMAN0", Rd(X, RtsOff + 0x20));
    }
    MmUnmapIoSpace((PVOID)X, 0x8000);
}

static VOID SnapshotFpciIpfs(VOID)
{
    volatile ULONG *F = MapWindow(0x70098000, 0x1000);
    volatile ULONG *I = MapWindow(0x70099000, 0x1000);

    if (F != NULL) {
        Put(L"FpciCommand", Rd(F, 0x04));
        Put(L"MboxCmd", Rd(F, 0xE4));          /* XUSB_CFG_ARU_MBOX_CMD */
        Put(L"MboxDataIn", Rd(F, 0xE8));       /* host -> firmware */
        Put(L"MboxDataOut", Rd(F, 0xEC));      /* firmware -> host: (cmd << 24) | data */
        Put(L"MboxOwner", Rd(F, 0xF0));        /* 0 none, 0x40 SW (host), 0x10 FW? (raw) */
        Put(L"SmiIntr", Rd(F, 0x428));         /* XUSB_CFG_ARU_SMI_INTR: bit 3 mailbox, bit 1 FW hang */
        Put(L"AruControl", Rd(F, 0x200));
        MmUnmapIoSpace((PVOID)F, 0x1000);
    } else {
        Put(L"FpciMapFailed", 1);
    }
    if (I != NULL) {
        Put(L"IpfsConfig", Rd(I, 0x180));
        Put(L"IpfsIntrMask", Rd(I, 0x188));
        Put(L"IpfsClkGateHyst", Rd(I, 0x1BC));
        MmUnmapIoSpace((PVOID)I, 0x1000);
    } else {
        Put(L"IpfsMapFailed", 1);
    }
}

static VOID SnapshotPadctl(VOID)
{
    volatile ULONG *P = MapWindow(0x7009F000, 0x1000);
    if (P == NULL) {
        Put(L"PadctlMapFailed", 1);
        return;
    }
    Put(L"PadMux", Rd(P, 0x004));
    Put(L"PortCap", Rd(P, 0x008));
    Put(L"ElpgProgram0", Rd(P, 0x020));
    Put(L"ElpgProgram1", Rd(P, 0x024));
    Put(L"BattChrgOtgPad0Ctl0", Rd(P, 0x080));
    Put(L"BattChrgOtgPad0Ctl1", Rd(P, 0x084));
    Put(L"OtgPad0Ctl0", Rd(P, 0x088));
    Put(L"OtgPad0Ctl1", Rd(P, 0x08C));
    Put(L"BiasPadCtl0", Rd(P, 0x284));
    Put(L"Usb2VbusId", Rd(P, 0xC60));
    MmUnmapIoSpace((PVOID)P, 0x1000);
}

VOID SwoaDiagUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    LARGE_INTEGER Now;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&Config, WDF_NO_EVENT_CALLBACK);
    Config.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    Config.EvtDriverUnload = SwoaDiagUnload;
    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (RegistryPath->Length / sizeof(WCHAR) + 12 >= RTL_NUMBER_OF(ParamsPath)) {
        return STATUS_SUCCESS;
    }
    RtlCopyMemory(ParamsPath, RegistryPath->Buffer, RegistryPath->Length);
    RtlCopyMemory(&ParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
    (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, ParamsPath);

    SnapshotXhci();
    SnapshotFpciIpfs();
    SnapshotPadctl();
    KeQuerySystemTime(&Now);
    Put(L"SnapshotTimeLow", Now.LowPart);
    Put(L"DriverVersion", 1);
    return STATUS_SUCCESS;
}
