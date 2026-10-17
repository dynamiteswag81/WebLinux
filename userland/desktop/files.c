/*
 * The file manager: a list of one directory with a toolbar and a status
 * line.  Rename (F2), delete, new file/folder, and copy/cut/paste of files
 * through the desktop's file clipboard.  Copies and deletes run BusyBox's
 * cp and rm.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "desk.h"

#define BAR_H	30	/* toolbar */
#define STATUS_H ROW_H

struct entry { char name[NAME_MAX + 1]; int dir; long long size; };

struct files {
	char path[PATH_MAX];
	struct entry *ent;
	int n, sel, top;
	int hidden;		/* show dot files */
	struct timespec mtime;	/* of the directory when it was read */
	char msg[96];
};

/* the file clipboard: one path, copied or moved on paste */
static char fclip[PATH_MAX];
static int fclip_cut;

static int has_ext(const char *name, const char *ext)
{
	size_t n = strlen(name), e = strlen(ext);
	return n > e && !strcasecmp(name + n - e, ext);
}

int is_image(const char *name)
{
	return has_ext(name, ".bmp");
}

/* open a file with the program for its type */
void open_path(const char *p)
{
	char cmd[PATH_MAX + 64];
	struct stat st;

	if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
		open_files(p);
	} else if (has_ext(p, ".html") || has_ext(p, ".htm")) {
		snprintf(cmd, sizeof(cmd), "file://%s", p);
		browse(cmd);
	} else if (has_ext(p, ".wav")) {
		/* quoted for the shell: no single quotes in the name */
		if (strchr(p, '\''))
			return;
		snprintf(cmd, sizeof(cmd), "play '%s'; sleep 1", p);
		open_terminal(cmd);
	} else if (is_image(p)) {
		open_paint(p);
	} else if (stat(p, &st) == 0 && (st.st_mode & 0111) && access(p, X_OK) == 0 &&
		   !strchr(p, '\'')) {
		/* a program: run it in a terminal */
		snprintf(cmd, sizeof(cmd), "'%s'; echo; echo '[exited: press Enter]'; read x", p);
		open_terminal(cmd);
	} else {
		open_editor(p);
	}
}

static int entcmp(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	if (!strcmp(x->name, ".."))
		return -1;
	if (!strcmp(y->name, ".."))
		return 1;
	if (x->dir != y->dir)
		return y->dir - x->dir;
	return strcasecmp(x->name, y->name);
}

static int list_rows(struct win *w)
{
	return (w->h - BAR_H - STATUS_H) / ROW_H;
}

