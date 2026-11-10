/* Host-side format checks: cc -std=c11 -Wall -Wextra -Werror
 * -fsanitize=address,undefined test_nvram.c -o test-nvram && ./test-nvram
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATIC static
#define CONST const
#define IN
#define OUT
#define TRUE 1
#define FALSE 0
typedef int BOOLEAN;
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef size_t UINTN;
typedef char CHAR8;
#define AllocateZeroPool(n) calloc(1, (n))
#define FreePool free
#define CopyMem memcpy
#define CompareMem memcmp
#include "../core/WifiNvram.h"

static void expect(const void *input, size_t input_size,
                   const char *entries, size_t entry_bytes)
{
    UINT8 *output = NULL;
    UINT32 size = 0, token;
    size_t i;
    assert(PexNvramBuild(input, input_size, &output, &size));
    assert(size % 4 == 0 && size >= entry_bytes + 5);
    assert(memcmp(output, entries, entry_bytes) == 0);
    for (i = entry_bytes; i < size - 4; i++) assert(output[i] == 0);
    token = (UINT32)output[size - 4] | ((UINT32)output[size - 3] << 8) |
            ((UINT32)output[size - 2] << 16) | ((UINT32)output[size - 1] << 24);
    assert((token & 0xffff) == (size - 4) / 4);
    assert((token >> 16) == ((~token) & 0xffff));
    free(output);
}

static void reject(const void *input, size_t size)
{
    UINT8 *output = (UINT8 *)(uintptr_t)1;
    UINT32 output_size = 123;
    assert(!PexNvramBuild(input, size, &output, &output_size));
    assert(output == NULL && output_size == 0);
}

int main(void)
{
    static const char text[] = "# board file\r\n\tfoo=bar# inline\r\nboardrev=0x1100\n";
    static const char entries[] = "foo=bar\0boardrev=0x1100\0";
    static const char packed[] = "a=1\0b=2\0";
    static const char packed_entries[] = "a=1\0b=2\0boardrev=0xff\0";
    static const char bad_lines[] = "bad key=x\nRAW1=ignored\nwithout_equals\nfoo=bar";
    static const char valid_tail[] = "foo=bar\0boardrev=0xff\0";
    static const char no_newline[] = "x=aaa";
    static const char no_newline_entries[] = "x=aaa\0boardrev=0xff\0";
    char *large = malloc(PEX_NVRAM_MAX + 2);

    expect(text, sizeof(text) - 1, entries, sizeof(entries) - 1);
    expect(packed, sizeof(packed) - 1, packed_entries, sizeof(packed_entries) - 1);
    expect(bad_lines, sizeof(bad_lines) - 1, valid_tail, sizeof(valid_tail) - 1);
    expect(no_newline, sizeof(no_newline) - 1, no_newline_entries, sizeof(no_newline_entries) - 1);
    reject(NULL, 0);
    reject("# comment only", 14);
    reject("devpath0=pcie/0/1/\n0:boardrev=1", 30);
    reject("pcie/0/1/boardrev=1", 18);
    memset(large, 'x', PEX_NVRAM_MAX + 2);
    large[1] = '=';
    reject(large, PEX_NVRAM_MAX + 2);
    free(large);
    UINT8 firmware[0x74] = {0};
    UINT32 ram;
    assert(PexFirmwareRamSize(firmware,sizeof(firmware),1024,&ram) && ram==1024);
    memcpy(firmware+0x6c,"RAMS",4);firmware[0x71]=2;
    assert(PexFirmwareRamSize(firmware,sizeof(firmware),1024,&ram) && ram==512);
    firmware[0x71]=8;
    assert(!PexFirmwareRamSize(firmware,sizeof(firmware),1024,&ram) && ram==0);
    firmware[0x71]=0;
    assert(!PexFirmwareRamSize(firmware,sizeof(firmware),1024,&ram));
    firmware[0x71]=2;firmware[0x70]=1;
    assert(!PexFirmwareRamSize(firmware,sizeof(firmware),1024,&ram));
    assert(!PexFirmwareRamSize(firmware,3,1024,&ram));
    puts("NVRAM packing/bounds and firmware RAMS aperture checks passed");
    return 0;
}
