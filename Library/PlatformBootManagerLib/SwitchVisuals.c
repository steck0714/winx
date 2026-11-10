// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>

#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BmpSupportLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/BootLogo.h>
#include <Protocol/BootLogo2.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>

#include "SwitchVisuals.h"

#define WINX_ASSET_ROOT             L"\\bootloader\\res\\winx\\"
#define WINX_MAX_BITMAP_FILE_SIZE   (16U * 1024U * 1024U)
#define WINX_MAX_BITMAP_PIXELS      (4U * 1024U * 1024U)
#define WINX_MAX_BITMAP_DIMENSION   4096U
#define WINX_ANIMATION_MAX_FRAMES   8U
#define WINX_ANIMATION_FRAME_DELAY  80000U

typedef struct {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixels;
  UINTN                          Width;
  UINTN                          Height;
} WINX_BITMAP;

STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *mMenuBackground;
STATIC UINTN                          mMenuBackgroundWidth;
STATIC UINTN                          mMenuBackgroundHeight;
STATIC BOOLEAN                        mMenuBackgroundChecked;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *mFirmwareSplash;
STATIC UINTN                          mFirmwareSplashWidth;
STATIC UINTN                          mFirmwareSplashHeight;
STATIC UINTN                          mFirmwareSplashX;
STATIC UINTN                          mFirmwareSplashY;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *mSavedBootLogo;
STATIC UINTN                          mSavedBootLogoWidth;
STATIC UINTN                          mSavedBootLogoHeight;
STATIC UINTN                          mSavedBootLogoX;
STATIC UINTN                          mSavedBootLogoY;
STATIC BOOLEAN                        mBootLogoInvalidated;
STATIC BOOLEAN                        mRemovableStorageConnected;

STATIC
EFI_STATUS
LocateGop (
  OUT EFI_GRAPHICS_OUTPUT_PROTOCOL  **Gop
  )
{
  if (Gop == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Gop = NULL;
  return gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)Gop);
}

STATIC
EFI_STATUS
LoadBitmapFromRoot (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path,
  OUT WINX_BITMAP        *Bitmap
  )
{
  EFI_STATUS       Status;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO    *Info;
  UINTN            InfoSize;
  UINT8            *FileBuffer;
  UINTN            FileSize;
  UINTN            ReadSize;
  UINTN            GopBltSize;
  RETURN_STATUS    ReturnStatus;

  if ((Root == NULL) || (Path == NULL) || (Bitmap == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Bitmap, sizeof (*Bitmap));
  File = NULL;
  Info = NULL;
  FileBuffer = NULL;
  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    goto Done;
  }

  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if ((Info->Attribute & EFI_FILE_DIRECTORY) != 0 ||
      (Info->FileSize < 54) || (Info->FileSize > WINX_MAX_BITMAP_FILE_SIZE))
  {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  FileSize = (UINTN)Info->FileSize;
  FileBuffer = AllocatePool (FileSize);
  if (FileBuffer == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  ReadSize = FileSize;
  Status = File->Read (File, &ReadSize, FileBuffer);
  if (EFI_ERROR (Status) || (ReadSize != FileSize)) {
    if (!EFI_ERROR (Status)) {
      Status = EFI_DEVICE_ERROR;
    }
    goto Done;
  }

  GopBltSize = 0;
  ReturnStatus = TranslateBmpToGopBlt (
                   FileBuffer,
                   FileSize,
                   &Bitmap->Pixels,
                   &GopBltSize,
                   &Bitmap->Height,
                   &Bitmap->Width
                   );
  if (RETURN_ERROR (ReturnStatus)) {
    Status = (EFI_STATUS)ReturnStatus;
    goto Done;
  }

  if ((Bitmap->Width == 0) || (Bitmap->Height == 0) ||
      (Bitmap->Width > WINX_MAX_BITMAP_DIMENSION) ||
      (Bitmap->Height > WINX_MAX_BITMAP_DIMENSION) ||
      ((UINT64)Bitmap->Width * Bitmap->Height > WINX_MAX_BITMAP_PIXELS))
  {
    FreePool (Bitmap->Pixels);
    ZeroMem (Bitmap, sizeof (*Bitmap));
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  Status = EFI_SUCCESS;

Done:
  if (FileBuffer != NULL) {
    FreePool (FileBuffer);
  }
  if (Info != NULL) {
    FreePool (Info);
  }
  if (File != NULL) {
    File->Close (File);
  }
  return Status;
}

STATIC
VOID
ConnectRemovableStorage (
  VOID
  )
{
  EFI_HANDLE             *BlockHandles;
  UINTN                  BlockCount;
  UINTN                  BlockIndex;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;

  BlockHandles = NULL;
  BlockCount = 0;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
                                           NULL, &BlockCount, &BlockHandles))) {
    for (BlockIndex = 0; BlockIndex < BlockCount; ++BlockIndex) {
      BlockIo = NULL;
      if (!EFI_ERROR (gBS->HandleProtocol (BlockHandles[BlockIndex], &gEfiBlockIoProtocolGuid,
                                          (VOID **)&BlockIo)) &&
          (BlockIo != NULL) && (BlockIo->Media != NULL) &&
          BlockIo->Media->MediaPresent && BlockIo->Media->RemovableMedia)
      {
        gBS->ConnectController (BlockHandles[BlockIndex], NULL, NULL, TRUE);
      }
    }
    FreePool (BlockHandles);
  }
}

