#include "app.h"
#include "http.h"
#include "ui.h"
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINES 5000
#define HISTORY_COUNT 50
#define RECONNECT_MAX 60

typedef struct {
	char err[512];
} Err;

static void line_free(Line *ln)
{
	free(ln->nick);
	free(ln->name);
	free(ln->text);
	free(ln->msgid);
	free(ln->raw);
	free(ln->tmid);
}

static Window *win_new(App *app, Server *srv, const char *rid, const char *name, char type)
{
	Window *w = xcalloc(1, sizeof(*w));
	w->srv = srv;
	w->rid = rid ? xstrdup(rid) : NULL;
	w->name = ui_sanitize(name);
	w->type = type;
	app->wins = xrealloc(app->wins, (app->nwins + 1) * sizeof(*app->wins));
	app->wins[app->nwins++] = w;
	app->dirty = 1;
	return w;
}

static void win_free(Window *w)
{
	for (size_t i = 0; i < w->nlines; i++)
		line_free(&w->lines[i]);
	free(w->lines);
	free(w->rid);
	free(w->name);
	free(w->label);
	free(w->topic);
	free(w);
}

static size_t win_index(const App *app, const Window *w)
{
	for (size_t i = 0; i < app->nwins; i++)
		if (app->wins[i] == w)
			return i;
	return 0;
}

static Window *win_by_rid(App *app, Server *srv, const char *rid)
{
	for (size_t i = 0; i < app->nwins; i++)
		if (app->wins[i]->srv == srv && app->wins[i]->rid && strcmp(app->wins[i]->rid, rid) == 0)
			return app->wins[i];
	return NULL;
}

static Window *win_by_name(App *app, Server *srv, const char *name)
{
	const char *bare = name[0] == '#' ? name + 1 : name;
	for (size_t i = 0; i < app->nwins; i++) {
		const char *wn = app->wins[i]->name;
		if (app->wins[i]->srv == srv && app->wins[i]->rid && str_casecmp(wn[0] == '#' ? wn + 1 : wn, bare) == 0)
			return app->wins[i];
	}
	return NULL;
}

static Window *current(App *app)
{
	return app->wins[app->cur];
}

static Server *cur_srv(App *app)
{
	return win_server(app, current(app));
}

static void load_history(App *app, Window *w);

static void reset_modes(App *app)
{
	free(app->sel_id);
	app->sel_id = NULL;
	free(app->reply_tmid);
	app->reply_tmid = NULL;
	free(app->reply_to);
	app->reply_to = NULL;
	app->react_active = 0;
	app->scroll_mode = 0;
	app->search_active = 0;
	app->search_text[0] = '\0';
	app->search_hit = 0;
	app->search_reveal = 0;
	app->search_failed = 0;
}

static void win_remove(App *app, Window *w)
{
	size_t idx = win_index(app, w);
	if (idx == 0)
		return;
	reset_modes(app);
	win_free(w);
	memmove(app->wins + idx, app->wins + idx + 1, (app->nwins - idx - 1) * sizeof(*app->wins));
	app->nwins--;
	if (app->cur >= idx && app->cur > 0)
		app->cur--;
	Window *c = current(app);
	c->activity = ACT_NONE;
	c->unread = 0;
	if (!c->history_loaded)
		load_history(app, c);
	app->dirty = 1;
}

static void win_append(App *app, Window *w, Line *ln, int notify)
{
	if (w->nlines == w->cap) {
		w->cap = w->cap ? w->cap * 2 : 64;
		w->lines = xrealloc(w->lines, w->cap * sizeof(*w->lines));
	}
	w->lines[w->nlines++] = *ln;
	if (w->nlines > MAX_LINES) {
		size_t drop = w->nlines - MAX_LINES;
		for (size_t i = 0; i < drop; i++)
			line_free(&w->lines[i]);
		memmove(w->lines, w->lines + drop, MAX_LINES * sizeof(*w->lines));
		w->nlines = MAX_LINES;
	}
	if (notify && !ln->own) {
		Activity a = ACT_EVENT;
		if (ln->highlight)
			a = ACT_HIGHLIGHT;
		else if (ln->kind == LINE_MSG || ln->kind == LINE_ACTION)
			a = ACT_MSG;
		if (w != current(app)) {
			if (a > w->activity)
				w->activity = a;
			if (a >= ACT_MSG)
				w->unread++;
		}
		if (a >= ACT_MSG && w->rid)
			w->needs_read = 1;
	}
	app->dirty = 1;
}

static void add_text(App *app, Window *w, LineKind kind, const char *fmt, va_list ap)
{
	char tmp[4096];
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	Line ln;
	memset(&ln, 0, sizeof(ln));
	ln.ts = time(NULL);
	ln.kind = kind;
	ln.text = ui_sanitize(tmp);
	win_append(app, w ? w : app->wins[0], &ln, 0);
}

void app_info(App *app, Window *w, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	add_text(app, w, LINE_INFO, fmt, ap);
	va_end(ap);
}

void app_error(App *app, Window *w, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	add_text(app, w, LINE_ERROR, fmt, ap);
	va_end(ap);
}

