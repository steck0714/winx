/* SPDX-License-Identifier: GPL-2.0-only
 * T214 SDMMC1 board support. Register sequences adapted from Hekate v6.5.3
 * (naehrwert / CTCaer) and Switchroot's Linux sdhci-tegra implementation.
 * This file is included only by the HDH-001 SDR104 build of nvtsdhc.c.
 */
#include "../../common/tegra_i2c5_gate.h"
#define T214_DMA_BYTES (1024u * 512u)
#define T214_DMA_ALLOC (T214_DMA_BYTES + PAGE_SIZE)
#define T214_IO_BIT (1u << 12)
#if NV_T214_DDR200
static NTSTATUS T214ParentClock(PSDHC_EXTENSION s);
static NTSTATUS T214DdrClock(PSDHC_EXTENSION s, ULONG Frequency);
#endif

static ULONG T214Read(PUCHAR Base, ULONG Offset)
{ return READ_REGISTER_ULONG((PULONG)(Base + Offset)); }
static VOID T214Write(PUCHAR Base, ULONG Offset, ULONG Value)
{ WRITE_REGISTER_ULONG((PULONG)(Base + Offset), Value); }
static BOOLEAN T214WaitClear(PUCHAR Base, ULONG Offset, ULONG Mask, ULONG Us)
{
    for (ULONG i = 0; i < Us; i += 5) {
        if (!(T214Read(Base, Offset) & Mask)) return TRUE;
        KeStallExecutionProcessor(5);
    }
    return FALSE;
}
static PUCHAR T214Map(ULONG Address, ULONG Length)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = Address;
    return (PUCHAR)MmMapIoSpaceEx(pa, Length, PAGE_READWRITE | PAGE_NOCACHE);
}

/* I2C5 normal-mode short messages. The gate spans register-select + read and
 * read/modify/write, including restoration of the SPB controller's mode.
 * Neither clock nor reset nor another PMIC rail is changed here.
 */
static NTSTATUS T214PmicMessage(PSDHC_EXTENSION s, BOOLEAN Read, ULONG *Word, ULONG Bytes)
{
    PUCHAR b = s->T214I2c;
    T214Write(b, 4, (0x3cu << 1) | (Read ? 1u : 0u));
    if (!Read) T214Write(b, 12, *Word);
    T214Write(b, 0, 0x2800u | (Read ? 0x40u : 0u) | ((Bytes - 1) << 1));
    T214Write(b, 0x8c, 0x25);
    if (!T214WaitClear(b, 0x8c, 1, 10000)) return STATUS_IO_TIMEOUT;
    T214Write(b, 0, T214Read(b, 0) | 0x200);
    KeStallExecutionProcessor(10);
    if (!T214WaitClear(b, 0x1c, 0x100, 20000)) return STATUS_IO_TIMEOUT;
    if (T214Read(b, 0x1c) & 0xf) return STATUS_IO_DEVICE_ERROR;
    if (Read) *Word = T214Read(b, 12);
    return STATUS_SUCCESS;
}
static NTSTATUS T214Ldo2(PSDHC_EXTENSION s, ULONG Millivolts, BOOLEAN On)
{
    NTSTATUS st = TegraI2c5Acquire(s->T214Gate);
    ULONG cfg, mask, value = 0x27, check = 0x27;
    if (!NT_SUCCESS(st)) return st;
    cfg = T214Read(s->T214I2c, 0); mask = T214Read(s->T214I2c, 0x64);
    T214Write(s->T214I2c, 0x64, 0);
    if (!T214WaitClear(s->T214I2c, 0x1c, 0x100, 20000)) { st = STATUS_IO_TIMEOUT; goto done; }
    st = T214PmicMessage(s, FALSE, &value, 1);
    if (NT_SUCCESS(st)) st = T214PmicMessage(s, TRUE, &value, 1);
    if (NT_SUCCESS(st)) {
        value = On ? ((value & 0xc0) | ((Millivolts - 800) / 50) | 0xc0) : (value & 0x3f);
        ULONG packet = 0x27 | ((value & 0xff) << 8);
        st = T214PmicMessage(s, FALSE, &packet, 2);
        if (NT_SUCCESS(st)) st = T214PmicMessage(s, FALSE, &check, 1);
        if (NT_SUCCESS(st)) st = T214PmicMessage(s, TRUE, &check, 1);
        if (NT_SUCCESS(st) && (check & 0xff) != (value & 0xff)) st = STATUS_DEVICE_POWER_FAILURE;
    }
done:
    T214Write(s->T214I2c, 0, cfg & ~0x200u);
    T214Write(s->T214I2c, 0x8c, 0x25);
    if (!T214WaitClear(s->T214I2c, 0x8c, 1, 10000)) st = STATUS_IO_TIMEOUT;
    T214Write(s->T214I2c, 0x64, mask);
    TegraI2c5Release(s->T214Gate);
    return st;
}

