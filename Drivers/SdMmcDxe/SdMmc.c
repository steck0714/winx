// SdMmc.c - SD/MMC driver for NintendoSwitchPkg (GPL-2.0-only, see ../../LICENSE).
//
// Portions ported from hekate (https://github.com/CTCaer/hekate, bdk/storage/sdmmc_driver.c, sdmmc.c),
// Copyright (c) 2018-2024 CTCaer, GPL-2.0-only: SDMMC1 power-up sequencing, pad calibration and vendor
// setup after controller reset. Modified for winx, 2026 (see ../../NOTICE.winx.md).

#include <PiDxe.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/DevicePath.h>
#include <Library/ArmLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Guid/EventGroup.h>

#include <Protocol/UBootClockManagement.h>
#include <Protocol/Utc/Clock.h>
#include <Protocol/Utc/ErrNo.h>
#include <Protocol/Utc/Mmc.h>
#include <Protocol/Utc/UBootBlk.h>

#include <Foundation/Types.h>
#include <Device/T210.h>
#include <Device/Pmc.h>
#include <Library/GpioLib.h>
#include <Protocol/Pmic.h>
#include <Shim/DebugLib.h>
#include <Shim/UBootIo.h>
#include <Shim/TimerLib.h>
#include <Shim/BitOps.h>
#include <Library/Utc/BounceBuf.h>

#include "Include/SdMmc.h"
#include "Include/HostOp.h"
#include "Include/EfiProto.h"

TEGRA210_UBOOT_CLOCK_MANAGEMENT_PROTOCOL* mClkProtocol;
PMIC_PROTOCOL* mPmicProtocol;
MMC_CONFIG mConfig;
TEGRA_MMC_PRIV mPriv;
struct mmc mMmcInstance;
struct blk_desc mBlkDesc;


//
// ---- Diagnostics -----------------------------------------------------------
// hekate leaves no UART for BL33, so SD bring-up progress is collected in a buffer and printed
// on the console right before the boot manager runs (ReadyToBoot), and kept in a volatile
// variable "SdDiag" (Shell: dmpstore SdDiag).
//
// Data path state. mSdFast is set once the card is fully initialised: from then on the 1 ms
// pacing delay after every command is dropped (it capped a 32 KiB read at ~6 MiB/s).
BOOLEAN mSdFast = FALSE;
#define SD_DMA_BUF_PAGES   128                       // 512 KiB persistent bounce buffer
#define SD_DMA_BUF_SIZE    (SD_DMA_BUF_PAGES * EFI_PAGE_SIZE)
STATIC VOID *mDmaBuf = NULL;

STATIC CHAR16 mSdDiag[3072];
STATIC UINTN  mSdDiagLen;

VOID
SdLog (
    IN CONST CHAR16 *Fmt,
    ...
)
{
    VA_LIST Args;
    UINTN   Room = (sizeof (mSdDiag) / sizeof (CHAR16)) - mSdDiagLen;
    if (Room < 8) return;
    VA_START (Args, Fmt);
    mSdDiagLen += UnicodeVSPrint (&mSdDiag[mSdDiagLen], Room * sizeof (CHAR16), Fmt, Args);
    VA_END (Args);
}

// Publish the log as a volatile variable "SdDiag" (called after every logged read error too, so a
// tool run later from the Shell, e.g. SdReadTest, can show what happened).
VOID
SdDiagPublish (
    VOID
)
{
    gRT->SetVariable (
        L"SdDiag",
        &gEfiCallerIdGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS,
        (mSdDiagLen + 1) * sizeof (CHAR16),
        mSdDiag
    );
}

STATIC VOID SdLogControllerRegs (VOID);

STATIC
VOID
EFIAPI
SdDiagOnReadyToBoot (
    IN EFI_EVENT Event,
    IN VOID      *Context
)
{
    SdLogControllerRegs ();
    if (gST->ConOut != NULL) {
        gST->ConOut->OutputString (gST->ConOut, mSdDiag);
    }
    SdDiagPublish ();
    gBS->CloseEvent (Event);
}

//
// ---- Platform power sequencing (ported from hekate bdk/storage/sdmmc_driver.c) -----------
// hekate calls sdmmc1_disable_power() before it hands over to L4T/BL33: SD card power, LDO2,
// the SDMMC1 IO power domain and the pad configuration are all off when UEFI starts.
//
#define SD_APB_MISC_BASE            0x70000000UL
#define SD_PINMUX_AUX_BASE          0x70003000UL
#define SD_PMC_BASE                 0x7000E400UL
#define SD_BASE                     0x700B0000UL

#define APB_MISC_SDMMC1_CLK_LPBK    (SD_APB_MISC_BASE + 0x8D4)
#define APB_MISC_SDMMC1_PADCTRL     (SD_APB_MISC_BASE + 0xA98)
#define APB_MISC_VGPIO_MUX_SEL      (SD_APB_MISC_BASE + 0xB74)
#define PMC_NO_IOPOWER              (SD_PMC_BASE + 0x44)
#define PMC_PWR_DET_VAL             (SD_PMC_BASE + 0xE4)
#define PMC_SDMMC1_BIT              (1U << 12)

#define PINMUX_SDMMC1_CLK           (SD_PINMUX_AUX_BASE + 0x00)
#define PINMUX_SDMMC1_CMD           (SD_PINMUX_AUX_BASE + 0x04)
#define PINMUX_SDMMC1_DAT3          (SD_PINMUX_AUX_BASE + 0x08)
#define PINMUX_SDMMC1_DAT2          (SD_PINMUX_AUX_BASE + 0x0C)
#define PINMUX_SDMMC1_DAT1          (SD_PINMUX_AUX_BASE + 0x10)
#define PINMUX_SDMMC1_DAT0          (SD_PINMUX_AUX_BASE + 0x14)
#define PINMUX_DMIC3_CLK            (SD_PINMUX_AUX_BASE + 0xB4)
#define PINMUX_GPIO_PZ1             (SD_PINMUX_AUX_BASE + 0x280)
#define PINMUX_PULL_DOWN_V          (1U << 2)
#define PINMUX_PULL_UP_V            (2U << 2)
#define PINMUX_INPUT_ENABLE_V       (1U << 6)
#define PINMUX_DRIVE_2X_V           (1U << 13)

