/* SPDX-License-Identifier: GPL-2.0-only
 * T214 DDR200 transport. Included after t214_sdmmc.h by the opt-in build.
 * Register/tuning sequence: Switchroot kernel 2d0059fd3167a8df756de2aa0489d4aa70a9fc15
 * and Hekate v6.5.3. SDPORT must have quiesced the slot before these helpers run.
 */
#include "t214_ddr200_policy.h"

static BOOLEAN T214DdrResetLines(PSDHC_EXTENSION s)
{
    SdhcWriteRegisterUchar(s, SDHC_RESET, SDHC_RESET_CMD | SDHC_RESET_DAT);
    for (ULONG i = 0; i < 1000; i += 5) {
        if (!(SdhcReadRegisterUchar(s, SDHC_RESET) & 6)) return TRUE;
        KeStallExecutionProcessor(5);
    }
    return FALSE;
}

/* Only command/configuration writes and read-only data transfers. No storage
 * write can bypass LiteAllowRequest through this polling helper.
 */
static NTSTATUS T214DdrCommand(PSDHC_EXTENSION s, ULONG Index, ULONG Argument,
                              USHORT Flags, PULONG Buffer, ULONG Bytes,
                              PULONG Response)
{
    switch (Index) {
    case 0: case 2: case 3: case 6: case 7: case 8: case 11: case 13:
    case 17: case 41: case 51: case 55: break;
    default: return STATUS_INVALID_PARAMETER;
    }
    if (Bytes > 512 || (Bytes & 3) || (Bytes && Buffer == NULL) ||
        (Bytes && Index != 6 && Index != 17 && Index != 51) ||
        (Index == 17 && Bytes != 512) || (Index == 51 && Bytes != 8) ||
        (Index == 6 && Bytes != 0 && Bytes != 64))
        return STATUS_INVALID_PARAMETER;
    if (s->T214DmaActive || s->OutstandingRequests[0]) return STATUS_DEVICE_BUSY;
    PUCHAR b = s->BaseAddress;
    ULONG signals = T214Read(b, 0x38), enables = T214Read(b, 0x34);
    NTSTATUS st = STATUS_IO_TIMEOUT;
    BOOLEAN commandDone = FALSE, dataDone = Bytes == 0;
    T214Write(b, 0x38, 0);
    T214Write(b, 0x34, 0x037f8033);
    if (!T214WaitClear(b, SDHC_PRESENT_STATE, 3, 10000)) goto done;
    T214Write(b, 0x30, 0xffffffffu);
    SdhcWriteRegisterUchar(s, SDHC_TIMEOUT_CONTROL, 14);
    SdhcWriteRegisterUshort(s, SDHC_BLOCK_SIZE, (USHORT)Bytes);
    SdhcWriteRegisterUshort(s, SDHC_BLOCK_COUNT, Bytes ? 1 : 0);
    SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE, Bytes ? SDHC_TM_TRANSFER_READ : 0);
    SdhcWriteRegisterUlong(s, SDHC_ARGUMENT, Argument);
    SdhcWriteRegisterUshort(s, SDHC_COMMAND,
                           (USHORT)((Index << 8) | Flags | (Bytes ? 0x20 : 0)));
    for (ULONG waited = 0; waited < 50000; waited += 5) {
        ULONG events = T214Read(b, 0x30);
        if (events & SDHC_IS_ERROR_INTERRUPT) {
            st = SdhcConvertErrorToStatus((USHORT)(events >> 16));
            break;
        }
        if (!commandDone && (events & SDHC_IS_CMD_COMPLETE)) {
            commandDone = TRUE;
            if (Response) {
                for (ULONG i = 0; i < ((Flags & 3) == 1 ? 4u : 1u); ++i)
                    Response[i] = T214Read(b, SDHC_RESPONSE + i * 4);
            }
            ULONG r = T214Read(b, SDHC_RESPONSE);
            if ((Index == 3 && (r & 0xe000u)) ||
                (Index != 0 && Index != 2 && Index != 3 && Index != 8 &&
                 Index != 41 && (r & 0xfdffe008u))) {
                st = STATUS_DEVICE_PROTOCOL_ERROR;
                break;
            }
        }
        if (Bytes && !dataDone && (events & SDHC_IS_BUFFER_READ_READY)) {
            for (ULONG i = 0; i < Bytes / 4; ++i)
                Buffer[i] = T214Read(b, SDHC_DATA_PORT);
            dataDone = TRUE;
            SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS, SDHC_IS_BUFFER_READ_READY);
        }
        if (commandDone && dataDone &&
            (!(Bytes || (Flags & 3) == 3) || (events & SDHC_IS_TRANSFER_COMPLETE))) {
            st = STATUS_SUCCESS;
            break;
        }
        KeStallExecutionProcessor(5);
    }
