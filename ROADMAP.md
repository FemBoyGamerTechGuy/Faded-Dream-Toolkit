# FDK — Faded Dream Kit — Development Roadmap

> **Current version: v0.0.1 — NOT READY FOR USE**
> Version 0.x.x means the toolkit is in active development and is not
> considered stable or complete. Do not build production apps on it yet.

Platform: Linux (Wayland + X11) | Language: C11 | License: Proprietary (see LICENSE)

---

## Version scheme

| Version | Meaning |
|---|---|
| 0.x.x | Under construction — APIs may change, features missing, not for general use |
| 1.0.0 | First stable release — safe to build apps on, API considered settled |
| 1.x.x | Stable — new features and fixes, no breaking changes within major version |

---

## ✅ Implemented and confirmed working

### Core / Platform
- [x] **Wayland backend** — `wl_surface`, `xdg_toplevel`, `wl_seat`, input events
- [x] **X11 backend** — Xlib, XImage, MIT-SHM shared memory, XKB keyboard
- [x] **Software renderer** — CPU pixel buffer, clip stack, rounded rects, circles, gradients, box-blur shadows
- [x] **OpenGL renderer** — GL 3.3 core profile, batch vertex buffer, FreeType glyph atlas, GLX (X11) + EGL (Wayland)
- [x] **Clipboard** — Wayland `wl_data_device`, X11 selection protocol — `Ctrl+C/V/X` wired into text inputs
- [x] **Tween / animation engine** — `fdk_tween()`, easing functions, per-frame callbacks
- [x] **Font loading** — FreeType, recursive `/usr/share/fonts/` scan by name, absolute path fallback
- [x] **inotify hot-reload** — background pthread watches `.fdktheme` files or `fdk.conf` for live updates

### Widgets (19 types)
- [x] Container (hbox / vbox, FILL / WRAP sizing, gap)
- [x] Label
- [x] Button (with variant support: accent, danger, ghost, custom)
- [x] Text input (cursor, selection, Ctrl shortcuts)
- [x] TextArea (multiline, scrollable, read-only mode)
- [x] Checkbox
- [x] Toggle button (with `[toggle.on]` variant)
- [x] Radio button (group-exclusive selection)
- [x] Slider (continuous, draggable thumb)
- [x] Progress bar (determinate + indeterminate)
- [x] Spinner (numeric up/down input)
- [x] Dropdown (single-select popup)
- [x] Badge (count/status pill)
- [x] Separator
- [x] Scroll view
- [x] Image
- [x] Tabs (horizontal tab bar + pane switcher)
- [x] MenuBar (top-of-window, Alt+key mnemonics, keyboard navigation)
- [x] Context menu (right-click popup, `fdk_widget_set_context_menu()` auto-wires)

### Theme system
- [x] **`.fdktheme` file format** — variables, per-widget sections, named variants, gradients, shadows, fonts, inline comments
- [x] **`fdk_theme_load(theme, path)`** — full parser, unknown keys silently ignored (forward-compatible)
- [x] **`fdk_theme_force()` / `fdk_theme_force_file()`** — tier 1: hard developer override, bypasses everything
- [x] **`~/.FDKthemes/overrides/<app_name>`** — tier 2: user per-app override file, no developer code needed
- [x] **`~/.config/FDK/fdk.conf`** — records active theme name, read by all FDK apps on startup
- [x] **`~/.FDKthemes/theme.fdktheme`** — tier 3: direct file fallback (backward-compat, works without fdk.conf)
- [x] **`fdk_ui_create(win, NULL)`** — auto-resolves all three tiers, auto-starts hot-reload watch
- [x] **`fdk_theme_watch()`** — watches a specific `.fdktheme` file for content changes
- [x] **`fdk_theme_watch_conf()`** — watches `~/.config/FDK/fdk.conf`, re-resolves on `fdk-theme set`
- [x] **`fdk_widget_set_variant(w, "name")`** — maps to `[widget.name]` in theme file
- [x] **`fdk_widget_set_style(w, &style)`** — per-widget programmatic override, highest priority
- [x] **`resolve_style()` wired into all 19 widget paint cases** — bg, fg, border, radius, padding all driven by theme file
- [x] **Three bundled themes** — `faded-dream.fdktheme`, `void.fdktheme`, `rose.fdktheme`

