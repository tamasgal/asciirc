#ifndef ASCIIRC_UTIL_H
#define ASCIIRC_UTIL_H

#include <stddef.h>
#include <time.h>

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} Buf;

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_reserve(Buf *b, size_t extra);
void buf_append(Buf *b, const void *data, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_consume(Buf *b, size_t n);
char *buf_steal(Buf *b);
void buf_json_str(Buf *b, const char *s);

long long now_ms(void);
time_t parse_iso8601(const char *s);
int str_casecmp(const char *a, const char *b);
int strncasecmp_ascii(const char *a, const char *b, size_t n);

#endif
