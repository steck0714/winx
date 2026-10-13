/* SimpleFbDxe: Simple FrameBuffer */
#include <PiDxe.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/ShellParameters.h>

/// Defines
/*
 * Convert enum video_log2_bpp to bytes and bits. Note we omit the outer
 * brackets to allow multiplication by fractional pixels.
 */
#define VNBYTES(bpix)	(1 << (bpix)) / 8
#define VNBITS(bpix)	(1 << (bpix))

#define POS_TO_FB(posX, posY) ((UINT8 *)                                \
                               ((UINTN)This->Mode->FrameBufferBase +    \
                                (posY) * This->Mode->Info->PixelsPerScanLine * \
                                FB_BYTES_PER_PIXEL +                   \
                                (posX) * FB_BYTES_PER_PIXEL))

#define FB_BITS_PER_PIXEL                   (32)
#define FB_BYTES_PER_PIXEL                  (FB_BITS_PER_PIXEL / 8)
#define DISPLAYDXE_PHYSICALADDRESS32(_x_)   (UINTN)((_x_) & 0xFFFFFFFF)

#define DISPLAYDXE_RED_MASK                0xFF0000
#define DISPLAYDXE_GREEN_MASK              0x00FF00
#define DISPLAYDXE_BLUE_MASK               0x0000FF
#define DISPLAYDXE_ALPHA_MASK              0x000000

/*
 * Bits per pixel selector. Each value n is such that the bits-per-pixel is
 * 2 ^ n
 */
enum video_log2_bpp {
	VIDEO_BPP1	= 0,
	VIDEO_BPP2,
	VIDEO_BPP4,
	VIDEO_BPP8,
	VIDEO_BPP16,
	VIDEO_BPP32,
};

typedef struct {
  VENDOR_DEVICE_PATH DisplayDevicePath;
  EFI_DEVICE_PATH EndDevicePath;
} DISPLAY_DEVICE_PATH;

DISPLAY_DEVICE_PATH mDisplayDevicePath =
{
    {
      {
        HARDWARE_DEVICE_PATH,
        HW_VENDOR_DP,
        {
          (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
          (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8),
        }
      },
      EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID
    },
    {
      END_DEVICE_PATH_TYPE,
      END_ENTIRE_DEVICE_PATH_SUBTYPE,
      {
        sizeof(EFI_DEVICE_PATH_PROTOCOL),
        0
      }
    }
};

/*
 * Optional 90 degree rotation (build with -D SWITCH_FB_ROTATE=1 or 2).
 *
 * The Switch panel is a 720x1280 portrait panel that is held in landscape. With rotation enabled the
 * driver exposes two modes:
 *   mode 0: 1280x720 landscape, PixelBltOnly. Blt() rotates into the panel framebuffer. Used by the
 *           UEFI console / setup menus.
 *   mode 1: 720x1280 native, linear framebuffer. What an OS loader needs.
 * The driver starts in mode 0 and switches to mode 1 when an OS loader is started (StartImage hook, see
 * ImageNeedsNativeFramebuffer), so Windows sees the same linear framebuffer as without rotation while the
 * Shell and the setup menus stay landscape.
 *
 *   SWITCH_FB_ROTATE=1: landscape (Lx,Ly) -> panel (x = W-1-Ly, y = Lx)
 *   SWITCH_FB_ROTATE=2: the same rotated by another 180 degrees (use it if 1 comes out upside down)
 */
#if defined(SWITCH_FB_ROTATE)
#define SWITCH_FB_HAS_ROTATION 1
#endif

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE      mDisplayMode;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION   mModeInfo[2];
STATIC UINT32                                 mNativeWidth;
STATIC UINT32                                 mNativeHeight;
STATIC BOOLEAN                                mRotatedActive;

#define MODE_ROTATED  0
#define MODE_NATIVE   1

/// Declares

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode
(
    IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
    IN  UINT32                                ModeNumber,
    OUT UINTN                                 *SizeOfInfo,
    OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
);

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode
(
    IN  EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN  UINT32                       ModeNumber
);

STATIC
EFI_STATUS
EFIAPI
DisplayBlt
(
    IN  EFI_GRAPHICS_OUTPUT_PROTOCOL            *This,
    IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL           *BltBuffer,   OPTIONAL
    IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION       BltOperation,
    IN  UINTN                                   SourceX,
    IN  UINTN                                   SourceY,
    IN  UINTN                                   DestinationX,
    IN  UINTN                                   DestinationY,
    IN  UINTN                                   Width,
    IN  UINTN                                   Height,
    IN  UINTN                                   Delta         OPTIONAL
);

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mDisplay = {
  DisplayQueryMode,
  DisplaySetMode,
  DisplayBlt,
  NULL
};

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode
(
    IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
    IN  UINT32                                ModeNumber,
    OUT UINTN                                 *SizeOfInfo,
    OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
)
{
    EFI_STATUS Status;

    if (ModeNumber >= This->Mode->MaxMode || Info == NULL || SizeOfInfo == NULL)
    {
        return EFI_INVALID_PARAMETER;
    }

    Status = gBS->AllocatePool(
        EfiBootServicesData,
        sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
        (VOID **) Info);

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status)) return Status;

    *SizeOfInfo = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
    CopyMem(*Info, &mModeInfo[ModeNumber], sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));

    return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode
