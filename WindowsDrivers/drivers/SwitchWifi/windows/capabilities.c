/* SPDX-License-Identifier: ISC */
#include "capabilities.h"
#include "../core/WifiEscan.h"
#include <netadaptercx.h>
#include <wificx.h>
#include "../core/phy_rates.h"

enum { MaxBands = 2, MaxBandPhys = 4, MaxWidths = 3, MaxPhys = 5 };
C_ASSERT(SWIFI_RATE_RX == WDI_DATA_RATE_RX_RATE && SWIFI_RATE_TX == WDI_DATA_RATE_TX_RATE);

struct SWIFI_RADIO_CAPS {
    SWIFI_FIRMWARE_INFO Identity;
    WIFI_DEVICE_CAPABILITIES Device;
    WIFI_STATION_CAPABILITIES Station;
    WIFI_BAND_CAPABILITIES Bands;
    WIFI_PHY_CAPABILITIES Phys;
    WIFI_BAND_INFO Band[MaxBands];
    WIFI_PHY_INFO Phy[MaxPhys];
    WDI_PHY_TYPE BandPhys[MaxBands][MaxBandPhys];
    WDI_CHANNEL_MAPPING_ENTRY Channels[MaxBands][SWIFI_MAX_CHANNELS];
    UINT32 Widths[MaxBands][MaxWidths];
    ULONG Published;
};

