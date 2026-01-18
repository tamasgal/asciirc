#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) {
		fputs("out of memory\n", stderr);
		abort();
	}
	return p;
}

void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (!p) {
		fputs("out of memory\n", stderr);
		abort();
	}
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p) {
		fputs("out of memory\n", stderr);
		abort();
	}
	return p;
}

char *xstrdup(const char *s)
{
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

void buf_init(Buf *b)
{
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

void buf_free(Buf *b)
{
	free(b->data);
	buf_init(b);
}

void buf_reserve(Buf *b, size_t extra)
{
	size_t need = b->len + extra + 1;
	if (need <= b->cap)
		return;
	size_t cap = b->cap ? b->cap : 64;
	while (cap < need)
		cap *= 2;
	b->data = xrealloc(b->data, cap);
	b->cap = cap;
}

void buf_append(Buf *b, const void *data, size_t n)
{
	buf_reserve(b, n);
	if (n)
		memcpy(b->data + b->len, data, n);
	b->len += n;
	b->data[b->len] = '\0';
}

void buf_puts(Buf *b, const char *s)
{
	buf_append(b, s, strlen(s));
}

void buf_putc(Buf *b, char c)
{
	buf_append(b, &c, 1);
}

void buf_printf(Buf *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	buf_reserve(b, (size_t)n);
	va_start(ap, fmt);
	vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

void buf_consume(Buf *b, size_t n)
{
	if (n >= b->len) {
		b->len = 0;
	} else {
		memmove(b->data, b->data + n, b->len - n);
		b->len -= n;
	}
	if (b->data)
		b->data[b->len] = '\0';
}

char *buf_steal(Buf *b)
{
	char *p = b->data ? b->data : xstrdup("");
	buf_init(b);
	return p;
}

void buf_json_str(Buf *b, const char *s)
{
	buf_putc(b, '"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '"': buf_puts(b, "\\\""); break;
		case '\\': buf_puts(b, "\\\\"); break;
		case '\n': buf_puts(b, "\\n"); break;
		case '\r': buf_puts(b, "\\r"); break;
		case '\t': buf_puts(b, "\\t"); break;
		default:
			if (c < 0x20)
				buf_printf(b, "\\u%04x", c);
			else
				buf_putc(b, (char)c);
		}
	}
	buf_putc(b, '"');
}

long long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static long long days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	long long era = (y >= 0 ? y : y - 399) / 400;
	int yoe = y - (int)(era * 400);
	int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

time_t parse_iso8601(const char *s)
{
	int y, mo, d, h = 0, mi = 0, sec = 0;
	if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3)
		return 0;
	long long days = days_from_civil(y, mo, d);
	return (time_t)(days * 86400 + h * 3600 + mi * 60 + sec);
}

int str_casecmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb || !ca)
			return ca - cb;
	}
}

int strncasecmp_ascii(const char *a, const char *b, size_t n)
{
	for (; n > 0; a++, b++, n--) {
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb || !ca)
			return ca - cb;
	}
	return 0;
}
