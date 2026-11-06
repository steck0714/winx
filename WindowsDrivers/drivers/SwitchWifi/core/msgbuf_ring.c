/* SPDX-License-Identifier: ISC */
#include "msgbuf_ring.h"
#include <string.h>

swifi_result swifi_ring_init(swifi_ring *r, const swifi_ring_ops *ops,
    void *context, void *memory, size_t memory_length, uint16_t depth,
    uint16_t item_size, int host_produces, uint8_t prefix)
{
    if (!r || !ops || !ops->read_peer || !ops->write_host || !ops->barrier ||
        !memory || depth < 2 || item_size < 8 || item_size > SWIFI_TX_ITEM_SIZE ||
        prefix > item_size - 8 || (host_produces && prefix) ||
        (size_t)depth * item_size > memory_length)
        return SWIFI_INVALID;
    memset(r, 0, sizeof(*r));
    r->memory = memory;
    r->ops = *ops;
    r->context = context;
    r->depth = depth;
    r->item_size = item_size;
    r->host_produces = host_produces != 0;
    r->prefix = prefix;
    if (r->ops.read_peer(context) != 0)
        return SWIFI_PROTOCOL_ERROR;
    r->ops.barrier(context);
    return SWIFI_OK;
}

swifi_result swifi_ring_submit(swifi_ring *r, const uint8_t *item, size_t length)
{
    return swifi_ring_submit_batch(r, item, length, 1);
}

swifi_result swifi_ring_submit_batch(swifi_ring *r, const uint8_t *items,
    size_t length, uint16_t count)
{
    uint32_t peer, free_slots;
    uint16_t next, i;
    if (!r || !r->host_produces || !items || !count || count >= r->depth ||
        length != (size_t)count * r->item_size)
        return SWIFI_INVALID;
    peer = r->ops.read_peer(r->context);
    if (peer >= r->depth)
        return SWIFI_PROTOCOL_ERROR;
    r->ops.barrier(r->context);
    free_slots = (peer + r->depth - r->host_index - 1u) % r->depth;
    if (count > free_slots)
        return SWIFI_RETRY;
    next = r->host_index;
    for (i = 0; i < count; ++i) {
        memcpy(r->memory + (size_t)next * r->item_size,
            items + (size_t)i * r->item_size, r->item_size);
        next = (uint16_t)((next + 1u) % r->depth);
    }
    r->ops.barrier(r->context);
    r->host_index = next;
    r->ops.write_host(r->context, next);
    return SWIFI_OK;
}

swifi_result swifi_ring_drain(swifi_ring *r, uint16_t budget,
    swifi_ring_handler handler, void *context, uint16_t *processed, int *more)
{
    uint8_t local[SWIFI_TX_ITEM_SIZE];
    uint32_t peer;
    swifi_result result;
    *processed = 0;
    *more = 0;
    if (r->host_produces || !budget || !handler)
        return SWIFI_INVALID;
    peer = r->ops.read_peer(r->context);
    if (peer >= r->depth)
        return SWIFI_PROTOCOL_ERROR;
    r->ops.barrier(r->context);
    while (r->host_index != peer && *processed < budget) {
        memcpy(local, r->memory + (size_t)r->host_index * r->item_size,
            r->item_size);
        r->host_index = (uint16_t)((r->host_index + 1u) % r->depth);
        ++*processed;
        r->ops.barrier(r->context);
        r->ops.write_host(r->context, r->host_index);
        result = handler(context, local + r->prefix, r->item_size - r->prefix);
        if (result != SWIFI_OK && result != SWIFI_UNHANDLED)
            return result;
    }
    /* Re-read after the snapshot: arrivals during this drain need another
     * DPC even when there will be no new edge while interrupts are masked. */
    peer = r->ops.read_peer(r->context);
    if (peer >= r->depth)
        return SWIFI_PROTOCOL_ERROR;
    *more = peer != r->host_index;
    return SWIFI_OK;
}
