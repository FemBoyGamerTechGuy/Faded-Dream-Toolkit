#define _GNU_SOURCE
/*
 * wayland.c — FDK Wayland platform backend
 *
 * Keyboard translation uses xkbcommon (MIT licensed, separate from FDK's license) — loads the
 * compositor-provided keymap so every locale/layout works correctly.
 *
 * Event loop design:
 *   wl_wait_event  — blocks with wl_display_dispatch() (reads + dispatches)
 *   wl_poll_event  — non-blocking: dispatch_pending only, no socket read
 *   Both push translated events into the evq ring buffer.
 */
#include "platform_internal.h"
#include "../core/core_internal.h"

#ifdef FDK_HAVE_WAYLAND

#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "xdg-shell-client-protocol.h"

#include <xkbcommon/xkbcommon.h>
#include <wayland-cursor.h>

#ifdef FDK_HAVE_OPENGL
#  include <EGL/egl.h>
#  include <EGL/eglext.h>
#  include <wayland-egl.h>
#endif

#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>

/* ─── Platform-private window ─────────────────────────────────────────────── */
struct FDK_PlatformWindow {
    struct wl_surface    *surface;
    struct xdg_surface   *xdg_surface;
    struct xdg_toplevel  *xdg_toplevel;

    /* Double-buffered SHM — back is what we draw into,
     * front is what the compositor is currently displaying */
    struct wl_shm_pool   *shm_pool;       /* single pool covering both bufs */
    struct wl_buffer     *shm_buf[2];     /* [0] and [1] */
    uint32_t             *shm_pixels[2];  /* mapped pixel pointers          */
    int                   shm_back;       /* index of back buffer (0 or 1)  */
    bool                  shm_buf_busy[2];/* true while compositor holds it */
    int                   shm_fd;
    size_t                shm_size;       /* size of ONE buffer              */
    size_t                shm_pool_size;  /* total pool size (2x shm_size)  */

    int                   w, h;
    int                   stride_px;
    bool                  configured;
    bool                  close_requested;
#ifdef FDK_HAVE_OPENGL
    struct wl_egl_window *egl_win;
    EGLSurface            egl_surface;
    EGLContext            egl_ctx;
#endif
};

/* ─── Global Wayland state ─────────────────────────────────────────────────── */
static struct wl_display    *s_dpy        = NULL;
static struct wl_registry   *s_registry   = NULL;
static struct wl_compositor *s_compositor = NULL;
static struct wl_shm        *s_shm        = NULL;
static struct xdg_wm_base   *s_xdg_wm    = NULL;
static struct wl_seat        *s_seat      = NULL;
static struct wl_pointer     *s_pointer   = NULL;
static struct wl_keyboard    *s_keyboard  = NULL;
/* Surface → FDK_Window lookup — supports up to 32 windows */
#define FDK_MAX_WINDOWS 32
typedef struct { struct wl_surface *surface; FDK_Window *fdkw; } WinEntry;
static WinEntry s_windows[FDK_MAX_WINDOWS];
static int      s_win_count = 0;
static FDK_Window *s_last_fdkwin = NULL; /* convenience: most recently created */

static void win_register(struct wl_surface *surf, FDK_Window *fdkw)
{
    if (s_win_count < FDK_MAX_WINDOWS) {
        s_windows[s_win_count].surface = surf;
        s_windows[s_win_count].fdkw    = fdkw;
        s_win_count++;
    }
    s_last_fdkwin = fdkw;
}

static void win_unregister(struct wl_surface *surf)
{
    for (int i = 0; i < s_win_count; i++) {
        if (s_windows[i].surface == surf) {
            s_windows[i] = s_windows[--s_win_count];
            return;
        }
    }
}

static FDK_Window *win_lookup(struct wl_surface *surf)
{
    for (int i = 0; i < s_win_count; i++)
        if (s_windows[i].surface == surf)
            return s_windows[i].fdkw;
    return s_last_fdkwin; /* fallback for events without surface context */
}

/* ─── XKB state ────────────────────────────────────────────────────────────── */
static struct xkb_context *s_xkb_ctx   = NULL;
static struct xkb_keymap  *s_xkb_map   = NULL;
static struct xkb_state   *s_xkb_state = NULL;

/* ─── Cursor ───────────────────────────────────────────────────────────────── */
static struct wl_cursor_theme  *s_cursor_theme  = NULL;
static struct wl_surface       *s_cursor_surface = NULL;

/* Load a named cursor and set it on the pointer */
static void set_cursor(struct wl_pointer *ptr, uint32_t serial,
                       const char *name)
{
    if (!s_cursor_theme || !ptr) return;
    struct wl_cursor *cur = wl_cursor_theme_get_cursor(s_cursor_theme, name);
    if (!cur || cur->image_count == 0) return;
    struct wl_cursor_image  *img = cur->images[0];
    struct wl_buffer        *buf = wl_cursor_image_get_buffer(img);
    if (!buf) return;
    wl_pointer_set_cursor(ptr, serial, s_cursor_surface,
                          img->hotspot_x, img->hotspot_y);
    wl_surface_attach(s_cursor_surface, buf, 0, 0);
    wl_surface_damage(s_cursor_surface, 0, 0, img->width, img->height);
    wl_surface_commit(s_cursor_surface);
}

/* ─── Clipboard ────────────────────────────────────────────────────────────── */
static struct wl_data_device_manager *s_data_mgr    = NULL;
static struct wl_data_device         *s_data_device = NULL;
static struct wl_data_source         *s_data_src    = NULL;
static char                          *s_clipboard    = NULL; /* owned copy  */
static uint32_t                       s_kbd_serial   = 0;    /* last key serial */

