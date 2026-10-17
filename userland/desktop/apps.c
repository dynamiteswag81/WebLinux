/*
 * Small desktop applications: calculator, Minesweeper, Snake, task
 * manager, Paint (24-bit BMP files) and settings.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#include "desk.h"

static int in_box(int x, int y, int bx, int by, int bw, int bh)
{
	return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

static void center_text(struct surface *s, int x, int y, int w, const char *t, uint32_t c)
{
	text(s, x + (w - (int)strlen(t) * FONT_W) / 2, y, t, c, w);
}

static void button(struct surface *s, int x, int y, int w, int h, const char *label,
		   uint32_t bg, uint32_t fg)
{
	fill(s, x, y, w, h, bg);
	frame(s, x, y, w, h, rgb(0xb4, 0xbb, 0xc4));
	center_text(s, x, y + (h - FONT_H) / 2, w, label, fg);
}

/* ---------------------------------------------------------------- calculator */

struct calc {
	char disp[32];
	double acc;
	char op;		/* pending operator, 0 if none */
	int fresh;		/* the next digit starts a new number */
};

static const char *const calc_keys[5][4] = {
	{ "C", "+/-", "%", "/" },
	{ "7", "8", "9", "*" },
	{ "4", "5", "6", "-" },
	{ "1", "2", "3", "+" },
	{ "0", ".", "<-", "=" },
};
#define CALC_BW 64
#define CALC_BH 44
#define CALC_TOP 64

static void calc_show(struct calc *c, double v)
{
	if (isnan(v) || isinf(v)) {
		snprintf(c->disp, sizeof(c->disp), "Error");
		return;
	}
	snprintf(c->disp, sizeof(c->disp), "%.12g", v);
}

static double calc_apply(double a, char op, double b)
{
	switch (op) {
	case '+': return a + b;
	case '-': return a - b;
	case '*': return a * b;
	case '/': return a / b;
	}
	return b;
}

static void calc_press(struct calc *c, const char *k)
{
	double v = strtod(c->disp, NULL);

	if ((k[0] >= '0' && k[0] <= '9') || !strcmp(k, ".")) {
		if (c->fresh || !strcmp(c->disp, "0") || !strcmp(c->disp, "Error"))
			snprintf(c->disp, sizeof(c->disp), "%s", k[0] == '.' ? "0" : "");
		if (k[0] == '.' && strchr(c->disp, '.'))
			return;
		if (strlen(c->disp) < 16)
			strcat(c->disp, k);
		c->fresh = 0;
	} else if (!strcmp(k, "C")) {
		strcpy(c->disp, "0");
		c->acc = 0;
		c->op = 0;
		c->fresh = 1;
	} else if (!strcmp(k, "<-")) {
		size_t n = strlen(c->disp);
		if (c->fresh || n <= 1 || (n == 2 && c->disp[0] == '-'))
			strcpy(c->disp, "0");
		else
			c->disp[n - 1] = 0;
	} else if (!strcmp(k, "+/-")) {
		calc_show(c, -v);
	} else if (!strcmp(k, "%")) {
		calc_show(c, c->op ? c->acc * v / 100 : v / 100);
		c->fresh = 1;
	} else {	/* an operator or = */
		if (c->op && !c->fresh)
			v = calc_apply(c->acc, c->op, v);
		calc_show(c, v);
		c->acc = v;
		c->op = k[0] == '=' ? 0 : k[0];
		c->fresh = 1;
	}
}

static void calc_draw(struct win *w)
{
	struct calc *c = w->priv;

	fill(&w->surf, 0, 0, w->w, w->h, rgb(0x26, 0x2b, 0x33));
	fill(&w->surf, 8, 8, w->w - 16, 48, rgb(0x16, 0x1a, 0x20));
	if (c->op) {
		char o[2] = { c->op, 0 };
		text(&w->surf, 14, 12, o, C_ACCENT, 0);
	}
	/* the number, right aligned and twice the size */
	int n = strlen(c->disp), x = w->w - 16 - n * 16;
	for (int i = 0; i < n; i++) {
		struct surface g;
		if (surface_init(&g, FONT_W, FONT_H))
			break;
		fill(&g, 0, 0, FONT_W, FONT_H, rgb(0x16, 0x1a, 0x20));
		glyph(&g, 0, 0, c->disp[i], C_TEXT, TRANSPARENT);
		for (int y = 0; y < FONT_H * 2; y++)
			for (int xx = 0; xx < FONT_W * 2; xx++)
				fill(&w->surf, x + i * 16 + xx, 18 + y - 4, 1, 1, g.px[(y / 2) * g.stride + xx / 2]);
		surface_free(&g);
	}
	for (int r = 0; r < 5; r++) {
		for (int col = 0; col < 4; col++) {
			const char *k = calc_keys[r][col];
			uint32_t bg = col == 3 ? rgb(0x3d, 0x7e, 0xc0) : r == 0 ? rgb(0x4a, 0x52, 0x5e) :
				      rgb(0x37, 0x3e, 0x48);
			button(&w->surf, 8 + col * (CALC_BW + 4), CALC_TOP + r * (CALC_BH + 4), CALC_BW, CALC_BH,
			       k, bg, C_TEXT);
		}
	}
}

static void calc_mouse(struct win *w, int x, int y, int ev, int arg)
{
	(void)arg;
	if (ev != M_DOWN && ev != M_DOUBLE)
		return;
	int col = (x - 8) / (CALC_BW + 4), r = (y - CALC_TOP) / (CALC_BH + 4);
	if (x < 8 || y < CALC_TOP || col > 3 || r > 4)
		return;
	calc_press(w->priv, calc_keys[r][col]);
	win_dirty(w);
}

static void calc_key(struct win *w, const struct key *k)
{
	char s[2] = { (char)k->ch, 0 };

	if (k->code == KEY_ENTER || k->code == KEY_KPENTER || k->ch == '=')
		calc_press(w->priv, "=");
	else if (k->code == KEY_BACKSPACE)
		calc_press(w->priv, "<-");
	else if (k->code == KEY_ESC || k->ch == 'c' || k->ch == 'C')
		calc_press(w->priv, "C");
	else if (k->ch && strchr("0123456789.+-*/%", k->ch))
		calc_press(w->priv, s);
	else
		return;
	win_dirty(w);
}

