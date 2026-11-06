# HDH-001 Wi-Fi bring-up

An HDH-001 completed a pre-Windows scan. The signed Windows driver implements
open/WPA2-Personal association, CCMP keys, disconnect and receive filtering.
Version 0.5.0.0 bound to an ACPI-enumerated device in ARM64 Windows PE
22621.2861. The guest executed the isolation method and registered the board
interrupt. Driver 0.5.1 fixes firmware event delivery after that point.
Previously the control worker passed events only to the escan handler. A
SET_SSID success, failure or disconnect could therefore never reach the
Windows association handler. A control-ring fixture exercises the corrected
dispatch. The same Home firmware and external preparation EFI are used with
this driver. Windows scanning, association and IP traffic still need a
physical HDH-001 run.

## Verified source facts

The Switchroot B01 board description selects BCM4356 FullMAC on PCIe root port index 1 (`pci@2,0`), using UPHY lane `pcie-0`. Port index 0 is disabled. The old SDIO commands in WifiBtApp are explicitly unsuitable for the Switch Wi-Fi path.

The existing upstream `Drivers/WifiBtApp/WifiPcie.h` and `WifiPexStages.h` already implement draft PCIe link, endpoint BAR, backplane/chip and firmware-console stages. The shared source now includes msgbuf-v5 control/event rings and passive scanning. Windows reuses its protocol formats in the station driver.

Switchroot's `tegra210b01_pcie_data` differs from Erista: it does not request the Erista PCIe IO-DPD and MBIST workarounds, and its lane configuration resolves to x4+x1. Power supplies are MAX77620 LDO1/SD3. Wi-Fi and Bluetooth share the chip and low-power clock. The shared PCIe entry configures only Wi-Fi pins, leaving Bluetooth/UART pinmux intact, and skips the Erista IO-DPD workaround on B01.

The SoC-specific `t214/uphy.h` helper enables B01's 102 MHz UPHY management clock through the PLLP HSIO/XUSB branch and the repurposed CAR 0x6D0 divider. It applies Switchroot's lane-0 indirect defaults and four-entry PLL CTL10 table instead of Erista's direct CAL_CTRL/DCO_CTRL values. The shared PLLE routine selects B01's spread coefficients, and lane release respects B01's six-lane layout. An already-running UPHY sequencer keeps its calibration. These are compiled source adaptations, not evidence of a working Lite PCIe link. Rail state, root-port configuration and physical link/chip-ID readback still need acceptance.

Vali's GPIO defaults hold PH0 (RF_DISABLE) low and PH1 (Wi-Fi reset/power) high while running. The B01 PCIe path now follows those levels instead of the legacy probe's unverified active-high PH0 enable assumption. PH3/PH4 Bluetooth wake/reset are not changed by PCIe bring-up.

## Firmware loading

`pchip` now discovers the CR4, D11 and PCIe Gen2 cores from the chip's AI EROM descriptors rather than resetting guessed wrapper addresses. It handles master/slave wrappers, address/size extension words and the table terminator. BCM4356 TCM size uses the CR4 six-bit bank-size field in 8 KiB units. BAR0 window writes require configuration readback, and reset failures stop the operation. Explicit diagnostic core/size overrides remain available but are reported in the log. Failed discovery cannot proceed to firmware loading or reset an unknown core during cleanup.

`wifibt pfw brcmfmac4356-pcie.bin brcmfmac4356-pcie.txt` requires both files after `pex` and `pchip`. `pall` runs those stages together and leaves the Bluetooth/UART setup alone. PCIe stage failures return a nonzero shell status as well as a log message.

The NVRAM converter follows brcmfmac's printable key/value, inline-comment, NUL-separated entry, default boardrev and aligned length-token format. It accepts a final entry without a newline and rejects empty/oversized data. Switch-specific single-board files are required; router-style multi-device files with `devpath` or `pcie/` prefixes need device selection and are rejected rather than uploaded unchanged.

The loader also honors the optional firmware `RAMS` aperture header. Files and the combined firmware/NVRAM RAM layout are checked before writing TCM. The loader checks existing firmware-word and NVRAM-token readbacks before release, reports the shared-area handshake, and keeps console reads within chip RAM and the BAR2 aperture. Host EROM and NVRAM fixtures pass with address/undefined-behavior sanitizers, and the AARCH64 command module builds. R6 subsequently accepted chip firmware startup and passive scanning on hardware (see below); these host checks alone do not establish a working Wi-Fi link.

## Control transport and passive scan