static void join(char *out, size_t n, const char *dir, const char *name)
{
	snprintf(out, n, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

static void files_title(struct win *w)
{
	struct files *f = w->priv;
	const char *base = strrchr(f->path, '/');

	snprintf(w->title, sizeof(w->title), "Files - %s", base && base[1] ? base + 1 : f->path);
}

/* (re)read the directory; keep the selection on `keep` when it is there */
static void files_load_keep(struct win *w, const char *keep)
{
	struct files *f = w->priv;
	DIR *d = opendir(f->path);
	struct dirent *de;
	struct stat st;
	int cap = 0;

	free(f->ent);
	f->ent = NULL;
	f->n = f->sel = 0;
	if (stat(f->path, &st) == 0)
		f->mtime = st.st_mtim;
	files_title(w);
	while (d && (de = readdir(d))) {
		char full[PATH_MAX * 2];

		if (!strcmp(de->d_name, "."))
			continue;
		if (!strcmp(de->d_name, "..") ? !strcmp(f->path, "/") : de->d_name[0] == '.' && !f->hidden)
			continue;
		if (f->n == cap) {
			struct entry *e = realloc(f->ent, sizeof(*e) * (cap = cap ? cap * 2 : 64));
			if (!e)
				break;
			f->ent = e;
		}
		join(full, sizeof(full), f->path, de->d_name);
		snprintf(f->ent[f->n].name, sizeof(f->ent[0].name), "%s", de->d_name);
		if (stat(full, &st) == 0) {
			f->ent[f->n].dir = S_ISDIR(st.st_mode);
			f->ent[f->n].size = st.st_size;
		} else {
			f->ent[f->n].dir = 0;
			f->ent[f->n].size = 0;
		}
		f->n++;
	}
	if (d)
		closedir(d);
	else
		snprintf(f->msg, sizeof(f->msg), "%s", strerror(errno));
	qsort(f->ent, f->n, sizeof(*f->ent), entcmp);
	for (int i = 0; keep && i < f->n; i++)
		if (!strcmp(f->ent[i].name, keep))
			f->sel = i;
	if (f->top > f->sel || f->top >= f->n)
		f->top = f->sel;
	win_dirty(w);
}

static void files_load(struct win *w)
{
	struct files *f = w->priv;

	f->top = 0;
	f->msg[0] = 0;
	files_load_keep(w, NULL);
}

static void files_reload(struct win *w)
{
	struct files *f = w->priv;
	char keep[NAME_MAX + 1] = "";

	if (f->sel >= 0 && f->sel < f->n)
		snprintf(keep, sizeof(keep), "%s", f->ent[f->sel].name);
	files_load_keep(w, keep);
}

static void files_cd(struct win *w, const char *path)
{
	struct files *f = w->priv;
	char old[PATH_MAX];
	const char *keep = NULL;

	snprintf(old, sizeof(old), "%s", f->path);
	if (!strcmp(path, "..")) {
		char *s = strrchr(f->path, '/');
		if (!s || !strcmp(f->path, "/"))
			return;
		keep = strrchr(old, '/') + 1;	/* select the folder we came from */
		if (s == f->path)
			s[1] = 0;
		else
			*s = 0;
	} else {
		snprintf(f->path, sizeof(f->path), "%s", path);
	}
	f->top = 0;
	f->msg[0] = 0;
	files_load_keep(w, keep);
}

static void size_str(char *buf, size_t n, long long size)
{
	if (size < 10240)
		snprintf(buf, n, "%lld B", size);
	else if (size < 10 << 20)
		snprintf(buf, n, "%lld KiB", size >> 10);
	else
		snprintf(buf, n, "%lld MiB", size >> 20);
}

static void tool_button(struct surface *s, int x, const char *label, int w)
{
	fill(s, x, 4, w, BAR_H - 8, rgb(0xf8, 0xf9, 0xfa));
	frame(s, x, 4, w, BAR_H - 8, rgb(0xb4, 0xbb, 0xc4));
	text(s, x + (w - (int)strlen(label) * FONT_W) / 2, 7, label, C_WIN_FG, w);
}

static void files_draw(struct win *w)
{
	struct files *f = w->priv;
	int rows = list_rows(w);
	char size[32], status[160];

	fill(&w->surf, 0, 0, w->w, w->h, C_WIN_BG);
	/* toolbar: Up, Home, the path */
	fill(&w->surf, 0, 0, w->w, BAR_H, C_PANEL);
	tool_button(&w->surf, 4, "Up", 40);
	tool_button(&w->surf, 48, "Home", 48);
	tool_button(&w->surf, 100, "New", 40);
	fill(&w->surf, 146, 4, w->w - 150, BAR_H - 8, rgb(255, 255, 255));
	frame(&w->surf, 146, 4, w->w - 150, BAR_H - 8, rgb(0xb4, 0xbb, 0xc4));
	text(&w->surf, 150, 7, f->path, C_WIN_FG, w->w - 158);
	for (int i = 0; i < rows && f->top + i < f->n; i++) {
		struct entry *e = &f->ent[f->top + i];
		int y = BAR_H + i * ROW_H;
		uint32_t fg = C_WIN_FG;

		if (f->top + i == f->sel) {
			fill(&w->surf, 0, y, w->w, ROW_H, C_SEL);
			fg = C_TEXT;
		}
		bitmap(&w->surf, 6, y + 1, e->dir ? icon_folder : is_image(e->name) ? icon_image : icon_file,
		       16, 16, e->dir ? rgb(0xd0, 0x9a, 0x2a) : fg);
		text(&w->surf, 28, y + 1, e->name, fg, w->w - 130);
		if (!e->dir) {
			size_str(size, sizeof(size), e->size);
			text(&w->surf, w->w - 90, y + 1, size, fg == C_TEXT ? fg : C_DIM, 0);
		}
	}
	if (!f->n)
		text(&w->surf, 28, BAR_H + 4, "(empty folder)", C_DIM, 0);
	/* status line */
	fill(&w->surf, 0, w->h - STATUS_H, w->w, STATUS_H, C_PANEL);
	if (f->msg[0])
		snprintf(status, sizeof(status), "%s", f->msg);
	else
		snprintf(status, sizeof(status), "%d items%s%s%s", f->n - (f->n && !strcmp(f->ent[0].name, "..")),
			 f->hidden ? ", hidden files shown" : "",
			 fclip[0] ? fclip_cut ? "   (1 file cut)" : "   (1 file copied)" : "", "");
	text(&w->surf, 6, w->h - STATUS_H + 1, status, C_WIN_FG, w->w - 12);
}

static const char *sel_name(struct files *f)
{
	if (f->sel < 0 || f->sel >= f->n || !strcmp(f->ent[f->sel].name, ".."))
		return NULL;
	return f->ent[f->sel].name;
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
		files_cd(w, "..");
		return;
	}
	join(p, sizeof(p), f->path, e->name);
	if (e->dir)
		files_cd(w, p);
	else
		open_path(p);
}

/* ---- operations; a pop-up or prompt carries the window as its context */

static void op_done(struct win *w, const char *keep)
{
	sync();
	if (keep)
		files_load_keep(w, keep);
	else
		files_reload(w);
}

static int valid_name(struct files *f, const char *name)
{
	if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
		snprintf(f->msg, sizeof(f->msg), "Not a valid name: %s", name);
		return 0;
	}
	return 1;
}

