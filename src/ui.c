#include "ui.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

static struct termios saved_termios;
static int term_active;
static volatile sig_atomic_t resized;
static int rows = 24, cols = 80;
static char *history_draft;

static void on_winch(int sig)
{
	(void)sig;
	resized = 1;
}

static void update_size(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
		rows = ws.ws_row;
		cols = ws.ws_col;
	}
}

static void write_all(const char *s, size_t n)
{
	while (n > 0) {
		ssize_t w = write(STDOUT_FILENO, s, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		s += w;
		n -= (size_t)w;
	}
}

int ui_init(void)
{
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		return -1;
	if (tcgetattr(STDIN_FILENO, &saved_termios) < 0)
		return -1;
	struct termios t = saved_termios;
	t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
	t.c_cflag |= CS8;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) < 0)
		return -1;
	term_active = 1;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_winch;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGWINCH, &sa, NULL);

	update_size();
	const char *init = "\x1b[?1049h\x1b[?7l\x1b[H\x1b[2J";
	write_all(init, strlen(init));
	return 0;
}

void ui_shutdown(void)
{
	if (!term_active)
		return;
	const char *fini = "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";
	write_all(fini, strlen(fini));
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
	term_active = 0;
}

int ui_check_resize(void)
{
	if (!resized)
		return 0;
	resized = 0;
	update_size();
	return 1;
}

void ui_redraw_all(void)
{
	write_all("\x1b[2J", 4);
}

int ui_rows(void)
{
	return rows;
}

int ui_cols(void)
{
	return cols;
}

static size_t utf8_decode(const char *s, size_t n, unsigned *cp)
{
	const unsigned char *p = (const unsigned char *)s;
	if (n == 0)
		return 0;
	if (p[0] < 0x80) {
		*cp = p[0];
		return 1;
	}
	size_t len;
	unsigned v, min;
	if ((p[0] & 0xe0) == 0xc0) {
		len = 2;
		v = p[0] & 0x1f;
		min = 0x80;
	} else if ((p[0] & 0xf0) == 0xe0) {
		len = 3;
		v = p[0] & 0x0f;
		min = 0x800;
	} else if ((p[0] & 0xf8) == 0xf0) {
		len = 4;
		v = p[0] & 0x07;
		min = 0x10000;
	} else {
		return 0;
	}
	if (n < len)
		return 0;
	for (size_t i = 1; i < len; i++) {
		if ((p[i] & 0xc0) != 0x80)
			return 0;
		v = (v << 6) | (p[i] & 0x3f);
	}
	if (v < min || v > 0x10ffff || (v >= 0xd800 && v < 0xe000))
		return 0;
	*cp = v;
	return len;
}

static int cp_width(unsigned cp)
{
	if (cp < 0x80)
		return 1;
	int w = wcwidth((wchar_t)cp);
	return w < 0 ? 1 : w;
}

static size_t next_char(const char *s, size_t n, int *width)
{
	unsigned cp;
	size_t l = utf8_decode(s, n, &cp);
	if (l == 0) {
		*width = 1;
		return 1;
	}
	*width = cp_width(cp);
	return l;
}

static int str_width(const char *s, size_t n)
{
	int w = 0, cw;
	size_t i = 0;
	while (i < n) {
		i += next_char(s + i, n - i, &cw);
		w += cw;
	}
	return w;
}

char *ui_sanitize(const char *s)
{
	Buf b;
	buf_init(&b);
	size_t n = strlen(s);
	size_t i = 0;
	while (i < n) {
		unsigned cp;
		size_t l = utf8_decode(s + i, n - i, &cp);
		if (l == 0) {
			buf_putc(&b, '?');
			i++;
			continue;
		}
		if (cp == '\n')
			buf_putc(&b, '\n');
		else if (cp == '\t')
			buf_putc(&b, ' ');
		else if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0) || (cp >= 0x202a && cp <= 0x202e) ||
			 (cp >= 0x2066 && cp <= 0x2069))
			;
		else
			buf_append(&b, s + i, l);
		i += l;
	}
	return buf_steal(&b);
}

static int put_trunc(Buf *out, const char *s, int maxw)
{
	int w = 0, cw;
	size_t n = strlen(s), i = 0;
	while (i < n) {
		size_t l = next_char(s + i, n - i, &cw);
		if (w + cw > maxw)
			break;
		if (s[i] == '\n')
			buf_putc(out, ' ');
		else
			buf_append(out, s + i, l);
		w += cw;
		i += l;
	}
	return w;
}

