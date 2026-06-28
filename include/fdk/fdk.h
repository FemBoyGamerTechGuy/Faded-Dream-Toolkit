/*
 * fdk.h — Faded Dream Kit
 * Master public header. Include this in your application.
 *
 * License: FDK Proprietary License — see LICENSE in the project root
 * Dependencies: none (this header only)
 */
#ifndef FDK_H
#define FDK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ─── Version ────────────────────────────────────────────────────────────── */
#define FDK_VERSION_MAJOR 0
#define FDK_VERSION_MINOR 8
#define FDK_VERSION_PATCH 0

/* ─── Forward declarations ───────────────────────────────────────────────── */
typedef struct FDK_Window   FDK_Window;
typedef struct FDK_Surface  FDK_Surface;
typedef struct FDK_Font     FDK_Font;

/* ─── Colour (RGBA, 0‥255) ───────────────────────────────────────────────── */
typedef struct {
    uint8_t r, g, b, a;
} FDK_Color;

#define FDK_RGBA(r,g,b,a)  ((FDK_Color){(r),(g),(b),(a)})
#define FDK_RGB(r,g,b)     FDK_RGBA((r),(g),(b),255)
#define FDK_BLACK          FDK_RGB(0,0,0)
#define FDK_WHITE          FDK_RGB(255,255,255)
#define FDK_TRANSPARENT    FDK_RGBA(0,0,0,0)

/* ─── Geometry ───────────────────────────────────────────────────────────── */
typedef struct { int x, y; }          FDK_Point;
typedef struct { int w, h; }          FDK_Size;
typedef struct { int x, y, w, h; }    FDK_Rect;

/* ─── Events ─────────────────────────────────────────────────────────────── */
typedef enum {
    FDK_EVENT_NONE = 0,
    FDK_EVENT_QUIT,
    FDK_EVENT_CLOSE,        /* window close requested  */
    FDK_EVENT_RESIZE,
    FDK_EVENT_EXPOSE,       /* window needs redraw     */
    FDK_EVENT_KEY_DOWN,
    FDK_EVENT_KEY_UP,
    FDK_EVENT_MOUSE_MOVE,
    FDK_EVENT_MOUSE_DOWN,
    FDK_EVENT_MOUSE_UP,
    FDK_EVENT_MOUSE_SCROLL,
} FDK_EventType;

/* Keyboard key codes — minimal portable set */
typedef enum {
    FDK_KEY_UNKNOWN = 0,
    FDK_KEY_ESCAPE, FDK_KEY_RETURN, FDK_KEY_SPACE, FDK_KEY_BACKSPACE,
    FDK_KEY_TAB, FDK_KEY_DELETE,
    FDK_KEY_LEFT, FDK_KEY_RIGHT, FDK_KEY_UP, FDK_KEY_DOWN,
    FDK_KEY_HOME, FDK_KEY_END, FDK_KEY_PAGEUP, FDK_KEY_PAGEDOWN,
    FDK_KEY_F1,  FDK_KEY_F2,  FDK_KEY_F3,  FDK_KEY_F4,
    FDK_KEY_F5,  FDK_KEY_F6,  FDK_KEY_F7,  FDK_KEY_F8,
    FDK_KEY_F9,  FDK_KEY_F10, FDK_KEY_F11, FDK_KEY_F12,
    FDK_KEY_A = 'a', /* printable ASCII keys map directly */
} FDK_Key;

typedef enum {
    FDK_MOD_NONE  = 0,
    FDK_MOD_SHIFT = 1 << 0,
    FDK_MOD_CTRL  = 1 << 1,
    FDK_MOD_ALT   = 1 << 2,
    FDK_MOD_SUPER = 1 << 3,
} FDK_Modifier;

typedef enum {
    FDK_BUTTON_LEFT = 1,
    FDK_BUTTON_MIDDLE,
    FDK_BUTTON_RIGHT,
} FDK_MouseButton;

