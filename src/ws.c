#include "ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MAX_MESSAGE (32u * 1024 * 1024)

enum { OP_CONT = 0, OP_TEXT = 1, OP_BINARY = 2, OP_CLOSE = 8, OP_PING = 9, OP_PONG = 10 };

static int send_frame(WebSocket *ws, int opcode, const void *data, size_t n)
{
	unsigned char hdr[14];
	size_t h = 0;
	hdr[h++] = (unsigned char)(0x80 | opcode);
	if (n < 126) {
		hdr[h++] = (unsigned char)(0x80 | n);
	} else if (n < 65536) {
		hdr[h++] = 0x80 | 126;
		hdr[h++] = (unsigned char)(n >> 8);
		hdr[h++] = (unsigned char)n;
	} else {
		hdr[h++] = 0x80 | 127;
		for (int i = 7; i >= 0; i--)
			hdr[h++] = (unsigned char)((unsigned long long)n >> (8 * i));
	}
	unsigned char mask[4];
	if (RAND_bytes(mask, 4) != 1)
		return -1;
	memcpy(hdr + h, mask, 4);
	h += 4;

	Buf out;
	buf_init(&out);
	buf_append(&out, hdr, h);
	buf_reserve(&out, n);
	const unsigned char *src = data;
	for (size_t i = 0; i < n; i++)
		out.data[out.len + i] = (char)(src[i] ^ mask[i & 3]);
	out.len += n;
	int r = net_write_all(ws->conn, out.data, out.len);
	buf_free(&out);
	return r;
}

int ws_connect(WebSocket *ws, const Url *u, const char *path, char *err, size_t errlen)
{
	memset(ws, 0, sizeof(*ws));
	buf_init(&ws->in);
	buf_init(&ws->frag);

	unsigned char nonce[16];
	char key[32];
	if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
		snprintf(err, errlen, "no randomness available");
		return -1;
	}
	EVP_EncodeBlock((unsigned char *)key, nonce, sizeof(nonce));

	ws->conn = net_connect(u, err, errlen);
	if (!ws->conn)
		return -1;

	int default_port = strcmp(u->port, u->tls ? "443" : "80") == 0;
	Buf req;
	buf_init(&req);
	buf_printf(&req,
		   "GET %s%s HTTP/1.1\r\n"
		   "Host: %s%s%s\r\n"
		   "User-Agent: asciirc\r\n"
		   "Upgrade: websocket\r\n"
		   "Connection: Upgrade\r\n"
		   "Sec-WebSocket-Key: %s\r\n"
		   "Sec-WebSocket-Version: 13\r\n\r\n",
		   u->path, path, u->host, default_port ? "" : ":", default_port ? "" : u->port, key);
	int wr = net_write_all(ws->conn, req.data, req.len);
	buf_free(&req);
	if (wr < 0) {
		snprintf(err, errlen, "websocket handshake write failed");
		goto fail;
	}

	char *hend = NULL;
	char tmp[4096];
	while (!hend) {
		ssize_t r = net_read(ws->conn, tmp, sizeof(tmp));
		if (r <= 0) {
			snprintf(err, errlen, "websocket handshake: connection closed");
			goto fail;
		}
		buf_append(&ws->in, tmp, (size_t)r);
		hend = strstr(ws->in.data, "\r\n\r\n");
		if (!hend && ws->in.len > 65536) {
			snprintf(err, errlen, "websocket handshake: headers too large");
			goto fail;
		}
	}
	int status = 0;
	sscanf(ws->in.data, "HTTP/%*d.%*d %d", &status);
	if (status != 101) {
		snprintf(err, errlen, "websocket handshake: unexpected HTTP status %d", status);
		goto fail;
	}

	char expect_src[64];
	unsigned char digest[SHA_DIGEST_LENGTH];
	char expect[64];
	snprintf(expect_src, sizeof(expect_src), "%s%s", key, WS_GUID);
	SHA1((const unsigned char *)expect_src, strlen(expect_src), digest);
	EVP_EncodeBlock((unsigned char *)expect, digest, SHA_DIGEST_LENGTH);
	*hend = '\0';
	int ok = 0;
	for (char *line = strstr(ws->in.data, "\r\n"); line; line = strstr(line, "\r\n")) {
		line += 2;
		if (strncasecmp(line, "Sec-WebSocket-Accept:", 21) == 0) {
			char *v = line + 21;
			while (*v == ' ')
				v++;
			ok = strncmp(v, expect, strlen(expect)) == 0;
			break;
		}
	}
	if (!ok) {
		snprintf(err, errlen, "websocket handshake: bad Sec-WebSocket-Accept");
		goto fail;
	}
	buf_consume(&ws->in, (size_t)(hend - ws->in.data) + 4);
	net_set_nonblocking(ws->conn);
	return 0;

