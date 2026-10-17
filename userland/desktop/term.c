#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "term.h"

/* the Linux console's colours */
const uint32_t term_palette[16] = {
	0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
	0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff,
};

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_CHARSET };

#define DEF_FG 7
#define DEF_BG 0

static struct cell blank(struct term *t)
{
	return (struct cell){ ' ', t->fg, t->bg, 0 };
}

static struct cell *at(struct term *t, int x, int y)
{
	return &t->cells[y * t->cols + x];
}

static void clear_cells(struct term *t, int from, int to)
{
	struct cell b = blank(t);

	for (int i = from; i < to; i++)
		t->cells[i] = b;
	t->dirty = 1;
}

int term_init(struct term *t, int cols, int rows)
{
	memset(t, 0, sizeof(*t));
	t->fg = DEF_FG;
	t->bg = DEF_BG;
	t->cursor_on = 1;
	return term_resize(t, cols, rows);
}

void term_free(struct term *t)
{
	free(t->cells);
	free(t->alt);
	t->cells = t->alt = NULL;
}

int term_resize(struct term *t, int cols, int rows)
{
	struct cell *c = malloc(sizeof(*c) * cols * rows);
	struct cell *a = malloc(sizeof(*a) * cols * rows);
	struct cell *main = t->using_alt ? t->alt : t->cells;

	if (!c || !a) {
		free(c);
		free(a);
		return -1;
	}
	for (int i = 0; i < cols * rows; i++)
		c[i] = a[i] = (struct cell){ ' ', DEF_FG, DEF_BG, 0 };
	/* keep the bottom of the old main screen */
	if (main) {
		int shift = t->cy >= rows ? t->cy - rows + 1 : 0;
		for (int y = 0; y < rows && y + shift < t->rows; y++)
			for (int x = 0; x < cols && x < t->cols; x++)
				c[y * cols + x] = main[(y + shift) * t->cols + x];
		t->cy -= shift;
		free(t->cells);
		free(t->alt);
	}
	t->cells = c;
	t->alt = a;
	t->using_alt = 0;
	t->cols = cols;
	t->rows = rows;
	t->top = 0;
	t->bot = rows - 1;
	if (t->cx >= cols)
		t->cx = cols - 1;
	if (t->cy >= rows)
		t->cy = rows - 1;
	t->dirty = 1;
	return 0;
}

static void scroll_up(struct term *t, int top, int bot, int n)
{
	if (n > bot - top + 1)
		n = bot - top + 1;
	memmove(at(t, 0, top), at(t, 0, top + n), sizeof(struct cell) * t->cols * (bot - top + 1 - n));
	clear_cells(t, (bot - n + 1) * t->cols, (bot + 1) * t->cols);
}

static void scroll_down(struct term *t, int top, int bot, int n)
{
	if (n > bot - top + 1)
		n = bot - top + 1;
	memmove(at(t, 0, top + n), at(t, 0, top), sizeof(struct cell) * t->cols * (bot - top + 1 - n));
	clear_cells(t, top * t->cols, (top + n) * t->cols);
}

static void newline(struct term *t)
{
	if (t->cy == t->bot)
		scroll_up(t, t->top, t->bot, 1);
	else if (t->cy < t->rows - 1)
		t->cy++;
}

static void put(struct term *t, uint32_t ch)
{
	if (t->wrap_next) {
		t->cx = 0;
		newline(t);
		t->wrap_next = 0;
	}
	/* the font covers Latin-1 (CP437 layout aside); others show as '?' */
	*at(t, t->cx, t->cy) = (struct cell){ ch < 256 ? ch : '?', t->fg, t->bg, t->attr };
	if (t->cx == t->cols - 1)
		t->wrap_next = 1;
	else
		t->cx++;
	t->dirty = 1;
}

static int param(struct term *t, int i, int def)
{
	return i < t->nparams && t->params[i] > 0 ? t->params[i] : def;
}

static void move(struct term *t, int x, int y)
{
	t->cx = x < 0 ? 0 : x >= t->cols ? t->cols - 1 : x;
	t->cy = y < 0 ? 0 : y >= t->rows ? t->rows - 1 : y;
	t->wrap_next = 0;
}

