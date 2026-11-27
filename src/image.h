#ifndef ASCIIRC_IMAGE_H
#define ASCIIRC_IMAGE_H

#include <stddef.h>

#include "util.h"

typedef enum {
	GFX_NONE,
	GFX_ITERM,
	GFX_KITTY
} Graphics;

int image_size(const unsigned char *d, size_t n, int *w, int *h);
void fit_cells(int img_w, int img_h, int cell_w, int cell_h, int max_cols, int max_rows, int *c, int *r);
void gfx_iterm(Buf *out, const void *data, size_t len, int c, int r);
void gfx_kitty(Buf *out, const void *data, size_t len, int c, int r);
void gfx_kitty_clear(Buf *out);
Graphics gfx_detect(const char *setting);

#endif