static void calc_close(struct win *w)
{
	free(w->priv);
}

static const struct app_ops calc_ops = {
	.draw = calc_draw, .mouse = calc_mouse, .key = calc_key, .close = calc_close,
};

void open_calc(void)
{
	struct calc *c = calloc(1, sizeof(*c));
	struct win *w;

	if (!c)
		return;
	strcpy(c->disp, "0");
	c->fresh = 1;
	w = win_new("Calculator", 8 + 4 * (CALC_BW + 4) + 4, CALC_TOP + 5 * (CALC_BH + 4) + 4, &calc_ops);
	if (!w) {
		free(c);
		return;
	}
	w->priv = c;
	w->fixed = 1;
}

/* ---------------------------------------------------------------- minesweeper */

#define MW 16
#define MH 16
#define MINES 40
#define CELL 22
#define MTOP 40

struct mines {
	unsigned char mine[MH][MW], open[MH][MW], flag[MH][MW];
	int started, over, won, flags, opened;
	long long t0, t1;
};

static int mines_count(struct mines *m, int x, int y)
{
	int n = 0;

	for (int dy = -1; dy <= 1; dy++)
		for (int dx = -1; dx <= 1; dx++)
			if (x + dx >= 0 && x + dx < MW && y + dy >= 0 && y + dy < MH)
				n += m->mine[y + dy][x + dx];
	return n;
}

static void mines_new(struct mines *m)
{
	memset(m, 0, sizeof(*m));
}

/* the first click is never a mine: place them afterwards */
static void mines_place(struct mines *m, int sx, int sy)
{
	for (int placed = 0; placed < MINES;) {
		int x = rand() % MW, y = rand() % MH;
		if (m->mine[y][x] || (abs(x - sx) <= 1 && abs(y - sy) <= 1))
			continue;
		m->mine[y][x] = 1;
		placed++;
	}
	m->started = 1;
	m->t0 = now_ms();
}

static void mines_open(struct mines *m, int x, int y)
{
	if (x < 0 || x >= MW || y < 0 || y >= MH || m->open[y][x] || m->flag[y][x])
		return;
	m->open[y][x] = 1;
	m->opened++;
	if (m->mine[y][x]) {
		m->over = 1;
		m->t1 = now_ms();
		return;
	}
	if (!mines_count(m, x, y))
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++)
				mines_open(m, x + dx, y + dy);
	if (!m->over && m->opened == MW * MH - MINES) {
		m->over = m->won = 1;
		m->t1 = now_ms();
	}
}

static void mines_draw(struct win *w)
{
	struct mines *m = w->priv;
	static const uint32_t numc[9] = {
		0, RGB(0x1e, 0x5f, 0xd0), RGB(0x2e, 0x8b, 0x3a), RGB(0xd0, 0x30, 0x30), RGB(0x30, 0x20, 0x90),
		RGB(0x90, 0x20, 0x20), RGB(0x20, 0x88, 0x88), RGB(0, 0, 0), RGB(0x66, 0x66, 0x66),
	};
	char s[64];
	int secs = m->started ? (int)(((m->over ? m->t1 : now_ms()) - m->t0) / 1000) : 0;

	fill(&w->surf, 0, 0, w->w, w->h, rgb(0xc8, 0xcd, 0xd3));
	snprintf(s, sizeof(s), "Mines: %d", MINES - m->flags);
	text(&w->surf, 10, 12, s, C_WIN_FG, 0);
	snprintf(s, sizeof(s), "Time: %d", secs);
	text(&w->surf, w->w - 90, 12, s, C_WIN_FG, 0);
	button(&w->surf, w->w / 2 - 44, 6, 88, 28, m->over ? (m->won ? "You won!" : "Again") : "New game",
	       m->won ? rgb(0x9c, 0xd8, 0x9c) : m->over ? rgb(0xf0, 0xa0, 0xa0) : rgb(0xf4, 0xf5, 0xf7),
	       C_WIN_FG);
	for (int y = 0; y < MH; y++) {
		for (int x = 0; x < MW; x++) {
			int px = 4 + x * CELL, py = MTOP + y * CELL;
			if (m->open[y][x] || (m->over && m->mine[y][x])) {
				fill(&w->surf, px, py, CELL - 1, CELL - 1,
				     m->mine[y][x] && m->open[y][x] ? rgb(0xe0, 0x50, 0x50) : rgb(0xe8, 0xea, 0xee));
				if (m->mine[y][x]) {
					bitmap(&w->surf, px + 3, py + 3, icon_mine, 16, 16, 0);
				} else {
					int n = mines_count(m, x, y);
					if (n) {
						s[0] = '0' + n;
						s[1] = 0;
						text(&w->surf, px + 7, py + 3, s, numc[n], 0);
					}
				}
			} else {
				fill(&w->surf, px, py, CELL - 1, CELL - 1, rgb(0x8f, 0xa3, 0xb8));
				fill(&w->surf, px, py, CELL - 1, 2, rgb(0xb5, 0xc4, 0xd4));
				if (m->flag[y][x]) {
					fill(&w->surf, px + 8, py + 4, 2, 14, 0);
					fill(&w->surf, px + 10, py + 4, 7, 6, rgb(0xd0, 0x20, 0x20));
				}
			}
		}
	}
}

static void mines_mouse(struct win *w, int x, int y, int ev, int arg)
{
	struct mines *m = w->priv;
	int cx = (x - 4) / CELL, cy = (y - MTOP) / CELL;

	(void)arg;
	if (ev != M_DOWN && ev != M_DOUBLE && ev != M_RIGHT)
		return;
	if (y < MTOP) {
		if (ev != M_RIGHT && in_box(x, y, w->w / 2 - 44, 6, 88, 28))
			mines_new(m);
		win_dirty(w);
		return;
	}
	if (m->over || x < 4 || cx >= MW || cy >= MH)
		return;
	if (ev == M_RIGHT) {
		if (!m->open[cy][cx]) {
			m->flag[cy][cx] = !m->flag[cy][cx];
			m->flags += m->flag[cy][cx] ? 1 : -1;
		}
	} else if (m->open[cy][cx]) {
		/* a number with all its flags placed opens the rest */
		int f = 0;
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++)
				if (cx + dx >= 0 && cx + dx < MW && cy + dy >= 0 && cy + dy < MH)
					f += m->flag[cy + dy][cx + dx];
		if (f == mines_count(m, cx, cy))
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
					mines_open(m, cx + dx, cy + dy);
	} else {
		if (!m->started)
			mines_place(m, cx, cy);
		mines_open(m, cx, cy);
	}
	win_dirty(w);
}

