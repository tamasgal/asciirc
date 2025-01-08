#include "rc.h"
#include "http.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DDP_LOGIN_ID "login"

void rc_init(Rc *rc, const Url *url)
{
	memset(rc, 0, sizeof(*rc));
	rc->url = *url;
	rc->state = DDP_OFFLINE;
	rc->ws.closed = 1;
	rc->next_id = 1;
}

void rc_free(Rc *rc)
{
	rc_ddp_disconnect(rc);
	free(rc->user_id);
	free(rc->token);
	free(rc->username);
}

static void auth_headers(Rc *rc, Buf *h)
{
	if (rc->user_id && rc->token)
		buf_printf(h, "X-User-Id: %s\r\nX-Auth-Token: %s\r\n", rc->user_id, rc->token);
}

static Json *rc_request(Rc *rc, const char *method, const char *path, const char *body, char *err,
			size_t errlen)
{
	Buf h;
	buf_init(&h);
	auth_headers(rc, &h);
	HttpResponse resp;
	int r = http_request(&rc->url, method, path, h.data, body, &resp, err, errlen);
	buf_free(&h);
	if (r < 0)
		return NULL;

	Json *j = json_parse(resp.body.data ? resp.body.data : "", resp.body.len);
	if (resp.status < 200 || resp.status >= 300 || !j ||
	    (json_get(j, "success") && !json_bool(json_get(j, "success")))) {
		const char *msg = json_get_str(j, "error");
		if (!msg)
			msg = json_get_str(j, "message");
		if (msg)
			snprintf(err, errlen, "%s", msg);
		else if (resp.status == 401)
			snprintf(err, errlen, "%s %s: unauthorized (check user id and token)", method, path);
		else
			snprintf(err, errlen, "%s %s: HTTP %d", method, path, resp.status);
		json_free(j);
		j = NULL;
	}
	http_response_free(&resp);
	return j;
}

Json *rc_get(Rc *rc, const char *path, char *err, size_t errlen)
{
	return rc_request(rc, "GET", path, NULL, err, errlen);
}

Json *rc_post(Rc *rc, const char *path, const char *body, char *err, size_t errlen)
{
	return rc_request(rc, "POST", path, body ? body : "{}", err, errlen);
}

int rc_fetch_me(Rc *rc, char *err, size_t errlen)
{
	Json *j = rc_get(rc, "/api/v1/me", err, errlen);
	if (!j)
		return -1;
	const char *name = json_get_str(j, "username");
	if (!name) {
		snprintf(err, errlen, "/api/v1/me returned no username");
		json_free(j);
		return -1;
	}
	free(rc->username);
	rc->username = xstrdup(name);
	json_free(j);
	return 0;
}

void rc_ddp_disconnect(Rc *rc)
{
	if (!rc->ws.closed || rc->ws.conn)
		ws_close(&rc->ws);
	rc->state = DDP_OFFLINE;
}