(
    IN  EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN  UINT32                       ModeNumber
)
{
  if (ModeNumber >= This->Mode->MaxMode)
  {
    return EFI_UNSUPPORTED;
  }

  This->Mode->Mode = ModeNumber;
  This->Mode->Info = &mModeInfo[ModeNumber];
  mRotatedActive = (BOOLEAN)(This->Mode->MaxMode > 1 && ModeNumber == MODE_ROTATED);

  // The specification asks SetMode() to clear the screen.
  SetMem32(
    (VOID *)(UINTN)This->Mode->FrameBufferBase,
    (UINTN)This->Mode->FrameBufferSize,
    0
  );

  return EFI_SUCCESS;
}

/* Map a landscape pixel to its location in the panel framebuffer. */
STATIC
inline
UINT32 *
RotatedPixel
(
    IN UINTN Lx,
    IN UINTN Ly
)
{
    UINTN X, Y;

#if defined(SWITCH_FB_ROTATE) && (SWITCH_FB_ROTATE == 2)
    X = Ly;
    Y = (mNativeHeight - 1) - Lx;
#else
    X = (mNativeWidth - 1) - Ly;
    Y = Lx;
#endif

    return (UINT32 *)(UINTN)mDisplayMode.FrameBufferBase + Y * mNativeWidth + X;
}

