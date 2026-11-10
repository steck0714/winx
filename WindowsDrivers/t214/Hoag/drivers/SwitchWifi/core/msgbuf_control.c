/* SPDX-License-Identifier: ISC */
#include "msgbuf_control.h"
#include <string.h>

#define IOCTL_REQUEST 0x09u
#define IOCTL_ACK 0x0au
#define IOCTL_BUFFER 0x0bu
#define IOCTL_COMPLETE 0x0cu
#define EVENT_BUFFER 0x0du
#define EVENT_COMPLETE 0x0eu
#define IOCTL_REQUEST_ID 0xfffeu

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int valid_buffer(swifi_control_buffer buffer)
{
    return buffer.memory && buffer.dma &&
        buffer.dma <= UINT32_MAX - (SWIFI_CONTROL_BUFFER_SIZE - 1u);
}

static void post_buffer(uint8_t *item, uint8_t type, uint32_t request_id,
    swifi_control_buffer buffer)
{
    memset(item, 0, SWIFI_CONTROL_ITEM_SIZE);
    item[0] = type;
    put32(item + 4, request_id);
    put16(item + 8, SWIFI_CONTROL_BUFFER_SIZE);
    put32(item + 16, (uint32_t)buffer.dma);
}

swifi_result swifi_control_init(swifi_control *c,
    const swifi_control_ops *ops, void *context, swifi_control_buffer input,
    swifi_control_buffer response,
    const swifi_control_buffer events[SWIFI_EVENT_COUNT], uint16_t rx_data_offset)
{
    unsigned i;
    if (!c || !ops || !ops->submit || !ops->command_complete || !ops->event_ready ||
        !valid_buffer(input) || !valid_buffer(response) || !events ||
        rx_data_offset >= SWIFI_CONTROL_BUFFER_SIZE)
        return SWIFI_INVALID;
    for (i = 0; i < SWIFI_EVENT_COUNT; ++i)
        if (!valid_buffer(events[i]))
            return SWIFI_INVALID;
    memset(c, 0, sizeof(*c));
    c->ops = *ops;
    c->context = context;
    c->input = input;
    c->response = response;
    c->rx_data_offset = rx_data_offset;
    for (i = 0; i < SWIFI_EVENT_COUNT; ++i)
        c->events[i].buffer = events[i];
    return SWIFI_OK;
}

swifi_result swifi_control_command(swifi_control *c, uint8_t ifidx,
    uint32_t command, const void *input, uint16_t input_length,
    uint16_t output_length, void *cookie, uint32_t *request_id)
{
    uint8_t items[2 * SWIFI_CONTROL_ITEM_SIZE];
    uint8_t *request = items + SWIFI_CONTROL_ITEM_SIZE;
    uint32_t sequence, id;
    swifi_result result;
    if (!request_id || (input_length && !input) ||
        input_length > SWIFI_CONTROL_BUFFER_SIZE || output_length > SWIFI_CONTROL_BUFFER_SIZE)
        return SWIFI_INVALID;
    if (c->stopped)
        return SWIFI_STOPPED;
    if (c->pending)
        return SWIFI_RETRY;
    sequence = c->sequence + 1;
    id = 0x40000000u | (sequence & 0x3fffffffu);
    if (input_length)
        memmove(c->input.memory, input, input_length);
    memset(c->input.memory + input_length, 0, SWIFI_CONTROL_BUFFER_SIZE - input_length);
    memset(c->response.memory, 0, SWIFI_CONTROL_BUFFER_SIZE);
    post_buffer(items, IOCTL_BUFFER, id, c->response);
    memset(request, 0, SWIFI_CONTROL_ITEM_SIZE);
    request[0] = IOCTL_REQUEST;
    request[1] = ifidx;
    put32(request + 4, IOCTL_REQUEST_ID);
    put32(request + 8, command);
    put16(request + 12, (uint16_t)sequence);
    put16(request + 14, input_length);
    put16(request + 16, output_length);
    put32(request + 24, (uint32_t)c->input.dma);
    result = c->ops.submit(c->context, items, 2);
    if (result != SWIFI_OK)
        return result;
    c->sequence = sequence;
    c->request_id = id;
    c->transaction = (uint16_t)sequence;
    c->command = command;
    c->ifidx = ifidx;
    c->output_length = output_length;
    c->cookie = cookie;
    c->pending = 1;
    *request_id = id;
    return SWIFI_OK;
}

swifi_result swifi_control_refill_events(swifi_control *c)
{
    unsigned i;
    uint8_t item[SWIFI_CONTROL_ITEM_SIZE];
    swifi_result result;
    if (c->stopped)
        return SWIFI_STOPPED;
    for (i = 0; i < SWIFI_EVENT_COUNT; ++i) {
        swifi_event_slot *slot = &c->events[i];
        uint32_t generation, id;
        if (slot->state != SWIFI_EVENT_AVAILABLE)
            continue;
        generation = (slot->generation + 1u) & 0x7fffffu;
        id = 0x80000000u | (generation << 8) | i;
        post_buffer(item, EVENT_BUFFER, id, slot->buffer);
        result = c->ops.submit(c->context, item, 1);
        if (result != SWIFI_OK)
            return result;
        slot->generation = generation;
        slot->request_id = id;
        slot->state = SWIFI_EVENT_POSTED;
    }
    return SWIFI_OK;
}

