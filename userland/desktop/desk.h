/* Shared by the window manager (desk.c) and the applications. */
#ifndef DESK_H
#define DESK_H

#include <sys/types.h>

#include "gfx.h"

#define TITLE_H		22
#define TASKBAR_H	30
#define ROW_H		18

#define C_DESK_TOP	RGB(0x1d, 0x3b, 0x5c)
#define C_DESK_BOT	RGB(0x0b, 0x14, 0x22)
#define C_BAR		RGB(0x16, 0x1b, 0x22)
#define C_BAR_HI	RGB(0x2a, 0x33, 0x40)
#define C_TITLE		RGB(0x2b, 0x5d, 0x8c)
#define C_TITLE_OFF	RGB(0x3a, 0x40, 0x48)
#define C_TEXT		RGB(0xe8, 0xec, 0xf0)
#define C_DIM		RGB(0x8a, 0x94, 0xa0)
#define C_WIN_BG	RGB(0xf4, 0xf5, 0xf7)
#define C_WIN_FG	RGB(0x1b, 0x1f, 0x24)
#define C_SEL		RGB(0x3d, 0x7e, 0xc0)
#define C_BORDER	RGB(0x10, 0x14, 0x1a)
#define C_ACCENT	RGB(0x4f, 0xc3, 0xf7)
#define C_PANEL		RGB(0xe2, 0xe6, 0xea)

#define MOD_SHIFT	1
#define MOD_CTRL	2
#define MOD_ALT		4

/* mouse events for app_ops.mouse; M_RIGHT is a right button press */
enum { M_DOWN, M_UP, M_MOVE, M_WHEEL, M_DOUBLE, M_RIGHT };

struct key { int code, ch, mods; };
struct win;

struct app_ops {
	void (*draw)(struct win *);
	void (*key)(struct win *, const struct key *);
	void (*mouse)(struct win *, int x, int y, int ev, int arg);
	void (*io)(struct win *);
	void (*resized)(struct win *);
	void (*close)(struct win *);
	void (*tick)(struct win *);	/* every tick_ms while tick_ms > 0 */
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
	int maximized, rx, ry, rw, rh;	/* rx..rh: the size before maximizing */
	int fixed;			/* no resizing */
	int tick_ms;
	long long next_tick;
};

extern struct surface screen;
extern int need_redraw;
extern char *child_env[];

long long now_ms(void);
struct win *focus(void);
struct win *win_new(const char *title, int w, int h, const struct app_ops *ops);
void win_resize(struct win *w, int nw, int nh);
void win_center(struct win *w);
static inline void win_dirty(struct win *w) { w->dirty = need_redraw = 1; }

/*
 * A pop-up menu at (x, y) on the screen.  Items are labels; "-" is a
 * separator and a label starting with '!' is greyed out.  fn(ctx, i) runs
 * when item i is picked.
 */
void popup_menu(int x, int y, const char *const *items, int n,
		void (*fn)(void *ctx, int i), void *ctx);

/* one-line text prompt; done(ctx, text) on Enter */
void open_prompt(const char *title, const char *label, const char *init,
		 void (*done)(void *ctx, const char *text), void *ctx);
/* a message with OK, or with Yes/No when yes is set (called on Yes) */
void open_message(const char *title, const char *msg,
		  void (*yes)(void *ctx), void *ctx);

int win_alive(const struct win *w);

/* the desktop's text clipboard; also handed to the host (OSC 52) */
extern char *clip;
void set_clip(const char *s, int n);

void term_reaped(pid_t pid);
void open_terminal(const char *cmd);
void open_terminal_in(const char *dir, const char *cmd);
void open_editor(const char *path);
void open_files(const char *path);
void open_path(const char *path);
void browse(const char *url);
/* run argv to completion; returns the exit status, or -1 */
int run_wait(char *const argv[]);

/* apps.c */
void open_calc(void);
void open_mines(void);
void open_snake(void);
void open_tasks(void);
void open_paint(const char *path);
void open_settings(void);
int is_image(const char *path);

/* settings shared with apps.c */
extern uint32_t desk_top, desk_bot;
void make_wallpaper(void);
void save_settings(void);

extern const uint16_t icon_term[16], icon_folder[16], icon_file[16], icon_globe[16],
	icon_edit[16], icon_calc[16], icon_mine[16], icon_snake[16], icon_tasks[16],
	icon_paint[16], icon_gear[16], icon_image[16];

#endif
