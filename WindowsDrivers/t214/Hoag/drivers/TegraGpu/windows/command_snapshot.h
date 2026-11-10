/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_COMMAND_SNAPSHOT_H
#define TG_GPU_COMMAND_SNAPSHOT_H
#include <ntddk.h>
#include <dispmprt.h>
#include "submit_abi.h"
typedef struct _TG_GPU_COMMAND_SNAPSHOT {UINT Bytes;UINT Words[];} TG_GPU_COMMAND_SNAPSHOT;
NTSTATUS TgGpuCaptureCommandSnapshot(const DXGKARG_SUBMITCOMMANDVIRTUAL *,TG_GPU_COMMAND_SNAPSHOT **);
VOID TgGpuFreeCommandSnapshot(TG_GPU_COMMAND_SNAPSHOT *);
#endif
