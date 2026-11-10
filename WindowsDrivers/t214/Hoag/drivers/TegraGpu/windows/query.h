/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_QUERY_H
#define TEGRA_GPU_QUERY_H
#include "context.h"

/* Memory/topology queries for the existing context engine adapter handle.
 * Start/stop and query calls must be serialized by the adapter lifecycle.
 * UMD-private identity reports the 512 MiB ceiling and OS adapter LUID; its
 * native-submit bit remains unset. DRIVERCAPS stays unsupported until real
 * submission and TDR are wired. This does not expose a complete adapter.
 */
DXGKDDI_QUERYADAPTERINFO TgGpuDdiQueryAdapterInfo;
void TgGpuRegisterQueryDdi(DRIVER_INITIALIZATION_DATA *);
#endif