done:
    if (!NT_SUCCESS(st)) (VOID)T214DdrResetLines(s);
    T214Write(b, 0x30, 0xffffffffu);
    T214Write(b, 0x34, enables);
    T214Write(b, 0x38, signals);
    return st;
}

static NTSTATUS T214DdrClock(PSDHC_EXTENSION s, ULONG Frequency)
{
    USHORT clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);
    if (Frequency == 0) {
        SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~5u);
        NvDiag.ClockKhzRequested = NvDiag.ClockKhzActual = 0;
        NvDiag.RegClockControl = clock & ~5u;
        return STATUS_SUCCESS;
    }
    /* SDPORT may request SDR104's nominal 208 MHz. The B01 board limit is
     * 200 MHz, just as in the base SDR104 clock setter and Switchroot DT.
     */
    if (Frequency > 200000) Frequency = 200000;
    ULONG divider = T214Ddr200CarDivider(Frequency);
    if (divider == MAXULONG) return STATUS_NOT_SUPPORTED;
    ULONG pll = T214Read(s->T214Car, 0x5a4);
    /* OUT0 must be the undivided, locked 998.4-MHz VCO. Never retune a
     * shared PLL to make this extension work.
     */
    if ((pll & 0x48000000u) != 0x48000000u ||
        (pll & 0x00ffffffu) != ((104u << 8) | 4u))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, 0);
    ULONG source = (T214Read(s->T214Car, 0x150) & ~0xe00000ffu) |
                   (7u << 29) | divider;
    T214Write(s->T214Car, 0x150, source);
    if (T214Read(s->T214Car, 0x150) != source) return STATUS_DEVICE_CONFIGURATION_ERROR;
    s->Capabilities.BaseClockFrequencyKhz = 1996800u / (divider + 2);
    /* Keep the SDHCI DDR divisor at two; low rates change CAR instead. */
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, 0x101);
    for (ULONG i = 0; i < 20000; i += 5) {
        clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);
        if (clock & SDHC_CC_CLOCK_STABLE) {
            SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock | SDHC_CC_CLOCK_ENABLE);
            NvDiag.ClockKhzRequested = Frequency;
            NvDiag.ClockKhzActual = s->Capabilities.BaseClockFrequencyKhz / 2;
            NvDiag.CarSdmmc1After = source;
            NvDiag.RegClockControl = clock | SDHC_CC_CLOCK_ENABLE;
            s->T214DdrClockKhz = Frequency;
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(5);
    }
    return STATUS_IO_TIMEOUT;
}

/* Card clock is gated by the caller while a manual tap is selected. */
static VOID T214DdrSetTap(PSDHC_EXTENSION s, ULONG Tap)
{
    PUCHAR b = s->BaseAddress;
    T214Write(b, 0x1c0, T214Read(b, 0x1c0) & ~(1u << 17));
    T214Write(b, 0x100, (T214Read(b, 0x100) & ~0xff0000u) | (Tap << 16));
    T214Write(b, 0x1c0, T214Read(b, 0x1c0) | (1u << 17));
}

