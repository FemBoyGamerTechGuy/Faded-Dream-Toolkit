/*
 * widget.c — FDK widget layer implementation
 *
 * Covers:
 *   - FDK_Widget base struct and all widget types
 *   - Layout engine (vertical / horizontal flex)
 *   - Event dispatch (hit-test, hover, focus, click)
 *   - Paint pass (per-widget draw logic)
 *   - Focus manager (tab order, keyboard routing)
 *   - FDK_UI context (glues everything to a window)
 */
#define _POSIX_C_SOURCE 200809L

/* Pull in stb_image implementation exactly once when PNG/JPEG support
 * is requested.  The header must be on the include path (set FDK_STB_DIR
 * in CMake, or pass -DFDK_STB_DIR=/path/to/stb). */
#ifdef FDK_WITH_STB_IMAGE
#  define STB_IMAGE_IMPLEMENTATION
#  define STBI_ONLY_PNG
#  define STBI_ONLY_JPEG
#  define STBI_ONLY_BMP
#  define STBI_NO_STDIO 0
#  include "stb_image.h"
#endif

#include "fdk/fdk_widget.h"
#include "core/core_internal.h"
#include "render/font_internal.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <ctype.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Text input buffer size ─────────────────────────────────────────────── */
#define FDK_INPUT_MAX_DEFAULT 255

/* ─── Widget struct ──────────────────────────────────────────────────────── */
#define FDK_MAX_CHILDREN 64

struct FDK_Widget {
    FDK_WidgetType type;

    /* Layout */
    FDK_Rect  rect;       /* computed by layout pass        */
    int       w_hint;     /* FDK_SIZE_WRAP / FILL / px      */
    int       h_hint;
    int       min_w, min_h;

    /* State */
    bool      visible;
    bool      enabled;
    bool      hovered;
    bool      focused;
    void     *userdata;

    int       max_w, max_h;  /* 0 = no limit */

    /* Tag for lookup */
    int       tag;
    /* Tooltip */
    char     *tooltip;
    uint64_t  hover_start_ms;  /* when hovered state began, for delay */

    /* Theme variant tag (e.g. "danger", "accent", "ghost") */
    char      variant[32];
    /* Per-widget style override (set via fdk_widget_set_style) */
    FDK_WidgetStyle *style_override;  /* NULL = none */
    /* Right-click context menu — auto-shown on MOUSE_DOWN button 3 */
    FDK_ContextMenu *ctx_menu;        /* NULL = none */

    /* Tree */
    FDK_Widget  *parent;
    FDK_Widget  *children[FDK_MAX_CHILDREN];
    int          child_count;

    /* Container layout params */
    FDK_Layout   layout;

    /* Per-type data */
    union {
        /* Label */
        struct {
            char      *text;
            FDK_Color  color;
            bool       use_color;
        } label;

        /* Button */
        struct {
            char        *label;
            FDK_ClickCb  cb;
            void        *cb_ud;
            bool         pressed;
        } button;

        /* Text input */
        struct {
            char          *buf;
            int            buf_len;
            int            max_len;
            char          *placeholder;
            int            cursor;      /* byte offset of caret           */
            int            sel_start;   /* byte offset of selection start */
            int            sel_end;     /* byte offset of selection end   */
            bool           has_sel;     /* true when selection is active  */
            int            scroll_x;   /* horizontal scroll px            */
            FDK_ChangeCb   cb;
            void          *cb_ud;
        } input;

        /* Checkbox */
        struct {
            char        *label;
            bool         checked;
            FDK_ClickCb  cb;
            void        *cb_ud;
        } checkbox;

        /* Custom */
        struct {
            FDK_DrawCb  draw_cb;
            void       *cb_ud;
        } custom;

        /* Slider */
        struct {
            float        min, max, value, step;
            bool         dragging;
            FDK_ValueCb  cb;
            void        *cb_ud;
        } slider;

        /* Progress bar */
        struct {
            float    value;       /* 0.0 – 1.0 */
            bool     indeterminate;
            float    anim_pos;    /* 0.0 – 1.0, for indeterminate animation */
        } progress;

        /* Scroll view */
        struct {
            FDK_Widget *content;
            int         scroll_x, scroll_y;
            int         content_w, content_h;
        } scroll;

        /* Dropdown */
        struct {
            char        *placeholder;
            char        *items[64];
            int          item_count;
            int          selected;      /* -1 = none        */
            int          hovered_item;  /* -1 = none        */
            bool         open;
            FDK_SelectCb cb;
            void        *cb_ud;
        } dropdown;

        /* Image */
        struct {
            uint8_t *pixels;  /* RGBA8888, owned by widget */
            int      w, h;
        } image;

        /* Toggle button */
        struct {
            char        *label;
            bool         active;
            FDK_ClickCb  cb;
            void        *cb_ud;
        } toggle;

        /* Radio button */
        struct {
            char        *label;
            int         *group;   /* shared group pointer (owned by caller) */
            int          index;
            FDK_ClickCb  cb;
            void        *cb_ud;
        } radio;

        /* Spinner */
        struct {
            double       min, max, value, step;
            int          decimals;
            bool         editing;        /* text field active      */
            char         edit_buf[64];   /* raw text while editing */
            int          edit_len;
            FDK_ValueCb  cb;
            void        *cb_ud;
        } spinner;

        /* Badge */
        struct {
            char       *text;
            FDK_Color   bg;
            FDK_Color   fg;
            bool        custom_color;
        } badge;

        /* Tabs */
        struct {
            char       *labels[FDK_TABS_MAX];
            FDK_Widget *pages[FDK_TABS_MAX];
            int         count;
            int         active;
            float       indicator_x;   /* animated underline x (px) */
            float       indicator_w;   /* animated underline w (px) */
            void      (*cb)(FDK_Widget*, int, void*);
            void       *cb_ud;
        } tabs;

        /* Menu bar */
        struct {
            FDK_Menu menus[FDK_MENU_MAX_MENUS];
            int      count;
            int      open_menu;   /* -1 = none open */
        } menubar;

        /* Text area */
        struct {
            char    *buf;         /* UTF-8 content                  */
            int      len;         /* bytes used (excl. NUL)         */
            int      cap;         /* buffer capacity                */
            int      cursor;      /* byte offset of insert point    */
            int      sel_start;   /* selection range (bytes)        */
            int      sel_end;
            int      scroll_y;    /* px scrolled down               */
            bool     readonly;
            void   (*cb)(FDK_Widget *, void *);
            void    *cb_ud;
        } textarea;
    };
};

/* ─── UI context ─────────────────────────────────────────────────────────── */
struct FDK_UI {
    FDK_Window  *win;
    FDK_Theme    theme;
    bool         dirty;
    FDK_Widget  *hover;
    FDK_Widget  *focused;
    FDK_Widget  *tab_order[256];
    int          tab_count;
    /* Cursor blink */
    uint64_t     blink_last;
    bool         blink_on;
    /* Mouse state */
    bool         mouse_down;       /* left button currently held */
    FDK_Widget  *mouse_down_widget;/* widget that received MOUSE_DOWN */
    /* Mouse position for tooltip placement */
    int          mouse_x, mouse_y;

    /* ── Toast / notification queue ── */
    struct {
        char       message[256];
        FDK_NotifyKind kind;
        uint64_t   created_ms;
        uint32_t   duration_ms;
        float      slide_y;       /* 0 = fully visible, >0 = sliding in/out */
        float      alpha;         /* 0..1 opacity                             */
        bool       active;
        bool       dismissing;    /* fade-out tween running                   */
    } toasts[FDK_TOAST_MAX];
    int toast_count;

    /* ── Context menu ── */
    FDK_ContextMenu *ctx_menu;   /* NULL = none active */
    int              ctx_x, ctx_y;
};

/* ─── Helpers ────────────────────────────────────────────────────────────── */
static FDK_Widget *widget_alloc(FDK_WidgetType type)
{
    FDK_Widget *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->type    = type;
    w->visible = true;
    w->enabled = true;
    w->w_hint  = FDK_SIZE_WRAP;
    w->h_hint  = FDK_SIZE_WRAP;
    return w;
}

static char *fdk_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d  = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}


/* Get real line height from font metrics, fall back to theme value */
static inline int font_line_h(const FDK_Font *f, const FDK_Theme *th)
{
    if (f && f->line_height > 0) return f->line_height;
    return th->line_height;
}

/* Get real ascender from font, fall back to 80% of theme line_height */
static inline int font_ascender(const FDK_Font *f, const FDK_Theme *th)
{
    if (f && f->ascender > 0) return f->ascender;
    return (th->line_height * 4) / 5;
}

/* ─── Theme ──────────────────────────────────────────────────────────────── */
FDK_Theme fdk_theme_faded_dream(void)
{
    FDK_Theme t = {0};
    t.bg_window        = FDK_RGB(18,  18,  24);
    t.bg_widget        = FDK_RGB(30,  30,  42);
    t.bg_widget_hover  = FDK_RGB(40,  40,  56);
    t.bg_widget_active = FDK_RGB(24,  24,  34);
    t.bg_input         = FDK_RGB(24,  24,  34);
    t.bg_input_focus   = FDK_RGB(28,  28,  40);

    t.fg_primary       = FDK_RGB(230, 225, 245);
    t.fg_secondary     = FDK_RGB(160, 155, 185);
    t.fg_disabled      = FDK_RGB(90,  88, 110);
    t.fg_on_accent     = FDK_RGB(255, 255, 255);

    t.accent           = FDK_RGB(138,  99, 210);
    t.accent_hover     = FDK_RGB(158, 119, 230);
    t.accent_active    = FDK_RGB(118,  79, 190);

    t.border           = FDK_RGBA(138, 99, 210, 60);
    t.border_focus     = FDK_RGBA(138, 99, 210, 180);
    t.separator        = FDK_RGBA(255, 255, 255, 20);

    t.radius_sm        = 6;
    t.radius_md        = 12;
    t.line_height      = 22;
    return t;
}

/* ─── UI context ─────────────────────────────────────────────────────────── */

/* fdk_ui_set_theme — runtime theme switch, preserves fonts if new theme has none */
void fdk_ui_set_theme(FDK_UI *ui, FDK_Widget *root, const FDK_Theme *theme)
{
    if (!ui || !theme) return;
    FDK_Font *body  = ui->theme.font_body;
    FDK_Font *label = ui->theme.font_label;
    FDK_Font *mono  = ui->theme.font_mono;
    ui->theme = *theme;
    if (!ui->theme.font_body)  ui->theme.font_body  = body;
    if (!ui->theme.font_label) ui->theme.font_label = label;
    if (!ui->theme.font_mono)  ui->theme.font_mono  = mono;
    (void)root;
    ui->dirty = true;
}

/* ─── Context menu ───────────────────────────────────────────────────────── */
struct FDK_ContextMenu {
    struct {
        char  *label;       /* NULL = separator */
        void (*cb)(void *);
        void  *ud;
        bool   disabled;
    } items[FDK_CTX_MAX_ITEMS];
    int count;
};

FDK_ContextMenu *fdk_context_menu_new(void)
{
    FDK_ContextMenu *cm = calloc(1, sizeof *cm);
    return cm;
}

void fdk_context_menu_free(FDK_ContextMenu *cm)
{
    if (!cm) return;
    for (int i = 0; i < cm->count; i++) free(cm->items[i].label);
    free(cm);
}

void fdk_context_menu_add(FDK_ContextMenu *cm,
                            const char *label,
                            void (*cb)(void *), void *ud)
{
    if (!cm || cm->count >= FDK_CTX_MAX_ITEMS) return;
    cm->items[cm->count].label    = fdk_strdup(label ? label : "");
    cm->items[cm->count].cb       = cb;
    cm->items[cm->count].ud       = ud;
    cm->items[cm->count].disabled = false;
    cm->count++;
}

void fdk_context_menu_add_separator(FDK_ContextMenu *cm)
{
    if (!cm || cm->count >= FDK_CTX_MAX_ITEMS) return;
    cm->items[cm->count].label = NULL;  /* NULL label = separator */
    cm->count++;
}

void fdk_context_menu_show(FDK_UI *ui, FDK_ContextMenu *cm, int x, int y)
{
    if (!ui) return;
    ui->ctx_menu = cm;
    ui->ctx_x    = x;
    ui->ctx_y    = y;
    ui->dirty    = true;
}

void fdk_context_menu_hide(FDK_UI *ui)
{
    if (!ui) return;
    ui->ctx_menu = NULL;
    ui->dirty    = true;
}

/* ─── Text area ──────────────────────────────────────────────────────────── */
FDK_Widget *fdk_textarea(const char *initial_text)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_TEXTAREA);
    if (!w) return NULL;
    w->textarea.cap = 4096;
    w->textarea.buf = calloc(1, w->textarea.cap);
    if (!w->textarea.buf) { free(w); return NULL; }
    if (initial_text) {
        int n = (int)strlen(initial_text);
        if (n >= w->textarea.cap) n = w->textarea.cap - 1;
        memcpy(w->textarea.buf, initial_text, n);
        w->textarea.len    = n;
        w->textarea.cursor = n;
    }
    w->w_hint = FDK_SIZE_FILL;
    w->h_hint = FDK_SIZE_FILL;
    return w;
}

const char *fdk_textarea_get_text(const FDK_Widget *w)
{ return w ? w->textarea.buf : ""; }

void fdk_textarea_set_text(FDK_Widget *w, const char *text)
{
    if (!w || w->type != FDK_WIDGET_TEXTAREA) return;
    int n = text ? (int)strlen(text) : 0;
    if (n >= w->textarea.cap) {
        /* Grow buffer */
        int newcap = n + 1;
        char *nb = realloc(w->textarea.buf, newcap);
        if (!nb) return;
        w->textarea.buf = nb;
        w->textarea.cap = newcap;
    }
    if (text) memcpy(w->textarea.buf, text, n);
    w->textarea.buf[n] = '\0';
    w->textarea.len    = n;
    w->textarea.cursor = n;
    w->textarea.sel_start = w->textarea.sel_end = 0;
}

void fdk_textarea_set_readonly(FDK_Widget *w, bool ro)
{ if (w) w->textarea.readonly = ro; }

void fdk_textarea_on_change(FDK_Widget *w,
                              void (*cb)(FDK_Widget *, void *), void *ud)
{
    if (!w || w->type != FDK_WIDGET_TEXTAREA) return;
    w->textarea.cb    = cb;
    w->textarea.cb_ud = ud;
}

/* fdk_theme_resolve_ex is declared in fdk_widget.h (defined in
 * src/core/theme.c) — it's now a proper public API function, not just
 * an internal helper, since apps may want to know which file backed a
 * resolved theme (see its doc comment in the header). */

FDK_UI *fdk_ui_create(FDK_Window *win, FDK_Theme *theme)
{
    FDK_UI *ui = calloc(1, sizeof *ui);
    if (!ui) return NULL;
    ui->win   = win;
    ui->dirty = true;

    if (theme) {
        /* Explicit theme passed — caller is fully in control, same as
         * before. No auto-resolution, no auto-watch: an explicit theme
         * is itself a form of opting out of the automatic system. */
        ui->theme = *theme;
        return ui;
    }

    /* No theme passed: resolve via the three-tier system (developer
     * force -> user per-app override -> system-wide ~/.FDKthemes). */
    char resolved_path[512];
    const char *app_name = fdk__get_app_name();
    ui->theme = fdk_theme_resolve_ex(app_name, resolved_path, sizeof resolved_path);

    /* Auto-watch for live updates, but only for tier 2/3 (a real file
     * backs the resolved theme). Tier 1 (fdk_theme_force) has no
     * backing file — resolved_path is left empty in that case — and is
     * deliberately not auto-watched, since forcing is an explicit
     * opt-out of the dynamic/shared theme ecosystem. */
    if (resolved_path[0])
        fdk_theme_watch(ui, NULL, resolved_path);

    return ui;
}

void fdk_ui_destroy(FDK_UI *ui) { free(ui); }

static void do_layout(FDK_UI *ui, FDK_Widget *root); /* forward decl */

void fdk_ui_layout(FDK_UI *ui, FDK_Widget *root)
{
    if (!ui || !root) return;
    FDK_Size sz = fdk_window_get_size(ui->win);
    if (sz.w > 0 && sz.h > 0)
        do_layout(ui, root);
}
void fdk_ui_invalidate(FDK_UI *ui) { if (ui) ui->dirty = true; }
void fdk_ui_reset_blink(FDK_UI *ui)
{
    if (!ui) return;
    ui->blink_on   = true;
    ui->blink_last = fdk_time_ms();
}

/* ─── Widget base API ────────────────────────────────────────────────────── */
FDK_Rect fdk_widget_get_rect(const FDK_Widget *w)  { return w->rect; }
void fdk_widget_set_size(FDK_Widget *w, int wh, int hh) { w->w_hint=wh; w->h_hint=hh; }
void fdk_widget_set_min_size(FDK_Widget *w, int mw, int mh) { w->min_w=mw; w->min_h=mh; }
void fdk_widget_set_max_size(FDK_Widget *w, int mw, int mh) { w->max_w=mw; w->max_h=mh; }
void fdk_widget_show(FDK_Widget *w)  { w->visible = true; }
void fdk_widget_hide(FDK_Widget *w)  { w->visible = false; }
bool fdk_widget_is_visible(const FDK_Widget *w) { return w->visible; }
void fdk_widget_enable(FDK_Widget *w)  { w->enabled = true; }
void fdk_widget_disable(FDK_Widget *w) { w->enabled = false; }
bool fdk_widget_is_enabled(const FDK_Widget *w) { return w->enabled; }
void fdk_widget_set_userdata(FDK_Widget *w, void *d) { w->userdata = d; }
void *fdk_widget_get_userdata(const FDK_Widget *w)   { return w->userdata; }

