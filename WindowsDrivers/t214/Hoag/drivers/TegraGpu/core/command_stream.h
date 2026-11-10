/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_COMMAND_STREAM_H
#define TG_GPU_COMMAND_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Decode the complete linear B06F stream. Reject control-flow/subdevice
 * headers, reserved bits, overruns and host methods other than object binding
 * and WFI. In particular USER code cannot write the kernel Reference marker.
 * Class-specific address/privilege/residency validation is still separate.
 */
bool tg_gpu_validate_command_stream(const uint32_t *, size_t bytes);
#endif