static NTSTATUS T214DdrManualTuning(PSDHC_EXTENSION s, PULONG Selected,
                                   PULONG Window)
{
    if (!s->T214Voltage18 ||
        (SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2) & 7) != 4)
        return STATUS_INVALID_DEVICE_STATE;
    PUCHAR b = s->BaseAddress;
    unsigned int results[8] = {0}, selected = 0, width = 0;
    ULONG tested = 0, timeouts = 0;
    ULONG signals = T214Read(b, 0x38), enables = T214Read(b, 0x34);
    USHORT clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL), hc;
    NTSTATUS st = STATUS_IO_DEVICE_ERROR;
    *Selected = *Window = 0;
    s->T214Tuned = FALSE;
    T214Write(b, 0x38, 0);
    T214Write(b, 0x34, 0x037f8023);
    SdhcWriteRegisterUchar(s, SDHC_TIMEOUT_CONTROL, 14);
    T214Write(b, 0x1c4, 0);
    T214Write(b, 0x1c0, (T214Read(b, 0x1c0) & ~0xffc0u) |
               (2u << 13) | (1u << 6) | (1u << 17));
    hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2);
    SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2,
                           (hc & ~SDHC_HC2_SELECT_SAMPLING_CLOCK) | SDHC_HC2_EXECUTE_TUNING);
    for (ULONG tap = 0; tap < T214_DDR200_TAPS; ++tap) {
        if (!T214WaitClear(b, SDHC_PRESENT_STATE, 3, 10000)) goto done;
        T214Write(b, 0x30, 0xffffffffu);
        SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~SDHC_CC_CLOCK_ENABLE);
        T214DdrSetTap(s, tap);
        SdhcWriteRegisterUshort(s, SDHC_BLOCK_SIZE, 64);
        SdhcWriteRegisterUshort(s, SDHC_BLOCK_COUNT, 1);
        SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE, SDHC_TM_TRANSFER_READ);
        SdhcWriteRegisterUlong(s, SDHC_ARGUMENT, 0);
        SdhcWriteRegisterUshort(s, SDHC_COMMAND, (19u << 8) | 0x3a);
        KeStallExecutionProcessor(1);
        if (!T214DdrResetLines(s)) goto done;
        SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock | SDHC_CC_CLOCK_ENABLE);
        BOOLEAN ready = FALSE;
        for (ULONG wait = 0; wait < 5000; wait += 5) {
            if (SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS) & SDHC_IS_BUFFER_READ_READY) {
                ready = TRUE;
                break;
            }
            KeStallExecutionProcessor(5);
        }
        SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS, SDHC_IS_BUFFER_READ_READY);
        KeStallExecutionProcessor(1);
        hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2);
        if (ready && (hc & SDHC_HC2_SELECT_SAMPLING_CLOCK))
            results[tap / 32] |= 1u << (tap % 32);
        tested = tap + 1;
        if (!ready) {
            if (++timeouts >= T214_DDR200_TAPS / 32 || !T214DdrResetLines(s)) goto done;
        }
        if (!(hc & SDHC_HC2_EXECUTE_TUNING)) break;
    }
    BOOLEAN usable = (BOOLEAN)T214Ddr200Window(results, tested, &selected, &width);
    *Selected = selected;
    *Window = width;
    if (usable) {
        SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~SDHC_CC_CLOCK_ENABLE);
        T214DdrSetTap(s, *Selected);
        s->T214Tap = *Selected;
        s->T214Tuned = TRUE;
        st = STATUS_SUCCESS;
    }
done:
    hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2) & ~SDHC_HC2_EXECUTE_TUNING;
    if (NT_SUCCESS(st)) hc |= SDHC_HC2_SELECT_SAMPLING_CLOCK;
    else hc &= ~SDHC_HC2_SELECT_SAMPLING_CLOCK;
    SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2, hc);
    if (!NT_SUCCESS(st)) (VOID)T214DdrResetLines(s);
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock);
    T214Write(b, 0x30, 0xffffffffu);
    T214Write(b, 0x34, enables);
    T214Write(b, 0x38, signals);
    return st;
}

