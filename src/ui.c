#include "ui.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
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
	const char *init = "\x1b[?1049h\x1b[?7l\x1b[>1u\x1b[>4;1m\x1b[H\x1b[2J";
	write_all(init, strlen(init));
	return 0;
}

void ui_shutdown(void)
{
	if (!term_active)
		return;
	const char *fini = "\x1b[0m\x1b[?7h\x1b[?25h\x1b[<u\x1b[>4;0m\x1b[?1049l";
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
	{
		"tokyo-night",
		BG(26, 27, 38) ";" RGB(192, 202, 245),
		BG(41, 46, 66) ";" RGB(192, 202, 245),
		BG(41, 46, 66) ";1;" RGB(224, 175, 104),
		BG(41, 46, 66) ";1;" RGB(187, 154, 247),
		BG(41, 46, 66) ";1;" RGB(247, 118, 142),
		RGB(86, 95, 137),
		"1;" RGB(192, 202, 245),
		"1;" RGB(255, 158, 100),
		"1;" RGB(224, 175, 104),
		"1;" RGB(187, 154, 247),
		RGB(125, 207, 255),
		"1;" RGB(122, 162, 247),
		RGB(247, 118, 142),
		RGB(86, 95, 137),
		{ RGB(158, 206, 106), RGB(125, 207, 255), RGB(224, 175, 104), RGB(187, 154, 247),
		  RGB(255, 158, 100), RGB(115, 218, 202), RGB(122, 162, 247), RGB(247, 118, 142) },
	},
	{
		"catppuccin-mocha",
		BG(30, 30, 46) ";" RGB(205, 214, 244),
		BG(49, 50, 68) ";" RGB(205, 214, 244),
		BG(49, 50, 68) ";1;" RGB(249, 226, 175),
		BG(49, 50, 68) ";1;" RGB(245, 194, 231),
		BG(49, 50, 68) ";1;" RGB(243, 139, 168),
		RGB(108, 112, 134),
		"1;" RGB(205, 214, 244),
		"1;" RGB(250, 179, 135),
		"1;" RGB(249, 226, 175),
		"1;" RGB(203, 166, 247),
		RGB(148, 226, 213),
		"1;" RGB(137, 180, 250),
		RGB(243, 139, 168),
		RGB(108, 112, 134),
		{ RGB(166, 227, 161), RGB(137, 220, 235), RGB(250, 179, 135), RGB(203, 166, 247),
		  RGB(249, 226, 175), RGB(245, 194, 231), RGB(180, 190, 254), RGB(243, 139, 168) },
	},
	{
		"catppuccin-latte",
		BG(239, 241, 245) ";" RGB(76, 79, 105),
		BG(204, 208, 218) ";" RGB(76, 79, 105),
		BG(204, 208, 218) ";1;" RGB(76, 79, 105),
		BG(204, 208, 218) ";1;" RGB(234, 118, 203),
		BG(204, 208, 218) ";1;" RGB(210, 15, 57),
		RGB(156, 160, 176),
		"1;" RGB(76, 79, 105),
		"1;" RGB(254, 100, 11),
		"1;" RGB(223, 142, 29),
		"1;" RGB(136, 57, 239),
		RGB(23, 146, 153),
		"1;" RGB(30, 102, 245),
		RGB(210, 15, 57),
		RGB(156, 160, 176),
		{ RGB(64, 160, 43), RGB(4, 165, 229), RGB(254, 100, 11), RGB(136, 57, 239),
		  RGB(223, 142, 29), RGB(234, 118, 203), RGB(114, 135, 253), RGB(210, 15, 57) },
	},
	{
		"one-dark",
		BG(40, 44, 52) ";" RGB(171, 178, 191),
		BG(62, 68, 81) ";" RGB(171, 178, 191),
		BG(62, 68, 81) ";1;" RGB(229, 192, 123),
		BG(62, 68, 81) ";1;" RGB(198, 120, 221),
		BG(62, 68, 81) ";1;" RGB(224, 108, 117),
		RGB(92, 99, 112),
		"1;" RGB(171, 178, 191),
		"1;" RGB(209, 154, 102),
		"1;" RGB(229, 192, 123),
		"1;" RGB(198, 120, 221),
		RGB(86, 182, 194),
		"1;" RGB(97, 175, 239),
		RGB(224, 108, 117),
		RGB(92, 99, 112),
		{ RGB(152, 195, 121), RGB(86, 182, 194), RGB(229, 192, 123), RGB(198, 120, 221),
		  RGB(209, 154, 102), RGB(97, 175, 239), RGB(224, 108, 117) },
	},
	{
		"gruvbox-light",
		BG(251, 241, 199) ";" RGB(60, 56, 54),
		BG(235, 219, 178) ";" RGB(60, 56, 54),
		BG(235, 219, 178) ";1;" RGB(60, 56, 54),
		BG(235, 219, 178) ";1;" RGB(143, 63, 113),
		BG(235, 219, 178) ";1;" RGB(157, 0, 6),
		RGB(146, 131, 116),
		"1;" RGB(60, 56, 54),
		"1;" RGB(175, 58, 3),
		"1;" RGB(181, 118, 20),
		"1;" RGB(143, 63, 113),
		RGB(66, 123, 88),
		"1;" RGB(7, 102, 120),
		RGB(157, 0, 6),
		RGB(146, 131, 116),
		{ RGB(121, 116, 14), RGB(181, 118, 20), RGB(7, 102, 120), RGB(143, 63, 113),
		  RGB(66, 123, 88), RGB(175, 58, 3), RGB(157, 0, 6) },
	},
	{
		"everforest",
		BG(45, 53, 59) ";" RGB(211, 198, 170),
		BG(61, 72, 77) ";" RGB(211, 198, 170),
		BG(61, 72, 77) ";1;" RGB(219, 188, 127),
		BG(61, 72, 77) ";1;" RGB(214, 153, 182),
		BG(61, 72, 77) ";1;" RGB(230, 126, 128),
		RGB(133, 146, 137),
		"1;" RGB(211, 198, 170),
		"1;" RGB(230, 152, 117),
		"1;" RGB(219, 188, 127),
		"1;" RGB(214, 153, 182),
		RGB(131, 192, 146),
		"1;" RGB(127, 187, 179),
		RGB(230, 126, 128),
		RGB(133, 146, 137),
		{ RGB(167, 192, 128), RGB(127, 187, 179), RGB(219, 188, 127), RGB(214, 153, 182),
		  RGB(230, 152, 117), RGB(131, 192, 146), RGB(230, 126, 128) },
	},
	{
		"rose-pine",
		BG(25, 23, 36) ";" RGB(224, 222, 244),
		BG(38, 35, 58) ";" RGB(224, 222, 244),
		BG(38, 35, 58) ";1;" RGB(246, 193, 119),
		BG(38, 35, 58) ";1;" RGB(235, 188, 186),
		BG(38, 35, 58) ";1;" RGB(235, 111, 146),
		RGB(110, 106, 134),
		"1;" RGB(224, 222, 244),
		"1;" RGB(235, 188, 186),
		"1;" RGB(246, 193, 119),
		"1;" RGB(196, 167, 231),
		RGB(156, 207, 216),
		"1;" RGB(49, 116, 143),
		RGB(235, 111, 146),
		RGB(110, 106, 134),
		{ RGB(156, 207, 216), RGB(246, 193, 119), RGB(196, 167, 231), RGB(235, 188, 186),
		  RGB(235, 111, 146), RGB(49, 116, 143) },
	},
	{
		"kanagawa",
		BG(31, 31, 40) ";" RGB(220, 215, 186),
		BG(54, 54, 70) ";" RGB(220, 215, 186),
		BG(54, 54, 70) ";1;" RGB(230, 195, 132),
		BG(54, 54, 70) ";1;" RGB(210, 126, 153),
		BG(54, 54, 70) ";1;" RGB(255, 93, 98),
		RGB(114, 113, 105),
		"1;" RGB(220, 215, 186),
		"1;" RGB(255, 160, 102),
		"1;" RGB(230, 195, 132),
		"1;" RGB(149, 127, 184),
		RGB(122, 168, 159),
		"1;" RGB(126, 156, 216),
		RGB(255, 93, 98),
		RGB(114, 113, 105),
		{ RGB(152, 187, 108), RGB(127, 180, 202), RGB(230, 195, 132), RGB(149, 127, 184),
		  RGB(255, 160, 102), RGB(210, 126, 153), RGB(122, 168, 159), RGB(126, 156, 216) },
	},
	{
		"ayu-dark",
		BG(11, 14, 20) ";" RGB(191, 189, 182),
		BG(25, 30, 42) ";" RGB(191, 189, 182),
		BG(25, 30, 42) ";1;" RGB(230, 180, 80),
		BG(25, 30, 42) ";1;" RGB(210, 166, 255),
		BG(25, 30, 42) ";1;" RGB(240, 113, 120),
		RGB(92, 103, 115),
		"1;" RGB(191, 189, 182),
		"1;" RGB(255, 143, 64),
		"1;" RGB(230, 180, 80),
		"1;" RGB(210, 166, 255),
		RGB(149, 230, 203),
		"1;" RGB(89, 194, 255),
		RGB(240, 113, 120),
		RGB(92, 103, 115),
		{ RGB(170, 217, 76), RGB(89, 194, 255), RGB(230, 180, 80), RGB(210, 166, 255),
		  RGB(255, 143, 64), RGB(149, 230, 203), RGB(240, 113, 120) },
	},
	{
		"ayu-light",
		BG(248, 249, 250) ";" RGB(92, 97, 102),
		BG(231, 234, 237) ";" RGB(92, 97, 102),
		BG(231, 234, 237) ";1;" RGB(92, 97, 102),
		BG(231, 234, 237) ";1;" RGB(163, 122, 204),
		BG(231, 234, 237) ";1;" RGB(240, 113, 113),
		RGB(138, 145, 153),
		"1;" RGB(92, 97, 102),
		"1;" RGB(250, 141, 62),
		"1;" RGB(242, 174, 73),
		"1;" RGB(163, 122, 204),
		RGB(76, 191, 153),
		"1;" RGB(57, 158, 230),
		RGB(240, 113, 113),
		RGB(138, 145, 153),
		{ RGB(134, 179, 0), RGB(57, 158, 230), RGB(250, 141, 62), RGB(163, 122, 204),
		  RGB(76, 191, 153), RGB(240, 113, 113) },
	},
	{
		"github-dark",
		BG(13, 17, 23) ";" RGB(201, 209, 217),
		BG(33, 38, 45) ";" RGB(201, 209, 217),
		BG(33, 38, 45) ";1;" RGB(227, 179, 65),
		BG(33, 38, 45) ";1;" RGB(210, 168, 255),
		BG(33, 38, 45) ";1;" RGB(255, 123, 114),
		RGB(139, 148, 158),
		"1;" RGB(201, 209, 217),
		"1;" RGB(255, 166, 87),
		"1;" RGB(227, 179, 65),
		"1;" RGB(210, 168, 255),
		RGB(165, 214, 255),
		"1;" RGB(121, 192, 255),
		RGB(255, 123, 114),
		RGB(139, 148, 158),
		{ RGB(126, 231, 135), RGB(121, 192, 255), RGB(255, 166, 87), RGB(210, 168, 255),
		  RGB(227, 179, 65), RGB(165, 214, 255), RGB(255, 123, 114) },
	},
	{
		"github-light",
		BG(255, 255, 255) ";" RGB(36, 41, 47),
		BG(234, 238, 242) ";" RGB(36, 41, 47),
		BG(234, 238, 242) ";1;" RGB(36, 41, 47),
		BG(234, 238, 242) ";1;" RGB(130, 80, 223),
		BG(234, 238, 242) ";1;" RGB(207, 34, 46),
		RGB(110, 119, 129),
		"1;" RGB(36, 41, 47),
		"1;" RGB(149, 56, 0),
		"1;" RGB(154, 103, 0),
		"1;" RGB(130, 80, 223),
		RGB(27, 124, 131),
		"1;" RGB(9, 105, 218),
		RGB(207, 34, 46),
		RGB(110, 119, 129),
		{ RGB(17, 99, 41), RGB(9, 105, 218), RGB(149, 56, 0), RGB(130, 80, 223),
		  RGB(27, 124, 131), RGB(154, 103, 0), RGB(207, 34, 46) },
	},
	{
		"nightfox",
		BG(25, 35, 48) ";" RGB(205, 206, 207),
		BG(41, 57, 79) ";" RGB(205, 206, 207),
		BG(41, 57, 79) ";1;" RGB(219, 192, 116),
		BG(41, 57, 79) ";1;" RGB(214, 122, 210),
		BG(41, 57, 79) ";1;" RGB(201, 79, 109),
		RGB(115, 128, 145),
		"1;" RGB(205, 206, 207),
		"1;" RGB(244, 162, 97),
		"1;" RGB(219, 192, 116),
		"1;" RGB(157, 121, 214),
		RGB(99, 205, 207),
		"1;" RGB(113, 156, 214),
		RGB(201, 79, 109),
		RGB(115, 128, 145),
		{ RGB(129, 178, 154), RGB(113, 156, 214), RGB(219, 192, 116), RGB(157, 121, 214),
		  RGB(244, 162, 97), RGB(99, 205, 207), RGB(214, 122, 210), RGB(201, 79, 109) },
	},
	{
		"palenight",
		BG(41, 45, 62) ";" RGB(166, 172, 205),
		BG(52, 50, 74) ";" RGB(166, 172, 205),
		BG(52, 50, 74) ";1;" RGB(255, 203, 107),
		BG(52, 50, 74) ";1;" RGB(199, 146, 234),
		BG(52, 50, 74) ";1;" RGB(240, 113, 120),
		RGB(103, 110, 149),
		"1;" RGB(166, 172, 205),
		"1;" RGB(247, 140, 108),
		"1;" RGB(255, 203, 107),
		"1;" RGB(199, 146, 234),
		RGB(137, 221, 255),
		"1;" RGB(130, 170, 255),
		RGB(240, 113, 120),
		RGB(103, 110, 149),
		{ RGB(195, 232, 141), RGB(137, 221, 255), RGB(255, 203, 107), RGB(199, 146, 234),
		  RGB(247, 140, 108), RGB(130, 170, 255), RGB(240, 113, 120) },
	},
	{
		"zenburn",
		BG(63, 63, 63) ";" RGB(220, 220, 204),
		BG(79, 79, 79) ";" RGB(220, 220, 204),
		BG(79, 79, 79) ";1;" RGB(240, 223, 175),
		BG(79, 79, 79) ";1;" RGB(220, 140, 195),
		BG(79, 79, 79) ";1;" RGB(204, 147, 147),
		RGB(112, 144, 112),
		"1;" RGB(220, 220, 204),
		"1;" RGB(223, 175, 143),
		"1;" RGB(240, 223, 175),
		"1;" RGB(220, 140, 195),
		RGB(147, 224, 227),
		"1;" RGB(140, 208, 211),
		RGB(204, 147, 147),
		RGB(112, 144, 112),
		{ RGB(143, 176, 143), RGB(140, 208, 211), RGB(223, 175, 143), RGB(220, 140, 195),
		  RGB(240, 223, 175), RGB(147, 224, 227), RGB(204, 147, 147) },
	},
	{
		"papercolor-light",
		BG(238, 238, 238) ";" RGB(68, 68, 68),
		BG(208, 208, 208) ";" RGB(68, 68, 68),
		BG(208, 208, 208) ";1;" RGB(68, 68, 68),
		BG(208, 208, 208) ";1;" RGB(215, 0, 135),
		BG(208, 208, 208) ";1;" RGB(175, 0, 0),
		RGB(135, 135, 135),
		"1;" RGB(68, 68, 68),
		"1;" RGB(215, 95, 0),
		"1;" RGB(215, 95, 0),
		"1;" RGB(135, 0, 175),
		RGB(0, 135, 175),
		"1;" RGB(0, 95, 175),
		RGB(175, 0, 0),
		RGB(135, 135, 135),
		{ RGB(0, 135, 0), RGB(0, 95, 175), RGB(215, 95, 0), RGB(135, 0, 175),
		  RGB(0, 135, 175), RGB(215, 0, 135), RGB(175, 0, 0) },
	},
	{
		"synthwave",
		BG(38, 35, 53) ";" RGB(241, 241, 241),
		BG(52, 41, 79) ";" RGB(241, 241, 241),
		BG(52, 41, 79) ";1;" RGB(254, 222, 93),
		BG(52, 41, 79) ";1;" RGB(255, 126, 219),
		BG(52, 41, 79) ";1;" RGB(254, 68, 80),
		RGB(132, 139, 189),
		"1;" RGB(241, 241, 241),
		"1;" RGB(249, 126, 114),
		"1;" RGB(254, 222, 93),
		"1;" RGB(255, 126, 219),
		RGB(54, 249, 246),
		"1;" RGB(46, 226, 250),
		RGB(254, 68, 80),
		RGB(132, 139, 189),
		{ RGB(114, 241, 184), RGB(54, 249, 246), RGB(254, 222, 93), RGB(255, 126, 219),
		  RGB(249, 126, 114), RGB(187, 134, 252), RGB(254, 68, 80) },
	},
	{
		"matrix",
		BG(0, 10, 0) ";" RGB(0, 220, 90),
		BG(0, 45, 15) ";" RGB(0, 220, 90),
		BG(0, 45, 15) ";1;" RGB(170, 255, 170),
		BG(0, 45, 15) ";1;" RGB(220, 255, 220),
		BG(0, 45, 15) ";1;" RGB(255, 90, 90),
		RGB(0, 120, 50),
		"1;" RGB(0, 220, 90),
		"1;" RGB(210, 255, 210),
		"1;" RGB(170, 255, 170),
		"1;" RGB(120, 255, 140),
		RGB(0, 180, 120),
		"1;" RGB(80, 255, 120),
		RGB(255, 90, 90),
		RGB(0, 120, 50),
		{ RGB(0, 255, 70), RGB(120, 255, 160), RGB(0, 190, 60), RGB(170, 255, 120),
		  RGB(60, 230, 160), RGB(200, 255, 200), RGB(0, 160, 90) },
	},
	{
		"amber",
		BG(18, 10, 0) ";" RGB(255, 176, 0),
		BG(55, 32, 0) ";" RGB(255, 176, 0),
		BG(55, 32, 0) ";1;" RGB(255, 220, 120),
		BG(55, 32, 0) ";1;" RGB(255, 240, 200),
		BG(55, 32, 0) ";1;" RGB(255, 90, 40),
		RGB(150, 100, 0),
		"1;" RGB(255, 176, 0),
		"1;" RGB(255, 230, 160),
		"1;" RGB(255, 220, 120),
		"1;" RGB(255, 200, 90),
		RGB(220, 150, 20),
		"1;" RGB(255, 200, 60),
		RGB(255, 90, 40),
		RGB(150, 100, 0),
		{ RGB(255, 200, 0), RGB(255, 150, 40), RGB(255, 225, 130), RGB(230, 160, 0),
		  RGB(255, 185, 90), RGB(255, 215, 60), RGB(210, 130, 20) },
	},
	{
		"mono", "", "7", "1;7", "1;4;7", "1;7", "2", "1", "1;4", "1", "3", "2", "1", "1", "2",
		{ "1" },
	},
};