/* data_source listeners */
static void ds_target(void *d, struct wl_data_source *src, const char *mime) { (void)d;(void)src;(void)mime; }
static void ds_send(void *d, struct wl_data_source *src, const char *mime, int32_t fd)
{
    (void)d;(void)src;(void)mime;
    if (s_clipboard) {
        size_t len = strlen(s_clipboard);
        size_t written = 0;
        while (written < len) {
            ssize_t n = write(fd, s_clipboard + written, len - written);
            if (n <= 0) break;
            written += n;
        }
    }
    close(fd);
}
static void ds_cancelled(void *d, struct wl_data_source *src)
{
    (void)d;
    wl_data_source_destroy(src);
    if (s_data_src == src) s_data_src = NULL;
}
static void ds_dnd_drop(void *d, struct wl_data_source *s) { (void)d;(void)s; }
static void ds_dnd_finished(void *d, struct wl_data_source *s) { (void)d;(void)s; }
static void ds_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d;(void)s;(void)a; }
static const struct wl_data_source_listener s_ds_listener = {
    .target        = ds_target,
    .send          = ds_send,
    .cancelled     = ds_cancelled,
    .dnd_drop_performed = ds_dnd_drop,
    .dnd_finished  = ds_dnd_finished,
    .action        = ds_action,
};

/* data_offer (incoming paste) */
static struct wl_data_offer *s_paste_offer = NULL;
static void do_target(void *d, struct wl_data_offer *o, const char *mime) { (void)d;(void)o;(void)mime; }
static void do_source_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d;(void)o;(void)a; }
static void do_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d;(void)o;(void)a; }
static const struct wl_data_offer_listener s_do_listener = {
    .offer          = do_target,
    .source_actions = do_source_actions,
    .action         = do_action,
};

/* data_device listeners */
static void dd_data_offer(void *d, struct wl_data_device *dev,
                           struct wl_data_offer *offer)
{
    (void)d;(void)dev;
    wl_data_offer_add_listener(offer, &s_do_listener, NULL);
    s_paste_offer = offer;
}
static void dd_enter(void *d, struct wl_data_device *dev, uint32_t s,
                     struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y,
                     struct wl_data_offer *o) { (void)d;(void)dev;(void)s;(void)surf;(void)x;(void)y;(void)o; }
static void dd_leave(void *d, struct wl_data_device *dev) { (void)d;(void)dev; }
static void dd_motion(void *d, struct wl_data_device *dev, uint32_t t,
                      wl_fixed_t x, wl_fixed_t y) { (void)d;(void)dev;(void)t;(void)x;(void)y; }
static void dd_drop(void *d, struct wl_data_device *dev) { (void)d;(void)dev; }
static void dd_selection(void *d, struct wl_data_device *dev,
                          struct wl_data_offer *offer) { (void)d;(void)dev; s_paste_offer = offer; }
static const struct wl_data_device_listener s_dd_listener = {
    .data_offer = dd_data_offer,
    .enter      = dd_enter,
    .leave      = dd_leave,
    .motion     = dd_motion,
    .drop       = dd_drop,
    .selection  = dd_selection,
};

/* Current modifier state */
static FDK_Modifier s_mods = FDK_MOD_NONE;

/* Current pointer position */
static int32_t s_ptr_x = 0, s_ptr_y = 0;

#ifdef FDK_HAVE_OPENGL
static EGLDisplay s_egl_dpy    = EGL_NO_DISPLAY;
static EGLConfig  s_egl_cfg    = NULL;
static EGLContext s_egl_shared = EGL_NO_CONTEXT; /* one context shared by all windows */
#endif

/* ─── Event queue ──────────────────────────────────────────────────────────── */
#define EVQ_SIZE 256
static FDK_Event s_evq[EVQ_SIZE];
static int s_evq_head = 0, s_evq_tail = 0;

static void evq_push_ev(const FDK_Event *ev)
{
    int next = (s_evq_tail + 1) % EVQ_SIZE;
    if (next == s_evq_head) return; /* drop on overflow */
    s_evq[s_evq_tail] = *ev;
    s_evq_tail = next;
}

static bool evq_pop(FDK_Event *out)
{
    if (s_evq_head == s_evq_tail) return false;
    *out = s_evq[s_evq_head];
    s_evq_head = (s_evq_head + 1) % EVQ_SIZE;
    return true;
}

/* ─── xdg_wm_base ping ─────────────────────────────────────────────────────── */
static void xdg_wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial)
{ (void)d; xdg_wm_base_pong(wm, serial); }

static const struct xdg_wm_base_listener s_xdg_wm_listener = { .ping = xdg_wm_ping };

