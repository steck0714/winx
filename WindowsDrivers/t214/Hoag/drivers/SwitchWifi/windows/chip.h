/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_WINDOWS_CHIP_H
#define SWIFI_WINDOWS_CHIP_H

#include "resources.h"

typedef enum SWIFI_CHIP_STAGE {
    SwifiChipUnmapped,
    SwifiChipBarsMapped,
    SwifiChipDiscovered,
    SwifiChipFirmwareRead,
    SwifiChipNvramRead,
    SwifiChipUploading,
    SwifiChipStarting,
    SwifiChipFirmwareReady,
    SwifiChipStopped
} SWIFI_CHIP_STAGE;

typedef struct SWIFI_CHIP {
    SWIFI_PCIE_RESOURCES *Bus;
    PUCHAR Bar0;
    PUCHAR Bar2;
    ULONG Bar0Length;
    ULONG Bar2Length;
    ULONG BackplaneWindow;
    ULONG ChipId;
    ULONG ArmBase;
    ULONG ArmWrapper;
    ULONG D11Wrapper;
    ULONG PcieBase;
    ULONG PcieRevision;
    ULONG RamBase;
    ULONG RamCapacity;
    ULONG RamSize;
    ULONG SharedAddress;
    ULONG SharedFlags;
    ULONG RxDataOffset;
    ULONG RingInfo;
    USHORT MaxRxBuffers;
    BOOLEAN RuntimePinned;
    SWIFI_CHIP_STAGE Stage;
} SWIFI_CHIP;

/* All initialization calls run in a system worker at PASSIVE_LEVEL with
 * special kernel APCs enabled, before interrupts or queues are enabled.
 * The caller owns power/resources and supplies a prepared link. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiChipMapAndDiscover(SWIFI_CHIP *Chip, SWIFI_PCIE_RESOURCES *Bus);

/* Paths must be kernel DOS paths, e.g.
 * \SystemRoot\System32\drivers\SwitchWOA\brcmfmac4356A3-pcie.bin
 * and its matching .txt board file. Firmware/NVRAM are supplied privately.
 * A successfully stopped, still-powered endpoint can be reloaded through the
 * retained BAR mappings; a platform/PERST reset requires fresh preparation. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiChipLoadFirmware(SWIFI_CHIP *Chip, PCWSTR FirmwarePath, PCWSTR NvramPath);

/* After successful load, BAR0 is pinned to the PCIe core until quiescence.
 * Pass this base to the transport, not an assumed BAR0+0x2000 alias. */
_IRQL_requires_max_(DISPATCH_LEVEL)
PUCHAR SwifiChipRuntimeRegisters(SWIFI_CHIP *Chip);

/* Enable only after publishing synchronized host ring/index allocations. */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS SwifiChipSetBusMaster(SWIFI_CHIP *Chip, BOOLEAN Enable);

/* TCM addresses are chip RAM offsets (including RamBase), not host virtual
 * or physical addresses. Naturally aligned 32-bit accesses only. */
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN SwifiChipTcmRange(const SWIFI_CHIP *Chip, ULONG Address, ULONG Length);
_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG SwifiChipReadTcm(SWIFI_CHIP *Chip, ULONG Address);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID SwifiChipWriteTcm(SWIFI_CHIP *Chip, ULONG Address, ULONG Value);

/* Caller has quiesced interrupts and all protocol callbacks first. On
 * success bus mastering is off, CR4 is halted and D11 is held reset; only
 * then free DMA.
 * Failure requires platform reset before freeing any hardware-owned buffer. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiChipStopAfterQuiesce(SWIFI_CHIP *Chip);

/* Caller has completed the stop/reset contract above; this only unmaps BARs. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiChipUnmapAfterReset(SWIFI_CHIP *Chip);

#endif