// SDMMC1 vendor registers (offsets in the controller window)
#define SD_VENCLKCTL                0x100
#define SD_VENIOTRIMCTL             0x1AC
#define SD_SDMEMCMPPADCTL           0x1E0
#define SD_AUTOCALCFG               0x1E4
#define SD_AUTOCALSTS               0x1EC
#define SD_IOSPARE                  0x1F0

// Standard SDHCI registers + Tegra vendor registers of SDMMC1, logged for Windows bring-up: the
// in-box SD port driver (sdport/PNP0D40) trusts CAPABILITIES; Linux marks Tegra210 with
// SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN / NO_HISPD_BIT, so these values show what Windows will see.
STATIC
VOID
SdLogControllerRegs (
    VOID
)
{
    SdLog (L"[SDDIAG] SDHCI caps=%08x %08x ver=%04x pres=%08x pwr=%02x hc=%02x clk=%04x\r\n",
           MmioRead32 (SD_BASE + 0x40), MmioRead32 (SD_BASE + 0x44),
           MmioRead16 (SD_BASE + 0xFE), MmioRead32 (SD_BASE + 0x24),
           MmioRead8  (SD_BASE + 0x29), MmioRead8  (SD_BASE + 0x28),
           MmioRead16 (SD_BASE + 0x2C));
    SdLog (L"[SDDIAG] vendor clk=%08x misc=%08x capov=%08x iotrim=%08x memcmp=%08x autocal=%08x/%08x\r\n",
           MmioRead32 (SD_BASE + SD_VENCLKCTL), MmioRead32 (SD_BASE + 0x120),
           MmioRead32 (SD_BASE + 0x10C), MmioRead32 (SD_BASE + SD_VENIOTRIMCTL),
           MmioRead32 (SD_BASE + SD_SDMEMCMPPADCTL), MmioRead32 (SD_BASE + SD_AUTOCALCFG),
           MmioRead32 (SD_BASE + SD_AUTOCALSTS));
}

// Hand the controller to the OS in a quiet, still-powered state: no pending interrupt status, no
// interrupt signal routed to the GIC (Windows installs its own handler), SDMA idle. Power, pads,
// clock and the card itself are left alone - the OS driver re-initialises the card (CMD0).
STATIC
VOID
EFIAPI
SdQuiesceOnExitBootServices (
    IN EFI_EVENT Event,
    IN VOID      *Context
)
{
    MmioWrite32 (SD_BASE + 0x38, 0);            // signal enable
    MmioWrite32 (SD_BASE + 0x34, 0);            // status enable
    MmioWrite32 (SD_BASE + 0x30, 0xFFFFFFFF);   // clear status
}


STATIC
BOOLEAN
SdCardInserted (
    VOID
)
{
    return (gpio_read (GPIO_PORT_Z, GPIO_PIN_1) == 0);
}

STATIC
VOID
SdPowerOff (
    VOID
)
{
    // Discharge state, then drop LDO2 and the card supply (hekate sdmmc1_disable_power()).
    if (mPmicProtocol != NULL) {
        mPmicProtocol->DisableRegulator (REGULATOR_LDO2);
    }
    udelay (4000);
    gpio_write (GPIO_PORT_E, GPIO_PIN_4, GPIO_LOW);
    udelay (10000);
    MmioOr32 (PMC_NO_IOPOWER, PMC_SDMMC1_BIT);
    (VOID) MmioRead32 (PMC_NO_IOPOWER);
    MmioOr32 (PMC_PWR_DET_VAL, PMC_SDMMC1_BIT);
    (VOID) MmioRead32 (PMC_PWR_DET_VAL);
}

STATIC
VOID
SdPowerOn (
    VOID
)
{
    // Card detect: PZ1 as GPIO input with pull-up.
    MmioWrite32 (PINMUX_GPIO_PZ1, PINMUX_INPUT_ENABLE_V | PINMUX_PULL_UP_V | 2);
    MmioWrite32 (APB_MISC_VGPIO_MUX_SEL, 0);
    gpio_config (GPIO_PORT_Z, GPIO_PIN_1, GPIO_MODE_GPIO);
    gpio_output_enable (GPIO_PORT_Z, GPIO_PIN_1, GPIO_OUTPUT_DISABLE);
    udelay (100);

    // Deep loopback for the CLK pad so reads work.
    MmioWrite32 (APB_MISC_SDMMC1_CLK_LPBK, 1);

    // SDMMC1 pad pinmux (T210: CLK pulled down, others pulled up).
    MmioWrite32 (PINMUX_SDMMC1_CLK,  PINMUX_DRIVE_2X_V | PINMUX_INPUT_ENABLE_V | PINMUX_PULL_DOWN_V);
    MmioWrite32 (PINMUX_SDMMC1_CMD,  PINMUX_DRIVE_2X_V | PINMUX_INPUT_ENABLE_V | PINMUX_PULL_UP_V);
    MmioWrite32 (PINMUX_SDMMC1_DAT3, PINMUX_DRIVE_2X_V | PINMUX_INPUT_ENABLE_V | PINMUX_PULL_UP_V);
    MmioWrite32 (PINMUX_SDMMC1_DAT2, PINMUX_DRIVE_2X_V | PINMUX_INPUT_ENABLE_V | PINMUX_PULL_UP_V);
    MmioWrite32 (PINMUX_SDMMC1_DAT1, PINMUX_DRIVE_2X_V | PINMUX_INPUT_ENABLE_V | PINMUX_PULL_UP_V);
    MmioWrite32 (PINMUX_SDMMC1_DAT0, PINMUX_DRIVE_2X_V | PINMUX_INPUT_ENABLE_V | PINMUX_PULL_UP_V);
    // Pads M0..M5 back to their special function (hekate leaves them as discharge GPIOs).
    gpio_output_enable (GPIO_PORT_M, 0x3F, GPIO_OUTPUT_DISABLE);
    gpio_config (GPIO_PORT_M, 0x3F, GPIO_MODE_SPIO);

    // Make sure the SDMMC1 IO power domain is powered.
    MmioOr32  (PMC_NO_IOPOWER, PMC_SDMMC1_BIT);
    udelay (1000);
    MmioAnd32 (PMC_NO_IOPOWER, ~PMC_SDMMC1_BIT);
    (VOID) MmioRead32 (PMC_NO_IOPOWER);

    // SD card power (GPIO E4, routed through DMIC3_CLK), 3..10 ms to settle.
    MmioWrite32 (PINMUX_DMIC3_CLK, PINMUX_PULL_DOWN_V | 2);
    gpio_config (GPIO_PORT_E, GPIO_PIN_4, GPIO_MODE_GPIO);
    gpio_write (GPIO_PORT_E, GPIO_PIN_4, GPIO_HIGH);
    gpio_output_enable (GPIO_PORT_E, GPIO_PIN_4, GPIO_OUTPUT_ENABLE);
    udelay (10000);

    // Tell the IO pads that the rail is 3.3 V, then enable LDO2 (3.3 V).
    MmioOr32 (PMC_PWR_DET_VAL, PMC_SDMMC1_BIT);
    (VOID) MmioRead32 (PMC_PWR_DET_VAL);
    if (mPmicProtocol != NULL) {
        mPmicProtocol->SetRegulatorVoltage (REGULATOR_LDO2, 3300000);
        mPmicProtocol->EnableRegulator (REGULATOR_LDO2);
    }
    udelay (1000);

    // Pad slew codes for a clean clock.
    MmioWrite32 (APB_MISC_SDMMC1_PADCTRL, (MmioRead32 (APB_MISC_SDMMC1_PADCTRL) & 0x0FFFFFFF) | 0x50000000);
    (VOID) MmioRead32 (APB_MISC_SDMMC1_PADCTRL);
    udelay (1000);
}

