/* SPDX-License-Identifier: ISC */
#include "phy_rates.h"

static int add_rate(swifi_phy_rate *rates, size_t *count, size_t capacity,
    uint16_t value, uint8_t directions)
{
    size_t index = 0;
    while (index < *count && rates[index].half_mbps < value)
        ++index;
    if (index < *count && rates[index].half_mbps == value) {
        rates[index].directions |= directions;
        return 1;
    }
    if (*count == capacity)
        return 0;
    for (size_t end = *count; end > index; --end)
        rates[end] = rates[end - 1];
    rates[index].half_mbps = value;
    rates[index].directions = directions;
    ++*count;
    return 1;
}

size_t swifi_phy_rates(enum swifi_phy phy, uint32_t widths, unsigned rx_streams,
    unsigned tx_streams, swifi_phy_rate *out, size_t capacity)
{
    /* Switchroot's brcmfmac legacy rate set, in 500 kbit/s units. ERP
     * supports both CCK and OFDM. 5 GHz OFDM excludes the CCK rates. */
    static const uint8_t cck[] = {2, 4, 11, 22};
    static const uint8_t ofdm[] = {12, 18, 24, 36, 48, 72, 96, 108};
    static const unsigned data_subcarriers[] = {52, 108, 234};
    /* Coded bits per subcarrier for BPSK through 256-QAM, MCS 0..9. */
    static const unsigned numerator[] = {1, 1, 3, 2, 3, 4, 9, 5, 6, 20};
    static const unsigned denominator[] = {2, 1, 2, 1, 1, 1, 2, 1, 1, 3};
    size_t count = 0;
    if (!out || !capacity || !(widths & SWIFI_WIDTH20) ||
        !rx_streams || !tx_streams || rx_streams > SWIFI_MAX_NSS || tx_streams > SWIFI_MAX_NSS)
        return 0;
    if (phy == SWIFI_PHY_HRDSSS || phy == SWIFI_PHY_ERP)
        for (size_t i = 0; i < sizeof(cck); ++i)
            if (!add_rate(out, &count, capacity, cck[i], SWIFI_RATE_RX | SWIFI_RATE_TX))
                return 0;
    if (phy == SWIFI_PHY_OFDM || phy == SWIFI_PHY_ERP)
        for (size_t i = 0; i < sizeof(ofdm); ++i)
            if (!add_rate(out, &count, capacity, ofdm[i], SWIFI_RATE_RX | SWIFI_RATE_TX))
                return 0;
    if (phy != SWIFI_PHY_HT && phy != SWIFI_PHY_VHT)
        return count;

    const unsigned max_width = phy == SWIFI_PHY_HT ? 2 : 3;
    const unsigned mcs_count = phy == SWIFI_PHY_HT ? 8 : 10;
    for (unsigned width = 0; width < max_width; ++width) {
        if (!(widths & (1u << width)))
            continue;
        for (unsigned nss = 1; nss <= rx_streams || nss <= tx_streams; ++nss) {
            uint8_t directions = (nss <= rx_streams ? SWIFI_RATE_RX : 0) |
                                 (nss <= tx_streams ? SWIFI_RATE_TX : 0);
            for (unsigned mcs = 0; mcs < mcs_count; ++mcs) {
                unsigned coded = nss * data_subcarriers[width] * numerator[mcs];
                /* For this 2x2 chip, VHT MCS9 at 20 MHz has no valid symbol. */
                if (coded % denominator[mcs])
                    continue;
                unsigned bits = coded / denominator[mcs];
                /* Long/short GI symbols last 4.0/3.6 us. Express each rate
                 * in the closest half-Mbit/s unit required by WiFiCx. */
                unsigned long_gi = (bits + 1) / 2;
                unsigned short_gi = (bits * 5 + 4) / 9;
                if (!add_rate(out, &count, capacity, (uint16_t)long_gi, directions) ||
                    !add_rate(out, &count, capacity, (uint16_t)short_gi, directions))
                    return 0;
            }
        }
    }
    return count;
}
