#define _GNU_SOURCE
/*
 * x11.c — FDK X11 platform backend
 *
 * Uses Xlib for windowing and XKB (via libxkbcommon-x11 or plain Xlib)
 * for keyboard handling. No XDG, no D-Bus, no systemd.
 *
 * Software render path: uses XImage / MIT-SHM (fallback to plain XPutImage)
 * OpenGL path:          uses GLX
 */
#include "platform_internal.h"
#include "../core/core_internal.h"

#ifdef FDK_HAVE_X11

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xcursor/Xcursor.h>
#include <time.h>
#include <string.h>
#include <poll.h>
#include <errno.h>

#ifdef FDK_HAVE_OPENGL
#  include <GL/glx.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Platform-private window ────────────────────────────────────────────── */
struct FDK_PlatformWindow {
    Window   xwin;
    int      w, h;
    bool     mapped;

    /* Software render resources */
    GC       gc;
    XImage  *ximage;
    uint32_t *pixels;   /* BGRA, row-major */
    int      stride_px;

#ifdef FDK_HAVE_OPENGL
    GLXContext  glx_ctx;
    GLXFBConfig glx_fbc;
#endif
};

/* ─── Global X11 state ───────────────────────────────────────────────────── */
static Display *s_dpy        = NULL;
static int      s_screen     = 0;
static Atom     s_wm_delete;

/* Clipboard atoms */
static Atom     s_atom_clipboard;   /* CLIPBOARD selection              */
static Atom     s_atom_targets;     /* TARGETS — list supported formats */
static Atom     s_atom_utf8;        /* UTF8_STRING                      */
static Atom     s_atom_string;      /* STRING (plain ASCII fallback)    */
static Atom     s_atom_fdk_sel;     /* FDK_SELECTION — our temp prop    */

/* Clipboard state */
static char    *s_clipboard     = NULL;  /* text we own              */
static Window   s_clip_owner   = None;   /* window that owns CLIPBOARD */

/* Cursor cache — one Cursor per FDK_Cursor value */
static Cursor   s_cursors[8];           /* indexed by FDK_Cursor enum  */
static bool     s_cursors_loaded = false;

/* ─── GLX global state ──────────────────────────────────────────────────── */
#ifdef FDK_HAVE_OPENGL
static GLXFBConfig s_glx_fbc     = NULL;
static GLXContext  s_glx_ctx     = NULL; /* shared context */
static XVisualInfo *s_glx_vis    = NULL;

/* Initialise GLX — find a suitable FBConfig and create a shared context */
static bool x11_glx_init(void)
{
    if (s_glx_ctx) return true; /* already done */

    int attrs[] = {
        GLX_DOUBLEBUFFER,  True,
        GLX_RED_SIZE,      8,
        GLX_GREEN_SIZE,    8,
        GLX_BLUE_SIZE,     8,
        GLX_ALPHA_SIZE,    8,
        GLX_DEPTH_SIZE,    0,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        None
    };

    int n = 0;
    GLXFBConfig *cfgs = glXChooseFBConfig(s_dpy, s_screen, attrs, &n);
    if (!cfgs || n == 0) {
        fprintf(stderr, "[FDK/X11/GL] No suitable GLX FBConfig found\n");
        return false;
    }
    s_glx_fbc = cfgs[0];
    XFree(cfgs);

    s_glx_vis = glXGetVisualFromFBConfig(s_dpy, s_glx_fbc);
    if (!s_glx_vis) {
        fprintf(stderr, "[FDK/X11/GL] glXGetVisualFromFBConfig failed\n");
        return false;
    }

    /* Try GL 3.3 core context first */
    typedef GLXContext (*glXCreateContextAttribsARBProc)
        (Display*, GLXFBConfig, GLXContext, Bool, const int*);
    glXCreateContextAttribsARBProc createCtx =
        (glXCreateContextAttribsARBProc)
        glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");

    if (createCtx) {
        int ctx_attrs[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 3,
            GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None
        };
        s_glx_ctx = createCtx(s_dpy, s_glx_fbc, NULL, True, ctx_attrs);
    }
    /* Fallback: legacy context */
    if (!s_glx_ctx)
        s_glx_ctx = glXCreateNewContext(s_dpy, s_glx_fbc,
                                        GLX_RGBA_TYPE, NULL, True);

    if (!s_glx_ctx) {
        fprintf(stderr, "[FDK/X11/GL] Failed to create GLX context\n");
        return false;
    }
    return true;
}
#endif /* FDK_HAVE_OPENGL */