STATIC
EFI_STATUS
SearchFileSystems (
  IN  CONST CHAR16  *FullPath,
  OUT WINX_BITMAP   *Bitmap
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;

  Handles = NULL;
  HandleCount = 0;
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

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; ++Index) {
    FileSystem = NULL;
    Root = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid,
                                       (VOID **)&FileSystem)) || FileSystem == NULL) {
      continue;
    }
    if (EFI_ERROR (FileSystem->OpenVolume (FileSystem, &Root)) || Root == NULL) {
      continue;
    }

    Status = LoadBitmapFromRoot (Root, FullPath, Bitmap);
    Root->Close (Root);
    if (!EFI_ERROR (Status)) {
      break;
    }
    if (Status == EFI_OUT_OF_RESOURCES) {
      break;
    }
    Status = EFI_NOT_FOUND;
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }
  return Status;
}

STATIC
EFI_STATUS
LoadBitmap (
  IN  CONST CHAR16  *RelativePath,
  OUT WINX_BITMAP   *Bitmap
  )
{
  EFI_STATUS  Status;
  CHAR16      FullPath[160];

  if ((RelativePath == NULL) || (Bitmap == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  UnicodeSPrint (FullPath, sizeof (FullPath), L"%s%s", WINX_ASSET_ROOT, RelativePath);
  Status = SearchFileSystems (FullPath, Bitmap);
  if (!EFI_ERROR (Status) || (Status == EFI_OUT_OF_RESOURCES)) {
    return Status;
  }

  if (!mRemovableStorageConnected) {
    ConnectRemovableStorage ();
    mRemovableStorageConnected = TRUE;
    Status = SearchFileSystems (FullPath, Bitmap);
  }
  return Status;
}

STATIC
EFI_STATUS
AllocateScreenBuffer (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop,
  OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL **Pixels,
  OUT UINTN                         *Width,
  OUT UINTN                         *Height
  )
{
  UINT64  PixelCount;
  UINT64  BufferSize;

  if ((Gop == NULL) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL) ||
      (Pixels == NULL) || (Width == NULL) || (Height == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *Pixels = NULL;
  *Width = Gop->Mode->Info->HorizontalResolution;
  *Height = Gop->Mode->Info->VerticalResolution;
  PixelCount = (UINT64)*Width * *Height;
  BufferSize = PixelCount * sizeof (**Pixels);
  if ((*Width == 0) || (*Height == 0) ||
      (*Width > WINX_MAX_BITMAP_DIMENSION) || (*Height > WINX_MAX_BITMAP_DIMENSION) ||
      (PixelCount > WINX_MAX_BITMAP_PIXELS) || (BufferSize > MAX_UINTN))
  {
    return EFI_UNSUPPORTED;
  }

  *Pixels = AllocatePool ((UINTN)BufferSize);
  return (*Pixels == NULL) ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
}

STATIC
VOID
RenderBitmap (
  IN  WINX_BITMAP                    *Bitmap,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Screen,
  IN  UINTN                          ScreenWidth,
  IN  UINTN                          ScreenHeight,
  IN  BOOLEAN                        Cover
  )
{
  UINTN  DestX;
  UINTN  DestY;
  UINTN  DestWidth;
  UINTN  DestHeight;
  UINTN  CropX;
  UINTN  CropY;
  UINTN  CropWidth;
  UINTN  CropHeight;
  UINTN  X;
  UINTN  Y;
  UINTN  SourceX;
  UINTN  SourceY;
  UINT64 LeftRatio;
  UINT64 RightRatio;

  SetMem (Screen, ScreenWidth * ScreenHeight * sizeof (*Screen), 0);
  DestX = 0;
  DestY = 0;
  DestWidth = ScreenWidth;
  DestHeight = ScreenHeight;
  CropX = 0;
  CropY = 0;
  CropWidth = Bitmap->Width;
  CropHeight = Bitmap->Height;

  LeftRatio = (UINT64)Bitmap->Width * ScreenHeight;
  RightRatio = (UINT64)Bitmap->Height * ScreenWidth;
  if (Cover) {
    if (LeftRatio > RightRatio) {
      CropWidth = (UINTN)((UINT64)Bitmap->Height * ScreenWidth / ScreenHeight);
      CropX = (Bitmap->Width - CropWidth) / 2;
    } else if (LeftRatio < RightRatio) {
      CropHeight = (UINTN)((UINT64)Bitmap->Width * ScreenHeight / ScreenWidth);
      CropY = (Bitmap->Height - CropHeight) / 2;
    }
  } else {
    if (LeftRatio > RightRatio) {
      DestHeight = (UINTN)((UINT64)Bitmap->Height * ScreenWidth / Bitmap->Width);
      DestY = (ScreenHeight - DestHeight) / 2;
    } else if (LeftRatio < RightRatio) {
      DestWidth = (UINTN)((UINT64)Bitmap->Width * ScreenHeight / Bitmap->Height);
      DestX = (ScreenWidth - DestWidth) / 2;
    }
  }

  for (Y = 0; Y < DestHeight; ++Y) {
    SourceY = CropY + (UINTN)((UINT64)Y * CropHeight / DestHeight);
    for (X = 0; X < DestWidth; ++X) {
      SourceX = CropX + (UINTN)((UINT64)X * CropWidth / DestWidth);
      Screen[(DestY + Y) * ScreenWidth + DestX + X] = Bitmap->Pixels[SourceY * Bitmap->Width + SourceX];
    }
  }
}

STATIC
VOID
RenderCenteredLogo (
  IN  WINX_BITMAP                    *Bitmap,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Screen,
  IN  UINTN                          ScreenWidth,
  IN  UINTN                          ScreenHeight,
  OUT UINTN                          *LogoX,
  OUT UINTN                          *LogoY,
  OUT UINTN                          *LogoWidth,
  OUT UINTN                          *LogoHeight
  )
{
  UINTN  X;
  UINTN  Y;
  UINTN  SourceX;
  UINTN  SourceY;
  UINT64 LeftRatio;
  UINT64 RightRatio;

  SetMem (Screen, ScreenWidth * ScreenHeight * sizeof (*Screen), 0);
  *LogoWidth = Bitmap->Width;
  *LogoHeight = Bitmap->Height;
  if ((*LogoWidth > ScreenWidth) || (*LogoHeight > ScreenHeight)) {
    LeftRatio = (UINT64)Bitmap->Width * ScreenHeight;
    RightRatio = (UINT64)Bitmap->Height * ScreenWidth;
    if (LeftRatio > RightRatio) {
      *LogoWidth = ScreenWidth;
      *LogoHeight = (UINTN)((UINT64)Bitmap->Height * ScreenWidth / Bitmap->Width);
    } else {
      *LogoHeight = ScreenHeight;
      *LogoWidth = (UINTN)((UINT64)Bitmap->Width * ScreenHeight / Bitmap->Height);
    }
    if (*LogoWidth == 0) *LogoWidth = 1;
    if (*LogoHeight == 0) *LogoHeight = 1;
  }

  *LogoX = (ScreenWidth - *LogoWidth) / 2;
  *LogoY = (ScreenHeight - *LogoHeight) / 2;
  for (Y = 0; Y < *LogoHeight; ++Y) {
    SourceY = (UINTN)((UINT64)Y * Bitmap->Height / *LogoHeight);
    for (X = 0; X < *LogoWidth; ++X) {
      SourceX = (UINTN)((UINT64)X * Bitmap->Width / *LogoWidth);
      Screen[(*LogoY + Y) * ScreenWidth + *LogoX + X] =
        Bitmap->Pixels[SourceY * Bitmap->Width + SourceX];
    }
  }
}

STATIC
EFI_STATUS
CopyScreenRegion (
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Screen,
  IN  UINTN                          ScreenWidth,
  IN  UINTN                          LogoX,
  IN  UINTN                          LogoY,
  IN  UINTN                          LogoWidth,
  IN  UINTN                          LogoHeight,
  OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL  **LogoPixels
  )
{
  UINTN  Y;
  UINTN  BufferSize;

  if ((Screen == NULL) || (LogoPixels == NULL) || (LogoWidth == 0) || (LogoHeight == 0) ||
      (LogoWidth > MAX_UINTN / LogoHeight / sizeof (**LogoPixels)))
  {
    return EFI_INVALID_PARAMETER;
  }
  BufferSize = LogoWidth * LogoHeight * sizeof (**LogoPixels);
  *LogoPixels = AllocatePool (BufferSize);
  if (*LogoPixels == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  for (Y = 0; Y < LogoHeight; ++Y) {
    CopyMem (
      *LogoPixels + Y * LogoWidth,
      Screen + (LogoY + Y) * ScreenWidth + LogoX,
      LogoWidth * sizeof (**LogoPixels)
      );
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
PresentScreen (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixels,
  IN UINTN                          Width,
  IN UINTN                          Height
  )
{
  return Gop->Blt (
                Gop,
                Pixels,
                EfiBltBufferToVideo,
                0,
                0,
                0,
                0,
                Width,
                Height,
                Width * sizeof (*Pixels)
                );
}

STATIC
EFI_STATUS
SetBootLogo (
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixels,
  IN UINTN                          X,
  IN UINTN                          Y,
  IN UINTN                          Width,
  IN UINTN                          Height
  )
{
  EFI_STATUS                    Status;
  EDKII_BOOT_LOGO2_PROTOCOL     *BootLogo2;
  EFI_BOOT_LOGO_PROTOCOL        *BootLogo;

  Status = gBS->LocateProtocol (&gEdkiiBootLogo2ProtocolGuid, NULL, (VOID **)&BootLogo2);
  if (!EFI_ERROR (Status) && (BootLogo2 != NULL)) {
    Status = BootLogo2->SetBootLogo (BootLogo2, Pixels, X, Y, Width, Height);
    if (!EFI_ERROR (Status)) {
      mBootLogoInvalidated = FALSE;
      return Status;
    }
  }

  Status = gBS->LocateProtocol (&gEfiBootLogoProtocolGuid, NULL, (VOID **)&BootLogo);
  if (!EFI_ERROR (Status) && (BootLogo != NULL)) {
    Status = BootLogo->SetBootLogo (BootLogo, Pixels, X, Y, Width, Height);
    if (!EFI_ERROR (Status)) {
      mBootLogoInvalidated = FALSE;
    }
    return Status;
  }

  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
ShowAsset (
  IN CONST CHAR16  *RelativePath,
  IN BOOLEAN       CacheFirmwareSplash
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Screen;
  UINTN                         Width;
  UINTN                         Height;
  UINTN                         LogoX;
  UINTN                         LogoY;
  UINTN                         LogoWidth;
  UINTN                         LogoHeight;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *LogoPixels;
  WINX_BITMAP                   Bitmap;

  Status = LoadBitmap (RelativePath, &Bitmap);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = LocateGop (&Gop);
  if (EFI_ERROR (Status)) {
    FreePool (Bitmap.Pixels);
    return Status;
  }
  Status = AllocateScreenBuffer (Gop, &Screen, &Width, &Height);
  if (EFI_ERROR (Status)) {
    FreePool (Bitmap.Pixels);
    return Status;
  }

  RenderCenteredLogo (&Bitmap, Screen, Width, Height, &LogoX, &LogoY, &LogoWidth, &LogoHeight);
  FreePool (Bitmap.Pixels);
  Status = PresentScreen (Gop, Screen, Width, Height);
  if (EFI_ERROR (Status)) {
    FreePool (Screen);
    return Status;
  }

  Status = CopyScreenRegion (Screen, Width, LogoX, LogoY, LogoWidth, LogoHeight, &LogoPixels);
  if (EFI_ERROR (Status)) {
    FreePool (Screen);
    return Status;
  }

  if (CacheFirmwareSplash) {
    if (mFirmwareSplash != NULL) {
      FreePool (mFirmwareSplash);
    }
    mFirmwareSplash = AllocateCopyPool (LogoWidth * LogoHeight * sizeof (*LogoPixels), LogoPixels);
    if (mFirmwareSplash != NULL) {
      mFirmwareSplashWidth = LogoWidth;
      mFirmwareSplashHeight = LogoHeight;
      mFirmwareSplashX = LogoX;
      mFirmwareSplashY = LogoY;
    }
  }

  SetBootLogo (LogoPixels, LogoX, LogoY, LogoWidth, LogoHeight);
  FreePool (LogoPixels);
  FreePool (Screen);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ShowAssetSequence (
  IN CONST CHAR16  *FramePattern,
  IN CONST CHAR16  *StaticPath,
  IN BOOLEAN       CacheFirmwareSplash
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Screen;
  UINTN                         ScreenWidth;
  UINTN                         ScreenHeight;
  UINTN                         Frame;
  UINTN                         DisplayedFrames;
  UINTN                         LogoX;
  UINTN                         LogoY;
  UINTN                         LogoWidth;
  UINTN                         LogoHeight;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *LogoPixels;
  CHAR16                        Path[160];
  WINX_BITMAP                   Bitmap;

  Status = LocateGop (&Gop);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Screen = NULL;
  ScreenWidth = 0;
  ScreenHeight = 0;
  DisplayedFrames = 0;
  LogoPixels = NULL;
  for (Frame = 0; Frame < WINX_ANIMATION_MAX_FRAMES; ++Frame) {
    UnicodeSPrint (Path, sizeof (Path), FramePattern, Frame);
    Status = LoadBitmap (Path, &Bitmap);
    if (EFI_ERROR (Status)) {
      break;
    }
    if (Screen == NULL) {
      Status = AllocateScreenBuffer (Gop, &Screen, &ScreenWidth, &ScreenHeight);
      if (EFI_ERROR (Status)) {
        FreePool (Bitmap.Pixels);
        return Status;
      }
    }

    if (DisplayedFrames != 0) {
      gBS->Stall (WINX_ANIMATION_FRAME_DELAY);
    }
    RenderCenteredLogo (&Bitmap, Screen, ScreenWidth, ScreenHeight,
                        &LogoX, &LogoY, &LogoWidth, &LogoHeight);
    FreePool (Bitmap.Pixels);
    Status = PresentScreen (Gop, Screen, ScreenWidth, ScreenHeight);
    if (EFI_ERROR (Status)) {
      FreePool (Screen);
      return Status;
    }
    ++DisplayedFrames;
  }

  if (DisplayedFrames == 0) {
    if (Screen != NULL) {
      FreePool (Screen);
    }
    return ShowAsset (StaticPath, CacheFirmwareSplash);
  }

  Status = CopyScreenRegion (Screen, ScreenWidth, LogoX, LogoY,
                             LogoWidth, LogoHeight, &LogoPixels);
  if (EFI_ERROR (Status)) {
    FreePool (Screen);
    return Status;
  }

  if (CacheFirmwareSplash) {
    if (mFirmwareSplash != NULL) {
      FreePool (mFirmwareSplash);
    }
    mFirmwareSplash = AllocateCopyPool (LogoWidth * LogoHeight * sizeof (*LogoPixels), LogoPixels);
    if (mFirmwareSplash != NULL) {
      mFirmwareSplashWidth = LogoWidth;
      mFirmwareSplashHeight = LogoHeight;
      mFirmwareSplashX = LogoX;
      mFirmwareSplashY = LogoY;
    }
  }

  SetBootLogo (LogoPixels, LogoX, LogoY, LogoWidth, LogoHeight);
  FreePool (LogoPixels);
  FreePool (Screen);
  return EFI_SUCCESS;
}

EFI_STATUS
WinxVisualsShowFirmwareSplash (
  VOID
  )
{
  return ShowAssetSequence (
           L"uefi-%02u.bmp",
           L"uefi.bmp",
           TRUE
           );
}

EFI_STATUS
WinxVisualsDrawMenuBackground (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  WINX_BITMAP                   Bitmap;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Screen;
    UINTN                         Width;
    UINTN                         Height;

  if (mMenuBackground == NULL) {
    if (mMenuBackgroundChecked) {
      return EFI_NOT_FOUND;
    }
    mMenuBackgroundChecked = TRUE;
    Status = LoadBitmap (L"menu.bmp", &Bitmap);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = LocateGop (&Gop);
    if (EFI_ERROR (Status)) {
      FreePool (Bitmap.Pixels);
      return Status;
    }
    Status = AllocateScreenBuffer (Gop, &Screen, &Width, &Height);
    if (EFI_ERROR (Status)) {
      FreePool (Bitmap.Pixels);
      return Status;
    }
    RenderBitmap (&Bitmap, Screen, Width, Height, TRUE);
    FreePool (Bitmap.Pixels);
    mMenuBackground = Screen;
    mMenuBackgroundWidth = Width;
    mMenuBackgroundHeight = Height;
  }

  Status = LocateGop (&Gop);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((Gop->Mode->Info->HorizontalResolution != mMenuBackgroundWidth) ||
      (Gop->Mode->Info->VerticalResolution != mMenuBackgroundHeight))
  {
    return EFI_UNSUPPORTED;
  }
  return PresentScreen (Gop, mMenuBackground, mMenuBackgroundWidth, mMenuBackgroundHeight);
}

VOID
WinxVisualsInvalidateBootLogo (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_STATUS                    LegacyStatus;
  EDKII_BOOT_LOGO2_PROTOCOL     *BootLogo2;
  EFI_BOOT_LOGO_PROTOCOL        *BootLogo;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels;
  UINTN                         X;
  UINTN                         Y;
  UINTN                         Width;
  UINTN                         Height;

  if (mBootLogoInvalidated) {
    return;
  }

  Status = gBS->LocateProtocol (&gEdkiiBootLogo2ProtocolGuid, NULL, (VOID **)&BootLogo2);
  if (!EFI_ERROR (Status) && (BootLogo2 != NULL)) {
    Status = BootLogo2->GetBootLogo (BootLogo2, &Pixels, &X, &Y, &Width, &Height);
    if (!EFI_ERROR (Status) && (Pixels != NULL) && (Width != 0) && (Height != 0) &&
        (Width <= MAX_UINTN / Height / sizeof (*Pixels)))
    {
      if (mSavedBootLogo != NULL) {
        FreePool (mSavedBootLogo);
      }
      mSavedBootLogo = AllocateCopyPool (Width * Height * sizeof (*Pixels), Pixels);
      if (mSavedBootLogo != NULL) {
        mSavedBootLogoWidth = Width;
        mSavedBootLogoHeight = Height;
        mSavedBootLogoX = X;
        mSavedBootLogoY = Y;
      }
    }
    Status = BootLogo2->SetBootLogo (BootLogo2, NULL, 0, 0, 0, 0);
    if (!EFI_ERROR (Status)) {
      mBootLogoInvalidated = TRUE;
      return;
    }
  }

  LegacyStatus = gBS->LocateProtocol (&gEfiBootLogoProtocolGuid, NULL, (VOID **)&BootLogo);
  if (!EFI_ERROR (LegacyStatus) && (BootLogo != NULL)) {
    BootLogo->SetBootLogo (BootLogo, NULL, 0, 0, 0, 0);
  }
  mBootLogoInvalidated = TRUE;
}

STATIC
CHAR16
UpperAscii (
  IN CHAR16  Character
  )
{
  if ((Character >= L'a') && (Character <= L'z')) {
    return (CHAR16)(Character - (L'a' - L'A'));
  }
  return Character;
}

STATIC
BOOLEAN
ContainsAsciiInsensitive (
  IN CONST CHAR16  *Text,
  IN CONST CHAR16  *Needle
  )
{
  UINTN  TextIndex;
  UINTN  NeedleIndex;

  if ((Text == NULL) || (Needle == NULL) || (*Needle == L'\0')) {
    return FALSE;
  }
  for (TextIndex = 0; Text[TextIndex] != L'\0'; ++TextIndex) {
    for (NeedleIndex = 0; Needle[NeedleIndex] != L'\0'; ++NeedleIndex) {
      if ((Text[TextIndex + NeedleIndex] == L'\0') ||
          (UpperAscii (Text[TextIndex + NeedleIndex]) != UpperAscii (Needle[NeedleIndex])))
      {
        break;
      }
    }
    if (Needle[NeedleIndex] == L'\0') {
      return TRUE;
    }
  }
  return FALSE;
}

VOID
WinxVisualsShowBootSplash (
  IN CONST CHAR16  *Description
  )
{
  EFI_STATUS  Status;

  WinxVisualsInvalidateBootLogo ();
  if (ContainsAsciiInsensitive (Description, L"WINDOWS") ||
      ContainsAsciiInsensitive (Description, L"WINARM"))
  {
    Status = ShowAssetSequence (L"os\\windows-%02u.bmp", L"os\\windows.bmp", FALSE);
  } else if (ContainsAsciiInsensitive (Description, L"IPADOS") ||
             ContainsAsciiInsensitive (Description, L"IOS") ||
             ContainsAsciiInsensitive (Description, L"XNU"))
  {
    Status = ShowAssetSequence (L"os\\ipados-%02u.bmp", L"os\\ipados.bmp", FALSE);
  } else if (ContainsAsciiInsensitive (Description, L"LINUX") ||
             ContainsAsciiInsensitive (Description, L"GRUB") ||
             ContainsAsciiInsensitive (Description, L"L4T") ||
             ContainsAsciiInsensitive (Description, L"UBUNTU") ||
             ContainsAsciiInsensitive (Description, L"DEBIAN"))
  {
    Status = ShowAssetSequence (L"os\\linux-%02u.bmp", L"os\\linux.bmp", FALSE);
  } else {
    Status = EFI_NOT_FOUND;
  }

  if (EFI_ERROR (Status) && (mFirmwareSplash != NULL)) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

    if (!EFI_ERROR (LocateGop (&Gop)) && (Gop->Mode != NULL) && (Gop->Mode->Info != NULL) &&
        (mFirmwareSplashWidth <= Gop->Mode->Info->HorizontalResolution) &&
        (mFirmwareSplashHeight <= Gop->Mode->Info->VerticalResolution))
    {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;
      UINTN                          X;
      UINTN                          Y;

      Black.Red = 0;
      Black.Green = 0;
      Black.Blue = 0;
      Black.Reserved = 0;
      Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0,
                Gop->Mode->Info->HorizontalResolution,
                Gop->Mode->Info->VerticalResolution, 0);
      X = mFirmwareSplashX;
      Y = mFirmwareSplashY;
      if ((X + mFirmwareSplashWidth > Gop->Mode->Info->HorizontalResolution) ||
          (Y + mFirmwareSplashHeight > Gop->Mode->Info->VerticalResolution))
      {
        X = (Gop->Mode->Info->HorizontalResolution - mFirmwareSplashWidth) / 2;
        Y = (Gop->Mode->Info->VerticalResolution - mFirmwareSplashHeight) / 2;
      }
      Gop->Blt (Gop, mFirmwareSplash, EfiBltBufferToVideo, 0, 0, X, Y,
                mFirmwareSplashWidth, mFirmwareSplashHeight,
                mFirmwareSplashWidth * sizeof (*mFirmwareSplash));
      SetBootLogo (mFirmwareSplash, X, Y, mFirmwareSplashWidth, mFirmwareSplashHeight);
      return;
    }
  }

  if (EFI_ERROR (Status) && (mSavedBootLogo != NULL)) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black;
    UINTN                         X;
    UINTN                         Y;

    if (!EFI_ERROR (LocateGop (&Gop)) && (Gop->Mode != NULL) && (Gop->Mode->Info != NULL) &&
        (mSavedBootLogoWidth <= Gop->Mode->Info->HorizontalResolution) &&
        (mSavedBootLogoHeight <= Gop->Mode->Info->VerticalResolution))
    {
      Black.Red = 0;
      Black.Green = 0;
      Black.Blue = 0;
      Black.Reserved = 0;
      Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0,
                Gop->Mode->Info->HorizontalResolution,
                Gop->Mode->Info->VerticalResolution, 0);
      X = mSavedBootLogoX;
      Y = mSavedBootLogoY;
      if ((X + mSavedBootLogoWidth > Gop->Mode->Info->HorizontalResolution) ||
          (Y + mSavedBootLogoHeight > Gop->Mode->Info->VerticalResolution))
      {
        X = (Gop->Mode->Info->HorizontalResolution - mSavedBootLogoWidth) / 2;
        Y = (Gop->Mode->Info->VerticalResolution - mSavedBootLogoHeight) / 2;
      }
      Gop->Blt (Gop, mSavedBootLogo, EfiBltBufferToVideo, 0, 0,
                X, Y, mSavedBootLogoWidth,
                mSavedBootLogoHeight, mSavedBootLogoWidth * sizeof (*mSavedBootLogo));
      SetBootLogo (mSavedBootLogo, X, Y,
                   mSavedBootLogoWidth, mSavedBootLogoHeight);
    }
  }
}

VOID
WinxVisualsPrepareShell (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black;

  WinxVisualsInvalidateBootLogo ();
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGRAY | EFI_BRIGHT | EFI_BACKGROUND_BLACK);
    gST->ConOut->ClearScreen (gST->ConOut);
  }
  if (!EFI_ERROR (LocateGop (&Gop))) {
    Black.Red = 0;
    Black.Green = 0;
    Black.Blue = 0;
    Black.Reserved = 0;
    Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0,
              Gop->Mode->Info->HorizontalResolution,
              Gop->Mode->Info->VerticalResolution, 0);
  }
}
