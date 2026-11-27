#ifndef ASCIIRC_CONFIG_H
#define ASCIIRC_CONFIG_H

#include <stddef.h>

typedef struct {
	char *name;
	char *server;
	char *user_id;
	char *token;
	char *username;
	int insecure;
} ServerConfig;

typedef struct {
	char *theme;
	char *downloads;
	char *images;
	int show_usernames;
	ServerConfig *servers;
	size_t nservers;
} Config;

void config_set(char **dst, const char *v);
void config_parse(Config *cfg, const char *text, const char *path);
int config_load(Config *cfg, const char *path, int required);
int config_apply(Config *cfg, const char *select, const ServerConfig *ov, char *err, size_t errlen);
void config_free(Config *cfg);

#endif
