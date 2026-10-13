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

    } // Scope(_SB)
}