static VOID T214DdrHostMode(PSDHC_EXTENSION s, USHORT Mode)
{
    USHORT clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~SDHC_CC_CLOCK_ENABLE);
    USHORT hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2);
    hc = (hc & ~(7u | SDHC_HC2_EXECUTE_TUNING | SDHC_HC2_SELECT_SAMPLING_CLOCK)) | Mode;
    SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2, hc);
    /* DDR mode only permits its fixed SDHCI divider. Keep the card clock
     * gated until T214DdrClock installs that divider and its CAR source.
     */
    if (Mode != 4) SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock);
}

static NTSTATUS T214DdrSwitch(PSDHC_EXTENSION s, ULONG Argument, UCHAR Selection)
{
    ULONG words[16];
    NTSTATUS st = T214DdrCommand(s, 6, Argument, 0x1a, words, sizeof(words), NULL);
    if (NT_SUCCESS(st) && ((PUCHAR)words)[16] != Selection)
        st = STATUS_DEVICE_PROTOCOL_ERROR;
    return st;
}

static NTSTATUS T214DdrApp(PSDHC_EXTENSION s, ULONG Rca, ULONG Index,
                          ULONG Argument, PULONG Response)
{
    ULONG r;
    NTSTATUS st = T214DdrCommand(s, 55, Rca, 0x1a, NULL, 0, &r);
    if (!NT_SUCCESS(st)) return st;
    if (!(r & 0x20u)) return STATUS_DEVICE_PROTOCOL_ERROR;
    return T214DdrCommand(s, Index, Argument, Index == 41 ? 2 : 0x1a,
                         NULL, 0, Response);
}

/* Reinitialize at SDR104, as Switchroot does after failed DDR200 tuning.
 * SDPORT owns the RCA: it must still identify the same card afterwards.
 * Never invent or translate an RCA, and never return success with a mismatch.
 */
