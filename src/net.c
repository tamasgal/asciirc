#include "net.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

struct Conn {
	int fd;
	SSL *ssl;
};

static SSL_CTX *ssl_ctx;

int url_parse(const char *s, Url *u)
{
	memset(u, 0, sizeof(*u));
	if (strncmp(s, "https://", 8) == 0) {
		u->tls = 1;
		s += 8;
	} else if (strncmp(s, "http://", 7) == 0) {
		s += 7;
	} else if (strstr(s, "://")) {
		return -1;
	} else {
		u->tls = 1;
	}
	const char *slash = strchr(s, '/');
	size_t hostlen = slash ? (size_t)(slash - s) : strlen(s);
	const char *colon = memchr(s, ':', hostlen);
	size_t namelen = colon ? (size_t)(colon - s) : hostlen;
	if (namelen == 0 || namelen >= sizeof(u->host))
		return -1;
	memcpy(u->host, s, namelen);
	if (colon) {
		size_t plen = hostlen - namelen - 1;
		if (plen == 0 || plen >= sizeof(u->port))
			return -1;
		memcpy(u->port, colon + 1, plen);
	} else {
		strcpy(u->port, u->tls ? "443" : "80");
	}
	if (slash) {
		size_t plen = strlen(slash);
		while (plen > 0 && slash[plen - 1] == '/')
			plen--;
		if (plen >= sizeof(u->path))
			return -1;
		memcpy(u->path, slash, plen);
	}
	return 0;
}

static int ensure_ctx(void)
{
	if (ssl_ctx)
		return 0;
	ssl_ctx = SSL_CTX_new(TLS_client_method());
	if (!ssl_ctx)
		return -1;
	SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);
	SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
	SSL_CTX_set_default_verify_paths(ssl_ctx);
	return 0;
}

static void set_ssl_err(char *err, size_t errlen, const char *what)
{
	unsigned long e = ERR_get_error();
	char tmp[256];
	if (e) {
		ERR_error_string_n(e, tmp, sizeof(tmp));
		snprintf(err, errlen, "%s: %s", what, tmp);
	} else {
		snprintf(err, errlen, "%s", what);
	}
	ERR_clear_error();
}

Conn *net_connect(const Url *u, char *err, size_t errlen)
{
	struct addrinfo hints, *res, *ai;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	int rc = getaddrinfo(u->host, u->port, &hints, &res);
	if (rc != 0) {
		snprintf(err, errlen, "%s: %s", u->host, gai_strerror(rc));
		return NULL;
	}
	int fd = -1;
	int saved = 0;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) {
			saved = errno;
			continue;
		}
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		saved = errno;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		snprintf(err, errlen, "connect %s:%s: %s", u->host, u->port, strerror(saved));
		return NULL;
	}
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

	Conn *c = xcalloc(1, sizeof(*c));
	c->fd = fd;
	if (!u->tls)
		return c;

	if (ensure_ctx() < 0) {
		set_ssl_err(err, errlen, "TLS init");
		net_close(c);
		return NULL;
	}
	c->ssl = SSL_new(ssl_ctx);
	SSL_set_fd(c->ssl, fd);
	SSL_set_tlsext_host_name(c->ssl, u->host);
	if (u->insecure)
		SSL_set_verify(c->ssl, SSL_VERIFY_NONE, NULL);
	else
		X509_VERIFY_PARAM_set1_host(SSL_get0_param(c->ssl), u->host, 0);
	if (SSL_connect(c->ssl) != 1) {
		long vr = SSL_get_verify_result(c->ssl);
		if (vr != X509_V_OK) {
			snprintf(err, errlen, "TLS certificate verification failed: %s (insecure = 1 or -k skips the check)",
				 X509_verify_cert_error_string(vr));
			ERR_clear_error();
		} else {
			set_ssl_err(err, errlen, "TLS handshake failed");
		}
		net_close(c);
		return NULL;
	}
	return c;
}

void net_close(Conn *c)
{
	if (!c)
		return;
	if (c->ssl) {
		SSL_shutdown(c->ssl);
		SSL_free(c->ssl);
	}
	if (c->fd >= 0)
		close(c->fd);
	free(c);
}

int net_fd(const Conn *c)
{
	return c->fd;
}

int net_set_nonblocking(Conn *c)
{
	int fl = fcntl(c->fd, F_GETFL);
	if (fl < 0)
		return -1;
	return fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
}

ssize_t net_read(Conn *c, void *buf, size_t n)
{
	if (!c->ssl) {
		for (;;) {
			ssize_t r = read(c->fd, buf, n);
			if (r >= 0)
				return r;
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return NET_AGAIN;
			return -1;
		}
	}
	int r = SSL_read(c->ssl, buf, n > 0x7fffffff ? 0x7fffffff : (int)n);
	if (r > 0)
		return r;
	switch (SSL_get_error(c->ssl, r)) {
	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
		return NET_AGAIN;
	case SSL_ERROR_ZERO_RETURN:
		return 0;
	case SSL_ERROR_SYSCALL:
		ERR_clear_error();
		return r == 0 ? 0 : -1;
	default:
		ERR_clear_error();
		return -1;
	}
}

static int wait_fd(int fd, short events)
{
	struct pollfd p = { fd, events, 0 };
	int r;
	do
		r = poll(&p, 1, 30000);
	while (r < 0 && errno == EINTR);
	return r > 0 ? 0 : -1;
}

int net_write_all(Conn *c, const void *buf, size_t n)
{
	const char *p = buf;
	while (n > 0) {
		if (c->ssl) {
			int w = SSL_write(c->ssl, p, n > 0x7fffffff ? 0x7fffffff : (int)n);
			if (w > 0) {
				p += w;
				n -= (size_t)w;
				continue;
			}
			int e = SSL_get_error(c->ssl, w);
			ERR_clear_error();
			if (e == SSL_ERROR_WANT_WRITE && wait_fd(c->fd, POLLOUT) == 0)
				continue;
			if (e == SSL_ERROR_WANT_READ && wait_fd(c->fd, POLLIN) == 0)
				continue;
			return -1;
		}
		ssize_t w = send(c->fd, p, n, 0);
		if (w > 0) {
			p += w;
			n -= (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_fd(c->fd, POLLOUT) == 0)
			continue;
		return -1;
	}
	return 0;
}

int net_pending(const Conn *c)
{
	return c->ssl ? SSL_pending(c->ssl) : 0;
}
