/* SPDX-License-Identifier: ISC */
#include "receive_filter.h"
#include <string.h>

int swifi_filter_list(swifi_receive_filter *filter, const uint8_t *addresses,
    size_t count, size_t stride)
{
    if (!filter || count > SWIFI_MAX_MULTICAST ||
        (count && (!addresses || stride < 6 || stride > SIZE_MAX / count))) return 0;
    for (size_t i = 0; i < count; ++i)
        if (!(addresses[i * stride] & 1)) return 0;
    for (size_t i = 0; i < count; ++i)
        memcpy(filter->addresses[i], addresses + i * stride, 6);
    filter->count = (unsigned)count;
    return 1;
}

int swifi_filter_accepts(const swifi_receive_filter *filter, const uint8_t station[6],
    const uint8_t destination[6])
{
    static const uint8_t broadcast[6] = {255,255,255,255,255,255};
    if (!filter || !station || !destination) return 0;
    if (!(destination[0] & 1))
        return (filter->flags & SWIFI_DIRECTED) && !memcmp(destination, station, 6);
    if (!memcmp(destination, broadcast, 6)) return (filter->flags & SWIFI_BROADCAST) != 0;
    if (filter->flags & SWIFI_ALL_MULTICAST) return 1;
    if (filter->flags & SWIFI_MULTICAST)
        for (unsigned i = 0; i < filter->count; ++i)
            if (!memcmp(filter->addresses[i], destination, 6)) return 1;
    return 0;
}

size_t swifi_filter_wire(const swifi_receive_filter *filter, uint8_t *wire, size_t capacity)
{
    if (!filter || !wire || filter->count > SWIFI_MAX_MULTICAST) return 0;
    unsigned count = filter->flags & SWIFI_MULTICAST ? filter->count : 0;
    size_t size = 4 + count * 6;
    if (size > capacity) return 0;
    wire[0] = (uint8_t)count;
    wire[1] = wire[2] = wire[3] = 0;
    memcpy(wire + 4, filter->addresses, count * 6);
    return size;
}
