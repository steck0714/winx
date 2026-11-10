/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_ADAPTER_H
#define TG_GPU_ADAPTER_H
#include "context.h"
#include "work_queue.h"
#include "resources.h"
#include "hardware.h"
#include "platform_io.h"
#include "worker.h"

/* Enough dedicated table backing for the initial single-process Vulkan
 * triangle; application storage remains dynamically allocated system memory.
 * VidMm suballocates this segment. This is not the 512 MiB application budget.
 */
#define TG_GPU_ADAPTER_TABLE_BYTES (16u*TG_GPU_TABLE_BYTES)
enum tg_gpu_adapter_state { TgGpuAdapterAdded,TgGpuAdapterStarting,TgGpuAdapterReady,
    TgGpuAdapterStopping,TgGpuAdapterResetting,TgGpuAdapterSuspended };
typedef struct _TG_GPU_ADAPTER {
    /* All implemented allocation/context/query DDIs accept this same handle. */
    TG_GPU_CONTEXT_ENGINE Engine;
    PDEVICE_OBJECT PhysicalDevice;
    TG_GPU_RESOURCES Resources;
    TG_GPU_TABLE_SEGMENT Tables;
    TG_GPU_WORK_QUEUE Queue;
    TG_GPU_HARDWARE Hardware;
    TG_GPU_PLATFORM Platform;
    TG_GPU_WORKER Worker;
    enum tg_gpu_adapter_state State;
    BOOLEAN EngineReady,QueueReady;
} TG_GPU_ADAPTER;
_Static_assert(FIELD_OFFSET(TG_GPU_ADAPTER,Engine)==0,"adapter DDI handle layout");

DXGKDDI_ADD_DEVICE TgGpuDdiAddDevice;
DXGKDDI_START_DEVICE TgGpuDdiStartDevice;
DXGKDDI_STOP_DEVICE TgGpuDdiStopDevice;
DXGKDDI_REMOVE_DEVICE TgGpuDdiRemoveDevice;
DXGKDDI_RESETFROMTIMEOUT TgGpuDdiResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT TgGpuDdiRestartFromTimeout;
NTSTATUS TgGpuRestartAdapterHardware(TG_GPU_ADAPTER *);
void TgGpuRegisterResetDdis(DRIVER_INITIALIZATION_DATA *);
/* The platform provider starts after StartDevice acquires resources. */
NTSTATUS TgGpuStartAdapterHardware(TG_GPU_ADAPTER *,const struct tg_gpu_power_io *,
    UCHAR PmicAddress,const struct tg_gpu_gr_payload *);
/* Platform, firmware and worker hookup after resource acquisition. */
NTSTATUS TgGpuStartPlatformAdapter(TG_GPU_ADAPTER *);
/* Populate shared callbacks. The miniport adds render-only lifecycle DDIs. */
void TgGpuRegisterAdapterDdis(DRIVER_INITIALIZATION_DATA *);
#endif
