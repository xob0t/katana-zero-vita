/*
 * Fast path of the loader's log formatting (debugPrintf). newlib's vsnprintf costs ~17 us per log line on the Vita,
 * most of it in _dtoa for %f; the 2-s debug report formats ~45 lines on the game thread.
 *
 * log_format handles only what it can reproduce byte for byte and returns -1 for everything else, so the caller
 * formats the line with vsnprintf instead:
 *  - conversions d i u x X c s f % (no e g p n), length modifier none or l, flags '-' and '0', a decimal width,
 *    a decimal precision on s and f (none on integers, no '*');
 *  - %f: finite values with |value| * 10^precision < 2^52 and precision <= 9. The digits come from one
 *    rounded product value * 10^precision; when that product lies within its rounding error of a half-way point the
 *    exact decimal rounding could differ, so it falls back. Negative values that round to zero ("-0.0") fall back.
 *  - %s of NULL falls back (newlib prints "(null)").
 * tests/log_format_test.c compares it with the C library's snprintf.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "log_format.h"

static const double pow10_d[10] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9 };
static const uint32_t pow10_u[10] = { 1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u,
	1000000000u };

// Digits of v in base (10 or 16), most significant first, into the end of buf; returns the first digit
static char *digits(char *end, uint64_t v, unsigned base, int upper) {
	const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char *p = end;
	if (v <= 0xffffffffu) { // 32-bit division is much cheaper than __aeabi_uldivmod
		uint32_t w = (uint32_t)v;
		do {
			*--p = set[w % base];
			w /= base;
		} while (w);
	} else {
		do {
			*--p = set[v % base];
			v /= base;
		} while (v);
	}
	return p;
}

// Appends sign + body padded to width; returns 0 if it doesn't fit
static int put(char *out, int cap, int *len, const char *sign, int sign_len, const char *body, int body_len,
	int width, int left, int zero) {
	int total = sign_len + body_len;
	int pad = width > total ? width - total : 0;
	if (*len + total + pad >= cap)
		return 0;
	char *p = out + *len;
	if (!left && !zero)
		for (int i = 0; i < pad; i++)
			*p++ = ' ';
	memcpy(p, sign, sign_len);
	p += sign_len;
	if (!left && zero)
		for (int i = 0; i < pad; i++)
			*p++ = '0';
	memcpy(p, body, body_len);
	p += body_len;
	if (left)
		for (int i = 0; i < pad; i++)
			*p++ = ' ';
	*len = (int)(p - out);
	return 1;
}

int log_format(char *out, int cap, const char *fmt, va_list ap) {
	int len = 0;
	char num[48];
	char *const num_end = num + sizeof(num);
	for (const char *f = fmt; *f;) {
		if (*f != '%') {
			const char *q = f;
			while (*q && *q != '%')
				q++;
			int n = (int)(q - f);
			if (len + n >= cap)
				return -1;
			memcpy(out + len, f, n);
			len += n;
			f = q;
			continue;
		}
		f++;
		int left = 0, zero = 0;
		for (;; f++) {
			if (*f == '-')
				left = 1;
			else if (*f == '0')
				zero = 1;
			else
				break;
		}
		int width = 0;
		while (*f >= '0' && *f <= '9') {
			width = width * 10 + (*f++ - '0');
			if (width > 512)
				return -1;
		}
		int prec = -1;
		if (*f == '.') {
			f++;
			prec = 0;
			while (*f >= '0' && *f <= '9') {
				prec = prec * 10 + (*f++ - '0');
				if (prec > 512)
					return -1;
			}
		}
		int is_long = 0;
		if (*f == 'l') {
			is_long = 1;
			f++;
			if (*f == 'l')
				return -1;
		}
		char conv = *f++;
		switch (conv) {
		case '%':
			if (left || zero || width || prec >= 0 || is_long)
				return -1;
			if (len + 1 >= cap)
				return -1;
			out[len++] = '%';
			break;
		case 'd':
		case 'i': {
			if (prec >= 0)
				return -1;
			int64_t v = is_long ? (int64_t)va_arg(ap, long) : (int64_t)va_arg(ap, int);
			uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
			char *p = digits(num_end, mag, 10, 0);
			if (!put(out, cap, &len, "-", v < 0, p, (int)(num_end - p), width, left, zero))
				return -1;
			break;
		}
		case 'u':
		case 'x':
		case 'X': {
			if (prec >= 0)
				return -1;
			uint64_t v = is_long ? (uint64_t)va_arg(ap, unsigned long) : (uint64_t)va_arg(ap, unsigned int);
			char *p = digits(num_end, v, conv == 'u' ? 10 : 16, conv == 'X');
			if (!put(out, cap, &len, "", 0, p, (int)(num_end - p), width, left, zero))
				return -1;
			break;
		}
		case 'c': {
			if (prec >= 0 || zero || is_long)
				return -1;
			char c = (char)va_arg(ap, int);
			if (!put(out, cap, &len, "", 0, &c, 1, width, left, 0))
				return -1;
			break;
		}
		case 's': {
			if (zero || is_long)
				return -1;
			const char *s = va_arg(ap, const char *);
			if (!s)
				return -1;
			int n;
			if (prec >= 0) {
				const char *z = memchr(s, 0, prec);
				n = z ? (int)(z - s) : prec;
			} else {
				n = (int)strlen(s);
			}
			if (!put(out, cap, &len, "", 0, s, n, width, left, 0))
				return -1;
			break;
		}
		case 'f': {
			if (is_long)
				return -1;
			double v = va_arg(ap, double);
			if (prec < 0)
				prec = 6;
			if (prec > 9 || !isfinite(v))
				return -1;
			int neg = signbit(v) != 0;
			double a = fabs(v);
			double s = a * pow10_d[prec]; // one rounding: |s - exact| <= s * 2^-53
			if (!(s < 4503599627370496.0)) // 2^52: floor and s - floor are exact below it
				return -1;
			double r = floor(s);
			double frac = s - r;
			double err = s * 0x1p-52 + 0x1p-1074;
			if (fabs(frac - 0.5) <= err)
				return -1;
			uint64_t n = (uint64_t)r + (frac > 0.5);
			if (neg && n == 0)
				return -1;
			char *p = num_end;
			if (prec > 0) {
				uint64_t ip;
				uint32_t fp;
				if (n <= 0xffffffffu) {
					ip = (uint32_t)n / pow10_u[prec];
					fp = (uint32_t)n - (uint32_t)ip * pow10_u[prec];
				} else {
					ip = n / pow10_u[prec];
					fp = (uint32_t)(n - ip * pow10_u[prec]);
				}
				for (int i = 0; i < prec; i++) {
					*--p = (char)('0' + fp % 10);
					fp /= 10;
				}
				*--p = '.';
				p = digits(p, ip, 10, 0);
			} else {
				p = digits(p, n, 10, 0);
			}
			if (!put(out, cap, &len, "-", neg, p, (int)(num_end - p), width, left, zero))
				return -1;
			break;
		}
		default:
			return -1;
		}
	}
	if (len >= cap)
		return -1;
	out[len] = 0;
	return len;
}