STATIC
EFI_STATUS
DisplayBltRotated
(
	IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *BltBuffer, OPTIONAL
	IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
	IN  UINTN                             SourceX,
	IN  UINTN                             SourceY,
	IN  UINTN                             DestinationX,
	IN  UINTN                             DestinationY,
	IN  UINTN                             Width,
	IN  UINTN                             Height,
	IN  UINTN                             Delta         OPTIONAL
)
{
	// Landscape surface size is the native size with the axes swapped.
	CONST UINTN  LW = mNativeHeight;
	CONST UINTN  LH = mNativeWidth;
	UINTN        Lx, Ly;
	UINT32       *Src;
	UINT32       *Tmp;
	EFI_STATUS   Status;

	if (Width == 0 || Height == 0) return EFI_SUCCESS;

	if (Delta == 0)
	{
		Delta = Width * FB_BYTES_PER_PIXEL;
	}

	switch (BltOperation) {
	case EfiBltVideoFill:
		if (BltBuffer == NULL || DestinationX + Width > LW || DestinationY + Height > LH)
			return EFI_INVALID_PARAMETER;

		for (Lx = DestinationX; Lx < DestinationX + Width; Lx++)
			for (Ly = DestinationY; Ly < DestinationY + Height; Ly++)
				*RotatedPixel(Lx, Ly) = *(UINT32 *)BltBuffer;
		break;

	case EfiBltVideoToBltBuffer:
		if (BltBuffer == NULL || SourceX + Width > LW || SourceY + Height > LH)
			return EFI_INVALID_PARAMETER;

		for (Ly = 0; Ly < Height; Ly++)
		{
			UINT32 *Dst = (UINT32 *)((UINT8 *)BltBuffer + (DestinationY + Ly) * Delta) + DestinationX;
			for (Lx = 0; Lx < Width; Lx++)
				Dst[Lx] = *RotatedPixel(SourceX + Lx, SourceY + Ly);
		}
		break;

	case EfiBltBufferToVideo:
		if (BltBuffer == NULL || DestinationX + Width > LW || DestinationY + Height > LH)
			return EFI_INVALID_PARAMETER;

		for (Ly = 0; Ly < Height; Ly++)
		{
			Src = (UINT32 *)((UINT8 *)BltBuffer + (SourceY + Ly) * Delta) + SourceX;
			for (Lx = 0; Lx < Width; Lx++)
				*RotatedPixel(DestinationX + Lx, DestinationY + Ly) = Src[Lx];
		}
		break;

	case EfiBltVideoToVideo:
		if (SourceX + Width > LW || SourceY + Height > LH ||
		    DestinationX + Width > LW || DestinationY + Height > LH)
			return EFI_INVALID_PARAMETER;

		Status = gBS->AllocatePool(EfiBootServicesData, Width * Height * FB_BYTES_PER_PIXEL, (VOID **) &Tmp);
		if (EFI_ERROR(Status)) return Status;

		for (Ly = 0; Ly < Height; Ly++)
			for (Lx = 0; Lx < Width; Lx++)
				Tmp[Ly * Width + Lx] = *RotatedPixel(SourceX + Lx, SourceY + Ly);

		for (Ly = 0; Ly < Height; Ly++)
			for (Lx = 0; Lx < Width; Lx++)
				*RotatedPixel(DestinationX + Lx, DestinationY + Ly) = Tmp[Ly * Width + Lx];

		gBS->FreePool(Tmp);
		break;

	default:
		return EFI_INVALID_PARAMETER;
	}

	return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplayBlt(
	IN  EFI_GRAPHICS_OUTPUT_PROTOCOL      *This,
	IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *BltBuffer, OPTIONAL
	IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
	IN  UINTN                             SourceX,
	IN  UINTN                             SourceY,
	IN  UINTN                             DestinationX,
	IN  UINTN                             DestinationY,
	IN  UINTN                             Width,
	IN  UINTN                             Height,
	IN  UINTN                             Delta         OPTIONAL
)
{
	UINT8 *VidBuf, *BltBuf, *VidBuf1;
	UINTN i;

	if (mRotatedActive)
	{
		return DisplayBltRotated(BltBuffer, BltOperation, SourceX, SourceY,
			DestinationX, DestinationY, Width, Height, Delta);
	}

	switch (BltOperation) {
	case EfiBltVideoFill:
		// (upstream read the fill colour from the framebuffer's first pixel instead of BltBuffer)
		BltBuf = (UINT8 *) BltBuffer;

		for (i = 0; i < Height; i++) 
		{
			VidBuf = POS_TO_FB(DestinationX, DestinationY + i);
			
			SetMem32(VidBuf, Width * FB_BYTES_PER_PIXEL, *(UINT32 *) BltBuf);
		}
		break;

	case EfiBltVideoToBltBuffer:
		if (Delta == 0) 
		{
			Delta = Width * FB_BYTES_PER_PIXEL;
		}

		// A R G B
		// A B G R
		for (i = 0; i < Height; i++) 
		{
			VidBuf = POS_TO_FB(SourceX, SourceY + i);

			BltBuf = (UINT8 *)((UINTN)BltBuffer + (DestinationY + i) * Delta +
				DestinationX * FB_BYTES_PER_PIXEL);

			gBS->CopyMem((VOID *)BltBuf, (VOID *)VidBuf, FB_BYTES_PER_PIXEL * Width);
		}
		break;

	case EfiBltBufferToVideo:
		if (Delta == 0) 
		{
			Delta = Width * FB_BYTES_PER_PIXEL;
		}

		for (i = 0; i < Height; i++) 
		{
			VidBuf = POS_TO_FB(DestinationX, DestinationY + i);
			BltBuf = (UINT8 *)((UINTN)BltBuffer + (SourceY + i) * Delta + SourceX * FB_BYTES_PER_PIXEL);

			gBS->CopyMem((VOID *)VidBuf, (VOID *)BltBuf, Width * FB_BYTES_PER_PIXEL);
		}
		break;

	case EfiBltVideoToVideo:
		for (i = 0; i < Height; i++) 
		{
			VidBuf = POS_TO_FB(SourceX, SourceY + i);
			VidBuf1 = POS_TO_FB(DestinationX, DestinationY + i);

			gBS->CopyMem((VOID *)VidBuf1, (VOID *)VidBuf, Width * FB_BYTES_PER_PIXEL);
		}
		break;

	default:
		ASSERT_EFI_ERROR(EFI_SUCCESS);
		break;
	}

	return EFI_SUCCESS;
}

#if defined(SWITCH_FB_HAS_ROTATION)
/*
 * Who gets the panel-native linear framebuffer?
 *
 * BDS signals ReadyToBoot for every boot option, the built-in Shell included, so that event cannot tell
 * an OS loader from the Shell. Switching the mode there leaves the console drawing 1280x720 coordinates
 * into a 720x1280 framebuffer. Instead StartImage() is hooked and the native mode is selected only for
 * an image that really is an OS loader:
 *   - images from a firmware volume (Shell, setup, ...)                       -> stay landscape
 *   - bootmgfw.efi / bootaa64.efi / bootmgr.efi / winload.efi, wherever
 *     they were started from (also from the Shell)                            -> native
 *   - any other image started from within the Shell (it has the
 *     EFI_SHELL_PARAMETERS_PROTOCOL installed on its handle)                  -> stay landscape
 *   - any other image from a file system (boot option, e.g. a boot loader)    -> native
 * If the image returns, the landscape mode is restored.
 */
STATIC EFI_IMAGE_START  mOriginalStartImage;

STATIC
BOOLEAN
NameEqualsNoCase
(
    IN CONST CHAR16  *A,
    IN CONST CHAR16  *B
)
{
    while (*A != 0 && *B != 0)
    {
        CHAR16 CA = (*A >= L'A' && *A <= L'Z') ? (CHAR16)(*A + 32) : *A;
        CHAR16 CB = (*B >= L'A' && *B <= L'Z') ? (CHAR16)(*B + 32) : *B;
        if (CA != CB) return FALSE;
        A++; B++;
    }
    return (BOOLEAN)(*A == 0 && *B == 0);
}

STATIC
BOOLEAN
ImageIsUefiShell
(
    IN EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage
)
{
    // A UEFI Shell copied to a file system (EFI\BOOT\BOOTAA64.EFI) has the same name as a boot loader.
    // Recognise it by its prompt string (UTF-16 "Shell>"); an OS loader never contains it.
    STATIC CONST UINT8  Pattern[] = { 'S',0,'h',0,'e',0,'l',0,'l',0,'>',0 };
    CONST UINT8         *Base = (CONST UINT8 *) LoadedImage->ImageBase;
    UINTN               Size = (UINTN) LoadedImage->ImageSize;
    UINTN               I, J;

    if (Base == NULL || Size < sizeof(Pattern)) return FALSE;
    for (I = 0; I + sizeof(Pattern) <= Size; I += 2)
    {
        if (Base[I] != 'S') continue;
        for (J = 1; J < sizeof(Pattern) && Base[I + J] == Pattern[J]; J++) { }
        if (J == sizeof(Pattern)) return TRUE;
    }
    return FALSE;
}

STATIC
BOOLEAN
ImageNeedsNativeFramebuffer
(
    IN EFI_HANDLE  ImageHandle
)
{
    EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
    EFI_DEVICE_PATH_PROTOCOL   *Node;
    EFI_DEVICE_PATH_PROTOCOL   *LastNode;
    VOID                       *ShellParameters;
    BOOLEAN                    InFirmwareVolume;
    CONST CHAR16               *FileName;
    CONST CHAR16               *P;
    STATIC CONST CHAR16        *LoaderNames[] = {
        L"bootmgfw.efi", L"bootaa64.efi", L"bootmgr.efi", L"winload.efi"
    };
    UINTN                      Index;

    if (EFI_ERROR(gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **) &LoadedImage)) ||
        LoadedImage->FilePath == NULL)
    {
        return FALSE;
    }

    InFirmwareVolume = FALSE;
    FileName = NULL;
    LastNode = NULL;
    for (Node = LoadedImage->FilePath; !IsDevicePathEnd(Node); Node = NextDevicePathNode(Node))
    {
        LastNode = Node;
        if (DevicePathType(Node) == MEDIA_DEVICE_PATH &&
            (DevicePathSubType(Node) == MEDIA_PIWG_FW_VOL_DP || DevicePathSubType(Node) == MEDIA_PIWG_FW_FILE_DP))
        {
            InFirmwareVolume = TRUE;
        }
    }

    // Shell, setup and the other firmware applications keep the landscape console.
    if (InFirmwareVolume) return FALSE;

    // The UEFI Shell started from a file system (BOOTAA64.EFI on the SD card) stays landscape too.
    if (ImageIsUefiShell(LoadedImage)) return FALSE;

    if (LastNode != NULL && DevicePathType(LastNode) == MEDIA_DEVICE_PATH &&
        DevicePathSubType(LastNode) == MEDIA_FILEPATH_DP)
    {
        FileName = ((FILEPATH_DEVICE_PATH *) LastNode)->PathName;
        for (P = FileName; *P != 0; P++)
        {
            if (*P == L'\\' || *P == L'/') FileName = P + 1;
        }

        for (Index = 0; Index < ARRAY_SIZE(LoaderNames); Index++)
        {
            if (NameEqualsNoCase(FileName, LoaderNames[Index])) return TRUE;
        }
    }

    // Some other tool run by the user from the Shell.
    if (!EFI_ERROR(gBS->HandleProtocol(ImageHandle, &gEfiShellParametersProtocolGuid, &ShellParameters)))
    {
        return FALSE;
    }

    return TRUE;
}

