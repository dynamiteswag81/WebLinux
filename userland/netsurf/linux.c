/*
 * libnsfb surface for the Linux frame buffer (/dev/fb0) with evdev input
 * (/dev/input/event*), written for WebLinux.  Copied into libnsfb's
 * src/surface/ by build/build-netsurf.sh.
 *
 * The frame buffer is mapped and drawn into directly; the pointer is a
 * software cursor, handled like the SDL surface does.  Keys are reported
 * unshifted with separate modifier events: the NetSurf frontend applies
 * Shift itself.
 *
 * This file is licensed under the MIT licence, like libnsfb.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"
#include "libnsfb_plot_util.h"

#include "cursor.h"
#include "nsfb.h"
#include "plot.h"
#include "surface.h"

#define MAX_INPUT 8
#define QUEUE 64

struct lnx_priv {
    int fb;
    void *map;
    size_t maplen;
    int in[MAX_INPUT];
    int nin;
    int vt_mode;            /* KD_* mode to restore, or -1 */
    int abs_x, abs_y, moved;
    nsfb_event_t queue[QUEUE];
    int qhead, qtail;
};

static int lnx_defaults(nsfb_t *nsfb)
{
    nsfb->width = 0;
    nsfb->height = 0;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int lnx_initialise(nsfb_t *nsfb)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    struct lnx_priv *p;
    char path[32];
    int i;

    p = calloc(1, sizeof(*p));
    if (p == NULL)
        return -1;
    p->vt_mode = -1;
    p->fb = open("/dev/fb0", O_RDWR);
    if (p->fb < 0 ||
        ioctl(p->fb, FBIOGET_VSCREENINFO, &v) < 0 ||
        ioctl(p->fb, FBIOGET_FSCREENINFO, &f) < 0 ||
        v.bits_per_pixel != 32) {
        fprintf(stderr, "linux surface: no usable 32 bpp /dev/fb0\n");
        if (p->fb >= 0)
            close(p->fb);
        free(p);
        return -1;
    }
    p->maplen = f.smem_len;
    p->map = mmap(NULL, p->maplen, PROT_READ | PROT_WRITE, MAP_SHARED, p->fb, 0);
    if (p->map == MAP_FAILED) {
        perror("linux surface: mmap /dev/fb0");
        close(p->fb);
        free(p);
        return -1;
    }

    /* the frame buffer decides the geometry */
    nsfb->width = v.xres;
    nsfb->height = v.yres;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    nsfb->ptr = p->map;
    nsfb->linelen = f.line_length;

    for (i = 0; i < 16 && p->nin < MAX_INPUT; i++) {
        int fd;
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0)
            p->in[p->nin++] = fd;
    }

    /* keep the text console from drawing over us */
    {
        int mode;
        if (ioctl(0, KDGETMODE, &mode) == 0 && mode != KD_GRAPHICS) {
            p->vt_mode = mode;
            ioctl(0, KDSETMODE, KD_GRAPHICS);
        }
    }

    p->abs_x = p->abs_y = -1;
    nsfb->surface_priv = p;
    return 0;
}

static int lnx_finalise(nsfb_t *nsfb)
{
    struct lnx_priv *p = nsfb->surface_priv;
    int i;

    if (p == NULL)
        return 0;
    if (p->vt_mode >= 0)
        ioctl(0, KDSETMODE, p->vt_mode);
    for (i = 0; i < p->nin; i++)
        close(p->in[i]);
    munmap(p->map, p->maplen);
    close(p->fb);
    free(p);
    nsfb->surface_priv = NULL;
    nsfb->ptr = NULL;
    return 0;
}

static int lnx_set_geometry(nsfb_t *nsfb, int width, int height,
                            enum nsfb_format_e format)
{
    /* before initialise: remember the request; afterwards the frame
     * buffer's geometry is fixed */
    if (nsfb->surface_priv != NULL)
        return (width <= 0 || width == nsfb->width) &&
               (height <= 0 || height == nsfb->height) ? 0 : -1;
    if (width > 0)
        nsfb->width = width;
    if (height > 0)
        nsfb->height = height;
    if (format != NSFB_FMT_ANY && format != NSFB_FMT_XRGB8888)
        return -1;
    return 0;
}

