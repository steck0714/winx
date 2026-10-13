#ifndef _DEVICE_MEMORY_MAP_H_
#define _DEVICE_MEMORY_MAP_H_

#include <Library/ArmLib.h>

#define MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT 64

/* Below flag is used for system memory */
#define SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES \
                EFI_RESOURCE_ATTRIBUTE_PRESENT |                 \
                EFI_RESOURCE_ATTRIBUTE_INITIALIZED |             \
                EFI_RESOURCE_ATTRIBUTE_TESTED |                  \
                EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |             \
                EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |       \
                EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE | \
                EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |    \
                EFI_RESOURCE_ATTRIBUTE_EXECUTION_PROTECTABLE

typedef enum {
	NoHob,
	AddMem,
	AddDev,
	MaxMem
} DeviceMemoryAddHob;

typedef struct {
	EFI_PHYSICAL_ADDRESS				Address;
	UINT64								Length;
	EFI_RESOURCE_TYPE					ResourceType;
	EFI_RESOURCE_ATTRIBUTE_TYPE			ResourceAttribute;
	ARM_MEMORY_REGION_ATTRIBUTES		ArmAttributes;
	DeviceMemoryAddHob					HobOption;
	EFI_MEMORY_TYPE						MemoryType;
} ARM_MEMORY_REGION_DESCRIPTOR_EX, *PARM_MEMORY_REGION_DESCRIPTOR_EX;

/*
 *    Memory Map is as follows
 *
 * ------------------------------   <-- Start of DRAM
 * |                            |
 * |      Available DRAM        |
 * |____________________________|
 * |                            |
 * |   UEFI Mem, Stack and FB   |
 * |____________________________|
 * |                            |
 * |      Other carveouts       |
 * | (with dynamic allocation)  |
 * |____________________________|
 * |                            |
 * |    TZ carveout of size     |
 * | TRUSTZONE_CARVEOUT_SIZE_MB |
 * |____________________________|   <-- 0x100000000
 * |                            |
 * |      Available DRAM        |
 * |                            |
 * ------------------------------   <-- End of DRAM
 *
 */

#ifdef SWITCH_BOOT_CHAIN_HEKATE
/*
 * hekate (L4T launcher) -> BL31 -> BL33 = bl33.bin (stub + this FD).
 *
 * Derived from reading CTCaer/hekate master 45cd81e (v6.5.x): bootloader/l4t/l4t.c,
 * bootloader/l4t/l4t_config.inl and bdk/memory_map.h. NOT confirmed on hardware.
 *
 *   0x080000000 - 0x0A0000000  free
 *   0x0A0000000 - 0x0A0100000  staging buffer for mtc_tbl_b01.bin (T210B01 only). hekate copies
 *                              it into the BPMP-FW DTB (l4t.c) so it should be dead after
 *                              hand-off; kept reserved anyway because it costs 1 MiB.
 *   0x0A0100000 - 0x0AA000000  free
 *   0x0AA000000 - 0x0AA100000  bl33.bin: 4 KiB stub + FD (BL33_LOAD_BASE)
 *   0x0AA100000 - 0x0F5A00000  free
 *   0x0F5A00000 - 0x0F6A00000  hekate framebuffers: IPL_FB 0xF5A00000, LOG_FB 0xF5E00000,
 *                              NYX_FB 0xF6200000, NYX_FB2 0xF6600000. With sld_type=1 (default)
 *                              the display keeps scanning one of them after hand-off, so this
 *                              range must never be handed out as free memory.
 *   0x0F6A00000 - 0x0FE000000  free
 *   0x0FE000000 - 0x100000000  NOT MAPPED. hekate's L4T security carveouts live here: with the
 *                              default config (CARVEOUT_NVDEC_TSEC_ENABLE=0) GPU WPR 0xFFC00000,
 *                              GPU FW 0xFFD00000, SECFW 0xFFE00000, TZDRAM 0xFFF00000; with
 *                              CARVEOUT_NVDEC_TSEC_ENABLE=1 they reach down to 0xFE600000.
 *                              The 0xFE000000 cut covers both with margin. Non-secure access
 *                              to a memory-controller carveout faults, so stay clear.
 *   0x100000000 - 0x170000000  free (bank 1, unchanged from the coreboot map)
 */
