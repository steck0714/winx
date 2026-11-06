/* SPDX-License-Identifier: ISC */
/*
 * BCM4356 PCIe msgbuf-v5 control probe, based on brcmfmac/pcie.c and msgbuf.c.
 * Copyright (c) 2014 Broadcom Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Included after WifiPexStages.h. The probe owns DMA until it resets the Wi-Fi
 * endpoint, then unmaps/frees it before returning to the shell. It does not
 * leave live DMA across ExitBootServices or claim a Windows network adapter.
 */
#ifndef WIFI_MSGBUF_H
#define WIFI_MSGBUF_H

#define MSG_RING_COUNT 5U
#define MSG_BUFFER_SIZE 8192U
#define MSG_EVENT_SLOTS 8U
#define MSG_MC_AFI_ASID 0x70019238UL

typedef struct {
  UINT32 Offset;
  UINT32 WriteIndex;
  UINT32 ReadIndex;
  UINT16 Depth;
  UINT16 ItemSize;
  UINT16 HostIndex;
} PEX_MSG_RING;

typedef struct {
  UINT8 *Memory;
  VOID *Mapping;
  PHYSICAL_ADDRESS Bus;
  UINTN Pages;
  UINT32 IndexWidth;
  UINT32 RingInfo;
  UINT32 RxOffset;
  UINT32 InputOffset;
  UINT32 ReplyOffset;
  UINT32 EventOffset;
  UINT32 EventId[MSG_EVENT_SLOTS];
  BOOLEAN EventPosted[MSG_EVENT_SLOTS];
  UINT16 EventSequence;
  BOOLEAN (*EventHandler)(VOID *Context, CONST UINT8 *Data, UINT16 Length);
  VOID *EventContext;
  UINT32 SavedAfiAsid;
  BOOLEAN HostIndices;
  BOOLEAN AfiChanged;
  BOOLEAN Published;
  UINT16 Sequence;
  PEX_MSG_RING Ring[MSG_RING_COUNT];
} PEX_MSGBUF;

STATIC UINT16 MsgLe16 (CONST UINT8 *P) { return (UINT16)(P[0] | ((UINT16)P[1] << 8)); }
STATIC VOID MsgPut16 (UINT8 *P, UINT16 V) { P[0] = (UINT8)V; P[1] = (UINT8)(V >> 8); }
STATIC VOID MsgPut32 (UINT8 *P, UINT32 V)
{
  P[0] = (UINT8)V; P[1] = (UINT8)(V >> 8); P[2] = (UINT8)(V >> 16); P[3] = (UINT8)(V >> 24);
}
STATIC VOID MsgPutAddress (UINT8 *P, PHYSICAL_ADDRESS Address)
{
  MsgPut32 (P, (UINT32)Address); MsgPut32 (P + 4, (UINT32)(Address >> 32));
}
STATIC VOID MsgTcmAddress (UINT32 Address, PHYSICAL_ADDRESS Value)
{
  TcmWr32 (Address, (UINT32)Value); TcmWr32 (Address + 4, (UINT32)(Value >> 32));
}

STATIC UINT16 MsgIndexRead (PEX_MSGBUF *M, UINT32 Address)
{
  if (!M->HostIndices) return (UINT16)TcmRd32 (Address);
  if (M->IndexWidth == 2) return *(volatile UINT16 *)(M->Memory + Address);
  return (UINT16)*(volatile UINT32 *)(M->Memory + Address);
}

STATIC VOID MsgIndexWrite (PEX_MSGBUF *M, UINT32 Address, UINT16 Value)
{
  if (!M->HostIndices) {
    TcmWr32 (Address, (TcmRd32 (Address) & 0xFFFF0000U) | Value);
  } else if (M->IndexWidth == 2) {
    *(volatile UINT16 *)(M->Memory + Address) = Value;
  } else {
    *(volatile UINT32 *)(M->Memory + Address) = Value;
  }
  MemoryFence ();
}

STATIC BOOLEAN MsgSubmit (PEX_MSGBUF *M, CONST UINT8 *Message)
{
  PEX_MSG_RING *R = &M->Ring[0];
  UINT16 Peer = MsgIndexRead (M, R->ReadIndex);
  UINT16 Next = (UINT16)((R->HostIndex + 1U) % R->Depth);
  if (Peer >= R->Depth || Next == Peer) return FALSE;
  CopyMem (M->Memory + R->Offset + R->HostIndex * R->ItemSize, Message, R->ItemSize);
  MemoryFence ();
  R->HostIndex = Next;
  MsgIndexWrite (M, R->WriteIndex, Next);
  return BpWr32 (mPcieCore + 0x140, 1); // H2D doorbell 0
}