static void mines_key(struct win *w, const struct key *k)
{
	if (k->code == KEY_F2 || k->ch == 'n') {
		mines_new(w->priv);
		win_dirty(w);
	}
}

static void mines_tick(struct win *w)
{
	struct mines *m = w->priv;

	if (m->started && !m->over)
		win_dirty(w);	/* the clock */
}

static void mines_close(struct win *w)
{
	free(w->priv);
}

static const struct app_ops mines_ops = {
	.draw = mines_draw, .mouse = mines_mouse, .key = mines_key, .tick = mines_tick,
	.close = mines_close,
};

void open_mines(void)
{
	struct mines *m = calloc(1, sizeof(*m));
	struct win *w;

	if (!m)
		return;
	srand(time(NULL) ^ getpid());
	w = win_new("Minesweeper", 8 + MW * CELL, MTOP + MH * CELL + 4, &mines_ops);
	if (!w) {
		free(m);
		return;
	}
	w->priv = m;
	w->fixed = 1;
	w->tick_ms = 1000;
}

/* ---------------------------------------------------------------- snake */

#define SW 30
#define SH 22
#define SC 16
#define STOP 24

struct snake {
	int x[SW * SH], y[SW * SH];	/* body, head first */
	int len, dx, dy, ndx, ndy;
	int fx, fy, score, best, over, paused;
};

static void snake_food(struct snake *s)
{
	for (;;) {
		int ok = 1;
		s->fx = rand() % SW;
		s->fy = rand() % SH;
		for (int i = 0; i < s->len; i++)
			if (s->x[i] == s->fx && s->y[i] == s->fy)
				ok = 0;
		if (ok)
			return;
	}
}

static void snake_new(struct snake *s)
{
	int best = s->best;

	memset(s, 0, sizeof(*s));
	s->best = best;
	s->len = 4;
	for (int i = 0; i < s->len; i++) {
		s->x[i] = SW / 2 - i;
		s->y[i] = SH / 2;
	}
	s->dx = s->ndx = 1;
	snake_food(s);
}

static void snake_tick(struct win *w)
{
	struct snake *s = w->priv;
	int nx, ny;

	if (s->over || s->paused || focus() != w)
		return;
	if (s->ndx != -s->dx || s->ndy != -s->dy) {
		s->dx = s->ndx;
		s->dy = s->ndy;
	}
	nx = s->x[0] + s->dx;
	ny = s->y[0] + s->dy;
	if (nx < 0 || nx >= SW || ny < 0 || ny >= SH) {
		s->over = 1;
		win_dirty(w);
		return;
	}
	for (int i = 0; i < s->len - 1; i++) {
		if (s->x[i] == nx && s->y[i] == ny) {
			s->over = 1;
			win_dirty(w);
			return;
		}
	}
	int grow = nx == s->fx && ny == s->fy;
	if (grow && s->len < SW * SH)
		s->len++;
	memmove(s->x + 1, s->x, sizeof(int) * (s->len - 1));
	memmove(s->y + 1, s->y, sizeof(int) * (s->len - 1));
	s->x[0] = nx;
	s->y[0] = ny;
	if (grow) {
		s->score += 10;
		if (s->score > s->best)
			s->best = s->score;
		snake_food(s);
		w->tick_ms = 120 - (s->len < 40 ? s->len : 40);
	}
	win_dirty(w);
}

static void snake_draw(struct win *w)
{
	struct snake *s = w->priv;
	char t[80];

	fill(&w->surf, 0, 0, w->w, w->h, rgb(0x14, 0x1c, 0x14));
	fill(&w->surf, 0, 0, w->w, STOP, rgb(0x22, 0x2e, 0x22));
	snprintf(t, sizeof(t), "Score %d   Best %d", s->score, s->best);
	text(&w->surf, 8, 4, t, C_TEXT, 0);
	text(&w->surf, w->w - 25 * FONT_W, 4, "Arrows move, P pauses", C_DIM, 0);
	fill(&w->surf, s->fx * SC + 3, STOP + s->fy * SC + 3, SC - 6, SC - 6, rgb(0xe0, 0x50, 0x40));
	for (int i = s->len - 1; i >= 0; i--)
		fill(&w->surf, s->x[i] * SC + 1, STOP + s->y[i] * SC + 1, SC - 2, SC - 2,
		     i ? rgb(0x5c, 0xc0, 0x5c) : rgb(0xa8, 0xf0, 0x80));
	if (s->over || s->paused || focus() != w) {
		const char *msg = s->over ? "Game over - press Space" : "Paused - press Space";
		int tw = strlen(msg) * FONT_W;
		fill_alpha(&w->surf, (w->w - tw) / 2 - 12, w->h / 2 - 16, tw + 24, 32, 0, 160);
		text(&w->surf, (w->w - tw) / 2, w->h / 2 - 8, msg, C_TEXT, 0);
	}
}

static void snake_key(struct win *w, const struct key *k)
{
	struct snake *s = w->priv;

	switch (k->code) {
	case KEY_UP: case KEY_W: s->ndx = 0, s->ndy = -1; break;
	case KEY_DOWN: case KEY_S: s->ndx = 0, s->ndy = 1; break;
	case KEY_LEFT: case KEY_A: s->ndx = -1, s->ndy = 0; break;
	case KEY_RIGHT: case KEY_D: s->ndx = 1, s->ndy = 0; break;
	case KEY_P: s->paused = !s->paused; break;
	case KEY_SPACE: case KEY_ENTER:
		if (s->over) {
			snake_new(s);
			w->tick_ms = 120;
		}
		s->paused = 0;
		break;
	}
	win_dirty(w);
}