### Tools
- [x] **`fdk-theme` CLI** — `list`, `set <name>`, `set --app <app> <name>`, `unset --app`, `show`, `show --app`
  - Themes live in `~/.FDKthemes/` as individual named files — user drops them in, picks one by name
  - Active theme recorded in `~/.config/FDK/fdk.conf` (plain text, editable by hand)
  - Running FDK apps re-theme within ~1 second via `fdk_theme_watch_conf()`
  - No themes auto-install, auto-copy, or write anywhere without user action

### Build / Packaging
- [x] **CMake build system** — `FDK_BUILD_SHARED`, `FDK_BUILD_STATIC`, `FDK_BUILD_TOOLS`, `FDK_BUILD_EXAMPLES`, `FDK_BUILD_TESTS`
- [x] **CMake install target** — installs library, headers, `fdk-theme` binary, `.pc` file — themes NOT installed (user-managed)
- [x] **pkg-config** — `fdk.pc` generated at build time
- [x] **Packaging recipes** — `packaging/PKGBUILD` (Arch), `packaging/fdk.spec` (RPM), `packaging/debian/` (Debian/Ubuntu)
- [x] **Headless test suite** (`tests/test_theme.c`) — 49+ assertions, runs via `ctest`, no window/compositor needed

---

## 🔶 Needed before v1.0.0

These are the gaps that currently make FDK apps feel incomplete or broken
to a normal user. All must be done before FDK can be called ready for
building real apps.

- [ ] **Key repeat** — holding a key in a text input should repeat after a
  short delay. Currently each physical key event fires once and stops.
  Wayland: `wl_keyboard` repeat info + timer. X11: auto-repeat handling.

- [ ] **Window decorations and state** — minimize, maximize, fullscreen.
  Currently FDK creates a bare surface with no way for the user to minimize
  or maximize through standard WM means. Wayland: handle `xdg_toplevel`
  configure events for `maximized`, `fullscreen`, `activated` states.
  Also needed: server-side vs client-side decoration negotiation
  (`xdg-decoration` protocol).

- [ ] **Multiple windows / dialogs** — creating a second window from within
  a running app (about dialog, settings panel, file picker) is untested and
  formally unsupported. Needs proper multi-window support and per-UI watch
  contexts (replace single global `g_watch` with a dynamic list, each with
  its own inotify fd + pthread).

- [ ] **File dialogs** — open/save file pickers. One of the most commonly
  needed features in any real app. Normally done via XDG portal (D-Bus)
  which FDK deliberately avoids. Needs a custom implementation: either a
  built-in FDK file picker widget or a lightweight external helper binary
  that communicates via stdin/stdout with no D-Bus dependency.

- [ ] **App window icon** — setting the icon that appears in the taskbar.
  On Wayland: `xdg_toplevel_icon` protocol. Without it all FDK apps show
  as blank/generic in Hyprland's taskbar.

- [ ] **Drag and drop** — X11 XDND protocol, Wayland `wl_data_device`.
  Needed for file managers, media players, anything that accepts dropped
  files from outside the app.

- [ ] **Font discovery improvements** — current scan works for fonts in
  `/usr/share/fonts/` but misses user-installed fonts in
  `~/.local/share/fonts/`. Needs to also scan `$XDG_DATA_HOME/fonts`.
  No fontconfig dependency — keep the custom scanner, extend its coverage.

- [ ] **Test coverage for `fdk-theme` CLI** — currently verified by hand
  only. Needs a permanent automated test running via `ctest`, covering
  set/list/show/unset and error cases against a scratch `$HOME`.

---

## ⬜ Planned (post v1.0.0)

Real features that will meaningfully expand what FDK apps can do, but are
not blockers for a first stable release.

- [ ] **IME / compose sequences** — dead key composition, input method
  editors for CJK and other scripts. Wayland: `zwp_input_method_v2`.

- [ ] **Text shaping / RTL** — HarfBuzz integration for Arabic, Hebrew,
  Devanagari and other scripts needing shaping or right-to-left layout.

- [ ] **Accessibility** — queryable widget tree for screen readers.
  AT-SPI2 compatible without D-Bus — custom socket protocol or direct
  Orca integration.

