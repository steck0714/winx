/* SPDX-License-Identifier: ISC */
#include "station_frame.h"
#include <string.h>

static int special_type(unsigned type)
{
    return type == 0x80f3 || type == 0x8137; /* AARP and IPX */
}

size_t swifi_station_tx(const uint8_t *wifi, size_t length, const uint8_t station[6],
    const uint8_t peer[6], uint8_t *ethernet, size_t capacity, uint8_t *priority)
{
    uint8_t header[14];
    if (!wifi || !station || !peer || !ethernet || !priority || length < 24 ||
        (wifi[0] != 0x08 && wifi[0] != 0x88) || (wifi[1] & 7) != 1 || (wifi[22] & 15) ||
        memcmp(wifi + 4, peer, 6) || memcmp(wifi + 10, station, 6)) return 0;
    size_t offset = 24;
    uint8_t tid = 0;
    if (wifi[0] == 0x88) {
        if (length < 26 || (wifi[24] & 0x88)) return 0; /* no A-MSDU or non-EDCA TID */
        tid = wifi[24] & 7;
        offset = (wifi[1] & 0x80) ? 30 : 26;
    }
    if (offset >= length) return 0;
    memcpy(header, wifi + 16, 6); /* ToDS: destination is address 3. */
    memcpy(header + 6, wifi + 10, 6);
    const uint8_t *llc = wifi + offset;
    size_t payload = length - offset;
    unsigned type = 0;
    if (payload >= 8 && llc[0] == 0xaa && llc[1] == 0xaa && llc[2] == 3 &&
        llc[3] == 0 && llc[4] == 0 && (llc[5] == 0 || llc[5] == 0xf8)) {
        unsigned pid = ((unsigned)llc[6] << 8) | llc[7];
        if (pid >= 1536 && (llc[5] == 0xf8 || !special_type(pid))) {
            type = pid;
            llc += 8;
            payload -= 8;
        }
    }
    if (!payload || payload > 1500 + (type == 0x8100 || type == 0x88a8 ? 4u : 0u) ||
        capacity < 14 + payload) return 0;
    if (!type) type = (unsigned)payload; /* 802.3 length with the original LLC */
    header[12] = (uint8_t)(type >> 8);
    header[13] = (uint8_t)type;
    memmove(ethernet + 14, llc, payload);
    memcpy(ethernet, header, sizeof(header));
    *priority = tid;
    return 14 + payload;
}

size_t swifi_station_rx(const uint8_t *ethernet, size_t length, const uint8_t station[6],
    const uint8_t peer[6], uint16_t sequence, int protected_msdu, uint8_t *wifi, size_t capacity)
{
    uint8_t header[32] = {0};
    if (!ethernet || !station || !peer || !wifi || length <= 14 ||
        (!(ethernet[0] & 1) && memcmp(ethernet, station, 6))) return 0;
    unsigned type = ((unsigned)ethernet[12] << 8) | ethernet[13];
    size_t payload = length - 14;
    size_t offset = 24;
    header[0] = 0x08;
    header[1] = protected_msdu ? 0x42 : 0x02; /* FromDS, no crypto header or MIC in the MSDU */
    memcpy(header + 4, ethernet, 6);
    memcpy(header + 10, peer, 6);
    memcpy(header + 16, ethernet + 6, 6);
    header[22] = (uint8_t)(sequence << 4);
    header[23] = (uint8_t)(sequence >> 4);
    if (type >= 1536) {
        header[24] = header[25] = 0xaa;
        header[26] = 3;
        header[29] = special_type(type) ? 0xf8 : 0;
        header[30] = ethernet[12];
        header[31] = ethernet[13];
        offset = 32;
        if (payload > 1500 + (type == 0x8100 || type == 0x88a8 ? 4u : 0u)) return 0;
    } else {
        if (!type || type > 1500 || type > payload) return 0;
        payload = type; /* discard Ethernet padding, retain LLC */
    }
    if (capacity < offset + payload) return 0;
    memmove(wifi + offset, ethernet + 14, payload);
    memcpy(wifi, header, offset);
    return offset + payload;
}