static const Theme *theme = &themes[0];

int ui_set_theme(const char *name)
{
	size_t n = sizeof(themes) / sizeof(themes[0]);
	if (str_casecmp(name, "random") == 0) {
		static int seeded;
		if (!seeded) {
			srand((unsigned)time(NULL) ^ (unsigned)getpid());
			seeded = 1;
		}
		size_t cur = (size_t)(theme - themes);
		size_t i = (size_t)rand() % (n - 1);
		theme = &themes[i >= cur ? i + 1 : i];
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
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

static void render_line(const Line *ln, int width, RowList *rl, int selected)
{
	Buf pre;
	buf_init(&pre);
	int prew = 0;
	char ts[16];
	struct tm tm;
	localtime_r(&ln->ts, &tm);
	strftime(ts, sizeof(ts), "%H:%M", &tm);
	style(&pre, selected ? theme->bar_act : theme->time);
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
	size_t dim_end = ln->ctx_len < len ? ln->ctx_len : len;
	size_t dim_start = ln->meta_len < len ? len - ln->meta_len : 0;
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
		for (size_t a = row_start; a < row_end;) {
			size_t e = row_end;
			const char *st = text_style;
			if (a < dim_end) {
				e = row_end < dim_end ? row_end : dim_end;
				st = theme->daychange;
			} else if (a >= dim_start) {
				st = theme->daychange;
			} else if (row_end > dim_start) {
				e = dim_start;
			}
			style(&row, st);
			buf_append(&row, text + a, e - a);
			a = e;
		}
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

	Server *srv = win_server(app, w);
	const char *nick = srv && srv->rc.username ? srv->rc.username : "?";
	buf_puts(&s, "[");
	sw += 1 + put_trunc(&s, nick, 20);
	buf_puts(&s, "] ");
	sw += 2;

	snprintf(tmp, sizeof(tmp), "[%zu:", app->cur + 1);
	buf_puts(&s, tmp);
	sw += (int)strlen(tmp);
	if (app->nservers > 1 && w->srv) {
		sw += put_trunc(&s, w->srv->name, 12) + 1;
		buf_putc(&s, '/');
	}
	sw += put_trunc(&s, win_label(w), 30);
	buf_puts(&s, "] ");
	sw += 2;

	for (size_t i = 0; i < app->nservers; i++) {
		Server *o = app->servers[i];
		const char *st = state_label(o->rc.state);
		if (!st || (o != srv && o->rc.state != DDP_OFFLINE))
			continue;
		style(&s, theme->bar_warn);
		buf_putc(&s, '[');
		if (app->nservers > 1) {
			sw += put_trunc(&s, o->name, 12) + 2;
			buf_puts(&s, ": ");
		}
		buf_printf(&s, "%s]", st);
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
	} else if (app->react_active && app->react_n > 0) {
		snprintf(tmp, sizeof(tmp), "[React %zu/%zu: ", app->react_idx + 1, app->react_n);
		style(&hint, theme->bar_act);
		buf_puts(&hint, tmp);
		hw = (int)strlen(tmp) + 1;
		hw += put_trunc(&hint, app->react_list[app->react_idx], cols / 2);
		buf_putc(&hint, ']');
		style(&hint, theme->bar);
	} else if (app->goto_active && app->goto_n > 0) {
		snprintf(tmp, sizeof(tmp), "[Goto %zu/%zu: ", app->goto_idx + 1, app->goto_n);
		style(&hint, theme->bar_act);
		buf_puts(&hint, tmp);
		hw = (int)strlen(tmp) + 1;
		Window *m = app->wins[app->goto_match];
		if (app->nservers > 1 && m->srv) {
			hw += put_trunc(&hint, m->srv->name, 12) + 1;
			buf_putc(&hint, '/');
		}
		hw += put_trunc(&hint, win_label(m), cols / 2);
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

static void draw_messages(Window *w, const char *sel_id, size_t hit, int *reveal, Buf *out, int top, int height)
{
	size_t sel = 0;
	if (sel_id)
		for (size_t i = w->nlines; i > 0 && !sel; i--)
			if (w->lines[i - 1].msgid && strcmp(w->lines[i - 1].msgid, sel_id) == 0)
				sel = i;
	if (!sel && hit <= w->nlines)
		sel = hit;
	size_t sel_lo = 0, sel_hi = 0;
	RowList all = { NULL, 0, 0 };
	for (size_t i = w->nlines; i > 0 && (all.n < (size_t)(height + w->scroll) || (sel && (i >= sel || *reveal))); i--) {
		RowList one = { NULL, 0, 0 };
		render_line(&w->lines[i - 1], cols, &one, i == sel);
		if (i == sel) {
			sel_lo = all.n;
			sel_hi = all.n + one.n;
		}
		for (size_t k = one.n; k > 0; k--)
			rows_push(&all, one.rows[k - 1]);
		free(one.rows);
		if (i > 1 && day_key(w->lines[i - 2].ts) != day_key(w->lines[i - 1].ts))
			rows_push(&all, render_day_change(w->lines[i - 1].ts));
	}
	int total = (int)all.n;
	if (sel && !sel_id && *reveal) {
		int h = (int)(sel_hi - sel_lo);
		w->scroll = (int)sel_lo - (h < height ? (height - h) / 2 : 0);
		if (w->scroll < 0)
			w->scroll = 0;
	} else if (sel && sel_id) {
		if ((int)sel_lo < w->scroll)
			w->scroll = (int)sel_lo;
		else if ((int)sel_hi > w->scroll + height)
			w->scroll = (int)sel_hi - height;
	}
	if (w->scroll > 0 && total < height + w->scroll)
		w->scroll = total > height ? total - height : 0;
	*reveal = 0;

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

static int draw_prompt(Buf *out, int row, const char *label, const char *text, const char *hint)
{
	buf_printf(out, "\x1b[%d;1H", row);
	style(out, theme->bar_act);
	buf_printf(out, "[%s]", label);
	style(out, NULL);
	buf_putc(out, ' ');
	int col = (int)strlen(label) + 4 + put_trunc(out, text, cols / 3);
	style(out, theme->time);
	put_trunc(out, *text ? " " : "", 1);
	put_trunc(out, hint, cols - col - 1);
	style(out, NULL);
	buf_puts(out, "\x1b[K");
	return col;
}

static int draw_goto(App *app, Buf *out, int row)
{
	char hint[96];
	if (app->goto_match >= 0)
	{
		Window *m = app->wins[app->goto_match];
		int tagged = app->nservers > 1 && m->srv;
		snprintf(hint, sizeof(hint), "-> %ld:%s%s%s", app->goto_match + 1, tagged ? m->srv->name : "", tagged ? "/" : "",
			 win_label(m));
	}
	else if (app->goto_text[0])
		snprintf(hint, sizeof(hint), "no match");
	else
		snprintf(hint, sizeof(hint), "number or part of a name, Tab cycles, Enter switches, Esc cancels");
	return draw_prompt(out, row, "goto", app->goto_text, hint);
}

static int prompt_mode(const App *app)
{
	return app->goto_active || app->react_active || app->sel_id || app->search_active || app->scroll_mode;
}

static int input_rows(const App *app)
{
	if (prompt_mode(app))
		return 1;
	int n = 1;
	for (size_t i = 0; i < app->input.len; i++)
		n += app->input.text[i] == '\n';
	int max = (rows - 4) / 2;
	if (max < 1)
		max = 1;
	return n < max ? n : max;
}

static int put_input_line(Buf *out, const char *s, size_t len, size_t cur, int avail)
{
	size_t start = cur;
	int before = 0;
	while (start > 0) {
		size_t p = start - 1;
		while (p > 0 && ((unsigned char)s[p] & 0xc0) == 0x80)
			p--;
		int cw = str_width(s + p, start - p);
		if (before + cw > avail)
			break;
		before += cw;
		start = p;
	}
	int used = before, cw;
	size_t i = cur;
	while (i < len) {
		size_t l = next_char(s + i, len - i, &cw);
		if (used + cw > avail)
			break;
		used += cw;
		i += l;
	}
	buf_append(out, s + start, i - start);
	return before;
}

static int draw_input(App *app, Buf *out, int top, int nrows, int *crow)
{
	*crow = top;
	if (app->goto_active)
		return draw_goto(app, out, top);
	if (app->react_active) {
		char hint[96];
		if (app->react_n > 0)
			snprintf(hint, sizeof(hint), "-> %s", app->react_list[app->react_idx]);
		else if (app->react_text[0])
			snprintf(hint, sizeof(hint), "-> :%s:", app->react_text);
		else
			snprintf(hint, sizeof(hint), "emoji name, Tab cycles, Enter reacts (again removes it), Esc cancels");
		return draw_prompt(out, top, "react", app->react_text, hint);
	}
	if (app->sel_id)
		return draw_prompt(out, top, "select", "", "Up/Down move, Enter or r reply in thread, + react, Esc cancels");
	if (app->search_active)
		return draw_prompt(out, top, "search", app->search_text,
				   app->search_failed ? "no match" : "type to search older lines, Enter keeps, Esc cancels");
	if (app->scroll_mode) {
		char text[72];
		snprintf(text, sizeof(text), "%s%s", app->search_text[0] ? "/" : "", app->search_text);
		return draw_prompt(out, top, "scroll", text,
				   app->search_failed ? "no more matches"
				   : "j/k line, ^U/^D half page, ^B/^F page, g/G top/bottom, / search, n/N older/newer, q quits");
	}
	Window *w = app->wins[app->cur];
	Input *in = &app->input;
	buf_printf(out, "\x1b[%d;1H", top);
	style(out, NULL);
	buf_putc(out, '[');
	int pw = 1 + put_trunc(out, win_label(w), cols / 3);
	if (app->reply_to) {
		style(out, theme->daychange);
		buf_puts(out, " re ");
		pw += 4 + put_trunc(out, app->reply_to, cols / 4);
		style(out, NULL);
	}
	buf_puts(out, "] ");
	pw += 2;

	int avail = cols - pw - 1;
	if (avail < 1)
		avail = 1;
	int nl = 1, cl = 0;
	for (size_t i = 0; i < in->len; i++) {
		if (in->text[i] == '\n') {
			nl++;
			if (i < in->cur)
				cl++;
		}
	}
	int first = nl - nrows;
	if (first > cl)
		first = cl;
	int col = pw + 1;
	size_t pos = 0;
	for (int li = 0; li < nl && li < first + nrows; li++) {
		size_t end = pos;
		while (end < in->len && in->text[end] != '\n')
			end++;
		if (li >= first) {
			int r = top + li - first;
			if (r != top) {
				buf_printf(out, "\x1b[%d;1H", r);
				style(out, NULL);
				buf_printf(out, "%*s", pw, "");
			}
			int cur = li == cl;
			int before = put_input_line(out, in->text + pos, end - pos, cur ? in->cur - pos : 0, avail);
			buf_puts(out, "\x1b[K");
			if (cur) {
				*crow = r;
				col = pw + before + 1;
			}
		}
		pos = end + 1;
	}
	return col;
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
	int tw = 1;
	if (app->nservers > 1 && w->srv) {
		tw += put_trunc(&top, w->srv->name, 12) + 2;
		buf_puts(&top, ": ");
	}
	tw += put_trunc(&top, w->topic && *w->topic ? w->topic : win_label(w), cols - tw);
	draw_bar(&out, 1, top.data, tw);
	buf_free(&top);

	int nin = input_rows(app);
	draw_messages(w, app->sel_id, app->scroll_mode ? app->search_hit : 0, &app->search_reveal, &out, 2, rows - 2 - nin);
	draw_status(app, &out, rows - nin);
	int crow;
	int col = draw_input(app, &out, rows - nin + 1, nin, &crow);
	buf_printf(&out, "\x1b[%d;%dH\x1b[?25h", crow, col);
	write_all(out.data, out.len);
	buf_free(&out);
}

/* CSI u (kitty keyboard protocol) and CSI 27;mods;code ~ (xterm modifyOtherKeys) */
static void modified_key(Key *k, int code, int mods)
{
	int shift = mods & 1, alt = mods & 2, ctrl = mods & 4;
	if (code == 13)
		k->type = mods ? K_NEWLINE : K_ENTER;
	else if (code == 27)
		k->type = K_ESC;
	else if (code == 9)
		k->type = shift ? K_BTAB : K_TAB;
	else if (code == 127 || code == 8)
		k->type = K_BACKSPACE;
	else if (ctrl && code < 128 && isalpha(code)) {
		k->type = K_CTRL;
		k->c = tolower(code);
	} else if (alt && code > 0x20 && code < 0x7f) {
		k->type = K_ALT;
		k->c = code;
	} else if (!ctrl && !alt && code >= 0x20 && code < 0x7f) {
		k->type = K_CHAR;
		k->utf8[0] = (char)code;
		k->utf8[1] = '\0';
	}
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
				int prm[4] = {0, 0, 0, 0};
				int np = 0, sub = 0;
				while (j < n && ((p[j] >= '0' && p[j] <= '9') || p[j] == ';' || p[j] == ':')) {
					if (p[j] == ';') {
						if (np < 3)
							np++;
						sub = 0;
					} else if (p[j] == ':') {
						sub = 1;
					} else if (!sub && prm[np] < 100000) {
						prm[np] = prm[np] * 10 + (p[j] - '0');
					}
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
				case 'u':
					modified_key(&k, prm[0], prm[1] > 0 ? prm[1] - 1 : 0);
					break;
				case '~':
					switch (prm[0]) {
					case 1: case 7: k.type = K_HOME; break;
					case 4: case 8: k.type = K_END; break;
					case 3: k.type = K_DELETE; break;
					case 5: k.type = K_PGUP; break;
					case 6: k.type = K_PGDN; break;
					case 27: modified_key(&k, prm[2], prm[1] > 0 ? prm[1] - 1 : 0); break;
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
			} else if (p[i + 1] == '\r' || p[i + 1] == '\n') {
				k.type = K_NEWLINE;
				i += 2;
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

static size_t line_start(const Input *in, size_t pos)
{
	while (pos > 0 && in->text[pos - 1] != '\n')
		pos--;
	return pos;
}

static size_t line_end(const Input *in, size_t pos)
{
	while (pos < in->len && in->text[pos] != '\n')
		pos++;
	return pos;
}

static void insert(Input *in, const char *s, size_t n)
{
	if (in->len + n >= INPUT_MAX)
		return;
	memmove(in->text + in->cur + n, in->text + in->cur, in->len - in->cur);
	memcpy(in->text + in->cur, s, n);
	in->len += n;
	in->cur += n;
	in->text[in->len] = '\0';
}

static void line_move(Input *in, int dir)
{
	size_t ls = line_start(in, in->cur);
	int col = str_width(in->text + ls, in->cur - ls);
	size_t p = dir < 0 ? line_start(in, ls - 1) : line_end(in, in->cur) + 1;
	size_t end = line_end(in, p);
	int w = 0, cw;
	while (p < end) {
		size_t l = next_char(in->text + p, end - p, &cw);
		if (w + cw > col)
			break;
		w += cw;
		p += l;
	}
	in->cur = p;
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
	case K_CHAR:
		insert(in, k->utf8, strlen(k->utf8));
		return 1;
	case K_NEWLINE:
		insert(in, "\n", 1);
		return 1;
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
		in->cur = line_start(in, in->cur);
		return 1;
	case K_END:
		in->cur = line_end(in, in->cur);
		return 1;
	case K_UP:
		if (line_start(in, in->cur) > 0)
			line_move(in, -1);
		else
			history_move(in, -1);
		return 1;
	case K_DOWN:
		if (line_end(in, in->cur) < in->len)
			line_move(in, 1);
		else
			history_move(in, 1);
		return 1;
	case K_CTRL:
		switch (k->c) {
		case 'a': in->cur = line_start(in, in->cur); return 1;
		case 'e': in->cur = line_end(in, in->cur); return 1;
		case 'b': in->cur = prev_pos(in, in->cur); return 1;
		case 'f': in->cur = next_pos(in, in->cur); return 1;
		case 'd': delete_range(in, in->cur, next_pos(in, in->cur)); return 1;
		case 'k': {
			size_t e = line_end(in, in->cur);
			delete_range(in, in->cur, e > in->cur ? e : next_pos(in, in->cur));
			return 1;
		}
		case 'u': {
			size_t b = line_start(in, in->cur);
			delete_range(in, b < in->cur ? b : prev_pos(in, in->cur), in->cur);
			return 1;
		}
		case 'w': {
			size_t p = in->cur;
			while (p > 0 && (in->text[p - 1] == ' ' || in->text[p - 1] == '\n'))
				p--;
			while (p > 0 && in->text[p - 1] != ' ' && in->text[p - 1] != '\n')
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