STATIC BOOLEAN MsgInitialize (PEX_MSGBUF *M)
{
  STATIC CONST UINT16 Depth[MSG_RING_COUNT] = {64, 512, 64, 1024, 512};
  STATIC CONST UINT16 ItemSize[MSG_RING_COUNT] = {40, 32, 24, 16, 32};
  UINT32 Flags, RingMemory, MaxSubmit, Total, Scratch, Update, I;
  UINT32 Index[4];
  UINTN MapSize;
  EFI_STATUS Status;

  if (!mChipReady || !mSharedAddr || !mPcieCore || !PexTcmContains (mSharedAddr, 80)) return FALSE;
  Flags = TcmRd32 (mSharedAddr);
  if ((Flags & 0xFFU) != 5) {
    Log (L"pctl: unsupported shared protocol %d (this transport implements v5)", Flags & 0xFFU);
    return FALSE;
  }
  M->RxOffset = TcmRd32 (mSharedAddr + 36);
  if (M->RxOffset > 8) {
    Log (L"pctl: completion header does not fit control ring, rx offset=%d", M->RxOffset);
    return FALSE;
  }
  M->RingInfo = TcmRd32 (mSharedAddr + 48);
  if ((M->RingInfo & 3) || !PexTcmContains (M->RingInfo, 56)) return FALSE;
  RingMemory = TcmRd32 (M->RingInfo);
  MaxSubmit = TcmRd32 (M->RingInfo + 52) & 0xFFFFU;
  if (MaxSubmit < 2 || (RingMemory & 3) || !PexTcmContains (RingMemory, 16 * MSG_RING_COUNT)) return FALSE;
  M->HostIndices = (Flags & BIT16) != 0;
  M->IndexWidth = M->HostIndices && (Flags & BIT20) ? 2 : 4;
  Total = 0;
  if (M->HostIndices) {
    Index[0] = 0;
    Index[1] = MaxSubmit * M->IndexWidth;
    Index[2] = 2 * MaxSubmit * M->IndexWidth;
    Index[3] = (2 * MaxSubmit + 3) * M->IndexWidth;
    Total = (2 * MaxSubmit + 6) * M->IndexWidth;
  } else {
    for (I = 0; I < 4; I++) {
      Index[I] = TcmRd32 (M->RingInfo + 4 + I * 4);
      if ((Index[I] & 3) || !PexTcmContains (Index[I], (I < 2 ? MaxSubmit : 3) * 4)) return FALSE;
    }
  }
  for (I = 0; I < MSG_RING_COUNT; I++) {
    PEX_MSG_RING *R = &M->Ring[I];
    Total = (Total + 63U) & ~63U;
    R->Offset = Total; R->Depth = Depth[I]; R->ItemSize = ItemSize[I];
    R->WriteIndex = Index[I < 2 ? 0 : 2] + (I < 2 ? I : I - 2) * M->IndexWidth;
    R->ReadIndex = Index[I < 2 ? 1 : 3] + (I < 2 ? I : I - 2) * M->IndexWidth;
    Total += (UINT32)R->Depth * R->ItemSize;
  }
  Scratch = (Total + 63U) & ~63U;
  Update = Scratch + 64;
  M->InputOffset = Update + 1024;
  M->ReplyOffset = M->InputOffset + MSG_BUFFER_SIZE;
  M->EventOffset = M->ReplyOffset + MSG_BUFFER_SIZE;
  Total = M->EventOffset + MSG_EVENT_SLOTS * MSG_BUFFER_SIZE;
  M->Pages = EFI_SIZE_TO_PAGES (Total);
  Status = DmaAllocateBuffer (EfiBootServicesData, M->Pages, (VOID **)&M->Memory);
  if (EFI_ERROR (Status)) return FALSE;
  MapSize = EFI_PAGES_TO_SIZE (M->Pages);
  Status = DmaMap (MapOperationBusMasterCommonBuffer, M->Memory, &MapSize, &M->Bus, &M->Mapping);
  if (EFI_ERROR (Status) || MapSize < Total || M->Bus > 0xFFFFFFFFULL || Total - 1 > 0xFFFFFFFFULL - M->Bus) return FALSE;
  ZeroMem (M->Memory, EFI_PAGES_TO_SIZE (M->Pages));

  // Isolate descriptor publication from bus mastering. The standalone UEFI
  // allocator supplies physical addresses; bypass only AFI's SMMU group and
  // restore its original setting after the endpoint has been reset.
  I = PexCfgRd (1, 0, 0, 4) & 0xFFFFU;
  PexCfgWr (1, 0, 0, 4, I & ~4U);
  if (PexCfgRd (1, 0, 0, 4) & 4U) return FALSE;
  M->SavedAfiAsid = MmioRead32 (MSG_MC_AFI_ASID);
  M->AfiChanged = TRUE;
  MmioWrite32 (MSG_MC_AFI_ASID, 0);
  if (MmioRead32 (MSG_MC_AFI_ASID) != 0) return FALSE;
  if (!BpWr32 (mPcieCore + 0x4C, 0)) return FALSE; // polled probe, no MSI/INTx
  MemoryFence ();
  M->Published = TRUE;
  if (M->HostIndices) {
    for (I = 0; I < 4; I++) MsgTcmAddress (M->RingInfo + 20 + 8 * I, M->Bus + Index[I]);
  }
  for (I = 0; I < MSG_RING_COUNT; I++) {
    PEX_MSG_RING *R = &M->Ring[I];
    TcmWr32 (RingMemory + 16 * I + 4, R->Depth | ((UINT32)R->ItemSize << 16));
    MsgTcmAddress (RingMemory + 16 * I + 8, M->Bus + R->Offset);
    // Fresh firmware owns zeroed peer indices; host publishes its own zero.
    if (MsgIndexRead (M, R->WriteIndex) || MsgIndexRead (M, R->ReadIndex)) {
      Log (L"pctl: nonzero initial ring indices; reload firmware before probing");
      return FALSE;
    }
    MsgIndexWrite (M, I < 2 ? R->WriteIndex : R->ReadIndex, 0);
  }
  TcmWr32 (mSharedAddr + 52, 8);
  MsgTcmAddress (mSharedAddr + 56, M->Bus + Scratch);
  TcmWr32 (mSharedAddr + 64, 1024);
  MsgTcmAddress (mSharedAddr + 68, M->Bus + Update);
  MemoryFence ();
  I = PexCfgRd (1, 0, 0, 4) & 0xFFFFU;
  PexCfgWr (1, 0, 0, 4, I | 6U);
  if ((PexCfgRd (1, 0, 0, 4) & 6U) != 6U) return FALSE;
  if ((Flags & BIT28) && !BpWr32 (mPcieCore + 0x144, 1)) return FALSE;
  Log (L"pctl: msgbuf v5, %s indices (%d bytes), %d submission queues, DMA=%lx size=%d",
       M->HostIndices ? L"host" : L"TCM", M->IndexWidth, MaxSubmit, M->Bus, Total);
  return TRUE;
}

