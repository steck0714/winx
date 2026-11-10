/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWIFI_STATION_KEYS_H
#define SWIFI_STATION_KEYS_H
#include "station_data.h"
#include <TlvGeneratorParser.hpp>

typedef struct SWIFI_KEY_SLOT {
    UCHAR Key[16], Pn[6], Id;
    BOOLEAN Known, Present;
} SWIFI_KEY_SLOT;
typedef struct SWIFI_STATION_KEYS {
    SWIFI_KEY_SLOT Slot[5]; /* Default 0..3, peer key at 4. */
} SWIFI_STATION_KEYS;

BOOLEAN SwifiKeysHaveGroup(const SWIFI_STATION_KEYS *Keys);
NTSTATUS SwifiKeysClear(SWIFI_STATION_KEYS *Keys, SWIFI_DEVICE *Device, const UCHAR *Peer);
NTSTATUS SwifiKeysChange(SWIFI_STATION_KEYS *Keys, SWIFI_DEVICE *Device, const UCHAR Peer[6],
    TLV_CONTEXT *Tlv, WDI_MESSAGE_HEADER *H, ULONG Bytes, BOOLEAN Add, BOOLEAN *Uncertain);
#endif
