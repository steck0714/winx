// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>

#include <Guid/FileSystemInfo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>

#include "SwitchScreenshot.h"

#define BMP_HEADER_SIZE  54
#define CAPTURE_ROWS_PER_WRITE  32

STATIC UINT32  mCaptureSequence;
STATIC UINT32  mLastCaptureSecondOfDay;
STATIC BOOLEAN mLastCaptureTimeValid;

STATIC
VOID
PutLe16 (
  OUT UINT8   *Buffer,
  IN  UINT16  Value
  )
{
  Buffer[0] = (UINT8)Value;
  Buffer[1] = (UINT8)(Value >> 8);
}

STATIC
VOID
PutLe32 (
  OUT UINT8   *Buffer,
  IN  UINT32  Value
  )
{
  Buffer[0] = (UINT8)Value;
  Buffer[1] = (UINT8)(Value >> 8);
  Buffer[2] = (UINT8)(Value >> 16);
  Buffer[3] = (UINT8)(Value >> 24);
}

STATIC
EFI_STATUS
WriteAll (
  IN EFI_FILE_PROTOCOL  *File,
  IN CONST VOID         *Buffer,
  IN UINTN              BufferSize
  )
{
  EFI_STATUS  Status;
  UINTN       Written;

  while (BufferSize != 0) {
    Written = BufferSize;
    Status  = File->Write (File, &Written, (VOID *)Buffer);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if ((Written == 0) || (Written > BufferSize)) {
      return EFI_DEVICE_ERROR;
    }

    Buffer     = (CONST UINT8 *)Buffer + Written;
    BufferSize -= Written;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
OpenOrCreateDirectory (
  IN  EFI_FILE_PROTOCOL  *Parent,
  IN  CONST CHAR16       *Name,
  OUT EFI_FILE_PROTOCOL  **Directory
  )
{
  return Parent->Open (
                  Parent,
                  Directory,
                  (CHAR16 *)Name,
                  EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                  EFI_FILE_DIRECTORY
                  );
}

STATIC
EFI_STATUS
WriteBitmap (
  IN EFI_FILE_PROTOCOL               *File,
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL    *Gop
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels;
  UINT8                         Header[BMP_HEADER_SIZE];
  UINT8                         *Row;
  UINT64                        PixelCount;
  UINT64                        ImageSize;
  UINT64                        RowStride64;
  UINTN                         RowStride;
  UINTN                         PixelBufferSize;
  UINTN                         RowBufferSize;
  UINTN                         RowsPerChunk;
  UINTN                         ChunkRows;
  UINT32                        Width;
  UINT32                        Height;
  UINT32                        X;
  UINT32                        Y;
  UINT32                        RowIndex;
  UINTN                         SourceY;

  if (Gop == NULL || Gop->Mode == NULL || Gop->Mode->Info == NULL) {
    return EFI_UNSUPPORTED;
  }

  Width  = Gop->Mode->Info->HorizontalResolution;
  Height = Gop->Mode->Info->VerticalResolution;
  if ((Width == 0) || (Height == 0) || (Width > MAX_INT32) || (Height > MAX_INT32)) {
    return EFI_UNSUPPORTED;
  }

  PixelCount = (UINT64)Width * Height;
  RowStride64 = (((UINT64)Width * 3) + 3) & ~((UINT64)3);
  ImageSize = RowStride64 * Height;
  if ((RowStride64 > MAX_UINTN) || (ImageSize > MAX_UINT32 - BMP_HEADER_SIZE))
  {
    return EFI_UNSUPPORTED;
  }

  RowStride = (UINTN)RowStride64;
  RowsPerChunk = (Height < CAPTURE_ROWS_PER_WRITE) ? Height : CAPTURE_ROWS_PER_WRITE;
  if ((PixelCount == 0) ||
      ((UINT64)Width * RowsPerChunk > MAX_UINTN / sizeof (*Pixels)) ||
      ((UINT64)RowStride * RowsPerChunk > MAX_UINTN))
  {
    return EFI_UNSUPPORTED;
  }

  PixelBufferSize = (UINTN)Width * RowsPerChunk * sizeof (*Pixels);
  RowBufferSize   = RowStride * RowsPerChunk;
  Pixels          = AllocatePool (PixelBufferSize);
  Row             = AllocateZeroPool (RowBufferSize);
  if ((Pixels == NULL) || (Row == NULL)) {
    if (Pixels != NULL) {
      FreePool (Pixels);
    }
    if (Row != NULL) {
      FreePool (Row);
    }
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (Header, sizeof (Header));
  Header[0] = 'B';
  Header[1] = 'M';
  PutLe32 (&Header[2], (UINT32)(BMP_HEADER_SIZE + ImageSize));
  PutLe32 (&Header[10], BMP_HEADER_SIZE);
  PutLe32 (&Header[14], 40);
  PutLe32 (&Header[18], Width);
  PutLe32 (&Header[22], Height);
  PutLe16 (&Header[26], 1);
  PutLe16 (&Header[28], 24);
  PutLe32 (&Header[34], (UINT32)ImageSize);

  Status = WriteAll (File, Header, sizeof (Header));
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  for (Y = 0; Y < Height; Y += (UINT32)RowsPerChunk) {
    ChunkRows = Height - Y;
    if (ChunkRows > RowsPerChunk) ChunkRows = RowsPerChunk;
    SourceY = (UINTN)Height - Y - ChunkRows;
    Status = Gop->Blt (
                    Gop,
                    Pixels,
                    EfiBltVideoToBltBuffer,
                    0,
                    SourceY,
                    0,
                    0,
                    Width,
                    (UINT32)ChunkRows,
                    0
                    );
    if (EFI_ERROR (Status)) goto Done;

    for (RowIndex = 0; RowIndex < ChunkRows; ++RowIndex) {
      UINT8  *OutputRow;

      OutputRow = Row + ((UINTN)RowIndex * RowStride);
      ZeroMem (OutputRow, RowStride);
      for (X = 0; X < Width; ++X) {
        SourceY = ((UINTN)ChunkRows - 1 - RowIndex) * Width + X;
        OutputRow[X * 3]     = Pixels[SourceY].Blue;
        OutputRow[X * 3 + 1] = Pixels[SourceY].Green;
        OutputRow[X * 3 + 2] = Pixels[SourceY].Red;
      }
    }

    Status = WriteAll (File, Row, ChunkRows * RowStride);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  Status = File->Flush (File);

Done:
  FreePool (Pixels);
  FreePool (Row);
  return Status;
}

STATIC
EFI_STATUS
CaptureOnVolume (
  IN  EFI_HANDLE                       Handle,
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL    *Gop,
  OUT CHAR16                          *SavedPath,
  IN  UINTN                           SavedPathChars
  )
{
  EFI_STATUS                    Status;
  EFI_BLOCK_IO_PROTOCOL         *BlockIo;
  EFI_FILE_SYSTEM_INFO          *FileSystemInfo;
  EFI_FILE_PROTOCOL             *Root;
  EFI_FILE_PROTOCOL             *EfiDirectory;
  EFI_FILE_PROTOCOL             *WinxDirectory;
  EFI_FILE_PROTOCOL             *CaptureDirectory;
  EFI_FILE_PROTOCOL             *File;
  EFI_FILE_PROTOCOL             *Existing;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFileSystem;
  UINTN                         InfoSize;
  CHAR16                        FileName[64];
  EFI_TIME                      Time;
  UINT32                        CurrentSecondOfDay;
  UINT32                        ElapsedSeconds;
  BOOLEAN                       TimeValid;
  UINT32                        Sequence;
  UINTN                         Attempt;

  Root             = NULL;
  EfiDirectory     = NULL;
  WinxDirectory    = NULL;
  CaptureDirectory = NULL;
  File             = NULL;
  Existing         = NULL;
  FileSystemInfo   = NULL;

  Status = gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
  if (EFI_ERROR (Status) || BlockIo == NULL || BlockIo->Media == NULL ||
      !BlockIo->Media->MediaPresent || !BlockIo->Media->RemovableMedia)
  {
    return EFI_UNSUPPORTED;
  }

  Status = gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&SimpleFileSystem);
  if (EFI_ERROR (Status) || SimpleFileSystem == NULL) {
    return EFI_UNSUPPORTED;
  }

  Status = SimpleFileSystem->OpenVolume (SimpleFileSystem, &Root);
  if (EFI_ERROR (Status) || Root == NULL) {
    return EFI_UNSUPPORTED;
  }

  InfoSize = 0;
  Status   = Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  FileSystemInfo = AllocatePool (InfoSize);
  if (FileSystemInfo == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Status = Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &InfoSize, FileSystemInfo);
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  if (FileSystemInfo->ReadOnly) {
    Status = EFI_WRITE_PROTECTED;
    goto Done;
  }

  Status = OpenOrCreateDirectory (Root, L"bootloader", &EfiDirectory);
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  Status = OpenOrCreateDirectory (EfiDirectory, L"screenshots", &WinxDirectory);
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  CaptureDirectory = WinxDirectory;
  WinxDirectory = NULL;

  ZeroMem (&Time, sizeof (Time));
  Status = gRT->GetTime (&Time, NULL);
  TimeValid = (BOOLEAN)(!EFI_ERROR (Status) && Time.Year >= 2000 &&
                        Time.Month >= 1 && Time.Month <= 12 &&
                        Time.Day >= 1 && Time.Day <= 31 &&
                        Time.Hour <= 23 && Time.Minute <= 59 && Time.Second <= 59);
  CurrentSecondOfDay = (UINT32)Time.Hour * 3600U + (UINT32)Time.Minute * 60U + Time.Second;
  if (TimeValid) {
    ElapsedSeconds = (CurrentSecondOfDay >= mLastCaptureSecondOfDay) ?
                     CurrentSecondOfDay - mLastCaptureSecondOfDay :
                     86400U - mLastCaptureSecondOfDay + CurrentSecondOfDay;
    if (mLastCaptureTimeValid && (ElapsedSeconds < 2)) {
      Status = EFI_NOT_READY;
      goto Done;
    }
  } else {
    // Without a valid RTC, keep capture available and use the sequence suffix
    // below to avoid overwriting an earlier image with the same zero timestamp.
    mLastCaptureTimeValid = FALSE;
  }

  for (Attempt = 0; Attempt < 10000; ++Attempt) {
    Sequence = mCaptureSequence++;
    UnicodeSPrint (
      FileName,
      sizeof (FileName),
      L"winx%04u%02u%02u_%02u%02u%02u-%04u.bmp",
      (UINT32)Time.Year,
      (UINT32)Time.Month,
      (UINT32)Time.Day,
      (UINT32)Time.Hour,
      (UINT32)Time.Minute,
      (UINT32)Time.Second,
      Sequence
      );

    Existing = NULL;
    Status = CaptureDirectory->Open (
                               CaptureDirectory,
                               &Existing,
                               FileName,
                               EFI_FILE_MODE_READ,
                               0
                               );
    if (!EFI_ERROR (Status)) {
      Existing->Close (Existing);
      Existing = NULL;
      continue;
    }
    if (Status != EFI_NOT_FOUND) {
      goto Done;
    }

    Status = CaptureDirectory->Open (
                               CaptureDirectory,
                               &File,
                               FileName,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                               EFI_FILE_ARCHIVE
                               );
    if (!EFI_ERROR (Status)) {
      break;
    }
    goto Done;
  }

  if (File == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Status = WriteBitmap (File, Gop);
  if (!EFI_ERROR (Status)) {
    if (TimeValid) {
      mLastCaptureSecondOfDay = CurrentSecondOfDay;
      mLastCaptureTimeValid = TRUE;
    } else {
      mLastCaptureTimeValid = FALSE;
    }
    UnicodeSPrint (
      SavedPath,
      SavedPathChars * sizeof (*SavedPath),
      L"\\bootloader\\screenshots\\%s",
      FileName
      );
  }

Done:
  if (File != NULL) {
    File->Close (File);
  }
  if (CaptureDirectory != NULL) {
    CaptureDirectory->Close (CaptureDirectory);
  }
  if (WinxDirectory != NULL) {
    WinxDirectory->Close (WinxDirectory);
  }
  if (EfiDirectory != NULL) {
    EfiDirectory->Close (EfiDirectory);
  }
  if (Root != NULL) {
    Root->Close (Root);
  }
  if (FileSystemInfo != NULL) {
    FreePool (FileSystemInfo);
  }

  return Status;
}

EFI_STATUS
WinxCaptureBootMenuScreenshot (
  OUT CHAR16  *SavedPath,
  IN  UINTN   SavedPathChars
  )
{
  EFI_STATUS                       Status;
  EFI_STATUS                       LastStatus;
  EFI_GRAPHICS_OUTPUT_PROTOCOL    *Gop;
  EFI_HANDLE                      *Handles;
  UINTN                           HandleCount;
  UINTN                           Index;

  if ((SavedPath == NULL) || (SavedPathChars == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  SavedPath[0] = L'\0';

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || Gop == NULL) {
    return EFI_UNSUPPORTED;
  }

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  LastStatus = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; ++Index) {
    Status = CaptureOnVolume (Handles[Index], Gop, SavedPath, SavedPathChars);
    if (!EFI_ERROR (Status)) {
      LastStatus = EFI_SUCCESS;
      break;
    }
    if (Status != EFI_UNSUPPORTED && Status != EFI_WRITE_PROTECTED && Status != EFI_ACCESS_DENIED) {
      LastStatus = Status;
    }
  }

  FreePool (Handles);
  return LastStatus;
}
