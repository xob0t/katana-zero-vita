#ifndef LOG_FORMAT_H
#define LOG_FORMAT_H

#include <stdarg.h>

// Formats like vsnprintf(out, cap, fmt, ap) for the subset described in log_format.c and returns the length, or -1
// when the format, a value or the length is outside it (the caller then formats with vsnprintf; ap is used up).
int log_format(char *out, int cap, const char *fmt, va_list ap);

#endif
