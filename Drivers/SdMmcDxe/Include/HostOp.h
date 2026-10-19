#ifndef __HOSTOP_H__
#define __HOSTOP_H__

#include <Uefi.h>
EFIAPI
int
SdFxInit(
    VOID
);

EFIAPI
int
SdFxInitFinalize
(
	VOID
);

ulong mmc_bread(UINT64 start, UINT64 blkcnt, void *dst);

// Diagnostics log (SdMmc.c) and "init finished, use the fast data path" flag.
VOID SdLog (CONST CHAR16 *Fmt, ...);
VOID SdDiagPublish (VOID);
extern BOOLEAN mSdFast;
extern UINT32  mLastXferSts;
extern UINT16  mLastAcmd12;
VOID tegra_mmc_recover_lines (VOID);

#endif