// TegraEhciFix.c - TEST-ONLY WDM kernel driver for the Nintendo Switch (Tegra210) EHCI "USB1".
//
// Problem it works around (UNVERIFIED, see ehci_reset_probe.nsh):
//   The in-box Windows usbehci driver resets the controller (USBCMD.HCRESET) when it starts. The Tegra EHCI core
//   (ChipIdea based) comes out of reset in IDLE mode (USBMODE.CM = 0) and software has to write USBMODE.CM = 3 (host)
//   again (the Linux ehci-tegra driver does this). usbehci does not know that, so no host controller would appear.
//
// What it does: a dedicated thread watches USBMODE. As soon as the reset is finished (USBCMD.HCRESET = 0) and the mode is
// IDLE, it writes CM = 3. The register addresses are hard-coded for the Switch (USB1 at 0x7D000000, operational
// registers at +0x130, USBMODE at +0x1F8, as in the firmware and in the ACPI table).
//
// NOT BUILT, NOT TESTED. Needs the WDK (ARM64), test-signing or "nointegritychecks", and a CPU core that spins.
#include <ntddk.h>

#define EHCI_BASE_PA     0x7D000000ULL
#define EHCI_MAP_LEN     0x1000
#define REG_USBCMD       0x130
#define REG_USBSTS       0x134
#define REG_PORTSC       0x174
#define REG_USBMODE      0x1F8
#define USBCMD_HCRESET   0x00000002
#define USBMODE_CM_MASK  0x00000003
#define USBMODE_CM_HOST  0x00000003

static PUCHAR         g_Regs;
static PKTHREAD       g_Thread;
static volatile LONG  g_Stop;
static volatile LONG  g_Fixes;

static ULONG Rd32 (ULONG Off) { return READ_REGISTER_ULONG ((PULONG)(g_Regs + Off)); }
static VOID  Wr32 (ULONG Off, ULONG V) { WRITE_REGISTER_ULONG ((PULONG)(g_Regs + Off), V); }

static VOID FixThread (PVOID Context)
{
    UNREFERENCED_PARAMETER (Context);
    KeSetPriorityThread (KeGetCurrentThread (), LOW_REALTIME_PRIORITY);

    while (InterlockedCompareExchange (&g_Stop, 0, 0) == 0) {
        // Only touch USBMODE after the reset has finished.
        if ((Rd32 (REG_USBCMD) & USBCMD_HCRESET) == 0) {
            ULONG Mode = Rd32 (REG_USBMODE);
            if ((Mode & USBMODE_CM_MASK) == 0) {
                Wr32 (REG_USBMODE, Mode | USBMODE_CM_HOST);
                LONG N = InterlockedIncrement (&g_Fixes);
                DbgPrint ("TegraEhciFix: host mode restored (#%ld) usbmode=%08x usbcmd=%08x usbsts=%08x portsc=%08x\n",
                          N, Rd32 (REG_USBMODE), Rd32 (REG_USBCMD), Rd32 (REG_USBSTS), Rd32 (REG_PORTSC));
            }
        }
        KeStallExecutionProcessor (10);          // ~10 us; the thread keeps one core busy (test only)
    }
    PsTerminateSystemThread (STATUS_SUCCESS);
}

static VOID DriverUnload (PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER (DriverObject);
    InterlockedExchange (&g_Stop, 1);
    if (g_Thread != NULL) {
        KeWaitForSingleObject (g_Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject (g_Thread);
        g_Thread = NULL;
    }
    if (g_Regs != NULL) {
        MmUnmapIoSpace (g_Regs, EHCI_MAP_LEN);
        g_Regs = NULL;
    }
}

NTSTATUS DriverEntry (PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    PHYSICAL_ADDRESS  Pa;
    HANDLE            Th;
    NTSTATUS          St;

    UNREFERENCED_PARAMETER (RegistryPath);
    Pa.QuadPart = EHCI_BASE_PA;
    g_Regs = (PUCHAR)MmMapIoSpace (Pa, EHCI_MAP_LEN, MmNonCached);
    if (g_Regs == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DbgPrint ("TegraEhciFix: usbcmd=%08x usbsts=%08x usbmode=%08x portsc=%08x\n",
              Rd32 (REG_USBCMD), Rd32 (REG_USBSTS), Rd32 (REG_USBMODE), Rd32 (REG_PORTSC));

    St = PsCreateSystemThread (&Th, THREAD_ALL_ACCESS, NULL, NULL, NULL, FixThread, NULL);
    if (!NT_SUCCESS (St)) {
        MmUnmapIoSpace (g_Regs, EHCI_MAP_LEN);
        g_Regs = NULL;
        return St;
    }
    St = ObReferenceObjectByHandle (Th, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&g_Thread, NULL);
    ZwClose (Th);
    if (!NT_SUCCESS (St)) {
        InterlockedExchange (&g_Stop, 1);          // thread ends by itself; leave the mapping in place (never unloads)
        return St;
    }

    DriverObject->DriverUnload = DriverUnload;
    return STATUS_SUCCESS;
}
