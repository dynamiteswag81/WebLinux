/*
 * wldesk: the WebLinux desktop.
 *
 * Runs on a VT (tty1) with the text console switched off, draws straight
 * into /dev/fb0 and reads the keyboard and pointer from /dev/input.  One
 * process: a window manager with a taskbar, a start menu, pop-up menus and
 * desktop icons (the launchers plus the files in ~/Desktop), and built-in
 * applications (termapp.c, files.c, editor.c, apps.c).  Other graphical
 * programs, like the NetSurf browser, get the whole frame buffer while they
 * run.
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

#include "desk.h"

#define MAX_WINS	32
#define MAX_INPUT	8
#define CORNER		14
#define DESKTOP_DIR	"/root/Desktop"

/* ---------------------------------------------------------------- state */

struct surface screen;
static struct surface back, wallpaper;
static uint32_t *fbmem;
static size_t fblen;
static struct win *wins[MAX_WINS];	/* z order, last on top */
static int nwins;
static int input_fd[MAX_INPUT], ninput;
static int mx, my, buttons, mods;
int need_redraw = 1;
static pid_t fullscreen_pid;
static volatile sig_atomic_t got_child;
static struct { struct win *w; int dx, dy, resize; } drag;
static struct win *mouse_capture;	/* gets moves and the release after a click */
static struct { long long t; int x, y; } last_click;
static int last_clock = -1;
static long long last_redraw;
uint32_t desk_top = C_DESK_TOP, desk_bot = C_DESK_BOT;

long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct win *focus(void)
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

struct win *win_new(const char *title, int w, int h, const struct app_ops *ops)
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

void win_center(struct win *w)
{
	w->x = (screen.w - w->w) / 2;
	w->y = (screen.h - TASKBAR_H - w->h) / 3 + TITLE_H;
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

void win_resize(struct win *w, int nw, int nh)
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

int win_alive(const struct win *w)
{
	for (int i = 0; i < nwins; i++)
		if (wins[i] == w)
			return 1;
	return 0;
}

static void win_toggle_max(struct win *w)
{
	if (w->fixed)
		return;
	if (w->maximized) {
		w->maximized = 0;
		w->x = w->rx;
		w->y = w->ry;
		win_resize(w, w->rw, w->rh);
	} else {
		w->maximized = 1;
		w->rx = w->x, w->ry = w->y, w->rw = w->w, w->rh = w->h;
		w->x = 0;
		w->y = TITLE_H;
		win_resize(w, screen.w, screen.h - TASKBAR_H - TITLE_H);
	}
	need_redraw = 1;
}

/* Run a program and wait for it: file operations use BusyBox's cp/mv/rm. */
int run_wait(char *const argv[])
{
	int st;
	pid_t pid = vfork();

	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
		}
		execve(argv[0], argv, child_env);
		_exit(127);
	}
	if (pid < 0)
		return -1;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ---------------------------------------------------------------- icons */

