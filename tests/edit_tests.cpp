// SPDX-License-Identifier: GPL-3.0-or-later

#include "application.hpp"
#include "main_window.hpp"

#include <glib.h>
#include <functional>
#include <glibmm/convert.h>
#include <gtkmm/textiter.h>
#include <gtkmm/clipboard.h>
#include <gdkmm/cursor.h>

#include <giomm/file.h>
#include <gtkmm/printoperation.h>

#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <csetjmp>
#include <cmath>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace {

sigjmp_buf hostile_alarm_jmp;

void hostile_alarm(int) { siglongjmp(hostile_alarm_jmp, 1); }

class AlarmGuard {
 public:
  AlarmGuard() {
    struct sigaction sa {};
    sa.sa_handler = hostile_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, &old_);
  }
  ~AlarmGuard() {
    alarm(0);
    sigaction(SIGALRM, &old_, nullptr);
  }
  bool arm(unsigned seconds) {
    if (sigsetjmp(hostile_alarm_jmp, 1) != 0) {
      return false;
    }
    alarm(seconds);
    return true;
  }
  void disarm() { alarm(0); }

 private:
  struct sigaction old_ {};
};

int g_short_write_calls = 0;

ssize_t short_inplace_write(int fd, const void* buf, std::size_t n, bool) {
  if (g_short_write_calls++ == 0 && n > 0) {
    return ::write(fd, buf, 1);
  }
  errno = ENOSPC;
  return -1;
}

int fail_dir_fsync(int) {
  errno = EOPNOTSUPP;
  return -1;
}

struct AccelProbe {
  guint key;
  GdkModifierType mods;
  bool found;
};