static NTSTATUS T214DdrRecover(PSDHC_EXTENSION s)
{
    if (s->T214DmaActive || s->OutstandingRequests[0]) return STATUS_DEVICE_BUSY;
    PUCHAR b = s->BaseAddress;
    ULONG signals = T214Read(b, 0x38), enables = T214Read(b, 0x34);
    ULONG cid[4], expectedCid[4], rca = s->T214Rca, response;
    ULONG restoreSwitch = 0x80ff0003u | ((ULONG)s->T214SwitchStatus[15] << 8);
    /* The private reset-all clears both SDHCI event masks. SDPORT is not
     * performing this reset and will not necessarily re-arm them afterwards.
     * Keep polling responses private, then return its exact masks on every
     * exit so ordinary commands (or native recovery) can deliver interrupts.
     */
    T214Write(b, 0x38, 0);
    RtlCopyMemory(expectedCid, s->T214Cid, sizeof(expectedCid));
    s->T214DdrFallbacks++;
    s->T214DdrRecoverNeeded = FALSE; /* One recovery per failed transaction. */
    s->T214DdrFatal = TRUE;
    NTSTATUS st = T214Power(s, TRUE);
    if (!NT_SUCCESS(st)) goto done;
    st = SdhcResetHost(s, SdResetTypeAll);
    if (!NT_SUCCESS(st)) goto done;
    st = T214SetBusVoltage(s, SdBusVoltage33);
    if (!NT_SUCCESS(st)) goto done;
    st = SdhcSetBusWidth(s, SdBusWidth1Bit);
    if (!NT_SUCCESS(st)) goto done;
    st = SdhcSetClock(s, 400);
    if (!NT_SUCCESS(st)) goto done;
    st = T214DdrCommand(s, 0, 0, 0, NULL, 0, NULL);
    if (!NT_SUCCESS(st)) goto done;
    SdPortWait(1000);
    st = T214DdrCommand(s, 8, 0x1aa, 0x1a, NULL, 0, &response);
    if (!NT_SUCCESS(st)) goto done;
    if ((response & 0xfffu) != 0x1aau) { st = STATUS_DEVICE_PROTOCOL_ERROR; goto done; }
    for (ULONG i = 0; i < 200; ++i) {
        st = T214DdrApp(s, 0, 41, 0x51ff8000u, &response);
        if (!NT_SUCCESS(st)) goto done;
        if (response & 0x80000000u) break;
        SdPortWait(5000);
    }
    if ((response & 0xc1000000u) != 0xc1000000u) { st = STATUS_IO_TIMEOUT; goto done; }
    st = T214DdrCommand(s, 11, 0, 0x1a, NULL, 0, NULL);
    if (!NT_SUCCESS(st)) goto done;
    st = T214Signaling(s, TRUE);
    if (!NT_SUCCESS(st)) goto done;
    st = T214DdrCommand(s, 2, 0, 9, NULL, 0, cid);
    if (!NT_SUCCESS(st)) goto done;
    if (!LiteBytesEqual((PUCHAR)cid, (PUCHAR)expectedCid, sizeof(cid))) {
        st = STATUS_WRONG_VOLUME; goto done;
    }
    st = T214DdrCommand(s, 3, 0, 0x1a, NULL, 0, &response);
    if (!NT_SUCCESS(st)) goto done;
    if (rca == 0 || (response & 0xffff0000u) != rca) {
        st = STATUS_DEVICE_PROTOCOL_ERROR; goto done;
    }
    st = T214DdrCommand(s, 7, rca, 0x1b, NULL, 0, NULL);
    if (!NT_SUCCESS(st)) goto done;
    st = T214DdrApp(s, rca, 6, 2, NULL);
    if (!NT_SUCCESS(st)) goto done;
    st = SdhcSetBusWidth(s, SdBusWidth4Bit);
    if (!NT_SUCCESS(st)) goto done;
    st = SdhcSetClock(s, 25000);
    if (!NT_SUCCESS(st)) goto done;
    st = T214DdrSwitch(s, restoreSwitch, 3);
    if (!NT_SUCCESS(st)) goto done;
    s->SpeedMode = SdhcSpeedModeSDR104;
    T214DdrHostMode(s, 3);
    st = SdhcSetClock(s, 200000);
    if (!NT_SUCCESS(st)) goto done;
    st = T214Tune(s);
    if (NT_SUCCESS(st)) {
        ULONG sector[128];
        st = T214DdrCommand(s, 17, 0, 0x1a, sector, sizeof(sector), NULL);
        if (NT_SUCCESS(st) && !LiteSelectWindowsPartition((PUCHAR)sector)) st = STATUS_WRONG_VOLUME;
        if (NT_SUCCESS(st)) st = T214DdrCommand(s, 17, WINARM_FIRST, 0x1a, sector, sizeof(sector), NULL);
        if (NT_SUCCESS(st) && !LiteIsBootSector((PUCHAR)sector)) st = STATUS_WRONG_VOLUME;
#ifdef FAT32_FIRST
        if (NT_SUCCESS(st)) st = T214DdrCommand(s, 17, FAT32_FIRST, 0x1a, sector, sizeof(sector), NULL);
        if (NT_SUCCESS(st) && !LiteIsFatBootSector((PUCHAR)sector)) st = STATUS_WRONG_VOLUME;
#endif
        if (NT_SUCCESS(st)) {
            InterlockedExchange(&s->TargetFacts, LiteWriteFacts
#ifdef FAT32_FIRST
                                | LiteFatBoot
#endif
                                );
            s->T214Rca = rca;
            RtlCopyMemory(s->T214Cid, expectedCid, sizeof(expectedCid));
            s->T214CardInfoValid = 4; /* Remaining metadata is recaptured, not fabricated. */
            s->T214DdrFatal = FALSE;
            s->T214DdrRecoverNeeded = FALSE;
        }
    }
done:
    s->T214DdrRecoveryStatus = st;
    T214Write(b, 0x30, 0xffffffffu);
    T214Write(b, 0x34, enables);
    T214Write(b, 0x38, signals);
    return st;
}