STATIC BOOLEAN MsgPostEvent (PEX_MSGBUF *M, UINT32 Slot)
{
  UINT8 Message[40];
  if (Slot >= MSG_EVENT_SLOTS || M->EventPosted[Slot]) return FALSE;
  M->EventSequence++;
  M->EventId[Slot] = 0x20000000U | ((UINT32)M->EventSequence << 8) | Slot;
  ZeroMem (Message, sizeof (Message));
  Message[0] = 0x0D;
  MsgPut32 (Message + 4, M->EventId[Slot]);
  MsgPut16 (Message + 8, MSG_BUFFER_SIZE);
  MsgPutAddress (Message + 16, M->Bus + M->EventOffset + Slot * MSG_BUFFER_SIZE);
  M->EventPosted[Slot] = TRUE;
  return MsgSubmit (M, Message);
}

STATIC BOOLEAN MsgStartEvents (PEX_MSGBUF *M,
  BOOLEAN (*Handler)(VOID *, CONST UINT8 *, UINT16), VOID *Context)
{
  UINT32 I;
  M->EventHandler = Handler;
  M->EventContext = Context;
  for (I = 0; I < MSG_EVENT_SLOTS; I++) if (!MsgPostEvent (M, I)) return FALSE;
  return TRUE;
}

/* Drain both IOCTL replies and asynchronous firmware events. A handler must
 * consume its payload synchronously; the buffer is reposted afterwards.
 * Return -1 on transport failure, 1 for the requested IOCTL, 0 if pending. */
