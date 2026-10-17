/*
 * wldesk: the WebLinux desktop.
 *
 * Runs on a VT (tty1) with the text console switched off, draws straight
 * into /dev/fb0 and reads the keyboard and pointer from /dev/input.  One
 * process: a window manager with a taskbar and a start menu, plus built-in
 * applications (terminal, files, editor).  Other graphical programs, like
 * the NetSurf browser, get the whole frame buffer while they run.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "gfx.h"
#include "term.h"

#define TITLE_H		22
#define TASKBAR_H	30
#define MAX_WINS	32
#define MAX_INPUT	8
#define ROW_H		18
#define CORNER		14

#define C_DESK_TOP	rgb(0x1d, 0x3b, 0x5c)
#define C_DESK_BOT	rgb(0x0b, 0x14, 0x22)
#define C_BAR		rgb(0x16, 0x1b, 0x22)
#define C_BAR_HI	rgb(0x2a, 0x33, 0x40)
#define C_TITLE		rgb(0x2b, 0x5d, 0x8c)
#define C_TITLE_OFF	rgb(0x3a, 0x40, 0x48)
#define C_TEXT		rgb(0xe8, 0xec, 0xf0)
#define C_DIM		rgb(0x8a, 0x94, 0xa0)
#define C_WIN_BG	rgb(0xf4, 0xf5, 0xf7)
#define C_WIN_FG	rgb(0x1b, 0x1f, 0x24)
#define C_SEL		rgb(0x3d, 0x7e, 0xc0)
#define C_BORDER	rgb(0x10, 0x14, 0x1a)
#define C_ACCENT	rgb(0x4f, 0xc3, 0xf7)

#define MOD_SHIFT	1
#define MOD_CTRL	2
#define MOD_ALT		4

enum { M_DOWN, M_UP, M_MOVE, M_WHEEL, M_DOUBLE };

struct key { int code, ch, mods; };
struct win;

struct app_ops {
	void (*draw)(struct win *);
	void (*key)(struct win *, const struct key *);
	void (*mouse)(struct win *, int x, int y, int ev, int arg);
	void (*io)(struct win *);
	void (*resized)(struct win *);
	void (*close)(struct win *);
};

struct win {
	int x, y, w, h;			/* content area on screen */
	char title[64];
	struct surface surf;
	const struct app_ops *ops;
	void *priv;
	int fd;				/* polled for input when >= 0 */
	int dirty, minimized, dead;
	int drawn_focused;		/* focus state of the last draw */
};

/* ---------------------------------------------------------------- state */

static struct surface screen, back, wallpaper;
static uint32_t *fbmem;
static size_t fblen;
static struct win *wins[MAX_WINS];	/* z order, last on top */
static int nwins;
static int input_fd[MAX_INPUT], ninput;
static int mx, my, buttons, mods;
static int need_redraw = 1;
static int menu_open;
static pid_t fullscreen_pid;
static volatile sig_atomic_t got_child;
static struct { struct win *w; int dx, dy, resize; } drag;
static struct win *mouse_capture;	/* gets moves and the release after a click */
static struct { long long t; int x, y; } last_click;
static int last_clock = -1;
static long long last_redraw;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct win *focus(void)
{
	for (int i = nwins - 1; i >= 0; i--)
		if (!wins[i]->minimized)
			return wins[i];
	return NULL;
}

static void raise_win(struct win *w)
{
	int i;

	for (i = 0; i < nwins && wins[i] != w; i++)
		;
	if (i == nwins)
		return;
	memmove(&wins[i], &wins[i + 1], sizeof(wins[0]) * (nwins - i - 1));
	wins[nwins - 1] = w;
	w->minimized = 0;
	need_redraw = 1;
}

static struct win *win_new(const char *title, int w, int h, const struct app_ops *ops)
{
	static int cascade;
	struct win *win;

	if (nwins == MAX_WINS)
		return NULL;
	if (w > screen.w - 20)
		w = screen.w - 20;
	if (h > screen.h - TASKBAR_H - TITLE_H - 20)
		h = screen.h - TASKBAR_H - TITLE_H - 20;
	win = calloc(1, sizeof(*win));
	if (!win || surface_init(&win->surf, w, h)) {
		free(win);
		return NULL;
	}
	win->x = 110 + cascade * 28;
	win->y = 40 + cascade * 28;
	if (win->x + w > screen.w)
		win->x = 10;
	if (win->y + h + TASKBAR_H > screen.h)
		win->y = TITLE_H + 4;
	cascade = (cascade + 1) % 8;
	win->w = w;
	win->h = h;
	win->fd = -1;
	win->dirty = 1;
	win->ops = ops;
	snprintf(win->title, sizeof(win->title), "%s", title);
	wins[nwins++] = win;
	need_redraw = 1;
	return win;
}

static void win_close(struct win *w)
{
	int i;

	if (w->ops->close)
		w->ops->close(w);
	for (i = 0; i < nwins && wins[i] != w; i++)
		;
	if (i < nwins) {
		memmove(&wins[i], &wins[i + 1], sizeof(wins[0]) * (nwins - i - 1));
		nwins--;
	}
	if (drag.w == w)
		drag.w = NULL;
	if (mouse_capture == w)
		mouse_capture = NULL;
	surface_free(&w->surf);
	free(w);
	need_redraw = 1;
}

static void win_resize(struct win *w, int nw, int nh)
{
	struct surface s;

	if (nw < 160)
		nw = 160;
	if (nh < 80)
		nh = 80;
	if (nw == w->w && nh == w->h)
		return;
	if (surface_init(&s, nw, nh))
		return;
	surface_free(&w->surf);
	w->surf = s;
	w->w = nw;
	w->h = nh;
	w->dirty = 1;
	if (w->ops->resized)
		w->ops->resized(w);
	need_redraw = 1;
}

/* ---------------------------------------------------------------- icons */

static const uint16_t icon_term[16] = {
	0xffff, 0x8001, 0xbffd, 0xa005, 0xa805, 0xa405, 0xa205, 0xa405,
	0xa8f5, 0xa005, 0xa005, 0xbffd, 0x8001, 0xffff, 0x1ff8, 0x3ffc,
};
static const uint16_t icon_folder[16] = {
	0x0000, 0x7c00, 0x8200, 0x81fe, 0x8001, 0xffff, 0x8001, 0x8001,
	0x8001, 0x8001, 0x8001, 0x8001, 0x8001, 0xffff, 0x0000, 0x0000,
};
static const uint16_t icon_file[16] = {
	0x3fc0, 0x2060, 0x2050, 0x2078, 0x2008, 0x27c8, 0x2008, 0x27c8,
	0x2008, 0x27c8, 0x2008, 0x2008, 0x2008, 0x3ff8, 0x0000, 0x0000,
};
static const uint16_t icon_globe[16] = {
	0x07e0, 0x1998, 0x2184, 0x4182, 0x7ffe, 0x8181, 0x8181, 0xffff,
	0x8181, 0x8181, 0x7ffe, 0x4182, 0x2184, 0x1998, 0x07e0, 0x0000,
};
static const uint16_t icon_edit[16] = {
	0x0006, 0x000f, 0x001e, 0x003c, 0x0078, 0x00f0, 0x01e0, 0x03c0,
	0x0780, 0x0f00, 0x1e00, 0x3c00, 0x7800, 0x7000, 0xe000, 0x0000,
};
/* pointer: outline and fill */
static const uint16_t cur_out[19] = {
	0x8000, 0xc000, 0xa000, 0x9000, 0x8800, 0x8400, 0x8200, 0x8100, 0x8080, 0x8040,
	0x8020, 0x87e0, 0x9400, 0xb200, 0xd200, 0x8900, 0x0900, 0x0480, 0x0380,
};
static const uint16_t cur_in[19] = {
	0x0000, 0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7c00, 0x7e00, 0x7f00, 0x7f80,
	0x7fc0, 0x7800, 0x6800, 0x4c00, 0x0c00, 0x0600, 0x0600, 0x0300, 0x0000,
};