static VOID T214CardPower(PSDHC_EXTENSION s, BOOLEAN On)
{
    /* GPIO E4 OUT masked alias: one pin only; GPIO ownership is unchanged. */
    T214Write(s->T214Gpio, 0x1a0, (1u << 12) | (On ? (1u << 4) : 0));
    (VOID)T214Read(s->T214Gpio, 0x120);
}
static NTSTATUS T214PadCalibrate(PSDHC_EXTENSION s)
{
    PUCHAR b = s->BaseAddress;
    USHORT clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~SDHC_CC_CLOCK_ENABLE);
    ULONG value = T214Read(b, 0x1e4);
    value = (value & 0xffff8080u) | (s->T214Voltage18 ? 0x606u : 0u);
    T214Write(b, 0x1e4, value);
    T214Write(b, 0x1e0, (T214Read(b, 0x1e0) & ~0xfu) | (1u << 31));
    KeStallExecutionProcessor(1);
    T214Write(b, 0x1e4, value | (1u << 29) | (1u << 31));
    KeStallExecutionProcessor(2);
    BOOLEAN ok = T214WaitClear(b, 0x1ec, 1u << 31, 10000);
    if (!ok) {
        /* B01's specified 50-ohm fallback, PU=8 / PD=8. */
        T214Write(b, 0x1e4, value & ~(1u << 29));
        T214Write(s->T214Misc, 0xa98, (T214Read(s->T214Misc, 0xa98) & 0xf8080fff) | 0x808000);
        s->T214CalFallbacks++;
    }
    T214Write(b, 0x1e0, T214Read(b, 0x1e0) & ~(1u << 31));
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock);
    return STATUS_SUCCESS;
}
static VOID T214VendorReset(PSDHC_EXTENSION s)
{
    PUCHAR b = s->BaseAddress;
    T214Write(b, 0x1f0, T214Read(b, 0x1f0) | (1u << 19));
    T214Write(b, 0x1ac, T214Read(b, 0x1ac) & ~(1u << 2));
    T214Write(b, 0x1c0, T214Read(b, 0x1c0) & ~(1u << 17));
    T214Write(b, 0x100, (T214Read(b, 0x100) & 0xe000fffbu) | (14u << 24) | (11u << 16));
    SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2, s->T214Voltage18 ? SDHC_HC2_1_8V_SIGNALING : 0);
    s->T214Tuned = FALSE;
    (VOID)T214PadCalibrate(s);
}
static NTSTATUS T214Power(PSDHC_EXTENSION s, BOOLEAN On)
{
    LiteForgetCard(s);
    USHORT clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~SDHC_CC_CLOCK_ENABLE);
#if NV_T214_DDR200
    if (s->T214DdrActive) {
        /* The powered-off card returns to SDR. Do not restore its old DDR
         * divider while the card is coming up at 3.3 V.
         */
        s->T214DdrActive = FALSE;
        s->SpeedMode = SdhcSpeedModeNormal;
        clock = 0;
        NTSTATUS parent = T214ParentClock(s);
        if (!NT_SUCCESS(parent)) return parent;
    }
#endif
    SdhcWriteRegisterUchar(s, SDHC_POWER_CONTROL, 0);
    NTSTATUS st = T214Ldo2(s, 3300, FALSE);
    if (!NT_SUCCESS(st)) return st;
    SdPortWait(4000);
    T214CardPower(s, FALSE);
    SdPortWait(10000);
    T214Write(s->T214Pmc, 0x444, T214Read(s->T214Pmc, 0x444) | T214_IO_BIT);
    T214Write(s->T214Pmc, 0x4e4, T214Read(s->T214Pmc, 0x4e4) | T214_IO_BIT);
    s->T214CardOn = FALSE;
    s->T214Voltage18 = FALSE; s->T214Tuned = FALSE;
    if (!On) return STATUS_SUCCESS;
    T214Write(s->T214Pmc, 0x444, T214Read(s->T214Pmc, 0x444) & ~T214_IO_BIT);
    T214CardPower(s, TRUE); SdPortWait(10000);
    st = T214Ldo2(s, 3300, TRUE);
    if (!NT_SUCCESS(st)) return st;
    SdPortWait(1000);
    s->T214CardOn = TRUE;
    T214VendorReset(s);
    SdhcWriteRegisterUchar(s, SDHC_POWER_CONTROL, SDHC_PC_3_3V | SDHC_PC_BUS_POWER);
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock);
    return STATUS_SUCCESS;
}
/* A reset-all clears SDHCI POWER_CONTROL without switching GPIO E4/LDO2.
 * Physical card power and the host's output-power bit are different states.
 */