static void srv_msg(Server *srv, LineKind kind, const char *fmt, ...)
{
	char tmp[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (srv->app->nservers > 1)
		(kind == LINE_ERROR ? app_error : app_info)(srv->app, NULL, "[%s] %s", srv->name, tmp);
	else
		(kind == LINE_ERROR ? app_error : app_info)(srv->app, NULL, "%s", tmp);
}

static Line *find_line(Window *w, const char *msgid)
{
	for (size_t i = w->nlines; i > 0; i--)
		if (w->lines[i - 1].msgid && strcmp(w->lines[i - 1].msgid, msgid) == 0)
			return &w->lines[i - 1];
	return NULL;
}

static void base_url(const Url *u, Buf *b)
{
	int def = strcmp(u->port, u->tls ? "443" : "80") == 0;
	buf_printf(b, "%s://%s%s%s%s", u->tls ? "https" : "http", u->host, def ? "" : ":", def ? "" : u->port,
		   u->path);
}

static void describe_event(Buf *b, const char *t, const char *user, const char *msg)
{
	if (strcmp(t, "uj") == 0 || strcmp(t, "ujt") == 0)
		buf_printf(b, "%s has joined", user);
	else if (strcmp(t, "ul") == 0 || strcmp(t, "ult") == 0)
		buf_printf(b, "%s has left", user);
	else if (strcmp(t, "au") == 0 || strcmp(t, "added-user-to-team") == 0)
		buf_printf(b, "%s added %s", user, msg);
	else if (strcmp(t, "ru") == 0 || strcmp(t, "removed-user-from-team") == 0)
		buf_printf(b, "%s removed %s", user, msg);
	else if (strcmp(t, "r") == 0)
		buf_printf(b, "%s changed the room name to %s", user, msg);
	else if (strcmp(t, "room_changed_topic") == 0)
		buf_printf(b, "%s changed the topic to: %s", user, msg);
	else if (strcmp(t, "room_changed_description") == 0)
		buf_printf(b, "%s changed the description to: %s", user, msg);
	else if (strcmp(t, "room_changed_announcement") == 0)
		buf_printf(b, "%s changed the announcement to: %s", user, msg);
	else if (strcmp(t, "user-muted") == 0)
		buf_printf(b, "%s muted %s", user, msg);
	else if (strcmp(t, "user-unmuted") == 0)
		buf_printf(b, "%s unmuted %s", user, msg);
	else if (strcmp(t, "subscription-role-added") == 0)
		buf_printf(b, "%s was given a role by %s", msg, user);
	else if (strcmp(t, "subscription-role-removed") == 0)
		buf_printf(b, "%s lost a role, removed by %s", msg, user);
	else if (strcmp(t, "message_pinned") == 0)
		buf_printf(b, "%s pinned a message", user);
	else if (strcmp(t, "discussion-created") == 0)
		buf_printf(b, "%s started a discussion: %s", user, msg);
	else if (strcmp(t, "rm") == 0)
		buf_printf(b, "message by %s was removed", user);
	else if (*msg)
		buf_printf(b, "%s: %s (%s)", user, msg, t);
	else
		buf_printf(b, "%s: %s", user, t);
}

static int mentions_me(const Json *m, const char *me)
{
	const Json *mentions = json_get(m, "mentions");
	for (size_t i = 0; i < json_len(mentions); i++) {
		const char *u = json_get_str(json_at(mentions, i), "username");
		if (u && (strcmp(u, "all") == 0 || strcmp(u, "here") == 0 || (me && strcmp(u, me) == 0)))
			return 1;
	}
	return 0;
}

static void describe_attachments(Server *srv, Buf *b, const Json *m)
{
	const Json *atts = json_get(m, "attachments");
	for (size_t i = 0; i < json_len(atts); i++) {
		const Json *a = json_at(atts, i);
		const char *title = json_get_str(a, "title");
		const char *link = json_get_str(a, "title_link");
		const char *desc = json_get_str(a, "description");
		const char *text = json_get_str(a, "text");
		const char *author = json_get_str(a, "author_name");
		if (b->len > 0)
			buf_putc(b, '\n');
		if (link) {
			buf_printf(b, "[file] %s ", title ? title : "");
			if (link[0] == '/')
				base_url(&srv->rc.url, b);
			buf_puts(b, link);
			if (desc && *desc)
				buf_printf(b, " %s", desc);
		} else if (author && text) {
			buf_printf(b, "[quote] %s: %s", author, text);
		} else {
			buf_puts(b, "[attachment]");
			if (title)
				buf_printf(b, " %s", title);
			if (text)
				buf_printf(b, " %s", text);
			if (desc)
				buf_printf(b, " %s", desc);
		}
	}
}

static int is_me(Server *srv, const char *user)
{
	return user && srv->rc.username && strcmp(user, srv->rc.username) == 0;
}

static void put_snippet(Buf *b, const char *s, size_t max)
{
	size_t n = 0;
	while (s[n] && n < max)
		n++;
	while (n > 0 && s[n] && ((unsigned char)s[n] & 0xc0) == 0x80)
		n--;
	for (size_t i = 0; i < n; i++)
		buf_putc(b, s[i] == '\n' || s[i] == '\r' || s[i] == '\t' ? ' ' : s[i]);
	if (s[n])
		buf_puts(b, "...");
}

static void put_context(App *app, Buf *b, const Json *m)
{
	const char *user = json_str(json_path(m, "u.username"));
	const char *name = json_str(json_path(m, "u.name"));
	const char *msg = json_get_str(m, "msg");
	buf_puts(b, !app->show_usernames && name && *name ? name : user ? user : "?");
	if (msg && *msg) {
		buf_puts(b, ": ");
		put_snippet(b, msg, 40);
	}
}

static const Json *find_msg(const Json *arr, const char *id)
{
	for (size_t i = 0; i < json_len(arr); i++) {
		const char *mid = json_get_str(json_at(arr, i), "_id");
		if (mid && strcmp(mid, id) == 0)
			return json_at(arr, i);
	}
	return NULL;
}

static char *thread_context(App *app, Window *w, const char *tmid, const Json *scope)
{
	Server *srv = w->srv;
	Buf b;
	buf_init(&b);
	Line *p = find_line(w, tmid);
	const Json *pm = find_msg(scope, tmid);
	if (p) {
		buf_puts(&b, p->name ? p->name : p->nick);
		if (p->raw && *p->raw) {
			buf_puts(&b, ": ");
			put_snippet(&b, p->raw, 40);
		}
		return buf_steal(&b);
	}
	if (pm) {
		put_context(app, &b, pm);
		return buf_steal(&b);
	}
	for (size_t i = 0; i < srv->ntctx; i++)
		if (strcmp(srv->tctx_id[i], tmid) == 0)
			return srv->tctx_text[i] ? xstrdup(srv->tctx_text[i]) : NULL;
	if (srv->tctx_budget == 0)
		return NULL;
	if (srv->tctx_budget > 0)
		srv->tctx_budget--;
	Buf path;
	buf_init(&path);
	buf_puts(&path, "/api/v1/chat.getMessage?msgId=");
	url_encode(&path, tmid);
	Err e;
	Json *j = rc_get(&srv->rc, path.data, e.err, sizeof(e.err));
	buf_free(&path);
	const Json *jm = json_get(j, "message");
	char *text = NULL;
	if (jm) {
		put_context(app, &b, jm);
		text = buf_steal(&b);
	}
	json_free(j);
	srv->tctx_id = xrealloc(srv->tctx_id, (srv->ntctx + 1) * sizeof(*srv->tctx_id));
	srv->tctx_text = xrealloc(srv->tctx_text, (srv->ntctx + 1) * sizeof(*srv->tctx_text));
	srv->tctx_id[srv->ntctx] = xstrdup(tmid);
	srv->tctx_text[srv->ntctx] = text;
	srv->ntctx++;
	buf_free(&b);
	return text ? xstrdup(text) : NULL;
}

static void describe_meta(App *app, Server *srv, Buf *b, const Json *m)
{
	const Json *r = json_get(m, "reactions");
	for (size_t i = 0; r && r->type == JSON_OBJECT && i < r->len; i++) {
		const Json *users = json_get(r->items[i], "usernames");
		const Json *names = app->show_usernames ? NULL : json_get(r->items[i], "names");
		size_t n = json_len(users);
		if (n == 0)
			continue;
		if (b->len)
			buf_puts(b, "  ");
		buf_puts(b, r->keys[i]);
		buf_putc(b, ' ');
		for (size_t k = 0; k < n && k < 3; k++) {
			const char *u = json_str(json_at(users, k));
			const char *nm = json_str(json_at(names, k));
			if (k)
				buf_puts(b, ", ");
			buf_puts(b, is_me(srv, u) ? "you" : nm && *nm ? nm : u ? u : "?");
		}
		if (n > 3)
			buf_printf(b, " +%zu", n - 3);
	}
	int tc = (int)json_num(json_get(m, "tcount"), 0);
	if (tc > 0) {
		if (b->len)
			buf_puts(b, "  ");
		buf_printf(b, tc == 1 ? "%d reply" : "%d replies", tc);
	}
}

static int msg_to_line(App *app, const Json *m, Window *w, Line *ln, const Json *scope)
{
	Server *srv = w->srv;
	memset(ln, 0, sizeof(*ln));
	const char *id = json_get_str(m, "_id");
	const char *user = json_str(json_path(m, "u.username"));
	const char *msg = json_get_str(m, "msg");
	const char *t = json_get_str(m, "t");
	const char *name = json_str(json_path(m, "u.name"));
	if (!user)
		user = "?";
	if (app->show_usernames || (name && !*name))
		name = NULL;
	if (!msg)
		msg = "";

	const Json *tsj = json_get(m, "ts");
	ln->ts = time(NULL);
	if (json_str(tsj))
		ln->ts = parse_iso8601(json_str(tsj));
	else if (json_get(tsj, "$date"))
		ln->ts = (time_t)(json_num(json_get(tsj, "$date"), 0) / 1000);

	Buf text;
	buf_init(&text);
	if (t && strcmp(t, "e2e") != 0) {
		ln->kind = LINE_EVENT;
		describe_event(&text, t, name ? name : user, msg);
	} else {
		ln->kind = LINE_MSG;
		if (t)
			buf_puts(&text, "[encrypted message]");
		else
			buf_puts(&text, msg);
		size_t before = text.len;
		Buf att;
		buf_init(&att);
		describe_attachments(srv, &att, m);
		if (att.len) {
			if (before > 0 && msg[0])
				buf_putc(&text, '\n');
			buf_append(&text, att.data, att.len);
		}
		buf_free(&att);
		if (json_get(m, "editedAt"))
			buf_puts(&text, " (edited)");
	}

	ln->nick = ui_sanitize(user);
	ln->name = name ? ui_sanitize(name) : NULL;
	const char *tmid = json_get_str(m, "tmid");
	if (tmid)
		ln->tmid = xstrdup(tmid);
	Buf full;
	buf_init(&full);
	if (tmid && ln->kind == LINE_MSG) {
		char *ctx = thread_context(app, w, tmid, scope);
		Buf c;
		buf_init(&c);
		buf_puts(&c, ctx ? "[re " : "[thread");
		if (ctx)
			buf_puts(&c, ctx);
		buf_puts(&c, "] ");
		free(ctx);
		char *cs = ui_sanitize(c.data);
		buf_puts(&full, cs);
		ln->ctx_len = full.len;
		free(cs);
		buf_free(&c);
	}
	char *body = ui_sanitize(text.data ? text.data : "");
	buf_puts(&full, body);
	free(body);
	buf_free(&text);
	if (ln->kind == LINE_MSG) {
		Buf meta;
		buf_init(&meta);
		describe_meta(app, srv, &meta, m);
		if (meta.len) {
			char *ms = ui_sanitize(meta.data);
			buf_putc(&full, '\n');
			buf_puts(&full, ms);
			ln->meta_len = strlen(ms);
			free(ms);
		}
		buf_free(&meta);
	}
	ln->text = buf_steal(&full);
	ln->msgid = id ? xstrdup(id) : NULL;
	ln->raw = xstrdup(msg);
	ln->own = is_me(srv, user);
	if (!ln->own && ln->kind == LINE_MSG)
		ln->highlight = w->type == 'd' || mentions_me(m, srv->rc.username);
	return 0;
}

static void add_message(App *app, Window *w, const Json *m, int notify)
{
	Line ln;
	if (msg_to_line(app, m, w, &ln, NULL) < 0)
		return;
	if (ln.msgid) {
		Line *old = find_line(w, ln.msgid);
		if (old) {
			if (strcmp(old->text, ln.text) != 0 || ln.kind != old->kind) {
				char *tmp = old->text;
				old->text = ln.text;
				ln.text = tmp;
				tmp = old->raw;
				old->raw = ln.raw;
				ln.raw = tmp;
				old->kind = ln.kind;
				old->ctx_len = ln.ctx_len;
				old->meta_len = ln.meta_len;
				app->dirty = 1;
			}
			line_free(&ln);
			return;
		}
	}
	win_append(app, w, &ln, notify);
}

static const char *history_endpoint(char type)
{
	switch (type) {
	case 'c': return "channels";
	case 'p': return "groups";
	case 'd': return "im";
	default: return NULL;
	}
}

static void merge_history(App *app, Window *w, const Json *msgs);

static void load_history(App *app, Window *w)
{
	w->history_loaded = 1;
	const char *ep = history_endpoint(w->type);
	if (!w->rid || !ep)
		return;
	Buf params;
	buf_init(&params);
	buf_puts(&params, "[");
	buf_json_str(&params, w->rid);
	buf_printf(&params, ",null,%d,null]", HISTORY_COUNT);
	w->history_call = rc_ddp_call(&w->srv->rc, "loadHistory", params.data);
	buf_free(&params);
	if (w->history_call)
		return;
	Buf path;
	buf_init(&path);
	buf_printf(&path, "/api/v1/%s.history?roomId=", ep);
	url_encode(&path, w->rid);
	buf_printf(&path, "&count=%d", HISTORY_COUNT);
	Err e;
	Json *j = rc_get(&w->srv->rc, path.data, e.err, sizeof(e.err));
	buf_free(&path);
	if (!j) {
		app_error(app, w, "Could not load history: %s", e.err);
		return;
	}
	merge_history(app, w, json_get(j, "messages"));
	json_free(j);
}

static void merge_history(App *app, Window *w, const Json *msgs)
{
	size_t n = json_len(msgs);
	Line *hist = xcalloc(n ? n : 1, sizeof(*hist));
	size_t nh = 0;
	w->srv->tctx_budget = 10;
	for (size_t i = n; i > 0; i--) {
		Line ln;
		if (msg_to_line(app, json_at(msgs, i - 1), w, &ln, msgs) < 0)
			continue;
		Line *old = ln.msgid ? find_line(w, ln.msgid) : NULL;
		if (old) {
			if (strcmp(old->text, ln.text) != 0) {
				char *tmp = old->text;
				old->text = ln.text;
				ln.text = tmp;
				old->ctx_len = ln.ctx_len;
				old->meta_len = ln.meta_len;
			}
			line_free(&ln);
			continue;
		}
		hist[nh++] = ln;
	}
	w->srv->tctx_budget = -1;
	for (size_t i = 1; i < nh; i++) {
		Line ln = hist[i];
		size_t k = i;
		for (; k > 0 && hist[k - 1].ts > ln.ts; k--)
			hist[k] = hist[k - 1];
		hist[k] = ln;
	}

	size_t total = nh + w->nlines;
	Line *merged = xcalloc(total ? total : 1, sizeof(*merged));
	size_t a = 0, b = 0, k = 0;
	while (a < nh || b < w->nlines) {
		if (b >= w->nlines || (a < nh && hist[a].ts <= w->lines[b].ts))
			merged[k++] = hist[a++];
		else
			merged[k++] = w->lines[b++];
	}
	free(hist);
	free(w->lines);
	w->lines = merged;
	w->nlines = total;
	w->cap = total ? total : 1;
	app->dirty = 1;
}

static void mark_read(Window *w)
{
	w->needs_read = 0;
	if (!w->rid)
		return;
	Buf body;
	buf_init(&body);
	buf_puts(&body, "{\"rid\":");
	buf_json_str(&body, w->rid);
	buf_puts(&body, "}");
	Err e;
	Json *j = rc_post(&w->srv->rc, "/api/v1/subscriptions.read", body.data, e.err, sizeof(e.err));
	buf_free(&body);
	if (!j)
		srv_msg(w->srv, LINE_ERROR, "Could not mark %s as read: %s", w->name, e.err);
	json_free(j);
}

void app_switch(App *app, size_t idx)
{
	if (idx >= app->nwins)
		return;
	if (idx != app->cur)
		reset_modes(app);
	Window *old = current(app);
	if (old->needs_read && idx != app->cur)
		mark_read(old);
	app->cur = idx;
	Window *w = current(app);
	int unread = w->activity >= ACT_MSG || w->needs_read;
	w->activity = ACT_NONE;
	w->unread = 0;
	if (!w->history_loaded)
		load_history(app, w);
	if (unread)
		mark_read(w);
	app->dirty = 1;
}

static char *window_name(const char *name, char type)
{
	Buf b;
	buf_init(&b);
	if (type == 'c' || type == 'p')
		buf_putc(&b, '#');
	buf_puts(&b, name);
	return buf_steal(&b);
}

static const char *sub_display_name(const Json *s)
{
	const char *fname = json_get_str(s, "fname");
	if (json_get(s, "prid") && fname)
		return fname;
	const char *name = json_get_str(s, "name");
	return name ? name : fname ? fname : "?";
}

static void label_window(App *app, Window *w)
{
	if (app->show_usernames || !w->rid)
		return;
	for (size_t i = 0; i < w->srv->nsubs; i++) {
		Subscription *s = &w->srv->subs[i];
		if (!s->fname || strcmp(s->rid, w->rid) != 0)
			continue;
		if (!w->label || strcmp(w->label, s->fname) != 0) {
			free(w->label);
			w->label = ui_sanitize(s->fname);
			free(w->topic);
			w->topic = xstrdup(w->label);
			app->dirty = 1;
		}
		return;
	}
}

static Window *open_window(App *app, Server *srv, const char *rid, const char *name, char type)
{
	Window *w = win_by_rid(app, srv, rid);
	if (w)
		return w;
	char *wn = window_name(name, type);
	w = win_new(app, srv, rid, wn, type);
	free(wn);
	if (type == 'd')
		w->topic = xstrdup(w->name);
	label_window(app, w);
	return w;
}

static int type_rank(char t)
{
	return t == 'c' || t == 'p' ? 0 : t == 'd' ? 1 : 2;
}

static int sub_cmp(const void *pa, const void *pb)
{
	const Subscription *a = pa, *b = pb;
	int ra = type_rank(a->type), rb = type_rank(b->type);
	if (ra != rb)
		return ra - rb;
	return str_casecmp(a->name, b->name);
}

static void free_subs(Server *srv)
{
	for (size_t i = 0; i < srv->nsubs; i++) {
		free(srv->subs[i].rid);
		free(srv->subs[i].name);
		free(srv->subs[i].fname);
	}
	free(srv->subs);
	srv->subs = NULL;
	srv->nsubs = 0;
}

static int load_subscriptions(App *app, Server *srv, int initial)
{
	Err e;
	Json *j = rc_get(&srv->rc, "/api/v1/subscriptions.get", e.err, sizeof(e.err));
	if (!j) {
		srv_msg(srv, LINE_ERROR, "Could not load subscriptions: %s", e.err);
		return -1;
	}
	const Json *upd = json_get(j, "update");
	size_t n = json_len(upd);
	free_subs(srv);
	srv->subs = xcalloc(n ? n : 1, sizeof(*srv->subs));
	for (size_t i = 0; i < n; i++) {
		const Json *s = json_at(upd, i);
		const char *rid = json_get_str(s, "rid");
		const char *t = json_get_str(s, "t");
		if (!rid || !t)
			continue;
		Subscription *sub = &srv->subs[srv->nsubs++];
		sub->rid = xstrdup(rid);
		sub->name = xstrdup(sub_display_name(s));
		const char *fname = json_get_str(s, "fname");
		sub->fname = fname && *fname && t[0] == 'd' ? xstrdup(fname) : NULL;
		sub->type = t[0];
		sub->open = json_get(s, "open") ? json_bool(json_get(s, "open")) : 1;
		sub->unread = (int)json_num(json_get(s, "unread"), 0);
		sub->mentions = (int)json_num(json_get(s, "userMentions"), 0);
	}
	json_free(j);
	qsort(srv->subs, srv->nsubs, sizeof(*srv->subs), sub_cmp);

	for (size_t i = 0; i < srv->nsubs; i++) {
		Subscription *s = &srv->subs[i];
		if (!s->open || win_by_rid(app, srv, s->rid))
			continue;
		Window *w = open_window(app, srv, s->rid, s->name, s->type);
		if (initial && (s->unread > 0 || s->mentions > 0)) {
			w->activity = s->mentions > 0 || s->type == 'd' ? ACT_HIGHLIGHT : ACT_MSG;
			w->unread = s->unread;
			w->needs_read = 1;
		}
	}
	for (size_t i = 0; i < app->nwins; i++)
		if (app->wins[i]->srv == srv)
			label_window(app, app->wins[i]);
	return 0;
}

static void load_topics(App *app, Server *srv)
{
	Err e;
	Json *j = rc_get(&srv->rc, "/api/v1/rooms.get", e.err, sizeof(e.err));
	if (!j) {
		srv_msg(srv, LINE_ERROR, "Could not load room topics: %s", e.err);
		return;
	}
	const Json *upd = json_get(j, "update");
	for (size_t i = 0; i < json_len(upd); i++) {
		const Json *r = json_at(upd, i);
		const char *id = json_get_str(r, "_id");
		const char *topic = json_get_str(r, "topic");
		Window *w = id ? win_by_rid(app, srv, id) : NULL;
		if (w && w->type != 'd' && topic) {
			free(w->topic);
			w->topic = ui_sanitize(topic);
		}
	}
	json_free(j);
}

static Subscription *sub_by_name(Server *srv, const char *name, int want_dm)
{
	const char *bare = name[0] == '#' ? name + 1 : name;
	for (size_t i = 0; i < srv->nsubs; i++) {
		Subscription *s = &srv->subs[i];
		if ((s->type == 'd') == want_dm && str_casecmp(s->name, bare) == 0)
			return s;
	}
	return NULL;
}

static void sync_unread(App *app, Window *w, const Json *sub)
{
	const Json *unread = json_get(sub, "unread");
	if (!unread || unread->type != JSON_NUMBER)
		return;
	w->unread = (int)unread->num;
	if (w->unread == 0 && !json_bool(json_get(sub, "alert"))) {
		w->activity = ACT_NONE;
		w->needs_read = 0;
	} else if (w->unread > 0 && w->activity < ACT_MSG) {
		w->activity = ACT_MSG;
	}
	if (json_num(json_get(sub, "userMentions"), 0) > 0)
		w->activity = ACT_HIGHLIGHT;
	app->dirty = 1;
}

static void on_stream(void *ctx, const char *coll, const char *event, const Json *args)
{
	Server *srv = ctx;
	App *app = srv->app;
	if (strcmp(coll, "stream-room-messages") == 0) {
		const Json *m = json_at(args, 0);
		const char *rid = json_get_str(m, "rid");
		if (!rid)
			return;
		Window *w = win_by_rid(app, srv, rid);
		if (!w) {
			load_subscriptions(app, srv, 0);
			w = win_by_rid(app, srv, rid);
			if (!w)
				return;
		}
		add_message(app, w, m, 1);
	} else if (strcmp(coll, "stream-notify-user") == 0) {
		const char *slash = strrchr(event, '/');
		if (!slash)
			return;
		const char *action = json_str(json_at(args, 0));
		const Json *obj = json_at(args, 1);
		if (strcmp(slash, "/subscriptions-changed") == 0) {
			const char *rid = json_get_str(obj, "rid");
			Window *w = rid ? win_by_rid(app, srv, rid) : NULL;
			if (action && strcmp(action, "removed") != 0 && rid && !w && json_bool(json_get(obj, "open")))
				load_subscriptions(app, srv, 0);
			else if (w && w != current(app))
				sync_unread(app, w, obj);
		} else if (strcmp(slash, "/rooms-changed") == 0) {
			const char *id = json_get_str(obj, "_id");
			Window *w = id ? win_by_rid(app, srv, id) : NULL;
			const char *topic = json_get_str(obj, "topic");
			if (w && topic && (!w->topic || strcmp(w->topic, topic) != 0)) {
				free(w->topic);
				w->topic = ui_sanitize(topic);
				app->dirty = 1;
			}
		}
	}
}

static void on_status(void *ctx, int is_error, const char *text)
{
	srv_msg(ctx, is_error ? LINE_ERROR : LINE_INFO, "%s", text);
}

static void on_ready(void *ctx)
{
	Server *srv = ctx;
	App *app = srv->app;
	srv->reconnect_delay = 0;
	srv_msg(srv, LINE_INFO, "Realtime connection established");
	if (srv->ever_ready) {
		load_subscriptions(app, srv, 0);
		for (size_t i = 0; i < app->nwins; i++)
			if (app->wins[i]->srv == srv && app->wins[i]->history_loaded)
				load_history(app, app->wins[i]);
	}
	srv->ever_ready = 1;
}

static void on_result(void *ctx, unsigned id, const Json *result, const char *error)
{
	Server *srv = ctx;
	App *app = srv->app;
	for (size_t i = 0; i < app->nwins; i++) {
		Window *w = app->wins[i];
		if (w->srv != srv || w->history_call != id)
			continue;
		w->history_call = 0;
		if (error)
			app_error(app, w, "Could not load history: %s", error);
		else
			merge_history(app, w, json_get(result, "messages"));
		return;
	}
}

static const RcHandlers handlers = { on_stream, on_status, on_ready, on_result };

static void schedule_reconnect(Server *srv)
{
	srv->reconnect_delay = srv->reconnect_delay ? srv->reconnect_delay * 2 : 2;
	if (srv->reconnect_delay > RECONNECT_MAX)
		srv->reconnect_delay = RECONNECT_MAX;
	srv->reconnect_at = now_ms() + srv->reconnect_delay * 1000LL;
	srv_msg(srv, LINE_INFO, "Reconnecting in %d seconds", srv->reconnect_delay);
}

void app_ddp_tick(App *app)
{
	for (size_t i = 0; i < app->nservers; i++) {
		Server *srv = app->servers[i];
		if (srv->rc.state != DDP_OFFLINE || srv->reconnect_at < 0 || now_ms() < srv->reconnect_at)
			continue;
		Err e;
		srv_msg(srv, LINE_INFO, "Connecting to realtime API");
		app->dirty = 1;
		if (rc_ddp_connect(&srv->rc, e.err, sizeof(e.err)) < 0) {
			srv_msg(srv, LINE_ERROR, "Realtime connection failed: %s", e.err);
			schedule_reconnect(srv);
		}
	}
}

void app_ddp_readable(App *app, Server *srv)
{
	if (rc_ddp_process(&srv->rc, &handlers, srv) < 0) {
		srv_msg(srv, LINE_ERROR, "Realtime connection lost");
		schedule_reconnect(srv);
	}
	app->dirty = 1;
}

long long app_next_reconnect(const App *app)
{
	long long next = -1;
	for (size_t i = 0; i < app->nservers; i++) {
		const Server *srv = app->servers[i];
		if (srv->rc.state == DDP_OFFLINE && srv->reconnect_at >= 0 && (next < 0 || srv->reconnect_at < next))
			next = srv->reconnect_at;
	}
	return next;
}

void app_init(App *app)
{
	app->servers = NULL;
	app->nservers = 0;
	app->status_srv = NULL;
	app->wins = NULL;
	app->nwins = 0;
	app->cur = 0;
	memset(&app->input, 0, sizeof(app->input));
	app->quit = 0;
	app->dirty = 1;
	app->show_usernames = 0;
	app->comp = NULL;
	app->comp_names = NULL;
	app->ncomp = 0;
	app->goto_active = 0;
	app->goto_text[0] = '\0';
	app->goto_match = -1;
	app->goto_list = NULL;
	app->goto_n = 0;
	app->goto_idx = 0;
	app->sel_id = NULL;
	app->reply_tmid = NULL;
	app->reply_to = NULL;
	app->react_active = 0;
	app->react_list = NULL;
	app->react_n = 0;
	app->scroll_mode = 0;
	app->search_active = 0;
	app->search_text[0] = '\0';
	app->search_hit = 0;
	app->search_reveal = 0;
	app->search_failed = 0;
	Window *st = win_new(app, NULL, NULL, "(status)", 0);
	st->history_loaded = 1;
}

void app_free(App *app)
{
	for (size_t i = 0; i < app->nwins; i++)
		win_free(app->wins[i]);
	free(app->wins);
	for (size_t i = 0; i < app->nservers; i++) {
		Server *srv = app->servers[i];
		free_subs(srv);
		for (size_t k = 0; k < srv->ntctx; k++) {
			free(srv->tctx_id[k]);
			free(srv->tctx_text[k]);
		}
		free(srv->tctx_id);
		free(srv->tctx_text);
		rc_free(&srv->rc);
		free(srv->name);
		free(srv);
	}
	free(app->servers);
	app_tab_reset(app);
	free(app->goto_list);
	free(app->sel_id);
	free(app->reply_tmid);
	free(app->reply_to);
	free(app->react_list);
	for (int i = 0; i < app->input.nhistory; i++)
		free(app->input.history[i]);
}

Server *app_add_server(App *app, const char *name, const Rc *rc)
{
	Server *srv = xcalloc(1, sizeof(*srv));
	srv->app = app;
	srv->name = xstrdup(name);
	srv->rc = *rc;
	srv->tctx_budget = -1;
	app->servers = xrealloc(app->servers, (app->nservers + 1) * sizeof(*app->servers));
	app->servers[app->nservers++] = srv;
	if (!app->status_srv)
		app->status_srv = srv;
	return srv;
}

void app_start(App *app)
{
	Buf b;
	buf_init(&b);
	for (size_t i = 0; i < app->nservers; i++) {
		Server *srv = app->servers[i];
		if (i)
			buf_puts(&b, ", ");
		base_url(&srv->rc.url, &b);
	}
	free(app->wins[0]->topic);
	app->wins[0]->topic = xstrdup(b.data ? b.data : "");
	buf_free(&b);
	app_info(app, NULL, "Type /help for a list of commands, Ctrl+G or Ctrl+N/P to switch windows");
	for (size_t i = 0; i < app->nservers; i++) {
		Server *srv = app->servers[i];
		buf_init(&b);
		base_url(&srv->rc.url, &b);
		srv_msg(srv, LINE_INFO, "Logged in to %s as %s", b.data, srv->rc.username);
		buf_free(&b);
		size_t before = app->nwins;
		if (load_subscriptions(app, srv, 1) == 0) {
			load_topics(app, srv);
			srv_msg(srv, LINE_INFO, "%zu rooms, %zu windows open", srv->nsubs, app->nwins - before);
		}
	}
	app_ddp_tick(app);
}

static int send_text(App *app, Window *w, const char *text)
{
	if (!w->rid) {
		app_error(app, w, "Not in a room, use /join or /query, or switch windows with Ctrl+G");
		return -1;
	}
	Buf body;
	buf_init(&body);
	buf_puts(&body, "{\"message\":{\"rid\":");
	buf_json_str(&body, w->rid);
	buf_puts(&body, ",\"msg\":");
	buf_json_str(&body, text);
	int reply = app->reply_tmid && w == current(app);
	if (reply) {
		buf_puts(&body, ",\"tmid\":");
		buf_json_str(&body, app->reply_tmid);
	}
	buf_puts(&body, "}}");
	Err e;
	Json *j = rc_post(&w->srv->rc, "/api/v1/chat.sendMessage", body.data, e.err, sizeof(e.err));
	buf_free(&body);
	if (!j) {
		app_error(app, w, "Message not sent: %s", e.err);
		return -1;
	}
	const Json *m = json_get(j, "message");
	if (m)
		add_message(app, w, m, 0);
	json_free(j);
	if (reply)
		app_reply_cancel(app);
	return 0;
}

static void post_room_action(App *app, Window *w, const char *action, const char *extra_key, const char *extra_val)
{
	const char *ep = history_endpoint(w->type);
	if (!ep) {
		app_error(app, w, "Not supported for this window");
		return;
	}
	char path[128];
	snprintf(path, sizeof(path), "/api/v1/%s.%s", ep, action);
	Buf body;
	buf_init(&body);
	buf_puts(&body, "{\"roomId\":");
	buf_json_str(&body, w->rid);
	if (extra_key) {
		buf_printf(&body, ",\"%s\":", extra_key);
		buf_json_str(&body, extra_val);
	}
	buf_puts(&body, "}");
	Err e;
	Json *j = rc_post(&w->srv->rc, path, body.data, e.err, sizeof(e.err));
	buf_free(&body);
	if (!j) {
		app_error(app, w, "%s", e.err);
		return;
	}
	json_free(j);
	if (strcmp(action, "leave") == 0 || strcmp(action, "close") == 0)
		win_remove(app, w);
}

static Window *query_window(App *app, Server *srv, const char *user)
{
	const char *bare = user[0] == '@' ? user + 1 : user;
	Window *w = win_by_name(app, srv, bare);
	if (w && w->type == 'd')
		return w;
	Subscription *s = sub_by_name(srv, bare, 1);
	if (s)
		return open_window(app, srv, s->rid, s->name, 'd');
	Buf body;
	buf_init(&body);
	buf_puts(&body, "{\"username\":");
	buf_json_str(&body, bare);
	buf_puts(&body, "}");
	Err e;
	Json *j = rc_post(&srv->rc, "/api/v1/im.create", body.data, e.err, sizeof(e.err));
	buf_free(&body);
	if (!j) {
		app_error(app, NULL, "Could not open conversation with %s: %s", bare, e.err);
		return NULL;
	}
	const char *rid = json_str(json_path(j, "room._id"));
	if (!rid)
		rid = json_str(json_path(j, "room.rid"));
	w = rid ? open_window(app, srv, rid, bare, 'd') : NULL;
	json_free(j);
	if (w)
		load_subscriptions(app, srv, 0);
	return w;
}

static void cmd_join(App *app, const char *arg)
{
	Server *srv = cur_srv(app);
	if (!*arg) {
		app_error(app, current(app), "Usage: /join #channel");
		return;
	}
	Window *w = win_by_name(app, srv, arg);
	if (w && w->type != 'd') {
		app_switch(app, win_index(app, w));
		return;
	}
	Subscription *s = sub_by_name(srv, arg, 0);
	if (s) {
		w = open_window(app, srv, s->rid, s->name, s->type);
		app_switch(app, win_index(app, w));
		return;
	}
	const char *bare = arg[0] == '#' ? arg + 1 : arg;
	Buf path;
	buf_init(&path);
	buf_puts(&path, "/api/v1/channels.info?roomName=");
	url_encode(&path, bare);
	Err e;
	Json *info = rc_get(&srv->rc, path.data, e.err, sizeof(e.err));
	buf_free(&path);
	if (!info) {
		app_error(app, current(app), "Cannot join %s: %s", arg, e.err);
		return;
	}
	const char *rid = json_str(json_path(info, "channel._id"));
	const char *name = json_str(json_path(info, "channel.name"));
	const char *topic = json_str(json_path(info, "channel.topic"));
	if (!rid) {
		app_error(app, current(app), "Cannot join %s: no such channel", arg);
		json_free(info);
		return;
	}
	Buf body;
	buf_init(&body);
	buf_puts(&body, "{\"roomId\":");
	buf_json_str(&body, rid);
	buf_puts(&body, "}");
	Json *j = rc_post(&srv->rc, "/api/v1/channels.join", body.data, e.err, sizeof(e.err));
	buf_free(&body);
	if (!j) {
		app_error(app, current(app), "Cannot join %s: %s", arg, e.err);
		json_free(info);
		return;
	}
	json_free(j);
	w = open_window(app, srv, rid, name ? name : bare, 'c');
	if (topic) {
		free(w->topic);
		w->topic = ui_sanitize(topic);
	}
	json_free(info);
	app_switch(app, win_index(app, w));
	load_subscriptions(app, srv, 0);
}

static Window *target_window(App *app, const char *arg)
{
	if (!*arg)
		return current(app);
	Window *w = win_by_name(app, cur_srv(app), arg);
	if (!w)
		app_error(app, current(app), "No window named %s", arg);
	return w;
}

static void cmd_part(App *app, const char *arg, const char *action)
{
	Window *w = target_window(app, arg);
	if (!w)
		return;
	if (!w->rid) {
		app_error(app, w, "Cannot %s the status window", action);
		return;
	}
	if (w->type == 'd' && strcmp(action, "leave") == 0)
		action = "close";
	post_room_action(app, w, action, NULL, NULL);
}

static void cmd_query(App *app, const char *arg)
{
	char user[256];
	const char *rest = "";
	size_t n = strcspn(arg, " ");
	if (n == 0 || n >= sizeof(user)) {
		app_error(app, current(app), "Usage: /query user [message]");
		return;
	}
	memcpy(user, arg, n);
	user[n] = '\0';
	if (arg[n])
		rest = arg + n + 1;
	Window *w = query_window(app, cur_srv(app), user);
	if (!w)
		return;
	app_switch(app, win_index(app, w));
	if (*rest)
		send_text(app, w, rest);
}

static void cmd_msg(App *app, const char *arg)
{
	char target[256];
	size_t n = strcspn(arg, " ");
	if (n == 0 || n >= sizeof(target) || !arg[n] || !arg[n + 1]) {
		app_error(app, current(app), "Usage: /msg #channel|user message");
		return;
	}
	memcpy(target, arg, n);
	target[n] = '\0';
	Server *srv = cur_srv(app);
	Window *w = target[0] == '#' ? win_by_name(app, srv, target) : query_window(app, srv, target);
	if (!w) {
		app_error(app, current(app), "No window for %s, use /join first", target);
		return;
	}
	send_text(app, w, arg + n + 1);
}

static void cmd_me(App *app, const char *arg)
{
	Window *w = current(app);
	if (!w->rid) {
		app_error(app, w, "Not in a room");
		return;
	}
	Buf body;
	buf_init(&body);
	buf_puts(&body, "{\"command\":\"me\",\"roomId\":");
	buf_json_str(&body, w->rid);
	buf_puts(&body, ",\"params\":");
	buf_json_str(&body, arg);
	buf_puts(&body, "}");
	Err e;
	Json *j = rc_post(&w->srv->rc, "/api/v1/commands.run", body.data, e.err, sizeof(e.err));
	buf_free(&body);
	if (!j)
		app_error(app, w, "%s", e.err);
	json_free(j);
}

static void cmd_window(App *app, const char *arg)
{
	if (strcmp(arg, "next") == 0 || strcmp(arg, "n") == 0) {
		app_switch(app, (app->cur + 1) % app->nwins);
	} else if (strcmp(arg, "prev") == 0 || strcmp(arg, "p") == 0) {
		app_switch(app, (app->cur + app->nwins - 1) % app->nwins);
	} else if (!*arg || strcmp(arg, "list") == 0) {
		for (size_t i = 0; i < app->nwins; i++) {
			Window *w = app->wins[i];
			const char *tag = app->nservers > 1 && w->srv ? w->srv->name : "";
			const char *sep = *tag ? "/" : "";
			if (w->label)
				app_info(app, current(app), "%3zu %s%s%s (%s)", i + 1, tag, sep, w->label, w->name);
			else
				app_info(app, current(app), "%3zu %s%s%s", i + 1, tag, sep, w->name);
		}
	} else if (isdigit((unsigned char)arg[0])) {
		long n = strtol(arg, NULL, 10);
		if (n < 1 || (size_t)n > app->nwins)
			app_error(app, current(app), "No such window: %ld", n);
		else
			app_switch(app, (size_t)n - 1);
	} else {
		Window *w = target_window(app, arg);
		if (w)
			app_switch(app, win_index(app, w));
	}
}

static void cmd_list(App *app, const char *arg)
{
	Err e;
	Json *j = rc_get(&cur_srv(app)->rc, "/api/v1/channels.list?count=500&sort=%7B%22name%22%3A1%7D", e.err, sizeof(e.err));
	if (!j) {
		app_error(app, current(app), "%s", e.err);
		return;
	}
	const Json *chans = json_get(j, "channels");
	size_t shown = 0;
	for (size_t i = 0; i < json_len(chans); i++) {
		const Json *c = json_at(chans, i);
		const char *name = json_get_str(c, "name");
		const char *topic = json_get_str(c, "topic");
		if (!name || (*arg && !strstr(name, arg)))
			continue;
		app_info(app, current(app), "#%-24s %5d  %s", name, (int)json_num(json_get(c, "usersCount"), 0),
			 topic ? topic : "");
		shown++;
	}
	app_info(app, current(app), "End of channel list (%zu of %d)", shown, (int)json_num(json_get(j, "total"), 0));
	json_free(j);
}

static void cmd_names(App *app)
{
	Window *w = current(app);
	const char *ep = history_endpoint(w->type);
	if (!w->rid || !ep) {
		app_error(app, w, "Not in a room");
		return;
	}
	Buf path;
	buf_init(&path);
	buf_printf(&path, "/api/v1/%s.members?count=500&roomId=", ep);
	url_encode(&path, w->rid);
	Err e;
	Json *j = rc_get(&w->srv->rc, path.data, e.err, sizeof(e.err));
	buf_free(&path);
	if (!j) {
		app_error(app, w, "%s", e.err);
		return;
	}
	const Json *members = json_get(j, "members");
	Buf out;
	buf_init(&out);
	for (size_t i = 0; i < json_len(members); i++) {
		const char *u = json_get_str(json_at(members, i), "username");
		const char *st = json_get_str(json_at(members, i), "status");
		if (!u)
			continue;
		buf_printf(&out, "[%s%s] ", st && strcmp(st, "online") == 0 ? "+" : " ", u);
	}
	app_info(app, w, "Users in %s (%d): %s", w->name, (int)json_num(json_get(j, "total"), (double)json_len(members)),
		 out.data ? out.data : "");
	buf_free(&out);
	json_free(j);
}

static void cmd_topic(App *app, const char *arg)
{
	Window *w = current(app);
	if (!*arg) {
		app_info(app, w, "Topic for %s: %s", w->name, w->topic ? w->topic : "(none)");
		return;
	}
	if (!w->rid || w->type == 'd') {
		app_error(app, w, "Cannot set a topic here");
		return;
	}
	post_room_action(app, w, "setTopic", "topic", arg);
}

static void cmd_clear(App *app)
{
	Window *w = current(app);
	for (size_t i = 0; i < w->nlines; i++)
		line_free(&w->lines[i]);
	w->nlines = 0;
	w->scroll = 0;
	app->dirty = 1;
}

static void cmd_theme(App *app, const char *arg)
{
	if (*arg) {
		if (ui_set_theme(arg) < 0)
			app_error(app, current(app), "Unknown theme: %s", arg);
		else
			app_info(app, current(app), "Theme set to %s, add \"theme = %s\" to the config file to keep it",
				 ui_theme_name(), ui_theme_name());
		return;
	}
	Buf b;
	buf_init(&b);
	for (size_t i = 0; ui_theme_at(i); i++)
		buf_printf(&b, "%s%s", i ? ", " : "", ui_theme_at(i));
	app_info(app, current(app), "Current theme: %s. Available: %s, or random", ui_theme_name(), b.data);
	buf_free(&b);
}

static const char *state_name(DdpState st)
{
	switch (st) {
	case DDP_READY: return "connected";
	case DDP_CONNECTING: return "connecting";
	case DDP_LOGGING_IN: return "logging in";
	default: return "offline";
	}
}

static void cmd_server(App *app, const char *arg)
{
	if (!*arg) {
		for (size_t i = 0; i < app->nservers; i++) {
			Server *srv = app->servers[i];
			Buf b;
			buf_init(&b);
			base_url(&srv->rc.url, &b);
			app_info(app, current(app), "%s %-12s %s as %s, %s", srv == app->status_srv ? "*" : " ", srv->name, b.data,
				 srv->rc.username ? srv->rc.username : "?", state_name(srv->rc.state));
			buf_free(&b);
		}
		return;
	}
	for (size_t i = 0; i < app->nservers; i++) {
		if (str_casecmp(app->servers[i]->name, arg) == 0) {
			app->status_srv = app->servers[i];
			app_info(app, current(app), "Commands in the status window now go to %s", app->status_srv->name);
			return;
		}
	}
	app_error(app, current(app), "No server named %s, /server lists them", arg);
}

static void cmd_help(App *app)
{
	static const char *help[] = {
		"/join #channel          join a channel or switch to it",
		"/part [#channel]        leave a channel (closes a direct message)",
		"/close [#channel]       hide a room without leaving it",
		"/query user [message]   open a direct message",
		"/msg target message     send to #channel or user without switching",
		"/me action              send an action",
		"/window n|next|prev     switch window, /window list shows all",
		"/list [filter]          list public channels",
		"/names                  list members of the current room",
		"/topic [text]           show or set the topic",
		"/clear                  clear the current window",
		"/theme [name|random]    list themes or switch to one",
		"/reconnect              reconnect the realtime connection",
		"/server [name]          list servers, or pick the one for the status window",
		"/quit                   exit",
		"Keys: Ctrl+G go to a window by number or name, Ctrl+X jump to activity,",
		"      Ctrl+R select a message to reply in its thread (r) or react to it (+),",
		"      Ctrl+N/P next/previous, Alt+1..9,0 and Alt+q..o switch windows,",
		"      PgUp/PgDn scroll, Ctrl+S scroll mode with vi keys and / search,",
		"      Tab/Shift+Tab cycle @mentions, // sends a leading slash",
	};
	for (size_t i = 0; i < sizeof(help) / sizeof(help[0]); i++)
		app_info(app, current(app), "%s", help[i]);
}

static void run_command(App *app, const char *line)
{
	char cmd[32];
	size_t n = strcspn(line, " ");
	if (n >= sizeof(cmd))
		n = sizeof(cmd) - 1;
	for (size_t i = 0; i < n; i++)
		cmd[i] = (char)tolower((unsigned char)line[i]);
	cmd[n] = '\0';
	const char *arg = line + strcspn(line, " ");
	while (*arg == ' ')
		arg++;

	if (strcmp(cmd, "join") == 0 || strcmp(cmd, "j") == 0)
		cmd_join(app, arg);
	else if (strcmp(cmd, "part") == 0 || strcmp(cmd, "leave") == 0)
		cmd_part(app, arg, "leave");
	else if (strcmp(cmd, "close") == 0 || strcmp(cmd, "wc") == 0)
		cmd_part(app, arg, "close");
	else if (strcmp(cmd, "query") == 0 || strcmp(cmd, "q") == 0)
		cmd_query(app, arg);
	else if (strcmp(cmd, "msg") == 0)
		cmd_msg(app, arg);
	else if (strcmp(cmd, "me") == 0)
		cmd_me(app, arg);
	else if (strcmp(cmd, "window") == 0 || strcmp(cmd, "win") == 0 || strcmp(cmd, "w") == 0)
		cmd_window(app, arg);
	else if (strcmp(cmd, "list") == 0)
		cmd_list(app, arg);
	else if (strcmp(cmd, "names") == 0 || strcmp(cmd, "who") == 0)
		cmd_names(app);
	else if (strcmp(cmd, "topic") == 0)
		cmd_topic(app, arg);
	else if (strcmp(cmd, "theme") == 0)
		cmd_theme(app, arg);
	else if (strcmp(cmd, "clear") == 0)
		cmd_clear(app);
	else if (strcmp(cmd, "reconnect") == 0) {
		Server *srv = cur_srv(app);
		rc_ddp_disconnect(&srv->rc);
		srv->reconnect_delay = 0;
		srv->reconnect_at = 0;
		app_ddp_tick(app);
	} else if (strcmp(cmd, "server") == 0)
		cmd_server(app, arg);
	else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0)
		app->quit = 1;
	else if (strcmp(cmd, "help") == 0)
		cmd_help(app);
	else
		app_error(app, current(app), "Unknown command: /%s (see /help)", cmd);
}

