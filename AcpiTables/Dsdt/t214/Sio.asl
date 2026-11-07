// Nintendo Switch Lite (Vali) internal controller on UART-C.
Device (SIO0)
{
    Name (_HID, "SWOA0902")
    Name (_UID, Zero)
    Name (_DEP, Package () { \_SB.GPI0 })
    Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0x70006200, 0x40)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 78 }
        GpioIo (Exclusive, PullUp, 0, 0, IoRestrictionInputOnly, "\\_SB.GPI0", 0, ResourceConsumer, , ) { 171 }
    })

    OperationRegion (SCAR, SystemMemory, 0x60006000, 0x448)
    Field (SCAR, DWordAcc, NoLock, WriteAsZeros) {
        Offset (0x008), HRST, 32, Offset (0x014), HCLK, 32,
        Offset (0x1A0), USRC, 32,
        Offset (0x308), HRSE, 32, HRCL, 32,
        Offset (0x328), HCSE, 32, HCCL, 32,
        Offset (0x358), VRST, 32, Offset (0x360), VCLK, 32,
        Offset (0x3F0), ESRC, 32,
        Offset (0x430), VRSE, 32, VRCL, 32,
        Offset (0x440), VCSE, 32, VCCL, 32
    }
    OperationRegion (SCLK, SystemMemory, 0x7000E5A8, 4)
    Field (SCLK, DWordAcc, Lock, Preserve) { , 10, CKEN, 1, , 3, CKMX, 2 }
    OperationRegion (SMUX, SystemMemory, 0x70003000, 0x21C)
    Field (SMUX, DWordAcc, NoLock, Preserve) {
        Offset (0x104), UTXP, 32, URXP, 32, URTS, 32, UCTS, 32,
        Offset (0x1AC), PORP, 32, Offset (0x1F0), BOTP, 32, RSTP, 32,
        Offset (0x208), HOMP, 32, Offset (0x218), MCLK, 32
    }
    // Tegra masked aliases modify only the pins named by bits 8..15.
    OperationRegion (SGIO, SystemMemory, 0x6000D000, 0x7A4)
    Field (SGIO, DWordAcc, NoLock, WriteAsZeros) {
        Offset (0x08C), DCNF, 32,
        Offset (0x48C), TCNF, 32, Offset (0x49C), TOEN, 32, Offset (0x4AC), TOUT, 32,
        Offset (0x584), VCNF, 32,
        Offset (0x780), CCNF, 32, Offset (0x790), COEN, 32, Offset (0x7A0), COUT, 32
    }
    // Legacy interrupt controller bank 1, SPI 46 (bit 14).
    OperationRegion (SLIC, SystemMemory, 0x60004100, 0x30)
    Field (SLIC, DWordAcc, NoLock, WriteAsZeros) {
        Offset (0x1C), IFCL, 32, Offset (0x24), IESE, 32, IECL, 32
    }
    Field (SLIC, DWordAcc, Lock, Preserve) { Offset (0x2C), , 14, ICLS, 1 }

    Method (SPWR, 1, Serialized)
    {
        If (Arg0 == Zero) {
            IECL = 0x4000
            COUT = 0x2000
            HRSE = 0x00800000
            HCCL = 0x00800000
            CKEN = Zero
            VRSE = 0x02000000
            VCCL = 0x02000000
            Return (One)
        }
        If (Arg0 != One) { Return (Zero) }

        // Select normal SIO flash boot, hold nPOR low and leave NRST as input.
        PORP = 0x501
        BOTP = 5
        RSTP = 5
        TOUT = 0x0100
        TOEN = 0x0301
        TCNF = 0x0303
        COUT = 0x2000
        COEN = 0x2020
        CCNF = 0x2020

        HOMP = 0x59
        MCLK = 4
        VCNF = 0x8000
        UTXP = Zero
        URXP = 0x50
        URTS = Zero
        UCTS = 0x58
        DCNF = 0x1E00

        // PLLP / 102 supplies 4 MHz on CLK_OUT_2. UART-C runs at 16 MHz.
        VRSE = 0x02000000
        VCCL = 0x02000000
        ESRC = 0x400000CA
        VCSE = 0x02000000
        Stall (2)
        VRCL = 0x02000000
        CKMX = 3
        CKEN = One
        HRSE = 0x00800000
        HCCL = 0x00800000
        USRC = 0x01000031
        HCSE = 0x00800000
        Stall (2)
        HRCL = 0x00800000
        If ((HRST & 0x00800000) || (VRST & 0x02000000) ||
            !(HCLK & 0x00800000) || !(VCLK & 0x02000000)) { Return (Zero) }
        ICLS = Zero
        IFCL = 0x4000
        IESE = 0x4000
        Sleep (1) // At least 300 us low after clock setup.
        COUT = 0x2020
        Sleep (100)
        Return (One)
    }
}
