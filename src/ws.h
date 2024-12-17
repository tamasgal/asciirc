#ifndef ASCIIRC_WS_H
#define ASCIIRC_WS_H

#include "net.h"
#include "util.h"

typedef struct {
	Conn *conn;
	Buf in;
	Buf frag;
	int frag_active;
	int closed;
} WebSocket;

int ws_connect(WebSocket *ws, const Url *u, const char *path, char *err, size_t errlen);
void ws_close(WebSocket *ws);
int ws_send_text(WebSocket *ws, const char *s, size_t n);
int ws_fill(WebSocket *ws);
int ws_next_message(WebSocket *ws, char **msg, size_t *len);

#endif