// Vendor/pad setup that has to follow a controller "reset all" (hekate sdmmc_init()).
STATIC
VOID
SdVendorInit (
    VOID
)
{
    UINT32 V;
    UINTN  Timeout;
    BOOLEAN ClkWasOn;

    MmioOr32  (SD_BASE + SD_IOSPARE, (1U << 19));                      // 1 cycle delayed cmd_oen
    MmioAnd32 (SD_BASE + SD_VENIOTRIMCTL, ~(1U << 2));                  // band gap VREG supplies the DLL
    V = MmioRead32 (SD_BASE + SD_VENCLKCTL);
    MmioWrite32 (SD_BASE + SD_VENCLKCTL, (V & 0xE0FFFFFB) | (2U << 24)); // trim value (T210, SDMMC1)
    V = MmioRead32 (SD_BASE + SD_SDMEMCMPPADCTL);
    MmioWrite32 (SD_BASE + SD_SDMEMCMPPADCTL, (V & ~0xFU) | 7);          // vref_sel = 7
    // Auto-cal offsets for 3.3 V on T210: pd = -3, pu = 0.
    V = MmioRead32 (SD_BASE + SD_AUTOCALCFG);
    MmioWrite32 (SD_BASE + SD_AUTOCALCFG, (V & 0xFFFF8080) | (0x7DU << 8));

    // Pad auto calibration.
    ClkWasOn = (MmioRead16 (SD_BASE + 0x2C) & (1U << 2)) != 0;
    if (ClkWasOn) MmioAnd16 (SD_BASE + 0x2C, (UINT16) ~(1U << 2));
    if (!(MmioRead32 (SD_BASE + SD_SDMEMCMPPADCTL) & (1U << 31))) {
        MmioOr32 (SD_BASE + SD_SDMEMCMPPADCTL, (1U << 31));
        udelay (1);
    }
    MmioOr32 (SD_BASE + SD_AUTOCALCFG, (1U << 29) | (1U << 31));
    udelay (2);
    for (Timeout = 10000; Timeout != 0; Timeout--) {
        if (!(MmioRead32 (SD_BASE + SD_AUTOCALSTS) & (1U << 31))) break;
        udelay (1);
    }
    if (Timeout == 0) {
        // Calibration failed: load the suggested standard 3.3 V 50 ohm values.
        MmioAnd32 (SD_BASE + SD_AUTOCALCFG, ~(1U << 29));
        V = MmioRead32 (APB_MISC_SDMMC1_PADCTRL) & 0xF8080FFF;
        MmioWrite32 (APB_MISC_SDMMC1_PADCTRL, V | (0xC0CU << 12));
        SdLog (L"[SDDIAG] autocal TIMEOUT (fallback pad cfg)\r\n");
    }
    SdLog (L"[SDDIAG] autocal sts=%08x cfg=%08x padctl=%08x\r\n",
           MmioRead32 (SD_BASE + SD_AUTOCALSTS), MmioRead32 (SD_BASE + SD_AUTOCALCFG),
           MmioRead32 (APB_MISC_SDMMC1_PADCTRL));
    MmioAnd32 (SD_BASE + SD_SDMEMCMPPADCTL, ~(1U << 31));
    if (ClkWasOn) MmioOr16 (SD_BASE + 0x2C, (UINT16) (1U << 2));
}

void tegra_mmc_set_power(
    struct tegra_mmc_priv *priv,
    unsigned short power)
{
	u8 pwr = 0;
	debug("%s: power = %x\n", __func__, power);

	if (power != (unsigned short)-1) {
		switch (1 << power) {
		case MMC_VDD_165_195:
			pwr = TEGRA_MMC_PWRCTL_SD_BUS_VOLTAGE_V1_8;
			break;
		case MMC_VDD_29_30:
		case MMC_VDD_30_31:
			pwr = TEGRA_MMC_PWRCTL_SD_BUS_VOLTAGE_V3_0;
			break;
		case MMC_VDD_32_33:
		case MMC_VDD_33_34:
			pwr = TEGRA_MMC_PWRCTL_SD_BUS_VOLTAGE_V3_3;
			break;
		}
	}
	debug("%s: pwr = %X\n", __func__, pwr);

	/* Set the bus voltage first (if any) */
	writeb(pwr, &priv->reg->pwrcon);
	if (pwr == 0) return;

	/* Now enable bus power */
	pwr |= TEGRA_MMC_PWRCTL_SD_BUS_POWER;
	writeb(pwr, &priv->reg->pwrcon);
}

