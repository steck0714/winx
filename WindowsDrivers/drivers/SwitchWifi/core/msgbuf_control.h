/* SPDX-License-Identifier: ISC */
/* BCM4356 msgbuf-v5 command/event path; see NOTICE for the protocol source. */
#ifndef SWIFI_MSGBUF_CONTROL_H
#define SWIFI_MSGBUF_CONTROL_H

#include "msgbuf_data.h"

#define SWIFI_CONTROL_BUFFER_SIZE 8192u
#define SWIFI_EVENT_COUNT 8u

typedef struct swifi_control_buffer {
    uint8_t *memory; /* Resident, coherent/uncached, exactly 8192 bytes or more. */
    uint64_t dma;
} swifi_control_buffer;

typedef enum swifi_command_reason {
    SWIFI_COMMAND_COMPLETED,
    SWIFI_COMMAND_FIRMWARE_ERROR,
    SWIFI_COMMAND_TRUNCATED,
    SWIFI_COMMAND_TIMED_OUT,
    SWIFI_COMMAND_CANCELLED
} swifi_command_reason;

typedef struct swifi_command_completion {
    void *cookie;
    const uint8_t *response; /* Valid only during the callback; copy before return. */
    uint32_t command;
    uint32_t request_id;
    uint16_t length; /* At most the requested output length. */
    uint16_t returned_length; /* Firmware length before truncation. */
    int16_t firmware_status;
    uint8_t ifidx;
    swifi_command_reason reason;
} swifi_command_completion;

typedef struct swifi_event_completion {
    const uint8_t *data; /* Retained until release_event, never reposted early. */
    uint32_t request_id;
    uint16_t length;
    uint16_t sequence;
    int16_t firmware_status;
    uint8_t ifidx;
} swifi_event_completion;

typedef struct swifi_control_ops {
    /* Copy/publish all count control-ring items atomically, then ring the
     * doorbell. Each item is 40 bytes. Any failure publishes NOTHING; a
     * failure after publication requires reset and must still return OK.
     * Apply a DMA release barrier to the descriptors and payload buffers. */
    swifi_result (*submit)(void *context, const uint8_t *items, uint16_t count);
    /* Nonblocking, non-reentrant callbacks under the device DataLock. Queue
     * notifications only; complete OS requests outside that lock. */
    void (*command_complete)(void *context, const swifi_command_completion *reply);
    void (*event_ready)(void *context, const swifi_event_completion *event);
} swifi_control_ops;

typedef enum swifi_event_state {
    SWIFI_EVENT_AVAILABLE,
    SWIFI_EVENT_POSTED,
    SWIFI_EVENT_DELIVERED
} swifi_event_state;

typedef struct swifi_event_slot {
    swifi_control_buffer buffer;
    uint32_t generation;
    uint32_t request_id;
    swifi_event_state state;
} swifi_event_slot;

typedef struct swifi_control {
    swifi_control_ops ops;
    void *context;
    swifi_control_buffer input;
    swifi_control_buffer response;
    swifi_event_slot events[SWIFI_EVENT_COUNT];
    void *cookie;
    uint32_t sequence;
    uint32_t request_id;
    uint32_t command;
    uint16_t output_length;
    uint16_t transaction;
    uint16_t rx_data_offset;
    uint8_t ifidx;
    uint8_t pending;
    uint8_t stopped;
} swifi_control;

/* Device-lifetime caller-owned buffers; all addresses must fit 32-bit DMA.
 * Calls are serialized with packet/ring operations. Initialize before DMA
 * starts; reinitialize only after endpoint reset, deadline cancellation and
 * release of all delivered events. */
swifi_result swifi_control_init(swifi_control *control,
    const swifi_control_ops *ops, void *context, swifi_control_buffer input,
    swifi_control_buffer response,
    const swifi_control_buffer events[SWIFI_EVENT_COUNT], uint16_t rx_data_offset);

/* One outstanding command. RETRY leaves ownership with the caller (another
 * command is active or the control ring cannot accept BOTH descriptors).
 * On OK, request_id identifies the command for the caller's deadline timer.
 * The input is copied; no caller input pointer is retained. */
swifi_result swifi_control_command(swifi_control *control, uint8_t ifidx,
    uint32_t command, const void *input, uint16_t input_length,
    uint16_t output_length, void *cookie, uint32_t *request_id);

/* Post available event buffers. A ring-full return leaves remaining slots
 * available; retry when control-ring completions/space become available.
 * Completed events remain host-owned until explicitly released. */
swifi_result swifi_control_refill_events(swifi_control *control);
swifi_result swifi_control_release_event(swifi_control *control, uint32_t request_id);

/* Dispatch after the D2H acquire barrier, past the completion-ring prefix.
 * Command responses start at byte zero; event payloads use rx_data_offset. */
swifi_result swifi_control_complete(swifi_control *control,
    const uint8_t *item, size_t length);

/* Called once the matching deadline expires. A timeout completes the command
 * exactly once, stops new submissions and retains ALL DMA allocations. The
 * owner must recover/reset hardware; a late reply never makes reuse safe. */
swifi_result swifi_control_timeout(swifi_control *control, uint32_t request_id);

/* DMA is already stopped/reset. Cancel a pending command once and release
 * posted buffers. Delivered events remain retained until release_event;
 * drain those consumers before freeing their DMA allocations. */
void swifi_control_stopped_after_reset(swifi_control *control);

#endif