typedef struct {
    FDK_EventType   type;
    FDK_Window     *window;
    union {
        struct { int w, h; }                           resize;
        struct { FDK_Key key; FDK_Modifier mods;
                 uint32_t codepoint; }                 key;
        struct { int x, y; FDK_Modifier mods; }        motion;
        struct { int x, y; FDK_MouseButton button;
                 FDK_Modifier mods; }                  mouse;
        struct { float dx, dy; }                       scroll;
    };
} FDK_Event;

/* ─── Render backend selection ───────────────────────────────────────────── */
typedef enum {
    FDK_RENDER_SOFTWARE = 0,   /* CPU pixel buffer — always available  */
    FDK_RENDER_OPENGL,         /* OpenGL 3.3 core profile              */
    FDK_RENDER_VULKAN,         /* Vulkan (future)                      */
    FDK_RENDER_AUTO,           /* pick best available                  */
} FDK_RenderBackend;

/* ─── Platform backend selection ─────────────────────────────────────────── */
typedef enum {
    FDK_PLATFORM_AUTO = 0,     /* detect at runtime                    */
    FDK_PLATFORM_X11,
    FDK_PLATFORM_WAYLAND,
} FDK_PlatformBackend;

/* ─── Init / shutdown ────────────────────────────────────────────────────── */
typedef struct {
    FDK_PlatformBackend platform;
    FDK_RenderBackend   render;
    const char         *app_name;
} FDK_InitInfo;

bool  fdk_init(const FDK_InitInfo *info);
void  fdk_shutdown(void);

/* ─── Window ─────────────────────────────────────────────────────────────── */
typedef struct {
    const char         *title;
    int                 x, y;        /* FDK_WINDOW_POS_CENTER = -1  */
    int                 w, h;
    bool                resizable;
    FDK_RenderBackend   render;      /* override per-window         */
} FDK_WindowDesc;

#define FDK_WINDOW_POS_CENTER (-1)

FDK_Window *fdk_window_create(const FDK_WindowDesc *desc);
void        fdk_window_destroy(FDK_Window *win);
void        fdk_window_show(FDK_Window *win);
void        fdk_window_hide(FDK_Window *win);
void        fdk_window_set_title(FDK_Window *win, const char *title);
FDK_Size    fdk_window_get_size(FDK_Window *win);
void        fdk_window_request_redraw(FDK_Window *win);

/* ─── Event loop ─────────────────────────────────────────────────────────── */
/* Returns false when the application should quit */
bool fdk_poll_event(FDK_Event *out_event);
void fdk_wait_event(FDK_Event *out_event);
/* Returns false on timeout, true if event was produced. timeout_ms<0 = block forever */
bool fdk_wait_event_timeout(FDK_Event *out_event, int timeout_ms);

/* ─── Drawing (immediate‑mode, called between begin/end) ─────────────────── */
void fdk_begin_frame(FDK_Window *win);
void fdk_end_frame(FDK_Window *win);

void fdk_clear(FDK_Color color);
void fdk_fill_rect(FDK_Rect r, FDK_Color color);
void fdk_stroke_rect(FDK_Rect r, FDK_Color color, int thickness);
void fdk_fill_rect_rounded(FDK_Rect r, int radius, FDK_Color color);

/* ─── Gradient / shadow types (used by draw API and theme system) ──────── */
typedef struct {
    FDK_Color color;
    float     pos;      /* 0.0 … 1.0 */
} FDK_GradStop;

typedef enum {
    FDK_GRAD_NONE = 0,
    FDK_GRAD_LINEAR_V,
    FDK_GRAD_LINEAR_H,
} FDK_GradType;

typedef struct {
    FDK_GradType  type;
    FDK_GradStop  stops[4];
    int           stop_count;
} FDK_Gradient;

typedef struct {
    int       offset_x, offset_y;
    int       blur;
    FDK_Color color;
    bool      enabled;
} FDK_Shadow;

