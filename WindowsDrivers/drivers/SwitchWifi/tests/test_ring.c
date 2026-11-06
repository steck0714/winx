/* SPDX-License-Identifier: ISC */
#include "../core/msgbuf_ring.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct model_ring {
    swifi_ring ring;
    uint8_t memory[4 * 48];
    uint16_t peer, host;
    unsigned barriers, last_publish, publications;
    int overwrite_released;
} model_ring;

static uint32_t read_peer(void *context)
{
    return ((model_ring *)context)->peer;
}

static void barrier(void *context)
{
    ++((model_ring *)context)->barriers;
}

static void write_host(void *context, uint16_t value)
{
    model_ring *m = context;
    assert(m->barriers > m->last_publish);
    assert(value == (m->host + 1) % m->ring.depth);
    if (m->overwrite_released)
        memset(m->memory + m->host * m->ring.item_size, 0xcc, m->ring.item_size);
    m->host = value;
    m->last_publish = m->barriers;
    ++m->publications;
}

static void ring_init(model_ring *m, uint16_t size, int h2d, uint8_t prefix)
{
    static const swifi_ring_ops ops = {read_peer, write_host, barrier};
    memset(m, 0, sizeof(*m));
    assert(swifi_ring_init(&m->ring, &ops, m, m->memory, sizeof(m->memory),
        4, size, h2d, prefix) == SWIFI_OK);
}

static void device_read(model_ring *m, uint8_t *item)
{
    assert(m->ring.host_produces && m->peer != m->host);
    memcpy(item, m->memory + m->peer * m->ring.item_size, m->ring.item_size);
    m->peer = (uint16_t)((m->peer + 1) % m->ring.depth);
}

static void device_write(model_ring *m, const uint8_t *item)
{
    uint16_t next = (uint16_t)((m->peer + 1) % m->ring.depth);
    assert(!m->ring.host_produces && next != m->host);
    memcpy(m->memory + m->peer * m->ring.item_size, item, m->ring.item_size);
    m->peer = next;
}

typedef struct consumer {
    unsigned received;
    model_ring *inject;
    swifi_result result;
} consumer;

static swifi_result consume(void *context, const uint8_t *p, size_t length)
{
    consumer *c = context;
    assert(length == 20 && p[0] == (uint8_t)(c->received + 1));
    ++c->received;
    if (c->inject) {
        uint8_t next[24] = {0};
        next[4] = (uint8_t)(c->received + 1);
        device_write(c->inject, next);
        c->inject = NULL;
    }
    return c->result;
}

static void test_wrap_full_and_invalid_peer(void)
{
    model_ring m;
    uint8_t item[40] = {0}, returned[40];
    ring_init(&m, 40, 1, 0);
    for (unsigned i = 1; i <= 3; ++i) {
        item[0] = (uint8_t)i;
        assert(swifi_ring_submit(&m.ring, item, sizeof(item)) == SWIFI_OK);
    }
    assert(swifi_ring_submit(&m.ring, item, sizeof(item)) == SWIFI_RETRY);
    assert(m.publications == 3 && m.host == 3);
    device_read(&m, returned);
    assert(returned[0] == 1);
    item[0] = 4;
    assert(swifi_ring_submit(&m.ring, item, sizeof(item)) == SWIFI_OK);
    assert(m.host == 0);
    for (unsigned i = 2; i <= 4; ++i) {
        device_read(&m, returned);
        assert(returned[0] == i);
    }
    m.peer = 4;
    assert(swifi_ring_submit(&m.ring, item, sizeof(item)) == SWIFI_PROTOCOL_ERROR);
    assert(m.publications == 4 && m.host == 0);
    m.peer = 0;
    assert(swifi_ring_submit(&m.ring, item, sizeof(item) - 1) == SWIFI_INVALID);
}

static void test_drain_budget_prefix_and_arrival(void)
{
    model_ring m;
    consumer c = {0};
    uint8_t item[24] = {0};
    uint16_t processed;
    int more;
    ring_init(&m, 24, 0, 4);
    m.overwrite_released = 1;
    for (unsigned i = 1; i <= 3; ++i) {
        item[4] = (uint8_t)i;
        device_write(&m, item);
    }
    assert(swifi_ring_drain(&m.ring, 2, consume, &c, &processed, &more) == SWIFI_OK);
    assert(processed == 2 && more && c.received == 2);
    assert(swifi_ring_drain(&m.ring, 2, consume, &c, &processed, &more) == SWIFI_OK);
    assert(processed == 1 && !more && c.received == 3);
    item[4] = 4;
    device_write(&m, item);
    c.inject = &m;
    assert(swifi_ring_drain(&m.ring, 2, consume, &c, &processed, &more) == SWIFI_OK);
    assert(processed == 1 && more && m.host == 0);
    c.result = SWIFI_UNHANDLED;
    assert(swifi_ring_drain(&m.ring, 2, consume, &c, &processed, &more) == SWIFI_OK);
    assert(processed == 1 && !more && c.received == 5);
    item[4] = 6;
    device_write(&m, item);
    c.result = SWIFI_PROTOCOL_ERROR;
    assert(swifi_ring_drain(&m.ring, 2, consume, &c, &processed, &more) == SWIFI_PROTOCOL_ERROR);
    assert(processed == 1 && m.peer == m.host);
    m.peer = 4;
    assert(swifi_ring_drain(&m.ring, 2, consume, &c, &processed, &more) == SWIFI_PROTOCOL_ERROR);
    assert(processed == 0);
}

