// Runs hekate's real fan_set_duty() (bdk/thermal/fan.c + power/regulator_5v.c + soc/gpio.c, compiled as
// C++ unchanged) against the fake register file and prints the register writes.
#include <utils/types.h>
#include <thermal/fan.h>
#include <soc/hw_init.h>
#include <soc/fuse.h>
#include <soc/clock.h>
#include <soc/timer.h>
#include <soc/t210.h>
#include "scenario.hpp"

// Stand-ins for the parts of hekate that are not under test.
u32  hw_get_chip_id () {                     // same logic as bdk/soc/hw_init.c
  return (((u32) *(vu32 *)(APB_MISC_BASE + APB_MISC_GP_HIDREV) >> 4) & 0xF) >= GP_HIDREV_MAJOR_T210B01
         ? GP_HIDREV_MAJOR_T210B01 : GP_HIDREV_MAJOR_T210;
}
u32  fuse_read_hw_type () { return FUSE_NX_HW_TYPE_ICOSA; }   // Erista; clock_enable_pwm() is Aula-only
void clock_enable_pwm () { fprintf (stderr, "hekate side: clock_enable_pwm() called (Aula only!)\n"); exit (3); }
u32  get_tmr_us () { return 0; }

int main (int argc, char **argv) {
  Scenario s = parse_scenario (argc, argv);
  prefill_registers (s);
  fan_set_duty (s.duty);
  dump_trace ("hekate", s);
  return 0;
}