void tegra_mmc_prepare_data(
    struct tegra_mmc_priv *priv,
    struct mmc_data *data,
    struct bounce_buffer *bbstate
)
{
	unsigned char ctrl;


	debug("buf: %p (%p), data->blocks: %u, data->blocksize: %u\n",
		bbstate->bounce_buffer, bbstate->user_buffer, data->blocks,
		data->blocksize);
	
	ASSERT(((UINTN) bbstate->bounce_buffer) < __UINT32_MAX__);
	writel((u32)(UINTN) bbstate->bounce_buffer, &priv->reg->sysad);
	/*
	 * DMASEL[4:3]
	 * 00 = Selects SDMA
	 * 01 = Reserved
	 * 10 = Selects 32-bit Address ADMA2
	 * 11 = Selects 64-bit Address ADMA2
	 */
	ctrl = readb(&priv->reg->hostctl);
	ctrl &= ~TEGRA_MMC_HOSTCTL_DMASEL_MASK;
	ctrl |= TEGRA_MMC_HOSTCTL_DMASEL_SDMA;
	writeb(ctrl, &priv->reg->hostctl);

	/* We do not handle DMA boundaries, so set it to max (512 KiB) */
	writew((7 << 12) | (data->blocksize & 0xFFF), &priv->reg->blksize);
	writew(data->blocks, &priv->reg->blkcnt);
}

void tegra_mmc_set_transfer_mode(
    struct tegra_mmc_priv *priv,
    struct mmc_data *data
)
{
	unsigned short mode;
	debug(" mmc_set_transfer_mode called\n");
	/*
	 * TRNMOD
	 * MUL1SIN0[5]	: Multi/Single Block Select
	 * RD1WT0[4]	: Data Transfer Direction Select
	 *	1 = read
	 *	0 = write
	 * ENACMD12[2]	: Auto CMD12 Enable
	 * ENBLKCNT[1]	: Block Count Enable
	 * ENDMA[0]	: DMA Enable
	 */
	mode = (TEGRA_MMC_TRNMOD_DMA_ENABLE |
		TEGRA_MMC_TRNMOD_BLOCK_COUNT_ENABLE);

	if (data->blocks > 1)
		mode |= TEGRA_MMC_TRNMOD_MULTI_BLOCK_SELECT;

	if (data->flags & MMC_DATA_READ)
		mode |= TEGRA_MMC_TRNMOD_DATA_XFER_DIR_SEL_READ;
#ifdef SWITCH_SD_FAST
	// Auto CMD12: the controller sends the stop command itself (saves one command round trip).
	if (mSdFast && (data->flags & MMC_DATA_READ) && data->blocks > 1)
		mode |= (1 << 2);
#endif

	writew(mode, &priv->reg->trnmod);
}

int tegra_mmc_wait_inhibit(
    struct tegra_mmc_priv *priv,
    struct mmc_cmd *cmd,
    struct mmc_data *data,
    unsigned int timeout
)
{
	/*
	 * PRNSTS
	 * CMDINHDAT[1] : Command Inhibit (DAT)
	 * CMDINHCMD[0] : Command Inhibit (CMD)
	 */
	unsigned int mask = TEGRA_MMC_PRNSTS_CMD_INHIBIT_CMD;

	/*
	 * We shouldn't wait for data inhibit for stop commands, even
	 * though they might use busy signaling
	 */
	if ((data == NULL) && (cmd->resp_type & MMC_RSP_BUSY))
		mask |= TEGRA_MMC_PRNSTS_CMD_INHIBIT_DAT;

	while (readl(&priv->reg->prnsts) & mask) {
		if (timeout == 0) {
			printf("%s: timeout error\n", __func__);
			return -1;
		}
		timeout--;
		udelay(1000);
	}

	return 0;
}


// Bring the host controller back to a sane state after a failed command/transfer: reset the CMD and
// DAT lines and clear every latched status bit. Without this a single CRC/timeout error left the
// controller wedged and every following read returned Device Error.
VOID
tegra_mmc_recover_lines (
    VOID
)
{
	struct tegra_mmc_priv *priv = &mPriv;
	unsigned int timeout = 100;

	writeb(TEGRA_MMC_SWRST_SW_RESET_FOR_CMD_LINE | TEGRA_MMC_SWRST_SW_RESET_FOR_DAT_LINE, &priv->reg->swrst);
	while (readb(&priv->reg->swrst) & (TEGRA_MMC_SWRST_SW_RESET_FOR_CMD_LINE | TEGRA_MMC_SWRST_SW_RESET_FOR_DAT_LINE)) {
		if (timeout-- == 0) break;
		udelay(100);
	}
	writel(0xffffffff, &priv->reg->norintsts);
	writew(0xffff, &priv->reg->acmd12errsts);
}

UINT32 mLastXferSts;
UINT16 mLastAcmd12;

