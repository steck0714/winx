/* Host-side AI descriptor fixtures; see test_nvram.c for compiler options. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define STATIC static
#define CONST const
#define IN
#define OUT
#define TRUE 1
#define FALSE 0
typedef int BOOLEAN;
typedef uint32_t UINT32;
typedef size_t UINTN;
#include "../core/WifiErom.h"

int main(void)
{
    /* Component pairs, master ports and 4 KiB slave/wrapper descriptors.
     * An unrelated component also contains a 64-bit address and an explicit
     * size descriptor; neither payload is a new component header. */
    static const UINT32 words[] = {
        0x4bf83e01, 0x2a004211, 0x00000003,
        0x18002005, 0x18100085, 0x181020c5,
        0x4bf81201, 0x07080201, 0x18001005, 0x18101085,
        0x4bf83c01, 0x05004211, 0x00000003, 0x18003005, 0x181030c5,
        0x4bf90001, 0x01080201,
        0x0000100d, 0x00000001,
        0x18004035, 0x00010008, 0x00000001,
        0x18005005, 0x18105085,
        0x0000000f
    };
    static const UINT32 bad_pair[] = {0x4bf83e01, 0x00000003, 0xf};
    static const UINT32 bad_extension[] = {0x4bf83e01, 0x2a004211, 0x1800200d};
    static const UINT32 bus_error[] = {0xffffffff};
    PEX_CORE cores[8];
    UINTN count = 0;
    size_t n = sizeof(words) / sizeof(words[0]);
    assert(PexEromParse(words, n, cores, 8, &count));
    assert(count == 4);
    assert(cores[0].Id == 0x83e && cores[0].Revision == 42);
    assert(cores[0].Base == 0x18002000 && cores[0].Wrapper == 0x18102000);
    assert(cores[1].Id == 0x812 && cores[1].Wrapper == 0x18101000);
    assert(cores[2].Id == 0x83c && cores[2].Base == 0x18003000);
    assert(cores[3].Base == 0x18005000 && cores[3].Wrapper == 0x18105000);
    assert(!PexEromParse(words, n, cores, 3, &count));
    assert(!PexEromParse(words, n - 1, cores, 8, &count));
    assert(!PexEromParse(bad_pair, 3, cores, 8, &count));
    assert(!PexEromParse(bad_extension, 3, cores, 8, &count));
    assert(!PexEromParse(bus_error, 1, cores, 8, &count));
    puts("EROM core/wrapper selection, extensions and truncated input passed");
    return 0;
}
