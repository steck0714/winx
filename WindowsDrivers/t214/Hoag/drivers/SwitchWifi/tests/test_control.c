/* SPDX-License-Identifier: ISC */
#include "../core/msgbuf_control.h"
#include "../core/msgbuf_ring.h"
#include "../core/WifiEscan.h"
#include "../windows/firmware_events.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct model {
    swifi_control control;
    swifi_ring ring;
    uint8_t descriptors[16 * 40];
    uint8_t buffers[10][8192];
    uint16_t producer, consumer;
    unsigned publications, commands, event_count;
    swifi_command_completion reply;
    swifi_event_completion event[8];
    uint8_t response[8192];
} model;

static uint32_t read_peer(void *context)
{
    return ((model *)context)->consumer;
}

static void write_host(void *context, uint16_t index)
{
    model *m = context;
    m->producer = index;
    ++m->publications;
}

static void barrier(void *context) { (void)context; }

static swifi_result submit(void *context, const uint8_t *items, uint16_t count)
{
    model *m = context;
    return swifi_ring_submit_batch(&m->ring, items, (size_t)count * 40, count);
}

static void command_done(void *context, const swifi_command_completion *reply)
{
    model *m = context;
    m->reply = *reply;
    if (reply->length)
        memcpy(m->response, reply->response, reply->length);
    ++m->commands;
}

static void event_ready(void *context, const swifi_event_completion *event)
{
    model *m = context;
    assert(m->event_count < 8);
    m->event[m->event_count++] = *event;
}

static void init(model *m)
{
    const swifi_control_ops ops = {submit, command_done, event_ready};
    const swifi_ring_ops ring_ops = {read_peer, write_host, barrier};
    swifi_control_buffer event[8], input, response;
    memset(m, 0, sizeof(*m));
    input.memory = m->buffers[0]; input.dma = 0x80000000;
    response.memory = m->buffers[1]; response.dma = 0x80010000;
    for (unsigned i = 0; i < 8; ++i) {
        event[i].memory = m->buffers[i + 2];
        event[i].dma = 0x80020000 + i * 0x10000;
    }
    assert(swifi_ring_init(&m->ring, &ring_ops, m, m->descriptors,
        sizeof(m->descriptors), 16, 40, 1, 0) == SWIFI_OK);
    assert(swifi_control_init(&m->control, &ops, m, input, response, event, 8) == SWIFI_OK);
}

static void read_item(model *m, uint8_t item[40])
{
    assert(m->consumer != m->producer);
    memcpy(item, m->descriptors + m->consumer * 40, 40);
    m->consumer = (uint16_t)((m->consumer + 1) % 16);
}

static void put16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

/* Construct replies from device-observed descriptors, not the core state. */
static void command_reply(uint8_t reply[24], const uint8_t post[40],
    const uint8_t request[40], unsigned size)
{
    memset(reply, 0, 24);
    reply[0] = 0x0c; reply[1] = request[1];
    memcpy(reply + 4, post + 4, 4);
    put16(reply + 12, size);
    memcpy(reply + 14, request + 12, 2);
    memcpy(reply + 16, request + 8, 4);
}