/* ─── SHM helpers ──────────────────────────────────────────────────────────── */
static int shm_create_anon(size_t size)
{
    int fd = memfd_create("fdk-shm", MFD_CLOEXEC);
    if (fd < 0) { perror("[FDK/Wayland] memfd_create"); return -1; }
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

/* Buffer-release callback — marks the buffer as free for reuse */
static void shm_buffer_release(void *data, struct wl_buffer *buf)
{
    FDK_PlatformWindow *pw = data;
    for (int i = 0; i < 2; i++)
        if (pw->shm_buf[i] == buf)
            pw->shm_buf_busy[i] = false;
}
static const struct wl_buffer_listener s_shm_buf_listener = {
    .release = shm_buffer_release,
};

static bool create_shm_buffers(FDK_PlatformWindow *pw, int w, int h)
{
    pw->stride_px     = w;
    pw->shm_size      = (size_t)w * h * 4;
    pw->shm_pool_size = pw->shm_size * 2;

    pw->shm_fd = shm_create_anon(pw->shm_pool_size);
    if (pw->shm_fd < 0) return false;

    /* Map the entire pool — both buffers are contiguous in this mapping */
    uint8_t *base = mmap(NULL, pw->shm_pool_size,
                         PROT_READ | PROT_WRITE, MAP_SHARED, pw->shm_fd, 0);
    if (base == MAP_FAILED) { close(pw->shm_fd); pw->shm_fd = -1; return false; }

    pw->shm_pixels[0] = (uint32_t *)base;
    pw->shm_pixels[1] = (uint32_t *)(base + pw->shm_size);

    pw->shm_pool = wl_shm_create_pool(s_shm, pw->shm_fd,
                                       (int32_t)pw->shm_pool_size);

    for (int i = 0; i < 2; i++) {
        pw->shm_buf[i] = wl_shm_pool_create_buffer(
            pw->shm_pool,
            (int32_t)(i * pw->shm_size), /* offset into pool */
            w, h, w * 4,
            WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(pw->shm_buf[i], &s_shm_buf_listener, pw);
        pw->shm_buf_busy[i] = false;
    }

    pw->shm_back = 0;
    return true;
}

static void shm_destroy_buffers(FDK_PlatformWindow *pw)
{
    for (int i = 0; i < 2; i++) {
        if (pw->shm_buf[i]) {
            wl_buffer_destroy(pw->shm_buf[i]);
            pw->shm_buf[i] = NULL;
        }
    }
    if (pw->shm_pool) { wl_shm_pool_destroy(pw->shm_pool); pw->shm_pool = NULL; }
    if (pw->shm_pixels[0]) {
        munmap(pw->shm_pixels[0], pw->shm_pool_size);
        pw->shm_pixels[0] = pw->shm_pixels[1] = NULL;
    }
    if (pw->shm_fd >= 0) { close(pw->shm_fd); pw->shm_fd = -1; }
}

static void wl_shm_resize(FDK_PlatformWindow *pw, int w, int h)
{
    if (pw->w == w && pw->h == h) return;
    if (w <= 0 || h <= 0) return;

    pw->w = w;
    pw->h = h;

#ifdef FDK_HAVE_OPENGL
    if (pw->egl_win) {
        wl_egl_window_resize(pw->egl_win, w, h, 0, 0);
        return;
    }
#endif

    shm_destroy_buffers(pw);
    create_shm_buffers(pw, w, h);
}

/* ─── xdg_surface / toplevel listeners ────────────────────────────────────── */
static void xdg_surface_configure(void *data, struct xdg_surface *xs,
                                   uint32_t serial)
{
    FDK_PlatformWindow *pw = data;
    xdg_surface_ack_configure(xs, serial);
    pw->configured = true;
    /* Synthetic expose so the app knows the window is ready to paint */
    FDK_Event ev = {0};
    ev.type   = FDK_EVENT_EXPOSE;
    ev.window = win_lookup(pw->surface);
    evq_push_ev(&ev);
}

static const struct xdg_surface_listener s_xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *top,
                                int32_t w, int32_t h, struct wl_array *states)
{
    (void)top; (void)states;
    FDK_PlatformWindow *pw = data;
    if (w > 0 && h > 0) {
        wl_shm_resize(pw, w, h);
        FDK_Event ev = {0};
        ev.type     = FDK_EVENT_RESIZE;
        ev.window   = win_lookup(pw->surface);
        ev.resize.w = pw->w;
        ev.resize.h = pw->h;
        evq_push_ev(&ev);
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *top)
{
    (void)top;
    ((FDK_PlatformWindow*)data)->close_requested = true;
}

static const struct xdg_toplevel_listener s_toplevel_listener = {
    .configure = toplevel_configure,
    .close     = toplevel_close,
};

/* ─── Pointer events ───────────────────────────────────────────────────────── */
static struct wl_surface *s_ptr_surface = NULL;
static uint32_t           s_ptr_serial  = 0;

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)d;
    s_ptr_surface = surf;
    s_ptr_serial  = serial;
    s_ptr_x = wl_fixed_to_int(sx);
    s_ptr_y = wl_fixed_to_int(sy);
    set_cursor(p, serial, "default");
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s,
                      struct wl_surface *surf)
{ (void)d;(void)p;(void)s;(void)surf; s_ptr_surface = NULL; }

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t,
                       wl_fixed_t sx, wl_fixed_t sy)
{
    (void)d;(void)p;(void)t;
    s_ptr_x = wl_fixed_to_int(sx);
    s_ptr_y = wl_fixed_to_int(sy);
    FDK_Event ev = {0};
    ev.type      = FDK_EVENT_MOUSE_MOVE;
    ev.window    = win_lookup(s_ptr_surface);
    ev.motion.x  = s_ptr_x;
    ev.motion.y  = s_ptr_y;
    ev.motion.mods = s_mods;
    evq_push_ev(&ev);
}

static uint32_t s_last_serial = 0;
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                       uint32_t t, uint32_t button, uint32_t state)
{
    (void)d;(void)p;(void)serial;(void)t;
    s_last_serial = serial;
    FDK_MouseButton btn;
    switch (button) {
    case 0x110: btn = FDK_BUTTON_LEFT;   break;
    case 0x111: btn = FDK_BUTTON_RIGHT;  break;
    case 0x112: btn = FDK_BUTTON_MIDDLE; break;
    default: return;
    }
    FDK_Event ev = {0};
    ev.type         = (state == WL_POINTER_BUTTON_STATE_PRESSED)
                          ? FDK_EVENT_MOUSE_DOWN : FDK_EVENT_MOUSE_UP;
    ev.window       = win_lookup(s_ptr_surface);
    ev.mouse.x      = s_ptr_x;
    ev.mouse.y      = s_ptr_y;
    ev.mouse.button = btn;
    ev.mouse.mods   = s_mods;
    evq_push_ev(&ev);
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t,
                     uint32_t axis, wl_fixed_t value)
{
    (void)d;(void)p;(void)t;
    FDK_Event ev = {0};
    ev.type   = FDK_EVENT_MOUSE_SCROLL;
    ev.window = win_lookup(s_ptr_surface);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        ev.scroll.dy = -wl_fixed_to_double(value) / 10.0f;
    else
        ev.scroll.dx = -wl_fixed_to_double(value) / 10.0f;
    evq_push_ev(&ev);
}

