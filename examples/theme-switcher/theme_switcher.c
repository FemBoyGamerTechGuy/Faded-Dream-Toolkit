/*
 * theme_switcher.c — FDK v0.0.1 visual theme test
 *
 * Shows every widget type at once so theme differences are immediately
 * visible: buttons (including accent/danger/ghost variants), toggle,
 * radio, checkbox, slider, progress bar, spinner, badge, dropdown,
 * text input, separator, tabs, menubar.
 *
 * A radio group switches between the three bundled themes at runtime
 * via fdk_ui_set_theme(). A "Watch live" toggle starts/stops
 * fdk_theme_watch() on /tmp/fdk_live_theme.fdktheme — edit that file
 * while the demo is running and the whole UI updates within one frame.
 *
 * Run:
 *   ./fdk_theme_switcher                 — uses themes/ next to the binary
 *   ./fdk_theme_switcher /path/to/themes — explicit themes directory
 */
#include <fdk/fdk.h>
#include <fdk/fdk_widget.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Globals ──────────────────────────────────────────────────────────── */
static bool        g_running    = true;
static FDK_UI     *g_ui         = NULL;
static FDK_Widget *g_root       = NULL;
static int         g_theme_sel  = 0;   /* radio group: 0=faded-dream 1=void 2=rose */
static char        g_themes_dir[256] = "themes";
static const char *LIVE_PATH    = "/tmp/fdk_live_theme.fdktheme";

static const char *font_paths[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    NULL
};

/* ── Theme switching ──────────────────────────────────────────────────── */
static void apply_theme_file(const char *filename)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_themes_dir, filename);

    FDK_Theme t = fdk_theme_faded_dream();  /* sane base for unset fields */
    bool ok = fdk_theme_load(&t, path);
    if (!ok) {
        fprintf(stderr, "[theme-switcher] failed to load %s\n", path);
        fdk_notify(g_ui, "Theme file not found", FDK_NOTIFY_ERROR, 0);
        return;
    }
    fdk_ui_set_theme(g_ui, g_root, &t);

    char msg[128];
    snprintf(msg, sizeof msg, "Theme: %s", filename);
    fdk_notify(g_ui, msg, FDK_NOTIFY_INFO, 1500);
}

static void on_theme_radio(FDK_Widget *w, void *ud)
{
    (void)w; (void)ud;
    switch (g_theme_sel) {
    case 0: apply_theme_file("faded-dream.fdktheme"); break;
    case 1: apply_theme_file("void.fdktheme");        break;
    case 2: apply_theme_file("rose.fdktheme");         break;
    }
}

/* ── Hot-reload toggle ────────────────────────────────────────────────── */
static void on_watch_toggle(FDK_Widget *w, void *ud)
{
    (void)ud;
    bool active = fdk_toggle_button_get_active(w);
    if (active) {
        /* Seed the live file with a copy of the currently-selected
         * bundled theme so editing has a sane starting point. */
        const char *src_name =
            g_theme_sel == 0 ? "faded-dream.fdktheme" :
            g_theme_sel == 1 ? "void.fdktheme" : "rose.fdktheme";
        char src[512];
        snprintf(src, sizeof src, "%s/%s", g_themes_dir, src_name);

        struct stat st;
        if (stat(LIVE_PATH, &st) != 0) {
            /* Only seed if it doesn't already exist — don't clobber edits */
            FILE *in = fopen(src, "r");
            FILE *out = in ? fopen(LIVE_PATH, "w") : NULL;
            if (in && out) {
                char buf[4096]; size_t n;
                while ((n = fread(buf, 1, sizeof buf, in)) > 0)
                    fwrite(buf, 1, n, out);
            }
            if (in)  fclose(in);
            if (out) fclose(out);
        }

        fdk_theme_watch(g_ui, g_root, LIVE_PATH);
        fdk_notify(g_ui, "Watching /tmp/fdk_live_theme.fdktheme \xe2\x80\x94 edit it!",
                   FDK_NOTIFY_SUCCESS, 3000);
    } else {
        fdk_theme_watch(g_ui, g_root, NULL);  /* stop watching */
        fdk_notify(g_ui, "Stopped watching live theme", FDK_NOTIFY_INFO, 1500);
    }
}

