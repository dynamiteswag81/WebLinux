#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "font8x16.h"	/* generated from the kernel's lib/fonts/font_8x16.c */

int surface_init(struct surface *s, int w, int h)
{
	s->px = calloc((size_t)w * h, 4);
	s->w = s->stride = w;
	s->h = h;
	surface_noclip(s);
	return s->px ? 0 : -1;
}

void surface_free(struct surface *s)
{
	free(s->px);
	s->px = NULL;
}

void surface_noclip(struct surface *s)
{
	s->clip = (struct rect){ 0, 0, s->w, s->h };
}

int rect_intersect(struct rect *a, const struct rect *b)
{
	int x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
	int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
	int y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;

	if (x1 <= x0 || y1 <= y0) {
		a->w = a->h = 0;
		return 0;
	}
	*a = (struct rect){ x0, y0, x1 - x0, y1 - y0 };
	return 1;
}

static int clip(struct surface *s, int *x, int *y, int *w, int *h)
{
	struct rect r = { *x, *y, *w, *h };

	if (!rect_intersect(&r, &s->clip))
		return 0;
	*x = r.x, *y = r.y, *w = r.w, *h = r.h;
	return 1;
}

void fill(struct surface *s, int x, int y, int w, int h, uint32_t c)
{
	if (!clip(s, &x, &y, &w, &h))
		return;
	for (int j = 0; j < h; j++) {
		uint32_t *p = s->px + (size_t)(y + j) * s->stride + x;
		for (int i = 0; i < w; i++)
			p[i] = c;
	}
}

void fill_alpha(struct surface *s, int x, int y, int w, int h, uint32_t c, int a)
{
	uint32_t cr = (c >> 16 & 255) * a, cg = (c >> 8 & 255) * a, cb = (c & 255) * a;
	int na = 255 - a;

	if (!clip(s, &x, &y, &w, &h))
		return;
	for (int j = 0; j < h; j++) {
		uint32_t *p = s->px + (size_t)(y + j) * s->stride + x;
		for (int i = 0; i < w; i++) {
			uint32_t d = p[i];
			p[i] = ((d >> 16 & 255) * na + cr) / 255 << 16 |
			       ((d >> 8 & 255) * na + cg) / 255 << 8 |
			       ((d & 255) * na + cb) / 255;
		}
	}
}

void frame(struct surface *s, int x, int y, int w, int h, uint32_t c)
{
	fill(s, x, y, w, 1, c);
	fill(s, x, y + h - 1, w, 1, c);
	fill(s, x, y, 1, h, c);
	fill(s, x + w - 1, y, 1, h, c);
}

void gradient(struct surface *s, int x, int y, int w, int h, uint32_t t, uint32_t b)
{
	for (int j = 0; j < h; j++) {
		int k = h > 1 ? j * 255 / (h - 1) : 0;
		uint32_t c = rgb(((t >> 16 & 255) * (255 - k) + (b >> 16 & 255) * k) / 255,
				 ((t >> 8 & 255) * (255 - k) + (b >> 8 & 255) * k) / 255,
				 ((t & 255) * (255 - k) + (b & 255) * k) / 255);
		fill(s, x, y + j, w, 1, c);
	}
}

void blit(struct surface *dst, int x, int y, const struct surface *src)
{
	int x0 = x, y0 = y, w = src->w, h = src->h;

	if (!clip(dst, &x, &y, &w, &h))
		return;
	for (int j = 0; j < h; j++)
		memcpy(dst->px + (size_t)(y + j) * dst->stride + x,
		       src->px + (size_t)(y - y0 + j) * src->stride + (x - x0), w * 4);
}

void glyph(struct surface *s, int x, int y, unsigned char c, uint32_t fg, uint32_t bg)
{
	const unsigned char *g = font8x16 + c * FONT_H;
	int cx = x, cy = y, cw = FONT_W, ch = FONT_H;

	if (!clip(s, &cx, &cy, &cw, &ch))
		return;
	for (int j = cy - y; j < cy - y + ch; j++) {
		uint32_t *p = s->px + (size_t)(y + j) * s->stride + x;
		unsigned char bits = g[j];
		for (int i = cx - x; i < cx - x + cw; i++) {
			if (bits & (0x80 >> i))
				p[i] = fg;
			else if (bg != TRANSPARENT)
				p[i] = bg;
		}
	}
}

int text(struct surface *s, int x, int y, const char *str, uint32_t fg, int max_w)
{
	int n = 0;

	for (; *str; str++, n += FONT_W) {
		if (max_w > 0 && n + FONT_W > max_w)
			break;
		glyph(s, x + n, y, (unsigned char)*str, fg, TRANSPARENT);
	}
	return n;
}

void bitmap(struct surface *s, int x, int y, const uint16_t *rows, int w, int h, uint32_t c)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			if (rows[j] & (0x8000 >> i))
				fill(s, x + i, y + j, 1, 1, c);
}
