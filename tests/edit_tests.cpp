// SPDX-License-Identifier: GPL-3.0-or-later

#include "application.hpp"
#include "main_window.hpp"

#include <glib.h>
#include <gtkmm/textiter.h>

#include <giomm/file.h>
#include <gtkmm/printoperation.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace lundukeedit {

struct EditChecks {
  static int failures;

  static void expect(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << "\n";
      ++failures;
    }
  }

  static void write_bytes(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    expect(static_cast<bool>(out) || bytes.empty(), "write_bytes");
  }

  static std::string read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string data;
    char buf[4096];
    while (in) {
      in.read(buf, sizeof buf);
      data.append(buf, static_cast<std::size_t>(in.gcount()));
    }
    return data;
  }

  static void flush_ui() {
    while (g_main_context_pending(nullptr)) {
      g_main_context_iteration(nullptr, false);
    }
  }

  static std::string selection_text(MainWindow& w) {
    Gtk::TextIter a, b;
    auto buf = w.buffer();
    if (!buf->get_selection_bounds(a, b) || a == b) {
      return {};
    }
    return a.get_text(b);
  }

  static void test_parse_and_save_as_path() {
    int line = 0;
    expect(!MainWindow::parse_go_to_line("12abc", line), "12abc rejected");
    expect(!MainWindow::parse_go_to_line("abc", line), "abc rejected");
    expect(!MainWindow::parse_go_to_line("", line), "empty rejected");
    expect(!MainWindow::parse_go_to_line("12 ", line), "trailing space rejected");
    expect(MainWindow::parse_go_to_line("12", line) && line == 12, "12 accepted");
    expect(MainWindow::parse_go_to_line(" 7", line) && line == 7, "leading space ok");

    expect(MainWindow::ensure_save_as_path("notes.txt.txt") == "notes.txt.txt",
           "notes.txt.txt unchanged");
    expect(MainWindow::ensure_save_as_path("report") == "report",
           "report unchanged");
    expect(MainWindow::ensure_save_as_path("/tmp/report.txt") == "/tmp/report.txt",
           "report.txt unchanged");
  }

  static void test_columns_and_gutter(MainWindow& w) {
    w.apply_tab_width(4);
    w.buffer()->set_text("\t");
    w.buffer()->place_cursor(w.buffer()->get_iter_at_offset(1));
    w.update_status();
    expect(w.status_pos_.get_text() == "Ln 1, Col 5", "tab width 4 is Col 5");

    w.apply_tab_width(8);
    w.update_status();
    expect(w.status_pos_.get_text() == "Ln 1, Col 9", "tab width 8 is Col 9");

    w.buffer()->place_cursor(w.buffer()->begin());
    w.update_status();
    expect(w.status_pos_.get_text() == "Ln 1, Col 1", "line start is Col 1");

    expect(w.gutter_ && w.gutter_->follows_text_view_adjustment(),
           "gutter tracks the scrolling adjustment");
    expect(w.scrolled_.get_vadjustment() == w.text_view_.get_vadjustment(),
           "text view uses the scrolled window adjustment");
  }

  static void test_save_open_undo(MainWindow& w, const std::string& dir) {
    const std::string path = dir + "/orig.txt";
    const std::string original = "ORIGINAL-BYTES-12345";
    write_bytes(path, original);

    expect(w.open_file(path), "open orig");
    expect(!w.dirty_, "opened file is clean");
    expect(w.buffer()->get_text() == original, "opened text");

    w.buffer()->insert(w.buffer()->end(), "MORE");
    expect(w.dirty_, "edit marks dirty");
    expect(!w.save_to_path(dir + "/missing-dir/out.txt"), "save to missing dir fails");
    expect(w.dirty_, "failed save stays dirty");
    expect(w.file_path_ == path, "failed save keeps path");
    expect(read_bytes(path) == original, "failed save does not truncate original");

    expect(w.save_to_path(path), "save success");
    expect(!w.dirty_, "successful save is clean");
    expect(read_bytes(path) == original + "MORE", "saved bytes");
    expect(!w.buffer()->get_modified(), "save point recorded");

    w.buffer()->insert(w.buffer()->end(), "Z");
    expect(w.dirty_, "dirty after further edit");
    w.on_undo();
    expect(w.buffer()->get_text() == original + "MORE", "undo restored saved text");
    expect(!w.dirty_, "undo to save point clears dirty");
    expect(w.confirm_discard_or_save(), "close does not need a prompt");

    const std::string latin_path = dir + "/latin1.txt";
    const std::string latin("caf\xE9", 4);
    write_bytes(latin_path, latin);
    expect(w.open_file(latin_path), "open latin1");
    expect(w.encoding_ == "ISO-8859-1", "detected Latin-1");
    expect(!w.dirty_, "latin1 open is clean");
    expect(w.status_enc_.get_text() == "Latin-1", "status shows Latin-1");
    expect(w.status_bytes_.get_text() == "4 bytes",
           "latin1 status is the size that will be saved");
    expect(w.save_to_path(latin_path), "save latin1 with no edits");
    expect(read_bytes(latin_path) == latin, "latin1 bytes preserved");
    expect(!w.dirty_, "still clean after round-trip save");

    w.buffer()->set_text("keep-me");
    w.buffer()->set_modified(true);
    w.refresh_dirty_from_buffer();
    expect(w.dirty_, "dirty before bad open");
    expect(!w.open_file(dir), "directory is not a successful open");
    expect(w.buffer()->get_text() == "keep-me", "bad open keeps the buffer");
    expect(w.dirty_, "bad open does not mark clean");
  }

  static void test_find_replace(MainWindow& w) {
    auto buf = w.buffer();
    buf->set_text("xx");
    buf->place_cursor(buf->begin());
    buf->set_modified(false);

    FindOptions o;
    o.search_for = "x";
    o.replace_with = "Y";
    o.wrap_around = true;
    o.start_at_top = false;
    o.search_selection_only = false;
    o.extend_selection = false;

    expect(w.replace_current(o) == MainWindow::ReplaceResult::Found,
           "first replace finds");
    expect(w.buffer()->get_text() == "xx", "find does not edit");
    expect(selection_text(w) == "x", "first x selected");

    expect(w.replace_current(o) == MainWindow::ReplaceResult::Replaced,
           "second replace edits");
    expect(w.buffer()->get_text() == "Yx", "first x replaced");
    expect(selection_text(w) == "x", "second x selected, not skipped");

    expect(w.replace_current(o) == MainWindow::ReplaceResult::Replaced,
           "third replace edits");
    expect(w.buffer()->get_text() == "YY", "both x replaced");

    w.on_undo();
    expect(w.buffer()->get_text() == "Yx", "one undo restores one replace");
    w.on_undo();
    expect(w.buffer()->get_text() == "xx", "second undo restores original");

    buf->set_text("x---x");
    buf->place_cursor(buf->get_iter_at_offset(4));
    o.wrap_around = true;
    expect(w.find_match(o, true), "find next at cursor");
    expect(selection_text(w) == "x", "found an x");
    Gtk::TextIter a, b;
    expect(buf->get_selection_bounds(a, b), "has selection");
    expect(a.get_offset() == 4, "did not wrap to the earlier x");

    buf->set_text("one two one");
    buf->place_cursor(buf->begin());
    o.search_for = "one";
    o.replace_with = "ONE";
    o.extend_selection = true;
    o.start_at_top = true;
    o.wrap_around = false;
    w.clear_extend_anchor();
    w.last_match_valid_ = false;
    expect(w.find_match(o, false), "extend first find");
    expect(selection_text(w) == "one", "first one");
    expect(w.find_match(o, false), "extend second find");
    expect(selection_text(w) == "one two one", "selection grew");
    expect(w.replace_current(o) == MainWindow::ReplaceResult::Replaced,
           "replace grown selection");
    expect(w.buffer()->get_text() == "one two ONE",
           "replaced the found needle, not the whole span");

    buf->set_text("abc abc");
    buf->place_cursor(buf->begin());
    w.clear_selection_only_range();
    FindOptions sel;
    sel.search_for = "a";
    sel.search_selection_only = true;
    sel.wrap_around = true;
    sel.start_at_top = true;
    expect(!w.find_match(sel, false), "selection only without selection");
    expect(selection_text(w).empty(), "did not select a hit in the whole buffer");
    expect(w.count_matches(sel) == 0, "count does not search the whole buffer");
    expect(w.replace_all(sel) == 0, "replace all does not search the whole buffer");
    expect(w.buffer()->get_text() == "abc abc", "buffer unchanged");

    FindReplaceDialog dlg(w, o);
    dlg.clear_start_at_top();
    expect(!dlg.options().start_at_top, "start at top clears");
  }

  static int main_window_count(Application& app) {
    int n = 0;
    for (auto* win : app.get_windows()) {
      if (dynamic_cast<MainWindow*>(win)) {
        ++n;
      }
    }
    return n;
  }

  static bool window_has_path(Application& app, const std::string& path) {
    for (auto* win : app.get_windows()) {
      auto* mw = dynamic_cast<MainWindow*>(win);
      if (mw && mw->file_path_ == path) {
        return true;
      }
    }
    return false;
  }

  static void pump_job(MainWindow& w) {
    for (int i = 0; i < 100000 && w.find_scan_.active; ++i) {
      g_main_context_iteration(nullptr, false);
    }
  }

  static bool menu_has_path(MainWindow& w, const std::string& path) {
    if (!w.recents_menu_) {
      return false;
    }
    for (auto* child : w.recents_menu_->get_children()) {
      auto* item = dynamic_cast<Gtk::MenuItem*>(child);
      if (item && item->get_label() == path) {
        return true;
      }
    }
    return false;
  }

  static void focus_window(Application& app, MainWindow* w) {
    w->present();
    flush_ui();
    app.note_window_focus(w);
  }

  static std::set<MainWindow*> window_set(Application& app) {
    std::set<MainWindow*> out;
    for (auto* win : app.get_windows()) {
      if (auto* mw = dynamic_cast<MainWindow*>(win)) {
        out.insert(mw);
      }
    }
    return out;
  }

  static int selection_offset(MainWindow& w) {
    Gtk::TextIter a, b;
    if (!w.buffer()->get_selection_bounds(a, b)) {
      return -1;
    }
    return a.get_offset();
  }

  // A Gio/second-instance open reuses a window only when that window is
  // focused and still an empty untitled document. A dirty document is left
  // alone (no discard prompt) and the file opens in a new window. A failed
  // open deletes the window it just created.
  static void test_open_many(Application& app, const std::string& dir) {
    const std::string a = dir + "/a.txt";
    const std::string b = dir + "/b.txt";
    const std::string c = dir + "/c.txt";
    const std::string good = dir + "/good.txt";
    const std::string missing = dir + "/missing.txt";
    write_bytes(a, "file-a");
    write_bytes(b, "file-b");
    write_bytes(c, "file-c");
    write_bytes(good, "good");
    ::unlink(missing.c_str());

    if (!app.main_window() || !app.main_window()->get_visible()) {
      auto* created = app.create_window();
      created->present();
      flush_ui();
    }
    auto* current = app.main_window();
    current->load_seed_sample();
    expect(current->is_empty_untitled(), "seeded window is empty untitled");
    focus_window(app, current);

    app.open_files({a, b});
    flush_ui();
    expect(current->file_path_ == a && current->buffer()->get_text() == "file-a",
           "focused empty untitled is reused for the first file");
    expect(window_has_path(app, b), "the next file opens in a new window");
    expect(main_window_count(app) == 2,
           "only the first path reuses the empty window");

    current->buffer()->set_text("unsaved-edits");
    current->buffer()->set_modified(true);
    current->refresh_dirty_from_buffer();
    expect(current->dirty_, "window dirty before a second-instance open");
    const std::string kept = current->buffer()->get_text();
    focus_window(app, current);
    const int before_dirty = main_window_count(app);
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    app.open_files({c});
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    flush_ui();
    expect(main_window_count(app) == before_dirty + 1,
           "dirty focused window is not reused");
    expect(current->get_visible(), "dirty window stays visible");
    expect(current->dirty_, "second-instance open does not prompt to discard");
    expect(current->buffer()->get_text() == kept, "dirty buffer is kept");
    expect(current->file_path_ == a, "dirty window keeps its path");
    expect(window_has_path(app, c), "requested file opens in a new window");

    // File → Open still asks. Cancel leaves the dirty buffer alone.
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    current->on_open();
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(current->buffer()->get_text() == kept, "File → Open cancel keeps edits");

    const int before_miss = main_window_count(app);
    app.open_files({missing});
    flush_ui();
    expect(main_window_count(app) == before_miss,
           "failed open deletes the unused window");
    expect(current->buffer()->get_text() == kept,
           "failed open keeps the existing buffer");

    focus_window(app, current);
    const int before_mix = main_window_count(app);
    app.open_files({good, missing});
    flush_ui();
    expect(window_has_path(app, good), "good file opens");
    expect(main_window_count(app) == before_mix + 1,
           "missing file does not leave an extra window");
  }

  static void test_review_fixes(Application& app, MainWindow& w,
                                const std::string& dir) {
    expect(w.text_view_.get_wrap_mode() == Gtk::WRAP_NONE,
           "default wrap is off");

    // st_size 0 (/proc) is not EOF.
    expect(w.open_file("/proc/self/comm"), "open /proc/self/comm");
    expect(!w.buffer()->get_text().empty(), "/proc text was read");
    struct stat proc_st {};
    expect(::stat("/proc/self/comm", &proc_st) == 0 && proc_st.st_size == 0,
           "/proc/self/comm reports size 0");

    const std::string big = dir + "/twenty.txt";
    write_bytes(big, std::string(20, 'Z'));
    w.buffer()->set_text("keep-buffer");
    w.buffer()->set_modified(true);
    w.refresh_dirty_from_buffer();
    g_setenv("LUNDUKE_EDIT_TEST_MAX_OPEN", "8", TRUE);
    expect(!w.open_file(big), "oversize open is refused in test mode");
    expect(w.buffer()->get_text() == "keep-buffer", "refused open keeps buffer");
    g_setenv("LUNDUKE_EDIT_TEST_LARGE", "1", TRUE);
    expect(w.open_file(big), "oversize open proceeds when allowed");
    expect(w.buffer()->get_text() == std::string(20, 'Z'), "oversize bytes loaded");
    g_unsetenv("LUNDUKE_EDIT_TEST_LARGE");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_OPEN");

    w.encoding_ = "UTF-8";
    w.newline_style_ = MainWindow::NewlineStyle::Lf;
    w.buffer()->set_text("via-link");

    const std::string target = dir + "/link-target.txt";
    const std::string linkpath = dir + "/link.txt";
    write_bytes(target, "old-target");
    ::unlink(linkpath.c_str());
    expect(::symlink(target.c_str(), linkpath.c_str()) == 0, "make symlink");
    expect(w.save_to_path(linkpath), "save through symlink");
    expect(read_bytes(target) == "via-link", "symlink save updates the target");
    struct stat lst {};
    expect(::lstat(linkpath.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode),
           "symlink itself is not replaced");

    const std::string hard1 = dir + "/hard1.txt";
    const std::string hard2 = dir + "/hard2.txt";
    write_bytes(hard1, "old-hard");
    ::unlink(hard2.c_str());
    expect(::link(hard1.c_str(), hard2.c_str()) == 0, "make hard link");
    struct stat before_hard {};
    expect(::stat(hard1.c_str(), &before_hard) == 0, "stat hard link");
    w.buffer()->set_text("new-hard");
    expect(w.save_to_path(hard1), "save hard-linked file");
    struct stat after_hard {};
    expect(::stat(hard1.c_str(), &after_hard) == 0, "stat after hard save");
    expect(before_hard.st_ino == after_hard.st_ino, "hard link keeps the inode");
    expect(after_hard.st_nlink >= 2, "hard link nlink stays above 1");
    expect(read_bytes(hard2) == "new-hard", "other name sees the new bytes");

    const std::string mode_path = dir + "/mode.txt";
    write_bytes(mode_path, "old-mode");
    expect(::chmod(mode_path.c_str(), 0600) == 0, "chmod 0600");
    struct stat before_mode {};
    expect(::stat(mode_path.c_str(), &before_mode) == 0, "stat mode file");
    w.buffer()->set_text("new-mode");
    expect(w.save_to_path(mode_path), "save preserves mode");
    struct stat after_mode {};
    expect(::stat(mode_path.c_str(), &after_mode) == 0, "stat after mode save");
    expect((after_mode.st_mode & 0777) == 0600, "mode 0600 preserved");
    expect(after_mode.st_uid == before_mode.st_uid, "owner preserved when permitted");

    const std::string broken = dir + "/broken-link.txt";
    ::unlink(broken.c_str());
    expect(::symlink((dir + "/does-not-exist").c_str(), broken.c_str()) == 0,
           "make broken symlink");
    w.buffer()->set_text("nope");
    expect(!w.save_to_path(broken), "broken symlink save fails");
    struct stat broken_st {};
    expect(::lstat(broken.c_str(), &broken_st) == 0 && S_ISLNK(broken_st.st_mode),
           "broken symlink is not replaced with a regular file");

    const std::string crlf_path = dir + "/crlf.txt";
    write_bytes(crlf_path, "a\r\nb\r\n");
    expect(w.open_file(crlf_path), "open crlf");
    expect(w.buffer()->get_text() == "a\nb\n", "crlf normalized to lf");
    expect(w.newline_style_ == MainWindow::NewlineStyle::Crlf, "style is crlf");
    expect(w.status_bytes_.get_text() == "6 bytes", "crlf status counts cr");
    w.buffer()->insert(w.buffer()->end(), "c");
    expect(w.status_bytes_.get_text() == "7 bytes", "crlf status tracks the insert");
    expect(w.save_to_path(crlf_path), "save crlf");
    expect(read_bytes(crlf_path) == "a\r\nb\r\nc", "saved with crlf");

    const std::string cr_path = dir + "/cr.txt";
    write_bytes(cr_path, "a\rb\r");
    expect(w.open_file(cr_path), "open cr");
    expect(w.buffer()->get_text() == "a\nb\n", "cr normalized to lf");
    expect(w.newline_style_ == MainWindow::NewlineStyle::Cr, "style is cr");
    expect(w.save_to_path(cr_path), "save cr");
    expect(read_bytes(cr_path) == "a\rb\r", "saved with cr");

    const std::string lf_path = dir + "/lf.txt";
    write_bytes(lf_path, "a\nb\n");
    expect(w.open_file(lf_path), "open lf");
    expect(w.buffer()->get_text() == "a\nb\n", "lf buffer unchanged");
    expect(w.newline_style_ == MainWindow::NewlineStyle::Lf, "style is lf");
    expect(w.save_to_path(lf_path), "save lf");
    expect(read_bytes(lf_path) == "a\nb\n", "lf bytes unchanged");

    const std::string latin_path = dir + "/latin-new.txt";
    write_bytes(latin_path, std::string("caf\xE9", 4));
    expect(w.open_file(latin_path), "open latin for new-document reset");
    expect(w.encoding_ == "ISO-8859-1", "detected latin-1");
    expect(w.prefer_utf8_, "auto-detect does not change the open preference");
    expect(w.enc_utf8_item_ && w.enc_utf8_item_->get_active(),
           "auto-detect does not flip the encoding radio");
    w.on_new();
    expect(w.encoding_ == "UTF-8" && w.saved_encoding_ == "UTF-8",
           "new document is UTF-8");
    expect(w.newline_style_ == MainWindow::NewlineStyle::Lf, "new document is lf");
    expect(w.prefer_utf8_, "new document leaves the open preference alone");
    const Glib::ustring cafe("caf\xc3\xa9");
    w.buffer()->set_text(cafe);
    const std::string cafe_path = dir + "/cafe-utf8.txt";
    expect(w.save_to_path(cafe_path), "save cafe as utf-8");
    expect(read_bytes(cafe_path) == std::string("caf\xc3\xa9", 5),
           "new document writes UTF-8");

    expect(w.enc_latin1_item_ != nullptr, "latin-1 radio exists");
    w.enc_latin1_item_->set_active(true);
    expect(!w.prefer_utf8_ && w.open_charset_ == "ISO-8859-1",
           "explicit latin-1 sets the open preference");
    expect(w.encoding_ == "ISO-8859-1", "explicit latin-1 sets document encoding");
    expect(!app.prefer_utf8() && app.open_charset() == "ISO-8859-1",
           "open preference is application owned");
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
    w.on_new();
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.encoding_ == "UTF-8", "new resets document encoding");
    expect(!w.prefer_utf8_ && w.enc_latin1_item_->get_active(),
           "new keeps the open-charset radio");

    app.set_tab_width(8);
    app.set_font("Monospace 13");
    app.set_wrap_text(true);
    auto* inherited = app.create_window();
    inherited->present();
    flush_ui();
    expect(inherited->tab_width_ == 8, "new window inherits tab width");
    expect(inherited->font_desc_.get_size() == 13 * Pango::SCALE,
           "new window inherits font size");
    expect(inherited->text_view_.get_wrap_mode() == Gtk::WRAP_WORD_CHAR,
           "new window inherits wrap");
    expect(!inherited->prefer_utf8_ && inherited->open_charset_ == "ISO-8859-1",
           "new window inherits open charset");
    expect(inherited->encoding_ == "UTF-8",
           "inherited window document encoding is still UTF-8");

    expect(app.wrap_text(), "wrap preference is on before a long line");
    inherited->buffer()->set_text(std::string(5000, 'x'));
    expect(inherited->text_view_.get_wrap_mode() == Gtk::WRAP_NONE,
           "a long line turns wrap off in that window");
    expect(app.wrap_text(), "long line does not change the wrap preference");
    auto* still_wraps = app.create_window();
    expect(still_wraps->text_view_.get_wrap_mode() == Gtk::WRAP_WORD_CHAR,
           "later window still inherits wrap");

    w.apply_tab_width(4);
    w.buffer()->set_text(std::string("\t") + std::string(5000, 'a'));
    w.buffer()->place_cursor(w.buffer()->end());
    w.update_status();
    expect(w.status_pos_.get_text() == "Ln 1, Col 5002",
           "column past the walk cap is the character index");

    const std::string recent1 = dir + "/recent1.txt";
    const std::string recent2 = dir + "/recent2.txt";
    auto* other = app.create_window();
    other->present();
    flush_ui();
    w.newline_style_ = MainWindow::NewlineStyle::Lf;
    w.encoding_ = "UTF-8";
    w.buffer()->set_text("r1");
    expect(w.save_to_path(recent1), "save recent1");
    other->buffer()->set_text("r2");
    expect(other->save_to_path(recent2), "save recent2");
    expect(menu_has_path(w, recent1) && menu_has_path(w, recent2),
           "first window menu has both recents");
    expect(menu_has_path(*other, recent1) && menu_has_path(*other, recent2),
           "second window menu has both recents");
    w.buffer()->set_text("r1b");
    expect(w.save_to_path(recent1), "save recent1 again");
    expect(menu_has_path(w, recent2) && menu_has_path(*other, recent2),
           "a later save does not drop the other recent");

    const int before_close = main_window_count(app);
    auto* closable = app.create_window();
    closable->present();
    flush_ui();
    expect(main_window_count(app) == before_close + 1, "closable window exists");
    expect(closable->on_delete_event(nullptr), "clean close handles delete");
    expect(!closable->get_visible(), "clean close hides");
    flush_ui();
    expect(main_window_count(app) == before_close,
           "closed window is deleted after hide");

    auto* dirty_close = app.create_window();
    dirty_close->present();
    flush_ui();
    dirty_close->buffer()->set_text("stay-open");
    dirty_close->buffer()->set_modified(true);
    dirty_close->refresh_dirty_from_buffer();
    const int before_cancel_close = main_window_count(app);
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    expect(dirty_close->on_delete_event(nullptr), "cancel close returns true");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(dirty_close->get_visible(), "cancel close does not hide");
    flush_ui();
    expect(main_window_count(app) == before_cancel_close,
           "cancelled close does not delete the window");
    expect(dirty_close->buffer()->get_text() == "stay-open",
           "cancelled close keeps the text");
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
    expect(dirty_close->on_delete_event(nullptr), "discard close handles delete");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    flush_ui();
    expect(main_window_count(app) == before_cancel_close - 1,
           "discarded close deletes the window");

    const auto before_remote = window_set(app);
    Gio::Application::type_vec_files remote;
    remote.push_back(Gio::File::create_for_uri("sftp://example.invalid/nope.txt"));
    app.on_open(remote, "");
    flush_ui();
    expect(window_set(app) == before_remote, "non-local open creates no window");

    const std::string native = dir + "/native-only.txt";
    write_bytes(native, "native");
    focus_window(app, &w);
    expect(!w.is_empty_untitled(), "focused window is not an empty untitled");
    const int before_mixed = main_window_count(app);
    Gio::Application::type_vec_files mixed;
    mixed.push_back(Gio::File::create_for_uri("sftp://example.invalid/nope.txt"));
    mixed.push_back(Gio::File::create_for_path(native));
    app.on_open(mixed, "");
    flush_ui();
    expect(window_has_path(app, native), "native file in a mixed open is opened");
    expect(main_window_count(app) == before_mixed + 1,
           "remote file in a mixed open adds no window");

    w.buffer()->set_text("abc");
    w.buffer()->place_cursor(w.buffer()->begin());
    FindOptions back;
    back.search_for = "c";
    back.search_backwards = true;
    back.start_at_top = true;
    back.wrap_around = false;
    w.clear_extend_anchor();
    w.last_match_valid_ = false;
    expect(w.find_match(back, false), "search backwards from the top finds c");
    expect(selection_text(w) == "c", "backwards start selects c");

    w.buffer()->set_text("ababa");
    FindOptions all;
    all.search_for = "a";
    all.search_backwards = false;
    all.start_at_top = true;
    w.highlight_all_matches(all);
    expect(selection_offset(w) == 0, "find all forward selects the first hit");
    expect(w.status_find_.get_text() == "3 matches", "match count has its own label");
    expect(w.status_pos_.get_text().find("matches") == std::string::npos,
           "match count is not appended to the position");
    all.search_backwards = true;
    w.highlight_all_matches(all);
    expect(selection_offset(w) == 4, "find all backward selects the last hit");
    expect(w.status_find_.get_text() == "3 matches", "backward find all counts");
    w.buffer()->insert(w.buffer()->end(), "z");
    expect(w.status_find_.get_text().empty(), "editing clears the match count");

    g_setenv("LUNDUKE_EDIT_TEST_MAX_HITS", "2", TRUE);
    w.buffer()->set_text("aaaa");
    all.search_for = "a";
    all.search_backwards = true;
    w.highlight_all_matches(all);
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_HITS");
    expect(selection_offset(w) == 3, "capped backward scan keeps the last hit");
    expect(w.status_find_.get_text().find('+') != std::string::npos,
           "capped scan marks the count");

    g_setenv("LUNDUKE_EDIT_TEST_CHUNK", "1", TRUE);
    w.buffer()->set_text("aaa");
    w.buffer()->set_modified(false);
    FindOptions repl;
    repl.search_for = "a";
    repl.replace_with = "b";
    w.start_replace_all(repl, nullptr);
    pump_job(w);
    expect(!w.find_scan_.active, "chunked replace finishes");
    expect(w.buffer()->get_text() == "bbb", "chunked replace all");
    w.on_undo();
    expect(w.buffer()->get_text() == "aaa", "chunked replace all is one undo");
    g_unsetenv("LUNDUKE_EDIT_TEST_CHUNK");

    const std::string thirty(30, 'a');
    g_setenv("LUNDUKE_EDIT_TEST_HUGE_BYTES", "20", TRUE);
    g_setenv("LUNDUKE_EDIT_TEST_HUGE_UNDO", "deny", TRUE);
    w.buffer()->set_text(thirty);
    repl.replace_with = "bb";
    w.start_replace_all(repl, nullptr);
    pump_job(w);
    expect(w.buffer()->get_text() == thirty, "denied huge undo leaves the buffer");
    g_setenv("LUNDUKE_EDIT_TEST_HUGE_UNDO", "allow", TRUE);
    w.start_replace_all(repl, nullptr);
    pump_job(w);
    expect(w.buffer()->get_text() == std::string(60, 'b'), "allowed huge replace");
    w.on_undo();
    expect(w.buffer()->get_text() == thirty, "huge replace is one undo");
    g_unsetenv("LUNDUKE_EDIT_TEST_HUGE_UNDO");
    g_unsetenv("LUNDUKE_EDIT_TEST_HUGE_BYTES");

    w.newline_style_ = MainWindow::NewlineStyle::Lf;
    w.encoding_ = "UTF-8";
    w.buffer()->set_text("hello");
    w.note_loaded_text("hello");
    w.update_status();
    expect(w.status_bytes_.get_text() == "5 bytes", "status bytes after load");
    w.buffer()->insert(w.buffer()->end(), "!");
    expect(w.status_bytes_.get_text() == "6 bytes", "status bytes after insert");
    w.on_undo();
    expect(w.status_bytes_.get_text() == "5 bytes", "status bytes after undo");
    w.buffer()->place_cursor(w.buffer()->begin());
    expect(w.status_bytes_.get_text() == "5 bytes",
           "cursor motion does not recount bytes");

    w.apply_tab_width(4);
    std::string printable;
    for (int i = 0; i < 80; ++i) {
      printable += "line ";
      printable += std::to_string(i);
      printable += "\n";
    }
    w.buffer()->set_text(std::string(5000, 'Q') + "\n" + printable);
    expect(w.next_print_end(0) == 4000, "print splits a long line");
    const std::string pdf = dir + "/print.pdf";
    ::unlink(pdf.c_str());
    auto op = Gtk::PrintOperation::create();
    op->set_export_filename(pdf);
    op->signal_begin_print().connect(
        [&w, op](const Glib::RefPtr<Gtk::PrintContext>& context) {
          w.on_begin_print(context);
          op->set_n_pages(static_cast<int>(w.print_page_breaks_.size()) + 1);
        });
    op->signal_draw_page().connect(
        [&w](const Glib::RefPtr<Gtk::PrintContext>& context, int page) {
          w.on_draw_page(context, page);
        });
    bool printed = false;
    try {
      const auto result = op->run(Gtk::PRINT_OPERATION_ACTION_EXPORT, w);
      printed = result == Gtk::PRINT_OPERATION_RESULT_APPLY;
    } catch (const Gtk::PrintError&) {
      printed = false;
    }
    expect(printed, "print export succeeds");
    expect(!w.print_page_breaks_.empty(), "long text paginates");
    expect(g_file_test(pdf.c_str(), G_FILE_TEST_EXISTS) != 0, "pdf was written");

    app.on_startup();
    app.on_startup();
  }

  static int run() {
    failures = 0;
    g_setenv("LUNDUKE_EDIT_TEST", "1", TRUE);
    g_setenv("GDK_BACKEND", "x11", FALSE);

    test_parse_and_save_as_path();

    const std::string dir = "/tmp/lunduke-edit-tests";
    g_mkdir_with_parents(dir.c_str(), 0700);

    auto app = Application::create();
    expect(app->register_application(), "register application");
    auto* w = app->create_window();
    w->present();
    flush_ui();

    test_columns_and_gutter(*w);
    test_save_open_undo(*w, dir);
    test_find_replace(*w);
    test_open_many(*app.get(), dir);
    test_review_fixes(*app.get(), *w, dir);

    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_OPEN");
    g_unsetenv("LUNDUKE_EDIT_TEST_LARGE");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_HITS");
    g_unsetenv("LUNDUKE_EDIT_TEST_CHUNK");
    g_unsetenv("LUNDUKE_EDIT_TEST_HUGE_BYTES");
    g_unsetenv("LUNDUKE_EDIT_TEST_HUGE_UNDO");

    return failures;
  }
};

int EditChecks::failures = 0;

}  // namespace lundukeedit

int main() {
  const int failures = lundukeedit::EditChecks::run();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}
