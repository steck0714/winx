/* SPDX-License-Identifier: ISC */
#include "../core/msgbuf_data.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    swifi_data data;
    swifi_flow flows[2];
    swifi_packet_slot tx[2], rx[2];
    uint8_t published[48];
    size_t published_length;
    uint16_t published_ring;
    swifi_result submit_result;
    unsigned publishes, tx_done, rx_done, flow_done;
    unsigned cancelled_tx, cancelled_rx;
    swifi_packet_completion last_tx, last_rx;
    swifi_flow_state last_flow;
    int16_t last_flow_status;
} fixture;

static const uint8_t source[6] = {2, 0, 0, 0, 0, 1};
static const uint8_t destination[6] = {2, 0, 0, 0, 0, 2};
static const uint8_t ethernet[14] = {2, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 1, 8, 0};
static int cookies[8];

static swifi_result submit(void *context, uint16_t ring,
    const uint8_t *item, size_t length)
{
    fixture *f = context;
    if (f->submit_result != SWIFI_OK)
        return f->submit_result;
    assert(length <= sizeof(f->published));
    memset(f->published, 0xff, sizeof(f->published));
    memcpy(f->published, item, length);
    f->published_length = length;
    f->published_ring = ring;
    ++f->publishes;
    return SWIFI_OK;
}

static void tx_complete(void *context, const swifi_packet_completion *p)
{
    fixture *f = context;
    ++f->tx_done;
    f->last_tx = *p;
    if (p->reason == SWIFI_CANCELLED) {
        unsigned bit = 1u << ((int *)p->cookie - cookies);
        assert(!(f->cancelled_tx & bit));
        f->cancelled_tx |= bit;
    }
}

static void rx_complete(void *context, const swifi_packet_completion *p)
{
    fixture *f = context;
    ++f->rx_done;
    f->last_rx = *p;
    if (p->reason == SWIFI_CANCELLED) {
        unsigned bit = 1u << ((int *)p->cookie - cookies);
        assert(!(f->cancelled_rx & bit));
        f->cancelled_rx |= bit;
    }
}

static void flow_changed(void *context, uint16_t ring,
    swifi_flow_state state, int16_t status)
{
    fixture *f = context;
    assert(ring == 2 || ring == 3);
    ++f->flow_done;
    f->last_flow = state;
    f->last_flow_status = status;
}

static void init(fixture *f)
{
    static const swifi_data_ops ops = {submit, tx_complete, rx_complete, flow_changed};
    memset(f, 0, sizeof(*f));
    assert(swifi_data_init(&f->data, &ops, f, f->flows, 2,
        f->tx, 2, f->rx, 2, 8) == SWIFI_OK);
}

static swifi_result flow_reply(fixture *f, uint8_t type, uint16_t ring, uint16_t status)
{
    uint8_t wire[24] = {0};
    wire[0] = type;
    wire[8] = (uint8_t)status;
    wire[9] = (uint8_t)(status >> 8);
    wire[10] = (uint8_t)ring;
    wire[11] = (uint8_t)(ring >> 8);
    return swifi_data_complete(&f->data, wire, sizeof(wire));
}

static void open_flow(fixture *f, uint16_t ring, uint8_t ifidx, uint8_t tid)
{
    assert(swifi_data_create_flow(&f->data, ring, ifidx, tid,
        source, destination, 0x80004000, 512) == SWIFI_OK);
    assert(flow_reply(f, 4, ring, 0) == SWIFI_OK);
    assert(f->last_flow == SWIFI_FLOW_OPEN);
}

static void tx_reply(fixture *f, uint8_t wire[16])
{
    /* Hardware echoes request_id and ifidx, and adds the firmware ring ID. */
    memset(wire, 0, 16);
    wire[0] = 0x10;
    wire[1] = f->published[1];
    memcpy(wire + 4, f->published + 4, 4);
    wire[10] = (uint8_t)f->published_ring;
    wire[11] = (uint8_t)(f->published_ring >> 8);
}

static void rx_reply(fixture *f, uint8_t wire[32], uint16_t offset, uint16_t length)
{
    memset(wire, 0, 32);
    wire[0] = 0x12;
    memcpy(wire + 4, f->published + 4, 4);
    wire[14] = (uint8_t)length;
    wire[15] = (uint8_t)(length >> 8);
    wire[16] = (uint8_t)offset;
    wire[17] = (uint8_t)(offset >> 8);
}

