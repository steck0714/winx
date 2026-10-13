// Runs the real Drivers/FanDxe/FanDxe.c and Library/GpioLib/GpioLib.c (compiled as C++ unchanged, with the
// EDK2 headers they need replaced by tiny shims) against the same fake register file.
#include <cstdarg>
#include "scenario.hpp"
#include <Uefi.h>

uint8_t            g_pcd_duty = 0;
static EFI_BOOT_SERVICES g_bs;
EFI_BOOT_SERVICES *gBS = &g_bs;

void shim_debug (unsigned level, const char *fmt, ...) {
  va_list ap; va_start (ap, fmt);
  fprintf (stderr, "[DEBUG %02x] ", level); vfprintf (stderr, fmt, ap);
  va_end (ap);
}

extern EFI_STATUS FanDxeInitialize (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

int main (int argc, char **argv) {
  Scenario s = parse_scenario (argc, argv);
  prefill_registers (s);
  g_pcd_duty = (uint8_t) s.duty;
  EFI_STATUS st = FanDxeInitialize (nullptr, nullptr);
  dump_trace ("fandxe", s);
  printf ("# status %llu\n", (unsigned long long) st);
  return 0;
}