void app_submit(App *app, const char *line)
{
	if (!*line)
		return;
	if (line[0] == '/' && line[1] != '/')
		run_command(app, line + 1);
	else
		send_text(app, current(app), line[0] == '/' ? line + 1 : line);
	app->dirty = 1;
}

static int completes(const char *candidate, const char *prefix, size_t plen)
{
	return candidate && strlen(candidate) >= plen && strncasecmp_ascii(candidate, prefix, plen) == 0;
}

static int name_completes(const char *name, const char *prefix, size_t plen)
{
	if (!name)
		return 0;
	for (const char *p = name; p; p = strchr(p, ' ')) {
		while (*p == ' ')
			p++;
		if (completes(p, prefix, plen))
			return 1;
	}
	return 0;
}

void app_tab_reset(App *app)
{
	for (size_t i = 0; i < app->ncomp; i++) {
		free(app->comp[i]);
		free(app->comp_names[i]);
	}
	free(app->comp);
	free(app->comp_names);
	app->comp = NULL;
	app->comp_names = NULL;
	app->ncomp = 0;
}

static void build_completions(App *app, const char *prefix, size_t plen)
{
	Window *w = current(app);
	for (size_t i = w->nlines; i > 0; i--) {
		const Line *ln = &w->lines[i - 1];
		if (!ln->nick || ln->own || ln->kind == LINE_EVENT)
			continue;
		if (!completes(ln->nick, prefix, plen) && !name_completes(ln->name, prefix, plen))
			continue;
		size_t k;
		for (k = 0; k < app->ncomp; k++)
			if (strcmp(app->comp[k], ln->nick) == 0)
				break;
		if (k < app->ncomp)
			continue;
		app->comp = xrealloc(app->comp, (app->ncomp + 1) * sizeof(*app->comp));
		app->comp_names = xrealloc(app->comp_names, (app->ncomp + 1) * sizeof(*app->comp_names));
		app->comp[app->ncomp] = xstrdup(ln->nick);
		app->comp_names[app->ncomp++] = xstrdup(ln->name ? ln->name : ln->nick);
	}
}