static void do_rename(void *ctx, const char *name)
{
	struct win *w = ctx;
	struct files *f;
	char from[PATH_MAX * 2], to[PATH_MAX * 2];

	if (!win_alive(w))
		return;
	f = w->priv;
	if (!sel_name(f) || !valid_name(f, name)) {
		win_dirty(w);
		return;
	}
	join(from, sizeof(from), f->path, sel_name(f));
	join(to, sizeof(to), f->path, name);
	if (access(to, F_OK) == 0 && strcmp(from, to)) {
		snprintf(f->msg, sizeof(f->msg), "%s already exists", name);
		win_dirty(w);
		return;
	}
	if (rename(from, to) < 0) {
		snprintf(f->msg, sizeof(f->msg), "Rename failed: %s", strerror(errno));
		win_dirty(w);
		return;
	}
	f->msg[0] = 0;
	op_done(w, name);
}

static void ask_rename(struct win *w)
{
	struct files *f = w->priv;

	if (sel_name(f))
		open_prompt("Rename", "New name:", sel_name(f), do_rename, w);
}

static char del_path[PATH_MAX * 2];
static struct win *del_win;

static void do_delete(void *ctx)
{
	char *argv[] = { "/bin/rm", "-rf", del_path, NULL };

	(void)ctx;
	if (run_wait(argv) != 0 && win_alive(del_win))
		snprintf(((struct files *)del_win->priv)->msg, 96, "Could not delete everything");
	sync();
	if (win_alive(del_win))
		files_reload(del_win);
	if (!strcmp(del_path, fclip))
		fclip[0] = 0;
}

static void ask_delete(struct win *w)
{
	struct files *f = w->priv;
	char msg[NAME_MAX + 64];

	if (!sel_name(f))
		return;
	join(del_path, sizeof(del_path), f->path, sel_name(f));
	del_win = w;
	snprintf(msg, sizeof(msg), "Delete %s%s?\nThis cannot be undone.", sel_name(f),
		 f->ent[f->sel].dir ? " and everything in it" : "");
	open_message("Delete", msg, do_delete, NULL);
}