static void test_commands(void)
{
    model m;
    uint32_t id, other = 0;
    uint8_t post[40], request[40], reply[24];
    const uint8_t input[] = "cur_etheraddr";
    const uint8_t expected_request[40] = {
        9, 2, 0, 0, 0xfe, 0xff, 0, 0, 6, 1, 0, 0, 1, 0, 14, 0,
        32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80
    };
    const uint8_t expected_post[40] = {
        11, 0, 0, 0, 1, 0, 0, 0x40, 0, 0x20, 0, 0, 0, 0, 0, 0,
        0, 0, 1, 0x80
    };
    init(&m);
    assert(swifi_control_command(&m.control, 2, 262, input, sizeof(input), 32,
        &m, &id) == SWIFI_OK);
    assert(id == 0x40000001 && m.publications == 1);
    assert(!memcmp(m.buffers[0], input, sizeof(input)) && !m.buffers[0][sizeof(input)]);
    assert(swifi_control_command(&m.control, 0, 1, NULL, 0, 4, NULL, &other) == SWIFI_RETRY);
    assert(!other && m.publications == 1);
    read_item(&m, post); read_item(&m, request);
    assert(!memcmp(post, expected_post, 40));
    assert(!memcmp(request, expected_request, 40));
    memset(reply, 0, sizeof(reply)); reply[0] = 10; reply[4] = 0xfe; reply[5] = 0xff;
    assert(swifi_control_complete(&m.control, reply, 24) == SWIFI_OK);
    reply[4] = reply[5] = 0;
    assert(swifi_control_complete(&m.control, reply, 8) == SWIFI_OK);
    assert(m.commands == 0 && m.control.pending);
    memcpy(m.buffers[1], "result", 7);
    command_reply(reply, post, request, 7);
    assert(swifi_control_complete(&m.control, reply, 24) == SWIFI_OK);
    assert(m.commands == 1 && !m.control.pending && m.reply.cookie == &m);
    assert(m.reply.reason == SWIFI_COMMAND_COMPLETED && m.reply.length == 7);
    assert(!memcmp(m.response, "result", 7)); /* Response has no RX offset. */
    assert(swifi_control_timeout(&m.control, id) == SWIFI_UNHANDLED);
    assert(swifi_control_complete(&m.control, reply, 24) == SWIFI_PROTOCOL_ERROR);
    assert(m.commands == 1);

    assert(swifi_control_command(&m.control, 0, 263, NULL, 0, 2, NULL, &other) == SWIFI_OK);
    read_item(&m, post); read_item(&m, request);
    assert(other != id);
    assert(swifi_control_timeout(&m.control, id) == SWIFI_UNHANDLED);
    memcpy(m.buffers[1], "abcd", 4);
    command_reply(reply, post, request, 4);
    assert(swifi_control_complete(&m.control, reply, 24) == SWIFI_OK);
    assert(m.reply.reason == SWIFI_COMMAND_TRUNCATED && m.reply.length == 2 && m.reply.returned_length == 4);
    assert(!memcmp(m.response, "ab", 2));
    assert(swifi_control_command(&m.control, 0, 263, NULL, 0, 0, NULL, &id) == SWIFI_OK);
    read_item(&m, post); read_item(&m, request);
    command_reply(reply, post, request, 0); reply[8] = 0xe9; reply[9] = 0xff;
    assert(swifi_control_complete(&m.control, reply, 24) == SWIFI_OK);
    assert(m.reply.reason == SWIFI_COMMAND_FIRMWARE_ERROR && m.reply.firmware_status == -23);
}

static void test_backpressure_timeout_and_cancel(void)
{
    model m;
    uint8_t dummy[40] = {0}, post[40], request[40], reply[24], before[16 * 40];
    uint32_t id = 0;
    init(&m);
    for (unsigned i = 0; i < 14; ++i)
        assert(swifi_ring_submit(&m.ring, dummy, 40) == SWIFI_OK);
    memcpy(before, m.descriptors, sizeof(before));
    assert(swifi_control_command(&m.control, 0, 1, NULL, 0, 4, &m, &id) == SWIFI_RETRY);
    assert(!id && !m.control.pending && m.publications == 14);
    assert(!memcmp(before, m.descriptors, sizeof(before)));
    for (unsigned i = 0; i < 14; ++i) read_item(&m, dummy);
    assert(swifi_control_command(&m.control, 0, 1, NULL, 0, 4, &m, &id) == SWIFI_OK);
    read_item(&m, post); read_item(&m, request);
    assert(swifi_control_timeout(&m.control, id) == SWIFI_OK);
    assert(m.reply.reason == SWIFI_COMMAND_TIMED_OUT && !m.reply.response && m.commands == 1);
    assert(swifi_control_timeout(&m.control, id) == SWIFI_UNHANDLED);
    assert(swifi_control_command(&m.control, 0, 1, NULL, 0, 4, &m, &id) == SWIFI_STOPPED);
    command_reply(reply, post, request, 4);
    assert(swifi_control_complete(&m.control, reply, 24) == SWIFI_STOPPED);
    swifi_control_stopped_after_reset(&m.control);
    assert(m.commands == 1);
    init(&m);
    assert(swifi_control_command(&m.control, 0, 1, NULL, 0, 4, &m, &id) == SWIFI_OK);
    swifi_control_stopped_after_reset(&m.control);
    swifi_control_stopped_after_reset(&m.control);
    assert(m.commands == 1 && m.reply.reason == SWIFI_COMMAND_CANCELLED);
}