void app_tab_complete(App *app, int dir)
{
	Input *in = &app->input;
	if (app->ncomp == 0) {
		size_t start = in->cur;
		while (start > 0 && in->text[start - 1] != ' ')
			start--;
		const char *prefix = in->text + start;
		size_t plen = in->cur - start;
		size_t skip = plen > 0 && prefix[0] == '@';
		if (plen == skip)
			return;
		build_completions(app, prefix + skip, plen - skip);
		if (app->ncomp == 0)
			return;
		app->comp_start = start;
		app->comp_len = plen;
		app->comp_idx = dir > 0 ? 0 : app->ncomp - 1;
	} else {
		app->comp_idx = (app->comp_idx + app->ncomp + (size_t)(dir > 0 ? 1 : -1)) % app->ncomp;
	}

	char completed[512];
	snprintf(completed, sizeof(completed), "@%s ", app->comp[app->comp_idx]);
	size_t clen = strlen(completed);
	size_t start = app->comp_start, end = start + app->comp_len;
	if (end > in->len || in->len - app->comp_len + clen >= INPUT_MAX)
		return;
	memmove(in->text + start + clen, in->text + end, in->len - end);
	memcpy(in->text + start, completed, clen);
	in->len = in->len - app->comp_len + clen;
	in->cur = start + clen;
	in->text[in->len] = '\0';
	app->comp_len = clen;
	app->dirty = 1;
}

