#if defined(SWITCH_LITE_GPU) || defined(SWITCH_LITE_DISPLAY)
#include "../../../Platforms/Switch/t214/Hoag/Acpi/Windows.asl"
#endif
#if defined(SWITCH_ACPI_SWOA_ENABLE) || defined(SWITCH_LITE_TOUCH)
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
#ifdef SWITCH_LITE_TOUCH
                // One assigned GIC interrupt per bank in _CRS.
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x40 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x41 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x42 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x43 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x57 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x77 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x79 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x9D }
#else
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                {
                    0x00000040, 0x00000041, 0x00000042, 0x00000043,
                    0x00000057, 0x00000077, 0x00000079, 0x0000009D
                }
#endif
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

#endif
#if defined(SWITCH_ACPI_SWOA_ENABLE) || defined(SWITCH_LITE_SYSTEM)
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
#ifdef SWITCH_LITE_SYSTEM
            // HDH-001 shares PX6/PX7; Hekate configures both as inputs.
            Name (_DEP, Package () { \_SB.GPI0 })
#endif
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                GpioInt (Edge, ActiveBoth, Exclusive, PullUp, 0x07D0, "\\_SB.GPI0", 0x00, ResourceConsumer, ,)
                {
                    190     // PX6: volume up
                }
                GpioInt (Edge, ActiveBoth, Exclusive, PullUp, 0x07D0, "\\_SB.GPI0", 0x00, ResourceConsumer, ,)
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

        // R371: system battery for switchbattery.sys (ACPI\SWOA0301, battc).
        // Connection 0 = MAX17050 fuel gauge, 1 = BQ24193 charger, both on I2C1.
        Device (BAT0)
        {
#ifdef SWITCH_LITE_XUSB
            Name (_HID, "SWOA0314")
#else
            Name (_HID, "SWOA0301")
#endif
#ifdef SWITCH_LITE_SYSTEM
            Name (_DEP, Package () { \_SB.I2C1 })
#endif
            Name (_UID, Zero)
#ifdef SWITCH_LITE_XUSB
            Name (BRES, ResourceTemplate ()
#else
            Name (_CRS, ResourceTemplate ()
#endif
            {
                I2cSerialBusV2 (0x0036, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
                I2cSerialBusV2 (0x006B, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
#ifndef SWITCH_LITE_SYSTEM
                // R378: connection 2 = BM92T USB-C PD controller, for VBUS / charge
                // role switching on hot-plug (switchbattery.sys v4).
                I2cSerialBusV2 (0x0018, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
#else
                // The Lite battery gauge and charger work before XUSB preparation.
                // The optional host profile appends the Type-C controller below.
#endif
            })
#ifdef SWITCH_LITE_XUSB
            Name (PRES, ResourceTemplate () {
                I2cSerialBusV2 (0x0018, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
            })
            Method (_CRS, 0) {
                If (\_SB.XHC0.XSTA == 0x0F) { Return (ConcatenateResTemplate (BRES, PRES)) }
                Return (BRES)
            }
#endif

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }


#endif
#if defined(SWITCH_ACPI_SWOA_ENABLE) || defined(SWITCH_LITE_TOUCH)
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

#endif
#ifdef SWITCH_ACPI_SWOA_ENABLE
        // R379: I2C2 (GEN2_I2C, 0x7000C400) for the BH1730FVC light sensor. UEFI R379 clocks it.
        Device (I2C2)
        {
            Name (_HID, "SWOA0201")
            Name (_UID, 0x02)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x7000C400, 0x00000100)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x00000074 }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R379: sensors for switchsensor.sys (ACPI\SWOA0901). SPI4 controller (the driver runs it in
        // PIO mode; IMU LSM6DS3H on CS0, UEFI R379 clocks it) and the BH1730FVC light sensor 0x29 on I2C2.
        // Needs UsbPowerAuditDxe >= R379: SPI4 reads stall while its clock is gated.
        Device (SENS)
        {
            Name (_HID, "SWOA0901")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x7000DA00, 0x00000200)
                I2cSerialBusV2 (0x0029, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C2", 0x00, ResourceConsumer, , Exclusive, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

#endif
#ifdef SWITCH_WIFI_RUNTIME
        // Composite AFI host + BCM4356 endpoint. Not an ECAM/pci.sys bridge.
        // SPI 98 / GSI 130 carries legacy INTx and AFI host notifications.
        // Home only exposes this device after WifiPrepare succeeds; touching
        // an unpowered host controller from Windows is not permitted.
        Device (WFI0)
        {
            Name (_HID, SWIFI_ACPI_HID)
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (WSTA, 0x10) // BytePrefix marker patched to 0x0F by Home BDS.
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, SWIFI_HOST_BASE, SWIFI_HOST_LENGTH)
                Memory32Fixed (ReadWrite, SWIFI_CONFIG_BASE, SWIFI_CONFIG_LENGTH)
                Memory32Fixed (ReadWrite, SWIFI_APERTURE_BASE, SWIFI_APERTURE_SIZE)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { SWIFI_LEGACY_GSI }
            })
            // Tegra CAR reset-group U: status and write-one-to-set aliases.
            // Restrict this method to the PCIe controller's three reset lines.
            OperationRegion (WCRS, SystemMemory, 0x6000600C, 4)
            Field (WCRS, DWordAcc, NoLock, Preserve) { WRST, 32 }
            OperationRegion (WCRW, SystemMemory, 0x60006310, 4)
            Field (WCRW, DWordAcc, NoLock, WriteAsZeros) { WSET, 32 }
            Method (WIRS, 1, Serialized)
            {
                If (WSTA != 0x0F) { Return (Zero) }
                If (Arg0 == Zero) {
                    If ((WRST & 0x540) == Zero) { Return (One) }
                    Return (Zero)
                }
                If (Arg0 != One) { Return (Zero) }
                // Switchroot asserts PCIEX, then PEX, then AFI. Do not
                // cycle the shared radio supply or the USB pad controller.
                WSET = 0x400
                WSET = 0x040
                WSET = 0x100
                Sleep (20)
                Local0 = WRST
                If (Local0 == 0xFFFFFFFF) { Return (Zero) }
                If ((Local0 & 0x540) == 0x540) { Return (One) }
                Return (Zero)
            }
            Method (_STA, 0, NotSerialized)
            {
                If ((WSTA == 0x0F)) { Return (0x0F) }
                Return (Zero)
            }
        }
#endif
#ifdef SWITCH_LITE_SIO
#include "../../../Platforms/Switch/t214/Hoag/Acpi/Sio.asl"
#endif
#if defined(SWITCH_ACPI_SWOA_ENABLE) || defined(SWITCH_LITE_BLUETOOTH)
        // R390 (hayato41200/Windows-for-Switch): Bluetooth for switchbt.sys (ACPI\SWOA0B01).
        // BCM4356A3 on UART-D (GIC SPI 90 = GSI 122, level high). UEFI R390 sets the pins,
        // clocks the UART and powers the chip (PH4). Lite uses the same wiring;
        // its dedicated entry does not enable the Erista SPI4 sensor path.
        Device (BTH0)
        {
            Name (_HID, "SWOA0B01")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x70006300, 0x00000040)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x0000007A }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

#endif
#if defined(SWITCH_ACPI_SWOA_ENABLE) || defined(SWITCH_LITE_TOUCH)
        // R373: STM FTM4 touch controller (ACPI\SWOA0501), 0x49 on I2C3,
        // interrupt PX1 (GPIO 185) level low.
        Device (TCH0)
        {
#ifdef SWITCH_LITE_TOUCH
            Name (_HID, "SWOA0502")
            Name (_DEP, Package () { \_SB.GPI0, \_SB.I2C3 })
#else
            Name (_HID, "SWOA0501")
#endif
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

#endif
#if defined(SWITCH_ACPI_SWOA_ENABLE) || defined(SWITCH_LITE_AUDIO)
        // R402: audio for switchaudio.sys (ACPI\SWOA0C01). AHUB (ADMAIF, XBAR,
        // I2S1 at 0x702D1000), ADMA, and the ALC5639 codec on I2C1. UEFI turns
        // on the APE clocks, sets up the codec and starts I2S1.
        Device (AUD0)
        {
            Name (_HID, "SWOA0C01")
#ifdef SWITCH_LITE_AUDIO
            Name (_DEP, Package () { \_SB.I2C1 })
#endif
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x702D0000, 0x00002000)
                Memory32Fixed (ReadWrite, 0x702E2000, 0x00001000)
                I2cSerialBusV2 (0x001C, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

#endif
#ifdef SWITCH_ACPI_SWOA_ENABLE
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

#endif
#if defined(SWITCH_ACPI_SWOA_ENABLE) || defined(SWITCH_LITE_SYSTEM)
        // R377: real-time clock for switchrtc.sys (ACPI\SWOA0801). MAX77620 RTC
        // block at I2C5 0x68. The driver only reads it and keeps its own offset,
        // so the time Horizon keeps in the RTC is left alone.
        Device (RTC0)
        {
            Name (_HID, "SWOA0801")
#ifdef SWITCH_LITE_SYSTEM
            Name (_DEP, Package () { \_SB.I2C5 })
#endif
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

#if defined(SWITCH_ACPI_SWOA_ENABLE) || defined(SWITCH_LITE_POWER)
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

#endif


#if defined(SWITCH_ACPI_SWOA_ENABLE) || defined(SWITCH_LITE_FAN)
        // TMP451 temperature sensor bus. Firmware enables I2C1 before Windows.
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

        // R372: fan control for switchfan.sys (ACPI\SWOA0401). PWM controller
        // (the driver only writes PWM_CSR_1 = fan) and the TMP451 sensor on I2C1.
        Device (FAN0)
        {
            #ifdef SWITCH_LITE_FAN
            Name (_HID, "SWOA0402")
#else
            Name (_HID, "SWOA0401")
#endif
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                #ifdef SWITCH_LITE_FAN
                // Dedicated PWM1 keeps Lite identification and fan control stable.
                Memory32Fixed (ReadWrite, 0x7000A010, 0x00000004)
#ifdef SWITCH_LITE_BACKLIGHT
                // Lite LCD brightness: PWM0/PV0, separate from the fan register.
                Memory32Fixed (ReadWrite, 0x7000A000, 0x00000004)
#endif
#ifdef SWITCH_LITE_PERFORMANCE
                Memory32Fixed (ReadWrite, 0x60006000, 0x00000100)
                Memory32Fixed (ReadOnly,  0xA9FB0000, 0x00001000)
#endif
#else
                Memory32Fixed (ReadWrite, 0x7000A000, 0x00000100)
#endif
                I2cSerialBusV2 (0x004C, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
#ifdef SWITCH_LITE_PERFORMANCE
                // Only the detected phase variant is opened by switchlitefan.
                I2cSerialBusV2 (0x0031, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C5", 0x00, ResourceConsumer, , Exclusive, )
                I2cSerialBusV2 (0x0033, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C5", 0x00, ResourceConsumer, , Exclusive, )
#endif
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

#endif
