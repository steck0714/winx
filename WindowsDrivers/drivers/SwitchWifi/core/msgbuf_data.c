/* SPDX-License-Identifier: ISC */
/* Host packet ownership and msgbuf-v5 wire encoding, adapted for Windows.
 * Wire formats and sequencing: Broadcom brcmfmac (2014), see NOTICE.
 * Windows-port implementation: 2026 Switch Lite contributors. */
#include "msgbuf_data.h"
#include <string.h>

/* brcmfmac groups 802.1D priorities by access category for flow creation.
 * TX descriptors still carry each packet's original 0..7 priority. */
static const uint8_t priority_fifo[8] = {0, 1, 1, 0, 2, 2, 3, 3};

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void put_address(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/* The initial T210/T214 AFI transport is limited to the 32-bit DMA aperture. */
static int dma_range(uint64_t address, uint32_t length)
{
    return length && address <= UINT32_MAX &&
        (uint64_t)(length - 1) <= UINT32_MAX - address;
}

static swifi_flow *flow_at(swifi_data *d, uint16_t ring)
{
    if (ring < SWIFI_FIRST_FLOW_RING ||
        ring - SWIFI_FIRST_FLOW_RING >= d->flow_count)
        return NULL;
    return &d->flows[ring - SWIFI_FIRST_FLOW_RING];
}

/* Request IDs are opaque to firmware. Low 16 bits address the table directly;
 * high 16 bits distinguish a late/duplicate completion from a reused slot. */
static swifi_packet_slot *reserve_slot(swifi_packet_slot *slots,
    uint16_t count, uint16_t *cursor, void *cookie)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        uint16_t index = *cursor;
        swifi_packet_slot *slot = &slots[index];
        *cursor = (uint16_t)((index + 1u) % count);
        if (slot->owned)
            continue;
        if (++slot->generation == 0)
            ++slot->generation;
        slot->request_id = ((uint32_t)slot->generation << 16) | (index + 1u);
        slot->cookie = cookie;
        slot->owned = 1;
        return slot;
    }
    return NULL;
}

static swifi_packet_slot *find_slot(swifi_packet_slot *slots,
    uint16_t count, uint32_t id)
{
    uint32_t index = id & 0xffffu;
    swifi_packet_slot *slot;
    if (!index || index > count)
        return NULL;
    slot = &slots[index - 1];
    if (!slot->owned || slot->request_id != id)
        return NULL;
    return slot;
}

static void release_slot(swifi_packet_slot *slot)
{
    slot->owned = 0;
    slot->cookie = NULL;
}

swifi_result swifi_data_init(swifi_data *d, const swifi_data_ops *ops,
    void *context, swifi_flow *flows, uint16_t flow_count,
    swifi_packet_slot *tx, uint16_t tx_count,
    swifi_packet_slot *rx, uint16_t rx_count, uint16_t rx_data_offset)
{
    if (!d || !ops || !ops->submit || !ops->tx_complete || !ops->rx_complete ||
        !ops->flow_changed || !flows || !flow_count ||
        flow_count > UINT16_MAX - SWIFI_FIRST_FLOW_RING + 1u ||
        !tx || !tx_count || !rx || !rx_count ||
        rx_data_offset > SWIFI_PACKET_SIZE - SWIFI_ETH_HEADER_SIZE)
        return SWIFI_INVALID;
    memset(d, 0, sizeof(*d));
    memset(flows, 0, sizeof(*flows) * flow_count);
    memset(tx, 0, sizeof(*tx) * tx_count);
    memset(rx, 0, sizeof(*rx) * rx_count);
    d->ops = *ops;
    d->context = context;
    d->flows = flows;
    d->flow_count = flow_count;
    d->tx = tx;
    d->tx_count = tx_count;
    d->rx = rx;
    d->rx_count = rx_count;
    d->rx_data_offset = rx_data_offset;
    return SWIFI_OK;
}

swifi_result swifi_data_create_flow(swifi_data *d, uint16_t ring,
    uint8_t ifidx, uint8_t priority, const uint8_t source[6],
    const uint8_t destination[6], uint64_t ring_dma, uint16_t depth)
{
    uint8_t item[SWIFI_CONTROL_ITEM_SIZE] = {0};
    swifi_flow *flow = flow_at(d, ring);
    swifi_result result;
    if (d->stopped)
        return SWIFI_STOPPED;
    if (!flow || !source || !destination || priority > 7 || depth < 2 ||
        (ring_dma & 7) || !dma_range(ring_dma, (uint32_t)depth * SWIFI_TX_ITEM_SIZE))
        return SWIFI_INVALID;
    if (flow->state != SWIFI_FLOW_CLOSED)
        return SWIFI_RETRY;
    item[0] = 3; /* FLOW_RING_CREATE */
    item[1] = ifidx;
    memcpy(item + 8, destination, 6);
    memcpy(item + 14, source, 6);
    item[20] = priority_fifo[priority];
    put16(item + 22, ring);
    put16(item + 28, depth);
    put16(item + 30, SWIFI_TX_ITEM_SIZE);
    put_address(item + 32, ring_dma);
    flow->ifidx = ifidx;
    flow->fifo = priority_fifo[priority];
    flow->state = SWIFI_FLOW_CREATING;
    result = d->ops.submit(d->context, SWIFI_CONTROL_RING, item, sizeof(item));
    if (result != SWIFI_OK)
        flow->state = SWIFI_FLOW_CLOSED;
    return result;
}