static void test_wire_layout_and_flow_lifetime(void)
{
    fixture f;
    uint8_t reply[16];
    /* Independent golden layouts from brcmfmac's packed wire structures. */
    static const uint8_t create[40] = {
        3, 1, 0, 0, 0, 0, 0, 0,
        2, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 1,
        2, 0, 2, 0, 0, 0, 0, 0, 0, 2, 48, 0,
        0, 0x40, 0, 0x80, 0, 0, 0, 0
    };
    static const uint8_t transmit[48] = {
        0x0f, 1, 0, 0, 1, 0, 1, 0,
        2, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 1, 8, 0,
        0xa9, 1, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0x30, 0x12, 0x80, 0, 0, 0, 0, 0, 0, 0xdc, 5, 0, 0, 0, 0
    };
    init(&f);
    assert(swifi_data_create_flow(&f.data, 2, 1, 5, source, destination,
        0x80004000, 512) == SWIFI_OK);
    assert(f.published_ring == 0 && f.published_length == sizeof(create));
    assert(!memcmp(f.published, create, sizeof(create)));
    assert(swifi_data_tx(&f.data, 2, 5, ethernet, 0x80123000, 1500, &cookies[0]) == SWIFI_RETRY);
    assert(flow_reply(&f, 4, 2, 0) == SWIFI_OK);
    assert(swifi_data_tx_with_exemption(&f.data, 2, 5, 3, ethernet, 0x80123000, 1500, &cookies[0]) == SWIFI_INVALID);
    assert(swifi_data_tx_with_exemption(&f.data, 2, 5, 2, ethernet, 0x80123000, 1500, &cookies[0]) == SWIFI_OK);
    assert(f.published_ring == 2 && f.published_length == sizeof(transmit));
    assert(!memcmp(f.published, transmit, sizeof(transmit)));
    assert(swifi_data_delete_flow(&f.data, 2) == SWIFI_RETRY);
    tx_reply(&f, reply);
    assert(swifi_data_complete(&f.data, reply, sizeof(reply)) == SWIFI_OK);
    assert(f.tx_done == 1 && f.last_tx.cookie == &cookies[0]);
    assert(f.last_tx.length == 1514 && f.last_tx.reason == SWIFI_COMPLETED);
    assert(f.flows[0].outstanding == 0);
    assert(swifi_data_delete_flow(&f.data, 2) == SWIFI_OK);
    assert(f.published[0] == 5 && f.published[1] == 1 && f.published[8] == 2);
    assert(f.flows[0].state == SWIFI_FLOW_DELETING);
    assert(flow_reply(&f, 6, 2, 0) == SWIFI_OK);
    assert(f.last_flow == SWIFI_FLOW_CLOSED);
    assert(flow_reply(&f, 6, 2, 0) == SWIFI_PROTOCOL_ERROR);
}

static void test_rx_layout_bounds_and_return(void)
{
    fixture f;
    uint8_t reply[32];
    static const uint8_t post[32] = {
        0x11, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 8, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0x80, 0, 0, 0, 0
    };
    init(&f);
    assert(swifi_data_post_rx(&f.data, 0x80200000, 2048, &cookies[0]) == SWIFI_OK);
    assert(f.published_ring == 1 && f.published_length == sizeof(post));
    assert(!memcmp(f.published, post, sizeof(post)));
    rx_reply(&f, reply, 0, 1514);
    assert(swifi_data_complete(&f.data, reply, sizeof(reply)) == SWIFI_OK);
    assert(f.rx_done == 1 && f.last_rx.cookie == &cookies[0]);
    assert(f.last_rx.offset == 8 && f.last_rx.length == 1514);
    assert(swifi_data_complete(&f.data, reply, sizeof(reply)) == SWIFI_PROTOCOL_ERROR);
    assert(f.rx_done == 1);
    assert(swifi_data_post_rx(&f.data, 0x80200000, 2048, &cookies[1]) == SWIFI_OK);
    rx_reply(&f, reply, 16, 2032);
    reply[1] = 1;
    assert(swifi_data_complete(&f.data, reply, sizeof(reply)) == SWIFI_OK);
    assert(f.last_rx.offset == 16 && f.last_rx.length == 2032 && f.last_rx.ifidx == 1);
    assert(swifi_data_post_rx(&f.data, 0x80200000, 2048, &cookies[2]) == SWIFI_OK);
    rx_reply(&f, reply, 16, 2033);
    assert(swifi_data_complete(&f.data, reply, sizeof(reply)) == SWIFI_PROTOCOL_ERROR);
    assert(f.last_rx.reason == SWIFI_BAD_PACKET && f.last_rx.length == 0);
    assert(f.rx_done == 3);
    assert(swifi_data_post_rx(&f.data, 0x80200000, 2048, &cookies[3]) == SWIFI_OK);
    rx_reply(&f, reply, 0xffff, 1);
    assert(swifi_data_complete(&f.data, reply, sizeof(reply)) == SWIFI_PROTOCOL_ERROR);
    assert(f.last_rx.offset == 0 && f.last_rx.length == 0);
    assert(swifi_data_post_rx(&f.data, 0x80200000, 2048, &cookies[4]) == SWIFI_OK);
    rx_reply(&f, reply, 0, 0);
    reply[8] = 1; /* Failed receive, no packet payload is valid. */
    assert(swifi_data_complete(&f.data, reply, sizeof(reply)) == SWIFI_OK);
    assert(f.last_rx.reason == SWIFI_FIRMWARE_ERROR && f.last_rx.length == 0);
}

