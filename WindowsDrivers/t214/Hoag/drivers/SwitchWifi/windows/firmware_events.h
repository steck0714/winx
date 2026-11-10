/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_WINDOWS_FIRMWARE_EVENTS_H
#define SWIFI_WINDOWS_FIRMWARE_EVENTS_H

#include "../core/msgbuf_control.h"

typedef struct SWIFI_SCAN SWIFI_SCAN;
typedef struct SWIFI_JOIN SWIFI_JOIN;
uint8_t SwifiScanFirmwareEvent(SWIFI_SCAN *scan, const swifi_event_completion *event);
uint8_t SwifiJoinEvent(SWIFI_JOIN *join, const swifi_event_completion *event);

/* The control-ring worker retains the DMA buffer across both calls. Escan
 * claims its own notifications. The association state machine consumes
 * SET_SSID, link loss, deauthentication and roam events on the same ring. */
static inline uint8_t SwifiRouteFirmwareEvent(SWIFI_SCAN *scan, SWIFI_JOIN *join,
    const swifi_event_completion *event)
{
    if (!event || event->firmware_status || event->ifidx)
        return 0;
    if (SwifiScanFirmwareEvent(scan, event))
        return 1;
    return SwifiJoinEvent(join, event);
}
#endif