STATIC
EFI_STATUS
EFIAPI
HookedStartImage
(
    IN  EFI_HANDLE  ImageHandle,
    OUT UINTN       *ExitDataSize,
    OUT CHAR16      **ExitData    OPTIONAL
)
{
    EFI_STATUS  Status;
    BOOLEAN     Switched = FALSE;

    if (mRotatedActive && ImageNeedsNativeFramebuffer(ImageHandle))
    {
        DisplaySetMode(&mDisplay, MODE_NATIVE);
        Switched = TRUE;
    }

    Status = mOriginalStartImage(ImageHandle, ExitDataSize, ExitData);

    // The loader came back (failed or exited): give the UEFI console its landscape mode again.
    if (Switched)
    {
        DisplaySetMode(&mDisplay, MODE_ROTATED);
    }

    return Status;
}
#endif

EFI_STATUS
EFIAPI
SimpleFbDxeInitialize
(
    IN EFI_HANDLE         ImageHandle,
    IN EFI_SYSTEM_TABLE   *SystemTable
)
{

    EFI_STATUS          Status                  = EFI_SUCCESS;
    EFI_HANDLE          hUEFIDisplayHandle      = NULL;

    /* Retrieve simple frame buffer from pre-SEC bootloader */
    DEBUG((EFI_D_ERROR, "SimpleFbDxe: Retrieve MIPI FrameBuffer parameters from PCD\n"));
    UINT32              MipiFrameBufferAddr     = FixedPcdGet32(PcdMipiFrameBufferAddress);
    UINT32              MipiFrameBufferWidth    = FixedPcdGet32(PcdMipiFrameBufferWidth);
    UINT32              MipiFrameBufferHeight   = FixedPcdGet32(PcdMipiFrameBufferHeight);

    /* Sanity check */
    if (MipiFrameBufferAddr == 0 || MipiFrameBufferWidth == 0 || MipiFrameBufferHeight == 0)
    {
        DEBUG((EFI_D_ERROR, "SimpleFbDxe: Invalid FrameBuffer parameters\n"));
        return EFI_DEVICE_ERROR;
    }

    mNativeWidth  = MipiFrameBufferWidth;
    mNativeHeight = MipiFrameBufferHeight;

    /* SimpleFB runs on a8r8g8b8 (VIDEO_BPP32) for this device */
    UINT32 LineLength = MipiFrameBufferWidth * VNBYTES(VIDEO_BPP32);
    UINT32 FrameBufferSize = LineLength * MipiFrameBufferHeight;
    EFI_PHYSICAL_ADDRESS FrameBufferAddress = MipiFrameBufferAddr;

    /* Native (panel) mode */
    UINT32 NativeIndex = 0;
#if defined(SWITCH_FB_HAS_ROTATION)
    /* mode 0 = rotated landscape (Blt only), mode 1 = native */
    NativeIndex = MODE_NATIVE;
    mModeInfo[MODE_ROTATED].Version = 0;
    mModeInfo[MODE_ROTATED].HorizontalResolution = MipiFrameBufferHeight;
    mModeInfo[MODE_ROTATED].VerticalResolution = MipiFrameBufferWidth;
    mModeInfo[MODE_ROTATED].PixelFormat = PixelBltOnly;
    mModeInfo[MODE_ROTATED].PixelsPerScanLine = MipiFrameBufferHeight;
#endif
    mModeInfo[NativeIndex].Version = 0;
    mModeInfo[NativeIndex].HorizontalResolution = MipiFrameBufferWidth;
    mModeInfo[NativeIndex].VerticalResolution = MipiFrameBufferHeight;
    mModeInfo[NativeIndex].PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
    mModeInfo[NativeIndex].PixelsPerScanLine = MipiFrameBufferWidth;

    mDisplayMode.SizeOfInfo = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
    mDisplayMode.FrameBufferBase = FrameBufferAddress;
    mDisplayMode.FrameBufferSize = FrameBufferSize;
    mDisplay.Mode = &mDisplayMode;

#if defined(SWITCH_FB_HAS_ROTATION)
    mDisplayMode.MaxMode = 2;
    mDisplayMode.Mode = MODE_ROTATED;
    mDisplayMode.Info = &mModeInfo[MODE_ROTATED];
    mRotatedActive = TRUE;
#else
    mDisplayMode.MaxMode = 1;
    mDisplayMode.Mode = 0;
    mDisplayMode.Info = &mModeInfo[0];
    mRotatedActive = FALSE;
#endif

    /* Register handle */
    Status = gBS->InstallMultipleProtocolInterfaces(
        &hUEFIDisplayHandle,
        &gEfiDevicePathProtocolGuid,
        &mDisplayDevicePath,
        &gEfiGraphicsOutputProtocolGuid,
        &mDisplay,
        NULL);

    ASSERT_EFI_ERROR (Status);

#if defined(SWITCH_FB_HAS_ROTATION)
    if (!EFI_ERROR(Status))
    {
        // Select the native framebuffer only when an OS loader is started (see HookedStartImage).
        mOriginalStartImage = gBS->StartImage;
        gBS->StartImage = HookedStartImage;
        gBS->Hdr.CRC32 = 0;
        gBS->CalculateCrc32(gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);

        // Start from a cleared screen in the rotated mode.
        DisplaySetMode(&mDisplay, MODE_ROTATED);
    }
#endif

    return Status;
}
