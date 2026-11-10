/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_PAGING_DISPATCH_H
#define TG_GPU_PAGING_DISPATCH_H
#include "context.h"
DXGKDDI_BUILDPAGINGBUFFER TgGpuDdiBuildPagingBuffer;
void TgGpuRegisterPagingDdi(DRIVER_INITIALIZATION_DATA *);
#endif