static void ptr_frame(void *d, struct wl_pointer *p) { (void)d;(void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d;(void)p;(void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d;(void)p;(void)t;(void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d;(void)p;(void)a;(void)v; }

static const struct wl_pointer_listener s_pointer_listener = {
    .enter         = ptr_enter,
    .leave         = ptr_leave,
    .motion        = ptr_motion,
    .button        = ptr_button,
    .axis          = ptr_axis,
    .frame         = ptr_frame,
    .axis_source   = ptr_axis_source,
    .axis_stop     = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
};

/* ─── Keyboard events (xkbcommon) ──────────────────────────────────────────── */
static void kbd_keymap(void *d, struct wl_keyboard *k,
                       uint32_t fmt, int32_t fd, uint32_t size)
{
    (void)d;(void)k;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }

    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map_str == MAP_FAILED) return;

    /* Rebuild xkb state from compositor keymap — handles all layouts/locales */
    if (s_xkb_state) { xkb_state_unref(s_xkb_state); s_xkb_state = NULL; }
    if (s_xkb_map)   { xkb_keymap_unref(s_xkb_map);  s_xkb_map   = NULL; }

    s_xkb_map = xkb_keymap_new_from_string(s_xkb_ctx, map_str,
                    XKB_KEYMAP_FORMAT_TEXT_V1,
                    XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_str, size);
    if (!s_xkb_map) return;

    s_xkb_state = xkb_state_new(s_xkb_map);
}

static struct wl_surface *s_kbd_surface = NULL;

static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t s,
                      struct wl_surface *surf, struct wl_array *keys)
{ (void)d;(void)k;(void)s;(void)keys; s_kbd_surface = surf; }

static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t s,
                      struct wl_surface *surf)
{ (void)d;(void)k;(void)s;(void)surf; s_kbd_surface = NULL; }

/* Map xkb keysym to FDK_Key for special keys */
static FDK_Key keysym_to_fdk(xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_Escape:    return FDK_KEY_ESCAPE;
    case XKB_KEY_Return:    return FDK_KEY_RETURN;
    case XKB_KEY_space:     return FDK_KEY_SPACE;
    case XKB_KEY_BackSpace: return FDK_KEY_BACKSPACE;
    case XKB_KEY_Tab:       return FDK_KEY_TAB;
    case XKB_KEY_Delete:    return FDK_KEY_DELETE;
    case XKB_KEY_Left:      return FDK_KEY_LEFT;
    case XKB_KEY_Right:     return FDK_KEY_RIGHT;
    case XKB_KEY_Up:        return FDK_KEY_UP;
    case XKB_KEY_Down:      return FDK_KEY_DOWN;
    case XKB_KEY_Home:      return FDK_KEY_HOME;
    case XKB_KEY_End:       return FDK_KEY_END;
    case XKB_KEY_Page_Up:   return FDK_KEY_PAGEUP;
    case XKB_KEY_Page_Down: return FDK_KEY_PAGEDOWN;
    case XKB_KEY_F1:        return FDK_KEY_F1;
    case XKB_KEY_F2:        return FDK_KEY_F2;
    case XKB_KEY_F3:        return FDK_KEY_F3;
    case XKB_KEY_F4:        return FDK_KEY_F4;
    case XKB_KEY_F5:        return FDK_KEY_F5;
    case XKB_KEY_F6:        return FDK_KEY_F6;
    case XKB_KEY_F7:        return FDK_KEY_F7;
    case XKB_KEY_F8:        return FDK_KEY_F8;
    case XKB_KEY_F9:        return FDK_KEY_F9;
    case XKB_KEY_F10:       return FDK_KEY_F10;
    case XKB_KEY_F11:       return FDK_KEY_F11;
    case XKB_KEY_F12:       return FDK_KEY_F12;
    default:
        if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) return (FDK_Key)sym;
        if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) return (FDK_Key)(sym - XKB_KEY_A + XKB_KEY_a);
        return FDK_KEY_UNKNOWN;
    }
}

static void kbd_key(void *d, struct wl_keyboard *k, uint32_t serial,
                    uint32_t t, uint32_t key, uint32_t state)
{
    (void)d;(void)k;(void)t;
    s_last_serial = serial;   /* keep fresh for clipboard set_selection */
    if (!s_xkb_state) return;

    /* Wayland key codes are evdev codes; xkb uses keycode+8 */
    xkb_keycode_t  keycode = key + 8;
    xkb_keysym_t   sym     = xkb_state_key_get_one_sym(s_xkb_state, keycode);

    FDK_Event ev = {0};
    ev.type    = (state == WL_KEYBOARD_KEY_STATE_PRESSED)
                     ? FDK_EVENT_KEY_DOWN : FDK_EVENT_KEY_UP;
    s_kbd_serial = 0; /* serial not directly available here — use seat serial */
    ev.window  = win_lookup(s_kbd_surface);
    ev.key.key = keysym_to_fdk(sym);
    ev.key.mods = s_mods;

    /* Get the Unicode codepoint for printable characters.
     * xkb_state_key_get_utf32 returns 0 for non-printable keys. */
    uint32_t cp = xkb_state_key_get_utf32(s_xkb_state, keycode);
    ev.key.codepoint = cp;

    evq_push_ev(&ev);
}

