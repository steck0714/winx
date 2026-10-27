/** @file
  Differentiated System Description Table Fields (DSDT)

  Copyright (c) 2014-2018, ARM Ltd. All rights reserved.<BR>
  Copyright (c) 2019-2020, Bingxing Wang. All rights reserved.<BR>
  Portion retrieved from Surface RT 2 DSDT dump

  This program and the accompanying materials are licensed and made available under 
  the terms and conditions of the BSD License which accompanies this distribution.  
  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "Platform.h"

DefinitionBlock("DsdtTable.aml", "DSDT", 1, "NVIDIA", "TEGRA210", EFI_ACPI_ARM_OEM_REVISION) 
{
    Scope(_SB) 
    {

        Device(CPU0) 
        {
            Name(_HID, "ACPI0007")
            Name(_UID, 0)
        }

        Device(CPU1) 
        {
            Name(_HID, "ACPI0007")
            Name(_UID, 1)
        }

        Device(CPU2) 
        {
            Name(_HID, "ACPI0007")
            Name(_UID, 2)
        }

        Device(CPU3) 
        {
            Name(_HID, "ACPI0007")
            Name(_UID, 3)
        }

        // Helper functions
        Method (GCRS, 3, NotSerialized)
        {
            Name (RSRC, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite,
                    0x00000000,         // Address Base
                    0x00000000,         // Address Length
                    _Y00)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, _Y01)
                {
                    0x00000000,
                }
            })
            CreateDWordField (RSRC, \_SB.GCRS._Y00._BAS, MBAS)  // _BAS: Base Address
            CreateDWordField (RSRC, \_SB.GCRS._Y00._LEN, MBLE)  // _LEN: Length
            CreateWordField (RSRC, \_SB.GCRS._Y01._INT, INTN)  // _INT: Interrupts
            MBAS = Arg0
            MBLE = Arg1
            INTN = Arg2
            Return (RSRC)
        }

        Method (CRSD, 2, NotSerialized)
        {
            Name (RSRC, ResourceTemplate ()
            {
                FixedDMA (0x0000, 0x0000, Width32bit, _Y07)
                FixedDMA (0x0001, 0x0001, Width32bit, _Y08)
            })
            CreateWordField (RSRC, One, REQ1)
            CreateWordField (RSRC, \_SB.CRSD._Y07._TYP, CHN1)  // _TYP: Type
            CreateWordField (RSRC, \_SB.CRSD._Y08._DMA, REQ2)  // _DMA: Direct Memory Access
            CreateWordField (RSRC, \_SB.CRSD._Y08._TYP, CHN2)  // _TYP: Type
            REQ1 = Arg0
            REQ2 = (Arg0 | 0x20)
            CHN1 = Arg1
            CHN2 = Arg1++
            Return (RSRC)
        }

        Method (CRS3, 5, NotSerialized)
        {
            Local0 = GCRS (Arg0, Arg1, Arg2)
            Local1 = CRSD (Arg3, Arg4)
            Return (ConcatenateResTemplate (Local0, Local1))
        }

        // UART 2: Right Joy-Con
        Device (UAR2)
        {
            Method (_HID, 0, NotSerialized)
            {
                Return ("NVDA0100")
            }

            Name (_UID, One)
            Method (_CRS, 0, NotSerialized)
            {
                Return (CRS3 (0x70006040, 0x40, 0x45, 0x09, 0x02))
            }

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // SD Host Controller: removable microSD slot (SDMMC1)
        // Base/length/IRQ verified against Include/Device/NvAddressMap.h
        // (TEGRA_SDMMC1_BASE) and public Tegra210 device-tree sources.
        // The 2019 da0dd9a attempt used base 0x78000000 here, which
        // does not match any known Tegra210 address map and was most
        // likely never functional; corrected to 0x700B0000 below.
        // UNTESTED ON HARDWARE - compiles cleanly, PNP0D40 binding
        // and clock/power state at ExitBootServices are unverified.
        Device (SDM1)
        {
            Method (_HID, 0, NotSerialized)
            {
                Return ("NVDA0212")
            }

            Name (_CID, "PNP0D40" /* SDA Standard Compliant SD Host Controller */)
            Name (_UID, Zero)
            Method (_CRS, 0, NotSerialized)
            {
                Return (GCRS (0x700B0000, 0x0200, 0x2E))
            }

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // eMMC (internal storage, SDMMC4). Base/length/IRQ cross-
        // checked against three independent sources: this repo's
        // NvAddressMap.h, coreboot's own tegra210/addressmap.h, and
        // public Tegra210 device-tree sources (sdhci@700b0600,
        // interrupts = <0 31 0x04>). All three agree.
        // UNTESTED ON HARDWARE. eMMC is also 8-bit/HS200-capable
        // where the microSD slot is 4-bit only; PNP0D40/sdport.sys
        // is expected to self-negotiate this from the SDHCI
        // capabilities register, but that negotiation itself is
        // unverified here, same as SDM1.
        Device (SDM4)
        {
            Method (_HID, 0, NotSerialized)
            {
                Return ("NVDA0213")
            }

            Name (_CID, "PNP0D40" /* SDA Standard Compliant SD Host Controller */)
            Name (_UID, One)
            Method (_CRS, 0, NotSerialized)
            {
                Return (GCRS (0x700B0600, 0x0200, 0x3F))
            }

            Method (_STA, 0, NotSerialized)
            {
#ifdef SWITCH_ACPI_EMMC_ENABLE
                Return (0x0F)
#else
                // The eMMC holds PRODINFO (console-unique calibration/keys), BOOT0/1 and the
                // Horizon system partitions. A Windows disk stack that sees it can write to
                // it by mistake (auto-initialise, "format disk", bad-block scan...).
                // Hidden unless the firmware is built with -D SWITCH_ACPI_EMMC=1.
                Return (Zero)
#endif
            }
        }


