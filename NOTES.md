# Lunduke Edit 0.3 — notes

## Build

```
cd /workspace/lunduke-edit
meson setup build --reconfigure
meson compile -C build
./build/lunduke-edit
```

Requirements: C++17, Meson ≥0.56, gtkmm-3.0 ≥3.24, **gtksourceviewmm-3.0 ≥3.18**.

## Debian package (overlay apt only)

```
./packaging/build-deb.sh
# → packaging/debs/lunduke-edit_0.3-1_amd64.deb
```

Runtime Depends include the gtkmm-3.0 stack and **libgtksourceviewmm-3.0-0v5** (via shlibdeps). Ships `org.lunduke.LundukeEdit.desktop`. **Not** seeded into `lcos-live-06/config/packages.chroot` (optional overlay install only).

## Screenshot (dev)

```
DISPLAY=:2 ./build/lunduke-edit &
# File → Print… then:
import -window "$(xdotool search --name 'Print' | head -1)" \
  /workspace/uploads/lunduke-edit-0.3-print.png
```

## 0.3 features

- **Print…** (File → Print…, Ctrl+P): `Gtk::PrintOperation` draw-pages from the text buffer / GtkSourceView (Pango layout, multi-page).
- **Page Setup…**: `Gtk::run_page_setup_dialog` / print settings retained across jobs; print dialog also embeds page setup.

## 0.2 features (still present)

- **Undo / Redo**: via `GtkSourceView` / `Gsv::Buffer` (`can_undo` / `can_redo`, Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y).
- **Find & Replace**: BBEdit Lite–inspired dialog (Search → Find…, Ctrl+F).
- **Go to Line**: Search → Go to Line… (Ctrl+G).
- **Encoding**: UTF-8 / Latin-1.
- **Open Recent**: File → Open Recent.

## Known gaps

- **Grep / multi-file find**: intentionally omitted.
- **Syntax highlighting**: SourceView is used for undo; no language styles wired yet.
- **Find All** highlights all hits with a tag and selects the first; GTK only supports one selection range.
- **Desktop/metainfo**: minimal `.desktop` only; AppStream / icons optional/future.

## Design notes

- Classic `Gtk::MenuBar` (no HeaderBar), Clearlooks/90s XFCE spirit.
- Line numbers via custom `LineGutter` (still used alongside SourceView).
- Cream editor background via CSS when the theme allows.