void fdk_widget_destroy(FDK_Widget *w)
{
    if (!w) return;
    for (int i = 0; i < w->child_count; i++)
        fdk_widget_destroy(w->children[i]);
    free(w->tooltip);
    free(w->style_override);
    switch (w->type) {
    case FDK_WIDGET_LABEL:      free(w->label.text);       break;
    case FDK_WIDGET_BUTTON:     free(w->button.label);     break;
    case FDK_WIDGET_CHECKBOX:   free(w->checkbox.label);   break;
    case FDK_WIDGET_TEXT_INPUT:
        free(w->input.buf);
        free(w->input.placeholder);
        break;
    case FDK_WIDGET_DROPDOWN:
        free(w->dropdown.placeholder);
        for (int i = 0; i < w->dropdown.item_count; i++)
            free(w->dropdown.items[i]);
        break;
    case FDK_WIDGET_IMAGE:
        free(w->image.pixels);
        break;
    case FDK_WIDGET_SCROLL_VIEW:
        /* content widget is owned by the caller — don't destroy it here */
        break;
    case FDK_WIDGET_TOGGLE_BUTTON: free(w->toggle.label); break;
    case FDK_WIDGET_RADIO_BUTTON:  free(w->radio.label);  break;
    case FDK_WIDGET_BADGE:         free(w->badge.text);   break;
    case FDK_WIDGET_TABS:
        for (int i = 0; i < w->tabs.count; i++) {
            free(w->tabs.labels[i]);
            /* pages are child widgets — already freed in the child loop above */
        }
        break;
    case FDK_WIDGET_MENUBAR:
        for (int i = 0; i < w->menubar.count; i++) {
            free(w->menubar.menus[i].label);
            for (int j = 0; j < w->menubar.menus[i].item_count; j++) {
                free(w->menubar.menus[i].items[j].label);
                free(w->menubar.menus[i].items[j].shortcut);
            }
        }
        break;
    case FDK_WIDGET_TEXTAREA:
        free(w->textarea.buf);
        break;
    default: break;
    }
    free(w);
}

/* ─── Container ──────────────────────────────────────────────────────────── */
FDK_Widget *fdk_container(FDK_Layout layout)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_CONTAINER);
    if (!w) return NULL;
    w->layout = layout;
    return w;
}

FDK_Widget *fdk_vbox(int gap, int padding)
{
    return fdk_container((FDK_Layout){
        .dir = FDK_LAYOUT_VERTICAL, .align = FDK_ALIGN_STRETCH,
        .gap = gap, .padding = padding });
}

FDK_Widget *fdk_hbox(int gap, int padding)
{
    return fdk_container((FDK_Layout){
        .dir = FDK_LAYOUT_HORIZONTAL, .align = FDK_ALIGN_STRETCH,
        .gap = gap, .padding = padding });
}

void fdk_container_add(FDK_Widget *c, FDK_Widget *child)
{
    if (!c || !child || c->child_count >= FDK_MAX_CHILDREN) return;
    child->parent = c;
    c->children[c->child_count++] = child;
}

void fdk_container_remove(FDK_Widget *c, FDK_Widget *child)
{
    for (int i = 0; i < c->child_count; i++) {
        if (c->children[i] == child) {
            child->parent = NULL;
            memmove(&c->children[i], &c->children[i+1],
                    (c->child_count - i - 1) * sizeof(FDK_Widget*));
            c->child_count--;
            return;
        }
    }
}

void fdk_container_clear(FDK_Widget *c)
{
    for (int i = 0; i < c->child_count; i++)
        c->children[i]->parent = NULL;
    c->child_count = 0;
}

/* ─── Label ──────────────────────────────────────────────────────────────── */
FDK_Widget *fdk_label(const char *text)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_LABEL);
    if (!w) return NULL;
    w->label.text = fdk_strdup(text ? text : "");
    return w;
}

void fdk_label_set_text(FDK_Widget *w, const char *t)
{
    free(w->label.text);
    w->label.text = fdk_strdup(t ? t : "");
}

const char *fdk_label_get_text(const FDK_Widget *w) { return w->label.text; }

void fdk_label_set_color(FDK_Widget *w, FDK_Color c)
{
    w->label.color     = c;
    w->label.use_color = true;
}

/* ─── Button ─────────────────────────────────────────────────────────────── */
FDK_Widget *fdk_button(const char *label)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_BUTTON);
    if (!w) return NULL;
    w->button.label = fdk_strdup(label ? label : "");
    /* Default size: fill width, fixed height */
    w->h_hint = 36;
    return w;
}

void fdk_button_set_label(FDK_Widget *w, const char *l)
{
    free(w->button.label);
    w->button.label = fdk_strdup(l ? l : "");
}

void fdk_button_on_click(FDK_Widget *w, FDK_ClickCb cb, void *ud)
{
    w->button.cb    = cb;
    w->button.cb_ud = ud;
}

/* ─── Text input ─────────────────────────────────────────────────────────── */
FDK_Widget *fdk_text_input(const char *placeholder)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_TEXT_INPUT);
    if (!w) return NULL;
    w->input.max_len     = FDK_INPUT_MAX_DEFAULT;
    w->input.buf         = calloc(1, FDK_INPUT_MAX_DEFAULT + 1);
    w->input.buf_len     = 0;
    w->input.placeholder = fdk_strdup(placeholder ? placeholder : "");
    w->h_hint            = 36;
    return w;
}

void fdk_text_input_set_text(FDK_Widget *w, const char *t)
{
    if (!t) t = "";
    int len = (int)strlen(t);
    if (len > w->input.max_len) len = w->input.max_len;
    memcpy(w->input.buf, t, len);
    w->input.buf[len] = '\0';
    w->input.buf_len  = len;
    w->input.cursor   = len;
}

const char *fdk_text_input_get_text(const FDK_Widget *w)
{
    return w->input.buf;
}

void fdk_text_input_set_placeholder(FDK_Widget *w, const char *t)
{
    free(w->input.placeholder);
    w->input.placeholder = fdk_strdup(t ? t : "");
}

void fdk_text_input_on_change(FDK_Widget *w, FDK_ChangeCb cb, void *ud)
{
    w->input.cb    = cb;
    w->input.cb_ud = ud;
}

void fdk_text_input_set_max_len(FDK_Widget *w, int max)
{
    w->input.max_len = max;
}

/* ─── Checkbox ───────────────────────────────────────────────────────────── */
FDK_Widget *fdk_checkbox(const char *label, bool checked)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_CHECKBOX);
    if (!w) return NULL;
    w->checkbox.label   = fdk_strdup(label ? label : "");
    w->checkbox.checked = checked;
    w->h_hint = 28;
    return w;
}

bool fdk_checkbox_get_checked(const FDK_Widget *w) { return w->checkbox.checked; }

void fdk_checkbox_set_checked(FDK_Widget *w, bool c) { w->checkbox.checked = c; }

void fdk_checkbox_on_change(FDK_Widget *w, FDK_ClickCb cb, void *ud)
{
    w->checkbox.cb    = cb;
    w->checkbox.cb_ud = ud;
}

/* ─── Separator ──────────────────────────────────────────────────────────── */
FDK_Widget *fdk_separator(void)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_SEPARATOR);
    if (!w) return NULL;
    w->h_hint = 1;
    return w;
}

/* ─── Custom ─────────────────────────────────────────────────────────────── */
FDK_Widget *fdk_custom(int wh, int hh, FDK_DrawCb cb, void *ud)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_CUSTOM);
    if (!w) return NULL;
    w->w_hint       = wh;
    w->h_hint       = hh;
    w->custom.draw_cb = cb;
    w->custom.cb_ud   = ud;
    return w;
}

/* ─── Slider ─────────────────────────────────────────────────────────────── */
FDK_Widget *fdk_slider(float min, float max, float value)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_SLIDER);
    if (!w) return NULL;
    w->slider.min   = min;
    w->slider.max   = max;
    w->slider.value = value < min ? min : value > max ? max : value;
    w->slider.step  = 0.0f;
    w->h_hint = 28;
    return w;
}
float fdk_slider_get_value(const FDK_Widget *w) { return w->slider.value; }
void  fdk_slider_set_value(FDK_Widget *w, float v)
{
    if (v < w->slider.min) v = w->slider.min;
    if (v > w->slider.max) v = w->slider.max;
    w->slider.value = v;
}
void fdk_slider_set_range(FDK_Widget *w, float mn, float mx)
{ w->slider.min = mn; w->slider.max = mx; }
void fdk_slider_set_step(FDK_Widget *w, float s)  { w->slider.step = s; }
void fdk_slider_on_change(FDK_Widget *w, FDK_ValueCb cb, void *ud)
{ w->slider.cb = cb; w->slider.cb_ud = ud; }

/* ─── Progress bar ───────────────────────────────────────────────────────── */
FDK_Widget *fdk_progress_bar(float value)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_PROGRESS_BAR);
    if (!w) return NULL;
    w->progress.value = value < 0.f ? 0.f : value > 1.f ? 1.f : value;
    w->h_hint = 8;
    return w;
}
void  fdk_progress_set_value(FDK_Widget *w, float v)
{ w->progress.value = v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
float fdk_progress_get_value(const FDK_Widget *w) { return w->progress.value; }
void  fdk_progress_set_indeterminate(FDK_Widget *w, bool on)
{ w->progress.indeterminate = on; }

/* ─── Scroll view ────────────────────────────────────────────────────────── */
FDK_Widget *fdk_scroll_view(FDK_Widget *content)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_SCROLL_VIEW);
    if (!w) return NULL;
    w->scroll.content = content;
    return w;
}
void fdk_scroll_view_set_content(FDK_Widget *w, FDK_Widget *content)
{ w->scroll.content = content; }
void fdk_scroll_view_scroll_to(FDK_Widget *w, int x, int y)
{ w->scroll.scroll_x = x; w->scroll.scroll_y = y; }

/* ─── Dropdown ───────────────────────────────────────────────────────────── */
FDK_Widget *fdk_dropdown(const char *placeholder)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_DROPDOWN);
    if (!w) return NULL;
    w->dropdown.placeholder = fdk_strdup(placeholder ? placeholder : "Select...");
    w->dropdown.selected    = -1;
    w->dropdown.hovered_item = -1;
    w->h_hint = 36;
    return w;
}
void fdk_dropdown_add_item(FDK_Widget *w, const char *text)
{
    if (w->dropdown.item_count >= 64) return;
    w->dropdown.items[w->dropdown.item_count++] = fdk_strdup(text);
}
void fdk_dropdown_clear(FDK_Widget *w)
{
    for (int i = 0; i < w->dropdown.item_count; i++)
        free(w->dropdown.items[i]);
    w->dropdown.item_count = 0;
    w->dropdown.selected   = -1;
}
int         fdk_dropdown_get_selected(const FDK_Widget *w)
{ return w->dropdown.selected; }
const char *fdk_dropdown_get_selected_text(const FDK_Widget *w)
{
    int i = w->dropdown.selected;
    return (i >= 0 && i < w->dropdown.item_count) ? w->dropdown.items[i] : NULL;
}
void fdk_dropdown_set_selected(FDK_Widget *w, int index)
{ w->dropdown.selected = index; }
void fdk_dropdown_on_change(FDK_Widget *w, FDK_SelectCb cb, void *ud)
{ w->dropdown.cb = cb; w->dropdown.cb_ud = ud; }

/* ─── Image ──────────────────────────────────────────────────────────────── */
FDK_Widget *fdk_image(const uint8_t *pixels, int width, int height)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_IMAGE);
    if (!w) return NULL;
    if (pixels && width > 0 && height > 0) {
        size_t sz = (size_t)width * height * 4;
        w->image.pixels = malloc(sz);
        if (w->image.pixels) {
            memcpy(w->image.pixels, pixels, sz);
            w->image.w = width;
            w->image.h = height;
        }
    }
    w->w_hint = width;
    w->h_hint = height;
    return w;
}
void fdk_image_set_pixels(FDK_Widget *w, const uint8_t *pixels,
                           int width, int height)
{
    free(w->image.pixels);
    w->image.pixels = NULL;
    w->image.w = width;
    w->image.h = height;
    if (pixels && width > 0 && height > 0) {
        size_t sz = (size_t)width * height * 4;
        w->image.pixels = malloc(sz);
        if (w->image.pixels) memcpy(w->image.pixels, pixels, sz);
    }
}
FDK_Widget *fdk_image_from_file(const char *path)
{
    if (!path) return NULL;

    /* ── STB_IMAGE path (PNG, JPEG, BMP, TGA, …) ────────────────────────── */
#ifdef FDK_WITH_STB_IMAGE
    {
        int w = 0, h = 0, comp = 0;
        unsigned char *data = stbi_load(path, &w, &h, &comp, 4);
        if (data) {
            FDK_Widget *widget = fdk_image(data, w, h);
            stbi_image_free(data);
            return widget;
        }
        /* Fall through to PPM on stb failure (e.g. wrong format) */
    }
#endif

    /* ── Fallback: PPM P6 loader (no extra deps) ─────────────────────────── */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1 || magic[0] != 'P' || magic[1] != '6') {
        fclose(f); return NULL;
    }
    int w = 0, h = 0, maxval = 0;
    if (fscanf(f, " %d %d %d ", &w, &h, &maxval) != 3 ||
        w <= 0 || h <= 0) {
        fclose(f); return NULL;
    }
    size_t rgb_sz = (size_t)w * h * 3;
    uint8_t *rgb = malloc(rgb_sz);
    if (!rgb) { fclose(f); return NULL; }
    if (fread(rgb, 1, rgb_sz, f) != rgb_sz) {
        free(rgb); fclose(f); return NULL;
    }
    fclose(f);
    /* Convert RGB → RGBA */
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) { free(rgb); return NULL; }
    for (int i = 0; i < w * h; i++) {
        rgba[i*4+0] = rgb[i*3+0];
        rgba[i*4+1] = rgb[i*3+1];
        rgba[i*4+2] = rgb[i*3+2];
        rgba[i*4+3] = 255;
    }
    free(rgb);
    FDK_Widget *widget = fdk_image(rgba, w, h);
    free(rgba);
    return widget;
}

/* ─── Tag / ID ───────────────────────────────────────────────────────────── */
void fdk_widget_set_tag(FDK_Widget *w, int tag) { w->tag = tag; }
int  fdk_widget_get_tag(const FDK_Widget *w)    { return w->tag; }

FDK_Widget *fdk_widget_find(FDK_Widget *root, int tag)
{
    if (!root) return NULL;
    if (root->tag == tag) return root;
    for (int i = 0; i < root->child_count; i++) {
        FDK_Widget *r = fdk_widget_find(root->children[i], tag);
        if (r) return r;
    }
    /* Also search scroll view content */
    if (root->type == FDK_WIDGET_SCROLL_VIEW && root->scroll.content) {
        FDK_Widget *r = fdk_widget_find(root->scroll.content, tag);
        if (r) return r;
    }
    return NULL;
}

/* ─── Tooltip ────────────────────────────────────────────────────────────── */
void fdk_widget_set_tooltip(FDK_Widget *w, const char *text)
{
    free(w->tooltip);
    w->tooltip = fdk_strdup(text);
}
const char *fdk_widget_get_tooltip(const FDK_Widget *w) { return w->tooltip; }

/* ─── Variant + per-widget style ────────────────────────────────────────── */
void fdk_widget_set_variant(FDK_Widget *w, const char *variant)
{
    if (!w) return;
    snprintf(w->variant, sizeof w->variant, "%s", variant ? variant : "");
}

void fdk_widget_set_style(FDK_Widget *w, const FDK_WidgetStyle *style)
{
    if (!w) return;
    free(w->style_override);
    if (!style) { w->style_override = NULL; return; }
    w->style_override = malloc(sizeof *style);
    if (w->style_override) *w->style_override = *style;
}

void fdk_widget_set_context_menu(FDK_Widget *w, FDK_ContextMenu *cm)
{
    if (w) w->ctx_menu = cm;
}

/* Helper used by paint: resolve effective style for a widget.
 * Priority: style_override > variant > widget_styles[type] */
static const FDK_WidgetStyle *resolve_style(const FDK_Widget *w,
                                              const FDK_Theme  *th)
{
    if (w->style_override) return w->style_override;
    if (w->variant[0]) {
        /* Look up "type:variant" in theme variant table.
         * Must match the format AND buffer size used in theme.c's
         * fdk_theme_load() — see comment there re: GCC's INT_MIN
         * worst-case analysis on %d. */
        char key[48];
        snprintf(key, sizeof key, "%d:%.31s", (int)w->type, w->variant);
        for (int i = 0; i < th->variant_count; i++)
            if (strcmp(th->variants[i].name, key) == 0)
                return &th->variants[i].style;
    }
    if (w->type < 24) {
        const FDK_WidgetStyle *s = &th->widget_styles[w->type];
        /* Only return if at least one override is set */
        if (s->has_bg || s->has_fg || s->has_radius || s->has_pad)
            return s;
    }
    return NULL;
}

/* ─── Toggle button ──────────────────────────────────────────────────────── */
FDK_Widget *fdk_toggle_button(const char *label, bool active)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_TOGGLE_BUTTON);
    if (!w) return NULL;
    w->toggle.label  = fdk_strdup(label ? label : "");
    w->toggle.active = active;
    w->h_hint = 36;
    return w;
}
bool fdk_toggle_button_get_active(const FDK_Widget *w) { return w->toggle.active; }
void fdk_toggle_button_set_active(FDK_Widget *w, bool a) { w->toggle.active = a; }
void fdk_toggle_button_on_change(FDK_Widget *w, FDK_ClickCb cb, void *ud)
{ w->toggle.cb = cb; w->toggle.cb_ud = ud; }

/* ─── Radio button ───────────────────────────────────────────────────────── */
FDK_Widget *fdk_radio_button(const char *label, int *group, int index)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_RADIO_BUTTON);
    if (!w) return NULL;
    w->radio.label = fdk_strdup(label ? label : "");
    w->radio.group = group;
    w->radio.index = index;
    w->h_hint = 28;
    return w;
}
void fdk_radio_button_on_change(FDK_Widget *w, FDK_ClickCb cb, void *ud)
{ w->radio.cb = cb; w->radio.cb_ud = ud; }

