/* SPDX-License-Identifier: ISC */
/* BCM4356 msgbuf-v5 packet path. See NOTICE for the protocol source. */
#ifndef SWIFI_MSGBUF_DATA_H
#define SWIFI_MSGBUF_DATA_H

#include <stddef.h>
#include <stdint.h>

#define SWIFI_CONTROL_RING 0u
#define SWIFI_RXPOST_RING 1u
#define SWIFI_FIRST_FLOW_RING 2u
#define SWIFI_CONTROL_ITEM_SIZE 40u
#define SWIFI_RXPOST_ITEM_SIZE 32u
#define SWIFI_TX_ITEM_SIZE 48u
#define SWIFI_ETH_HEADER_SIZE 14u
#define SWIFI_PACKET_SIZE 2048u

typedef enum swifi_result {
    SWIFI_OK,
    SWIFI_RETRY,
    SWIFI_INVALID,
    SWIFI_PROTOCOL_ERROR,
    SWIFI_UNHANDLED,
    SWIFI_STOPPED
} swifi_result;

typedef enum swifi_flow_state {
    SWIFI_FLOW_CLOSED,
    SWIFI_FLOW_CREATING,
    SWIFI_FLOW_OPEN,
    SWIFI_FLOW_DELETING
} swifi_flow_state;

typedef enum swifi_completion_reason {
    SWIFI_COMPLETED,
    SWIFI_FIRMWARE_ERROR,
    SWIFI_BAD_PACKET,
    SWIFI_CANCELLED
} swifi_completion_reason;

typedef struct swifi_packet_completion {
    void *cookie;
    uint32_t request_id;
    uint16_t flow_ring;
    uint16_t offset;
    uint16_t rx_flags; /* v5 completion flags, including native 802.11 vs Ethernet. */
    uint16_t length;
    int16_t firmware_status;
    uint16_t tx_status; /* Firmware TX result, not an NDIS status. */
    uint8_t ifidx;
    swifi_completion_reason reason;
} swifi_packet_completion;

typedef struct swifi_data_ops {
    /*
     * Copy and publish one complete item in the selected H2D ring. OK transfers
     * DMA ownership even if a subsequent doorbell write fails. RETRY means
     * ring full; all other failures MUST also mean nothing was published.
     * Apply DMA/cache synchronization and a release barrier before publishing
     * the producer index. A failure after publication requires endpoint reset,
     * never a retry of the same buffer. No callback may re-enter this core.
     */
    swifi_result (*submit)(void *context, uint16_t ring,
                           const uint8_t *item, size_t length);
    void (*tx_complete)(void *context, const swifi_packet_completion *packet);
    void (*rx_complete)(void *context, const swifi_packet_completion *packet);
    void (*flow_changed)(void *context, uint16_t ring,
                         swifi_flow_state state, int16_t firmware_status);
} swifi_data_ops;

typedef struct swifi_packet_slot {
    void *cookie;
    uint32_t request_id;
    uint16_t generation;
    uint16_t capacity;
    uint16_t flow_ring;
    uint8_t ifidx;
    uint8_t owned;
} swifi_packet_slot;

typedef struct swifi_flow {
    uint32_t outstanding;
    uint8_t ifidx;
    uint8_t fifo;
    swifi_flow_state state;
} swifi_flow;

typedef struct swifi_data {
    swifi_data_ops ops;
    void *context;
    swifi_flow *flows;
    swifi_packet_slot *tx;
    swifi_packet_slot *rx;
    uint16_t flow_count;
    uint16_t tx_count;
    uint16_t rx_count;
    uint16_t tx_cursor;
    uint16_t rx_cursor;
    uint16_t rx_data_offset;
    uint8_t stopped;
} swifi_data;

/*
 * All tables and callbacks are caller-owned, nonpaged and device-lifetime.
 * Serialize calls with the same device/queue lock, including reset. There is
 * no allocation, sleeping, polling or OS dependency in this component.
 * Init is only valid before firmware can DMA into these tables/buffers.
 */
swifi_result swifi_data_init(swifi_data *data, const swifi_data_ops *ops,
    void *context, swifi_flow *flows, uint16_t flow_count,
    swifi_packet_slot *tx, uint16_t tx_count,
    swifi_packet_slot *rx, uint16_t rx_count, uint16_t rx_data_offset);

/* Returns UINT8_MAX for a priority outside the eight WMM user priorities. */
uint8_t swifi_priority_fifo(uint8_t priority);

/* ring is the firmware H2D ring number (2 + zero-based flow index). The
 * caller zeroes ring storage AND both indices before creating/reusing it.
 * Keep that allocation until CLOSED is reported or DMA is stopped/reset.
 * priority is the original 802.1D priority (0..7); the firmware FIFO mapping
 * is 0/1/1/0/2/2/3/3. Multiple priorities in that FIFO may share the ring. */
swifi_result swifi_data_create_flow(swifi_data *data, uint16_t ring,
    uint8_t ifidx, uint8_t priority, const uint8_t source[6],
    const uint8_t destination[6], uint64_t ring_dma, uint16_t depth);
swifi_result swifi_data_delete_flow(swifi_data *data, uint16_t ring);

/* payload_dma addresses bytes AFTER the 14-byte Ethernet header. The header
 * is copied into the descriptor; the caller retains the mapped payload and
 * cookie until exactly one completion or cancellation callback. priority is
 * per-packet and must map to the same firmware FIFO as the selected ring. */
swifi_result swifi_data_tx(swifi_data *data, uint16_t ring, uint8_t priority,
    const uint8_t header[SWIFI_ETH_HEADER_SIZE], uint64_t payload_dma,
    uint16_t payload_length, void *cookie);
/* v5 TXPOST flag bits 3:2 contain the per-packet cipher exemption (0..2). */
swifi_result swifi_data_tx_with_exemption(swifi_data *data, uint16_t ring, uint8_t priority,
    uint8_t exemption, const uint8_t header[SWIFI_ETH_HEADER_SIZE], uint64_t payload_dma,
    uint16_t payload_length, void *cookie);
swifi_result swifi_data_post_rx(swifi_data *data, uint64_t buffer_dma,
    uint16_t capacity, void *cookie);

/* Call after an acquire barrier/cache synchronization. item points to the
 * common message header, after the firmware's completion-ring prefix. The
 * caller drains/advances the D2H ring even for an unhandled control message.
 * IOCTL/event completions return UNHANDLED for the control-path dispatcher. */
swifi_result swifi_data_complete(swifi_data *data,
    const uint8_t *item, size_t length);

/* Only after stopping bus mastering and resetting/quiescing the endpoint.
 * Cancels remaining owned packets exactly once, then closes all flow rings.
 * This function itself does NOT stop hardware or free any DMA allocation. */
void swifi_data_stopped_after_reset(swifi_data *data);

#endif
