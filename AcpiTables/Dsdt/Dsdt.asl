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
            Name (_HID, "NVDA0210")
            Name (_CID, "PNP0D10" /* XHCI USB controller */)
            Name (_UID, Zero)
            Method (_CRS, 0, NotSerialized)
            {
                Return (GCRS (0x70090000, 0x8000, 0x47))
            }

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
    } // Scope(_SB)
}
