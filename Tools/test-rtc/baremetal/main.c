/* Bare-metal AArch64 driver around the REAL LibGetTime() (Library/VirtualRealTimeClockLib).
   QEMU preloads [u64 count][u64 elapsed-seconds ...] at 0x41000000. One result line per value goes to the
   PL011 in the same format as ../test_rtc.c, then "DONE". Driven by run-qemu.sh / qemu-run.py. */
#include <PiDxe.h>

UINT64 g_counter, g_freq = 19200000;

/* In the firmware memcpy() comes from CompilerIntrinsicsLib; here the compiler emits a call for the
   local string arrays in BuildDateEpochSeconds(). volatile: keep GCC from turning the loop back into memcpy. */
void *memcpy (void *dst, const void *src, unsigned long n) {
  volatile unsigned char *d = dst; const volatile unsigned char *s = src;
  while (n--) *d++ = *s++;
  return dst;
}
extern EFI_STATUS LibGetTime (EFI_TIME *Time, EFI_TIME_CAPABILITIES *Cap);

#define UART ((volatile UINT32 *) 0x09000000)
static void putc_ (char c) { *UART = (UINT8) c; }
static void puts_ (const char *s) { while (*s) putc_ (*s++); }
static void putu (UINT64 v) { char b[24]; int i = 0; if (!v) b[i++] = '0'; while (v) { b[i++] = (char) ('0' + v % 10); v /= 10; } while (i) putc_ (b[--i]); }
static void puti (INT64 v) { if (v < 0) { putc_ ('-'); v = -v; } putu ((UINT64) v); }
static void sp (void) { putc_ (' '); }

void main_ (void) {
  const volatile UINT64 *p = (const volatile UINT64 *) 0x41000000;
  UINT64 n = p[0];
  for (UINT64 i = 1; i <= n; i++) {
    UINT64 elapsed = p[i];
    EFI_TIME t; EFI_TIME_CAPABILITIES c;
    g_counter = elapsed * g_freq + 12345;                    /* + a fraction of a second: must floor */
    EFI_STATUS st = LibGetTime (&t, &c);
    putu (elapsed); sp (); putu (st); sp (); putu (t.Year); sp (); putu (t.Month); sp (); putu (t.Day); sp ();
    putu (t.Hour); sp (); putu (t.Minute); sp (); putu (t.Second); sp (); puti (t.TimeZone); sp (); putu (t.Nanosecond);
    putc_ ('\n');
  }
  puts_ ("DONE\n");
  for (;;) __asm__ volatile ("wfi");
}
