// C++-friendly replacement for hekate's bdk/utils/types.h (only what fan.c / regulator_5v.c / gpio.c
// and the register headers need). `vu32` is the register proxy of the fake register file.
#ifndef _TYPES_H_
#define _TYPES_H_

#include <cstdint>
#include <cstddef>
#include "regmodel.hpp"

typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long long int s64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long int u64;
typedef volatile unsigned char vu8;
typedef volatile unsigned short vu16;
typedef unsigned long long uptr;

#define DISABLE 0
#define ENABLE  1
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))

#endif