/* ── Misc callbacks ───────────────────────────────────────────────────── */
static void on_quit(FDK_Widget *w, void *ud) { (void)w; *(bool*)ud = false; }

static void on_notify_success(FDK_Widget *w, void *ud)
{ (void)w; (void)ud; fdk_notify(g_ui, "Success toast", FDK_NOTIFY_SUCCESS, 0); }
static void on_notify_warning(FDK_Widget *w, void *ud)
{ (void)w; (void)ud; fdk_notify(g_ui, "Warning toast", FDK_NOTIFY_WARNING, 0); }
static void on_notify_error(FDK_Widget *w, void *ud)
{ (void)w; (void)ud; fdk_notify(g_ui, "Error toast", FDK_NOTIFY_ERROR, 0); }

static FDK_Widget *g_tween_bar = NULL;
static void tween_cb(float v, void *ud) { (void)ud; fdk_progress_set_value(g_tween_bar, v); }
static void on_animate(FDK_Widget *w, void *ud)
{ (void)w; (void)ud; fdk_tween(0.f, 1.f, 900, FDK_EASE_OUT_ELASTIC, tween_cb, NULL, NULL); }

static void on_spinner(FDK_Widget *w, float v, void *ud)
{ (void)w; fdk_progress_set_value((FDK_Widget*)ud, v / 100.f); }

