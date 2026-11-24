#include "app.h"
#include "config.h"
#include "net.h"
#include "rc.h"
#include "ui.h"
#include "util.h"

#define ASCIIRC_VERSION "1.1.1"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t got_term;

static void on_term(int sig)
{
	(void)sig;
	got_term = 1;
}

static void usage(FILE *f)
{
	fputs("usage: asciirc [-S name] [-s server] [-i user_id] [-t token] [-u username] [-c config] [-k] [-v]\n"
	      "\n"
	      "  -S name      connect only to the server [name] of the config file\n"
	      "  -s server    server URL, e.g. https://chat.example.com\n"
	      "  -i user_id   user id belonging to a personal access token\n"
	      "  -t token     personal access token (prefer ASCIIRC_TOKEN or the config file)\n"
	      "  -u username  log in with username and password instead of a token\n"
	      "  -c config    config file (default: $XDG_CONFIG_HOME/asciirc/config)\n"
	      "  -k           do not verify TLS certificates\n"
	      "  -v           print the version\n"
	      "\n"
	      "Environment: ASCIIRC_SERVER, ASCIIRC_USER_ID, ASCIIRC_TOKEN, ASCIIRC_USERNAME\n"
	      "Config file lines: server = ..., user_id = ..., token = ..., username = ...,\n"
	      "                   insecure = 1, nicks = name|username, theme = name\n"
	      "Several servers: put each one's lines below a [name] line.\n",
	      f);
}

static char *default_config_path(void)
{
	Buf b;
	buf_init(&b);
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && *xdg)
		buf_printf(&b, "%s/asciirc/config", xdg);
	else if (home)
		buf_printf(&b, "%s/.config/asciirc/config", home);
	else
		return NULL;
	return buf_steal(&b);
}

static char *read_password(const char *prompt)
{
	int fd = open("/dev/tty", O_RDWR);
	if (fd < 0)
		return NULL;
	struct termios old, t;
	int have_tty = tcgetattr(fd, &old) == 0;
	if (have_tty) {
		t = old;
		t.c_lflag &= ~(tcflag_t)ECHO;
		tcsetattr(fd, TCSAFLUSH, &t);
	}
	if (write(fd, prompt, strlen(prompt)) < 0) {
	}
	char buf[512];
	size_t n = 0;
	for (;;) {
		char c;
		ssize_t r = read(fd, &c, 1);
		if (r <= 0 || c == '\n' || c == '\r')
			break;
		if (n < sizeof(buf) - 1)
			buf[n++] = c;
	}
	buf[n] = '\0';
	if (have_tty)
		tcsetattr(fd, TCSAFLUSH, &old);
	if (write(fd, "\n", 1) < 0) {
	}
	close(fd);
	char *p = xstrdup(buf);
	memset(buf, 0, sizeof(buf));
	return p;
}

static void prompt_edit(char *t, size_t cap, const Key *k)
{
	size_t len = strlen(t);
	if (k->type == K_CHAR && len + strlen(k->utf8) < cap) {
		strcat(t, k->utf8);
	} else if (k->type == K_BACKSPACE) {
		while (len > 0 && ((unsigned char)t[len - 1] & 0xc0) == 0x80)
			len--;
		if (len > 0)
			len--;
		t[len] = '\0';
	} else if (k->type == K_CTRL && k->c == 'u') {
		t[0] = '\0';
	}
}

static void handle_goto_key(App *app, const Key *k)
{
	switch (k->type) {
	case K_ENTER:
		app->goto_active = 0;
		if (app->goto_match >= 0)
			app_switch(app, (size_t)app->goto_match);
		return;
	case K_ESC:
		app->goto_active = 0;
		return;
	case K_TAB:
		app_goto_cycle(app, 1);
		return;
	case K_BTAB:
		app_goto_cycle(app, -1);
		return;
	case K_CTRL:
		if (k->c == 'g' || k->c == 'c') {
			app->goto_active = 0;
			return;
		}
		break;
	default:
		break;
	}
	prompt_edit(app->goto_text, sizeof(app->goto_text), k);
	app_goto_update(app);
}

