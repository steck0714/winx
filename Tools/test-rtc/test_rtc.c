/* Feeds elapsed-second values (stdin) to the REAL LibGetTime() and prints the EFI_TIME it returns. */
#include <PiDxe.h>
#include <stdio.h>
#include <stdlib.h>
UINT64 g_counter, g_freq = 19200000;
extern EFI_STATUS LibGetTime (EFI_TIME *Time, EFI_TIME_CAPABILITIES *Cap);

int main (void) {
  unsigned long long elapsed;
  while (scanf ("%llu", &elapsed) == 1) {
    EFI_TIME t; EFI_TIME_CAPABILITIES c;
    g_counter = (UINT64) elapsed * g_freq + 12345;          /* + a fraction of a second: must floor */
    EFI_STATUS st = LibGetTime (&t, &c);
    printf ("%llu %llu %u %u %u %u %u %u %d %u\n", elapsed, (unsigned long long) st, t.Year, t.Month, t.Day,
            t.Hour, t.Minute, t.Second, t.TimeZone, t.Nanosecond);
  }
  return 0;
}