int tegra_mmc_send_cmd_bounced(
    struct tegra_mmc_priv *priv, 
    struct mmc_cmd *cmd,
    struct mmc_data *data,
    struct bounce_buffer *bbstate
)
{
	int flags, i;
	int result;
	unsigned int mask = 0;
	unsigned int retry = 0x100000;
	debug(" mmc_send_cmd called\n");

	result = tegra_mmc_wait_inhibit(priv, cmd, data, 10 /* ms */);

	if (result < 0)
		return result;

	if (data)
		tegra_mmc_prepare_data(priv, data, bbstate);

	debug("cmd->arg: %08x\n", cmd->cmdarg);
	writel(cmd->cmdarg, &priv->reg->argument);

	if (data)
		tegra_mmc_set_transfer_mode(priv, data);

	if ((cmd->resp_type & MMC_RSP_136) && (cmd->resp_type & MMC_RSP_BUSY))
	{
		debug("MMC_RSP_136 or MMC_RSP_BUSY cannot coexist \n");
		return -1;
	}

	/*
	 * CMDREG
	 * CMDIDX[13:8]	: Command index
	 * DATAPRNT[5]	: Data Present Select
	 * ENCMDIDX[4]	: Command Index Check Enable
	 * ENCMDCRC[3]	: Command CRC Check Enable
	 * RSPTYP[1:0]
	 *	00 = No Response
	 *	01 = Length 136
	 *	10 = Length 48
	 *	11 = Length 48 Check busy after response
	 */
	if (!(cmd->resp_type & MMC_RSP_PRESENT))
		flags = TEGRA_MMC_CMDREG_RESP_TYPE_SELECT_NO_RESPONSE;
	else if (cmd->resp_type & MMC_RSP_136)
		flags = TEGRA_MMC_CMDREG_RESP_TYPE_SELECT_LENGTH_136;
	else if (cmd->resp_type & MMC_RSP_BUSY)
		flags = TEGRA_MMC_CMDREG_RESP_TYPE_SELECT_LENGTH_48_BUSY;
	else
		flags = TEGRA_MMC_CMDREG_RESP_TYPE_SELECT_LENGTH_48;

	if (cmd->resp_type & MMC_RSP_CRC)
		flags |= TEGRA_MMC_TRNMOD_CMD_CRC_CHECK;
	if (cmd->resp_type & MMC_RSP_OPCODE)
		flags |= TEGRA_MMC_TRNMOD_CMD_INDEX_CHECK;
	if (data)
		flags |= TEGRA_MMC_TRNMOD_DATA_PRESENT_SELECT_DATA_TRANSFER;

	debug("cmd: %d\n", cmd->cmdidx);

	writew((cmd->cmdidx << 8) | flags, &priv->reg->cmdreg);

	for (i = 0; i < retry; i++) 
	{
		mask = readl(&priv->reg->norintsts);
		/* Command Complete */
		if (mask & TEGRA_MMC_NORINTSTS_CMD_COMPLETE) 
		{
			if (!data) writel(mask, &priv->reg->norintsts);
			break;
		}
	}

	if (i == retry) 
	{
		printf("%s: waiting for status update\n", __func__);
		writel(mask, &priv->reg->norintsts);
		if (mSdFast) tegra_mmc_recover_lines();
		return -ETIMEDOUT;
	}

	if (mask & TEGRA_MMC_NORINTSTS_CMD_TIMEOUT) 
	{
		/* Timeout Error */
		debug("timeout: %08x cmd %d \n", mask, cmd->cmdidx);
		writel(mask, &priv->reg->norintsts);
		if (mSdFast) tegra_mmc_recover_lines();
		return -ETIMEDOUT;
	} 
	else if (mask & TEGRA_MMC_NORINTSTS_ERR_INTERRUPT) 
	{
		/* Error Interrupt */
		debug("error: %08x cmd %d \n", mask, cmd->cmdidx);
		writel(mask, &priv->reg->norintsts);
		if (mSdFast) tegra_mmc_recover_lines();
		return -1;
	}

	if (cmd->resp_type & MMC_RSP_PRESENT) 
	{
		if (cmd->resp_type & MMC_RSP_136) 
		{
			/* CRC is stripped so we need to do some shifting. */
			for (i = 0; i < 4; i++) 
			{
				unsigned long offset = (unsigned long)
					(&priv->reg->rspreg3 - i);
				cmd->response[i] = readl(offset) << 8;

				if (i != 3) 
				{
					cmd->response[i] |= readb(offset - 1);
				}
				debug("cmd->resp[%d]: %08x\n", i, cmd->response[i]);
			}
		} 
		else if (cmd->resp_type & MMC_RSP_BUSY) 
		{
			for (i = 0; i < retry; i++) 
			{
				/* PRNTDATA[23:20] : DAT[3:0] Line Signal */
				if (readl(&priv->reg->prnsts)
					& (1 << 20))	/* DAT[0] */
					break;
			}

			if (i == retry) 
			{
				printf("%s: card is still busy\n", __func__);
				writel(mask, &priv->reg->norintsts);
				return -ETIMEDOUT;
			}

			cmd->response[0] = readl(&priv->reg->rspreg0);
			debug("cmd->resp[0]: %08x\n", cmd->response[0]);
		} 
		else 
		{
			cmd->response[0] = readl(&priv->reg->rspreg0);
			debug("cmd->resp[0]: %08x\n", cmd->response[0]);
		}
	}

	if (data) 
	{
		unsigned long start = get_timer(0);

		while (1) 
		{
			mask = readl(&priv->reg->norintsts);

			if (mask & TEGRA_MMC_NORINTSTS_ERR_INTERRUPT) 
			{
				/* Error Interrupt */
				mLastXferSts = mask;
				mLastAcmd12 = readw(&priv->reg->acmd12errsts);
				writel(mask, &priv->reg->norintsts);
				printf("%s: error during transfer: 0x%08x\n",
						__func__, mask);
				tegra_mmc_recover_lines();
				return -1;
			} 
			else if (mask & TEGRA_MMC_NORINTSTS_DMA_INTERRUPT) 
			{
				/*
				 * DMA Interrupt, restart the transfer where
				 * it was interrupted.
				 */
				unsigned int address = readl(&priv->reg->sysad);

				debug("DMA end\n");
				writel(TEGRA_MMC_NORINTSTS_DMA_INTERRUPT,
				       &priv->reg->norintsts);
				writel(address, &priv->reg->sysad);
			} 
			else if (mask & TEGRA_MMC_NORINTSTS_XFER_COMPLETE) 
			{
				/* Transfer Complete */
				debug("r/w is done\n");
				break;
			} 
			else if (get_timer(start) > 8000UL) 
			{
				mLastXferSts = mask;
				mLastAcmd12 = readw(&priv->reg->acmd12errsts);
				writel(mask, &priv->reg->norintsts);
				printf("%s: MMC Timeout\n"
				       "    Interrupt status        0x%08x\n"
				       "    Interrupt status enable 0x%08x\n"
				       "    Interrupt signal enable 0x%08x\n"
				       "    Present status          0x%08x\n",
				       __func__, mask,
				       readl(&priv->reg->norintstsen),
				       readl(&priv->reg->norintsigen),
				       readl(&priv->reg->prnsts));
				tegra_mmc_recover_lines();
				return -1;
			}
		}
		writel(mask, &priv->reg->norintsts);
	}

	if (!mSdFast) udelay(1000);
	return 0;
}

