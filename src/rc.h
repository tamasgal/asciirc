#ifndef ASCIIRC_RC_H
#define ASCIIRC_RC_H

#include "json.h"
#include "net.h"
#include "ws.h"

typedef enum {
	DDP_OFFLINE,
	DDP_CONNECTING,
	DDP_LOGGING_IN,
	DDP_READY
} DdpState;

typedef struct {
	Url url;
	char *user_id;
	char *token;
	char *username;
	WebSocket ws;
	DdpState state;
	unsigned next_id;
} Rc;

typedef struct {
	void (*stream)(void *ctx, const char *collection, const char *event, const Json *args);
	void (*status)(void *ctx, int is_error, const char *text);
	void (*ready)(void *ctx);
	void (*result)(void *ctx, unsigned id, const Json *result, const char *error);
} RcHandlers;

void rc_init(Rc *rc, const Url *url);
void rc_free(Rc *rc);

int rc_login_password(Rc *rc, const char *user, const char *password, char *err, size_t errlen);
int rc_fetch_me(Rc *rc, char *err, size_t errlen);

Json *rc_get(Rc *rc, const char *path, char *err, size_t errlen);
Json *rc_post(Rc *rc, const char *path, const char *body, char *err, size_t errlen);

int rc_ddp_connect(Rc *rc, char *err, size_t errlen);
void rc_ddp_disconnect(Rc *rc);
int rc_ddp_fd(const Rc *rc);
int rc_ddp_process(Rc *rc, const RcHandlers *h, void *ctx);
unsigned rc_ddp_call(Rc *rc, const char *method, const char *params);

#endif