static void kbd_modifiers(void *d, struct wl_keyboard *k, uint32_t serial,
                          uint32_t mods_depressed, uint32_t mods_latched,
                          uint32_t mods_locked, uint32_t group)
{
    (void)d;(void)k;(void)serial;
    if (!s_xkb_state) return;
    xkb_state_update_mask(s_xkb_state,
                          mods_depressed, mods_latched, mods_locked,
                          0, 0, group);

    /* Update our cached modifier flags */
    s_mods = FDK_MOD_NONE;
    if (xkb_state_mod_name_is_active(s_xkb_state, XKB_MOD_NAME_SHIFT,
                                      XKB_STATE_MODS_EFFECTIVE) > 0)
        s_mods |= FDK_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(s_xkb_state, XKB_MOD_NAME_CTRL,
                                      XKB_STATE_MODS_EFFECTIVE) > 0)
        s_mods |= FDK_MOD_CTRL;
    if (xkb_state_mod_name_is_active(s_xkb_state, XKB_MOD_NAME_ALT,
                                      XKB_STATE_MODS_EFFECTIVE) > 0)
        s_mods |= FDK_MOD_ALT;
    if (xkb_state_mod_name_is_active(s_xkb_state, XKB_MOD_NAME_LOGO,
                                      XKB_STATE_MODS_EFFECTIVE) > 0)
        s_mods |= FDK_MOD_SUPER;
}

static void kbd_repeat_info(void *d, struct wl_keyboard *k,
                            int32_t rate, int32_t delay)
{ (void)d;(void)k;(void)rate;(void)delay; }

static const struct wl_keyboard_listener s_keyboard_listener = {
    .keymap      = kbd_keymap,
    .enter       = kbd_enter,
    .leave       = kbd_leave,
    .key         = kbd_key,
    .modifiers   = kbd_modifiers,
    .repeat_info = kbd_repeat_info,
};

/* ─── Seat ─────────────────────────────────────────────────────────────────── */
static void seat_capabilities(void *d, struct wl_seat *seat, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !s_pointer) {
        s_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(s_pointer, &s_pointer_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !s_keyboard) {
        s_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(s_keyboard, &s_keyboard_listener, NULL);
    }
    if (!(caps & WL_SEAT_CAPABILITY_POINTER) && s_pointer) {
        wl_pointer_destroy(s_pointer); s_pointer = NULL;
    }
    if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && s_keyboard) {
        wl_keyboard_destroy(s_keyboard); s_keyboard = NULL;
    }
}

static void seat_name(void *d, struct wl_seat *s, const char *n)
{ (void)d;(void)s;(void)n; }

static const struct wl_seat_listener s_seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

/* ─── Registry ─────────────────────────────────────────────────────────────── */
static void registry_global(void *d, struct wl_registry *reg,
                             uint32_t name, const char *iface, uint32_t version)
{
    (void)d;(void)version;
    if (!strcmp(iface, wl_compositor_interface.name))
        s_compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        s_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        s_xdg_wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(s_xdg_wm, &s_xdg_wm_listener, NULL);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        s_seat = wl_registry_bind(reg, name, &wl_seat_interface, 4);
        wl_seat_add_listener(s_seat, &s_seat_listener, NULL);
    } else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
        s_data_mgr = wl_registry_bind(reg, name,
                         &wl_data_device_manager_interface, 3);
    }
}

static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d;(void)r;(void)n; }

static const struct wl_registry_listener s_registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ─── Init / shutdown ──────────────────────────────────────────────────────── */
static bool wl_init(void)
{
    s_dpy = wl_display_connect(NULL);
    if (!s_dpy) {
        fprintf(stderr, "[FDK/Wayland] Cannot connect to display.\n");
        return false;
    }

    /* xkbcommon context — needed for keymap parsing */
    s_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!s_xkb_ctx) {
        fprintf(stderr, "[FDK/Wayland] xkb_context_new failed.\n");
        return false;
    }

    s_registry = wl_display_get_registry(s_dpy);
    wl_registry_add_listener(s_registry, &s_registry_listener, NULL);
    wl_display_roundtrip(s_dpy); /* collect globals */
    wl_display_roundtrip(s_dpy); /* collect seat capabilities + keymap */

    if (!s_compositor || !s_shm || !s_xdg_wm) {
        fprintf(stderr, "[FDK/Wayland] Missing globals.\n");
        return false;
    }

    /* Cursor theme — 24px, inherits from the environment */
    s_cursor_theme   = wl_cursor_theme_load(NULL, 24, s_shm);
    s_cursor_surface = wl_compositor_create_surface(s_compositor);

    /* Data device for clipboard */
    if (s_data_mgr && s_seat) {
        s_data_device = wl_data_device_manager_get_data_device(
                            s_data_mgr, s_seat);
        wl_data_device_add_listener(s_data_device, &s_dd_listener, NULL);
    }

#ifdef FDK_HAVE_OPENGL
    /* Initialise EGL against the Wayland display */
    s_egl_dpy = eglGetDisplay((EGLNativeDisplayType)s_dpy);
    if (s_egl_dpy != EGL_NO_DISPLAY) {
        EGLint major, minor;
        if (eglInitialize(s_egl_dpy, &major, &minor)) {
            eglBindAPI(EGL_OPENGL_API);
            EGLint attrs[] = {
                EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                EGL_RED_SIZE,   8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE,  8,
                EGL_ALPHA_SIZE, 8,
                EGL_DEPTH_SIZE, 0,
                EGL_NONE
            };
            EGLint n = 0;
            if (eglChooseConfig(s_egl_dpy, attrs, &s_egl_cfg, 1, &n) && n > 0) {
                EGLint ctx_attrs[] = {
                    EGL_CONTEXT_MAJOR_VERSION, 3,
                    EGL_CONTEXT_MINOR_VERSION, 3,
                    EGL_NONE
                };
                s_egl_shared = eglCreateContext(s_egl_dpy, s_egl_cfg,
                                                EGL_NO_CONTEXT, ctx_attrs);
                if (s_egl_shared == EGL_NO_CONTEXT)
                    fprintf(stderr, "[FDK/Wayland] EGL: GL 3.3 context failed, OpenGL unavailable\n");
            }
        }
    }