static void snake_close(struct win *w)
{
	free(w->priv);
}

static const struct app_ops snake_ops = {
	.draw = snake_draw, .key = snake_key, .tick = snake_tick, .close = snake_close,
};

void open_snake(void)
{
	struct snake *s = calloc(1, sizeof(*s));
	struct win *w;

	if (!s)
		return;
	srand(time(NULL) ^ getpid());
	snake_new(s);
	s->paused = 1;
	w = win_new("Snake", SW * SC, STOP + SH * SC, &snake_ops);
	if (!w) {
		free(s);
		return;
	}
	w->priv = s;
	w->fixed = 1;
	w->tick_ms = 120;
}

/* ---------------------------------------------------------------- task manager */

struct proc { int pid; char name[32]; char state; long rss_kb; };

struct tasks {
	struct proc *p;
	int n, sel, top;
	long mem_total, mem_free;
	char msg[80];
};

static int proc_cmp(const void *a, const void *b)
{
	return ((const struct proc *)a)->pid - ((const struct proc *)b)->pid;
}

static void tasks_load(struct tasks *t)
{
	DIR *d = opendir("/proc");
	struct dirent *de;
	int cap = 0, sel_pid = t->sel >= 0 && t->sel < t->n ? t->p[t->sel].pid : -1;
	FILE *fp;
	char line[128];

	t->n = 0;
	while (d && (de = readdir(d))) {
		char path[64];
		int pid = atoi(de->d_name);
		if (pid <= 0)
			continue;
		if (t->n == cap) {
			struct proc *p = realloc(t->p, sizeof(*p) * (cap = cap ? cap * 2 : 64));
			if (!p)
				break;
			t->p = p;
		}
		struct proc *p = &t->p[t->n];
		memset(p, 0, sizeof(*p));
		p->pid = pid;
		/* kernel threads have no command line: leave them out */
		snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
		fp = fopen(path, "r");
		if (!fp)
			continue;
		int c = fgetc(fp);
		fclose(fp);
		if (c == EOF)
			continue;
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		fp = fopen(path, "r");
		if (!fp)
			continue;
		if (fgets(line, sizeof(line), fp)) {
			char *l = strchr(line, '('), *r = strrchr(line, ')');
			if (l && r && r > l) {
				snprintf(p->name, sizeof(p->name), "%.*s", (int)(r - l - 1), l + 1);
				p->state = r[2];
			}
		}
		fclose(fp);
		snprintf(path, sizeof(path), "/proc/%d/statm", pid);
		fp = fopen(path, "r");
		if (fp) {
			long size, rss;
			if (fscanf(fp, "%ld %ld", &size, &rss) == 2)
				p->rss_kb = rss * 4;
			fclose(fp);
		}
		t->n++;
	}
	if (d)
		closedir(d);
	qsort(t->p, t->n, sizeof(*t->p), proc_cmp);
	t->sel = -1;
	for (int i = 0; i < t->n; i++)
		if (t->p[i].pid == sel_pid)
			t->sel = i;
	fp = fopen("/proc/meminfo", "r");
	while (fp && fgets(line, sizeof(line), fp)) {
		sscanf(line, "MemTotal: %ld", &t->mem_total);
		sscanf(line, "MemAvailable: %ld", &t->mem_free);
	}
	if (fp)
		fclose(fp);
}

#define TROW ROW_H
#define TTOP (ROW_H * 2 + 8)

static void tasks_draw(struct win *w)
{
	struct tasks *t = w->priv;
	int rows = (w->h - TTOP - 40) / TROW;
	char s[128];

	fill(&w->surf, 0, 0, w->w, w->h, C_WIN_BG);
	snprintf(s, sizeof(s), "%d processes   memory: %ld MiB used of %ld MiB", t->n,
		 (t->mem_total - t->mem_free) / 1024, t->mem_total / 1024);
	text(&w->surf, 8, 4, s, C_WIN_FG, w->w - 16);
	fill(&w->surf, 0, ROW_H + 6, w->w, ROW_H, C_PANEL);
	text(&w->surf, 8, ROW_H + 7, "PID", C_WIN_FG, 0);
	text(&w->surf, 64, ROW_H + 7, "Name", C_WIN_FG, 0);
	text(&w->surf, w->w - 170, ROW_H + 7, "State", C_WIN_FG, 0);
	text(&w->surf, w->w - 90, ROW_H + 7, "Memory", C_WIN_FG, 0);
	if (t->top > t->n - rows)
		t->top = t->n - rows > 0 ? t->n - rows : 0;
	for (int i = 0; i < rows && t->top + i < t->n; i++) {
		struct proc *p = &t->p[t->top + i];
		int y = TTOP + i * TROW;
		uint32_t fg = C_WIN_FG;
		const char *st = p->state == 'R' ? "running" : p->state == 'S' ? "sleeping" :
				 p->state == 'T' ? "stopped" : p->state == 'Z' ? "zombie" : p->state == 'I' ? "idle" : "other";
		if (t->top + i == t->sel) {
			fill(&w->surf, 0, y, w->w, TROW, C_SEL);
			fg = C_TEXT;
		}
		snprintf(s, sizeof(s), "%d", p->pid);
		text(&w->surf, 8, y + 1, s, fg, 0);
		text(&w->surf, 64, y + 1, p->name, fg, w->w - 250);
		text(&w->surf, w->w - 170, y + 1, st, fg, 0);
		snprintf(s, sizeof(s), "%ld KiB", p->rss_kb);
		text(&w->surf, w->w - 90, y + 1, s, fg, 0);
	}
	fill(&w->surf, 0, w->h - 36, w->w, 36, C_PANEL);
	button(&w->surf, w->w - 108, w->h - 30, 100, 24, "End task",
	       t->sel >= 0 ? C_SEL : rgb(0xdd, 0xe1, 0xe6), t->sel >= 0 ? C_TEXT : C_DIM);
	text(&w->surf, 8, w->h - 26, t->msg, C_WIN_FG, w->w - 130);
}

