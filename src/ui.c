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
