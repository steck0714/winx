/* SPDX-License-Identifier: MIT */
#include "test.h"

#include <stdlib.h>
#include <string.h>

#define MAX_TESTS 1024

static struct {
	const char *name;
	void (*fn)(void);
} tests[MAX_TESTS];
static int ntests;
static int cur_failed;
static int total_failed;

void test_register(const char *name, void (*fn)(void))
{
	if (ntests >= MAX_TESTS) {
		fprintf(stderr, "too many tests\n");
		abort();
	}
	tests[ntests].name = name;
	tests[ntests].fn = fn;
	ntests++;
}

void test_fail(const char *file, int line, const char *expr)
{
	cur_failed = 1;
	fprintf(stderr, "    FAIL %s:%d: %s\n", file, line, expr);
}

void test_fail_eq(const char *file, int line, const char *ea, const char *eb,
		  unsigned long long a, unsigned long long b)
{
	cur_failed = 1;
	fprintf(stderr, "    FAIL %s:%d: %s == %s  (0x%llx vs 0x%llx)\n", file, line, ea, eb, a, b);
}

static int cmp(const void *x, const void *y)
{
	const char *a = *(const char *const *)x;
	const char *b = *(const char *const *)y;

	return strcmp(a, b);
}

int main(int argc, char **argv)
{
	int i, order[MAX_TESTS];
	const char *filter = argc > 1 ? argv[1] : NULL;
	int ran = 0;

	/* stable, name-sorted order so output is deterministic */
	for (i = 0; i < ntests; i++)
		order[i] = i;
	for (i = 1; i < ntests; i++) {
		int k = order[i], j = i - 1;

		while (j >= 0 && cmp(&tests[order[j]].name, &tests[k].name) > 0) {
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = k;
	}

	for (i = 0; i < ntests; i++) {
		int t = order[i];

		if (filter && !strstr(tests[t].name, filter))
			continue;
		cur_failed = 0;
		tests[t].fn();
		ran++;
		printf("[%s] %s\n", cur_failed ? "FAIL" : " ok ", tests[t].name);
		total_failed += cur_failed;
	}
	printf("\n%d tests, %d failed\n", ran, total_failed);
	return total_failed ? 1 : 0;
}
