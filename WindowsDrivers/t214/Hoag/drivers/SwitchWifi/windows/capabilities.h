/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_WINDOWS_CAPABILITIES_H
#define SWIFI_WINDOWS_CAPABILITIES_H

#include "firmware.h"

typedef struct SWIFI_RADIO_CAPS SWIFI_RADIO_CAPS;

/* D0EntryPostInterruptsEnabled, after firmware interrogation and
 * WifiDeviceInitialize. Capability lists stay resident for the WDF device's
 * lifetime. PnP/power is the sole caller and serializes publication. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiPublishRadioCaps(WDFDEVICE Device, SWIFI_RADIO_CAPS **Caps,
    const SWIFI_FIRMWARE_INFO *Info);

#endif