typedef struct transport_model {
    model_ring h2d[3], d2h[3];
    swifi_data data;
    swifi_flow flow;
    swifi_packet_slot tx[4], rx[4];
    uint8_t received_frame[2048];
    unsigned tx_done, rx_done, opened;
} transport_model;

static swifi_result submit(void *context, uint16_t ring, const uint8_t *item, size_t size)
{
    transport_model *m = context;
    assert(ring < 3);
    return swifi_ring_submit(&m->h2d[ring].ring, item, size);
}

static void tx_done(void *context, const swifi_packet_completion *p)
{
    transport_model *m = context;
    assert(p->cookie == &m->tx_done && p->reason == SWIFI_COMPLETED && p->length == 64);
    ++m->tx_done;
}

static void rx_done(void *context, const swifi_packet_completion *p)
{
    transport_model *m = context;
    assert(p->cookie == m->received_frame && p->reason == SWIFI_COMPLETED);
    assert(p->offset == 8 && p->length == 64);
    for (unsigned i = 0; i < p->length; ++i)
        assert(m->received_frame[p->offset + i] == (uint8_t)i);
    ++m->rx_done;
}

static void flow_done(void *context, uint16_t ring, swifi_flow_state state, int16_t status)
{
    transport_model *m = context;
    assert(ring == 2 && state == SWIFI_FLOW_OPEN && status == 0);
    ++m->opened;
}

static swifi_result dispatch(void *context, const uint8_t *item, size_t size)
{
    return swifi_data_complete(&((transport_model *)context)->data, item, size);
}

static void drain(transport_model *m, unsigned ring)
{
    uint16_t processed;
    int more;
    assert(swifi_ring_drain(&m->d2h[ring].ring, 32, dispatch, m,
        &processed, &more) == SWIFI_OK);
    assert(processed == 1 && !more);
}

static void test_packet_core_over_real_rings(void)
{
    static const swifi_data_ops ops = {submit, tx_done, rx_done, flow_done};
    const uint8_t src[6] = {2, 0, 0, 0, 0, 1}, dst[6] = {2, 0, 0, 0, 0, 2};
    uint8_t header[14] = {0}, request[48], reply[32] = {0};
    transport_model m;
    memset(&m, 0, sizeof(m));
    ring_init(&m.h2d[0], 40, 1, 0);
    ring_init(&m.h2d[1], 32, 1, 0);
    ring_init(&m.h2d[2], 48, 1, 0);
    ring_init(&m.d2h[0], 24, 0, 0);
    ring_init(&m.d2h[1], 16, 0, 0);
    ring_init(&m.d2h[2], 32, 0, 0);
    assert(swifi_data_init(&m.data, &ops, &m, &m.flow, 1, m.tx, 4, m.rx, 4, 8) == SWIFI_OK);
    assert(swifi_data_create_flow(&m.data, 2, 0, 0, src, dst, 0x80000000, 4) == SWIFI_OK);
    device_read(&m.h2d[0], request);
    assert(request[0] == 3 && request[22] == 2 && request[28] == 4 && request[30] == 48);
    reply[0] = 4; reply[10] = 2;
    device_write(&m.d2h[0], reply);
    drain(&m, 0);
    assert(m.opened == 1);
    memcpy(header, dst, 6); memcpy(header + 6, src, 6); header[12] = 8;
    for (unsigned i = 0; i < 12; ++i) {
        assert(swifi_data_tx(&m.data, 2, 0, header, 0x80100000, 50, &m.tx_done) == SWIFI_OK);
        assert(swifi_data_post_rx(&m.data, 0x80200000, 2048, m.received_frame) == SWIFI_OK);
        device_read(&m.h2d[2], request);
        assert(request[0] == 0x0f && !memcmp(request + 8, header, 14));
        assert(request[42] == 50 && request[34] == 0x10 && request[35] == 0x80);
        memset(reply, 0, sizeof(reply));
        reply[0] = 0x10; reply[10] = 2; memcpy(reply + 4, request + 4, 4);
        device_write(&m.d2h[1], reply);
        device_read(&m.h2d[1], request);
        assert(request[0] == 0x11 && request[26] == 0x20 && request[27] == 0x80);
        for (unsigned n = 0; n < 64; ++n) m.received_frame[n + 8] = (uint8_t)n;
        memset(reply, 0, sizeof(reply));
        reply[0] = 0x12; reply[14] = 64; memcpy(reply + 4, request + 4, 4);
        device_write(&m.d2h[2], reply);
        drain(&m, 2); drain(&m, 1);
    }
    assert(m.tx_done == 12 && m.rx_done == 12 && !m.flow.outstanding);
}

int main(void)
{
    test_wrap_full_and_invalid_peer();
    test_drain_budget_prefix_and_arrival();
    test_packet_core_over_real_rings();
    puts("msgbuf rings: wrap/full, bounded drain, prefix, rearm arrivals and packet integration passed");
    return 0;
}
