/* SPDX-License-Identifier: ISC */
#include "rx_metadata.h"
#include <string.h>

int swifi_parse_rx_metadata(const uint8_t *bytes, size_t length, swifi_rx_metadata *result)
{
    if (!result) return 0;
    swifi_rx_metadata parsed = {0};
    *result = parsed;
    if (!length) return 1;
    if (!bytes || length < 4 || length > SWIFI_RX_METADATA_SIZE) return 0;
    for (size_t at = 4; at < length;) {
        unsigned type = bytes[at++];
        if (!type || type == 255) break; /* End/padding in D2H metadata. */
        if (at == length) return 0;
        unsigned count = bytes[at++];
        if (count > length - at) return 0;
        if (type == 8) {
            if (count != 1) return 0;
            int value = bytes[at] < 128 ? bytes[at] : (int)bytes[at] - 256;
            if (value >= -127 && value < 0) {
                parsed.rssi_dbm = (int8_t)value;
                parsed.have_rssi = 1;
            }
        } else if (type == 16) {
            parsed.have_timestamp = 1;
        } else {
            parsed.other = 1;
        }
        at += count;
    }
    *result = parsed;
    return 1;
}

int swifi_prekey_ethernet(uint32_t posted_epoch, uint32_t current_epoch,
    int posted_with_key, int current_with_key)
{
    return posted_epoch == current_epoch && !posted_with_key && !current_with_key;
}

int swifi_signal_quality(int dbm)
{
    return dbm <= -100 ? 0 : dbm >= -50 ? 100 : 2 * (dbm + 100);
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static unsigned le16(const uint8_t *p) { return p[0] | (unsigned)p[1]<<8; }

int swifi_station_rates(const uint8_t *s, size_t size, const uint8_t peer[6],
    uint32_t *tx, uint32_t *rx)
{
    if (!tx || !rx) return 0;
    *tx = *rx = 0;
    if (!s || !peer || size < 76) return 0;
    unsigned version = le16(s), length = le16(s+2);
    if (version < 3 || version > 5 || length < 76 || memcmp(s+16,peer,6)) return 0;
    /* brcmf_sta_info_le: packet counts at 52/60/64, rates at 68/72. */
    uint32_t tx_rate = le32(s+68), rx_rate = le32(s+72);
    if (le32(s+52) && tx_rate <= 1000000) *tx = tx_rate;
    if ((le32(s+60) || le32(s+64)) && rx_rate <= 1000000) *rx = rx_rate;
    return *tx != 0 || *rx != 0;
}