/* ---------------------------------------------------------------- terminal */

struct termapp {
	struct term t;
	int master;
	pid_t pid;
	int sel, sx0, sy0, sx1, sy1;	/* selection, in view cells */
};

/* is view cell (x, y) inside the selection? */
static int term_selected(const struct termapp *a, int x, int y)
{
	int x0 = a->sx0, y0 = a->sy0, x1 = a->sx1, y1 = a->sy1;

	if (!a->sel)
		return 0;
	if (y0 > y1 || (y0 == y1 && x0 > x1)) {
		x0 = a->sx1, y0 = a->sy1, x1 = a->sx0, y1 = a->sy0;
	}
	if (y < y0 || y > y1)
		return 0;
	if (y == y0 && x < x0)
		return 0;
	if (y == y1 && x > x1)
		return 0;
	return 1;
}

/* Hand text to the host's clipboard: OSC 52 on the debug console, which
 * the page turns into navigator.clipboard.writeText(). */
static void host_clipboard(const char *s, int n)
{
	static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char *out = malloc(n / 3 * 4 + 16);
	int fd, o = 0;

	if (!out)
		return;
	for (int i = 0; i < n; i += 3) {
		unsigned v = (unsigned char)s[i] << 16;
		if (i + 1 < n)
			v |= (unsigned char)s[i + 1] << 8;
		if (i + 2 < n)
			v |= (unsigned char)s[i + 2];
		out[o++] = b64[v >> 18 & 63];
		out[o++] = b64[v >> 12 & 63];
		out[o++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
		out[o++] = i + 2 < n ? b64[v & 63] : '=';
	}
	fd = open("/dev/ttyW0", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		dprintf(fd, "\033]52;c;%.*s\a", o, out);
		close(fd);
	}
	free(out);
}

static void term_copy(struct termapp *a)
{
	struct term *t = &a->t;
	char *buf = malloc((size_t)(t->cols + 1) * t->rows + 1);
	int n = 0;

	if (!buf)
		return;
	for (int y = 0; y < t->rows; y++) {
		int start = n, any = 0;
		for (int x = 0; x < t->cols; x++) {
			if (!term_selected(a, x, y))
				continue;
			any = 1;
			buf[n++] = term_view_cell(t, x, y)->ch;
		}
		if (!any)
			continue;
		while (n > start && buf[n - 1] == ' ')
			n--;	/* no trailing blanks */
		buf[n++] = '\n';
	}
	if (n && buf[n - 1] == '\n' && a->sy0 == a->sy1)
		n--;
	if (n)
		host_clipboard(buf, n);
	free(buf);
}

static void term_draw(struct win *w)
{
	struct termapp *a = w->priv;
	struct term *t = &a->t;
	int has_focus = focus() == w;

	fill(&w->surf, 0, 0, w->w, w->h, term_palette[0]);
	for (int y = 0; y < t->rows; y++) {
		for (int x = 0; x < t->cols; x++) {
			struct cell c = *term_view_cell(t, x, y);
			int fg = c.fg, bg = c.bg;
			if ((c.attr & ATTR_BOLD) && fg < 8)
				fg += 8;
			if (!!(c.attr & ATTR_REVERSE) != term_selected(a, x, y)) {
				int tmp = fg;
				fg = bg;
				bg = tmp;
			}
			if (x == t->cx && y == t->cy + t->view && t->cursor_on && has_focus) {
				int tmp = fg;
				fg = bg;
				bg = tmp == bg ? 7 : tmp;
			}
			glyph(&w->surf, 2 + x * FONT_W, 2 + y * FONT_H, c.ch,
			      term_palette[fg], term_palette[bg]);
			if (c.attr & ATTR_UNDERLINE)
				fill(&w->surf, 2 + x * FONT_W, 2 + y * FONT_H + 14, FONT_W, 1,
				     term_palette[fg]);
		}
	}
	if (!has_focus && t->cursor_on && t->cy + t->view < t->rows)
		frame(&w->surf, 2 + t->cx * FONT_W, 2 + (t->cy + t->view) * FONT_H, FONT_W, FONT_H,
		      term_palette[7]);
	if (t->view) {
		char info[32];
		snprintf(info, sizeof(info), " scrolled back %d ", t->view);
		int iw = strlen(info) * FONT_W;
		fill(&w->surf, w->w - iw - 4, 2, iw, FONT_H, term_palette[4]);
		text(&w->surf, w->w - iw - 4, 2, info, term_palette[15], 0);
	}
}

static void term_send(struct termapp *a, const char *s, int n)
{
	while (n > 0) {
		int k = write(a->master, s, n);
		if (k <= 0)
			break;
		s += k;
		n -= k;
	}
}

static void term_key(struct win *w, const struct key *k)
{
	struct termapp *a = w->priv;
	const char *seq = NULL;
	char buf[8];
	int app = a->t.app_cursor;

	/* Shift+PageUp/PageDown scroll back; any other key returns to live */
	if ((k->mods & MOD_SHIFT) && (k->code == KEY_PAGEUP || k->code == KEY_PAGEDOWN)) {
		term_scroll_view(&a->t, (k->code == KEY_PAGEUP ? 1 : -1) * (a->t.rows - 1));
		w->dirty = need_redraw = 1;
		return;
	}
	if (a->t.view && k->code != KEY_LEFTSHIFT && k->code != KEY_RIGHTSHIFT) {
		term_scroll_view(&a->t, -a->t.view);
		w->dirty = need_redraw = 1;
	}

	switch (k->code) {
	case KEY_UP: seq = app ? "\033OA" : "\033[A"; break;
	case KEY_DOWN: seq = app ? "\033OB" : "\033[B"; break;
	case KEY_RIGHT: seq = app ? "\033OC" : "\033[C"; break;
	case KEY_LEFT: seq = app ? "\033OD" : "\033[D"; break;
	case KEY_HOME: seq = "\033[H"; break;
	case KEY_END: seq = "\033[F"; break;
	case KEY_PAGEUP: seq = "\033[5~"; break;
	case KEY_PAGEDOWN: seq = "\033[6~"; break;
	case KEY_INSERT: seq = "\033[2~"; break;
	case KEY_DELETE: seq = "\033[3~"; break;
	case KEY_F1: seq = "\033OP"; break;
	case KEY_F2: seq = "\033OQ"; break;
	case KEY_F3: seq = "\033OR"; break;
	case KEY_F4: seq = "\033OS"; break;
	case KEY_ENTER: case KEY_KPENTER: seq = "\r"; break;
	case KEY_BACKSPACE: seq = "\177"; break;
	case KEY_TAB: seq = "\t"; break;
	case KEY_ESC: seq = "\033"; break;
	}
	if (seq) {
		term_send(a, seq, strlen(seq));
		return;
	}
	if (!k->ch)
		return;
	int n = 0, c = k->ch;
	if (k->mods & MOD_ALT)
		buf[n++] = '\033';
	if (k->mods & MOD_CTRL) {
		if (c >= 'a' && c <= 'z')
			c -= 'a' - 1;
		else if (c >= '@' && c <= '_')
			c -= '@';
		else if (c == ' ')
			c = 0;
	}
	buf[n++] = c;
	term_send(a, buf, n);
}

static void term_io(struct win *w)
{
	struct termapp *a = w->priv;
	unsigned char buf[4096];
	int n = read(a->master, buf, sizeof(buf));

	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		w->dead = 1;	/* the shell is gone */
		return;
	}
	term_write(&a->t, buf, n);
	if (a->t.nreply) {
		term_send(a, a->t.reply, a->t.nreply);
		a->t.nreply = 0;
	}
	if (a->t.dirty) {
		a->t.dirty = 0;
		w->dirty = 1;
		need_redraw = 1;
	}
}