`WifiMsgbuf.h` implements the BCM4356 shared-memory v5 control transport from
Switchroot brcmfmac. It publishes five common rings, supports TCM indices and
2-/4-byte host DMA indices, and posts separate IOCTL/event response buffers.
`pctl` queries the firmware command/version interface. `pscan [clm_blob]`
registers the escan event, brings the radio up and requests a passive scan.
Scan events report BSSID, channel, RSSI and SSID through the shared log.

The transport owns a below-4-GiB uncached DMA arena, uses producer/consumer
barriers, and temporarily bypasses only the AFI SMMU group. Before unmapping or
freeing that arena it disables endpoint bus mastering, stops CR4 and holds the
Wi-Fi endpoint in reset. Both commands therefore require a fresh `pall` before
a subsequent probe; no DMA allocation is handed to Windows.

For firmware that requires a matching external CLM file, `pscan` uploads it
before `WLC_UP`, using the Linux v1 download header and 1400-byte chunks. The
first/last flags, payload length, zero CRC/padding and failed-chunk abort follow
brcmfmac. Do not mix a generic Cypress CLM with a different board firmware.

## Automatic physical check

The owner’s Ubuntu partition was read without mounting it writable. Its boot
log identifies BCM4356 revision 3, loads firmware **7.35.252.108 / FWID
01-65333037**, and creates `wlp1s0`. The matching files are
`brcm/brcmfmac4356A3-pcie.bin` and `brcm/brcmfmac4356A3-pcie.txt`. The similarly
named `fw_bcmdhd.bin` is a different BCM4354 SDIO image and is not used. These
local files and the device identifiers are not committed to this repository.

`WifiProbe.inf` builds an application from the same source as `wifibt`. It uses
EFI file handles directly, so it does not depend on a shell or keyboard. The
shell command explicitly initializes ShellLib before using its file helpers.
The Home boot manager starts the application only when its Hekate environment
contains `wifi_check=1`; a normal configuration never runs the PCIe probe.

WifiCheck R1 returned `Not Found` before probing hardware. Home's SD Block I/O
publishes only the selected read-only WINARM view, so the application placed
on FAT was not visible. R2 keeps that bounded disk view and loads these files
from WINARM instead:

```text
\SwitchWOA\WifiProbe\WifiProbe.efi
\SwitchWOA\WifiProbe\firmware.bin   # the owner's A3 PCIe image
\SwitchWOA\WifiProbe\nvram.txt     # matching A3 board data
```

A matching `firmware.clm_blob` is optional. The current A3 package does not add
the unrelated generic Cypress CLM. The probe runs controller/link, endpoint,
chip discovery, firmware handshake and passive scan in that order. It reports
the last stage on screen and stops Wi-Fi. UEFI's boot disk is read-only, so R2
does not attempt to create a log file. Photograph the final result. The Home
boot manager displays it for 30 seconds and requests shutdown without starting
Windows. The entry remains **Windows 11 Lite**.

R2 was deployed and its three files were read back from WINARM. Physical radio
results were still pending at that stage. The offline copy adds only the three
files above; Windows registry, drivers, boot store, partitions and BL31 are
unchanged. The FAT update replaces Home BL33. Remove `wifi_check=1` after
collecting the result to restore normal Windows launch. A completed scan is
not a Windows network connection; R6 physical PCIe/firmware/scan acceptance is
recorded below.

The R2 physical run returned `Device Error`; its final photograph retained
only the parent status, so the exact child failure stage is unknown. Loader
R3 captures the child console output and reprints the last 12 lines after
image exit. This diagnostic changes only Home BL33; the R2 application and
firmware/NVRAM on WINARM are unchanged. The R3 result confirmed that the probe reached PCIe initialization, but
`Print()` truncated the replay at its configured 320-character limit. R4 sends
the already-formatted tail directly through `OutputString`, preserving the
end of the trace. R4 is deployed; the actual failing hardware stage is still
identified by the R4 result below.

R4's complete tail reports **LAST STAGE 1: UPHY CAL_DONE timeout**, with PLL
CTL2 `0x00000005`. B01 management-clock readback is `PLLP_MISC1=0x30000000`,
`DIV=0x00000106`. This is before endpoint enumeration, chip firmware or scan.
The first R4 launch was black before the logo; the owner retried and obtained
the diagnostic result. That isolated startup failure has not been explained.

