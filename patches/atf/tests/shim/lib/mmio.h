#include <stdint.h>
uint32_t mmio_read_32(uintptr_t address);
void mmio_write_32(uintptr_t address, uint32_t value);