static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] =
{
	/* Address, Length, ResourceType, Resource Attribute, ARM MMU Attribute, HobOption, EFI Memory Type */
	{
		// Registers regions
		0x00000000,
		0x80000000,
		EFI_RESOURCE_MEMORY_MAPPED_IO,
		EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE,
		ARM_MEMORY_REGION_ATTRIBUTE_DEVICE,
		AddDev,
		EfiMemoryMappedIO
	},
	{
		// DRAM below the T210B01 MTC staging buffer
		0x80000000,
		0x20000000,
		EFI_RESOURCE_SYSTEM_MEMORY,
		SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
		AddMem,
		EfiConventionalMemory
	},
	{
		// hekate T210B01 MTC staging buffer (defensive)
		0xa0000000,
		0x00100000,
		EFI_RESOURCE_MEMORY_RESERVED,
		EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH,
		AddMem,
		EfiMaxMemoryType
	},
	{
		// DRAM up to bl33.bin
		0xa0100000,
		0x09f00000,
		EFI_RESOURCE_SYSTEM_MEMORY,
		SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
		AddMem,
		EfiConventionalMemory
	},
	{
		// bl33.bin: entry stub + UEFI FD
		0xaa000000,
		0x00100000,
		EFI_RESOURCE_SYSTEM_MEMORY,
		SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
		AddMem,
		EfiBootServicesCode
	},
	{
		// DRAM up to the hekate framebuffers
		0xaa100000,
		0x4b900000,
		EFI_RESOURCE_SYSTEM_MEMORY,
		SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
		AddMem,
		EfiConventionalMemory
	},
	{
		// hekate framebuffers (IPL/LOG/NYX/NYX2)
		0xf5a00000,
		0x01000000,
		EFI_RESOURCE_MEMORY_RESERVED,
		EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH,
		AddMem,
		EfiMaxMemoryType
	},
	{
		// DRAM up to the hekate carveouts
		0xf6a00000,
		0x07600000,
		EFI_RESOURCE_SYSTEM_MEMORY,
		SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
		AddMem,
		EfiConventionalMemory
	},
	{
		// HLOS memory 4 (bank 1)
		0x100000000,
		0x70000000,
		EFI_RESOURCE_SYSTEM_MEMORY,
		SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
		AddMem,
		EfiConventionalMemory
	},
	/* Terminator */
	{ 0 }
};
#else /* !SWITCH_BOOT_CHAIN_HEKATE: coreboot + ARM-TF chain */
static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] =
{
	/* Address, Length, ResourceType, Resource Attribute, ARM MMU Attribute, HobOption, EFI Memory Type */
	{
		// Registers regions
		0x00000000,
		0x80000000,
		EFI_RESOURCE_MEMORY_MAPPED_IO, 
		EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE, 
		ARM_MEMORY_REGION_ATTRIBUTE_DEVICE, 
		AddDev, 
		EfiMemoryMappedIO
	},
	{
		// HLOS memory 1 (hopefully)
		0x80000000,
		0x00110000,
		EFI_RESOURCE_SYSTEM_MEMORY,
		SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
		AddMem,
		EfiConventionalMemory
	},
	{
		// UEFI FD
		0x80110000,
		0x000E0000,
		EFI_RESOURCE_SYSTEM_MEMORY,
		SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
		AddMem,
		EfiBootServicesCode
	},
	{
		// HLOS memory 2
		0x801f0000,
		0x5f990000,
		EFI_RESOURCE_SYSTEM_MEMORY,
		SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
		AddMem,
		EfiConventionalMemory
	},
	{
		// Display Reserved
		0xdfb80000,
		0x480000,
		EFI_RESOURCE_MEMORY_RESERVED, 
		EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE, 
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH, 
		AddMem, 
		EfiMaxMemoryType
	},
	{
		// HLOS memory 5
		// Be aware that we are avoiding carveout regions.
		// The carveout is configured in Coreboot.
		// VPR has been modified, do not attempt to call hardware codec
		0xE0000000,
		0x1E900000,
		EFI_RESOURCE_SYSTEM_MEMORY,
		SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
		AddMem,
		EfiConventionalMemory
	},
	// Carveout
	{
		// HLOS memory 4 (they told me this, but actually not)
		0x100000000,
		0x70000000,
		EFI_RESOURCE_SYSTEM_MEMORY,
		SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES,
		ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK,
		AddMem,
		EfiConventionalMemory
	},
	/* Terminator */
	{ 0 }
};
#endif /* SWITCH_BOOT_CHAIN_HEKATE */

#endif