static void sgr(struct term *t)
{
	if (!t->nparams)
		t->params[t->nparams++] = 0;
	for (int i = 0; i < t->nparams; i++) {
		int p = t->params[i];
		if (p == 0)
			t->fg = DEF_FG, t->bg = DEF_BG, t->attr = 0;
		else if (p == 1)
			t->attr |= ATTR_BOLD;
		else if (p == 4)
			t->attr |= ATTR_UNDERLINE;
		else if (p == 7)
			t->attr |= ATTR_REVERSE;
		else if (p == 22)
			t->attr &= ~ATTR_BOLD;
		else if (p == 24)
			t->attr &= ~ATTR_UNDERLINE;
		else if (p == 27)
			t->attr &= ~ATTR_REVERSE;
		else if (p >= 30 && p <= 37)
			t->fg = p - 30;
		else if (p == 39)
			t->fg = DEF_FG;
		else if (p >= 40 && p <= 47)
			t->bg = p - 40;
		else if (p == 49)
			t->bg = DEF_BG;
		else if (p >= 90 && p <= 97)
			t->fg = p - 90 + 8;
		else if (p >= 100 && p <= 107)
			t->bg = p - 100 + 8;
		else if ((p == 38 || p == 48) && i + 2 < t->nparams && t->params[i + 1] == 5) {
			/* 256 colours: map the first 16, approximate the rest */
			int c = t->params[i + 2];
			int idx = c < 16 ? c : c >= 232 ? (c >= 244 ? 7 : 8) : 7;
			if (p == 38)
				t->fg = idx;
			else
				t->bg = idx;
			i += 2;
		}
	}
}

static void mode(struct term *t, int set)
{
	for (int i = 0; i < t->nparams; i++) {
		int p = t->params[i];
		if (!t->priv)
			continue;
		if (p == 25) {
			t->cursor_on = set;
		} else if (p == 1) {
			t->app_cursor = set;
		} else if (p == 1049 || p == 47 || p == 1047) {
			if (set == t->using_alt)
				continue;
			struct cell *tmp = t->cells;
			t->cells = t->alt;
			t->alt = tmp;
			t->using_alt = set;
			if (set) {
				t->saved_cx = t->cx, t->saved_cy = t->cy;
				clear_cells(t, 0, t->cols * t->rows);
			} else {
				move(t, t->saved_cx, t->saved_cy);
			}
			t->dirty = 1;
		}
	}
}

static void reply(struct term *t, const char *s)
{
	int n = strlen(s);
	if (t->nreply + n <= (int)sizeof(t->reply)) {
		memcpy(t->reply + t->nreply, s, n);
		t->nreply += n;
	}
}

static void csi(struct term *t, unsigned char c)
{
	int n = param(t, 0, 1);
	char buf[32];

	switch (c) {
	case 'A': move(t, t->cx, t->cy - n); break;
	case 'B': case 'e': move(t, t->cx, t->cy + n); break;
	case 'C': case 'a': move(t, t->cx + n, t->cy); break;
	case 'D': move(t, t->cx - n, t->cy); break;
	case 'E': move(t, 0, t->cy + n); break;
	case 'F': move(t, 0, t->cy - n); break;
	case 'G': case '`': move(t, n - 1, t->cy); break;
	case 'd': move(t, t->cx, n - 1); break;
	case 'H': case 'f': move(t, param(t, 1, 1) - 1, param(t, 0, 1) - 1); break;
	case 'J': {
		int p = param(t, 0, 0), cur = t->cy * t->cols + t->cx;
		if (p == 0)
			clear_cells(t, cur, t->cols * t->rows);
		else if (p == 1)
			clear_cells(t, 0, cur + 1);
		else
			clear_cells(t, 0, t->cols * t->rows);
		break;
	}
	case 'K': {
		int p = param(t, 0, 0), row = t->cy * t->cols;
		if (p == 0)
			clear_cells(t, row + t->cx, row + t->cols);
		else if (p == 1)
			clear_cells(t, row, row + t->cx + 1);
		else
			clear_cells(t, row, row + t->cols);
		break;
	}
	case 'X': {
		int row = t->cy * t->cols, end = t->cx + n > t->cols ? t->cols : t->cx + n;
		clear_cells(t, row + t->cx, row + end);
		break;
	}
	case 'P': {
		struct cell *r = at(t, 0, t->cy);
		if (n > t->cols - t->cx)
			n = t->cols - t->cx;
		memmove(r + t->cx, r + t->cx + n, sizeof(*r) * (t->cols - t->cx - n));
		clear_cells(t, t->cy * t->cols + t->cols - n, (t->cy + 1) * t->cols);
		break;
	}
	case '@': {
		struct cell *r = at(t, 0, t->cy);
		if (n > t->cols - t->cx)
			n = t->cols - t->cx;
		memmove(r + t->cx + n, r + t->cx, sizeof(*r) * (t->cols - t->cx - n));
		clear_cells(t, t->cy * t->cols + t->cx, t->cy * t->cols + t->cx + n);
		break;
	}
	case 'L':
		if (t->cy >= t->top && t->cy <= t->bot)
			scroll_down(t, t->cy, t->bot, n);
		break;
	case 'M':
		if (t->cy >= t->top && t->cy <= t->bot)
			scroll_up(t, t->cy, t->bot, n);
		break;
	case 'S': scroll_up(t, t->top, t->bot, n); break;
	case 'T': scroll_down(t, t->top, t->bot, n); break;
	case 'm': sgr(t); break;
	case 'r': {
		int top = param(t, 0, 1) - 1, bot = param(t, 1, t->rows) - 1;
		if (top < bot && bot < t->rows) {
			t->top = top;
			t->bot = bot;
			move(t, 0, 0);
		}
		break;
	}
	case 'h': mode(t, 1); break;
	case 'l': mode(t, 0); break;
	case 's': t->saved_cx = t->cx, t->saved_cy = t->cy; break;
	case 'u': move(t, t->saved_cx, t->saved_cy); break;
	case 'n':
		if (param(t, 0, 0) == 6) {
			snprintf(buf, sizeof(buf), "\033[%d;%dR", t->cy + 1, t->cx + 1);
			reply(t, buf);
		} else if (param(t, 0, 0) == 5) {
			reply(t, "\033[0n");
		}
		break;
	case 'c':
		if (!t->priv)
			reply(t, "\033[?6c");
		break;
	}
}

