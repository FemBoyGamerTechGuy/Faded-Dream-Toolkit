/*
 * showcase.c — FDK v0.7.0 feature showcase
 *
 * Demonstrates:
 *   FDK_MenuBar  — File / Edit / View menus with keyboard shortcuts displayed
 *   FDK_Tabs     — three-page tab view
 *   fdk_notify() — INFO / SUCCESS / WARNING / ERROR toast notifications
 *   All v0.6.0 widgets still working (toggle, radio, spinner, badge, tween)
 */
#include <fdk/fdk.h>
#include <fdk/fdk_widget.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Fonts ───────────────────────────────────────────────────────────────── */
static const char *font_paths[] = {
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/Hack-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    NULL
};

/* ── App state ───────────────────────────────────────────────────────────── */
static bool   g_running   = true;
static int    g_radio_sel = 0;
static FDK_UI *g_ui       = NULL;   /* for menu callbacks */

/* ── Toast notification callbacks ────────────────────────────────────────── */
static void menu_new(void *ud)
{ (void)ud; fdk_notify(g_ui, "New file created", FDK_NOTIFY_SUCCESS, 0); }

static void menu_open(void *ud)
{ (void)ud; fdk_notify(g_ui, "Open: no file selected", FDK_NOTIFY_WARNING, 0); }

static void menu_save(void *ud)
{ (void)ud; fdk_notify(g_ui, "Saved successfully", FDK_NOTIFY_SUCCESS, 0); }

static void menu_quit(void *ud)
{ (void)ud; g_running = false; }

static void menu_undo(void *ud)
{ (void)ud; fdk_notify(g_ui, "Nothing to undo", FDK_NOTIFY_INFO, 0); }

static void menu_about(void *ud)
{ (void)ud; fdk_notify(g_ui, "FDK v0.7.0 — Faded Dream Kit", FDK_NOTIFY_INFO, 3500); }

static void on_quit_btn(FDK_Widget *w, void *ud)
{ (void)w; *(bool*)ud = false; }

/* ── Tween demo ───────────────────────────────────────────────────────────── */
static FDK_Widget *g_tween_bar = NULL;
static void tween_update(float v, void *ud) {
    (void)ud;
    if (g_tween_bar) fdk_progress_set_value(g_tween_bar, v);
}
static void on_animate(FDK_Widget *w, void *ud) {
    (void)w; (void)ud;
    fdk_tween(0.f, 1.f, 1200, FDK_EASE_OUT_ELASTIC, tween_update, NULL, NULL);
}
static void on_notify_error(FDK_Widget *w, void *ud) {
    (void)w; (void)ud;
    fdk_notify(g_ui, "Something went wrong!", FDK_NOTIFY_ERROR, 0);
}
static void on_notify_warn(FDK_Widget *w, void *ud) {
    (void)w; (void)ud;
    fdk_notify(g_ui, "Disk space is running low", FDK_NOTIFY_WARNING, 0);
}

/* ── Spinner ─────────────────────────────────────────────────────────────── */
typedef struct { FDK_Widget *bar; FDK_Widget *badge; } SpUD;
static void on_spinner(FDK_Widget *w, float v, void *ud) {
    (void)w;
    SpUD *s = ud;
    fdk_progress_set_value(s->bar, v / 100.f);
    char buf[12]; snprintf(buf, sizeof buf, "%.0f%%", v);
    fdk_badge_set_text(s->badge, buf);
}

/* ── Build pages ─────────────────────────────────────────────────────────── */

