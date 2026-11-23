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
	char *tmid;
	size_t ctx_len;
	size_t meta_len;
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
	char *label;
	char type;
	char *topic;
	Line *lines;
	size_t nlines;
	size_t cap;
	Activity activity;
	int unread;
	int scroll;
	int history_loaded;
	int needs_read;
} Window;

static inline const char *win_label(const Window *w)
{
	return w->label ? w->label : w->name;
}

typedef struct {
	char *rid;
	char *name;
	char *fname;
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
	char **comp;
	char **comp_names;
	size_t ncomp;
	size_t comp_idx;
	size_t comp_start;
	size_t comp_len;
	int goto_active;
	char goto_text[64];
	long goto_match;
	size_t *goto_list;
	size_t goto_n;
	size_t goto_idx;
	char *sel_id;
	char *reply_tmid;
	char *reply_to;
	int react_active;
	char react_text[64];
	const char **react_list;
	size_t react_n;
	size_t react_idx;
	char **tctx_id;
	char **tctx_text;
	size_t ntctx;
	int tctx_budget;
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
void app_tab_complete(App *app, int dir);
void app_tab_reset(App *app);
void app_goto_update(App *app);
void app_goto_cycle(App *app, int dir);
void app_jump_activity(App *app);
void app_select_start(App *app);
void app_select_move(App *app, int dir);
void app_select_end(App *app);
void app_reply_start(App *app);
void app_reply_cancel(App *app);
void app_react_start(App *app);
void app_react_update(App *app);
void app_react_cycle(App *app, int dir);
void app_react_send(App *app);
void app_ddp_tick(App *app);
void app_ddp_readable(App *app);

#endif