static void term_resized(struct win *w)
{
	struct termapp *a = w->priv;
	struct winsize ws = { 0 };

	ws.ws_col = (w->w - 4) / FONT_W;
	ws.ws_row = (w->h - 4) / FONT_H;
	if (ws.ws_col < 2 || ws.ws_row < 2)
		return;
	term_resize(&a->t, ws.ws_col, ws.ws_row);
	ioctl(a->master, TIOCSWINSZ, &ws);
}

static void term_close(struct win *w)
{
	struct termapp *a = w->priv;

	if (a->pid > 0)
		kill(-a->pid, SIGHUP);
	close(a->master);
	term_free(&a->t);
	free(a);
}

static void term_mouse(struct win *w, int x, int y, int ev, int arg)
{
	struct termapp *a = w->priv;
	int cx = (x - 2) / FONT_W, cy = (y - 2) / FONT_H;

	if (cx < 0)
		cx = 0;
	if (cx >= a->t.cols)
		cx = a->t.cols - 1;
	if (cy < 0)
		cy = 0;
	if (cy >= a->t.rows)
		cy = a->t.rows - 1;
	switch (ev) {
	case M_WHEEL:
		if (!a->t.using_alt)
			term_scroll_view(&a->t, arg * 3);
		break;
	case M_DOWN:
	case M_DOUBLE:
		/* drag to select; the selection is copied to the host */
		a->sel = 0;
		a->sx0 = a->sx1 = cx;
		a->sy0 = a->sy1 = cy;
		break;
	case M_MOVE:
		a->sel = 1;
		a->sx1 = cx;
		a->sy1 = cy;
		break;
	case M_UP:
		if (a->sel)
			term_copy(a);
		break;
	}
	w->dirty = need_redraw = 1;
}

static const struct app_ops term_ops = {
	.draw = term_draw, .key = term_key, .io = term_io, .mouse = term_mouse,
	.resized = term_resized, .close = term_close,
};

static char *child_env[] = {
	"TERM=xterm", "HOME=/root", "SHELL=/bin/bash", "USER=root", "LOGNAME=root",
	"PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", NULL, NULL
};

static void open_terminal(const char *cmd)
{
	struct termapp *a = calloc(1, sizeof(*a));
	struct win *w;
	struct winsize ws = { 0 };
	char *pts;

	if (!a)
		return;
	a->master = posix_openpt(O_RDWR | O_NOCTTY);
	if (a->master < 0 || grantpt(a->master) || unlockpt(a->master) ||
	    !(pts = ptsname(a->master))) {
		if (a->master >= 0)
			close(a->master);
		free(a);
		return;
	}
	w = win_new("Terminal", 80 * FONT_W + 4, 25 * FONT_H + 4, &term_ops);
	if (!w) {
		close(a->master);
		free(a);
		return;
	}
	w->priv = a;
	w->fd = a->master;
	ws.ws_col = (w->w - 4) / FONT_W;
	ws.ws_row = (w->h - 4) / FONT_H;
	term_init(&a->t, ws.ws_col, ws.ws_row);
	ioctl(a->master, TIOCSWINSZ, &ws);
	fcntl(a->master, F_SETFL, O_NONBLOCK);

	char *argv[] = { "/bin/bash", "--login", NULL, NULL, NULL };
	if (cmd) {
		argv[1] = "-c";
		argv[2] = (char *)cmd;
	}
	a->pid = vfork();
	if (a->pid == 0) {
		setsid();
		int s = open(pts, O_RDWR);
		if (s < 0)
			_exit(126);
		ioctl(s, TIOCSCTTY, 0);
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		if (s > 2)
			close(s);
		execve(argv[0], argv, child_env);
		_exit(127);
	}
}

/* ---------------------------------------------------------------- files */

struct entry { char name[NAME_MAX + 1]; int dir; long long size; };

struct files {
	char path[PATH_MAX];
	struct entry *ent;
	int n, sel, top;
};

static void open_editor(const char *path);
static void open_files(const char *path);

static int entcmp(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	if (x->dir != y->dir)
		return y->dir - x->dir;
	return strcmp(x->name, y->name);
}

static void files_load(struct win *w)
{
	struct files *f = w->priv;
	DIR *d = opendir(f->path);
	struct dirent *de;
	int cap = 0;

	free(f->ent);
	f->ent = NULL;
	f->n = f->sel = f->top = 0;
	snprintf(w->title, sizeof(w->title), "Files - %s", f->path);
	if (strcmp(f->path, "/")) {
		f->ent = calloc(cap = 64, sizeof(*f->ent));
		strcpy(f->ent[0].name, "..");
		f->ent[0].dir = 1;
		f->n = 1;
	}
	while (d && (de = readdir(d))) {
		char full[PATH_MAX * 2];
		struct stat st;

		if (de->d_name[0] == '.' && (!de->d_name[1] || !strcmp(de->d_name, "..")))
			continue;
		if (f->n == cap) {
			struct entry *e = realloc(f->ent, sizeof(*e) * (cap = cap ? cap * 2 : 64));
			if (!e)
				break;
			f->ent = e;
		}
		snprintf(full, sizeof(full), "%s/%s", f->path, de->d_name);
		snprintf(f->ent[f->n].name, sizeof(f->ent[0].name), "%s", de->d_name);
		f->ent[f->n].dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
		f->ent[f->n].size = st.st_size;
		f->n++;
	}
	if (d)
		closedir(d);
	qsort(f->ent + (strcmp(f->path, "/") ? 1 : 0), f->n - (strcmp(f->path, "/") ? 1 : 0),
	      sizeof(*f->ent), entcmp);
	w->dirty = need_redraw = 1;
}