static NTSTATUS MakeBand(SWIFI_RADIO_CAPS *Caps, const SWIFI_FIRMWARE_INFO *Info,
    ULONG BandIndex, BOOLEAN FiveGhz)
{
    WIFI_BAND_INFO *Band = &Caps->Band[BandIndex];
    ULONG Widths = Info->Bandwidth[FiveGhz ? 1 : 0];
    Band->BandID = FiveGhz ? WDI_BAND_ID_5000 : WDI_BAND_ID_2400;
    Band->BandState = TRUE;
    Band->ValidPhyTypeList = Caps->BandPhys[BandIndex];
    if (FiveGhz)
        Band->ValidPhyTypeList[Band->NumValidPhyTypes++] = WDI_PHY_TYPE_OFDM;
    else {
        Band->ValidPhyTypeList[Band->NumValidPhyTypes++] = WDI_PHY_TYPE_HRDSSS;
        Band->ValidPhyTypeList[Band->NumValidPhyTypes++] = WDI_PHY_TYPE_ERP;
    }
    if (Info->Ht) Band->ValidPhyTypeList[Band->NumValidPhyTypes++] = WDI_PHY_TYPE_HT;
    if (FiveGhz && Info->Vht) Band->ValidPhyTypeList[Band->NumValidPhyTypes++] = WDI_PHY_TYPE_VHT;
    Band->ChannelWidthList = Caps->Widths[BandIndex];
    for (ULONG Bit = 0; Bit < MaxWidths; ++Bit)
        if (Widths & (1u << Bit)) Band->ChannelWidthList[Band->NumChannelWidths++] = 20u << Bit;
    Band->ValidChannelTypes = Caps->Channels[BandIndex];
    for (ULONG I = 0; I < Info->ChannelCount; ++I) {
        UCHAR Channel = Info->Channels[I];
        if ((Channel > 14) != FiveGhz) continue;
        WDI_CHANNEL_MAPPING_ENTRY *Entry = &Band->ValidChannelTypes[Band->NumValidChannelTypes++];
        Entry->ChannelNumber = Channel;
        Entry->ChannelCenterFrequency = SwifiChannelFrequencyMhz(Channel);
        if (!Entry->ChannelCenterFrequency) return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS AddPhy(SWIFI_RADIO_CAPS *Caps, WDI_PHY_TYPE WdiType, enum swifi_phy Type,
    ULONG Widths, const SWIFI_FIRMWARE_INFO *Info)
{
    swifi_phy_rate Rates[WDI_MAX_DATA_RATE_ENTRIES];
    WIFI_PHY_INFO *Phy = &Caps->Phy[Caps->Phys.NumPhyTypes];
    size_t Count = swifi_phy_rates(Type, Widths, Info->RxStreams, Info->TxStreams,
        Rates, RTL_NUMBER_OF(Rates));
    if (!Count) return STATUS_DEVICE_PROTOCOL_ERROR;
    Phy->PhyType = WdiType;
    Phy->NumberDataRateEntries = Count;
    for (size_t I = 0; I < Count; ++I) {
        Phy->DataRateList[I].DataRateFlag = Rates[I].directions;
        Phy->DataRateList[I].DataRateValue = Rates[I].half_mbps;
    }
    ++Caps->Phys.NumPhyTypes;
    return STATUS_SUCCESS;
}

static NTSTATUS Build(SWIFI_RADIO_CAPS *Caps, const SWIFI_FIRMWARE_INFO *Info)
{
    NTSTATUS Status;
    size_t Length = 0;
    ULONG Widths = 0;
    Caps->Identity = *Info;
    Caps->Device.Size = sizeof(Caps->Device);
    /* WLC_GET_RADIO bits 0 and 1 are software and hardware disable. Power
     * saving flags do not represent a physical switch. */
    Caps->Device.SoftwareRadioState = !(Info->RadioDisabled & 1);
    Caps->Device.HardwareRadioState = !(Info->RadioDisabled & 2);
    Caps->Device.NumRxStreams = Info->RxStreams;
    Caps->Device.NumTxStreams = Info->TxStreams;
    Caps->Device.BluetoothCoexistenceSupport = WDI_BLUETOOTH_COEXISTENCE_UNKNOWN;
    while (Length + 1 < sizeof(Caps->Device.FirmwareVersion) && Info->Version[Length]) ++Length;
    RtlCopyMemory(Caps->Device.FirmwareVersion, Info->Version, Length);

    WIFI_STATION_CAPABILITIES_INIT(&Caps->Station);
    /* The 22621/22631 WiFiCx 1.0 structure ends before the later MLO fields. */
    Caps->Station.Size = FIELD_OFFSET(WIFI_STATION_CAPABILITIES, MaxMultiLinkConnectionsSupported);
    Caps->Station.ScanSSIDListSize = SwifiScanSsids;
    /* This build implements station discovery. Publish authentication/key
     * pairs only together with their WDI association and key tasks. */
    Caps->Bands.Size = sizeof(Caps->Bands);
    Caps->Bands.BandInfoList = Caps->Band;
    Caps->Phys.Size = sizeof(Caps->Phys);
    Caps->Phys.PhyInfoList = Caps->Phy;
    for (ULONG Index = 0; Index < MaxBands; ++Index) {
        if (!(Info->Bands & (1u << Index))) continue;
        WIFI_BAND_INFO *Band = &Caps->Band[Caps->Bands.NumBands];
        Status = MakeBand(Caps, Info, Caps->Bands.NumBands, Index != 0);
        if (!NT_SUCCESS(Status)) return Status;
        /* A physically supported band may have no currently legal channels.
         * Keep it out of both the band and the PHY lists in that case. */
        if (!Band->NumValidChannelTypes) {
            RtlZeroMemory(Band, sizeof(*Band));
            continue;
        }
        Widths |= Info->Bandwidth[Index];
        ++Caps->Bands.NumBands;
        if (Index == 0) {
            Status = AddPhy(Caps, WDI_PHY_TYPE_HRDSSS, SWIFI_PHY_HRDSSS, SWIFI_WIDTH20, Info);
            if (!NT_SUCCESS(Status)) return Status;
            Status = AddPhy(Caps, WDI_PHY_TYPE_ERP, SWIFI_PHY_ERP, SWIFI_WIDTH20, Info);
        } else {
            Status = AddPhy(Caps, WDI_PHY_TYPE_OFDM, SWIFI_PHY_OFDM, SWIFI_WIDTH20, Info);
        }
        if (!NT_SUCCESS(Status)) return Status;
    }
    if (!Caps->Bands.NumBands) return STATUS_DEVICE_PROTOCOL_ERROR;
    if (Info->Ht) {
        Status = AddPhy(Caps, WDI_PHY_TYPE_HT, SWIFI_PHY_HT, Widths, Info);
        if (!NT_SUCCESS(Status)) return Status;
    }
    if (Info->Vht && Caps->Band[Caps->Bands.NumBands - 1].BandID == WDI_BAND_ID_5000)
        return AddPhy(Caps, WDI_PHY_TYPE_VHT, SWIFI_PHY_VHT, Info->Bandwidth[1], Info);
    return STATUS_SUCCESS;
}

static BOOLEAN SameCapabilities(const SWIFI_FIRMWARE_INFO *Old, const SWIFI_FIRMWARE_INFO *Info)
{
    return RtlEqualMemory(Old->Address, Info->Address, sizeof(Info->Address)) &&
        Old->Bands == Info->Bands && Old->Ht == Info->Ht && Old->Vht == Info->Vht &&
        Old->RxStreams == Info->RxStreams && Old->TxStreams == Info->TxStreams &&
        RtlEqualMemory(Old->Bandwidth, Info->Bandwidth, sizeof(Info->Bandwidth)) &&
        Old->ChannelCount == Info->ChannelCount &&
        RtlEqualMemory(Old->Channels, Info->Channels, Info->ChannelCount);
}

NTSTATUS SwifiPublishRadioCaps(WDFDEVICE Device, SWIFI_RADIO_CAPS **Slot,
    const SWIFI_FIRMWARE_INFO *Info)
{
    SWIFI_RADIO_CAPS *Caps = *Slot;
    NTSTATUS Status;
    if (!Caps) {
        WDF_OBJECT_ATTRIBUTES Attributes;
        WDFMEMORY Memory;
        WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
        Attributes.ParentObject = Device;
        Status = WdfMemoryCreate(&Attributes, NonPagedPoolNx, 'pFwS', sizeof(*Caps),
            &Memory, (PVOID *)&Caps);
        if (!NT_SUCCESS(Status)) return Status;
        RtlZeroMemory(Caps, sizeof(*Caps));
        Status = Build(Caps, Info);
        if (!NT_SUCCESS(Status)) {
            WdfObjectDelete(Memory);
            return Status;
        }
        *Slot = Caps;
    } else if (!SameCapabilities(&Caps->Identity, Info)) {
        /* WiFiCx may already have created the adapter for this WDFDEVICE.
         * Re-enumerate to publish different capabilities, rather than
         * changing the arrays behind a running adapter. */
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (Caps->Published < 1) {
        Status = WifiDeviceSetDeviceCapabilities(Device, &Caps->Device);
        if (!NT_SUCCESS(Status)) return Status;
        Caps->Published = 1;
    }
    if (Caps->Published < 2) {
        Status = WifiDeviceSetBandCapabilities(Device, &Caps->Bands);
        if (!NT_SUCCESS(Status)) return Status;
        Caps->Published = 2;
    }
    if (Caps->Published < 3) {
        Status = WifiDeviceSetPhyCapabilities(Device, &Caps->Phys);
        if (!NT_SUCCESS(Status)) return Status;
        Caps->Published = 3;
    }
    if (Caps->Published < 4) {
        Status = WifiDeviceSetStationCapabilities(Device, &Caps->Station);
        if (!NT_SUCCESS(Status)) return Status;
        Caps->Published = 4;
    }
    return STATUS_SUCCESS;
}
