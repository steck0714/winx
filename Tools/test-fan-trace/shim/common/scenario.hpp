#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

struct Scenario {
  uint32_t duty       = 76;
  uint32_t hidrev     = 0x00012110;   // GP_HIDREV: chip 0x21, major (bits 7:4) 1 = T210, 2 = T210B01
  int      pwm_clk    = 1;            // CAR PWM clock: bit0 = enabled, bit1 = held in reset (1 = running)
  bool     pmc_bits   = false;        // PMC NO_IOPOWER / PWR_DET_VAL bit 21 set (wants clearing)
  bool     gpio_rand  = false;        // random GPIO controller contents instead of zeros
  uint32_t seed       = 1;
};

Scenario parse_scenario (int argc, char **argv);
void     prefill_registers (const Scenario &s);
void     dump_trace (const char *who, const Scenario &s);
