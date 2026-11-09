// Lite-only GPU/display ACPI devices. Included inside Scope (\_SB).
#ifdef SWITCH_LITE_GPU
        // Separate render adapter; the POST display remains owned by its
        // existing driver. Publishing resources does not power up the GPU.
        Device (GPU0)
        {
            Name (_HID, T214_GPU_ACPI_ID)
            Name (_UID, Zero)
            Name (_CLS, Package () { 0x03, 0x02, Zero })
            Name (_DEP, Package () { \_SB.FAN0 })
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, T214_GPU_REG_BASE, T214_GPU_REG_SIZE)
                Memory32Fixed (ReadWrite, T214_GPU_BAR1_BASE, T214_GPU_BAR1_SIZE)
                Memory32Fixed (ReadWrite, T214_GPU_AUX_BASE, T214_GPU_AUX_SIZE)
#define GPU_POWER_ASL(Base, Size) Memory32Fixed (ReadWrite, Base, Size)
                T214_GPU_POWER_RESOURCES(GPU_POWER_ASL)
#undef GPU_POWER_ASL
#define GPU_ID_ASL(Base, Size) Memory32Fixed (ReadOnly, Base, Size)
                T214_GPU_ID_RESOURCES(GPU_ID_ASL)
#undef GPU_ID_ASL
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { T214_GPU_STALL_GSI }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { T214_GPU_NONSTALL_GSI }
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
#endif
#ifdef SWITCH_LITE_DISPLAY
        // Display-only ownership of Hekate's existing POST framebuffer.
        // The Windows miniport obtains the active address/stride through
        // DxgkCbAcquirePostDisplayOwnership; no Tegra GPU engines are exposed.
        Device (DISP)
        {
            Name (_HID, "WINX0001")
            Name (_UID, Zero)
            Name (_CLS, Package () { 0x03, Zero, Zero })
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0xF5A00000, 0x01000000)
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
#endif