static int str_icontains(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (strncasecmp_ascii(hay, needle, n) == 0)
			return 1;
	return 0;
}

static int word_prefix(const char *s, const char *q)
{
	size_t n = strlen(q);
	for (const char *p = s; p && *p; p++)
		if ((p == s || p[-1] == ' ' || p[-1] == ',') && strncasecmp_ascii(p, q, n) == 0)
			return 1;
	return 0;
}

static void goto_add(App *app, int *scores, size_t i, int score)
{
	size_t k = app->goto_n++;
	while (k > 0 && scores[k - 1] < score) {
		app->goto_list[k] = app->goto_list[k - 1];
		scores[k] = scores[k - 1];
		k--;
	}
	app->goto_list[k] = i;
	scores[k] = score;
}

void app_goto_update(App *app)
{
	const char *q = app->goto_text;
	app->goto_match = -1;
	app->goto_n = 0;
	app->goto_idx = 0;
	if (!*q)
		return;
	app->goto_list = xrealloc(app->goto_list, app->nwins * sizeof(*app->goto_list));
	const char *p = q;
	while (isdigit((unsigned char)*p))
		p++;
	if (!*p) {
		long n = strtol(q, NULL, 10);
		if (n >= 1 && (size_t)n <= app->nwins) {
			app->goto_list[0] = (size_t)n - 1;
			app->goto_n = 1;
			app->goto_match = n - 1;
		}
		return;
	}
	const char *slash = app->nservers > 1 ? strchr(q, '/') : NULL;
	size_t taglen = slash ? (size_t)(slash - q) : 0;
	if (slash)
		q = slash + 1;
	int *scores = xcalloc(app->nwins, sizeof(*scores));
	for (size_t i = 0; i < app->nwins; i++) {
		const char *name = app->wins[i]->name;
		const char *label = app->wins[i]->label;
		const char *bare = name[0] == '#' ? name + 1 : name;
		Server *srv = app->wins[i]->srv;
		if (slash && (!srv || strncasecmp_ascii(srv->name, app->goto_text, taglen) != 0))
			continue;
		int score;
		if (!*q || strncasecmp_ascii(bare, q, strlen(q)) == 0 || word_prefix(label, q))
			score = 20;
		else if (str_icontains(name, q) || (label && str_icontains(label, q)))
			score = 10;
		else
			continue;
		goto_add(app, scores, i, score + (int)app->wins[i]->activity);
	}
	free(scores);
	if (app->goto_n > 0)
		app->goto_match = (long)app->goto_list[0];
}

