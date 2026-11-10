/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_RECEIVE_FILTER_H
#define SWIFI_RECEIVE_FILTER_H
#include <stdint.h>
#include <stddef.h>

enum {
    SWIFI_DIRECTED = 1, SWIFI_MULTICAST = 2, SWIFI_ALL_MULTICAST = 4,
    SWIFI_BROADCAST = 8, SWIFI_STATION_FILTERS = 15, SWIFI_MAX_MULTICAST = 32
};
typedef struct swifi_receive_filter {
    uint32_t flags;
    unsigned count;
    uint8_t addresses[SWIFI_MAX_MULTICAST][6];
} swifi_receive_filter;

/* The list for a Windows station port is independent of its current mask. */
int swifi_filter_list(swifi_receive_filter *filter, const uint8_t *addresses,
    size_t count, size_t stride);
int swifi_filter_accepts(const swifi_receive_filter *filter, const uint8_t station[6],
    const uint8_t destination[6]);
/* BCM mcast_list uses a little-endian uint32 count followed by packed MACs. */
size_t swifi_filter_wire(const swifi_receive_filter *filter, uint8_t *wire, size_t capacity);
#endif