/* SDPORT still requests its standard SDR104 service. The private vendor
 * mode is only attempted at this quiesced tuning boundary, never in a DPC.
 */
static NTSTATUS T214DdrTry(PSDHC_EXTENSION s)
{
    NTSTATUS st;
    ULONG baseline[128], observed[128];
    if (s->T214DmaActive || s->OutstandingRequests[0]) return STATUS_DEVICE_BUSY;
    if (s->T214DdrActive) {
        st = T214DdrManualTuning(s, &s->T214DdrTap, &s->T214DdrWindow);
        s->T214DdrStatus = st;
        return NT_SUCCESS(st) ? st : T214DdrRecover(s);
    }
    st = T214Tune(s);
    if (NT_SUCCESS(st) && s->T214DdrFatal &&
        (s->T214CardInfoValid & 4) && s->T214Rca) {
        /* After failed private recovery, SDPORT may perform its own full
         * enumeration and learn a changed RCA. Its fresh CID/RCA and tuning
         * permit normal SDR104 reads again; volume write guards still apply.
         */
        s->T214DdrFatal = FALSE;
        s->T214DdrRecoverNeeded = FALSE;
        s->T214DdrRecoveryStatus = STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(st) || s->T214DdrAttempted) return st;
    s->T214DdrStatus = STATUS_NOT_SUPPORTED;
    if ((s->T214CardInfoValid & 7) != 7 || s->T214Rca == 0) return STATUS_SUCCESS;
    s->T214DdrAttempted = TRUE;
    if (!T214Ddr200Supported(s->T214Scr, s->T214SwitchStatus) ||
        s->T214SwitchStatus[16] != 3 || (s->T214SwitchStatus[15] & 15) != 0)
        return STATUS_SUCCESS;
    st = T214DdrCommand(s, 17, WINARM_FIRST, 0x1a, baseline, sizeof(baseline), NULL);
    if (!NT_SUCCESS(st)) return st;
    if (!LiteIsBootSector((PUCHAR)baseline)) return STATUS_WRONG_VOLUME;
    st = SdhcSetClock(s, 25000);
    if (!NT_SUCCESS(st)) return st;
    st = T214DdrSwitch(s, 0x80fffff0u, 0);
    if (!NT_SUCCESS(st)) goto recover;
    T214DdrHostMode(s, 0);
    /* Mark possible vendor state before issuing CMD6: a lost reply cannot
     * establish whether the card changed mode. Recovery must power-cycle it.
     */
    s->T214DdrActive = TRUE;
    st = T214DdrSwitch(s, 0x80ffffefu, 0xe0);
    if (!NT_SUCCESS(st)) goto recover;
    T214DdrHostMode(s, 4);
    st = T214DdrClock(s, 200000);
    if (!NT_SUCCESS(st)) goto recover;
    st = T214DdrManualTuning(s, &s->T214DdrTap, &s->T214DdrWindow);
    if (!NT_SUCCESS(st)) goto recover;
    for (ULONG i = 0; i < 4; ++i) {
        st = T214DdrCommand(s, 17, WINARM_FIRST, 0x1a, observed, sizeof(observed), NULL);
        if (!NT_SUCCESS(st)) goto recover;
        if (!LiteBytesEqual((PUCHAR)baseline, (PUCHAR)observed, sizeof(baseline))) {
            st = STATUS_DATA_ERROR; goto recover;
        }
    }
    s->T214DdrStatus = STATUS_SUCCESS;
    return STATUS_SUCCESS;
recover:
    s->T214DdrStatus = st;
    return T214DdrRecover(s);
}