static void files_draw(struct win *w)
{
	struct files *f = w->priv;
	int rows = (w->h - ROW_H - 4) / ROW_H;
	char size[32];

	fill(&w->surf, 0, 0, w->w, w->h, C_WIN_BG);
	fill(&w->surf, 0, 0, w->w, ROW_H + 4, rgb(0xe2, 0xe6, 0xea));
	text(&w->surf, 6, 3, f->path, C_WIN_FG, w->w - 12);
	for (int i = 0; i < rows && f->top + i < f->n; i++) {
		struct entry *e = &f->ent[f->top + i];
		int y = ROW_H + 4 + i * ROW_H;
		uint32_t fg = C_WIN_FG;

		if (f->top + i == f->sel) {
			fill(&w->surf, 0, y, w->w, ROW_H, C_SEL);
			fg = C_TEXT;
		}
		bitmap(&w->surf, 6, y + 1, e->dir ? icon_folder : icon_file, 16, 16,
		       e->dir ? rgb(0xd0, 0x9a, 0x2a) : fg);
		text(&w->surf, 28, y + 1, e->name, fg, w->w - 130);
		if (!e->dir) {
			if (e->size < 10240)
				snprintf(size, sizeof(size), "%lld B", e->size);
			else if (e->size < 10 << 20)
				snprintf(size, sizeof(size), "%lld KiB", e->size >> 10);
			else
				snprintf(size, sizeof(size), "%lld MiB", e->size >> 20);
			text(&w->surf, w->w - 90, y + 1, size, fg == C_TEXT ? fg : C_DIM, 0);
		}
	}
}

static void files_open(struct win *w)
{
	struct files *f = w->priv;
	struct entry *e;
	char p[PATH_MAX * 2];

	if (f->sel < 0 || f->sel >= f->n)
		return;
	e = &f->ent[f->sel];
	if (!strcmp(e->name, "..")) {
		char *s = strrchr(f->path, '/');
		if (s == f->path)
			s[1] = 0;
		else if (s)
			*s = 0;
		files_load(w);
		return;
	}
	snprintf(p, sizeof(p), "%s%s%s", f->path, strcmp(f->path, "/") ? "/" : "", e->name);
	if (e->dir) {
		snprintf(f->path, sizeof(f->path), "%s", p);
		files_load(w);
	} else {
		open_editor(p);
	}
}

static void files_scroll_to_sel(struct win *w)
{
	struct files *f = w->priv;
	int rows = (w->h - ROW_H - 4) / ROW_H;

	if (f->sel < f->top)
		f->top = f->sel;
	if (f->sel >= f->top + rows)
		f->top = f->sel - rows + 1;
	w->dirty = need_redraw = 1;
}

static void files_key(struct win *w, const struct key *k)
{
	struct files *f = w->priv;

	switch (k->code) {
	case KEY_UP: if (f->sel > 0) f->sel--; break;
	case KEY_DOWN: if (f->sel < f->n - 1) f->sel++; break;
	case KEY_HOME: f->sel = 0; break;
	case KEY_END: f->sel = f->n - 1; break;
	case KEY_ENTER: files_open(w); return;
	case KEY_BACKSPACE:
		if (f->n && !strcmp(f->ent[0].name, "..")) {
			f->sel = 0;
			files_open(w);
		}
		return;
	case KEY_F5: files_load(w); return;
	default: return;
	}
	files_scroll_to_sel(w);
}

static void files_mouse(struct win *w, int x, int y, int ev, int arg)
{
	struct files *f = w->priv;
	int i = f->top + (y - ROW_H - 4) / ROW_H;

	if (ev == M_WHEEL) {
		f->top -= arg * 3;
		if (f->top > f->n - 1)
			f->top = f->n - 1;
		if (f->top < 0)
			f->top = 0;
		w->dirty = need_redraw = 1;
		return;
	}
	if ((ev != M_DOWN && ev != M_DOUBLE) || y < ROW_H + 4 || i >= f->n)
		return;
	f->sel = i;
	w->dirty = need_redraw = 1;
	if (ev == M_DOUBLE)
		files_open(w);
}

static void files_close(struct win *w)
{
	struct files *f = w->priv;

	free(f->ent);
	free(f);
}

static const struct app_ops files_ops = {
	.draw = files_draw, .key = files_key, .mouse = files_mouse, .close = files_close,
};

static void open_files(const char *path)
{
	struct files *f = calloc(1, sizeof(*f));
	struct win *w;

	if (!f)
		return;
	snprintf(f->path, sizeof(f->path), "%s", path);
	w = win_new("Files", 460, 420, &files_ops);
	if (!w) {
		free(f);
		return;
	}
	w->priv = f;
	files_load(w);
}

/* ---------------------------------------------------------------- editor */

struct editor {
	char path[PATH_MAX];
	char **line;
	int n, cap;
	int cx, cy, top, left;
	int modified;
	char msg[80];
};

static void ed_title(struct win *w)
{
	struct editor *e = w->priv;
	const char *base = strrchr(e->path, '/');

	snprintf(w->title, sizeof(w->title), "%s%s - Editor", e->modified ? "*" : "",
		 e->path[0] ? (base ? base + 1 : e->path) : "untitled");
}

static void ed_insert_line(struct editor *e, int at, const char *s, int len)
{
	if (e->n == e->cap) {
		char **l = realloc(e->line, sizeof(*l) * (e->cap = e->cap ? e->cap * 2 : 64));
		if (!l)
			return;
		e->line = l;
	}
	memmove(&e->line[at + 1], &e->line[at], sizeof(*e->line) * (e->n - at));
	e->line[at] = strndup(s, len);
	e->n++;
}

static void ed_load(struct editor *e)
{
	FILE *fp = e->path[0] ? fopen(e->path, "r") : NULL;
	char *buf = NULL;
	size_t cap = 0;
	ssize_t len;

	while (fp && (len = getline(&buf, &cap, fp)) >= 0) {
		while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
			len--;
		ed_insert_line(e, e->n, buf, len);
	}
	free(buf);
	if (fp)
		fclose(fp);
	if (!e->n)
		ed_insert_line(e, 0, "", 0);
}

static void ed_save(struct win *w)
{
	struct editor *e = w->priv;
	FILE *fp;

	if (!e->path[0]) {
		snprintf(e->path, sizeof(e->path), "/root/untitled.txt");
	}
	fp = fopen(e->path, "w");
	if (!fp) {
		snprintf(e->msg, sizeof(e->msg), "can't save: %s", strerror(errno));
		return;
	}
	for (int i = 0; i < e->n; i++)
		fprintf(fp, "%s\n", e->line[i]);
	if (fclose(fp)) {
		snprintf(e->msg, sizeof(e->msg), "can't save: %s", strerror(errno));
		return;
	}
	sync();
	e->modified = 0;
	snprintf(e->msg, sizeof(e->msg), "saved %d lines", e->n);
	ed_title(w);
}