static void do_new(void *ctx, const char *name, int dir)
{
	struct win *w = ctx;
	struct files *f;
	char p[PATH_MAX * 2];
	int fd;

	if (!win_alive(w))
		return;
	f = w->priv;
	if (!valid_name(f, name)) {
		win_dirty(w);
		return;
	}
	join(p, sizeof(p), f->path, name);
	if (dir) {
		fd = mkdir(p, 0755);
	} else {
		fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd >= 0)
			close(fd);
	}
	if (fd < 0) {
		snprintf(f->msg, sizeof(f->msg), "%s: %s", name, strerror(errno));
		win_dirty(w);
		return;
	}
	f->msg[0] = 0;
	op_done(w, name);
	if (!dir)
		open_editor(p);
}

static void do_new_folder(void *ctx, const char *name) { do_new(ctx, name, 1); }
static void do_new_file(void *ctx, const char *name) { do_new(ctx, name, 0); }

static void clip_set(struct win *w, int cut)
{
	struct files *f = w->priv;

	if (!sel_name(f))
		return;
	join(fclip, sizeof(fclip), f->path, sel_name(f));
	fclip_cut = cut;
	snprintf(f->msg, sizeof(f->msg), "%s %s", cut ? "Cut" : "Copied", sel_name(f));
	win_dirty(w);
}

static void paste(struct win *w)
{
	struct files *f = w->priv;
	char dest[PATH_MAX * 2], name[NAME_MAX + 1];
	const char *base;
	int r;

	if (!fclip[0])
		return;
	if (access(fclip, F_OK) < 0) {
		snprintf(f->msg, sizeof(f->msg), "The copied file is gone");
		fclip[0] = 0;
		win_dirty(w);
		return;
	}
	base = strrchr(fclip, '/') + 1;
	snprintf(name, sizeof(name), "%s", base);
	join(dest, sizeof(dest), f->path, name);
	if (!strcmp(dest, fclip) && fclip_cut)
		return;
	/* "a.txt" -> "a (copy).txt", "a (copy 2).txt", ... */
	for (int i = 1; access(dest, F_OK) == 0 && i < 100; i++) {
		const char *dot = strrchr(base, '.');
		int stem = dot && dot != base ? (int)(dot - base) : (int)strlen(base);
		if (i == 1)
			snprintf(name, sizeof(name), "%.*s (copy)%s", stem, base, base + stem);
		else
			snprintf(name, sizeof(name), "%.*s (copy %d)%s", stem, base, i, base + stem);
		join(dest, sizeof(dest), f->path, name);
	}
	{
		size_t n = strlen(fclip);
		if (!strncmp(dest, fclip, n) && dest[n] == '/') {
			snprintf(f->msg, sizeof(f->msg), "Can't put a folder inside itself");
			win_dirty(w);
			return;
		}
	}
	if (fclip_cut) {
		if (rename(fclip, dest) == 0) {
			r = 0;
		} else {
			char *argv[] = { "/bin/mv", fclip, dest, NULL };
			r = run_wait(argv);
		}
		if (r == 0)
			fclip[0] = 0;
	} else {
		char *argv[] = { "/bin/cp", "-a", fclip, dest, NULL };
		r = run_wait(argv);
	}
	if (r)
		snprintf(f->msg, sizeof(f->msg), "Paste failed");
	else
		f->msg[0] = 0;
	op_done(w, name);
}

