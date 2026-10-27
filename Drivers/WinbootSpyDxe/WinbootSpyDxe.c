/** @file
  WinbootSpyDxe: find out which firmware call makes bootmgr.efi give up (0xc0000185).

  Records, deduplicated with counts, every failing call of
    BootServices: HandleProtocol/OpenProtocol/LocateProtocol/LocateDevicePath/LoadImage/StartImage/
                  AllocatePages/AllocatePool/GetMemoryMap
    RuntimeServices: GetVariable/SetVariable/QueryVariableInfo/GetTime
    BlockIo ReadBlocks/WriteBlocks (LBA, size, status, largest OK read)
    File protocol Open/Read (file name, status, sizes)
  plus an ordered event list (file opens, LoadImage/StartImage, ExitBootServices), every BlockIo handle
  with its device path / partition info, configuration tables and a memory map summary.
  The text is rewritten to \winboot_spy.log of the volume that holds \EFI or \bootmgr.efi, once a
  second while something changed (and just before ExitBootServices).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DevicePathToText.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

typedef EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME EFI_VOLUME_OPEN;
#define IsDevicePathEnd(N)      ((N)->Type == END_DEVICE_PATH_TYPE && (N)->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)
#define DevicePathType(N)       ((N)->Type)
#define DevicePathSubType(N)    ((N)->SubType)
#define NextDevicePathNode(N)   ((CONST EFI_DEVICE_PATH_PROTOCOL *)((CONST UINT8 *)(N) + ((N)->Length[0] | ((N)->Length[1] << 8))))
#define LOG_MAX      (96 * 1024)
#define MAXU         320          // unique failure entries
#define MAXEV        300          // ordered events
#define MAXBLK       16
#define MAXFILE      256
#define MAXSFS       8

enum { K_HANDLEPROTO, K_OPENPROTO, K_LOCATEPROTO, K_LOCATEDP, K_LOADIMAGE, K_STARTIMAGE, K_ALLOCPAGES,
       K_ALLOCPOOL, K_GETMMAP, K_GETVAR, K_SETVAR, K_QUERYVAR, K_GETTIME, K_BLKREAD, K_BLKWRITE,
       K_FOPEN, K_FREAD, K_KINDS };
STATIC CONST CHAR8 *mKindName[K_KINDS] = {
  "HandleProtocol", "OpenProtocol", "LocateProtocol", "LocateDevicePath", "LoadImage", "StartImage",
  "AllocatePages", "AllocatePool", "GetMemoryMap", "GetVariable", "SetVariable", "QueryVariableInfo",
  "GetTime", "BlockIo.ReadBlocks", "BlockIo.WriteBlocks", "File.Open", "File.Read" };

typedef struct {
  UINT8       Kind;
  EFI_STATUS  Status;
  UINT32      Count;
  EFI_GUID    Guid;
  CHAR8       Name[56];
  UINT64      A, B;          // first occurrence arguments
} UENT;

typedef struct { UINT32 Seq; UINT8 Kind; EFI_STATUS Status; CHAR8 Text[88]; } EVENT;

typedef struct {
  EFI_BLOCK_IO_PROTOCOL *P;
  EFI_BLOCK_READ         OrigRead;
  EFI_BLOCK_WRITE        OrigWrite;
  UINT64                 Reads, Writes, BytesRead, FailRead, FailWrite;
  UINT64                 MaxOkRead, MinFailRead;
} BLK;

typedef struct {
  EFI_FILE_PROTOCOL *F;
  EFI_FILE_OPEN      OrigOpen;
  EFI_FILE_READ      OrigRead;
  EFI_FILE_CLOSE     OrigClose;
  CHAR8              Name[64];
  UINT64             Bytes, MaxReq, Reads, Fails;
} FIL;

typedef struct { EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *P; EFI_VOLUME_OPEN Orig; } SFSH;

STATIC UENT   *mU;      STATIC UINTN mNU;     STATIC UINT64 mDropU;
STATIC EVENT  *mEv;     STATIC UINTN mNEv;    STATIC UINT32 mSeq;
STATIC BLK     mBlk[MAXBLK];  STATIC UINTN mNBlk;
STATIC FIL    *mFil;    STATIC UINTN mNFil;
STATIC SFSH    mSfs[MAXSFS];  STATIC UINTN mNSfs;
STATIC UINT32  mDirty, mFlushed;
STATIC volatile UINTN mBusy;     // BlockIo calls in flight: never touch the SD from the timer while one is running
STATIC BOOLEAN mInSpy;
STATIC CHAR8  *mLog;
STATIC EFI_HANDLE mVolHandle;
STATIC EFI_EVENT  mTimer, mNotifyEv, mNotifyEv2;
STATIC VOID   *mReg, *mReg2;

STATIC EFI_HANDLE_PROTOCOL        OrigHandleProtocol;
STATIC EFI_OPEN_PROTOCOL          OrigOpenProtocol;
STATIC EFI_LOCATE_PROTOCOL        OrigLocateProtocol;
STATIC EFI_LOCATE_DEVICE_PATH     OrigLocateDevicePath;
STATIC EFI_IMAGE_LOAD             OrigLoadImage;
STATIC EFI_IMAGE_START            OrigStartImage;
STATIC EFI_ALLOCATE_PAGES         OrigAllocatePages;
STATIC EFI_ALLOCATE_POOL          OrigAllocatePool;
STATIC EFI_GET_MEMORY_MAP         OrigGetMemoryMap;
STATIC EFI_EXIT_BOOT_SERVICES     OrigExitBS;
STATIC EFI_GET_VARIABLE           OrigGetVariable;
STATIC EFI_SET_VARIABLE           OrigSetVariable;
STATIC EFI_QUERY_VARIABLE_INFO    OrigQueryVariableInfo;
STATIC EFI_GET_TIME               OrigGetTime;

STATIC VOID Flush (BOOLEAN Final);
STATIC VOID ConsoleSummary (VOID);

STATIC VOID UcsToAscii (CONST CHAR16 *S, CHAR8 *D, UINTN Max)
{
  UINTN I = 0;
  if (S != NULL) { for (; S[I] != 0 && I + 1 < Max; I++) D[I] = (S[I] < 0x80 && S[I] >= 0x20) ? (CHAR8)S[I] : '?'; }
  D[I] = 0;
}

// Record one failing call (deduplicated by kind/guid/name/status).
STATIC VOID Note (UINT8 Kind, EFI_STATUS St, CONST EFI_GUID *G, CONST CHAR8 *Name, UINT64 A, UINT64 B)
{
  UINTN I; EFI_TPL T;
  if (mInSpy || mU == NULL) return;
  T = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  for (I = 0; I < mNU; I++) {
    if (mU[I].Kind == Kind && mU[I].Status == St &&
        ((G == NULL && IsZeroGuid (&mU[I].Guid)) || (G != NULL && CompareGuid (&mU[I].Guid, G))) &&
        (Name == NULL || AsciiStrnCmp (mU[I].Name, Name, sizeof (mU[I].Name) - 1) == 0)) {
      mU[I].Count++; goto done;
    }
  }
  if (mNU < MAXU) {
    UENT *E = &mU[mNU++];
    E->Kind = Kind; E->Status = St; E->Count = 1; E->A = A; E->B = B;
    if (G != NULL) CopyGuid (&E->Guid, G); else ZeroMem (&E->Guid, sizeof (EFI_GUID));
    if (Name != NULL) AsciiStrnCpyS (E->Name, sizeof (E->Name), Name, sizeof (E->Name) - 1); else E->Name[0] = 0;
  } else mDropU++;
done:
  mDirty++;
  gBS->RestoreTPL (T);
}

STATIC VOID Event (UINT8 Kind, EFI_STATUS St, CONST CHAR8 *Text)
{
  EFI_TPL T;
  if (mInSpy || mEv == NULL) return;
  T = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if (mNEv < MAXEV) {
    EVENT *E = &mEv[mNEv++];
    E->Seq = mSeq++; E->Kind = Kind; E->Status = St;
    AsciiStrnCpyS (E->Text, sizeof (E->Text), Text, sizeof (E->Text) - 1);
    mDirty++;
  }
  gBS->RestoreTPL (T);
}

STATIC VOID PathTail (CONST EFI_DEVICE_PATH_PROTOCOL *P, CHAR8 *Out, UINTN Max)
{
  CONST EFI_DEVICE_PATH_PROTOCOL *N = P;
  CONST CHAR16 *Last = NULL;
  Out[0] = 0;
  while (N != NULL && N->Length[0] >= 4 && !IsDevicePathEnd (N)) {
    if (DevicePathType (N) == MEDIA_DEVICE_PATH && DevicePathSubType (N) == MEDIA_FILEPATH_DP) Last = (CONST CHAR16 *)(N + 1);
    N = NextDevicePathNode (N);
  }
  if (Last != NULL) UcsToAscii (Last, Out, Max);
}

// ---- Boot services hooks ----
STATIC EFI_STATUS EFIAPI HHandleProtocol (EFI_HANDLE H, EFI_GUID *G, VOID **I)
{
  EFI_STATUS S = OrigHandleProtocol (H, G, I);
  if (EFI_ERROR (S)) Note (K_HANDLEPROTO, S, G, NULL, 0, 0);
  return S;
}
STATIC EFI_STATUS EFIAPI HOpenProtocol (EFI_HANDLE H, EFI_GUID *G, VOID **I, EFI_HANDLE AH, EFI_HANDLE CH, UINT32 A)
{
  EFI_STATUS S = OrigOpenProtocol (H, G, I, AH, CH, A);
  if (EFI_ERROR (S)) Note (K_OPENPROTO, S, G, NULL, A, 0);
  return S;
}
STATIC EFI_STATUS EFIAPI HLocateProtocol (EFI_GUID *G, VOID *R, VOID **I)
{
  EFI_STATUS S = OrigLocateProtocol (G, R, I);
  if (EFI_ERROR (S)) Note (K_LOCATEPROTO, S, G, NULL, 0, 0);
  return S;
}
STATIC EFI_STATUS EFIAPI HLocateDevicePath (EFI_GUID *G, EFI_DEVICE_PATH_PROTOCOL **DP, EFI_HANDLE *D)
{
  EFI_STATUS S = OrigLocateDevicePath (G, DP, D);
  if (EFI_ERROR (S)) Note (K_LOCATEDP, S, G, NULL, 0, 0);
  return S;
}
STATIC EFI_STATUS EFIAPI HLoadImage (BOOLEAN BP, EFI_HANDLE PH, EFI_DEVICE_PATH_PROTOCOL *DP, VOID *SB, UINTN SS, EFI_HANDLE *IH)
{
  CHAR8 Nm[64], Tx[88];
  EFI_STATUS S = OrigLoadImage (BP, PH, DP, SB, SS, IH);
  PathTail (DP, Nm, sizeof (Nm));
  AsciiSPrint (Tx, sizeof (Tx), "LoadImage '%a'%a size=%lu", Nm, SB != NULL ? " (buffer)" : "", (UINT64)SS);
  Event (K_LOADIMAGE, S, Tx);
  if (EFI_ERROR (S)) Note (K_LOADIMAGE, S, NULL, Nm, SS, 0);
  return S;
}
STATIC EFI_STATUS EFIAPI HStartImage (EFI_HANDLE IH, UINTN *ES, CHAR16 **ED)
{
  CHAR8 Tx[88];
  EFI_STATUS S;
  AsciiSPrint (Tx, sizeof (Tx), "StartImage handle=%p ...", IH);
  Event (K_STARTIMAGE, EFI_SUCCESS, Tx);
  S = OrigStartImage (IH, ES, ED);
  AsciiSPrint (Tx, sizeof (Tx), "StartImage handle=%p returned", IH);
  Event (K_STARTIMAGE, S, Tx);
  if (EFI_ERROR (S)) { Note (K_STARTIMAGE, S, NULL, NULL, (UINT64)(UINTN)IH, 0); ConsoleSummary (); }
  return S;
}
STATIC EFI_STATUS EFIAPI HAllocatePages (EFI_ALLOCATE_TYPE T, EFI_MEMORY_TYPE M, UINTN P, EFI_PHYSICAL_ADDRESS *A)
{
  EFI_PHYSICAL_ADDRESS In = (A != NULL) ? *A : 0;
  EFI_STATUS S = OrigAllocatePages (T, M, P, A);
  if (EFI_ERROR (S)) { CHAR8 Tx[56]; AsciiSPrint (Tx, sizeof (Tx), "type=%d mem=%d", T, M); Note (K_ALLOCPAGES, S, NULL, Tx, (UINT64)P << 12, In); }
  return S;
}
STATIC EFI_STATUS EFIAPI HAllocatePool (EFI_MEMORY_TYPE M, UINTN Sz, VOID **B)
{
  EFI_STATUS S = OrigAllocatePool (M, Sz, B);
  if (EFI_ERROR (S)) { CHAR8 Tx[56]; AsciiSPrint (Tx, sizeof (Tx), "mem=%d", M); Note (K_ALLOCPOOL, S, NULL, Tx, Sz, 0); }
  return S;
}
STATIC EFI_STATUS EFIAPI HGetMemoryMap (UINTN *Sz, EFI_MEMORY_DESCRIPTOR *Map, UINTN *Key, UINTN *DS, UINT32 *DV)
{
  EFI_STATUS S = OrigGetMemoryMap (Sz, Map, Key, DS, DV);
  if (EFI_ERROR (S) && S != EFI_BUFFER_TOO_SMALL) Note (K_GETMMAP, S, NULL, NULL, 0, 0);
  return S;
}
STATIC EFI_STATUS EFIAPI HExitBS (EFI_HANDLE IH, UINTN Key)
{
  EFI_STATUS S;
  Event (K_STARTIMAGE, EFI_SUCCESS, "ExitBootServices called");
  Flush (TRUE);
  S = OrigExitBS (IH, Key);
  if (EFI_ERROR (S)) Note (K_STARTIMAGE, S, NULL, "ExitBootServices", Key, 0);
  return S;
}

// ---- Runtime services hooks ----
STATIC EFI_STATUS EFIAPI HGetVariable (CHAR16 *N, EFI_GUID *G, UINT32 *At, UINTN *Sz, VOID *D)
{
  EFI_STATUS S = OrigGetVariable (N, G, At, Sz, D);
  if (EFI_ERROR (S) && S != EFI_BUFFER_TOO_SMALL) { CHAR8 Nm[56]; UcsToAscii (N, Nm, sizeof (Nm)); Note (K_GETVAR, S, G, Nm, 0, 0); }
  return S;
}
STATIC EFI_STATUS EFIAPI HSetVariable (CHAR16 *N, EFI_GUID *G, UINT32 At, UINTN Sz, VOID *D)
{
  EFI_STATUS S = OrigSetVariable (N, G, At, Sz, D);
  if (EFI_ERROR (S)) { CHAR8 Nm[56]; UcsToAscii (N, Nm, sizeof (Nm)); Note (K_SETVAR, S, G, Nm, At, Sz); }
  return S;
}
STATIC EFI_STATUS EFIAPI HQueryVariableInfo (UINT32 At, UINT64 *M, UINT64 *R, UINT64 *Mx)
{
  EFI_STATUS S = OrigQueryVariableInfo (At, M, R, Mx);
  if (EFI_ERROR (S)) Note (K_QUERYVAR, S, NULL, NULL, At, 0);
  return S;
}
STATIC EFI_STATUS EFIAPI HGetTime (EFI_TIME *T, EFI_TIME_CAPABILITIES *C)
{
  EFI_STATUS S = OrigGetTime (T, C);
  if (EFI_ERROR (S)) Note (K_GETTIME, S, NULL, NULL, 0, 0);
  return S;
}

// ---- BlockIo ----
STATIC BLK *FindBlk (EFI_BLOCK_IO_PROTOCOL *P)
{
  UINTN I;
  for (I = 0; I < mNBlk; I++) if (mBlk[I].P == P) return &mBlk[I];
  return NULL;
}
STATIC EFI_STATUS EFIAPI HBlkRead (EFI_BLOCK_IO_PROTOCOL *This, UINT32 Mid, EFI_LBA Lba, UINTN Sz, VOID *Buf)
{
  BLK *B = FindBlk (This);
  EFI_STATUS S;
  if (B == NULL) return EFI_DEVICE_ERROR;
  mBusy++;
  S = B->OrigRead (This, Mid, Lba, Sz, Buf);
  mBusy--;
  if (!mInSpy) {
    B->Reads++;
    if (EFI_ERROR (S)) {
      CHAR8 Tx[56];
      B->FailRead++;
      if (B->MinFailRead == 0 || Sz < B->MinFailRead) B->MinFailRead = Sz;
      AsciiSPrint (Tx, sizeof (Tx), "blk#%d", (INT32)(B - mBlk));
      Note (K_BLKREAD, S, NULL, Tx, Lba, Sz);
    } else {
      B->BytesRead += Sz;
      if (Sz > B->MaxOkRead) B->MaxOkRead = Sz;
    }
    mDirty++;
  }
  return S;
}
STATIC EFI_STATUS EFIAPI HBlkWrite (EFI_BLOCK_IO_PROTOCOL *This, UINT32 Mid, EFI_LBA Lba, UINTN Sz, VOID *Buf)
{
  BLK *B = FindBlk (This);
  EFI_STATUS S;
  if (B == NULL) return EFI_DEVICE_ERROR;
  mBusy++;
  S = B->OrigWrite (This, Mid, Lba, Sz, Buf);
  mBusy--;
  if (!mInSpy) {
    B->Writes++;
    if (EFI_ERROR (S)) { CHAR8 Tx[56]; B->FailWrite++; AsciiSPrint (Tx, sizeof (Tx), "blk#%d", (INT32)(B - mBlk)); Note (K_BLKWRITE, S, NULL, Tx, Lba, Sz); }
  }
  return S;
}

// ---- File protocol ----
STATIC FIL *FindFil (EFI_FILE_PROTOCOL *F)
{
  UINTN I;
  for (I = 0; I < mNFil; I++) if (mFil[I].F == F) return &mFil[I];
  return NULL;
}
STATIC EFI_STATUS EFIAPI HFClose (EFI_FILE_PROTOCOL *This);
STATIC EFI_STATUS EFIAPI HFOpen (EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 At);
STATIC EFI_STATUS EFIAPI HFRead (EFI_FILE_PROTOCOL *This, UINTN *Sz, VOID *Buf);

STATIC VOID HookFile (EFI_FILE_PROTOCOL *F, CONST CHAR8 *Name)
{
  FIL *E; EFI_TPL T;
  if (F == NULL || F->Open == HFOpen) return;     // already hooked
  T = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if (mNFil < MAXFILE) {
    E = &mFil[mNFil++];
    ZeroMem (E, sizeof (*E));
    E->F = F; E->OrigOpen = F->Open; E->OrigRead = F->Read; E->OrigClose = F->Close;
    AsciiStrnCpyS (E->Name, sizeof (E->Name), Name, sizeof (E->Name) - 1);
    F->Open = HFOpen; F->Read = HFRead; F->Close = HFClose;
  }
  gBS->RestoreTPL (T);
}
STATIC EFI_STATUS EFIAPI HFOpen (EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 At)
{
  FIL *E = FindFil (This);
  CHAR8 Nm[64], Full[64], Tx[88];
  EFI_STATUS S;
  if (E == NULL) return EFI_DEVICE_ERROR;
  S = E->OrigOpen (This, New, Name, Mode, At);
  UcsToAscii (Name, Nm, sizeof (Nm));
  if (Nm[0] == '\\' || E->Name[0] == 0) AsciiStrCpyS (Full, sizeof (Full), Nm);
  else AsciiSPrint (Full, sizeof (Full), "%a\\%a", E->Name, Nm);
  AsciiSPrint (Tx, sizeof (Tx), "Open '%a' mode=%lx", Full, Mode);
  Event (K_FOPEN, S, Tx);
  if (EFI_ERROR (S)) Note (K_FOPEN, S, NULL, Full, Mode, 0);
  else if (New != NULL && *New != NULL) HookFile (*New, Full);
  return S;
}
STATIC EFI_STATUS EFIAPI HFRead (EFI_FILE_PROTOCOL *This, UINTN *Sz, VOID *Buf)
{
  FIL *E = FindFil (This);
  UINTN Req = (Sz != NULL) ? *Sz : 0;
  EFI_STATUS S;
  if (E == NULL) return EFI_DEVICE_ERROR;
  S = E->OrigRead (This, Sz, Buf);
  if (!mInSpy) {
    E->Reads++;
    if (Req > E->MaxReq) E->MaxReq = Req;
    if (EFI_ERROR (S)) { E->Fails++; Note (K_FREAD, S, NULL, E->Name, Req, 0); }
    else if (Sz != NULL) E->Bytes += *Sz;
    mDirty++;
  }
  return S;
}
STATIC EFI_STATUS EFIAPI HFClose (EFI_FILE_PROTOCOL *This)
{
  FIL *E = FindFil (This);
  EFI_FILE_CLOSE C;
  EFI_TPL T;
  if (E == NULL) return EFI_DEVICE_ERROR;
  C = E->OrigClose;
  // keep a one-line summary of big files in the event list
  if (E->Bytes >= 0x100000 && !mInSpy) {
    CHAR8 Tx[88];
    AsciiSPrint (Tx, sizeof (Tx), "Close '%a' read=%lu bytes in %lu reads, max req=%lu", E->Name, E->Bytes, E->Reads, E->MaxReq);
    Event (K_FREAD, EFI_SUCCESS, Tx);
  }
  T = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  *E = mFil[--mNFil];       // unhook slot (swap with last); the handle is freed by Close
  gBS->RestoreTPL (T);
  return C (This);
}
STATIC EFI_STATUS EFIAPI HOpenVolume (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root)
{
  UINTN I; EFI_VOLUME_OPEN O = NULL; EFI_STATUS S;
  for (I = 0; I < mNSfs; I++) if (mSfs[I].P == This) O = mSfs[I].Orig;
  if (O == NULL) return EFI_DEVICE_ERROR;
  S = O (This, Root);
  if (EFI_ERROR (S)) Note (K_FOPEN, S, NULL, "OpenVolume", 0, 0);
  else if (Root != NULL && !mInSpy) HookFile (*Root, "");
  return S;
}

// ---- hooking of new protocol instances ----
STATIC VOID ScanHandles (VOID)
{
  EFI_HANDLE *H = NULL; UINTN N = 0, I;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &N, &H))) {
    for (I = 0; I < N; I++) {
      EFI_BLOCK_IO_PROTOCOL *P;
      if (EFI_ERROR (OrigHandleProtocol (H[I], &gEfiBlockIoProtocolGuid, (VOID **)&P)) || FindBlk (P) != NULL || mNBlk >= MAXBLK) continue;
      ZeroMem (&mBlk[mNBlk], sizeof (BLK));
      mBlk[mNBlk].P = P; mBlk[mNBlk].OrigRead = P->ReadBlocks; mBlk[mNBlk].OrigWrite = P->WriteBlocks;
      P->ReadBlocks = HBlkRead; P->WriteBlocks = HBlkWrite;
      mNBlk++;
    }
    FreePool (H);
  }
  H = NULL; N = 0;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &N, &H))) {
    for (I = 0; I < N; I++) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *P; UINTN J; BOOLEAN Have = FALSE;
      if (EFI_ERROR (OrigHandleProtocol (H[I], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&P))) continue;
      for (J = 0; J < mNSfs; J++) if (mSfs[J].P == P) Have = TRUE;
      if (Have || mNSfs >= MAXSFS) continue;
      mSfs[mNSfs].P = P; mSfs[mNSfs].Orig = P->OpenVolume; P->OpenVolume = HOpenVolume; mNSfs++;
    }
    FreePool (H);
  }
}
STATIC VOID EFIAPI NotifyScan (EFI_EVENT E, VOID *C)
{
  BOOLEAN Save = mInSpy;
  mInSpy = TRUE; ScanHandles (); mInSpy = Save;
}

// ---- report ----
STATIC UINTN mLen;
STATIC VOID Pr (CONST CHAR8 *Fmt, ...)
{
  VA_LIST V;
  if (mLen + 400 >= LOG_MAX) return;
  VA_START (V, Fmt);
  mLen += AsciiVSPrint (mLog + mLen, LOG_MAX - mLen, Fmt, V);
  VA_END (V);
}

STATIC VOID Report (VOID)
{
  UINTN I, N, MS = 0, Key, DS; UINT32 DV; EFI_MEMORY_DESCRIPTOR *Map = NULL, *D;
  EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *DT = NULL;
  UINT64 Conv = 0, Big = 0, Cnt[16];
  mLen = 0;
  Pr ("# winboot_spy (flush #%u, %u events%a)\r\n\r\n", mFlushed, (UINT32)mNEv, mNEv >= MAXEV ? " TRUNCATED" : "");
  OrigLocateProtocol (&gEfiDevicePathToTextProtocolGuid, NULL, (VOID **)&DT);

  Pr ("## Block devices\r\n");
  for (I = 0; I < mNBlk; I++) {
    BLK *B = &mBlk[I]; EFI_HANDLE *H = NULL; UINTN NH = 0, J; CHAR16 *Txt = NULL;
    Pr ("blk#%u media=%u blocksize=%u last=%lu logical-part=%u removable=%u readonly=%u present=%u\r\n",
        (UINT32)I, B->P->Media->MediaId, B->P->Media->BlockSize, B->P->Media->LastBlock,
        B->P->Media->LogicalPartition, B->P->Media->RemovableMedia, B->P->Media->ReadOnly, B->P->Media->MediaPresent);
    if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &NH, &H))) {
      for (J = 0; J < NH; J++) {
        EFI_BLOCK_IO_PROTOCOL *P; EFI_DEVICE_PATH_PROTOCOL *DP = NULL; EFI_PARTITION_INFO_PROTOCOL *PI = NULL;
        if (EFI_ERROR (OrigHandleProtocol (H[J], &gEfiBlockIoProtocolGuid, (VOID **)&P)) || P != B->P) continue;
        if (!EFI_ERROR (OrigHandleProtocol (H[J], &gEfiDevicePathProtocolGuid, (VOID **)&DP)) && DT != NULL) {
          Txt = DT->ConvertDevicePathToText (DP, FALSE, FALSE);
          if (Txt != NULL) { Pr ("   path: %S\r\n", Txt); FreePool (Txt); }
        }
        if (!EFI_ERROR (OrigHandleProtocol (H[J], &gEfiPartitionInfoProtocolGuid, (VOID **)&PI)) && PI != NULL) {
          Pr ("   partinfo: type=%u (1=MBR 2=GPT) sys=%u", PI->Type, PI->System);
          if (PI->Type == PARTITION_TYPE_GPT) Pr (" typeguid=%g uniqueguid=%g", &PI->Info.Gpt.PartitionTypeGUID, &PI->Info.Gpt.UniquePartitionGUID);
          else if (PI->Type == PARTITION_TYPE_MBR) Pr (" ostype=0x%02x start=%u size=%u", PI->Info.Mbr.OSIndicator, *(UINT32 *)PI->Info.Mbr.StartingLBA, *(UINT32 *)PI->Info.Mbr.SizeInLBA);
          Pr ("\r\n");
        } else Pr ("   partinfo: none\r\n");
      }
      FreePool (H);
    }
    Pr ("   reads=%lu fail=%lu bytes=%lu maxOkRead=%lu minFailRead=%lu writes=%lu writefail=%lu\r\n",
        B->Reads, B->FailRead, B->BytesRead, B->MaxOkRead, B->MinFailRead, B->Writes, B->FailWrite);
  }

  Pr ("\r\n## Configuration tables\r\n");
  for (I = 0; I < gST->NumberOfTableEntries; I++) Pr ("%g @ %p\r\n", &gST->ConfigurationTable[I].VendorGuid, gST->ConfigurationTable[I].VendorTable);

  Pr ("\r\n## Memory\r\n");
  MS = 0;
  if (OrigGetMemoryMap (&MS, NULL, &Key, &DS, &DV) == EFI_BUFFER_TOO_SMALL) {
    MS += 8 * DS;
    if (!EFI_ERROR (OrigAllocatePool (EfiBootServicesData, MS, (VOID **)&Map))) {
      if (!EFI_ERROR (OrigGetMemoryMap (&MS, Map, &Key, &DS, &DV))) {
        ZeroMem (Cnt, sizeof (Cnt));
        for (D = Map; (UINT8 *)D < (UINT8 *)Map + MS; D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)D + DS)) {
          if (D->Type < 16) Cnt[D->Type] += D->NumberOfPages;
          if (D->Type == EfiConventionalMemory) { Conv += D->NumberOfPages; if (D->NumberOfPages > Big) Big = D->NumberOfPages; }
          if (D->NumberOfPages >= 0x10000) Pr ("region type=%u phys=0x%lx pages=%lu (%lu MiB) attr=0x%lx\r\n", D->Type, D->PhysicalStart, D->NumberOfPages, D->NumberOfPages >> 8, D->Attribute);
        }
        Pr ("conventional=%lu MiB, largest free block=%lu MiB, descriptors=%u\r\n", Conv >> 8, Big >> 8, (UINT32)(MS / DS));
        for (I = 0; I < 16; I++) if (Cnt[I]) Pr ("  type %u: %lu pages\r\n", (UINT32)I, Cnt[I]);
      }
      FreePool (Map);
    }
  }

  Pr ("\r\n## Events (in order)\r\n");
  for (I = 0; I < mNEv; I++) Pr ("%u %a -> %r\r\n", mEv[I].Seq, mEv[I].Text, mEv[I].Status);

  Pr ("\r\n## Failing calls (unique, count)\r\n");
  for (I = 0; I < mNU; I++) {
    UENT *E = &mU[I];
    Pr ("%a %r x%u", mKindName[E->Kind], E->Status, E->Count);
    if (!IsZeroGuid (&E->Guid)) Pr (" guid=%g", &E->Guid);
    if (E->Name[0]) Pr (" name='%a'", E->Name);
    if (E->A || E->B) Pr (" a=0x%lx b=0x%lx", E->A, E->B);
    Pr ("\r\n");
  }
  if (mDropU) Pr ("(+%lu dropped, table full)\r\n", mDropU);

  Pr ("\r\n## Open files\r\n");
  N = mNFil;
  for (I = 0; I < N; I++) if (mFil[I].Reads) Pr ("'%a' reads=%lu fail=%lu bytes=%lu maxreq=%lu\r\n", mFil[I].Name, mFil[I].Reads, mFil[I].Fails, mFil[I].Bytes, mFil[I].MaxReq);
}

STATIC BOOLEAN TryWrite (EFI_HANDLE H)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs; EFI_FILE_PROTOCOL *Root = NULL, *F = NULL, *T = NULL; EFI_STATUS S; UINTN Sz;
  BOOLEAN Ok = FALSE;
  if (EFI_ERROR (OrigHandleProtocol (H, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))) return FALSE;
  // call the ORIGINAL OpenVolume so the hooks aren't involved
  { UINTN I; EFI_VOLUME_OPEN O = Fs->OpenVolume; for (I = 0; I < mNSfs; I++) if (mSfs[I].P == Fs) O = mSfs[I].Orig; S = O (Fs, &Root); }
  if (EFI_ERROR (S) || Root == NULL) return FALSE;
  {
    FIL *E = FindFil (Root);
    EFI_FILE_OPEN Op = (E != NULL) ? E->OrigOpen : Root->Open;
    EFI_FILE_CLOSE Cl = (E != NULL) ? E->OrigClose : Root->Close;
    EFI_FILE_WRITE Wr;
    // only the volume that carries the boot files
    if (mVolHandle != H) {
      if (EFI_ERROR (Op (Root, &T, L"\\EFI", EFI_FILE_MODE_READ, 0)) && EFI_ERROR (Op (Root, &T, L"\\bootmgr.efi", EFI_FILE_MODE_READ, 0))) { Cl (Root); return FALSE; }
      T->Close (T);   // handle may already be hooked? it was opened with the original Open, so it is not
    }
    S = Op (Root, &F, L"\\winboot_spy.log", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (!EFI_ERROR (S) && F != NULL) {
      Wr = F->Write; Sz = mLen;
      S = Wr (F, &Sz, mLog);
      if (!EFI_ERROR (S)) { F->Flush (F); Ok = TRUE; mVolHandle = H; }
      F->Close (F);
    }
    Cl (Root);
  }
  return Ok;
}

STATIC VOID Flush (BOOLEAN Final)
{
  EFI_HANDLE *H = NULL; UINTN N = 0, I; BOOLEAN Save = mInSpy;
  if (mInSpy || mLog == NULL) return;
  if (!Final && (mDirty == mFlushed || mBusy != 0)) return;
  mInSpy = TRUE;
  mFlushed = mDirty;
  Report ();
  if (mVolHandle != NULL && TryWrite (mVolHandle)) goto out;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &N, &H))) {
    for (I = 0; I < N; I++) if (TryWrite (H[I])) break;
    FreePool (H);
  }
out:
  mInSpy = Save;
}

// Print the failing calls on screen (when an image returns an error), then wait so it can be photographed.
STATIC VOID ConsoleSummary (VOID)
{
  UINTN I, Shown = 0, NotFound = 0; BOOLEAN Save = mInSpy;
  if (mInSpy) return;
  mInSpy = TRUE;
  Flush (TRUE);
  mInSpy = TRUE;
  Print (L"\n[winboot_spy] image returned an error; failing calls (not NOT_FOUND):\n");
  for (I = 0; I < mNU; I++) {
    UENT *E = &mU[I];
    if (E->Status == EFI_NOT_FOUND) { NotFound++; continue; }
    if (Shown++ >= 30) continue;
    Print (L"%a %r x%u %a a=%lx b=%lx\n", mKindName[E->Kind], E->Status, E->Count, E->Name, E->A, E->B);
  }
  Print (L"(+%u more, %u NOT_FOUND; blk reads/fail:", (UINT32)(Shown > 30 ? Shown - 30 : 0), (UINT32)NotFound);
  for (I = 0; I < mNBlk; I++) Print (L" #%u %lu/%lu max=%lu", (UINT32)I, mBlk[I].Reads, mBlk[I].FailRead, mBlk[I].MaxOkRead);
  Print (L")\nFull log: \\winboot_spy.log. Waiting 30 s...\n");
  gBS->Stall (30000000);
  mInSpy = Save;
}

STATIC VOID EFIAPI TimerTick (EFI_EVENT E, VOID *C)
{
  Flush (FALSE);
}

STATIC VOID HookTables (VOID)
{
  OrigHandleProtocol = gBS->HandleProtocol;       gBS->HandleProtocol = HHandleProtocol;
  OrigOpenProtocol = gBS->OpenProtocol;           gBS->OpenProtocol = HOpenProtocol;
  OrigLocateProtocol = gBS->LocateProtocol;       gBS->LocateProtocol = HLocateProtocol;
  OrigLocateDevicePath = gBS->LocateDevicePath;   gBS->LocateDevicePath = HLocateDevicePath;
  OrigLoadImage = gBS->LoadImage;                 gBS->LoadImage = HLoadImage;
  OrigStartImage = gBS->StartImage;               gBS->StartImage = HStartImage;
  OrigAllocatePages = gBS->AllocatePages;         gBS->AllocatePages = HAllocatePages;
  OrigAllocatePool = gBS->AllocatePool;           gBS->AllocatePool = HAllocatePool;
  OrigGetMemoryMap = gBS->GetMemoryMap;           gBS->GetMemoryMap = HGetMemoryMap;
  OrigExitBS = gBS->ExitBootServices;             gBS->ExitBootServices = HExitBS;
  gBS->Hdr.CRC32 = 0; gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
  OrigGetVariable = gRT->GetVariable;             gRT->GetVariable = HGetVariable;
  OrigSetVariable = gRT->SetVariable;             gRT->SetVariable = HSetVariable;
  OrigQueryVariableInfo = gRT->QueryVariableInfo; gRT->QueryVariableInfo = HQueryVariableInfo;
  OrigGetTime = gRT->GetTime;                     gRT->GetTime = HGetTime;
  gRT->Hdr.CRC32 = 0; gBS->CalculateCrc32 (gRT, gRT->Hdr.HeaderSize, &gRT->Hdr.CRC32);
}

EFI_STATUS EFIAPI WinbootSpyEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  mU   = AllocateZeroPool (MAXU * sizeof (UENT));
  mEv  = AllocateZeroPool (MAXEV * sizeof (EVENT));
  mFil = AllocateZeroPool (MAXFILE * sizeof (FIL));
  mLog = AllocateZeroPool (LOG_MAX);
  if (mU == NULL || mEv == NULL || mFil == NULL || mLog == NULL) return EFI_OUT_OF_RESOURCES;
  HookTables ();
  mInSpy = TRUE; ScanHandles (); mInSpy = FALSE;
  gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, NotifyScan, NULL, &mNotifyEv);
  gBS->RegisterProtocolNotify (&gEfiBlockIoProtocolGuid, mNotifyEv, &mReg);
  gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, NotifyScan, NULL, &mNotifyEv2);
  gBS->RegisterProtocolNotify (&gEfiSimpleFileSystemProtocolGuid, mNotifyEv2, &mReg2);
  gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, TimerTick, NULL, &mTimer);
  gBS->SetTimer (mTimer, TimerPeriodic, 30000000);
  Event (K_STARTIMAGE, EFI_SUCCESS, "WinbootSpyDxe loaded");
  return EFI_SUCCESS;
}
