/*
 * widgets.c — FDK widget showcase (updated for v0.7.0 fixes)
 *
 * Tests: toggle buttons, radio buttons, spinner+badge+progress,
 *        tween animation, tooltips, tag lookup.
 */
#include <fdk/fdk.h>
#include <fdk/fdk_widget.h>
#include <stdio.h>
#include <string.h>

#define TAG_STATUS   1
#define TAG_BADGE    2
#define TAG_PROGRESS 3

static const char *fonts[] = {
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    NULL
};

static bool g_running   = true;
static int  g_radio_sel = 0;

/* ── Callbacks ─────────────────────────────────────────────────────────── */
static void on_quit(FDK_Widget *w, void *ud)   { (void)w; *(bool*)ud = false; }

static void on_toggle(FDK_Widget *w, void *ud)
{
    FDK_Widget *status = fdk_widget_find((FDK_Widget*)ud, TAG_STATUS);
    if (status)
        fdk_label_set_text(status,
            fdk_toggle_button_get_active(w) ? "Status: enabled"
                                            : "Status: disabled");
}

static void on_radio(FDK_Widget *w, void *ud)
{
    (void)w;
    FDK_Widget *status = fdk_widget_find((FDK_Widget*)ud, TAG_STATUS);
    const char *names[] = { "Alpha", "Beta", "Gamma" };
    if (status) {
        char buf[64];
        snprintf(buf, sizeof buf, "Format: %s",
                 g_radio_sel < 3 ? names[g_radio_sel] : "?");
        fdk_label_set_text(status, buf);
    }
}

typedef struct { FDK_Widget *root; FDK_Widget *bar; } SpUD;
static void on_spinner(FDK_Widget *w, float v, void *ud)
{
    (void)w;
    SpUD *s = ud;
    fdk_progress_set_value(s->bar, v / 100.f);
    FDK_Widget *badge = fdk_widget_find(s->root, TAG_BADGE);
    if (badge) {
        char buf[12];
        snprintf(buf, sizeof buf, "%.0f%%", v);
        fdk_badge_set_text(badge, buf);
    }
}

static FDK_Widget *g_tween_bar = NULL;
static void tween_cb(float v, void *ud) {
    (void)ud;
    if (g_tween_bar) fdk_progress_set_value(g_tween_bar, v);
}
static void on_animate(FDK_Widget *w, void *ud) {
    (void)w; (void)ud;
    fdk_tween(0.f, 1.f, 1200, FDK_EASE_OUT_ELASTIC, tween_cb, NULL, NULL);
}

