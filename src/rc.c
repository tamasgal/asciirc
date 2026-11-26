#include "rc.h"
#include "http.h"
#include "util.h"

#include <openssl/rand.h>

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

static Json *rc_request(Rc *rc, const char *method, const char *path, const char *ctype, const char *body,
			size_t body_len, int *status, char *err, size_t errlen)
{
	Buf h;
	buf_init(&h);
	auth_headers(rc, &h);
	HttpResponse resp;
	int r = http_request(&rc->url, method, path, h.data, ctype, body, body_len, &resp, err, errlen);
	buf_free(&h);
	if (status)
		*status = r < 0 ? 0 : resp.status;
	if (r < 0) {
		char tmp[256];
		snprintf(tmp, sizeof(tmp), "%s", err);
		snprintf(err, errlen, "%.*s: %s", (int)strcspn(path, "?"), path, tmp);
		return NULL;
	}

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
	return rc_request(rc, "GET", path, NULL, NULL, 0, NULL, err, errlen);
}

Json *rc_post(Rc *rc, const char *path, const char *body, char *err, size_t errlen)
{
	if (!body)
		body = "{}";
	return rc_request(rc, "POST", path, "application/json", body, strlen(body), NULL, err, errlen);
}

static void form_field(Buf *b, const char *boundary, const char *name, const char *value)
{
	buf_printf(b, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", boundary, name, value);
}

void rc_multipart(Buf *b, const char *boundary, const RcFile *f, const char *desc, const char *tmid)
{
	buf_printf(b, "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"", boundary);
	for (const char *p = f->name; *p; p++)
		buf_putc(b, *p == '"' || *p == '\r' || *p == '\n' || *p == '\\' ? '_' : *p);
	buf_printf(b, "\"\r\nContent-Type: %s\r\n\r\n", f->mime);
	buf_append(b, f->data, f->len);
	buf_puts(b, "\r\n");
	if (desc && *desc)
		form_field(b, boundary, "description", desc);
	if (tmid)
		form_field(b, boundary, "tmid", tmid);
	buf_printf(b, "--%s--\r\n", boundary);
}

static Json *post_multipart(Rc *rc, const char *path, const RcFile *f, const char *desc, const char *tmid,
			    int *status, char *err, size_t errlen)
{
	unsigned char rnd[12];
	char boundary[64] = "asciirc-";
	if (RAND_bytes(rnd, sizeof(rnd)) != 1)
		memset(rnd, 0x5a, sizeof(rnd));
	for (size_t i = 0; i < sizeof(rnd); i++)
		snprintf(boundary + 8 + 2 * i, 3, "%02x", rnd[i]);
	Buf body, ctype;
	buf_init(&body);
	buf_init(&ctype);
	rc_multipart(&body, boundary, f, desc, tmid);
	buf_printf(&ctype, "multipart/form-data; boundary=%s", boundary);
	Json *j = rc_request(rc, "POST", path, ctype.data, body.data, body.len, status, err, errlen);
	buf_free(&body);
	buf_free(&ctype);
	return j;
}

Json *rc_upload(Rc *rc, const char *rid, const RcFile *f, const char *desc, const char *tmid, char *err,
		size_t errlen)
{
	Buf path;
	buf_init(&path);
	buf_puts(&path, "/api/v1/rooms.media/");
	url_encode(&path, rid);
	int status;
	Json *j = post_multipart(rc, path.data, f, NULL, NULL, &status, err, errlen);
	if (!j && status == 404) {
		buf_free(&path);
		buf_init(&path);
		buf_puts(&path, "/api/v1/rooms.upload/");
		url_encode(&path, rid);
		j = post_multipart(rc, path.data, f, desc, tmid, NULL, err, errlen);
		buf_free(&path);
		return j;
	}
	if (!j) {
		buf_free(&path);
		return NULL;
	}
	const char *fid = json_str(json_path(j, "file._id"));
	if (!fid) {
		snprintf(err, errlen, "upload response without a file id");
		json_free(j);
		buf_free(&path);
		return NULL;
	}
	buf_free(&path);
	buf_init(&path);
	buf_puts(&path, "/api/v1/rooms.mediaConfirm/");
	url_encode(&path, rid);
	buf_putc(&path, '/');
	url_encode(&path, fid);
	json_free(j);
	Buf body;
	buf_init(&body);
	buf_puts(&body, "{\"description\":");
	buf_json_str(&body, desc ? desc : "");
	if (tmid) {
		buf_puts(&body, ",\"tmid\":");
		buf_json_str(&body, tmid);
	}
	buf_putc(&body, '}');
	j = rc_post(rc, path.data, body.data, err, errlen);
	buf_free(&body);
	buf_free(&path);
	return j;
}

int rc_login_password(Rc *rc, const char *user, const char *password, char *err, size_t errlen)
{
	Buf b;
	buf_init(&b);
	buf_puts(&b, "{\"user\":");
	buf_json_str(&b, user);
	buf_puts(&b, ",\"password\":");
	buf_json_str(&b, password);
	buf_puts(&b, "}");
	Json *j = rc_post(rc, "/api/v1/login", b.data, err, errlen);
	memset(b.data, 0, b.len);
	buf_free(&b);
	if (!j)
		return -1;
	const char *uid = json_str(json_path(j, "data.userId"));
	const char *tok = json_str(json_path(j, "data.authToken"));
	if (!uid || !tok) {
		snprintf(err, errlen, "login response without credentials (two-factor authentication is not supported, use a personal access token)");
		json_free(j);
		return -1;
	}
	free(rc->user_id);
	free(rc->token);
	rc->user_id = xstrdup(uid);
	rc->token = xstrdup(tok);
	json_free(j);
	return 0;
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

static int ddp_send(Rc *rc, Buf *b)
{
	return ws_send_text(&rc->ws, b->data, b->len);
}

int rc_ddp_connect(Rc *rc, char *err, size_t errlen)
{
	rc_ddp_disconnect(rc);
	if (ws_connect(&rc->ws, &rc->url, "/websocket", err, errlen) < 0)
		return -1;
	Buf b;
	buf_init(&b);
	buf_puts(&b, "{\"msg\":\"connect\",\"version\":\"1\",\"support\":[\"1\",\"pre2\",\"pre1\"]}");
	int r = ddp_send(rc, &b);
	buf_free(&b);
	if (r < 0) {
		snprintf(err, errlen, "websocket write failed");
		rc_ddp_disconnect(rc);
		return -1;
	}
	rc->state = DDP_CONNECTING;
	return 0;
}

void rc_ddp_disconnect(Rc *rc)
{
	if (!rc->ws.closed || rc->ws.conn)
		ws_close(&rc->ws);
	rc->state = DDP_OFFLINE;
}

int rc_ddp_fd(const Rc *rc)
{
	return rc->ws.conn && !rc->ws.closed ? net_fd(rc->ws.conn) : -1;
}

static void ddp_subscribe(Rc *rc, const char *name, const char *event)
{
	Buf b;
	buf_init(&b);
	buf_printf(&b, "{\"msg\":\"sub\",\"id\":\"sub%u\",\"name\":", rc->next_id++);
	buf_json_str(&b, name);
	buf_puts(&b, ",\"params\":[");
	buf_json_str(&b, event);
	buf_puts(&b, ",{\"useCollection\":false,\"args\":[]}]}");
	ddp_send(rc, &b);
	buf_free(&b);
}

static void ddp_handle(Rc *rc, const Json *m, const RcHandlers *h, void *ctx)
{
	const char *type = json_get_str(m, "msg");
	if (!type)
		return;
	if (strcmp(type, "ping") == 0) {
		Buf b;
		buf_init(&b);
		const char *id = json_get_str(m, "id");
		if (id) {
			buf_puts(&b, "{\"msg\":\"pong\",\"id\":");
			buf_json_str(&b, id);
			buf_puts(&b, "}");
		} else {
			buf_puts(&b, "{\"msg\":\"pong\"}");
		}
		ddp_send(rc, &b);
		buf_free(&b);
	} else if (strcmp(type, "connected") == 0) {
		Buf b;
		buf_init(&b);
		/* resume works for personal access tokens too */
		buf_puts(&b, "{\"msg\":\"method\",\"method\":\"login\",\"id\":\"" DDP_LOGIN_ID "\",\"params\":[{\"resume\":");
		buf_json_str(&b, rc->token);
		buf_puts(&b, "}]}");
		ddp_send(rc, &b);
		memset(b.data, 0, b.len);
		buf_free(&b);
		rc->state = DDP_LOGGING_IN;
	} else if (strcmp(type, "failed") == 0) {
		h->status(ctx, 1, "realtime connection refused: unsupported DDP version");
		rc_ddp_disconnect(rc);
	} else if (strcmp(type, "result") == 0) {
		const char *id = json_get_str(m, "id");
		if (id && id[0] == 'm' && h->result) {
			const Json *e = json_get(m, "error");
			const char *reason = json_get_str(e, "reason");
			if (!reason)
				reason = json_get_str(e, "message");
			h->result(ctx, (unsigned)strtoul(id + 1, NULL, 10), json_get(m, "result"),
				  e ? (reason ? reason : "unknown error") : NULL);
			return;
		}
		if (!id || strcmp(id, DDP_LOGIN_ID) != 0)
			return;
		const Json *e = json_get(m, "error");
		if (e) {
			char tmp[512];
			const char *reason = json_get_str(e, "reason");
			if (!reason)
				reason = json_get_str(e, "message");
			snprintf(tmp, sizeof(tmp), "realtime login failed: %s", reason ? reason : "unknown error");
			h->status(ctx, 1, tmp);
			rc_ddp_disconnect(rc);
			return;
		}
		char ev[256];
		ddp_subscribe(rc, "stream-room-messages", "__my_messages__");
		snprintf(ev, sizeof(ev), "%s/subscriptions-changed", rc->user_id);
		ddp_subscribe(rc, "stream-notify-user", ev);
		snprintf(ev, sizeof(ev), "%s/rooms-changed", rc->user_id);
		ddp_subscribe(rc, "stream-notify-user", ev);
		rc->state = DDP_READY;
		h->ready(ctx);
	} else if (strcmp(type, "nosub") == 0) {
		const Json *e = json_get(m, "error");
		if (e) {
			char tmp[512];
			const char *reason = json_get_str(e, "reason");
			snprintf(tmp, sizeof(tmp), "subscription rejected: %s", reason ? reason : "unknown error");
			h->status(ctx, 1, tmp);
		}
	} else if (strcmp(type, "changed") == 0) {
		const char *coll = json_get_str(m, "collection");
		const Json *fields = json_get(m, "fields");
		const char *event = json_get_str(fields, "eventName");
		const Json *args = json_get(fields, "args");
		if (coll && event && args)
			h->stream(ctx, coll, event, args);
	} else if (strcmp(type, "error") == 0) {
		const char *reason = json_get_str(m, "reason");
		char tmp[512];
		snprintf(tmp, sizeof(tmp), "realtime protocol error: %s", reason ? reason : "unknown");
		h->status(ctx, 1, tmp);
	}
}

int rc_ddp_process(Rc *rc, const RcHandlers *h, void *ctx)
{
	int alive = ws_fill(&rc->ws) == 0;
	for (;;) {
		char *msg;
		size_t len;
		int r = ws_next_message(&rc->ws, &msg, &len);
		if (r == 0)
			break;
		if (r < 0) {
			alive = 0;
			break;
		}
		Json *j = json_parse(msg, len);
		free(msg);
		if (j)
			ddp_handle(rc, j, h, ctx);
		json_free(j);
		if (rc->state == DDP_OFFLINE)
			return -1;
	}
	if (!alive) {
		rc_ddp_disconnect(rc);
		return -1;
	}
	return 0;
}

unsigned rc_ddp_call(Rc *rc, const char *method, const char *params)
{
	if (rc->state != DDP_READY)
		return 0;
	unsigned id = rc->next_id++;
	Buf b;
	buf_init(&b);
	buf_printf(&b, "{\"msg\":\"method\",\"id\":\"m%u\",\"method\":", id);
	buf_json_str(&b, method);
	buf_printf(&b, ",\"params\":%s}", params);
	ddp_send(rc, &b);
	buf_free(&b);
	return id;
}
