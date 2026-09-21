# Lunduke Edit 0.1 — notes

## Build

```
cd /workspace/lunduke-edit
meson setup build
meson compile -C build
./build/lunduke-edit
```

Requirements: C++17, Meson ≥0.56, gtkmm-3.0 ≥3.24.

## Screenshot (dev)

```
DISPLAY=:7 ./build/lunduke-edit &
# then capture the window
import -window "$(xdotool search --name 'Lunduke Edit' | head -1)" /workspace/uploads/lunduke-edit-0.1.png
```

## Known gaps vs full Notepad / BBEdit Lite

- **Undo / Redo**: plain `Gtk::TextView` / `GtkTextBuffer` has no undo stack (that lives on `GtkSourceBuffer`). Menu item is present but a no-op until GtkSourceView or a custom undo log is wired. Deferred.
- **Replace / Replace All**: Search → Find / Find Next only.
- **Print / Page Setup**: not implemented.
- **Go to Line**: not implemented.
- **Encoding / line-ending picker**: UTF-8 bytes only; no CR/LF or charset UI.
- **Recent files / Open from command line** beyond `Gio::APPLICATION_HANDLES_OPEN`: basic open-path works; no recents menu.
- **Status bar “1,024 bytes”** in the mockup is illustrative; live size is the UTF-8 byte length of the buffer.
- **No .deb / packaging** in this pass (local tree only).
- **Desktop/metainfo**: a minimal `.desktop` is shipped; AppStream metainfo and hicolor icons are optional/future.
- **Syntax highlighting / soft tabs as spaces**: out of scope for 0.1.

## Design notes

- Classic `Gtk::MenuBar` (no HeaderBar), Clearlooks/90s XFCE spirit.
- Line numbers via a custom `LineGutter` `DrawingArea` synced to the text view (GtkSourceView not required / not installed here).
- Cream editor background via CSS when the theme allows.
