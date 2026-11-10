/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_RX_METADATA_H
#define SWIFI_RX_METADATA_H

#include <stddef.h>
#include <stdint.h>

enum { SWIFI_RX_METADATA_SIZE = 128 };
typedef struct swifi_rx_metadata {
    int8_t rssi_dbm;
    uint8_t have_rssi, have_timestamp, other;
} swifi_rx_metadata;

/* BCM v5 D2H optional metadata starts with four header bytes, followed by
 * WLFC type/length/value records. Unknown records convey no crypto proof. */
int swifi_parse_rx_metadata(const uint8_t *bytes, size_t length, swifi_rx_metadata *result);
/* A decapsulated EAPOL is known clear only before the first key transition
 * crossed by this posted buffer. A native header has its own validation. */
int swifi_prekey_ethernet(uint32_t posted_epoch, uint32_t current_epoch,
    int posted_with_key, int current_with_key);
int swifi_signal_quality(int rssi_dbm);

/* Parse the common, documented v3/v4/v5 sta_info prefix. Speeds are kbps.
 * A direction without packets or without a measured rate is returned as 0. */
int swifi_station_rates(const uint8_t *sta, size_t length, const uint8_t peer[6],
    uint32_t *tx_kbps, uint32_t *rx_kbps);

#endif
