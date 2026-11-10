/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_WINDOWS_REQUESTS_H
#define SWIFI_WINDOWS_REQUESTS_H

#include "capabilities.h"
#include <netadaptercx.h>
#include <wificx.h>

typedef struct SWIFI_WDI SWIFI_WDI;

/* DeviceAdd calls NetDeviceInitConfig and WifiDeviceInitConfig (in that
 * order) before creating Device. This binds WiFiCx to the real station
 * adapter factory and creates one passive command queue for the device. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiWdiCreate(WDFDEVICE Device, SWIFI_WIN_CONTROL *Control,
    SWIFI_FIRMWARE_INFO *Info, PFN_WIFI_DEVICE_CREATE_ADAPTER CreateAdapter,
    VOID (*Recover)(PVOID, NTSTATUS), PVOID RecoveryContext, SWIFI_WDI **Wdi);
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiWdiStart(SWIFI_WDI *Wdi);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID SwifiWdiRequestStop(SWIFI_WDI *Wdi);
_IRQL_requires_(PASSIVE_LEVEL)
VOID SwifiWdiQuiesce(SWIFI_WDI *Wdi);
VOID SwifiWdiAfterReset(SWIFI_WDI *Wdi);
/* The real station adapter supplies the port obtained from WifiAdapterGetPortId. */
VOID SwifiWdiStation(SWIFI_WDI *Wdi, WDI_PORT_ID Port, BOOLEAN Present);
BOOLEAN SwifiWdiFirmwareEvent(SWIFI_WDI *Wdi, const swifi_event_completion *Event);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID SwifiCompleteWdiRequest(WIFIREQUEST Request, NTSTATUS Status);
NTSTATUS SwifiAllocWdiMessage(WDFDEVICE Device, ULONG Length, WDFMEMORY *Memory,
    WDI_MESSAGE_HEADER **Buffer);

#endif