static void test_wrong_and_short_completions(void)
{
    model m;
    uint8_t post[40], request[40], good[24], bad[24];
    uint32_t id;
    init(&m);
    assert(swifi_control_command(&m.control, 0, 1, NULL, 0, 4, &m, &id) == SWIFI_OK);
    read_item(&m, post); read_item(&m, request);
    command_reply(good, post, request, 4);
    for (unsigned n = 0; n < 20; ++n)
        assert(swifi_control_complete(&m.control, good, n) == SWIFI_PROTOCOL_ERROR);
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned positions[] = {4, 14, 16};
        memcpy(bad, good, 24); bad[positions[i]] ^= 1;
        assert(swifi_control_complete(&m.control, bad, 24) == SWIFI_PROTOCOL_ERROR);
    }
    memcpy(bad, good, 24); put16(bad + 12, 8193);
    assert(swifi_control_complete(&m.control, bad, 24) == SWIFI_PROTOCOL_ERROR);
    assert(m.control.pending && !m.commands);
    assert(swifi_control_complete(&m.control, good, 24) == SWIFI_OK);
    bad[0] = 0x10;
    assert(swifi_control_complete(&m.control, bad, 24) == SWIFI_UNHANDLED);
}

static void test_event_lifetime(void)
{
    model m;
    uint8_t posts[8][40], reply[24] = {0}, newer[40], dummy[40] = {0};
    uint32_t old_id;
    init(&m);
    for (unsigned i = 0; i < 10; ++i)
        assert(swifi_ring_submit(&m.ring, dummy, 40) == SWIFI_OK);
    assert(swifi_control_refill_events(&m.control) == SWIFI_RETRY);
    for (unsigned i = 0; i < 10; ++i) read_item(&m, dummy);
    for (unsigned i = 0; i < 5; ++i) read_item(&m, posts[i]);
    assert(swifi_control_refill_events(&m.control) == SWIFI_OK);
    for (unsigned i = 5; i < 8; ++i) read_item(&m, posts[i]);
    for (unsigned i = 0; i < 8; ++i) {
        assert(posts[i][0] == 13 && posts[i][8] == 0 && posts[i][9] == 0x20);
        assert(posts[i][16] == 0 && posts[i][17] == 0 && posts[i][18] == i + 2 && posts[i][19] == 0x80);
        assert(posts[i][4] == i && posts[i][5] == 1 && posts[i][7] == 0x80);
        memset(m.buffers[i + 2], 0xcc, 8);
        memcpy(m.buffers[i + 2] + 8, "event", 6);
        memset(reply, 0, 24); reply[0] = 14; reply[1] = 2;
        memcpy(reply + 4, posts[i] + 4, 4);
        put16(reply + 12, 6); put16(reply + 14, i + 100);
        assert(swifi_control_complete(&m.control, reply, 24) == SWIFI_OK);
        assert(m.event[i].sequence == i + 100 && m.event[i].ifidx == 2);
        assert(m.event[i].length == 6 && !memcmp(m.event[i].data, "event", 6));
    }
    assert(m.event_count == 8);
    unsigned publications = m.publications;
    assert(swifi_control_refill_events(&m.control) == SWIFI_OK);
    assert(m.publications == publications); /* None can be reposted while retained. */
    old_id = m.event[3].request_id;
    assert(swifi_control_release_event(&m.control, old_id) == SWIFI_OK);
    assert(swifi_control_release_event(&m.control, old_id) == SWIFI_INVALID);
    assert(swifi_control_refill_events(&m.control) == SWIFI_OK);
    read_item(&m, newer);
    assert(newer[4] == 3 && newer[5] == 2);
    put32(reply + 4, old_id);
    assert(swifi_control_complete(&m.control, reply, 24) == SWIFI_PROTOCOL_ERROR);
    memcpy(reply + 4, newer + 4, 4); put16(reply + 12, 8192 - 7);
    assert(swifi_control_complete(&m.control, reply, 24) == SWIFI_PROTOCOL_ERROR);
    assert(m.control.events[3].state == SWIFI_EVENT_POSTED);
    swifi_control_stopped_after_reset(&m.control);
    assert(m.control.events[3].state == SWIFI_EVENT_AVAILABLE);
    assert(m.control.events[0].state == SWIFI_EVENT_DELIVERED);
    assert(swifi_control_release_event(&m.control, m.event[0].request_id) == SWIFI_OK);
    assert(swifi_control_refill_events(&m.control) == SWIFI_STOPPED);
}