fail:
	ws->closed = 1;
	ws_close(ws);
	return -1;
}

void ws_close(WebSocket *ws)
{
	if (ws->conn && !ws->closed)
		send_frame(ws, OP_CLOSE, "\x03\xe8", 2);
	net_close(ws->conn);
	ws->conn = NULL;
	ws->closed = 1;
	buf_free(&ws->in);
	buf_free(&ws->frag);
}

int ws_send_text(WebSocket *ws, const char *s, size_t n)
{
	if (!ws->conn || ws->closed)
		return -1;
	return send_frame(ws, OP_TEXT, s, n);
}

int ws_fill(WebSocket *ws)
{
	if (!ws->conn || ws->closed)
		return -1;
	char tmp[16384];
	for (;;) {
		ssize_t r = net_read(ws->conn, tmp, sizeof(tmp));
		if (r == NET_AGAIN)
			return 0;
		if (r <= 0) {
			ws->closed = 1;
			return -1;
		}
		buf_append(&ws->in, tmp, (size_t)r);
	}
}

int ws_next_message(WebSocket *ws, char **msg, size_t *len)
{
	for (;;) {
		const unsigned char *p = (const unsigned char *)ws->in.data;
		size_t avail = ws->in.len;
		if (avail < 2)
			return 0;
		int fin = p[0] & 0x80;
		int opcode = p[0] & 0x0f;
		int masked = p[1] & 0x80;
		unsigned long long plen = p[1] & 0x7f;
		size_t h = 2;
		if (plen == 126) {
			if (avail < 4)
				return 0;
			plen = ((unsigned long long)p[2] << 8) | p[3];
			h = 4;
		} else if (plen == 127) {
			if (avail < 10)
				return 0;
			plen = 0;
			for (int i = 0; i < 8; i++)
				plen = (plen << 8) | p[2 + i];
			h = 10;
		}
		if (plen > WS_MAX_MESSAGE)
			return -1;
		unsigned char mask[4] = { 0, 0, 0, 0 };
		if (masked) {
			if (avail < h + 4)
				return 0;
			memcpy(mask, p + h, 4);
			h += 4;
		}
		if (avail < h + plen)
			return 0;

		size_t n = (size_t)plen;
		char *payload = xmalloc(n + 1);
		for (size_t i = 0; i < n; i++)
			payload[i] = (char)(p[h + i] ^ mask[i & 3]);
		payload[n] = '\0';
		buf_consume(&ws->in, h + n);

		switch (opcode) {
		case OP_PING:
			send_frame(ws, OP_PONG, payload, n);
			free(payload);
			continue;
		case OP_PONG:
			free(payload);
			continue;
		case OP_CLOSE:
			free(payload);
			ws->closed = 1;
			return -1;
		case OP_TEXT:
		case OP_BINARY:
			if (fin) {
				*msg = payload;
				*len = n;
				return 1;
			}
			ws->frag.len = 0;
			buf_append(&ws->frag, payload, n);
			ws->frag_active = 1;
			free(payload);
			continue;
		case OP_CONT:
			if (!ws->frag_active) {
				free(payload);
				return -1;
			}
			buf_append(&ws->frag, payload, n);
			free(payload);
			if (ws->frag.len > WS_MAX_MESSAGE)
				return -1;
			if (fin) {
				ws->frag_active = 0;
				*len = ws->frag.len;
				*msg = buf_steal(&ws->frag);
				return 1;
			}
			continue;
		default:
			free(payload);
			return -1;
		}
	}
}
