#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
typedef uint8_t UINT8; typedef uint32_t UINT32; typedef uint64_t UINT64; typedef uintptr_t UINTN; typedef char CHAR8; typedef int BOOLEAN; typedef void VOID;
#define TRUE 1
#define FALSE 0
#define IN
#define OUT
#define CONST const
#define EFIAPI
#define VA_LIST va_list
#define VA_START va_start
#define VA_END va_end
#define FixedPcdGet32(x) x
#define PcdMipiFrameBufferAddress (g_fb_addr)
#define PcdMipiFrameBufferWidth 768
#define PcdMipiFrameBufferHeight 1280
#define PcdMipiFrameBufferVisibleWidth 720
extern UINTN g_fb_addr;
#define STATIC static
