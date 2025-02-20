#ifndef ASCIIRC_UI_H
#define ASCIIRC_UI_H

#include "app.h"

typedef enum {
	K_NONE,
	K_CHAR,
	K_ENTER,
	K_TAB,
	K_BACKSPACE,
	K_DELETE,
	K_LEFT,
	K_RIGHT,
	K_UP,
	K_DOWN,
	K_HOME,
	K_END,
	K_PGUP,
	K_PGDN,
	K_CTRL,
	K_ALT
} KeyType;

typedef struct {
	KeyType type;
	int c;
	char utf8[8];
} Key;

int ui_init(void);
void ui_shutdown(void);
int ui_check_resize(void);
int ui_rows(void);
int ui_cols(void);
void ui_draw(App *app);
void ui_redraw_all(void);

size_t ui_parse_keys(const char *buf, size_t n, Key *keys, size_t max);
int ui_input_key(Input *in, const Key *k);
void ui_input_set(Input *in, const char *s);
void ui_input_push_history(Input *in, const char *s);

char *ui_sanitize(const char *s);

#endif
