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

`core/msgbuf_ring.c` now connects that packet core to producer/consumer rings.
`windows/transport.c` provides the KMDF DMA common buffers, TCM or host-memory
indices, mailbox ISR, bounded DPC dispatch and interrupt quiescence. Each DPC
handles at most 32 entries per completion ring, rotating which ring goes first;
more work queues another DPC. Runtime traffic uses hardware interrupts, not a
periodic polling timer. Upper-stack notifications run outside the protocol lock.

These components are host-tested and compile to an ARM64 KMDF static library. There is
currently **no installable SwitchWifi.sys/INF and no Windows Wi-Fi option**.
WiFiCx device registration and firmware/shared-ring setup,
connection/key management and NetAdapterCx packet queues still need integration and hardware
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

## Windows transport integration

The platform must supply a real DMA-capable WDF device, its translated interrupt
resource, and a fixed PCIe-core register aperture. A mutable BAR0 backplane
window cannot be shared with the ISR. Use the WDF DMA enabler and its logical
addresses rather than deriving device addresses from virtual/physical pointers.
The experimental companion firmware establishes a direct AFI DMA domain before
Windows (_CCA=0, no Tegra SMMU/IORT remapper in this profile). The Windows
transport does not change ASIDs and requires 32-bit DMA logical addresses.

`SwifiWinCreateRing` allocates 64-byte-aligned common buffers and validates the
32-bit DMA range and initial indices. The platform publishes those allocations
in the firmware's shared descriptor table. Dynamic TX flow rings also use the
firmware control command. These steps are not performed by the library alone.

`SwifiWinCreateTransport` creates an ISR plus a separately cancellable KMDF DPC.
Mark it ready only after firmware and shared buffers are initialized, before
KMDF enables interrupts. The ISR masks/acknowledges owned causes; the DPC handles
mailbox/control/TX/RX work, unmasks when drained and checks for a pending cause
under the interrupt lock. Protocol handlers only queue completions while holding
`DataLock`; `Notify` delivers them afterward. A protocol fault disables further
submissions and schedules passive recovery without releasing DMA prematurely.

During D0 exit/recovery, stop upper queues, call `SwifiWinQuiesceInterrupts` to
mask the source and cancel/wait for the DPC, then stop/reset the endpoint. Return
owned packet buffers only after that reset. Delete ring buffers and transport
objects during resource release before unmapping BARs. The post-reset deletion
path does not touch endpoint registers.

## Board preparation and resource binding

The experimental companion build flag `SWITCH_WIFI_RUNTIME=1` adds `WFI0` /
`ACPI\SWOA0D01`. Home loads `\SwitchWOA\Wifi\WifiPrepare.efi` from its existing
read-only WINARM view. The application reuses the accepted B01 LDO1, PHY, link,
BAR and chip-discovery implementation; it leaves CR4 halted, bus mastering off,
endpoint/AFI interrupts masked and no firmware-owned host DMA allocations.
Preparation also enables only SPI 98's CPU route in legacy controller bank 3,
clearing its FIQ-class/forced state while AFI remains masked. GIC control stays
with Windows; preparation failure restores the inherited route enable/class.
Only after preparation succeeds does Home set the DSDT readiness byte, repair
the checksum and expose the device. An absent/failed application leaves WFI0
hidden and Windows boot continues. The shipped R5/R6 card setup is unchanged;
this new runtime profile is compiled locally and has not been deployed.

`platform/tegra_resources.h` is shared by the C driver and companion ASL:

| Resource | Base | Length / ID |
|---|---|---|
| Root ports, PADS and AFI | `0x01000000` | `0x4000` |
| Reprogrammable endpoint config window | `0x02000000` | `0x1000` |
| Outbound memory aperture | `0x13000000` | `0x0D000000` |
| Legacy INTx / AFI interrupt | GIC SPI 98 | GSI 130, level high |

The configuration aperture is the one programmed by the existing UEFI path;
Switchroot chooses a different CPU window (`0x11FFF000`) for the same AFI
translation. This device is a composite platform controller, not a standard
ECAM root bridge. `windows/resources.c` maps the host/config ranges and only
validated BAR slices inside the reserved outbound aperture. Its configuration
access uses Tegra's extended-register encoding for endpoint 1:0.0.

The ISR now acknowledges AFI's host reason and PME status as well as BCM mailbox
causes. Unexpected AFI errors retain their reason/signature, mask the host and
schedule recovery without further endpoint MMIO. MSI remains disabled. PME
acknowledgment does not establish suspend/resume or wake support.

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
RX offset/length bounds and exactly-once cancellation after reset. Ring tests
cover wrap/full boundaries, completion-prefix stripping, arrivals during a
bounded drain and a device-side model exchanging packets with the packet core.
They do not simulate KMDF or prove hardware interrupt/cache behavior.

The script compiles the portable core, transport and resource mapping with WDK/KMDF
1.33 headers, then archives `SwitchWifiTransport.lib` for ARM64. `--kmdf-include`
can specify separately extracted KMDF headers. This is not a linked, signed or
installed SYS driver, and the new runtime transport is not hardware-accepted.
The companion runtime Home firmware and WifiPrepare application also compile
for AARCH64. ACPICA evaluation returns WFI0 status 0 before preparation and
0x0F after the readiness-byte patch, with the three ranges and GSI above.
The full-table interpreter reported four outstanding cache allocations on exit;
the WFI0 evaluations themselves returned the expected values. No physical
handoff, Windows interrupt delivery or Windows network connection is claimed.

References: [Switchroot msgbuf](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/msgbuf.c),
[WiFiCx client model](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/writing-a-wificx-client-driver),
[KMDF common buffers](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfcommonbuffer/nf-wdfcommonbuffer-wdfcommonbuffercreate),
[DPC cancellation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdpc/nf-wdfdpc-wdfdpccancel).
