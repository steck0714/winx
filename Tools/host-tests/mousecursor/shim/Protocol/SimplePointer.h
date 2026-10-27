#ifndef Gshim_Protocol_SimplePointer_h_
#define Gshim_Protocol_SimplePointer_h_
typedef struct { INT32 RelativeMovementX, RelativeMovementY, RelativeMovementZ; BOOLEAN LeftButton, RightButton; } EFI_SIMPLE_POINTER_STATE;
typedef struct { UINT64 ResolutionX, ResolutionY, ResolutionZ; BOOLEAN LeftButton, RightButton; } EFI_SIMPLE_POINTER_MODE;
typedef struct _SP EFI_SIMPLE_POINTER_PROTOCOL;
struct _SP { void *Reset; EFI_STATUS (*GetState)(EFI_SIMPLE_POINTER_PROTOCOL*, EFI_SIMPLE_POINTER_STATE*); void *W; EFI_SIMPLE_POINTER_MODE *Mode; };
#endif