void app_goto_cycle(App *app, int dir)
{
	if (app->goto_n == 0)
		return;
	app->goto_idx = (app->goto_idx + app->goto_n + (size_t)(dir > 0 ? 1 : -1)) % app->goto_n;
	app->goto_match = (long)app->goto_list[app->goto_idx];
}

void app_jump_activity(App *app)
{
	size_t best = app->nwins;
	for (size_t i = 0; i < app->nwins; i++) {
		Window *w = app->wins[i];
		if (i == app->cur || w->activity == ACT_NONE)
			continue;
		if (best == app->nwins || w->activity > app->wins[best]->activity ||
		    (w->activity == app->wins[best]->activity && w->unread > app->wins[best]->unread))
			best = i;
	}
	if (best < app->nwins)
		app_switch(app, best);
}

static int selectable(const Line *ln)
{
	return ln->msgid && (ln->kind == LINE_MSG || ln->kind == LINE_ACTION);
}

static long selected_index(App *app)
{
	Window *w = current(app);
	if (!app->sel_id)
		return -1;
	for (size_t i = w->nlines; i > 0; i--)
		if (w->lines[i - 1].msgid && strcmp(w->lines[i - 1].msgid, app->sel_id) == 0)
			return (long)i - 1;
	return -1;
}

static void select_line(App *app, const Line *ln)
{
	free(app->sel_id);
	app->sel_id = xstrdup(ln->msgid);
	app->dirty = 1;
}

