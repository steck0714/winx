/* SPDX-License-Identifier: ISC */
#include <ntddk.h>

/* Allocation hooks required by the WDK's WificxTLVGenParse library. */
void *operator new(size_t size, ULONG_PTR context)
{
    UNREFERENCED_PARAMETER(context);
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, size, 'tFwS');
}

void operator delete(void *buffer) noexcept
{
    if (buffer) ExFreePoolWithTag(buffer, 'tFwS');
}