/* ─── Spinner ────────────────────────────────────────────────────────────── */
FDK_Widget *fdk_spinner(double min, double max, double value, double step)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_SPINNER);
    if (!w) return NULL;
    if (value < min) value = min;
    if (value > max) value = max;
    w->spinner.min      = min;
    w->spinner.max      = max;
    w->spinner.value    = value;
    w->spinner.step     = step > 0 ? step : 1.0;
    w->spinner.decimals = 0;
    w->h_hint = 36;
    return w;
}
double fdk_spinner_get_value(const FDK_Widget *w) { return w->spinner.value; }
void   fdk_spinner_set_value(FDK_Widget *w, double v)
{
    if (v < w->spinner.min) v = w->spinner.min;
    if (v > w->spinner.max) v = w->spinner.max;
    w->spinner.value = v;
}
void fdk_spinner_set_range(FDK_Widget *w, double mn, double mx)
{ w->spinner.min = mn; w->spinner.max = mx; }
void fdk_spinner_set_step(FDK_Widget *w, double s)     { w->spinner.step = s; }
void fdk_spinner_set_decimals(FDK_Widget *w, int d)    { w->spinner.decimals = d; }
void fdk_spinner_on_change(FDK_Widget *w, FDK_ValueCb cb, void *ud)
{ w->spinner.cb = cb; w->spinner.cb_ud = ud; }

/* ─── Badge ──────────────────────────────────────────────────────────────── */
FDK_Widget *fdk_badge(const char *text)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_BADGE);
    if (!w) return NULL;
    w->badge.text = fdk_strdup(text ? text : "");
    return w;
}
void fdk_badge_set_text(FDK_Widget *w, const char *t)
{ free(w->badge.text); w->badge.text = fdk_strdup(t ? t : ""); }
void fdk_badge_set_color(FDK_Widget *w, FDK_Color bg, FDK_Color fg)
{ w->badge.bg = bg; w->badge.fg = fg; w->badge.custom_color = true; }

/* ─── Tabs ───────────────────────────────────────────────────────────────── */
FDK_Widget *fdk_tabs(void)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_TABS);
    if (!w) return NULL;
    w->tabs.active = 0;
    w->h_hint      = FDK_SIZE_FILL;
    w->w_hint      = FDK_SIZE_FILL;
    return w;
}

FDK_Widget *fdk_tabs_add_page(FDK_Widget *tabs, const char *label)
{
    if (!tabs || tabs->type != FDK_WIDGET_TABS) return NULL;
    int idx = tabs->tabs.count;
    if (idx >= FDK_TABS_MAX) return NULL;
    tabs->tabs.labels[idx] = fdk_strdup(label ? label : "");
    /* Each page is a vbox stored as a "child" of the tabs widget */
    FDK_Widget *page = fdk_vbox(8, 12);
    page->w_hint = FDK_SIZE_FILL;
    page->h_hint = FDK_SIZE_FILL;
    page->parent = tabs;
    tabs->tabs.pages[idx] = page;
    /* Register in child array too so layout/destroy work */
    if (tabs->child_count < FDK_MAX_CHILDREN)
        tabs->children[tabs->child_count++] = page;
    tabs->tabs.count++;
    /* Hide all but first */
    page->visible = (idx == 0);
    return page;
}

void fdk_tabs_set_active(FDK_Widget *w, int idx)
{
    if (!w || w->type != FDK_WIDGET_TABS) return;
    if (idx < 0 || idx >= w->tabs.count) return;
    for (int i = 0; i < w->tabs.count; i++)
        if (w->tabs.pages[i]) w->tabs.pages[i]->visible = (i == idx);
    w->tabs.active = idx;
}

int fdk_tabs_get_active(const FDK_Widget *w)
{ return w ? w->tabs.active : 0; }

void fdk_tabs_on_change(FDK_Widget *w,
                         void (*cb)(FDK_Widget*, int, void*), void *ud)
{
    if (!w || w->type != FDK_WIDGET_TABS) return;
    w->tabs.cb    = cb;
    w->tabs.cb_ud = ud;
}

/* ─── Menu bar ───────────────────────────────────────────────────────────── */
FDK_Widget *fdk_menubar(void)
{
    FDK_Widget *w = widget_alloc(FDK_WIDGET_MENUBAR);
    if (!w) return NULL;
    w->menubar.open_menu = -1;
    w->w_hint = FDK_SIZE_FILL;
    w->h_hint = 28;
    return w;
}

int fdk_menubar_add_menu(FDK_Widget *mb, const char *label)
{
    if (!mb || mb->type != FDK_WIDGET_MENUBAR) return -1;
    int idx = mb->menubar.count;
    if (idx >= FDK_MENU_MAX_MENUS) return -1;
    mb->menubar.menus[idx].label      = fdk_strdup(label ? label : "");
    mb->menubar.menus[idx].item_count = 0;
    mb->menubar.menus[idx].open       = false;
    mb->menubar.count++;
    return idx;
}

void fdk_menu_add_item(FDK_Widget *mb, int menu_idx,
                        const char *label, const char *shortcut,
                        void (*cb)(void *ud), void *ud)
{
    if (!mb || mb->type != FDK_WIDGET_MENUBAR) return;
    if (menu_idx < 0 || menu_idx >= mb->menubar.count) return;
    FDK_Menu *m = &mb->menubar.menus[menu_idx];
    if (m->item_count >= FDK_MENU_MAX_ITEMS) return;
    FDK_MenuItem *it = &m->items[m->item_count++];
    it->label     = fdk_strdup(label ? label : "");
    it->shortcut  = shortcut ? fdk_strdup(shortcut) : NULL;
    it->separator = false;
    it->disabled  = false;
    it->cb        = cb;
    it->ud        = ud;
}

void fdk_menu_add_separator(FDK_Widget *mb, int menu_idx)
{
    if (!mb || mb->type != FDK_WIDGET_MENUBAR) return;
    if (menu_idx < 0 || menu_idx >= mb->menubar.count) return;
    FDK_Menu *m = &mb->menubar.menus[menu_idx];
    if (m->item_count >= FDK_MENU_MAX_ITEMS) return;
    FDK_MenuItem *it = &m->items[m->item_count++];
    memset(it, 0, sizeof *it);
    it->separator = true;
}

