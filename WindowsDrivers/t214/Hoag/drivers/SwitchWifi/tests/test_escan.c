/* SPDX-License-Identifier: ISC */
#include "../core/WifiEscan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    SWIFI_SCAN_SPEC spec = {0};
    unsigned char out[SwifiScanWireMax], event[216] = {0};
    SWIFI_SCAN_BSS bss;
    unsigned status;
    memset(spec.Bssid, 255, 6);
    spec.ChannelCount = 3;
    spec.Channels[0] = 1; spec.Channels[1] = 36; spec.Channels[2] = 149;
    spec.SsidCount = 2;
    spec.Ssids[0].Length = 3;
    memcpy(spec.Ssids[0].Bytes, "lab", 3);
    spec.ActiveMs = 25;
    assert(SwifiEncodeScan(out, sizeof(out), 2, 0x5678, &spec) == 152);
    assert(!memcmp(out, "\x01\0\0\0\x01\0\x78\x56", 8));
    assert(!memcmp(out + 68, "\x03\0\x02\0\x01\x10\x24\xd0\x95\xd0\0\0", 12));
    assert(!memcmp(out + 80, "\x03\0\0\0lab", 7) && out[116] == 0);
    assert(out[51] == 0 && out[56] == 25 && SwifiScanLe32(out + 60) == 0xffffffff);
    assert(!SwifiEncodeScan(out, 151, 2, 1, &spec));
    assert(SwifiScanChanspec(1, 9) == 0x2b09 && SwifiScanChanspec(1, 36) == 0x1b24);
    assert(SwifiScanPrimary(2, 0xe02a) == 36 && SwifiScanPrimary(2, 0xe32a) == 48);
    SwifiEncodeScanAbort(out);
    assert(out[36] == 255 && out[42] == 2 && SwifiScanLe32(out + 60) == 1);
    assert(out[64] == 255 && out[65] == 255 && !out[66] && !out[67]);

    event[12] = 0x88; event[13] = 0x6c; event[20] = 0x10; event[21] = 0x18; event[23] = 1;
    event[31] = 69; event[35] = 8; event[47] = 144;
    unsigned char *r = event + 72, *b = r + 12;
    SwifiScanPut32(r, 144); SwifiScanPut16(r + 8, 7); SwifiScanPut16(r + 10, 1);
    SwifiScanPut32(b, 109); SwifiScanPut32(b + 4, 132);
    b[18] = 1; b[19] = 'x'; SwifiScanPut16(b + 72, 0xe02a); SwifiScanPut16(b + 78, 0xffbc);
    SwifiScanPut16(b + 116, 128); SwifiScanPut32(b + 120, 4); b[128] = 0; b[129] = 2;
    assert(SwifiDecodeScan(event, sizeof(event), 2, 7, &bss, &status) == SwifiScanPartial);
    assert(bss.Channel == 36 && bss.Rssi == -68 && bss.IeLength == 4 && bss.Ies == b + 128);
    assert(SwifiDecodeScan(event, sizeof(event), 2, 8, &bss, &status) == SwifiScanOther);
    SwifiScanPut32(b + 120, 5);
    assert(SwifiDecodeScan(event, sizeof(event), 2, 7, &bss, &status) == SwifiScanInvalid);
    event[35] = 4; event[47] = 12;
    assert(SwifiDecodeScan(event, 84, 2, 8, &bss, &status) == SwifiScanOther);
    event[47] = 0;
    assert(SwifiDecodeScan(event, 72, 2, 8, &bss, &status) == SwifiScanTerminal && status == 4);
    puts("BCM4356 escan: directed request, chanspecs, abort, BSS bounds and terminal ownership passed");
    return 0;
}
