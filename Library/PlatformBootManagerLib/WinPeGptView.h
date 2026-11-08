// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef WINPE_GPT_VIEW_H
#define WINPE_GPT_VIEW_H

#include <Uefi.h>

EFI_STATUS BeginWinPeGptView (EFI_HANDLE Volume);
VOID EndWinPeGptView (VOID);

#endif