/* ─── Notification / toast ───────────────────────────────────────────────── */
void fdk_notify(FDK_UI *ui, const char *message,
                FDK_NotifyKind kind, uint32_t duration_ms)
{
    if (!ui || !message) return;
    if (duration_ms == 0) duration_ms = 2500;

    /* Find a free slot — evict oldest active if full */
    int slot = -1;
    for (int i = 0; i < FDK_TOAST_MAX; i++) {
        if (!ui->toasts[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        /* Evict the toast that was created earliest */
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < FDK_TOAST_MAX; i++) {
            if (ui->toasts[i].created_ms < oldest) {
                oldest = ui->toasts[i].created_ms;
                slot   = i;
            }
        }
    }

    ui->toasts[slot].kind        = kind;
    ui->toasts[slot].created_ms  = fdk_time_ms();
    ui->toasts[slot].duration_ms = duration_ms;
    ui->toasts[slot].slide_y     = 60.f;
    ui->toasts[slot].alpha       = 0.f;
    ui->toasts[slot].active      = true;
    ui->toasts[slot].dismissing  = false;
    snprintf(ui->toasts[slot].message, sizeof ui->toasts[slot].message,
             "%s", message);
    ui->dirty = true;
}

/* Measure intrinsic size of a leaf widget (no children) */
static FDK_Size measure_widget(const FDK_Widget *w, const FDK_Theme *th)
{
    FDK_Size s = {0, 0};

    /* For containers, return fixed hint if set; otherwise measure children */
    if (w->type == FDK_WIDGET_CONTAINER) {
        bool w_wrap = (w->w_hint == FDK_SIZE_WRAP || w->w_hint == FDK_SIZE_FILL);
        bool h_wrap = (w->h_hint == FDK_SIZE_WRAP || w->h_hint == FDK_SIZE_FILL);
        if (!w_wrap) s.w = w->w_hint;
        if (!h_wrap) s.h = w->h_hint;

        if (w_wrap || h_wrap) {
            /* Measure children to derive natural size */
            bool vert = (w->layout.dir == FDK_LAYOUT_VERTICAL);
            int main_total = 0, cross_max = 0, vis = 0;
            for (int i = 0; i < w->child_count; i++) {
                FDK_Widget *c = w->children[i];
                if (!c->visible) continue;
                FDK_Size cs = measure_widget(c, th);
                int cm = vert ? cs.h : cs.w;
                int cc = vert ? cs.w : cs.h;
                /* FILL children contribute 0 to wrap measurement */
                if ((vert ? c->h_hint : c->w_hint) != FDK_SIZE_FILL)
                    main_total += cm;
                if (cc > cross_max) cross_max = cc;
                vis++;
            }
            int gaps = vis > 1 ? w->layout.gap * (vis - 1) : 0;
            int pad2 = w->layout.padding * 2;
            if (h_wrap) s.h = (vert  ? main_total + gaps : cross_max) + pad2;
            if (w_wrap) s.w = (!vert ? main_total + gaps : cross_max) + pad2;
        }

        if (s.w < w->min_w) s.w = w->min_w;
        if (s.h < w->min_h) s.h = w->min_h;
        return s;
    }

    switch (w->type) {
    case FDK_WIDGET_LABEL:
        if (th->font_body && w->label.text)
            s = fdk_measure_text(th->font_body, w->label.text);
        else
            s = (FDK_Size){ 60, font_line_h(th->font_body, th) };
        break;
    case FDK_WIDGET_BUTTON: {
        FDK_Size ts = {0, 0};
        if (th->font_body && w->button.label)
            ts = fdk_measure_text(th->font_body, w->button.label);
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        int pad_h = (ws && ws->has_pad) ? ws->pad_h : 16;  /* per-side */
        int pad_v = (ws && ws->has_pad) ? ws->pad_v : 8;
        s.w = ts.w + pad_h * 2;
        s.h = font_line_h(th->font_body, th) + pad_v * 2;
        if (s.h < 36) s.h = 36;  /* sensible minimum touch target */
        break;
    }
    case FDK_WIDGET_TEXT_INPUT:
        s.w = 120;
        s.h = 36;
        break;
    case FDK_WIDGET_CHECKBOX: {
        FDK_Size ts = {0, 0};
        if (th->font_body && w->checkbox.label)
            ts = fdk_measure_text(th->font_body, w->checkbox.label);
        s.w = 20 + 6 + ts.w;   /* box + gap + text */
        s.h = 28;
        break;
    }
    case FDK_WIDGET_SEPARATOR:
        s.w = 1; s.h = 2;   /* 2px line; gap in parent provides visual spacing */
        break;
    case FDK_WIDGET_CUSTOM:
        s.w = (w->w_hint > 0) ? w->w_hint : 0;
        s.h = (w->h_hint > 0) ? w->h_hint : 0;
        break;
    case FDK_WIDGET_SLIDER:
        s.w = 120; s.h = 28;
        break;
    case FDK_WIDGET_PROGRESS_BAR:
        s.w = 60; s.h = 8;
        break;
    case FDK_WIDGET_SCROLL_VIEW:
        s.w = (w->w_hint > 0) ? w->w_hint : 120;
        s.h = (w->h_hint > 0) ? w->h_hint : 120;
        break;
    case FDK_WIDGET_DROPDOWN:
        s.w = 120; s.h = 36;
        break;
    case FDK_WIDGET_IMAGE:
        s.w = w->image.w > 0 ? w->image.w : 0;
        s.h = w->image.h > 0 ? w->image.h : 0;
        break;
    case FDK_WIDGET_TOGGLE_BUTTON: {
        FDK_Size ts = {0, 0};
        if (th->font_body && w->toggle.label)
            ts = fdk_measure_text(th->font_body, w->toggle.label);
        s.w = ts.w + 32;
        s.h = 36;
        break;
    }
    case FDK_WIDGET_RADIO_BUTTON: {
        FDK_Size ts = {0, 0};
        if (th->font_body && w->radio.label)
            ts = fdk_measure_text(th->font_body, w->radio.label);
        s.w = 18 + 6 + ts.w;   /* dot + gap + text */
        s.h = 28;
        break;
    }
    case FDK_WIDGET_SPINNER:
        s.w = 120; s.h = 36;
        break;
    case FDK_WIDGET_BADGE: {
        FDK_Size ts = {0, 0};
        if (th->font_label && w->badge.text)
            ts = fdk_measure_text(th->font_label, w->badge.text);
        s.w = ts.w + 12;
        s.h = font_line_h(th->font_label, th) + 6;
        break;
    }
    case FDK_WIDGET_TABS:
        /* Tab bar height = 36; total height from parent layout (FILL) */
        s.w = 0; s.h = 36;
        break;
    case FDK_WIDGET_MENUBAR:
        s.w = 0; s.h = 28;
        break;
    case FDK_WIDGET_TEXTAREA:
        s.w = 160; s.h = 120;
        break;
    default:
        break;
    }

    /* Explicit fixed pixel overrides always win over intrinsic size */
    if (w->w_hint > 0) s.w = w->w_hint;
    if (w->h_hint > 0) s.h = w->h_hint;

    if (s.w < w->min_w) s.w = w->min_w;
    if (s.h < w->min_h) s.h = w->min_h;
    return s;
}

/* Forward declaration */
static void layout_widget(FDK_Widget *w, FDK_Rect bounds,
                           const FDK_Theme *th,
                           FDK_Widget **tab, int *tab_cnt);

static void layout_container(FDK_Widget *w, FDK_Rect bounds,
                              const FDK_Theme *th,
                              FDK_Widget **tab, int *tab_cnt)
{
    int pad = w->layout.padding;
    int gap = w->layout.gap;
    bool vert = (w->layout.dir == FDK_LAYOUT_VERTICAL);

    /* Inner area after padding */
    FDK_Rect inner = {
        bounds.x + pad,
        bounds.y + pad,
        bounds.w - pad * 2,
        bounds.h - pad * 2
    };

    /* Count visible children and measure fixed ones */
    int    vis      = 0;
    int    fill_cnt = 0;
    int    fixed_total = 0;

    for (int i = 0; i < w->child_count; i++) {
        FDK_Widget *c = w->children[i];
        if (!c->visible) continue;
        vis++;
        int hint = vert ? c->h_hint : c->w_hint;
        if (hint == FDK_SIZE_FILL)
            fill_cnt++;
        else {
            FDK_Size ms = measure_widget(c, th);
            fixed_total += (hint == FDK_SIZE_WRAP)
                               ? (vert ? ms.h : ms.w)
                               : hint;
        }
    }

    int total_gaps   = (vis > 1) ? gap * (vis - 1) : 0;
    int avail        = (vert ? inner.h : inner.w) - fixed_total - total_gaps;
    int fill_each    = (fill_cnt > 0 && avail > 0)
                           ? avail / fill_cnt : 0;

    /* Place each child */
    int cursor = vert ? inner.y : inner.x;

    for (int i = 0; i < w->child_count; i++) {
        FDK_Widget *c = w->children[i];
        if (!c->visible) continue;

        int hint_main  = vert ? c->h_hint : c->w_hint;
        int hint_cross = vert ? c->w_hint : c->h_hint;
        FDK_Size ms    = measure_widget(c, th);

        int main_sz = (hint_main == FDK_SIZE_FILL)  ? fill_each
                    : (hint_main == FDK_SIZE_WRAP)   ? (vert ? ms.h : ms.w)
                    : hint_main;

        int cross_avail = vert ? inner.w : inner.h;
        int cross_sz;
        if (hint_cross == FDK_SIZE_FILL || hint_cross == FDK_SIZE_WRAP)
            cross_sz = (w->layout.align == FDK_ALIGN_STRETCH)
                           ? cross_avail
                           : (vert ? ms.w : ms.h);
        else
            cross_sz = hint_cross;

        FDK_Rect child_bounds;
        if (vert) {
            int cx = inner.x;
            if (w->layout.align == FDK_ALIGN_CENTER)
                cx = inner.x + (inner.w - cross_sz) / 2;
            else if (w->layout.align == FDK_ALIGN_END)
                cx = inner.x + inner.w - cross_sz;
            child_bounds = (FDK_Rect){ cx, cursor, cross_sz, main_sz };
        } else {
            int cy = inner.y;
            if (w->layout.align == FDK_ALIGN_CENTER)
                cy = inner.y + (inner.h - cross_sz) / 2;
            else if (w->layout.align == FDK_ALIGN_END)
                cy = inner.y + inner.h - cross_sz;
            child_bounds = (FDK_Rect){ cursor, cy, main_sz, cross_sz };
        }

        layout_widget(c, child_bounds, th, tab, tab_cnt);
        cursor += main_sz + gap;
    }
}

static void layout_widget(FDK_Widget *w, FDK_Rect bounds,
                           const FDK_Theme *th,
                           FDK_Widget **tab, int *tab_cnt)
{
    /* Apply max size constraints */
    if (w->max_w > 0 && bounds.w > w->max_w) {
        /* Centre the widget within the available space */
        bounds.x += (bounds.w - w->max_w) / 2;
        bounds.w  = w->max_w;
    }
    if (w->max_h > 0 && bounds.h > w->max_h)
        bounds.h = w->max_h;

    w->rect = bounds;

    /* Add to tab order if focusable */
    if (w->enabled && w->visible &&
        (w->type == FDK_WIDGET_BUTTON        ||
         w->type == FDK_WIDGET_TEXT_INPUT     ||
         w->type == FDK_WIDGET_CHECKBOX       ||
         w->type == FDK_WIDGET_SLIDER         ||
         w->type == FDK_WIDGET_DROPDOWN       ||
         w->type == FDK_WIDGET_TOGGLE_BUTTON  ||
         w->type == FDK_WIDGET_RADIO_BUTTON   ||
         w->type == FDK_WIDGET_SPINNER        ||
         w->type == FDK_WIDGET_TEXTAREA) &&
        *tab_cnt < 255)
        tab[(*tab_cnt)++] = w;

    if (w->type == FDK_WIDGET_CONTAINER)
        layout_container(w, bounds, th, tab, tab_cnt);

    /* Tabs: lay out the bar children (pages) in the content rect below the bar */
    if (w->type == FDK_WIDGET_TABS) {
        int bar_h = 36;
        FDK_Rect page_bounds = {
            bounds.x, bounds.y + bar_h,
            bounds.w, bounds.h - bar_h
        };
        for (int i = 0; i < w->tabs.count; i++) {
            FDK_Widget *pg = w->tabs.pages[i];
            if (!pg) continue;
            /* Layout all pages so hit-test rects are correct even when hidden */
            layout_widget(pg, page_bounds, th, tab, tab_cnt);
        }
    }

    /* Scroll view: lay out content at its natural size, offset by scroll */
    if (w->type == FDK_WIDGET_SCROLL_VIEW && w->scroll.content) {
        FDK_Widget *content = w->scroll.content;
        FDK_Size    cs      = measure_widget(content, th);
        FDK_Rect content_bounds = {
            bounds.x - w->scroll.scroll_x,
            bounds.y - w->scroll.scroll_y,
            cs.w > bounds.w ? cs.w : bounds.w,
            cs.h > bounds.h ? cs.h : bounds.h
        };
        w->scroll.content_w = content_bounds.w;
        w->scroll.content_h = content_bounds.h;
        layout_widget(content, content_bounds, th, tab, tab_cnt);
    }
}

/* Draw overlay elements (open dropdowns) on top of the entire widget tree */
/* Forward declarations needed by paint_overlays */
static bool rect_contains(FDK_Rect r, int x, int y);
static void paint_toasts(FDK_UI *ui);
static void paint_widget(FDK_Widget *w, const FDK_UI *ui);

static void paint_overlays(FDK_Widget *w, const FDK_UI *ui)
{
    const FDK_Theme *th = &ui->theme;

    if (w->type == FDK_WIDGET_DROPDOWN && w->dropdown.open &&
        w->dropdown.item_count > 0) {
        FDK_Rect r      = w->rect;
        int item_h      = 30;
        int popup_h     = w->dropdown.item_count * item_h + 2;
        FDK_Rect popup  = { r.x, r.y + r.h, r.w, popup_h };

        fdk_fill_rect_rounded(popup, th->radius_sm, th->bg_widget);
        fdk_stroke_rect(popup, th->border_focus, 1);

        for (int i = 0; i < w->dropdown.item_count; i++) {
            FDK_Rect ir = { popup.x + 1, popup.y + 1 + i * item_h,
                            popup.w - 2, item_h };
            if (i == w->dropdown.hovered_item)
                fdk_fill_rect(ir, FDK_RGBA(138, 99, 210, 35));
            if (i == w->dropdown.selected)
                fdk_fill_rect(ir, FDK_RGBA(138, 99, 210, 70));
            if (th->font_body) {
                int lh = font_line_h(th->font_body, th);
                int ty = ir.y + (ir.h - lh) / 2;
                fdk_push_clip(ir);
                fdk_draw_text(th->font_body, w->dropdown.items[i],
                              ir.x + 8, ty, th->fg_primary);
                fdk_pop_clip();
            }
            if (i < w->dropdown.item_count - 1)
                fdk_fill_rect(
                    (FDK_Rect){ ir.x + 4, ir.y + item_h - 1, ir.w - 8, 1 },
                    th->separator);
        }
    }

    /* Menubar open popup */
    if (w->type == FDK_WIDGET_MENUBAR && w->menubar.open_menu >= 0) {
        int mi = w->menubar.open_menu;
        FDK_Menu *m = &w->menubar.menus[mi];
        FDK_Font *f = th->font_body;
        int item_h  = 28;
        int sep_h   = 9;

        /* Calculate total popup height (separators are shorter) */
        int popup_h = 4;
        for (int i = 0; i < m->item_count; i++)
            popup_h += m->items[i].separator ? sep_h : item_h;

        /* Calculate x position of this menu title */
        int mx = w->rect.x + 8;
        int pad = 12;
        for (int i = 0; i < mi; i++) {
            if (!w->menubar.menus[i].label) continue;
            FDK_Size ts = f ? fdk_measure_text(f, w->menubar.menus[i].label)
                            : (FDK_Size){60, 16};
            mx += ts.w + pad * 2;
        }
        int popup_w = 200;
        FDK_Size wsz = fdk_window_get_size(ui->win);
        if (mx + popup_w > wsz.w) mx = wsz.w - popup_w - 4;

        FDK_Rect popup = { mx, w->rect.y + w->rect.h, popup_w, popup_h };
        fdk_fill_rect_rounded(popup, th->radius_sm, th->bg_widget);
        fdk_stroke_rect(popup, th->border_focus, 1);

        int iy = popup.y + 4;
        int lh = f ? font_line_h(f, th) : 16;
        for (int i = 0; i < m->item_count; i++) {
            FDK_MenuItem *it = &m->items[i];
            if (it->separator) {
                fdk_fill_rect(
                    (FDK_Rect){ popup.x + 8, iy + sep_h/2,
                                popup.w - 16, 1 }, th->separator);
                iy += sep_h;
                continue;
            }
            FDK_Rect ir = { popup.x + 1, iy, popup.w - 2, item_h };
            bool hovered = rect_contains(ir, ui->mouse_x, ui->mouse_y);
            if (hovered && !it->disabled)
                fdk_fill_rect_rounded(ir, th->radius_sm,
                                      th->bg_widget_hover);
            if (f && it->label) {
                FDK_Color fc = it->disabled ? th->fg_disabled : th->fg_primary;
                fdk_draw_text(f, it->label, ir.x + 12,
                              iy + (item_h - lh) / 2, fc);
            }
            if (f && it->shortcut) {
                FDK_Size ss = fdk_measure_text(f, it->shortcut);
                fdk_draw_text(f, it->shortcut,
                              popup.x + popup_w - ss.w - 12,
                              iy + (item_h - lh) / 2, th->fg_disabled);
            }
            iy += item_h;
        }
    }

    /* Recurse */
    for (int i = 0; i < w->child_count; i++)
        paint_overlays(w->children[i], ui);
    if (w->type == FDK_WIDGET_SCROLL_VIEW && w->scroll.content)
        paint_overlays(w->scroll.content, ui);
}

static void do_layout(FDK_UI *ui, FDK_Widget *root)
{
    FDK_Size wsz = fdk_window_get_size(ui->win);
    ui->tab_count = 0;
    layout_widget(root, (FDK_Rect){0, 0, wsz.w, wsz.h},
                  &ui->theme, ui->tab_order, &ui->tab_count);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PAINT PASS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void paint_widget(FDK_Widget *w, const FDK_UI *ui)
{
    if (!w->visible) return;

    const FDK_Theme *th = &ui->theme;
    FDK_Rect r = w->rect;

    /* ── Shadow + gradient pre-pass ──
     * Drawn before the widget's own background so they appear behind it.
     * Source priority: style_override > variant > widget_styles[type] */
    {
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        if (ws) {
            if (ws->has_shadow)
                fdk_draw_shadow(r, th->radius_sm, &ws->shadow);
            if (ws->has_gradient)
                fdk_fill_rect_gradient(r, th->radius_sm, &ws->gradient);
        }
    }

    switch (w->type) {

    /* ── Container ── */
    case FDK_WIDGET_CONTAINER:
        for (int i = 0; i < w->child_count; i++)
            paint_widget(w->children[i], ui);
        break;

    /* ── Label ── */
    case FDK_WIDGET_LABEL: {
        if (!w->label.text || !th->font_body) break;
        FDK_Color col = w->label.use_color
                            ? w->label.color
                            : (w->enabled ? th->fg_primary : th->fg_disabled);
        /* Vertically centre: fdk_draw_text takes the top-left of the
         * ascender box, so offset by (height - line_height) / 2 */
        int lh = font_line_h(th->font_body, th);
        int ty = r.y + (r.h - lh) / 2;
        fdk_draw_text(th->font_body, w->label.text, r.x, ty, col);
        break;
    }

    /* ── Button ── */
    case FDK_WIDGET_BUTTON: {
        const FDK_WidgetStyle *ws = resolve_style(w, th);

        /* Background: style override > variant > theme accent states */
        FDK_Color bg;
        if (ws && ws->has_bg) {
            bg = !w->enabled        ? th->bg_widget
               : w->button.pressed  ? (ws->has_bg_active ? ws->bg_active : ws->bg)
               : w->hovered         ? (ws->has_bg_hover  ? ws->bg_hover  : ws->bg)
               :                       ws->bg;
        } else {
            bg = !w->enabled           ? th->bg_widget
               : w->button.pressed     ? th->accent_active
               : w->hovered            ? th->accent_hover
               :                         th->accent;
        }

        int radius = (ws && ws->has_radius) ? ws->radius : th->radius_sm;

        fdk_fill_rect_rounded(r, radius, bg);

        /* Border — only drawn if the style/variant specifies one
         * (e.g. [button.ghost] border = $purple_lo) */
        if (ws && ws->has_border) {
            FDK_Color bc = (w->focused && ws->has_border_focus)
                          ? ws->border_focus : ws->border;
            fdk_stroke_rect(r, bc, 1);
        }

        /* Focus ring */
        if (w->focused) {
            FDK_Rect fr = { r.x-2, r.y-2, r.w+4, r.h+4 };
            fdk_stroke_rect(fr, th->border_focus, 2);
        }

        if (w->button.label && th->font_body) {
            FDK_Color fg = !w->enabled ? th->fg_disabled
                          : (ws && ws->has_fg) ? ws->fg
                          : th->fg_on_accent;
            FDK_Size ts  = fdk_measure_text(th->font_body, w->button.label);
            int lh = font_line_h(th->font_body, th);
            int tx = r.x + (r.w - ts.w) / 2;
            int ty = r.y + (r.h - lh) / 2;
            fdk_draw_text(th->font_body, w->button.label, tx, ty, fg);
        }
        break;
    }

    /* ── Text input ── */
    case FDK_WIDGET_TEXT_INPUT: {
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        int radius = (ws && ws->has_radius) ? ws->radius : th->radius_sm;
        FDK_Color bg = w->focused
            ? ((ws && ws->has_bg_hover) ? ws->bg_hover : th->bg_input_focus)
            : ((ws && ws->has_bg)       ? ws->bg       : th->bg_input);
        fdk_fill_rect_rounded(r, radius, bg);

        FDK_Color border_col = w->focused
            ? ((ws && ws->has_border_focus) ? ws->border_focus : th->border_focus)
            : ((ws && ws->has_border)       ? ws->border       : th->border);
        fdk_stroke_rect(r, border_col, 1);

        int pad = (ws && ws->has_pad) ? ws->pad_h : 8;
        fdk_push_clip((FDK_Rect){ r.x+1, r.y+1, r.w-2, r.h-2 });

        int lh = font_line_h(th->font_body, th);
        int ty = r.y + (r.h - lh) / 2;
        bool has_text = w->input.buf && w->input.buf[0] != '\0';
        if (has_text && th->font_body) {
            fdk_draw_text(th->font_body, w->input.buf,
                          r.x + pad - w->input.scroll_x, ty, th->fg_primary);
        } else if (!has_text && w->input.placeholder && th->font_body) {
            fdk_draw_text(th->font_body, w->input.placeholder,
                          r.x + pad, ty, th->fg_secondary);
        }

        /* Selection highlight */
        if (w->input.has_sel && th->font_body) {
            int s0 = w->input.sel_start < w->input.sel_end
                         ? w->input.sel_start : w->input.sel_end;
            int s1 = w->input.sel_start < w->input.sel_end
                         ? w->input.sel_end : w->input.sel_start;
            char tmp[512];
            int c0 = s0 < (int)sizeof(tmp)-1 ? s0 : (int)sizeof(tmp)-1;
            int c1 = s1 < (int)sizeof(tmp)-1 ? s1 : (int)sizeof(tmp)-1;
            memcpy(tmp, w->input.buf, c0); tmp[c0] = '\0';
            FDK_Size ws0 = fdk_measure_text(th->font_body, tmp);
            memcpy(tmp, w->input.buf, c1); tmp[c1] = '\0';
            FDK_Size ws1 = fdk_measure_text(th->font_body, tmp);
            int sx0 = r.x + pad + ws0.w - w->input.scroll_x;
            int sx1 = r.x + pad + ws1.w - w->input.scroll_x;
            FDK_Rect sel_r = { sx0, ty, sx1 - sx0, lh };
            fdk_fill_rect(sel_r, th->bg_selection);
            /* Redraw text on top of highlight */
            if (has_text)
                fdk_draw_text(th->font_body, w->input.buf,
                              r.x + pad - w->input.scroll_x, ty, th->fg_primary);
        }

        /* Cursor — blink driven by ui->blink_on */
        if (w->focused && th->font_body && ((const FDK_UI*)ui)->blink_on) {
            char tmp[512];
            int cpos = w->input.cursor;
            if (cpos > (int)sizeof(tmp)-1) cpos = (int)sizeof(tmp)-1;
            memcpy(tmp, w->input.buf, cpos);
            tmp[cpos] = '\0';
            FDK_Size cs = fdk_measure_text(th->font_body, tmp);
            int cx  = r.x + pad + cs.w - w->input.scroll_x;
            int cy1 = ty;          /* ty is already absolute */
            int cy2 = ty + lh;
            fdk_draw_line(cx, cy1, cx, cy2, th->accent, 2);
        }

        fdk_pop_clip();
        break;
    }

    /* ── Checkbox ── */
    case FDK_WIDGET_CHECKBOX: {
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        int box_sz = 18;
        int bx = r.x;
        int by = r.y + (r.h - box_sz) / 2;
        FDK_Rect box = { bx, by, box_sz, box_sz };
        int radius = (ws && ws->has_radius) ? ws->radius : 4;

        FDK_Color bg = w->checkbox.checked ? th->accent
                     : (ws && ws->has_bg)  ? ws->bg
                     : th->bg_input;
        fdk_fill_rect_rounded(box, radius, bg);

        FDK_Color border_col = w->focused
            ? ((ws && ws->has_border_focus) ? ws->border_focus : th->border_focus)
            : ((ws && ws->has_border)       ? ws->border       : th->border);
        fdk_stroke_rect(box, border_col, 1);

        /* Check mark */
        if (w->checkbox.checked) {
            int mx = bx + 3, my = by + box_sz / 2;
            fdk_draw_line(mx,     my,     mx+4, my+4, th->fg_on_accent, 2);
            fdk_draw_line(mx+4,   my+4,   mx+11, my-4, th->fg_on_accent, 2);
        }

        if (w->checkbox.label && th->font_body) {
            FDK_Color fg = w->enabled ? th->fg_primary : th->fg_disabled;
            int lh2 = font_line_h(th->font_body, th);
            int ty2 = r.y + (r.h - lh2) / 2;
            fdk_draw_text(th->font_body, w->checkbox.label,
                          bx + box_sz + 6, ty2, fg);
        }
        break;
    }

    /* ── Separator ── */
    case FDK_WIDGET_SEPARATOR:
        fdk_fill_rect(r, th->separator);
        break;

    /* ── Custom ── */
    case FDK_WIDGET_CUSTOM:
        if (w->custom.draw_cb)
            w->custom.draw_cb((FDK_Widget*)w, r, w->custom.cb_ud);
        break;

    /* ── Slider ── */
    case FDK_WIDGET_SLIDER: {
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        int track_h  = 4;
        int thumb_r  = 8;
        int track_y  = r.y + (r.h - track_h) / 2;
        int track_x0 = r.x + thumb_r;
        int track_x1 = r.x + r.w - thumb_r;
        int track_w  = track_x1 - track_x0;

        float t = (w->slider.max > w->slider.min)
                      ? (w->slider.value - w->slider.min) /
                        (w->slider.max  - w->slider.min)
                      : 0.f;
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        int thumb_x = track_x0 + (int)(t * track_w);

        FDK_Color track_bg = (ws && ws->has_bg) ? ws->bg : th->bg_widget;
        fdk_fill_rect_rounded(
            (FDK_Rect){ track_x0, track_y, track_w, track_h },
            track_h / 2, track_bg);

        FDK_Color accent_col = (ws && ws->has_fg) ? ws->fg : th->accent;
        int fill_w = thumb_x - track_x0;
        if (fill_w > 0)
            fdk_fill_rect_rounded(
                (FDK_Rect){ track_x0, track_y, fill_w, track_h },
                track_h / 2,
                w->enabled ? accent_col : th->fg_disabled);

        FDK_Color thumb_col = !w->enabled         ? th->fg_disabled
                            : w->slider.dragging   ? th->accent_active
                            : w->hovered           ? th->accent_hover
                            :                        accent_col;
        fdk_fill_circle(thumb_x, r.y + r.h / 2, thumb_r, thumb_col);
        if (w->focused) {
            FDK_Color fc = (ws && ws->has_border_focus) ? ws->border_focus
                                                        : th->border_focus;
            fdk_stroke_rect(
                (FDK_Rect){ thumb_x - thumb_r - 2, r.y + r.h/2 - thumb_r - 2,
                            (thumb_r + 2) * 2, (thumb_r + 2) * 2 },
                fc, 2);
        }
        break;
    }

    /* ── Progress bar ── */
    case FDK_WIDGET_PROGRESS_BAR: {
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        int rad = r.h / 2;
        FDK_Color track_bg = (ws && ws->has_bg) ? ws->bg : th->bg_widget;
        fdk_fill_rect_rounded(r, rad, track_bg);

        FDK_Color fill_col = (ws && ws->has_fg) ? ws->fg : th->accent;
        fdk_push_clip(r);
        if (w->progress.indeterminate) {
            float pos   = w->progress.anim_pos;
            int bar_w   = r.w / 3;
            int bar_x   = r.x + (int)(pos * (float)(r.w + bar_w)) - bar_w;
            FDK_Rect bar = { bar_x, r.y, bar_w, r.h };
            fdk_fill_rect_rounded(bar, rad, fill_col);
        } else {
            int fill_w = (int)(w->progress.value * (float)r.w + 0.5f);
            if (fill_w > 0) {
                if (fill_w > r.w) fill_w = r.w;
                FDK_Rect fill = { r.x, r.y, fill_w, r.h };
                int fill_rad = fill.h / 2;
                if (fill_rad > fill.w / 2) fill_rad = fill.w / 2;
                fdk_fill_rect_rounded(fill, fill_rad, fill_col);
            }
        }
        fdk_pop_clip();
        break;
    }

    /* ── Scroll view ── */
    case FDK_WIDGET_SCROLL_VIEW: {
        fdk_push_clip(r);
        if (w->scroll.content)
            paint_widget(w->scroll.content, ui);
        fdk_pop_clip();

        /* Vertical scrollbar — show only if content taller than viewport */
        if (w->scroll.content_h > r.h) {
            int sb_w  = 4;
            int sb_x  = r.x + r.w - sb_w - 2;
            int sb_h  = r.h * r.h / w->scroll.content_h;
            if (sb_h < 16) sb_h = 16;
            int sb_y  = r.y + (int)((float)w->scroll.scroll_y /
                             (w->scroll.content_h - r.h) *
                             (r.h - sb_h));
            fdk_fill_rect_rounded(
                (FDK_Rect){ sb_x, sb_y, sb_w, sb_h },
                sb_w / 2, FDK_RGBA(255,255,255,60));
        }
        /* Horizontal scrollbar */
        if (w->scroll.content_w > r.w) {
            int sb_h  = 4;
            int sb_y  = r.y + r.h - sb_h - 2;
            int sb_w2 = r.w * r.w / w->scroll.content_w;
            if (sb_w2 < 16) sb_w2 = 16;
            int sb_x  = r.x + (int)((float)w->scroll.scroll_x /
                             (w->scroll.content_w - r.w) *
                             (r.w - sb_w2));
            fdk_fill_rect_rounded(
                (FDK_Rect){ sb_x, sb_y, sb_w2, sb_h },
                sb_h / 2, FDK_RGBA(255,255,255,60));
        }
        break;
    }

    /* ── Dropdown ── */
    case FDK_WIDGET_DROPDOWN: {
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        int radius = (ws && ws->has_radius) ? ws->radius : th->radius_sm;
        FDK_Color bg = w->hovered && !w->dropdown.open
            ? ((ws && ws->has_bg_hover) ? ws->bg_hover : th->bg_widget_hover)
            : ((ws && ws->has_bg)       ? ws->bg       : th->bg_input);
        fdk_fill_rect_rounded(r, radius, bg);

        FDK_Color border_col = w->focused
            ? ((ws && ws->has_border_focus) ? ws->border_focus : th->border_focus)
            : ((ws && ws->has_border)       ? ws->border       : th->border);
        fdk_stroke_rect(r, border_col, 1);

        /* Current text */
        const char *txt = (w->dropdown.selected >= 0 &&
                           w->dropdown.selected < w->dropdown.item_count)
                              ? w->dropdown.items[w->dropdown.selected]
                              : w->dropdown.placeholder;
        FDK_Color fg = (w->dropdown.selected >= 0)
                           ? th->fg_primary : th->fg_secondary;
        if (th->font_body && txt) {
            int lh = font_line_h(th->font_body, th);
            int ty = r.y + (r.h - lh) / 2;
            fdk_push_clip((FDK_Rect){ r.x+1, r.y+1, r.w-28, r.h-2 });
            fdk_draw_text(th->font_body, txt, r.x + 8, ty, fg);
            fdk_pop_clip();
        }

        /* Chevron */
        int cx = r.x + r.w - 18;
        int cy = r.y + r.h / 2;
        fdk_draw_line(cx - 4, cy - 3, cx,     cy + 3, th->fg_secondary, 2);
        fdk_draw_line(cx,     cy + 3, cx + 4, cy - 3, th->fg_secondary, 2);

        /* Popup is drawn in a second pass after all widgets, so it
         * appears on top of everything — see paint_widget_overlays() */
        break;
    }

    /* ── Image ── */
    case FDK_WIDGET_IMAGE: {
        if (!w->image.pixels || w->image.w <= 0 || w->image.h <= 0) break;
        /* Blit RGBA pixels into the current frame using fill_rect per-pixel.
         * This is slow for large images — the GL backend should use a texture.
         * For now it works correctly on the software backend. */
        int dst_w = r.w < w->image.w ? r.w : w->image.w;
        int dst_h = r.h < w->image.h ? r.h : w->image.h;
        fdk_push_clip(r);
        for (int y = 0; y < dst_h; y++) {
            for (int x = 0; x < dst_w; x++) {
                const uint8_t *px = w->image.pixels +
                                    (y * w->image.w + x) * 4;
                FDK_Color col = FDK_RGBA(px[0], px[1], px[2], px[3]);
                fdk_fill_rect((FDK_Rect){ r.x + x, r.y + y, 1, 1 }, col);
            }
        }
        fdk_pop_clip();
        break;
    }

    /* ── Toggle button ── */
    case FDK_WIDGET_TOGGLE_BUTTON: {
        bool active = w->toggle.active;
        /* When active, try the "on" variant first, then fall back to accent */
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        const FDK_WidgetStyle *ws_on = NULL;
        if (active) {
            /* Temporarily set variant to "on" to resolve [toggle_button.on] */
            char saved_variant[48];
            memcpy(saved_variant, w->variant, sizeof saved_variant);
            snprintf(((FDK_Widget*)w)->variant,
                     sizeof w->variant, "%s", "on");
            ws_on = resolve_style(w, th);
            memcpy(((FDK_Widget*)w)->variant, saved_variant,
                   sizeof w->variant);
        }
        const FDK_WidgetStyle *wse = (active && ws_on) ? ws_on : ws;

        FDK_Color bg = active
            ? ((wse && wse->has_bg)      ? wse->bg      : th->accent)
            : w->hovered
            ? ((ws && ws->has_bg_hover)  ? ws->bg_hover : th->bg_widget_hover)
            : ((ws && ws->has_bg)        ? ws->bg       : th->bg_widget);
        FDK_Color fg = active
            ? ((wse && wse->has_fg)      ? wse->fg      : th->fg_on_accent)
            : ((ws && ws->has_fg)        ? ws->fg       : th->fg_primary);
        int radius = (ws && ws->has_radius) ? ws->radius : th->radius_sm;

        fdk_push_clip(r);
        fdk_fill_rect(r, th->bg_window);
        fdk_fill_rect_rounded(r, radius, bg);
        if (w->focused) {
            FDK_Color fc = (ws && ws->has_border_focus) ? ws->border_focus
                                                        : th->border_focus;
            fdk_stroke_rect(r, fc, 2);
        } else if (!active) {
            FDK_Color bc = (ws && ws->has_border) ? ws->border : th->border;
            fdk_stroke_rect(r, bc, 1);
        }
        if (th->font_body && w->toggle.label) {
            FDK_Size ts = fdk_measure_text(th->font_body, w->toggle.label);
            int tx = r.x + (r.w - ts.w) / 2;
            int ty = r.y + (r.h - font_line_h(th->font_body, th)) / 2;
            fdk_draw_text(th->font_body, w->toggle.label, tx, ty, fg);
        }
        fdk_pop_clip();
        break;
    }

    /* ── Radio button ── */
    case FDK_WIDGET_RADIO_BUTTON: {
        int dot_r = 9;   /* outer radius */
        int cx    = r.x + dot_r;
        int cy    = r.y + r.h / 2;
        bool selected = (w->radio.group && *w->radio.group == w->radio.index);

        /* Outer circle */
        FDK_Color ring = w->focused ? th->border_focus : th->border;
        fdk_fill_circle(cx, cy, dot_r, th->bg_input);
        fdk_stroke_rect((FDK_Rect){ cx - dot_r, cy - dot_r,
                                    dot_r*2, dot_r*2 }, ring, 1);
        /* Inner fill when selected */
        if (selected)
            fdk_fill_circle(cx, cy, dot_r - 4, th->accent);

        /* Label */
        if (th->font_body && w->radio.label) {
            int lh = font_line_h(th->font_body, th);
            int ty = r.y + (r.h - lh) / 2;
            fdk_draw_text(th->font_body, w->radio.label,
                          cx + dot_r + 6, ty,
                          w->enabled ? th->fg_primary : th->fg_disabled);
        }
        break;
    }

    /* ── Spinner ── */
    case FDK_WIDGET_SPINNER: {
        FDK_Color bg = w->focused ? th->bg_input_focus : th->bg_input;
        fdk_push_clip(r);   /* never bleed outside the widget rect */
        fdk_fill_rect_rounded(r, th->radius_sm, bg);
        fdk_stroke_rect(r, w->focused ? th->border_focus : th->border, 1);

        /* Button column is 26px wide on the right, split into up/down halves */
        int btn_w  = 26;
        int body_w = r.w - btn_w;

        /* Divider */
        fdk_fill_rect((FDK_Rect){ r.x + body_w, r.y + 1, 1, r.h - 2 },
                      th->border);

        /* Up-half and down-half click areas */
        int half_h = r.h / 2;
        int ax     = r.x + body_w + btn_w / 2;  /* horizontal centre of btn col */

        /* ▲ up arrow — three horizontal pixel rows forming a triangle */
        int uy = r.y + half_h / 2 - 1;   /* vertical centre of upper half */
        FDK_Color ac = th->fg_secondary;
        fdk_fill_rect((FDK_Rect){ ax,     uy - 2, 1, 1 }, ac);  /* tip */
        fdk_fill_rect((FDK_Rect){ ax - 1, uy - 1, 3, 1 }, ac);  /* mid */
        fdk_fill_rect((FDK_Rect){ ax - 2, uy,     5, 1 }, ac);  /* base */

        /* ▼ down arrow */
        int dy = r.y + half_h + half_h / 2 + 1;
        fdk_fill_rect((FDK_Rect){ ax - 2, dy - 2, 5, 1 }, ac);  /* base */
        fdk_fill_rect((FDK_Rect){ ax - 1, dy - 1, 3, 1 }, ac);  /* mid */
        fdk_fill_rect((FDK_Rect){ ax,     dy,     1, 1 }, ac);   /* tip */

        /* Horizontal divider between halves */
        fdk_fill_rect((FDK_Rect){ r.x + body_w, r.y + half_h, btn_w, 1 },
                      th->border);

        /* Value text */
        if (th->font_body) {
            char buf[64];
            snprintf(buf, sizeof buf, "%.*f",
                     w->spinner.decimals, w->spinner.value);
            int lh = font_line_h(th->font_body, th);
            int ty = r.y + (r.h - lh) / 2;
            fdk_push_clip((FDK_Rect){ r.x + 4, r.y + 1, body_w - 8, r.h - 2 });
            fdk_draw_text(th->font_body, buf, r.x + 8, ty, th->fg_primary);
            fdk_pop_clip();
        }
        fdk_pop_clip();
        break;
    }

    /* ── Badge ── */
    case FDK_WIDGET_BADGE: {
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        /* Priority: fdk_widget_set_style() override -> variant -> theme [badge] -> accent */
        FDK_Color bg = w->badge.custom_color ? w->badge.bg
                     : (ws && ws->has_bg)    ? ws->bg
                     : th->accent;
        FDK_Color fg = w->badge.custom_color ? w->badge.fg
                     : (ws && ws->has_fg)    ? ws->fg
                     : th->fg_on_accent;
        int radius = (ws && ws->has_radius) ? ws->radius : r.h / 2;
        fdk_fill_rect_rounded(r, radius, bg);
        if (th->font_label && w->badge.text) {
            FDK_Size ts = fdk_measure_text(th->font_label, w->badge.text);
            int tx = r.x + (r.w - ts.w) / 2;
            int ty = r.y + (r.h - font_line_h(th->font_label, th)) / 2;
            fdk_draw_text(th->font_label, w->badge.text, tx, ty, fg);
        }
        break;
    }

    /* ── Tabs ── */
    case FDK_WIDGET_TABS: {
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        int bar_h  = 36;
        FDK_Font *f = th->font_body;

        FDK_Color bar_bg  = (ws && ws->has_bg)     ? ws->bg     : th->bg_widget;
        FDK_Color bar_bdr = (ws && ws->has_border)  ? ws->border : th->border;
        fdk_fill_rect((FDK_Rect){ r.x, r.y, r.w, bar_h }, bar_bg);
        fdk_fill_rect((FDK_Rect){ r.x, r.y + bar_h - 1, r.w, 1 }, bar_bdr);

        /* Draw each tab label */
        int tab_x = r.x + 4;
        int tab_pad = 16;
        for (int i = 0; i < w->tabs.count; i++) {
            FDK_Size ts = {0, 0};
            if (f && w->tabs.labels[i])
                ts = fdk_measure_text(f, w->tabs.labels[i]);
            int tw = ts.w + tab_pad * 2;
            int lh = f ? font_line_h(f, th) : 16;
            int ty = r.y + (bar_h - lh) / 2;

            bool active = (i == w->tabs.active);
            FDK_Color fg = active ? th->accent : th->fg_secondary;

            /* Hover tint */
            if (!active && w->hovered) {
                /* Can't easily get per-tab hover here — skip for now */
            }
            if (f && w->tabs.labels[i])
                fdk_draw_text(f, w->tabs.labels[i], tab_x + tab_pad, ty, fg);

            /* Animated underline for active tab */
            if (active) {
                /* Snap indicator position on first paint; tween on change */
                if (w->tabs.indicator_w < 1.f) {
                    w->tabs.indicator_x = (float)tab_x;
                    w->tabs.indicator_w = (float)tw;
                }
                int ix = (int)(w->tabs.indicator_x + 0.5f);
                int iw = (int)(w->tabs.indicator_w + 0.5f);
                fdk_fill_rect((FDK_Rect){ ix, r.y + bar_h - 3, iw, 3 },
                              th->accent);
            }

            tab_x += tw;
        }

        /* Paint the active page inside the area below the bar */
        int page_h = r.h - bar_h;
        if (page_h > 0 && w->tabs.active >= 0 &&
            w->tabs.active < w->tabs.count) {
            FDK_Widget *pg = w->tabs.pages[w->tabs.active];
            if (pg && pg->visible) {
                /* Page rect is already set by layout_widget — just paint it */
                paint_widget(pg, ui);
            }
        }
        break;
    }

    /* ── Menu bar ── */
    case FDK_WIDGET_MENUBAR: {
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        FDK_Font *f = th->font_body;
        FDK_Color mb_bg  = (ws && ws->has_bg)    ? ws->bg    : th->bg_widget;
        FDK_Color mb_bdr = (ws && ws->has_border) ? ws->border : th->border;
        fdk_fill_rect(r, mb_bg);
        fdk_fill_rect((FDK_Rect){ r.x, r.y + r.h - 1, r.w, 1 }, mb_bdr);

        int mx = r.x + 8;
        int pad = 12;
        int lh = f ? font_line_h(f, th) : 16;
        for (int i = 0; i < w->menubar.count; i++) {
            FDK_Menu *m = &w->menubar.menus[i];
            if (!m->label) continue;
            FDK_Size ts = f ? fdk_measure_text(f, m->label) : (FDK_Size){60,16};
            int mw = ts.w + pad * 2;
            int ty = r.y + (r.h - lh) / 2;
            bool open = (w->menubar.open_menu == i);
            if (open)
                fdk_fill_rect_rounded(
                    (FDK_Rect){ mx, r.y + 2, mw, r.h - 4 },
                    th->radius_sm, th->bg_widget_hover);
            if (f)
                fdk_draw_text(f, m->label, mx + pad, ty,
                              open ? th->accent : th->fg_primary);
            mx += mw;
        }
        break;
    }

    /* ── Text area ── */
    case FDK_WIDGET_TEXTAREA: {
        const FDK_WidgetStyle *ws = resolve_style(w, th);
        int radius = (ws && ws->has_radius) ? ws->radius : th->radius_sm;
        FDK_Color bg = w->focused
            ? ((ws && ws->has_bg_hover) ? ws->bg_hover : th->bg_input_focus)
            : ((ws && ws->has_bg)       ? ws->bg       : th->bg_input);
        fdk_push_clip(r);
        fdk_fill_rect_rounded(r, radius, bg);
        FDK_Color border_col = w->focused
            ? ((ws && ws->has_border_focus) ? ws->border_focus : th->border_focus)
            : ((ws && ws->has_border)       ? ws->border       : th->border);
        fdk_stroke_rect(r, border_col, 1);
        FDK_Color fg_col = w->enabled
            ? ((ws && ws->has_fg) ? ws->fg : th->fg_primary)
            : th->fg_disabled;

        if (th->font_body && w->textarea.buf && w->textarea.len > 0) {
            int lh   = font_line_h(th->font_body, th);
            int pad  = 6;
            int tx   = r.x + pad;
            int ty   = r.y + pad - w->textarea.scroll_y;
            FDK_Rect clip = { r.x + 1, r.y + 1, r.w - 2, r.h - 2 };
            fdk_push_clip(clip);

            /* Render line by line */
            const char *s = w->textarea.buf;
            const char *end = s + w->textarea.len;
            while (s < end && ty < r.y + r.h) {
                const char *nl = memchr(s, '\n', (size_t)(end - s));
                int line_len = nl ? (int)(nl - s) : (int)(end - s);
                /* Render this line */
                if (ty + lh >= r.y) {
                    char line_buf[512];
                    int copy = line_len < 511 ? line_len : 511;
                    memcpy(line_buf, s, copy);
                    line_buf[copy] = '\0';
                    fdk_draw_text(th->font_body, line_buf, tx, ty, fg_col);
                }
                /* Draw cursor on the line that contains it */
                if (w->focused) {
                    int cursor_byte = w->textarea.cursor;
                    int line_start  = (int)(s - w->textarea.buf);
                    int line_end    = line_start + line_len;
                    if (cursor_byte >= line_start && cursor_byte <= line_end) {
                        char pre[512];
                        int pre_len = cursor_byte - line_start;
                        if (pre_len > 511) pre_len = 511;
                        memcpy(pre, s, pre_len);
                        pre[pre_len] = '\0';
                        FDK_Size ps = fdk_measure_text(th->font_body, pre);
                        int cx = tx + ps.w;
                        fdk_fill_rect((FDK_Rect){ cx, ty, 2, lh }, th->accent);
                    }
                }
                s = nl ? nl + 1 : end;
                ty += lh;
            }
            fdk_pop_clip();
        } else if (th->font_body) {
            /* Cursor at start when empty */
            if (w->focused) {
                int lh = font_line_h(th->font_body, th);
                fdk_fill_rect((FDK_Rect){ r.x + 6, r.y + 6, 2, lh }, th->accent);
            }
        }
        fdk_pop_clip();
        break;
    }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * EVENT DISPATCH
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool rect_contains(FDK_Rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* Walk tree depth-first, return topmost visible enabled widget at (x,y) */
static FDK_Widget *hit_test(FDK_Widget *w, int x, int y)
{
    if (!w->visible) return NULL;
    if (!rect_contains(w->rect, x, y)) return NULL;

    /* For tabs: check active page content first, then the bar itself */
    if (w->type == FDK_WIDGET_TABS) {
        int bar_h = 36;
        /* If click is in page area, forward to active page */
        if (y > w->rect.y + bar_h && w->tabs.active >= 0 &&
            w->tabs.active < w->tabs.count) {
            FDK_Widget *pg = w->tabs.pages[w->tabs.active];
            if (pg && pg->visible) {
                FDK_Widget *hit = hit_test(pg, x, y);
                if (hit) return hit;
            }
        }
        /* Click in bar area returns the tabs widget itself */
        return w;
    }

    /* Check children first (topmost = last child in list) */
    for (int i = w->child_count - 1; i >= 0; i--) {
        FDK_Widget *hit = hit_test(w->children[i], x, y);
        if (hit) return hit;
    }

    /* Scroll view: also check content */
    if (w->type == FDK_WIDGET_SCROLL_VIEW && w->scroll.content) {
        FDK_Widget *hit = hit_test(w->scroll.content, x, y);
        if (hit) return hit;
    }

    /* Return self if interactive */
    if (w->enabled &&
        (w->type == FDK_WIDGET_BUTTON        ||
         w->type == FDK_WIDGET_TEXT_INPUT     ||
         w->type == FDK_WIDGET_CHECKBOX       ||
         w->type == FDK_WIDGET_CUSTOM         ||
         w->type == FDK_WIDGET_SLIDER         ||
         w->type == FDK_WIDGET_DROPDOWN       ||
         w->type == FDK_WIDGET_SCROLL_VIEW    ||
         w->type == FDK_WIDGET_TOGGLE_BUTTON  ||
         w->type == FDK_WIDGET_RADIO_BUTTON   ||
         w->type == FDK_WIDGET_SPINNER        ||
         w->type == FDK_WIDGET_TEXTAREA       ||
         w->type == FDK_WIDGET_MENUBAR))
        return w;

    return NULL;
}

static void sel_clear(FDK_Widget *w); /* forward decl */

static void set_focus(FDK_UI *ui, FDK_Widget *w)
{
    if (ui->focused == w) return;
    if (ui->focused) {
        ui->focused->focused = false;
        /* Clear selection when focus leaves a text input */
        if (ui->focused->type == FDK_WIDGET_TEXT_INPUT)
            sel_clear(ui->focused);
    }
    ui->focused = w;
    if (w) {
        w->focused = true;
        /* Reset blink on focus gain */
        ui->blink_on   = true;
        ui->blink_last = fdk_time_ms();
    }
    ui->dirty = true;
}

/* Handle a character being typed into a text input */
static void input_insert_char(FDK_Widget *w, uint32_t cp, FDK_UI *ui)
{
    if (cp < 32 || cp == 127) return; /* control chars handled elsewhere */
    if (w->input.buf_len >= w->input.max_len) return;

    /* Encode codepoint to UTF-8 */
    char enc[5] = {0};
    int  enc_len;
    if (cp < 0x80) {
        enc[0] = (char)cp; enc_len = 1;
    } else if (cp < 0x800) {
        enc[0] = (char)(0xC0 | (cp >> 6));
        enc[1] = (char)(0x80 | (cp & 0x3F));
        enc_len = 2;
    } else {
        enc[0] = (char)(0xE0 | (cp >> 12));
        enc[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        enc[2] = (char)(0x80 | (cp & 0x3F));
        enc_len = 3;
    }

    if (w->input.buf_len + enc_len > w->input.max_len) return;

    int c = w->input.cursor;
    memmove(w->input.buf + c + enc_len,
            w->input.buf + c,
            w->input.buf_len - c + 1);
    memcpy(w->input.buf + c, enc, enc_len);
    w->input.buf_len  += enc_len;
    w->input.cursor   += enc_len;

    if (w->input.cb)
        w->input.cb(w, w->input.buf, w->input.cb_ud);
    ui->dirty = true;
}

/* Move cursor left/right by one UTF-8 character */
static void input_move_cursor(FDK_Widget *w, int dir)
{
    int c = w->input.cursor;
    if (dir < 0) {
        if (c == 0) return;
        c--;
        while (c > 0 && (w->input.buf[c] & 0xC0) == 0x80) c--;
    } else {
        if (c >= w->input.buf_len) return;
        c++;
        while (c < w->input.buf_len && (w->input.buf[c] & 0xC0) == 0x80) c++;
    }
    w->input.cursor = c;
}

static void input_backspace(FDK_Widget *w, FDK_UI *ui)
{
    if (w->input.cursor == 0) return;
    int c   = w->input.cursor;
    int old = c;
    c--;
    while (c > 0 && (w->input.buf[c] & 0xC0) == 0x80) c--;
    int del = old - c;
    memmove(w->input.buf + c,
            w->input.buf + old,
            w->input.buf_len - old + 1);
    w->input.buf_len -= del;
    w->input.cursor   = c;
    if (w->input.cb)
        w->input.cb(w, w->input.buf, w->input.cb_ud);
    ui->dirty = true;
}

/* Advance focus to next/prev in tab order */
static void tab_focus(FDK_UI *ui, bool forward)
{
    if (ui->tab_count == 0) return;
    int cur = -1;
    for (int i = 0; i < ui->tab_count; i++)
        if (ui->tab_order[i] == ui->focused) { cur = i; break; }

    int next = forward
                   ? (cur + 1) % ui->tab_count
                   : (cur <= 0 ? ui->tab_count - 1 : cur - 1);
    set_focus(ui, ui->tab_order[next]);
}

/* ─── Selection helpers ──────────────────────────────────────────────────── */
static void sel_clear(FDK_Widget *w)
{
    w->input.has_sel   = false;
    w->input.sel_start = 0;
    w->input.sel_end   = 0;
}

/* Start or extend selection anchor at cursor */
static void sel_begin(FDK_Widget *w)
{
    if (!w->input.has_sel) {
        w->input.sel_start = w->input.cursor;
        w->input.sel_end   = w->input.cursor;
        w->input.has_sel   = true;
    }
}

/* Extend selection to current cursor position */
static void sel_extend(FDK_Widget *w)
{
    w->input.sel_end = w->input.cursor;
}

/* Delete selected text, place cursor at selection start */
static void sel_delete(FDK_Widget *w, FDK_UI *ui)
{
    if (!w->input.has_sel) return;
    int s0 = w->input.sel_start < w->input.sel_end
                 ? w->input.sel_start : w->input.sel_end;
    int s1 = w->input.sel_start < w->input.sel_end
                 ? w->input.sel_end : w->input.sel_start;
    int del = s1 - s0;
    memmove(w->input.buf + s0, w->input.buf + s1,
            w->input.buf_len - s1 + 1);
    w->input.buf_len -= del;
    w->input.cursor   = s0;
    sel_clear(w);
    if (w->input.cb) w->input.cb(w, w->input.buf, w->input.cb_ud);
    ui->dirty = true;
}

/* Get pixel X offset of a byte position in the input field */
static int input_x_of_cursor(FDK_Widget *w, int byte_pos,
                               const FDK_Theme *th, int pad)
{
    if (!th->font_body) return pad;
    char tmp[512];
    int cp = byte_pos < (int)sizeof(tmp)-1 ? byte_pos : (int)sizeof(tmp)-1;
    memcpy(tmp, w->input.buf, cp);
    tmp[cp] = '\0';
    FDK_Size s = fdk_measure_text(th->font_body, tmp);
    return w->rect.x + pad + s.w - w->input.scroll_x;
}

/* Find nearest byte offset for a given pixel X in the input field */
static int input_cursor_at_x(FDK_Widget *w, int px,
                               const FDK_Theme *th, int pad)
{
    if (!th->font_body || !w->input.buf) return 0;
    int text_x = w->rect.x + pad - w->input.scroll_x;
    /* Walk UTF-8 characters until we overshoot px */
    const char *p   = w->input.buf;
    const char *end = w->input.buf + w->input.buf_len;
    int best_pos = 0;
    while (p < end) {
        const char *next = p;
        /* advance one UTF-8 char */
        unsigned char ch = (unsigned char)*next++;
        if      (ch >= 0xF0) next += 3;
        else if (ch >= 0xE0) next += 2;
        else if (ch >= 0xC0) next += 1;
        int byte_pos = (int)(next - w->input.buf);
        char tmp[512];
        int cp = byte_pos < (int)sizeof(tmp)-1 ? byte_pos : (int)sizeof(tmp)-1;
        memcpy(tmp, w->input.buf, cp); tmp[cp] = '\0';
        FDK_Size s = fdk_measure_text(th->font_body, tmp);
        int char_x = text_x + s.w;
        if (char_x > px) {
            /* Pick whichever edge is closer */
            int prev_x = input_x_of_cursor(w, best_pos, th, pad);
            return (px - prev_x < char_x - px) ? best_pos : byte_pos;
        }
        best_pos = byte_pos;
        p = next;
    }
    return w->input.buf_len;
}

/* Returns true if the UI consumed the event and the app need not see it */
/* Recursively find the first MENUBAR widget with an open popup */
static FDK_Widget *find_open_menubar(FDK_Widget *w)
{
    if (!w || !w->visible) return NULL;
    if (w->type == FDK_WIDGET_MENUBAR && w->menubar.open_menu >= 0) return w;
    for (int i = 0; i < w->child_count; i++) {
        FDK_Widget *r = find_open_menubar(w->children[i]);
        if (r) return r;
    }
    if (w->type == FDK_WIDGET_TABS)
        for (int i = 0; i < w->tabs.count; i++) {
            FDK_Widget *r = find_open_menubar(w->tabs.pages[i]);
            if (r) return r;
        }
    return NULL;
}

static bool dispatch_event(FDK_UI *ui, FDK_Widget *root, const FDK_Event *ev)
{
    switch (ev->type) {

    case FDK_EVENT_MOUSE_MOVE: {
        FDK_Widget *hit = hit_test(root, ev->motion.x, ev->motion.y);
        ui->mouse_x = ev->motion.x;
        ui->mouse_y = ev->motion.y;
        if (hit != ui->hover) {
            if (ui->hover) {
                ui->hover->hovered        = false;
                ui->hover->hover_start_ms = 0;
            }
            ui->hover = hit;
            if (hit) {
                hit->hovered        = true;
                hit->hover_start_ms = fdk_time_ms();
            }
            ui->dirty = true;
        }
        /* Slider drag */
        if (ui->mouse_down &&
            ui->mouse_down_widget &&
            ui->mouse_down_widget->type == FDK_WIDGET_SLIDER) {
            FDK_Widget *sl   = ui->mouse_down_widget;
            int thumb_r  = 8;
            int track_x0 = sl->rect.x + thumb_r;
            int track_w  = sl->rect.w - thumb_r * 2;
            float t = track_w > 0
                ? (float)(ev->motion.x - track_x0) / track_w
                : 0.f;
            t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
            float v = sl->slider.min + t * (sl->slider.max - sl->slider.min);
            if (sl->slider.step > 0.f)
                v = sl->slider.min +
                    ((int)((v - sl->slider.min) /
                           sl->slider.step + 0.5f)) *
                    sl->slider.step;
            if (v != sl->slider.value) {
                sl->slider.value = v;
                if (sl->slider.cb)
                    sl->slider.cb(sl, v, sl->slider.cb_ud);
                ui->dirty = true;
            }
        }

        /* Drag selection — only when left button is held down */
        if (ui->mouse_down &&
            ui->mouse_down_widget &&
            ui->mouse_down_widget->type == FDK_WIDGET_TEXT_INPUT) {
            FDK_Widget *f = ui->mouse_down_widget;
            int pos = input_cursor_at_x(f, ev->motion.x, &ui->theme, 8);
            if (pos != f->input.cursor) {
                f->input.cursor  = pos;
                f->input.sel_end = pos;
                f->input.has_sel = (f->input.sel_start != f->input.sel_end);
                ui->dirty = true;
            }
        }
        /* Track hover over open dropdown popup items */
        bool in_popup = false;
        for (int _i = 0; _i < ui->tab_count; _i++) {
            FDK_Widget *dd = ui->tab_order[_i];
            if (dd->type != FDK_WIDGET_DROPDOWN || !dd->dropdown.open) continue;
            int item_h  = 30;
            int popup_y = dd->rect.y + dd->rect.h;
            FDK_Rect popup = { dd->rect.x, popup_y,
                               dd->rect.w,
                               dd->dropdown.item_count * item_h + 2 };
            if (rect_contains(popup, ev->motion.x, ev->motion.y)) {
                in_popup = true;
                fdk_set_cursor(FDK_CURSOR_POINTER);
                ui->dirty = true;
            }
        }
        if (in_popup) return false;

        /* Update cursor shape based on what's under the pointer */
        if (!hit)
            fdk_set_cursor(FDK_CURSOR_DEFAULT);
        else if (hit->type == FDK_WIDGET_TEXT_INPUT)
            fdk_set_cursor(FDK_CURSOR_TEXT);
        else if (hit->type == FDK_WIDGET_BUTTON   ||
                 hit->type == FDK_WIDGET_CHECKBOX  ||
                 hit->type == FDK_WIDGET_DROPDOWN  ||
                 hit->type == FDK_WIDGET_SLIDER)
            fdk_set_cursor(FDK_CURSOR_POINTER);
        else
            fdk_set_cursor(FDK_CURSOR_DEFAULT);
        return false;
    }

    case FDK_EVENT_MOUSE_DOWN: {
        /* ── Right-click: show widget context menu ── */
        if (ev->mouse.button == FDK_BUTTON_RIGHT) {
            FDK_Widget *hit = hit_test(root, ev->mouse.x, ev->mouse.y);
            if (hit && hit->ctx_menu) {
                fdk_context_menu_show(ui, hit->ctx_menu,
                                      ev->mouse.x, ev->mouse.y);
                return true;
            }
            /* Close any open context menu on right-click elsewhere */
            if (ui->ctx_menu) { fdk_context_menu_hide(ui); return true; }
            return false;
        }
        if (ev->mouse.button != FDK_BUTTON_LEFT) return false;

        /* ── Dismiss or activate open context menu ── */
        if (ui->ctx_menu) {
            FDK_ContextMenu *cm  = ui->ctx_menu;
            const int item_h     = 28;
            const int sep_h      = 9;
            const int popup_w    = 200;
            int total_h          = 4;
            for (int i = 0; i < cm->count; i++)
                total_h += cm->items[i].label ? item_h : sep_h;
            FDK_Size wsz = fdk_window_get_size(ui->win);
            int px = ui->ctx_x, py = ui->ctx_y;
            if (px + popup_w > wsz.w) px = wsz.w - popup_w - 4;
            if (py + total_h > wsz.h) py = wsz.h - total_h - 4;
            FDK_Rect popup = { px, py, popup_w, total_h };
            if (rect_contains(popup, ev->mouse.x, ev->mouse.y)) {
                int iy = py + 4;
                for (int i = 0; i < cm->count; i++) {
                    if (!cm->items[i].label) { iy += sep_h; continue; }
                    FDK_Rect ir = { px+1, iy, popup_w-2, item_h };
                    if (rect_contains(ir, ev->mouse.x, ev->mouse.y)) {
                        ui->ctx_menu = NULL;
                        if (!cm->items[i].disabled && cm->items[i].cb)
                            cm->items[i].cb(cm->items[i].ud);
                        ui->dirty = true;
                        return true;
                    }
                    iy += item_h;
                }
            }
            /* Click outside — close */
            ui->ctx_menu = NULL;
            ui->dirty    = true;
        }

        /* ── Check open menubar popup FIRST ── */
        {
            FDK_Widget *mb = find_open_menubar(root);
            if (mb) {
                int mi = mb->menubar.open_menu;
                FDK_Menu *m = &mb->menubar.menus[mi];
                FDK_Font *f = ui->theme.font_body;
                const int item_h = 28, sep_h = 9, pad = 12, popup_w = 200;

                /* Recalculate popup x — same logic as paint_overlays */
                int mx2 = mb->rect.x + 8;
                for (int i = 0; i < mi; i++) {
                    if (!mb->menubar.menus[i].label) continue;
                    FDK_Size ts = f ? fdk_measure_text(f, mb->menubar.menus[i].label)
                                    : (FDK_Size){60, 16};
                    mx2 += ts.w + pad * 2;
                }
                FDK_Size wsz = fdk_window_get_size(ui->win);
                if (mx2 + popup_w > wsz.w) mx2 = wsz.w - popup_w - 4;

                int iy = mb->rect.y + mb->rect.h + 4;
                for (int i = 0; i < m->item_count; i++) {
                    FDK_MenuItem *it = &m->items[i];
                    if (it->separator) { iy += sep_h; continue; }
                    FDK_Rect ir = { mx2 + 1, iy, popup_w - 2, item_h };
                    if (rect_contains(ir, ev->mouse.x, ev->mouse.y)) {
                        mb->menubar.open_menu = -1;
                        if (!it->disabled && it->cb) it->cb(it->ud);
                        ui->dirty = true;
                        return true;
                    }
                    iy += item_h;
                }
                /* Click outside popup — close it and fall through */
                mb->menubar.open_menu = -1;
                ui->dirty = true;
            }
        }

        /* ── Check open dropdown popups FIRST, before hit_test ──
         * Popup items are outside their widget's rect so hit_test
         * can't find them — we must intercept here. */
        for (int _i = 0; _i < ui->tab_count; _i++) {
            FDK_Widget *dd = ui->tab_order[_i];
            if (dd->type != FDK_WIDGET_DROPDOWN || !dd->dropdown.open) continue;
            int item_h  = 30;
            int popup_y = dd->rect.y + dd->rect.h; /* match paint_overlays */
            /* Check each item rect — offset by 1 for border, matching paint */
            for (int j = 0; j < dd->dropdown.item_count; j++) {
                FDK_Rect ir = { dd->rect.x + 1, popup_y + 1 + j * item_h,
                                dd->rect.w - 2, item_h };
                if (rect_contains(ir, ev->mouse.x, ev->mouse.y)) {
                    /* Select item, close popup */
                    dd->dropdown.selected     = j;
                    dd->dropdown.open         = false;
                    dd->dropdown.hovered_item = -1;
                    if (dd->dropdown.cb)
                        dd->dropdown.cb(dd, j, dd->dropdown.items[j],
                                        dd->dropdown.cb_ud);
                    ui->dirty = true;
                    return true; /* swallow event — don't let it reach anything else */
                }
            }
            /* Click is outside this open popup — close it */
            FDK_Rect popup_full = { dd->rect.x, popup_y,
                                    dd->rect.w,
                                    dd->dropdown.item_count * item_h + 2 };
            if (!rect_contains(popup_full, ev->mouse.x, ev->mouse.y) &&
                !rect_contains(dd->rect, ev->mouse.x, ev->mouse.y)) {
                dd->dropdown.open         = false;
                dd->dropdown.hovered_item = -1;
                ui->dirty = true;
            }
        }

        FDK_Widget *hit = hit_test(root, ev->mouse.x, ev->mouse.y);
        set_focus(ui, hit);
        ui->mouse_down        = true;
        ui->mouse_down_widget = hit;

        if (hit) {
            if (hit->type == FDK_WIDGET_BUTTON)
                hit->button.pressed = true;
            if (hit->type == FDK_WIDGET_TEXT_INPUT) {
                int pos = input_cursor_at_x(hit, ev->mouse.x,
                                            &ui->theme, 8);
                hit->input.cursor    = pos;
                hit->input.sel_start = pos;
                hit->input.sel_end   = pos;
                sel_clear(hit);
                ui->blink_on   = true;
                ui->blink_last = fdk_time_ms();
            }
            if (hit->type == FDK_WIDGET_SLIDER) {
                hit->slider.dragging = true;
                /* Set value from click position */
                int thumb_r  = 8;
                int track_x0 = hit->rect.x + thumb_r;
                int track_w  = hit->rect.w - thumb_r * 2;
                float t = track_w > 0
                    ? (float)(ev->mouse.x - track_x0) / track_w
                    : 0.f;
                t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
                float v = hit->slider.min +
                          t * (hit->slider.max - hit->slider.min);
                if (hit->slider.step > 0.f)
                    v = hit->slider.min +
                        ((int)((v - hit->slider.min) /
                               hit->slider.step + 0.5f)) *
                        hit->slider.step;
                hit->slider.value = v;
                if (hit->slider.cb)
                    hit->slider.cb(hit, v, hit->slider.cb_ud);
            }
            if (hit->type == FDK_WIDGET_DROPDOWN) {
                /* First check if click is inside an open popup */
                bool clicked_item = false;
                for (int i = 0; i < ui->tab_count; i++) {
                    FDK_Widget *dd = ui->tab_order[i];
                    if (dd->type != FDK_WIDGET_DROPDOWN || !dd->dropdown.open) continue;
                    int item_h  = 30;
                    int popup_y = dd->rect.y + dd->rect.h;
                    for (int j = 0; j < dd->dropdown.item_count; j++) {
                        FDK_Rect ir = { dd->rect.x, popup_y + 1 + j * item_h,
                                        dd->rect.w, item_h };
                        if (rect_contains(ir, ev->mouse.x, ev->mouse.y)) {
                            dd->dropdown.selected = j;
                            dd->dropdown.open     = false;
                            if (dd->dropdown.cb)
                                dd->dropdown.cb(dd, j,
                                                dd->dropdown.items[j],
                                                dd->dropdown.cb_ud);
                            clicked_item = true;
                            break;
                        }
                    }
                    if (clicked_item) break;
                }
                if (!clicked_item) {
                    /* Toggle open/close — close others first */
                    for (int i = 0; i < ui->tab_count; i++)
                        if (ui->tab_order[i] != hit &&
                            ui->tab_order[i]->type == FDK_WIDGET_DROPDOWN)
                            ui->tab_order[i]->dropdown.open = false;
                    hit->dropdown.open = !hit->dropdown.open;
                }
            }
            if (hit->type == FDK_WIDGET_TOGGLE_BUTTON) {
                hit->toggle.active = !hit->toggle.active;
                if (hit->toggle.cb)
                    hit->toggle.cb(hit, hit->toggle.cb_ud);
            }
            if (hit->type == FDK_WIDGET_RADIO_BUTTON) {
                if (hit->radio.group)
                    *hit->radio.group = hit->radio.index;
                if (hit->radio.cb)
                    hit->radio.cb(hit, hit->radio.cb_ud);
            }
            if (hit->type == FDK_WIDGET_SPINNER) {
                int btn_w  = 26;  /* must match paint */
                int bx     = hit->rect.x + hit->rect.w - btn_w;
                int half_h = hit->rect.h / 2;
                int split_y = hit->rect.y + half_h;
                if (ev->mouse.x >= bx) {
                    double v = hit->spinner.value;
                    if (ev->mouse.y < split_y)
                        v += hit->spinner.step;   /* upper half = up */
                    else
                        v -= hit->spinner.step;   /* lower half = down */
                    if (v < hit->spinner.min) v = hit->spinner.min;
                    if (v > hit->spinner.max) v = hit->spinner.max;
                    hit->spinner.value = v;
                    if (hit->spinner.cb)
                        hit->spinner.cb(hit, (float)v, hit->spinner.cb_ud);
                }
            }
            /* Tab bar — find which tab was clicked */
            if (hit->type == FDK_WIDGET_TABS) {
                FDK_Font *f  = ui->theme.font_body;
                int tab_x    = hit->rect.x + 4;
                int bar_h    = 36;
                int tab_pad  = 16;
                if (ev->mouse.y <= hit->rect.y + bar_h) {
                    for (int i = 0; i < hit->tabs.count; i++) {
                        FDK_Size ts = {0, 0};
                        if (f && hit->tabs.labels[i])
                            ts = fdk_measure_text(f, hit->tabs.labels[i]);
                        int tw = ts.w + tab_pad * 2;
                        if (ev->mouse.x >= tab_x &&
                            ev->mouse.x <  tab_x + tw) {
                            if (i != hit->tabs.active) {
                                fdk_tabs_set_active(hit, i);
                                if (hit->tabs.cb)
                                    hit->tabs.cb(hit, i, hit->tabs.cb_ud);
                            }
                            break;
                        }
                        tab_x += tw;
                    }
                }
            }
            /* Menu bar — find which menu title was clicked */
            if (hit->type == FDK_WIDGET_MENUBAR) {
                FDK_Font *f = ui->theme.font_body;
                int mx      = hit->rect.x + 8;
                int pad     = 12;
                int clicked = -1;
                for (int i = 0; i < hit->menubar.count; i++) {
                    if (!hit->menubar.menus[i].label) continue;
                    FDK_Size ts = f
                        ? fdk_measure_text(f, hit->menubar.menus[i].label)
                        : (FDK_Size){60, 16};
                    int mw = ts.w + pad * 2;
                    if (ev->mouse.x >= mx && ev->mouse.x < mx + mw) {
                        clicked = i; break;
                    }
                    mx += mw;
                }
                if (clicked >= 0)
                    hit->menubar.open_menu =
                        (hit->menubar.open_menu == clicked) ? -1 : clicked;
                else
                    hit->menubar.open_menu = -1;
            }
            ui->dirty = true;
        }
        return hit != NULL;
    }

    case FDK_EVENT_MOUSE_UP: {
        if (ev->mouse.button != FDK_BUTTON_LEFT) return false;
        ui->mouse_down        = false;
        if (ui->mouse_down_widget &&
            ui->mouse_down_widget->type == FDK_WIDGET_SLIDER)
            ui->mouse_down_widget->slider.dragging = false;
        ui->mouse_down_widget = NULL;
        FDK_Widget *hit = hit_test(root, ev->mouse.x, ev->mouse.y);

        /* Fire click if mouse-up is on the same widget as mouse-down */
        if (hit && hit->button.pressed && hit->type == FDK_WIDGET_BUTTON) {
            hit->button.pressed = false;
            if (hit->button.cb)
                hit->button.cb(hit, hit->button.cb_ud);
            ui->dirty = true;
            return true;
        }
        if (hit && hit->type == FDK_WIDGET_CHECKBOX) {
            hit->checkbox.checked = !hit->checkbox.checked;
            if (hit->checkbox.cb)
                hit->checkbox.cb(hit, hit->checkbox.cb_ud);
            ui->dirty = true;
            return true;
        }
        /* Dropdown selection is handled fully in MOUSE_DOWN above */

        /* Clear any stuck pressed state */
        for (int i = 0; i < ui->tab_count; i++)
            if (ui->tab_order[i]->type == FDK_WIDGET_BUTTON)
                ui->tab_order[i]->button.pressed = false;
        ui->dirty = true;
        return false;
    }

    case FDK_EVENT_MOUSE_SCROLL: {
        FDK_Widget *hit = hit_test(root, ui->hover
                            ? (ui->hover->rect.x + ui->hover->rect.w/2)
                            : 0,
                            ui->hover
                            ? (ui->hover->rect.y + ui->hover->rect.h/2)
                            : 0);
        if (!hit) return false;
        if (hit->type == FDK_WIDGET_SCROLL_VIEW) {
            hit->scroll.scroll_y += (int)(ev->scroll.dy * -20);
            int max_y = hit->scroll.content_h - hit->rect.h;
            if (hit->scroll.scroll_y < 0) hit->scroll.scroll_y = 0;
            if (hit->scroll.scroll_y > max_y && max_y > 0)
                hit->scroll.scroll_y = max_y;
            ui->dirty = true;
            return true;
        }
        if (hit->type == FDK_WIDGET_SLIDER) {
            float step = hit->slider.step > 0.f
                             ? hit->slider.step
                             : (hit->slider.max - hit->slider.min) / 20.f;
            float v = hit->slider.value + ev->scroll.dy * step;
            if (v < hit->slider.min) v = hit->slider.min;
            if (v > hit->slider.max) v = hit->slider.max;
            if (v != hit->slider.value) {
                hit->slider.value = v;
                if (hit->slider.cb)
                    hit->slider.cb(hit, v, hit->slider.cb_ud);
                ui->dirty = true;
            }
            return true;
        }
        return false;
    } /* FDK_EVENT_MOUSE_SCROLL */

    case FDK_EVENT_KEY_DOWN: {
        /* Escape: close any open context menu or menubar popup */
        if (ev->key.key == FDK_KEY_ESCAPE) {
            bool consumed = false;
            if (ui->ctx_menu) { ui->ctx_menu = NULL; ui->dirty = true; consumed = true; }
            FDK_Widget *mb = find_open_menubar(root);
            if (mb) { mb->menubar.open_menu = -1; ui->dirty = true; consumed = true; }
            if (consumed) return true;
        }

        /* Alt+key: menubar mnemonic activation.
         * Menu labels use underscore prefix: "_File" → mnemonic 'F' */
        if ((ev->key.mods & FDK_MOD_ALT) && ev->key.key >= 'a' &&
            ev->key.key <= 'z') {
            /* Find a menubar anywhere in the tree */
            for (int ti = 0; ti < ui->tab_count; ti++) {
                /* menubar not in tab order — scan root children */
                (void)ti; break;
            }
            /* Recursive walk for menubar */
            FDK_Widget *mb = NULL;
            for (int ci = 0; ci < root->child_count && !mb; ci++)
                if (root->children[ci]->type == FDK_WIDGET_MENUBAR)
                    mb = root->children[ci];
            if (mb) {
                char key_lc = (char)ev->key.key;
                for (int i = 0; i < mb->menubar.count; i++) {
                    const char *lbl = mb->menubar.menus[i].label;
                    if (!lbl) continue;
                    /* Find underscore-prefixed char */
                    const char *us = strchr(lbl, '_');
                    if (!us || !us[1]) continue;
                    char mnemonic = (char)tolower((unsigned char)us[1]);
                    if (mnemonic == key_lc) {
                        mb->menubar.open_menu =
                            (mb->menubar.open_menu == i) ? -1 : i;
                        ui->dirty = true;
                        return true;
                    }
                }
            }
        }

        /* Tab / Shift-Tab focus cycling */
        if (ev->key.key == FDK_KEY_TAB) {
            tab_focus(ui, !(ev->key.mods & FDK_MOD_SHIFT));
            return true;
        }

        /* Route keyboard to focused widget */
        FDK_Widget *f = ui->focused;
        if (!f) return false;

        if (f->type == FDK_WIDGET_BUTTON) {
            if (ev->key.key == FDK_KEY_RETURN || ev->key.key == FDK_KEY_SPACE) {
                f->button.pressed = true;
                ui->dirty = true;
                return true;
            }
        }

        if (f->type == FDK_WIDGET_TEXT_INPUT) {
            bool ctrl  = (ev->key.mods & FDK_MOD_CTRL)  != 0;
            bool shift = (ev->key.mods & FDK_MOD_SHIFT) != 0;

            /* ── Ctrl shortcuts ── */
            if (ctrl) {
                FDK_Key k = ev->key.key;
                if (k == (FDK_Key)'c') { /* Copy */
                    if (f->input.has_sel) {
                        int s0 = f->input.sel_start < f->input.sel_end
                                     ? f->input.sel_start : f->input.sel_end;
                        int s1 = f->input.sel_start < f->input.sel_end
                                     ? f->input.sel_end : f->input.sel_start;
                        char tmp[512];
                        int len = s1 - s0;
                        if (len > (int)sizeof(tmp)-1) len = (int)sizeof(tmp)-1;
                        memcpy(tmp, f->input.buf + s0, len);
                        tmp[len] = '\0';
                        fdk_clipboard_set(tmp);
                    } else if (f->input.buf_len > 0) {
                        fdk_clipboard_set(f->input.buf);
                    }
                    return true;
                } else if (k == (FDK_Key)'x') { /* Cut */
                    if (f->input.has_sel) {
                        int s0 = f->input.sel_start < f->input.sel_end
                                     ? f->input.sel_start : f->input.sel_end;
                        int s1 = f->input.sel_start < f->input.sel_end
                                     ? f->input.sel_end : f->input.sel_start;
                        char tmp[512];
                        int len = s1 - s0;
                        if (len > (int)sizeof(tmp)-1) len = (int)sizeof(tmp)-1;
                        memcpy(tmp, f->input.buf + s0, len);
                        tmp[len] = '\0';
                        fdk_clipboard_set(tmp);
                        sel_delete(f, ui);
                    }
                    return true;
                } else if (k == (FDK_Key)'v') { /* Paste */
                    if (f->input.has_sel) sel_delete(f, ui);
                    {
                        char *text = fdk_clipboard_get();
                        if (text) {
                            for (const char *p = text; *p; ) {
                                unsigned char ch = (unsigned char)*p;
                                uint32_t cp; int extra;
                                if      (ch < 0x80) { cp=ch;       extra=0; }
                                else if (ch < 0xE0) { cp=ch&0x1F;  extra=1; }
                                else if (ch < 0xF0) { cp=ch&0x0F;  extra=2; }
                                else                { cp=ch&0x07;  extra=3; }
                                p++;
                                for (int i=0; i<extra && *p; i++,p++)
                                    cp=(cp<<6)|((unsigned char)*p&0x3F);
                                if (cp >= 32 && cp != 127)
                                    input_insert_char(f, cp, ui);
                            }
                            free(text);
                        }
                    }
                    return true;
                } else if (k == (FDK_Key)'a') { /* Select all */
                    f->input.sel_start = 0;
                    f->input.sel_end   = f->input.buf_len;
                    f->input.has_sel   = f->input.buf_len > 0;
                    f->input.cursor    = f->input.buf_len;
                    ui->dirty = true;
                    return true;
                }
            }

            /* ── Navigation + editing ── */
            switch (ev->key.key) {
            case FDK_KEY_BACKSPACE:
                if (f->input.has_sel) sel_delete(f, ui);
                else input_backspace(f, ui);
                return true;
            case FDK_KEY_DELETE:
                if (f->input.has_sel) { sel_delete(f, ui); }
                else if (f->input.cursor < f->input.buf_len) {
                    int old_c = f->input.cursor;
                    input_move_cursor(f, 1);
                    int del = f->input.cursor - old_c;
                    memmove(f->input.buf + old_c,
                            f->input.buf + f->input.cursor,
                            f->input.buf_len - f->input.cursor + 1);
                    f->input.buf_len -= del;
                    f->input.cursor   = old_c;
                    if (f->input.cb) f->input.cb(f, f->input.buf, f->input.cb_ud);
                    ui->dirty = true;
                }
                return true;
            case FDK_KEY_LEFT:
                if (shift) { sel_begin(f); input_move_cursor(f,-1); sel_extend(f); }
                else       { sel_clear(f); input_move_cursor(f,-1); }
                ui->dirty = true; ui->blink_on = true; ui->blink_last = fdk_time_ms();
                return true;
            case FDK_KEY_RIGHT:
                if (shift) { sel_begin(f); input_move_cursor(f, 1); sel_extend(f); }
                else       { sel_clear(f); input_move_cursor(f, 1); }
                ui->dirty = true; ui->blink_on = true; ui->blink_last = fdk_time_ms();
                return true;
            case FDK_KEY_HOME:
                if (shift) { sel_begin(f); f->input.cursor=0; sel_extend(f); }
                else       { sel_clear(f); f->input.cursor=0; }
                ui->dirty = true;
                return true;
            case FDK_KEY_END:
                if (shift) { sel_begin(f); f->input.cursor=f->input.buf_len; sel_extend(f); }
                else       { sel_clear(f); f->input.cursor=f->input.buf_len; }
                ui->dirty = true;
                return true;
            default:
                if (ev->key.codepoint >= 32 && ev->key.codepoint != 127) {
                    if (f->input.has_sel) sel_delete(f, ui);
                    input_insert_char(f, ev->key.codepoint, ui);
                    /* Reset blink so cursor is immediately visible after typing */
                    ui->blink_on   = true;
                    ui->blink_last = fdk_time_ms();
                }
                return true;
            }
        }

        if (f->type == FDK_WIDGET_CHECKBOX) {
            if (ev->key.key == FDK_KEY_SPACE || ev->key.key == FDK_KEY_RETURN) {
                f->checkbox.checked = !f->checkbox.checked;
                if (f->checkbox.cb)
                    f->checkbox.cb(f, f->checkbox.cb_ud);
                ui->dirty = true;
                return true;
            }
        }
        if (f->type == FDK_WIDGET_SLIDER) {
            float step = f->slider.step > 0.f
                             ? f->slider.step
                             : (f->slider.max - f->slider.min) / 20.f;
            float v = f->slider.value;
            if (ev->key.key == FDK_KEY_LEFT  || ev->key.key == FDK_KEY_DOWN)
                v -= step;
            else if (ev->key.key == FDK_KEY_RIGHT || ev->key.key == FDK_KEY_UP)
                v += step;
            else if (ev->key.key == FDK_KEY_HOME) v = f->slider.min;
            else if (ev->key.key == FDK_KEY_END)  v = f->slider.max;
            else return false;
            if (v < f->slider.min) v = f->slider.min;
            if (v > f->slider.max) v = f->slider.max;
            if (v != f->slider.value) {
                f->slider.value = v;
                if (f->slider.cb) f->slider.cb(f, v, f->slider.cb_ud);
                ui->dirty = true;
            }
            return true;
        }
        if (f->type == FDK_WIDGET_DROPDOWN) {
            int sel = f->dropdown.selected;
            int n   = f->dropdown.item_count;
            if (ev->key.key == FDK_KEY_RETURN || ev->key.key == FDK_KEY_SPACE) {
                f->dropdown.open = !f->dropdown.open;
                ui->dirty = true;
                return true;
            }
            if (ev->key.key == FDK_KEY_ESCAPE) {
                f->dropdown.open         = false;
                f->dropdown.hovered_item = -1;
                ui->dirty = true;
                return true;
            }
            if (ev->key.key == FDK_KEY_DOWN && sel < n - 1) {
                f->dropdown.selected = sel + 1;
                if (f->dropdown.cb)
                    f->dropdown.cb(f, f->dropdown.selected,
                                   f->dropdown.items[f->dropdown.selected],
                                   f->dropdown.cb_ud);
                ui->dirty = true;
                return true;
            }
            if (ev->key.key == FDK_KEY_UP && sel > 0) {
                f->dropdown.selected = sel - 1;
                if (f->dropdown.cb)
                    f->dropdown.cb(f, f->dropdown.selected,
                                   f->dropdown.items[f->dropdown.selected],
                                   f->dropdown.cb_ud);
                ui->dirty = true;
                return true;
            }
        }
        if (f->type == FDK_WIDGET_TOGGLE_BUTTON) {
            if (ev->key.key == FDK_KEY_RETURN || ev->key.key == FDK_KEY_SPACE) {
                f->toggle.active = !f->toggle.active;
                if (f->toggle.cb) f->toggle.cb(f, f->toggle.cb_ud);
                ui->dirty = true;
                return true;
            }
        }
        if (f->type == FDK_WIDGET_RADIO_BUTTON) {
            if (ev->key.key == FDK_KEY_RETURN || ev->key.key == FDK_KEY_SPACE) {
                if (f->radio.group) *f->radio.group = f->radio.index;
                if (f->radio.cb) f->radio.cb(f, f->radio.cb_ud);
                ui->dirty = true;
                return true;
            }
        }
        if (f->type == FDK_WIDGET_SPINNER) {
            bool changed = false;
            if (ev->key.key == FDK_KEY_UP || ev->key.key == FDK_KEY_RIGHT) {
                f->spinner.value += f->spinner.step;
                changed = true;
            } else if (ev->key.key == FDK_KEY_DOWN || ev->key.key == FDK_KEY_LEFT) {
                f->spinner.value -= f->spinner.step;
                changed = true;
            } else if (ev->key.key == FDK_KEY_HOME) {
                f->spinner.value = f->spinner.min;
                changed = true;
            } else if (ev->key.key == FDK_KEY_END) {
                f->spinner.value = f->spinner.max;
                changed = true;
            }
            if (changed) {
                if (f->spinner.value < f->spinner.min)
                    f->spinner.value = f->spinner.min;
                if (f->spinner.value > f->spinner.max)
                    f->spinner.value = f->spinner.max;
                if (f->spinner.cb)
                    f->spinner.cb(f, (float)f->spinner.value, f->spinner.cb_ud);
                ui->dirty = true;
                return true;
            }
        }
        return false;
    }

    case FDK_EVENT_KEY_UP: {
        FDK_Widget *f = ui->focused;
        if (f && f->type == FDK_WIDGET_BUTTON && f->button.pressed) {
            f->button.pressed = false;
            if (f->button.cb)
                f->button.cb(f, f->button.cb_ud);
            ui->dirty = true;
            return true;
        }
        return false;
    }

    default: return false;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PUBLIC UI LOOP
 * ═══════════════════════════════════════════════════════════════════════════ */

void fdk_ui_paint(FDK_UI *ui, FDK_Widget *root)
{
    fdk_begin_frame(ui->win);
    fdk_clear(ui->theme.bg_window);
    paint_widget(root, ui);
    paint_overlays(root, ui);

    /* ── Tooltip pass ── */
    if (ui->hover && ui->hover->tooltip) {
        uint64_t now    = fdk_time_ms();
        uint64_t hstart = ui->hover->hover_start_ms;
        if (hstart > 0 && now - hstart >= FDK_TOOLTIP_DELAY_MS) {
            const FDK_Theme *th  = &ui->theme;
            const char      *tip = ui->hover->tooltip;
            FDK_Font        *f   = th->font_label ? th->font_label : th->font_body;
            if (f) {
                FDK_Size  ts  = fdk_measure_text(f, tip);
                int pad       = 6;
                int tw        = ts.w + pad * 2;
                int th_h      = font_line_h(f, th) + pad * 2;
                FDK_Size wsz  = fdk_window_get_size(ui->win);

                int tx = ui->mouse_x + 12;
                int ty = ui->mouse_y + 18;
                if (tx + tw > wsz.w) tx = wsz.w - tw - 4;
                if (ty + th_h > wsz.h) ty = ui->mouse_y - th_h - 4;

                FDK_Rect bg = { tx - 1, ty - 1, tw + 2, th_h + 2 };
                fdk_fill_rect_rounded(bg, 4, FDK_RGBA(30, 22, 42, 230));
                fdk_stroke_rect(bg, th->border, 1);
                fdk_draw_text(f, tip, tx + pad, ty + pad,
                              FDK_RGB(220, 210, 235));
            }
        }
    }

    /* ── Toast pass (always on top) ── */
    paint_toasts(ui);

    /* ── Context menu overlay (always topmost) ── */
    if (ui->ctx_menu) {
        const FDK_Theme *th  = &ui->theme;
        FDK_Font        *f   = th->font_body;
        FDK_ContextMenu *cm  = ui->ctx_menu;
        const int item_h     = 28;
        const int sep_h      = 9;
        const int popup_w    = 200;
        const int pad        = 12;

        /* Calculate total height */
        int total_h = 4;
        for (int i = 0; i < cm->count; i++)
            total_h += cm->items[i].label ? item_h : sep_h;

        FDK_Size wsz = fdk_window_get_size(ui->win);
        int px = ui->ctx_x, py = ui->ctx_y;
        if (px + popup_w > wsz.w) px = wsz.w - popup_w - 4;
        if (py + total_h > wsz.h) py = wsz.h - total_h - 4;

        FDK_Rect bg = { px, py, popup_w, total_h };
        /* Shadow */
        FDK_Shadow sh = { 0, 4, 14, FDK_RGBA(0,0,0,120), true };
        fdk_draw_shadow(bg, th->radius_sm, &sh);
        fdk_fill_rect_rounded(bg, th->radius_sm, th->bg_widget);
        fdk_stroke_rect(bg, th->border_focus, 1);

        int lh = f ? font_line_h(f, th) : 16;
        int iy = py + 4;
        for (int i = 0; i < cm->count; i++) {
            if (!cm->items[i].label) {
                fdk_fill_rect((FDK_Rect){ px+8, iy+sep_h/2, popup_w-16, 1 },
                              th->separator);
                iy += sep_h;
                continue;
            }
            FDK_Rect ir = { px+1, iy, popup_w-2, item_h };
            bool hovered = rect_contains(ir, ui->mouse_x, ui->mouse_y);
            if (hovered && !cm->items[i].disabled)
                fdk_fill_rect_rounded(ir, th->radius_sm, th->bg_widget_hover);
            if (f) {
                FDK_Color fc = cm->items[i].disabled
                             ? th->fg_disabled : th->fg_primary;
                fdk_draw_text(f, cm->items[i].label,
                              px + pad, iy + (item_h - lh)/2, fc);
            }
            iy += item_h;
        }
    }

    fdk_end_frame(ui->win);
    ui->dirty = false;
}

/* ─── Toast rendering helpers ─────────────────────────────────────────────── */
static void paint_toasts(FDK_UI *ui)
{
    const FDK_Theme *th = &ui->theme;
    FDK_Font *f = th->font_body;
    if (!f) return;

    FDK_Size wsz   = fdk_window_get_size(ui->win);
    int toast_w    = 280;
    int toast_h    = 48;
    int margin     = 12;
    int stack_gap  = 8;

    /* Kind accent colours */
    static const FDK_Color kind_colors[] = {
        [FDK_NOTIFY_INFO]    = { 80, 140, 255, 255 },
        [FDK_NOTIFY_SUCCESS] = { 72, 200, 120, 255 },
        [FDK_NOTIFY_WARNING] = { 240, 180,  40, 255 },
        [FDK_NOTIFY_ERROR]   = { 220,  70,  70, 255 },
    };

    int slot_y = wsz.h - margin;
    for (int i = FDK_TOAST_MAX - 1; i >= 0; i--) {
        if (!ui->toasts[i].active) continue;

        float alpha   = ui->toasts[i].alpha;
        float slide_y = ui->toasts[i].slide_y;
        int   ay      = (int)(ui->toasts[i].alpha * 255.f + 0.5f);
        if (ay < 2) continue;

        int tx = wsz.w - toast_w - margin;
        int ty = slot_y - toast_h - (int)slide_y;
        slot_y = ty - stack_gap;

        FDK_Rect bg = { tx, ty, toast_w, toast_h };

        /* Shadow / background */
        fdk_fill_rect_rounded(bg, 8, FDK_RGBA(28, 20, 40, (int)(200 * alpha)));

        /* Left accent strip */
        FDK_Color acc = kind_colors[ui->toasts[i].kind & 3];
        acc.a = (uint8_t)(255 * alpha);
        fdk_fill_rect((FDK_Rect){ tx, ty + 6, 3, toast_h - 12 }, acc);

        /* Message text */
        int lh = font_line_h(f, th);
        int tty = ty + (toast_h - lh) / 2;
        fdk_push_clip((FDK_Rect){ tx + 10, ty + 2, toast_w - 14, toast_h - 4 });
        FDK_Color fc = FDK_RGBA(220, 210, 235, (int)(255 * alpha));
        fdk_draw_text(f, ui->toasts[i].message, tx + 12, tty, fc);
        fdk_pop_clip();
    }
}

/* Recursively advance animations. Returns true if any widget needs a repaint. */
static bool tick_animations(FDK_Widget *w, float dt)
{
    if (!w->visible) return false;
    bool dirty = false;

    if (w->type == FDK_WIDGET_PROGRESS_BAR && w->progress.indeterminate) {
        w->progress.anim_pos += dt * 0.7f;
        if (w->progress.anim_pos > 1.f)
            w->progress.anim_pos -= 1.f;
        dirty = true; /* indeterminate bar always needs repaint */
    }

    for (int i = 0; i < w->child_count; i++)
        if (tick_animations(w->children[i], dt)) dirty = true;

    if (w->type == FDK_WIDGET_SCROLL_VIEW && w->scroll.content)
        if (tick_animations(w->scroll.content, dt)) dirty = true;

    return dirty;
}

#define FDK_BLINK_MS 530   /* cursor on/off interval in milliseconds */

bool fdk_ui_step(FDK_UI *ui, FDK_Widget *root, FDK_Event *ev)
{
    FDK_Size sz = fdk_window_get_size(ui->win);
    bool has_size = (sz.w > 0 && sz.h > 0);

    /* Re-layout whenever the size changes OR on first step (root.w==0).
     * This catches both resize events AND the initial compositor configure
     * which arrives as an EXPOSE event with the real window dimensions. */
    bool need_layout = (ev->type == FDK_EVENT_RESIZE ||
                        ev->type == FDK_EVENT_EXPOSE  ||
                        root->rect.w == 0             ||
                        root->rect.w != sz.w          ||
                        root->rect.h != sz.h);

    if (need_layout && has_size) {
        do_layout(ui, root);
        ui->dirty = true;
    }

    bool consumed = dispatch_event(ui, root, ev);

    /* Animation tick — advance progress bars, tweens, etc. */
    {
        static uint64_t last_tick = 0;
        uint64_t now = fdk_time_ms();
        if (last_tick == 0) last_tick = now;
        float dt = (float)(now - last_tick) / 1000.f;
        last_tick = now;
        /* Only mark dirty if an animation actually advanced */
        if (tick_animations(root, dt)) ui->dirty = true;
        /* Advance tween system */
        fdk__tweens_tick();
    }

    /* ── Toast tick ── slide in, hold, slide out */
    {
        uint64_t now = fdk_time_ms();
        bool toast_dirty = false;
        for (int i = 0; i < FDK_TOAST_MAX; i++) {
            if (!ui->toasts[i].active) continue;
            uint64_t age = now - ui->toasts[i].created_ms;

            /* Slide in: first 200ms */
            if (!ui->toasts[i].dismissing) {
                float in_t = (float)age / 200.f;
                if (in_t > 1.f) in_t = 1.f;
                /* ease out cubic for slide */
                float e = 1.f - (1.f - in_t) * (1.f - in_t) * (1.f - in_t);
                ui->toasts[i].slide_y = 60.f * (1.f - e);
                ui->toasts[i].alpha   = in_t;

                /* Start dismissing when duration expires */
                if (age >= (uint64_t)ui->toasts[i].duration_ms)
                    ui->toasts[i].dismissing = true;
            } else {
                /* Fade out: 300ms */
                uint64_t dismiss_start = ui->toasts[i].created_ms
                                       + ui->toasts[i].duration_ms;
                float out_t = (float)(now - dismiss_start) / 300.f;
                if (out_t > 1.f) out_t = 1.f;
                ui->toasts[i].alpha   = 1.f - out_t;
                ui->toasts[i].slide_y = 60.f * out_t;
                if (out_t >= 1.f) ui->toasts[i].active = false;
            }
            toast_dirty = true;
        }
        if (toast_dirty) ui->dirty = true;
    }

    /* Tooltip delay — mark dirty once the hover dwell time is reached
     * so the tooltip renders without needing another input event. */
    if (ui->hover && ui->hover->tooltip && ui->hover->hover_start_ms > 0) {
        uint64_t now     = fdk_time_ms();
        uint64_t elapsed = now - ui->hover->hover_start_ms;
        /* Trigger a repaint right when the delay expires (±16 ms) */
        if (elapsed >= FDK_TOOLTIP_DELAY_MS &&
            elapsed <  FDK_TOOLTIP_DELAY_MS + 32)
            ui->dirty = true;
    }

    /* Blink tick — toggle cursor every FDK_BLINK_MS when a text input is focused */
    if (ui->focused && ui->focused->type == FDK_WIDGET_TEXT_INPUT) {
        uint64_t now = fdk_time_ms();
        if (ui->blink_last == 0) {
            ui->blink_last = now;
            ui->blink_on   = true;
        }
        if (now - ui->blink_last >= FDK_BLINK_MS) {
            ui->blink_on   = !ui->blink_on;
            ui->blink_last = now;
            ui->dirty      = true;
        }
    } else {
        /* Reset blink so it starts visible when a field gains focus */
        ui->blink_on   = true;
        ui->blink_last = 0;
    }

    /* Drain all additional pending events before painting.
     * This collapses bursts of MOUSE_MOVE events into a single repaint,
     * eliminating the hover flicker on rapid mouse movement. */
    FDK_Event pending;
    while (fdk_poll_event(&pending)) {
        if (pending.type == FDK_EVENT_QUIT ||
            pending.type == FDK_EVENT_CLOSE) {
            /* Re-inject close so the caller's loop sees it */
            *ev = pending;
            if (ui->dirty) fdk_ui_paint(ui, root);
            return consumed;
        }

        FDK_Size psz = fdk_window_get_size(ui->win);
        bool phas = (psz.w > 0 && psz.h > 0);
        bool pneed = (pending.type == FDK_EVENT_RESIZE ||
                      pending.type == FDK_EVENT_EXPOSE);
        if (pneed && phas) {
            do_layout(ui, root);
            ui->dirty = true;
        }
        dispatch_event(ui, root, &pending);
    }

    if (ui->dirty)
        fdk_ui_paint(ui, root);

    return consumed;
}

void fdk_ui_run(FDK_UI *ui, FDK_Widget *root)
{
    do_layout(ui, root);
    fdk_ui_paint(ui, root);

    FDK_Event ev;
    for (;;) {
        /* Use 16ms timeout so cursor blink ticks work without blocking
         * the compositor — fdk_wait_event_timeout reads the socket
         * properly unlike poll+sleep */
        bool got = fdk_wait_event_timeout(&ev, 16);
        if (!got) ev.type = FDK_EVENT_NONE;
        if (ev.type == FDK_EVENT_QUIT || ev.type == FDK_EVENT_CLOSE)
            break;
        fdk_ui_step(ui, root, &ev);
    }
}
