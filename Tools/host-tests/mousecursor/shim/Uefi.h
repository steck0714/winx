#ifndef Gshim_Uefi_h_
#define Gshim_Uefi_h_
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef int32_t INT32; typedef int64_t INT64; typedef uintptr_t UINTN; typedef char CHAR8; typedef int BOOLEAN; typedef void VOID;
typedef UINTN EFI_STATUS; typedef void *EFI_HANDLE; typedef void *EFI_EVENT; typedef UINTN EFI_TPL;
#define TRUE 1
#define FALSE 0
#define IN
#define OUT
#define OPTIONAL
#define CONST const
#define STATIC static
#define EFIAPI
#define EFI_SUCCESS 0
#define EFI_NOT_READY 6
#define EFI_INVALID_PARAMETER 2
#define EFI_ERROR(s) ((s) != 0)
typedef struct { UINT32 d1; UINT16 d2, d3; UINT8 d4[8]; } EFI_GUID;
typedef struct { void *x; } EFI_SYSTEM_TABLE;
extern EFI_GUID gEfiSimplePointerProtocolGuid, gEfiGraphicsOutputProtocolGuid;
#define EVT_TIMER 1
#define EVT_NOTIFY_SIGNAL 2
#define TPL_CALLBACK 8
#define TPL_NOTIFY 16
typedef enum { TimerPeriodic } EFI_TIMER_DELAY;
typedef enum { ByProtocol } EFI_LOCATE_SEARCH_TYPE;
#endif
