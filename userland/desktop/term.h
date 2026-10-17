/* A VT100/xterm-subset terminal screen: feed it output, draw its cells. */
#ifndef TERM_H
#define TERM_H

#include <stdint.h>

struct cell {
	uint8_t ch;
	uint8_t fg, bg;		/* palette indexes 0..15 */
	uint8_t attr;		/* ATTR_* */
};

#define ATTR_BOLD	1
#define ATTR_REVERSE	2
#define ATTR_UNDERLINE	4

struct term {
	int cols, rows;
	struct cell *cells, *alt;	/* main and alternate screen */
	int using_alt;
	int cx, cy;			/* cursor */
	int saved_cx, saved_cy;
	int top, bot;			/* scroll region, inclusive */
	uint8_t fg, bg, attr;
	int wrap_next;			/* deferred autowrap */
	int cursor_on;
	int app_cursor;			/* DECCKM: arrows send ESC O x */
	/* escape parser */
	int state;
	int params[16], nparams;
	int priv;
	/* UTF-8 decoder */
	uint32_t uc;
	int ulen;
	/* scrollback: lines that scrolled off the top of the main screen */
	struct cell *hist;		/* hist_cap rows of cols cells, a ring */
	int hist_cap, hist_len, hist_head;
	int view;			/* rows scrolled back, 0 = live */
	int dirty;
	/* bytes the terminal answers with (device status reports) */
	char reply[64];
	int nreply;
};

extern const uint32_t term_palette[16];

int term_init(struct term *t, int cols, int rows);
void term_free(struct term *t);
int term_resize(struct term *t, int cols, int rows);
void term_write(struct term *t, const unsigned char *buf, int n);
/* the cell shown at (x, y) with the view scrolled back t->view rows */
const struct cell *term_view_cell(const struct term *t, int x, int y);
void term_scroll_view(struct term *t, int rows);

#endif