static void test_backpressure_stale_completion_and_wrong_ring(void)
{
    fixture f;
    uint8_t first[16], second[16], current[16], bad[16];
    init(&f);
    open_flow(&f, 2, 0, 0);
    open_flow(&f, 3, 1, 7);
    f.submit_result = SWIFI_RETRY;
    assert(swifi_data_tx(&f.data, 2, 0, ethernet, 0x80000000, 64, &cookies[0]) == SWIFI_RETRY);
    assert(!f.tx[0].owned && !f.flows[0].outstanding && !f.tx_done);
    f.submit_result = SWIFI_OK;
    assert(swifi_data_tx(&f.data, 2, 0, ethernet, 0x80000000, 64, &cookies[0]) == SWIFI_OK);
    tx_reply(&f, first);
    assert(swifi_data_tx(&f.data, 3, 7, ethernet, 0x80001000, 64, &cookies[1]) == SWIFI_OK);
    tx_reply(&f, second);
    assert(swifi_data_tx(&f.data, 2, 0, ethernet, 0x80002000, 64, &cookies[2]) == SWIFI_RETRY);
    memcpy(bad, first, sizeof(bad));
    bad[10] = 3;
    assert(swifi_data_complete(&f.data, bad, sizeof(bad)) == SWIFI_PROTOCOL_ERROR);
    assert(f.flows[0].outstanding == 1 && f.flows[1].outstanding == 1 && !f.tx_done);
    assert(swifi_data_complete(&f.data, first, sizeof(first)) == SWIFI_OK);
    assert(swifi_data_tx(&f.data, 2, 0, ethernet, 0x80002000, 64, &cookies[2]) == SWIFI_OK);
    tx_reply(&f, current);
    assert(memcmp(current + 4, first + 4, 4));
    assert(swifi_data_complete(&f.data, first, sizeof(first)) == SWIFI_PROTOCOL_ERROR);
    assert(f.tx_done == 1 && f.flows[0].outstanding == 1);
    second[8] = 0xfe; second[9] = 0xff; /* Firmware error -2. */
    second[14] = 7;
    assert(swifi_data_complete(&f.data, second, sizeof(second)) == SWIFI_OK);
    assert(f.last_tx.firmware_status == -2 && f.last_tx.tx_status == 7);
    assert(f.last_tx.reason == SWIFI_FIRMWARE_ERROR && f.last_tx.cookie == &cookies[1]);
    assert(swifi_data_complete(&f.data, current, sizeof(current)) == SWIFI_OK);
    assert(f.tx_done == 3 && !f.flows[0].outstanding && !f.flows[1].outstanding);
}

