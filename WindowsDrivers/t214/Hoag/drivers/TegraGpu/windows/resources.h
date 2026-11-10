/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_WINDOWS_RESOURCES_H
#define TEGRA_GPU_WINDOWS_RESOURCES_H

#include <ntddk.h>
#include <dispmprt.h>

enum tg_gpu_aperture {
    TgGpuRegisters,
    TgGpuBar1,
    TgGpuAuxRegisters,
    TgGpuCarX,
    TgGpuOut5,
    TgGpuClamp,
    TgGpuMcFlush,
    TgGpuMcAsid,
    TgGpuHidrev,
    TgGpuSpeedo,
    TgGpuCalibration,
    TgGpuApertureCount
};

typedef struct _TG_GPU_RESOURCES {
    CM_PARTIAL_RESOURCE_DESCRIPTOR Memory[TgGpuApertureCount];
    CM_PARTIAL_RESOURCE_DESCRIPTOR Interrupt[2]; /* stall, nonstall */
    PVOID Mapping[TgGpuApertureCount];
    /* Optional on the earlier GPU table. Required before enabling nonstall
     * interrupts; supplied by SWITCH_LITE_GPU_IRQ=1, not a raw mapping guess.
     */
    CM_PARTIAL_RESOURCE_DESCRIPTOR Lic;
    PVOID LicMapping;
} TG_GPU_RESOURCES;

/* Adapter start/stop helpers following the render-only WDDM sample's
 * DxgkCbGetDeviceInformation / DxgkCbMapMemory ownership model.
 * Call at PASSIVE_LEVEL with adapter start/stop serialized by the caller.
 * The context must be zero-initialized; release before another acquire.
 * Mapping a resource does not enable its power rail or access its registers.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuAcquireResources(const DXGKRNL_INTERFACE *Dxgk,
                              TG_GPU_RESOURCES *Resources);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuReleaseResources(const DXGKRNL_INTERFACE *Dxgk,
                              TG_GPU_RESOURCES *Resources);

#endif