static void tasks_kill(struct win *w, int sig)
{
	struct tasks *t = w->priv;

	if (t->sel < 0 || t->sel >= t->n)
		return;
	if (t->p[t->sel].pid == 1 || t->p[t->sel].pid == getpid()) {
		snprintf(t->msg, sizeof(t->msg), "Not ending %s: the system needs it", t->p[t->sel].name);
	} else if (kill(t->p[t->sel].pid, sig) < 0) {
		snprintf(t->msg, sizeof(t->msg), "%s", strerror(errno));
	} else {
		snprintf(t->msg, sizeof(t->msg), "Sent %s to %d", sig == SIGKILL ? "KILL" : "TERM",
			 t->p[t->sel].pid);
	}
	tasks_load(t);
	win_dirty(w);
}

static void tasks_menu_pick(void *ctx, int i)
{
	struct win *w = ctx;

	if (!win_alive(w))
		return;
	tasks_kill(w, i == 0 ? SIGTERM : i == 1 ? SIGKILL : i == 2 ? SIGSTOP : SIGCONT);
}

static void tasks_mouse(struct win *w, int x, int y, int ev, int arg)
{
	struct tasks *t = w->priv;
	int i = t->top + (y - TTOP) / TROW;

	if (ev == M_WHEEL) {
		t->top -= arg * 3;
		if (t->top < 0)
			t->top = 0;
		win_dirty(w);
		return;
	}
	if (ev != M_DOWN && ev != M_DOUBLE && ev != M_RIGHT)
		return;
	if (y >= w->h - 36) {
		if (ev != M_RIGHT && x >= w->w - 108)
			tasks_kill(w, SIGTERM);
		return;
	}
	if (y >= TTOP && i < t->n) {
		t->sel = i;
		t->msg[0] = 0;
		if (ev == M_RIGHT) {
			static const char *const items[] = { "End (TERM)", "Kill (KILL)", "Stop", "Continue" };
			popup_menu(w->x + x, w->y + y, items, 4, tasks_menu_pick, w);
		}
	}
	win_dirty(w);
}

static void tasks_key(struct win *w, const struct key *k)
{
	struct tasks *t = w->priv;

	if (k->code == KEY_UP && t->sel > 0)
		t->sel--;
	else if (k->code == KEY_DOWN && t->sel < t->n - 1)
		t->sel++;
	else if (k->code == KEY_DELETE)
		tasks_kill(w, SIGTERM);
	win_dirty(w);
}

static void tasks_tick(struct win *w)
{
	tasks_load(w->priv);
	win_dirty(w);
}

static void tasks_close(struct win *w)
{
	struct tasks *t = w->priv;

	free(t->p);
	free(t);
}

static const struct app_ops tasks_ops = {
	.draw = tasks_draw, .mouse = tasks_mouse, .key = tasks_key, .tick = tasks_tick,
	.close = tasks_close,
};

void open_tasks(void)
{
	struct tasks *t = calloc(1, sizeof(*t));
	struct win *w;

	if (!t)
		return;
	t->sel = -1;
	tasks_load(t);
	w = win_new("Task manager", 480, 420, &tasks_ops);
	if (!w) {
		free(t);
		return;
	}
	w->priv = t;
	w->tick_ms = 2000;
}

/* ---------------------------------------------------------------- paint */

#define PTOOL_W 64
#define PPAL_H 34
#define MAX_IMG 2048

enum { T_PENCIL, T_BRUSH, T_ERASER, T_LINE, T_RECT, T_FILL, NTOOLS };
static const char *const tool_names[NTOOLS] = { "Pencil", "Brush", "Eraser", "Line", "Box", "Fill" };

static const uint32_t palette[16] = {
	0x000000, 0xffffff, 0x808080, 0xc0c0c0, 0xe02020, 0xff8a00, 0xffd800, 0x30b030,
	0x1e90ff, 0x2040c0, 0x8a2be2, 0xff60b0, 0x8b4513, 0x00c0c0, 0x606000, 0x404040,
};

struct paint {
	char path[512];
	struct surface img, undo;
	int tool, color, modified;
	int down, lx, ly, sx, sy;	/* stroke state, image coordinates */
	int ox, oy;			/* scroll offset */
};

static int paint_cw(struct win *w) { return w->w - PTOOL_W; }
static int paint_ch(struct win *w) { return w->h - PPAL_H; }

static void put_px(struct surface *s, int x, int y, uint32_t c)
{
	if (x >= 0 && y >= 0 && x < s->w && y < s->h)
		s->px[(size_t)y * s->stride + x] = c;
}

static void dab(struct paint *p, int x, int y)
{
	uint32_t c = p->tool == T_ERASER ? 0xffffff : palette[p->color];
	int r = p->tool == T_PENCIL ? 0 : p->tool == T_BRUSH ? 3 : 6;

	for (int dy = -r; dy <= r; dy++)
		for (int dx = -r; dx <= r; dx++)
			if (dx * dx + dy * dy <= r * r + r)
				put_px(&p->img, x + dx, y + dy, c);
}