static void test_failures_and_reset(void)
{
    fixture f;
    uint8_t short_tx[8] = {0x10}, control[8] = {0x0e};
    unsigned done;
    init(&f);
    f.submit_result = SWIFI_RETRY;
    assert(swifi_data_create_flow(&f.data, 2, 0, 0, source, destination,
        0x80000000, 512) == SWIFI_RETRY);
    assert(f.flows[0].state == SWIFI_FLOW_CLOSED);
    assert(swifi_data_post_rx(&f.data, 0x80200000, 2048, &cookies[0]) == SWIFI_RETRY);
    assert(!f.rx[0].owned && !f.rx_done);
    f.submit_result = SWIFI_OK;
    assert(swifi_data_create_flow(&f.data, 2, 0, 0, source, destination,
        0x80000000, 512) == SWIFI_OK);
    assert(flow_reply(&f, 4, 2, 3) == SWIFI_OK);
    assert(f.last_flow == SWIFI_FLOW_CLOSED && f.last_flow_status == 3);
    open_flow(&f, 2, 0, 0);
    assert(swifi_data_delete_flow(&f.data, 2) == SWIFI_OK);
    assert(flow_reply(&f, 6, 2, 1) == SWIFI_OK);
    assert(f.last_flow == SWIFI_FLOW_OPEN && f.last_flow_status == 1);
    assert(swifi_data_complete(&f.data, control, sizeof(control)) == SWIFI_UNHANDLED);
    assert(swifi_data_complete(&f.data, short_tx, sizeof(short_tx)) == SWIFI_PROTOCOL_ERROR);
    assert(swifi_data_tx(&f.data, 2, 0, ethernet, 0x100000000ull, 64, &cookies[0]) == SWIFI_INVALID);
    assert(swifi_data_tx(&f.data, 2, 0, ethernet, 0xfffffff0u, 64, &cookies[0]) == SWIFI_INVALID);
    assert(swifi_data_post_rx(&f.data, 0xfffffff0u, 2048, &cookies[0]) == SWIFI_INVALID);
    assert(swifi_data_post_rx(&f.data, 0x80200000u, 16, &cookies[0]) == SWIFI_INVALID);
    assert(swifi_data_tx(&f.data, 2, 0, ethernet, 0x80000000, 64, &cookies[0]) == SWIFI_OK);
    assert(swifi_data_post_rx(&f.data, 0x80200000, 2048, &cookies[1]) == SWIFI_OK);
    assert(swifi_data_post_rx(&f.data, 0x80201000, 2048, &cookies[2]) == SWIFI_OK);
    assert(swifi_data_post_rx(&f.data, 0x80202000, 2048, &cookies[3]) == SWIFI_RETRY);
    done = f.flow_done;
    swifi_data_stopped_after_reset(&f.data);
    assert(f.tx_done == 1 && f.rx_done == 2 && f.flow_done == done + 1);
    assert(f.last_tx.reason == SWIFI_CANCELLED && f.last_rx.reason == SWIFI_CANCELLED);
    assert(f.cancelled_tx == 1 && f.cancelled_rx == ((1u << 1) | (1u << 2)));
    assert(!f.flows[0].outstanding && f.last_flow == SWIFI_FLOW_CLOSED);
    swifi_data_stopped_after_reset(&f.data);
    assert(f.tx_done == 1 && f.rx_done == 2 && f.flow_done == done + 1);
    assert(swifi_data_post_rx(&f.data, 0x80200000, 2048, &cookies[0]) == SWIFI_STOPPED);
    assert(swifi_data_complete(&f.data, control, sizeof(control)) == SWIFI_STOPPED);
}

static void test_rx_metadata_dma(void)
{
    fixture f;
    uint8_t reply[32];
    init(&f);
    assert(swifi_data_post_rx_meta(&f.data, 0x80200000, 2048, 0x80301000, 128, &cookies[0]) == SWIFI_OK);
    static const uint8_t expected[32] = {
        0x11,0,0,0,1,0,1,0, 128,0,0,8,0,0,0,0,
        0,0x10,0x30,0x80,0,0,0,0, 0,0,0x20,0x80,0,0,0,0
    };
    assert(!memcmp(f.published,expected,sizeof(expected)));
    rx_reply(&f,reply,16,64);
    reply[12]=7;
    assert(swifi_data_complete(&f.data, reply, sizeof(reply)) == SWIFI_OK);
    assert(f.last_rx.metadata_length==7 && f.last_rx.offset==16 && f.last_rx.length==64);
    assert(swifi_data_post_rx_meta(&f.data,0x80200000,2048,0x802007ff,128,&cookies[0])==SWIFI_INVALID);
    assert(swifi_data_post_rx_meta(&f.data,0x80200000,2048,0xfffffff0,128,&cookies[0])==SWIFI_INVALID);
    assert(swifi_data_post_rx_meta(&f.data,0x80200000,2048,0,128,&cookies[0])==SWIFI_INVALID);
    assert(swifi_data_post_rx_meta(&f.data,0x80200000,2048,0x80301000,128,&cookies[1])==SWIFI_OK);
    rx_reply(&f,reply,0,64);
    reply[12]=129;
    assert(swifi_data_complete(&f.data,reply,sizeof(reply))==SWIFI_PROTOCOL_ERROR);
    assert(!f.last_rx.metadata_length && !f.last_rx.length && f.last_rx.reason==SWIFI_BAD_PACKET);
    assert(swifi_data_post_rx(&f.data,0x80200000,2048,&cookies[1])==SWIFI_OK);
    rx_reply(&f,reply,0,64);
    reply[12]=1;
    assert(swifi_data_complete(&f.data,reply,sizeof(reply))==SWIFI_PROTOCOL_ERROR);
}

int main(void)
{
    test_rx_metadata_dma();
    test_wire_layout_and_flow_lifetime();
    test_rx_layout_bounds_and_return();
    test_backpressure_stale_completion_and_wrong_ring();
    test_failures_and_reset();
    puts("msgbuf data: wire layout, flow lifetime, RX bounds, ownership and reset passed");
    return 0;
}
