/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_WINDOWS_FIRMWARE_H
#define SWIFI_WINDOWS_FIRMWARE_H

#include "control.h"

#define SWIFI_BAND_2GHZ 1u
#define SWIFI_BAND_5GHZ 2u
#define SWIFI_MAX_CHANNELS 196u

typedef struct SWIFI_FIRMWARE_INFO {
    UCHAR Address[6];
    ULONG IoctlVersion;
    ULONG RadioDisabled;
    ULONG Bands;
    /* Firmware bandwidth bits: 20/40/80/160 MHz = 1/2/4/8. Index 0=2G, 1=5G. */
    ULONG Bandwidth[2];
    BOOLEAN Ht;
    BOOLEAN Vht;
    UCHAR RxStreams;
    UCHAR TxStreams;
    USHORT ChannelCount;
    UCHAR Channels[SWIFI_MAX_CHANNELS];
    CHAR Version[256];
    CHAR Features[512];
} SWIFI_FIRMWARE_INFO;

/* Firmware commands use an initialized, in/out buffer like brcmfmac. The
 * command number distinguishes a get from a set. Preserve firmware status;
 * firmware's unsupported and not-found replies allow optional capability
 * fallback. Transport failures propagate to the device lifecycle. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiFirmwareCommand(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex,
    ULONG Command, PVOID Buffer, USHORT Length, PUSHORT Returned, PSHORT FirmwareStatus);

/* Get carries the caller's initialized query bytes after name\0, and copies
 * the reply from byte zero. Returned is the bounded number of bytes copied.
 * Input and output DMA lengths include the name, matching brcmfmac. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiFirmwareIovarGet(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex,
    PCSTR Name, PVOID Data, USHORT Length, PUSHORT Returned, PSHORT FirmwareStatus);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiFirmwareIovarSet(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex,
    PCSTR Name, const VOID *Data, USHORT Length, PSHORT FirmwareStatus);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiFirmwareGetU32(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex,
    PCSTR Name, PULONG Value);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiFirmwareSetU32(SWIFI_WIN_CONTROL *Control, UCHAR IfIndex,
    PCSTR Name, ULONG Value);

/* Call after control startup/interrupt arming, before publishing WiFiCx
 * capabilities. This only queries the station interface. It does not enable
 * the radio, choose a country, configure keys, or claim OS feature support.
 * Info is cleared on failure, so partial results cannot become capabilities. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS SwifiFirmwareQueryInfo(SWIFI_WIN_CONTROL *Control, SWIFI_FIRMWARE_INFO *Info);

/* MHz, matching WDI_CHANNEL_MAPPING_ENTRY.ChannelCenterFrequency. */
ULONG SwifiChannelFrequencyMhz(UCHAR Channel);

#endif