#endif

    return true;
}

static void wl_backend_shutdown(void)
{
#ifdef FDK_HAVE_OPENGL
    if (s_egl_shared != EGL_NO_CONTEXT) {
        eglDestroyContext(s_egl_dpy, s_egl_shared);
        s_egl_shared = EGL_NO_CONTEXT;
    }
    if (s_egl_dpy != EGL_NO_DISPLAY) {
        eglTerminate(s_egl_dpy);
        s_egl_dpy = EGL_NO_DISPLAY;
    }
#endif
    free(s_clipboard); s_clipboard = NULL;
    if (s_data_src)    { wl_data_source_destroy(s_data_src); s_data_src = NULL; }
    if (s_data_device) { wl_data_device_destroy(s_data_device); s_data_device = NULL; }
    if (s_data_mgr)    { wl_data_device_manager_destroy(s_data_mgr); s_data_mgr = NULL; }
    if (s_cursor_surface) { wl_surface_destroy(s_cursor_surface); s_cursor_surface = NULL; }
    if (s_cursor_theme)   { wl_cursor_theme_destroy(s_cursor_theme); s_cursor_theme = NULL; }
    if (s_xkb_state) { xkb_state_unref(s_xkb_state);   s_xkb_state = NULL; }
    if (s_xkb_map)   { xkb_keymap_unref(s_xkb_map);    s_xkb_map   = NULL; }
    if (s_xkb_ctx)   { xkb_context_unref(s_xkb_ctx);   s_xkb_ctx   = NULL; }
    if (s_pointer)   { wl_pointer_destroy(s_pointer);   s_pointer   = NULL; }
    if (s_keyboard)  { wl_keyboard_destroy(s_keyboard); s_keyboard  = NULL; }
    if (s_seat)      { wl_seat_destroy(s_seat);         s_seat      = NULL; }
    if (s_xdg_wm)    { xdg_wm_base_destroy(s_xdg_wm);  s_xdg_wm    = NULL; }
    if (s_shm)       { wl_shm_destroy(s_shm);           s_shm       = NULL; }
    if (s_compositor){ wl_compositor_destroy(s_compositor); s_compositor = NULL; }
    if (s_registry)  { wl_registry_destroy(s_registry); s_registry  = NULL; }
    if (s_dpy)       { wl_display_disconnect(s_dpy);    s_dpy       = NULL; }
}

/* ─── Window ───────────────────────────────────────────────────────────────── */
static FDK_PlatformWindow *wl_window_create(const FDK_WindowDesc *desc)
{
    FDK_PlatformWindow *pw = calloc(1, sizeof *pw);
    if (!pw) return NULL;
    pw->shm_fd = -1;
    pw->w = desc->w;
    pw->h = desc->h;

    pw->surface      = wl_compositor_create_surface(s_compositor);
    pw->xdg_surface  = xdg_wm_base_get_xdg_surface(s_xdg_wm, pw->surface);
    pw->xdg_toplevel = xdg_surface_get_toplevel(pw->xdg_surface);

    xdg_surface_add_listener(pw->xdg_surface,   &s_xdg_surface_listener, pw);
    xdg_toplevel_add_listener(pw->xdg_toplevel, &s_toplevel_listener,    pw);

    if (desc->title)
        xdg_toplevel_set_title(pw->xdg_toplevel, desc->title);
    if (!desc->resizable) {
        xdg_toplevel_set_min_size(pw->xdg_toplevel, desc->w, desc->h);
        xdg_toplevel_set_max_size(pw->xdg_toplevel, desc->w, desc->h);
    }

#ifdef FDK_HAVE_OPENGL
    bool use_gl = (desc->render == FDK_RENDER_OPENGL ||
                   desc->render == FDK_RENDER_AUTO) &&
                  s_egl_shared != EGL_NO_CONTEXT;
    if (use_gl) {
        /* EGL window surface — no SHM needed */
        pw->egl_win = wl_egl_window_create(pw->surface, desc->w, desc->h);
        pw->egl_surface = eglCreateWindowSurface(s_egl_dpy, s_egl_cfg,
                              (EGLNativeWindowType)pw->egl_win, NULL);
        pw->egl_ctx = s_egl_shared; /* share the global context */
        eglMakeCurrent(s_egl_dpy, pw->egl_surface, pw->egl_surface, pw->egl_ctx);
        eglSwapInterval(s_egl_dpy, 1); /* vsync on */
    } else {
        create_shm_buffers(pw, desc->w, desc->h);
    }
#else
    create_shm_buffers(pw, desc->w, desc->h);
#endif

    wl_surface_commit(pw->surface);
    wl_display_roundtrip(s_dpy);
    return pw;
}

