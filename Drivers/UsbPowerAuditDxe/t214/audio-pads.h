// SPDX-License-Identifier: GPL-2.0-only
// HDH-001 audio/audio-hv pad setup follows Switchroot's B01 PMC table and DT.
// The board supplies both domains at 1.8 V. This changes pad selection only,
// not regulator voltage, clock source or the codec's output gain.
#ifndef LITE_AUDIO_PADS_H
#define LITE_AUDIO_PADS_H

STATIC EFI_STATUS LiteAudioPreparePads (VOID)
{
  CONST UINTN Pmc = 0x7000E400UL;
  CONST UINT32 VoltageMask = (1U << 5) | (1U << 18);
  STATIC CONST struct { UINTN Request, Status; UINT32 Bit; } Pads[] = {
    { 0x1B8, 0x1BC, 1U << 17 }, // AUDIO
    { 0x1C0, 0x1C4, 1U << 29 }  // AUDIO_HV
  };
  UINT32 BeforeVoltage = MmioRead32 (Pmc + 0xE4);
  UINT32 BeforePower = MmioRead32 (Pmc + 0x44);
  UINT32 SavedSample = MmioRead32 (Pmc + 0x20);
  UINT32 SavedTimer = MmioRead32 (Pmc + 0x1C8);
  UINTN Index, Poll;
  EFI_STATUS Status = EFI_SUCCESS;

  // Linux: write-enable PWR_DET_VAL, select 1.8 V, allow the pads' I/O power.
  MmioOr32 (Pmc + 0x48, VoltageMask);
  MmioAnd32 (Pmc + 0xE4, ~VoltageMask);
  MmioAnd32 (Pmc + 0x44, ~VoltageMask);
  gBS->Stall (250);
  for (Index = 0; Index < ARRAY_SIZE (Pads); ++Index) {
    if ((MmioRead32 (Pmc + Pads[Index].Status) & Pads[Index].Bit) == 0) continue;
    MmioWrite32 (Pmc + 0x20, 1); // enable DPD sampling
    // At most 204 MHz PCLK: 41 cycles exceeds Linux's 200 ns minimum.
    MmioWrite32 (Pmc + 0x1C8, 41);
    MmioWrite32 (Pmc + Pads[Index].Request, (1U << 30) | Pads[Index].Bit);
    for (Poll = 0; Poll < 2500; ++Poll) {
      gBS->Stall (100);
      if ((MmioRead32 (Pmc + Pads[Index].Status) & Pads[Index].Bit) == 0) break;
    }
    if (Poll == 2500) { Status = EFI_TIMEOUT; break; }
  }
  MmioWrite32 (Pmc + 0x20, SavedSample);
  MmioWrite32 (Pmc + 0x1C8, SavedTimer);
  if (((MmioRead32 (Pmc + 0xE4) | MmioRead32 (Pmc + 0x44)) & VoltageMask) != 0)
    Status = EFI_DEVICE_ERROR;
  Print (L"Lite audio pads: voltage %08x -> %08x, power %08x -> %08x, DPD %08x/%08x: %r\r\n",
         BeforeVoltage, MmioRead32 (Pmc + 0xE4), BeforePower, MmioRead32 (Pmc + 0x44),
         MmioRead32 (Pmc + 0x1BC), MmioRead32 (Pmc + 0x1C4), Status);
  return Status;
}

#endif