- [ ] **Vulkan backend** — `VkSurface` via `VK_KHR_wayland_surface` /
  `VK_KHR_xcb_surface`. Lower CPU overhead than OpenGL for complex UIs.

- [ ] **`wl_surface_frame` callback** — truly vsync-driven animation,
  only repainting when the compositor is ready rather than on a timer.

- [ ] **Notifications** — sending a desktop notification from an FDK app
  without D-Bus/libnotify. Lightweight helper binary or direct compositor
  protocol.

- [ ] **Per-app theme packaging** — a standard way for an FDK app to ship
  its own `.fdktheme` files so `fdk-theme list` can discover them after
  the app is installed.

---

## ⬜ Subprojects (separate repositories, post v1.0.0)

- [ ] **FDK Overlay** — compositor overlay layer built on FDK
- [ ] **GTK Theme Bridge** — reads the active GTK theme and translates
  its color tokens into a `.fdktheme` file automatically, so FDK apps and
  GTK apps look visually consistent without manual coordination
- [ ] **Qt Theme Bridge** — reads the active Qt/KDE theme (via
  `~/.config/qt5ct/`, `~/.config/qt6ct/`, or Kvantum theme files) and
  translates its color palette into a `.fdktheme` file automatically, so
  FDK apps look consistent with Qt apps on KDE / LXQt setups. No Qt
  dependency in FDK itself — the bridge reads Qt's plain-text config
  files directly.

---

## Dependency audit (all permissive — no GPL/LGPL contamination)

| Library | License | Purpose |
|---|---|---|
| libwayland-client | MIT | Wayland platform backend |
| libxcb / Xlib | MIT/X11 | X11 platform backend |
| xdg-shell (wayland-protocols) | MIT | Window management protocol |
| libxkbcommon | MIT | Keyboard layout + Unicode input |
| wayland-cursor | MIT | Cursor shape changes |
| FreeType | FTL (BSD-like) | Font rasterisation |
| libGL / EGL | spec only | OpenGL + EGL render backend |
| wayland-egl | MIT | EGL window surface on Wayland |
| pthread | system | inotify hot-reload background thread |
| stb_image.h (optional) | MIT/Public Domain | PNG / JPEG image loading |

---

## File structure

```
fdk/
├── include/fdk/
│   ├── fdk.h                  ← core types, FDK_Gradient, FDK_Shadow
│   └── fdk_widget.h           ← FDK_Theme, FDK_WidgetStyle, full public API
├── src/
│   ├── core/
│   │   ├── core.c             ← init, shutdown, clipboard, app_name stash
│   │   ├── core_internal.h
│   │   ├── tween.c            ← animation engine
│   │   └── theme.c            ← .fdktheme parser, font scanner, inotify watch, 3-tier resolver
│   ├── platform/
│   │   ├── wayland.c          ← Wayland backend incl. clipboard, data device
│   │   └── x11.c              ← X11 backend incl. clipboard, selection protocol
│   ├── render/
│   │   ├── software.c         ← CPU renderer, gradients, box-blur shadows
│   │   └── opengl.c           ← GL 3.3 batch renderer, glyph atlas
│   └── widgets/
│       └── widget.c           ← all 19 widget types, layout, paint, resolve_style
├── themes/                    ← bundled .fdktheme reference files (not auto-installed)
│   ├── faded-dream.fdktheme
│   ├── void.fdktheme
│   └── rose.fdktheme
├── examples/
│   ├── hello/
│   ├── widgets/
│   ├── showcase/
│   ├── theme-switcher/        ← every widget type, live switching, hot-reload demo
│   └── systemwide-test/       ← tests fdk_theme_watch_conf() live re-theme
├── tools/
│   └── fdk-theme/             ← CLI: list/set/unset/show system-wide + per-app themes
├── tests/
│   ├── test_theme.c           ← 49+ headless assertions, runs via ctest
│   └── CMakeLists.txt
├── packaging/
│   ├── PKGBUILD               ← Arch Linux (.pkg.tar.zst)
│   ├── fdk.spec               ← RPM (Fedora / openSUSE)
│   └── debian/                ← Debian / Ubuntu (.deb)
├── sublicense/
│   └── SUBLICENSE             ← terms for Dependent Projects using FDK as a library
├── .gitignore
├── CMakeLists.txt
├── LICENSE
├── README.md
└── ROADMAP.md
```