/* ─── Init / shutdown ────────────────────────────────────────────────────── */
static bool x11_init(void)
{
    s_dpy = XOpenDisplay(NULL);
    if (!s_dpy) {
        fprintf(stderr, "[FDK/X11] Cannot open display '%s'\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return false;
    }
    s_screen     = DefaultScreen(s_dpy);
    s_wm_delete  = XInternAtom(s_dpy, "WM_DELETE_WINDOW", False);

    /* Enable XKB key repeat control */
    XkbSetDetectableAutoRepeat(s_dpy, True, NULL);

    /* Intern clipboard atoms */
    s_atom_clipboard = XInternAtom(s_dpy, "CLIPBOARD",       False);
    s_atom_targets   = XInternAtom(s_dpy, "TARGETS",         False);
    s_atom_utf8      = XInternAtom(s_dpy, "UTF8_STRING",      False);
    s_atom_string    = XInternAtom(s_dpy, "STRING",           False);
    s_atom_fdk_sel   = XInternAtom(s_dpy, "FDK_SELECTION",   False);

    return true;
}

static void x11_shutdown(void)
{
#ifdef FDK_HAVE_OPENGL
    if (s_glx_ctx) { glXDestroyContext(s_dpy, s_glx_ctx); s_glx_ctx = NULL; }
    if (s_glx_vis) { XFree(s_glx_vis); s_glx_vis = NULL; }
#endif
    free(s_clipboard); s_clipboard = NULL;
    if (s_dpy) { XCloseDisplay(s_dpy); s_dpy = NULL; }
}

/* ─── Window creation ────────────────────────────────────────────────────── */
static FDK_PlatformWindow *x11_window_create(const FDK_WindowDesc *desc)
{
    FDK_PlatformWindow *pw = calloc(1, sizeof *pw);
    if (!pw) return NULL;

    pw->w = desc->w;
    pw->h = desc->h;

    int x = desc->x == FDK_WINDOW_POS_CENTER
                ? (DisplayWidth(s_dpy,s_screen)  - desc->w) / 2
                : desc->x;
    int y = desc->y == FDK_WINDOW_POS_CENTER
                ? (DisplayHeight(s_dpy,s_screen) - desc->h) / 2
                : desc->y;

    /* Decide GL vs software — initialise GLX if needed */
    bool use_gl = false;
#ifdef FDK_HAVE_OPENGL
    use_gl = (desc->render == FDK_RENDER_OPENGL ||
              desc->render == FDK_RENDER_AUTO);
    if (use_gl && !x11_glx_init()) {
        fprintf(stderr, "[FDK/X11] GLX init failed, falling back to software\n");
        use_gl = false;
    }
#endif

    /* Choose visual — GLX needs its own, software uses default */
    Visual *vis;
    int     depth;
    Colormap cmap;

#ifdef FDK_HAVE_OPENGL
    if (use_gl && s_glx_vis) {
        vis   = s_glx_vis->visual;
        depth = s_glx_vis->depth;
        cmap  = XCreateColormap(s_dpy, RootWindow(s_dpy, s_screen),
                                vis, AllocNone);
    } else
#endif
    {
        vis   = DefaultVisual(s_dpy, s_screen);
        depth = DefaultDepth(s_dpy, s_screen);
        cmap  = DefaultColormap(s_dpy, s_screen);
    }

    XSetWindowAttributes attrs = {0};
    attrs.colormap       = cmap;
    attrs.background_pixel = 0;
    attrs.event_mask     = ExposureMask | StructureNotifyMask
                         | KeyPressMask | KeyReleaseMask
                         | ButtonPressMask | ButtonReleaseMask
                         | PointerMotionMask;

    unsigned long attr_mask = CWColormap | CWBackPixel | CWEventMask;

    pw->xwin = XCreateWindow(
        s_dpy, RootWindow(s_dpy, s_screen),
        x, y, desc->w, desc->h, 0,
        depth, InputOutput, vis,
        attr_mask, &attrs);

    if (!pw->xwin) { free(pw); return NULL; }

    /* WM protocols */
    XSetWMProtocols(s_dpy, pw->xwin, &s_wm_delete, 1);
    XStoreName(s_dpy, pw->xwin, desc->title ? desc->title : "FDK");

    /* Size hints */
    if (!desc->resizable) {
        XSizeHints *sh = XAllocSizeHints();
        sh->flags      = PMinSize | PMaxSize;
        sh->min_width  = sh->max_width  = desc->w;
        sh->min_height = sh->max_height = desc->h;
        XSetWMNormalHints(s_dpy, pw->xwin, sh);
        XFree(sh);
    }

#ifdef FDK_HAVE_OPENGL
    if (use_gl) {
        /* Create a per-window GLX context that shares with the global one */
        pw->glx_ctx = glXCreateNewContext(s_dpy, s_glx_fbc,
                                           GLX_RGBA_TYPE, s_glx_ctx, True);
        pw->glx_fbc = s_glx_fbc;
        /* Make current so the GL renderer can init */
        glXMakeCurrent(s_dpy, pw->xwin, pw->glx_ctx);
        /* vsync — look up glXSwapIntervalEXT at runtime, ignore if absent */
        {
            typedef void (*PFNGLXSWAPINTERVALEXTPROC)(Display*, GLXDrawable, int);
            PFNGLXSWAPINTERVALEXTPROC swapInterval =
                (PFNGLXSWAPINTERVALEXTPROC)
                glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT");
            if (swapInterval)
                swapInterval(s_dpy, pw->xwin, 1);
        }
    } else
#endif
    {
        /* Software-render pixel buffer */
        pw->stride_px = desc->w;
        pw->pixels    = calloc((size_t)desc->w * desc->h, 4);
        pw->gc        = XCreateGC(s_dpy, pw->xwin, 0, NULL);
        pw->ximage    = XCreateImage(
            s_dpy, vis, depth, ZPixmap, 0,
            (char *)pw->pixels, desc->w, desc->h, 32, desc->w * 4);
    }

    XFlush(s_dpy);
    return pw;
}

static void x11_window_destroy(FDK_PlatformWindow *pw)
{
    if (!pw) return;
#ifdef FDK_HAVE_OPENGL
    if (pw->glx_ctx) {
        glXMakeCurrent(s_dpy, None, NULL);
        glXDestroyContext(s_dpy, pw->glx_ctx);
        pw->glx_ctx = NULL;
    }
#endif
    if (pw->ximage) {
        pw->ximage->data = NULL;
        XDestroyImage(pw->ximage);
    }
    free(pw->pixels);
    if (pw->gc)   XFreeGC(s_dpy, pw->gc);
    if (pw->xwin) XDestroyWindow(s_dpy, pw->xwin);
    free(pw);
}

static void x11_window_show(FDK_PlatformWindow *pw)
{
    XMapRaised(s_dpy, pw->xwin);
    XFlush(s_dpy);
    pw->mapped = true;
}

static void x11_window_hide(FDK_PlatformWindow *pw)
{
    XUnmapWindow(s_dpy, pw->xwin);
    XFlush(s_dpy);
    pw->mapped = false;
}

static void x11_window_set_title(FDK_PlatformWindow *pw, const char *t)
{
    XStoreName(s_dpy, pw->xwin, t);
    XFlush(s_dpy);
}

static FDK_Size x11_window_get_size(FDK_PlatformWindow *pw)
{
    XWindowAttributes wa;
    XGetWindowAttributes(s_dpy, pw->xwin, &wa);
    return (FDK_Size){ wa.width, wa.height };
}

static void x11_window_request_redraw(FDK_PlatformWindow *pw)
{
    XEvent ev = {0};
    ev.type           = Expose;
    ev.xexpose.window = pw->xwin;
    ev.xexpose.count  = 0;
    XSendEvent(s_dpy, pw->xwin, False, ExposureMask, &ev);
    XFlush(s_dpy);
}

/* ─── Software framebuffer access ────────────────────────────────────────── */
static uint32_t *x11_get_framebuffer(FDK_PlatformWindow *pw, int *stride_px)
{
#ifdef FDK_HAVE_OPENGL
    if (pw->glx_ctx) { *stride_px = 0; return NULL; } /* GL window — no CPU buffer */
#endif
    *stride_px = pw->stride_px;
    return pw->pixels;
}

static void x11_present(FDK_PlatformWindow *pw)
{
#ifdef FDK_HAVE_OPENGL
    if (pw->glx_ctx) return; /* GL windows present via gl_swap_buffers */
#endif
    if (!pw->mapped) return;
    XPutImage(s_dpy, pw->xwin, pw->gc, pw->ximage,
              0, 0, 0, 0, pw->w, pw->h);
    XFlush(s_dpy);
}

/* ─── OpenGL ─────────────────────────────────────────────────────────────── */
static bool x11_gl_make_current(FDK_PlatformWindow *pw)
{
#ifdef FDK_HAVE_OPENGL
    if (!pw->glx_ctx) return false;
    return glXMakeCurrent(s_dpy, pw->xwin, pw->glx_ctx) == True;
#else
    (void)pw; return false;
#endif
}

static void x11_gl_swap_buffers(FDK_PlatformWindow *pw)
{
#ifdef FDK_HAVE_OPENGL
    if (pw->glx_ctx)
        glXSwapBuffers(s_dpy, pw->xwin);
#else
    (void)pw;
#endif
}

/* ─── Key translation ────────────────────────────────────────────────────── */
static FDK_Key keysym_to_fdk(KeySym ks)
{
    switch (ks) {
    case XK_Escape:    return FDK_KEY_ESCAPE;
    case XK_Return:    return FDK_KEY_RETURN;
    case XK_space:     return FDK_KEY_SPACE;
    case XK_BackSpace: return FDK_KEY_BACKSPACE;
    case XK_Tab:       return FDK_KEY_TAB;
    case XK_Delete:    return FDK_KEY_DELETE;
    case XK_Left:      return FDK_KEY_LEFT;
    case XK_Right:     return FDK_KEY_RIGHT;
    case XK_Up:        return FDK_KEY_UP;
    case XK_Down:      return FDK_KEY_DOWN;
    case XK_Home:      return FDK_KEY_HOME;
    case XK_End:       return FDK_KEY_END;
    case XK_Page_Up:   return FDK_KEY_PAGEUP;
    case XK_Page_Down: return FDK_KEY_PAGEDOWN;
    case XK_F1:        return FDK_KEY_F1;
    case XK_F2:        return FDK_KEY_F2;
    case XK_F3:        return FDK_KEY_F3;
    case XK_F4:        return FDK_KEY_F4;
    case XK_F5:        return FDK_KEY_F5;
    case XK_F6:        return FDK_KEY_F6;
    case XK_F7:        return FDK_KEY_F7;
    case XK_F8:        return FDK_KEY_F8;
    case XK_F9:        return FDK_KEY_F9;
    case XK_F10:       return FDK_KEY_F10;
    case XK_F11:       return FDK_KEY_F11;
    case XK_F12:       return FDK_KEY_F12;
    default:
        if (ks >= 'a' && ks <= 'z') return (FDK_Key)ks;
        if (ks >= 'A' && ks <= 'Z') return (FDK_Key)(ks - 'A' + 'a');
        return FDK_KEY_UNKNOWN;
    }
}

static FDK_Modifier x11_mods(unsigned int state)
{
    FDK_Modifier m = FDK_MOD_NONE;
    if (state & ShiftMask)   m |= FDK_MOD_SHIFT;
    if (state & ControlMask) m |= FDK_MOD_CTRL;
    if (state & Mod1Mask)    m |= FDK_MOD_ALT;
    if (state & Mod4Mask)    m |= FDK_MOD_SUPER;
    return m;
}

/* ─── Event translation ──────────────────────────────────────────────────── */
/* We keep a simple linked list of FDK_Windows so we can look up by XWindow */
typedef struct WinNode { Window xw; FDK_Window *fdkw; struct WinNode *next; } WinNode;
static WinNode *s_wins = NULL;



static FDK_Window *find_fdkw(Window xw)
{
    for (WinNode *n = s_wins; n; n = n->next)
        if (n->xw == xw) return n->fdkw;
    return NULL;
}

static bool x11_poll_event(FDK_Event *out)
{
    if (!XPending(s_dpy)) return false;

    XEvent xe;
    XNextEvent(s_dpy, &xe);
    memset(out, 0, sizeof *out);

    switch (xe.type) {
    case ClientMessage:
        if ((Atom)xe.xclient.data.l[0] == s_wm_delete) {
            out->type   = FDK_EVENT_CLOSE;
            out->window = find_fdkw(xe.xclient.window);
            return true;
        }
        break;

    case ConfigureNotify: {
        FDK_Window *fw = find_fdkw(xe.xconfigure.window);
        if (fw) {
            out->type        = FDK_EVENT_RESIZE;
            out->window      = fw;
            out->resize.w    = xe.xconfigure.width;
            out->resize.h    = xe.xconfigure.height;
            fw->pw->w = xe.xconfigure.width;
            fw->pw->h = xe.xconfigure.height;
            fw->w     = xe.xconfigure.width;
            fw->h     = xe.xconfigure.height;
            return true;
        }
        break;
    }

    case Expose:
        if (xe.xexpose.count == 0) {
            out->type   = FDK_EVENT_EXPOSE;
            out->window = find_fdkw(xe.xexpose.window);
            return true;
        }
        break;

    case KeyPress:
    case KeyRelease: {
        KeySym ks = XkbKeycodeToKeysym(s_dpy, xe.xkey.keycode, 0, 0);
        out->type     = (xe.type == KeyPress) ? FDK_EVENT_KEY_DOWN
                                               : FDK_EVENT_KEY_UP;
        out->window   = find_fdkw(xe.xkey.window);
        out->key.key  = keysym_to_fdk(ks);
        out->key.mods = x11_mods(xe.xkey.state);
        /* decode UTF-8 codepoint */
        char buf[8] = {0};
        XLookupString(&xe.xkey, buf, sizeof buf, NULL, NULL);
        out->key.codepoint = (uint32_t)(unsigned char)buf[0];
        return true;
    }

    case ButtonPress:
    case ButtonRelease: {
        if (xe.xbutton.button == 4 || xe.xbutton.button == 5) {
            out->type          = FDK_EVENT_MOUSE_SCROLL;
            out->window        = find_fdkw(xe.xbutton.window);
            out->scroll.dx     = 0;
            out->scroll.dy     = (xe.xbutton.button == 4) ? 1.0f : -1.0f;
            return true;
        }
        out->type          = (xe.type == ButtonPress)
                                ? FDK_EVENT_MOUSE_DOWN : FDK_EVENT_MOUSE_UP;
        out->window        = find_fdkw(xe.xbutton.window);
        out->mouse.x       = xe.xbutton.x;
        out->mouse.y       = xe.xbutton.y;
        out->mouse.button  = (FDK_MouseButton)xe.xbutton.button;
        out->mouse.mods    = x11_mods(xe.xbutton.state);
        return true;
    }

    case MotionNotify:
        out->type        = FDK_EVENT_MOUSE_MOVE;
        out->window      = find_fdkw(xe.xmotion.window);
        out->motion.x    = xe.xmotion.x;
        out->motion.y    = xe.xmotion.y;
        out->motion.mods = x11_mods(xe.xmotion.state);
        return true;

    default:
        break;
    }

    out->type = FDK_EVENT_NONE;
    return false;
}

static void x11_wait_event(FDK_Event *out)
{
    /* Block until we successfully translate an event.
     * XNextEvent blocks when the queue is empty — no busy-wait. */
    do {
        XEvent xe;
        XNextEvent(s_dpy, &xe);
        /* Push it back and let poll_event handle the translation,
         * so all event logic stays in one place. */
        XPutBackEvent(s_dpy, &xe);
    } while (!x11_poll_event(out));
}

/* ─── Cursor ────────────────────────────────────────────────────────────────── */
static void x11_load_cursors(void)
{
    if (s_cursors_loaded) return;

    /* Map FDK_Cursor values to Xcursor theme names */
    const char *names[] = {
        "default",      /* FDK_CURSOR_DEFAULT     */
        "pointer",      /* FDK_CURSOR_POINTER     */
        "text",         /* FDK_CURSOR_TEXT        */
        "crosshair",    /* FDK_CURSOR_CROSSHAIR   */
        "move",         /* FDK_CURSOR_MOVE        */
        "ew-resize",    /* FDK_CURSOR_RESIZE_H    */
        "ns-resize",    /* FDK_CURSOR_RESIZE_V    */
        "not-allowed",  /* FDK_CURSOR_NOT_ALLOWED */
    };

    for (int i = 0; i < 8; i++) {
        s_cursors[i] = XcursorLibraryLoadCursor(s_dpy, names[i]);
        if (s_cursors[i] == None) {
            /* Fallback to classic X11 font cursors */
            unsigned int fallbacks[] = { 68, 60, 152, 34, 52, 108, 116, 0 };
            s_cursors[i] = XCreateFontCursor(s_dpy, fallbacks[i]);
        }
    }
    s_cursors_loaded = true;
}

static void x11_set_cursor(FDK_Cursor cursor)
{
    if (!s_dpy) return;
    x11_load_cursors();

    int idx = (int)cursor;
    if (idx < 0 || idx >= 8) idx = 0;

    /* Apply cursor to all registered windows */
    for (WinNode *n = s_wins; n; n = n->next)
        XDefineCursor(s_dpy, n->xw, s_cursors[idx]);
    XFlush(s_dpy);
}

/* ─── Window register ────────────────────────────────────────────────────────── */
void x11_window_register(FDK_PlatformWindow *pw, FDK_Window *fdkw)
{
    WinNode *n = malloc(sizeof *n);
    n->xw = pw->xwin; n->fdkw = fdkw; n->next = s_wins; s_wins = n;
}

/* ─── Clipboard ──────────────────────────────────────────────────────────────── */

/* Set our text as the CLIPBOARD owner */
static void x11_clipboard_set(const char *text)
{
    if (!text || !s_dpy) return;
    free(s_clipboard);
    s_clipboard = strdup(text);

    /* We need a window to own the selection — use the first registered one */
    Window owner = s_wins ? s_wins->xw : None;
    if (owner == None) return;

    XSetSelectionOwner(s_dpy, s_atom_clipboard, owner, CurrentTime);
    if (XGetSelectionOwner(s_dpy, s_atom_clipboard) == owner)
        s_clip_owner = owner;
    XFlush(s_dpy);
}

/* Handle SelectionRequest — another app wants our clipboard data */
static void x11_handle_selection_request(XSelectionRequestEvent *req)
{
    XSelectionEvent reply = {0};
    reply.type      = SelectionNotify;
    reply.display   = s_dpy;
    reply.requestor = req->requestor;
    reply.selection = req->selection;
    reply.target    = req->target;
    reply.property  = None;   /* default: refuse */
    reply.time      = req->time;

    if (req->target == s_atom_targets) {
        /* Report which formats we support */
        Atom supported[] = { s_atom_targets, s_atom_utf8, s_atom_string };
        XChangeProperty(s_dpy, req->requestor, req->property,
                        XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)supported, 3);
        reply.property = req->property;
    } else if ((req->target == s_atom_utf8 ||
                req->target == s_atom_string ||
                req->target == XA_STRING) && s_clipboard) {
        XChangeProperty(s_dpy, req->requestor, req->property,
                        req->target, 8, PropModeReplace,
                        (unsigned char*)s_clipboard,
                        (int)strlen(s_clipboard));
        reply.property = req->property;
    }

    XSendEvent(s_dpy, req->requestor, False, 0, (XEvent*)&reply);
    XFlush(s_dpy);
}

