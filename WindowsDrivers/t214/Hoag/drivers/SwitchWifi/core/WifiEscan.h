/* SPDX-License-Identifier: ISC */
/* BCM4356 escan v1 wire format, shared by the UEFI probe and Windows.
 * See NOTICE for the Broadcom brcmfmac protocol reference. */
#ifndef SWIFI_ESCAN_H
#define SWIFI_ESCAN_H

enum { SwifiScanChannels = 196, SwifiScanSsids = 10,
       SwifiScanWireMax = 72 + 2 * SwifiScanChannels + 36 * SwifiScanSsids };

typedef struct SWIFI_SCAN_SSID {
    unsigned char Length;
    unsigned char Bytes[32];
} SWIFI_SCAN_SSID;

typedef struct SWIFI_SCAN_SPEC {
    unsigned char Bssid[6];
    unsigned char Channels[SwifiScanChannels];
    SWIFI_SCAN_SSID Ssids[SwifiScanSsids];
    unsigned ChannelCount;
    unsigned SsidCount;
    unsigned ActiveMs;
    unsigned PassiveMs;
    unsigned char Passive;
} SWIFI_SCAN_SPEC;

typedef struct SWIFI_SCAN_BSS {
    const unsigned char *Bssid, *Ssid, *Ies;
    unsigned SsidLength, IeLength, Channel, Interval, Capability, Flags;
    int Rssi;
} SWIFI_SCAN_BSS;

enum swifi_scan_event {
    SwifiScanInvalid, SwifiScanOther, SwifiScanPartial, SwifiScanTerminal
};

static inline unsigned SwifiScanLe16(const unsigned char *p)
{
    return p[0] | ((unsigned)p[1] << 8);
}
static inline unsigned SwifiScanLe32(const unsigned char *p)
{
    return SwifiScanLe16(p) | (SwifiScanLe16(p + 2) << 16);
}
static inline unsigned SwifiScanBe32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}
static inline void SwifiScanPut16(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}
static inline void SwifiScanPut32(unsigned char *p, unsigned value)
{
    SwifiScanPut16(p, value);
    SwifiScanPut16(p + 2, value >> 16);
}

static inline unsigned SwifiScanChanspec(unsigned version, unsigned channel)
{
    if (!channel || channel > SwifiScanChannels) return 0;
    if (version == 1) return channel | 0x0b00 | (channel <= 14 ? 0x2000 : 0x1000);
    if (version == 2) return channel | 0x1000 | (channel <= 14 ? 0 : 0xc000);
    return 0;
}

static inline unsigned SwifiScanPrimary(unsigned version, unsigned chanspec)
{
    int center = (int)(chanspec & 0xff);
    if (version == 1) {
        switch (chanspec & 0x0c00) {
        case 0x0800: break;
        case 0x0c00:
            if ((chanspec & 0x0300) == 0x0100) center -= 2;
            else if ((chanspec & 0x0300) == 0x0200) center += 2;
            else return 0;
            break;
        default: return 0;
        }
    } else if (version == 2) {
        unsigned n, side = (chanspec >> 8) & 7;
        switch (chanspec & 0x3800) {
        case 0x1000: n = 1; break;
        case 0x1800: n = 2; break;
        case 0x2000: n = 4; break;
        case 0x2800: n = 8; break;
        default: return 0;
        }
        if (side >= n) return 0;
        center += (int)(4 * side) - (int)(2 * (n - 1));
    } else return 0;
    return center > 0 && center <= SwifiScanChannels ? (unsigned)center : 0;
}

/* Channel entries are 20 MHz chanspecs. The counted SSID array follows a
 * DWORD-aligned channel array. A zero dwell requests the firmware default. */
