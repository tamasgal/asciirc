#include "image.h"

#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

static unsigned be16(const unsigned char *p)
{
	return (unsigned)p[0] << 8 | p[1];
}

static unsigned long be32(const unsigned char *p)
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

/* pixel size from a PNG, GIF or JPEG header */
int image_size(const unsigned char *d, size_t n, int *w, int *h)
{
	if (n >= 24 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0 && memcmp(d + 12, "IHDR", 4) == 0) {
		*w = (int)be32(d + 16);
		*h = (int)be32(d + 20);
		return 0;
	}
	if (n >= 10 && (memcmp(d, "GIF87a", 6) == 0 || memcmp(d, "GIF89a", 6) == 0)) {
		*w = d[6] | d[7] << 8;
		*h = d[8] | d[9] << 8;
		return 0;
	}
	if (n >= 4 && d[0] == 0xff && d[1] == 0xd8) {
		size_t i = 2;
		while (i + 4 <= n) {
			if (d[i] != 0xff) {
				i++;
				continue;
			}
			unsigned char m = d[i + 1];
			if (m == 0xff) {
				i++;
				continue;
			}
			if (m == 0xd8 || m == 0x01 || (m >= 0xd0 && m <= 0xd7)) {
				i += 2;
				continue;
			}
			unsigned seg = be16(d + i + 2);
			if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
				if (i + 9 > n)
					return -1;
				*h = (int)be16(d + i + 5);
				*w = (int)be16(d + i + 7);
				return 0;
			}
			if (seg < 2)
				return -1;
			i += 2 + seg;
		}
	}
	return -1;
}

/* largest box of cells that keeps the aspect ratio, without upscaling */
void fit_cells(int img_w, int img_h, int cell_w, int cell_h, int max_cols, int max_rows, int *c, int *r)
{
	if (cell_w <= 0 || cell_h <= 0) {
		cell_w = 8;
		cell_h = 16;
	}
	if (max_cols < 1)
		max_cols = 1;
	if (max_rows < 1)
		max_rows = 1;
	if (img_w <= 0 || img_h <= 0) {
		*c = max_cols;
		*r = max_rows;
		return;
	}
	double s = 1.0;
	if ((double)img_w / cell_w > max_cols)
		s = (double)max_cols * cell_w / img_w;
	if ((double)img_h * s / cell_h > max_rows)
		s = (double)max_rows * cell_h / img_h;
	*c = (int)(img_w * s / cell_w + 0.5);
	*r = (int)(img_h * s / cell_h + 0.5);
	if (*c < 1)
		*c = 1;
	if (*r < 1)
		*r = 1;
	if (*c > max_cols)
		*c = max_cols;
	if (*r > max_rows)
		*r = max_rows;
}

static void put_base64(Buf *out, const unsigned char *data, size_t len)
{
	buf_reserve(out, (len + 2) / 3 * 4);
	int n = EVP_EncodeBlock((unsigned char *)out->data + out->len, data, (int)len);
	out->len += (size_t)n;
	out->data[out->len] = '\0';
}

void gfx_iterm(Buf *out, const void *data, size_t len, int c, int r)
{
	buf_printf(out, "\x1b]1337;File=inline=1;size=%zu;width=%d;height=%d;preserveAspectRatio=1:", len, c, r);
	put_base64(out, data, len);
	buf_putc(out, '\a');
}

#define KITTY_CHUNK 3072

void gfx_kitty(Buf *out, const void *data, size_t len, int c, int r)
{
	const unsigned char *p = data;
	size_t off = 0;
	do {
		size_t n = len - off > KITTY_CHUNK ? KITTY_CHUNK : len - off;
		int more = off + n < len;
		if (off == 0)
			buf_printf(out, "\x1b_Ga=T,f=100,q=2,c=%d,r=%d,m=%d;", c, r, more);
		else
			buf_printf(out, "\x1b_Gm=%d;", more);
		put_base64(out, p + off, n);
		buf_puts(out, "\x1b\\");
		off += n;
	} while (off < len);
}

void gfx_kitty_clear(Buf *out)
{
	buf_puts(out, "\x1b_Ga=d,d=A,q=2\x1b\\");
}

static int env_is(const char *name, const char *value)
{
	const char *v = getenv(name);
	return v && strcmp(v, value) == 0;
}

Graphics gfx_detect(const char *setting)
{
	if (setting && strcmp(setting, "off") == 0)
		return GFX_NONE;
	if (setting && strcmp(setting, "iterm") == 0)
		return GFX_ITERM;
	if (setting && strcmp(setting, "kitty") == 0)
		return GFX_KITTY;
	if (getenv("TMUX"))
		return GFX_NONE;
	if (env_is("TERM_PROGRAM", "iTerm.app") || env_is("TERM_PROGRAM", "WezTerm") || env_is("LC_TERMINAL", "iTerm2"))
		return GFX_ITERM;
	if (env_is("TERM", "xterm-kitty") || getenv("KITTY_WINDOW_ID") || env_is("TERM_PROGRAM", "ghostty") ||
	    env_is("TERM", "xterm-ghostty"))
		return GFX_KITTY;
	return GFX_NONE;
}