static void properties(struct win *w)
{
	struct files *f = w->priv;
	char p[PATH_MAX * 2], msg[512], size[32], when[64];
	struct stat st;

	if (!sel_name(f))
		return;
	join(p, sizeof(p), f->path, sel_name(f));
	if (lstat(p, &st) < 0)
		return;
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&st.st_mtime));
	size_str(size, sizeof(size), st.st_size);
	snprintf(msg, sizeof(msg),
		 "Name:      %s\nLocation:  %s\nType:      %s\nSize:      %s (%lld bytes)\n"
		 "Modified:  %s\nMode:      %04o, owner %d",
		 sel_name(f), f->path,
		 S_ISDIR(st.st_mode) ? "folder" : S_ISLNK(st.st_mode) ? "symbolic link" :
		 S_ISREG(st.st_mode) ? ((st.st_mode & 0111) ? "program" : "file") : "special file",
		 size, (long long)st.st_size, when, st.st_mode & 07777, (int)st.st_uid);
	open_message("Properties", msg, NULL, NULL);
}

static void toggle_hidden(struct win *w)
{
	struct files *f = w->priv;

	f->hidden = !f->hidden;
	files_reload(w);
}

static void item_menu_pick(void *ctx, int i)
{
	struct win *w = ctx;
	struct files *f;
	char p[PATH_MAX * 2];

	if (!win_alive(w))
		return;
	f = w->priv;
	if (f->sel < 0 || f->sel >= f->n)
		return;
	join(p, sizeof(p), f->path, f->ent[f->sel].name);
	switch (i) {
	case 0: files_open(w); break;
	case 1:
		if (f->ent[f->sel].dir)
			open_terminal_in(p, NULL);
		else
			open_editor(p);
		break;
	case 3: clip_set(w, 1); break;
	case 4: clip_set(w, 0); break;
	case 5: paste(w); break;
	case 7: ask_rename(w); break;
	case 8: ask_delete(w); break;
	case 10: properties(w); break;
	}
}

static void bg_menu_pick(void *ctx, int i)
{
	struct win *w = ctx;
	struct files *f;

	if (!win_alive(w))
		return;
	f = w->priv;
	switch (i) {
	case 0: open_prompt("New folder", "Folder name:", "New folder", do_new_folder, w); break;
	case 1: open_prompt("New file", "File name:", "new.txt", do_new_file, w); break;
	case 2: paste(w); break;
	case 4: open_terminal_in(f->path, NULL); break;
	case 5: toggle_hidden(w); break;
	case 6: files_reload(w); break;
	}
}

static void bg_menu(struct win *w, int x, int y)
{
	struct files *f = w->priv;
	const char *items[] = {
		"New folder...", "New file...", fclip[0] ? "Paste" : "!Paste", "-",
		"Open terminal here", f->hidden ? "Hide hidden files" : "Show hidden files", "Refresh",
	};
	popup_menu(x, y, items, 7, bg_menu_pick, w);
}

static void item_menu(struct win *w, int x, int y)
{
	struct files *f = w->priv;
	int dir = f->ent[f->sel].dir;
	int real = sel_name(f) != NULL;
	const char *items[] = {
		"Open", dir ? "Open terminal here" : "Open in editor", "-",
		real ? "Cut" : "!Cut", real ? "Copy" : "!Copy",
		fclip[0] ? "Paste" : "!Paste", "-",
		real ? "Rename...      F2" : "!Rename...      F2",
		real ? "Delete        Del" : "!Delete        Del", "-",
		real ? "Properties" : "!Properties",
	};
	popup_menu(x, y, items, 11, item_menu_pick, w);
}

static void files_scroll_to_sel(struct win *w)
{
	struct files *f = w->priv;
	int rows = list_rows(w);

	if (f->sel < f->top)
		f->top = f->sel;
	if (f->sel >= f->top + rows)
		f->top = f->sel - rows + 1;
	if (f->top < 0)
		f->top = 0;
	win_dirty(w);
}

