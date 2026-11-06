/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_MSGBUF_RING_H
#define SWIFI_MSGBUF_RING_H

#include "msgbuf_data.h"

typedef struct swifi_ring_ops {
    /* Read the firmware-owned index. A value outside depth is an error. */
    uint32_t (*read_peer)(void *context);
    /* Publish the host-owned index. Publication cannot be rolled back. */
    void (*write_host)(void *context, uint16_t value);
    /* DMA acquire/release barrier, including compiler ordering. */
    void (*barrier)(void *context);
} swifi_ring_ops;

typedef struct swifi_ring {
    uint8_t *memory;
    swifi_ring_ops ops;
    void *context;
    uint16_t depth;
    uint16_t item_size;
    uint16_t host_index;
    uint8_t host_produces;
    uint8_t prefix;
} swifi_ring;

typedef swifi_result (*swifi_ring_handler)(void *context,
    const uint8_t *item, size_t length);

/* Firmware is quiescent; storage and both indices must already be zero.
 * One device lock serializes ring operations. All memory is DMA coherent
 * (or uncached); streaming mappings require platform cache synchronization. */
swifi_result swifi_ring_init(swifi_ring *ring, const swifi_ring_ops *ops,
    void *context, void *memory, size_t memory_length, uint16_t depth,
    uint16_t item_size, int host_produces, uint8_t completion_prefix);

/* Publishes one item; the platform rings the H2D doorbell after OK. A ring
 * reserves one empty slot to distinguish full from empty, including wrap. */
swifi_result swifi_ring_submit(swifi_ring *ring,
    const uint8_t *item, size_t length);

/* Copy each item before publishing its released slot, so the handler never
 * sees memory that firmware can overwrite. budget bounds DPC execution.
 * Handler may return UNHANDLED for other control message types. */
swifi_result swifi_ring_drain(swifi_ring *ring, uint16_t budget,
    swifi_ring_handler handler, void *context, uint16_t *processed, int *more);

#endif