static void build_page_controls(FDK_Widget *page)
{
    fdk_container_add(page, fdk_label("Toggle buttons:"));
    FDK_Widget *row1 = fdk_hbox(10, 0);
    fdk_widget_set_size(row1, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    fdk_container_add(row1, fdk_toggle_button("Wi-Fi", true));
    fdk_container_add(row1, fdk_toggle_button("Bluetooth", false));
    fdk_container_add(row1, fdk_toggle_button("Airplane", false));
    fdk_container_add(page, row1);

    fdk_container_add(page, fdk_separator());

    fdk_container_add(page, fdk_label("Radio group:"));
    FDK_Widget *row2 = fdk_hbox(20, 0);
    fdk_widget_set_size(row2, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    const char *opts[] = { "Small", "Medium", "Large" };
    for (int i = 0; i < 3; i++)
        fdk_container_add(row2, fdk_radio_button(opts[i], &g_radio_sel, i));
    fdk_container_add(page, row2);

    fdk_container_add(page, fdk_separator());

    /* Spinner + progress + badge */
    static SpUD sp_ud;
    FDK_Widget *prog = fdk_progress_bar(0.f);
    fdk_widget_set_size(prog, FDK_SIZE_FILL, 18);
    sp_ud.bar  = prog;

    FDK_Widget *row3 = fdk_hbox(10, 0);
    fdk_widget_set_size(row3, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    fdk_container_add(row3, fdk_label("Volume:"));
    FDK_Widget *spin = fdk_spinner(0.0, 100.0, 42.0, 1.0);
    fdk_spinner_set_decimals(spin, 0);
    fdk_widget_set_size(spin, 130, 36);
    FDK_Widget *badge = fdk_badge("42%");
    sp_ud.badge = badge;
    fdk_spinner_on_change(spin, on_spinner, &sp_ud);
    /* Fire once to initialise progress bar */
    fdk_progress_set_value(prog, 0.42f);
    fdk_container_add(row3, spin);
    fdk_container_add(row3, badge);
    fdk_container_add(page, row3);
    fdk_container_add(page, prog);
}

static void build_page_animation(FDK_Widget *page)
{
    fdk_container_add(page, fdk_label("Tween system (OUT_ELASTIC):"));

    FDK_Widget *bar = fdk_progress_bar(0.f);
    fdk_widget_set_size(bar, FDK_SIZE_FILL, 18);
    g_tween_bar = bar;
    fdk_container_add(page, bar);

    FDK_Widget *row = fdk_hbox(10, 0);
    fdk_widget_set_size(row, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    FDK_Widget *btn_anim = fdk_button("Animate");
    fdk_button_on_click(btn_anim, on_animate, NULL);
    fdk_widget_set_tooltip(btn_anim, "Launches an elastic tween on the bar");
    fdk_widget_set_size(btn_anim, 110, 36);
    fdk_container_add(row, btn_anim);
    fdk_container_add(page, row);

    fdk_container_add(page, fdk_separator());
    fdk_container_add(page, fdk_label("Toast notifications:"));

    FDK_Widget *row2 = fdk_hbox(10, 0);
    fdk_widget_set_size(row2, FDK_SIZE_FILL, FDK_SIZE_WRAP);

    FDK_Widget *b_warn = fdk_button("Warning");
    fdk_button_on_click(b_warn, on_notify_warn, NULL);
    fdk_widget_set_size(b_warn, 110, 36);
    fdk_container_add(row2, b_warn);

    FDK_Widget *b_err = fdk_button("Error");
    fdk_button_on_click(b_err, on_notify_error, NULL);
    fdk_widget_set_size(b_err, 110, 36);
    fdk_container_add(row2, b_err);

    fdk_container_add(page, row2);

    fdk_container_add(page, fdk_separator());
    fdk_container_add(page,
        fdk_label("Toasts also fire from File \xe2\x86\x92 New / Save / Quit."));
}

static void build_page_info(FDK_Widget *page)
{
    fdk_container_add(page, fdk_label("FDK v0.7.0 \xe2\x80\x94 Faded Dream Kit"));
    fdk_container_add(page, fdk_separator());
    fdk_container_add(page, fdk_label("Platform:   Linux (Wayland + X11)"));
    fdk_container_add(page, fdk_label("Renderer:   Software (Cairo-free)"));
    fdk_container_add(page, fdk_label("License:    Proprietary — see LICENSE"));
    fdk_container_add(page, fdk_label("Font:       FreeType (FTL)"));
    fdk_container_add(page, fdk_separator());
    fdk_container_add(page, fdk_label("New in v0.7.0:"));
    fdk_container_add(page, fdk_label("  \xe2\x80\xa2  FDK_Tabs  \xe2\x80\x94 tab bar with page switching"));
    fdk_container_add(page, fdk_label("  \xe2\x80\xa2  FDK_MenuBar  \xe2\x80\x94 drop-down menu system"));
    fdk_container_add(page, fdk_label("  \xe2\x80\xa2  fdk_notify()  \xe2\x80\x94 animated toast overlays"));
    fdk_container_add(page, fdk_label("  \xe2\x80\xa2  measure_widget hint override fix"));
}

/* ── Build the full UI ───────────────────────────────────────────────────── */
static FDK_Widget *build_ui(void)
{
    /* Outer vbox: no padding — menubar touches edges */
    FDK_Widget *root = fdk_vbox(0, 0);
    fdk_widget_set_size(root, FDK_SIZE_FILL, FDK_SIZE_FILL);

    /* ── Menu bar ── */
    FDK_Widget *mb = fdk_menubar();
    fdk_widget_set_size(mb, FDK_SIZE_FILL, 28);

    int file = fdk_menubar_add_menu(mb, "_File");
    fdk_menu_add_item(mb, file, "New",  "Ctrl+N", menu_new,  NULL);
    fdk_menu_add_item(mb, file, "Open", "Ctrl+O", menu_open, NULL);
    fdk_menu_add_item(mb, file, "Save", "Ctrl+S", menu_save, NULL);
    fdk_menu_add_separator(mb, file);
    fdk_menu_add_item(mb, file, "Quit", "Ctrl+Q", menu_quit, NULL);

    int edit = fdk_menubar_add_menu(mb, "_Edit");
    fdk_menu_add_item(mb, edit, "Undo", "Ctrl+Z", menu_undo, NULL);
    fdk_menu_add_item(mb, edit, "Redo", "Ctrl+Y", NULL,      NULL);
    fdk_menu_add_separator(mb, edit);
    fdk_menu_add_item(mb, edit, "Cut",   "Ctrl+X", NULL, NULL);
    fdk_menu_add_item(mb, edit, "Copy",  "Ctrl+C", NULL, NULL);
    fdk_menu_add_item(mb, edit, "Paste", "Ctrl+V", NULL, NULL);

    int help = fdk_menubar_add_menu(mb, "_Help");
    fdk_menu_add_item(mb, help, "About FDK", NULL, menu_about, NULL);

    fdk_container_add(root, mb);

    /* Inner vbox with padding for the content area */
    FDK_Widget *body = fdk_vbox(12, 16);
    fdk_widget_set_size(body, FDK_SIZE_FILL, FDK_SIZE_FILL);

    /* ── Tab widget ── */
    FDK_Widget *tabs = fdk_tabs();
    fdk_widget_set_size(tabs, FDK_SIZE_FILL, FDK_SIZE_FILL);

    FDK_Widget *p1 = fdk_tabs_add_page(tabs, "Controls");
    FDK_Widget *p2 = fdk_tabs_add_page(tabs, "Animation");
    FDK_Widget *p3 = fdk_tabs_add_page(tabs, "About");

    build_page_controls(p1);
    build_page_animation(p2);
    build_page_info(p3);

    fdk_container_add(body, tabs);

    /* ── Bottom bar ── */
    FDK_Widget *bot = fdk_hbox(10, 0);
    fdk_widget_set_size(bot, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    fdk_container_add(bot, fdk_label("FDK v0.7.0"));
    FDK_Widget *sp = fdk_label("");
    fdk_widget_set_size(sp, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    fdk_container_add(bot, sp);
    FDK_Widget *quit = fdk_button("Quit");
    fdk_button_on_click(quit, on_quit_btn, &g_running);
    fdk_widget_set_tooltip(quit, "Exit the application  (also in File menu)");
    fdk_container_add(bot, quit);
    fdk_container_add(body, bot);

    fdk_container_add(root, body);
    return root;
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    FDK_InitInfo init = {
        .platform = FDK_PLATFORM_AUTO,
        .render   = FDK_RENDER_SOFTWARE,
        .app_name = "FDK Showcase"
    };
    if (!fdk_init(&init)) return 1;

    FDK_WindowDesc wd = {
        .title     = "FDK v0.7.0 \xe2\x80\x94 Showcase",
        .x         = FDK_WINDOW_POS_CENTER,
        .y         = FDK_WINDOW_POS_CENTER,
        .w         = 580, .h = 520,
        .resizable = true
    };
    FDK_Window *win = fdk_window_create(&wd);
    if (!win) { fdk_shutdown(); return 1; }
    fdk_window_show(win);

    FDK_Theme theme  = fdk_theme_faded_dream();
    /* Auto-load system theme if present ($FDK_THEME_FILE or ~/.config/fdk/theme.fdktheme).
     * Fonts in the file override the fallback scan below. */
    fdk_theme_load_system(&theme);

    /* Fallback: scan for a font if theme file didn't specify one */
    if (!theme.font_body) {
        for (int i = 0; font_paths[i] && !theme.font_body; i++)
            theme.font_body = fdk_font_load(font_paths[i], 15);
    }
    if (!theme.font_label) theme.font_label = theme.font_body;
    if (!theme.font_mono)  theme.font_mono  = theme.font_body;

    g_ui = fdk_ui_create(win, &theme);
    FDK_Widget *root = build_ui();

    /* Hot-reload: watch theme file for live changes */
    const char *theme_file = getenv("FDK_THEME_FILE");
    if (!theme_file) {
        static char auto_path[512];
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        if (xdg)  snprintf(auto_path, sizeof auto_path, "%s/fdk/theme.fdktheme", xdg);
        else if (home) snprintf(auto_path, sizeof auto_path, "%s/.config/fdk/theme.fdktheme", home);
        /* Only watch if file exists */
        struct stat st;
        if (auto_path[0] && stat(auto_path, &st) == 0) theme_file = auto_path;
    }
    if (theme_file) fdk_theme_watch(g_ui, root, theme_file);

    /* Greet with a toast on startup */
    fdk_notify(g_ui, "Welcome to FDK v0.7.0!", FDK_NOTIFY_INFO, 3000);

    /* Wait for first EXPOSE/RESIZE to get real window dimensions */
    FDK_Event ev;
    bool got_size = false;
    while (!got_size) {
        fdk_wait_event(&ev);
        if (ev.type == FDK_EVENT_QUIT || ev.type == FDK_EVENT_CLOSE)
            goto done;
        if (ev.type == FDK_EVENT_RESIZE || ev.type == FDK_EVENT_EXPOSE) {
            got_size = true;
            fdk_ui_layout(g_ui, root);
            fdk_ui_paint(g_ui, root);
        }
    }

    while (g_running) {
        bool got = fdk_wait_event_timeout(&ev, 16);
        if (!got) ev.type = FDK_EVENT_NONE;
        if (ev.type == FDK_EVENT_QUIT  || ev.type == FDK_EVENT_CLOSE) break;
        if (ev.type == FDK_EVENT_KEY_DOWN) {
            /* Global Ctrl+Q / Ctrl+W to quit */
            if ((ev.key.mods & FDK_MOD_CTRL) &&
                (ev.key.key == 'q' || ev.key.key == 'w')) break;
        }
        fdk_ui_step(g_ui, root, &ev);
    }

done:
    fdk_ui_destroy(g_ui);
    fdk_widget_destroy(root);
    fdk_window_destroy(win);
    fdk_shutdown();
    return 0;
}