/* Request clipboard text from the current CLIPBOARD owner */
static char *x11_clipboard_get(void)
{
    if (!s_dpy) return NULL;

    /* If we own the clipboard just return our copy */
    Window owner = XGetSelectionOwner(s_dpy, s_atom_clipboard);
    if (owner == None) return NULL;
    if (owner == s_clip_owner && s_clipboard)
        return strdup(s_clipboard);

    /* Need a requestor window */
    Window req_win = s_wins ? s_wins->xw : None;
    if (req_win == None) return NULL;

    /* Ask the owner to convert to UTF8_STRING and put it in our property */
    XConvertSelection(s_dpy, s_atom_clipboard, s_atom_utf8,
                      s_atom_fdk_sel, req_win, CurrentTime);
    XFlush(s_dpy);

    /* Wait for SelectionNotify with a timeout (~200ms) */
    XEvent ev;
    for (int i = 0; i < 200; i++) {
        if (XCheckTypedWindowEvent(s_dpy, req_win,
                                   SelectionNotify, &ev)) {
            if (ev.xselection.property == None) return NULL;

            /* Read the property */
            Atom           actual_type;
            int            actual_fmt;
            unsigned long  nitems, bytes_left;
            unsigned char *data = NULL;

            XGetWindowProperty(s_dpy, req_win, s_atom_fdk_sel,
                               0, 65536, True,
                               AnyPropertyType,
                               &actual_type, &actual_fmt,
                               &nitems, &bytes_left, &data);
            if (data && nitems > 0) {
                char *result = strdup((char*)data);
                XFree(data);
                return result;
            }
            if (data) XFree(data);
            return NULL;
        }
        /* Process any pending events while waiting */
        while (XPending(s_dpy)) {
            XEvent pending;
            XNextEvent(s_dpy, &pending);
            if (pending.type == SelectionRequest)
                x11_handle_selection_request(&pending.xselectionrequest);
            else
                XPutBackEvent(s_dpy, &pending);
        }
        struct timespec ts = {0, 1000000}; /* 1ms */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static bool x11_wait_event_timeout(FDK_Event *out, int timeout_ms)
{
    /* Check already-queued events first */
    if (x11_poll_event(out)) return true;
    if (timeout_ms == 0) return false;

    int fd = ConnectionNumber(s_dpy);
    struct pollfd pfd = { fd, POLLIN, 0 };
    int ret = poll(&pfd, 1, timeout_ms);

    if (ret > 0)
        return x11_poll_event(out);
    if (ret == 0)
        return false; /* timeout */
    if (errno == EINTR)
        return false;
    return false;
}

/* ─── VTable ─────────────────────────────────────────────────────────────── */
const FDK_PlatformVTable fdk_platform_x11 = {
    .name                  = "X11",
    .init                  = x11_init,
    .shutdown              = x11_shutdown,
    .window_create         = x11_window_create,
    .window_destroy        = x11_window_destroy,
    .window_show           = x11_window_show,
    .window_hide           = x11_window_hide,
    .window_set_title      = x11_window_set_title,
    .window_get_size       = x11_window_get_size,
    .window_request_redraw = x11_window_request_redraw,
    .window_get_framebuffer= x11_get_framebuffer,
    .window_present        = x11_present,
    .gl_make_current       = x11_gl_make_current,
    .gl_swap_buffers       = x11_gl_swap_buffers,
    .poll_event            = x11_poll_event,
    .wait_event            = x11_wait_event,
    .window_register       = x11_window_register,
    .set_cursor            = x11_set_cursor,
    .clipboard_set         = x11_clipboard_set,
    .clipboard_get         = x11_clipboard_get,
};

#endif /* FDK_HAVE_X11 */
