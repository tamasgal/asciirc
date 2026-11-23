#include "config.h"
#include "net.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void config_set(char **dst, const char *v)
{
	free(*dst);
	*dst = v ? xstrdup(v) : NULL;
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
	return s;
}

static ServerConfig *add_server(Config *cfg, const char *name)
{
	cfg->servers = xrealloc(cfg->servers, (cfg->nservers + 1) * sizeof(*cfg->servers));
	ServerConfig *sc = &cfg->servers[cfg->nservers++];
	memset(sc, 0, sizeof(*sc));
	sc->name = name ? xstrdup(name) : NULL;
	return sc;
}

static void free_server(ServerConfig *sc)
{
	free(sc->name);
	free(sc->server);
	free(sc->user_id);
	free(sc->token);
	free(sc->username);
}

static int truthy(const char *v)
{
	return strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0;
}

void config_parse(Config *cfg, const char *text, const char *path)
{
	char *copy = xstrdup(text);
	long section = -1;
	long unnamed = -1;
	int lineno = 0;
	for (char *line = copy; line;) {
		char *next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		lineno++;
		char *s = trim(line);
		line = next;
		if (!*s || *s == '#')
			continue;
		if (*s == '[') {
			char *end = strchr(s, ']');
			if (!end) {
				fprintf(stderr, "asciirc: %s:%d: expected [name]\n", path, lineno);
				continue;
			}
			*end = '\0';
			add_server(cfg, trim(s + 1));
			section = (long)cfg->nservers - 1;
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq) {
			fprintf(stderr, "asciirc: %s:%d: expected key = value\n", path, lineno);
			continue;
		}
		*eq = '\0';
		char *key = trim(s);
		char *val = trim(eq + 1);
		if (strcmp(key, "theme") == 0) {
			config_set(&cfg->theme, val);
			continue;
		}
		if (strcmp(key, "nicks") == 0) {
			cfg->show_usernames = strcmp(val, "username") == 0;
			continue;
		}
		if (strcmp(key, "server") != 0 && strcmp(key, "user_id") != 0 && strcmp(key, "token") != 0 &&
		    strcmp(key, "username") != 0 && strcmp(key, "insecure") != 0) {
			fprintf(stderr, "asciirc: %s:%d: unknown key '%s'\n", path, lineno, key);
			continue;
		}
		if (section < 0 && unnamed < 0) {
			add_server(cfg, NULL);
			unnamed = (long)cfg->nservers - 1;
		}
		ServerConfig *sc = &cfg->servers[section >= 0 ? section : unnamed];
		if (strcmp(key, "server") == 0)
			config_set(&sc->server, val);
		else if (strcmp(key, "user_id") == 0)
			config_set(&sc->user_id, val);
		else if (strcmp(key, "token") == 0)
			config_set(&sc->token, val);
		else if (strcmp(key, "username") == 0)
			config_set(&sc->username, val);
		else
			sc->insecure = truthy(val);
	}
	free(copy);
}

int config_load(Config *cfg, const char *path, int required)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		if (required) {
			fprintf(stderr, "asciirc: %s: %s\n", path, strerror(errno));
			return -1;
		}
		return 0;
	}
	struct stat st;
	if (fstat(fileno(f), &st) == 0 && (st.st_mode & 077))
		fprintf(stderr, "asciirc: warning: %s is readable by other users, run chmod 600 on it\n", path);
	Buf b;
	buf_init(&b);
	char tmp[4096];
	size_t n;
	while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
		buf_append(&b, tmp, n);
	fclose(f);
	config_parse(cfg, b.data ? b.data : "", path);
	buf_free(&b);
	return 0;
}

int config_apply(Config *cfg, const char *select, const ServerConfig *ov, char *err, size_t errlen)
{
	if (select) {
		size_t keep = cfg->nservers;
		for (size_t i = 0; i < cfg->nservers; i++)
			if (cfg->servers[i].name && strcmp(cfg->servers[i].name, select) == 0)
				keep = i;
		if (keep == cfg->nservers) {
			snprintf(err, errlen, "no server [%s] in the config", select);
			return -1;
		}
		for (size_t i = 0; i < cfg->nservers; i++)
			if (i != keep)
				free_server(&cfg->servers[i]);
		cfg->servers[0] = cfg->servers[keep];
		cfg->nservers = 1;
	}
	if (ov->server || ov->user_id || ov->token || ov->username) {
		if (ov->server && cfg->nservers != 1) {
			for (size_t i = 0; i < cfg->nservers; i++)
				free_server(&cfg->servers[i]);
			cfg->nservers = 0;
		}
		if (cfg->nservers == 0)
			add_server(cfg, NULL);
		if (cfg->nservers > 1) {
			snprintf(err, errlen, "several servers are configured, pick one with -S name");
			return -1;
		}
		ServerConfig *sc = &cfg->servers[0];
		if (ov->server)
			config_set(&sc->server, ov->server);
		if (ov->user_id)
			config_set(&sc->user_id, ov->user_id);
		if (ov->token)
			config_set(&sc->token, ov->token);
		if (ov->username) {
			config_set(&sc->username, ov->username);
			if (!ov->token)
				config_set(&sc->token, NULL);
		}
	}
	if (cfg->nservers == 0) {
		snprintf(err, errlen, "no server given");
		return -1;
	}
	for (size_t i = 0; i < cfg->nservers; i++) {
		ServerConfig *sc = &cfg->servers[i];
		if (ov->insecure)
			sc->insecure = 1;
		Url u;
		if (!sc->server) {
			snprintf(err, errlen, "no server URL for %s", sc->name ? sc->name : "the server");
			return -1;
		}
		if (url_parse(sc->server, &u) < 0) {
			snprintf(err, errlen, "invalid server URL: %s", sc->server);
			return -1;
		}
		if (!sc->name)
			sc->name = xstrdup(u.host);
		for (size_t k = 0; k < i; k++) {
			if (strcmp(cfg->servers[k].name, sc->name) == 0) {
				snprintf(err, errlen, "server name %s is used twice", sc->name);
				return -1;
			}
		}
	}
	return 0;
}

void config_free(Config *cfg)
{
	for (size_t i = 0; i < cfg->nservers; i++)
		free_server(&cfg->servers[i]);
	free(cfg->servers);
	free(cfg->theme);
	memset(cfg, 0, sizeof(*cfg));
}
