/* The text editor. */
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

/* ---------------------------------------------------------------- editor */

struct editor {
	char path[PATH_MAX];
	char **line;
	int n, cap;
	int cx, cy, top, left;
	int modified;
	int sel, ax, ay;		/* selection from the anchor to the cursor */
	char msg[80];
};


static void ed_sel_range(const struct editor *e, int *x0, int *y0, int *x1, int *y1)
{
	if (e->ay < e->cy || (e->ay == e->cy && e->ax <= e->cx)) {
		*x0 = e->ax, *y0 = e->ay, *x1 = e->cx, *y1 = e->cy;
	} else {
		*x0 = e->cx, *y0 = e->cy, *x1 = e->ax, *y1 = e->ay;
	}
}

static int ed_selected(const struct editor *e, int x, int y)
{
	int x0, y0, x1, y1;

	if (!e->sel)
		return 0;
	ed_sel_range(e, &x0, &y0, &x1, &y1);
	if (y < y0 || y > y1)
		return 0;
	return !(y == y0 && x < x0) && !(y == y1 && x >= x1);
}

static char *ed_sel_text(const struct editor *e)
{
	int x0, y0, x1, y1;
	size_t len = 0, o = 0;
	char *t;

	ed_sel_range(e, &x0, &y0, &x1, &y1);
	for (int y = y0; y <= y1; y++)
		len += strlen(e->line[y]) + 1;
	t = malloc(len + 1);
	if (!t)
		return NULL;
	for (int y = y0; y <= y1; y++) {
		const char *l = e->line[y];
		int a = y == y0 ? x0 : 0, b = y == y1 ? x1 : (int)strlen(l);
		memcpy(t + o, l + a, b - a);
		o += b - a;
		if (y < y1)
			t[o++] = '\n';
	}
	t[o] = 0;
	return t;
}

static void ed_delete_sel(struct editor *e)
{
	int x0, y0, x1, y1;

	if (!e->sel)
		return;
	ed_sel_range(e, &x0, &y0, &x1, &y1);
	char *first = e->line[y0], *last = e->line[y1];
	char *j = malloc(x0 + strlen(last + x1) + 1);
	if (!j)
		return;
	memcpy(j, first, x0);
	strcpy(j + x0, last + x1);
	for (int y = y0; y <= y1; y++)
		free(e->line[y]);
	e->line[y0] = j;
	memmove(&e->line[y0 + 1], &e->line[y1 + 1], sizeof(*e->line) * (e->n - y1 - 1));
	e->n -= y1 - y0;
	e->cx = x0;
	e->cy = y0;
	e->sel = 0;
	e->modified = 1;
}

static void ed_insert_line(struct editor *e, int at, const char *s, int len);

/* insert text with newlines at the cursor */
static void ed_insert_text(struct editor *e, const char *t)
{
	while (*t) {
		const char *nl = strchr(t, '\n');
		int n = nl ? nl - t : (int)strlen(t);
		char *l = e->line[e->cy];
		int len = strlen(l);
		char *j = realloc(l, len + n + 1);
		if (!j)
			return;
		memmove(j + e->cx + n, j + e->cx, len - e->cx + 1);
		memcpy(j + e->cx, t, n);
		e->line[e->cy] = j;
		e->cx += n;
		t += n;
		if (*t == '\n') {
			ed_insert_line(e, e->cy + 1, j + e->cx, strlen(j + e->cx));
			e->line[e->cy][e->cx] = 0;
			e->cy++;
			e->cx = 0;
			t++;
		}
	}
	e->modified = 1;
}

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

static void ed_save_as(struct win *w);

