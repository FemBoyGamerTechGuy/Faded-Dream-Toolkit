/*
 * platform_internal.h — FDK Platform Abstraction Layer (internal)
 */
#ifndef FDK_PLATFORM_INTERNAL_H
#define FDK_PLATFORM_INTERNAL_H

#include "fdk/fdk.h"

typedef struct FDK_PlatformWindow FDK_PlatformWindow;

typedef struct {
    const char *name;
    bool (*init)(void);
    void (*shutdown)(void);
    FDK_PlatformWindow *(*window_create)(const FDK_WindowDesc *desc);
    void                (*window_destroy)(FDK_PlatformWindow *pw);
    void                (*window_show)(FDK_PlatformWindow *pw);
    void                (*window_hide)(FDK_PlatformWindow *pw);
    void                (*window_set_title)(FDK_PlatformWindow *pw, const char *title);
    FDK_Size            (*window_get_size)(FDK_PlatformWindow *pw);
    void                (*window_request_redraw)(FDK_PlatformWindow *pw);
    uint32_t *(*window_get_framebuffer)(FDK_PlatformWindow *pw, int *out_stride_px);
    void      (*window_present)(FDK_PlatformWindow *pw);
    bool (*gl_make_current)(FDK_PlatformWindow *pw);
    void (*gl_swap_buffers)(FDK_PlatformWindow *pw);
    bool (*poll_event)(FDK_Event *out);
    void (*wait_event)(FDK_Event *out);
    /* Returns false on timeout, true if event produced. timeout_ms<0 = block forever */
    bool (*wait_event_timeout)(FDK_Event *out, int timeout_ms);
    /* Called after window creation so backends can store the FDK_Window* */
    void (*window_register)(FDK_PlatformWindow *pw, struct FDK_Window *fdkw);
    /* Change the cursor shape */
    void  (*set_cursor)(FDK_Cursor cursor);
    /* Clipboard */
    void  (*clipboard_set)(const char *text);
    char *(*clipboard_get)(void);   /* caller must free() */
} FDK_PlatformVTable;

extern const FDK_PlatformVTable fdk_platform_x11;
extern const FDK_PlatformVTable fdk_platform_wayland;
extern const FDK_PlatformVTable *fdk__platform;

#endif /* FDK_PLATFORM_INTERNAL_H */
