/* SPDX-License-Identifier: GPL-2.0-only */
/* Tegra AFI configuration translation follows Switchroot pci-tegra.c and
 * the pre-Windows WifiPcie.h implementation. */
#include "resources.h"
#include "../core/pci_intx.h"
#include <acpiioct.h>

NTSTATUS SwifiMapResources(SWIFI_PCIE_RESOURCES *Bus,
    WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    ULONG Count = WdfCmResourceListGetCount(Translated);
    ULONG Found = 0;
    PHYSICAL_ADDRESS Host = {0}, Config = {0};
    ULONG Index;
    RtlZeroMemory(Bus, sizeof(*Bus));
    if (WdfCmResourceListGetCount(Raw) != Count)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR R = WdfCmResourceListGetDescriptor(Raw, Index);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR T = WdfCmResourceListGetDescriptor(Translated, Index);
        ULONG Bit;
        if (!R || !T || R->Type != T->Type)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (R->Type != CmResourceTypeMemory) continue;
        if (R->u.Memory.Length != T->u.Memory.Length || !T->u.Memory.Length ||
            T->u.Memory.Start.QuadPart < 0 ||
            T->u.Memory.Start.QuadPart > MAXLONGLONG - (T->u.Memory.Length - 1))
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        /* Match the board's bus addresses in the raw list. Only the
         * translated CPU addresses supplied by PnP may be mapped. */
        if (R->u.Memory.Start.QuadPart == SWIFI_HOST_BASE && R->u.Memory.Length == SWIFI_HOST_LENGTH) {
            Bit = 1; Host = T->u.Memory.Start;
        } else if (R->u.Memory.Start.QuadPart == SWIFI_CONFIG_BASE && R->u.Memory.Length == SWIFI_CONFIG_LENGTH) {
            Bit = 2; Config = T->u.Memory.Start;
        } else if (R->u.Memory.Start.QuadPart == SWIFI_APERTURE_BASE && R->u.Memory.Length == SWIFI_APERTURE_SIZE) {
            Bit = 4; Bus->Aperture = T->u.Memory.Start;
            Bus->ApertureLength = T->u.Memory.Length;
        } else {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (Found & Bit)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        Found |= Bit;
    }
    if (Found != 7)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    Bus->Host = MmMapIoSpaceEx(Host, SWIFI_HOST_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    if (!Bus->Host)
        return STATUS_INSUFFICIENT_RESOURCES;
    Bus->Config = MmMapIoSpaceEx(Config, SWIFI_CONFIG_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    if (!Bus->Config) {
        SwifiUnmapResources(Bus);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

VOID SwifiUnmapResources(SWIFI_PCIE_RESOURCES *Bus)
{
    if (Bus->Config)
        MmUnmapIoSpace(Bus->Config, SWIFI_CONFIG_LENGTH);
    if (Bus->Host)
        MmUnmapIoSpace(Bus->Host, SWIFI_HOST_LENGTH);
    RtlZeroMemory(Bus, sizeof(*Bus));
}

static ULONG SwifiSelectConfig(SWIFI_PCIE_RESOURCES *Bus, USHORT Offset)
{
    /* Tegra's extended config-register bits are not standard ECAM bits. */
    ULONG Address = ((ULONG)(Offset & 0xf00u) << 16) | (1u << 16) | (Offset & 0xffu);
    PULONG Window = (PULONG)(Bus->Host + SWIFI_AFI_OFFSET + 0x30);
    WRITE_REGISTER_ULONG(Window, 0xfe100000u + ((Address & ~0xfffu) >> 8));
    (VOID)READ_REGISTER_ULONG(Window);
    return Address & 0xffcu;
}

ULONG SwifiConfigRead(SWIFI_PCIE_RESOURCES *Bus, USHORT Offset)
{
    if ((Offset & 3) || Offset >= 4096)
        return MAXULONG;
    return READ_REGISTER_ULONG((PULONG)(Bus->Config + SwifiSelectConfig(Bus, Offset)));
}

VOID SwifiConfigWrite(SWIFI_PCIE_RESOURCES *Bus, USHORT Offset, ULONG Value)
{
    if ((Offset & 3) || Offset >= 4096)
        return;
    WRITE_REGISTER_ULONG((PULONG)(Bus->Config + SwifiSelectConfig(Bus, Offset)), Value);
}

static uint32_t ReadEndpoint(void *Context, uint16_t Offset)
{
    return SwifiConfigRead(Context, Offset);
}

static void WriteEndpoint(void *Context, uint16_t Offset, uint32_t Value)
{
    SwifiConfigWrite(Context, Offset, Value);
}

NTSTATUS SwifiSelectLegacyInterrupt(SWIFI_PCIE_RESOURCES *Bus)
{
    const swifi_pci_config Io = {Bus, ReadEndpoint, WriteEndpoint};
    switch (swifi_select_intx(&Io)) {
    case SWIFI_INTX_OK: return STATUS_SUCCESS;
    case SWIFI_INTX_NO_DEVICE: return STATUS_DEVICE_DOES_NOT_EXIST;
    case SWIFI_INTX_NO_PIN: return STATUS_NOT_SUPPORTED;
    case SWIFI_INTX_BUS_MASTER: return STATUS_INVALID_DEVICE_STATE;
    default: return STATUS_DEVICE_PROTOCOL_ERROR;
    }
}

NTSTATUS SwifiMapBar(SWIFI_PCIE_RESOURCES *Bus, ULONG Address, ULONG Length,
    PUCHAR *Mapping)
{
    PHYSICAL_ADDRESS Physical;
    ULONGLONG Offset;
    *Mapping = NULL;
    if (!Length || Address < SWIFI_APERTURE_BASE)
        return STATUS_CONFLICTING_ADDRESSES;
    Offset = Address - SWIFI_APERTURE_BASE;
    if (Offset >= Bus->ApertureLength || Length > Bus->ApertureLength - Offset)
        return STATUS_CONFLICTING_ADDRESSES;
    Physical.QuadPart = Bus->Aperture.QuadPart + (LONGLONG)Offset;
    *Mapping = MmMapIoSpaceEx(Physical, Length, PAGE_READWRITE | PAGE_NOCACHE);
    return *Mapping ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

ULONG SwifiAfiAcknowledge(SWIFI_PCIE_RESOURCES *Bus, ULONG *Signature)
{
    PUCHAR Afi = Bus->Host + SWIFI_AFI_OFFSET;
    ULONG Code = READ_REGISTER_ULONG((PULONG)(Afi + 0xb8)) & 0x1fu;
    *Signature = 0;
    if (!Code)
        return 0;
    *Signature = READ_REGISTER_ULONG((PULONG)(Afi + 0xbc));
    if (Code == 6) {
        ULONG Message = READ_REGISTER_ULONG((PULONG)(Afi + 0x190));
        ULONG Port;
        /* PME status lives in each root port, unlike ordinary W1C messages.
         * Acknowledge it without claiming support for selective suspend. */
        for (Port = 0; Port < 2; ++Port) {
            if (Message & (1u << (4 + Port * 16))) {
                PULONG Status = (PULONG)(Bus->Host + Port * 0x1000 + 0xa0);
                WRITE_REGISTER_ULONG(Status, READ_REGISTER_ULONG(Status) | (1u << 16));
            }
        }
        if (!(Message & (0x1f001f00u | 0x00100010u)))
            WRITE_REGISTER_ULONG((PULONG)(Afi + 0x190), Message);
        /* INTx bits reflect endpoint assertion; masking/acknowledging the
         * BCM mailbox deasserts them. Do not clear them as ordinary W1C bits. */
    }
    WRITE_REGISTER_ULONG((PULONG)(Afi + 0xb8), 0);
    (VOID)READ_REGISTER_ULONG((PULONG)(Afi + 0xb8));
    return Code;
}

VOID SwifiAfiInterruptMask(SWIFI_PCIE_RESOURCES *Bus, BOOLEAN Enable)
{
    PUCHAR Afi = Bus->Host + SWIFI_AFI_OFFSET;
    if (Enable) {
        WRITE_REGISTER_ULONG((PULONG)(Afi + 0xc8), 0x7f); /* Host errors, as Switchroot. */
        WRITE_REGISTER_ULONG((PULONG)(Afi + 0xc4), MAXULONG); /* INTx / SM messages. */
    }
    WRITE_REGISTER_ULONG((PULONG)(Afi + 0xb4), Enable ? 1 : 0); /* MSI remains off. */
    (VOID)READ_REGISTER_ULONG((PULONG)(Afi + 0xb4));
}

NTSTATUS SwifiHoldEndpointReset(SWIFI_PCIE_RESOURCES *Bus)
{
    PULONG Control, Link;
    ULONG Value, Attempt;
    LARGE_INTEGER Delay;
    if (!Bus->Host)
        return STATUS_INVALID_DEVICE_STATE;
    Control = (PULONG)(Bus->Host + SWIFI_AFI_OFFSET + 0x118);
    Link = (PULONG)(Bus->Host + SWIFI_ROOT_OFFSET + 0x90);
    SwifiAfiInterruptMask(Bus, FALSE);
    Value = READ_REGISTER_ULONG(Control);
    if (Value == MAXULONG)
        return STATUS_DEVICE_DOES_NOT_EXIST;
    /* AFI_PEX1_CTRL.RST is active-low, as in tegra_pcie_port_reset. */
    WRITE_REGISTER_ULONG(Control, Value & ~1u);
    Value = READ_REGISTER_ULONG(Control);
    if (Value == MAXULONG || (Value & 1u))
        return STATUS_DEVICE_PROTOCOL_ERROR;
    Delay.QuadPart = -20 * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    Delay.QuadPart = -2 * 10000;
    for (Attempt = 0; Attempt < 50; ++Attempt) {
        Value = READ_REGISTER_ULONG(Link);
        if (Value == MAXULONG)
            return STATUS_DEVICE_DOES_NOT_EXIST;
        if (!(Value & 0x20000000u)) {
            KeMemoryBarrier();
            return STATUS_SUCCESS;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    return STATUS_IO_TIMEOUT;
}

NTSTATUS SwifiPlatformIsolation(WDFDEVICE Device, BOOLEAN AssertReset)
{
    ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER Input = {0};
    ACPI_EVAL_OUTPUT_BUFFER Output = {0};
    WDF_MEMORY_DESCRIPTOR In, Out;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE;
    RtlCopyMemory(Input.MethodName, "WIRS", 4);
    Input.IntegerArgument = AssertReset ? 1 : 0;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&In, &Input, sizeof(Input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Out, &Output, sizeof(Output));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT | WDF_REQUEST_SEND_OPTION_IGNORE_TARGET_STATE);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options, WDF_REL_TIMEOUT_IN_MS(1000));
    NTSTATUS Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device), NULL,
        IOCTL_ACPI_EVAL_METHOD, &In, &Out, &Options, &Bytes);
    if (!NT_SUCCESS(Status)) return Status;
    if (Bytes < sizeof(Output) || Output.Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE ||
        Output.Length != sizeof(Output) || Output.Count != 1 ||
        Output.Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER ||
        Output.Argument[0].DataLength != sizeof(ULONG) || Output.Argument[0].Argument != 1)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    return STATUS_SUCCESS;
}