/* Forward declaration — full definition in fdk_widget.h */
typedef struct FDK_ContextMenu FDK_ContextMenu;

/* Gradient-filled rounded rect — falls back to flat fill if backend lacks support */
void fdk_fill_rect_gradient(FDK_Rect r, int radius, const FDK_Gradient *gradient);
/* Draw a soft drop shadow behind r — renders before the widget fill */
void fdk_draw_shadow(FDK_Rect r, int radius, const FDK_Shadow *shadow);
void fdk_fill_circle(int cx, int cy, int radius, FDK_Color color);
void fdk_draw_line(int x0, int y0, int x1, int y1, FDK_Color color, int thickness);

/* ─── Text ───────────────────────────────────────────────────────────────── */
FDK_Font *fdk_font_load(const char *path, float size_px);
FDK_Font *fdk_font_load_memory(const uint8_t *data, size_t len, float size_px);
void      fdk_font_destroy(FDK_Font *font);

void      fdk_draw_text(FDK_Font *font, const char *utf8,
                        int x, int y, FDK_Color color);
FDK_Size  fdk_measure_text(FDK_Font *font, const char *utf8);

/* ─── Scissor / clipping ─────────────────────────────────────────────────── */
void fdk_push_clip(FDK_Rect r);
void fdk_pop_clip(void);

/* ─── Cursor ─────────────────────────────────────────────────────────────── */
typedef enum {
    FDK_CURSOR_DEFAULT = 0,
    FDK_CURSOR_POINTER,     /* hand — for buttons/links  */
    FDK_CURSOR_TEXT,        /* I-beam — for text inputs  */
    FDK_CURSOR_CROSSHAIR,
    FDK_CURSOR_MOVE,
    FDK_CURSOR_RESIZE_H,
    FDK_CURSOR_RESIZE_V,
    FDK_CURSOR_NOT_ALLOWED,
} FDK_Cursor;

void fdk_set_cursor(FDK_Cursor cursor);

/* ─── Clipboard ──────────────────────────────────────────────────────────── */
void  fdk_clipboard_set(const char *text);
char *fdk_clipboard_get(void);   /* returns malloc'd string — caller must free() */

/* ─── Utilities ──────────────────────────────────────────────────────────── */
uint64_t fdk_time_ms(void);   /* monotonic milliseconds */
void     fdk_sleep_ms(uint32_t ms);

/* ─── Animation / tween system ───────────────────────────────────────────── */
typedef enum {
    FDK_EASE_LINEAR = 0,
    FDK_EASE_IN_QUAD,
    FDK_EASE_OUT_QUAD,
    FDK_EASE_IN_OUT_QUAD,
    FDK_EASE_IN_CUBIC,
    FDK_EASE_OUT_CUBIC,
    FDK_EASE_IN_OUT_CUBIC,
    FDK_EASE_OUT_ELASTIC,
    FDK_EASE_OUT_BOUNCE,
} FDK_Easing;

typedef struct FDK_Tween FDK_Tween;

/* Create a tween from `from` to `to` over `duration_ms`.
 * on_update(value, ud) is called each tick. on_done(ud) called on completion.
 * Returns a handle valid until the tween completes or fdk_tween_cancel(). */
typedef void (*FDK_TweenUpdateCb)(float value, void *ud);
typedef void (*FDK_TweenDoneCb)(void *ud);

FDK_Tween *fdk_tween(float from, float to, uint32_t duration_ms,
                      FDK_Easing easing,
                      FDK_TweenUpdateCb on_update,
                      FDK_TweenDoneCb   on_done,
                      void *ud);
void       fdk_tween_cancel(FDK_Tween *t);
bool       fdk_tween_is_done(const FDK_Tween *t);
/* Advance all live tweens — call once per frame (fdk_ui_step does this) */
void       fdk__tweens_tick(void);

#ifdef __cplusplus
}
#endif
#endif /* FDK_H */