static void wl_window_destroy(FDK_PlatformWindow *pw)
{
    if (!pw) return;
#ifdef FDK_HAVE_OPENGL
    if (pw->egl_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(s_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(s_egl_dpy, pw->egl_surface);
    }
    if (pw->egl_win) wl_egl_window_destroy(pw->egl_win);
#endif
    shm_destroy_buffers(pw);
    if (pw->xdg_toplevel) xdg_toplevel_destroy(pw->xdg_toplevel);
    if (pw->xdg_surface)  xdg_surface_destroy(pw->xdg_surface);
    if (pw->surface) {
        win_unregister(pw->surface);
        wl_surface_destroy(pw->surface);
    }
    free(pw);
}

static void wl_window_show(FDK_PlatformWindow *pw)
{
#ifdef FDK_HAVE_OPENGL
    if (pw->egl_surface != EGL_NO_SURFACE) {
        /* For GL windows just commit the surface — EGL handles presentation */
        wl_surface_commit(pw->surface);
        wl_display_flush(s_dpy);
        return;
    }
#endif
    if (!pw->shm_buf[pw->shm_back]) return;
    wl_surface_attach(pw->surface, pw->shm_buf[pw->shm_back], 0, 0);
    wl_surface_damage(pw->surface, 0, 0, pw->w, pw->h);
    wl_surface_commit(pw->surface);
    wl_display_flush(s_dpy);
}

static void wl_window_hide(FDK_PlatformWindow *pw)
{
    wl_surface_attach(pw->surface, NULL, 0, 0);
    wl_surface_commit(pw->surface);
}

static void wl_window_set_title(FDK_PlatformWindow *pw, const char *t)
{
    xdg_toplevel_set_title(pw->xdg_toplevel, t);
    wl_display_flush(s_dpy);
}

static FDK_Size wl_window_get_size(FDK_PlatformWindow *pw)
{
    return (FDK_Size){ pw->w, pw->h };
}

static void wl_window_request_redraw(FDK_PlatformWindow *pw)
{
    wl_surface_damage(pw->surface, 0, 0, pw->w, pw->h);
    wl_surface_commit(pw->surface);
    wl_display_flush(s_dpy);
}

static uint32_t *wl_get_framebuffer(FDK_PlatformWindow *pw, int *stride_px)
{
    *stride_px = pw->stride_px;
    return pw->shm_pixels[pw->shm_back];
}

static void wl_present(FDK_PlatformWindow *pw)
{
    if (!pw->shm_buf[pw->shm_back]) return;

    int front = pw->shm_back;
    /* Swap: next draw goes to the other buffer */
    pw->shm_back = 1 - front;
    pw->shm_buf_busy[front] = true;

    wl_surface_attach(pw->surface, pw->shm_buf[front], 0, 0);
    wl_surface_damage(pw->surface, 0, 0, pw->w, pw->h);
    wl_surface_commit(pw->surface);
    wl_display_flush(s_dpy);
}

static bool wl_gl_make_current(FDK_PlatformWindow *pw)
{
#ifdef FDK_HAVE_OPENGL
    if (pw->egl_surface == EGL_NO_SURFACE) return false;
    return eglMakeCurrent(s_egl_dpy, pw->egl_surface,
                          pw->egl_surface, pw->egl_ctx) == EGL_TRUE;
#else
    (void)pw; return false;
#endif
}

static void wl_gl_swap_buffers(FDK_PlatformWindow *pw)
{
#ifdef FDK_HAVE_OPENGL
    if (pw->egl_surface != EGL_NO_SURFACE)
        eglSwapBuffers(s_egl_dpy, pw->egl_surface);
#else
    (void)pw;
#endif
}

/* Write text to the Wayland clipboard */
static void wl_clipboard_set(const char *text)
{
    if (!s_data_mgr || !s_seat || !s_data_device || !text) return;
    if (s_last_serial == 0) return; /* no valid serial yet — ignore */

    /* Store locally — ds_send() will serve it when compositor asks */
    free(s_clipboard);
    s_clipboard = strdup(text);

    /* Replace old source */
    if (s_data_src) {
        wl_data_source_destroy(s_data_src);
        s_data_src = NULL;
    }

    s_data_src = wl_data_device_manager_create_data_source(s_data_mgr);
    if (!s_data_src) return;
    wl_data_source_add_listener(s_data_src, &s_ds_listener, NULL);
    wl_data_source_offer(s_data_src, "text/plain;charset=utf-8");
    wl_data_source_offer(s_data_src, "text/plain");
    wl_data_device_set_selection(s_data_device, s_data_src, s_last_serial);
    wl_display_flush(s_dpy);
}

/* Read text from the Wayland clipboard.
 *
 * Protocol flow:
 *   1. We call wl_data_offer_receive() — tells the compositor which fd to
 *      write into and which mime type we want.
 *   2. We flush our request to the compositor.
 *   3. The compositor calls ds_send() on the data source owner (could be us
 *      or another app) which writes into the write-end of our pipe.
 *   4. We read from the read-end.
 *
 * The blocking read() deadlock is avoided by:
 *   - Setting the read-end non-blocking so we can poll.
 *   - Calling wl_display_roundtrip() to let ds_send fire before we read.
 *   - Reading with a small retry loop.
 */
static char *wl_clipboard_get(void)
{
    if (!s_paste_offer) return NULL;

    const char *mimes[] = {
        "text/plain;charset=utf-8",
        "text/plain",
        NULL
    };

    for (int mi = 0; mimes[mi]; mi++) {
        int fds[2];
        if (pipe(fds) < 0) return NULL;

        /* Make read end non-blocking */
        fcntl(fds[0], F_SETFL, O_NONBLOCK);

        wl_data_offer_receive(s_paste_offer, mimes[mi], fds[1]);
        wl_display_flush(s_dpy);
        close(fds[1]);  /* close write end — compositor has its own copy */

        /* Give the compositor / data-source owner a chance to write.
         * wl_display_roundtrip sends our pending requests and waits for
         * the server to process them, which triggers ds_send(). */
        wl_display_roundtrip(s_dpy);

        /* Read with a short retry loop in case write is slightly delayed */
        char buf[65536];
        ssize_t total = 0;
        for (int attempts = 0; attempts < 10; attempts++) {
            ssize_t n = read(fds[0], buf + total,
                             sizeof(buf) - 1 - total);
            if (n > 0) {
                total += n;
                if (total >= (ssize_t)(sizeof(buf) - 1)) break;
            } else if (n == 0) {
                break; /* EOF */
            } else {
                /* EAGAIN — no data yet, yield briefly */
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct timespec ts = {0, 2000000}; /* 2ms */
                    nanosleep(&ts, NULL);
                } else {
                    break; /* real error */
                }
            }
        }
        close(fds[0]);

        if (total > 0) {
            buf[total] = '\0';
            return strdup(buf);
        }
    }
    return NULL;
}

