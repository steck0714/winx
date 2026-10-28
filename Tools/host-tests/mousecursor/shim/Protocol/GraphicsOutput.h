#ifndef Gshim_Protocol_GraphicsOutput_h_
#define Gshim_Protocol_GraphicsOutput_h_
typedef struct { UINT8 Blue, Green, Red, Reserved; } EFI_GRAPHICS_OUTPUT_BLT_PIXEL;
typedef enum { EfiBltVideoFill, EfiBltVideoToBltBuffer, EfiBltBufferToVideo, EfiBltVideoToVideo } EFI_GRAPHICS_OUTPUT_BLT_OPERATION;
typedef struct { UINT32 HorizontalResolution, VerticalResolution; } EFI_GOP_INFO;
typedef struct { EFI_GOP_INFO *Info; } EFI_GOP_MODE;
typedef struct _GOP EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct _GOP {
  EFI_STATUS (*Blt)(EFI_GRAPHICS_OUTPUT_PROTOCOL*, EFI_GRAPHICS_OUTPUT_BLT_PIXEL*, EFI_GRAPHICS_OUTPUT_BLT_OPERATION, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN);
  EFI_GOP_MODE *Mode;
};
#endif
