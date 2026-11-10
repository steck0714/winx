/* SPDX-License-Identifier: MIT */
/*
 * The compiler may emit calls to these even with -ffreestanding.  They are written so that they can never be
 * turned back into calls to themselves (optnone), and only ever used on ordinary memory (device memory is
 * touched exclusively through the HAL's aligned 32-bit accesses).
 */
#include <stddef.h>

#define NOOPT __attribute__((optnone, noinline))

NOOPT void *memset(void *dst, int c, size_t n)
{
	unsigned char *p = (unsigned char *)dst;

	while (n--)
		*p++ = (unsigned char)c;
	return dst;
}

NOOPT void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	while (n--)
		*d++ = *s++;
	return dst;
}

NOOPT void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	if (d == s || n == 0)
		return dst;
	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

NOOPT int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;

	while (n--) {
		if (*x != *y)
			return *x < *y ? -1 : 1;
		x++;
		y++;
	}
	return 0;
}