static void handle_react_key(App *app, const Key *k)
{
	switch (k->type) {
	case K_ENTER:
		app_react_send(app);
		return;
	case K_ESC:
		app->react_active = 0;
		return;
	case K_TAB:
		app_react_cycle(app, 1);
		return;
	case K_BTAB:
		app_react_cycle(app, -1);
		return;
	case K_CTRL:
		if (k->c == 'r' || k->c == 'c') {
			app_select_end(app);
			return;
		}
		break;
	default:
		break;
	}
	prompt_edit(app->react_text, sizeof(app->react_text), k);
	app_react_update(app);
}

static void handle_select_key(App *app, const Key *k)
{
	const char *c = k->type == K_CHAR ? k->utf8 : "";
	if (k->type == K_UP || strcmp(c, "k") == 0)
		app_select_move(app, -1);
	else if (k->type == K_DOWN || strcmp(c, "j") == 0)
		app_select_move(app, 1);
	else if (k->type == K_PGUP || k->type == K_PGDN)
		for (int i = 0; i < 10; i++)
			app_select_move(app, k->type == K_PGUP ? -1 : 1);
	else if (k->type == K_ENTER || strcmp(c, "r") == 0)
		app_reply_start(app);
	else if (strcmp(c, "+") == 0)
		app_react_start(app);
	else if (k->type == K_ESC || strcmp(c, "q") == 0 || (k->type == K_CTRL && (k->c == 'r' || k->c == 'c')))
		app_select_end(app);
}

static void handle_search_key(App *app, const Key *k)
{
	if (k->type == K_ENTER || k->type == K_ESC || (k->type == K_CTRL && k->c == 'c')) {
		app->search_active = 0;
		if (k->type != K_ENTER) {
			app->search_text[0] = '\0';
			app->search_hit = 0;
			app->search_failed = 0;
		}
		return;
	}
	prompt_edit(app->search_text, sizeof(app->search_text), k);
	app->search_hit = 0;
	app->search_failed = 0;
	if (app->search_text[0])
		app_search(app, -1);
}

static void handle_scroll_key(App *app, const Key *k, int page)
{
	Window *w = app->wins[app->cur];
	const char *c = k->type == K_CHAR ? k->utf8 : "";
	int ctrl = k->type == K_CTRL ? k->c : 0;
	if (k->type == K_UP || strcmp(c, "k") == 0 || ctrl == 'y')
		w->scroll += 1;
	else if (k->type == K_DOWN || strcmp(c, "j") == 0 || ctrl == 'e')
		w->scroll -= 1;
	else if (ctrl == 'u')
		w->scroll += page;
	else if (ctrl == 'd')
		w->scroll -= page;
	else if (k->type == K_PGUP || ctrl == 'b')
		w->scroll += 2 * page;
	else if (k->type == K_PGDN || ctrl == 'f' || strcmp(c, " ") == 0)
		w->scroll -= 2 * page;
	else if (k->type == K_HOME || strcmp(c, "g") == 0)
		w->scroll = 1 << 30;
	else if (k->type == K_END || strcmp(c, "G") == 0)
		w->scroll = 0;
	else if (strcmp(c, "/") == 0) {
		app->search_active = 1;
		app->search_text[0] = '\0';
		app->search_hit = 0;
		app->search_failed = 0;
	} else if (strcmp(c, "n") == 0 && app->search_text[0])
		app_search(app, -1);
	else if (strcmp(c, "N") == 0 && app->search_text[0])
		app_search(app, 1);
	else if (k->type == K_ESC || strcmp(c, "q") == 0 || ctrl == 's' || ctrl == 'c')
		app_scroll_end(app);
	if (w->scroll < 0)
		w->scroll = 0;
}

