/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWIFI_STATION_FILTER_H
#define SWIFI_STATION_FILTER_H
#include "station_data.h"
#include <TlvGeneratorParser.hpp>

NTSTATUS SwifiFilterCreate(WDFDEVICE Device, SWIFI_DEVICE *Owner);
VOID SwifiFilterRegister(NETADAPTER Adapter);
/* Called after WLC_UP and before the association request. */
NTSTATUS SwifiFilterStart(SWIFI_DEVICE *Device);
VOID SwifiFilterStop(SWIFI_DEVICE *Device, BOOLEAN RemovePort);
NTSTATUS SwifiFilterResetList(SWIFI_DEVICE *Device);
NTSTATUS SwifiFilterWdi(SWIFI_DEVICE *Device, UINT16 Id,
    TLV_CONTEXT *Tlv, WDI_MESSAGE_HEADER *Header, ULONG Bytes);
#endif