static NTSTATUS T214SetBusVoltage(PSDHC_EXTENSION s, SDPORT_BUS_VOLTAGE Voltage)
{
    if (Voltage == SdBusVoltage33 && s->T214CardOn && !s->T214Voltage18) {
        SdhcWriteRegisterUchar(s, SDHC_POWER_CONTROL, SDHC_PC_3_3V | SDHC_PC_BUS_POWER);
        SdPortWait(1000);
        return (SdhcReadRegisterUchar(s, SDHC_POWER_CONTROL) &
                (SDHC_PC_VOLTAGE_MASK | SDHC_PC_BUS_POWER)) ==
                (SDHC_PC_3_3V | SDHC_PC_BUS_POWER) ? STATUS_SUCCESS : STATUS_DEVICE_POWER_FAILURE;
    }
    return T214Power(s, Voltage == SdBusVoltage33);
}
static NTSTATUS T214Signaling(PSDHC_EXTENSION s, BOOLEAN Enable)
{
    if (Enable == s->T214Voltage18) return STATUS_SUCCESS;
    if (!Enable) return T214Power(s, TRUE); /* A CMD11 card needs an actual power cycle. */
    NTSTATUS st = STATUS_DEVICE_PROTOCOL_ERROR;
    USHORT clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);
    if (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) & SDHC_PS_DAT_3_0) goto fail;
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~SDHC_CC_CLOCK_ENABLE);
    st = T214Ldo2(s, 1800, TRUE);
    if (!NT_SUCCESS(st)) goto fail;
    KeStallExecutionProcessor(150);
    T214Write(s->T214Pmc, 0x4e4, T214Read(s->T214Pmc, 0x4e4) & ~T214_IO_BIT);
    for (ULONG off = 0x3000; off <= 0x3014; off += 4)
        T214Write(s->T214Misc, off, T214Read(s->T214Misc, off) | (1u << 12));
    s->T214Voltage18 = TRUE;
    (VOID)T214PadCalibrate(s);
    USHORT hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2) | SDHC_HC2_1_8V_SIGNALING;
    SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2, hc);
    SdhcWriteRegisterUchar(s, SDHC_POWER_CONTROL, SDHC_PC_1_8V | SDHC_PC_BUS_POWER);
    SdPortWait(5000);
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock | SDHC_CC_CLOCK_ENABLE);
    SdPortWait(1000);
    if ((SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2) & SDHC_HC2_1_8V_SIGNALING) &&
        (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) & SDHC_PS_DAT_3_0) == SDHC_PS_DAT_3_0) {
        s->T214VoltageStatus = STATUS_SUCCESS; return STATUS_SUCCESS;
    }
    st = STATUS_DEVICE_PROTOCOL_ERROR;
fail:
    s->T214VoltageStatus = st; s->T214VoltageFailures++;
    (VOID)T214Power(s, TRUE);
    return st;
}