void term_write(struct term *t, const unsigned char *buf, int n)
{
	for (int i = 0; i < n; i++) {
		unsigned char c = buf[i];

		/* control characters act in any state but OSC */
		if (c < 0x20 && t->state != S_OSC && t->state != S_OSC_ESC) {
			switch (c) {
			case '\a': continue;
			case '\b': move(t, t->cx - 1, t->cy); continue;
			case '\t': move(t, (t->cx / 8 + 1) * 8, t->cy); continue;
			case '\n': case '\v': case '\f': newline(t); t->wrap_next = 0; continue;
			case '\r': t->cx = 0; t->wrap_next = 0; continue;
			case 0x18: case 0x1a: t->state = S_GROUND; continue;
			case 0x1b: t->state = S_ESC; continue;
			default: continue;
			}
		}
		switch (t->state) {
		case S_GROUND:
			if (c < 0x80) {
				t->ulen = 0;
				put(t, c);
			} else if (c < 0xc0) {
				if (t->ulen) {
					t->uc = t->uc << 6 | (c & 0x3f);
					if (--t->ulen == 0)
						put(t, t->uc);
				}
			} else {
				t->ulen = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
				t->uc = c & (0x3f >> t->ulen);
			}
			break;
		case S_ESC:
			t->state = S_GROUND;
			switch (c) {
			case '[':
				t->state = S_CSI;
				t->nparams = 0;
				t->priv = 0;
				memset(t->params, 0, sizeof(t->params));
				break;
			case ']': t->state = S_OSC; break;
			case '(': case ')': t->state = S_CHARSET; break;
			case '7': t->saved_cx = t->cx, t->saved_cy = t->cy; break;
			case '8': move(t, t->saved_cx, t->saved_cy); break;
			case 'D': newline(t); break;
			case 'E': t->cx = 0; newline(t); break;
			case 'M':
				if (t->cy == t->top)
					scroll_down(t, t->top, t->bot, 1);
				else
					move(t, t->cx, t->cy - 1);
				break;
			case 'c':
				t->fg = DEF_FG, t->bg = DEF_BG, t->attr = 0;
				t->top = 0, t->bot = t->rows - 1;
				clear_cells(t, 0, t->cols * t->rows);
				move(t, 0, 0);
				break;
			}
			break;
		case S_CSI:
			if (c >= '0' && c <= '9') {
				if (!t->nparams)
					t->nparams = 1;
				int *p = &t->params[t->nparams - 1];
				*p = *p * 10 + (c - '0');
			} else if (c == ';' || c == ':') {
				if (!t->nparams)
					t->nparams = 1;
				if (t->nparams < 16)
					t->params[t->nparams++] = 0;
			} else if (c == '?' || c == '>' || c == '=') {
				t->priv = c;
			} else if (c >= 0x40 && c <= 0x7e) {
				csi(t, c);
				t->state = S_GROUND;
			}
			break;
		case S_OSC:
			/* window titles etc.: ignored up to BEL or ST */
			if (c == '\a')
				t->state = S_GROUND;
			else if (c == 0x1b)
				t->state = S_OSC_ESC;
			break;
		case S_OSC_ESC:
			t->state = c == '\\' ? S_GROUND : S_OSC;
			break;
		case S_CHARSET:
			t->state = S_GROUND;
			break;
		}
	}
}
