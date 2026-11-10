/* SPDX-License-Identifier: MIT */
/* Minimal self-registering test harness (constructor-based; link every sim source file). */
#ifndef SIM_TEST_H
#define SIM_TEST_H

#include <stdio.h>

void test_register(const char *name, void (*fn)(void));
void test_fail(const char *file, int line, const char *expr);
void test_fail_eq(const char *file, int line, const char *ea, const char *eb,
		  unsigned long long a, unsigned long long b);

#define TEST(name)                                                              \
	static void name(void);                                                 \
	__attribute__((constructor)) static void name##_register(void)          \
	{                                                                       \
		test_register(#name, name);                                     \
	}                                                                       \
	static void name(void)

#define T_ASSERT(c)                                                             \
	do {                                                                    \
		if (!(c)) {                                                     \
			test_fail(__FILE__, __LINE__, #c);                      \
			return;                                                 \
		}                                                               \
	} while (0)

#define T_EQ(a, b)                                                              \
	do {                                                                    \
		unsigned long long _a = (unsigned long long)(a);                \
		unsigned long long _b = (unsigned long long)(b);                \
		if (_a != _b) {                                                 \
			test_fail_eq(__FILE__, __LINE__, #a, #b, _a, _b);       \
			return;                                                 \
		}                                                               \
	} while (0)

#endif