static void files_key(struct win *w, const struct key *k)
{
	struct files *f = w->priv;
	int rows = list_rows(w);

	f->msg[0] = 0;
	if (k->mods & MOD_CTRL) {
		switch (k->code) {
		case KEY_C: clip_set(w, 0); return;
		case KEY_X: clip_set(w, 1); return;
		case KEY_V: paste(w); return;
		case KEY_H: toggle_hidden(w); return;
		case KEY_N:
			open_prompt("New folder", "Folder name:", "New folder", do_new_folder, w);
			return;
		}
	}
	switch (k->code) {
	case KEY_UP: if (f->sel > 0) f->sel--; break;
	case KEY_DOWN: if (f->sel < f->n - 1) f->sel++; break;
	case KEY_PAGEUP: f->sel = f->sel > rows ? f->sel - rows : 0; break;
	case KEY_PAGEDOWN: f->sel = f->sel + rows < f->n ? f->sel + rows : f->n - 1; break;
	case KEY_HOME: f->sel = 0; break;
	case KEY_END: f->sel = f->n - 1; break;
	case KEY_ENTER: case KEY_KPENTER: files_open(w); return;
	case KEY_BACKSPACE: files_cd(w, ".."); return;
	case KEY_F2: ask_rename(w); return;
	case KEY_DELETE: ask_delete(w); return;
	case KEY_F5: files_reload(w); return;
	default:
		/* a letter jumps to the next name starting with it */
		if (k->ch > ' ' && k->ch < 127 && !(k->mods & (MOD_CTRL | MOD_ALT))) {
			for (int j = 1; j <= f->n; j++) {
				int i = (f->sel + j) % f->n;
				if ((f->ent[i].name[0] | 32) == (k->ch | 32)) {
					f->sel = i;
					break;
				}
			}
			break;
		}
		return;
	}
	files_scroll_to_sel(w);
}

static void files_mouse(struct win *w, int x, int y, int ev, int arg)
{
	struct files *f = w->priv;
	int i = f->top + (y - BAR_H) / ROW_H;
	int on_item = y >= BAR_H && y < w->h - STATUS_H && i < f->n;

	if (ev == M_WHEEL) {
		f->top -= arg * 3;
		if (f->top > f->n - list_rows(w))
			f->top = f->n - list_rows(w);
		if (f->top < 0)
			f->top = 0;
		win_dirty(w);
		return;
	}
	if (ev == M_RIGHT) {
		if (on_item) {
			f->sel = i;
			win_dirty(w);
			item_menu(w, w->x + x, w->y + y);
		} else if (y >= BAR_H) {
			bg_menu(w, w->x + x, w->y + y);
		}
		return;
	}
	if (ev != M_DOWN && ev != M_DOUBLE)
		return;
	if (y < BAR_H) {
		if (x >= 4 && x < 44)
			files_cd(w, "..");
		else if (x >= 48 && x < 96)
			files_cd(w, "/root");
		else if (x >= 100 && x < 140)
			bg_menu(w, w->x + 100, w->y + BAR_H - 4);
		return;
	}
	if (!on_item)
		return;
	f->msg[0] = 0;
	f->sel = i;
	win_dirty(w);
	if (ev == M_DOUBLE)
		files_open(w);
}

/* pick up changes made elsewhere (a terminal, another window) */
static void files_tick(struct win *w)
{
	struct files *f = w->priv;
	struct stat st;

	if (stat(f->path, &st) == 0 &&
	    (st.st_mtim.tv_sec != f->mtime.tv_sec || st.st_mtim.tv_nsec != f->mtime.tv_nsec))
		files_reload(w);
}

static void files_close(struct win *w)
{
	struct files *f = w->priv;

	free(f->ent);
	free(f);
}

static const struct app_ops files_ops = {
	.draw = files_draw, .key = files_key, .mouse = files_mouse, .close = files_close,
	.tick = files_tick,
};

void open_files(const char *path)
{
	struct files *f = calloc(1, sizeof(*f));
	struct win *w;

	if (!f)
		return;
	snprintf(f->path, sizeof(f->path), "%s", path);
	w = win_new("Files", 500, 440, &files_ops);
	if (!w) {
		free(f);
		return;
	}
	w->priv = f;
	w->tick_ms = 1000;
	files_load(w);
}
