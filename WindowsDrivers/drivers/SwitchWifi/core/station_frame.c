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

size_t swifi_ether_eapol(uint8_t *frame, size_t length)
{
    static const uint8_t snap[] = {0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e};
    if (!frame || length < 14) return 0;
    size_t llc = ((size_t)frame[12] << 8) | frame[13];
    if (llc > 1500 || length < 14 + sizeof(snap) ||
        memcmp(frame + 14, snap, sizeof(snap))) return length;
    if (llc < sizeof(snap) + 4 || llc > length - 14) return 0;
    size_t eapol = llc - sizeof(snap);
    memmove(frame + 14, frame + 14 + sizeof(snap), eapol);
    frame[12] = 0x88;
    frame[13] = 0x8e;
    return 14 + eapol;
}

size_t swifi_clear_eapol(uint8_t *f, size_t n, const uint8_t station[6], const uint8_t peer[6])
{
    if (!f || !station || !peer || n < 24 || (f[0] != 8 && f[0] != 0x88) ||
        (f[1] & 0x47) != 2 || (f[22] & 15) || memcmp(f+10, peer, 6) ||
        (f[16] & 1) || !memcmp(f+16, "\0\0\0\0\0\0", 6) || (!(f[4]&1) && memcmp(f+4, station, 6))) return 0;
    size_t offset = 24;
    if (f[0] == 0x88) {
        if (n < 26 || (f[24] & 0x88)) return 0;
        offset = f[1] & 0x80 ? 30 : 26;
    }
    const uint8_t snap[] = {0xaa,0xaa,3,0,0,0,0x88,0x8e};
    if (n < offset + 12 || memcmp(f+offset,snap,sizeof(snap))) return 0;
    const uint8_t *eapol = f+offset+8;
    size_t payload = n-offset-8;
    if ((((size_t)eapol[2] << 8) | eapol[3]) > payload-4) return 0;
    uint8_t header[14];
    memcpy(header,f+4,6); memcpy(header+6,f+16,6); header[12]=0x88; header[13]=0x8e;
    memmove(f+14,eapol,payload);
    memcpy(f,header,sizeof(header));
    return 14+payload;
}
