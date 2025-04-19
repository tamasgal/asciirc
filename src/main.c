#include "app.h"
#include "net.h"
#include "rc.h"
#include "ui.h"
#include "util.h"

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

typedef struct {
	char *server;
	char *user_id;
	char *token;
	char *username;
	int insecure;
} Config;

static volatile sig_atomic_t got_term;

static void on_term(int sig)
{
	(void)sig;
	got_term = 1;
}

static void usage(FILE *f)
{
	fputs("usage: asciirc [-s server] [-i user_id] [-t token] [-c config] [-k]\n"
	      "\n"
	      "  -s server    server URL, e.g. https://chat.example.com\n"
	      "  -i user_id   user id belonging to a personal access token\n"
	      "  -t token     personal acess token (prefer ASCIIRC_TOKEN)\n"
	      "  -k           do not verify TLS certificates\n"
	      "\n"
	      "Environment: ASCIIRC_SERVER, ASCIIRC_USER_ID, ASCIIRC_TOKEN, ASCIIRC_USERNAME\n",
	      f);
}

static void set_str(char **dst, const char *v)
{
	free(*dst);
	*dst = v ? xstrdup(v) : NULL;
}

static void handle_key(App *app, const Key *k)
{
	Window *w = app->wins[app->cur];
	int page = (ui_rows() - 3) / 2;
	if (page < 1)
		page = 1;
	switch (k->type) {
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
	ui_draw(app);
	while (!app->quit && !got_term) {
		struct pollfd fds[2];
		int nfds = 0;
		fds[nfds].fd = STDIN_FILENO;
		fds[nfds].events = POLLIN;
		nfds++;
		int wsfd = rc_ddp_fd(&app->rc);
		if (wsfd >= 0) {
			fds[nfds].fd = wsfd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		long long now = now_ms();
		long long timeout = 60000 - now % 60000 + 10;
		if (app->rc.state == DDP_OFFLINE && app->reconnect_at >= 0) {
			long long until = app->reconnect_at - now;
			if (until < timeout)
				timeout = until < 0 ? 0 : until;
		}
		int r = poll(fds, (nfds_t)nfds, (int)timeout);
		if (r < 0 && errno != EINTR)
			return -1;

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
		if (r > 0 && nfds > 1 && fds[1].revents)
			app_ddp_readable(app);
		app_ddp_tick(app);
		if (r == 0)
			app->dirty = 1;
		if (app->dirty && !app->quit) {
			ui_draw(app);
			app->dirty = 0;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	setlocale(LC_CTYPE, "");

	Config cfg;
	memset(&cfg, 0, sizeof(cfg));
	const char *opt_server = NULL, *opt_uid = NULL, *opt_token = NULL;
	int opt_insecure = 0;
	int c;
	while ((c = getopt(argc, argv, "s:i:t:c:kh")) != -1) {
		switch (c) {
		case 's': opt_server = optarg; break;
		case 'i': opt_uid = optarg; break;
		case 't': opt_token = optarg; break;
		case 'k': opt_insecure = 1; break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (optind < argc) {
		usage(stderr);
		return 2;
	}

	if (getenv("ASCIIRC_SERVER"))
		set_str(&cfg.server, getenv("ASCIIRC_SERVER"));
	if (getenv("ASCIIRC_USER_ID"))
		set_str(&cfg.user_id, getenv("ASCIIRC_USER_ID"));
	if (getenv("ASCIIRC_TOKEN"))
		set_str(&cfg.token, getenv("ASCIIRC_TOKEN"));
	if (getenv("ASCIIRC_USERNAME"))
		set_str(&cfg.username, getenv("ASCIIRC_USERNAME"));
	if (opt_server)
		set_str(&cfg.server, opt_server);
	if (opt_uid)
		set_str(&cfg.user_id, opt_uid);
	if (opt_token)
		set_str(&cfg.token, opt_token);
	if (opt_insecure)
		cfg.insecure = 1;

	if (!cfg.server) {
		fputs("asciirc: no server given\n\n", stderr);
		usage(stderr);
		return 2;
	}
	Url url;
	if (url_parse(cfg.server, &url) < 0) {
		fprintf(stderr, "asciirc: invalid server URL: %s\n", cfg.server);
		return 2;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	net_init(cfg.insecure);
	App app;
	rc_init(&app.rc, &url);
	char err[512];
	if (cfg.token) {
		if (!cfg.user_id) {
			fputs("asciirc: a personal access token also needs the user id (-i or ASCIIRC_USER_ID)\n", stderr);
			return 2;
		}
		app.rc.user_id = xstrdup(cfg.user_id);
		app.rc.token = xstrdup(cfg.token);
	} else {
		fputs("asciirc: no credentials, give a user id and token\n\n", stderr);
		usage(stderr);
		return 2;
	}
	if (rc_fetch_me(&app.rc, err, sizeof(err)) < 0) {
		fprintf(stderr, "asciirc: %s\n", err);
		return 1;
	}

	if (ui_init() < 0) {
		fputs("asciirc: stdin and stdout must be a terminal\n", stderr);
		return 1;
	}
	atexit(ui_shutdown);
	app_init(&app);
	app_start(&app);
	int rc = run(&app);
	ui_shutdown();
	app_free(&app);
	rc_free(&app.rc);
	free(cfg.server);
	free(cfg.user_id);
	free(cfg.token);
	free(cfg.username);
	return rc < 0 ? 1 : 0;
}