#if defined(SWITCH_ACPI_XHCI_ENABLE) && defined(SWITCH_ACPI_EHCI_ENABLE)
        // Both controllers are described but they share the one physical USB-C port, so only one may be
        // visible to the OS. The firmware picks it at ExitBootServices by patching this byte in the loaded
        // DSDT (AML encoding 08 'U' 'B' 'S' 'L' 00 -> 01): 0 = EHC0 (default), 1 = XHC0 (xusb firmware is
        // running, see Drivers/XusbInitApp `xusbinit acpi`).
        Name (UBSL, Zero)
#define SWITCH_ACPI_USB_SELECT 1
#endif

#ifdef SWITCH_ACPI_XHCI_ENABLE
        // XUSB xHCI (usb@70090000, 32 KiB; SPI 39 = INTID 71, level high; values from
        // tegra210.dtsi). Only valid AFTER the firmware has loaded xusb.bin and the host
        // mailbox is serviced (see Drivers/XusbInitApp) - otherwise the controller is dead.
        // Enabled with -D SWITCH_ACPI_XHCI=1. UNTESTED ON HARDWARE.
        Device (XHC0)
        {
            // Layout verified on hardware by hayato41200/Windows-for-Switch (R378):
            // PNP0D10 binds the inbox USBXHCI, _CCA = 0 declares the controller
            // non-cache-coherent (the xHCI DMA buffers are not snooped), and the FPCI
            // (0x70098000) / IPFS (0x70099000) windows plus SPI 39/40/49 are listed
            // together with the host registers. The previous winx entry listed only
            // 0x70090000 and SPI 39 and had no _CCA.
            Name (_HID, "PNP0D10")
            Name (_CID, "PNP0D10")
            Name (_UID, Zero)
            Name (_CCA, Zero)

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x70090000, 0x00008000)
                Memory32Fixed (ReadWrite, 0x70098000, 0x00001000)
                Memory32Fixed (ReadWrite, 0x70099000, 0x00001000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared)
                {
                    0x00000047, // GIC SPI 39
                    0x00000048, // GIC SPI 40
                    0x00000051  // GIC SPI 49
                }
            })

            Method (_STA, 0, NotSerialized)
            {
#ifdef SWITCH_ACPI_USB_SELECT
                If ((UBSL == One))
                {
                    Return (0x0F)
                }

                Return (Zero)
#else
                Return (0x0F)
#endif
            }
        }