static void ed_save(struct win *w)
{
	struct editor *e = w->priv;
	FILE *fp;

	if (!e->path[0]) {
		ed_save_as(w);
		return;
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

static void ed_save_as_done(void *ctx, const char *name)
{
	struct win *w = ctx;
	struct editor *e;

	if (!win_alive(w))
		return;
	e = w->priv;
	/* a bare name goes to the home folder */
	snprintf(e->path, sizeof(e->path), "%s%s", name[0] == '/' ? "" : "/root/", name);
	ed_save(w);
	win_dirty(w);
}

static void ed_save_as(struct win *w)
{
	struct editor *e = w->priv;

	open_prompt("Save as", "Save to (a path, or a name in /root):",
		    e->path[0] ? e->path : "/root/Desktop/untitled.txt", ed_save_as_done, w);
}

static void ed_open_done(void *ctx, const char *name)
{
	char p[PATH_MAX];

	(void)ctx;
	snprintf(p, sizeof(p), "%s%s", name[0] == '/' ? "" : "/root/", name);
	open_editor(p);
}

static void ed_copy(struct editor *e, int cut)
{
	char *t;

	if (!e->sel)
		return;
	t = ed_sel_text(e);
	if (t) {
		set_clip(t, strlen(t));
		free(t);
	}
	if (cut)
		ed_delete_sel(e);
}

static void ed_paste(struct editor *e)
{
	if (!clip)
		return;
	ed_delete_sel(e);
	ed_insert_text(e, clip);
}

static void ed_select_all(struct editor *e)
{
	e->ax = e->ay = 0;
	e->cy = e->n - 1;
	e->cx = strlen(e->line[e->cy]);
	e->sel = 1;
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
		for (int c = 0; c < cols && e->left + c <= len; c++) {
			int on = ed_selected(e, e->left + c, e->top + i);
			if (e->left + c == len) {	/* the line break */
				if (on && e->top + i < e->n - 1)
					fill(&w->surf, 4 + c * FONT_W, 2 + i * FONT_H, FONT_W / 2, FONT_H, C_SEL);
				break;
			}
			glyph(&w->surf, 4 + c * FONT_W, 2 + i * FONT_H, (unsigned char)l[e->left + c],
			      on ? rgb(255, 255, 255) : C_WIN_FG, on ? C_SEL : TRANSPARENT);
		}
	}
	if (focus() == w)
		fill(&w->surf, 4 + (e->cx - e->left) * FONT_W, 2 + (e->cy - e->top) * FONT_H,
		     2, FONT_H, C_SEL);
	fill(&w->surf, 0, w->h - ROW_H, w->w, ROW_H, rgb(0xe2, 0xe6, 0xea));
	snprintf(status, sizeof(status), "Ln %d, Col %d   Ctrl+S save, Ctrl+O open, right-click for more   %s",
		 e->cy + 1, e->cx + 1, e->msg);
	text(&w->surf, 6, w->h - ROW_H + 1, status, C_WIN_FG, w->w - 12);
}

static void ed_key(struct win *w, const struct key *k)
{
	struct editor *e = w->priv;
	int rows = (w->h - ROW_H - 4) / FONT_H;
	char *l = e->line[e->cy];
	int len = strlen(l);

	e->msg[0] = 0;
	if ((k->mods & MOD_CTRL) && k->code == KEY_S) {
		if (k->mods & MOD_SHIFT)
			ed_save_as(w);
		else
			ed_save(w);
		goto out;
	}
	if ((k->mods & MOD_CTRL) && k->code == KEY_O) {
		open_prompt("Open file", "File to open (a path, or a name in /root):", "/root/",
			    ed_open_done, NULL);
		goto out;
	}
	if ((k->mods & MOD_CTRL) && (k->code == KEY_C || k->code == KEY_X)) {
		ed_copy(e, k->code == KEY_X);
		goto out;
	}
	if ((k->mods & MOD_CTRL) && k->code == KEY_V) {
		ed_paste(e);
		goto out;
	}
	if ((k->mods & MOD_CTRL) && k->code == KEY_A) {
		ed_select_all(e);
		goto out;
	}
	{
		int moving = k->code == KEY_LEFT || k->code == KEY_RIGHT || k->code == KEY_UP ||
			     k->code == KEY_DOWN || k->code == KEY_HOME || k->code == KEY_END ||
			     k->code == KEY_PAGEUP || k->code == KEY_PAGEDOWN;
		if (moving && (k->mods & MOD_SHIFT)) {
			if (!e->sel) {
				e->sel = 1;
				e->ax = e->cx;
				e->ay = e->cy;
			}
		} else if (moving) {
			e->sel = 0;
		} else if (e->sel && (k->code == KEY_BACKSPACE || k->code == KEY_DELETE)) {
			ed_delete_sel(e);
			goto out;
		} else if (e->sel && (k->code == KEY_ENTER || k->code == KEY_TAB ||
				      (k->ch >= 32 && k->ch < 127 && !(k->mods & (MOD_CTRL | MOD_ALT))))) {
			ed_delete_sel(e);
			l = e->line[e->cy];
			len = strlen(l);
		}
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
	case KEY_HOME:
		if (k->mods & MOD_CTRL)
			e->cy = 0;
		e->cx = 0;
		goto out;
	case KEY_END:
		if (k->mods & MOD_CTRL)
			e->cy = e->n - 1;
		e->cx = strlen(e->line[e->cy]);
		goto out;
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

static void ed_menu_pick(void *ctx, int i)
{
	struct win *w = ctx;
	struct editor *e;

	if (!win_alive(w))
		return;
	e = w->priv;
	e->msg[0] = 0;
	switch (i) {
	case 0: ed_copy(e, 1); break;
	case 1: ed_copy(e, 0); break;
	case 2: ed_paste(e); break;
	case 3: ed_select_all(e); break;
	case 5: ed_save(w); break;
	case 6: ed_save_as(w); break;
	}
	ed_title(w);
	win_dirty(w);
}

static void ed_mouse(struct win *w, int x, int y, int ev, int arg)
{
	struct editor *e = w->priv;

	if (ev == M_RIGHT) {
		const char *items[] = {
			e->sel ? "Cut          Ctrl+X" : "!Cut          Ctrl+X",
			e->sel ? "Copy         Ctrl+C" : "!Copy         Ctrl+C",
			clip ? "Paste        Ctrl+V" : "!Paste        Ctrl+V",
			"Select all   Ctrl+A", "-",
			"Save         Ctrl+S", "Save as...",
		};
		popup_menu(w->x + x, w->y + y, items, 7, ed_menu_pick, w);
		return;
	}
	if (ev == M_WHEEL) {
		e->cy -= arg * 3;
		if (e->cy < 0)
			e->cy = 0;
		if (e->cy >= e->n)
			e->cy = e->n - 1;
	} else if (ev == M_DOWN || ev == M_DOUBLE || ev == M_MOVE) {
		int cy = e->top + (y - 2) / FONT_H, cx = e->left + (x - 4 + FONT_W / 2) / FONT_W;
		if (cy >= e->n)
			cy = e->n - 1;
		e->cy = cy < 0 ? 0 : cy;
		int len = strlen(e->line[e->cy]);
		e->cx = cx < 0 ? 0 : cx > len ? len : cx;
		if (ev == M_MOVE) {
			e->sel = e->cx != e->ax || e->cy != e->ay;
		} else {
			e->sel = 0;
			e->ax = e->cx;
			e->ay = e->cy;
		}
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

void open_editor(const char *path)
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

