// Host test of loader/loader/log_format.c: whatever it formats must equal the C library's snprintf byte for byte,
// and it must take the fast path for the common log formats (else the test passes vacuously).
// gcc -O1 -Wall -Iloader/loader tests/log_format_test.c -o log-format-test -lm && ./log-format-test
#define __USE_MINGW_ANSI_STDIO 1 // MinGW: the mingw printf (exact decimal rounding, like glibc and newlib)
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../loader/loader/log_format.c"

static unsigned long long tests, fast, failures;

static void check(const char *fmt, ...) {
	char want[1024], got[1024];
	va_list ap;
	va_start(ap, fmt);
	int want_len = vsnprintf(want, sizeof(want), fmt, ap);
	va_end(ap);
	va_start(ap, fmt);
	int got_len = log_format(got, sizeof(got), fmt, ap);
	va_end(ap);
	tests++;
	if (got_len < 0)
		return;
	fast++;
	if (got_len != want_len || memcmp(got, want, want_len + 1)) {
		if (failures++ < 20)
			printf("MISMATCH fmt \"%s\": libc \"%s\" (%d), log_format \"%s\" (%d)\n", fmt, want, want_len, got, got_len);
	}
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) {
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}
static double uniform(void) { return (next() >> 11) * 0x1p-53; }

static double value(void) {
	switch (next() % 9) {
	case 0: return (uniform() - 0.5) * 2000.0;
	case 1: return uniform() * 20.0;
	case 2: return (float)(uniform() * 100.0); // float counters divided by frames
	case 3: return (double)(next() % 100000) / (double)(1 + next() % 240); // count / frames
	case 4: return (double)(int64_t)(next() % 2000001 - 1000000) / 1000.0; // near-ties in decimal
	case 5: return (double)(int64_t)(next() % 2001 - 1000) / 8.0; // exact binary ties
	case 6: return ldexp(uniform(), (int)(next() % 120) - 60) * ((next() & 1) ? -1 : 1);
	case 7: return (double)(next() % 1000000) * 0.05;
	default: return -uniform() * 0.1; // rounds to "-0.0"-like
	}
}

int main(void) {
	// The fixed shapes the loader logs (from the report)
	check("[%5u.%03u] ", 19u, 115u);
	check("[PROF] fps %.1f | process %.2fms | worst frame %.2fms | %dMHz\n", 57.8f, 3.12f, 110.58f, 500);
	check("[LAYERDRAW] %-28s %6.1f draws %6.2fms\n", "obj_player", 12.5f, 0.125f);
	check("[RUNNER] %-24s %7.1f calls %6.2fms (max %.2fms)\n", "draw_self", 1234.56, 0.005, 1.0);
	check("%08X %08x %x %X %c%c%%", 0xdeadbeefu, 0x1234u, 0u, 0xabcu, 'a', 10);
	check("%d %d %d %i %5d %-5d| %05d %-05d|", 0, -1, (int)0x80000000u, 2147483647, -42, -42, -42, 42);
	check("%ld %lu %lx", -123456789L, 4000000000UL, 0xffffffffUL);
	check("%s|%.3s|%10s|%-10s|%.0s|", "abc", "abcdef", "xy", "xy", "z");
	check("%.0f %.0f %.0f %.0f %f %.9f", 0.5, 1.5, 2.5, 3.49999, 1.0 / 3.0, 2.0 / 3.0);
	check("%.1f %.1f %.1f %.2f %.2f", 0.05, 0.15, 0.25, 1.005, 2.675);
	check("%.1f %.3f %6.2f", -0.04, -0.0004, -0.0);
	check("%.2f %.2f", 1e300, -INFINITY);
	check("%.1f", NAN);
	check("%p %g %e %lld %hhd %+d % d %#x %*d", (void *)0, 1.0, 1.0, 1LL, 1, 1, 1, 1, 3, 1);

	const char *ffmts[] = { "%.0f", "%.1f", "%.2f", "%.3f", "%.4f", "%f", "%.9f", "%6.2f", "%6.1f", "%9.1f", "%-8.3f|",
		"%08.2f", "%-08.1f|", "%5.1f" };
	for (int i = 0; i < 400000; i++) {
		const char *f = ffmts[i % (sizeof(ffmts) / sizeof(ffmts[0]))];
		check(f, value());
	}
	for (int i = 0; i < 100000; i++) {
		uint32_t u = (uint32_t)next() >> (next() % 32);
		check("%u %d %x %X %08X %7u %-6d|%3u", u, (int)u, u, u, u, u, (int)u, u % 1000);
	}
	unsigned long long common_fast = fast;
	printf("%llu checks, %llu on the fast path, %llu mismatches\n", tests, fast, failures);
	if (failures)
		return 1;
	// Mostly fast: only near-ties, -0 roundings, huge values and the unsupported shapes fall back
	if (common_fast < tests * 9 / 10) {
		printf("fast path too rare\n");
		return 1;
	}
	return 0;
}
