/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWIFI_WINDOWS_RESOURCES_H
#define SWIFI_WINDOWS_RESOURCES_H

#include <ntddk.h>
#include <wdf.h>
#include "../platform/tegra_resources.h"

typedef struct SWIFI_PCIE_RESOURCES {
    PUCHAR Host;
    PUCHAR Config;
    PHYSICAL_ADDRESS Aperture;
    ULONG ApertureLength;
    CM_PARTIAL_RESOURCE_DESCRIPTOR RawInterrupt;
    CM_PARTIAL_RESOURCE_DESCRIPTOR TranslatedInterrupt;
} SWIFI_PCIE_RESOURCES;

/* The WFI0 composite device owns the host and one BCM4356 endpoint. This
 * is not an ECAM root bridge and must not bind the inbox pci.sys to it. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiMapResources(SWIFI_PCIE_RESOURCES *Bus,
    WDFCMRESLIST Raw, WDFCMRESLIST Translated);

/* Caller has disabled interrupts, stopped DMA and reset the endpoint. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiUnmapResources(SWIFI_PCIE_RESOURCES *Bus);

/* Serialize configuration-window/backplane accesses with the device lock.
 * Offset is a naturally aligned endpoint 1:0.0 configuration DWORD. */
_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG SwifiConfigRead(SWIFI_PCIE_RESOURCES *Bus, USHORT Offset);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID SwifiConfigWrite(SWIFI_PCIE_RESOURCES *Bus, USHORT Offset, ULONG Value);

/* Map a validated BAR slice only, not the whole 208 MiB outbound aperture. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiMapBar(SWIFI_PCIE_RESOURCES *Bus, ULONG Address, ULONG Length,
    PUCHAR *Mapping);

/* ISR-lock protected. AFI legacy INTx shares GSI 130 with host notifications.
 * Return the AFI reason code (0=no host reason, 6=legacy, 10=presence). */
ULONG SwifiAfiAcknowledge(SWIFI_PCIE_RESOURCES *Bus, ULONG *Signature);
VOID SwifiAfiInterruptMask(SWIFI_PCIE_RESOURCES *Bus, BOOLEAN Enable);

/* Stop fallback after quiescing IRQs/callers: assert the Wi-Fi port's PERST
 * and confirm link-down before freeing DMA. Does not touch GPIO, Bluetooth,
 * shared supplies or UPHY. A successful hold needs cold firmware preparation
 * before reuse because the endpoint BAR configuration has been reset. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiHoldEndpointReset(SWIFI_PCIE_RESOURCES *Bus);

#endif
