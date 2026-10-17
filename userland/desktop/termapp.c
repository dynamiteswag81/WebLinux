/* The terminal application: a pty running bash, drawn by term.c. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "desk.h"
#include "term.h"

/* ---------------------------------------------------------------- terminal */

struct termapp {
	struct term t;
	int master;
	pid_t pid;
	int sel, sx0, sy0, sx1, sy1;	/* selection, in view cells */
	struct termapp *next;
};

static struct termapp *terms;	/* all terminals, for term_reaped() */
char *clip;

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

void set_clip(const char *s, int n)
{
	char *c = malloc(n + 1);

	if (!c)
		return;
	memcpy(c, s, n);
	c[n] = 0;
	free(clip);
	clip = c;
	host_clipboard(s, n);
}

void term_reaped(pid_t pid)
{
	for (struct termapp *a = terms; a; a = a->next)
		if (a->pid == pid)
			a->pid = 0;
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
		set_clip(buf, n);
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
	for (struct termapp **p = &terms; *p; p = &(*p)->next) {
		if (*p == a) {
			*p = a->next;
			break;
		}
	}
	close(a->master);
	term_free(&a->t);
	free(a);
}

static void term_menu_pick(void *ctx, int i)
{
	struct win *w = ctx;
	struct termapp *a;

	if (!win_alive(w))
		return;
	a = w->priv;
	switch (i) {
	case 0: term_copy(a); break;
	case 1: if (clip) term_send(a, clip, strlen(clip)); break;
	case 3: open_terminal(NULL); break;
	case 4:
		term_write(&a->t, (const unsigned char *)"\033c", 2);
		term_send(a, "\f", 1);	/* Ctrl+L: the shell redraws its prompt */
		break;
	}
	win_dirty(w);
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
	case M_RIGHT: {
		const char *items[] = { a->sel ? "Copy" : "!Copy", clip ? "Paste" : "!Paste", "-",
					"New terminal", "Reset" };
		popup_menu(w->x + x, w->y + y, items, 5, term_menu_pick, w);
		return;
	}
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

char *child_env[] = {
	"TERM=xterm", "HOME=/root", "SHELL=/bin/bash", "USER=root", "LOGNAME=root",
	"PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", NULL, NULL
};

void open_terminal(const char *cmd)
{
	open_terminal_in(NULL, cmd);
}

void open_terminal_in(const char *dir, const char *cmd)
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
	a->next = terms;
	terms = a;
	a->pid = vfork();
	if (a->pid == 0) {
		setsid();
		if (dir && chdir(dir) < 0)
			_exit(126);
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