Switchroot enables the PCIe supply regulators before clock/PHY initialization.
The Home probe lacked that step. B01's board description assigns PCIe analog
and PLL supplies to MAX77620 LDO1 at 1.0 V; SD3 is the shared 1.8 V rail.
`t214/power.h` provides the R5 preparation in the explicit diagnostic boot path:
observe SD3 without writing it, enable an off LDO1 at 1.0 V, wait for power-good,
and preserve an already-powered 1.0-1.05 V shared supply. It restores the original
LDO1/FPS state after the child exits, asserting PCIEX reset before releasing an
acquired supply. This also covers failure before the child marks PEX up.

R5 changes only Home BL33 and is deployed. The probe application/data on WINARM
are unchanged. The supply policy builds and its focused model covers ramp,
shared-rail preservation, restoration and I2C failures; physical power-good,
calibration and link results are now recorded below.

R5's physical readback is **LDO1 0x0A -> 0xC8, power status 0xC8**. UPHY,
PCIe link, endpoint and chip discovery progressed successfully. CR4 reports
`0xC0000` bytes at `0x180000`. The remaining stage-4 failure is the first TCM
firmware-word readback: `0xFFFFFFFF` instead of `0xB83EF180`.

The existing root-port bridge memory window ended at `0x133FFFFF`. BAR2 needs
at least 4 MiB to contain that TCM range, and the allocator places it outside
the fixed bridge limit. R6 computes the bridge memory base/limit after BAR
sizing, covers the full aligned BAR0/BAR2 allocation and verifies configuration
readback before memory access. Endpoint memory/bus mastering are disabled while
sizing/programming BARs, then the existing command policy is restored. The
outbound AFI aperture is unchanged. The focused layout fixture decodes the
bridge register and checks coverage of both BARs and the firmware TCM interval.

R6 is deployed and read back as an application-only update on WINARM; R5 BL33 and the Ubuntu board data
remain in place. The owner’s R6 result is **SCAN COMPLETE / LAST STAGE 5**, UEFI return
`Success`, terminal scan status 0 and six scan records on channels 1, 9 and 36.
Records include repeats and are not a count of distinct networks. This accepts
pre-Windows PCIe/firmware startup, control/event DMA and passive scanning.
The probe reported endpoint reset and DMA release before exit. Network names,
BSSIDs and device calibration are not copied into this public record.

The temporary `wifi_check=1` line has been removed from the returned card,
restoring normal **Windows 11 Lite** launch. R6 was the firmware-only scan.
The Windows driver has since gained station queues, association and CCMP
keys. Its hardware activation is pending the BasicDisplay boot check.

## Build and focused checks

From the configured EDK2 workspace, build the command module with:

```sh
build -a AARCH64 -t GCC5 -b RELEASE -p NintendoSwitchPkg/NintendoSwitch.dsc -m NintendoSwitchPkg/Drivers/WifiBtApp/WifiBtCmd.inf -D SWITCH_WIFIBT=1
```