static void stroke(struct paint *p, int x0, int y0, int x1, int y1, int brush)
{
	int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;

	for (;;) {
		if (brush)
			dab(p, x0, y0);
		else
			put_px(&p->img, x0, y0, palette[p->color]);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

static void flood(struct surface *s, int x, int y, uint32_t c)
{
	uint32_t from;
	int *stack, n = 0, cap = 4096;

	if (x < 0 || y < 0 || x >= s->w || y >= s->h)
		return;
	from = s->px[(size_t)y * s->stride + x];
	if (from == c || !(stack = malloc(sizeof(int) * 2 * cap)))
		return;
	stack[n++] = x;
	stack[n++] = y;
	while (n) {
		y = stack[--n];
		x = stack[--n];
		int l = x, r = x;
		uint32_t *row = s->px + (size_t)y * s->stride;
		if (row[x] != from)
			continue;
		while (l > 0 && row[l - 1] == from)
			l--;
		while (r < s->w - 1 && row[r + 1] == from)
			r++;
		for (int i = l; i <= r; i++)
			row[i] = c;
		for (int ny = y - 1; ny <= y + 1; ny += 2) {
			if (ny < 0 || ny >= s->h)
				continue;
			uint32_t *nr = s->px + (size_t)ny * s->stride;
			for (int i = l; i <= r; i++) {
				if (nr[i] != from || (i > l && nr[i - 1] == from))
					continue;
				if (n + 2 > cap * 2) {
					int *ns = realloc(stack, sizeof(int) * 4 * cap);
					if (!ns)
						goto out;
					stack = ns;
					cap *= 2;
				}
				stack[n++] = i;
				stack[n++] = ny;
			}
		}
	}
out:
	free(stack);
}

static void paint_draw(struct win *w)
{
	struct paint *p = w->priv;
	struct surface view = p->img;

	fill(&w->surf, 0, 0, w->w, w->h, rgb(0x80, 0x88, 0x94));
	/* the image, offset by the scroll position */
	view.px = p->img.px + (size_t)p->oy * p->img.stride + p->ox;
	view.w = p->img.w - p->ox;
	view.h = p->img.h - p->oy;
	surface_noclip(&view);
	{
		struct rect c = { PTOOL_W, 0, paint_cw(w), paint_ch(w) };
		w->surf.clip = c;
		blit(&w->surf, PTOOL_W, 0, &view);
		/* the box or line being drawn */
		if (p->down && (p->tool == T_LINE || p->tool == T_RECT)) {
			int x0 = PTOOL_W + p->sx - p->ox, y0 = p->sy - p->oy;
			int x1 = PTOOL_W + p->lx - p->ox, y1 = p->ly - p->oy;
			if (p->tool == T_RECT) {
				int x = x0 < x1 ? x0 : x1, y = y0 < y1 ? y0 : y1;
				frame(&w->surf, x, y, abs(x1 - x0) + 1, abs(y1 - y0) + 1, palette[p->color]);
			} else {
				int n = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
				for (int i = 0; i <= n; i++)
					fill(&w->surf, x0 + (n ? (x1 - x0) * i / n : 0),
					     y0 + (n ? (y1 - y0) * i / n : 0), 1, 1, palette[p->color]);
			}
		}
		surface_noclip(&w->surf);
	}
	/* tools */
	fill(&w->surf, 0, 0, PTOOL_W, w->h, C_PANEL);
	for (int i = 0; i < NTOOLS; i++)
		button(&w->surf, 4, 4 + i * 30, PTOOL_W - 8, 26, tool_names[i],
		       i == p->tool ? C_SEL : rgb(0xf8, 0xf9, 0xfa), i == p->tool ? C_TEXT : C_WIN_FG);
	button(&w->surf, 4, 4 + NTOOLS * 30 + 10, PTOOL_W - 8, 26, "Undo", rgb(0xf8, 0xf9, 0xfa), C_WIN_FG);
	button(&w->surf, 4, 4 + NTOOLS * 30 + 40, PTOOL_W - 8, 26, "Save", rgb(0xf8, 0xf9, 0xfa), C_WIN_FG);
	button(&w->surf, 4, 4 + NTOOLS * 30 + 70, PTOOL_W - 8, 26, "Clear", rgb(0xf8, 0xf9, 0xfa), C_WIN_FG);
	/* palette */
	fill(&w->surf, PTOOL_W, w->h - PPAL_H, w->w - PTOOL_W, PPAL_H, C_PANEL);
	fill(&w->surf, PTOOL_W + 6, w->h - PPAL_H + 4, 26, 26, palette[p->color]);
	frame(&w->surf, PTOOL_W + 6, w->h - PPAL_H + 4, 26, 26, 0);
	for (int i = 0; i < 16; i++) {
		int x = PTOOL_W + 40 + i * 22;
		fill(&w->surf, x, w->h - PPAL_H + 7, 20, 20, palette[i]);
		frame(&w->surf, x, w->h - PPAL_H + 7, 20, 20, i == p->color ? C_SEL : rgb(0x60, 0x60, 0x60));
	}
}

static void paint_title(struct win *w)
{
	struct paint *p = w->priv;
	const char *base = strrchr(p->path, '/');

	snprintf(w->title, sizeof(w->title), "%s%s - Paint", p->modified ? "*" : "",
		 p->path[0] ? (base ? base + 1 : p->path) : "untitled.bmp");
}

/* 24-bit uncompressed BMP, bottom-up */
static int bmp_save(const struct surface *s, const char *path)
{
	FILE *fp = fopen(path, "wb");
	int row = (s->w * 3 + 3) & ~3;
	unsigned char h[54] = { 'B', 'M' }, *line;

	if (!fp)
		return -1;
	line = calloc(1, row);
	if (!line) {
		fclose(fp);
		return -1;
	}
#define LE32(o, v) (h[o] = (v) & 255, h[o + 1] = (v) >> 8 & 255, h[o + 2] = (v) >> 16 & 255, h[o + 3] = (unsigned)(v) >> 24)
	LE32(2, 54 + row * s->h);
	LE32(10, 54);
	LE32(14, 40);
	LE32(18, s->w);
	LE32(22, s->h);
	h[26] = 1;
	h[28] = 24;
	LE32(34, row * s->h);
	fwrite(h, 1, 54, fp);
	for (int y = s->h - 1; y >= 0; y--) {
		for (int x = 0; x < s->w; x++) {
			uint32_t c = s->px[(size_t)y * s->stride + x];
			line[x * 3] = c;
			line[x * 3 + 1] = c >> 8;
			line[x * 3 + 2] = c >> 16;
		}
		fwrite(line, 1, row, fp);
	}
	free(line);
	return fclose(fp) ? -1 : 0;
}

static int rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

/* 24- and 32-bit uncompressed BMPs */
static int bmp_load(struct surface *s, const char *path)
{
	FILE *fp = fopen(path, "rb");
	unsigned char h[54], *line = NULL;
	int ok = -1;

	if (!fp)
		return -1;
	if (fread(h, 1, 54, fp) == 54 && h[0] == 'B' && h[1] == 'M') {
		int off = rd32(h + 10), w = rd32(h + 18), hh = rd32(h + 22), bpp = h[28];
		int comp = rd32(h + 30), flip = hh > 0;
		if (hh < 0)
			hh = -hh;
		int row = (w * (bpp / 8) + 3) & ~3;
		if ((bpp == 24 || bpp == 32) && (comp == 0 || comp == 3) && w > 0 && hh > 0 &&
		    w <= MAX_IMG && hh <= MAX_IMG && !surface_init(s, w, hh) && (line = malloc(row)) &&
		    fseek(fp, off, SEEK_SET) == 0) {
			ok = 0;
			for (int y = 0; y < hh; y++) {
				if (fread(line, 1, row, fp) != (size_t)row)
					break;
				int dy = flip ? hh - 1 - y : y;
				for (int x = 0; x < w; x++) {
					unsigned char *px = line + x * (bpp / 8);
					s->px[(size_t)dy * s->stride + x] = rgb(px[2], px[1], px[0]);
				}
			}
		}
	}
	free(line);
	fclose(fp);
	return ok;
}

static void paint_save_to(struct win *w)
{
	struct paint *p = w->priv;

	if (bmp_save(&p->img, p->path)) {
		open_message("Paint", "Could not save the picture.", NULL, NULL);
		return;
	}
	sync();
	p->modified = 0;
	paint_title(w);
	win_dirty(w);
}

static void paint_save_done(void *ctx, const char *name)
{
	struct win *w = ctx;
	struct paint *p;
	size_t n;

	if (!win_alive(w))
		return;
	p = w->priv;
	snprintf(p->path, sizeof(p->path), "%s%s", name[0] == '/' ? "" : "/root/", name);
	n = strlen(p->path);
	if (n < 4 || strcasecmp(p->path + n - 4, ".bmp"))
		snprintf(p->path + n, sizeof(p->path) - n, ".bmp");
	paint_save_to(w);
}

static void paint_save(struct win *w, int as)
{
	struct paint *p = w->priv;

	if (!p->path[0] || as)
		open_prompt("Save picture", "Save to (a path, or a name in /root):",
			    p->path[0] ? p->path : "/root/Desktop/picture.bmp", paint_save_done, w);
	else
		paint_save_to(w);
}

static void paint_snapshot(struct paint *p)
{
	memcpy(p->undo.px, p->img.px, (size_t)p->img.w * p->img.h * 4);
}

static void paint_undo(struct paint *p)
{
	size_t n = (size_t)p->img.w * p->img.h;

	for (size_t i = 0; i < n; i++) {	/* swap: undo twice redoes */
		uint32_t t = p->img.px[i];
		p->img.px[i] = p->undo.px[i];
		p->undo.px[i] = t;
	}
}

static void paint_clamp(struct win *w)
{
	struct paint *p = w->priv;
	int mx = p->img.w - paint_cw(w), my = p->img.h - paint_ch(w);

	if (p->ox > mx)
		p->ox = mx;
	if (p->oy > my)
		p->oy = my;
	if (p->ox < 0)
		p->ox = 0;
	if (p->oy < 0)
		p->oy = 0;
}

static void paint_mouse(struct win *w, int x, int y, int ev, int arg)
{
	struct paint *p = w->priv;
	int ix = x - PTOOL_W + p->ox, iy = y + p->oy;

	if (ev == M_WHEEL) {
		p->oy -= arg * 40;
		paint_clamp(w);
		win_dirty(w);
		return;
	}
	if (ev == M_RIGHT) {
		/* pick the color under the pointer */
		if (x >= PTOOL_W && y < paint_ch(w) && ix < p->img.w && iy < p->img.h) {
			uint32_t c = p->img.px[(size_t)iy * p->img.stride + ix];
			for (int i = 0; i < 16; i++)
				if (palette[i] == c)
					p->color = i;
			win_dirty(w);
		}
		return;
	}
	if (ev == M_DOWN || ev == M_DOUBLE) {
		if (x < PTOOL_W) {
			int i = (y - 4) / 30;
			if (i >= 0 && i < NTOOLS)
				p->tool = i;
			else if (y >= 4 + NTOOLS * 30 + 10 && y < 4 + NTOOLS * 30 + 36)
				paint_undo(p);
			else if (y >= 4 + NTOOLS * 30 + 40 && y < 4 + NTOOLS * 30 + 66)
				paint_save(w, 0);
			else if (y >= 4 + NTOOLS * 30 + 70 && y < 4 + NTOOLS * 30 + 96) {
				paint_snapshot(p);
				fill(&p->img, 0, 0, p->img.w, p->img.h, 0xffffff);
				p->modified = 1;
			}
		} else if (y >= paint_ch(w)) {
			int i = (x - PTOOL_W - 40) / 22;
			if (x >= PTOOL_W + 40 && i < 16)
				p->color = i;
		} else {
			paint_snapshot(p);
			p->down = 1;
			p->sx = p->lx = ix;
			p->sy = p->ly = iy;
			p->modified = 1;
			if (p->tool == T_FILL) {
				flood(&p->img, ix, iy, palette[p->color]);
				p->down = 0;
			} else if (p->tool <= T_ERASER) {
				dab(p, ix, iy);
			}
		}
		paint_title(w);
		win_dirty(w);
		return;
	}
	if (!p->down)
		return;
	if (ev == M_MOVE) {
		if (p->tool <= T_ERASER)
			stroke(p, p->lx, p->ly, ix, iy, p->tool != T_PENCIL);
		p->lx = ix;
		p->ly = iy;
	} else if (ev == M_UP) {
		if (p->tool == T_LINE) {
			stroke(p, p->sx, p->sy, ix, iy, 0);
		} else if (p->tool == T_RECT) {
			int x0 = p->sx < ix ? p->sx : ix, y0 = p->sy < iy ? p->sy : iy;
			frame(&p->img, x0, y0, abs(ix - p->sx) + 1, abs(iy - p->sy) + 1, palette[p->color]);
		}
		p->down = 0;
	}
	win_dirty(w);
}

static void paint_key(struct win *w, const struct key *k)
{
	struct paint *p = w->priv;

	if ((k->mods & MOD_CTRL) && k->code == KEY_S)
		paint_save(w, k->mods & MOD_SHIFT);
	else if ((k->mods & MOD_CTRL) && k->code == KEY_Z)
		paint_undo(p);
	else if (k->code >= KEY_1 && k->code < KEY_1 + NTOOLS)
		p->tool = k->code - KEY_1;
	win_dirty(w);
}

static void paint_resized(struct win *w)
{
	paint_clamp(w);
}

static void paint_close(struct win *w)
{
	struct paint *p = w->priv;

	surface_free(&p->img);
	surface_free(&p->undo);
	free(p);
}

static const struct app_ops paint_ops = {
	.draw = paint_draw, .mouse = paint_mouse, .key = paint_key, .resized = paint_resized,
	.close = paint_close,
};

void open_paint(const char *path)
{
	struct paint *p = calloc(1, sizeof(*p));
	struct win *w;

	if (!p)
		return;
	if (path) {
		snprintf(p->path, sizeof(p->path), "%s", path);
		if (bmp_load(&p->img, path)) {
			char msg[600];
			snprintf(msg, sizeof(msg), "%s:\nnot a 24- or 32-bit BMP picture this program can read.", path);
			open_message("Paint", msg, NULL, NULL);
			free(p);
			return;
		}
	} else if (surface_init(&p->img, 640, 420)) {
		free(p);
		return;
	} else {
		fill(&p->img, 0, 0, p->img.w, p->img.h, 0xffffff);
	}
	if (surface_init(&p->undo, p->img.w, p->img.h)) {
		surface_free(&p->img);
		free(p);
		return;
	}
	paint_snapshot(p);
	p->color = 0;
	p->tool = T_BRUSH;
	int ww = p->img.w + PTOOL_W, wh = p->img.h + PPAL_H;
	w = win_new("Paint", ww < 460 ? 460 : ww, wh < 300 ? 300 : wh, &paint_ops);
	if (!w) {
		paint_close(&(struct win){ .priv = p });
		return;
	}
	w->priv = p;
	paint_title(w);
}

/* ---------------------------------------------------------------- settings */

static const struct { const char *name; uint32_t top, bot; } wallpapers[] = {
	{ "Night", C_DESK_TOP, C_DESK_BOT },
	{ "Ocean", RGB(0x10, 0x6e, 0x8c), RGB(0x05, 0x22, 0x36) },
	{ "Forest", RGB(0x2f, 0x5d, 0x3a), RGB(0x0d, 0x1f, 0x12) },
	{ "Sunset", RGB(0xd9, 0x6a, 0x3b), RGB(0x3a, 0x16, 0x3c) },
	{ "Grape", RGB(0x6a, 0x3f, 0x9e), RGB(0x1a, 0x0f, 0x2e) },
	{ "Slate", RGB(0x5a, 0x63, 0x6e), RGB(0x1d, 0x21, 0x26) },
	{ "Rose", RGB(0xc4, 0x5c, 0x7c), RGB(0x2c, 0x10, 0x1c) },
	{ "Mint", RGB(0x3f, 0xa8, 0x8f), RGB(0x0c, 0x2b, 0x27) },
};
#define NWALL (int)(sizeof(wallpapers) / sizeof(wallpapers[0]))
#define SW_W 96
#define SW_H 64

static void settings_draw(struct win *w)
{
	struct statfs sf;
	char s[160];

	fill(&w->surf, 0, 0, w->w, w->h, C_WIN_BG);
	text(&w->surf, 12, 10, "Wallpaper", C_WIN_FG, 0);
	for (int i = 0; i < NWALL; i++) {
		int x = 12 + i % 4 * (SW_W + 8), y = 32 + i / 4 * (SW_H + 26);
		gradient(&w->surf, x, y, SW_W, SW_H, wallpapers[i].top, wallpapers[i].bot);
		int on = wallpapers[i].top == desk_top && wallpapers[i].bot == desk_bot;
		frame(&w->surf, x - 1, y - 1, SW_W + 2, SW_H + 2, on ? C_SEL : rgb(0xb4, 0xbb, 0xc4));
		if (on)
			frame(&w->surf, x - 2, y - 2, SW_W + 4, SW_H + 4, C_SEL);
		center_text(&w->surf, x, y + SW_H + 4, SW_W, wallpapers[i].name, C_WIN_FG);
	}
	int y = 32 + 2 * (SW_H + 26) + 8;
	text(&w->surf, 12, y, "Storage", C_WIN_FG, 0);
	if (statfs("/mnt/disk", &sf) == 0 && sf.f_blocks) {
		long long total = (long long)sf.f_blocks * sf.f_bsize, avail = (long long)sf.f_bavail * sf.f_bsize;
		snprintf(s, sizeof(s), "Saved disk: %lld MiB used, %lld MiB free",
			 (total - avail) >> 20, avail >> 20);
		fill(&w->surf, 12, y + 24, w->w - 24, 10, rgb(0xdd, 0xe1, 0xe6));
		fill(&w->surf, 12, y + 24, (int)((w->w - 24) * (total - avail) / total), 10, C_SEL);
		text(&w->surf, 12, y + 40, s, C_DIM, w->w - 24);
	} else {
		text(&w->surf, 12, y + 24, "No saved disk: files are lost when the page closes.", C_DIM, w->w - 24);
	}
	snprintf(s, sizeof(s), "Screen: %dx%d (?mode=WxH in the address changes it)", screen.w, screen.h);
	text(&w->surf, 12, y + 64, s, C_DIM, w->w - 24);
}

static void settings_mouse(struct win *w, int x, int y, int ev, int arg)
{
	(void)arg;
	if (ev != M_DOWN && ev != M_DOUBLE)
		return;
	for (int i = 0; i < NWALL; i++) {
		int bx = 12 + i % 4 * (SW_W + 8), by = 32 + i / 4 * (SW_H + 26);
		if (in_box(x, y, bx, by, SW_W, SW_H + 20)) {
			desk_top = wallpapers[i].top;
			desk_bot = wallpapers[i].bot;
			make_wallpaper();
			save_settings();
			win_dirty(w);
		}
	}
}

static const struct app_ops settings_ops = { .draw = settings_draw, .mouse = settings_mouse };

void open_settings(void)
{
	struct win *w = win_new("Settings", 12 + 4 * (SW_W + 8) + 4, 32 + 2 * (SW_H + 26) + 100, &settings_ops);

	if (w)
		w->fixed = 1;
}