static NTSTATUS T214ParentClock(PSDHC_EXTENSION s)
{
    /* PLLC4 OUT2 = 38.4 MHz * 104 / 4 / 5 = 199.68 MHz. Never retune
     * an already-enabled PLL: it can also feed USB or another SDMMC. */
    PUCHAR c = s->T214Car;
    ULONG base = T214Read(c, 0x5a4);
    if (!(base & (1u << 30))) {
        T214Write(c, 0x5a8, T214Read(c, 0x5a8) | (1u << 30));
        T214Write(c, 0x5a4, base & ~((1u << 30) | (1u << 18)));
        KeStallExecutionProcessor(10);
        T214Write(c, 0x5a4, (104u << 8) | 4u);
        T214Write(c, 0x5a4, (1u << 30) | (104u << 8) | 4u);
    }
    for (ULONG i = 0; i < 1000; i += 5) {
        base = T214Read(c, 0x5a4);
        if (base & (1u << 27)) break;
        KeStallExecutionProcessor(5);
    }
    if ((base & ((1u << 30) | (1u << 27))) != ((1u << 30) | (1u << 27)) ||
        (base & 0x1fffffu) != ((104u << 8) | 4u)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    T214Write(c, 0x150, (T214Read(c, 0x150) & ~0xe00000ffu) | (3u << 29));
    (VOID)T214Read(c, 0x150);
    s->Capabilities.BaseClockFrequencyKhz = 199680;
    return STATUS_SUCCESS;
}

static NTSTATUS T214Tune(PSDHC_EXTENSION s)
{
    if (!s->T214Voltage18 || s->SpeedMode != SdhcSpeedModeSDR104) return STATUS_INVALID_DEVICE_STATE;
    PUCHAR b = s->BaseAddress;
    ULONG signals = T214Read(b, 0x38), enables = T214Read(b, 0x34);
    USHORT clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL), hc;
    NTSTATUS st = STATUS_IO_DEVICE_ERROR;
    s->T214Tuned = FALSE; s->T214TuningRuns++;
    T214Write(b, 0x38, 0); /* Poll tuning status; no SDPORT request owns it. */
    T214Write(b, 0x34, 0x037f8023);
    T214Write(b, 0x1c4, 0);
    T214Write(b, 0x1c0, (T214Read(b, 0x1c0) & ~0xffc0u) | (2u << 13) | (1u << 6) | (1u << 17));
    hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2);
    hc = (hc & ~SDHC_HC2_SELECT_SAMPLING_CLOCK) | SDHC_HC2_EXECUTE_TUNING;
    SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2, hc);
    for (ULONG i = 0; i < 128; ++i) {
        s->T214TuningIterations = i + 1;
        if (!T214WaitClear(b, SDHC_PRESENT_STATE, 3, 10000)) break;
        T214Write(b, 0x30, 0xffffffff);
        SdhcWriteRegisterUshort(s, SDHC_BLOCK_SIZE, 64);
        SdhcWriteRegisterUshort(s, SDHC_BLOCK_COUNT, 1);
        SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE, SDHC_TM_TRANSFER_READ);
        SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~SDHC_CC_CLOCK_ENABLE);
        SdhcWriteRegisterUlong(s, SDHC_ARGUMENT, 0);
        SdhcWriteRegisterUshort(s, SDHC_COMMAND, (19u << 8) | 0x3a);
        KeStallExecutionProcessor(1);
        /* Tegra NON_STD_TUN_CARD_CLOCK workaround, also used by Switchroot. */
        SdhcWriteRegisterUchar(s, SDHC_RESET, SDHC_RESET_CMD | SDHC_RESET_DAT);
        ULONG tries;
        for (tries = 0; tries < 1000; tries += 5) {
            if (!(SdhcReadRegisterUchar(s, SDHC_RESET) & 6)) break;
            KeStallExecutionProcessor(5);
        }
        if (tries == 1000) break;
        SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock | SDHC_CC_CLOCK_ENABLE);
        BOOLEAN ready = FALSE;
        for (ULONG wait = 0; wait < 5000; wait += 5) {
            USHORT events = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS);
            if (events & SDHC_IS_BUFFER_READ_READY) { ready = TRUE; break; }
            if (events & SDHC_IS_ERROR_INTERRUPT) break;
            KeStallExecutionProcessor(5);
        }
        SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS, SDHC_IS_BUFFER_READ_READY);
        KeStallExecutionProcessor(1);
        hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2);
        if (!(hc & SDHC_HC2_EXECUTE_TUNING)) {
            if (ready && (hc & SDHC_HC2_SELECT_SAMPLING_CLOCK)) st = STATUS_SUCCESS;
            break;
        }
    }
    if (!NT_SUCCESS(st)) {
        hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2) & ~(SDHC_HC2_EXECUTE_TUNING | SDHC_HC2_SELECT_SAMPLING_CLOCK);
        SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2, hc);
        SdhcWriteRegisterUchar(s, SDHC_RESET, SDHC_RESET_CMD | SDHC_RESET_DAT);
        for (ULONG i = 0; i < 1000 && (SdhcReadRegisterUchar(s, SDHC_RESET) & 6); i += 5) KeStallExecutionProcessor(5);
    }
    s->T214Tuned = NT_SUCCESS(st);
    s->T214TuneStatus = st;
    s->T214Tap = (T214Read(b, 0x100) >> 16) & 255;
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock);
    T214Write(b, 0x30, 0xffffffff);
    T214Write(b, 0x34, enables); T214Write(b, 0x38, signals);
    return st;
}

