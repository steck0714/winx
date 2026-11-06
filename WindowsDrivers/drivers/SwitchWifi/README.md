# Switch Wi-Fi

Windows Wi-Fi driver development for the BCM4356 PCIe device. Shared protocol
code lives here; any SoC-specific Windows platform code will stay under the
corresponding SoC directory. No firmware binary or board calibration is included.

## Current status

The companion firmware's R6 pre-Windows probe completed passive scanning on a
physical HDH-001 (six records, channels 1/9/36). It reset the Wi-Fi device and
released DMA before powering off. The normal `Windows 11 Lite` entry has
been restored. That test does **not** install a Windows network adapter.

`core/msgbuf_data.c` implements the packet-side msgbuf-v5 protocol required by
the Windows driver: asynchronous flow-ring creation/deletion, Ethernet TX,
RX buffer posting/completion, packet ownership and cancellation after endpoint
reset. It uses the same Broadcom wire protocol as Switchroot's brcmfmac. Firmware
status and TX status are retained for the OS layer; a completed TX descriptor
alone is not evidence of successful over-the-air delivery.

`core/msgbuf_control.c` adds serialized asynchronous IOCTLs and eight retained
event buffers. A command publishes its reply-buffer descriptor and request in
one ring update; ring-full retries cannot orphan the reply buffer. Matching
transaction IDs, response IDs and command numbers distinguish completions from
stale messages. Timeouts stop reuse until endpoint recovery.

`core/msgbuf_ring.c` now connects that packet core to producer/consumer rings.
`windows/transport.c` provides the KMDF DMA common buffers, TCM or host-memory
indices, mailbox ISR, bounded DPC dispatch and interrupt quiescence. Each DPC
handles at most 32 entries per completion ring, rotating which ring goes first;
more work queues another DPC. Runtime traffic uses hardware interrupts, not a
periodic polling timer. Upper-stack notifications run outside the protocol lock.

The ARM64 `switchwifi.sys` targets the opt-in WFI0 device. Windows PE
22621.2861 bound the signed 0.5.0.0 package to an ACPI-enumerated instance,
ran its isolation method and registered its interrupt. The fixture supplies an
interrupt and omits Tegra MMIO, so preparation stops at the memory check.
The station implements NetAdapterCx packet queues, radio, scan/abort, open and
WPA2-Personal association, and CCMP key operations. The physical Lite retains
its BasicDisplay boot profile. Windows networking on that board awaits testing.
Skipping networking during OOBE only affects first setup.

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
- TX payload DMA starts after the inline 14-byte Ethernet header. Each RX post advertises a separate 128-byte metadata buffer. Firmware may leave its length at zero. The initial AFI mapping uses 32-bit DMA addresses.
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

## Control and event ownership

- The same `DataLock` serializes command, event and packet operations. A command
  is active until a matching completion or its deadline. A successful request
  ACK does not complete it or release either DMA buffer.
- The owner supplies a deadline timer. `swifi_control_timeout` identifies the
  active request, completes it once and stops new commands/event posts. A late
  reply cannot authorize reuse; recovery must stop/reset the endpoint first.
- Command callbacks copy the bounded response before returning. The original
  firmware length and signed status remain available to the OS translation.
  Command replies start at byte zero, independently of the event RX offset.
- Event callbacks may retain a buffer. It is not posted again until the consumer
  calls `swifi_control_release_event`; a generation-tagged ID rejects duplicate
  or stale completions. Refill retries preserve all unpublished slots.
- After reset, command cancellation occurs once and posted event buffers become
  available. Already delivered events remain retained until their consumers
  release them. Cancel/join deadline callbacks and drain those consumers before
  deleting DMA allocations or reinitializing the protocol.

## Windows transport integration