From the repository root, the descriptor/format fixtures use the actual shared headers:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined Drivers/WifiBtApp/tests/test-erom.c -o /tmp/test-erom && /tmp/test-erom
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined Drivers/WifiBtApp/tests/test-nvram.c -o /tmp/test-nvram && /tmp/test-nvram
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all Drivers/WifiBtApp/tests/test-msgbuf.c -o /tmp/test-msgbuf && /tmp/test-msgbuf
```

The msgbuf model covers IOCTL/event interleaving, index wrap/full conditions, malformed replies, timeouts, passive-scan parameters, CLM chunking and reset-before-DMA-release. The actual Ubuntu firmware/NVRAM pair also passes the loader format/layout check. The shell module, automatic probe and Home firmware compile for AARCH64. These checks do not test physical PCIe link training, DMA, radio operation or Windows networking.

For the standalone application, enable `SWITCH_WIFI_PROBE=1` and build `Drivers/WifiBtApp/WifiProbe.inf`; the DSC supplies its `WIFI_AUTO_PROBE` and DMA configuration. Copy the resulting application and matching firmware/NVRAM to WINARM; the read-only Home disk view does not expose FAT. The shell module need not be included in normal Home firmware.

## Milestones

1. Adapt the shared PCIe bring-up for B01 and obtain a physical link/chip-ID readback. Do not claim a connected network from an enumerated endpoint.
2. Supply the Switch-specific `brcmfmac4356-pcie.bin` and matching board NVRAM locally. Load the chip firmware, verify its shared-memory/console handshake and retain the actual failure stage.
3. Extend the implemented polled control/event transport with packet rings, interrupts and association/key commands. Linux cfg80211 and network callbacks need Windows equivalents; a Linux module cannot be used as a Windows SYS file.
4. Add a Windows ARM64 Wi-Fi adapter interface with scan, authentication/association and packet transmit/receive. Validate DHCP, sustained traffic, disconnect/reconnect and Bluetooth coexistence.

Board firmware and calibration are supplied privately. The physical Lite has exercised the pre-Windows scan. Its installed Windows profile is currently reserved for the separate BasicDisplay boot test.

The Windows interface target is WiFiCx 1.0 with NetAdapterCx 2.2/KMDF 1.33 for the current 22631 system. The driver publishes firmware capabilities, dispatches scans and implements packet queues. Version 0.2 adds directed association, CCMP keys and disconnect.

## Primary references

- [B01 PCIe wiring and supplies](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi)
- [Lite GPIO running and suspend levels](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/linux-5.1.2/kernel-dts/nx-platforms/tegra210b01-vali-gpio.dtsi)
- [Switchroot Tegra PCIe host](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/pci/host/pci-tegra.c)
- [B01 UPHY management clock and PLLE coefficients](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/clk/tegra/clk-tegra210b01.c)
- [B01 PCIe PHY defaults and calibration](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/phy/tegra/xusb-tegra210.c)
- [brcmfmac PCIe transport](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c)
- [brcmfmac firmware/NVRAM parser](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/firmware.c)
- [brcmfmac EROM discovery and CR4 RAM sizing](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/chip.c)

- [brcmfmac control/event/packet message format](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/net/wireless/broadcom/brcm80211/brcmfmac/msgbuf.c)
- [Linux CLM upload](https://github.com/torvalds/linux/blob/v6.1/drivers/net/wireless/broadcom/brcm80211/brcmfmac/common.c)
- [WiFiCx client initialization](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/writing-a-wificx-client-driver)
- [WiFiCx/NetAdapterCx compatibility](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/building-a-wificx-driver-for-multiple-versions-of-windows)

## Windows runtime implementation

The canonical `drivers/SwitchWifi` implementation is mirrored in
[WindowsDrivers](../../WindowsDrivers/drivers/SwitchWifi/README.md). It includes
msgbuf-v5 flow creation/deletion, Ethernet TX/RX descriptors and packet ownership,
producer/consumer rings, a serialized command engine and retained event buffers.
Paired reply-buffer/request publication is atomic; a full ring publishes neither.
Command completions validate response ID, transaction and command. Deadlines
complete the request once and retain DMA until recovery; events cannot be
reposted while a consumer still owns them.

The KMDF transport uses HAL common buffers, mailbox interrupts and bounded DPC
work. Index slots have a two- or four-byte stride but always contain a 16-bit
counter, as in Switchroot. Interrupt masking/rearming closes the arrival race;
upper-stack notifications run outside the protocol lock. Quiescence joins
in-flight submissions and cancels/waits for the DPC before endpoint reset and
DMA deletion. No periodic runtime polling timer is used.

Native ASan/UBSan fixtures exercise wire messages, atomic ring-full/wrap behavior,
packet ownership, command deadlines and event retention. WDK/KMDF 1.33 builds an
ARM64 `SwitchWifiTransport.lib` including the Windows resource layer, chip loader
and shared-session allocator. The station driver links the library with WiFiCx and
NetAdapterCx. Its signed package stages in an ARM64 PE guest.

The existing OOBE bypass only skips first-setup networking. It does not disable
WLAN or account for the missing Windows Wi-Fi adapter.

## Experimental Windows resource handoff

`SWITCH_WIFI_RUNTIME=1` is a separate, opt-in Home build flag requiring the
Hekate, bounded boot-SD and I2C5 power profiles. It adds the WFI0 composite
platform device (`ACPI\SWOA0D01`, `_CCA=0`) and builds `WifiPrepare.efi` from the
same PCIe implementation used by the accepted R6 probe. Home loads it from
`\SwitchWOA\Wifi\WifiPrepare.efi` on WINARM before Windows Boot Manager.
It does not run the scan or use a second Hekate entry.

The preparation application enables LDO1, PHY and link, assigns BARs, discovers
and halts CR4, disables bus mastering and masks endpoint/AFI interrupts. It
establishes the direct AFI DMA domain used by this non-IORT Windows profile
while bus mastering is off; no host DMA buffers are left live across boot.
Only PCIe SPI 98's legacy-controller CPU route is enabled, with IRQ rather than
FIQ classification and stale forced state cleared. Windows still owns GIC.
The failure path stops the endpoint and restores acquired supply/domain/route
settings. Bluetooth reset/UART and the shared SD3 supply are preserved.

WFI0 remains hidden until the application returns success. Home then updates
its unique DSDT readiness byte and checksum. If preparation fails or the app is
absent, the device remains hidden and Windows boot continues. The resource
header is shared with the Windows driver: host/root/AFI registers at
0x01000000+0x4000, the UEFI config window at 0x02000000+0x1000, outbound memory
at 0x13000000+0x0D000000, and legacy INTx/AFI GSI 130 (SPI 98). This is not a
standard ECAM root bridge.

The Windows resource layer maps host/config registers and bounded BAR slices.
The ISR acknowledges AFI's reason/signature and PME status before handling the
BCM mailbox; unexpected host errors disable the source and schedule recovery
without touching a potentially inaccessible endpoint. MSI and selective-suspend
support are not claimed. Source references:
[Switchroot SoC resources](https://github.com/CTCaer/l4t-hardware-soc-t210/blob/l4t/l4t-r32.3.1/kernel-dts/tegra210-soc/tegra210-soc-base.dtsi),
[AFI IRQ handling](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/pci/host/pci-tegra.c),
[legacy IRQ forwarding](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-5.1.2/drivers/irqchip/irq-tegra.c).

The full runtime Home image and preparation app compile for AARCH64. ACPICA
returns WFI0 status 0 before readiness and 0x0F afterward, plus the expected
resource buffer and HID. The interpreter reported four outstanding cache
allocations at exit; WFI0 evaluations returned the expected values. Windows
source compiles with WDK/KMDF into the ARM64 transport library. These are local
artifacts only, not deployed or physically accepted. The Windows chip loader, shared-buffer publisher, device lifecycle and station
queues are implemented. The physical firmware handoff remains untested.

## Windows chip-firmware loader

The Windows library now includes `windows/chip.c`: prepared-link and endpoint
checks, BAR sizing/mapping with decoding and bus mastering disabled, AI core/TCM
discovery, private firmware/NVRAM loading, bounded aligned TCM writes, CR4 startup
and the v5 shared-area handshake. Bus mastering remains disabled until the
runtime owner has published host DMA allocations. Stop after interrupt quiescence
disables bus mastering, halts CR4 and holds D11 reset; failure requires platform
endpoint reset before freeing DMA buffers.

The EROM/NVRAM implementations were moved unchanged into the shared
`WindowsDrivers/drivers/SwitchWifi/core` files. Existing UEFI header names now
include those files. Native format fixtures pass, and both the automatic UEFI
probe and runtime Home/preparation builds still compile. The Windows library
build now uses `clang-cl /kernel`, matching the existing driver build convention.

Runtime register access pins BAR0's backplane window to the PCIe core, following
brcmfmac's `brcmf_pcie_select_core` sequence. No independent BAR0+0x2000 alias is
assumed. The Windows loader refuses backplane-window changes while runtime access
is pinned; only the quiesced stop path releases that constraint.

Windows firmware startup, station creation and data queues have yet to run on
the physical board. Association and key management now have Windows implementations. This update contains
no firmware binary, calibration data or network credentials and has not been
copied to the card.

## Windows shared-session allocation

`windows/shared.c` allocates five common rings, optional host-index arrays,
scratch/update areas, command input/reply buffers and eight event buffers using
HAL logical addresses. It validates TCM locations and initial indices with bus
mastering disabled, then publishes pointer/dimension fields and reads them back.
A post-publication failure retains allocations for endpoint reset. DMA and the
optional host-ready doorbell are enabled only after successful preparation.
TX flow allocations remain on demand and use the normal asynchronous create/
delete protocol. Their lifetimes are independent of the five common descriptors.

The fixed v5 session currently requires RX prefix zero because the 16-byte TX
completion has no unused prefix space. A different firmware layout needs its
actual descriptor-size contract before use. The portable control/ring code
retains offset handling; this narrower shared-session layout is explicit and
has not yet been hardware-accepted in Windows.

Mailbox flag reading/clearing is nonblocking in the DPC. The passive sender
waits at most one second for an outstanding host word, then uses PCI config
SBMBX with the double-notification workaround on core revisions <=13. The
Windows owner still must bind those helpers to its recovery/power state machine;
these helpers do not establish suspend/resume or solve system shutdown.

## Windows firmware-command execution

The Windows control layer now binds the portable engine to KMDF synchronization:
a passive command caller waits for interrupt-driven completion, ring progress or
stop, with a two-second command deadline. The DPC uses only resident staging
buffers. Event/mailbox work runs passively outside the protocol lock; event DMA
is retained until the callback returns. Normal device stop wakes commands and
joins workers before hardware reset; cancellation alone never authorizes DMA
release. A recovery callback queues separate owner work rather than resetting
inline from a DPC or a worker being flushed.

The [Windows Wi-Fi lifecycle](../../WindowsDrivers/drivers/SwitchWifi/README.md#device-startup-power-and-radio-capabilities) now initializes WiFiCx with the station-adapter factory supplied by the device owner. Interrupt-enabled D0 startup obtains the firmware radio state and legal channels and publishes device, band and PHY capabilities. The command worker implements the software radio task using WDI TLV parsing, a masked firmware write, readback, radio-status indication and matching task completion. The capability and command components build for ARM64. The Windows scan/abort handlers use the shared UEFI escan format, with firmware-regulated channels and WDI BSS indications. The KMDF device callbacks and station transmit/receive queues now link into
the signed driver. The runtime-only `WIRS` method checks PCIEX/PEX/AFI reset
state and can hold these dedicated reset lines if both endpoint halt and port
reset fail. The Windows package staged in an isolated PE VM. The Lite has not
received this runtime build. The Windows station now handles directed WPA2-Personal association and CCMP
key installation. Physical scanning, authentication, DHCP and rekey remain pending.

## Windows connection package

The [station driver](../../WindowsDrivers/drivers/SwitchWifi/README.md#association-and-keys)
uses Windows for the WPA2 handshake. It verifies the associated BSSID, SSID
and negotiated request before reporting success. The firmware checks receive
protection and the driver gates traffic during key changes. Protected-bit
provenance for post-key Ethernet EAPOL is still a hardware acceptance item.
The optional `wifi` installer mode stages only this driver, the board's two
private A3 files and the matching `WifiPrepare.efi` on WINARM. Loading the
runtime BL33 remains a separate step through the existing Windows boot entry.


Version 0.3 records the key state when each RX buffer is posted. The driver
discards Ethernet EAPOL frames that cross a key change without evidence of
air protection. Optional BCM metadata supplies receive signal strength.
Windows link indications use the measured rates for the associated peer.
The device's `SwitchWifiReport` value records initialization, scans, joins,
acknowledged key slots and observed frame types. See the
[status fields and read command](../../WindowsDrivers/drivers/SwitchWifi/README.md#on-device-status).
An isolated ARM64 Windows PE 22621.1 guest accepted the signed 0.3.0.0 package as `oem0.inf`. The existing 22631.2861 ARM64 installation media includes WiFiCx, NetAdapterCx and the WLAN service. Acceptance on the physical Windows
adapter, including DHCP and rekey, remains pending.

The station now registers the four ordinary data filters with NetAdapterCx
before adapter startup and handles WDI filter/list changes on the station port.
It programs Switchroot's BCM multicast controls after bringing the interface
up, tracks active policy in `SwitchWifiReport`, and rechecks queued frames
when Windows changes its filter or keys. DHCP and IPv6 neighbor discovery
still require physical traffic on HDH-001.

An isolated ARM64 Windows PE 22621.2861 guest selected and loaded the signed
0.4.2.0 package for a software-enumerated device. It reported
`step=2/C0000010 failure=2/C0000010` when the synthetic device rejected the
Switch-specific ACPI isolation request. The previous image had stopped at
driver load with problem 39 and `0xC0000018`. The link now applies the
Switch drivers' kernel section protections. The device-key diagnostic is
flushed on failed preparation. The current hardware acceptance item is the
real WFI0 node followed by a Windows scan, association, IP traffic and rekey.

Under the opt-in runtime profile, `WifiPrepare.efi` enables PCI memory decoding
and leaves endpoint bus mastering off until the OS publishes its DMA rings.
Windows matches the raw board memory ranges, maps their translated addresses
and registers its KMDF interrupt with the original resource-list descriptors.
The 0.5.0.0 ACPI fixture exposed only GSI 130 and returned
`hw=3/C0000182 irq=0 ring=0` at the expected missing-memory check. The
pre-Windows physical scan uses polling. Hardware interrupt delivery and ring
completions in Windows must be observed on the board.
[Driver report and fixture](../../WindowsDrivers/drivers/SwitchWifi/README.md#on-device-status).