static void ed_draw(struct win *w)
{
	struct editor *e = w->priv;
	int rows = (w->h - ROW_H - 4) / FONT_H, cols = (w->w - 8) / FONT_W;
	char status[160];

	if (e->cy < e->top)
		e->top = e->cy;
	if (e->cy >= e->top + rows)
		e->top = e->cy - rows + 1;
	if (e->cx < e->left)
		e->left = e->cx;
	if (e->cx >= e->left + cols)
		e->left = e->cx - cols + 1;
	fill(&w->surf, 0, 0, w->w, w->h, rgb(0xff, 0xff, 0xff));
	for (int i = 0; i < rows && e->top + i < e->n; i++) {
		const char *l = e->line[e->top + i];
		int len = strlen(l);
		for (int c = 0; c < cols && e->left + c < len; c++)
			glyph(&w->surf, 4 + c * FONT_W, 2 + i * FONT_H,
			      (unsigned char)l[e->left + c], C_WIN_FG, TRANSPARENT);
	}
	if (focus() == w)
		fill(&w->surf, 4 + (e->cx - e->left) * FONT_W, 2 + (e->cy - e->top) * FONT_H,
		     2, FONT_H, C_SEL);
	fill(&w->surf, 0, w->h - ROW_H, w->w, ROW_H, rgb(0xe2, 0xe6, 0xea));
	snprintf(status, sizeof(status), "Ln %d, Col %d   Ctrl+S save   %s", e->cy + 1, e->cx + 1,
		 e->msg);
	text(&w->surf, 6, w->h - ROW_H + 1, status, C_WIN_FG, w->w - 12);
}

static void ed_key(struct win *w, const struct key *k)
{
	struct editor *e = w->priv;
	int rows = (w->h - ROW_H - 4) / FONT_H;
	char *l = e->line[e->cy];
	int len = strlen(l);

	e->msg[0] = 0;
	if ((k->mods & MOD_CTRL) && (k->ch == 's' || k->ch == 'S')) {
		ed_save(w);
		goto out;
	}
	switch (k->code) {
	case KEY_LEFT:
		if (e->cx > 0)
			e->cx--;
		else if (e->cy > 0)
			e->cx = strlen(e->line[--e->cy]);
		goto out;
	case KEY_RIGHT:
		if (e->cx < len)
			e->cx++;
		else if (e->cy < e->n - 1)
			e->cy++, e->cx = 0;
		goto out;
	case KEY_UP: if (e->cy > 0) e->cy--; break;
	case KEY_DOWN: if (e->cy < e->n - 1) e->cy++; break;
	case KEY_PAGEUP: e->cy = e->cy > rows ? e->cy - rows : 0; break;
	case KEY_PAGEDOWN: e->cy = e->cy + rows < e->n ? e->cy + rows : e->n - 1; break;
	case KEY_HOME: e->cx = 0; goto out;
	case KEY_END: e->cx = len; goto out;
	case KEY_ENTER: case KEY_KPENTER:
		ed_insert_line(e, e->cy + 1, l + e->cx, len - e->cx);
		e->line[e->cy][e->cx] = 0;
		e->cy++;
		e->cx = 0;
		e->modified = 1;
		goto out;
	case KEY_BACKSPACE:
		if (e->cx > 0) {
			memmove(l + e->cx - 1, l + e->cx, len - e->cx + 1);
			e->cx--;
		} else if (e->cy > 0) {
			char *prev = e->line[e->cy - 1];
			int plen = strlen(prev);
			char *j = realloc(prev, plen + len + 1);
			if (!j)
				goto out;
			memcpy(j + plen, l, len + 1);
			e->line[e->cy - 1] = j;
			free(l);
			memmove(&e->line[e->cy], &e->line[e->cy + 1], sizeof(*e->line) * (e->n - e->cy - 1));
			e->n--;
			e->cy--;
			e->cx = plen;
		}
		e->modified = 1;
		goto out;
	case KEY_DELETE:
		if (e->cx < len) {
			memmove(l + e->cx, l + e->cx + 1, len - e->cx);
		} else if (e->cy < e->n - 1) {
			char *next = e->line[e->cy + 1];
			char *j = realloc(l, len + strlen(next) + 1);
			if (!j)
				goto out;
			strcpy(j + len, next);
			e->line[e->cy] = j;
			free(next);
			memmove(&e->line[e->cy + 1], &e->line[e->cy + 2], sizeof(*e->line) * (e->n - e->cy - 2));
			e->n--;
		}
		e->modified = 1;
		goto out;
	default:
		if (k->code == KEY_TAB || (k->ch >= 32 && k->ch < 127 && !(k->mods & (MOD_CTRL | MOD_ALT)))) {
			const char *ins = k->code == KEY_TAB ? "    " : NULL;
			char one[2] = { (char)k->ch, 0 };
			int n;
			if (!ins)
				ins = one;
			n = strlen(ins);
			char *j = realloc(l, len + n + 1);
			if (!j)
				goto out;
			memmove(j + e->cx + n, j + e->cx, len - e->cx + 1);
			memcpy(j + e->cx, ins, n);
			e->line[e->cy] = j;
			e->cx += n;
			e->modified = 1;
		}
		goto out;
	}
	/* vertical moves keep the column inside the line */
	len = strlen(e->line[e->cy]);
	if (e->cx > len)
		e->cx = len;
out:
	ed_title(w);
	w->dirty = need_redraw = 1;
}

static void ed_mouse(struct win *w, int x, int y, int ev, int arg)
{
	struct editor *e = w->priv;

	if (ev == M_WHEEL) {
		e->cy -= arg * 3;
		if (e->cy < 0)
			e->cy = 0;
		if (e->cy >= e->n)
			e->cy = e->n - 1;
	} else if (ev == M_DOWN) {
		int cy = e->top + (y - 2) / FONT_H, cx = e->left + (x - 4 + FONT_W / 2) / FONT_W;
		if (cy >= e->n)
			cy = e->n - 1;
		e->cy = cy < 0 ? 0 : cy;
		int len = strlen(e->line[e->cy]);
		e->cx = cx < 0 ? 0 : cx > len ? len : cx;
	} else {
		return;
	}
	if (e->cx > (int)strlen(e->line[e->cy]))
		e->cx = strlen(e->line[e->cy]);
	w->dirty = need_redraw = 1;
}

static void ed_close(struct win *w)
{
	struct editor *e = w->priv;

	for (int i = 0; i < e->n; i++)
		free(e->line[i]);
	free(e->line);
	free(e);
}

static const struct app_ops editor_ops = {
	.draw = ed_draw, .key = ed_key, .mouse = ed_mouse, .close = ed_close,
};

static void open_editor(const char *path)
{
	struct editor *e = calloc(1, sizeof(*e));
	struct win *w;

	if (!e)
		return;
	if (path)
		snprintf(e->path, sizeof(e->path), "%s", path);
	ed_load(e);
	w = win_new("Editor", 600, 440, &editor_ops);
	if (!w) {
		ed_close(&(struct win){ .priv = e });
		return;
	}
	w->priv = e;
	ed_title(w);
}

/* ---------------------------------------------------------------- about */

