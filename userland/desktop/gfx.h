/* Drawing on 32-bit XRGB surfaces with clipping, and the 8x16 text font. */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#define FONT_W 8
#define FONT_H 16

struct rect { int x, y, w, h; };

struct surface {
	uint32_t *px;
	int w, h, stride;	/* stride in pixels */
	struct rect clip;	/* drawing is limited to this */
};

static inline uint32_t rgb(int r, int g, int b)
{
	return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

int surface_init(struct surface *s, int w, int h);
void surface_free(struct surface *s);
void surface_noclip(struct surface *s);
int rect_intersect(struct rect *a, const struct rect *b);

void fill(struct surface *s, int x, int y, int w, int h, uint32_t c);
/* c blended over the surface with alpha 0..255 */
void fill_alpha(struct surface *s, int x, int y, int w, int h, uint32_t c, int alpha);
void frame(struct surface *s, int x, int y, int w, int h, uint32_t c);
void gradient(struct surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void blit(struct surface *dst, int x, int y, const struct surface *src);
/* one character; bg is not drawn when it is TRANSPARENT */
#define TRANSPARENT 0xff000000u
void glyph(struct surface *s, int x, int y, unsigned char c, uint32_t fg, uint32_t bg);
/* a string, clipped to max_w pixels (no limit if <= 0); returns the width */
int text(struct surface *s, int x, int y, const char *str, uint32_t fg, int max_w);
/* 1-bit bitmap rows (MSB first) of width w <= 16 */
void bitmap(struct surface *s, int x, int y, const uint16_t *rows, int w, int h, uint32_t c);

#endif