static inline unsigned SwifiEncodeScan(unsigned char *out, unsigned capacity,
    unsigned version, unsigned sync, const SWIFI_SCAN_SPEC *s)
{
    unsigned size, ssid_offset;
    if (!out || !s || (version != 1 && version != 2) || !sync || sync > 0xffff ||
        s->ChannelCount > SwifiScanChannels || s->SsidCount > SwifiScanSsids ||
        s->ActiveMs > 0x7fffffffU || s->PassiveMs > 0x7fffffffU) return 0;
    ssid_offset = 72 + ((s->ChannelCount * 2 + 3) & ~3u);
    size = ssid_offset + 36 * s->SsidCount;
    if (size > capacity) return 0;
    for (unsigned i = 0; i < size; ++i) out[i] = 0;
    SwifiScanPut32(out, 1);
    SwifiScanPut16(out + 4, 1);
    SwifiScanPut16(out + 6, sync);
    for (unsigned i = 0; i < 6; ++i) out[44 + i] = s->Bssid[i];
    out[50] = 2; /* DOT11_BSSTYPE_ANY */
    out[51] = s->Passive ? 1 : 0;
    SwifiScanPut32(out + 52, ~0u);
    SwifiScanPut32(out + 56, s->ActiveMs ? s->ActiveMs : ~0u);
    SwifiScanPut32(out + 60, s->PassiveMs ? s->PassiveMs : ~0u);
    SwifiScanPut32(out + 64, ~0u);
    SwifiScanPut32(out + 68, (s->SsidCount << 16) | s->ChannelCount);
    for (unsigned i = 0; i < s->ChannelCount; ++i) {
        unsigned code = SwifiScanChanspec(version, s->Channels[i]);
        if (!code) return 0;
        SwifiScanPut16(out + 72 + 2 * i, code);
    }
    for (unsigned i = 0; i < s->SsidCount; ++i) {
        const SWIFI_SCAN_SSID *ssid = &s->Ssids[i];
        unsigned char *slot = out + ssid_offset + 36 * i;
        if (ssid->Length > 32) return 0;
        SwifiScanPut32(slot, ssid->Length);
        for (unsigned j = 0; j < ssid->Length; ++j) slot[4 + j] = ssid->Bytes[j];
    }
    return size;
}

/* WLC_SCAN with one channel -1 aborts an existing escan in brcmfmac.
 * The C wire structure has two tail padding bytes after the channel. */
static inline void SwifiEncodeScanAbort(unsigned char out[68])
{
    for (unsigned i = 0; i < 68; ++i) out[i] = 0;
    for (unsigned i = 36; i < 42; ++i) out[i] = 0xff;
    out[42] = 2;
    SwifiScanPut32(out + 44, 1);
    for (unsigned i = 48; i <= 56; i += 4) SwifiScanPut32(out + i, ~0u);
    SwifiScanPut32(out + 60, 1);
    SwifiScanPut16(out + 64, 0xffff);
}

/* Returned pointers borrow the event buffer. An untagged terminal is
 * legitimate; the caller must drain an aborted scan before starting another. */
static inline enum swifi_scan_event SwifiDecodeScan(const unsigned char *data,
    unsigned length, unsigned version, unsigned sync, SWIFI_SCAN_BSS *b, unsigned *status)
{
    if (!data || !b || !status || length < 72 || data[12] != 0x88 || data[13] != 0x6c ||
        data[19] || data[20] != 0x10 || data[21] != 0x18 || data[22] || data[23] != 1)
        return SwifiScanInvalid;
    unsigned size = SwifiScanBe32(data + 44);
    if (size > length - 72) return SwifiScanInvalid;
    if (SwifiScanBe32(data + 28) != 69 || data[70]) return SwifiScanOther;
    const unsigned char *result = data + 72;
    *status = SwifiScanBe32(data + 32);
    if (size && size < 12) return SwifiScanInvalid;
    if (size && SwifiScanLe16(result + 8) != sync &&
        (*status == 8 || SwifiScanLe16(result + 8) != 0)) return SwifiScanOther;
    if (*status != 8) return SwifiScanTerminal;
    if (size < 140 || SwifiScanLe32(result) < 140 || SwifiScanLe32(result) > size ||
        SwifiScanLe16(result + 10) != 1) return SwifiScanInvalid;
    const unsigned char *record = result + 12;
    unsigned n = SwifiScanLe32(record + 4);
    if (SwifiScanLe32(record) != 109 || n != SwifiScanLe32(result) - 12 ||
        record[18] > 32) return SwifiScanInvalid;
    unsigned offset = SwifiScanLe16(record + 116), bytes = SwifiScanLe32(record + 120);
    if (bytes && (offset < 128 || offset > n || bytes > n - offset)) return SwifiScanInvalid;
    b->Bssid = record + 8;
    b->Ssid = record + 19;
    b->SsidLength = record[18];
    b->Ies = bytes ? record + offset : 0;
    b->IeLength = bytes;
    b->Capability = SwifiScanLe16(record + 16);
    b->Interval = SwifiScanLe16(record + 14);
    b->Channel = record[88] ? record[88] : SwifiScanPrimary(version, SwifiScanLe16(record + 72));
    unsigned rssi = SwifiScanLe16(record + 78);
    b->Rssi = rssi & 0x8000 ? (int)rssi - 65536 : (int)rssi;
    b->Flags = record[96];
    return SwifiScanPartial;
}
#endif
