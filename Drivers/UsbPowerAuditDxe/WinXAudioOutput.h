// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef WINX_AUDIO_OUTPUT_H_
#define WINX_AUDIO_OUTPUT_H_

#include <Uefi.h>

#define WINX_AUDIO_OUTPUT_PROTOCOL_REVISION  0x00010000U
#define WINX_AUDIO_OUTPUT_SAMPLE_RATE        48000U
#define WINX_AUDIO_OUTPUT_CHANNELS           2U
#define WINX_AUDIO_OUTPUT_BITS_PER_SAMPLE    16U
#define WINX_AUDIO_OUTPUT_MAX_FRAMES         480U

typedef struct _WINX_AUDIO_OUTPUT_PROTOCOL WINX_AUDIO_OUTPUT_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *WINX_AUDIO_OUTPUT_WRITE_PCM16)(
  IN WINX_AUDIO_OUTPUT_PROTOCOL  *This,
  IN CONST INT16                 *InterleavedStereo,
  IN UINTN                       FrameCount
  );

typedef
EFI_STATUS
(EFIAPI *WINX_AUDIO_OUTPUT_STOP)(
  IN WINX_AUDIO_OUTPUT_PROTOCOL  *This
  );

struct _WINX_AUDIO_OUTPUT_PROTOCOL {
  UINT32                         Revision;
  UINT32                         SampleRate;
  UINT16                         Channels;
  UINT16                         BitsPerSample;
  WINX_AUDIO_OUTPUT_WRITE_PCM16  WritePcm16;
  WINX_AUDIO_OUTPUT_STOP         Stop;
};

extern EFI_GUID gWinXAudioOutputProtocolGuid;

#endif