static void put_spaces(Buf *out, int n)
{
	while (n-- > 0)
		buf_putc(out, ' ');
}

typedef struct {
	const char *name;
	const char *base;
	const char *bar;
	const char *bar_act;
	const char *bar_hl;
	const char *bar_warn;
	const char *time;
	const char *own;
	const char *hl_nick;
	const char *hl_text;
	const char *action;
	const char *event;
	const char *info;
	const char *error;
	const char *daychange;
	const char *nicks[9];
} Theme;

#define RGB(r, g, b) "38;2;" #r ";" #g ";" #b
#define BG(r, g, b) "48;2;" #r ";" #g ";" #b

static const Theme themes[] = {
	{
		"default", "", "7", "1;7", "1;7;35", "1;7;31", "", "1", "1;33", "1", "1;35", "36", "1;34", "31", "2",
		{ "32", "33", "34", "35", "36", "91", "92", "94", "95" },
	},
	{
		"solarized-dark",
		BG(0, 43, 54) ";" RGB(131, 148, 150),
		BG(7, 54, 66) ";" RGB(147, 161, 161),
		BG(7, 54, 66) ";1;" RGB(253, 246, 227),
		BG(7, 54, 66) ";1;" RGB(211, 54, 130),
		BG(7, 54, 66) ";1;" RGB(220, 50, 47),
		RGB(88, 110, 117),
		"1;" RGB(147, 161, 161),
		"1;" RGB(203, 75, 22),
		"1;" RGB(147, 161, 161),
		"1;" RGB(108, 113, 196),
		RGB(42, 161, 152),
		"1;" RGB(38, 139, 210),
		RGB(220, 50, 47),
		RGB(88, 110, 117),
		{ RGB(181, 137, 0), RGB(203, 75, 22), RGB(211, 54, 130), RGB(108, 113, 196), RGB(38, 139, 210),
		  RGB(42, 161, 152), RGB(133, 153, 0), RGB(220, 50, 47) },
	},
	{
		"solarized-light",
		BG(253, 246, 227) ";" RGB(101, 123, 131),
		BG(238, 232, 213) ";" RGB(88, 110, 117),
		BG(238, 232, 213) ";1;" RGB(7, 54, 66),
		BG(238, 232, 213) ";1;" RGB(211, 54, 130),
		BG(238, 232, 213) ";1;" RGB(220, 50, 47),
		RGB(147, 161, 161),
		"1;" RGB(7, 54, 66),
		"1;" RGB(203, 75, 22),
		"1;" RGB(7, 54, 66),
		"1;" RGB(108, 113, 196),
		RGB(42, 161, 152),
		"1;" RGB(38, 139, 210),
		RGB(220, 50, 47),
		RGB(147, 161, 161),
		{ RGB(181, 137, 0), RGB(203, 75, 22), RGB(211, 54, 130), RGB(108, 113, 196), RGB(38, 139, 210),
		  RGB(42, 161, 152), RGB(133, 153, 0), RGB(220, 50, 47) },
	},
	{
		"monokai",
		BG(39, 40, 34) ";" RGB(248, 248, 242),
		BG(62, 61, 50) ";" RGB(248, 248, 242),
		BG(62, 61, 50) ";1;" RGB(230, 219, 116),
		BG(62, 61, 50) ";1;" RGB(249, 38, 114),
		BG(62, 61, 50) ";1;" RGB(253, 151, 31),
		RGB(117, 113, 94),
		"1;" RGB(248, 248, 242),
		"1;" RGB(249, 38, 114),
		"1;" RGB(230, 219, 116),
		"1;" RGB(174, 129, 255),
		RGB(102, 217, 239),
		"1;" RGB(166, 226, 46),
		RGB(249, 38, 114),
		RGB(117, 113, 94),
		{ RGB(166, 226, 46), RGB(102, 217, 239), RGB(253, 151, 31), RGB(174, 129, 255), RGB(230, 219, 116),
		  RGB(249, 38, 114) },
	},
	{
		"gruvbox",
		BG(40, 40, 40) ";" RGB(235, 219, 178),
		BG(60, 56, 54) ";" RGB(235, 219, 178),
		BG(60, 56, 54) ";1;" RGB(250, 189, 47),
		BG(60, 56, 54) ";1;" RGB(211, 134, 155),
		BG(60, 56, 54) ";1;" RGB(251, 73, 52),
		RGB(146, 131, 116),
		"1;" RGB(235, 219, 178),
		"1;" RGB(254, 128, 25),
		"1;" RGB(250, 189, 47),
		"1;" RGB(211, 134, 155),
		RGB(142, 192, 124),
		"1;" RGB(131, 165, 152),
		RGB(251, 73, 52),
		RGB(146, 131, 116),
		{ RGB(184, 187, 38), RGB(250, 189, 47), RGB(131, 165, 152), RGB(211, 134, 155), RGB(142, 192, 124),
		  RGB(254, 128, 25), RGB(251, 73, 52) },
	},
	{
		"nord",
		BG(46, 52, 64) ";" RGB(216, 222, 233),
		BG(59, 66, 82) ";" RGB(216, 222, 233),
		BG(59, 66, 82) ";1;" RGB(235, 203, 139),
		BG(59, 66, 82) ";1;" RGB(180, 142, 173),
		BG(59, 66, 82) ";1;" RGB(191, 97, 106),
		RGB(97, 110, 136),
		"1;" RGB(236, 239, 244),
		"1;" RGB(208, 135, 112),
		"1;" RGB(235, 203, 139),
		"1;" RGB(180, 142, 173),
		RGB(143, 188, 187),
		"1;" RGB(129, 161, 193),
		RGB(191, 97, 106),
		RGB(97, 110, 136),
		{ RGB(136, 192, 208), RGB(163, 190, 140), RGB(235, 203, 139), RGB(208, 135, 112), RGB(180, 142, 173),
		  RGB(143, 188, 187), RGB(129, 161, 193), RGB(191, 97, 106) },
	},
	{
		"dracula",
		BG(40, 42, 54) ";" RGB(248, 248, 242),
		BG(68, 71, 90) ";" RGB(248, 248, 242),
		BG(68, 71, 90) ";1;" RGB(241, 250, 140),
		BG(68, 71, 90) ";1;" RGB(255, 121, 198),
		BG(68, 71, 90) ";1;" RGB(255, 85, 85),
		RGB(98, 114, 164),
		"1;" RGB(248, 248, 242),
		"1;" RGB(255, 184, 108),
		"1;" RGB(241, 250, 140),
		"1;" RGB(189, 147, 249),
		RGB(139, 233, 253),
		"1;" RGB(189, 147, 249),
		RGB(255, 85, 85),
		RGB(98, 114, 164),
		{ RGB(80, 250, 123), RGB(139, 233, 253), RGB(255, 184, 108), RGB(255, 121, 198), RGB(189, 147, 249),
		  RGB(241, 250, 140), RGB(255, 85, 85) },
	},
};

