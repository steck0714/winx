#pragma once
#include <utils/types.h>
enum { FUSE_NX_HW_TYPE_ICOSA, FUSE_NX_HW_TYPE_IOWA, FUSE_NX_HW_TYPE_HOAG, FUSE_NX_HW_TYPE_AULA };
u32 fuse_read_hw_type ();