/* Private noncached low-memory ADMA buffer. SDPORT keeps its PIO buffer
 * contract, so there is no second cache invalidation after the MBR filter.
 * The device DMA addresses never refer to cached OS or user memory.
 */
static BOOLEAN T214BuildDma(PSDHC_EXTENSION s, PSDPORT_REQUEST r)
{
    PSDPORT_COMMAND c = &r->Command;
    s->T214DmaActive = FALSE; s->T214DmaComplete = FALSE;
    if (c->Index != 17 && c->Index != 18 && c->Index != 24 && c->Index != 25) return FALSE;
    ULONG size = c->BlockCount * 512u;
    if (s->T214DmaFault || s->T214Bounce == NULL || c->DataBuffer == NULL ||
        c->TransferMethod != SdTransferMethodPio || c->BlockSize != 512 ||
        !c->BlockCount || c->BlockCount > s->T214DmaCapacity / 512 || c->Length < size) {
        s->T214PioFallbacks++; return FALSE;
    }
    s->T214DmaRequests++;
    s->T214DmaMaxBlocks = MAX(s->T214DmaMaxBlocks, c->BlockCount);
    if (c->TransferDirection == SdTransferDirectionWrite) RtlCopyMemory(s->T214Bounce, c->DataBuffer, size);
    PULONG table = (PULONG)(s->T214Bounce + s->T214DmaCapacity);
    ULONG done = 0, i = 0;
    while (done < size) {
        ULONG n = min(size - done, 32768u);
        table[i++] = (n << 16) | 0x21u | ((done + n == size) ? 2u : 0);
        table[i++] = s->T214BouncePa.LowPart + done;
        done += n;
    }
    KeMemoryBarrier();
    UCHAR hc = SdhcReadRegisterUchar(s, SDHC_HOST_CONTROL);
    SdhcWriteRegisterUchar(s, SDHC_HOST_CONTROL, (hc & ~SDHC_HC_DMA_SELECT_MASK) | SDHC_HC_DMA_SELECT_ADMA32);
    SdhcWriteRegisterUlong(s, SDHC_ADMA_SYSADDR_LOW, s->T214BouncePa.LowPart + s->T214DmaCapacity);
    SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE, SdhcReadRegisterUshort(s, SDHC_TRANSFER_MODE) | SDHC_TM_DMA_ENABLE);
    s->T214DmaBuffer = c->DataBuffer; s->T214DmaLength = size;
    s->T214DmaActive = TRUE;
    return TRUE;
}
static VOID T214DmaDone(PSDHC_EXTENSION s, PSDPORT_REQUEST r)
{
    KeMemoryBarrier();
    if (r->Command.TransferDirection == SdTransferDirectionRead) {
        RtlCopyMemory(s->T214DmaBuffer, s->T214Bounce, s->T214DmaLength);
#if NV_T210_LITE_WINARM_WRITE
        SDPORT_COMMAND block = r->Command;
        block.DataBuffer = s->T214DmaBuffer;
        LiteObserveReadRange(s, &block, s->T214DmaLength / 512);
#endif
        NvDiag.BlocksRead += s->T214DmaLength / 512;
    } else NvDiag.BlocksWritten += s->T214DmaLength / 512;
    s->T214DmaBlocks += s->T214DmaLength / 512;
    s->T214DmaComplete = TRUE; s->T214DmaActive = FALSE;
}
static VOID T214Release(PSDHC_EXTENSION s)
{
    if (s->T214Bounce) { MmFreeContiguousMemory(s->T214Bounce); s->T214Bounce = NULL; }
    if (s->T214GateHandle) { ZwClose(s->T214GateHandle); s->T214GateHandle = NULL; s->T214Gate = NULL; }
    if (s->T214Car) { MmUnmapIoSpace(s->T214Car, 0x1000); s->T214Car = NULL; }
    if (s->T214Misc) { MmUnmapIoSpace(s->T214Misc, 0x4000); s->T214Misc = NULL; }
    if (s->T214Pmc) { MmUnmapIoSpace(s->T214Pmc, 0x1000); s->T214Pmc = NULL; }
    if (s->T214I2c) { MmUnmapIoSpace(s->T214I2c, 0x1000); s->T214I2c = NULL; }
    if (s->T214Gpio) { MmUnmapIoSpace(s->T214Gpio, 0x1000); s->T214Gpio = NULL; }
}