int tegra_mmc_send_cmd(
    struct tegra_mmc_priv *priv, 
    struct mmc_cmd *cmd,
    struct mmc_data *data
)
{
	void *buf;
	unsigned int bbflags;
	size_t len;
	struct bounce_buffer bbstate;
	BOOLEAN UseStatic = FALSE;
	int ret;

	if (data) 
	{
		if (data->flags & MMC_DATA_READ) 
		{
			buf = data->dest;
			bbflags = GEN_BB_WRITE;
		} 
		else 
		{
			buf = (void *)data->src;
			bbflags = GEN_BB_READ;
		}
		len = data->blocks * data->blocksize;

		// Zero-copy when the caller's buffer is already DMA-able (64-byte aligned, low memory);
		// otherwise use the persistent bounce buffer instead of allocating pages on every request.
		if (mDmaBuf != NULL && len <= SD_DMA_BUF_SIZE &&
		    (((UINTN) buf & 63) != 0 || (len & 63) != 0 ||
		     ((UINTN) buf + len) > (UINTN)(FixedPcdGet64(PcdSystemMemoryBase) + SIZE_1GB)))
		{
			UseStatic = TRUE;
			bbstate.user_buffer = buf;
			bbstate.bounce_buffer = mDmaBuf;
			bbstate.len = len;
			bbstate.len_aligned = (len + 63) & ~(size_t) 63;
			bbstate.flags = bbflags;
			if (bbflags & GEN_BB_READ) CopyMem(mDmaBuf, buf, len);
			WriteBackInvalidateDataCacheRange(mDmaBuf, bbstate.len_aligned);
			ret = 0;
		}
		else
		{
			ret = bounce_buffer_start(&bbstate, buf, len, bbflags);
			if (ret) return -ENOMEM;
		}
	}

	ret = tegra_mmc_send_cmd_bounced(priv, cmd, data, &bbstate);

	if (data)
	{
		if (UseStatic)
		{
			InvalidateDataCacheRange(mDmaBuf, bbstate.len_aligned);
			if (bbflags & GEN_BB_WRITE) CopyMem(buf, mDmaBuf, len);
		}
		else
		{
			bounce_buffer_stop(&bbstate);
		}
	}

	return ret;
}

void tegra_mmc_pad_init(struct tegra_mmc_priv *priv)
{
    // Nothing to do for Tegra 210
    return;
}

void tegra_mmc_change_clock(struct tegra_mmc_priv *priv, uint clock)
{
	ulong rate;
	int div;
	unsigned short clk;
	unsigned long timeout;

	debug(" mmc_change_clock called\n");

	/*
	 * Change Tegra SDMMCx clock divisor here. Source is PLLP_OUT0
	 */
	if (clock == 0) goto out;
    
	rate = mClkProtocol->SetRate(PERIPH_ID_SDMMC1, clock);
	div = (rate + clock - 1) / clock;
	debug("div = %d\n", div);

	writew(0, &priv->reg->clkcon);

	/*
	 * CLKCON
	 * SELFREQ[15:8]	: base clock divided by value
	 * ENSDCLK[2]		: SD Clock Enable
	 * STBLINTCLK[1]	: Internal Clock Stable
	 * ENINTCLK[0]		: Internal Clock Enable
	 */
	div >>= 1;
	clk = ((div << TEGRA_MMC_CLKCON_SDCLK_FREQ_SEL_SHIFT) |
	       TEGRA_MMC_CLKCON_INTERNAL_CLOCK_ENABLE);
	writew(clk, &priv->reg->clkcon);

	/* Wait max 10 ms */
	timeout = 10;
	while (!(readw(&priv->reg->clkcon) &
		 TEGRA_MMC_CLKCON_INTERNAL_CLOCK_STABLE)) {
		if (timeout == 0) {
			printf("%s: timeout error\n", __func__);
			return;
		}
		timeout--;
		udelay(1000);
	}

	clk |= TEGRA_MMC_CLKCON_SD_CLOCK_ENABLE;
	writew(clk, &priv->reg->clkcon);

	debug("mmc_change_clock: clkcon = %08X\n", clk);

out:
	priv->clock = clock;
}

int tegra_mmc_set_ios(
    struct mmc* mMmcInstance
)
{
	struct tegra_mmc_priv *priv = &mPriv;
	unsigned char ctrl;
	debug(" mmc_set_ios called\n");

	debug("bus_width: %x, clock: %d\n", mMmcInstance->bus_width, mMmcInstance->clock);

	/* Change clock first */
	tegra_mmc_change_clock(priv, mMmcInstance->clock);

	ctrl = readb(&priv->reg->hostctl);

	/*
	 * WIDE8[5]
	 * 0 = Depend on WIDE4
	 * 1 = 8-bit mode
	 * WIDE4[1]
	 * 1 = 4-bit mode
	 * 0 = 1-bit mode
	 */
	if (mMmcInstance->bus_width == 8)
		ctrl |= (1 << 5);
	else if (mMmcInstance->bus_width == 4)
		ctrl |= (1 << 1);
	else
		ctrl &= ~(1 << 1 | 1 << 5);

#ifdef SWITCH_SD_FAST
	// High Speed Enable (HOSTCTL[2]): required for the 50 MHz SD high-speed mode.
	if (mMmcInstance->clock > 26000000)
		ctrl |= (1 << 2);
	else
		ctrl &= ~(1 << 2);
#endif
	writeb(ctrl, &priv->reg->hostctl);
	debug("mmc_set_ios: hostctl = %08X\n", ctrl);

	return 0;
}

