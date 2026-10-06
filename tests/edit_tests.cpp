// SPDX-License-Identifier: GPL-3.0-or-later

#include "application.hpp"
#include "main_window.hpp"

#include <glib.h>
#include <gtkmm/textiter.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

  static void test_open_many(Application& app, const std::string& dir) {
    const std::string a = dir + "/a.txt";
    const std::string b = dir + "/b.txt";
    const std::string c = dir + "/c.txt";
    write_bytes(a, "file-a");
    write_bytes(b, "file-b");
    write_bytes(c, "file-c");

    if (!app.main_window() || !app.main_window()->get_visible()) {
      auto* w = app.create_window();
      w->present();
      flush_ui();
    }
    auto* current = app.main_window();
    expect(main_window_count(app) == 1, "one window before cancelled open");

    current->buffer()->set_text("unsaved-edits");
    current->buffer()->set_modified(true);
    current->refresh_dirty_from_buffer();
    expect(current->dirty_, "window dirty before cancelled open");
    const std::string kept = current->buffer()->get_text();
    const std::string kept_path = current->file_path_;

    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    app.open_files({c});
    app.open_files({a, b});
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    flush_ui();

    expect(main_window_count(app) == 1, "cancel does not open a second window");
    expect(app.main_window() == current, "cancel keeps the same window");
    expect(current->get_visible(), "dirty window stays visible");
    expect(current->dirty_, "cancel leaves the buffer dirty");
    expect(current->buffer()->get_text() == kept, "cancel keeps unsaved edits");
    expect(current->file_path_ == kept_path, "cancel keeps the open path");
    expect(!window_has_path(app, a), "cancel does not open the first requested file");
    expect(!window_has_path(app, b), "cancel does not open the second requested file");
    expect(!window_has_path(app, c), "cancel does not open the new file");

    // Don't Save still replaces the dirty buffer in the same window.
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
    app.open_files({c});
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    flush_ui();
    expect(main_window_count(app) == 1, "don't save reuses the same window");
    expect(current->file_path_ == c && current->buffer()->get_text() == "file-c",
           "don't save opens the file in place");
    expect(!current->dirty_, "don't save open is clean");

    current->load_seed_sample();
    expect(!current->dirty_, "seed is clean");

    app.open_files({a, b});
    flush_ui();

    bool saw_a = false;
    bool saw_b = false;
    int windows = 0;
    for (auto* win : app.get_windows()) {
      auto* mw = dynamic_cast<MainWindow*>(win);
      if (!mw) {
        continue;
      }
      ++windows;
      if (mw->file_path_ == a && mw->buffer()->get_text() == "file-a") {
        saw_a = true;
      }
      if (mw->file_path_ == b && mw->buffer()->get_text() == "file-b") {
        saw_b = true;
      }
    }
    expect(saw_a, "opened a.txt");
    expect(saw_b, "opened b.txt");
    expect(windows == 2, "each further command-line file opens its own window");
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