gboolean find_accel(GtkAccelKey* key, GClosure*, gpointer data) {
  auto* probe = static_cast<AccelProbe*>(data);
  if (key->accel_key == probe->key &&
      (key->accel_mods & GDK_MODIFIER_MASK) == probe->mods) {
    probe->found = true;
    return TRUE;
  }
  return FALSE;
}

}  // namespace

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
    expect(w.enc_latin1_item_ && w.enc_latin1_item_->get_active(),
           "auto-detect checks the document encoding radio");
    expect(w.status_enc_.get_text() == "Latin-1", "status shows Latin-1");
    w.on_new();
    expect(w.encoding_ == "UTF-8" && w.saved_encoding_ == "UTF-8",
           "new document is UTF-8");
    expect(w.enc_utf8_item_ && w.enc_utf8_item_->get_active(),
           "new document checks the UTF-8 encoding radio");
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
    expect(w.prefer_utf8_,
           "document encoding radio does not change the open preference");
    expect(w.encoding_ == "ISO-8859-1", "explicit latin-1 sets document encoding");
    expect(w.open_latin1_item_ != nullptr, "open-next radio exists");
    w.open_latin1_item_->set_active(true);
    expect(!w.prefer_utf8_ && w.open_charset_ == "ISO-8859-1",
           "open-next latin-1 sets the open preference");
    expect(!app.prefer_utf8() && app.open_charset() == "ISO-8859-1",
           "open preference is application owned");
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
    w.on_new();
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.encoding_ == "UTF-8", "new resets document encoding");
    expect(w.enc_utf8_item_ && w.enc_utf8_item_->get_active(),
           "new checks the UTF-8 encoding radio");
    expect(!w.prefer_utf8_ && w.open_latin1_item_->get_active(),
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
    expect(w.next_print_end(0) == 5001,
           "print keeps a long line together, including its newline");
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

  static void test_hostile_review(Application& app, MainWindow& w,
                                  const std::string& dir) {
    // 1. Short in-place write of a hard-linked file tears the inode and
    // stays dirty. The error says the file may be damaged.
    g_short_write_calls = 0;
    const std::string hard_a = dir + "/tear-a.txt";
    const std::string hard_b = dir + "/tear-b.txt";
    const std::string old_hard = "0123456789abcdef";
    write_bytes(hard_a, old_hard);
    ::unlink(hard_b.c_str());
    expect(::link(hard_a.c_str(), hard_b.c_str()) == 0, "tear hard link");
    w.encoding_ = "UTF-8";
    w.newline_style_ = MainWindow::NewlineStyle::Lf;
    w.buffer()->set_text("xyz");
    MainWindow::test_write_hook_ = short_inplace_write;
    expect(!w.save_to_path(hard_a), "short hard-link write fails");
    MainWindow::test_write_hook_ = nullptr;
    const std::string torn_a = read_bytes(hard_a);
    const std::string torn_b = read_bytes(hard_b);
    expect(torn_a == torn_b, "both hard-link names see the torn inode");
    expect(torn_a != "xyz" && torn_a != old_hard, "inode is neither old nor new");
    expect(w.dirty_, "failed hard-link save stays dirty");
    expect(w.last_save_error_.find("may be damaged") != std::string::npos,
           "short write says the file may be damaged");
    expect(w.last_save_error_.find("/") != std::string::npos,
           "short write names the staged copy");

    // 2. Directory fsync EOPNOTSUPP after rename is still a successful save.
    const std::string fsync_path = dir + "/dir-fsync.txt";
    write_bytes(fsync_path, "before");
    w.buffer()->set_text("after-fsync");
    MainWindow::test_dir_fsync_hook_ = fail_dir_fsync;
    expect(w.save_to_path(fsync_path), "dir fsync EOPNOTSUPP still saves");
    MainWindow::test_dir_fsync_hook_ = nullptr;
    expect(!w.dirty_, "dir fsync failure does not leave the buffer dirty");
    expect(read_bytes(fsync_path) == "after-fsync", "disk matches after dir fsync");
    expect(w.confirm_discard_or_save(), "clean close does not revert the save");
    expect(read_bytes(fsync_path) == "after-fsync",
           "Don't Save is not offered for a successful save");

    // 3. Mixed newlines round-trip. A pasted CR is not doubled on save.
    const std::string mixed = dir + "/mixed-nl.txt";
    const std::string mixed_bytes = std::string("a\nb\r\n", 5);
    write_bytes(mixed, mixed_bytes);
    expect(w.open_file(mixed), "open mixed newlines");
    expect(w.save_to_path(mixed), "save mixed newlines without edits");
    expect(read_bytes(mixed) == mixed_bytes, "mixed newlines are not restyled");
    const std::string stray = dir + "/stray-cr.txt";
    const std::string stray_bytes = std::string("hello\rworld\n", 12);
    write_bytes(stray, stray_bytes);
    expect(w.open_file(stray), "open stray cr");
    expect(w.save_to_path(stray), "save stray cr without edits");
    expect(read_bytes(stray) == stray_bytes, "stray cr is not rewritten as lf");
    const std::string crlf = dir + "/paste-crlf.txt";
    write_bytes(crlf, "a\r\nb\r\n");
    expect(w.open_file(crlf), "open crlf for paste");
    w.buffer()->insert(w.buffer()->end(), "\r\n");
    expect(w.save_to_path(crlf), "save pasted crlf");
    expect(read_bytes(crlf) == "a\r\nb\r\n\r\n",
           "pasted CR is not doubled into CR CR LF");

    // 4. Selection Only and Extend pins do not cover the next file.
    w.buffer()->set_text("one two one");
    w.buffer()->select_range(w.buffer()->begin(),
                             w.buffer()->get_iter_at_offset(3));
    w.find_opts_.search_for = "one";
    w.find_opts_.search_selection_only = true;
    w.find_opts_.extend_selection = true;
    w.find_opts_.search_backwards = false;
    w.find_opts_.wrap_around = false;
    w.find_opts_.start_at_top = false;
    w.find_opts_.entire_word = false;
    w.pin_selection_only_range();
    // Extend starts past the current selection, so this first Find may miss.
    // The pin must still not leak into the next document.
    w.find_match(w.find_opts_, false);
    const std::string sel_other = dir + "/sel-other.txt";
    write_bytes(sel_other, "xx one xx");
    expect(w.open_file(sel_other), "open another file over the pin");
    expect(w.find_opts_.search_selection_only && w.find_opts_.extend_selection,
           "selection-only and extend flags stay on");
    expect(!w.sel_only_range_valid_ && !w.extend_anchor_valid_,
           "open drops the search pins");
    w.on_find_next();
    expect(selection_text(w).empty(),
           "find next does not search outside an empty selection");

    // 5. A fifo is rejected without blocking in open.
    const std::string fifo = dir + "/edit.fifo";
    ::unlink(fifo.c_str());
    expect(::mkfifo(fifo.c_str(), 0600) == 0, "mkfifo");
    {
      AlarmGuard guard;
      if (!guard.arm(2)) {
        expect(false, "fifo open returned without blocking");
      } else {
        const bool opened = w.open_file(fifo);
        guard.disarm();
        expect(!opened, "fifo open fails");
      }
    }
    ::unlink(fifo.c_str());

    // 6. A new file follows umask. 077 must not produce 0644.
    const mode_t old_mask = ::umask(077);
    const std::string secret = dir + "/secret.txt";
    ::unlink(secret.c_str());
    w.encoding_ = "UTF-8";
    w.newline_style_ = MainWindow::NewlineStyle::Lf;
    w.buffer()->set_text("hidden");
    expect(w.save_to_path(secret), "save new file under umask 077");
    struct stat secret_st {};
    expect(::stat(secret.c_str(), &secret_st) == 0, "stat new file");
    expect((secret_st.st_mode & 0777) == 0600, "umask 077 creates mode 0600");
    ::umask(old_mask);

    // 7. user.* xattr survives Ctrl+S on a clean one-link file, and a
    // later dirty save copies it onto the replacement inode.
    const std::string xattr_path = dir + "/xattr.txt";
    write_bytes(xattr_path, "body");
    const char note[] = "keep";
    const bool xattr_set =
        ::setxattr(xattr_path.c_str(), "user.note", note, 4, 0) == 0;
    expect(xattr_set, "set user.note");
    unsigned char acl[36] = {
        0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    const bool acl_set =
        ::setxattr(xattr_path.c_str(), "system.posix_acl_access", acl,
                   sizeof(acl), 0) == 0;
    unsigned char acl_stored[64] = {};
    ssize_t acl_stored_n = 0;
    if (acl_set) {
      acl_stored_n = ::getxattr(xattr_path.c_str(), "system.posix_acl_access",
                                acl_stored, sizeof(acl_stored));
    }
    expect(w.open_file(xattr_path), "open xattr file");
    w.on_save();
    char got[8] = {};
    const ssize_t got_n =
        ::getxattr(xattr_path.c_str(), "user.note", got, sizeof(got));
    expect(got_n == 4 && std::string(got, got + 4) == "keep",
           "clean save keeps user.note");
    if (acl_set && acl_stored_n > 0) {
      unsigned char got_acl[64] = {};
      const ssize_t acl_n = ::getxattr(xattr_path.c_str(),
                                       "system.posix_acl_access", got_acl,
                                       sizeof(got_acl));
      expect(acl_n == acl_stored_n &&
                 std::memcmp(got_acl, acl_stored,
                             static_cast<std::size_t>(acl_stored_n)) == 0,
             "clean save keeps the posix acl");
    }
    w.buffer()->insert(w.buffer()->end(), "!");
    expect(w.save_to_path(xattr_path), "dirty save copies xattrs");
    std::memset(got, 0, sizeof(got));
    const ssize_t got_n2 =
        ::getxattr(xattr_path.c_str(), "user.note", got, sizeof(got));
    expect(got_n2 == 4 && std::string(got, got + 4) == "keep",
           "dirty save keeps user.note");

    // 8. A 255-character name saves, and a writable file in a
    // non-writable directory saves in place.
    const std::string long_name(255, 'a');
    const std::string long_path = dir + "/" + long_name;
    write_bytes(long_path, "old");
    expect(w.open_file(long_path), "open 255-character name");
    w.buffer()->insert(w.buffer()->end(), "!");
    expect(w.save_to_path(long_path), "save 255-character name");
    expect(read_bytes(long_path) == "old!", "255-character name was updated");

    const std::string ro_dir = dir + "/rodir";
    const std::string ro_file = ro_dir + "/f.txt";
    ::chmod(ro_dir.c_str(), 0755);
    ::unlink(ro_file.c_str());
    ::rmdir(ro_dir.c_str());
    expect(::mkdir(ro_dir.c_str(), 0755) == 0, "mkdir rodir");
    write_bytes(ro_file, "hi");
    expect(::chmod(ro_file.c_str(), 0644) == 0, "chmod file 0644");
    expect(::chmod(ro_dir.c_str(), 0555) == 0, "chmod dir 0555");
    expect(w.open_file(ro_file), "open file in read-only directory");
    w.buffer()->insert(w.buffer()->end(), "!");
    expect(w.save_to_path(ro_file), "save writable file in read-only directory");
    expect(::chmod(ro_dir.c_str(), 0755) == 0, "restore directory mode");
    expect(read_bytes(ro_file) == "hi!", "in-place save updated the file");

    // 9. Shift+Insert pastes. Keypad Insert toggles overwrite in the status.
    w.buffer()->set_text("abc");
    w.buffer()->place_cursor(w.buffer()->get_iter_at_offset(1));
    if (w.text_view_.get_overwrite()) {
      w.text_view_.set_overwrite(false);
    }
    w.sync_overwrite_status();
    auto clip = Gtk::Clipboard::get();
    clip->set_text("hello");
    flush_ui();
    gtk_widget_grab_focus(GTK_WIDGET(w.text_view_.gobj()));
    gtk_window_set_focus(GTK_WINDOW(w.gobj()),
                         GTK_WIDGET(w.text_view_.gobj()));
    flush_ui();
    GdkEventKey press {};
    press.type = GDK_KEY_PRESS;
    press.window = gtk_widget_get_window(GTK_WIDGET(w.gobj()));
    press.keyval = GDK_KEY_Insert;
    press.state = GDK_SHIFT_MASK;
    press.send_event = 1;
    w.on_key_press_event(&press);
    expect(!w.text_view_.get_overwrite() &&
               w.status_mode_.get_text() == "Insert",
           "shift+insert does not flip overwrite");
    // A synthetic GdkEvent does not travel gtk_window_propagate_key_event
    // in this harness. Activate the text-view binding Shift+Insert maps to
    // once the window no longer swallows the key.
    gtk_bindings_activate(G_OBJECT(w.text_view_.gobj()), GDK_KEY_Insert,
                          GDK_SHIFT_MASK);
    flush_ui();
    expect(w.buffer()->get_text().find("hello") != Glib::ustring::npos,
           "shift+insert pastes");
    expect(!w.text_view_.get_overwrite() && w.status_mode_.get_text() == "Insert",
           "shift+insert does not flip overwrite");
    press.keyval = GDK_KEY_KP_Insert;
    press.state = 0;
    w.on_key_press_event(&press);
    expect(w.text_view_.get_overwrite(), "keypad insert toggles overwrite");
    expect(w.status_mode_.get_text() == "Overwrite",
           "keypad insert updates the status");
    w.text_view_.set_overwrite(false);
    w.sync_overwrite_status();

    // 10. Middle-click of a too-large primary selection does not insert.
    g_setenv("LUNDUKE_EDIT_TEST_MAX_PASTE", "32", TRUE);
    w.buffer()->set_text("keep");
    const std::string huge(256 * 1024, 'Q');
    auto primary = Gtk::Clipboard::get(GDK_SELECTION_PRIMARY);
    primary->set_text(huge);
    flush_ui();
    GdkEventButton button {};
    button.type = GDK_BUTTON_PRESS;
    button.button = 2;
    button.x = 4;
    button.y = 4;
    button.window = gtk_text_view_get_window(GTK_TEXT_VIEW(w.text_view_.gobj()),
                                             GTK_TEXT_WINDOW_TEXT);
    {
      AlarmGuard guard;
      if (!guard.arm(3)) {
        expect(false, "middle-click paste returned");
      } else {
        expect(w.on_text_button_press(&button), "middle-click is handled");
        guard.disarm();
      }
    }
    expect(w.buffer()->get_text() == "keep",
           "oversize primary selection is not inserted");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_PASTE");

    // 11. Cancel after the first Replace All idle rolls the edit back.
    g_setenv("LUNDUKE_EDIT_TEST_CHUNK", "1", TRUE);
    const Glib::ustring before_replace = "aaa";
    w.buffer()->set_text(before_replace);
    FindOptions repl;
    repl.search_for = "a";
    repl.replace_with = "b";
    w.start_replace_all(repl, nullptr);
    w.on_find_idle();
    w.cancel_find_scan();
    expect(w.buffer()->get_text() == before_replace,
           "cancelled replace all restores the buffer");
    g_unsetenv("LUNDUKE_EDIT_TEST_CHUNK");

    // 12. An open delivered while the large-file confirm is up is queued.
    auto* blank = app.create_window();
    blank->present();
    flush_ui();
    app.note_window_focus(blank);
    const std::string big = dir + "/queued-big.txt";
    const std::string other = dir + "/queued-other.txt";
    write_bytes(big, std::string(20, 'Z'));
    write_bytes(other, "other-text");
    g_setenv("LUNDUKE_EDIT_TEST_MAX_OPEN", "8", TRUE);
    g_setenv("LUNDUKE_EDIT_TEST_LARGE", "1", TRUE);
    MainWindow::test_during_large_confirm_ = [&app, &other](MainWindow*) {
      app.open_files({other});
    };
    expect(blank->open_file(big), "large open proceeds");
    MainWindow::test_during_large_confirm_ = nullptr;
    g_unsetenv("LUNDUKE_EDIT_TEST_LARGE");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_OPEN");
    expect(blank->buffer()->get_text() == std::string(20, 'Z'),
           "queued open does not replace the large file");
    expect(blank->file_path_ == big, "large file keeps the window");
    expect(window_has_path(app, other), "queued path opens in another window");

    // Above the hard cap there is no "open anyway", even when the large
    // confirm would allow the 32 MiB path. A file between the two caps
    // still confirms and loads.
    g_setenv("LUNDUKE_EDIT_TEST_MAX_OPEN", "8", TRUE);
    g_setenv("LUNDUKE_EDIT_TEST_MAX_OPEN_HARD", "16", TRUE);
    g_setenv("LUNDUKE_EDIT_TEST_LARGE", "1", TRUE);
    const std::string above_hard = dir + "/above-hard.txt";
    write_bytes(above_hard, std::string(20, 'Q'));
    w.buffer()->set_text("keep-hard");
    bool asked = false;
    MainWindow::test_during_large_confirm_ = [&asked](MainWindow*) {
      asked = true;
    };
    expect(!w.open_file(above_hard), "file above the hard cap is refused");
    expect(!asked, "hard cap does not offer an unlimited confirm");
    expect(w.buffer()->get_text() == "keep-hard",
           "hard cap refusal keeps the buffer");
    expect(w.last_open_error_.find("will not be opened") != std::string::npos,
           "hard cap explains the refusal");
    const std::string between = dir + "/between-caps.txt";
    write_bytes(between, std::string(12, 'M'));
    expect(w.open_file(between), "file between confirm and hard cap still opens");
    expect(asked, "confirm still runs below the hard cap");
    expect(w.buffer()->get_text() == std::string(12, 'M'),
           "between-cap bytes loaded");
    MainWindow::test_during_large_confirm_ = nullptr;
    g_unsetenv("LUNDUKE_EDIT_TEST_LARGE");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_OPEN");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_OPEN_HARD");

    // 13. A path already open is presented in the window that has it.
    // An unchanged file is not loaded into the second window. A changed
    // file is not replaced when the user cancels.
    const std::string notes = dir + "/notes.txt";
    write_bytes(notes, "notes-v1");
    expect(w.open_file(notes), "open notes");
    auto* second = app.create_window();
    second->present();
    flush_ui();
    expect(second->open_file(notes), "second window presents the window that has the path");
    expect(w.buffer()->get_text() == "notes-v1", "unchanged file stays as it was");
    expect(w.file_path_ == notes, "the original window keeps the path");
    expect(second->file_path_ != notes, "second window is not a stale copy");
    w.buffer()->set_text("notes-v2");
    expect(w.save_to_path(notes), "save the only window on this path");
    expect(read_bytes(notes) == "notes-v2", "save wrote notes-v2");
    expect(second->file_path_.empty() || second->dirty_,
           "the other window is not a clean stale buffer");
    write_bytes(notes, "external");
    struct stat notes_st {};
    expect(::stat(notes.c_str(), &notes_st) == 0, "stat notes");
    struct timespec times[2] = {};
    times[0].tv_sec = notes_st.st_atim.tv_sec;
    times[0].tv_nsec = notes_st.st_atim.tv_nsec;
    times[1].tv_sec = notes_st.st_mtim.tv_sec + 10;
    times[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, notes.c_str(), times, 0);
    w.buffer()->set_text("mine");
    g_setenv("LUNDUKE_EDIT_TEST_REPLACE", "cancel", TRUE);
    expect(!w.save_to_path(notes), "cancel leaves a changed file alone");
    g_unsetenv("LUNDUKE_EDIT_TEST_REPLACE");
    expect(read_bytes(notes) == "external", "cancelled save did not replace disk");

    // 14. The encoding radio, the status, and the bytes on disk agree.
    const std::string latin = dir + "/hostile-latin.txt";
    const std::string latin_bytes("caf\xE9", 4);
    write_bytes(latin, latin_bytes);
    expect(w.open_file(latin), "open latin-1 cafe");
    expect(w.encoding_ == "ISO-8859-1", "document encoding is latin-1");
    expect(w.enc_latin1_item_ && w.enc_latin1_item_->get_active(),
           "latin-1 radio matches the document");
    expect(w.status_enc_.get_text() == "Latin-1", "status encoding is Latin-1");
    w.buffer()->insert(w.buffer()->end(), "!");
    expect(w.save_to_path(latin), "save latin-1 cafe");
    expect(read_bytes(latin) == std::string("caf\xE9!", 5),
           "save writes latin-1 bytes, not UTF-8");
    if (w.open_latin1_item_) {
      w.open_latin1_item_->set_active(true);
    }
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
    w.on_new();
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.enc_utf8_item_ && w.enc_utf8_item_->get_active(),
           "new document checks UTF-8");
    expect(w.status_enc_.get_text() == "UTF-8", "new document status is UTF-8");
    const Glib::ustring e_acute("caf\xc3\xa9");
    w.buffer()->set_text(e_acute);
    const std::string utf_path = dir + "/hostile-utf8.txt";
    expect(w.save_to_path(utf_path), "save new document");
    expect(read_bytes(utf_path) == std::string("caf\xc3\xa9", 5),
           "new document writes UTF-8");

    // 15. Match Entire Words yields inside one chunk.
    g_setenv("LUNDUKE_EDIT_TEST_CHUNK", "50", TRUE);
    w.buffer()->set_text(std::string(4000, 'a'));
    FindOptions words;
    words.search_for = "a";
    words.entire_word = true;
    words.start_at_top = true;
    w.start_find_all(words, nullptr);
    expect(w.on_find_idle(), "entire-word scan yields");
    expect(w.find_scan_.active, "entire-word scan is still active");
    expect(w.find_scan_.slice_steps > 0 && w.find_scan_.slice_steps <= 50,
           "one idle stays inside the chunk");
    expect(w.find_scan_.slice_steps < w.buffer()->get_char_count(),
           "one idle does not walk the whole buffer");
    w.cancel_find_scan();
    g_unsetenv("LUNDUKE_EDIT_TEST_CHUNK");

    // 16. Printed tabs come from the print context, in pango units.
    // A long tabbed line is one layout sliced across pages.
    w.apply_tab_width(8);
    w.buffer()->set_text("\t");
    const std::string tab_pdf = dir + "/tab-print.pdf";
    ::unlink(tab_pdf.c_str());
    int context_tab = -1;
    auto op = Gtk::PrintOperation::create();
    op->set_export_filename(tab_pdf);
    op->signal_begin_print().connect(
        [&w, op](const Glib::RefPtr<Gtk::PrintContext>& context) {
          w.on_begin_print(context);
          op->set_n_pages(static_cast<int>(w.print_page_breaks_.size()) + 1);
        });
    op->signal_draw_page().connect(
        [&w, &context_tab](const Glib::RefPtr<Gtk::PrintContext>& context,
                           int page) {
          w.on_draw_page(context, page);
          if (context_tab < 0 && context) {
            auto layout = context->create_pango_layout();
            layout->set_font_description(w.font_desc_);
            layout->set_text(Glib::ustring(std::max(1, w.tab_width_), ' '));
            int tw = 0;
            int th = 0;
            layout->get_size(tw, th);
            (void)th;
            context_tab = tw;
          }
        });
    bool printed = false;
    try {
      const auto result = op->run(Gtk::PRINT_OPERATION_ACTION_EXPORT, w);
      printed = result == Gtk::PRINT_OPERATION_RESULT_APPLY;
    } catch (const Gtk::PrintError&) {
      printed = false;
    }
    expect(printed, "tab print export succeeds");
    expect(context_tab > 0 && w.last_print_tab_pos_ == context_tab,
           "print tab matches the print context");
    expect(!w.print_tabs_in_pixels_, "print tabs are pango units");
    auto screen = w.text_view_.create_pango_layout(
        std::string(static_cast<std::size_t>(std::max(1, w.tab_width_)), ' '));
    screen->set_font_description(w.font_desc_);
    int pixel_w = 0;
    int pixel_h = 0;
    screen->get_pixel_size(pixel_w, pixel_h);
    (void)pixel_h;
    expect(w.last_print_tab_pos_ != pixel_w,
           "print tab is not the on-screen pixel width");

    std::string long_line(8000, 'b');
    long_line[5000] = '\t';
    w.buffer()->set_text(long_line);
    const std::string long_pdf = dir + "/long-print.pdf";
    ::unlink(long_pdf.c_str());
    auto op2 = Gtk::PrintOperation::create();
    op2->set_export_filename(long_pdf);
    op2->signal_begin_print().connect(
        [&w, op2](const Glib::RefPtr<Gtk::PrintContext>& context) {
          w.on_begin_print(context);
          op2->set_n_pages(static_cast<int>(w.print_page_breaks_.size()) + 1);
        });
    op2->signal_draw_page().connect(
        [&w](const Glib::RefPtr<Gtk::PrintContext>& context, int page) {
          w.on_draw_page(context, page);
        });
    try {
      op2->run(Gtk::PRINT_OPERATION_ACTION_EXPORT, w);
    } catch (const Gtk::PrintError&) {
    }
    bool shared_layout = false;
    if (w.print_pages_.size() >= 2) {
      for (const auto& slice_a : w.print_pages_[0].slices) {
        for (const auto& slice_b : w.print_pages_[1].slices) {
          if (slice_a.layout && slice_a.layout == slice_b.layout) {
            shared_layout = true;
          }
        }
      }
    }
    expect(w.print_pages_.size() >= 2 && shared_layout,
           "a long tabbed line is one layout sliced across pages");

    // 17. Quit saves the first dirty window and stops when the second cancels.
    // A save failure also aborts quit.
    for (auto* win : app.get_windows()) {
      if (auto* mw = dynamic_cast<MainWindow*>(win)) {
        if (mw->buffer()) {
          mw->buffer()->set_modified(false);
        }
        mw->set_dirty(false);
      }
    }
    std::vector<MainWindow*> order;
    for (auto* win : app.get_windows()) {
      if (auto* mw = dynamic_cast<MainWindow*>(win)) {
        if (mw->get_visible()) {
          order.push_back(mw);
        }
      }
    }
    expect(order.size() >= 2, "quit test has two windows");
    if (order.size() >= 2) {
      MainWindow* first = order[0];
      MainWindow* second = order[1];
      const std::string quit_path = dir + "/quit-first.txt";
      write_bytes(quit_path, "old");
      first->encoding_ = "UTF-8";
      first->newline_style_ = MainWindow::NewlineStyle::Lf;
      first->buffer()->set_text("saved-from-quit");
      expect(first->save_to_path(quit_path), "seed quit file");
      first->buffer()->insert(first->buffer()->end(), "!");
      second->buffer()->set_text("still-dirty");
      second->buffer()->set_modified(true);
      second->refresh_dirty_from_buffer();
      MainWindow::test_discard_choice_ = [first, second](MainWindow* mw) {
        if (mw == first) {
          return "save";
        }
        if (mw == second) {
          return "cancel";
        }
        return "discard";
      };
      expect(!app.confirm_quit(), "cancel on the second window aborts quit");
      expect(!first->dirty_, "first window was saved");
      expect(read_bytes(quit_path) == "saved-from-quit!",
             "first window reached disk");
      expect(second->dirty_ && second->get_visible(),
             "second window stays dirty and visible");
      MainWindow::test_discard_choice_ = nullptr;

      for (auto* win : app.get_windows()) {
        if (auto* mw = dynamic_cast<MainWindow*>(win)) {
          if (mw->buffer()) {
            mw->buffer()->set_modified(false);
          }
          mw->set_dirty(false);
        }
      }
      first->file_path_ = dir + "/no-such-dir/out.txt";
      first->buffer()->set_text("cannot-save");
      first->buffer()->set_modified(true);
      first->refresh_dirty_from_buffer();
      MainWindow::test_discard_choice_ = [](MainWindow*) { return "save"; };
      expect(!app.confirm_quit(), "save failure aborts quit");
      expect(first->dirty_ && first->get_visible(),
             "failed save keeps the dirty window");
      MainWindow::test_discard_choice_ = nullptr;
    }

    MainWindow::test_write_hook_ = nullptr;
    MainWindow::test_dir_fsync_hook_ = nullptr;
    MainWindow::test_during_large_confirm_ = nullptr;
    MainWindow::test_discard_choice_ = nullptr;
    w.find_opts_.search_selection_only = false;
    w.find_opts_.extend_selection = false;
    w.find_opts_.entire_word = false;
  }

  static void bump_mtime(const std::string& path) {
    // Each call sets a distinct absolute mtime. Adding a fixed offset to
    // the current mtime collides when two writes land in the same second
    // and the editor already stored the previous bumped stamp.
    static std::time_t stamp = 1'800'000'000;
    stamp += 60;
    struct timespec times[2] = {};
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = stamp;
    times[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, path.c_str(), times, 0);
  }

  static double printed_page_height(const MainWindow& w, std::size_t page) {
    if (page >= w.print_pages_.size()) {
      return 0.0;
    }
    double total = 0.0;
    for (const auto& slice : w.print_pages_[page].slices) {
      if (!slice.layout) {
        continue;
      }
      const int n = slice.layout->get_line_count();
      const int begin = std::max(0, slice.row_begin);
      const int end = std::min(n, slice.row_end);
      for (int i = begin; i < end; ++i) {
        auto line = slice.layout->get_line(i);
        if (!line) {
          continue;
        }
        Pango::Rectangle ink;
        Pango::Rectangle logical;
        line->get_extents(ink, logical);
        total += static_cast<double>(logical.get_height()) / Pango::SCALE;
      }
    }
    return total;
  }

  static bool page_fits(const MainWindow& w, double page_height, std::string& why) {
    if (w.print_pages_.empty() || page_height < 1.0) {
      why = "no pages";
      return false;
    }
    double all = 0.0;
    for (std::size_t i = 0; i < w.print_pages_.size(); ++i) {
      const double h = printed_page_height(w, i);
      all += h;
      if (h > page_height + 1.0) {
        why = "page " + std::to_string(i) + " is " + std::to_string(h) +
              " pt on a " + std::to_string(page_height) + " pt page";
        return false;
      }
    }
    if (all <= page_height + 1.0) {
      why = "text did not wrap onto more than one page of height";
      return false;
    }
    return true;
  }

  static void test_round3(Application& app, MainWindow& w, const std::string& dir) {
    (void)app;
    // 1. A null byte is refused. The previous buffer stays, and the file
    // is not replaced by the empty view set_text would have left behind.
    const std::string nul_mid = dir + "/nul-mid.txt";
    const std::string nul_lead = dir + "/nul-lead.txt";
    const std::string nul_tail = dir + "/nul-tail.txt";
    const std::string nul_mid_bytes("hello\0world", 11);
    const std::string nul_lead_bytes("\0hello", 6);
    const std::string nul_tail_bytes("hello\0", 6);
    write_bytes(nul_mid, nul_mid_bytes);
    write_bytes(nul_lead, nul_lead_bytes);
    write_bytes(nul_tail, nul_tail_bytes);
    const std::string safe = dir + "/nul-safe.txt";
    write_bytes(safe, "safe");
    expect(w.open_file(safe), "open a stand-in before the null file");
    expect(!w.dirty_, "stand-in is clean");
    for (const auto& sample : {nul_mid, nul_lead, nul_tail}) {
      const std::string disk = read_bytes(sample);
      expect(!w.open_file(sample), "null byte is refused");
      expect(w.buffer()->get_text() == "safe", "null open keeps the buffer");
      expect(w.file_path_ == safe, "null open does not take the path");
      expect(!w.dirty_, "null open does not clear or set the dirty flag");
      expect(w.last_open_error_.find("null byte") != std::string::npos,
             "null open explains why");
      expect(read_bytes(sample) == disk, "null file is not replaced");
    }
    w.buffer()->insert(w.buffer()->end(), "!");
    expect(w.save_to_path(safe), "save the stand-in after a refused null open");
    expect(read_bytes(safe) == "safe!", "save wrote the stand-in");
    expect(read_bytes(nul_mid) == nul_mid_bytes, "save did not wipe the null file");

    // 2. Middle-click pastes once, at the pointer, and leaves a selection
    // that the click is outside of.
    w.encoding_ = "UTF-8";
    w.newline_style_ = MainWindow::NewlineStyle::Lf;
    w.buffer()->set_text("hello world");
    // The view owns PRIMARY. Clipboard::set_text would take that ownership
    // and clear the highlight before the click, which is not the user path.
    w.text_view_.grab_focus();
    w.buffer()->select_range(w.buffer()->begin(),
                             w.buffer()->get_iter_at_offset(5));
    flush_ui();
    expect(selection_text(w) == "hello", "selection is in place before the click");
    GdkEventButton press {};
    press.type = GDK_BUTTON_PRESS;
    press.button = 2;
    press.x = 10000;
    press.y = 8;
    press.window = gtk_text_view_get_window(GTK_TEXT_VIEW(w.text_view_.gobj()),
                                            GTK_TEXT_WINDOW_TEXT);
    expect(w.on_text_button_press(&press), "middle-click press is handled");
    expect(w.buffer()->get_text() == "hello worldhello",
           "middle-click inserts once at the pointer");
    expect(selection_text(w) == "hello",
           "middle-click outside the selection leaves it in place");
    GdkEventButton release {};
    release.type = GDK_BUTTON_RELEASE;
    release.button = 2;
    release.x = 10000;
    release.y = 8;
    release.window = press.window;
    expect(w.on_text_button_release(&release), "middle-click release is swallowed");
    expect(w.buffer()->get_text() == "hello worldhello",
           "release does not paste a second time");

    w.buffer()->set_text("ABCDEFGHIJ");
    w.buffer()->place_cursor(w.buffer()->begin());
    flush_ui();
    auto primary = Gtk::Clipboard::get(GDK_SELECTION_PRIMARY);
    primary->set_text("ZZ");
    flush_ui();
    expect(w.on_text_button_press(&press), "middle-click with no selection");
    expect(w.buffer()->get_text() == "ABCDEFGHIJZZ",
           "middle-click with no selection inserts at the pointer");

    // A real button-press through the widget must not also run GTK's paste.
    w.buffer()->set_text("hello world");
    w.text_view_.grab_focus();
    w.buffer()->select_range(w.buffer()->begin(),
                             w.buffer()->get_iter_at_offset(5));
    flush_ui();
    if (press.window != nullptr) {
      gtk_widget_event(GTK_WIDGET(w.text_view_.gobj()),
                       reinterpret_cast<GdkEvent*>(&press));
      flush_ui();
      expect(w.buffer()->get_text() == "hello worldhello",
             "widget button-press pastes the primary selection once");
    }

    // 3 and 8. Mixed endings stay with their lines, and the status bar
    // counts the bytes a save would write.
    const std::string mixed = dir + "/round3-mixed.txt";
    const std::string mixed_bytes("a\nb\r\n", 5);
    write_bytes(mixed, mixed_bytes);
    expect(w.open_file(mixed), "open mixed endings");
    expect(w.status_bytes_.get_text() == "5 bytes",
           "mixed status matches the file");
    expect(w.source_lines_.size() >= 2 && w.source_lines_[0].kind == 'n' &&
               w.source_lines_[1].kind == 'c',
           "per-line endings are LF then CRLF");
    w.buffer()->insert(w.buffer()->begin(), "Q");
    expect(w.status_bytes_.get_text() == "6 bytes",
           "inline edit counts the original LF");
    expect(w.save_to_path(mixed), "save inline mixed edit");
    expect(read_bytes(mixed) == "Qa\nb\r\n", "inline edit keeps each ending");
    expect(w.status_bytes_.get_text() == "6 bytes",
           "status matches the saved mixed file");

    write_bytes(mixed, mixed_bytes);
    bump_mtime(mixed);
    expect(w.open_file(mixed), "reopen original mixed file");
    w.buffer()->insert(w.buffer()->begin(), "Z\n");
    expect(w.save_to_path(mixed), "save inserted line");
    expect(read_bytes(mixed) == "Z\r\na\nb\r\n",
           "new line uses the dominant ending and a stays LF");
    w.on_undo();
    expect(w.buffer()->get_text() == "a\nb\n", "undo removes the inserted line");
    expect(w.source_lines_.size() >= 2 && w.source_lines_[0].kind == 'n' &&
               w.source_lines_[1].kind == 'c',
           "undo restores the line endings");

    write_bytes(mixed, mixed_bytes);
    bump_mtime(mixed);
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
    expect(w.open_file(mixed), "reopen mixed file before delete");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    w.buffer()->erase(w.buffer()->begin(), w.buffer()->get_iter_at_line(1));
    expect(w.save_to_path(mixed), "save after deleting the first line");
    expect(read_bytes(mixed) == "b\r\n", "deleted line drops its ending only");

    // 4. Closing Find drops the hidden selection-only range. Find Next
    // uses the selection that is visible now.
    w.buffer()->set_text("one two one two");
    w.buffer()->select_range(w.buffer()->begin(),
                             w.buffer()->get_iter_at_offset(7));
    w.find_opts_.search_for = "one";
    w.find_opts_.search_selection_only = true;
    w.find_opts_.extend_selection = true;
    w.find_opts_.wrap_around = true;
    w.find_opts_.start_at_top = false;
    w.find_opts_.search_backwards = false;
    w.find_opts_.entire_word = false;
    w.find_opts_.case_sensitive = false;
    w.pin_selection_only_range();
    w.extend_anchor_valid_ = true;
    w.on_find_dialog_hidden();
    expect(!w.sel_only_range_valid_, "closing Find drops the selection pin");
    expect(!w.extend_anchor_valid_, "closing Find drops the extend anchor");
    w.buffer()->place_cursor(w.buffer()->end());
    w.on_find_next();
    expect(selection_text(w).empty(),
           "Find Next does not jump back into the old range");
    expect(w.last_notice_ == "No text is selected.",
           "Find Next without a selection says so");
    // Extend stays available as an option. It is off here so the hit itself
    // is visible; the anchor that would grow from the old range is already gone.
    w.find_opts_.extend_selection = false;
    w.buffer()->select_range(w.buffer()->get_iter_at_offset(8),
                             w.buffer()->end());
    w.on_find_next();
    expect(selection_offset(w) == 8 && selection_text(w) == "one",
           "Find Next pins the current selection");
    w.find_opts_.search_selection_only = false;
    w.find_opts_.extend_selection = false;

    // 5. Open failures name the reason, and the path, and keep the buffer.
    const std::string denied = dir + "/round3-denied.txt";
    write_bytes(denied, "secret");
    expect(::chmod(denied.c_str(), 0000) == 0, "chmod 000");
    w.buffer()->set_text("kept");
    expect(!w.open_file(denied), "unreadable file is refused");
    expect(w.buffer()->get_text() == "kept", "unreadable open keeps the buffer");
    expect(w.last_open_error_.find("Permission denied") != std::string::npos,
           "unreadable open says permission denied");
    expect(w.last_open_error_.find(denied) != std::string::npos,
           "unreadable open still names the path");
    expect(::chmod(denied.c_str(), 0644) == 0, "restore mode");

    const std::string missing = dir + "/round3-missing.txt";
    ::unlink(missing.c_str());
    expect(!w.open_file(missing), "missing file is refused");
    expect(w.last_open_error_.find("No such file") != std::string::npos,
           "missing open says the file is not there");
    expect(w.last_open_error_.find(missing) != std::string::npos,
           "missing open names the path");
    expect(w.buffer()->get_text() == "kept", "missing open keeps the buffer");

    expect(!w.open_file(dir), "directory open is refused");
    expect(w.last_open_error_.find("That path is a directory") != std::string::npos,
           "directory open says it is a directory");
    expect(w.last_open_error_.find(dir) != std::string::npos,
           "directory open names the path");
    expect(w.buffer()->get_text() == "kept", "directory open keeps the buffer");

    // 6. Wide glyphs and a proportional font paginate by measured rows.
    const Pango::FontDescription previous_font = w.font_desc_;
    w.apply_font(Pango::FontDescription("Sans 18"));
    std::string wides;
    for (int i = 0; i < 60; ++i) {
      wides.append(50, 'W');
      wides.push_back('\n');
    }
    w.buffer()->set_text(wides);
    double page_height = 0.0;
    const std::string wide_pdf = dir + "/round3-wide.pdf";
    ::unlink(wide_pdf.c_str());
    auto wide_op = Gtk::PrintOperation::create();
    wide_op->set_export_filename(wide_pdf);
    wide_op->signal_begin_print().connect(
        [&w, &page_height, wide_op](const Glib::RefPtr<Gtk::PrintContext>& context) {
          page_height = context->get_height();
          w.on_begin_print(context);
          wide_op->set_n_pages(static_cast<int>(w.print_page_breaks_.size()) + 1);
        });
    wide_op->signal_draw_page().connect(
        [&w](const Glib::RefPtr<Gtk::PrintContext>& context, int page) {
          w.on_draw_page(context, page);
        });
    bool wide_printed = false;
    try {
      wide_printed = wide_op->run(Gtk::PRINT_OPERATION_ACTION_EXPORT, w) ==
                     Gtk::PRINT_OPERATION_RESULT_APPLY;
    } catch (const Gtk::PrintError&) {
      wide_printed = false;
    }
    expect(wide_printed, "wide-line print exports");
    std::string wide_why;
    expect(page_fits(w, page_height, wide_why), wide_why.c_str());

    w.apply_font(Pango::FontDescription("Sans 16"));
    std::string cjk;
    const std::string han(reinterpret_cast<const char*>(u8"漢"));
    for (int i = 0; i < 40; ++i) {
      for (int c = 0; c < 40; ++c) {
        cjk += han;
      }
      cjk.push_back('\n');
    }
    w.buffer()->set_text(cjk);
    page_height = 0.0;
    const std::string cjk_pdf = dir + "/round3-cjk.pdf";
    ::unlink(cjk_pdf.c_str());
    auto cjk_op = Gtk::PrintOperation::create();
    cjk_op->set_export_filename(cjk_pdf);
    cjk_op->signal_begin_print().connect(
        [&w, &page_height, cjk_op](const Glib::RefPtr<Gtk::PrintContext>& context) {
          page_height = context->get_height();
          w.on_begin_print(context);
          cjk_op->set_n_pages(static_cast<int>(w.print_page_breaks_.size()) + 1);
        });
    cjk_op->signal_draw_page().connect(
        [&w](const Glib::RefPtr<Gtk::PrintContext>& context, int page) {
          w.on_draw_page(context, page);
        });
    try {
      cjk_op->run(Gtk::PRINT_OPERATION_ACTION_EXPORT, w);
    } catch (const Gtk::PrintError&) {
    }
    std::string cjk_why;
    expect(page_fits(w, page_height, cjk_why), cjk_why.c_str());
    w.apply_font(previous_font);

    // 7. Reload reads the copy on disk. Opening the same path again
    // re-reads a clean buffer when the file changed, and asks first when
    // the buffer is dirty.
    const std::string notes = dir + "/round3-notes.txt";
    write_bytes(notes, "notes-v1");
    expect(w.open_file(notes), "open notes for reload");
    write_bytes(notes, "notes-v2");
    bump_mtime(notes);
    expect(w.open_file(notes), "clean reopen of a changed file");
    expect(w.buffer()->get_text() == "notes-v2", "clean reopen loaded the disk copy");
    expect(!w.dirty_, "reloaded buffer is clean");

    w.buffer()->insert(w.buffer()->end(), "!");
    expect(w.dirty_, "edit after reload is dirty");
    write_bytes(notes, "notes-v3");
    bump_mtime(notes);
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    expect(!w.open_file(notes), "dirty reopen can be cancelled");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.buffer()->get_text() == "notes-v2!", "cancelled reopen keeps the buffer");
    expect(read_bytes(notes) == "notes-v3", "cancelled reopen leaves disk alone");

    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
    expect(w.open_file(notes), "dirty reopen can discard");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.buffer()->get_text() == "notes-v3", "discard reopen loads disk");

    w.buffer()->insert(w.buffer()->end(), "!");
    write_bytes(notes, "external");
    bump_mtime(notes);
    g_setenv("LUNDUKE_EDIT_TEST_REPLACE", "reload", TRUE);
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
    expect(!w.save_to_path(notes), "reload does not write the buffer");
    g_unsetenv("LUNDUKE_EDIT_TEST_REPLACE");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(read_bytes(notes) == "external", "reload kept the disk copy");
    expect(w.buffer()->get_text() == "external", "reload replaced the buffer");
    expect(!w.dirty_, "reload leaves a clean buffer");

    w.buffer()->set_text("mine");
    w.buffer()->set_modified(true);
    w.refresh_dirty_from_buffer();
    write_bytes(notes, "other");
    bump_mtime(notes);
    g_setenv("LUNDUKE_EDIT_TEST_REPLACE", "reload", TRUE);
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "save", TRUE);
    expect(w.save_to_path(notes), "reload prompt can still save");
    g_unsetenv("LUNDUKE_EDIT_TEST_REPLACE");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.last_prompt_default_ == Gtk::RESPONSE_REJECT,
           "reload follow-up focuses Don't Save");
    expect(w.last_prompt_primary_.find("reloading") != Glib::ustring::npos,
           "reload follow-up says it is a reload");
    expect(w.last_prompt_secondary_.find("cancel the reload") !=
               Glib::ustring::npos,
           "reload follow-up says Save cancels the reload");
    expect(read_bytes(notes) == "mine", "save from the reload prompt wrote the buffer");
    expect(w.buffer()->get_text() == "mine", "that save does not load the disk copy");
    expect(w.last_notice_.find("Reload cancelled") != std::string::npos,
           "saving from the reload prompt says the reload was cancelled");

    // 9. Cancelling the large-file question is not an error. The hard cap
    // still is.
    g_setenv("LUNDUKE_EDIT_TEST_MAX_OPEN", "8", TRUE);
    g_setenv("LUNDUKE_EDIT_TEST_MAX_OPEN_HARD", "64", TRUE);
    g_unsetenv("LUNDUKE_EDIT_TEST_LARGE");
    const std::string big = dir + "/round3-big.txt";
    write_bytes(big, std::string(20, 'Z'));
    w.buffer()->set_text("stay");
    w.last_open_error_ = "sentinel";
    expect(!w.open_file(big), "large open cancel returns false");
    expect(w.buffer()->get_text() == "stay", "large cancel keeps the buffer");
    expect(w.last_open_error_ == "sentinel",
           "large cancel is not reported as an error");
    const std::string huge = dir + "/round3-huge.txt";
    write_bytes(huge, std::string(80, 'Q'));
    expect(!w.open_file(huge), "hard cap still refuses");
    expect(w.last_open_error_.find("will not be opened") != std::string::npos,
           "hard cap still explains the refusal");
    expect(w.buffer()->get_text() == "stay", "hard cap keeps the buffer");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_OPEN");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_OPEN_HARD");

    // 10. Show Line Numbers and Latin-1 do not share a mnemonic.
    expect(w.line_numbers_item_ != nullptr && w.enc_latin1_item_ != nullptr,
           "text menu items exist");
    const std::string numbers = w.line_numbers_item_->get_label();
    const std::string latin = w.enc_latin1_item_->get_label();
    expect(numbers.find("_N") != std::string::npos, "line numbers mnemonic is N");
    expect(latin.find("_L") != std::string::npos, "latin-1 mnemonic is L");
    expect(numbers.find("_L") == std::string::npos,
           "line numbers does not use L");

    w.find_opts_.search_selection_only = false;
    w.find_opts_.extend_selection = false;
    w.find_opts_.entire_word = false;
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    g_unsetenv("LUNDUKE_EDIT_TEST_REPLACE");
    g_unsetenv("LUNDUKE_EDIT_TEST_LARGE");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_OPEN");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_OPEN_HARD");
  }

  struct LaidOutFace {
    int height{0};
    int i_width{0};
    int w_width{0};
    std::string family;
  };

  // Line height from the iterator yrange, and the family the layout
  // actually used. The stored FontDescription is not the check.
  static LaidOutFace measure_face(MainWindow& w) {
    w.buffer()->set_text("iW");
    w.text_view_.queue_resize();
    flush_ui();
    LaidOutFace face;
    auto iter = w.buffer()->get_iter_at_line(0);
    int y = 0;
    w.text_view_.get_line_yrange(iter, y, face.height);
    // gtkmm's default TextAttributes is not a live GtkTextAttributes.
    // gtk_text_iter_get_attributes merges the tags that the layout uses.
    GtkTextAttributes* raw = gtk_text_attributes_new();
    if (gtk_text_iter_get_attributes(iter.gobj(), raw) && raw->font != nullptr) {
      const char* family = pango_font_description_get_family(raw->font);
      if (family != nullptr) {
        face.family = family;
      }
    }
    gtk_text_attributes_unref(raw);
    Gdk::Rectangle i_rect;
    Gdk::Rectangle w_rect;
    w.text_view_.get_iter_location(iter, i_rect);
    w.text_view_.get_iter_location(w.buffer()->get_iter_at_offset(1), w_rect);
    face.i_width = i_rect.get_width();
    face.w_width = w_rect.get_width();
    return face;
  }

  static std::string font_config_path() {
    const char* dir = g_get_user_config_dir();
    if (dir == nullptr || dir[0] == '\0') {
      return {};
    }
    return std::string(dir) + "/lunduke-edit/font";
  }

  static bool page_inside(const MainWindow& w, double page_height,
                          std::string& why) {
    if (w.print_pages_.empty() || page_height < 1.0) {
      why = "no pages";
      return false;
    }
    for (std::size_t i = 0; i < w.print_pages_.size(); ++i) {
      const double h = printed_page_height(w, i);
      if (h > page_height + 0.2) {
        why = "page " + std::to_string(i) + " is " + std::to_string(h) +
              " pt on a " + std::to_string(page_height) + " pt page";
        return false;
      }
    }
    return true;
  }

  static void test_round4(Application& app, MainWindow& w, const std::string& dir,
                          const LaidOutFace& startup_face) {
    // 1. A clean file deleted on disk, and a file that cannot be stat'd.
    // Close, quit, save, reload, and reopen (this window, another window,
    // and a second launch) all have to notice.
    const std::string gone = dir + "/round4-gone.txt";
    write_bytes(gone, "please-keep-me");
    expect(w.open_file(gone), "open the file that will be deleted");
    expect(!w.dirty_, "deleted-file buffer starts clean");
    expect(::unlink(gone.c_str()) == 0, "delete the open file");

    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    expect(!w.confirm_discard_or_save(), "close asks after the file is deleted");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.last_prompt_primary_.find("deleted") != Glib::ustring::npos,
           "close says the file was deleted");
    expect(w.last_prompt_default_ == Gtk::RESPONSE_ACCEPT,
           "saving the deleted file again is the default");
    expect(w.file_missing_, "deleted file is marked missing");
    expect(w.dirty_, "deleted file marks the buffer unsaved");
    expect(w.buffer()->get_text() == "please-keep-me", "cancel keeps the text");
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    expect(w.on_delete_event(nullptr), "close is refused");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.get_visible(), "refused close leaves the window open");
    expect(::access(gone.c_str(), F_OK) != 0, "cancel does not recreate the file");

    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    expect(!app.confirm_quit(), "quit asks after the file is deleted");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.get_visible(), "quit cancel leaves the window open");
    expect(w.buffer()->get_text() == "please-keep-me", "quit cancel keeps the text");

    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "save", TRUE);
    expect(w.confirm_discard_or_save(), "close can save the deleted file");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(read_bytes(gone) == "please-keep-me", "save writes the deleted file back");
    expect(!w.dirty_ && !w.file_missing_, "recreated file is clean");

    expect(::unlink(gone.c_str()) == 0, "delete the file again");
    expect(w.save_document(), "Save recreates a clean deleted file");
    expect(read_bytes(gone) == "please-keep-me", "Save wrote the buffer back");
    expect(!w.file_missing_, "Save clears the missing flag");

    expect(::unlink(gone.c_str()) == 0, "delete before reopen");
    const Glib::ustring kept_gone = w.buffer()->get_text();
    expect(!w.open_file(gone), "same-window reopen of a deleted file fails");
    expect(w.buffer()->get_text() == kept_gone, "reopen keeps the buffer");
    expect(w.file_missing_, "reopen marks the file missing");
    expect(w.last_open_error_.find("deleted") != std::string::npos,
           "reopen says the file was deleted");
    expect(::access(gone.c_str(), F_OK) != 0, "reopen does not recreate the file");

    write_bytes(gone, "please-keep-me");
    expect(w.open_file(gone), "open again before the other window");
    expect(::unlink(gone.c_str()) == 0, "delete before the other window");
    auto* other = app.create_window();
    other->present();
    flush_ui();
    const int before_other = main_window_count(app);
    expect(!other->open_file(gone), "other window does not drop the deleted file");
    expect(main_window_count(app) == before_other,
           "other window does not open a second copy");
    expect(w.buffer()->get_text() == "please-keep-me",
           "other window left the buffer in place");
    expect(w.file_missing_, "other window marks the file missing");
    const int before_launch = main_window_count(app);
    app.open_files({gone});
    flush_ui();
    expect(main_window_count(app) == before_launch,
           "second launch does not open another window for a deleted file");
    expect(w.buffer()->get_text() == "please-keep-me",
           "second launch keeps the deleted file's text");
    app.destroy_window_now(other);

    // A changed mtime on a clean buffer is not an unsaved edit. Close must
    // not write the older text over the file.
    const std::string notes = dir + "/round4-notes.txt";
    write_bytes(notes, "version-one");
    expect(w.open_file(notes), "open notes");
    write_bytes(notes, "version-two");
    bump_mtime(notes);
    w.last_prompt_primary_.clear();
    expect(w.confirm_discard_or_save(),
           "close does not ask when a clean file is newer on disk");
    expect(w.last_prompt_primary_.empty(),
           "a newer clean file does not open a save question");
    expect(w.buffer()->get_text() == "version-one", "close leaves the buffer");
    expect(read_bytes(notes) == "version-two",
           "close does not overwrite the newer file");
    expect(w.open_file(notes), "same window reloads the newer file");
    expect(w.buffer()->get_text() == "version-two", "same window loaded version-two");

    write_bytes(notes, "version-three");
    bump_mtime(notes);
    auto* second = app.create_window();
    second->present();
    flush_ui();
    const int before_second = main_window_count(app);
    expect(second->open_file(notes), "other window reloads a newer file");
    expect(main_window_count(app) == before_second,
           "other window presents the one that has the path");
    expect(w.buffer()->get_text() == "version-three",
           "other window loaded version-three");
    write_bytes(notes, "version-four");
    bump_mtime(notes);
    const int before_files = main_window_count(app);
    app.open_files({notes});
    flush_ui();
    expect(main_window_count(app) == before_files,
           "second launch does not add a window");
    expect(w.buffer()->get_text() == "version-four",
           "second launch loaded version-four");

    w.buffer()->insert(w.buffer()->end(), "!");
    expect(w.dirty_, "edit before a second-window reload");
    write_bytes(notes, "version-five");
    bump_mtime(notes);
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    expect(!second->open_file(notes), "other window can cancel a dirty reload");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.buffer()->get_text() == "version-four!", "cancel keeps the dirty buffer");
    expect(read_bytes(notes) == "version-five", "cancel leaves the newer file");
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
    app.open_files({notes});
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.buffer()->get_text() == "version-five",
           "second launch Don't Save loads the disk copy");
    expect(read_bytes(notes) == "version-five",
           "second launch Don't Save does not write");
    app.destroy_window_now(second);

    // stat failing for a reason other than ENOENT is not "unchanged".
    if (::geteuid() != 0) {
      const std::string locked_dir = dir + "/round4-locked";
      g_mkdir_with_parents(locked_dir.c_str(), 0700);
      const std::string locked = locked_dir + "/file.txt";
      write_bytes(locked, "locked-bytes");
      expect(w.open_file(locked), "open the file that will be unstatable");
      expect(::chmod(locked_dir.c_str(), 0000) == 0, "hide the parent directory");
      expect(!w.save_to_path(locked), "a stat error is not a successful save");
      expect(w.file_unreadable_, "a stat error is remembered");
      expect(w.last_save_error_.find("Could not read file information") !=
                 std::string::npos,
             "a stat error says why");
      g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
      expect(!w.confirm_discard_or_save(), "close asks when stat fails");
      g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
      expect(w.last_prompt_primary_.find("Could not check") != Glib::ustring::npos,
             "close names the stat failure");
      g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
      expect(w.on_delete_event(nullptr), "close is refused after a stat error");
      g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
      expect(w.get_visible(), "stat error keeps the window");
      expect(::chmod(locked_dir.c_str(), 0700) == 0, "restore the directory");
      expect(read_bytes(locked) == "locked-bytes", "stat error does not rewrite");
    }

    // 6. Reload's follow-up focuses Don't Save. Save writes and says the
    // reload was cancelled. Don't Save loads the disk copy.
    write_bytes(notes, "on-disk");
    bump_mtime(notes);
    expect(w.open_file(notes), "open for the reload follow-up");
    w.buffer()->insert(w.buffer()->end(), "-edited");
    w.refresh_dirty_from_buffer();
    write_bytes(notes, "from-outside");
    bump_mtime(notes);
    g_setenv("LUNDUKE_EDIT_TEST_REPLACE", "reload", TRUE);
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    expect(!w.save_to_path(notes), "reload follow-up can cancel");
    g_unsetenv("LUNDUKE_EDIT_TEST_REPLACE");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.last_prompt_default_ == Gtk::RESPONSE_REJECT,
           "Don't Save is the default after Reload");
    expect(w.last_prompt_primary_ == "Save changes before reloading?",
           "reload follow-up names the reload");
    expect(w.last_prompt_secondary_.find("cancel the reload") !=
               Glib::ustring::npos,
           "reload follow-up explains Save");
    expect(w.buffer()->get_text() == "on-disk-edited", "cancel keeps the edits");
    expect(read_bytes(notes) == "from-outside", "cancel leaves disk alone");

    g_setenv("LUNDUKE_EDIT_TEST_REPLACE", "reload", TRUE);
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "save", TRUE);
    expect(w.save_to_path(notes), "explicit Save from the reload follow-up writes");
    g_unsetenv("LUNDUKE_EDIT_TEST_REPLACE");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(read_bytes(notes) == "on-disk-edited", "Save replaced the disk copy");
    expect(w.buffer()->get_text() == "on-disk-edited", "Save did not load disk");
    expect(w.last_notice_.find("Reload cancelled") != std::string::npos,
           "Save says the reload was cancelled");

    w.buffer()->set_text("still-mine");
    w.buffer()->set_modified(true);
    w.refresh_dirty_from_buffer();
    write_bytes(notes, "from-outside-again");
    bump_mtime(notes);
    g_setenv("LUNDUKE_EDIT_TEST_REPLACE", "reload", TRUE);
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
    expect(!w.save_to_path(notes), "Don't Save reloads instead of writing");
    g_unsetenv("LUNDUKE_EDIT_TEST_REPLACE");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.buffer()->get_text() == "from-outside-again",
           "Don't Save shows the disk copy");
    expect(read_bytes(notes) == "from-outside-again", "Don't Save leaves the file");
    expect(!w.dirty_, "Don't Save leaves a clean buffer");

    // 4. Find Next scope after Search Selection Only, with and without wrap,
    // with no selection, with a wider selection, and with Extend.
    w.buffer()->set_text("one two one two");
    w.find_opts_ = FindOptions{};
    w.find_opts_.search_for = "one";
    w.find_opts_.search_selection_only = true;
    w.find_opts_.extend_selection = true;
    w.find_opts_.wrap_around = true;
    w.find_opts_.start_at_top = true;
    w.buffer()->select_range(w.buffer()->begin(), w.buffer()->end());
    w.pin_selection_only_range();
    // The first Find selects the match itself. Extend is on in the dialog
    // and must turn off when the dialog closes, without changing that hit.
    w.find_opts_.extend_selection = false;
    expect(w.find_match(w.find_opts_, false), "find the first one");
    expect(selection_offset(w) == 0 && selection_text(w) == "one",
           "the first one is selected");
    w.find_opts_.extend_selection = true;
    w.on_find_dialog_hidden();
    expect(!w.find_opts_.extend_selection, "closing Find turns Extend off");
    expect(w.find_opts_.search_selection_only,
           "Search Selection Only stays on after Find closes");
    expect(!w.sel_only_range_valid_, "closing Find drops the hidden pin");
    w.last_notice_.clear();
    w.on_find_next();
    expect(selection_offset(w) == 8 && selection_text(w) == "one",
           "Find Next leaves the match it just found");
    expect(w.last_notice_.empty(), "wrap-on Find Next does not say not found");

    w.find_opts_.wrap_around = false;
    w.last_notice_.clear();
    w.on_find_next();
    expect(w.last_notice_ == "Text not found.",
           "wrap-off Find Next does not wrap to the first one");
    expect(selection_offset(w) == 8, "a miss leaves the current match");

    w.find_opts_.wrap_around = true;
    w.last_notice_.clear();
    w.on_find_next();
    expect(selection_offset(w) == 0 && selection_text(w) == "one",
           "wrap-on Find Next from the last hit finds the first");
    expect(w.last_notice_.empty(), "wrapping Find Next does not say not found");

    w.buffer()->place_cursor(w.buffer()->end());
    w.last_notice_.clear();
    w.on_find_next();
    expect(w.last_notice_ == "No text is selected.",
           "Find Next with no selection says so");
    expect(selection_text(w).empty(), "no selection is not widened");

    w.buffer()->select_range(w.buffer()->get_iter_at_offset(4),
                             w.buffer()->get_iter_at_offset(7));
    expect(selection_text(w) == "two", "wider selection is two");
    w.last_notice_.clear();
    w.on_find_next();
    expect(w.last_notice_ == "Text not found.",
           "a selection that is not the last match is searched on its own");
    expect(selection_text(w) == "two", "that search does not jump outside");

    w.buffer()->select_range(w.buffer()->get_iter_at_offset(4),
                             w.buffer()->get_iter_at_offset(11));
    expect(selection_text(w) == "two one", "selection covers the second one");
    w.last_notice_.clear();
    w.on_find_next();
    expect(selection_offset(w) == 8 && selection_text(w) == "one",
           "Find Next stays inside the wider selection");

    w.find_opts_.search_selection_only = false;
    w.buffer()->select_range(w.buffer()->get_iter_at_offset(4),
                             w.buffer()->get_iter_at_offset(7));
    w.last_notice_.clear();
    w.on_find_next();
    expect(selection_offset(w) == 8 && selection_text(w) == "one",
           "Find Next without Search Selection Only uses the whole buffer");

    w.find_opts_.extend_selection = true;
    w.find_opts_.wrap_around = false;
    w.find_opts_.search_selection_only = false;
    w.buffer()->select_range(w.buffer()->begin(),
                             w.buffer()->get_iter_at_offset(3));
    expect(w.find_match(w.find_opts_, true), "extend reaches the next one");
    expect(selection_text(w) == "one two one", "extend grows past the needle");
    w.on_find_dialog_hidden();
    expect(!w.find_opts_.extend_selection, "hide clears extend again");
    w.last_notice_.clear();
    w.on_find_next();
    expect(selection_text(w) == "one",
           "Find Next after hide selects the needle and does not keep growing");
    expect(selection_text(w).size() == 3, "the hit is the needle, not the grown span");
    w.find_opts_.search_selection_only = false;
    w.find_opts_.extend_selection = false;

    // 5. One middle-click gesture pastes once: single, double, and triple,
    // including a triple whose third press is outside the first interval.
    w.buffer()->set_text("hello world");
    w.last_middle_paste_time_ = 0;
    w.text_view_.grab_focus();
    auto primary = Gtk::Clipboard::get(GDK_SELECTION_PRIMARY);
    primary->set_text("hello");
    flush_ui();
    GdkWindow* text_window = gtk_text_view_get_window(
        GTK_TEXT_VIEW(w.text_view_.gobj()), GTK_TEXT_WINDOW_TEXT);
    auto click = [&](GdkEventType type, guint32 when) {
      GdkEventButton event {};
      event.type = type;
      event.button = 2;
      event.time = when;
      event.x = 10000;
      event.y = 8;
      event.window = text_window;
      if (type == GDK_BUTTON_RELEASE) {
        expect(w.on_text_button_release(&event), "middle release is swallowed");
      } else {
        expect(w.on_text_button_press(&event), "middle press is handled");
      }
    };
    click(GDK_BUTTON_PRESS, 5000);
    click(GDK_BUTTON_RELEASE, 5010);
    expect(w.buffer()->get_text() == "hello worldhello",
           "a single middle-click pastes once");
    click(GDK_BUTTON_PRESS, 6000);
    click(GDK_BUTTON_RELEASE, 6010);
    click(GDK_BUTTON_PRESS, 6200);
    click(GDK_2BUTTON_PRESS, 6200);
    click(GDK_BUTTON_RELEASE, 6210);
    expect(w.buffer()->get_text() == "hello worldhellohello",
           "a middle-button double-click pastes once");
    click(GDK_BUTTON_PRESS, 8000);
    click(GDK_BUTTON_RELEASE, 8010);
    click(GDK_BUTTON_PRESS, 8390);
    click(GDK_2BUTTON_PRESS, 8390);
    click(GDK_BUTTON_RELEASE, 8400);
    click(GDK_BUTTON_PRESS, 8780);
    click(GDK_3BUTTON_PRESS, 8780);
    click(GDK_BUTTON_RELEASE, 8790);
    expect(w.buffer()->get_text() == "hello worldhellohellohello",
           "a middle-button triple-click pastes once");
    click(GDK_BUTTON_PRESS, 10000);
    click(GDK_BUTTON_RELEASE, 10010);
    expect(w.buffer()->get_text() == "hello worldhellohellohellohello",
           "a later single middle-click pastes again");
    click(GDK_2BUTTON_PRESS, 12000);
    click(GDK_BUTTON_RELEASE, 12010);
    expect(w.buffer()->get_text() == "hello worldhellohellohellohello",
           "a double-press or a release on its own does not paste");
    w.last_middle_paste_time_ = 0;
    GdkEventButton zero {};
    zero.type = GDK_BUTTON_PRESS;
    zero.button = 2;
    zero.time = 0;
    zero.x = 10000;
    zero.y = 8;
    zero.window = text_window;
    expect(w.on_text_button_press(&zero), "time 0 press is handled");
    expect(w.on_text_button_press(&zero), "another time 0 press is handled");
    expect(w.buffer()->get_text() == "hello worldhellohellohellohellohellohello",
           "presses with no timestamp each paste once");

    // 7. Ordinary lines stay inside the page. Three short lines stay one page.
    const Pango::FontDescription previous_font = w.font_desc_;
    const bool previous_chosen = w.font_user_chosen_;
    w.apply_font(Pango::FontDescription("Sans 18"), false);
    std::string lines;
    for (int i = 0; i < 80; ++i) {
      lines += "hello world\n";
    }
    w.buffer()->set_text(lines);
    double page_height = 0.0;
    const std::string pdf = dir + "/round4-lines.pdf";
    ::unlink(pdf.c_str());
    auto op = Gtk::PrintOperation::create();
    op->set_export_filename(pdf);
    op->signal_begin_print().connect(
        [&](const Glib::RefPtr<Gtk::PrintContext>& context) {
          page_height = context->get_height();
          w.on_begin_print(context);
          op->set_n_pages(static_cast<int>(w.print_page_breaks_.size()) + 1);
        });
    op->signal_draw_page().connect(
        [&](const Glib::RefPtr<Gtk::PrintContext>& context, int page) {
          w.on_draw_page(context, page);
        });
    bool printed = false;
    try {
      printed = op->run(Gtk::PRINT_OPERATION_ACTION_EXPORT, w) ==
                Gtk::PRINT_OPERATION_RESULT_APPLY;
    } catch (const Gtk::PrintError&) {
      printed = false;
    }
    expect(printed, "ordinary-line print exports");
    expect(w.print_pages_.size() > 1, "ordinary lines fill more than one page");
    std::string fit_why;
    expect(page_inside(w, page_height, fit_why), fit_why.c_str());
    double tallest = 0.0;
    for (std::size_t i = 0; i < w.print_pages_.size(); ++i) {
      tallest = std::max(tallest, printed_page_height(w, i));
    }
    std::cout << "print-pages=" << w.print_pages_.size()
              << " page-height=" << page_height
              << " tallest=" << tallest << "\n";
    if (!w.print_pages_.empty() && w.print_pages_[0].slices.empty() == false &&
        w.print_pages_[0].slices[0].layout) {
      const auto printed_font =
          w.print_pages_[0].slices[0].layout->get_font_description();
      expect(printed_font.get_family() == "Sans", "print uses the Sans face");
    }

    w.buffer()->set_text("one\ntwo\nthree\n");
    page_height = 0.0;
    const std::string short_pdf = dir + "/round4-short.pdf";
    ::unlink(short_pdf.c_str());
    auto short_op = Gtk::PrintOperation::create();
    short_op->set_export_filename(short_pdf);
    short_op->signal_begin_print().connect(
        [&](const Glib::RefPtr<Gtk::PrintContext>& context) {
          page_height = context->get_height();
          w.on_begin_print(context);
          short_op->set_n_pages(static_cast<int>(w.print_page_breaks_.size()) + 1);
        });
    short_op->signal_draw_page().connect(
        [&](const Glib::RefPtr<Gtk::PrintContext>& context, int page) {
          w.on_draw_page(context, page);
        });
    try {
      short_op->run(Gtk::PRINT_OPERATION_ACTION_EXPORT, w);
    } catch (const Gtk::PrintError&) {
    }
    expect(w.print_pages_.size() == 1, "three short lines stay on one page");
    std::string short_why;
    expect(page_inside(w, page_height, short_why), short_why.c_str());

    // 2. The face and size on screen, for the startup default and Sans 24.
    expect(startup_face.height >= 14 && startup_face.height <= 24,
           "default line height is about 11 pt");
    expect(startup_face.family.find("ono") != std::string::npos,
           "default family is monospace");
    expect(startup_face.i_width > 0 && startup_face.i_width == startup_face.w_width,
           "default i and W have the same width");
    std::cout << "font-yrange default=" << startup_face.height
              << " family=" << startup_face.family
              << " i=" << startup_face.i_width << " W=" << startup_face.w_width
              << "\n";

    w.apply_font(Pango::FontDescription("Sans 24"), true);
    flush_ui();
    const LaidOutFace sans = measure_face(w);
    std::cout << "font-yrange sans24=" << sans.height
              << " family=" << sans.family
              << " i=" << sans.i_width << " W=" << sans.w_width << "\n";
    expect(sans.height >= startup_face.height * 2, "Sans 24 lines are taller");
    expect(sans.height >= 36, "Sans 24 line height grew");
    expect(sans.family.find("Sans") != std::string::npos ||
               sans.family.find("sans") != std::string::npos,
           "Sans stays the editing family");
    expect(sans.w_width > sans.i_width, "Sans makes W wider than i");
    expect(!w.text_view_.get_monospace(),
           "a chosen proportional face is not forced monospace");
    const std::string font_path = font_config_path();
    expect(!font_path.empty() && read_bytes(font_path).find("Sans") != std::string::npos,
           "the chosen face is saved");

    auto* font_window = app.create_window();
    font_window->present();
    flush_ui();
    const LaidOutFace second_face = measure_face(*font_window);
    expect(second_face.height == sans.height, "a new window uses the same line height");
    expect(second_face.family == sans.family, "a new window uses the same family");
    app.destroy_window_now(font_window);

    {
      // A second Application is a new launch: it reads the saved face
      // before any window exists. Creating that window needs startup,
      // which this unregistered app cannot run beside the test app.
      // The new window above already laid the saved face out; this checks
      // that the next launch loads the same description.
      auto relaunch = Application::create();
      expect(relaunch->font_chosen(), "a saved face counts as chosen");
      expect(relaunch->font().find("Sans") != std::string::npos,
             "a new launch loads the Sans face");
      expect(relaunch->font().find("24") != std::string::npos,
             "a new launch loads the 24 pt size");
    }

    w.buffer()->set_text("Ag");
    flush_ui();
    const std::string font_pdf = dir + "/round4-font.pdf";
    ::unlink(font_pdf.c_str());
    auto font_op = Gtk::PrintOperation::create();
    font_op->set_export_filename(font_pdf);
    font_op->signal_begin_print().connect(
        [&](const Glib::RefPtr<Gtk::PrintContext>& context) {
          w.on_begin_print(context);
          font_op->set_n_pages(1);
        });
    font_op->signal_draw_page().connect(
        [&](const Glib::RefPtr<Gtk::PrintContext>& context, int page) {
          w.on_draw_page(context, page);
        });
    try {
      font_op->run(Gtk::PRINT_OPERATION_ACTION_EXPORT, w);
    } catch (const Gtk::PrintError&) {
    }
    expect(!w.print_pages_.empty() && !w.print_pages_[0].slices.empty() &&
               w.print_pages_[0].slices[0].layout &&
               w.print_pages_[0].slices[0].layout->get_font_description()
                       .get_family()
                       .find("Sans") != std::string::npos,
           "print uses the face chosen in Text → Font");
    expect(w.font_desc_.to_string().find("Sans") != std::string::npos &&
               w.font_desc_.get_size() >= 24 * Pango::SCALE,
           "the print description is Sans 24");

    w.apply_font(previous_font, previous_chosen);
    if (!font_path.empty()) {
      ::unlink(font_path.c_str());
    }

    w.find_opts_.search_selection_only = false;
    w.find_opts_.extend_selection = false;
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    g_unsetenv("LUNDUKE_EDIT_TEST_REPLACE");
  }

  struct SpotFace {
    int height{0};
    int width{0};
    int size{0};
    std::string family;
    bool tagged{false};
  };

  static bool font_tag_at(MainWindow& w, int offset) {
    auto buf = w.buffer();
    if (!buf || offset < 0 || offset >= buf->get_char_count()) {
      return false;
    }
    const auto tags = buf->get_iter_at_offset(offset).get_tags();
    for (const auto& tag : tags) {
      if (tag && tag->property_name().get_value() == "lunduke-editor-font") {
        return true;
      }
    }
    return false;
  }

  static bool range_tagged(MainWindow& w, int begin, int end) {
    if (end <= begin) {
      return false;
    }
    for (int i = begin; i < end; ++i) {
      if (!font_tag_at(w, i)) {
        return false;
      }
    }
    return true;
  }

  // Line box plus the family and size the iterator's tags actually carry.
  // Same measurement as test_round4, at one character instead of after
  // replacing the buffer.
  static SpotFace measure_spot(MainWindow& w, int offset) {
    w.text_view_.queue_resize();
    flush_ui();
    SpotFace spot;
    auto buf = w.buffer();
    if (!buf || buf->get_char_count() == 0) {
      return spot;
    }
    if (offset < 0) {
      offset = 0;
    }
    if (offset >= buf->get_char_count()) {
      offset = buf->get_char_count() - 1;
    }
    auto iter = buf->get_iter_at_offset(offset);
    int y = 0;
    w.text_view_.get_line_yrange(iter, y, spot.height);
    Gdk::Rectangle rect;
    w.text_view_.get_iter_location(iter, rect);
    spot.width = rect.get_width();
    spot.tagged = font_tag_at(w, offset);
    GtkTextAttributes* raw = gtk_text_attributes_new();
    if (gtk_text_iter_get_attributes(iter.gobj(), raw) && raw->font != nullptr) {
      const char* family = pango_font_description_get_family(raw->font);
      if (family != nullptr) {
        spot.family = family;
      }
      const int sz = pango_font_description_get_size(raw->font);
      if (pango_font_description_get_size_is_absolute(raw->font)) {
        spot.size = static_cast<int>(
            std::lround(static_cast<double>(sz) / Pango::SCALE * 72.0 / 96.0));
      } else if (sz > 0) {
        spot.size = sz / Pango::SCALE;
      }
    }
    gtk_text_attributes_unref(raw);
    return spot;
  }

  static void expect_editor_spot(const SpotFace& spot, bool chosen, const char* msg) {
    expect(spot.tagged, msg);
    if (chosen) {
      expect(spot.family.find("Sans") != std::string::npos ||
                 spot.family.find("sans") != std::string::npos,
             msg);
      expect(spot.size >= 24, msg);
      expect(spot.height >= 36, msg);
    } else {
      expect(spot.family.find("ono") != std::string::npos, msg);
      expect(spot.size >= 10 && spot.size <= 12, msg);
      expect(spot.height >= 14 && spot.height <= 24, msg);
    }
  }

  static void type_at(MainWindow& w, int offset, const Glib::ustring& text) {
    auto iter = w.buffer()->get_iter_at_offset(offset);
    w.buffer()->insert(iter, text);
    flush_ui();
  }

  static void paste_at(MainWindow& w, int offset, const Glib::ustring& text) {
    auto clip = Gtk::Clipboard::get();
    clip->set_text(text);
    flush_ui();
    w.buffer()->place_cursor(w.buffer()->get_iter_at_offset(offset));
    w.on_paste();
    flush_ui();
  }

  // The text view inserts a drop at the gtk_drag_target mark. Move that
  // mark, then emit drag-data-received so the default handler runs.
  static bool drop_text(MainWindow& w, int offset, const Glib::ustring& payload) {
    auto buf = w.buffer();
    auto mark = buf->get_mark("gtk_drag_target");
    if (!mark) {
      return false;
    }
    buf->move_mark(mark, buf->get_iter_at_offset(offset));
    auto clip = Gtk::Clipboard::get();
    clip->set_text(payload);
    flush_ui();
    Gtk::SelectionData sel = clip->wait_for_contents("UTF8_STRING");
    if (sel.get_text() != payload) {
      return false;
    }
    GdkDragContext* ctx =
        GDK_DRAG_CONTEXT(g_object_new(GDK_TYPE_X11_DRAG_CONTEXT, nullptr));
    if (ctx == nullptr) {
      return false;
    }
    g_signal_emit_by_name(w.text_view_.gobj(), "drag-data-received", ctx, 0, 0,
                          sel.gobj(), static_cast<guint>(0),
                          static_cast<guint>(0));
    g_object_unref(ctx);
    flush_ui();
    return true;
  }

  static void test_round5_font(Application& app, MainWindow& w,
                               const std::string& dir) {
    const Pango::FontDescription previous_font = w.font_desc_;
    const bool previous_chosen = w.font_user_chosen_;
    const std::string font_path = font_config_path();

    const struct FaceCase {
      const char* desc;
      bool chosen;
    } faces[] = {
        {"Monospace 11", false},
        {"Sans 24", true},
    };

    for (const auto& face : faces) {
      w.apply_font(Pango::FontDescription(face.desc), face.chosen);
      flush_ui();
      const char* label = face.chosen ? "Sans 24" : "Monospace 11";

      // Typed at offset 0: a matching letter, a matching prefix, a
      // duplicated first line, and the whole buffer again.
      w.buffer()->set_text("hello");
      flush_ui();
      const SpotFace ref_h = measure_spot(w, 0);
      expect_editor_spot(ref_h, face.chosen, label);
      type_at(w, 0, "h");
      expect(w.buffer()->get_text() == "hhello", label);
      expect(range_tagged(w, 0, 6), label);
      const SpotFace new_h = measure_spot(w, 0);
      const SpotFace old_h = measure_spot(w, 1);
      expect_editor_spot(new_h, face.chosen, label);
      expect(std::abs(new_h.width - old_h.width) <= 2, label);
      expect(std::abs(new_h.height - old_h.height) <= 2, label);

      w.buffer()->set_text("hello");
      flush_ui();
      type_at(w, 0, "he");
      expect(w.buffer()->get_text() == "hehello", label);
      expect(range_tagged(w, 0, 7), label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);
      expect_editor_spot(measure_spot(w, 1), face.chosen, label);

      w.buffer()->set_text("hello\nworld\n");
      flush_ui();
      const SpotFace ref_line = measure_spot(w, 6);
      type_at(w, 0, "hello\n");
      expect(w.buffer()->get_text() == "hello\nhello\nworld\n", label);
      expect(range_tagged(w, 0, static_cast<int>(w.buffer()->get_char_count())),
             label);
      const SpotFace dup_line = measure_spot(w, 0);
      const SpotFace kept_line = measure_spot(w, 6);
      expect_editor_spot(dup_line, face.chosen, label);
      expect_editor_spot(kept_line, face.chosen, label);
      expect(std::abs(dup_line.height - ref_line.height) <= 2, label);
      expect(std::abs(dup_line.height - kept_line.height) <= 2, label);

      w.buffer()->set_text("hello");
      flush_ui();
      type_at(w, 0, "hello");
      expect(w.buffer()->get_text() == "hellohello", label);
      expect(range_tagged(w, 0, 10), label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);
      expect_editor_spot(measure_spot(w, 5), face.chosen, label);

      // Mid-line, including an insert that repeats the following text,
      // and at the end of the buffer.
      w.buffer()->set_text("abcabc");
      flush_ui();
      type_at(w, 3, "abc");
      expect(w.buffer()->get_text() == "abcabcabc", label);
      expect(range_tagged(w, 0, 9), label);
      expect_editor_spot(measure_spot(w, 3), face.chosen, label);

      w.buffer()->set_text("hello");
      flush_ui();
      type_at(w, 5, "!");
      expect(w.buffer()->get_text() == "hello!", label);
      expect(range_tagged(w, 0, 6), label);
      expect_editor_spot(measure_spot(w, 5), face.chosen, label);

      // A multi-byte character that matches the following text.
      w.buffer()->set_text("字hello");
      flush_ui();
      type_at(w, 0, "字");
      expect(w.buffer()->get_text() == "字字hello", label);
      expect(range_tagged(w, 0, static_cast<int>(w.buffer()->get_char_count())),
             label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);

      // Paste at offset 0, mid-line, and at the end.
      w.buffer()->set_text("hello\nworld\n");
      flush_ui();
      paste_at(w, 0, "hello\n");
      expect(w.buffer()->get_text() == "hello\nhello\nworld\n", label);
      expect(range_tagged(w, 0, static_cast<int>(w.buffer()->get_char_count())),
             label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);

      w.buffer()->set_text("abcabc");
      flush_ui();
      paste_at(w, 3, "abc");
      expect(w.buffer()->get_text() == "abcabcabc", label);
      expect(range_tagged(w, 3, 6), label);
      expect_editor_spot(measure_spot(w, 3), face.chosen, label);

      w.buffer()->set_text("hello");
      flush_ui();
      paste_at(w, 5, "XYZ");
      expect(w.buffer()->get_text() == "helloXYZ", label);
      expect(range_tagged(w, 5, 8), label);
      expect_editor_spot(measure_spot(w, 5), face.chosen, label);

      // Undo and redo, including on a loaded file so ending restore is
      // active while the text comes back.
      const std::string undo_path = dir + std::string("/round5-undo-") +
                                    (face.chosen ? "sans" : "mono") + ".txt";
      write_bytes(undo_path, "hello\nworld\n");
      expect(w.open_file(undo_path), label);
      expect(range_tagged(w, 0, static_cast<int>(w.buffer()->get_char_count())),
             label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);
      type_at(w, 0, "h");
      expect(w.buffer()->get_text() == "hhello\nworld\n", label);
      expect(font_tag_at(w, 0), label);
      w.on_undo();
      flush_ui();
      expect(w.buffer()->get_text() == "hello\nworld\n", label);
      expect(range_tagged(w, 0, static_cast<int>(w.buffer()->get_char_count())),
             label);
      expect(!w.buffer()->get_modified(), label);
      w.on_redo();
      flush_ui();
      expect(w.buffer()->get_text() == "hhello\nworld\n", label);
      expect(font_tag_at(w, 0), label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);

      w.buffer()->erase(w.buffer()->begin(), w.buffer()->get_iter_at_offset(1));
      flush_ui();
      expect(w.buffer()->get_text() == "hello\nworld\n", label);
      w.on_undo();
      flush_ui();
      expect(w.buffer()->get_text() == "hhello\nworld\n", label);
      expect(font_tag_at(w, 0), label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);

      // Open and reload paint the disk text in the editor face, and the
      // tag is not part of the bytes.
      const std::string open_path = dir + std::string("/round5-open-") +
                                    (face.chosen ? "sans" : "mono") + ".txt";
      write_bytes(open_path, "hello\nworld\n");
      expect(w.open_file(open_path), label);
      expect(w.buffer()->get_text() == "hello\nworld\n", label);
      expect(range_tagged(w, 0, static_cast<int>(w.buffer()->get_char_count())),
             label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);
      expect_editor_spot(measure_spot(w, 6), face.chosen, label);
      type_at(w, 0, "hello\n");
      expect(w.save_document(), label);
      const std::string saved = read_bytes(open_path);
      expect(saved == "hello\nhello\nworld\n", label);
      expect(saved.find("lunduke-editor-font") == std::string::npos, label);
      expect(saved.find("Sans") == std::string::npos, label);
      expect(saved.find("Monospace") == std::string::npos, label);

      write_bytes(open_path, "reloaded\nline\n");
      bump_mtime(open_path);
      g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
      expect(w.open_file(open_path), label);
      g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
      expect(w.buffer()->get_text() == "reloaded\nline\n", label);
      expect(range_tagged(w, 0, static_cast<int>(w.buffer()->get_char_count())),
             label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);
      expect(read_bytes(open_path) == "reloaded\nline\n", label);

      // Replace-all at offset 0 where the replacement equals the following
      // text, and a mid-buffer replacement that does the same.
      w.buffer()->set_text("Xhello");
      flush_ui();
      FindOptions repl;
      repl.search_for = "X";
      repl.replace_with = "hello";
      expect(w.replace_all(repl) == 1, label);
      expect(w.buffer()->get_text() == "hellohello", label);
      expect(range_tagged(w, 0, 10), label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);
      w.on_undo();
      flush_ui();
      expect(w.buffer()->get_text() == "Xhello", label);
      expect(range_tagged(w, 0, 6), label);
      w.on_redo();
      flush_ui();
      expect(w.buffer()->get_text() == "hellohello", label);
      expect(range_tagged(w, 0, 10), label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);

      w.buffer()->set_text("abcXabc");
      flush_ui();
      repl.search_for = "X";
      repl.replace_with = "abc";
      expect(w.replace_all(repl) == 1, label);
      expect(w.buffer()->get_text() == "abcabcabc", label);
      expect(range_tagged(w, 3, 6), label);
      expect_editor_spot(measure_spot(w, 3), face.chosen, label);

      // Drag-and-drop at offset 0, mid-line, and at the end.
      w.buffer()->set_text("hello");
      flush_ui();
      expect(drop_text(w, 0, "hello"), label);
      expect(w.buffer()->get_text() == "hellohello", label);
      expect(range_tagged(w, 0, 10), label);
      expect_editor_spot(measure_spot(w, 0), face.chosen, label);
      w.on_undo();
      flush_ui();
      expect(w.buffer()->get_text() == "hello", label);
      expect(range_tagged(w, 0, 5), label);
      w.on_redo();
      flush_ui();
      expect(w.buffer()->get_text() == "hellohello", label);
      expect(range_tagged(w, 0, 10), label);

      w.buffer()->set_text("abcabc");
      flush_ui();
      expect(drop_text(w, 3, "abc"), label);
      expect(w.buffer()->get_text() == "abcabcabc", label);
      expect(range_tagged(w, 3, 6), label);
      expect_editor_spot(measure_spot(w, 3), face.chosen, label);

      w.buffer()->set_text("hello");
      flush_ui();
      expect(drop_text(w, 5, "Z"), label);
      expect(w.buffer()->get_text() == "helloZ", label);
      expect(font_tag_at(w, 5), label);
      expect_editor_spot(measure_spot(w, 5), face.chosen, label);
    }

    // A second window still inherits the face chosen above.
    auto* font_window = app.create_window();
    font_window->present();
    flush_ui();
    const LaidOutFace inherited = measure_face(*font_window);
    expect(inherited.family.find("Sans") != std::string::npos ||
               inherited.family.find("sans") != std::string::npos,
           "a new window still inherits Sans");
    expect(inherited.height >= 36, "a new window still inherits the 24 pt size");
    app.destroy_window_now(font_window);

    w.apply_font(previous_font, previous_chosen);
    if (!font_path.empty() && !previous_chosen) {
      ::unlink(font_path.c_str());
    }
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
  }

  static std::string printed_layouts_text(const MainWindow& w) {
    std::string out;
    std::set<const PangoLayout*> seen;
    for (const auto& page : w.print_pages_) {
      for (const auto& slice : page.slices) {
        if (!slice.layout) {
          continue;
        }
        const PangoLayout* raw = slice.layout->gobj();
        if (!seen.insert(raw).second) {
          continue;
        }
        out.append(slice.layout->get_text().raw());
      }
    }
    return out;
  }

  static double export_print(MainWindow& w, const std::string& pdf) {
    double page_height = 0.0;
    ::unlink(pdf.c_str());
    auto op = Gtk::PrintOperation::create();
    op->set_export_filename(pdf);
    op->signal_begin_print().connect(
        [&](const Glib::RefPtr<Gtk::PrintContext>& context) {
          page_height = context->get_height();
          w.on_begin_print(context);
          op->set_n_pages(static_cast<int>(w.print_page_breaks_.size()) + 1);
        });
    op->signal_draw_page().connect(
        [&](const Glib::RefPtr<Gtk::PrintContext>& context, int page) {
          w.on_draw_page(context, page);
        });
    try {
      op->run(Gtk::PRINT_OPERATION_ACTION_EXPORT, w);
    } catch (const Gtk::PrintError&) {
    }
    return page_height;
  }

  static void expect_print_doc(MainWindow& w, const std::string& text,
                               const std::string& pdf, const char* msg) {
    w.buffer()->set_text(text);
    const double page_height = export_print(w, pdf);
    expect(page_height > 100.0, msg);
    std::string why = "fits";
    const bool inside = page_inside(w, page_height, why);
    if (!inside) {
      std::cerr << msg << ": " << why << "\n";
    }
    expect(inside, msg);
    const std::string painted = printed_layouts_text(w);
    if (painted != text) {
      std::cerr << msg << ": painted " << painted.size() << " bytes, buffer "
                << text.size() << " bytes\n";
    }
    expect(painted == text, msg);
    double tallest = 0.0;
    for (std::size_t i = 0; i < w.print_pages_.size(); ++i) {
      tallest = std::max(tallest, printed_page_height(w, i));
    }
    std::cout << msg << " pages=" << w.print_pages_.size()
              << " page-height=" << page_height << " tallest=" << tallest
              << "\n";
  }

  static void test_round5_print(MainWindow& w, const std::string& dir) {
    const Pango::FontDescription previous_font = w.font_desc_;
    const bool previous_chosen = w.font_user_chosen_;
    w.apply_font(Pango::FontDescription("Sans 18"), false);

    std::string tabbed;
    for (int i = 0; i < 8; ++i) {
      tabbed += "hello world\n";
    }
    tabbed += "col\tvalue\n";
    for (int i = 0; i < 40; ++i) {
      tabbed += "hello world\n";
    }
    expect_print_doc(w, tabbed, dir + "/round5-tab.pdf",
                     "tab among short lines stays inside the page");

    std::string wrapped;
    for (int i = 0; i < 8; ++i) {
      wrapped += "hello world\n";
    }
    wrapped += std::string(50, 'W');
    wrapped += "\n";
    for (int i = 0; i < 40; ++i) {
      wrapped += "hello world\n";
    }
    expect_print_doc(w, wrapped, dir + "/round5-wrap.pdf",
                     "wrapped wide line among short lines stays inside the page");

    std::string wrap_top = std::string(50, 'W');
    wrap_top += "\n";
    for (int i = 0; i < 40; ++i) {
      wrap_top += "hello world\n";
    }
    expect_print_doc(w, wrap_top, dir + "/round5-wrap-top.pdf",
                     "wrapped line at the top stays inside the page");

    std::string mixed;
    for (int i = 0; i < 6; ++i) {
      mixed += "hello world\n";
    }
    mixed += "\n";
    mixed += "col\tvalue\n";
    mixed += std::string(50, 'W');
    mixed += "\n";
    mixed += "中文中文中文中文中文中文中文中文中文中文\n";
    mixed += "\n";
    mixed += "\t\n";
    for (int i = 0; i < 36; ++i) {
      mixed += "hello world\n";
    }
    expect_print_doc(w, mixed, dir + "/round5-mixed.pdf",
                     "tabs, blanks, CJK, and wraps stay inside the page");

    expect_print_doc(w, std::string(80, '\n'), dir + "/round5-blanks.pdf",
                     "blank lines stay inside the page");

    w.apply_font(previous_font, previous_chosen);
  }

  struct DirMode {
    std::string path;
    mode_t mode{0700};
    DirMode(std::string p, mode_t m) : path(std::move(p)), mode(m) {}
    ~DirMode() {
      if (!path.empty()) {
        ::chmod(path.c_str(), mode);
      }
    }
  };

  static void test_round5_stat(Application& app, MainWindow& w,
                               const std::string& dir) {
    if (::geteuid() == 0) {
      expect(false, "stat-error test needs a non-root user");
      return;
    }
    const std::string locked_dir = dir + "/round5-locked";
    g_mkdir_with_parents(locked_dir.c_str(), 0700);
    DirMode restore{locked_dir, 0700};
    const std::string locked = locked_dir + "/file.txt";
    write_bytes(locked, "keep-these-bytes");
    expect(w.open_file(locked), "open the file that will be unstatable");
    expect(w.buffer()->get_text() == "keep-these-bytes",
           "stat fixture text is loaded");
    expect(::chmod(locked_dir.c_str(), 0000) == 0, "hide the parent directory");

    // Check: refresh records the failure and does not open a dialog.
    const int errors_at_check = w.error_reports_;
    w.refresh_disk_flags();
    expect(w.error_reports_ == errors_at_check,
           "checking a stat failure does not open a dialog");
    expect(w.file_unreadable_, "a stat failure is remembered");
    expect(w.last_notice_.find("Could not read file information") !=
               std::string::npos,
           "the check remembers the strerror");

    // Close: one save question, still no error dialog.
    w.last_prompt_primary_.clear();
    w.last_prompt_secondary_.clear();
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    expect(!w.confirm_discard_or_save(), "close asks when stat fails");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.error_reports_ == errors_at_check,
           "the save question is not a second error dialog");
    expect(w.last_prompt_primary_.find("Could not check") != Glib::ustring::npos,
           "the one dialog names the check");
    expect(w.last_prompt_secondary_.find("Could not read file information") !=
               Glib::ustring::npos,
           "the one dialog includes the strerror");
    expect(w.last_prompt_secondary_.find("cannot be written") !=
               Glib::ustring::npos,
           "the one dialog says the path cannot be written");
    expect(w.last_prompt_secondary_.find("Save As") != Glib::ustring::npos,
           "the one dialog leaves Save As as the way to keep the text");
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    expect(w.on_delete_event(nullptr), "close is refused after a stat error");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.get_visible(), "stat error keeps the window");
    expect(w.error_reports_ == errors_at_check,
           "closing does not add an error dialog");
    expect(w.buffer()->get_text() == "keep-these-bytes", "cancel keeps the text");

    // New and quit use the same question and do not add an error dialog.
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    w.on_new();
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.buffer()->get_text() == "keep-these-bytes",
           "New cancel keeps the text");
    expect(w.error_reports_ == errors_at_check, "New does not add an error dialog");
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
    expect(!app.confirm_quit(), "quit asks when stat fails");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.get_visible(), "quit cancel leaves the window open");
    expect(w.error_reports_ == errors_at_check, "quit does not add an error dialog");

    // Save from that question does not open another dialog.
    g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "save", TRUE);
    expect(!w.confirm_discard_or_save(), "Save from the question does not discard");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    expect(w.error_reports_ == errors_at_check,
           "Save does not open another dialog");
    expect(w.get_visible(), "a failed Save leaves the window open");
    expect(w.buffer()->get_text() == "keep-these-bytes", "Save keeps the buffer");
    expect(w.last_save_error_.find("Could not read file information") !=
               std::string::npos,
           "Save still records the strerror");
    expect(w.last_save_error_.find("cannot be written") != std::string::npos,
           "Save records that the path cannot be written");
    expect(w.last_save_error_.find("Save As") != std::string::npos,
           "Save records Save As as the way to keep the text");
    expect(::chmod(locked_dir.c_str(), 0700) == 0, "restore before reading");
    expect(read_bytes(locked) == "keep-these-bytes", "Save does not rewrite");
    expect(::chmod(locked_dir.c_str(), 0000) == 0, "hide the directory again");

    // Reopen after the question: no further dialog, buffer stays.
    const int errors_before_reopen = w.error_reports_;
    expect(!w.open_file(locked), "reopen of an unstatable file fails");
    expect(w.error_reports_ == errors_before_reopen,
           "reopen does not open another dialog");
    expect(w.buffer()->get_text() == "keep-these-bytes", "reopen keeps the buffer");
    expect(w.last_open_error_.find("Could not read file information") !=
               std::string::npos,
           "reopen records the strerror");
    expect(w.get_visible(), "reopen leaves the window open");

    // A direct Save, with no question yet, is the one dialog. A second
    // Save does not open another. Reopen after that stays quiet too.
    expect(::chmod(locked_dir.c_str(), 0700) == 0, "restore so the flag clears");
    w.refresh_disk_flags();
    expect(!w.file_unreadable_, "a readable file clears the stat failure");
    expect(::chmod(locked_dir.c_str(), 0000) == 0, "hide the directory for Save");
    const int before_direct = w.error_reports_;
    expect(!w.save_to_path(locked), "a direct Save fails when stat fails");
    expect(w.error_reports_ == before_direct + 1,
           "a direct Save opens one dialog");
    expect(w.last_error_primary_.find("cannot be written") != Glib::ustring::npos,
           "that dialog says the path cannot be written");
    expect(w.last_error_secondary_.find("Save As") != Glib::ustring::npos,
           "that dialog leaves Save As as the way to keep the text");
    expect(!w.save_to_path(locked), "a second Save still fails");
    expect(w.error_reports_ == before_direct + 1,
           "a second Save does not open another dialog");
    expect(!w.open_file(locked), "reopen after Save still fails");
    expect(w.error_reports_ == before_direct + 1,
           "reopen after the Save dialog does not open another");
    expect(w.buffer()->get_text() == "keep-these-bytes",
           "the direct Save left the buffer in place");

    expect(::chmod(locked_dir.c_str(), 0700) == 0, "restore the directory");
    expect(read_bytes(locked) == "keep-these-bytes",
           "the direct Save did not rewrite");
    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
  }

  static bool accel_present(MainWindow& w, guint key, GdkModifierType mods) {
    AccelProbe probe{key, mods, false};
    if (!w.get_accel_group()) {
      return false;
    }
    gtk_accel_group_find(w.get_accel_group()->gobj(), find_accel, &probe);
    return probe.found;
  }

  static void walk_widgets(Gtk::Widget& widget,
                           const std::function<void(Gtk::Widget&)>& fn) {
    fn(widget);
    if (auto* container = dynamic_cast<Gtk::Container*>(&widget)) {
      for (auto* child : container->get_children()) {
        if (child) {
          walk_widgets(*child, fn);
        }
      }
    }
  }

  static bool drop_uris(MainWindow& w, const std::vector<std::string>& paths,
                        bool on_window) {
    std::string payload;
    for (const auto& path : paths) {
      payload += Glib::filename_to_uri(path);
      payload += "\r\n";
    }
    struct Store {
      std::string payload;
    };
    auto* store = new Store{payload};
    GtkTargetEntry entry {};
    entry.target = const_cast<char*>("text/uri-list");
    entry.flags = 0;
    entry.info = 0;
    auto* clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_with_data(
        clip, &entry, 1,
        [](GtkClipboard*, GtkSelectionData* sel, guint, gpointer data) {
          auto* stored = static_cast<Store*>(data);
          gtk_selection_data_set(
              sel, gdk_atom_intern_static_string("text/uri-list"), 8,
              reinterpret_cast<const guchar*>(stored->payload.data()),
              static_cast<gint>(stored->payload.size()));
        },
        [](GtkClipboard*, gpointer data) { delete static_cast<Store*>(data); },
        store);
    flush_ui();
    Gtk::SelectionData sel =
        Gtk::Clipboard::get()->wait_for_contents("text/uri-list");
    if (sel.get_length() <= 0) {
      return false;
    }
    GdkDragContext* ctx =
        GDK_DRAG_CONTEXT(g_object_new(GDK_TYPE_X11_DRAG_CONTEXT, nullptr));
    if (ctx == nullptr) {
      return false;
    }
    GtkWidget* target = on_window ? GTK_WIDGET(w.gobj())
                                  : GTK_WIDGET(w.text_view_.gobj());
    g_signal_emit_by_name(target, "drag-data-received", ctx, 0, 0, sel.gobj(),
                          static_cast<guint>(0), static_cast<guint>(0));
    g_object_unref(ctx);
    flush_ui();
    return true;
  }

  static void test_round6(Application& app, MainWindow& w, const std::string& dir) {
    std::cout << "round6 begin\n";
    w.refresh_disk_flags();
    if (w.dirty_ || w.file_unreadable_) {
      g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
      w.on_new();
      g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    }

    // 1. A stat failure defaults to Save As, and that button writes a new file.
    if (::geteuid() == 0) {
      expect(false, "stat-failure Save As test needs a non-root user");
    } else {
      const std::string locked_dir = dir + "/round6-locked";
      g_mkdir_with_parents(locked_dir.c_str(), 0700);
      const std::string locked = locked_dir + "/file.txt";
      write_bytes(locked, "stat-bytes");
      expect(w.open_file(locked), "open the file that will be unstatable");
      expect(::chmod(locked_dir.c_str(), 0000) == 0, "hide the parent directory");
      w.refresh_disk_flags();
      expect(w.file_unreadable_, "round6 stat failure is remembered");
      const int errors = w.error_reports_;
      g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
      expect(!w.confirm_discard_or_save(), "stat failure still asks");
      g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
      expect(w.last_prompt_default_ == MainWindow::kPromptSaveAs,
             "stat failure defaults to Save As");
      expect(w.last_prompt_accept_.find("Save") != Glib::ustring::npos &&
                 w.last_prompt_accept_.find("As") != Glib::ustring::npos,
             "the default button is Save As");
      expect(w.error_reports_ == errors, "the question is not an error dialog");
      const std::string copied = dir + "/round6-saveas.txt";
      ::unlink(copied.c_str());
      g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "save-as", TRUE);
      g_setenv("LUNDUKE_EDIT_TEST_SAVE_AS", copied.c_str(), TRUE);
      expect(w.confirm_discard_or_save(), "Save As from the question writes");
      g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
      g_unsetenv("LUNDUKE_EDIT_TEST_SAVE_AS");
      expect(w.error_reports_ == errors, "Save As does not add an error dialog");
      expect(::chmod(locked_dir.c_str(), 0700) == 0, "restore the parent");
      expect(read_bytes(locked) == "stat-bytes", "Save As leaves the old path");
      expect(read_bytes(copied) == "stat-bytes", "Save As wrote the new path");
      expect(w.file_path_ == copied, "the window follows the new path");
    }

    // 2. A 20 MB open stays on the main loop, byte for byte.
    {
      const std::string big = dir + "/round6-20mb.txt";
      constexpr std::size_t kBytes = 20u * 1024u * 1024u;
      std::string bytes(kBytes, '\n');
      const char* piece = "0123456789abcdef0123456789abcdef";
      for (std::size_t i = 0; i + 32 < bytes.size(); i += 33) {
        std::memcpy(&bytes[i], piece, 32);
      }
      write_bytes(big, bytes);
      w.buffer()->set_text("before-open");
      w.buffer()->set_modified(false);
      w.set_dirty(false);

      struct Tick {
        MainWindow* window;
        gint64 last;
        gint64 max_gap;
        int count;
        bool saw_opening;
        bool saw_watch;
      } tick{&w, 0, 0, 0, false, false};
      const guint timer = g_timeout_add(
          10,
          [](gpointer data) -> gboolean {
            auto* t = static_cast<Tick*>(data);
            const gint64 now = g_get_monotonic_time();
            if (t->last != 0) {
              t->max_gap = std::max(t->max_gap, now - t->last);
            }
            t->last = now;
            ++t->count;
            if (t->window->status_find_.get_text().find("Opening") !=
                Glib::ustring::npos) {
              t->saw_opening = true;
            }
            auto cursor_is_watch = [](const Glib::RefPtr<Gdk::Window>& win) {
              if (!win) {
                return false;
              }
              auto cursor = win->get_cursor();
              return cursor && cursor->get_cursor_type() == Gdk::WATCH;
            };
            if (cursor_is_watch(t->window->get_window()) ||
                cursor_is_watch(
                    t->window->text_view_.get_window(Gtk::TEXT_WINDOW_TEXT))) {
              t->saw_watch = true;
            }
            return TRUE;
          },
          &tick);
      expect(w.open_file(big), "20 MB file opens");
      g_source_remove(timer);
      std::cout << "round6 open ticks " << tick.count << " max_gap_us "
                << tick.max_gap << "\n";
      expect(tick.count >= 5, "the main loop ticked during open");
      expect(tick.max_gap < 500000, "open did not freeze the main loop");
      expect(tick.saw_opening, "open shows an Opening status line");
      expect(tick.saw_watch, "open shows a busy cursor");
      expect(w.buffer()->get_text() == bytes, "opened text is byte-exact");
      expect(!w.buffer()->get_modified(), "open does not mark the buffer");
      expect(!w.dirty_, "open does not mark the document dirty");
      expect(!w.buffer()->can_undo(), "open is not an undo step");
      expect(font_tag_at(w, 0), "opened text keeps the editor font");

      FindOptions miss;
      miss.search_for = "NOT-IN-THIS-FILE";
      miss.start_at_top = true;
      miss.wrap_around = false;
      struct Gap {
        gint64 last;
        gint64 max_gap;
        int count;
      } gap{0, 0, 0};
      const guint find_timer = g_timeout_add(
          10,
          [](gpointer data) -> gboolean {
            auto* g = static_cast<Gap*>(data);
            const gint64 now = g_get_monotonic_time();
            if (g->last != 0) {
              g->max_gap = std::max(g->max_gap, now - g->last);
            }
            g->last = now;
            ++g->count;
            return TRUE;
          },
          &gap);
      expect(!w.find_match(miss, false), "a missing search misses");
      g_source_remove(find_timer);
      std::cout << "round6 find ticks " << gap.count << " max_gap_us "
                << gap.max_gap << "\n";
      expect(gap.count >= 3, "a long miss returns to the main loop");
      expect(gap.max_gap < 500000, "a long miss does not freeze the main loop");
      expect(w.buffer()->get_text() == bytes, "a miss leaves the text");
    }

    // Cancel and close during a load.
    {
      const std::string mid = dir + "/round6-cancel.txt";
      std::string bytes(4u * 1024u * 1024u, 'm');
      for (std::size_t i = 40; i < bytes.size(); i += 41) {
        bytes[i] = '\n';
      }
      write_bytes(mid, bytes);
      w.buffer()->set_text("keep-me");
      struct Esc {
        MainWindow* window;
        bool fired;
      } esc{&w, false};
      const guint esc_timer = g_timeout_add(
          1,
          [](gpointer data) -> gboolean {
            auto* e = static_cast<Esc*>(data);
            if (!e->window->loading_) {
              return TRUE;
            }
            e->fired = true;
            GdkEventKey event {};
            event.keyval = GDK_KEY_Escape;
            e->window->on_key_press_event(&event);
            return FALSE;
          },
          &esc);
      expect(!w.open_file(mid), "escape cancels an open");
      if (!esc.fired) {
        g_source_remove(esc_timer);
      }
      expect(esc.fired, "escape ran during the load");
      expect(w.buffer()->get_text() == "keep-me", "cancel keeps the previous text");
      expect(w.get_visible(), "cancel leaves the window open");

      auto* other = app.create_window();
      other->present();
      flush_ui();
      other->buffer()->set_text("");
      other->buffer()->set_modified(false);
      const int windows = main_window_count(app);
      struct Closer {
        MainWindow* window;
        bool fired;
      } closer{other, false};
      const guint close_timer = g_timeout_add(
          1,
          [](gpointer data) -> gboolean {
            auto* c = static_cast<Closer*>(data);
            if (!c->window->loading_) {
              return TRUE;
            }
            c->fired = true;
            c->window->on_delete_event(nullptr);
            return FALSE;
          },
          &closer);
      expect(!other->open_file(mid), "close cancels an open");
      if (!closer.fired) {
        g_source_remove(close_timer);
      }
      expect(closer.fired, "close ran during the load");
      expect(!other->get_visible(), "close during load hides that window");
      flush_ui();
      expect(main_window_count(app) == windows - 1,
             "close during load deletes that window");
      expect(w.get_visible(), "close during load leaves the other window");
      expect(w.buffer()->get_text() == "keep-me", "the other window's text stays");
    }

    // 3. Replace All is one fast undo step.
    {
      std::string body;
      body.reserve(30'000 * 11);
      for (int i = 0; i < 30000; ++i) {
        body += "alpha beta\n";
      }
      w.buffer()->begin_not_undoable_action();
      w.buffer()->set_text(body);
      w.buffer()->end_not_undoable_action();
      w.buffer()->set_modified(false);
      expect(!w.buffer()->can_undo(), "the fixture is not an undo step");
      FindOptions repl;
      repl.search_for = "alpha";
      repl.replace_with = "omega";
      const gint64 started = g_get_monotonic_time();
      expect(w.replace_all(repl) == 30000, "replace all hits every line");
      const gint64 replace_us = g_get_monotonic_time() - started;
      std::cout << "round6 replace_us " << replace_us << "\n";
      expect(replace_us < 5000000, "replace all stays within a few seconds");
      expect(w.buffer()->can_undo(), "replace all can be undone");
      expect(font_tag_at(w, 0), "replaced text keeps the editor font");
      const gint64 undo_started = g_get_monotonic_time();
      w.on_undo();
      const gint64 undo_us = g_get_monotonic_time() - undo_started;
      std::cout << "round6 undo_us " << undo_us << "\n";
      expect(undo_us < 5000000, "undo of replace all stays within a few seconds");
      expect(w.buffer()->get_text() == body, "one undo restores replace all");
      expect(!w.buffer()->can_undo(), "replace all was a single undo step");
    }

    // 4. Typing at the end of a very long line stays responsive.
    {
      const bool wrap_was = w.app_.wrap_text();
      if (w.wrap_item_ && !w.wrap_item_->get_active()) {
        w.wrap_item_->set_active(true);
      }
      expect(w.app_.wrap_text(), "wrap preference is on for the long line");
      const std::string line(200000, 'a');
      w.buffer()->set_text(line);
      flush_ui();
      expect(w.text_view_.get_wrap_mode() == Gtk::WRAP_NONE,
             "a very long line forces wrap off");
      expect(w.app_.wrap_text(), "the wrap preference stays on");
      const gint64 started = g_get_monotonic_time();
      constexpr int kKeys = 5;
      for (int i = 0; i < kKeys; ++i) {
        w.buffer()->insert(w.buffer()->end(), "z");
        flush_ui();
      }
      const gint64 each =
          (g_get_monotonic_time() - started) / kKeys;
      std::cout << "round6 long_line_us " << each << "\n";
      expect(each < 40000, "typing at the end of a long line stays fast");
      expect(w.buffer()->get_char_count() == 200000 + kKeys,
             "the long line kept every character");
      expect(w.buffer()->begin().has_tag(w.long_hidden_tag_),
             "the start of a long line is outside the caret window");
      Gtk::TextIter tail = w.buffer()->end();
      tail.backward_char();
      expect(!tail.has_tag(w.long_hidden_tag_),
             "the character at the caret stays visible");
      w.buffer()->place_cursor(w.buffer()->begin());
      flush_ui();
      expect(!w.buffer()->begin().has_tag(w.long_hidden_tag_),
             "Home shows the start of the long line");
      const int before_undo = w.buffer()->get_char_count();
      w.on_undo();
      expect(w.buffer()->get_char_count() == before_undo - 1,
             "undo removes the typed character");
      expect(w.text_view_.get_wrap_mode() == Gtk::WRAP_NONE,
             "wrap stays off while the line is long");
      if (!wrap_was && w.wrap_item_) {
        w.buffer()->set_text("short\n");
        flush_ui();
        w.wrap_item_->set_active(false);
      }
    }

    // 5. First-run menus.
    {
      std::vector<std::string> top;
      for (auto* child : w.menubar_.get_children()) {
        auto* item = dynamic_cast<Gtk::MenuItem*>(child);
        if (!item) {
          continue;
        }
        top.push_back(item->get_label());
      }
      expect(top.size() == 5, "five top-level menus");
      expect(top.size() == 5 && top[0].find("File") != std::string::npos &&
                 top[1].find("Edit") != std::string::npos &&
                 top[2].find("Search") != std::string::npos &&
                 top[3].find("Text") != std::string::npos &&
                 top[4].find("Help") != std::string::npos,
             "menus are File, Edit, Search, Text, Help");
      bool top_font = false;
      for (const auto& label : top) {
        if (label.find("Font") != std::string::npos) {
          top_font = true;
        }
      }
      expect(!top_font, "Font is not a top-level menu");
      int font_items = 0;
      int section_hits = 0;
      if (auto* text_item = dynamic_cast<Gtk::MenuItem*>(w.menubar_.get_children()[3])) {
        for (auto* child : text_item->get_submenu()->get_children()) {
          auto* item = dynamic_cast<Gtk::MenuItem*>(child);
          if (!item) {
            continue;
          }
          if (item->get_label().find("Font") != std::string::npos) {
            ++font_items;
          }
          if (auto* label = dynamic_cast<Gtk::Label*>(item->get_child())) {
            const std::string text = label->get_text();
            if (text == "Encoding" || text == "Open Next File As") {
              ++section_hits;
              expect(item->get_sensitive(), "section titles are not greyed out");
            }
          }
        }
      }
      expect(font_items == 1, "Text contains one Font command");
      expect(section_hits == 2, "Encoding and Open Next File As are section titles");
      expect(w.open_utf8_item_ &&
                 w.open_utf8_item_->get_label().find("Next file") != std::string::npos &&
                 w.open_utf8_item_->get_label().find("_8") != std::string::npos,
             "next-file UTF-8 has its own mnemonic");
      expect(w.open_latin1_item_ &&
                 w.open_latin1_item_->get_label().find("Next file") != std::string::npos &&
                 w.open_latin1_item_->get_label().find("_1") != std::string::npos,
             "next-file Latin-1 has its own mnemonic");
      expect(w.enc_utf8_item_->get_label() != w.open_utf8_item_->get_label(),
             "open-next does not reuse the encoding label");
      expect(accel_present(w, GDK_KEY_n, GDK_CONTROL_MASK), "Ctrl+N");
      expect(accel_present(w, GDK_KEY_o, GDK_CONTROL_MASK), "Ctrl+O");
      expect(accel_present(w, GDK_KEY_s, GDK_CONTROL_MASK), "Ctrl+S");
      expect(accel_present(w, GDK_KEY_s,
                           static_cast<GdkModifierType>(GDK_CONTROL_MASK | GDK_SHIFT_MASK)),
             "Ctrl+Shift+S");
      expect(accel_present(w, GDK_KEY_p, GDK_CONTROL_MASK), "Ctrl+P");
      expect(accel_present(w, GDK_KEY_q, GDK_CONTROL_MASK), "Ctrl+Q");
      expect(accel_present(w, GDK_KEY_z, GDK_CONTROL_MASK), "Ctrl+Z");
      expect(accel_present(w, GDK_KEY_f, GDK_CONTROL_MASK), "Ctrl+F");
      expect(accel_present(w, GDK_KEY_F3, static_cast<GdkModifierType>(0)), "F3");
      expect(accel_present(w, GDK_KEY_g, GDK_CONTROL_MASK), "Ctrl+G");
    }

    // 6. Don't Find is gone. Cancel remains.
    {
      FindReplaceDialog dlg(w, FindOptions{});
      int dont = 0;
      int cancel = 0;
      walk_widgets(*dlg.get_content_area(), [&](Gtk::Widget& widget) {
        if (auto* button = dynamic_cast<Gtk::Button*>(&widget)) {
          const auto label = button->get_label();
          if (label.find("Don't Find") != Glib::ustring::npos ||
              label.find("Dont Find") != Glib::ustring::npos) {
            ++dont;
          }
          if (label.find("Cancel") != Glib::ustring::npos) {
            ++cancel;
          }
        }
      });
      expect(dont == 0, "Don't Find is not in the dialog");
      expect(cancel == 1, "Cancel is the way out of Find");
    }

    // 7. File drops open. Text drops still insert.
    {
      const std::string dropped = dir + "/round6-drop.txt";
      write_bytes(dropped, "dropped-text");
      const std::string second = dir + "/round6-drop-2.txt";
      write_bytes(second, "second-drop");
      flush_ui();
      GtkTargetList* view_targets =
          gtk_drag_dest_get_target_list(GTK_WIDGET(w.text_view_.gobj()));
      GtkTargetList* win_targets =
          gtk_drag_dest_get_target_list(GTK_WIDGET(w.gobj()));
      const GdkAtom uri_atom = gdk_atom_intern_static_string("text/uri-list");
      expect(view_targets && gtk_target_list_find(view_targets, uri_atom, nullptr),
             "the text view accepts file drops");
      expect(win_targets && gtk_target_list_find(win_targets, uri_atom, nullptr),
             "the window accepts file drops");

      w.buffer()->set_text("dirty-drop");
      w.buffer()->set_modified(true);
      w.refresh_dirty_from_buffer();
      g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
      expect(drop_uris(w, {dropped}, false), "synthesized file drop is delivered");
      g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
      expect(w.buffer()->get_text() == "dirty-drop",
             "cancelling a file drop keeps the buffer");

      g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
      expect(drop_uris(w, {dropped}, false), "file drop opens after discard");
      g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
      expect(w.buffer()->get_text() == "dropped-text", "a file drop opens the file");
      expect(w.file_path_ == dropped, "a file drop uses the open path");
      expect(font_tag_at(w, 0), "a dropped file keeps the editor font");

      auto* other = app.create_window();
      other->present();
      flush_ui();
      w.buffer()->insert(w.buffer()->end(), "!");
      g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "cancel", TRUE);
      expect(drop_uris(*other, {dropped}, true), "drop on another window is delivered");
      g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
      expect(w.buffer()->get_text() == "dropped-text!",
             "a dirty window is not reloaded when the drop is cancelled");
      expect(!other->edits_path(dropped),
             "the empty window does not take a file another window has");
      g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
      expect(drop_uris(*other, {dropped}, true), "drop reuses the existing window");
      g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
      flush_ui();
      expect(w.buffer()->get_text() == "dropped-text",
             "the drop reloaded the window that already had the file");
      expect(w.edits_path(dropped), "the original window still has the file");
      other->hide();
      flush_ui();

      w.buffer()->set_text("xy");
      expect(drop_text(w, 1, "Z"), "a text drop is still delivered");
      expect(w.buffer()->get_text() == "xZy", "dragging text inside the view still inserts");

      const int errors = w.error_reports_;
      const Glib::ustring before = w.buffer()->get_text();
      std::string remote = "sftp://example.invalid/nope.txt";
      // A non-native URI is reported and does not replace the buffer.
      {
        std::string payload = remote + "\r\n";
        struct Store {
          std::string payload;
        };
        auto* store = new Store{payload};
        GtkTargetEntry entry {};
        entry.target = const_cast<char*>("text/uri-list");
        gtk_clipboard_set_with_data(
            gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), &entry, 1,
            [](GtkClipboard*, GtkSelectionData* sel, guint, gpointer data) {
              auto* stored = static_cast<Store*>(data);
              gtk_selection_data_set(
                  sel, gdk_atom_intern_static_string("text/uri-list"), 8,
                  reinterpret_cast<const guchar*>(stored->payload.data()),
                  static_cast<gint>(stored->payload.size()));
            },
            [](GtkClipboard*, gpointer data) { delete static_cast<Store*>(data); },
            store);
        flush_ui();
        Gtk::SelectionData sel =
            Gtk::Clipboard::get()->wait_for_contents("text/uri-list");
        GdkDragContext* ctx =
            GDK_DRAG_CONTEXT(g_object_new(GDK_TYPE_X11_DRAG_CONTEXT, nullptr));
        g_signal_emit_by_name(w.text_view_.gobj(), "drag-data-received", ctx, 0, 0,
                              sel.gobj(), static_cast<guint>(0),
                              static_cast<guint>(0));
        g_object_unref(ctx);
        flush_ui();
      }
      expect(w.error_reports_ == errors + 1, "a remote drop is reported");
      expect(w.buffer()->get_text() == before, "a remote drop does not change the buffer");

      g_setenv("LUNDUKE_EDIT_TEST_DISCARD", "discard", TRUE);
      expect(drop_uris(w, {dropped, second}, false), "two files can be dropped");
      g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
      flush_ui();
      expect(w.file_path_ == dropped || w.buffer()->get_text() == "dropped-text",
             "the first dropped file opens here");
      expect(window_has_path(app, second), "the second dropped file opens too");
    }

    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    g_unsetenv("LUNDUKE_EDIT_TEST_SAVE_AS");
    std::cout << "round6 end\n";
  }

  static void test_round5(Application& app, MainWindow& w, const std::string& dir) {
    test_round5_font(app, w, dir);
    test_round5_print(w, dir);
    test_round5_stat(app, w, dir);
  }

  static int run() {
    failures = 0;
    g_setenv("LUNDUKE_EDIT_TEST", "1", TRUE);
    g_setenv("GDK_BACKEND", "x11", FALSE);

    test_parse_and_save_as_path();

    const std::string dir = "/tmp/lunduke-edit-tests";
    g_mkdir_with_parents(dir.c_str(), 0700);

    // A face left by an earlier run must not become this process's default.
    const std::string font_path = font_config_path();
    std::string saved_font;
    if (!font_path.empty()) {
      saved_font = read_bytes(font_path);
      ::unlink(font_path.c_str());
    }

    auto app = Application::create();
    expect(app->register_application(), "register application");
    auto* w = app->create_window();
    w->present();
    flush_ui();
    const LaidOutFace startup_face = measure_face(*w);
    expect(w->text_view_.get_monospace(),
           "the default face keeps the monospace style");

    test_columns_and_gutter(*w);
    test_save_open_undo(*w, dir);
    test_find_replace(*w);
    test_open_many(*app.get(), dir);
    test_review_fixes(*app.get(), *w, dir);
    test_hostile_review(*app.get(), *w, dir);
    test_round3(*app.get(), *w, dir);
    test_round4(*app.get(), *w, dir, startup_face);
    test_round5(*app.get(), *w, dir);
    test_round6(*app.get(), *w, dir);

    if (!font_path.empty()) {
      if (saved_font.empty()) {
        ::unlink(font_path.c_str());
      } else {
        write_bytes(font_path, saved_font);
      }
    }

    g_unsetenv("LUNDUKE_EDIT_TEST_DISCARD");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_OPEN");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_OPEN_HARD");
    g_unsetenv("LUNDUKE_EDIT_TEST_LARGE");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_HITS");
    g_unsetenv("LUNDUKE_EDIT_TEST_CHUNK");
    g_unsetenv("LUNDUKE_EDIT_TEST_HUGE_BYTES");
    g_unsetenv("LUNDUKE_EDIT_TEST_HUGE_UNDO");
    g_unsetenv("LUNDUKE_EDIT_TEST_REPLACE");
    g_unsetenv("LUNDUKE_EDIT_TEST_MAX_PASTE");

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
