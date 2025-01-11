#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_MAX_RESPONSE (64u * 1024 * 1024)

void url_encode(Buf *b, const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			buf_putc(b, (char)c);
		else
			buf_printf(b, "%%%02X", c);
	}
}

static const char *find_header(const char *hdrs, size_t hlen, const char *name)
{
	size_t nlen = strlen(name);
	const char *p = hdrs;
	const char *end = hdrs + hlen;
	while (p < end) {
		const char *eol = strstr(p, "\r\n");
		if (!eol || eol > end)
			eol = end;
		if ((size_t)(eol - p) > nlen && p[nlen] == ':') {
			size_t i;
			for (i = 0; i < nlen; i++)
				if (tolower((unsigned char)p[i]) != tolower((unsigned char)name[i]))
					break;
			if (i == nlen) {
				const char *v = p + nlen + 1;
				while (*v == ' ' || *v == '\t')
					v++;
				return v;
			}
		}
		p = eol + 2;
	}
	return NULL;
}

static int dechunk(const char *p, size_t n, Buf *out)
{
	const char *end = p + n;
	for (;;) {
		char *endp;
		unsigned long sz = strtoul(p, &endp, 16);
		if (endp == p)
			return -1;
		const char *eol = strstr(endp, "\r\n");
		if (!eol || eol + 2 > end)
			return -1;
		p = eol + 2;
		if (sz == 0)
			return 0;
		if ((size_t)(end - p) < sz + 2)
			return -1;
		buf_append(out, p, sz);
		p += sz + 2;
	}
}

int http_request(const Url *u, const char *method, const char *path, const char *extra_headers,
		 const char *json_body, HttpResponse *resp, char *err, size_t errlen)
{
	resp->status = 0;
	buf_init(&resp->body);

	Conn *c = net_connect(u, err, errlen);
	if (!c)
		return -1;

	int default_port = strcmp(u->port, u->tls ? "443" : "80") == 0;
	Buf req;
	buf_init(&req);
	buf_printf(&req, "%s %s%s HTTP/1.1\r\n", method, u->path, path);
	if (default_port)
		buf_printf(&req, "Host: %s\r\n", u->host);
	else
		buf_printf(&req, "Host: %s:%s\r\n", u->host, u->port);
	buf_puts(&req, "User-Agent: asciirc\r\nAccept: application/json\r\nConnection: close\r\n");
	if (extra_headers)
		buf_puts(&req, extra_headers);
	if (json_body) {
		buf_printf(&req, "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
			   strlen(json_body));
		buf_puts(&req, json_body);
	} else {
		buf_puts(&req, "\r\n");
	}
	int wr = net_write_all(c, req.data, req.len);
	buf_free(&req);
	if (wr < 0) {
		snprintf(err, errlen, "write failed");
		net_close(c);
		return -1;
	}

	Buf raw;
	buf_init(&raw);
	char tmp[16384];
	for (;;) {
		ssize_t r = net_read(c, tmp, sizeof(tmp));
		if (r == 0)
			break;
		if (r < 0) {
			/* some servers drop tls without close_notify */
			if (raw.len > 0)
				break;
			snprintf(err, errlen, "read failed");
			buf_free(&raw);
			net_close(c);
			return -1;
		}
		buf_append(&raw, tmp, (size_t)r);
		if (raw.len > HTTP_MAX_RESPONSE) {
			snprintf(err, errlen, "response too large");
			buf_free(&raw);
			net_close(c);
			return -1;
		}
	}
	net_close(c);

	const char *hend = raw.data ? strstr(raw.data, "\r\n\r\n") : NULL;
	if (!hend || sscanf(raw.data, "HTTP/%*d.%*d %d", &resp->status) != 1) {
		snprintf(err, errlen, "malformed HTTP response");
		buf_free(&raw);
		return -1;
	}
	size_t hlen = (size_t)(hend - raw.data);
	const char *body = hend + 4;
	size_t blen = raw.len - hlen - 4;
	const char *te = find_header(raw.data, hlen, "Transfer-Encoding");
	if (te && strncmp(te, "chunked", 7) == 0) {
		if (dechunk(body, blen, &resp->body) < 0) {
			snprintf(err, errlen, "malformed chunked response");
			buf_free(&raw);
			buf_free(&resp->body);
			return -1;
		}
	} else {
		const char *cl = find_header(raw.data, hlen, "Content-Length");
		if (cl) {
			size_t want = (size_t)strtoull(cl, NULL, 10);
			if (want < blen)
				blen = want;
		}
		buf_append(&resp->body, body, blen);
	}
	buf_free(&raw);
	return 0;
}

void http_response_free(HttpResponse *resp)
{
	buf_free(&resp->body);
}