static const Theme *theme = &themes[0];

int ui_set_theme(const char *name)
{
	for (size_t i = 0; i < sizeof(themes) / sizeof(themes[0]); i++) {
		if (str_casecmp(themes[i].name, name) == 0) {
			theme = &themes[i];
			return 0;
		}
	}
	return -1;
}

const char *ui_theme_name(void)
{
	return theme->name;
}

const char *ui_theme_at(size_t i)
{
	return i < sizeof(themes) / sizeof(themes[0]) ? themes[i].name : NULL;
}

static void style(Buf *out, const char *params)
{
	buf_puts(out, "\x1b[0");
	if (*theme->base) {
		buf_putc(out, ';');
		buf_puts(out, theme->base);
	}
	if (params && *params) {
		buf_putc(out, ';');
		buf_puts(out, params);
	}
	buf_putc(out, 'm');
}

static const char *nick_color(const char *nick)
{
	size_t n = 0;
	while (n < sizeof(theme->nicks) / sizeof(theme->nicks[0]) && theme->nicks[n])
		n++;
	unsigned h = 5381;
	for (const char *p = nick; *p; p++)
		h = h * 33 + (unsigned char)*p;
	return theme->nicks[h % n];
}

typedef struct {
	char **rows;
	size_t n;
	size_t cap;
} RowList;

static void rows_push(RowList *rl, char *row)
{
	if (rl->n == rl->cap) {
		rl->cap = rl->cap ? rl->cap * 2 : 64;
		rl->rows = xrealloc(rl->rows, rl->cap * sizeof(*rl->rows));
	}
	rl->rows[rl->n++] = row;
}

static void put_marker(Buf *pre, const char *params)
{
	style(pre, params);
	buf_puts(pre, "-!-");
	style(pre, NULL);
	buf_putc(pre, ' ');
}

