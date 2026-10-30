// Merge this device into Scope (_SB) in NintendoSwitchPkg's DSDT.
// GIC SPI 14 is ACPI GSI 46 (0x2E).
Device (SDH0)
{
    Name (_HID, "NVDA0101")
    Name (_CID, "PNP0D40")
    Name (_UID, Zero)
    Name (_CCA, Zero)
    Name (_RMV, One)

    Name (_CRS, ResourceTemplate ()
    {
        Memory32Fixed (ReadWrite, 0x700B0000, 0x0200)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
        {
            0x0000002E
        }
    })

    Method (_STA, 0, NotSerialized)
    {
        Return (0x0F)
    }
}