/* Stand-ins for the two Windows state machines. The input comes through the
 * actual msgbuf event completion and the same router as the passive worker. */
struct SWIFI_SCAN { unsigned calls; };
struct SWIFI_JOIN { unsigned calls, type, status; };

uint8_t SwifiScanFirmwareEvent(SWIFI_SCAN *scan, const swifi_event_completion *event)
{
    ++scan->calls;
    return event->length >= 72 && SwifiScanBe32(event->data + 28) == 69;
}

uint8_t SwifiJoinEvent(SWIFI_JOIN *join, const swifi_event_completion *event)
{
    ++join->calls;
    assert(event->length >= 72);
    join->type = SwifiScanBe32(event->data + 28);
    join->status = SwifiScanBe32(event->data + 32);
    return join->type == 0 || join->type == 16 || join->type == 6;
}

static void test_station_event_delivery(void)
{
    enum { ignored, scan_owner, join_owner };
    static const struct {
        uint8_t type, status, ifidx;
        int16_t completion_status;
        unsigned owner;
    } cases[] = {
        {69, 8, 0, 0, scan_owner},  /* escan result */
        {0, 0, 0, 0, join_owner},  /* SET_SSID success */
        {0, 3, 0, 0, join_owner},  /* AP no longer visible */
        {16, 0, 0, 0, join_owner}, /* link down */
        {6, 0, 0, 0, join_owner},  /* peer deauthentication */
        {127, 0, 0, 0, ignored},   /* neither consumer claims it */
        {0, 0, 1, 0, ignored},     /* another interface */
        {69, 0, 0, -1, ignored},   /* failed buffer completion */
    };
    model m;
    SWIFI_SCAN scan = {0};
    SWIFI_JOIN join = {0};
    init(&m);
    assert(swifi_control_refill_events(&m.control) == SWIFI_OK);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t post[40], completion[24] = {14};
        read_item(&m, post);
        unsigned slot = post[4];
        uint8_t *frame = m.buffers[slot + 2] + m.control.rx_data_offset;
        frame[12] = 0x88; frame[13] = 0x6c;
        frame[20] = 0x10; frame[21] = 0x18; frame[23] = 1;
        frame[31] = cases[i].type;
        frame[35] = cases[i].status;
        completion[1] = cases[i].ifidx;
        memcpy(completion + 4, post + 4, 4);
        put16(completion + 8, (uint16_t)cases[i].completion_status);
        put16(completion + 12, 72);
        put16(completion + 14, 100 + i);
        assert(swifi_control_complete(&m.control, completion, sizeof(completion)) == SWIFI_OK);
        assert(m.event_count == i + 1 && m.event[i].sequence == 100 + i);
        unsigned old_scan = scan.calls, old_join = join.calls;
        int accepted = SwifiRouteFirmwareEvent(&scan, &join, &m.event[i]);
        int deliver = !cases[i].ifidx && !cases[i].completion_status;
        assert(accepted == (cases[i].owner != ignored));
        assert(scan.calls == old_scan + deliver);
        assert(join.calls == old_join + (deliver && cases[i].owner != scan_owner));
        if (cases[i].owner == join_owner) {
            assert(join.type == cases[i].type && join.status == cases[i].status);
        }
        assert(swifi_control_release_event(&m.control, m.event[i].request_id) == SWIFI_OK);
    }
}

int main(void)
{
    test_commands();
    test_backpressure_timeout_and_cancel();
    test_wrong_and_short_completions();
    test_event_lifetime();
    test_station_event_delivery();
    puts("msgbuf control: command lifetime, event ownership and scan/association delivery passed");
    return 0;
}