EFI_STATUS
TegraMmcReset
(
    PTEGRA_MMC_PRIV priv
)
{
    unsigned int timeout;
	debug(" mmc_reset called\n");

    /*
	 * RSTALL[0] : Software reset for all
	 * 1 = reset
	 * 0 = work
	 */
	writeb(TEGRA_MMC_SWRST_SW_RESET_FOR_ALL, &priv->reg->swrst);

	priv->clock = 0;

	/* Wait max 100 ms */
	timeout = 100;

    /* hw clears the bit when it's done */
	while (readb(&priv->reg->swrst) & TEGRA_MMC_SWRST_SW_RESET_FOR_ALL) {
		if (timeout == 0) {
			printf("%s: timeout error\n", __func__);
			return EFI_TIMEOUT;
		}
		timeout--;
		udelay(1000);
	}

    /* Vendor trim + pad auto calibration (must follow reset-all) */
	SdVendorInit();

    /* Set SD bus voltage & enable bus power */
	tegra_mmc_set_power(priv, fls(mMmcInstance.cfg->voltages) - 1);
	debug("%s: power control = %02X, host control = %02X\n", __func__,
		readb(&priv->reg->pwrcon), readb(&priv->reg->hostctl));

	/* Make sure SDIO pads are set up */
	tegra_mmc_pad_init(priv);

    return EFI_SUCCESS;
}

EFI_STATUS
TegraMmcInit
(
    VOID
)
{
    PTEGRA_MMC_PRIV priv = &mPriv;
    unsigned int mask;
    EFI_STATUS Status;
	debug(" tegra_mmc_init called\n");

    Status = TegraMmcReset(priv);
    if (EFI_ERROR(Status))
    {
        DEBUG((EFI_D_ERROR, "SDMMC reset failed \n"));
        goto exit;
    }

	priv->version = readw(&priv->reg->hcver);
	debug("host version = %x\n", priv->version);

    /* mask all */
	writel(0xffffffff, &priv->reg->norintstsen);
	writel(0xffffffff, &priv->reg->norintsigen);

    writeb(0xe, &priv->reg->timeoutcon);	/* TMCLK * 2^27 */
	/*
	 * NORMAL Interrupt Status Enable Register init
	 * [5] ENSTABUFRDRDY : Buffer Read Ready Status Enable
	 * [4] ENSTABUFWTRDY : Buffer write Ready Status Enable
	 * [3] ENSTADMAINT   : DMA boundary interrupt
	 * [1] ENSTASTANSCMPLT : Transfre Complete Status Enable
	 * [0] ENSTACMDCMPLT : Command Complete Status Enable
	*/
	mask = readl(&priv->reg->norintstsen);
	mask &= ~(0xffff);
	mask |= (TEGRA_MMC_NORINTSTSEN_CMD_COMPLETE |
		 TEGRA_MMC_NORINTSTSEN_XFER_COMPLETE |
		 TEGRA_MMC_NORINTSTSEN_DMA_INTERRUPT |
		 TEGRA_MMC_NORINTSTSEN_BUFFER_WRITE_READY |
		 TEGRA_MMC_NORINTSTSEN_BUFFER_READ_READY);
	writel(mask, &priv->reg->norintstsen);

	/*
	 * NORMAL Interrupt Signal Enable Register init
	 * [1] ENSTACMDCMPLT : Transfer Complete Signal Enable
	 */
	mask = readl(&priv->reg->norintsigen);
	mask &= ~(0xffff);
	mask |= TEGRA_MMC_NORINTSIGEN_XFER_COMPLETE;
	writel(mask, &priv->reg->norintsigen);

exit:
    return Status;
}

