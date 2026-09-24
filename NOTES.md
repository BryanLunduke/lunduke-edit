# Lunduke Edit 0.3.2 — notes

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
# → packaging/debs/lunduke-edit_0.3.2-1_amd64.deb
```

Runtime Depends include the gtkmm-3.0 stack and **libgtksourceviewmm-3.0-0v5** (via shlibdeps). Ships `org.lunduke.LundukeEdit.desktop`. **Not** seeded into `lcos-live-06/config/packages.chroot` (optional overlay install only).

## Screenshot (dev)

```
DISPLAY=:2 ./build/lunduke-edit &
# Empty main window titled Untitled — Lunduke Edit:
import -window "$(xdotool search --name 'Lunduke Edit' | head -1)" \
  /workspace/uploads/lunduke-edit-0.3.2-blank.png
```

## 0.3.2 changes

- **Blank first launch**: `load_seed_sample()` no longer seeds demo text or writes `/tmp/readme.txt`; opens an empty untitled buffer (clean, not dirty, undo history cleared via `begin_not_undoable_action`).
- **File → New** stays consistent (blank untitled).
- **Save As** default name: `Untitled.txt` when no path is set.
- README / About / packaging version **0.3.2** (`0.3.2-1` deb).

## 0.3.1 fixes

- **Edit → Undo / Redo sensitivity**: connect `Gsv::Buffer` `property_can_undo` / `property_can_redo` notify (plus Edit submenu `signal_map`) so menu items track undo-manager state; keyboard Ctrl+Z / Ctrl+Y no longer leave the menu stale.
- **Save As**: do not double-append `.txt` when the chosen basename already has a known text extension; only add `.txt` when there is no extension.
- README version line matched to About (**0.3.1**).

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