static void render_line(const Line *ln, int width, RowList *rl)
{
	Buf pre;
	buf_init(&pre);
	int prew = 0;
	char ts[16];
	struct tm tm;
	localtime_r(&ln->ts, &tm);
	strftime(ts, sizeof(ts), "%H:%M", &tm);
	style(&pre, theme->time);
	buf_puts(&pre, ts);
	style(&pre, NULL);
	buf_putc(&pre, ' ');
	prew += 6;

	const char *user = ln->nick ? ln->nick : "";
	const char *nick = ln->name ? ln->name : user;
	switch (ln->kind) {
	case LINE_MSG:
		buf_putc(&pre, '<');
		style(&pre, ln->own ? theme->own : ln->highlight ? theme->hl_nick : nick_color(user));
		buf_puts(&pre, nick);
		style(&pre, NULL);
		buf_puts(&pre, "> ");
		prew += str_width(nick, strlen(nick)) + 3;
		break;
	case LINE_ACTION:
		style(&pre, theme->action);
		buf_printf(&pre, " * %s", nick);
		style(&pre, NULL);
		buf_putc(&pre, ' ');
		prew += str_width(nick, strlen(nick)) + 4;
		break;
	case LINE_EVENT:
		put_marker(&pre, theme->event);
		prew += 4;
		break;
	case LINE_INFO:
		put_marker(&pre, theme->info);
		prew += 4;
		break;
	case LINE_ERROR:
		put_marker(&pre, theme->error);
		prew += 4;
		break;
	}

	const char *text_style = NULL;
	if (ln->kind == LINE_ERROR)
		text_style = theme->error;
	else if (ln->highlight)
		text_style = theme->hl_text;

	const char *text = ln->text ? ln->text : "";
	size_t len = strlen(text);
	int indent = prew < width / 2 ? prew : 2;
	int first_avail = width - prew;
	int avail = width - indent;
	if (first_avail < 1)
		first_avail = 1;
	if (avail < 1)
		avail = 1;

	size_t pos = 0, row_start = 0, last_space = 0;
	int have_space = 0, w = 0, cur_avail = first_avail, first = 1;
	for (;;) {
		int brk = 0;
		size_t row_end = pos, next_start = pos;
		if (pos >= len) {
			brk = 2;
		} else if (text[pos] == '\n') {
			brk = 1;
			next_start = pos + 1;
		} else {
			int cw;
			size_t l = next_char(text + pos, len - pos, &cw);
			if (w + cw > cur_avail && pos > row_start) {
				brk = 1;
				if (have_space) {
					row_end = last_space;
					next_start = last_space + 1;
				}
			} else {
				if (text[pos] == ' ') {
					last_space = pos;
					have_space = 1;
				}
				w += cw;
				pos += l;
			}
		}
		if (!brk)
			continue;

		Buf row;
		buf_init(&row);
		if (first)
			buf_append(&row, pre.data, pre.len);
		else
			put_spaces(&row, indent);
		style(&row, text_style);
		buf_append(&row, text + row_start, row_end - row_start);
		style(&row, NULL);
		rows_push(rl, buf_steal(&row));
		if (brk == 2)
			break;

		first = 0;
		row_start = next_start;
		pos = next_start;
		have_space = 0;
		w = 0;
		cur_avail = avail;
	}
	buf_free(&pre);
}

static char *render_day_change(time_t ts)
{
	char day[64];
	struct tm tm;
	localtime_r(&ts, &tm);
	strftime(day, sizeof(day), "%a %d %b %Y", &tm);
	Buf row;
	buf_init(&row);
	style(&row, theme->daychange);
	buf_puts(&row, "00:00 ");
	put_marker(&row, theme->daychange);
	style(&row, theme->daychange);
	buf_printf(&row, "Day changed to %s", day);
	style(&row, NULL);
	return buf_steal(&row);
}

static long day_key(time_t ts)
{
	struct tm tm;
	localtime_r(&ts, &tm);
	return (long)tm.tm_year * 1000 + tm.tm_yday;
}

static void draw_bar(Buf *out, int row, const char *content, int content_w)
{
	buf_printf(out, "\x1b[%d;1H", row);
	style(out, theme->bar);
	buf_puts(out, content);
	style(out, theme->bar);
	put_spaces(out, cols - content_w);
	style(out, NULL);
}

static const char *state_label(DdpState s)
{
	switch (s) {
	case DDP_OFFLINE: return "offline";
	case DDP_CONNECTING: return "connecting";
	case DDP_LOGGING_IN: return "logging in";
	default: return NULL;
	}
}