#endif
#ifdef SWITCH_ACPI_EHCI_ENABLE
        // Tegra210 USB1 (ChipIdea dual-role EHCI, usb@7d000000, SPI 20 = INTID 52, level high).
        // Windows binds its inbox EHCI driver through _CID PNP0D20. The capability registers start at
        // 0x7D000100 (the same window the firmware's EHCI emulation uses); the operational registers sit
        // at +0x30, i.e. USBMODE is at 0x7D0001F8.
        // _HRV = 2: integrated TT, port speed in HOSTPC (HOSTPC1_DEVLC.PSPD on this controller).
        // _DSM function 1 (UUID ce2ee385-00e6-48cb-9f05-2edb927c4899) is the documented "post-reset" hook for
        // dual-role controllers: every HCRESET drops ChipIdea back to idle mode, so set USBMODE.CM = host.
        // UNTESTED ON HARDWARE.
        Device (EHC0)
        {
            Name (_HID, "NVDA0211")
            Name (_CID, "PNP0D20" /* EHCI USB controller */)
            Name (_UID, Zero)
            Name (_HRV, 2)
            Method (_CRS, 0, NotSerialized)
            {
                Return (GCRS (0x7D000100, 0x740, 0x34))
            }

            OperationRegion (UMOD, SystemMemory, 0x7D0001F8, 0x04)
            Field (UMOD, DWordAcc, NoLock, Preserve)
            {
                USMD,   32
            }

            // USB_SUSP_CTRL (abs 0x400, op + 0x2D0). Bit 7 = USB_PHY_CLK_VALID. Linux keeps this register
            // readable on its own while the AHB clock of the controller is gated (ci_hdrc_tegra.c).
            OperationRegion (USPC, SystemMemory, 0x7D000400, 0x04)
            Field (USPC, DWordAcc, NoLock, Preserve)
            {
                SCTL,   32
            }

#ifdef SWITCH_ACPI_EHCI_TXFILL
            // TXFILLTUNING (abs 0x154, op + 0x24). Linux (ci_hdrc_tegra.c, CI_HDRC_CONTROLLER_RESET_EVENT)
            // rewrites it after every controller reset with txfifothresh << 16 (16 for the Tegra30 class).
            // The right value for Tegra210 is NOT confirmed, so this is opt-in (-D SWITCH_ACPI_EHCI_TXFILL=1).
            OperationRegion (UTXF, SystemMemory, 0x7D000154, 0x04)
            Field (UTXF, DWordAcc, NoLock, Preserve)
            {
                TXFT,   32
            }
#endif

            // Windows USB _DSM (UUID ce2ee385-00e6-48cb-9f05-2edb927c4899, Arg1 = 0).
            //   function 0: query. function 1: post-reset for dual-role controllers, called after every
            //   controller reset (Microsoft: bringup/usb-device-specific-method---dsm-).
            //   function 6: "register access type" (Windows 11 / Server 2022 and later): 1 = 32-bit only.
            // ChipIdea clears USBMODE.CM on every HCRESET and the write only sticks once the PHY clock is
            // valid again, so wait for it (bounded, 100 ms) and verify the mode afterwards (bounded).
            Method (_DSM, 4, Serialized)
            {
                If ((Arg0 == ToUUID ("ce2ee385-00e6-48cb-9f05-2edb927c4899")))
                {
                    If ((Arg2 == Zero))
                    {
                        Return (Buffer (One) { 0x43 })   // functions 0, 1 and 6
                    }

                    If ((Arg2 == One))
                    {
                        Local0 = 1000
                        While (Local0)
                        {
                            If ((SCTL & 0x80))
                            {
                                Break
                            }

                            Stall (100)
                            Local0--
                        }

                        USMD = ((USMD & 0xFFFFFFFC) | 0x03)   // CM = host
                        Local0 = 1000
                        While (Local0)
                        {
                            If (((USMD & 0x03) == 0x03))
                            {
                                Break
                            }

                            Stall (100)
                            Local0--
                        }

#ifdef SWITCH_ACPI_EHCI_TXFILL
                        TXFT = 0x00100000                     // 16 << 16
#endif
                        Return (Zero)
                    }

                    If ((Arg2 == 6))
                    {
                        Return (One)                          // must use 32-bit register access
                    }
                }

                Return (Buffer (One) { 0x00 })
            }

            Method (_STA, 0, NotSerialized)
            {
#ifdef SWITCH_ACPI_USB_SELECT
                If ((UBSL == Zero))
                {
                    Return (0x0F)
                }

                Return (Zero)
#else
                Return (0x0F)
#endif
            }
        }