/* SDPORT calls these only with the slot quiesced. Do not replay the command,
 * data, interrupt-status or DMA-address registers: they describe old I/O.
 * This supports controller context loss with card power retained. A real
 * card power loss still requires SDPORT's normal card reinitialization. */
static const ULONG T214ContextOffsets[] = {0x100, 0x1ac, 0x1c0, 0x1c4, 0x1f0};
static VOID T214SaveContext(PSDHC_EXTENSION s)
{
    s->T214ContextValid = FALSE;
    if (s->T214DmaActive || s->OutstandingRequests[0] ||
        (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) & 3)) {
        s->T214ResumeStatus = STATUS_DEVICE_BUSY;
        return;
    }
    for (ULONG i=0; i<RTL_NUMBER_OF(T214ContextOffsets); ++i)
        s->T214SavedVendor[i] = T214Read(s->BaseAddress, T214ContextOffsets[i]);
    s->T214SavedEnables = SdhcReadRegisterUlong(s, 0x34);
    s->T214SavedSignals = SdhcReadRegisterUlong(s, 0x38);
    s->T214SavedClock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL) & ~SDHC_CC_CLOCK_STABLE;
    s->T214SavedControl2 = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2) & ~SDHC_HC2_EXECUTE_TUNING;
    s->T214SavedHost = SdhcReadRegisterUchar(s, SDHC_HOST_CONTROL);
    s->T214SavedPower = SdhcReadRegisterUchar(s, SDHC_POWER_CONTROL);
    s->T214SavedTimeout = SdhcReadRegisterUchar(s, SDHC_TIMEOUT_CONTROL);
    s->T214SavedSource = T214Read(s->T214Car, 0x150);
    s->T214SavedBaseClock = s->Capabilities.BaseClockFrequencyKhz;
    s->T214ContextValid = TRUE;
    s->T214Saves++;
}

static NTSTATUS T214ResumeHostClock(PSDHC_EXTENSION s)
{
    const ULONG sdmmc1 = 1u << 14;
    PUCHAR car = s->T214Car;
    BOOLEAN ready = (T214Read(car, 0x10) & sdmmc1) &&
                    !(T214Read(car, 0x04) & sdmmc1);
    if (ready) {
        /* A retained host may still clock the card. Gate it before changing
         * the source. Do not access SDHCI at all while CAR holds it in reset. */
        SdhcWriteRegisterUlong(s, 0x38, 0);
        SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, 0);
    } else {
        T214Write(car, 0x300, sdmmc1); /* RST_DEV_L_SET */
    }
    NTSTATUS status = T214ParentClock(s);
    if (!NT_SUCCESS(status)) return status;
    /* DDR200 uses PLLC4 OUT0 and an SDHCI /2 divider. Restore that exact
     * source before the first SDCLK pulse, not through an SDR104 interval. */
    T214Write(car, 0x150, s->T214SavedSource);
    if (T214Read(car, 0x150) != s->T214SavedSource)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!ready) {
        T214Write(car, 0x320, sdmmc1); /* CLK_ENB_L_SET */
        KeStallExecutionProcessor(2);
        if (!(T214Read(car, 0x10) & sdmmc1)) return STATUS_DEVICE_POWER_FAILURE;
        T214Write(car, 0x304, sdmmc1); /* RST_DEV_L_CLR */
        KeStallExecutionProcessor(2);
        if (T214Read(car, 0x04) & sdmmc1) return STATUS_DEVICE_POWER_FAILURE;
    }
    s->Capabilities.BaseClockFrequencyKhz = s->T214SavedBaseClock;
    return STATUS_SUCCESS;
}