static void handle_key(App *app, const Key *k)
{
	Window *w = app->wins[app->cur];
	int page = (ui_rows() - 3) / 2;
	if (page < 1)
		page = 1;
	if (k->type != K_TAB && k->type != K_BTAB)
		app_tab_reset(app);
	if (app->goto_active || app->react_active || app->sel_id || app->scroll_mode) {
		if (app->goto_active)
			handle_goto_key(app, k);
		else if (app->react_active)
			handle_react_key(app, k);
		else if (app->sel_id)
			handle_select_key(app, k);
		else if (app->search_active)
			handle_search_key(app, k);
		else
			handle_scroll_key(app, k, page);
		app->dirty = 1;
		return;
	}
	switch (k->type) {
	case K_ESC:
		if (app->reply_tmid)
			app_reply_cancel(app);
		break;
	case K_ENTER: {
		char *line = xstrdup(app->input.text);
		ui_input_push_history(&app->input, line);
		ui_input_set(&app->input, "");
		app->input.hpos = app->input.nhistory;
		w->scroll = 0;
		app_submit(app, line);
		free(line);
		break;
	}
	case K_PGUP:
		w->scroll += page;
		break;
	case K_PGDN:
		w->scroll -= page;
		if (w->scroll < 0)
			w->scroll = 0;
		break;
	case K_TAB:
		app_tab_complete(app, 1);
		break;
	case K_BTAB:
		app_tab_complete(app, -1);
		break;
	case K_ALT: {
		static const char *keys = "1234567890qwertyuio";
		const char *p = k->c ? strchr(keys, k->c) : NULL;
		if (p)
			app_switch(app, (size_t)(p - keys));
		break;
	}
	case K_CTRL:
		if (k->c == 'n')
			app_switch(app, (app->cur + 1) % app->nwins);
		else if (k->c == 'p')
			app_switch(app, (app->cur + app->nwins - 1) % app->nwins);
		else if (k->c == 'l')
			ui_redraw_all();
		else if (k->c == 'g') {
			app->goto_active = 1;
			app->goto_text[0] = '\0';
			app_goto_update(app);
		} else if (k->c == 'x')
			app_jump_activity(app);
		else if (k->c == 'r')
			app_select_start(app);
		else if (k->c == 's')
			app_scroll_start(app);
		else
			ui_input_key(&app->input, k);
		break;
	default:
		ui_input_key(&app->input, k);
		break;
	}
	app->dirty = 1;
}

static int run(App *app)
{
	struct pollfd *fds = xcalloc(app->nservers + 1, sizeof(*fds));
	Server **owner = xcalloc(app->nservers + 1, sizeof(*owner));
	int ret = 0;
	ui_draw(app);
	while (!app->quit && !got_term) {
		nfds_t nfds = 0;
		fds[nfds].fd = STDIN_FILENO;
		fds[nfds].events = POLLIN;
		nfds++;
		for (size_t i = 0; i < app->nservers; i++) {
			int fd = rc_ddp_fd(&app->servers[i]->rc);
			if (fd < 0)
				continue;
			fds[nfds].fd = fd;
			fds[nfds].events = POLLIN;
			owner[nfds] = app->servers[i];
			nfds++;
		}

		long long now = now_ms();
		long long timeout = 60000 - now % 60000 + 10;
		long long next = app_next_reconnect(app);
		if (next >= 0 && next - now < timeout)
			timeout = next - now < 0 ? 0 : next - now;
		int r = poll(fds, nfds, (int)timeout);
		if (r < 0 && errno != EINTR) {
			ret = -1;
			break;
		}

		if (ui_check_resize())
			app->dirty = 1;
		if (r > 0 && (fds[0].revents & (POLLIN | POLLHUP))) {
			char buf[4096];
			ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
			if (n <= 0)
				break;
			Key keys[512];
			size_t nk = ui_parse_keys(buf, (size_t)n, keys, sizeof(keys) / sizeof(keys[0]));
			for (size_t i = 0; i < nk && !app->quit; i++)
				handle_key(app, &keys[i]);
		}
		for (nfds_t i = 1; r > 0 && i < nfds; i++)
			if (fds[i].revents)
				app_ddp_readable(app, owner[i]);
		app_ddp_tick(app);
		if (r == 0)
			app->dirty = 1;
		if (app->dirty && !app->quit) {
			ui_draw(app);
			app->dirty = 0;
		}
	}
	free(fds);
	free(owner);
	return ret;
}

static int login(const ServerConfig *sc, int several, Rc *rc, char *err, size_t errlen)
{
	Url url;
	if (url_parse(sc->server, &url) < 0) {
		snprintf(err, errlen, "invalid server URL: %s", sc->server);
		return -1;
	}
	url.insecure = sc->insecure;
	rc_init(rc, &url);
	if (sc->token) {
		if (!sc->user_id) {
			snprintf(err, errlen, "a personal access token also needs the user id (user_id, -i or ASCIIRC_USER_ID)");
			return -1;
		}
		rc->user_id = xstrdup(sc->user_id);
		rc->token = xstrdup(sc->token);
	} else if (sc->username) {
		char prompt[300];
		if (several)
			snprintf(prompt, sizeof(prompt), "Password for %s on %s: ", sc->username, sc->name);
		else
			snprintf(prompt, sizeof(prompt), "Password: ");
		char *pw = read_password(prompt);
		if (!pw) {
			snprintf(err, errlen, "cannot read password");
			return -1;
		}
		int r = rc_login_password(rc, sc->username, pw, err, errlen);
		memset(pw, 0, strlen(pw));
		free(pw);
		if (r < 0)
			return -1;
	} else {
		snprintf(err, errlen, "no credentials, give a user id and token or a username");
		return -1;
	}
	return rc_fetch_me(rc, err, errlen);
}