void app_select_start(App *app)
{
	Window *w = current(app);
	reset_modes(app);
	for (size_t i = w->nlines; i > 0; i--) {
		if (selectable(&w->lines[i - 1])) {
			select_line(app, &w->lines[i - 1]);
			return;
		}
	}
	app_error(app, w, "No message to select here");
}

void app_select_move(App *app, int dir)
{
	Window *w = current(app);
	long i = selected_index(app);
	if (i < 0) {
		app_select_start(app);
		return;
	}
	for (long k = i + dir; k >= 0 && (size_t)k < w->nlines; k += dir) {
		if (selectable(&w->lines[k])) {
			select_line(app, &w->lines[k]);
			return;
		}
	}
}

void app_scroll_start(App *app)
{
	reset_modes(app);
	app->scroll_mode = 1;
	app->dirty = 1;
}

void app_scroll_end(App *app)
{
	reset_modes(app);
	current(app)->scroll = 0;
	app->dirty = 1;
}

static int ci_contains(const char *s, const char *q)
{
	size_t n = strlen(q);
	if (!s || n == 0)
		return 0;
	for (; *s; s++) {
		size_t i = 0;
		while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)q[i]))
			i++;
		if (i == n)
			return 1;
	}
	return 0;
}

/* dir -1 searches older lines, 1 newer ones, starting at the current hit or at the bottom */
void app_search(App *app, int dir)
{
	Window *w = current(app);
	const char *q = app->search_text;
	if (app->search_hit > w->nlines)
		app->search_hit = 0;
	long k = app->search_hit ? (long)app->search_hit - 1 : dir < 0 ? (long)w->nlines : -1;
	for (k += dir; k >= 0 && (size_t)k < w->nlines; k += dir) {
		const Line *ln = &w->lines[k];
		if (ci_contains(ln->text, q) || ci_contains(ln->nick, q) || ci_contains(ln->name, q)) {
			app->search_hit = (size_t)k + 1;
			app->search_reveal = 1;
			app->search_failed = 0;
			app->dirty = 1;
			return;
		}
	}
	app->search_failed = 1;
	app->dirty = 1;
}