static VOID T214RestoreContext(PSDHC_EXTENSION s)
{
    if (!s->T214ContextValid) return;
    NTSTATUS status = T214ResumeHostClock(s);
    if (!NT_SUCCESS(status)) goto failed;
    SdhcWriteRegisterUlong(s, 0x38, 0);
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, 0);
    for (ULONG i=0; i<RTL_NUMBER_OF(T214ContextOffsets); ++i)
        T214Write(s->BaseAddress, T214ContextOffsets[i], s->T214SavedVendor[i]);
    SdhcWriteRegisterUchar(s, SDHC_HOST_CONTROL, s->T214SavedHost);
    SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2, s->T214SavedControl2);
    SdhcWriteRegisterUchar(s, SDHC_POWER_CONTROL, s->T214SavedPower);
    SdhcWriteRegisterUchar(s, SDHC_TIMEOUT_CONTROL, s->T214SavedTimeout);
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, s->T214SavedClock & ~SDHC_CC_CLOCK_ENABLE);
    if (s->T214SavedClock & SDHC_CC_INTERNAL_CLOCK_ENABLE) {
        ULONG i;
        for (i=0; i<10000; i+=10) {
            if (SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL) & SDHC_CC_CLOCK_STABLE) break;
            KeStallExecutionProcessor(10);
        }
        if (i==10000) {status=STATUS_IO_TIMEOUT;goto failed;}
    }
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, s->T214SavedClock);
    SdhcWriteRegisterUlong(s, 0x34, s->T214SavedEnables);
    SdhcWriteRegisterUlong(s, 0x38, s->T214SavedSignals);
    s->T214Restores++;
    s->T214ResumeStatus = STATUS_SUCCESS;
    return;
failed:
    s->T214ResumeStatus = status;
    s->T214DmaFault = TRUE;
#if NV_T214_DDR200
    if (s->T214DdrActive) {
        s->T214DdrRecoverNeeded = TRUE;
        s->T214DdrFatal = TRUE;
    }
#endif
}
static NTSTATUS T214Initialize(PSDHC_EXTENSION s)
{
    NTSTATUS st = STATUS_INSUFFICIENT_RESOURCES;
    s->T214Car = T214Map(0x60006000, 0x1000);
    s->T214Misc = T214Map(0x70000000, 0x4000);
    s->T214Pmc = T214Map(0x7000e000, 0x1000);
    s->T214I2c = T214Map(0x7000d000, 0x1000);
    s->T214Gpio = T214Map(0x6000d000, 0x1000);
    s->T214Gate = TegraI2c5OpenGate(&s->T214GateHandle);
    if (!s->T214Car || !s->T214Misc || !s->T214Pmc || !s->T214I2c || !s->T214Gpio || !s->T214Gate) goto fail;
    if ((T214Read(s->T214Misc, 0x804) & 0xfff0u) != 0x2120u) { st = STATUS_NOT_SUPPORTED; goto fail; }
    USHORT clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~SDHC_CC_CLOCK_ENABLE);
    st = T214ParentClock(s);
    if (!NT_SUCCESS(st)) goto fail;
    /* SDMMC1A uses physical DRAM only when its SMMU translation is off. */
    PUCHAR mc = T214Map(0x70019000, 0x1000);
    if (mc) {
        s->T214Smmu = T214Read(mc, 0xa94);
        MmUnmapIoSpace(mc, 0x1000);
        if (!(s->T214Smmu & (1u << 31))) {
            PHYSICAL_ADDRESS low, high, boundary;
            low.QuadPart = 0x80000000; high.QuadPart = 0xffffffffu; boundary.QuadPart = 0;
            /* Switchroot batches multi-MiB requests. Start with 512 KiB on
             * Windows, retaining the proven 64 KiB allocation fallback. */
            s->T214DmaCapacity = T214_DMA_BYTES;
            s->T214Bounce = MmAllocateContiguousMemorySpecifyCache(T214_DMA_ALLOC, low, high, boundary, MmNonCached);
            if (!s->T214Bounce) {
                s->T214DmaCapacity = 65536;
                s->T214Bounce = MmAllocateContiguousMemorySpecifyCache(
                    s->T214DmaCapacity + PAGE_SIZE, low, high, boundary, MmNonCached);
            }
            if (s->T214Bounce) s->T214BouncePa = MmGetPhysicalAddress(s->T214Bounce);
            else s->T214DmaCapacity = 0;
        }
    }
    s->Capabilities.MaximumBlockCount = (USHORT)(s->T214Bounce ? s->T214DmaCapacity / 512 : 128);
    s->Capabilities.MaximumBlockSize = 512;
    s->T214CardOn = !!(SdhcReadRegisterUchar(s, SDHC_POWER_CONTROL) & SDHC_PC_BUS_POWER);
    s->T214Voltage18 = !!(SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2) & SDHC_HC2_1_8V_SIGNALING);
    return STATUS_SUCCESS;
fail:
    T214Release(s); return st;
}