static void about_draw(struct win *w)
{
	struct utsname u;
	char line[160];
	long pages = sysconf(_SC_PHYS_PAGES), avail = sysconf(_SC_AVPHYS_PAGES);
	FILE *fp = fopen("/proc/uptime", "r");
	double up = 0;

	if (fp) {
		if (fscanf(fp, "%lf", &up) != 1)
			up = 0;
		fclose(fp);
	}
	uname(&u);
	fill(&w->surf, 0, 0, w->w, w->h, C_WIN_BG);
	bitmap(&w->surf, 16, 16, icon_globe, 16, 16, C_SEL);
	text(&w->surf, 44, 16, "WebLinux", C_WIN_FG, 0);
	snprintf(line, sizeof(line), "%s %s on %s", u.sysname, u.release, u.machine);
	text(&w->surf, 16, 48, line, C_WIN_FG, w->w - 32);
	snprintf(line, sizeof(line), "Memory: %ld MiB, %ld MiB free", pages / 256, avail / 256);
	text(&w->surf, 16, 68, line, C_WIN_FG, w->w - 32);
	snprintf(line, sizeof(line), "Up %d min %d s", (int)up / 60, (int)up % 60);
	text(&w->surf, 16, 88, line, C_WIN_FG, w->w - 32);
	text(&w->surf, 16, 120, "Linux natively in WebAssembly.", C_DIM, w->w - 32);
	text(&w->surf, 16, 140, "Written by AI (vibe coded).", C_DIM, w->w - 32);
}

static const struct app_ops about_ops = { .draw = about_draw };

/* ---------------------------------------------------------------- fullscreen programs */

static void run_fullscreen(char *const argv[])
{
	pid_t pid = vfork();

	if (pid == 0) {
		execve(argv[0], argv, child_env);
		_exit(127);
	}
	if (pid > 0)
		fullscreen_pid = pid;
}

/* ---------------------------------------------------------------- start menu */

struct menu_item { const char *label; const uint16_t *icon; void (*fn)(void); };

static void m_terminal(void) { open_terminal(NULL); }
static void m_files(void) { open_files("/root"); }
static void m_editor(void) { open_editor(NULL); }
static void m_browser(void)
{
	static char *argv[] = { "/usr/bin/netsurf", NULL };

	if (access(argv[0], X_OK) == 0)
		run_fullscreen(argv);
	else
		open_terminal("echo 'The browser (NetSurf) is not installed yet.'; sleep 5");
}
static void m_about(void) { win_new("About WebLinux", 340, 170, &about_ops); }
static void m_console(void);

static const struct menu_item menu[] = {
	{ "Terminal", icon_term, m_terminal },
	{ "Files", icon_folder, m_files },
	{ "Editor", icon_edit, m_editor },
	{ "Web browser", icon_globe, m_browser },
	{ "About", icon_file, m_about },
	{ "Text console", icon_term, m_console },
};
#define NMENU (int)(sizeof(menu) / sizeof(menu[0]))
#define MENU_W 200
#define MENU_ROW 28

static struct rect menu_rect(void)
{
	return (struct rect){ 4, screen.h - TASKBAR_H - NMENU * MENU_ROW - 8, MENU_W, NMENU * MENU_ROW + 8 };
}

/* desktop icons: the first four menu entries */
#define NICONS 4
static struct rect icon_rect(int i)
{
	return (struct rect){ 20, 20 + i * 76, 72, 64 };
}

/* ---------------------------------------------------------------- drawing */

static int task_button_w(void)
{
	int w = nwins ? (screen.w - 90 - 70) / nwins : 0;
	return w > 180 ? 180 : w;
}

static void draw_window(struct win *w, int top)
{
	int ox = w->x - 1, oy = w->y - TITLE_H - 1, ow = w->w + 2, oh = w->h + TITLE_H + 2;

	fill_alpha(&back, ox + 4, oy + 4, ow, oh, 0, 90);
	frame(&back, ox, oy, ow, oh, C_BORDER);
	fill(&back, w->x, w->y - TITLE_H, w->w, TITLE_H, top ? C_TITLE : C_TITLE_OFF);
	text(&back, w->x + 8, w->y - TITLE_H + 3, w->title, C_TEXT, w->w - 60);
	/* minimize and close */
	text(&back, w->x + w->w - 40, w->y - TITLE_H + 3, "_", C_TEXT, 0);
	text(&back, w->x + w->w - 18, w->y - TITLE_H + 3, "x", C_TEXT, 0);
	if (w->dirty || w->drawn_focused != top) {
		w->dirty = 0;
		w->drawn_focused = top;
		w->ops->draw(w);
	}
	blit(&back, w->x, w->y, &w->surf);
	/* resize grip */
	for (int i = 3; i < CORNER; i += 4)
		fill(&back, w->x + w->w - i, w->y + w->h - 2, i, 1, C_DIM);
}

static void redraw(void)
{
	struct win *top = focus();
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	char clock[16];
	int x;

	blit(&back, 0, 0, &wallpaper);
	for (int i = 0; i < NICONS; i++) {
		struct rect r = icon_rect(i);
		int lw = strlen(menu[i].label) * FONT_W;
		bitmap(&back, r.x + 28, r.y + 6, menu[i].icon, 16, 16, C_TEXT);
		fill_alpha(&back, r.x + (r.w - lw) / 2 - 2, r.y + 30, lw + 4, FONT_H + 2, 0, 100);
		text(&back, r.x + (r.w - lw) / 2, r.y + 31, menu[i].label, C_TEXT, 0);
	}
	for (int i = 0; i < nwins; i++)
		if (!wins[i]->minimized)
			draw_window(wins[i], wins[i] == top);

	/* taskbar */
	fill(&back, 0, screen.h - TASKBAR_H, screen.w, TASKBAR_H, C_BAR);
	fill(&back, 0, screen.h - TASKBAR_H, screen.w, 1, C_BAR_HI);
	fill(&back, 4, screen.h - TASKBAR_H + 4, 80, TASKBAR_H - 8, menu_open ? C_SEL : C_BAR_HI);
	bitmap(&back, 10, screen.h - TASKBAR_H + 7, icon_globe, 16, 16, C_ACCENT);
	text(&back, 32, screen.h - TASKBAR_H + 7, "Menu", C_TEXT, 0);
	x = 90;
	for (int i = 0; i < nwins; i++) {
		int bw = task_button_w();
		fill(&back, x, screen.h - TASKBAR_H + 4, bw - 4, TASKBAR_H - 8,
		     wins[i] == top ? C_TITLE : C_BAR_HI);
		text(&back, x + 6, screen.h - TASKBAR_H + 7, wins[i]->title, wins[i]->minimized ? C_DIM : C_TEXT, bw - 16);
		x += bw;
	}
	strftime(clock, sizeof(clock), "%H:%M", tm);
	text(&back, screen.w - 52, screen.h - TASKBAR_H + 7, clock, C_TEXT, 0);
	last_clock = tm->tm_min;

	if (menu_open) {
		struct rect r = menu_rect();
		fill_alpha(&back, r.x + 4, r.y + 4, r.w, r.h, 0, 90);
		fill(&back, r.x, r.y, r.w, r.h, C_BAR);
		frame(&back, r.x, r.y, r.w, r.h, C_BAR_HI);
		for (int i = 0; i < NMENU; i++) {
			int y = r.y + 4 + i * MENU_ROW;
			if (mx >= r.x && mx < r.x + r.w && my >= y && my < y + MENU_ROW)
				fill(&back, r.x + 2, y, r.w - 4, MENU_ROW, C_SEL);
			bitmap(&back, r.x + 10, y + 6, menu[i].icon, 16, 16, C_ACCENT);
			text(&back, r.x + 36, y + 6, menu[i].label, C_TEXT, 0);
		}
	}

	bitmap(&back, mx, my, cur_in, 16, 19, rgb(255, 255, 255));
	bitmap(&back, mx, my, cur_out, 16, 19, 0);

	for (int y = 0; y < screen.h; y++)
		memcpy(screen.px + (size_t)y * screen.stride, back.px + (size_t)y * back.stride, screen.w * 4);
	need_redraw = 0;
}