The platform must supply a real DMA-capable WDF device, its translated interrupt
resource, and BAR0 pinned to the PCIe core before interrupts start. No backplane
window changes are allowed until interrupt quiescence. Use the WDF DMA enabler and its logical
addresses rather than deriving device addresses from virtual/physical pointers.
The experimental companion firmware establishes a direct AFI DMA domain before
Windows (_CCA=0, no Tegra SMMU/IORT remapper in this profile). The Windows
transport does not change ASIDs and requires 32-bit DMA logical addresses.

`SwifiWinCreateRing` allocates 64-byte-aligned common buffers and validates the
32-bit DMA range and initial indices. `SwifiSharedPrepare` publishes those
allocations in the firmware's shared descriptor table. Dynamic TX flow rings
use the firmware control command.

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
access uses Tegra's extended-register encoding for endpoint 1:0.0. The driver
matches bus addresses from the raw PnP list, then maps CPU addresses from the
corresponding translated list. BAR addresses are offsets within this aperture.

During hardware preparation, KMDF receives its own raw and translated interrupt
descriptors directly from the resource lists. The driver binds the register and
ring pointers later, while both the host and chip interrupts are masked. Before
starting the chip it selects legacy INTx, disables any enabled MSI/MSI-X
capability and checks the PCI command and control bits by reading them back.
The Home preparation application enables memory decoding with bus mastering
off. DMA starts only after the ring addresses are installed.
[KMDF interrupt descriptors](https://github.com/microsoft/Windows-Driver-Frameworks/blob/main/src/framework/shared/irphandlers/pnp/interruptobject.cpp),
[Switchroot Tegra PCIe host](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/pci/host/pci-tegra.c).

The ISR now acknowledges AFI's host reason and PME status as well as BCM mailbox
causes. Unexpected AFI errors retain their reason/signature, mask the host and
schedule recovery without further endpoint MMIO. MSI remains disabled. PME
acknowledgment does not establish suspend/resume or wake support.

## Windows chip and firmware initialization

`windows/chip.c` maps/discovers the prepared BCM4356 endpoint, reads BAR sizes
with memory decoding/bus mastering disabled, rejects overlapping/out-of-range
BARs and identifies CR4/D11/PCIe cores from AI EROM. The EROM and NVRAM parsers
now live once in `core/WifiErom.h` and `core/WifiNvram.h`; the companion UEFI
includes those exact files through its existing header names. Their parser
behavior and the accepted R6 firmware format are unchanged.

The loader reads the privately supplied firmware and matching board file in a
system worker at PASSIVE_LEVEL, honors the optional RAMS size, writes aligned
TCM words and the NVRAM token, releases CR4 and waits up to five seconds for the
shared-area handshake. It accepts shared protocol v5 and retains the negotiated
RAM, RX and ring-info fields for the runtime session. Bus mastering remains
disabled until the caller publishes synchronized host DMA allocations.

At the end of startup the loader pins BAR0 to the PCIe core, as brcmfmac does;
`SwifiChipRuntimeRegisters` returns that base. It does not assume that an unused
BAR0+0x2000 constant provides an independent register alias. Backplane accesses
are rejected while pinned. After interrupt quiescence, stop disables bus
mastering, halts CR4 and holds D11 reset before permitting DMA teardown. If stop
fails, the owner must reset the platform endpoint before freeing DMA memory.

This code has compiled, but Windows device callbacks, WiFiCx request dispatch and
the WiFiCx interface have not yet been bound to it. Windows firmware startup
has not been physically tested. Firmware binaries, board data and keys are
not included in the repository or static library.

## Shared firmware session

`windows/shared.c` allocates the five common msgbuf rings, optional host-index
arrays, scratch/update areas, one command input/reply pair and eight event
buffers through the WDF DMA enabler. TCM index arrays are range-checked before
mapping. Both host-memory and TCM slots use a 16-bit counter, including when
the negotiated stride is four bytes, matching Switchroot's index access.

All allocations and initial indices are checked with bus mastering disabled
before their HAL logical addresses are published. Pointer/dimension readback
finishes before DMA can be enabled. A failure after publication retains the
allocations for reset/cleanup. Host-ready notification follows bus-master
enable; interrupt arming remains the device owner's next lifecycle step.
Flow-ring buffers are allocated on demand and submitted with the ordinary
asynchronous flow-create command, then freed after delete completion or reset.

This shared-session implementation currently supports the fixed v5 completion
layout with RX prefix zero. It rejects a nonzero prefix rather than truncating
the 16-byte TX completion. A different layout requires its actual firmware
descriptor-size contract before use; no alternative layout is hardware-accepted.

Device mailbox reading/clearing is nonblocking in the DPC. The passive send
helper waits at most one second for a previous host mailbox transaction and
uses the PCIe-core revision-dependent double SBMBX notification. This supports
the owner's later deep-sleep/power state machine; it does not itself implement
Windows suspend/resume. No periodic runtime polling loop is introduced.

## Windows command execution and event workers

`windows/control.c` connects the portable control engine to the KMDF transport.
`SwifiControlExecute` runs in a passive caller/command worker, serializes one
firmware IOCTL and waits on completion or stop events with a two-second deadline.
Ring backpressure waits for DPC progress rather than polling. Caller input is
staged in resident memory before the submission lock; the DPC copies replies
into resident storage, and the passive caller copies bounded results outward.
Firmware status and original response length are retained.

The WiFiCx SendCommand callback can run at DISPATCH_LEVEL. The WIFIREQUEST
worker in `windows/requests.c` queues to PASSIVE_LEVEL before calling Execute. Its event and mailbox work items have automatic serialization
disabled and use protocol-lock-protected queue ownership. Upper event callbacks
run without the protocol lock, may issue a subsequent command, and must copy
borrowed data before returning. Only then is the event DMA buffer reposted.

The device owner must wire this lifecycle in order:

1. Create the transport and control objects, prepare shared buffers, and bind a
   fresh control session before enabling interrupts.
2. After transport Ready/Armed, call `SwifiControlStart` from
   D0EntryPostInterruptsEnabled to prime events and allow capability queries.
3. Route D2H control messages and mailbox flags to this module. Pass unhandled
   packet/flow completions to the data core. Call `SwifiControlNotify` outside
   DataLock, before completing OS packets.
4. On stop, first request control cancellation, then quiesce/join the transport
   DPC. Quiesce the control module to flush event/mailbox work and join command
   callers. This wakes pending commands immediately without waiting for their
   normal deadline; it does not release hardware-owned DMA.
5. Stop/reset the chip, then call `SwifiControlStoppedAfterReset` to cancel any
   remaining firmware ownership. Delete/rebind buffers only after this step.

Timeout/completion races are resolved under DataLock. Timeout blocks buffer
reuse until recovery; a concurrent normal stop suppresses a second recovery
request. The recovery callback only queues owner work and must not reset or
flush workers inline. The kernel worker/lifecycle code is cross-compiled and
not yet hardware-tested. It does not establish Windows shutdown or resume.

## Device startup, power and radio capabilities

`windows/lifecycle.c` maps the ACPI resources in PrepareHardware. A system
worker loads the board files and publishes the shared rings before the callback
creates the KMDF interrupt. D0 entry enables DMA. Once interrupts are armed,
`windows/firmware.c` queries the station MAC, firmware version, radio state,
allowed channels, bandwidth and stream counts. The channel list comes from
firmware for the current regulatory configuration. A transport timeout aborts
startup. Only explicit firmware replies indicating a missing or unsupported
optional query permit a capability fallback.

On exit the owner cancels commands, joins interrupt and passive work, then
halts the endpoint before releasing DMA buffers. If the normal stop fails,
the board fallback holds the Wi-Fi port in reset and checks for link-down.
If the endpoint and port both fail to acknowledge isolation, the WFI0
`WIRS` method asserts PCIEX, PEX and AFI through Tegra CAR and reads back
their reset bits. The driver checks for this method before enabling DMA.
The last-resort reset and physical D0 teardown still require board testing.
A successful fallback requires a fresh firmware boot for the next session.
After control startup, `windows/capabilities.c` calls the WiFiCx device, band
and PHY capability setters. It supplies the firmware’s sorted legal channel
list and reports center frequencies in MHz. The BCM4356 silicon supports up
to two streams and 20/40/80 MHz on 5 GHz. The 2.4 GHz band uses at most 40 MHz.
The HT and VHT rate tables encode short and long guard intervals in WDI units
of 500 kbit/s. Entries carry independent transmit and receive flags. The rate
fixtures cover the 2x1 case, invalid 20 MHz MCS9 combinations and the maximum
867 Mbit/s class PHY rate. These are capability tables. Throughput has yet to
be measured on Windows.

The WDF device owner must call `NetDeviceInitConfig` followed by
`WifiDeviceInitConfig` before `WdfDeviceCreate`. `SwifiHardwareCreate` then
registers `WifiDeviceInitialize` with the owner's station-adapter factory and
creates a passive command worker. The worker parses `WDI_TASK_SET_RADIO_STATE`
with the WDK WiFiCx TLV library and the OS's negotiated WDI version. It saves
the task header before completing M3, updates only the firmware's software
radio-disable bit, queries the actual radio states and sends the radio status
and task completion to Windows. Stop wakes pending firmware commands and joins
this worker before clearing the radio state. The scan and abort handlers are described below.

The static library includes the radio path and the allocation hooks used by
`WificxTLVGenParse.lib`. No card deployment or Windows radio toggle is claimed.
[WiFiCx command handling](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/writing-a-wificx-client-driver),
[WiFiCx radio task](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-wdi-task-set-radio-state),
[BCM4356 radio capabilities](https://www.infineon.com/part/CYW4356).

## Windows station scans

The station adapter supplies its WiFiCx port ID to `SwifiWdiStation` after
initialization and removes it during teardown. The scan handler accepts
`WDI_TASK_SCAN` and its matching `WDI_ABORT_TASK` on that port. It parses
the WDK TLVs, prepares the BCM4356 escan command and completes M3 after the
firmware accepts it. A passive lock orders M3 before early firmware events.
The firmware path honors directed BSSIDs, up to ten SSIDs, channel lists,
dwell times, scan repeat count and the host time limit. An active-only request
filters restricted, passive and radar channels using the firmware’s per-channel
information. Probe-request vendor IEs and the prior MPC setting are restored
at the end of the task.

UEFI and Windows share the escan v1 layout and v109 result decoder. Windows
turns the BSS interval, capability and received information elements into a
WDI management-frame body. The chipset’s v109 summary omits the AP timestamp,
so the TSF field is zero. Live updates are batched at three records or 500 ms.
An abort sends the brcmfmac scan-abort command and waits for a terminal event
before reusing the scanner. A missing terminal requires device recovery because
some terminal events lack a sync ID. D0 exit joins the scan timer and returns
any accepted WDI task. Physical WDI scan delivery on HDH-001 is pending.

[Microsoft WDI frame-body example](https://github.com/microsoft/Windows-driver-samples/blob/main/network/wlan/WDI/PLATFORM/NDIS6/WDI_Common.c),
[Windows scan task](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-wdi-task-scan),
[Switchroot escan](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/cfg80211.c).

## Windows station queues

`windows/device.c` owns the KMDF PnP/power callbacks. WiFiCx calls
`windows/adapter.c` to create and start one station with the firmware MAC
and the assigned port ID. It starts disconnected. A net ring provides
system-managed buffers in each direction. Private 32-bit DMA buffers isolate
the lifetime of a Windows packet from a firmware completion. Canceling a
queue returns its Windows buffers while firmware-owned storage survives
until completion or confirmed reset.

`windows/station_data.c` posts and completes packets through the shared
msgbuf core. QoS traffic maps to four firmware FIFO flows when available.
A closed flow can be reused only after both indices have been reset.
Queue notifications run outside the protocol lock. `core/station_frame.c`
translates native 802.11 station MSDUs to and from the chip’s Ethernet
payloads. Fixtures cover EAPOL, QoS, multicast, length-framed LLC, RFC1042
and bridge-tunnel SNAP. The TX descriptor carries the WiFiCx per-packet encryption exemption. The station advertises open infrastructure and WPA2-Personal with CCMP-128.

[WiFiCx station initialization](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/writing-a-wificx-client-driver),
[NetAdapterCx packet queues](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/transmit-and-receive-queues),
[Receive MSDU format](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/wdi-receive-operations-and-offloads).

## Association and keys

The Windows station selects only host-listed BSSIDs and legal channels.
It checks the SSID, RSN suites and management-protection requirements in
the supplied beacon or probe response. When Windows omits the frame, it can
reuse a recent result that this port actually reported on the same channel.
The device holds at most 64 KiB of these results for up to one minute and
clears them on port removal or D0 exit.

`windows/join_firmware.c` configures the BCM4356 for Windows-owned authentication,
uses a directed join, and verifies the selected BSSID, SSID, negotiated RSN
request and association response from the chip. If Windows provides a PMKID,
the request and readback must include it. Each attempted BSSID gets an
association-result indication. The connect task completes on verified
association, while Windows handles EAPOL and supplies pairwise/group keys.
A disconnect, abort or failed readback gates traffic. The driver waits for
the firmware disconnect event and an empty BSSID before reusing an aborted
attempt. A missing fence triggers device recovery.

`windows/station_keys.c` validates each key batch before programming the
Broadcom `wsec_key` layout. It waits for submitted EAPOL frames, preserves
firmware packet counters on an idempotent reinstall, and removes keys on
disconnect. An ambiguous partial key change ends the session. RX requires
firmware cleartext rejection for secured data. Once a receive key is active,
an Ethernet-format EAPOL completion can lack the original 802.11 Protected
bit. The driver drops that ambiguous frame. A verified clear native EAPOL
frame can use the matching Windows exemption. Repeated handshakes and group
rekeys need a board trace before longer connections can be accepted.

[Switchroot association and keys](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/cfg80211.c),
[Broadcom TX exemption flags](https://android.googlesource.com/kernel/common/+/bcmdhd-3.10/drivers/net/wireless/bcmdhd/include/bcmmsgbuf.h),
[Broadcom association frame layout](https://android.googlesource.com/platform/hardware/broadcom/wlan/+/ac2bc7e7aaad957f235992ff74d5f9af34dc5f88/bcmdhd/dhdutil/include/wlioctl.h),
[Microsoft connect task](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-wdi-task-connect),
[Microsoft key parameters](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/wdi-tlv-set-cipher-key-info).

## Receive metadata and link measurements

Each v5 RX descriptor carries separate DMA addresses for the packet and its
optional sideband. A completion may return zero metadata bytes. The driver
checks both lengths against the original post. It decodes the signed WLFC
RSSI byte only from a complete metadata record. The timestamp tag and unknown
records convey no evidence of over-the-air encryption.
[Broadcom RX descriptor](https://android.googlesource.com/kernel/common/+/bcmdhd-3.10/drivers/net/wireless/bcmdhd/include/bcmmsgbuf.h) ·
[Broadcom metadata parser](https://android.googlesource.com/kernel/common/+/bcmdhd-3.10/drivers/net/wireless/bcmdhd/dhd_msgbuf.c).

An Ethernet EAPOL can be treated as clear only when the RX buffer was posted
and completed before a key transition. A buffer that spans a key change, was
posted with any live key, or becomes ambiguous before Windows dequeues it
is dropped. A native clear EAPOL must match the peer and frame layout.
A protected native data header identifies a frame type only. Its inner payload
and its authentication status cannot be inferred from that observation.

The passive link worker uses the associated peer's `sta_info` for TX and RX
rates. It can fall back to `WLC_GET_RATE` for TX and `WLC_GET_RSSI` for
signal when fresh RX metadata is unavailable. Unknown directions stay unknown
in NetAdapterCx. WiFiCx receives a link indication when both directions and a
quality measurement are available. Refreshes run at association and at most
once per five seconds of subsequent receive activity. The worker drains
before device release. [Microsoft link indication and units](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/wdi-tlv-link-state-change-parameters).

### Receive filters

The station advertises directed, broadcast, listed multicast and all-multicast
reception before calling `NetAdapterStart`. NetAdapterCx filter changes and
the two WDI port commands use the same receive policy. The driver retains up to
32 multicast addresses per port and programs `mcast_list` and `allmulti`
after `WLC_UP`, before requesting association. Firmware that rejects a
requested list can deliver all multicast while the driver enforces the
Windows list in software. Broadcast has its own flag. Changing ports clears
the policy, and an 802.11 reset clears the multicast list. A resumed station
reapplies its current policy before joining.

RX checks the destination at completion and again when Windows takes the frame.
A queued protected frame can no longer cross a key change, and a queued clear
EAPOL must still meet the current exemption. Raw monitor and promiscuous capture
remain outside the station capabilities. See the [NetAdapterCx filter contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/netadapter/ne-netadapter-net_packet_filter_flags)
and [Switchroot's BCM multicast initialization](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/core.c).

### On-device status

The driver writes `SwitchWifiReport` under its PnP device parameters. Read it
in a running Windows session with:

```cmd
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0D01" /s /v SwitchWifiReport
```

`step` identifies device creation (1), hardware preparation (2), D0 entry (3),
radio online (4) or station startup (5). `failure` retains the first failure
in the preparation session, including runtime (6) and stop (7), with its
NTSTATUS. Scan and connect fields count finished or rejected requests;
`keys` is the last acknowledged slot mask (GTK bits 0–3, PTK bit 4).
`eth_pre`, `eth_unknown` and `native_clear` count EAPOL observations.
`native_protected`, `meta`, `rssi` and `malformed` count datapath starts in
which each kind was first observed. The four `filter` fields hold the requested flag mask, address count, firmware
mode and NTSTATUS. Modes 0 through 4 mean pending, hardware list, software-filtered
wide reception, requested all-multicast and no requested group addresses, respectively. A passive work item saves a single value
on first observations, state changes and teardown. The report contains counters and status codes. Device addresses, network names, payloads and keys stay private. Windows association, DHCP and group rekey still need a board
result.

Version 5 adds `hw=stage/NTSTATUS` for isolation (1), interrupt registration
(2), memory windows (3), DMA adapter (4), endpoint discovery (5), legacy INTx
selection (6), BCM firmware (7), shared rings (8), and control path (9).
`irq=1` records an interrupt claimed by the hardware ISR during the current
D0 session. `ring=1` records at least one consumed device-to-host entry.
Software rearming alone leaves `irq` at zero.

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
Control fixtures cover atomic paired submission, ACK/completion distinction,
stale command/response IDs, truncated responses, timeout/reset cancellation,
event offsets and retained-buffer backpressure. The Windows shared-session
allocator, mailbox helpers and Windows command/event workers are cross-compiled,
not simulated by these tests.
Shared EROM/NVRAM fixtures also cover extended/truncated descriptors, board-file
packing and the RAMS aperture. They do not simulate KMDF or prove hardware
interrupt/cache behavior.

The build uses KMDF 1.33, NetAdapterCx 2.2 and WiFiCx 1.0. After the checks,
link the ARM64 native driver with the class-extension and WDI TLV libraries:

```sh
python3 tools/link-driver.py --objects /path/to/output \
  --kit /path/to/WindowsKits/10 --out /path/to/package
```

Use the matching WDK ARM64 libraries under `Lib/10.0.26100.0/km/ARM64`
and `Lib/wdf/kmdf/ARM64/1.33`. Put the signed SYS beside `package/switchwifi.inf` and
generate and sign the catalog using `package/switchwifi.cdf` after signing
the SYS. Use the exact INF bytes that generated the catalog.

The `0.5.0.0` test package targets ARM64 Windows 22621 or newer. The
Stage 6 installer accepts `Install-LiteSystem.cmd online wifi` and
`Install-LiteSystem.cmd offline W:\ wifi`. Place the signed `switchwifi`
package, matching `WifiPrepare.efi` and a `Firmware` directory beside the
script. Supply this board's `brcmfmac4356A3-pcie.bin` and
`brcmfmac4356A3-pcie.txt` privately. This mode installs just the wireless
package. It copies the board files under `Windows/System32/drivers/SwitchWOA`
and the EFI program to `SwitchWOA/Wifi` on the Windows volume. A complete existing board pair is preserved. A fresh install prepares
both files together and can finish an interrupted move on the next run. The experimental runtime BL33 is
a separate firmware update.

A cardless ARM64 Windows PE 22621.1 guest accepted the signed 0.3.0.0 package
as `oem0.inf` through the Wi-Fi-only installer. A retry completed a simulated
interruption between the two board-file moves. Readback matched both source
files and `WifiPrepare.efi`.

### ARM64 Windows load regression

In an isolated ARM64 Windows PE 22621.2861 guest, the Net setup class,
WiFiCx and NetAdapterCx were present when the driver was loaded. A root
fixture supplied the hardware ID `ACPI\SWOA0D01` and had no Switch platform
methods or PCIe radio. The earlier image staged successfully, then failed to
load with PnP problem 39 and status `0xC0000018`. The Wi-Fi linker now applies
the same resident section flags as the other Switch kernel drivers, including
a writable WDF class-binding data section and discardable INIT code.

With the signed 0.4.2.0 package, Windows selected `switchwifi` and invoked
device preparation. The actual driver report contained
`step=2/C0000010 failure=2/C0000010`. The root node rejected the first
Switch-specific ACPI request (`WIRS`), producing the expected PnP problem 10.
The hardware-key report persisted through failed device startup. The 0.4.2.0 INF
initialized the value to `v=4 pending` only if absent. That initial value
alone does not indicate that the driver ran.

To reproduce this boundary in a disposable ARM64 guest with the inbox Net
setup class and test signing, stage the signed package with PnPUtil and run
`devcon install switchwifi.inf "ACPI\SWOA0D01"` using the ARM64 WDK tool.
Read `SwitchWifiReport` from the created `ROOT\NET` instance. On the
physical Lite, Windows enumerates the real ACPI instance and the next stages
exercise the Tegra resources and BCM firmware.

### ACPI interrupt fixture (0.5.0.0)

An isolated Windows PE 22621.2861 guest enumerated `ACPI\SWOA0D01\0` from an
SSDT with the board's `WIRS` method, backed by guest memory. Its resources
include the production level/high, exclusive GSI 130 interrupt and omit the
three PCIe memory ranges. The generic Net setup class was available before
enumeration. Updating the enumerated device selected the signed package.
Windows ran `WIRS`, registered the interrupt and reported
`hw=3/C0000182 irq=0 ring=0` when it checked for those memory ranges.
The fixture exercises resource registration. Only a real Lite can exercise
the endpoint, deliver a hardware interrupt and return traffic through the rings.

The validated pre-Windows probe scanned on the Lite. Windows still needs to
enumerate WFI0, show its own scan results, connect to a WPA2 network, get
an IP address and sustain traffic through a reconnect and rekey. The current
device boot profile is reserved for the display investigation.

References: [Switchroot msgbuf](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/msgbuf.c),
[WiFiCx client model](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/writing-a-wificx-client-driver),
[KMDF common buffers](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfcommonbuffer/nf-wdfcommonbuffer-wdfcommonbuffercreate),
[DPC cancellation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdpc/nf-wdfdpc-wdfdpccancel).
