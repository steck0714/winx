/* SPDX-License-Identifier: ISC */
#include "../core/phy_rates.h"
#include <assert.h>
#include <stdio.h>

enum { Limit = 126 };

static unsigned directions(const swifi_phy_rate *rates, size_t count, unsigned value)
{
    for (size_t i = 0; i < count; ++i)
        if (rates[i].half_mbps == value) return rates[i].directions;
    return 0;
}

int main(void)
{
    swifi_phy_rate rates[Limit];
    size_t n = swifi_phy_rates(SWIFI_PHY_OFDM, 1, 2, 2, rates, Limit);
    assert(n == 8 && rates[0].half_mbps == 12 && rates[n - 1].half_mbps == 108);
    assert(!directions(rates, n, 22));
    n = swifi_phy_rates(SWIFI_PHY_ERP, 1, 2, 2, rates, Limit);
    assert(n == 12 && directions(rates, n, 22) == (SWIFI_RATE_RX | SWIFI_RATE_TX));
    n = swifi_phy_rates(SWIFI_PHY_HT, 1, 1, 1, rates, Limit);
    assert(n && rates[n - 1].half_mbps == 144);
    n = swifi_phy_rates(SWIFI_PHY_HT, 7, 1, 1, rates, Limit);
    assert(n && rates[n - 1].half_mbps == 300); /* HT stops at 40 MHz. */
    n = swifi_phy_rates(SWIFI_PHY_VHT, 1, 2, 2, rates, Limit);
    assert(n && rates[n - 1].half_mbps == 347 && !directions(rates, n, 385));
    n = swifi_phy_rates(SWIFI_PHY_VHT, 7, 2, 1, rates, Limit);
    assert(n && n < Limit && rates[n - 1].half_mbps == 1733);
    assert(directions(rates, n, 1733) == SWIFI_RATE_RX);
    assert(directions(rates, n, 867) == (SWIFI_RATE_RX | SWIFI_RATE_TX));
    for (size_t i = 1; i < n; ++i) assert(rates[i - 1].half_mbps < rates[i].half_mbps);
    assert(swifi_phy_rates(SWIFI_PHY_VHT, 7, 2, 1, rates, n - 1) == 0);
    puts("BCM4356 rate tables: legacy bands, HT/VHT width, asymmetric streams and WDI units passed");
    return 0;
}