/* ---------------------------------------------------------------- input */

static const char keymap[2][KEY_SPACE + 1] = {
	{ 0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, '\t',
	  'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
	  'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
	  'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ' },
	{ 0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, '\t',
	  'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
	  'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
	  'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ' },
};

static void m_console(void)
{
	/* hand the VT back to the text console: a login shell replaces us */
	static char *argv[] = { "/bin/bash", "--login", NULL };

	ioctl(0, KDSETMODE, KD_TEXT);
	ioctl(0, KDSKBMODE, K_UNICODE);
	printf("\033[2J\033[H(the desktop comes back when this shell exits)\n");
	fflush(stdout);
	execve(argv[0], argv, child_env);
	exit(1);
}

static struct win *win_at(int x, int y, int *part)
{
	for (int i = nwins - 1; i >= 0; i--) {
		struct win *w = wins[i];
		if (w->minimized)
			continue;
		if (x >= w->x - 1 && x < w->x + w->w + 1 && y >= w->y - TITLE_H - 1 && y < w->y + w->h + 1) {
			if (y < w->y)
				*part = x >= w->x + w->w - 22 ? 2 : x >= w->x + w->w - 44 ? 3 : 1;
			else if (x >= w->x + w->w - CORNER && y >= w->y + w->h - CORNER)
				*part = 4;
			else
				*part = 0;
			return w;
		}
	}
	return NULL;
}

static void handle_key(int code, int value)
{
	int bit = 0;
	struct win *w;
	struct key k;

	switch (code) {
	case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: bit = MOD_SHIFT; break;
	case KEY_LEFTCTRL: case KEY_RIGHTCTRL: bit = MOD_CTRL; break;
	case KEY_LEFTALT: case KEY_RIGHTALT: bit = MOD_ALT; break;
	}
	if (bit) {
		mods = value ? mods | bit : mods & ~bit;
		return;
	}
	if (!value)
		return;
	if ((mods & (MOD_CTRL | MOD_ALT)) == (MOD_CTRL | MOD_ALT) && code == KEY_T) {
		open_terminal(NULL);
		return;
	}
	w = focus();
	if ((mods & MOD_ALT) && code == KEY_F4) {
		if (w)
			win_close(w);
		return;
	}
	if ((mods & MOD_ALT) && code == KEY_TAB) {
		if (nwins > 1)
			raise_win(wins[0]);
		return;
	}
	if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA) {
		menu_open = !menu_open;
		need_redraw = 1;
		return;
	}
	if (!w || !w->ops->key)
		return;
	k.code = code;
	k.mods = mods;
	k.ch = 0;
	if (code <= KEY_SPACE)
		k.ch = (unsigned char)keymap[(mods & MOD_SHIFT) ? 1 : 0][code];
	else if (code == KEY_KPENTER)
		k.ch = '\n';
	if (k.ch == '\n' || k.ch == 8 || k.ch == 27)
		k.ch = 0;
	if (code == KEY_TAB)
		k.ch = 0;
	w->ops->key(w, &k);
}

static void click(int x, int y)
{
	long long t = now_ms();
	int dbl = t - last_click.t < 400 && abs(x - last_click.x) < 5 && abs(y - last_click.y) < 5;
	int part;
	struct win *w;

	last_click.t = dbl ? 0 : t;
	last_click.x = x;
	last_click.y = y;

	if (menu_open) {
		struct rect r = menu_rect();
		menu_open = 0;
		need_redraw = 1;
		if (x >= r.x && x < r.x + r.w && y >= r.y + 4 && y < r.y + r.h - 4) {
			menu[(y - r.y - 4) / MENU_ROW].fn();
			return;
		}
		if (!(y >= screen.h - TASKBAR_H && x < 86))
			return;
		return;
	}
	if (y >= screen.h - TASKBAR_H) {
		if (x < 86) {
			menu_open = 1;
		} else if (nwins) {
			int i = (x - 90) / task_button_w();
			if (i >= 0 && i < nwins) {
				w = wins[i];
				if (w == focus())
					w->minimized = 1;
				else
					raise_win(w);
			}
		}
		need_redraw = 1;
		return;
	}
	w = win_at(x, y, &part);
	if (!w) {
		for (int i = 0; i < NICONS; i++) {
			struct rect r = icon_rect(i);
			if (dbl && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
				menu[i].fn();
		}
		return;
	}
	if (w != focus()) {
		struct win *old = focus();
		raise_win(w);
		if (old)
			old->dirty = 1;
		w->dirty = 1;
	}
	switch (part) {
	case 1:
		drag.w = w;
		drag.dx = x - w->x;
		drag.dy = y - w->y;
		drag.resize = 0;
		break;
	case 2:
		w->dead = 1;
		break;
	case 3:
		w->minimized = 1;
		need_redraw = 1;
		break;
	case 4:
		drag.w = w;
		drag.dx = w->x + w->w - x;
		drag.dy = w->y + w->h - y;
		drag.resize = 1;
		break;
	default:
		if (w->ops->mouse) {
			mouse_capture = w;
			w->ops->mouse(w, x - w->x, y - w->y, dbl ? M_DOUBLE : M_DOWN, 1);
		}
	}
}

static void pointer_moved(void)
{
	if (mouse_capture && (buttons & 1))
		mouse_capture->ops->mouse(mouse_capture, mx - mouse_capture->x,
					  my - mouse_capture->y, M_MOVE, 0);
	if (drag.w) {
		struct win *w = drag.w;
		if (drag.resize) {
			win_resize(w, mx + drag.dx - w->x, my + drag.dy - w->y);
		} else {
			w->x = mx - drag.dx;
			w->y = my - drag.dy;
			if (w->y < TITLE_H)
				w->y = TITLE_H;
			if (w->y > screen.h - TASKBAR_H - 4)
				w->y = screen.h - TASKBAR_H - 4;
		}
	}
	need_redraw = 1;
}

static void handle_event(const struct input_event *ev)
{
	static int abs_x = -1, abs_y = -1, moved;

	if (fullscreen_pid)
		return;
	switch (ev->type) {
	case EV_KEY:
		if (ev->code == BTN_LEFT) {
			buttons = ev->value ? buttons | 1 : buttons & ~1;
			if (ev->value) {
				click(mx, my);
			} else {
				drag.w = NULL;
				if (mouse_capture) {
					struct win *c = mouse_capture;
					mouse_capture = NULL;
					c->ops->mouse(c, mx - c->x, my - c->y, M_UP, 0);
				}
			}
		} else if (ev->code < BTN_MISC || ev->code >= KEY_OK) {
			handle_key(ev->code, ev->value);
		}
		break;
	case EV_ABS:
		if (ev->code == ABS_X)
			abs_x = ev->value, moved = 1;
		else if (ev->code == ABS_Y)
			abs_y = ev->value, moved = 1;
		break;
	case EV_REL:
		if (ev->code == REL_WHEEL) {
			int part;
			struct win *w = win_at(mx, my, &part);
			if (w && part == 0 && w->ops->mouse)
				w->ops->mouse(w, mx - w->x, my - w->y, M_WHEEL, ev->value);
		}
		break;
	case EV_SYN:
		if (moved) {
			moved = 0;
			if (abs_x >= 0)
				mx = (int)((long long)abs_x * (screen.w - 1) / 32767);
			if (abs_y >= 0)
				my = (int)((long long)abs_y * (screen.h - 1) / 32767);
			pointer_moved();
		}
		break;
	}
}

/* ---------------------------------------------------------------- setup */

static void restore_console(void)
{
	ioctl(0, KDSETMODE, KD_TEXT);
	ioctl(0, KDSKBMODE, K_UNICODE);
}

static void on_signal(int sig)
{
	if (sig == SIGCHLD) {
		got_child = 1;
		return;
	}
	restore_console();
	_exit(0);
}

static void reap(void)
{
	int st;
	pid_t pid;

	got_child = 0;
	while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
		if (pid == fullscreen_pid) {
			fullscreen_pid = 0;
			ioctl(0, KDSETMODE, KD_GRAPHICS);
			for (int i = 0; i < nwins; i++)
				wins[i]->dirty = 1;
			need_redraw = 1;
		}
		for (int i = 0; i < nwins; i++) {
			if (wins[i]->ops == &term_ops) {
				struct termapp *a = wins[i]->priv;
				if (a->pid == pid)
					a->pid = 0;
			}
		}
	}
}

