# Lunduke Edit 0.2 — notes

## Build

```
cd /workspace/lunduke-edit
meson setup build --reconfigure
meson compile -C build
./build/lunduke-edit
```

Requirements: C++17, Meson ≥0.56, gtkmm-3.0 ≥3.24, **gtksourceviewmm-3.0 ≥3.18**.

LCOS packaging will need `libgtksourceviewmm-3.0` (runtime) and the matching `-dev` package for builds. No `.deb` in this pass.

## Screenshot (dev)

```
DISPLAY=:7 ./build/lunduke-edit &
# open Search → Find… then:
import -window "$(xdotool search --name 'Find' | head -1)" \
  /workspace/uploads/lunduke-edit-0.2-find-replace.png
```

## 0.2 features

- **Undo / Redo**: via `GtkSourceView` / `Gsv::Buffer` (`can_undo` / `can_redo`, Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y). Menu items enable from buffer state.
- **Find & Replace**: BBEdit Lite–inspired dialog (Search → Find…, Ctrl+F). Find Next remains F3. Options: Start at Top, Wrap Around, Search Backwards, Search Selection Only, Extend Selection, Case Sensitive, Match Entire Words. Buttons: Find, Find All, Replace, Replace All, Don’t Find, Cancel. No Grep / multi-file.
- **Go to Line**: Search → Go to Line… (Ctrl+G).
- **Encoding**: UTF-8 (default) and Latin-1 (ISO-8859-1). Text menu radios + status bar. Open/Save use `Glib::convert`.
- **Open Recent**: File → Open Recent (last ~8 paths; keyfile under `~/.config/lunduke-edit/recents.txt`, also registered with `Gio::RecentManager`).

## Known gaps

- **Print / Page Setup**: not implemented.
- **Grep / multi-file find**: intentionally omitted.
- **Syntax highlighting**: SourceView is used for undo only; no language styles wired yet.
- **No .deb / packaging** in this pass.
- **Find All** highlights all hits with a tag and selects the first; GTK only supports one selection range.
- **Desktop/metainfo**: minimal `.desktop` only; AppStream / icons optional/future.

## Design notes

- Classic `Gtk::MenuBar` (no HeaderBar), Clearlooks/90s XFCE spirit.
- Line numbers via custom `LineGutter` (still used alongside SourceView).
- Cream editor background via CSS when the theme allows.