typedef struct {
	size_t idx;
	Activity act;
	int width;
	int shown;
} ActEntry;

static int act_entry_width(const Window *win, size_t idx)
{
	char tmp[32];
	int w = snprintf(tmp, sizeof(tmp), "%zu:", idx + 1);
	const char *label = win_label(win);
	int lw = str_width(label, strlen(label));
	w += lw < 20 ? lw : 20;
	if (win->unread > 0)
		w += snprintf(tmp, sizeof(tmp), "(%d)", win->unread);
	return w;
}

static int draw_activity(App *app, Buf *s, int avail)
{
	ActEntry *e = xcalloc(app->nwins, sizeof(*e));
	size_t n = 0;
	for (size_t i = 0; i < app->nwins; i++) {
		Window *win = app->wins[i];
		if (i == app->cur || win->activity == ACT_NONE)
			continue;
		e[n].idx = i;
		e[n].act = win->activity;
		e[n].width = act_entry_width(win, i);
		e[n].shown = 1;
		n++;
	}
	if (n == 0) {
		free(e);
		return 0;
	}
	size_t shown = n;
	for (;;) {
		int total = 8;
		for (size_t k = 0; k < n; k++)
			if (e[k].shown)
				total += e[k].width + 1;
		char more[16] = "";
		if (shown < n)
			total += snprintf(more, sizeof(more), "+%zu ", n - shown);
		if (total <= avail || shown == 0)
			break;
		size_t drop = n;
		for (size_t k = 0; k < n; k++) {
			if (!e[k].shown)
				continue;
			if (drop == n || e[k].act < e[drop].act ||
			    (e[k].act == e[drop].act && app->wins[e[k].idx]->unread <= app->wins[e[drop].idx]->unread))
				drop = k;
		}
		e[drop].shown = 0;
		shown--;
	}
	if (shown == 0 && 13 > avail) {
		free(e);
		return 0;
	}

	int used = 0;
	buf_puts(s, "[Act: ");
	used += 6;
	for (size_t k = 0; k < n; k++) {
		if (!e[k].shown)
			continue;
		Window *win = app->wins[e[k].idx];
		style(s, e[k].act == ACT_HIGHLIGHT ? theme->bar_hl : e[k].act == ACT_MSG ? theme->bar_act : theme->bar);
		char tmp[32];
		snprintf(tmp, sizeof(tmp), "%zu:", e[k].idx + 1);
		buf_puts(s, tmp);
		put_trunc(s, win_label(win), 20);
		if (win->unread > 0)
			buf_printf(s, "(%d)", win->unread);
		style(s, theme->bar);
		buf_putc(s, ' ');
		used += e[k].width + 1;
	}
	if (shown < n) {
		char more[16];
		used += snprintf(more, sizeof(more), "+%zu ", n - shown);
		buf_puts(s, more);
	}
	if (s->data[s->len - 1] == ' ') {
		s->len--;
		s->data[s->len] = '\0';
		used--;
	}
	buf_puts(s, "] ");
	used += 2;
	free(e);
	return used;
}