STATIC INTN MsgPollControl (PEX_MSGBUF *M, UINT32 WantedId,
  VOID *Buffer, UINT16 Length, UINT16 *Returned)
{
  PEX_MSG_RING *R = &M->Ring[2];
  UINT16 Writer = MsgIndexRead (M, R->WriteIndex);
  if (Writer >= R->Depth) return -1;
  MemoryFence ();
  while (R->HostIndex != Writer) {
    UINT8 *Reply = M->Memory + R->Offset + R->HostIndex * R->ItemSize + M->RxOffset;
    UINT8 Type = Reply[0];
    UINT32 Id = Le32 (Reply + 4);
    INT16 Status = (INT16)MsgLe16 (Reply + 8);
    UINT16 Size = MsgLe16 (Reply + 12);
    R->HostIndex = (UINT16)((R->HostIndex + 1U) % R->Depth);
    MemoryFence ();
    MsgIndexWrite (M, R->ReadIndex, R->HostIndex);
    if (Type == 0x0C) {
      if (!WantedId || Id != WantedId || Size > MSG_BUFFER_SIZE) {
        Log (L"msgbuf: invalid IOCTL completion id=%08x length=%d", Id, Size);
        return -1;
      }
      if (Status != 0) { Log (L"msgbuf: IOCTL firmware status=%d", Status); return -1; }
      *Returned = Size < Length ? Size : Length;
      CopyMem (Buffer, M->Memory + M->ReplyOffset, *Returned);
      return 1;
    }
    if (Type == 0x0E) {
      UINT32 Slot = Id & 0xFFU;
      if (Slot >= MSG_EVENT_SLOTS || !M->EventPosted[Slot] || M->EventId[Slot] != Id ||
          Status != 0 || Size > MSG_BUFFER_SIZE - M->RxOffset) {
        Log (L"msgbuf: invalid event completion id=%08x status=%d length=%d", Id, Status, Size);
        return -1;
      }
      M->EventPosted[Slot] = FALSE;
      if (M->EventHandler != NULL && !M->EventHandler (M->EventContext,
          M->Memory + M->EventOffset + Slot * MSG_BUFFER_SIZE + M->RxOffset, Size)) return -1;
      if (!MsgPostEvent (M, Slot)) return -1;
    } else if ((Type == 1 || Type == 2 || Type == 0x0A) && Status != 0) {
      Log (L"msgbuf: control status type=%d status=%d", Type, Status);
      return -1;
    }
  }
  return 0;
}

STATIC BOOLEAN MsgIoctl (PEX_MSGBUF *M, UINT32 Command, VOID *Buffer, UINT16 Length, UINT16 *Returned)
{
  UINT8 Message[40];
  UINT32 Id;
  UINTN Time;

  *Returned = 0;
  if (!Length || Length > MSG_BUFFER_SIZE) return FALSE;
  M->Sequence++;
  Id = 0x10000U | M->Sequence;
  ZeroMem (M->Memory + M->ReplyOffset, MSG_BUFFER_SIZE);
  ZeroMem (Message, sizeof (Message));
  Message[0] = 0x0B; // IOCTL response-buffer post
  MsgPut32 (Message + 4, Id);
  MsgPut16 (Message + 8, MSG_BUFFER_SIZE);
  MsgPutAddress (Message + 16, M->Bus + M->ReplyOffset);
  if (!MsgSubmit (M, Message)) return FALSE;

  CopyMem (M->Memory + M->InputOffset, Buffer, Length);
  ZeroMem (Message, sizeof (Message));
  Message[0] = 0x09; // IOCTL pointer request
  MsgPut32 (Message + 4, 0xFFFE);
  MsgPut32 (Message + 8, Command);
  MsgPut16 (Message + 12, M->Sequence);
  MsgPut16 (Message + 14, Length);
  MsgPut16 (Message + 16, Length);
  MsgPutAddress (Message + 24, M->Bus + M->InputOffset);
  if (!MsgSubmit (M, Message)) return FALSE;

  for (Time = 0; Time < 2000; Time++) {
    INTN Result = MsgPollControl (M, Id, Buffer, Length, Returned);
    if (Result < 0) return FALSE;
    if (Result > 0) return TRUE;
    Ms (1);
  }
  Log (L"pctl: IOCTL timeout cmd=%d", Command);
  return FALSE;
}