#endif
#ifdef SWITCH_ACPI_SWOA_ENABLE
        // ---- Windows-for-Switch (hayato41200) SWOA devices -------------------------------
        // ACPI\SWOA0101..0801: GPIO, I2C, battery/USB-C, fan/backlight, touch, power
        // button, CPU clock, RTC. They are only useful together with the matching kernel
        // drivers (Windows-for-Switch-Drivers) AND a UEFI that has already powered/clocked
        // the hardware (see docs/SWOA-PORT-ja.md). Opt-in: -D SWITCH_ACPI_SWOA=1.
        // R367: Tegra210 GPIO controller (Windows driver tegra210gpio.sys,
        // GpioClx). MMIO 0x6000D000, 8 banks x 32 pins; one GIC SPI per bank
        // in bank order: SPI 32, 33, 34, 35, 55, 87, 89, 125 (Linux
        // tegra210 gpio node) = GSI 64, 65, 66, 67, 87, 119, 121, 157.
        // GPIO number = port * 8 + bit (PX6 = 190, PX7 = 191).
        Device (GPI0)
        {
            Name (_HID, "SWOA0101")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x6000D000, 0x00001000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                {
                    0x00000040, 0x00000041, 0x00000042, 0x00000043,
                    0x00000057, 0x00000077, 0x00000079, 0x0000009D
                }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R368: hardware buttons for Windows (inbox HID button driver).
        // R369: R368 used PNP0C40, which no inbox INF on Windows 10 matches
        // (problem 28); hidinterrupt.inf binds ACPI0011 with this _DSD.
        // Volume up = PX6 (GPIO 190), volume down = PX7 (GPIO 191), both
        // active low with pull-ups (L4T gpio-keys). The power button is on
        // the MAX77620 PMIC (and maybe PX5); it is not listed here yet.
        // GpioInt must be edge ActiveBoth so press and release are both seen.
        Device (BTNS)
        {
            Name (_HID, "ACPI0011")    // R369: ACPI 6.0 Generic Buttons Device (hidinterrupt.inf)
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                GpioInt (Edge, ActiveBoth, Exclusive, PullUp, 0x0000, "\\_SB.GPI0", 0x00, ResourceConsumer, ,)
                {
                    190     // PX6: volume up
                }
                GpioInt (Edge, ActiveBoth, Exclusive, PullUp, 0x0000, "\\_SB.GPI0", 0x00, ResourceConsumer, ,)
                {
                    191     // PX7: volume down
                }
            })

            // Map GpioInt index -> HID usage (Windows 10 button array _DSD).
            Name (_DSD, Package (0x02)
            {
                ToUUID ("fa6bd625-9ce8-470d-a2c7-b3ca36c4282e"),
                Package (0x03)
                {
                    Package (0x05) { Zero, One, Zero, One, 0x0D },          // Portable Device Control collection
                    Package (0x05) { One, Zero, One, 0x0C, 0xE9 },          // GpioInt 0: Volume Increment
                    Package (0x05) { One, One, One, 0x0C, 0xEA }            // GpioInt 1: Volume Decrement
                }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R370: Tegra210 I2C controllers for the Windows driver tegra210i2c.sys
        // (SpbCx, ACPI\SWOA0201). Only the buses whose clock/reset UEFI
        // already enables (PmicDxe: I2C1 and I2C5). Interrupts are from the
        // L4T DT (GIC SPI 38 and 53 = GSI 70 and 85); driver v1 polls.
        //   I2C1 0x7000C000, 100 kHz: BM92T 0x18, MAX17050 0x36, TMP451 0x4C, BQ24193 0x6B
        //   I2C5 0x7000D000, 400 kHz: MAX77621 CPU 0x1B / GPU 0x1C, MAX77620 PMIC 0x3C
        Device (I2C1)
        {
            Name (_HID, "SWOA0201")
            Name (_UID, One)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x7000C000, 0x00000100)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x00000046 }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        Device (I2C5)
        {
            Name (_HID, "SWOA0201")
            Name (_UID, 0x05)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x7000D000, 0x00000100)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x00000055 }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R371: system battery for switchbattery.sys (ACPI\SWOA0301, battc).
        // Connection 0 = MAX17050 fuel gauge, 1 = BQ24193 charger, both on I2C1.
        Device (BAT0)
        {
            Name (_HID, "SWOA0301")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                I2cSerialBusV2 (0x0036, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
                I2cSerialBusV2 (0x006B, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
                // R378: connection 2 = BM92T USB-C PD controller, for VBUS / charge
                // role switching on hot-plug (switchbattery.sys v4).
                I2cSerialBusV2 (0x0018, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R372: fan control for switchfan.sys (ACPI\SWOA0401). PWM controller
        // (the driver only writes PWM_CSR_1 = fan) and the TMP451 sensor on I2C1.
        Device (FAN0)
        {
            Name (_HID, "SWOA0401")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x7000A000, 0x00000100)
                I2cSerialBusV2 (0x004C, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R373: I2C3 (GEN3_I2C) for the touch panel. UEFI R373 powers the
        // panel (LDO6 2.9 V, reset PJ7 high) and enables the I2C3 clock.
        Device (I2C3)
        {
            Name (_HID, "SWOA0201")
            Name (_UID, 0x03)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x7000C500, 0x00000100)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x0000007C }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R373: STM FTM4 touch controller (ACPI\SWOA0501), 0x49 on I2C3,
        // interrupt PX1 (GPIO 185) level low.
        Device (TCH0)
        {
            Name (_HID, "SWOA0501")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                I2cSerialBusV2 (0x0049, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C3", 0x00, ResourceConsumer, , Exclusive, )
                GpioInt (Level, ActiveLow, Exclusive, PullUp, 0x0000, "\\_SB.GPI0", 0x00, ResourceConsumer, , ) { 185 }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R374: power button for switchpwrbtn.sys (ACPI\SWOA0601). The button
        // drives MAX77620 EN0; the driver polls ONOFFSTAT/ONOFFIRQ on I2C5.
        Device (PWRB)
        {
            Name (_HID, "SWOA0601")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                I2cSerialBusV2 (0x003C, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C5", 0x00, ResourceConsumer, , Exclusive, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R375: CPU clock control for switchcpu.sys (ACPI\SWOA0701). CAR window
        // (PLLX, CCLK burst policy), MAX77621 VDD_CPU on I2C5, TMP451 on I2C1
        // (shared with the fan driver) for the thermal guard.
        Device (CPUC)
        {
            Name (_HID, "SWOA0701")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x60006000, 0x00001000)
                I2cSerialBusV2 (0x001B, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C5", 0x00, ResourceConsumer, , Exclusive, )
                I2cSerialBusV2 (0x004C, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Shared, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R377: real-time clock for switchrtc.sys (ACPI\SWOA0801). MAX77620 RTC
        // block at I2C5 0x68. The driver only reads it and keeps its own offset,
        // so the time Horizon keeps in the RTC is left alone.
        Device (RTC0)
        {
            Name (_HID, "SWOA0801")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                I2cSerialBusV2 (0x0068, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C5", 0x00, ResourceConsumer, , Exclusive, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
#endif

    } // Scope(_SB)
}