static void wl_set_cursor(FDK_Cursor cursor)
{
    if (!s_pointer) return;
    const char *name;
    switch (cursor) {
    case FDK_CURSOR_POINTER:    name = "pointer";           break;
    case FDK_CURSOR_TEXT:       name = "text";              break;
    case FDK_CURSOR_CROSSHAIR:  name = "crosshair";         break;
    case FDK_CURSOR_MOVE:       name = "move";              break;
    case FDK_CURSOR_RESIZE_H:   name = "ew-resize";         break;
    case FDK_CURSOR_RESIZE_V:   name = "ns-resize";         break;
    case FDK_CURSOR_NOT_ALLOWED:name = "not-allowed";       break;
    default:                    name = "default";           break;
    }
    set_cursor(s_pointer, s_ptr_serial, name);
}

void wl_window_register(FDK_PlatformWindow *pw, FDK_Window *fdkw)
{
    win_register(pw->surface, fdkw);
}

/* ─── Events ───────────────────────────────────────────────────────────────── */

/*
 * Non-blocking poll: dispatch whatever is already in the local Wayland
 * buffer without reading the socket. Returns one event from the evq or false.
 *
 * IMPORTANT: never call wl_display_prepare_read() here — that would
 * conflict with wl_wait_event's use of wl_display_dispatch().
 */
static bool wl_poll_event(FDK_Event *out)
{
    wl_display_dispatch_pending(s_dpy);
    wl_display_flush(s_dpy);

    for (int _i = 0; _i < s_win_count; _i++) {
        FDK_Window *_fw = s_windows[_i].fdkw;
        if (!_fw) continue;
        FDK_PlatformWindow *pw = fdk_window_get_pw(_fw);
        if (pw && pw->close_requested) {
            pw->close_requested = false;
            out->type   = FDK_EVENT_CLOSE;
            out->window = _fw;
            return true;
        }
    }
    /* Dispatch any buffer release events so busy flags get cleared */
    wl_display_dispatch_pending(s_dpy);

    return evq_pop(out);
}

/*
 * Blocking wait: wl_display_dispatch() reads the socket AND dispatches,
 * then we try to pop from the evq. Loop until we get something.
 */
static void wl_wait_event(FDK_Event *out)
{
    for (;;) {
        /* Check what's already queued first */
        if (wl_poll_event(out))
            return;
        /* Block until compositor sends data, then dispatch callbacks */
        if (wl_display_dispatch(s_dpy) < 0)
            return;
    }
}

static bool check_close_requests(FDK_Event *out)
{
    for (int i = 0; i < s_win_count; i++) {
        FDK_Window *fw = s_windows[i].fdkw;
        if (!fw) continue;
        FDK_PlatformWindow *pw = fdk_window_get_pw(fw);
        if (pw && pw->close_requested) {
            pw->close_requested = false;
            out->type   = FDK_EVENT_CLOSE;
            out->window = fw;
            return true;
        }
    }
    return false;
}

static bool wl_wait_event_timeout(FDK_Event *out, int timeout_ms)
{
    for (;;) {
        wl_display_flush(s_dpy);
        wl_display_dispatch_pending(s_dpy);
        if (check_close_requests(out)) return true;
        if (evq_pop(out)) return true;

        /* Prepare to read socket */
        while (wl_display_prepare_read(s_dpy) != 0)
            wl_display_dispatch_pending(s_dpy);

        wl_display_flush(s_dpy);

        struct pollfd pfd = { wl_display_get_fd(s_dpy), POLLIN, 0 };
        int ret = poll(&pfd, 1, timeout_ms);

        if (ret > 0) {
            wl_display_read_events(s_dpy);
            wl_display_dispatch_pending(s_dpy);
            if (check_close_requests(out)) return true;
            if (evq_pop(out)) return true;
            /* For timed waits return after one socket read even if no FDK event */
            if (timeout_ms >= 0) return false;
        } else {
            wl_display_cancel_read(s_dpy);
            return false; /* timeout or error */
        }
    }
}

/* ─── VTable ───────────────────────────────────────────────────────────────── */
const FDK_PlatformVTable fdk_platform_wayland = {
    .name                  = "Wayland",
    .init                  = wl_init,
    .shutdown              = wl_backend_shutdown,
    .window_create         = wl_window_create,
    .window_destroy        = wl_window_destroy,
    .window_show           = wl_window_show,
    .window_hide           = wl_window_hide,
    .window_set_title      = wl_window_set_title,
    .window_get_size       = wl_window_get_size,
    .window_request_redraw = wl_window_request_redraw,
    .window_get_framebuffer= wl_get_framebuffer,
    .window_present        = wl_present,
    .gl_make_current       = wl_gl_make_current,
    .gl_swap_buffers       = wl_gl_swap_buffers,
    .poll_event            = wl_poll_event,
    .wait_event            = wl_wait_event,
    .wait_event_timeout    = wl_wait_event_timeout,
    .window_register       = wl_window_register,
    .set_cursor            = wl_set_cursor,
    .clipboard_set         = wl_clipboard_set,
    .clipboard_get         = wl_clipboard_get,
};

#endif /* FDK_HAVE_WAYLAND */
