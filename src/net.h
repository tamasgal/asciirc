#ifndef ASCIIRC_NET_H
#define ASCIIRC_NET_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
	int tls;
	char host[256];
	char port[8];
	char path[512];
	int insecure;
} Url;

typedef struct Conn Conn;

#define NET_AGAIN (-2)

int url_parse(const char *s, Url *u);

Conn *net_connect(const Url *u, char *err, size_t errlen);
void net_close(Conn *c);
int net_fd(const Conn *c);
int net_set_nonblocking(Conn *c);
ssize_t net_read(Conn *c, void *buf, size_t n);
int net_write_all(Conn *c, const void *buf, size_t n);
int net_pending(const Conn *c);

#endif
