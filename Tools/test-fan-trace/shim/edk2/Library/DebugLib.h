#pragma once
#include <Uefi.h>
#define DEBUG_WARN 0x00000002
#define DEBUG_INFO 0x00000040
void shim_debug (unsigned level, const char *fmt, ...);
#define DEBUG(Expression) shim_debug Expression
