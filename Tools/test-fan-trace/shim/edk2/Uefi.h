#pragma once
#include <cstdint>
#include <cstdio>
typedef uint8_t  UINT8;  typedef uint16_t UINT16; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef int8_t   INT8;   typedef int16_t  INT16;  typedef int32_t  INT32;  typedef int64_t  INT64;
typedef uint64_t UINTN;  typedef int64_t  INTN;   typedef bool BOOLEAN;    typedef void VOID;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef UINTN    EFI_STATUS;
typedef void    *EFI_HANDLE;
#define IN
#define OUT
#define OPTIONAL
#define CONST const
#define STATIC static
#define EFIAPI
#define TRUE  true
#define FALSE false
#define EFI_SUCCESS     ((EFI_STATUS) 0)
#define EFI_UNSUPPORTED ((EFI_STATUS) 3)
struct EFI_SYSTEM_TABLE { int dummy; };
struct EFI_BOOT_SERVICES { UINTN Stall (UINTN) { return 0; } };