int main(int argc, char **argv)
{
	setlocale(LC_CTYPE, "");

	Config cfg;
	memset(&cfg, 0, sizeof(cfg));
	ServerConfig ov;
	memset(&ov, 0, sizeof(ov));
	const char *config_path = NULL, *select = NULL;
	ov.server = getenv("ASCIIRC_SERVER");
	ov.user_id = getenv("ASCIIRC_USER_ID");
	ov.token = getenv("ASCIIRC_TOKEN");
	ov.username = getenv("ASCIIRC_USERNAME");
	int c, opt_t = 0, opt_u = 0;
	while ((c = getopt(argc, argv, "S:s:i:t:u:c:khv")) != -1) {
		switch (c) {
		case 'S': select = optarg; break;
		case 's': ov.server = optarg; break;
		case 'i': ov.user_id = optarg; break;
		case 't': ov.token = optarg; opt_t = 1; break;
		case 'u': ov.username = optarg; opt_u = 1; break;
		case 'c': config_path = optarg; break;
		case 'k': ov.insecure = 1; break;
		case 'h': usage(stdout); return 0;
		case 'v': puts("asciirc " ASCIIRC_VERSION); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (optind < argc) {
		usage(stderr);
		return 2;
	}
	if (opt_u && !opt_t)
		ov.token = NULL;

	if (config_path) {
		if (config_load(&cfg, config_path, 1) < 0)
			return 1;
	} else {
		char *p = default_config_path();
		if (p)
			config_load(&cfg, p, 0);
		free(p);
	}
	char err[512];
	if (config_apply(&cfg, select, &ov, err, sizeof(err)) < 0) {
		fprintf(stderr, "asciirc: %s\n\n", err);
		usage(stderr);
		config_free(&cfg);
		return 2;
	}
	if (cfg.theme && ui_set_theme(cfg.theme) < 0) {
		fprintf(stderr, "asciirc: unknown theme '%s', available:", cfg.theme);
		for (size_t i = 0; ui_theme_at(i); i++)
			fprintf(stderr, " %s", ui_theme_at(i));
		fputc('\n', stderr);
		config_free(&cfg);
		return 2;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	App app;
	app_init(&app);
	app.show_usernames = cfg.show_usernames;
	char **failed = xcalloc(cfg.nservers, sizeof(*failed));
	size_t nfailed = 0;
	for (size_t i = 0; i < cfg.nservers; i++) {
		ServerConfig *sc = &cfg.servers[i];
		Rc rc;
		if (login(sc, cfg.nservers > 1, &rc, err, sizeof(err)) < 0) {
			Buf b;
			buf_init(&b);
			buf_printf(&b, cfg.nservers > 1 ? "%s: %s" : "%.0s%s", sc->name, err);
			failed[nfailed++] = buf_steal(&b);
			rc_free(&rc);
			continue;
		}
		app_add_server(&app, sc->name, &rc);
	}
	int rc = 1;
	if (app.nservers == 0) {
		for (size_t i = 0; i < nfailed; i++)
			fprintf(stderr, "asciirc: %s\n", failed[i]);
	} else if (ui_init() < 0) {
		fputs("asciirc: stdin and stdout must be a terminal\n", stderr);
	} else {
		atexit(ui_shutdown);
		for (size_t i = 0; i < nfailed; i++)
			app_error(&app, NULL, "Login failed for %s", failed[i]);
		app_start(&app);
		rc = run(&app) < 0 ? 1 : 0;
		ui_shutdown();
	}
	for (size_t i = 0; i < nfailed; i++)
		free(failed[i]);
	free(failed);
	app_free(&app);
	config_free(&cfg);
	return rc;
}
