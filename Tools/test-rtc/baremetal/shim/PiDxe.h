#pragma once
/* Just enough of EDK2 for Library/VirtualRealTimeClockLib/VirtualRealTimeClockLib.c to compile on a PC. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t  UINT8;  typedef uint16_t UINT16; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef int8_t   INT8;   typedef int16_t  INT16;  typedef int32_t  INT32;  typedef int64_t  INT64;
typedef uint64_t UINTN;  typedef char CHAR8;      typedef bool BOOLEAN;    typedef void VOID;
typedef UINTN EFI_STATUS; typedef void *EFI_HANDLE; typedef void *EFI_EVENT;
#define IN
#define OUT
#define OPTIONAL
#define CONST const
#define STATIC static
#define EFIAPI
#define TRUE  true
#define FALSE false
#define EFI_SUCCESS           ((EFI_STATUS) 0)
#define EFI_INVALID_PARAMETER ((EFI_STATUS) 2)
#define EFI_UNSUPPORTED       ((EFI_STATUS) 3)
#define EFI_DEVICE_ERROR      ((EFI_STATUS) 7)
typedef struct { UINT16 Year; UINT8 Month; UINT8 Day; UINT8 Hour; UINT8 Minute; UINT8 Second; UINT8 Pad1;
                 UINT32 Nanosecond; int16_t TimeZone; UINT8 Daylight; UINT8 Pad2; } EFI_TIME;
typedef struct { UINT32 Resolution; UINT32 Accuracy; BOOLEAN SetsToZero; } EFI_TIME_CAPABILITIES;
typedef struct { int dummy; } EFI_SYSTEM_TABLE;
