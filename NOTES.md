# Lunduke Edit 0.9 — notes

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
# → packaging/debs/lunduke-edit_0.9-3_amd64.deb
```

Runtime Depends include the gtkmm-3.0 stack and **libgtksourceviewmm-3.0-0v5** (via shlibdeps). Ships `org.lunduke.LundukeEdit.desktop`. **Not** seeded into `lcos-live-06/config/packages.chroot` (optional overlay install only).

## Screenshot (dev)

```
DISPLAY=:2 ./build/lunduke-edit &
# Empty main window titled Untitled — Lunduke Edit:
import -window "$(xdotool search --name 'Lunduke Edit' | head -1)" \
  /workspace/uploads/lunduke-edit-0.7-blank.png
```

## 0.9-2

- **Save**: a symlink save replaces the canonical target. A broken symlink is left alone. Hard-linked files are written in place. Bytes are flushed before the rename, then the parent directory is flushed. The previous mode and, when permitted, owner are kept.
- **Open**: a second instance reuses a window only when it is focused and still an empty untitled document. A failed open does not present a spare window. Closed windows are freed. Cancelling a dirty close keeps the window.
- **Size**: the status bar shows the bytes a save would write, cached from edits. Files over 32 MiB ask first. A single paste is capped at 32 MiB. Reads go to EOF, so a reported size of 0 is not an empty file. Non-local locations are rejected.
- **Find**: Find All and Replace All scan in idle chunks, cap the hit count, and warn before a huge undo. Search Backwards is honored. The match count is a separate label and clears when the buffer changes.
- **Documents**: one shared recent list. New documents are UTF-8. New windows inherit font, wrap, tab width, and the open charset. Wrap defaults to off and turns off on a very long line. CRLF and CR are remembered and written back. Printing paginates and copies tab stops.
- App / Meson / About stay **0.9**. Debian package **0.9-2**.

## 0.9

- **LCOS identity**: app / Meson **0.9** / Debian package **0.9-1** for the LCOS 0.9 track. Features and UI unchanged from 0.8-2.
- README / About / packaging / AppStream metainfo match **0.9**.

## 0.8.1 / Debian 0.8-2

- **Save**: a failed save stays dirty and does not truncate the file. Bytes go to a temporary file and replace the original only after the write succeeds.
- **Save As**: the confirmed path is the path written. The extension is not rewritten after the overwrite prompt.
- **Encoding**: a non-UTF-8 file opened as Latin-1 stays Latin-1, so a clean save does not turn a byte such as 0xE9 into UTF-8.
- **Open**: a file opened in an already running instance asks before dropping unsaved edits. Every command-line file is opened in its own window.
- **Read errors**: a short read is not marked as a clean copy of the file.
- **Print**: the first line on each page is not clipped. Line numbers scroll with the text.
- **File → Exit** quits the process.
- **Find / Replace**: one Replace is one undo step. Find Next does not skip a character. Start at Top applies once, then clears. Search Selection Only does nothing without a selection. Replace still replaces the found match when Extend Selection has grown the selection.
- **Undo** back to the saved text clears the unsaved marker.
- **Go to Line** rejects trailing junk. The column readout uses tab width.
- Meson **0.8.1**. Debian package **0.8-2**. About / kVersion **0.8-2**.

## 0.8

- **LCOS identity**: app / Meson **0.8.0** / Debian package **0.8-1** for the LCOS 0.8 track. Features and UI unchanged from 0.7-5.
- README / About / packaging / AppStream metainfo match **0.8**.

## 0.7-5

- **Find — Search Selection Only**: pin the selected range when Find opens so matches stay inside that region (no longer escapes to outside hits after reselection).
- **Find — Extend Selection**: successive Find grows the selection from the original anchor through later matches (works with Start at Top still checked; Start at Top alone unchanged).
- **Edit → Find…**: Find is under Edit (Notepad-like); Search → Find remains.
- Debian package **0.7-5**; About/kVersion **0.7-5**.

## 0.7-4

- **Find & Replace dialog**: layout-only — reduce gap from Search For entry to first checkbox row (match dialog band spacing); shift right-hand button column down so Find aligns with the Search For entry (not the label). Keeps tight label→entry gaps, Cancel bottom-right, middle checkbox column alignment, and button banding. No Find/Replace behavior changes.
- Debian package **0.7-4**; About/kVersion **0.7-4**.

## 0.7-3

- **Find & Replace dialog**: layout-only — tighter Search For / Replace With label→entry gaps; Cancel moved to bottom-right below Replace With (not beside Case Sensitive); middle checkbox column left-aligned under Match Entire Words. Keeps 0.7-2 button-column banding otherwise. No Find/Replace behavior changes.
- Debian package **0.7-3**; About/kVersion **0.7-3**.

## 0.7-2

- **Find & Replace dialog**: layout/spacing tidy only — Gtk::Grid row bands so left fields/checkboxes align with the right-hand button column; wider button spacing, content border 12px, row spacing 8px, column spacing 14px. No Find/Replace behavior changes.
- Debian package **0.7-2**; About/kVersion **0.7-2**.

## 0.7

- **LCOS identity**: app version **0.7** / Debian package **0.7-1** now tracks the LCOS release train (same convention as Lunduke Paint). No feature changes vs 0.3.2.
- **App icon**: `Icon=org.lunduke.LundukeEdit`, `StartupWMClass=lunduke-edit`, `set_default_icon_name` / `set_icon_name` / About `set_logo_icon_name`. Bob’s hicolor PNGs (16, 22, 24, 32, 48, 64, 96, 128, 256, 512) ship in the package; 1024 master is in-tree only. postinst runs `gtk-update-icon-cache`.
- README / About / packaging / AppStream metainfo match **0.7**.

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
- **Icons**: desktop / WM / About use `org.lunduke.LundukeEdit` with shipped hicolor PNGs (`data/icons/README.md`). Fallback `accessories-text-editor` remains if the theme lookup fails. AppStream metainfo: `org.lunduke.LundukeEdit.metainfo.xml`.

## Design notes

- Classic `Gtk::MenuBar` (no HeaderBar), Clearlooks/90s XFCE spirit.
- Line numbers via custom `LineGutter` (still used alongside SourceView).
- Cream editor background via CSS when the theme allows.
