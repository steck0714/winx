/* SPDX-License-Identifier: GPL-2.0-only */
/* Tegra AFI configuration translation follows Switchroot pci-tegra.c and
 * the pre-Windows WifiPcie.h implementation. */
#include "resources.h"

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
        if (T->Type == CmResourceTypeMemory) {
            if (T->u.Memory.Start.QuadPart == SWIFI_HOST_BASE &&
                T->u.Memory.Length == SWIFI_HOST_LENGTH) {
                Bit = 1; Host = T->u.Memory.Start;
            } else if (T->u.Memory.Start.QuadPart == SWIFI_CONFIG_BASE &&
                T->u.Memory.Length == SWIFI_CONFIG_LENGTH) {
                Bit = 2; Config = T->u.Memory.Start;
            } else if (T->u.Memory.Start.QuadPart == SWIFI_APERTURE_BASE &&
                T->u.Memory.Length == SWIFI_APERTURE_SIZE) {
                Bit = 4; Bus->Aperture = T->u.Memory.Start;
                Bus->ApertureLength = T->u.Memory.Length;
            } else {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
        } else if (T->Type == CmResourceTypeInterrupt) {
            if (T->Flags & (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED))
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            Bit = 8;
            Bus->RawInterrupt = *R;
            Bus->TranslatedInterrupt = *T;
        } else {
            /* Connection/resource-private descriptors are not register ranges. */
            continue;
        }
        if (Found & Bit)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        Found |= Bit;
    }
    if (Found != 15)
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

NTSTATUS SwifiMapBar(SWIFI_PCIE_RESOURCES *Bus, ULONG Address, ULONG Length,
    PUCHAR *Mapping)
{
    PHYSICAL_ADDRESS Physical;
    ULONGLONG Offset;
    *Mapping = NULL;
    if (!Length || Address < (ULONGLONG)Bus->Aperture.QuadPart)
        return STATUS_CONFLICTING_ADDRESSES;
    Offset = Address - (ULONGLONG)Bus->Aperture.QuadPart;
    if (Offset >= Bus->ApertureLength || Length > Bus->ApertureLength - Offset)
        return STATUS_CONFLICTING_ADDRESSES;
    Physical.QuadPart = Address;
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