/* Linux key code -> libnsfb key code (unshifted) */
static enum nsfb_key_code_e lnx_key(int code)
{
    static const char map[] = {
        [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4', [KEY_5] = '5',
        [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8', [KEY_9] = '9', [KEY_0] = '0',
        [KEY_MINUS] = '-', [KEY_EQUAL] = '=', [KEY_Q] = 'q', [KEY_W] = 'w',
        [KEY_E] = 'e', [KEY_R] = 'r', [KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u',
        [KEY_I] = 'i', [KEY_O] = 'o', [KEY_P] = 'p', [KEY_LEFTBRACE] = '[',
        [KEY_RIGHTBRACE] = ']', [KEY_A] = 'a', [KEY_S] = 's', [KEY_D] = 'd',
        [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h', [KEY_J] = 'j', [KEY_K] = 'k',
        [KEY_L] = 'l', [KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '\'',
        [KEY_GRAVE] = '`', [KEY_BACKSLASH] = '\\', [KEY_Z] = 'z', [KEY_X] = 'x',
        [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b', [KEY_N] = 'n', [KEY_M] = 'm',
        [KEY_COMMA] = ',', [KEY_DOT] = '.', [KEY_SLASH] = '/', [KEY_SPACE] = ' ',
    };

    if (code < (int)sizeof(map) && map[code])
        return (enum nsfb_key_code_e)map[code];
    switch (code) {
    case KEY_ESC: return NSFB_KEY_ESCAPE;
    case KEY_BACKSPACE: return NSFB_KEY_BACKSPACE;
    case KEY_TAB: return NSFB_KEY_TAB;
    case KEY_ENTER: return NSFB_KEY_RETURN;
    case KEY_KPENTER: return NSFB_KEY_KP_ENTER;
    case KEY_DELETE: return NSFB_KEY_DELETE;
    case KEY_LEFTSHIFT: return NSFB_KEY_LSHIFT;
    case KEY_RIGHTSHIFT: return NSFB_KEY_RSHIFT;
    case KEY_LEFTCTRL: return NSFB_KEY_LCTRL;
    case KEY_RIGHTCTRL: return NSFB_KEY_RCTRL;
    case KEY_LEFTALT: return NSFB_KEY_LALT;
    case KEY_RIGHTALT: return NSFB_KEY_RALT;
    case KEY_LEFTMETA: return NSFB_KEY_LSUPER;
    case KEY_RIGHTMETA: return NSFB_KEY_RSUPER;
    case KEY_CAPSLOCK: return NSFB_KEY_CAPSLOCK;
    case KEY_UP: return NSFB_KEY_UP;
    case KEY_DOWN: return NSFB_KEY_DOWN;
    case KEY_LEFT: return NSFB_KEY_LEFT;
    case KEY_RIGHT: return NSFB_KEY_RIGHT;
    case KEY_HOME: return NSFB_KEY_HOME;
    case KEY_END: return NSFB_KEY_END;
    case KEY_PAGEUP: return NSFB_KEY_PAGEUP;
    case KEY_PAGEDOWN: return NSFB_KEY_PAGEDOWN;
    case KEY_INSERT: return NSFB_KEY_INSERT;
    case KEY_KPMINUS: return NSFB_KEY_KP_MINUS;
    case KEY_KPPLUS: return NSFB_KEY_KP_PLUS;
    case KEY_KPASTERISK: return NSFB_KEY_KP_MULTIPLY;
    case KEY_KPSLASH: return NSFB_KEY_KP_DIVIDE;
    case KEY_KPDOT: return NSFB_KEY_KP_PERIOD;
    case KEY_KP0: return NSFB_KEY_KP0;
    case KEY_KP1: return NSFB_KEY_KP1;
    case KEY_KP2: return NSFB_KEY_KP2;
    case KEY_KP3: return NSFB_KEY_KP3;
    case KEY_KP4: return NSFB_KEY_KP4;
    case KEY_KP5: return NSFB_KEY_KP5;
    case KEY_KP6: return NSFB_KEY_KP6;
    case KEY_KP7: return NSFB_KEY_KP7;
    case KEY_KP8: return NSFB_KEY_KP8;
    case KEY_KP9: return NSFB_KEY_KP9;
    }
    if (code >= KEY_F1 && code <= KEY_F10)
        return NSFB_KEY_F1 + (code - KEY_F1);
    if (code == KEY_F11)
        return NSFB_KEY_F11;
    if (code == KEY_F12)
        return NSFB_KEY_F12;
    return NSFB_KEY_UNKNOWN;
}

static void push(struct lnx_priv *p, enum nsfb_event_type_e type, int a, int b)
{
    nsfb_event_t *e;

    if ((p->qhead + 1) % QUEUE == p->qtail)
        return;
    e = &p->queue[p->qhead];
    memset(e, 0, sizeof(*e));
    e->type = type;
    if (type == NSFB_EVENT_MOVE_ABSOLUTE) {
        e->value.vector.x = a;
        e->value.vector.y = b;
    } else {
        e->value.keycode = a;
    }
    p->qhead = (p->qhead + 1) % QUEUE;
}

static void translate(nsfb_t *nsfb, struct lnx_priv *p, const struct input_event *ev)
{
    enum nsfb_event_type_e kt = ev->value ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;

    switch (ev->type) {
    case EV_KEY:
        if (ev->code == BTN_LEFT)
            push(p, kt, NSFB_KEY_MOUSE_1, 0);
        else if (ev->code == BTN_MIDDLE)
            push(p, kt, NSFB_KEY_MOUSE_2, 0);
        else if (ev->code == BTN_RIGHT)
            push(p, kt, NSFB_KEY_MOUSE_3, 0);
        else if (ev->code < BTN_MISC) {
            enum nsfb_key_code_e k = lnx_key(ev->code);
            /* value 2 is autorepeat: a fresh press for the frontend */
            if (k != NSFB_KEY_UNKNOWN)
                push(p, kt, k, 0);
        }
        break;
    case EV_REL:
        if (ev->code == REL_WHEEL && ev->value) {
            int k = ev->value > 0 ? NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
            push(p, NSFB_EVENT_KEY_DOWN, k, 0);
            push(p, NSFB_EVENT_KEY_UP, k, 0);
        }
        break;
    case EV_ABS:
        if (ev->code == ABS_X)
            p->abs_x = ev->value, p->moved = 1;
        else if (ev->code == ABS_Y)
            p->abs_y = ev->value, p->moved = 1;
        break;
    case EV_SYN:
        if (p->moved && p->abs_x >= 0 && p->abs_y >= 0) {
            push(p, NSFB_EVENT_MOVE_ABSOLUTE,
                 (int)((long long)p->abs_x * (nsfb->width - 1) / 32767),
                 (int)((long long)p->abs_y * (nsfb->height - 1) / 32767));
        }
        p->moved = 0;
        break;
    }
}

static bool lnx_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct lnx_priv *p = nsfb->surface_priv;
    struct pollfd pfd[MAX_INPUT];
    int i, r;

    if (p == NULL)
        return false;
    for (;;) {
        if (p->qtail != p->qhead) {
            *event = p->queue[p->qtail];
            p->qtail = (p->qtail + 1) % QUEUE;
            return true;
        }
        for (i = 0; i < p->nin; i++) {
            pfd[i].fd = p->in[i];
            pfd[i].events = POLLIN;
            pfd[i].revents = 0;
        }
        r = poll(pfd, p->nin, timeout < 0 ? -1 : timeout);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            event->type = NSFB_EVENT_CONTROL;
            event->value.controlcode = NSFB_CONTROL_TIMEOUT;
            return true;
        }
        for (i = 0; i < p->nin; i++) {
            struct input_event ev[32];
            int n, j;
            if (!(pfd[i].revents & POLLIN))
                continue;
            n = read(p->in[i], ev, sizeof(ev));
            for (j = 0; j < n / (int)sizeof(ev[0]); j++)
                translate(nsfb, p, &ev[j]);
        }
        /* only poll once per call when the caller does not want to wait */
        if (p->qtail == p->qhead && timeout == 0) {
            event->type = NSFB_EVENT_CONTROL;
            event->value.controlcode = NSFB_CONTROL_TIMEOUT;
            return true;
        }
    }
}

static int lnx_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;

    if (cursor != NULL && cursor->plotted &&
        nsfb_plot_bbox_intersect(box, &cursor->loc))
        nsfb_cursor_clear(nsfb, cursor);
    return 0;
}

static int lnx_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    if (cursor != NULL && cursor->plotted) {
        nsfb_cursor_clear(nsfb, cursor);
        nsfb_cursor_plot(nsfb, cursor);
    }
    return true;
}

static int lnx_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;

    (void)box;
    /* drawing went straight to the frame buffer: only the cursor */
    if (cursor != NULL && !cursor->plotted)
        nsfb_cursor_plot(nsfb, cursor);
    return 0;
}

const nsfb_surface_rtns_t linux_rtns = {
    .defaults = lnx_defaults,
    .initialise = lnx_initialise,
    .finalise = lnx_finalise,
    .input = lnx_input,
    .claim = lnx_claim,
    .update = lnx_update,
    .cursor = lnx_cursor,
    .geometry = lnx_set_geometry,
};

NSFB_SURFACE_DEF(linux, NSFB_SURFACE_LINUX, &linux_rtns)
