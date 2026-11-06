/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_PCI_INTX_H
#define SWIFI_PCI_INTX_H
#include <stdint.h>

typedef struct swifi_pci_config {
    void *context;
    uint32_t (*read)(void *context, uint16_t offset);
    void (*write)(void *context, uint16_t offset, uint32_t value);
} swifi_pci_config;

typedef enum swifi_intx_result {
    SWIFI_INTX_OK, SWIFI_INTX_NO_DEVICE, SWIFI_INTX_NO_PIN,
    SWIFI_INTX_INVALID_CAPS, SWIFI_INTX_BUS_MASTER, SWIFI_INTX_WRITE_FAILED
} swifi_intx_result;

/* Select PCI legacy INTx while the AFI and BCM interrupt sources are masked
 * and bus mastering is disabled. The caller provides serialized config access. */
swifi_intx_result swifi_select_intx(const swifi_pci_config *io);
#endif
