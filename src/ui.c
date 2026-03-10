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

static const char *nick_color(const char *nick)
{
	static const char *colors[] = { "32", "33", "34", "35", "36", "91", "92", "93", "94", "95", "96" };
	unsigned h = 5381;
	for (const char *p = nick; *p; p++)
		h = h * 33 + (unsigned char)*p;
	return colors[h % (sizeof(colors) / sizeof(colors[0]))];
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

static void render_line(const Line *ln, int width, RowList *rl)
{
	Buf pre;
	buf_init(&pre);
	int prew = 0;
	char ts[16];
	struct tm tm;
	localtime_r(&ln->ts, &tm);
	strftime(ts, sizeof(ts), "%H:%M ", &tm);
	buf_puts(&pre, ts);
	prew += 6;

	const char *nick = ln->nick ? ln->nick : "";
	switch (ln->kind) {
	case LINE_MSG:
		if (ln->own)
			buf_printf(&pre, "<\x1b[1m%s\x1b[0m> ", nick);
		else if (ln->highlight)
			buf_printf(&pre, "<\x1b[1;33m%s\x1b[0m> ", nick);
		else
			buf_printf(&pre, "<\x1b[%sm%s\x1b[0m> ", nick_color(nick), nick);
		prew += str_width(nick, strlen(nick)) + 3;
		break;
	case LINE_ACTION:
		buf_printf(&pre, "\x1b[1m * %s\x1b[0m ", nick);
		prew += str_width(nick, strlen(nick)) + 4;
		break;
	case LINE_EVENT:
		buf_puts(&pre, "\x1b[36m-!-\x1b[0m ");
		prew += 4;
		break;
	case LINE_INFO:
		buf_puts(&pre, "\x1b[1;34m-!-\x1b[0m ");
		prew += 4;
		break;
	case LINE_ERROR:
		buf_puts(&pre, "\x1b[1;31m-!-\x1b[0m ");
		prew += 4;
		break;
	}

	const char *style = "";
	if (ln->kind == LINE_ERROR)
		style = "\x1b[31m";
	else if (ln->highlight)
		style = "\x1b[1m";

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
		buf_puts(&row, style);
		buf_append(&row, text + row_start, row_end - row_start);
		buf_puts(&row, "\x1b[0m");
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

static void draw_bar(Buf *out, int row, const char *content, int content_w)
{
	buf_printf(out, "\x1b[%d;1H\x1b[0;37;44m%s", row, content);
	put_spaces(out, cols - content_w);
	buf_puts(out, "\x1b[0m");
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
	sw += put_trunc(&s, w->name, 30);
	buf_puts(&s, "] ");
	sw += 2;

	const char *st = state_label(app->rc.state);
	if (st) {
		buf_printf(&s, "\x1b[1;31m[%s]\x1b[0;37;44m ", st);
		sw += (int)strlen(st) + 3;
	}

	int any = 0;
	for (size_t i = 0; i < app->nwins; i++) {
		Activity a = app->wins[i]->activity;
		if (i == app->cur || a == ACT_NONE)
			continue;
		if (!any) {
			buf_puts(&s, "[Act: ");
			sw += 6;
		} else {
			buf_putc(&s, ',');
			sw++;
		}
		any = 1;
		const char *col = a == ACT_HIGHLIGHT ? "1;35" : a == ACT_MSG ? "1;37" : "0;37";
		snprintf(tmp, sizeof(tmp), "%zu", i + 1);
		buf_printf(&s, "\x1b[%s;44m%s\x1b[0;37;44m", col, tmp);
		sw += (int)strlen(tmp);
	}
	if (any) {
		buf_puts(&s, "] ");
		sw += 2;
	}
	if (w->scroll > 0) {
		buf_puts(&s, "-- more --");
		sw += 10;
	}

	if (sw > cols) {
		/* too wide, draw a plain bar */
		buf_free(&s);
		buf_init(&s);
		snprintf(tmp, sizeof(tmp), " [%zu]", app->cur + 1);
		buf_puts(&s, tmp);
		sw = (int)strlen(tmp);
		sw += put_trunc(&s, w->name, cols - sw > 0 ? cols - sw : 0);
	}
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
	}
	int total = (int)all.n;
	if (w->scroll > 0 && total < height + w->scroll)
		w->scroll = total > height ? total - height : 0;

	int visible = total - w->scroll;
	if (visible > height)
		visible = height;
	for (int r = 0; r < height; r++) {
		buf_printf(out, "\x1b[%d;1H\x1b[0m", top + r);
		if (r < visible) {
			size_t idx = (size_t)(w->scroll + visible - 1 - r);
			buf_puts(out, all.rows[idx]);
		}
		buf_puts(out, "\x1b[0m\x1b[K");
	}
	for (size_t i = 0; i < all.n; i++)
		free(all.rows[i]);
	free(all.rows);
}

static int draw_input(App *app, Buf *out, int row)
{
	Window *w = app->wins[app->cur];
	Input *in = &app->input;
	buf_printf(out, "\x1b[%d;1H\x1b[0m[", row);
	int pw = 1 + put_trunc(out, w->name, cols / 3);
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
	int tw = 1 + put_trunc(&top, w->topic && *w->topic ? w->topic : w->name, cols - 1);
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
				i++;
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
