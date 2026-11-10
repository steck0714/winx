/* SPDX-License-Identifier: ISC */
#include "pci_intx.h"

/* PCI command/status are adjacent. Only write the command half: PCI status
 * contains write-one-to-clear bits. MSI/MSI-X controls are in the high half
 * of each standard capability's first DWORD. */
enum {
    Command = 0x04, Capabilities = 0x34, InterruptLine = 0x3c,
    BusMaster = 4, DisableIntx = 0x400, HaveCaps = 0x00100000,
    MsiId = 5, MsixId = 0x11, MsiOn = 1u << 16
};
#define MSIX_ON (UINT32_C(1) << 31)
#define MSIX_MASK_ALL (UINT32_C(1) << 30)

static int update(const swifi_pci_config *io, uint16_t offset, uint32_t original,
    uint32_t clear, uint32_t set)
{
    uint32_t value = (original & ~clear) | set;
    if (original != value) io->write(io->context, offset, value);
    uint32_t readback = io->read(io->context, offset);
    return readback != UINT32_MAX && (readback & (clear | set)) == (value & (clear | set));
}

swifi_intx_result swifi_select_intx(const swifi_pci_config *io)
{
    uint32_t command = io->read(io->context, Command);
    uint32_t irq = io->read(io->context, InterruptLine);
    if (command == UINT32_MAX || irq == UINT32_MAX) return SWIFI_INTX_NO_DEVICE;
    if (command & BusMaster) return SWIFI_INTX_BUS_MASTER;
    unsigned pin = (irq >> 8) & 255;
    if (!pin || pin > 4) return SWIFI_INTX_NO_PIN;

    uint16_t msi = 0, msix = 0;
    uint32_t msi_header = 0, msix_header = 0;
    unsigned next = 0;
    uint64_t seen = 0;
    if (command & HaveCaps) {
        uint32_t head = io->read(io->context, Capabilities);
        if (head == UINT32_MAX) return SWIFI_INTX_NO_DEVICE;
        next = head & 255;
    }
    while (next) {
        if (next < 0x40 || (next & 3)) return SWIFI_INTX_INVALID_CAPS;
        uint64_t bit = UINT64_C(1) << (next / 4);
        if (seen & bit) return SWIFI_INTX_INVALID_CAPS;
        seen |= bit;
        uint32_t h = io->read(io->context, (uint16_t)next);
        if (h == UINT32_MAX) return SWIFI_INTX_NO_DEVICE;
        if ((h & 255) == MsiId) {
            if (msi) return SWIFI_INTX_INVALID_CAPS;
            msi = (uint16_t)next;
            msi_header = h;
        } else if ((h & 255) == MsixId) {
            if (msix) return SWIFI_INTX_INVALID_CAPS;
            msix = (uint16_t)next;
            msix_header = h;
        }
        next = (h >> 8) & 255;
    }
    command &= 0xffff;
    if ((msi_header & MsiOn) || (msix_header & MSIX_ON)) {
        if (!update(io, Command, command, 0, DisableIntx)) return SWIFI_INTX_WRITE_FAILED;
        command |= DisableIntx;
        if ((msi_header & MsiOn) && !update(io, msi, msi_header, MsiOn, 0))
            return SWIFI_INTX_WRITE_FAILED;
        if (msix_header & MSIX_ON) {
            if (!update(io, msix, msix_header, 0, MSIX_MASK_ALL) ||
                !update(io, msix, msix_header | MSIX_MASK_ALL, MSIX_ON, 0))
                return SWIFI_INTX_WRITE_FAILED;
        }
    }
    return update(io, Command, command, DisableIntx | BusMaster, 0) ?
        SWIFI_INTX_OK : SWIFI_INTX_WRITE_FAILED;
}
