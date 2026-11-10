/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_PHY_RATES_H
#define SWIFI_PHY_RATES_H

#include <stdint.h>
#include <stddef.h>

enum swifi_phy {
    SWIFI_PHY_HRDSSS, SWIFI_PHY_ERP, SWIFI_PHY_OFDM, SWIFI_PHY_HT, SWIFI_PHY_VHT
};

enum {
    SWIFI_WIDTH20 = 1, SWIFI_WIDTH40 = 2, SWIFI_WIDTH80 = 4,
    SWIFI_MAX_NSS = 2, SWIFI_RATE_RX = 2, SWIFI_RATE_TX = 4
};

typedef struct swifi_phy_rate {
    uint16_t half_mbps;
    uint8_t directions;
} swifi_phy_rate;

/* BCM4356A3. Return the number of distinct rates in increasing order, or
 * zero if the inputs or output capacity cannot describe this PHY. */
size_t swifi_phy_rates(enum swifi_phy phy, uint32_t widths, unsigned rx_streams,
    unsigned tx_streams, swifi_phy_rate *out, size_t capacity);

#endif