static int open_fb(void)
{
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	int fd = open("/dev/fb0", O_RDWR);

	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f) ||
	    v.bits_per_pixel != 32) {
		fprintf(stderr, "wldesk: no usable /dev/fb0\n");
		return -1;
	}
	fblen = f.smem_len;
	fbmem = mmap(NULL, fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fbmem == MAP_FAILED) {
		perror("wldesk: mmap /dev/fb0");
		return -1;
	}
	screen.px = fbmem;
	screen.w = v.xres;
	screen.h = v.yres;
	screen.stride = f.line_length / 4;
	surface_noclip(&screen);
	return 0;
}

static void open_inputs(void)
{
	char path[32];

	for (int i = 0; i < 16 && ninput < MAX_INPUT; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd >= 0)
			input_fd[ninput++] = fd;
	}
}

static void make_wallpaper(void)
{
	surface_init(&wallpaper, screen.w, screen.h);
	gradient(&wallpaper, 0, 0, screen.w, screen.h, C_DESK_TOP, C_DESK_BOT);
	/* a faint grid of dots */
	for (int y = 24; y < screen.h; y += 32)
		for (int x = 24; x < screen.w; x += 32)
			fill_alpha(&wallpaper, x, y, 2, 2, rgb(255, 255, 255), 18);
	const char *brand = "WebLinux";
	int w = strlen(brand) * FONT_W;
	text(&wallpaper, screen.w - w - 24, screen.h - TASKBAR_H - 36, brand, rgb(0x6d, 0x8c, 0xad), 0);
}

int main(int argc, char **argv)
{
	struct sigaction sa = { 0 };

	if (getenv("TZ")) {	/* pass the time zone on to programs */
		static char tz[64];
		snprintf(tz, sizeof(tz), "TZ=%s", getenv("TZ"));
		child_env[6] = tz;
	}
	if (open_fb())
		return 1;
	open_inputs();
	if (!ninput)
		fprintf(stderr, "wldesk: no input devices\n");
	if (surface_init(&back, screen.w, screen.h))
		return 1;
	make_wallpaper();
	mx = screen.w / 2;
	my = screen.h / 2;

	sa.sa_handler = on_signal;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	ioctl(0, KDSETMODE, KD_GRAPHICS);
	ioctl(0, KDSKBMODE, K_OFF);
	chdir("/root");

	if (argc > 1 && !strcmp(argv[1], "--bench")) {
		/* time full redraws with a few windows open */
		open_files("/bin");
		open_editor("/etc/profile");
		m_about();
		long long t0 = now_ms();
		for (int i = 0; i < 20; i++) {
			for (int j = 0; j < nwins; j++)
				wins[j]->dirty = 1;
			redraw();
		}
		printf("wldesk: %lld ms per full redraw\n", (now_ms() - t0) / 20);
		return 0;
	}
	if (argc < 2 || strcmp(argv[1], "--empty"))
		open_terminal(NULL);

	for (;;) {
		struct pollfd pfd[MAX_INPUT + MAX_WINS];
		struct win *pw[MAX_INPUT + MAX_WINS];
		int n = 0, timeout = 1000;

		if (got_child)
			reap();
		for (int i = 0; i < nwins; i++)
			if (wins[i]->dead)
				win_close(wins[i--]);
		for (int i = 0; i < ninput; i++) {
			pw[n] = NULL;
			pfd[n++] = (struct pollfd){ input_fd[i], POLLIN, 0 };
		}
		for (int i = 0; i < nwins; i++) {
			if (wins[i]->fd >= 0) {
				pw[n] = wins[i];
				pfd[n++] = (struct pollfd){ wins[i]->fd, POLLIN, 0 };
			}
		}
		long long since = now_ms() - last_redraw;
		if (need_redraw && !fullscreen_pid)
			timeout = since >= 16 ? 0 : 16 - since;
		int r = poll(pfd, n, timeout);
		if (r < 0 && errno != EINTR)
			break;
		for (int i = 0; r > 0 && i < n; i++) {
			if (!pfd[i].revents)
				continue;
			if (pw[i]) {
				if (pw[i]->ops->io)
					pw[i]->ops->io(pw[i]);
			} else {
				struct input_event ev[64];
				int k = read(pfd[i].fd, ev, sizeof(ev));
				for (int j = 0; j < k / (int)sizeof(ev[0]); j++)
					handle_event(&ev[j]);
			}
		}
		if (!fullscreen_pid) {
			time_t now = time(NULL);
			if (localtime(&now)->tm_min != last_clock)
				need_redraw = 1;
			if (need_redraw && now_ms() - last_redraw >= 16) {
				long long t0 = now_ms();
				redraw();
				last_redraw = now_ms();
				if (getenv("WLDESK_DEBUG"))
					fprintf(stderr, "redraw %lld ms\n", last_redraw - t0);
			}
		}
	}
	restore_console();
	return 0;
}
