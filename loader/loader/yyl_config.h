#ifndef YYL_CONFIG_H
#define YYL_CONFIG_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { YYL_KEY_SIZE = 64, YYL_PLATFORM_COUNT = 3 };

static inline int yyl_read_setting(FILE *config, char key[YYL_KEY_SIZE], int *value) {
	for (;;) {
		char line[128];
		size_t len = 0;
		int c, invalid = 0;
		while ((c = fgetc(config)) != EOF && c != '\n') {
			if (!c || len == sizeof(line) - 1)
				invalid = 1;
			else
				line[len++] = c;
		}
		if (c == EOF && (!len || ferror(config)))
			return 0;
		if (invalid)
			continue;
		line[len] = '\0';

		char *start = line;
		while (isspace((unsigned char)*start))
			start++;
		size_t n = strcspn(start, "= \t\r\v\f");
		if (!n || n >= YYL_KEY_SIZE)
			continue;
		char *p = start + n;
		while (isspace((unsigned char)*p))
			p++;
		if (*p++ != '=')
			continue;

		char *end;
		errno = 0;
		long parsed = strtol(p, &end, 10);
		if (end == p || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX)
			continue;
		while (isspace((unsigned char)*end))
			end++;
		if (*end)
			continue;
		memcpy(key, start, n);
		key[n] = '\0';
		*value = (int)parsed;
		return 1;
	}
}

#endif