swifi_result swifi_data_delete_flow(swifi_data *d, uint16_t ring)
{
    uint8_t item[SWIFI_CONTROL_ITEM_SIZE] = {0};
    swifi_flow *flow = flow_at(d, ring);
    swifi_result result;
    if (d->stopped)
        return SWIFI_STOPPED;
    if (!flow)
        return SWIFI_INVALID;
    if (flow->state != SWIFI_FLOW_OPEN || flow->outstanding)
        return SWIFI_RETRY;
    item[0] = 5; /* FLOW_RING_DELETE */
    item[1] = flow->ifidx;
    put16(item + 8, ring);
    flow->state = SWIFI_FLOW_DELETING;
    result = d->ops.submit(d->context, SWIFI_CONTROL_RING, item, sizeof(item));
    if (result != SWIFI_OK)
        flow->state = SWIFI_FLOW_OPEN;
    return result;
}

swifi_result swifi_data_tx(swifi_data *d, uint16_t ring, uint8_t priority,
    const uint8_t header[SWIFI_ETH_HEADER_SIZE], uint64_t payload_dma,
    uint16_t payload_length, void *cookie)
{
    uint8_t item[SWIFI_TX_ITEM_SIZE] = {0};
    swifi_flow *flow = flow_at(d, ring);
    swifi_packet_slot *slot;
    swifi_result result;
    if (d->stopped)
        return SWIFI_STOPPED;
    if (!flow || !header || !cookie || priority > 7 ||
        payload_length > SWIFI_PACKET_SIZE - SWIFI_ETH_HEADER_SIZE ||
        !dma_range(payload_dma, payload_length))
        return SWIFI_INVALID;
    if (flow->state != SWIFI_FLOW_OPEN)
        return SWIFI_RETRY;
    if (priority_fifo[priority] != flow->fifo)
        return SWIFI_INVALID;
    slot = reserve_slot(d->tx, d->tx_count, &d->tx_cursor, cookie);
    if (!slot)
        return SWIFI_RETRY;
    slot->flow_ring = ring;
    slot->ifidx = flow->ifidx;
    slot->capacity = payload_length;
    item[0] = 0x0f; /* TX_POST */
    item[1] = flow->ifidx;
    put32(item + 4, slot->request_id);
    memcpy(item + 8, header, SWIFI_ETH_HEADER_SIZE);
    item[22] = (uint8_t)(1u | (priority << 5)); /* 802.3, priority */
    item[23] = 1; /* One DMA payload segment. */
    put_address(item + 32, payload_dma);
    put16(item + 42, payload_length);
    ++flow->outstanding;
    result = d->ops.submit(d->context, ring, item, sizeof(item));
    if (result != SWIFI_OK) {
        --flow->outstanding;
        release_slot(slot);
    }
    return result;
}

swifi_result swifi_data_post_rx(swifi_data *d, uint64_t buffer_dma,
    uint16_t capacity, void *cookie)
{
    uint8_t item[SWIFI_RXPOST_ITEM_SIZE] = {0};
    swifi_packet_slot *slot;
    swifi_result result;
    if (d->stopped)
        return SWIFI_STOPPED;
    if (!cookie || capacity < d->rx_data_offset + SWIFI_ETH_HEADER_SIZE ||
        capacity > SWIFI_PACKET_SIZE || !dma_range(buffer_dma, capacity))
        return SWIFI_INVALID;
    slot = reserve_slot(d->rx, d->rx_count, &d->rx_cursor, cookie);
    if (!slot)
        return SWIFI_RETRY;
    slot->capacity = capacity;
    slot->flow_ring = 0;
    slot->ifidx = 0;
    item[0] = 0x11; /* RXBUF_POST; metadata buffer is unused. */
    put32(item + 4, slot->request_id);
    put16(item + 10, capacity);
    put_address(item + 24, buffer_dma);
    result = d->ops.submit(d->context, SWIFI_RXPOST_RING, item, sizeof(item));
    if (result != SWIFI_OK)
        release_slot(slot);
    return result;
}

