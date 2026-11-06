# Switch Wi-Fi

Windows Wi-Fi driver development for the BCM4356 PCIe device. Shared protocol
code lives here; any SoC-specific Windows platform code will stay under the
corresponding SoC directory. No firmware binary or board calibration is included.

## Current status

The companion firmware's R6 pre-Windows probe completed passive scanning on a
physical HDH-001 (six records, channels 1/9/36). It reset the Wi-Fi device and
released DMA before powering off. The normal `Lite Windows 11 Home` entry has
been restored. That test does **not** install a Windows network adapter.

`core/msgbuf_data.c` implements the packet-side msgbuf-v5 protocol required by
the Windows driver: asynchronous flow-ring creation/deletion, Ethernet TX,
RX buffer posting/completion, packet ownership and cancellation after endpoint
reset. It uses the same Broadcom wire protocol as Switchroot's brcmfmac. Firmware
status and TX status are retained for the OS layer; a completed TX descriptor
alone is not evidence of successful over-the-air delivery.

This component is host-tested and compiles to an ARM64 Windows object. There is
currently **no installable SwitchWifi.sys/INF and no Windows Wi-Fi option**.
WiFiCx device registration, Windows PCIe/DMA/interrupt ownership, connection/key
management and NetAdapterCx packet queues still need integration and hardware
acceptance. Do not add an empty network device or mark this driver as deployed.

Skipping the network page during OOBE only allows first setup to continue offline;
it does not create or disable the missing Wi-Fi driver.

## Packet ownership contract

- The platform owns DMA allocations, cache synchronization, producer/consumer
  indices and interrupts. `submit` copies a descriptor into the requested H2D
  ring and publishes it only after a release barrier. It must not report failure
  after publication: a later transport/doorbell failure requires reset instead.
- Calls and completion delivery are serialized, non-reentrant and nonblocking.
  All contexts, tables and buffers must be resident while hardware can DMA.
- Ring IDs are firmware H2D IDs: control 0, RX posting 1, TX flows 2 and above.
  Flow-ring storage and indices are zeroed before a create request. Flow DMA is
  retained until a matching delete completion or endpoint reset.
- TX payload DMA starts after the inline 14-byte Ethernet header. RX metadata
  buffers are disabled. The initial AFI mapping uses only 32-bit DMA addresses.
- Flow creation maps 802.1D priority to firmware FIFO (0/1/1/0/2/2/3/3);
  individual TX descriptors retain the original packet priority.
- Packets stay owned until a matching completion; ring-full retries leave
  ownership with the caller. A generation-tagged request ID rejects duplicate
  or late completions without releasing a newer packet.
- The transport strips the firmware completion-ring prefix before dispatch.
  RX packet offset uses the per-message value when nonzero, otherwise the
  firmware's shared RX offset. Bounds are checked before handing data upward.
- Stop bus mastering and reset/quiesce the endpoint **before** calling
  `swifi_data_stopped_after_reset`. It returns every remaining packet once;
  calling it does not itself stop DMA.

## Build and test

With LLVM clang and extracted/installed WDK headers:

```sh
python3 tools/check-core.py --cc clang \
  --wdk-include /path/to/WindowsKits/Include/10.0.26100.0 \
  --out /path/to/output
```

If the SDK headers were extracted separately, also pass `--sdk-include` with
their `Include/<version>` directory (the parent of `shared/specstrings.h`).

The native ASan/UBSan checks exercise independent wire fixtures, asynchronous
flow lifetime, ring-full rollback, table exhaustion, stale/wrong-ring replies,
RX offset/length bounds and exactly-once cancellation after reset. The script
also compiles `msgbuf_data.obj` for ARM64 Windows using the WDK kernel CRT headers.
It does not link, sign, install or exercise a Windows driver.

References: [Switchroot msgbuf](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/msgbuf.c),
[WiFiCx client model](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/writing-a-wificx-client-driver).
