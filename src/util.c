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