static void draw_status(App *app, Buf *out, int row)
{
	Window *w = app->wins[app->cur];
	Buf s;
	buf_init(&s);
	int sw = 0;
	char tmp[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(tmp, sizeof(tmp), " [%H:%M] ", &tm);
	buf_puts(&s, tmp);
	sw += (int)strlen(tmp);

	const char *nick = app->rc.username ? app->rc.username : "?";
	buf_puts(&s, "[");
	sw += 1 + put_trunc(&s, nick, 20);
	buf_puts(&s, "] ");
	sw += 2;

	snprintf(tmp, sizeof(tmp), "[%zu:", app->cur + 1);
	buf_puts(&s, tmp);
	sw += (int)strlen(tmp);
	sw += put_trunc(&s, win_label(w), 30);
	buf_puts(&s, "] ");
	sw += 2;

	const char *st = state_label(app->rc.state);
	if (st) {
		style(&s, theme->bar_warn);
		buf_printf(&s, "[%s]", st);
		style(&s, theme->bar);
		buf_putc(&s, ' ');
		sw += (int)strlen(st) + 3;
	}

	Buf hint;
	buf_init(&hint);
	int hw = 0;
	if (app->ncomp > 0) {
		snprintf(tmp, sizeof(tmp), "[Tab %zu/%zu: ", app->comp_idx + 1, app->ncomp);
		style(&hint, theme->bar_act);
		buf_puts(&hint, tmp);
		hw = (int)strlen(tmp) + 1;
		hw += put_trunc(&hint, app->comp_names[app->comp_idx], cols / 2);
		buf_putc(&hint, ']');
		style(&hint, theme->bar);
	} else if (app->goto_active && app->goto_n > 0) {
		snprintf(tmp, sizeof(tmp), "[Goto %zu/%zu: ", app->goto_idx + 1, app->goto_n);
		style(&hint, theme->bar_act);
		buf_puts(&hint, tmp);
		hw = (int)strlen(tmp) + 1;
		hw += put_trunc(&hint, win_label(app->wins[app->goto_match]), cols / 2);
		buf_putc(&hint, ']');
		style(&hint, theme->bar);
	}
	int reserved = (w->scroll > 0 ? 11 : 0) + (hw > 0 ? hw + 1 : 0);
	sw += draw_activity(app, &s, cols - sw - reserved);
	if (w->scroll > 0) {
		buf_puts(&s, "-- more -- ");
		sw += 11;
	}
	if (hw > 0 && sw + hw <= cols) {
		buf_append(&s, hint.data, hint.len);
		sw += hw;
	}

	if (sw > cols) {
		/* too wide, draw a plain bar */
		buf_free(&s);
		buf_init(&s);
		snprintf(tmp, sizeof(tmp), " [%zu]", app->cur + 1);
		buf_puts(&s, tmp);
		sw = (int)strlen(tmp);
		if (hw > 0 && sw + hw + 1 <= cols) {
			buf_putc(&s, ' ');
			buf_append(&s, hint.data, hint.len);
			sw += hw + 1;
		}
		sw += put_trunc(&s, win_label(w), cols - sw > 0 ? cols - sw : 0);
	}
	buf_free(&hint);
	draw_bar(out, row, s.data, sw);
	buf_free(&s);
}

static void draw_messages(Window *w, Buf *out, int top, int height)
{
	RowList all = { NULL, 0, 0 };
	for (size_t i = w->nlines; i > 0 && all.n < (size_t)(height + w->scroll); i--) {
		RowList one = { NULL, 0, 0 };
		render_line(&w->lines[i - 1], cols, &one);
		for (size_t k = one.n; k > 0; k--)
			rows_push(&all, one.rows[k - 1]);
		free(one.rows);
		if (i > 1 && day_key(w->lines[i - 2].ts) != day_key(w->lines[i - 1].ts))
			rows_push(&all, render_day_change(w->lines[i - 1].ts));
	}
	int total = (int)all.n;
	if (w->scroll > 0 && total < height + w->scroll)
		w->scroll = total > height ? total - height : 0;

	int visible = total - w->scroll;
	if (visible > height)
		visible = height;
	for (int r = 0; r < height; r++) {
		buf_printf(out, "\x1b[%d;1H", top + r);
		style(out, NULL);
		if (r < visible) {
			size_t idx = (size_t)(w->scroll + visible - 1 - r);
			buf_puts(out, all.rows[idx]);
		}
		buf_puts(out, "\x1b[K");
	}
	for (size_t i = 0; i < all.n; i++)
		free(all.rows[i]);
	free(all.rows);
}

static int draw_goto(App *app, Buf *out, int row)
{
	buf_printf(out, "\x1b[%d;1H", row);
	style(out, theme->bar_act);
	buf_puts(out, "[goto]");
	style(out, NULL);
	buf_putc(out, ' ');
	int col = 8 + put_trunc(out, app->goto_text, cols / 3);
	style(out, theme->time);
	if (app->goto_match >= 0) {
		Window *m = app->wins[app->goto_match];
		char tmp[32];
		snprintf(tmp, sizeof(tmp), "  -> %ld:", app->goto_match + 1);
		buf_puts(out, tmp);
		put_trunc(out, win_label(m), cols - col - (int)strlen(tmp) - 1);
	} else if (app->goto_text[0]) {
		buf_puts(out, "  no match");
	} else {
		buf_puts(out, "  number or part of a name, Tab cycles, Enter switches, Esc cancels");
	}
	style(out, NULL);
	buf_puts(out, "\x1b[K");
	return col;
}

static int draw_input(App *app, Buf *out, int row)
{
	if (app->goto_active)
		return draw_goto(app, out, row);
	Window *w = app->wins[app->cur];
	Input *in = &app->input;
	buf_printf(out, "\x1b[%d;1H", row);
	style(out, NULL);
	buf_putc(out, '[');
	int pw = 1 + put_trunc(out, win_label(w), cols / 3);
	buf_puts(out, "] ");
	pw += 2;

	int avail = cols - pw - 1;
	if (avail < 1)
		avail = 1;
	size_t start = in->cur;
	int before = 0;
	while (start > 0) {
		size_t p = start - 1;
		while (p > 0 && ((unsigned char)in->text[p] & 0xc0) == 0x80)
			p--;
		int cw = str_width(in->text + p, start - p);
		if (before + cw > avail)
			break;
		before += cw;
		start = p;
	}
	int used = before, cw;
	size_t i = in->cur;
	while (i < in->len) {
		size_t l = next_char(in->text + i, in->len - i, &cw);
		if (used + cw > avail)
			break;
		used += cw;
		i += l;
	}
	buf_append(out, in->text + start, i - start);
	buf_puts(out, "\x1b[K");
	return pw + before + 1;
}

void ui_draw(App *app)
{
	if (rows < 4 || cols < 10)
		return;
	Window *w = app->wins[app->cur];
	Buf out;
	buf_init(&out);
	buf_puts(&out, "\x1b[?25l");

	Buf top;
	buf_init(&top);
	buf_putc(&top, ' ');
	int tw = 1 + put_trunc(&top, w->topic && *w->topic ? w->topic : win_label(w), cols - 1);
	draw_bar(&out, 1, top.data, tw);
	buf_free(&top);

	draw_messages(w, &out, 2, rows - 3);
	draw_status(app, &out, rows - 1);
	int col = draw_input(app, &out, rows);
	buf_printf(&out, "\x1b[%d;%dH\x1b[?25h", rows, col);
	write_all(out.data, out.len);
	buf_free(&out);
}

size_t ui_parse_keys(const char *buf, size_t n, Key *keys, size_t max)
{
	size_t nk = 0, i = 0;
	const unsigned char *p = (const unsigned char *)buf;
	while (i < n && nk < max) {
		Key k;
		memset(&k, 0, sizeof(k));
		unsigned char c = p[i];
		if (c == 0x1b) {
			if (i + 1 >= n) {
				k.type = K_ESC;
				i++;
				keys[nk++] = k;
				continue;
			}
			if (p[i + 1] == '[') {
				size_t j = i + 2;
				int param = 0;
				int got_param = 0;
				while (j < n && ((p[j] >= '0' && p[j] <= '9') || p[j] == ';')) {
					if (p[j] != ';' && !got_param)
						param = param * 10 + (p[j] - '0');
					else
						got_param = 1;
					j++;
				}
				if (j >= n) {
					i = n;
					continue;
				}
				switch (p[j]) {
				case 'A': k.type = K_UP; break;
				case 'B': k.type = K_DOWN; break;
				case 'C': k.type = K_RIGHT; break;
				case 'D': k.type = K_LEFT; break;
				case 'H': k.type = K_HOME; break;
				case 'F': k.type = K_END; break;
				case 'Z': k.type = K_BTAB; break;
				case '~':
					switch (param) {
					case 1: case 7: k.type = K_HOME; break;
					case 4: case 8: k.type = K_END; break;
					case 3: k.type = K_DELETE; break;
					case 5: k.type = K_PGUP; break;
					case 6: k.type = K_PGDN; break;
					default: break;
					}
					break;
				default:
					break;
				}
				i = j + 1;
			} else if (p[i + 1] == 'O' && i + 2 < n) {
				switch (p[i + 2]) {
				case 'A': k.type = K_UP; break;
				case 'B': k.type = K_DOWN; break;
				case 'C': k.type = K_RIGHT; break;
				case 'D': k.type = K_LEFT; break;
				case 'H': k.type = K_HOME; break;
				case 'F': k.type = K_END; break;
				default: break;
				}
				i += 3;
			} else {
				k.type = K_ALT;
				k.c = p[i + 1];
				i += 2;
			}
		} else if (c == '\r' || c == '\n') {
			k.type = K_ENTER;
			i++;
		} else if (c == 0x7f || c == 0x08) {
			k.type = K_BACKSPACE;
			i++;
		} else if (c == '\t') {
			k.type = K_TAB;
			i++;
		} else if (c < 0x20) {
			if (c >= 1 && c <= 26) {
				k.type = K_CTRL;
				k.c = 'a' + c - 1;
			}
			i++;
		} else {
			size_t len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
			if (i + len > n)
				len = n - i;
			unsigned cp;
			if (utf8_decode(buf + i, len, &cp) == len) {
				k.type = K_CHAR;
				memcpy(k.utf8, buf + i, len);
				k.utf8[len] = '\0';
			}
			i += len;
		}
		if (k.type != K_NONE)
			keys[nk++] = k;
	}
	return nk;
}

static size_t prev_pos(const Input *in, size_t pos)
{
	if (pos == 0)
		return 0;
	pos--;
	while (pos > 0 && ((unsigned char)in->text[pos] & 0xc0) == 0x80)
		pos--;
	return pos;
}

static size_t next_pos(const Input *in, size_t pos)
{
	if (pos >= in->len)
		return in->len;
	pos++;
	while (pos < in->len && ((unsigned char)in->text[pos] & 0xc0) == 0x80)
		pos++;
	return pos;
}

static void delete_range(Input *in, size_t a, size_t b)
{
	if (b <= a)
		return;
	memmove(in->text + a, in->text + b, in->len - b);
	in->len -= b - a;
	in->text[in->len] = '\0';
	if (in->cur > b)
		in->cur -= b - a;
	else if (in->cur > a)
		in->cur = a;
}

void ui_input_set(Input *in, const char *s)
{
	size_t n = strlen(s);
	if (n >= INPUT_MAX)
		n = INPUT_MAX - 1;
	memcpy(in->text, s, n);
	in->text[n] = '\0';
	in->len = n;
	in->cur = n;
}

void ui_input_push_history(Input *in, const char *s)
{
	if (!*s)
		return;
	if (in->nhistory > 0 && strcmp(in->history[in->nhistory - 1], s) == 0) {
		in->hpos = in->nhistory;
		return;
	}
	if (in->nhistory == HISTORY_MAX) {
		free(in->history[0]);
		memmove(in->history, in->history + 1, (HISTORY_MAX - 1) * sizeof(*in->history));
		in->nhistory--;
	}
	in->history[in->nhistory++] = xstrdup(s);
	in->hpos = in->nhistory;
	free(history_draft);
	history_draft = NULL;
}

static void history_move(Input *in, int dir)
{
	int np = in->hpos + dir;
	if (np < 0 || np > in->nhistory)
		return;
	if (in->hpos == in->nhistory) {
		free(history_draft);
		history_draft = xstrdup(in->text);
	}
	in->hpos = np;
	ui_input_set(in, np == in->nhistory ? (history_draft ? history_draft : "") : in->history[np]);
}

int ui_input_key(Input *in, const Key *k)
{
	switch (k->type) {
	case K_CHAR: {
		size_t n = strlen(k->utf8);
		if (in->len + n >= INPUT_MAX)
			return 1;
		memmove(in->text + in->cur + n, in->text + in->cur, in->len - in->cur);
		memcpy(in->text + in->cur, k->utf8, n);
		in->len += n;
		in->cur += n;
		in->text[in->len] = '\0';
		return 1;
	}
	case K_BACKSPACE:
		delete_range(in, prev_pos(in, in->cur), in->cur);
		return 1;
	case K_DELETE:
		delete_range(in, in->cur, next_pos(in, in->cur));
		return 1;
	case K_LEFT:
		in->cur = prev_pos(in, in->cur);
		return 1;
	case K_RIGHT:
		in->cur = next_pos(in, in->cur);
		return 1;
	case K_HOME:
		in->cur = 0;
		return 1;
	case K_END:
		in->cur = in->len;
		return 1;
	case K_UP:
		history_move(in, -1);
		return 1;
	case K_DOWN:
		history_move(in, 1);
		return 1;
	case K_CTRL:
		switch (k->c) {
		case 'a': in->cur = 0; return 1;
		case 'e': in->cur = in->len; return 1;
		case 'b': in->cur = prev_pos(in, in->cur); return 1;
		case 'f': in->cur = next_pos(in, in->cur); return 1;
		case 'd': delete_range(in, in->cur, next_pos(in, in->cur)); return 1;
		case 'k': delete_range(in, in->cur, in->len); return 1;
		case 'u': delete_range(in, 0, in->cur); return 1;
		case 'w': {
			size_t p = in->cur;
			while (p > 0 && in->text[p - 1] == ' ')
				p--;
			while (p > 0 && in->text[p - 1] != ' ')
				p--;
			delete_range(in, p, in->cur);
			return 1;
		}
		default:
			return 0;
		}
	default:
		return 0;
	}
}
