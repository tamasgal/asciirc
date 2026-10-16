#ifndef ASCIIRC_APP_H
#define ASCIIRC_APP_H

#include <stddef.h>
#include <time.h>

#include "rc.h"

typedef enum {
	LINE_MSG,
	LINE_ACTION,
	LINE_EVENT,
	LINE_INFO,
	LINE_ERROR
} LineKind;

typedef struct {
	time_t ts;
	LineKind kind;
	char *nick;
	char *name;
	char *text;
	char *msgid;
	char *raw;
	int highlight;
	int own;
} Line;

typedef enum {
	ACT_NONE,
	ACT_EVENT,
	ACT_MSG,
	ACT_HIGHLIGHT
} Activity;

typedef struct {
	char *rid;
	char *name;
	char type;
	char *topic;
	Line *lines;
	size_t nlines;
	size_t cap;
	Activity activity;
	int scroll;
	int history_loaded;
	int needs_read;
} Window;

typedef struct {
	char *rid;
	char *name;
	char type;
	int open;
	int unread;
	int mentions;
} Subscription;

#define INPUT_MAX 8192
#define HISTORY_MAX 100

typedef struct {
	char text[INPUT_MAX];
	size_t len;
	size_t cur;
	char *history[HISTORY_MAX];
	int nhistory;
	int hpos;
} Input;

typedef struct {
	Rc rc;
	Window **wins;
	size_t nwins;
	size_t cur;
	Subscription *subs;
	size_t nsubs;
	Input input;
	int quit;
	int dirty;
	int ever_ready;
	int show_usernames;
	long long reconnect_at;
	int reconnect_delay;
} App;

void app_init(App *app);
void app_free(App *app);
void app_info(App *app, Window *w, const char *fmt, ...);
void app_error(App *app, Window *w, const char *fmt, ...);
void app_start(App *app);
void app_switch(App *app, size_t idx);
void app_submit(App *app, const char *line);
void app_tab_complete(App *app);
void app_ddp_tick(App *app);
void app_ddp_readable(App *app);

#endif