void app_select_end(App *app)
{
	free(app->sel_id);
	app->sel_id = NULL;
	app->react_active = 0;
	app->dirty = 1;
}

void app_reply_start(App *app)
{
	long i = selected_index(app);
	if (i < 0) {
		app_select_end(app);
		return;
	}
	const Line *ln = &current(app)->lines[i];
	app_reply_cancel(app);
	app->reply_tmid = xstrdup(ln->tmid ? ln->tmid : ln->msgid);
	app->reply_to = xstrdup(ln->name ? ln->name : ln->nick);
	app_select_end(app);
}

void app_reply_cancel(App *app)
{
	free(app->reply_tmid);
	app->reply_tmid = NULL;
	free(app->reply_to);
	app->reply_to = NULL;
	app->dirty = 1;
}

static const char *const emojis[] = {
	":thumbsup:", ":thumbsdown:", ":heart:", ":joy:", ":smile:", ":slight_smile:", ":laughing:",
	":rofl:", ":sweat_smile:", ":wink:", ":heart_eyes:", ":tada:", ":partying_face:", ":eyes:",
	":rocket:", ":pray:", ":clap:", ":raised_hands:", ":muscle:", ":fire:", ":100:", ":ok_hand:",
	":white_check_mark:", ":heavy_check_mark:", ":x:", ":thinking:", ":wave:", ":cry:", ":sob:",
	":scream:", ":see_no_evil:", ":sunglasses:", ":star:", ":coffee:", ":beer:", ":bug:", ":warning:",
	":question:", ":exclamation:", ":point_up:", ":face_palm:", ":shrug:", ":confused:",
};

void app_react_start(App *app)
{
	if (selected_index(app) < 0) {
		app_select_end(app);
		return;
	}
	app->react_active = 1;
	app->react_text[0] = '\0';
	app_react_update(app);
}

void app_react_update(App *app)
{
	const char *q = app->react_text[0] == ':' ? app->react_text + 1 : app->react_text;
	size_t n = strlen(q);
	while (n > 0 && q[n - 1] == ':')
		n--;
	size_t total = sizeof(emojis) / sizeof(emojis[0]);
	app->react_list = xrealloc(app->react_list, total * sizeof(*app->react_list));
	app->react_n = 0;
	app->react_idx = 0;
	app->dirty = 1;
	if (n == 0)
		return;
	for (int pass = 0; pass < 2; pass++) {
		for (size_t i = 0; i < total; i++) {
			int prefix = strncasecmp_ascii(emojis[i] + 1, q, n) == 0;
			int inside = 0;
			for (const char *p = emojis[i] + 2; *p && !inside; p++)
				inside = strncasecmp_ascii(p, q, n) == 0;
			if (pass == 0 ? prefix : (!prefix && inside))
				app->react_list[app->react_n++] = emojis[i];
		}
	}
}

void app_react_cycle(App *app, int dir)
{
	if (app->react_n == 0)
		return;
	app->react_idx = (app->react_idx + app->react_n + (size_t)(dir > 0 ? 1 : -1)) % app->react_n;
	app->dirty = 1;
}

void app_react_send(App *app)
{
	long i = selected_index(app);
	Buf emoji;
	buf_init(&emoji);
	if (app->react_n > 0) {
		buf_puts(&emoji, app->react_list[app->react_idx]);
	} else {
		const char *q = app->react_text;
		while (*q == ':')
			q++;
		size_t n = strlen(q);
		while (n > 0 && q[n - 1] == ':')
			n--;
		if (n > 0) {
			buf_putc(&emoji, ':');
			buf_append(&emoji, q, n);
			buf_putc(&emoji, ':');
		}
	}
	if (i >= 0 && emoji.len > 0) {
		Buf body;
		buf_init(&body);
		buf_puts(&body, "{\"messageId\":");
		buf_json_str(&body, app->sel_id);
		buf_puts(&body, ",\"emoji\":");
		buf_json_str(&body, emoji.data);
		buf_puts(&body, "}");
		Err e;
		Json *j = rc_post(&cur_srv(app)->rc, "/api/v1/chat.react", body.data, e.err, sizeof(e.err));
		buf_free(&body);
		if (!j)
			app_error(app, current(app), "Could not react with %s: %s", emoji.data, e.err);
		json_free(j);
	}
	buf_free(&emoji);
	app_select_end(app);
}