/* ── Build UI ─────────────────────────────────────────────────────────── */
static FDK_Widget *build_ui(void)
{
    FDK_Widget *root = fdk_vbox(14, 20);
    fdk_widget_set_size(root, FDK_SIZE_FILL, FDK_SIZE_FILL);

    /* ── Title + theme switcher row ── */
    fdk_container_add(root, fdk_label("FDK v0.0.1 \xe2\x80\x94 Theme Switcher"));

    FDK_Widget *row_theme = fdk_hbox(16, 0);
    fdk_widget_set_size(row_theme, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    fdk_container_add(row_theme, fdk_label("Theme:"));

    const char *theme_labels[] = { "Faded Dream", "Void", "Ros\xc3\xa9" };
    for (int i = 0; i < 3; i++) {
        FDK_Widget *rb = fdk_radio_button(theme_labels[i], &g_theme_sel, i);
        fdk_radio_button_on_change(rb, on_theme_radio, NULL);
        fdk_container_add(row_theme, rb);
    }

    FDK_Widget *spacer1 = fdk_label("");
    fdk_widget_set_size(spacer1, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    fdk_container_add(row_theme, spacer1);

    FDK_Widget *watch_toggle = fdk_toggle_button("Watch live", false);
    fdk_toggle_button_on_change(watch_toggle, on_watch_toggle, NULL);
    fdk_widget_set_tooltip(watch_toggle,
        "Hot-reload /tmp/fdk_live_theme.fdktheme via inotify");
    fdk_container_add(row_theme, watch_toggle);

    fdk_container_add(root, row_theme);
    fdk_container_add(root, fdk_separator());

    /* ── Buttons: default + variants ── */
    fdk_container_add(root, fdk_label("Buttons (variants from [button.*] sections):"));
    FDK_Widget *row_btn = fdk_hbox(10, 0);
    fdk_widget_set_size(row_btn, FDK_SIZE_FILL, FDK_SIZE_WRAP);

    FDK_Widget *btn_default = fdk_button("Default");
    fdk_widget_set_size(btn_default, 110, 36);
    fdk_container_add(row_btn, btn_default);

    FDK_Widget *btn_accent = fdk_button("Accent");
    fdk_widget_set_variant(btn_accent, "accent");
    fdk_widget_set_size(btn_accent, 110, 36);
    fdk_container_add(row_btn, btn_accent);

    FDK_Widget *btn_danger = fdk_button("Danger");
    fdk_widget_set_variant(btn_danger, "danger");
    fdk_widget_set_size(btn_danger, 110, 36);
    fdk_container_add(row_btn, btn_danger);

    FDK_Widget *btn_ghost = fdk_button("Ghost");
    fdk_widget_set_variant(btn_ghost, "ghost");
    fdk_widget_set_size(btn_ghost, 110, 36);
    fdk_container_add(row_btn, btn_ghost);

    fdk_container_add(root, row_btn);
    fdk_container_add(root, fdk_separator());

    /* ── Toggle / checkbox / slider row ── */
    FDK_Widget *row_inputs = fdk_hbox(16, 0);
    fdk_widget_set_size(row_inputs, FDK_SIZE_FILL, FDK_SIZE_WRAP);

    FDK_Widget *tb = fdk_toggle_button("Notifications", true);
    fdk_container_add(row_inputs, tb);

    FDK_Widget *cb = fdk_checkbox("Remember me", true);
    fdk_container_add(row_inputs, cb);

    FDK_Widget *slider = fdk_slider(0.f, 100.f, 60.f);
    fdk_widget_set_size(slider, 160, FDK_SIZE_WRAP);
    fdk_container_add(row_inputs, slider);

    fdk_container_add(root, row_inputs);
    fdk_container_add(root, fdk_separator());

    /* ── Spinner + progress + badge ── */
    FDK_Widget *progress = fdk_progress_bar(0.42f);
    fdk_widget_set_size(progress, FDK_SIZE_FILL, 18);

    FDK_Widget *row_spin = fdk_hbox(12, 0);
    fdk_widget_set_size(row_spin, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    fdk_container_add(row_spin, fdk_label("Value:"));

    FDK_Widget *spinner = fdk_spinner(0.0, 100.0, 42.0, 1.0);
    fdk_spinner_set_decimals(spinner, 0);
    fdk_widget_set_size(spinner, 130, 36);
    fdk_spinner_on_change(spinner, on_spinner, progress);
    fdk_container_add(row_spin, spinner);

    FDK_Widget *badge1 = fdk_badge("New");
    fdk_container_add(row_spin, badge1);

    FDK_WidgetStyle badge_style = {0};
    badge_style.bg = FDK_RGB(220, 70, 70);
    badge_style.fg = FDK_RGB(255, 255, 255);
    badge_style.has_bg = badge_style.has_fg = true;
    FDK_Widget *badge2 = fdk_badge("12");
    fdk_widget_set_style(badge2, &badge_style);
    fdk_container_add(row_spin, badge2);

    fdk_container_add(root, row_spin);
    fdk_container_add(root, progress);
    fdk_container_add(root, fdk_separator());

    /* ── Dropdown + text input ── */
    FDK_Widget *row_dd = fdk_hbox(12, 0);
    fdk_widget_set_size(row_dd, FDK_SIZE_FILL, FDK_SIZE_WRAP);

    FDK_Widget *dropdown = fdk_dropdown("Select option");
    fdk_dropdown_add_item(dropdown, "Option A");
    fdk_dropdown_add_item(dropdown, "Option B");
    fdk_dropdown_add_item(dropdown, "Option C");
    fdk_widget_set_size(dropdown, 160, 36);
    fdk_container_add(row_dd, dropdown);

    FDK_Widget *input = fdk_text_input("Type something...");
    fdk_widget_set_size(input, FDK_SIZE_FILL, 36);
    fdk_container_add(row_dd, input);

    fdk_container_add(root, row_dd);
    fdk_container_add(root, fdk_separator());

    /* ── Tween demo ── */
    fdk_container_add(root, fdk_label("Tween (OUT_ELASTIC):"));
    FDK_Widget *tween_bar = fdk_progress_bar(0.f);
    fdk_widget_set_size(tween_bar, FDK_SIZE_FILL, 18);
    g_tween_bar = tween_bar;
    fdk_container_add(root, tween_bar);

    FDK_Widget *row_toast = fdk_hbox(10, 0);
    fdk_widget_set_size(row_toast, FDK_SIZE_FILL, FDK_SIZE_WRAP);

    FDK_Widget *btn_anim = fdk_button("Animate");
    fdk_button_on_click(btn_anim, on_animate, NULL);
    fdk_widget_set_size(btn_anim, 100, 36);
    fdk_container_add(row_toast, btn_anim);

    FDK_Widget *btn_succ = fdk_button("Toast: Success");
    fdk_widget_set_variant(btn_succ, "accent");
    fdk_button_on_click(btn_succ, on_notify_success, NULL);
    fdk_widget_set_size(btn_succ, 140, 36);
    fdk_container_add(row_toast, btn_succ);

    FDK_Widget *btn_warn = fdk_button("Toast: Warning");
    fdk_button_on_click(btn_warn, on_notify_warning, NULL);
    fdk_widget_set_size(btn_warn, 140, 36);
    fdk_container_add(row_toast, btn_warn);

    FDK_Widget *btn_err = fdk_button("Toast: Error");
    fdk_widget_set_variant(btn_err, "danger");
    fdk_button_on_click(btn_err, on_notify_error, NULL);
    fdk_widget_set_size(btn_err, 130, 36);
    fdk_container_add(row_toast, btn_err);

    fdk_container_add(root, row_toast);
    fdk_container_add(root, fdk_separator());

    /* ── Bottom row ── */
    FDK_Widget *row_bot = fdk_hbox(12, 0);
    fdk_widget_set_size(row_bot, FDK_SIZE_FILL, FDK_SIZE_WRAP);

    fdk_container_add(row_bot, fdk_label(
        "[button.accent] [button.danger] [button.ghost] all from theme file"));

    FDK_Widget *spacer2 = fdk_label("");
    fdk_widget_set_size(spacer2, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    fdk_container_add(row_bot, spacer2);

    FDK_Widget *btn_quit = fdk_button("Quit");
    fdk_button_on_click(btn_quit, on_quit, &g_running);
    fdk_widget_set_size(btn_quit, 90, 36);
    fdk_container_add(row_bot, btn_quit);

    fdk_container_add(root, row_bot);

    return root;
}

/* ── Main ─────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    bool use_auto_resolve = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--auto") == 0) use_auto_resolve = true;
        else snprintf(g_themes_dir, sizeof g_themes_dir, "%s", argv[i]);
    }

    /* app_name kept as a single clean token (no spaces) since it
     * doubles as the filename FDK looks for at
     * ~/.FDKthemes/overrides/<app_name> for tier-2 per-app overrides. */
    FDK_InitInfo init = {
        .platform = FDK_PLATFORM_AUTO,
        .render   = FDK_RENDER_SOFTWARE,
        .app_name = "fdk-theme-switcher"
    };
    if (!fdk_init(&init)) return 1;

    FDK_WindowDesc wd = {
        .title     = "FDK v0.0.1 \xe2\x80\x94 Theme Switcher",
        .x         = FDK_WINDOW_POS_CENTER,
        .y         = FDK_WINDOW_POS_CENTER,
        .w         = 760, .h = 600,
        .resizable = true
    };
    FDK_Window *win = fdk_window_create(&wd);
    if (!win) { fdk_shutdown(); return 1; }
    fdk_window_show(win);

    FDK_Theme theme;
    char auto_resolved_path[512] = {0};
    if (use_auto_resolve) {
        /* --auto demonstrates the zero-code path: fdk_theme_resolve_ex()
         * is the exact same three-tier logic (developer force -> user
         * per-app override at ~/.FDKthemes/overrides/fdk-theme-switcher
         * -> system-wide ~/.FDKthemes/theme.fdktheme) that
         * fdk_ui_create(win, NULL) would run internally — the _ex
         * variant additionally reports which file backed the result,
         * which this demo reuses below to start its own hot-reload
         * watch. A real app that doesn't need either the font fallback
         * or the resolved-path string could skip all of this and just
         * call fdk_ui_create(win, NULL) directly — that one call alone
         * gets the same resolution AND automatic hot-reload watching,
         * with zero other code required. */
        theme = fdk_theme_resolve_ex("fdk-theme-switcher",
                                      auto_resolved_path, sizeof auto_resolved_path);
        fprintf(stderr, "[theme-switcher] --auto: resolved via three-tier system "
                        "(see ~/.FDKthemes)\n");
    } else {
        /* Default: explicit load from the bundled themes/ directory,
         * so the demo also proves fdk_theme_load() end-to-end on its
         * own bundled files regardless of what (if anything) exists
         * under ~/.FDKthemes on the machine running it. */
        theme = fdk_theme_faded_dream();
        char init_path[512];
        snprintf(init_path, sizeof init_path, "%s/faded-dream.fdktheme", g_themes_dir);
        if (!fdk_theme_load(&theme, init_path))
            fprintf(stderr, "[theme-switcher] warning: %s not found, using C defaults\n",
                    init_path);
    }

    if (!theme.font_body) {
        for (int i = 0; font_paths[i] && !theme.font_body; i++)
            theme.font_body = fdk_font_load(font_paths[i], 15);
    }
    if (!theme.font_label) theme.font_label = theme.font_body;
    if (!theme.font_mono)  theme.font_mono  = theme.font_body;

    /* Passing the resolved-and-font-patched theme explicitly here (for
     * both branches) means fdk_ui_create() won't auto-start a watch of
     * its own — it only does that when theme == NULL, since an
     * explicitly-supplied theme is itself a form of opting out of the
     * automatic system. So in --auto mode we start the watch
     * ourselves, pointed at the same file fdk_theme_resolve() actually
     * used, to keep the demo's hot-reload behaviour identical either
     * way. A real app that just wants the fully automatic behaviour —
     * including auto-watch — should call fdk_ui_create(win, NULL)
     * directly instead of pre-resolving like this demo does. */
    g_ui = fdk_ui_create(win, &theme);
    if (use_auto_resolve && auto_resolved_path[0])
        fdk_theme_watch(g_ui, NULL, auto_resolved_path);
    g_root = build_ui();

    /* Wait for first EXPOSE/RESIZE for real window dimensions */
    FDK_Event ev;
    bool got_size = false;
    while (!got_size) {
        fdk_wait_event(&ev);
        if (ev.type == FDK_EVENT_QUIT || ev.type == FDK_EVENT_CLOSE) goto done;
        if (ev.type == FDK_EVENT_RESIZE || ev.type == FDK_EVENT_EXPOSE) {
            got_size = true;
            fdk_ui_layout(g_ui, g_root);
            fdk_ui_paint(g_ui, g_root);
        }
    }

    fdk_notify(g_ui, "Pick a theme on the left to switch live", FDK_NOTIFY_INFO, 3000);

    while (g_running) {
        bool got = fdk_wait_event_timeout(&ev, 16);
        if (!got) ev.type = FDK_EVENT_NONE;
        if (ev.type == FDK_EVENT_QUIT || ev.type == FDK_EVENT_CLOSE) break;
        if (ev.type == FDK_EVENT_KEY_DOWN && ev.key.key == FDK_KEY_ESCAPE) break;
        fdk_ui_step(g_ui, g_root, &ev);
    }

done:
    fdk_theme_watch(g_ui, g_root, NULL);  /* stop any active watch thread */
    fdk_ui_destroy(g_ui);
    fdk_widget_destroy(g_root);
    fdk_window_destroy(win);
    fdk_shutdown();
    return 0;
}