/* ── Build UI ──────────────────────────────────────────────────────────── */
static FDK_Widget *build_ui(void)
{
    FDK_Widget *root = fdk_vbox(12, 16);
    fdk_widget_set_size(root, FDK_SIZE_FILL, FDK_SIZE_FILL);

    /* ─ Section: toggle buttons ─ */
    fdk_container_add(root, fdk_label("Toggle buttons:"));
    FDK_Widget *row_tog = fdk_hbox(10, 0);
    fdk_widget_set_size(row_tog, FDK_SIZE_FILL, FDK_SIZE_WRAP);

    FDK_Widget *tb1 = fdk_toggle_button("Notifications", false);
    fdk_toggle_button_on_change(tb1, on_toggle, root);
    fdk_widget_set_tooltip(tb1, "Toggle desktop notifications");
    fdk_container_add(row_tog, tb1);

    FDK_Widget *tb2 = fdk_toggle_button("Auto-save", true);
    fdk_widget_set_tooltip(tb2, "Automatically save changes");
    fdk_container_add(row_tog, tb2);

    FDK_Widget *tb3 = fdk_toggle_button("Sync", false);
    fdk_widget_set_tooltip(tb3, "Enable cloud sync");
    fdk_container_add(row_tog, tb3);

    fdk_container_add(root, row_tog);
    fdk_container_add(root, fdk_separator());

    /* ─ Section: radio buttons ─ */
    fdk_container_add(root, fdk_label("Output format:"));
    FDK_Widget *row_rad = fdk_hbox(20, 0);
    fdk_widget_set_size(row_rad, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    const char *rlabels[] = { "Alpha", "Beta", "Gamma" };
    for (int i = 0; i < 3; i++) {
        FDK_Widget *rb = fdk_radio_button(rlabels[i], &g_radio_sel, i);
        fdk_radio_button_on_change(rb, on_radio, root);
        fdk_container_add(row_rad, rb);
    }
    fdk_container_add(root, row_rad);
    fdk_container_add(root, fdk_separator());

    /* ─ Section: spinner + progress + badge ─ */
    FDK_Widget *prog = fdk_progress_bar(0.f);
    fdk_widget_set_size(prog, FDK_SIZE_FILL, 18);
    fdk_widget_set_tag(prog, TAG_PROGRESS);

    static SpUD spud;
    spud.root = root;
    spud.bar  = prog;

    FDK_Widget *row_sp = fdk_hbox(12, 0);
    fdk_widget_set_size(row_sp, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    fdk_container_add(row_sp, fdk_label("Progress %:"));

    FDK_Widget *spin = fdk_spinner(0.0, 100.0, 0.0, 1.0);
    fdk_spinner_set_decimals(spin, 0);
    fdk_widget_set_size(spin, 140, 36);
    fdk_spinner_on_change(spin, on_spinner, &spud);
    fdk_widget_set_tooltip(spin, "Up/Down arrows or click \xe2\x96\xb2\xe2\x96\xbc to adjust");
    fdk_container_add(row_sp, spin);

    FDK_Widget *badge = fdk_badge("0%");
    fdk_widget_set_tag(badge, TAG_BADGE);
    fdk_container_add(row_sp, badge);

    fdk_container_add(root, row_sp);
    fdk_container_add(root, prog);
    fdk_container_add(root, fdk_separator());

    /* ─ Section: tween ─ */
    fdk_container_add(root, fdk_label("Tween  (OUT_ELASTIC easing):"));
    FDK_Widget *tbar = fdk_progress_bar(0.f);
    fdk_widget_set_size(tbar, FDK_SIZE_FILL, 18);
    g_tween_bar = tbar;
    fdk_container_add(root, tbar);

    FDK_Widget *row_anim = fdk_hbox(10, 0);
    fdk_widget_set_size(row_anim, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    FDK_Widget *btn_anim = fdk_button("Animate");
    fdk_button_on_click(btn_anim, on_animate, NULL);
    fdk_widget_set_tooltip(btn_anim, "Fire an elastic tween on the bar above");
    fdk_widget_set_size(btn_anim, 110, 36);
    fdk_container_add(row_anim, btn_anim);
    fdk_container_add(root, row_anim);
    fdk_container_add(root, fdk_separator());

    /* ─ Status / quit bar ─ */
    FDK_Widget *row_bot = fdk_hbox(12, 0);
    fdk_widget_set_size(row_bot, FDK_SIZE_FILL, FDK_SIZE_WRAP);

    FDK_Widget *status = fdk_label("Status: disabled");
    fdk_widget_set_tag(status, TAG_STATUS);
    fdk_container_add(row_bot, status);

    FDK_Widget *spacer = fdk_label("");
    fdk_widget_set_size(spacer, FDK_SIZE_FILL, FDK_SIZE_WRAP);
    fdk_container_add(row_bot, spacer);

    FDK_Widget *quit = fdk_button("Quit");
    fdk_button_on_click(quit, on_quit, &g_running);
    fdk_widget_set_tooltip(quit, "Close window");
    fdk_container_add(row_bot, quit);
    fdk_container_add(root, row_bot);

    return root;
}

int main(void)
{
    FDK_InitInfo init = { .platform = FDK_PLATFORM_AUTO,
                          .render   = FDK_RENDER_SOFTWARE,
                          .app_name = "FDK Widgets" };
    if (!fdk_init(&init)) return 1;

    FDK_WindowDesc wd = { .title = "FDK v0.7.0 \xe2\x80\x94 Widget Test",
                          .x = FDK_WINDOW_POS_CENTER,
                          .y = FDK_WINDOW_POS_CENTER,
                          .w = 520, .h = 500, .resizable = true };
    FDK_Window *win = fdk_window_create(&wd);
    if (!win) { fdk_shutdown(); return 1; }
    fdk_window_show(win);

    FDK_Font *font = NULL;
    for (int i = 0; fonts[i] && !font; i++)
        font = fdk_font_load(fonts[i], 15);

    FDK_Theme theme = fdk_theme_faded_dream();
    theme.font_body = theme.font_label = font;

    FDK_UI     *ui   = fdk_ui_create(win, &theme);
    FDK_Widget *root = build_ui();

    FDK_Event ev;
    bool got = false;
    while (!got) {
        fdk_wait_event(&ev);
        if (ev.type == FDK_EVENT_QUIT || ev.type == FDK_EVENT_CLOSE) goto done;
        if (ev.type == FDK_EVENT_RESIZE || ev.type == FDK_EVENT_EXPOSE) {
            got = true;
            fdk_ui_layout(ui, root);
            fdk_ui_paint(ui, root);
        }
    }
    while (g_running) {
        bool ok = fdk_wait_event_timeout(&ev, 16);
        if (!ok) ev.type = FDK_EVENT_NONE;
        if (ev.type == FDK_EVENT_QUIT || ev.type == FDK_EVENT_CLOSE) break;
        if (ev.type == FDK_EVENT_KEY_DOWN && ev.key.key == FDK_KEY_ESCAPE) break;
        fdk_ui_step(ui, root, &ev);
    }
done:
    fdk_ui_destroy(ui);
    fdk_widget_destroy(root);
    fdk_font_destroy(font);
    fdk_window_destroy(win);
    fdk_shutdown();
    return 0;
}
