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
}

static Window *win_new(App *app, const char *rid, const char *name, char type)
{
	Window *w = xcalloc(1, sizeof(*w));
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

static Window *win_by_rid(App *app, const char *rid)
{
	for (size_t i = 0; i < app->nwins; i++)
		if (app->wins[i]->rid && strcmp(app->wins[i]->rid, rid) == 0)
			return app->wins[i];
	return NULL;
}

static Window *win_by_name(App *app, const char *name)
{
	const char *bare = name[0] == '#' ? name + 1 : name;
	for (size_t i = 0; i < app->nwins; i++) {
		const char *wn = app->wins[i]->name;
		if (app->wins[i]->rid && str_casecmp(wn[0] == '#' ? wn + 1 : wn, bare) == 0)
			return app->wins[i];
	}
	return NULL;
}

static Window *current(App *app)
{
	return app->wins[app->cur];
}

static void load_history(App *app, Window *w);

static void win_remove(App *app, Window *w)
{
	size_t idx = win_index(app, w);
	if (idx == 0)
		return;
	win_free(w);
	memmove(app->wins + idx, app->wins + idx + 1, (app->nwins - idx - 1) * sizeof(*app->wins));
	app->nwins--;
	if (app->cur >= idx && app->cur > 0)
		app->cur--;
	Window *c = current(app);
	c->activity = ACT_NONE;
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

static void describe_attachments(App *app, Buf *b, const Json *m)
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
				base_url(&app->rc.url, b);
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

static int msg_to_line(App *app, const Json *m, const Window *w, Line *ln)
{
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
		if (json_get(m, "tmid"))
			buf_puts(&text, "[thread] ");
		if (t)
			buf_puts(&text, "[encrypted message]");
		else
			buf_puts(&text, msg);
		size_t before = text.len;
		Buf att;
		buf_init(&att);
		describe_attachments(app, &att, m);
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
	ln->text = ui_sanitize(text.data ? text.data : "");
	buf_free(&text);
	ln->msgid = id ? xstrdup(id) : NULL;
	ln->raw = xstrdup(msg);
	ln->own = app->rc.username && strcmp(user, app->rc.username) == 0;
	if (!ln->own && ln->kind == LINE_MSG)
		ln->highlight = w->type == 'd' || mentions_me(m, app->rc.username);
	return 0;
}

static void add_message(App *app, Window *w, const Json *m, int notify)
{
	Line ln;
	if (msg_to_line(app, m, w, &ln) < 0)
		return;
	if (ln.msgid) {
		Line *old = find_line(w, ln.msgid);
		if (old) {
			if (strcmp(old->raw, ln.raw) != 0 || ln.kind != old->kind) {
				char *tmp = old->text;
				old->text = ln.text;
				ln.text = tmp;
				tmp = old->raw;
				old->raw = ln.raw;
				ln.raw = tmp;
				old->kind = ln.kind;
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

static void load_history(App *app, Window *w)
{
	w->history_loaded = 1;
	const char *ep = history_endpoint(w->type);
	if (!w->rid || !ep)
		return;
	Buf path;
	buf_init(&path);
	buf_printf(&path, "/api/v1/%s.history?roomId=", ep);
	url_encode(&path, w->rid);
	buf_printf(&path, "&count=%d", HISTORY_COUNT);
	Err e;
	Json *j = rc_get(&app->rc, path.data, e.err, sizeof(e.err));
	buf_free(&path);
	if (!j) {
		app_error(app, w, "Could not load history: %s", e.err);
		return;
	}
	const Json *msgs = json_get(j, "messages");
	size_t n = json_len(msgs);
	Line *hist = xcalloc(n ? n : 1, sizeof(*hist));
	size_t nh = 0;
	for (size_t i = n; i > 0; i--) {
		Line ln;
		if (msg_to_line(app, json_at(msgs, i - 1), w, &ln) < 0)
			continue;
		if (ln.msgid && find_line(w, ln.msgid)) {
			line_free(&ln);
			continue;
		}
		hist[nh++] = ln;
	}
	json_free(j);

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

static void mark_read(App *app, Window *w)
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
	Json *j = rc_post(&app->rc, "/api/v1/subscriptions.read", body.data, e.err, sizeof(e.err));
	buf_free(&body);
	if (!j)
		app_error(app, NULL, "Could not mark %s as read: %s", w->name, e.err);
	json_free(j);
}

void app_switch(App *app, size_t idx)
{
	if (idx >= app->nwins)
		return;
	Window *old = current(app);
	if (old->needs_read && idx != app->cur)
		mark_read(app, old);
	app->cur = idx;
	Window *w = current(app);
	int unread = w->activity >= ACT_MSG || w->needs_read;
	w->activity = ACT_NONE;
	if (!w->history_loaded)
		load_history(app, w);
	if (unread)
		mark_read(app, w);
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

static Window *open_window(App *app, const char *rid, const char *name, char type)
{
	Window *w = win_by_rid(app, rid);
	if (w)
		return w;
	char *wn = window_name(name, type);
	w = win_new(app, rid, wn, type);
	free(wn);
	if (type == 'd')
		w->topic = xstrdup(w->name);
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

static void free_subs(App *app)
{
	for (size_t i = 0; i < app->nsubs; i++) {
		free(app->subs[i].rid);
		free(app->subs[i].name);
		free(app->subs[i].fname);
	}
	free(app->subs);
	app->subs = NULL;
	app->nsubs = 0;
}

static int load_subscriptions(App *app, int initial)
{
	Err e;
	Json *j = rc_get(&app->rc, "/api/v1/subscriptions.get", e.err, sizeof(e.err));
	if (!j) {
		app_error(app, NULL, "Could not load subscriptions: %s", e.err);
		return -1;
	}
	const Json *upd = json_get(j, "update");
	size_t n = json_len(upd);
	free_subs(app);
	app->subs = xcalloc(n ? n : 1, sizeof(*app->subs));
	for (size_t i = 0; i < n; i++) {
		const Json *s = json_at(upd, i);
		const char *rid = json_get_str(s, "rid");
		const char *t = json_get_str(s, "t");
		if (!rid || !t)
			continue;
		Subscription *sub = &app->subs[app->nsubs++];
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
	qsort(app->subs, app->nsubs, sizeof(*app->subs), sub_cmp);

	for (size_t i = 0; i < app->nsubs; i++) {
		Subscription *s = &app->subs[i];
		if (!s->open || win_by_rid(app, s->rid))
			continue;
		Window *w = open_window(app, s->rid, s->name, s->type);
		if (s->fname && !app->show_usernames) {
			free(w->topic);
			w->topic = ui_sanitize(s->fname);
		}
		if (initial && (s->unread > 0 || s->mentions > 0)) {
			w->activity = s->mentions > 0 || s->type == 'd' ? ACT_HIGHLIGHT : ACT_MSG;
			w->needs_read = 1;
		}
	}
	return 0;
}

static void load_topics(App *app)
{
	Err e;
	Json *j = rc_get(&app->rc, "/api/v1/rooms.get", e.err, sizeof(e.err));
	if (!j) {
		app_error(app, NULL, "Could not load room topics: %s", e.err);
		return;
	}
	const Json *upd = json_get(j, "update");
	for (size_t i = 0; i < json_len(upd); i++) {
		const Json *r = json_at(upd, i);
		const char *id = json_get_str(r, "_id");
		const char *topic = json_get_str(r, "topic");
		Window *w = id ? win_by_rid(app, id) : NULL;
		if (w && w->type != 'd' && topic) {
			free(w->topic);
			w->topic = ui_sanitize(topic);
		}
	}
	json_free(j);
}

static Subscription *sub_by_name(App *app, const char *name, int want_dm)
{
	const char *bare = name[0] == '#' ? name + 1 : name;
	for (size_t i = 0; i < app->nsubs; i++) {
		Subscription *s = &app->subs[i];
		if ((s->type == 'd') == want_dm && str_casecmp(s->name, bare) == 0)
			return s;
	}
	return NULL;
}

static void on_stream(void *ctx, const char *coll, const char *event, const Json *args)
{
	App *app = ctx;
	if (strcmp(coll, "stream-room-messages") == 0) {
		const Json *m = json_at(args, 0);
		const char *rid = json_get_str(m, "rid");
		if (!rid)
			return;
		Window *w = win_by_rid(app, rid);
		if (!w) {
			load_subscriptions(app, 0);
			w = win_by_rid(app, rid);
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
			if (action && strcmp(action, "removed") != 0 && rid && !win_by_rid(app, rid) &&
			    json_bool(json_get(obj, "open")))
				load_subscriptions(app, 0);
		} else if (strcmp(slash, "/rooms-changed") == 0) {
			const char *id = json_get_str(obj, "_id");
			Window *w = id ? win_by_rid(app, id) : NULL;
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
	App *app = ctx;
	if (is_error)
		app_error(app, NULL, "%s", text);
	else
		app_info(app, NULL, "%s", text);
}

static void on_ready(void *ctx)
{
	App *app = ctx;
	app->reconnect_delay = 0;
	app_info(app, NULL, "Realtime connection established");
	if (app->ever_ready) {
		load_subscriptions(app, 0);
		for (size_t i = 0; i < app->nwins; i++)
			if (app->wins[i]->history_loaded)
				load_history(app, app->wins[i]);
	}
	app->ever_ready = 1;
}

static const RcHandlers handlers = { on_stream, on_status, on_ready };

static void schedule_reconnect(App *app)
{
	app->reconnect_delay = app->reconnect_delay ? app->reconnect_delay * 2 : 2;
	if (app->reconnect_delay > RECONNECT_MAX)
		app->reconnect_delay = RECONNECT_MAX;
	app->reconnect_at = now_ms() + app->reconnect_delay * 1000LL;
	app_info(app, NULL, "Reconnecting in %d seconds", app->reconnect_delay);
}

void app_ddp_tick(App *app)
{
	if (app->rc.state != DDP_OFFLINE || app->reconnect_at < 0 || now_ms() < app->reconnect_at)
		return;
	Err e;
	app_info(app, NULL, "Connecting to realtime API");
	app->dirty = 1;
	if (rc_ddp_connect(&app->rc, e.err, sizeof(e.err)) < 0) {
		app_error(app, NULL, "Realtime connection failed: %s", e.err);
		schedule_reconnect(app);
	}
}

void app_ddp_readable(App *app)
{
	if (rc_ddp_process(&app->rc, &handlers, app) < 0) {
		app_error(app, NULL, "Realtime connection lost");
		schedule_reconnect(app);
	}
	app->dirty = 1;
}

void app_init(App *app)
{
	app->wins = NULL;
	app->nwins = 0;
	app->cur = 0;
	app->subs = NULL;
	app->nsubs = 0;
	memset(&app->input, 0, sizeof(app->input));
	app->quit = 0;
	app->dirty = 1;
	app->ever_ready = 0;
	app->show_usernames = 0;
	app->reconnect_at = 0;
	app->reconnect_delay = 0;
	Window *st = win_new(app, NULL, "(status)", 0);
	st->history_loaded = 1;
}

void app_free(App *app)
{
	for (size_t i = 0; i < app->nwins; i++)
		win_free(app->wins[i]);
	free(app->wins);
	free_subs(app);
	for (int i = 0; i < app->input.nhistory; i++)
		free(app->input.history[i]);
}

void app_start(App *app)
{
	Buf b;
	buf_init(&b);
	base_url(&app->rc.url, &b);
	free(app->wins[0]->topic);
	app->wins[0]->topic = xstrdup(b.data);
	app_info(app, NULL, "Logged in to %s as %s", b.data, app->rc.username);
	buf_free(&b);
	app_info(app, NULL, "Type /help for a list of commands, Alt+1..9 or Ctrl+N/P to switch windows");
	if (load_subscriptions(app, 1) == 0) {
		load_topics(app);
		app_info(app, NULL, "%zu rooms, %zu windows open", app->nsubs, app->nwins - 1);
	}
	app_ddp_tick(app);
}

static int send_text(App *app, Window *w, const char *text)
{
	if (!w->rid) {
		app_error(app, w, "Not in a room, use /join or /query, or switch windows with Alt+number");
		return -1;
	}
	Buf body;
	buf_init(&body);
	buf_puts(&body, "{\"message\":{\"rid\":");
	buf_json_str(&body, w->rid);
	buf_puts(&body, ",\"msg\":");
	buf_json_str(&body, text);
	buf_puts(&body, "}}");
	Err e;
	Json *j = rc_post(&app->rc, "/api/v1/chat.sendMessage", body.data, e.err, sizeof(e.err));
	buf_free(&body);
	if (!j) {
		app_error(app, w, "Message not sent: %s", e.err);
		return -1;
	}
	const Json *m = json_get(j, "message");
	if (m)
		add_message(app, w, m, 0);
	json_free(j);
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
	Json *j = rc_post(&app->rc, path, body.data, e.err, sizeof(e.err));
	buf_free(&body);
	if (!j) {
		app_error(app, w, "%s", e.err);
		return;
	}
	json_free(j);
	if (strcmp(action, "leave") == 0 || strcmp(action, "close") == 0)
		win_remove(app, w);
}

static Window *query_window(App *app, const char *user)
{
	const char *bare = user[0] == '@' ? user + 1 : user;
	Window *w = win_by_name(app, bare);
	if (w && w->type == 'd')
		return w;
	Subscription *s = sub_by_name(app, bare, 1);
	if (s)
		return open_window(app, s->rid, s->name, 'd');
	Buf body;
	buf_init(&body);
	buf_puts(&body, "{\"username\":");
	buf_json_str(&body, bare);
	buf_puts(&body, "}");
	Err e;
	Json *j = rc_post(&app->rc, "/api/v1/im.create", body.data, e.err, sizeof(e.err));
	buf_free(&body);
	if (!j) {
		app_error(app, NULL, "Could not open conversation with %s: %s", bare, e.err);
		return NULL;
	}
	const char *rid = json_str(json_path(j, "room._id"));
	if (!rid)
		rid = json_str(json_path(j, "room.rid"));
	w = rid ? open_window(app, rid, bare, 'd') : NULL;
	json_free(j);
	return w;
}

static void cmd_join(App *app, const char *arg)
{
	if (!*arg) {
		app_error(app, current(app), "Usage: /join #channel");
		return;
	}
	Window *w = win_by_name(app, arg);
	if (w && w->type != 'd') {
		app_switch(app, win_index(app, w));
		return;
	}
	Subscription *s = sub_by_name(app, arg, 0);
	if (s) {
		w = open_window(app, s->rid, s->name, s->type);
		app_switch(app, win_index(app, w));
		return;
	}
	const char *bare = arg[0] == '#' ? arg + 1 : arg;
	Buf path;
	buf_init(&path);
	buf_puts(&path, "/api/v1/channels.info?roomName=");
	url_encode(&path, bare);
	Err e;
	Json *info = rc_get(&app->rc, path.data, e.err, sizeof(e.err));
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
	Json *j = rc_post(&app->rc, "/api/v1/channels.join", body.data, e.err, sizeof(e.err));
	buf_free(&body);
	if (!j) {
		app_error(app, current(app), "Cannot join %s: %s", arg, e.err);
		json_free(info);
		return;
	}
	json_free(j);
	w = open_window(app, rid, name ? name : bare, 'c');
	if (topic) {
		free(w->topic);
		w->topic = ui_sanitize(topic);
	}
	json_free(info);
	app_switch(app, win_index(app, w));
	load_subscriptions(app, 0);
}

static Window *target_window(App *app, const char *arg)
{
	if (!*arg)
		return current(app);
	Window *w = win_by_name(app, arg);
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
	Window *w = query_window(app, user);
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
	Window *w = target[0] == '#' ? win_by_name(app, target) : query_window(app, target);
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
	Json *j = rc_post(&app->rc, "/api/v1/commands.run", body.data, e.err, sizeof(e.err));
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
		for (size_t i = 0; i < app->nwins; i++)
			app_info(app, current(app), "%3zu %s", i + 1, app->wins[i]->name);
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
	Json *j = rc_get(&app->rc, "/api/v1/channels.list?count=500&sort=%7B%22name%22%3A1%7D", e.err, sizeof(e.err));
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
	Json *j = rc_get(&app->rc, path.data, e.err, sizeof(e.err));
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
	app_info(app, current(app), "Current theme: %s. Available: %s", ui_theme_name(), b.data);
	buf_free(&b);
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
		"/theme [name]           list themes or switch to one",
		"/reconnect              reconnect the realtime connection",
		"/quit                   exit",
		"Keys: Alt+1..9,0 and Alt+q..o switch windows, Ctrl+N/P next/previous,",
		"      PgUp/PgDn scroll, Tab completes @mentions, // sends a leading slash",
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
		rc_ddp_disconnect(&app->rc);
		app->reconnect_delay = 0;
		app->reconnect_at = 0;
		app_ddp_tick(app);
	} else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0)
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

void app_tab_complete(App *app)
{
	Input *in = &app->input;
	Window *w = current(app);
	size_t start = in->cur;
	while (start > 0 && in->text[start - 1] != ' ')
		start--;
	const char *prefix = in->text + start;
	size_t plen = in->cur - start;
	size_t skip = plen > 0 && prefix[0] == '@';
	if (plen == skip)
		return;
	const char *match = NULL;
	for (size_t i = w->nlines; i > 0 && !match; i--) {
		const Line *ln = &w->lines[i - 1];
		if (!ln->nick || ln->own)
			continue;
		if (completes(ln->nick, prefix + skip, plen - skip) || completes(ln->name, prefix + skip, plen - skip))
			match = ln->nick;
	}
	if (!match)
		return;
	char completed[512];
	snprintf(completed, sizeof(completed), "@%s ", match);
	size_t clen = strlen(completed);
	if (in->len - plen + clen >= INPUT_MAX)
		return;
	memmove(in->text + start + clen, in->text + in->cur, in->len - in->cur);
	memcpy(in->text + start, completed, clen);
	in->len = in->len - plen + clen;
	in->cur = start + clen;
	in->text[in->len] = '\0';
	app->dirty = 1;
}
