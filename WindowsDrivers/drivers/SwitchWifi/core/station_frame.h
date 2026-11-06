/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_STATION_FRAME_H
#define SWIFI_STATION_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* Convert one complete, decrypted infrastructure MSDU between WiFiCx's
 * native 802.11 layout and the BCM FullMAC Ethernet interface. In-place TX
 * is supported. Crypto, fragmentation and aggregation terminate in firmware.
 * A zero return means that this frame cannot be represented on this link. */
size_t swifi_station_tx(const uint8_t *wifi, size_t length, const uint8_t station[6],
    const uint8_t peer[6], uint8_t *ethernet, size_t capacity, uint8_t *priority);
size_t swifi_station_rx(const uint8_t *ethernet, size_t length, const uint8_t station[6],
    const uint8_t peer[6], uint16_t sequence, int protected_msdu, uint8_t *wifi, size_t capacity);

/* Normalize an 802.3 LLC/SNAP EAPOL frame in place before security filtering. */
size_t swifi_ether_eapol(uint8_t *frame, size_t length);

/* Accept only an explicitly clear, complete FromDS EAPOL frame from this AP.
 * Native protected payloads need verified decryption metadata before use. */
size_t swifi_clear_eapol(uint8_t *frame, size_t length, const uint8_t station[6], const uint8_t peer[6]);
#endif
