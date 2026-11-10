/* SPDX-License-Identifier: GPL-2.0-only
 * Switchroot's SD 6.x/vendor-command capability gate and minimum sampling
 * window, shared by the T214 miniport and its host-side tests.
 */
#ifndef T214_DDR200_POLICY_H
#define T214_DDR200_POLICY_H

#define T214_DDR200_MIN_WINDOW 8u
#define T214_DDR200_TAPS 128u

static int T214Ddr200Supported(const unsigned char scr[8],
                             const unsigned char status[64])
{
    /* SCR is big endian: SD_SPEC[59:56], SD_SPEC3[47], SD_SPECX[41:38].
     * CMD6 group 2 support is [431:416], vendor function 14. Also require
     * SDR104 support, since that is our recovery operating mode.
     */
    unsigned int specx = ((unsigned int)(scr[2] & 3u) << 2) | (scr[3] >> 6);
    return (scr[0] >> 4) == 0 && (scr[0] & 15u) == 2 &&
           (scr[2] & 0x80u) && specx >= 2 &&
           (scr[1] & 4u) && (status[10] & 0x40u) && (status[13] & 8u);
}

static int T214Ddr200Window(const unsigned int bits[8], unsigned int count,
                          unsigned int *tap, unsigned int *width)
{
    unsigned int start = 0, run = 0, best = 0, bestStart = 0;
    *tap = *width = 0;
    if (count == 0 || count > 256) return 0;
    for (unsigned int i = 0; i < count; ++i) {
        if (bits[i / 32] & (1u << (i % 32))) {
            if (!run) start = i;
            ++run;
            if (run > best) { best = run; bestStart = start; }
        } else run = 0;
    }
    /* Do not merge the two endpoints: tap delay is not a circular counter.
     * A failing last tap must not enlarge a seven-tap window to eight.
     */
    *width = best;
    if (best < T214_DDR200_MIN_WINDOW) return 0;
    *tap = bestStart + (best - 1) / 2;
    return 1;
}

static unsigned int T214Ddr200CarDivider(unsigned int cardKhz)
{
    /* PLLC4_OUT0 is 998.4 MHz, CAR uses a 7.1 divider and SDHCI remains
     * divide-by-two in DDR mode. Change CAR, not the DDR SDHCI divisor.
     */
    if (cardKhz == 0 || cardKhz > 200000) return ~0u;
    unsigned int div = (998400u + cardKhz - 1) / cardKhz;
    return div >= 5 && div <= 257 ? div - 2 : ~0u;
}

#endif