swifi_result swifi_data_complete(swifi_data *d, const uint8_t *p, size_t length)
{
    swifi_packet_completion result = {0};
    swifi_packet_slot *slot;
    swifi_flow *flow;
    uint16_t ring;
    int16_t status;
    if (d->stopped)
        return SWIFI_STOPPED;
    if (!p || length < 8)
        return SWIFI_PROTOCOL_ERROR;
    if (p[0] != 4 && p[0] != 6 && p[0] != 0x10 && p[0] != 0x12)
        return SWIFI_UNHANDLED;
    if (length < (p[0] == 0x12 ? 32u : p[0] == 0x10 ? 16u : 24u))
        return SWIFI_PROTOCOL_ERROR;
    ring = get16(p + 10);
    status = (int16_t)get16(p + 8);
    if (p[0] == 4 || p[0] == 6) {
        flow = flow_at(d, ring);
        if (!flow || (p[0] == 4 && flow->state != SWIFI_FLOW_CREATING) ||
            (p[0] == 6 && (flow->state != SWIFI_FLOW_DELETING || flow->outstanding)))
            return SWIFI_PROTOCOL_ERROR;
        if (p[0] == 4)
            flow->state = status ? SWIFI_FLOW_CLOSED : SWIFI_FLOW_OPEN;
        else
            flow->state = status ? SWIFI_FLOW_OPEN : SWIFI_FLOW_CLOSED;
        d->ops.flow_changed(d->context, ring, flow->state, status);
        return SWIFI_OK;
    }
    result.request_id = get32(p + 4);
    result.firmware_status = status;
    result.ifidx = p[1];
    result.reason = status ? SWIFI_FIRMWARE_ERROR : SWIFI_COMPLETED;
    if (p[0] == 0x10) {
        slot = find_slot(d->tx, d->tx_count, result.request_id);
        flow = flow_at(d, ring);
        if (!slot || !flow || slot->flow_ring != ring ||
            slot->ifidx != p[1] || !flow->outstanding)
            return SWIFI_PROTOCOL_ERROR;
        result.cookie = slot->cookie;
        result.flow_ring = ring;
        result.tx_status = get16(p + 14);
        result.length = (uint16_t)(slot->capacity + SWIFI_ETH_HEADER_SIZE);
        --flow->outstanding;
        release_slot(slot);
        d->ops.tx_complete(d->context, &result);
    } else {
        slot = find_slot(d->rx, d->rx_count, result.request_id);
        if (!slot)
            return SWIFI_PROTOCOL_ERROR;
        result.cookie = slot->cookie;
        result.offset = get16(p + 16);
        if (!result.offset)
            result.offset = d->rx_data_offset;
        result.length = get16(p + 14);
        if (status) {
            result.length = 0;
            result.offset = 0;
        } else if (result.offset > slot->capacity ||
            result.length > slot->capacity - result.offset ||
            result.length < SWIFI_ETH_HEADER_SIZE) {
            result.reason = SWIFI_BAD_PACKET;
            result.length = 0;
            result.offset = 0;
        }
        release_slot(slot);
        d->ops.rx_complete(d->context, &result);
        if (result.reason == SWIFI_BAD_PACKET)
            return SWIFI_PROTOCOL_ERROR;
    }
    return SWIFI_OK;
}

void swifi_data_stopped_after_reset(swifi_data *d)
{
    uint32_t i;
    swifi_packet_completion result;
    if (d->stopped)
        return;
    d->stopped = 1;
    for (i = 0; i < d->tx_count + (uint32_t)d->rx_count; ++i) {
        int is_tx = i < d->tx_count;
        swifi_packet_slot *slot = is_tx ? &d->tx[i] : &d->rx[i - d->tx_count];
        if (!slot->owned)
            continue;
        memset(&result, 0, sizeof(result));
        result.cookie = slot->cookie;
        result.request_id = slot->request_id;
        result.flow_ring = slot->flow_ring;
        result.ifidx = slot->ifidx;
        result.reason = SWIFI_CANCELLED;
        release_slot(slot);
        if (is_tx)
            d->ops.tx_complete(d->context, &result);
        else
            d->ops.rx_complete(d->context, &result);
    }
    for (i = 0; i < d->flow_count; ++i) {
        swifi_flow *flow = &d->flows[i];
        flow->outstanding = 0;
        if (flow->state == SWIFI_FLOW_CLOSED)
            continue;
        flow->state = SWIFI_FLOW_CLOSED;
        d->ops.flow_changed(d->context, (uint16_t)(i + SWIFI_FIRST_FLOW_RING),
            SWIFI_FLOW_CLOSED, 0);
    }
}
