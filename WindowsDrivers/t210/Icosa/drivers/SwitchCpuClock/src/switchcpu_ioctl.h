/*
 * switchcpu.sys user interface (\\.\SwitchCpu). Shared by the driver, the
 * test script (CpuClock.ps1) and the tray app; all fields are ULONG/LONG so
 * 32-bit (x86-emulated) callers see the same layout.
 */
#pragma once

#define SWCPU_DEVICE_NAME      L"\\Device\\SwitchCpu"
#define SWCPU_SYMLINK_NAME     L"\\DosDevices\\SwitchCpu"

#define IOCTL_SWCPU_GET_STATUS   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)    /* 0x00226000 */
#define IOCTL_SWCPU_GET_PROFILES CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)    /* 0x00226004 */
#define IOCTL_SWCPU_SET_PROFILE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)   /* 0x0022A008 */

#define SWCPU_MAX_PROFILES       8
#define SWCPU_PROFILE_EXPERIMENTAL 0x1
#define SWCPU_PROFILE_DEFAULT      0x2      /* what UEFI R365+ boots with */

typedef struct _SWCPU_STATUS {
    ULONG Version;          /* 1 */
    ULONG CurrentKhz;       /* from PLLX_BASE */
    ULONG CurrentMv;        /* from MAX77621 VOUT */
    LONG ActiveProfile;     /* -1 if PLLX matches no profile */
    LONG RequestedProfile;  /* last SET_PROFILE (or the default) */
    ULONG Throttled;        /* 1 while the thermal guard holds the default profile */
    ULONG TempSoC;          /* C, TMP451 remote */
    ULONG TempBoard;        /* C, TMP451 local */
    ULONG TempValid;        /* 0 if the sensor could not be read */
    ULONG ProfileCount;
    ULONG Switches;         /* successful clock changes since start */
    ULONG LastError;        /* NTSTATUS of the last failed change, 0 if none */
} SWCPU_STATUS;

typedef struct _SWCPU_PROFILE {
    ULONG Khz;
    ULONG Mv;
    ULONG Flags;
} SWCPU_PROFILE;

typedef struct _SWCPU_PROFILES {
    ULONG Count;
    SWCPU_PROFILE Profiles[SWCPU_MAX_PROFILES];
} SWCPU_PROFILES;

typedef struct _SWCPU_SET_PROFILE_INPUT {
    ULONG Profile;
} SWCPU_SET_PROFILE_INPUT;