STATIC BOOLEAN MsgSetIovar (PEX_MSGBUF *M, CONST CHAR8 *Name, CONST VOID *Value, UINT16 Length)
{
  UINT8 *Buffer = M->Memory + M->InputOffset;
  UINT16 Returned;
  UINTN NameLength = AsciiStrLen (Name) + 1;
  if (NameLength + Length > MSG_BUFFER_SIZE) return FALSE;
  CopyMem (Buffer, Name, NameLength);
  CopyMem (Buffer + NameLength, Value, Length);
  return MsgIoctl (M, 263, Buffer, (UINT16)(NameLength + Length), &Returned);
}

/* brcmfmac common.c: regulatory CLM data is downloaded before WLC_UP.
 * Keep the 1400-byte firmware chunk limit and the v1 dload_data wire layout.
 * The Linux structure includes three trailing padding bytes after data[1]. */
STATIC BOOLEAN MsgLoadClm (PEX_MSGBUF *M, CONST UINT8 *Data, UINTN Length)
{
  UINT8 Chunk[15 + 1400];
  UINTN Offset = 0;
  if (Data == NULL || Length == 0) return FALSE;
  while (Offset < Length) {
    UINT16 Count = (UINT16)((Length - Offset > 1400) ? 1400 : Length - Offset);
    UINT16 Flags = 0x1000;
    if (Offset == 0) Flags |= 2;
    if (Offset + Count == Length) Flags |= 4;
    ZeroMem (Chunk, sizeof (Chunk));
    MsgPut16 (Chunk, Flags);
    MsgPut16 (Chunk + 2, 2);
    MsgPut32 (Chunk + 4, Count);
    CopyMem (Chunk + 12, Data + Offset, Count);
    if (!MsgSetIovar (M, "clmload", Chunk, (UINT16)(15 + Count))) {
      Log (L"pscan: CLM upload failed at byte %d", (UINT32)Offset);
      return FALSE;
    }
    Offset += Count;
  }
  Log (L"pscan: CLM upload complete (%d bytes)", (UINT32)Length);
  return TRUE;
}

STATIC VOID MsgRelease (PEX_MSGBUF *M)
{
  if (M->Published || M->AfiChanged) {
    UINT32 Command = PexCfgRd (1, 0, 0, 4) & 0xFFFFU;
    PexCfgWr (1, 0, 0, 4, Command & ~4U);
    // Stop CR4, assert WLAN reset and hold the port in reset before freeing DMA.
    PexOff ();
    Ms (20);
  }
  if (M->AfiChanged) {
    MmioWrite32 (MSG_MC_AFI_ASID, M->SavedAfiAsid);
    (VOID)MmioRead32 (MSG_MC_AFI_ASID);
    MemoryFence ();
  }
  if (M->Mapping != NULL) DmaUnmap (M->Mapping);
  if (M->Memory != NULL) DmaFreeBuffer (M->Pages, M->Memory);
  ZeroMem (M, sizeof (*M));
}

STATIC BOOLEAN PexControlProbe (VOID)
{
  PEX_MSGBUF M;
  UINT8 Buffer[256];
  UINT16 Returned;
  BOOLEAN Result = FALSE;
  UINTN I;
  ZeroMem (&M, sizeof (M));
  if (!MsgInitialize (&M)) goto Done;
  ZeroMem (Buffer, sizeof (Buffer));
  if (!MsgIoctl (&M, 1, Buffer, 4, &Returned) || Returned != 4) goto Done;
  Log (L"pctl: WLC_GET_VERSION=%d", Le32 (Buffer));
  ZeroMem (Buffer, sizeof (Buffer));
  CopyMem (Buffer, "ver", 4);
  if (!MsgIoctl (&M, 262, Buffer, sizeof (Buffer), &Returned)) goto Done;
  for (I = 0; I < Returned && I < sizeof (Buffer) - 1; I++) {
    if (Buffer[I] == 0) break;
    if (Buffer[I] < 0x20 || Buffer[I] >= 0x7F) Buffer[I] = ' ';
  }
  Buffer[I] = 0;
  Log (L"pctl: firmware %a", Buffer);
  Result = TRUE;
Done:
  MsgRelease (&M);
  Log (L"pctl: %s; DMA released, reload Wi-Fi firmware before another probe", Result ? L"CONTROL PASS" : L"CONTROL FAILED");
  return Result;
}

#endif
