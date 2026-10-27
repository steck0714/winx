#ifndef __TOUCH_PANEL_H__
#define __TOUCH_PANEL_H__

#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Library/DevicePathLib.h>

// Vendor hardware device path used to expose the FTS4 touch panel (ConIn / AbsolutePointer).
#define PLAT_TOUCH_DEVICE_GUID \
    { 0x2abf720d, 0x6e2a, 0x4caa, { 0x80, 0x96, 0x31, 0x7e, 0xf8, 0x27, 0x5b, 0x70 } }

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH  Device;
  EFI_DEVICE_PATH     End;
} TOUCH_DEVICE_PATH;
#pragma pack()

#define TOUCH_DEVICE_PATH_INIT                                                       \
  {                                                                                  \
    { { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,                                          \
        { (UINT8)sizeof (VENDOR_DEVICE_PATH),                                        \
          (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },                             \
      PLAT_TOUCH_DEVICE_GUID },                                                      \
    { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,                          \
      { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }                                     \
  }

#endif
