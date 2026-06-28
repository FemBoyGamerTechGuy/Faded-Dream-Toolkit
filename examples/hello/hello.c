/*
 * hello.c — FDK full widget demo
 */
#include <fdk/fdk.h>
#include <fdk/fdk_widget.h>
#include <stdio.h>
#include <string.h>

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

/* Callbacks */
static void on_quit   (FDK_Widget *w, void *ud) { (void)w; *(bool*)ud = false; }
static void on_greet  (FDK_Widget *w, void *ud) { (void)w; fdk_label_set_text((FDK_Widget*)ud, "Hello from FDK!"); }
static void on_clear  (FDK_Widget *w, void *ud) { (void)w; fdk_text_input_set_text((FDK_Widget*)ud, ""); }
static void on_toggle (FDK_Widget *w, void *ud) { (void)ud; printf("[checkbox] %s\n", fdk_checkbox_get_checked(w) ? "on":"off"); }
typedef struct { FDK_Widget *label; FDK_Widget *progress; } SliderUD;
static SliderUD g_slider_ud;

static void on_slider(FDK_Widget *w, float v, void *ud)
{
    (void)w;
    SliderUD *s = (SliderUD*)ud;
    char buf[32]; snprintf(buf, sizeof buf, "Slider: %.2f", v);
    fdk_label_set_text(s->label, buf);
    fdk_progress_set_value(s->progress, v);
}
static void on_dropdown(FDK_Widget *w, int idx, const char *txt, void *ud)
{ (void)w;(void)ud; printf("[dropdown] selected %d: %s\n", idx, txt); }

int main(void)
{
    FDK_InitInfo init = { .platform=FDK_PLATFORM_AUTO,
                          .render=FDK_RENDER_SOFTWARE, .app_name="FDK Demo" };
    if (!fdk_init(&init)) return 1;

    FDK_WindowDesc wd = {
        .title="Faded Dream Kit \xe2\x80\x94 Widget Demo",
        .x=FDK_WINDOW_POS_CENTER, .y=FDK_WINDOW_POS_CENTER,
        .w=520, .h=560, .resizable=true
    };
    FDK_Window *win = fdk_window_create(&wd);
    if (!win) { fdk_shutdown(); return 1; }
    fdk_window_show(win);

    FDK_Font *font = NULL;
    for (int i = 0; font_paths[i] && !font; i++)
        font = fdk_font_load(font_paths[i], 15);

    FDK_Theme theme  = fdk_theme_faded_dream();
    theme.font_body  = font;
    theme.font_label = font;

    /* ── Widget tree ── */
    FDK_Widget *root = fdk_vbox(0, 0);
    FDK_Widget *body = fdk_vbox(10, 20);
    fdk_widget_set_size(body, FDK_SIZE_FILL, FDK_SIZE_FILL);
    fdk_container_add(root, body);

    /* Title */
    FDK_Widget *title = fdk_label("Faded Dream Kit");
    fdk_label_set_color(title, FDK_RGB(138, 99, 210));
    fdk_container_add(body, title);

    fdk_container_add(body, fdk_separator());

    /* Output label */
    FDK_Widget *out = fdk_label("Press Greet or move the slider.");
    fdk_container_add(body, out);

    /* Text input row */
    FDK_Widget *row1 = fdk_hbox(8, 0);
    fdk_widget_set_size(row1, FDK_SIZE_FILL, 36);
    fdk_container_add(body, row1);
    FDK_Widget *input = fdk_text_input("Type something...");
    fdk_widget_set_size(input, FDK_SIZE_FILL, 36);
    fdk_container_add(row1, input);
    FDK_Widget *clear_btn = fdk_button("Clear");
    fdk_widget_set_size(clear_btn, 72, 36);
    fdk_button_on_click(clear_btn, on_clear, input);
    fdk_container_add(row1, clear_btn);

    /* Checkbox */
    FDK_Widget *chk = fdk_checkbox("Enable something cool", false);
    fdk_checkbox_on_change(chk, on_toggle, NULL);
    fdk_container_add(body, chk);

    fdk_container_add(body, fdk_separator());

    /* Slider */
    FDK_Widget *slider_lbl = fdk_label("Slider: 0.50");
    fdk_container_add(body, slider_lbl);
    FDK_Widget *slider = fdk_slider(0.f, 1.f, 0.5f);
    fdk_widget_set_size(slider, FDK_SIZE_FILL, 28);
    fdk_slider_on_change(slider, on_slider, &g_slider_ud); /* ud set after prog */
    fdk_container_add(body, slider);

    /* Progress bar */
    FDK_Widget *prog_lbl = fdk_label("Progress bar:");
    fdk_container_add(body, prog_lbl);
    FDK_Widget *prog = fdk_progress_bar(0.65f);
    fdk_widget_set_size(prog, FDK_SIZE_FILL, 8);
    fdk_container_add(body, prog);
    /* Now both slider_lbl and prog exist — wire up the slider callback data */
    g_slider_ud.label    = slider_lbl;
    g_slider_ud.progress = prog;

    FDK_Widget *prog_indet = fdk_progress_bar(0.f);
    fdk_progress_set_indeterminate(prog_indet, true);
    fdk_widget_set_size(prog_indet, FDK_SIZE_FILL, 8);
    fdk_container_add(body, prog_indet);

    fdk_container_add(body, fdk_separator());

    /* Dropdown */
    FDK_Widget *dd_lbl = fdk_label("Dropdown:");
    fdk_container_add(body, dd_lbl);
    FDK_Widget *dd = fdk_dropdown("Choose an option...");
    fdk_widget_set_size(dd, FDK_SIZE_FILL, 36);
    fdk_dropdown_add_item(dd, "Wayland");
    fdk_dropdown_add_item(dd, "X11");
    fdk_dropdown_add_item(dd, "Software render");
    fdk_dropdown_add_item(dd, "OpenGL render");
    fdk_dropdown_on_change(dd, on_dropdown, NULL);
    fdk_container_add(body, dd);

    fdk_container_add(body, fdk_separator());

    /* Button row */
    FDK_Widget *btn_row = fdk_hbox(8, 0);
    fdk_widget_set_size(btn_row, FDK_SIZE_FILL, 36);
    fdk_container_add(body, btn_row);
    FDK_Widget *greet = fdk_button("Greet");
    fdk_widget_set_size(greet, FDK_SIZE_FILL, 36);
    fdk_button_on_click(greet, on_greet, out);
    fdk_container_add(btn_row, greet);
    FDK_Widget *quit = fdk_button("Quit");
    fdk_widget_set_size(quit, FDK_SIZE_FILL, 36);
    fdk_container_add(btn_row, quit);

    FDK_UI *ui   = fdk_ui_create(win, &theme);
    bool running = true;
    fdk_button_on_click(quit, on_quit, &running);

    FDK_Event ev;
    bool got_size = false;
    while (!got_size) {
        fdk_wait_event(&ev);
        if (ev.type == FDK_EVENT_QUIT || ev.type == FDK_EVENT_CLOSE) goto done;
        if (ev.type == FDK_EVENT_RESIZE || ev.type == FDK_EVENT_EXPOSE) {
            got_size = true;
            /* Layout with real compositor size before first paint */
            fdk_ui_layout(ui, root);
            fdk_ui_paint(ui, root);
            fdk_ui_reset_blink(ui);
        }
    }

    while (running) {
        /* Wait up to 16ms — keeps blink ticking without blocking compositor */
        bool got = fdk_wait_event_timeout(&ev, 16);
        if (!got) ev.type = FDK_EVENT_NONE;
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