swifi_result swifi_control_release_event(swifi_control *c, uint32_t request_id)
{
    unsigned i = request_id & 0xffu;
    swifi_event_slot *slot;
    if (i >= SWIFI_EVENT_COUNT)
        return SWIFI_INVALID;
    slot = &c->events[i];
    if (slot->state != SWIFI_EVENT_DELIVERED || slot->request_id != request_id)
        return SWIFI_INVALID;
    slot->state = SWIFI_EVENT_AVAILABLE;
    return SWIFI_OK;
}

static void finish_command(swifi_control *c, swifi_command_reason reason,
    int16_t status, uint16_t length)
{
    swifi_command_completion reply;
    memset(&reply, 0, sizeof(reply));
    reply.cookie = c->cookie;
    reply.command = c->command;
    reply.request_id = c->request_id;
    reply.ifidx = c->ifidx;
    reply.reason = reason;
    reply.firmware_status = status;
    reply.returned_length = length;
    reply.length = length < c->output_length ? length : c->output_length;
    if (length)
        reply.response = c->response.memory;
    c->pending = 0;
    c->cookie = NULL;
    c->ops.command_complete(c->context, &reply);
}

swifi_result swifi_control_complete(swifi_control *c, const uint8_t *item, size_t length)
{
    uint32_t id;
    uint16_t size;
    int16_t status;
    if (!item || length < 8)
        return SWIFI_PROTOCOL_ERROR;
    if (item[0] != IOCTL_ACK && item[0] != IOCTL_COMPLETE && item[0] != EVENT_COMPLETE)
        return SWIFI_UNHANDLED;
    if (c->stopped)
        return SWIFI_STOPPED;
    if (item[0] == IOCTL_ACK) {
        /* ACK does not release the input/response allocation. Only the
         * matching completion or a hardware reset ends DMA ownership.
         * Like brcmfmac, do not assign semantics to unused ACK fields. */
        return SWIFI_OK;
    }
    if (length < 16)
        return SWIFI_PROTOCOL_ERROR;
    id = get32(item + 4);
    status = (int16_t)get16(item + 8);
    size = get16(item + 12);
    if (item[0] == IOCTL_COMPLETE) {
        swifi_command_reason reason;
        if (length < 20 || !c->pending || id != c->request_id ||
            get16(item + 14) != c->transaction || get32(item + 16) != c->command ||
            size > SWIFI_CONTROL_BUFFER_SIZE)
            return SWIFI_PROTOCOL_ERROR;
        reason = status ? SWIFI_COMMAND_FIRMWARE_ERROR :
            size > c->output_length ? SWIFI_COMMAND_TRUNCATED : SWIFI_COMMAND_COMPLETED;
        finish_command(c, reason, status, size);
    } else {
        swifi_event_completion event;
        unsigned i = id & 0xffu;
        swifi_event_slot *slot;
        if (i >= SWIFI_EVENT_COUNT || size > SWIFI_CONTROL_BUFFER_SIZE - c->rx_data_offset)
            return SWIFI_PROTOCOL_ERROR;
        slot = &c->events[i];
        if (slot->state != SWIFI_EVENT_POSTED || id != slot->request_id)
            return SWIFI_PROTOCOL_ERROR;
        memset(&event, 0, sizeof(event));
        event.request_id = id;
        event.length = status ? 0 : size;
        event.data = slot->buffer.memory + c->rx_data_offset;
        event.sequence = get16(item + 14);
        event.firmware_status = status;
        event.ifidx = item[1];
        slot->state = SWIFI_EVENT_DELIVERED;
        c->ops.event_ready(c->context, &event);
    }
    return SWIFI_OK;
}

swifi_result swifi_control_timeout(swifi_control *c, uint32_t request_id)
{
    if (!c->pending || c->request_id != request_id)
        return SWIFI_UNHANDLED; /* Deadline for an already completed command. */
    c->stopped = 1;
    finish_command(c, SWIFI_COMMAND_TIMED_OUT, 0, 0);
    return SWIFI_OK;
}

void swifi_control_stopped_after_reset(swifi_control *c)
{
    unsigned i;
    c->stopped = 1;
    if (c->pending)
        finish_command(c, SWIFI_COMMAND_CANCELLED, 0, 0);
    for (i = 0; i < SWIFI_EVENT_COUNT; ++i)
        if (c->events[i].state == SWIFI_EVENT_POSTED)
            c->events[i].state = SWIFI_EVENT_AVAILABLE;
}