const uint16_t icon_term[16] = {
	0xffff, 0x8001, 0xbffd, 0xa005, 0xa805, 0xa405, 0xa205, 0xa405,
	0xa8f5, 0xa005, 0xa005, 0xbffd, 0x8001, 0xffff, 0x1ff8, 0x3ffc,
};
const uint16_t icon_folder[16] = {
	0x0000, 0x7c00, 0x8200, 0x81fe, 0x8001, 0xffff, 0x8001, 0x8001,
	0x8001, 0x8001, 0x8001, 0x8001, 0x8001, 0xffff, 0x0000, 0x0000,
};
const uint16_t icon_file[16] = {
	0x3fc0, 0x2060, 0x2050, 0x2078, 0x2008, 0x27c8, 0x2008, 0x27c8,
	0x2008, 0x27c8, 0x2008, 0x2008, 0x2008, 0x3ff8, 0x0000, 0x0000,
};
const uint16_t icon_globe[16] = {
	0x07e0, 0x1998, 0x2184, 0x4182, 0x7ffe, 0x8181, 0x8181, 0xffff,
	0x8181, 0x8181, 0x7ffe, 0x4182, 0x2184, 0x1998, 0x07e0, 0x0000,
};
const uint16_t icon_edit[16] = {
	0x0006, 0x000f, 0x001e, 0x003c, 0x0078, 0x00f0, 0x01e0, 0x03c0,
	0x0780, 0x0f00, 0x1e00, 0x3c00, 0x7800, 0x7000, 0xe000, 0x0000,
};
const uint16_t icon_calc[16] = {
	0x7ffe, 0x4002, 0x5ffa, 0x500a, 0x5ffa, 0x4002, 0x5b6a, 0x5b6a,
	0x4002, 0x5b6a, 0x5b6a, 0x4002, 0x5b6a, 0x5b6a, 0x4002, 0x7ffe,
};
const uint16_t icon_mine[16] = {
	0x0100, 0x0100, 0x2108, 0x13d0, 0x0ff0, 0x0e70, 0x1c78, 0xfc7f,
	0x1ff8, 0x1ff8, 0x0ff0, 0x0ff0, 0x13c8, 0x2104, 0x0100, 0x0100,
};
const uint16_t icon_snake[16] = {
	0x0000, 0x0000, 0x3ffc, 0x3ffc, 0x000c, 0x000c, 0x3ffc, 0x3ffc,
	0x3000, 0x3000, 0x3ff6, 0x3ff6, 0x0000, 0x0000, 0x0000, 0x0000,
};
const uint16_t icon_tasks[16] = {
	0xffff, 0x8001, 0x8001, 0x8001, 0x8009, 0x8015, 0x8015, 0x8823,
	0x9423, 0xa241, 0xa241, 0xc181, 0x8001, 0x8001, 0x8001, 0xffff,
};
const uint16_t icon_paint[16] = {
	0x0000, 0x000e, 0x001e, 0x003c, 0x0078, 0x00f0, 0x01e0, 0x03c0,
	0x0780, 0x0300, 0x3600, 0x7c00, 0x7c00, 0xf800, 0x7000, 0x0000,
};
const uint16_t icon_gear[16] = {
	0x0180, 0x0180, 0x1998, 0x3ffc, 0x1ff8, 0x1c38, 0x3818, 0xf81f,
	0xf81f, 0x3818, 0x1c38, 0x1ff8, 0x3ffc, 0x1998, 0x0180, 0x0180,
};
const uint16_t icon_image[16] = {
	0xffff, 0x8001, 0x8001, 0x8c01, 0x9e01, 0x8c01, 0x8001, 0x8081,
	0x81c1, 0x83e1, 0x87f1, 0x8ff9, 0x9ffd, 0x8001, 0xffff, 0x0000,
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

/* ---------------------------------------------------------------- pop-up menus */

#define POP_MAX		24
#define POP_ROW		24
#define POP_SEP		9

static struct {
	int open, x, y, w, h, n, hover;
	char item[POP_MAX][48];
	const uint16_t *icon[POP_MAX];
	void (*fn)(void *, int);
	void *ctx;
} pop;
static int start_menu_open;	/* the open pop-up is the start menu */

static int pop_item_h(int i)
{
	return strcmp(pop.item[i], "-") ? POP_ROW : POP_SEP;
}

static void popup_open(int x, int y, const char *const *items, const uint16_t *const *icons,
		       int n, void (*fn)(void *, int), void *ctx)
{
	int w = 0;

	if (n > POP_MAX)
		n = POP_MAX;
	pop.n = n;
	pop.h = 8;
	for (int i = 0; i < n; i++) {
		snprintf(pop.item[i], sizeof(pop.item[i]), "%s", items[i]);
		pop.icon[i] = icons ? icons[i] : NULL;
		int lw = strlen(pop.item[i]) * FONT_W;
		if (lw > w)
			w = lw;
		pop.h += pop_item_h(i);
	}
	pop.w = w + (icons ? 56 : 32);
	if (pop.w < 140)
		pop.w = 140;
	if (x + pop.w > screen.w)
		x = screen.w - pop.w;
	if (y + pop.h > screen.h - TASKBAR_H && !start_menu_open)
		y = y - pop.h >= 0 ? y - pop.h : screen.h - TASKBAR_H - pop.h;
	pop.x = x < 0 ? 0 : x;
	pop.y = y < 0 ? 0 : y;
	pop.fn = fn;
	pop.ctx = ctx;
	pop.hover = -1;
	pop.open = 1;
	need_redraw = 1;
}

void popup_menu(int x, int y, const char *const *items, int n,
		void (*fn)(void *ctx, int i), void *ctx)
{
	start_menu_open = 0;
	popup_open(x, y, items, NULL, n, fn, ctx);
}

static void popup_close(void)
{
	pop.open = start_menu_open = 0;
	need_redraw = 1;
}

static int pop_enabled(int i)
{
	return i >= 0 && i < pop.n && pop.item[i][0] != '!' && strcmp(pop.item[i], "-");
}

static int pop_at(int x, int y)
{
	int yy = pop.y + 4;

	if (x < pop.x || x >= pop.x + pop.w)
		return -1;
	for (int i = 0; i < pop.n; i++) {
		if (y >= yy && y < yy + pop_item_h(i))
			return i;
		yy += pop_item_h(i);
	}
	return -1;
}

static void pop_pick(int i)
{
	void (*fn)(void *, int) = pop.fn;
	void *ctx = pop.ctx;

	popup_close();
	if (fn && i >= 0)
		fn(ctx, i);
}

static void draw_popup(void)
{
	int y = pop.y + 4;

	fill_alpha(&back, pop.x + 4, pop.y + 4, pop.w, pop.h, 0, 90);
	fill(&back, pop.x, pop.y, pop.w, pop.h, C_BAR);
	frame(&back, pop.x, pop.y, pop.w, pop.h, C_BAR_HI);
	for (int i = 0; i < pop.n; i++) {
		const char *label = pop.item[i];
		int tx = pop.x + (pop.icon[i] || start_menu_open ? 36 : 14);
		if (!strcmp(label, "-")) {
			fill(&back, pop.x + 8, y + POP_SEP / 2, pop.w - 16, 1, C_BAR_HI);
			y += POP_SEP;
			continue;
		}
		if (i == pop.hover && pop_enabled(i))
			fill(&back, pop.x + 2, y, pop.w - 4, POP_ROW, C_SEL);
		if (pop.icon[i])
			bitmap(&back, pop.x + 10, y + 4, pop.icon[i], 16, 16, C_ACCENT);
		if (label[0] == '!')
			text(&back, tx, y + 4, label + 1, C_DIM, pop.w - 20);
		else
			text(&back, tx, y + 4, label, C_TEXT, pop.w - 20);
		y += POP_ROW;
	}
}

static int popup_key(int code)
{
	int i = pop.hover;

	switch (code) {
	case KEY_ESC:
		popup_close();
		return 1;
	case KEY_UP:
	case KEY_DOWN:
		for (int k = 0; k < pop.n; k++) {
			i = code == KEY_UP ? (i <= 0 ? pop.n - 1 : i - 1) : (i + 1) % pop.n;
			if (pop_enabled(i))
				break;
		}
		pop.hover = i;
		need_redraw = 1;
		return 1;
	case KEY_ENTER:
	case KEY_KPENTER:
		if (pop_enabled(i))
			pop_pick(i);
		return 1;
	}
	return 0;
}

/* ---------------------------------------------------------------- dialogs */

static void draw_button(struct surface *s, int x, int y, int w, const char *label, int primary)
{
	int lw = strlen(label) * FONT_W;

	fill(s, x, y, w, 24, primary ? C_SEL : rgb(0xdd, 0xe1, 0xe6));
	frame(s, x, y, w, 24, primary ? rgb(0x2c, 0x5f, 0x94) : rgb(0xb4, 0xbb, 0xc4));
	text(s, x + (w - lw) / 2, y + 4, label, primary ? C_TEXT : C_WIN_FG, w - 4);
}

static int in_box(int x, int y, int bx, int by, int bw, int bh)
{
	return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

struct prompt {
	char label[96];
	char text[512];
	int len;
	void (*done)(void *, const char *);
	void *ctx;
};

static void prompt_draw(struct win *w)
{
	struct prompt *p = w->priv;
	int fw = w->w - 24, shown = fw / FONT_W - 2;
	const char *t = p->text + (p->len > shown ? p->len - shown : 0);

	fill(&w->surf, 0, 0, w->w, w->h, C_WIN_BG);
	text(&w->surf, 12, 12, p->label, C_WIN_FG, w->w - 24);
	fill(&w->surf, 12, 34, fw, FONT_H + 8, rgb(255, 255, 255));
	frame(&w->surf, 12, 34, fw, FONT_H + 8, C_SEL);
	int tw = text(&w->surf, 16, 38, t, C_WIN_FG, fw - 8);
	fill(&w->surf, 16 + tw, 38, 2, FONT_H, C_SEL);
	draw_button(&w->surf, w->w - 192, w->h - 34, 84, "OK", 1);
	draw_button(&w->surf, w->w - 96, w->h - 34, 84, "Cancel", 0);
}

static void prompt_finish(struct win *w, int ok)
{
	struct prompt *p = w->priv;

	w->dead = 1;
	if (ok && p->len)
		p->done(p->ctx, p->text);
}

static void prompt_key(struct win *w, const struct key *k)
{
	struct prompt *p = w->priv;

	if (k->code == KEY_ENTER || k->code == KEY_KPENTER) {
		prompt_finish(w, 1);
		return;
	}
	if (k->code == KEY_ESC) {
		prompt_finish(w, 0);
		return;
	}
	if (k->code == KEY_BACKSPACE) {
		if (p->len)
			p->text[--p->len] = 0;
	} else if ((k->mods & MOD_CTRL) && k->ch == 'u') {
		p->len = 0;
		p->text[0] = 0;
	} else if (k->ch >= 32 && k->ch < 127 && !(k->mods & MOD_CTRL) &&
		   p->len < (int)sizeof(p->text) - 1) {
		p->text[p->len++] = k->ch;
		p->text[p->len] = 0;
	}
	win_dirty(w);
}

static void prompt_mouse(struct win *w, int x, int y, int ev, int arg)
{
	(void)arg;
	if (ev != M_DOWN && ev != M_DOUBLE)
		return;
	if (in_box(x, y, w->w - 192, w->h - 34, 84, 24))
		prompt_finish(w, 1);
	else if (in_box(x, y, w->w - 96, w->h - 34, 84, 24))
		prompt_finish(w, 0);
}

static void prompt_close(struct win *w)
{
	free(w->priv);
}

static const struct app_ops prompt_ops = {
	.draw = prompt_draw, .key = prompt_key, .mouse = prompt_mouse, .close = prompt_close,
};

void open_prompt(const char *title, const char *label, const char *init,
		 void (*done)(void *, const char *), void *ctx)
{
	struct prompt *p = calloc(1, sizeof(*p));
	struct win *w;

	if (!p)
		return;
	snprintf(p->label, sizeof(p->label), "%s", label);
	snprintf(p->text, sizeof(p->text), "%s", init);
	p->len = strlen(p->text);
	p->done = done;
	p->ctx = ctx;
	w = win_new(title, 480, 116, &prompt_ops);
	if (!w) {
		free(p);
		return;
	}
	w->priv = p;
	w->fixed = 1;
	win_center(w);
}

struct message {
	char text[512];
	void (*yes)(void *);
	void *ctx;
};

static void message_draw(struct win *w)
{
	struct message *m = w->priv;
	int y = 14;

	fill(&w->surf, 0, 0, w->w, w->h, C_WIN_BG);
	/* one line per '\n' */
	for (const char *s = m->text; *s && y < w->h - 50; y += FONT_H + 2) {
		const char *nl = strchr(s, '\n');
		int n = nl ? nl - s : (int)strlen(s);
		char line[128];
		snprintf(line, sizeof(line), "%.*s", n < 127 ? n : 127, s);
		text(&w->surf, 14, y, line, C_WIN_FG, w->w - 28);
		s += n + !!nl;
	}
	if (m->yes) {
		draw_button(&w->surf, w->w - 192, w->h - 34, 84, "Yes", 1);
		draw_button(&w->surf, w->w - 96, w->h - 34, 84, "No", 0);
	} else {
		draw_button(&w->surf, w->w - 96, w->h - 34, 84, "OK", 1);
	}
}

static void message_finish(struct win *w, int yes)
{
	struct message *m = w->priv;

	w->dead = 1;
	if (yes && m->yes)
		m->yes(m->ctx);
}

static void message_key(struct win *w, const struct key *k)
{
	if (k->code == KEY_ENTER || k->code == KEY_KPENTER || k->ch == 'y')
		message_finish(w, 1);
	else if (k->code == KEY_ESC || k->ch == 'n')
		message_finish(w, 0);
}

static void message_mouse(struct win *w, int x, int y, int ev, int arg)
{
	struct message *m = w->priv;

	(void)arg;
	if (ev != M_DOWN && ev != M_DOUBLE)
		return;
	if (m->yes && in_box(x, y, w->w - 192, w->h - 34, 84, 24))
		message_finish(w, 1);
	else if (in_box(x, y, w->w - 96, w->h - 34, 84, 24))
		message_finish(w, 0);
}

static void message_close(struct win *w)
{
	free(w->priv);
}

static const struct app_ops message_ops = {
	.draw = message_draw, .key = message_key, .mouse = message_mouse, .close = message_close,
};

void open_message(const char *title, const char *msg, void (*yes)(void *), void *ctx)
{
	struct message *m = calloc(1, sizeof(*m));
	struct win *w;
	int lines = 1, width = 0, cur = 0;

	if (!m)
		return;
	snprintf(m->text, sizeof(m->text), "%s", msg);
	for (const char *s = m->text; *s; s++) {
		if (*s == '\n') {
			lines++;
			cur = 0;
		} else if (++cur > width) {
			width = cur;
		}
	}
	m->yes = yes;
	m->ctx = ctx;
	width = width * FONT_W + 28;
	w = win_new(title, width < 300 ? 300 : width > 640 ? 640 : width,
		    lines * (FONT_H + 2) + 70, &message_ops);
	if (!w) {
		free(m);
		return;
	}
	w->priv = m;
	w->fixed = 1;
	win_center(w);
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
	text(&w->surf, 16, 160, "Files in /root are saved in the browser.", C_DIM, w->w - 32);
}

static void about_tick(struct win *w)
{
	win_dirty(w);	/* the uptime */
}

static const struct app_ops about_ops = { .draw = about_draw, .tick = about_tick };

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

static void m_browser(void)
{
	static char *argv[] = { "/usr/bin/netsurf", NULL };

	if (access(argv[0], X_OK) == 0)
		run_fullscreen(argv);
	else
		open_message("Web browser", "The browser (NetSurf) is not installed.", NULL, NULL);
}

void browse(const char *url)
{
	static char u[600];
	static char *argv[] = { "/usr/bin/netsurf", u, NULL };

	char clean[512];
	int n = 0;

	/* URLs have no blanks; a bare host name gets http:// */
	for (; *url && n < (int)sizeof(clean) - 1; url++)
		if (*url != ' ' && *url != '\t')
			clean[n++] = *url;
	clean[n] = 0;
	if (!n)
		return;
	snprintf(u, sizeof(u), "%s%s",
		 strstr(clean, "://") || !strncmp(clean, "about:", 6) ? "" : "http://", clean);
	if (access(argv[0], X_OK) == 0)
		run_fullscreen(argv);
}

/* ---------------------------------------------------------------- start menu */

static void browse_cb(void *ctx, const char *url) { (void)ctx; browse(url); }
static void m_terminal(void) { open_terminal(NULL); }
static void m_files(void) { open_files("/root"); }
static void m_editor(void) { open_editor(NULL); }
static void m_open_url(void) { open_prompt("Open web page", "Address (http:// is added if missing):", "", browse_cb, NULL); }
static void m_paint(void) { open_paint(NULL); }
static void m_about(void)
{
	struct win *w = win_new("About WebLinux", 360, 190, &about_ops);
	if (w) {
		w->fixed = 1;
		w->tick_ms = 1000;
	}
}
static void m_console(void);
static void m_help(void) { open_editor("/usr/share/doc/weblinux.txt"); }

struct menu_item { const char *label; const uint16_t *icon; void (*fn)(void); };

static const struct menu_item menu[] = {
	{ "Terminal", icon_term, m_terminal },
	{ "Files", icon_folder, m_files },
	{ "Editor", icon_edit, m_editor },
	{ "Web browser", icon_globe, m_browser },
	{ "-", NULL, NULL },
	{ "Open web page...", icon_globe, m_open_url },
	{ "Paint", icon_paint, m_paint },
	{ "Calculator", icon_calc, open_calc },
	{ "Minesweeper", icon_mine, open_mines },
	{ "Snake", icon_snake, open_snake },
	{ "Task manager", icon_tasks, open_tasks },
	{ "-", NULL, NULL },
	{ "Settings", icon_gear, open_settings },
	{ "Help", icon_file, m_help },
	{ "About", icon_file, m_about },
	{ "Text console", icon_term, m_console },
};
#define NMENU (int)(sizeof(menu) / sizeof(menu[0]))

static void start_pick(void *ctx, int i)
{
	(void)ctx;
	if (menu[i].fn)
		menu[i].fn();
}

static void open_start_menu(void)
{
	const char *items[NMENU];
	const uint16_t *icons[NMENU];
	int h = 8;

	for (int i = 0; i < NMENU; i++) {
		items[i] = menu[i].label;
		icons[i] = menu[i].icon;
		h += strcmp(items[i], "-") ? POP_ROW : POP_SEP;
	}
	start_menu_open = 1;
	popup_open(4, screen.h - TASKBAR_H - h - 2, items, icons, NMENU, start_pick, NULL);
}

/* ---------------------------------------------------------------- desktop icons */

/* The launchers come first, then the files in ~/Desktop. */
#define NLAUNCH 4
#define ICON_W 76
#define ICON_H 72

struct dicon { char name[NAME_MAX + 1]; int dir; };
static struct dicon *dicons;
static int ndicons, icon_sel = -1;
static struct timespec desktop_mtime;

static int icon_count(void) { return NLAUNCH + ndicons; }

static struct rect icon_rect(int i)
{
	int per_col = (screen.h - TASKBAR_H - 16) / ICON_H;

	if (per_col < 1)
		per_col = 1;
	return (struct rect){ 12 + i / per_col * ICON_W, 12 + i % per_col * ICON_H, ICON_W, ICON_H - 8 };
}

static int dicon_cmp(const void *a, const void *b)
{
	const struct dicon *x = a, *y = b;

	if (x->dir != y->dir)
		return y->dir - x->dir;
	return strcasecmp(x->name, y->name);
}

static void desktop_load(void)
{
	DIR *d = opendir(DESKTOP_DIR);
	struct dirent *de;
	struct stat st;
	int cap = 0;

	free(dicons);
	dicons = NULL;
	ndicons = 0;
	if (stat(DESKTOP_DIR, &st) == 0)
		desktop_mtime = st.st_mtim;
	while (d && (de = readdir(d))) {
		char full[PATH_MAX];
		if (de->d_name[0] == '.')
			continue;
		if (ndicons == cap) {
			struct dicon *n = realloc(dicons, sizeof(*n) * (cap = cap ? cap * 2 : 16));
			if (!n)
				break;
			dicons = n;
		}
		snprintf(full, sizeof(full), DESKTOP_DIR "/%s", de->d_name);
		snprintf(dicons[ndicons].name, sizeof(dicons[0].name), "%s", de->d_name);
		dicons[ndicons].dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
		ndicons++;
	}
	if (d)
		closedir(d);
	qsort(dicons, ndicons, sizeof(*dicons), dicon_cmp);
	if (icon_sel >= icon_count())
		icon_sel = -1;
	need_redraw = 1;
}

static void desktop_check(void)
{
	struct stat st;

	if (stat(DESKTOP_DIR, &st) == 0 &&
	    (st.st_mtim.tv_sec != desktop_mtime.tv_sec || st.st_mtim.tv_nsec != desktop_mtime.tv_nsec))
		desktop_load();
}

static const char *icon_label(int i)
{
	return i < NLAUNCH ? menu[i].label : dicons[i - NLAUNCH].name;
}

static const uint16_t *icon_bits(int i)
{
	if (i < NLAUNCH)
		return menu[i].icon;
	if (dicons[i - NLAUNCH].dir)
		return icon_folder;
	return is_image(dicons[i - NLAUNCH].name) ? icon_image : icon_file;
}

static void icon_path(int i, char *buf, size_t n)
{
	snprintf(buf, n, DESKTOP_DIR "/%s", dicons[i - NLAUNCH].name);
}

static void icon_open(int i)
{
	char p[PATH_MAX];

	if (i < NLAUNCH) {
		menu[i].fn();
		return;
	}
	icon_path(i, p, sizeof(p));
	if (dicons[i - NLAUNCH].dir)
		open_files(p);
	else
		open_path(p);
}

static int icon_at(int x, int y)
{
	for (int i = 0; i < icon_count(); i++) {
		struct rect r = icon_rect(i);
		if (in_box(x, y, r.x, r.y, r.w, r.h))
			return i;
	}
	return -1;
}

static void draw_icons(void)
{
	for (int i = 0; i < icon_count(); i++) {
		struct rect r = icon_rect(i);
		const char *label = icon_label(i);
		char shown[16];
		int n = strlen(label);

		/* long names: the start and an ellipsis */
		if (n > 9)
			snprintf(shown, sizeof(shown), "%.7s..", label);
		else
			snprintf(shown, sizeof(shown), "%s", label);
		int lw = strlen(shown) * FONT_W;
		if (i == icon_sel)
			fill_alpha(&back, r.x, r.y, r.w, r.h, C_SEL, 120);
		uint32_t c = C_TEXT;
		if (i >= NLAUNCH && dicons[i - NLAUNCH].dir)
			c = rgb(0xf0, 0xc0, 0x50);
		bitmap(&back, r.x + (r.w - 16) / 2, r.y + 8, icon_bits(i), 16, 16, c);
		fill_alpha(&back, r.x + (r.w - lw) / 2 - 2, r.y + 32, lw + 4, FONT_H + 2, 0, 100);
		text(&back, r.x + (r.w - lw) / 2, r.y + 33, shown, C_TEXT, 0);
	}
}

static char menu_target[PATH_MAX];	/* the file a desktop pop-up is about */

static void desk_new_folder(void *ctx, const char *name)
{
	char p[PATH_MAX];

	(void)ctx;
	if (strchr(name, '/'))
		return;
	snprintf(p, sizeof(p), DESKTOP_DIR "/%s", name);
	if (mkdir(p, 0755) < 0)
		open_message("New folder", strerror(errno), NULL, NULL);
	sync();
	desktop_load();
}

static void desk_new_file(void *ctx, const char *name)
{
	char p[PATH_MAX];
	int fd;

	(void)ctx;
	if (strchr(name, '/'))
		return;
	snprintf(p, sizeof(p), DESKTOP_DIR "/%s", name);
	fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		open_message("New file", strerror(errno), NULL, NULL);
		return;
	}
	close(fd);
	sync();
	desktop_load();
	open_editor(p);
}

static void desk_rename(void *ctx, const char *name)
{
	char p[PATH_MAX];

	(void)ctx;
	if (strchr(name, '/'))
		return;
	snprintf(p, sizeof(p), DESKTOP_DIR "/%s", name);
	if (rename(menu_target, p) < 0)
		open_message("Rename", strerror(errno), NULL, NULL);
	sync();
	desktop_load();
}

static void desk_delete(void *ctx)
{
	char *argv[] = { "/bin/rm", "-rf", menu_target, NULL };

	(void)ctx;
	run_wait(argv);
	sync();
	desktop_load();
}

static void desk_bg_pick(void *ctx, int i)
{
	(void)ctx;
	switch (i) {
	case 0: open_prompt("New folder", "Folder name:", "New folder", desk_new_folder, NULL); break;
	case 1: open_prompt("New text file", "File name:", "notes.txt", desk_new_file, NULL); break;
	case 3: open_terminal_in(DESKTOP_DIR, NULL); break;
	case 4: open_files(DESKTOP_DIR); break;
	case 6: open_settings(); break;
	case 7: desktop_load(); break;
	}
}

static void desk_icon_pick(void *ctx, int i)
{
	int idx = (int)(long)ctx;
	const char *base;

	if (idx >= icon_count())
		return;
	switch (i) {
	case 0:
		icon_open(idx);
		break;
	case 1:
		if (idx < NLAUNCH)
			break;
		open_editor(menu_target);
		break;
	case 3:
		base = strrchr(menu_target, '/') + 1;
		open_prompt("Rename", "New name:", base, desk_rename, NULL);
		break;
	case 4: {
		char msg[PATH_MAX + 64];
		snprintf(msg, sizeof(msg), "Delete %s?", strrchr(menu_target, '/') + 1);
		open_message("Delete", msg, desk_delete, NULL);
		break;
	}
	}
}

static void desktop_right_click(int x, int y)
{
	static const char *const bg[] = {
		"New folder...", "New text file...", "-", "Open terminal here", "Open in Files",
		"-", "Change wallpaper...", "Refresh",
	};
	int i = icon_at(x, y);

	if (i < 0) {
		icon_sel = -1;
		popup_menu(x, y, bg, 8, desk_bg_pick, NULL);
		return;
	}
	icon_sel = i;
	if (i < NLAUNCH) {
		static const char *const launcher[] = { "Open" };
		popup_menu(x, y, launcher, 1, desk_icon_pick, (void *)(long)i);
		return;
	}
	icon_path(i, menu_target, sizeof(menu_target));
	const char *items[] = { "Open", dicons[i - NLAUNCH].dir ? "!Open in editor" : "Open in editor",
				"-", "Rename...", "Delete" };
	popup_menu(x, y, items, 5, desk_icon_pick, (void *)(long)i);
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
	int bx = w->x + w->w, by = w->y - TITLE_H;

	fill_alpha(&back, ox + 4, oy + 4, ow, oh, 0, 90);
	frame(&back, ox, oy, ow, oh, C_BORDER);
	fill(&back, w->x, by, w->w, TITLE_H, top ? C_TITLE : C_TITLE_OFF);
	text(&back, w->x + 8, by + 3, w->title, C_TEXT, w->w - 80);
	/* minimize, maximize, close */
	fill(&back, bx - 60, by + 14, 8, 2, C_TEXT);
	if (!w->fixed) {
		frame(&back, bx - 38, by + 6, 10, 10, C_TEXT);
		fill(&back, bx - 38, by + 6, 10, 2, C_TEXT);
	}
	for (int i = 0; i < 9; i++) {
		fill(&back, bx - 16 + i, by + 6 + i, 2, 1, C_TEXT);
		fill(&back, bx - 16 + 8 - i, by + 6 + i, 2, 1, C_TEXT);
	}
	if (w->dirty || w->drawn_focused != top) {
		w->dirty = 0;
		w->drawn_focused = top;
		w->ops->draw(w);
	}
	blit(&back, w->x, w->y, &w->surf);
	/* resize grip */
	if (!w->fixed && !w->maximized)
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
	draw_icons();
	for (int i = 0; i < nwins; i++)
		if (!wins[i]->minimized)
			draw_window(wins[i], wins[i] == top);

	/* taskbar */
	fill(&back, 0, screen.h - TASKBAR_H, screen.w, TASKBAR_H, C_BAR);
	fill(&back, 0, screen.h - TASKBAR_H, screen.w, 1, C_BAR_HI);
	fill(&back, 4, screen.h - TASKBAR_H + 4, 80, TASKBAR_H - 8, start_menu_open ? C_SEL : C_BAR_HI);
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

	if (pop.open) {
		int i = pop_at(mx, my);
		if (i >= 0)
			pop.hover = i;
		draw_popup();
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

/* part: 0 content, 1 title, 2 close, 3 minimize, 4 resize grip, 5 maximize */
static struct win *win_at(int x, int y, int *part)
{
	for (int i = nwins - 1; i >= 0; i--) {
		struct win *w = wins[i];
		if (w->minimized)
			continue;
		if (x >= w->x - 1 && x < w->x + w->w + 1 && y >= w->y - TITLE_H - 1 && y < w->y + w->h + 1) {
			int rx = w->x + w->w - x;
			if (y < w->y)
				*part = rx <= 22 ? 2 : rx <= 44 ? (w->fixed ? 1 : 5) : rx <= 66 ? 3 : 1;
			else if (!w->fixed && !w->maximized && rx <= CORNER && y >= w->y + w->h - CORNER)
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
	if (pop.open && popup_key(code))
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
		if (pop.open)
			popup_close();
		else
			open_start_menu();
		return;
	}
	if (pop.open)
		popup_close();
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

static void focus_win(struct win *w)
{
	if (w != focus()) {
		struct win *old = focus();
		raise_win(w);
		if (old)
			old->dirty = 1;
		w->dirty = 1;
	}
}

static void win_menu_pick(void *ctx, int i)
{
	struct win *w = ctx;

	if (!win_alive(w))
		return;
	switch (i) {
	case 0:
		if (w->minimized)
			raise_win(w);
		else
			w->minimized = 1;
		break;
	case 1:
		focus_win(w);
		win_toggle_max(w);
		break;
	case 3:
		w->dead = 1;
		break;
	}
	need_redraw = 1;
}

static void win_menu(struct win *w, int x, int y)
{
	const char *items[] = {
		w->minimized ? "Restore" : "Minimize",
		w->fixed ? "!Maximize" : w->maximized ? "Unmaximize" : "Maximize",
		"-", "Close",
	};
	popup_menu(x, y, items, 4, win_menu_pick, w);
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

	if (pop.open) {
		int i = pop_at(x, y);
		int was_start = start_menu_open;
		if (i >= 0 && y >= pop.y && y < pop.y + pop.h) {
			if (pop_enabled(i))
				pop_pick(i);
			return;
		}
		popup_close();
		/* a click on the menu button closes the start menu */
		if (was_start && y >= screen.h - TASKBAR_H && x < 86)
			return;
		if (y < screen.h - TASKBAR_H)
			return;
	}
	if (y >= screen.h - TASKBAR_H) {
		if (x < 86) {
			open_start_menu();
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
		int i = icon_at(x, y);
		if (i != icon_sel) {
			icon_sel = i;
			need_redraw = 1;
		}
		if (dbl && i >= 0)
			icon_open(i);
		return;
	}
	focus_win(w);
	switch (part) {
	case 1:
		if (dbl && !w->fixed) {
			win_toggle_max(w);
			break;
		}
		if (w->maximized)
			break;
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
	case 5:
		win_toggle_max(w);
		break;
	default:
		if (w->ops->mouse) {
			mouse_capture = w;
			w->ops->mouse(w, x - w->x, y - w->y, dbl ? M_DOUBLE : M_DOWN, 1);
		}
	}
}

static void right_click(int x, int y)
{
	int part;
	struct win *w;

	if (pop.open) {
		popup_close();
		return;
	}
	if (y >= screen.h - TASKBAR_H) {
		if (x >= 90 && nwins) {
			int i = (x - 90) / task_button_w();
			if (i >= 0 && i < nwins)
				win_menu(wins[i], x, y);
		} else if (x < 86) {
			open_start_menu();
		}
		return;
	}
	w = win_at(x, y, &part);
	if (!w) {
		desktop_right_click(x, y);
		return;
	}
	focus_win(w);
	if (part == 0 && w->ops->mouse)
		w->ops->mouse(w, x - w->x, y - w->y, M_RIGHT, 0);
	else if (part != 0)
		win_menu(w, x, y);
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
		} else if (ev->code == BTN_RIGHT) {
			if (ev->value)
				right_click(mx, my);
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

/* ---------------------------------------------------------------- settings */

#define SETTINGS "/root/.config/wldesk"

static void load_settings(void)
{
	FILE *fp = fopen(SETTINGS, "r");
	unsigned a, b;

	if (!fp)
		return;
	if (fscanf(fp, "wallpaper %x %x", &a, &b) == 2) {
		desk_top = a & 0xffffff;
		desk_bot = b & 0xffffff;
	}
	fclose(fp);
}

void save_settings(void)
{
	FILE *fp;

	mkdir("/root/.config", 0755);
	fp = fopen(SETTINGS, "w");
	if (!fp)
		return;
	fprintf(fp, "wallpaper %06x %06x\n", desk_top, desk_bot);
	fclose(fp);
	sync();
}

void make_wallpaper(void)
{
	if (!wallpaper.px)
		surface_init(&wallpaper, screen.w, screen.h);
	gradient(&wallpaper, 0, 0, screen.w, screen.h, desk_top, desk_bot);
	/* a faint grid of dots */
	for (int y = 24; y < screen.h; y += 32)
		for (int x = 24; x < screen.w; x += 32)
			fill_alpha(&wallpaper, x, y, 2, 2, rgb(255, 255, 255), 18);
	const char *brand = "WebLinux";
	int w = strlen(brand) * FONT_W;
	text(&wallpaper, screen.w - w - 24, screen.h - TASKBAR_H - 36, brand, rgb(0x6d, 0x8c, 0xad), 0);
	need_redraw = 1;
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
		term_reaped(pid);
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

/* the first start on a new disk: a desktop folder with a note in it */
static void first_run(void)
{
	FILE *fp;

	if (mkdir(DESKTOP_DIR, 0755) < 0)
		return;
	fp = fopen(DESKTOP_DIR "/Welcome.txt", "w");
	if (!fp)
		return;
	fputs("Welcome to WebLinux!\n"
	      "\n"
	      "This is Linux compiled to WebAssembly, running in your browser tab.\n"
	      "\n"
	      "- Files in /root (this desktop is /root/Desktop) are saved in the\n"
	      "  browser and come back when you open the page again.\n"
	      "- Right-click the desktop, files, windows and taskbar buttons for menus.\n"
	      "- In the file manager: F2 renames, Delete deletes, Ctrl+C/X/V copy,\n"
	      "  cut and paste files, Ctrl+H shows hidden files.\n"
	      "- Double-click a title bar to maximize a window. Alt+F4 closes it,\n"
	      "  Alt+Tab switches, Ctrl+Alt+T opens a terminal.\n"
	      "- The Menu button has a web browser, Paint, games and more.\n", fp);
	fclose(fp);
	sync();
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
	load_settings();
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
	first_run();
	desktop_load();

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
		long long now = now_ms();

		if (got_child)
			reap();
		for (int i = 0; i < nwins; i++)
			if (wins[i]->dead)
				win_close(wins[i--]);
		for (int i = 0; i < nwins; i++) {
			struct win *w = wins[i];
			if (w->tick_ms <= 0 || !w->ops->tick || fullscreen_pid)
				continue;
			if (now >= w->next_tick) {
				w->next_tick = now + w->tick_ms;
				if (!w->minimized)
					w->ops->tick(w);
			}
			if (w->next_tick - now < timeout)
				timeout = w->next_tick - now;
		}
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
		if (timeout < 0)
			timeout = 0;
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
			time_t t = time(NULL);
			if (localtime(&t)->tm_min != last_clock)
				need_redraw = 1;
			desktop_check();
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