EFI_STATUS
SdControllerProbe
(
    VOID
)
{
    int ret = 0;

	// All init
	ZeroMem(&mBlkDesc, sizeof(struct blk_desc));
	ZeroMem(&mMmcInstance, sizeof(struct mmc));
	ZeroMem(&mPriv, sizeof(TEGRA_MMC_PRIV));
	ZeroMem(&mConfig, sizeof(MMC_CONFIG));

    // Set configuration
    mConfig.voltages = MMC_VDD_32_33 | MMC_VDD_33_34 | MMC_VDD_165_195;
    mConfig.host_caps = 0;
	
    // bus-width = <4>;
    mConfig.host_caps |= MMC_MODE_4BIT;
    mConfig.host_caps |= MMC_MODE_HS_52MHz | MMC_MODE_HS;

    /*
	 * min freq is for card identification, and is the highest
	 *  low-speed SDIO card frequency (actually 400KHz)
	 * max freq is highest HS eMMC clock as per the SD/MMC spec
	 *  (actually 52MHz)
	 */
	mConfig.f_min = 375000;
	mConfig.f_max = 48000000;

	mConfig.b_max = CONFIG_SYS_MMC_MAX_BLK_COUNT;

	// Bound to instance
	mMmcInstance.cfg = &mConfig;
	mMmcInstance.clock = mConfig.f_min;

    // sdhci@700b0000
    mPriv.reg = (VOID*) (UINTN) 0x700b0000;

    // Card detect + full power-up sequence (hekate has powered the slot down)
    SdPowerOn();
    if (!SdCardInserted())
    {
        SdLog (L"[SDDIAG] card detect (PZ1) says: no card\r\n");
        DEBUG((EFI_D_ERROR, "SD card not found \n"));
        return EFI_NOT_FOUND;
    }

    // Reset controller 1, &tegra_car 14
    mClkProtocol->AssertRst(PERIPH_ID_SDMMC1);

    // Clock enable, TEGRA210_CLK_SDMMC1
    mClkProtocol->EnableClk(PERIPH_ID_SDMMC1);

    // Set Rate
    ret = mClkProtocol->SetRate(PERIPH_ID_SDMMC1, 20000000);
    if (IS_ERR_VALUE(ret))
    {
        DEBUG((EFI_D_ERROR, "SD card set rate failed \n"));
        return EFI_DEVICE_ERROR;
    }

    // De-assert
    mClkProtocol->DeassertRst(PERIPH_ID_SDMMC1);

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SdMmcDxeInitialize
(
    IN EFI_HANDLE         ImageHandle,
    IN EFI_SYSTEM_TABLE   *SystemTable
)
{
    EFI_STATUS Status;
	BIO_INSTANCE *Instance;
    EFI_EVENT DiagEvent;
    UINTN Attempt;
    int ret;

    // Print the collected [SDDIAG] lines just before the boot manager runs.
    gBS->CreateEventEx (
        EVT_NOTIFY_SIGNAL, TPL_CALLBACK, SdDiagOnReadyToBoot, NULL,
        &gEfiEventReadyToBootGuid, &DiagEvent
    );
    {
        EFI_EVENT ExitEvent;
        gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, SdQuiesceOnExitBootServices, NULL,
                            &gEfiEventExitBootServicesGuid, &ExitEvent);
    }
    SdLog (L"\r\n[SDDIAG] SdMmcDxe start\r\n");

    {
        EFI_PHYSICAL_ADDRESS DmaAddr = (EFI_PHYSICAL_ADDRESS) (FixedPcdGet64(PcdSystemMemoryBase) + SIZE_1GB - 1);
        if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, SD_DMA_BUF_PAGES, &DmaAddr))) {
            SdLog (L"[SDDIAG] WARNING: could not reserve the DMA bounce buffer\r\n");
        } else {
            mDmaBuf = (VOID *) (UINTN) DmaAddr;
        }
    }

    Status = gBS->LocateProtocol(
        &gTegraUBootClockManagementProtocolGuid,
        NULL,
        (VOID**) &mClkProtocol
    );

    if (EFI_ERROR(Status)) { SdLog (L"[SDDIAG] no clock protocol %r\r\n", Status); goto exit; }

    Status = gBS->LocateProtocol(
        &gPmicProtocolGuid,
        NULL,
        (VOID**) &mPmicProtocol
    );

    if (EFI_ERROR(Status)) { SdLog (L"[SDDIAG] no pmic protocol %r\r\n", Status); goto exit; }

    // The card is power-cycled between attempts; a timing-sensitive first try is the usual failure.
    for (Attempt = 1; Attempt <= 3; Attempt++)
    {
        SdLog (L"[SDDIAG] attempt %d\r\n", Attempt);

        Status = SdControllerProbe();
        if (EFI_ERROR(Status)) { SdLog (L"[SDDIAG]  probe %r\r\n", Status); goto retry; }

        Status = TegraMmcInit();
        if (EFI_ERROR(Status)) { SdLog (L"[SDDIAG]  host init %r\r\n", Status); goto retry; }

        // Check some commands
        ret = SdFxInit();
        if (ret)
        {
            SdLog (L"[SDDIAG]  card init (CMD0/8/ACMD41) failed ret=%d\r\n", ret);
            Status = EFI_DEVICE_ERROR;
            goto retry;
        }
        ret = SdFxInitFinalize();
        if (ret)
        {
            SdLog (L"[SDDIAG]  card startup (CID/CSD/select) failed ret=%d\r\n", ret);
            Status = EFI_DEVICE_ERROR;
            goto retry;
        }
        Status = EFI_SUCCESS;
        mSdFast = TRUE;
        break;

retry:
        if (Status == EFI_NOT_FOUND) break;     // no card: nothing to retry
        SdPowerOff();
        udelay (100000);
    }
    if (EFI_ERROR(Status)) goto exit;

	if (mMmcInstance.has_init == 1)
	{
        SdLog (L"[SDDIAG] card OK: lba=%ld blksz=%d\r\n", (INT64) mBlkDesc.lba, (INT32) mBlkDesc.blksz);

		//
		// Self test: look for a (protective) MBR / boot sector signature in the first blocks.
		//
		UINT8 BlkDump[512];
		ZeroMem(BlkDump, 512);
		BOOLEAN FoundMbr = FALSE;
		for (UINTN i = 0; i <= MIN(mBlkDesc.lba - 1, 50); i++)
		{
			int blk = mmc_bread(i, 1, &BlkDump);
			if (blk)
			{
				if (BlkDump[510] == 0x55 && BlkDump[511] == 0xAA)
				{
					DEBUG((EFI_D_INFO, "MBR found at %d \n", i));
					SdLog (L"[SDDIAG] 55AA signature at LBA %d\r\n", (INT32) i);
					FoundMbr = TRUE;
					break;
				}
				DEBUG((EFI_D_INFO, "MBR not found at %d \n", i));
			}
			else
			{
				SdLog (L"[SDDIAG] read of LBA %d FAILED\r\n", (INT32) i);
				break;
			}
		}

		if (!FoundMbr)
		{
			// Not fatal any more (used to CpuDeadLoop): let PartitionDxe judge the disk.
			SdLog (L"[SDDIAG] no 55AA signature in first blocks\r\n");
		}

		// Install EFI protocol
		ASSERT(mBlkDesc.lba != 0);
		ASSERT(mBlkDesc.blksz != 0);
		Status = BioInstanceContructor(&Instance);
		if (EFI_ERROR(Status)) goto exit;

		Instance->BlockMedia.BlockSize = mBlkDesc.blksz;
		// mBlkDesc.lba is the block COUNT; Media->LastBlock is the index of the last block.
		Instance->BlockMedia.LastBlock = mBlkDesc.lba - 1;
		Status = gBS->InstallMultipleProtocolInterfaces(
			&Instance->Handle,
			&gEfiBlockIoProtocolGuid,    
			&Instance->BlockIo,
			&gEfiDevicePathProtocolGuid, 
			&Instance->DevicePath,
			NULL
		);
        SdLog (L"[SDDIAG] BlockIo install: %r (fast=%d clk=%d bw=%d)\r\n", Status, (INT32) mSdFast,
               (INT32) mMmcInstance.clock, (INT32) mMmcInstance.bus_width);
	}

exit:
    SdLog (L"[SDDIAG] SdMmcDxe result: %r\r\n", Status);
    return Status;
}
