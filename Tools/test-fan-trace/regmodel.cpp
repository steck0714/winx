// Register file + trace recorder + scenario set-up for the fan trace test.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "regmodel.hpp"
#include "scenario.hpp"

namespace {
const uint64_t CAR = 0x60006000;
const uint64_t GPIO = 0x6000D000;
const uint64_t PINMUX = 0x70003000;
const uint64_t APB_MISC = 0x70000000;
const uint64_t PWM = 0x7000A000;
const uint64_t PMC = 0x7000E400;

struct Event { char kind; uint64_t addr; uint32_t val; uint32_t old; };
std::map<uint64_t, uint32_t> g_val;
std::map<uint64_t, uint32_t> g_init;
std::vector<Event> g_ev;
bool g_recording = false;

uint32_t cur (uint64_t a) { auto it = g_val.find (a); return it == g_val.end () ? 0 : it->second; }
}

uint32_t reg_read (uint64_t a) {
  uint32_t v = cur (a);
  if (g_recording) g_ev.push_back ({'R', a, v, v});
  return v;
}

void reg_write (uint64_t a, uint32_t v) {
  uint32_t old = cur (a);
  if (g_recording) g_ev.push_back ({'W', a, v, old});
  // Tegra CAR write-1-to-set / write-1-to-clear aliases of RST_DEVICES_L and CLK_OUT_ENB_L.
  if      (a == CAR + 0x300) g_val[CAR + 0x004] = cur (CAR + 0x004) |  v;
  else if (a == CAR + 0x304) g_val[CAR + 0x004] = cur (CAR + 0x004) & ~v;
  else if (a == CAR + 0x320) g_val[CAR + 0x010] = cur (CAR + 0x010) |  v;
  else if (a == CAR + 0x324) g_val[CAR + 0x010] = cur (CAR + 0x010) & ~v;
  else                       g_val[a] = v;
}

static uint32_t xorshift (uint32_t &s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

Scenario parse_scenario (int argc, char **argv) {
  Scenario s;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if      (!strncmp (a, "--duty=", 7))    s.duty = strtoul (a + 7, 0, 0);
    else if (!strncmp (a, "--hidrev=", 9))  s.hidrev = strtoul (a + 9, 0, 0);
    else if (!strncmp (a, "--pwm-clk=", 10)) {
      const char *v = a + 10;
      s.pwm_clk = !strcmp (v, "on") ? 1 : !strcmp (v, "off") ? 2 : !strcmp (v, "rst") ? 3 : !strcmp (v, "noclk") ? 0 : -1;
      if (s.pwm_clk < 0) { fprintf (stderr, "bad --pwm-clk\n"); exit (2); }
    }
    else if (!strncmp (a, "--pmc-bits=", 11)) s.pmc_bits = !strcmp (a + 11, "1");
    else if (!strncmp (a, "--gpio-rand=", 12)) s.gpio_rand = !strcmp (a + 12, "1");
    else if (!strncmp (a, "--seed=", 7))    s.seed = strtoul (a + 7, 0, 0);
    else { fprintf (stderr, "bad arg %s\n", a); exit (2); }
  }
  return s;
}

void prefill_registers (const Scenario &s) {
  g_recording = false;
  g_val.clear (); g_ev.clear ();
  uint32_t r = s.seed ? s.seed : 1;

  // identity
  g_val[APB_MISC + 0x804] = s.hidrev;

  // clocks: other peripherals running, PWM per scenario
  uint32_t enb = 0x3A00A5C0 & ~(1u << 17), rst = 0x04000100 & ~(1u << 17);
  if (s.pwm_clk & 1) enb |= (1u << 17);         // clock enabled
  if (s.pwm_clk & 2) rst |= (1u << 17);         // block held in reset
  g_val[CAR + 0x010] = enb;
  g_val[CAR + 0x004] = rst;
  g_val[CAR + 0x110] = 0x12345678;            // PWM clock source register: junk that must be replaced if touched

  // GPIO controller
  for (uint64_t off = 0; off < 0x800; off += 4)
    g_val[GPIO + off] = s.gpio_rand ? xorshift (r) : 0;

  // pinmux: the pads this test cares about get a "PinMuxDxe-like" non-trivial value
  g_val[PINMUX + 0x20C] = 0x00000001 | (1u << 6);     // LCD_GPIO2
  g_val[PINMUX + 0x4C]  = (2u << 2) | (1u << 6);      // SATA_LED_ACTIVE: pull-up + input (see PinMux.h)
  g_val[PINMUX + 0x1A8] = 0x00000041;                 // USB_VBUS_EN0 on the USB function
  g_val[PINMUX + 0x1EC] = 0x00000050;                 // CAM1_PWDN (tach)

  // PWM: backlight (CSR_0) is running and must survive; CSR_1 = reset value
  g_val[PWM + 0x00] = 0x80000000u | (200u << 16) | 0x1234;
  g_val[PWM + 0x10] = 0;

  // PMC: unrelated bits set so read-modify-write preservation is visible
  uint32_t noio = 0x0A5A5A5A & ~(1u << 21), det = 0x00C3C3C3 & ~(1u << 21);
  if (s.pmc_bits) { noio |= (1u << 21); det |= (1u << 21); }
  g_val[PMC + 0x44] = noio;
  g_val[PMC + 0xE4] = det;

  g_init = g_val;
  g_recording = true;
}

void dump_trace (const char *who, const Scenario &s) {
  printf ("# %s duty=%u hidrev=0x%08x pwm_clk=%s pmc_bits=%d gpio_rand=%d seed=%u\n",
          who, s.duty, s.hidrev, s.pwm_clk == 1 ? "on" : s.pwm_clk == 2 ? "off" : s.pwm_clk == 3 ? "rst" : "noclk", s.pmc_bits, s.gpio_rand, s.seed);
  for (auto &kv : g_init)
    printf ("I %08llx %08x\n", (unsigned long long)kv.first, kv.second);
  for (auto &e : g_ev)
    if (e.kind == 'W') printf ("W %08llx %08x %08x\n", (unsigned long long)e.addr, e.val, e.old);
  for (auto &kv : g_val)
    printf ("S %08llx %08x\n", (unsigned long long)kv.first, kv.second);
}
