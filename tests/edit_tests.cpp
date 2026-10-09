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
#include <X11/Xlib.h>

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
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

std::vector<pid_t> live_children;

void remember_child(pid_t pid) {
  if (pid > 0) {
    live_children.push_back(pid);
  }
}

void forget_child(pid_t pid) {
  live_children.erase(std::remove(live_children.begin(), live_children.end(), pid),
                      live_children.end());
}

// Kill one child we forked, and the process group it leads, by that pid.
void kill_child_tree(pid_t pid) {
  if (pid <= 0) {
    return;
  }
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  forget_child(pid);
}

void kill_live_children() {
  const std::vector<pid_t> copy = live_children;
  for (pid_t pid : copy) {
    kill_child_tree(pid);
  }
}

}  // namespace

namespace lundukeedit {

struct EditChecks {
  static int failures;
  static std::string argv0;

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

  static bool gutter_has(const std::vector<int>& numbers, int line) {
    return std::find(numbers.begin(), numbers.end(), line) != numbers.end();
  }

  static void test_gutter_trailing_newline(MainWindow& w) {
    w.buffer()->set_text("a\nb\n");
    w.buffer()->place_cursor(w.buffer()->end());
    w.update_status();
    flush_ui();
    expect(w.buffer()->get_line_count() == 3,
           "a trailing newline is its own line");
    expect(w.status_pos_.get_text().find("Ln 3,") == 0,
           "status numbers the empty line after a final newline");
    auto end_short = w.buffer()->get_iter_at_line(2);
    w.text_view_.scroll_to(end_short);
    flush_ui();
    const auto short_numbers = w.gutter_->visible_line_numbers();
    expect(gutter_has(short_numbers, 3),
           "gutter numbers the empty line after a final newline");

    std::string many;
    many.reserve(200 * 2);
    for (int i = 0; i < 200; ++i) {
      many += "a\n";
    }
    w.buffer()->set_text(many);
    w.buffer()->place_cursor(w.buffer()->end());
    w.update_status();
    // scroll_to stops at unmeasured lines. The same idle Ctrl+End uses
    // validates in short slices, then scrolls the caret on screen. A tight
    // non-blocking loop returns before that 15 ms timeout is due, so wait
    // on the clock until the empty last line is in the gutter.
    w.note_caret_for_reveal();
    const int lines = w.buffer()->get_line_count();
    std::vector<int> numbers;
    const gint64 deadline = g_get_monotonic_time() + 3000000;
    while (g_get_monotonic_time() < deadline) {
      while (g_main_context_pending(nullptr)) {
        g_main_context_iteration(nullptr, false);
      }
      numbers = w.gutter_->visible_line_numbers();
      if (gutter_has(numbers, lines) && !w.follow_caret_) {
        break;
      }
      g_usleep(5000);
    }
    expect(lines == 201, "two hundred newlines make line 201");
    expect(w.status_pos_.get_text().find("Ln 201,") == 0,
           "status shows the empty last line of a longer file");
    if (!gutter_has(numbers, lines)) {
      std::cerr << "gutter lines:";
      for (int n : numbers) {
        std::cerr << " " << n;
      }
      std::cerr << "\n";
    }
    expect(gutter_has(numbers, lines),
           "gutter numbers the empty last line, not one short of it");
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

  // Block the instance handlers for drag-data-received. The class handler
  // may still run; it does not open files. A test that called open_file
  // directly would still open while these handlers are blocked.
  static guint set_drop_handlers_blocked(MainWindow& w, bool block) {
    const guint sig = g_signal_lookup("drag-data-received", GTK_TYPE_WIDGET);
    GtkWidget* targets[] = {GTK_WIDGET(w.text_view_.gobj()),
                            GTK_WIDGET(w.gobj())};
    guint n = 0;
    for (GtkWidget* target : targets) {
      if (block) {
        n += g_signal_handlers_block_matched(target, G_SIGNAL_MATCH_ID, sig, 0,
                                             nullptr, nullptr, nullptr);
      } else {
        n += g_signal_handlers_unblock_matched(target, G_SIGNAL_MATCH_ID, sig,
                                               0, nullptr, nullptr, nullptr);
      }
    }
    return n;
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
      // 1 ms, not 10. A release build finishes the read between 10 ms
      // fires, so five ticks was a flake even though the loop was yielding.
      const guint timer = g_timeout_add(
          1,
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
      repl.case_sensitive = true;
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
      w.buffer()->begin_not_undoable_action();
      w.buffer()->set_text(line);
      w.buffer()->end_not_undoable_action();
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
      expect(w.buffer()->can_undo(), "the typed character can be undone");
      w.on_undo();
      const int after_undo = w.buffer()->get_char_count();
      expect(after_undo < before_undo && after_undo >= 200000,
             "undo removes the typed character");
      expect(w.buffer()->get_text().raw().substr(0, 8) == "aaaaaaaa",
             "undo leaves the long line in place");
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

      // The drop is delivered by emitting drag-data-received, which is the
      // signal GTK runs after a text/uri-list drop. Blocking that handler
      // must leave the file unopened: the test is not calling open_file.
      {
        const std::string path_before = w.file_path_;
        w.buffer()->set_text("before-block");
        w.buffer()->set_modified(false);
        w.set_dirty(false);
        expect(set_drop_handlers_blocked(w, true) > 0,
               "the drag-data-received handler is connected");
        expect(drop_uris(w, {dropped}, false),
               "a blocked drop still emits drag-data-received");
        expect(w.file_path_ == path_before,
               "a blocked drop handler does not open the file");
        set_drop_handlers_blocked(w, false);
        w.buffer()->set_text("before-block");
        w.buffer()->set_modified(false);
        w.set_dirty(false);
      }

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
      write_bytes(dropped, "from-disk");
      bump_mtime(dropped);
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
      expect(w.buffer()->get_text() == "from-disk",
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

  struct ArgvChildState {
    Application* app{nullptr};
    std::string path;
    bool escape{false};
    bool saw_window{false};
    bool sent_escape{false};
    bool loaded{false};
    bool end_sent{false};
    bool end_done{false};
    int end_spins{0};
    bool burn{false};
    gint64 last_tick{0};
  };

  static gboolean argv_child_escape_idle(gpointer data) {
    auto* state = static_cast<ArgvChildState*>(data);
    if (!state->escape || !state->app) {
      return G_SOURCE_REMOVE;
    }
    for (auto* window : state->app->get_windows()) {
      auto* main = dynamic_cast<MainWindow*>(window);
      // present() maps synchronously, before the GdkWindow can take a
      // key. Wait until that window exists and the load is still running.
      if (!main || !main->get_mapped() || !main->loading_ || !main->get_window()) {
        continue;
      }
      state->saw_window = true;
      if (!state->sent_escape) {
        state->sent_escape = true;
        g_print("ARGV_ESCAPE\n");
        fflush(stdout);
      }
      // gtk_test_widget_send_key uses XSendEvent, which this display drops.
      // gtk_widget_event is the call gtk_propagate_event makes for a key:
      // the toplevel's key-press-event runs, and that is where Escape
      // cancels a load, before the key is handed to the focus child.
      GdkEvent* event = gdk_event_new(GDK_KEY_PRESS);
      event->key.window =
          GDK_WINDOW(g_object_ref(main->get_window()->gobj()));
      event->key.keyval = GDK_KEY_Escape;
      event->key.time = GDK_CURRENT_TIME;
      event->key.send_event = TRUE;
      if (GdkDisplay* display = gdk_window_get_display(event->key.window)) {
        if (GdkSeat* seat = gdk_display_get_default_seat(display)) {
          if (GdkDevice* keyboard = gdk_seat_get_keyboard(seat)) {
            gdk_event_set_device(event, keyboard);
          }
        }
      }
      gtk_widget_event(GTK_WIDGET(main->gobj()), event);
      gdk_event_free(event);
      if (!main->loading_) {
        return G_SOURCE_REMOVE;
      }
      return G_SOURCE_CONTINUE;
    }
    return G_SOURCE_CONTINUE;
  }

  static gboolean argv_child_watch(gpointer data) {
    auto* state = static_cast<ArgvChildState*>(data);
    const gint64 now = g_get_monotonic_time();
    if (state->last_tick == 0 || now - state->last_tick >= 100000) {
      g_print("ARGV_TICK\n");
      fflush(stdout);
      state->last_tick = now;
    }
    int windows = 0;
    for (auto* window : state->app->get_windows()) {
      auto* main = dynamic_cast<MainWindow*>(window);
      if (!main) {
        continue;
      }
      ++windows;
      state->saw_window = true;
      if (main->loading_ || main->file_path_ != state->path ||
          !main->get_mapped()) {
        continue;
      }
      if (!state->loaded) {
        state->loaded = true;
        g_print("ARGV_LOADED chars=%d\n", main->buffer()->get_char_count());
        fflush(stdout);
      }
      if (state->escape) {
        state->app->quit();
        return G_SOURCE_REMOVE;
      }
      if (!state->end_done && g_getenv("LUNDUKE_EDIT_TEST_ARGV_END") != nullptr) {
        if (!state->end_sent) {
          state->end_sent = true;
          g_signal_emit_by_name(main->text_view_.gobj(), "move-cursor",
                                GTK_MOVEMENT_BUFFER_ENDS, 1, FALSE);
          return G_SOURCE_CONTINUE;
        }
        ++state->end_spins;
        // Validating a million lines is sliced so the window stays
        // responsive. 4000 ticks was not always enough for that.
        if (main->follow_caret_ && state->end_spins < 8000) {
          return G_SOURCE_CONTINUE;
        }
        auto buf = main->buffer();
        auto caret = buf->get_iter_at_mark(buf->get_insert());
        int y = 0;
        int height = 0;
        main->text_view_.get_line_yrange(caret, y, height);
        Gdk::Rectangle vis;
        main->text_view_.get_visible_rect(vis);
        const bool visible = height > 0 && y < vis.get_y() + vis.get_height() &&
                             (y + height) > vis.get_y();
        Gtk::TextIter top = buf->begin();
        int line_top = 0;
        main->text_view_.get_line_at_y(top, vis.get_y(), line_top);
        g_print("ARGV_END visible=%d caret=%d top=%d\n", visible ? 1 : 0,
                caret.get_line() + 1, top.get_line() + 1);
        fflush(stdout);
        state->end_done = true;
      }
      if (state->burn) {
        struct rusage before {};
        getrusage(RUSAGE_SELF, &before);
        const gint64 t0 = g_get_monotonic_time();
        volatile std::uint32_t x = 1;
        while (g_get_monotonic_time() - t0 < 1500000) {
          x = x * 1664525u + 1013904223u;
        }
        struct rusage after {};
        getrusage(RUSAGE_SELF, &after);
        const auto usec = [](const timeval& tv) {
          return tv.tv_sec * 1000000L + tv.tv_usec;
        };
        g_print("ARGV_BURN user=%ld sys=%ld wall=%ld\n",
                usec(after.ru_utime) - usec(before.ru_utime),
                usec(after.ru_stime) - usec(before.ru_stime),
                g_get_monotonic_time() - t0);
        fflush(stdout);
        if (x == 0) {
          g_print("ARGV_BURN_SINK\n");
        }
      }
      state->app->quit();
      return G_SOURCE_REMOVE;
    }
    if (state->saw_window && windows == 0) {
      g_print("ARGV_CANCELLED\n");
      fflush(stdout);
      state->app->quit();
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }

  static int run_argv_child(int argc, char** argv) {
    if (argc < 2) {
      std::cerr << "argv child needs a file\n";
      return 2;
    }
    const char* display = g_getenv("DISPLAY");
    if (display == nullptr || display[0] == '\0') {
      std::cerr << "GUI tests require a display; refusing to skip\n";
      return 1;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    g_set_prgname("lunduke-edit");
    g_set_application_name("Lunduke Edit");
    if (g_getenv("GDK_BACKEND") == nullptr) {
      g_setenv("GDK_BACKEND", "x11", FALSE);
    }
    Gsv::init();
    auto app = Application::create();
    auto* state = new ArgvChildState;
    state->app = app.get();
    state->path = argv[1];
    state->escape = g_getenv("LUNDUKE_EDIT_TEST_ARGV_ESCAPE") != nullptr;
    state->burn = g_getenv("LUNDUKE_EDIT_TEST_ARGV_BURN") != nullptr;
    if (state->escape) {
      // A high-priority idle runs inside present(), before the window can
      // accept a key, and one dropped Escape used to count as sent.
      g_timeout_add(50, argv_child_escape_idle, state);
    }
    g_timeout_add(20, argv_child_watch, state);
    // Give up rather than hang the suite if the load never finishes.
    g_timeout_add(300000, +[](gpointer data) -> gboolean {
      auto* app = static_cast<Application*>(data);
      g_print("ARGV_TIMEOUT\n");
      fflush(stdout);
      app->quit();
      return G_SOURCE_REMOVE;
    }, app.get());
    return app->run(argc, argv);
  }

  struct ArgvRun {
    bool ok{false};
    int status{-1};
    double map_ms{-1};
    double load_ms{-1};
    bool opening{false};
    bool watch{false};
    bool viewable{false};
    bool loaded{false};
    bool cancelled{false};
    int chars{0};
    double ready_ms{-1};
    double max_gap_ms{0};
    int progress_min{1000};
    int progress_max{-1};
    int progress_values{0};
    bool end_checked{false};
    bool end_visible{false};
    int end_caret{0};
    int end_top{0};
    bool burned{false};
    long burn_user{0};
    long burn_sys{0};
    long burn_wall{0};
    std::string throttle;
    std::string output;
  };

  static bool xid_is_viewable(unsigned long xid) {
    const std::string cmd =
        "xwininfo -id " + std::to_string(xid) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
      return false;
    }
    std::string text;
    char buf[256];
    while (fgets(buf, sizeof buf, pipe) != nullptr) {
      text += buf;
    }
    pclose(pipe);
    return text.find("IsViewable") != std::string::npos;
  }

  static void unset_inherited_test_env() {
    // DBUS_SESSION_BUS_ADDRESS stays set so a child uses the suite's bus
    // instead of autolaunching a dbus-daemon.
    const char* names[] = {
        "LUNDUKE_EDIT_TEST",
        "LUNDUKE_EDIT_TEST_DISCARD",
        "LUNDUKE_EDIT_TEST_LARGE",
        "LUNDUKE_EDIT_TEST_MAX_OPEN",
        "LUNDUKE_EDIT_TEST_MAX_OPEN_HARD",
        "LUNDUKE_EDIT_TEST_CHUNK",
        "LUNDUKE_EDIT_TEST_MAX_HITS",
        "LUNDUKE_EDIT_TEST_HUGE_BYTES",
        "LUNDUKE_EDIT_TEST_HUGE_UNDO",
        "LUNDUKE_EDIT_TEST_COMMIT_STEP",
        "LUNDUKE_EDIT_TEST_REPLACE",
        "LUNDUKE_EDIT_TEST_MAX_PASTE",
        "LUNDUKE_EDIT_TEST_SAVE_AS",
    };
    for (const char* name : names) {
      unsetenv(name);
    }
  }

  static void note_argv_line(ArgvRun& result, const std::string& line,
                             gint64 start, gint64& gap_last, bool& gap_open) {
    const gint64 now = g_get_monotonic_time();
    const bool timed = line == "ARGV_TICK" ||
                       line.compare(0, 14, "ARGV_PROGRESS ") == 0 ||
                       line == "ARGV_READY" ||
                       line.compare(0, 12, "ARGV_LOADED ") == 0;
    if (timed && gap_open && !result.loaded && gap_last > 0) {
      const double gap = static_cast<double>(now - gap_last) / 1000.0;
      if (gap > result.max_gap_ms) {
        result.max_gap_ms = gap;
      }
    }
    if (timed) {
      gap_last = now;
    }
    if (line.compare(0, 12, "ARGV_MAPPED ") == 0 && result.map_ms < 0) {
      unsigned long xid = 0;
      int watch = 0;
      int viewable = 0;
      char status_text[512];
      status_text[0] = '\0';
      if (std::sscanf(line.c_str(),
                      "ARGV_MAPPED xid=%lu watch=%d viewable=%d status=%511[^\n]",
                      &xid, &watch, &viewable, status_text) >= 3) {
        result.map_ms = static_cast<double>(now - start) / 1000.0;
        result.watch = watch != 0;
        result.viewable = viewable != 0;
        result.opening = std::string(status_text).find("Opening") !=
                         std::string::npos;
        gap_open = true;
        gap_last = now;
        if (!result.viewable) {
          for (int attempt = 0; attempt < 10 && !result.viewable; ++attempt) {
            result.viewable = xid_is_viewable(xid);
            if (!result.viewable) {
              g_usleep(20 * 1000);
            }
          }
        } else {
          result.viewable = result.viewable || xid_is_viewable(xid);
        }
      }
    } else if (line.compare(0, 14, "ARGV_PROGRESS ") == 0) {
      int pct = 0;
      if (std::sscanf(line.c_str(), "ARGV_PROGRESS %d", &pct) == 1) {
        ++result.progress_values;
        if (pct < result.progress_min) {
          result.progress_min = pct;
        }
        if (pct > result.progress_max) {
          result.progress_max = pct;
        }
      }
    } else if (line == "ARGV_READY") {
      if (result.ready_ms < 0) {
        result.ready_ms = static_cast<double>(now - start) / 1000.0;
      }
    } else if (line.compare(0, 12, "ARGV_LOADED ") == 0) {
      result.loaded = true;
      result.load_ms = static_cast<double>(now - start) / 1000.0;
      std::sscanf(line.c_str(), "ARGV_LOADED chars=%d", &result.chars);
    } else if (line == "ARGV_CANCELLED") {
      result.cancelled = true;
      result.load_ms = static_cast<double>(now - start) / 1000.0;
    } else if (line.compare(0, 9, "ARGV_END ") == 0) {
      int visible = 0;
      result.end_checked = true;
      std::sscanf(line.c_str(), "ARGV_END visible=%d caret=%d top=%d", &visible,
                  &result.end_caret, &result.end_top);
      result.end_visible = visible != 0;
    } else if (line.compare(0, 10, "ARGV_BURN ") == 0) {
      result.burned = true;
      std::sscanf(line.c_str(), "ARGV_BURN user=%ld sys=%ld wall=%ld",
                  &result.burn_user, &result.burn_sys, &result.burn_wall);
    }
  }

  static ArgvRun run_argv_file(const std::string& path, bool throttle,
                               bool escape, int deadline_ms, bool burn = false,
                               bool ctrl_end = false) {
    ArgvRun result;
    const bool have_taskset = access("/usr/bin/taskset", X_OK) == 0;
    const bool have_cpulimit = access("/usr/bin/cpulimit", X_OK) == 0;
    if (throttle && !have_cpulimit) {
      result.throttle = "cpulimit missing";
      result.output =
          "cpulimit is required for the throttled test and was not found "
          "at /usr/bin/cpulimit; refusing to fall back\n";
      return result;
    }
    if (throttle && have_taskset) {
      result.throttle = "taskset -c 0 + cpulimit -c 1 -l 25";
    } else if (throttle) {
      result.throttle = "cpulimit -c 1 -l 25";
    } else {
      result.throttle = "unthrottled";
    }

    int fds[2];
    if (pipe(fds) != 0) {
      result.output = "pipe failed";
      return result;
    }
    const gint64 start = g_get_monotonic_time();
    const pid_t pid = fork();
    if (pid < 0) {
      close(fds[0]);
      close(fds[1]);
      result.output = "fork failed";
      return result;
    }
    if (pid == 0) {
      setpgid(0, 0);
      dup2(fds[1], STDOUT_FILENO);
      close(fds[0]);
      close(fds[1]);
      unset_inherited_test_env();
      setenv("LUNDUKE_EDIT_TEST_ARGV_CHILD", "1", 1);
      if (escape) {
        setenv("LUNDUKE_EDIT_TEST_ARGV_ESCAPE", "1", 1);
      } else {
        unsetenv("LUNDUKE_EDIT_TEST_ARGV_ESCAPE");
      }
      if (burn) {
        setenv("LUNDUKE_EDIT_TEST_ARGV_BURN", "1", 1);
      } else {
        unsetenv("LUNDUKE_EDIT_TEST_ARGV_BURN");
      }
      if (ctrl_end) {
        setenv("LUNDUKE_EDIT_TEST_ARGV_END", "1", 1);
      } else {
        unsetenv("LUNDUKE_EDIT_TEST_ARGV_END");
      }
      const char* bin = argv0.c_str();
      if (throttle && have_taskset) {
        execl("/usr/bin/taskset", "taskset", "-c", "0", "/usr/bin/cpulimit",
              "-f", "-q", "-c", "1", "-l", "25", "--", bin, path.c_str(),
              static_cast<char*>(nullptr));
      } else if (throttle) {
        execl("/usr/bin/cpulimit", "cpulimit", "-f", "-q", "-c", "1", "-l",
              "25", "--", bin, path.c_str(), static_cast<char*>(nullptr));
      } else {
        execl(bin, bin, path.c_str(), static_cast<char*>(nullptr));
      }
      _exit(127);
    }
    setpgid(pid, pid);
    remember_child(pid);
    close(fds[1]);

    std::string pending;
    bool child_done = false;
    int status = -1;
    gint64 gap_last = 0;
    bool gap_open = false;
    while (!child_done) {
      const gint64 now = g_get_monotonic_time();
      if ((now - start) / 1000 > deadline_ms) {
        kill(-pid, SIGKILL);
        result.output += "\nPARENT_TIMEOUT\n";
      }
      pollfd pfd {};
      pfd.fd = fds[0];
      pfd.events = POLLIN;
      poll(&pfd, 1, 100);
      if (pfd.revents & (POLLIN | POLLHUP)) {
        char buf[1024];
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n > 0) {
          pending.append(buf, static_cast<std::size_t>(n));
        }
      }
      std::size_t nl = 0;
      while ((nl = pending.find('\n')) != std::string::npos) {
        const std::string line = pending.substr(0, nl);
        pending.erase(0, nl + 1);
        result.output += line;
        result.output += '\n';
        note_argv_line(result, line, start, gap_last, gap_open);
      }
      int st = 0;
      const pid_t got = waitpid(pid, &st, WNOHANG);
      if (got == pid) {
        status = st;
        child_done = true;
      } else if ((g_get_monotonic_time() - start) / 1000 > deadline_ms) {
        kill(-pid, SIGKILL);
        waitpid(pid, &st, 0);
        status = st;
        child_done = true;
      }
      if (child_done) {
        // The child can exit between poll and waitpid. Read what it wrote.
        const int flags = fcntl(fds[0], F_GETFL, 0);
        if (flags >= 0) {
          fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
        }
        while (true) {
          char buf[1024];
          const ssize_t n = read(fds[0], buf, sizeof buf);
          if (n > 0) {
            pending.append(buf, static_cast<std::size_t>(n));
            continue;
          }
          break;
        }
      }
    }
    std::size_t nl = 0;
    while ((nl = pending.find('\n')) != std::string::npos) {
      const std::string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      result.output += line;
      result.output += '\n';
      note_argv_line(result, line, start, gap_last, gap_open);
    }
    close(fds[0]);
    forget_child(pid);
    result.status = status;
    result.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return result;
  }

  static void test_argv_large_open(const std::string& dir) {
    std::cout << "argv-large begin\n";
    if (argv0.empty()) {
      expect(false, "argv open test knows its executable");
      return;
    }
    const std::string path = dir + "/argv-12mb.txt";
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      const char piece[] = "foo bar baz\n";
      for (int i = 0; i < 1000000; ++i) {
        out.write(piece, 12);
      }
    }
    expect(read_bytes(path).size() == 12000000, "argv fixture is 12000000 bytes");

    const ArgvRun plain = run_argv_file(path, false, false, 240000, false, true);
    std::cout << "argv unthrottled map_ms=" << plain.map_ms
              << " ready_ms=" << plain.ready_ms << " load_ms=" << plain.load_ms
              << " gap_ms=" << plain.max_gap_ms
              << " progress=" << plain.progress_min << ".." << plain.progress_max
              << " steps=" << plain.progress_values << " chars=" << plain.chars
              << " end_visible=" << plain.end_visible
              << " caret=" << plain.end_caret << " top=" << plain.end_top
              << " opening=" << plain.opening << " watch=" << plain.watch
              << " viewable=" << plain.viewable << " status=" << plain.status
              << "\n";
    if (!plain.loaded || !plain.end_visible) {
      std::cerr << plain.output;
    }
    expect(plain.ok && plain.loaded, "unthrottled argv open loads the file");
    expect(plain.chars == 12000000, "unthrottled argv open keeps every character");
    expect(plain.viewable && plain.map_ms >= 0 && plain.map_ms < 3000,
           "unthrottled argv open maps the window quickly");
    expect(plain.opening, "unthrottled argv open shows Opening");
    expect(plain.watch, "unthrottled argv open shows the busy cursor");
    expect(plain.ready_ms >= plain.map_ms && plain.ready_ms < 15000,
           "unthrottled open clears Opening without a long freeze");
    expect(plain.progress_values >= 2 && plain.progress_max > plain.progress_min &&
               plain.progress_max > 0,
           "unthrottled open progress advances");
    expect(plain.max_gap_ms < 800, "unthrottled open keeps the main loop moving");
    expect(plain.end_checked && plain.end_visible,
           "Ctrl+End scrolls the caret on screen after a large open");
    expect(plain.end_caret > 1000 && plain.end_top + 80 >= plain.end_caret,
           "Ctrl+End view is the last lines, not the middle of the file");

    const ArgvRun slow = run_argv_file(path, true, false, 120000, true, false);
    const double burn_cpu = static_cast<double>(slow.burn_user + slow.burn_sys);
    const double burn_ratio =
        slow.burn_wall > 0 ? burn_cpu / static_cast<double>(slow.burn_wall) : 1.0;
    std::cout << "argv throttled (" << slow.throttle << ") map_ms="
              << slow.map_ms << " ready_ms=" << slow.ready_ms
              << " load_ms=" << slow.load_ms << " gap_ms=" << slow.max_gap_ms
              << " progress=" << slow.progress_min << ".." << slow.progress_max
              << " steps=" << slow.progress_values << " chars=" << slow.chars
              << " opening=" << slow.opening << " watch=" << slow.watch
              << " viewable=" << slow.viewable << " burn_user=" << slow.burn_user
              << " burn_sys=" << slow.burn_sys << " burn_wall=" << slow.burn_wall
              << " burn_ratio=" << burn_ratio << "\n";
    if (!slow.loaded || !slow.burned || burn_ratio > 0.50) {
      std::cerr << slow.output;
    }
    expect(slow.throttle.find("cpulimit") != std::string::npos &&
               slow.throttle.find("unavailable") == std::string::npos &&
               slow.throttle.find("missing") == std::string::npos,
           "throttled argv open uses cpulimit and does not fall back");
    expect(slow.ok && slow.loaded, "throttled argv open loads the file");
    expect(slow.chars == 12000000, "throttled argv open keeps every character");
    expect(slow.viewable && slow.map_ms >= 0 && slow.map_ms < 4000,
           "throttled argv open maps the window within a few seconds");
    expect(slow.opening, "throttled argv open shows Opening when the window maps");
    expect(slow.watch, "throttled argv open shows the busy cursor");
    expect(slow.ready_ms >= slow.map_ms, "throttled open reports Opening cleared");
    expect(slow.progress_values >= 2 && slow.progress_max > slow.progress_min &&
               slow.progress_max > 0,
           "throttled open progress advances");
    expect(slow.max_gap_ms < 2000,
           "throttled open does not freeze the main loop for seconds");
    expect(slow.burned && slow.burn_wall >= 1000000, "throttle burn ran");
    expect(burn_ratio <= 0.50,
           "cpulimit -l 25 holds the child near a quarter of one CPU");

    const ArgvRun esc = run_argv_file(path, true, true, 30000);
    std::cout << "argv escape (" << esc.throttle << ") map_ms=" << esc.map_ms
              << " cancel_ms=" << esc.load_ms << " cancelled=" << esc.cancelled
              << " loaded=" << esc.loaded << "\n";
    if (!esc.cancelled) {
      std::cerr << esc.output;
    }
    expect(esc.viewable && esc.map_ms >= 0 && esc.map_ms < 4000,
           "escape run maps the window quickly");
    expect(esc.opening, "escape run shows Opening");
    expect(esc.cancelled && !esc.loaded, "escape cancels the argv open");
    expect(esc.ok, "escape cancel exits cleanly");
    std::cout << "argv-large end\n";
  }

  static std::string make_replace_fixture(const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const char piece[] = "line dog sit quick ipsum fox value line\n";
    const std::size_t piece_n = sizeof piece - 1;
    std::size_t total = 0;
    while (total < 1000000) {
      out.write(piece, static_cast<std::streamsize>(piece_n));
      total += piece_n;
    }
    return path;
  }

  struct LoopGap {
    MainWindow* window{nullptr};
    gint64 last{0};
    gint64 max_gap{0};
    int progress_n{0};
    int last_pct{-1};
  };

  static gboolean loop_gap_cb(gpointer data) {
    auto* gap = static_cast<LoopGap*>(data);
    const gint64 now = g_get_monotonic_time();
    if (gap->last > 0) {
      gap->max_gap = std::max(gap->max_gap, now - gap->last);
    }
    gap->last = now;
    if (gap->window->find_scan_.active &&
        gap->window->find_scan_.progress_pct != gap->last_pct &&
        gap->window->find_scan_.progress_pct >= 0) {
      gap->last_pct = gap->window->find_scan_.progress_pct;
      ++gap->progress_n;
    }
    return G_SOURCE_CONTINUE;
  }

  static void mark_buffer_clean(MainWindow& w) {
    if (auto buf = w.buffer()) {
      buf->set_modified(false);
    }
    w.encoding_dirty_ = false;
    w.file_missing_ = false;
    w.file_unreadable_ = false;
    w.refresh_dirty_from_buffer();
  }

  static void note_dirty(MainWindow& w, bool& saw) {
    if (w.dirty_) {
      saw = true;
    }
  }

  static void settle_cancel(MainWindow& w, bool& saw) {
    for (int i = 0; i < 400000 && (w.find_scan_.active || w.bulk_undo_running_);
         ++i) {
      g_main_context_iteration(nullptr, false);
      note_dirty(w, saw);
    }
    for (int i = 0; i < 40; ++i) {
      g_main_context_iteration(nullptr, false);
      note_dirty(w, saw);
    }
  }

  // Escape while the huge-undo dialog is up, before the swap, and during
  // each part of the swap. A cancelled Replace All must finish unmodified
  // and must not publish a dirty title at any point in between.
  static void test_replace_cancel_phases(MainWindow& w) {
    std::cout << "round7 cancel-phases begin\n";
    const Glib::ustring original = w.buffer()->get_text();
    expect(original.bytes() >= 1000000,
           "cancel phases start from the 1MB fixture");
    FindOptions opts;
    opts.search_for = "line ";
    opts.replace_with = "row ";
    opts.case_sensitive = true;

    mark_buffer_clean(w);
    g_setenv("LUNDUKE_EDIT_TEST_HUGE_BYTES", "1000", TRUE);
    g_setenv("LUNDUKE_EDIT_TEST_HUGE_UNDO", "deny", TRUE);
    bool saw = false;
    note_dirty(w, saw);
    w.start_replace_all(opts, nullptr);
    note_dirty(w, saw);
    std::cout << "cancel-phase confirm saw_dirty=" << saw
              << " modified=" << w.buffer()->get_modified()
              << " dirty=" << w.dirty_ << "\n";
    expect(!w.find_scan_.active, "denying the huge-undo dialog does not apply");
    expect(w.buffer()->get_text() == original,
           "denying the huge-undo dialog leaves the text unchanged");
    expect(!w.buffer()->get_modified() && !w.dirty_,
           "denying the huge-undo dialog leaves the buffer clean");
    expect(!saw, "denying the huge-undo dialog never marks the buffer dirty");

    g_setenv("LUNDUKE_EDIT_TEST_HUGE_UNDO", "allow", TRUE);
    mark_buffer_clean(w);
    w.start_replace_all(opts, nullptr);
    saw = false;
    bool before_apply = false;
    for (int i = 0; i < 400000 && w.find_scan_.active; ++i) {
      g_main_context_iteration(nullptr, false);
      note_dirty(w, saw);
      if (!before_apply && !w.find_scan_.commit_started &&
          w.find_scan_.progress_pct >= 1) {
        w.cancel_find_scan();
        note_dirty(w, saw);
        before_apply = true;
      }
    }
    settle_cancel(w, saw);
    std::cout << "cancel-phase before-apply saw_dirty=" << saw
              << " hit=" << before_apply
              << " modified=" << w.buffer()->get_modified()
              << " dirty=" << w.dirty_ << "\n";
    expect(before_apply, "Escape before the apply sees the counting pass");
    expect(w.buffer()->get_text() == original,
           "Escape before the apply leaves the text unchanged");
    expect(!w.buffer()->get_modified() && !w.dirty_,
           "Escape before the apply leaves the buffer clean");
    expect(!saw, "Escape before the apply never marks the buffer dirty");

    auto cancel_at = [&](const char* step, const char* label, auto pred) {
      mark_buffer_clean(w);
      g_setenv("LUNDUKE_EDIT_TEST_COMMIT_STEP", step, TRUE);
      w.start_replace_all(opts, nullptr);
      bool hit = false;
      bool phase_saw = false;
      for (int i = 0; i < 400000 && (w.find_scan_.active || !hit); ++i) {
        g_main_context_iteration(nullptr, false);
        note_dirty(w, phase_saw);
        if (!hit && pred()) {
          w.cancel_find_scan();
          note_dirty(w, phase_saw);
          hit = true;
          break;
        }
        if (!w.find_scan_.active) {
          break;
        }
      }
      settle_cancel(w, phase_saw);
      std::cout << "cancel-phase " << label << " saw_dirty=" << phase_saw
                << " hit=" << hit
                << " modified=" << w.buffer()->get_modified()
                << " dirty=" << w.dirty_
                << " text_same=" << (w.buffer()->get_text() == original)
                << "\n";
      expect(hit, label);
      expect(w.buffer()->get_text() == original, label);
      expect(!w.buffer()->get_modified() && !w.dirty_, label);
      expect(!phase_saw, label);
      g_unsetenv("LUNDUKE_EDIT_TEST_COMMIT_STEP");
    };
    cancel_at("erase", "Escape during the swap erase leaves the buffer clean",
              [&]() {
                return w.find_scan_.commit_started && !w.find_scan_.commit_erased;
              });
    cancel_at(
        "insert", "Escape during the swap insert leaves the buffer clean",
        [&]() {
          return w.find_scan_.commit_erased && !w.find_scan_.commit_inserted;
        });
    cancel_at("line",
              "Escape during the swap line notes leaves the buffer clean",
              [&]() {
                return w.find_scan_.commit_inserted &&
                       !w.find_scan_.commit_lines_noted;
              });

    // Span above the heavy-commit threshold, hay below the sliced-undo
    // threshold: cancel takes the single undo() path.
    g_unsetenv("LUNDUKE_EDIT_TEST_HUGE_BYTES");
    g_unsetenv("LUNDUKE_EDIT_TEST_HUGE_UNDO");
    g_setenv("LUNDUKE_EDIT_TEST_COMMIT_STEP", "erase", TRUE);
    std::string small;
    small.reserve(12000);
    while (small.size() < 10000) {
      small += "line x\n";
    }
    w.buffer()->set_text(small);
    mark_buffer_clean(w);
    const Glib::ustring small_original = w.buffer()->get_text();
    w.start_replace_all(opts, nullptr);
    saw = false;
    bool small_hit = false;
    bool small_sliced = false;
    for (int i = 0; i < 400000 && w.find_scan_.active; ++i) {
      g_main_context_iteration(nullptr, false);
      note_dirty(w, saw);
      if (!small_hit && w.find_scan_.commit_started &&
          !w.find_scan_.commit_erased) {
        small_sliced = w.find_scan_.hay.size() >= 32u * 1024u;
        w.cancel_find_scan();
        note_dirty(w, saw);
        small_hit = true;
      }
    }
    settle_cancel(w, saw);
    std::cout << "cancel-phase small-undo saw_dirty=" << saw
              << " hit=" << small_hit << " sliced=" << small_sliced
              << " modified=" << w.buffer()->get_modified()
              << " dirty=" << w.dirty_ << "\n";
    expect(small_hit && !small_sliced,
           "a short heavy commit can be cancelled at the swap");
    expect(w.buffer()->get_text() == small_original,
           "cancelling a short swap leaves the text unchanged");
    expect(!w.buffer()->get_modified() && !w.dirty_ && !saw,
           "cancelling a short swap leaves the buffer clean");

    g_unsetenv("LUNDUKE_EDIT_TEST_HUGE_BYTES");
    g_unsetenv("LUNDUKE_EDIT_TEST_HUGE_UNDO");
    g_unsetenv("LUNDUKE_EDIT_TEST_COMMIT_STEP");
    std::cout << "round7 cancel-phases end\n";
  }

  static void test_round7_status_and_replace(MainWindow& w, const std::string& dir) {
    std::cout << "round7 status begin\n";
    w.buffer()->set_text("");
    w.buffer()->place_cursor(w.buffer()->begin());
    expect(w.status_pos_.get_text() == "Ln 1, Col 1",
           "an empty buffer reports Ln 1, Col 1");
    w.buffer()->insert_at_cursor("Zab");
    expect(w.status_pos_.get_text() == "Ln 1, Col 4",
           "typing updates the status column");
    w.buffer()->insert_at_cursor("\n");
    expect(w.status_pos_.get_text() == "Ln 2, Col 1",
           "Return updates the status line");
    w.buffer()->insert_at_cursor("q");
    expect(w.status_pos_.get_text() == "Ln 2, Col 2",
           "typing after Return updates the column");
    w.on_undo();
    expect(w.buffer()->get_text() == "Zab\n", "undo removes the typed character");
    expect(w.status_pos_.get_text() == "Ln 2, Col 1", "undo updates Ln/Col");
    w.insert_pasted_text("xyz");
    expect(w.status_pos_.get_text() == "Ln 2, Col 4", "paste updates Ln/Col");
    FindOptions one;
    one.search_for = "Zab";
    one.replace_with = "Q";
    one.case_sensitive = true;
    expect(w.replace_all(one) == 1, "a single replace hits");
    {
      auto iter = w.buffer()->get_iter_at_mark(w.buffer()->get_insert());
      const std::string want =
          "Ln " + std::to_string(iter.get_line() + 1) + ", Col " +
          std::to_string(w.display_column_at(iter));
      expect(w.status_pos_.get_text() == want, "Replace updates Ln/Col");
    }

    std::cout << "round7 replace begin\n";
    const std::string path = make_replace_fixture(dir + "/replace-1mb.txt");
    w.buffer()->set_text("");
    w.buffer()->set_modified(false);
    expect(w.open_file(path), "1MB replace fixture opens");
    const Glib::ustring original = w.buffer()->get_text();
    expect(original.bytes() >= 1000000, "replace fixture is at least 1MB");

    FindOptions opts;
    opts.search_for = "line ";
    opts.replace_with = "row ";
    opts.case_sensitive = false;
    opts.wrap_around = true;

    LoopGap gap;
    gap.window = &w;
    const guint timer =
        g_timeout_add_full(G_PRIORITY_DEFAULT, 1, loop_gap_cb, &gap, nullptr);
    w.start_replace_all(opts, nullptr);
    expect(w.status_find_.get_text().find("Replacing") == 0,
           "Replace All shows a status line as soon as it starts");
    bool cancelled = false;
    int progress_steps = 0;
    int last_pct = -1;
    gint64 loop_last = g_get_monotonic_time();
    gint64 loop_gap = 0;
    for (int i = 0; i < 200000 && w.find_scan_.active; ++i) {
      g_main_context_iteration(nullptr, false);
      const gint64 now = g_get_monotonic_time();
      loop_gap = std::max(loop_gap, now - loop_last);
      loop_last = now;
      if (w.find_scan_.progress_pct >= 0 &&
          w.find_scan_.progress_pct != last_pct) {
        last_pct = w.find_scan_.progress_pct;
        ++progress_steps;
      }
      if (!cancelled && progress_steps >= 2 && w.find_scan_.progress_pct > 0) {
        GdkEventKey key {};
        key.type = GDK_KEY_PRESS;
        key.keyval = GDK_KEY_Escape;
        expect(w.on_key_press_event(&key), "Escape cancels Replace All");
        cancelled = true;
      }
    }
    g_source_remove(timer);
    const gint64 cancel_gap = std::max(gap.max_gap, loop_gap);
    std::cout << "round7 cancel gap_us=" << cancel_gap
              << " progress_steps=" << progress_steps
              << " slice_us=" << w.find_scan_.max_slice_us << "\n";
    expect(cancelled, "Replace All reported progress before cancel");
    expect(progress_steps >= 2, "Replace All updates progress more than once");
    expect(cancel_gap < 500000, "Replace All returns to the main loop");
    expect(w.find_scan_.max_slice_us < 500000, "Replace All slices stay short");
    expect(!w.find_scan_.active, "cancelled Replace All is idle");
    expect(w.buffer()->get_text() == original,
           "cancel leaves the buffer unchanged");
    expect(w.status_find_.get_text().find("Replacing") == std::string::npos,
           "cancel clears the Replacing status");

    FindReplaceDialog dlg(w, opts);
    dlg.signal_hide().connect([&w]() { w.on_find_dialog_hidden(); });
    dlg.show();
    gtk_widget_realize(GTK_WIDGET(dlg.gobj()));
    flush_ui();
    w.start_replace_all(opts, &dlg);
    bool dialog_cancel = false;
    for (int i = 0; i < 200000 && w.find_scan_.active; ++i) {
      g_main_context_iteration(nullptr, false);
      if (!dialog_cancel && w.find_scan_.progress_pct >= 30 && dlg.get_window()) {
        GdkEvent* event = gdk_event_new(GDK_KEY_PRESS);
        event->key.window =
            GDK_WINDOW(g_object_ref(dlg.get_window()->gobj()));
        event->key.keyval = GDK_KEY_Escape;
        event->key.time = GDK_CURRENT_TIME;
        gtk_widget_event(GTK_WIDGET(dlg.gobj()), event);
        gdk_event_free(event);
        dialog_cancel = true;
      }
    }
    expect(dialog_cancel && !w.find_scan_.active,
           "Escape in the Find dialog cancels Replace All");
    expect(w.buffer()->get_text() == original,
           "dialog Escape leaves the buffer unchanged");

    gap = LoopGap{};
    gap.window = &w;
    const guint timer2 =
        g_timeout_add_full(G_PRIORITY_DEFAULT, 1, loop_gap_cb, &gap, nullptr);
    const gint64 started = g_get_monotonic_time();
    w.start_replace_all(opts, nullptr);
    int done_steps = 0;
    int done_pct = -1;
    gint64 done_last = g_get_monotonic_time();
    gint64 done_gap = 0;
    for (int i = 0; i < 200000 && w.find_scan_.active; ++i) {
      g_main_context_iteration(nullptr, false);
      const gint64 now = g_get_monotonic_time();
      done_gap = std::max(done_gap, now - done_last);
      done_last = now;
      if (w.find_scan_.progress_pct >= 0 &&
          w.find_scan_.progress_pct != done_pct) {
        done_pct = w.find_scan_.progress_pct;
        ++done_steps;
      }
    }
    g_source_remove(timer2);
    const gint64 replace_us = g_get_monotonic_time() - started;
    const gint64 replace_gap = std::max(gap.max_gap, done_gap);
    std::cout << "round7 replace_us=" << replace_us
              << " gap_us=" << replace_gap
              << " progress_steps=" << done_steps
              << " slice_us=" << w.find_scan_.max_slice_us << "\n";
    expect(!w.find_scan_.active, "Replace All finishes");
    expect(w.buffer()->get_text() != original, "Replace All changes the text");
    expect(w.buffer()->get_text().find("line ") == Glib::ustring::npos,
           "Replace All replaces every line ");
    expect(done_steps >= 2, "a finished Replace All posted progress");
    expect(replace_gap < 500000, "a finished Replace All stays responsive");
    expect(w.find_scan_.max_slice_us < 500000, "the commit slice stays short");
    expect(w.buffer()->can_undo(), "Replace All is one undo step");
    w.on_undo();
    expect(w.buffer()->get_text() == original, "one undo restores Replace All");
    expect(!w.buffer()->can_undo(), "Replace All did not push a second undo");
    {
      auto iter = w.buffer()->get_iter_at_mark(w.buffer()->get_insert());
      const std::string want =
          "Ln " + std::to_string(iter.get_line() + 1) + ", Col " +
          std::to_string(w.display_column_at(iter));
      expect(w.status_pos_.get_text() == want,
             "Ln/Col matches the caret after undo of Replace All");
    }
    std::cout << "round7 replace end\n";

    test_replace_cancel_phases(w);

    // A replacement whose worst-case undo record exceeds the huge-undo
    // limit takes a counting pass before it builds anything. That pass
    // has to show "Replacing…" instead of a blank status line.
    std::cout << "round7 count-status begin\n";
    {
      std::string lines;
      lines.reserve(4000);
      for (int i = 0; i < 2000; ++i) {
        lines += "a\n";
      }
      w.buffer()->set_text(lines);
      flush_ui();
      const Glib::ustring before = w.buffer()->get_text();
      FindOptions count_opts;
      count_opts.search_for = "a";
      // A long replacement makes the worst-case undo estimate exceed the
      // huge-undo limit, which is what turns the counting pass on.
      count_opts.replace_with = std::string(10000, 'b');
      count_opts.case_sensitive = true;
      w.start_replace_all(count_opts, nullptr);
      expect(w.find_scan_.counting,
             "an oversized undo estimate counts matches before editing");
      expect(w.status_find_.get_text().find("Replacing") == 0,
             "the counting pass shows Replacing immediately");
      int last_pct = w.find_scan_.progress_pct;
      bool advanced = false;
      for (int i = 0; i < 200000 && w.find_scan_.counting; ++i) {
        g_main_context_iteration(nullptr, false);
        if (w.buffer()->get_text() != before) {
          expect(false, "the counting pass does not change the buffer");
          break;
        }
        if (w.find_scan_.progress_pct > last_pct) {
          advanced = true;
          last_pct = w.find_scan_.progress_pct;
        }
        if (advanced && last_pct >= 1) {
          break;
        }
      }
      expect(advanced && last_pct >= 1,
             "the counting pass moves the percentage");
      GdkEventKey key {};
      key.type = GDK_KEY_PRESS;
      key.keyval = GDK_KEY_Escape;
      w.on_key_press_event(&key);
      expect(!w.find_scan_.active, "Escape during counting cancels");
      expect(w.buffer()->get_text() == before,
             "cancelling the counting pass leaves the buffer unchanged");
      expect(w.status_find_.get_text().find("Replacing") == std::string::npos,
             "cancelling the counting pass clears the status");
    }
    std::cout << "round7 count-status end\n";
  }

  struct ToolRun {
    int status{-1};
    bool ok{false};
    std::string output;
  };

  static ToolRun run_tool(const std::vector<std::string>& args, bool throttle,
                          const std::vector<std::pair<std::string, std::string>>& env,
                          int deadline_ms) {
    ToolRun result;
    const bool have_taskset = access("/usr/bin/taskset", X_OK) == 0;
    const bool have_cpulimit = access("/usr/bin/cpulimit", X_OK) == 0;
    if (throttle && !have_cpulimit) {
      result.output =
          "cpulimit is required for the throttled test and was not found "
          "at /usr/bin/cpulimit; refusing to fall back\n";
      return result;
    }
    int fds[2];
    if (pipe(fds) != 0) {
      result.output = "pipe failed\n";
      return result;
    }
    const gint64 start = g_get_monotonic_time();
    const pid_t pid = fork();
    if (pid < 0) {
      close(fds[0]);
      close(fds[1]);
      result.output = "fork failed\n";
      return result;
    }
    if (pid == 0) {
      setpgid(0, 0);
      dup2(fds[1], STDOUT_FILENO);
      close(fds[0]);
      close(fds[1]);
      unset_inherited_test_env();
      for (const auto& item : env) {
        setenv(item.first.c_str(), item.second.c_str(), 1);
      }
      std::vector<char*> argv;
      std::vector<std::string> storage;
      if (throttle && have_taskset) {
        storage.push_back("taskset");
        storage.push_back("-c");
        storage.push_back("0");
        storage.push_back("/usr/bin/cpulimit");
        storage.push_back("-f");
        storage.push_back("-q");
        storage.push_back("-c");
        storage.push_back("1");
        storage.push_back("-l");
        storage.push_back("25");
        storage.push_back("--");
      } else if (throttle) {
        storage.push_back("cpulimit");
        storage.push_back("-f");
        storage.push_back("-q");
        storage.push_back("-c");
        storage.push_back("1");
        storage.push_back("-l");
        storage.push_back("25");
        storage.push_back("--");
      }
      storage.insert(storage.end(), args.begin(), args.end());
      for (auto& s : storage) {
        argv.push_back(s.data());
      }
      argv.push_back(nullptr);
      if (throttle && have_taskset) {
        execv("/usr/bin/taskset", argv.data());
      } else if (throttle) {
        execv("/usr/bin/cpulimit", argv.data());
      } else {
        execv(args[0].c_str(), argv.data());
      }
      _exit(127);
    }
    setpgid(pid, pid);
    remember_child(pid);
    close(fds[1]);
    std::string pending;
    bool child_done = false;
    int status = -1;
    while (!child_done) {
      if ((g_get_monotonic_time() - start) / 1000 > deadline_ms) {
        kill(-pid, SIGKILL);
        result.output += "\nPARENT_TIMEOUT\n";
      }
      pollfd pfd {};
      pfd.fd = fds[0];
      pfd.events = POLLIN;
      poll(&pfd, 1, 100);
      if (pfd.revents & (POLLIN | POLLHUP)) {
        char buf[1024];
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n > 0) {
          pending.append(buf, static_cast<std::size_t>(n));
        }
      }
      int st = 0;
      const pid_t got = waitpid(pid, &st, WNOHANG);
      if (got == pid) {
        status = st;
        child_done = true;
      } else if ((g_get_monotonic_time() - start) / 1000 > deadline_ms) {
        kill(-pid, SIGKILL);
        waitpid(pid, &st, 0);
        status = st;
        child_done = true;
      }
      if (child_done) {
        const int flags = fcntl(fds[0], F_GETFL, 0);
        if (flags >= 0) {
          fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
        }
        while (true) {
          char buf[1024];
          const ssize_t n = read(fds[0], buf, sizeof buf);
          if (n <= 0) {
            break;
          }
          pending.append(buf, static_cast<std::size_t>(n));
        }
      }
    }
    close(fds[0]);
    forget_child(pid);
    result.output = pending;
    result.status = status;
    result.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return result;
  }

  static int run_replace_child(int argc, char** argv) {
    if (argc < 2) {
      std::cerr << "replace child needs a file\n";
      return 2;
    }
    const char* display = g_getenv("DISPLAY");
    if (display == nullptr || display[0] == '\0') {
      std::cerr << "GUI tests require a display; refusing to skip\n";
      return 1;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    g_setenv("LUNDUKE_EDIT_TEST", "1", TRUE);
    if (g_getenv("GDK_BACKEND") == nullptr) {
      g_setenv("GDK_BACKEND", "x11", FALSE);
    }
    Gsv::init();
    auto app = Application::create();
    if (!app->register_application()) {
      std::cerr << "replace child could not register\n";
      return 1;
    }
    auto* w = app->create_window();
    w->present();
    flush_ui();
    if (!w->open_file(argv[1])) {
      g_print("REPLACE_FAIL open\n");
      return 1;
    }
    const Glib::ustring original = w->buffer()->get_text();
    const bool cancel = g_getenv("LUNDUKE_EDIT_TEST_REPLACE_CANCEL") != nullptr;
    // Let cpulimit finish punishing the open burst before the gap is measured.
    g_usleep(1500 * 1000);
    LoopGap gap;
    gap.window = w;
    const guint timer =
        g_timeout_add_full(G_PRIORITY_DEFAULT, 5, loop_gap_cb, &gap, nullptr);
    FindOptions opts;
    opts.search_for = "line ";
    opts.replace_with = "row ";
    opts.case_sensitive = false;
    w->start_replace_all(opts, nullptr);
    bool sent_cancel = false;
    int progress_steps = 0;
    int last_pct = -1;
    for (int i = 0; i < 400000 && w->find_scan_.active; ++i) {
      g_main_context_iteration(nullptr, false);
      if (w->find_scan_.progress_pct >= 0 &&
          w->find_scan_.progress_pct != last_pct) {
        last_pct = w->find_scan_.progress_pct;
        ++progress_steps;
      }
      if (cancel && !sent_cancel && progress_steps >= 2 &&
          w->find_scan_.progress_pct > 0) {
        GdkEventKey key {};
        key.type = GDK_KEY_PRESS;
        key.keyval = GDK_KEY_Escape;
        w->on_key_press_event(&key);
        sent_cancel = true;
      }
    }
    g_source_remove(timer);
    g_print("REPLACE_GAP %ld\n", static_cast<long>(std::max(gap.max_gap, static_cast<gint64>(0))));
    g_print("REPLACE_SLICE %ld\n", static_cast<long>(w->find_scan_.max_slice_us));
    g_print("REPLACE_PROGRESS %d\n", progress_steps);
    if (cancel) {
      g_print("REPLACE_UNCHANGED %d\n",
              w->buffer()->get_text() == original ? 1 : 0);
    } else {
      const bool changed = w->buffer()->get_text() != original;
      w->on_undo();
      const bool restored = w->buffer()->get_text() == original;
      const bool single = !w->buffer()->can_undo();
      g_print("REPLACE_UNDO %d\n", (changed && restored && single) ? 1 : 0);
    }
    struct rusage before {};
    getrusage(RUSAGE_SELF, &before);
    const gint64 t0 = g_get_monotonic_time();
    volatile std::uint32_t x = 1;
    while (g_get_monotonic_time() - t0 < 1200000) {
      x = x * 1664525u + 1013904223u;
    }
    struct rusage after {};
    getrusage(RUSAGE_SELF, &after);
    const auto usec = [](const timeval& tv) {
      return static_cast<long>(tv.tv_sec * 1000000L + tv.tv_usec);
    };
    g_print("REPLACE_BURN user=%ld sys=%ld wall=%ld\n",
            usec(after.ru_utime) - usec(before.ru_utime),
            usec(after.ru_stime) - usec(before.ru_stime),
            static_cast<long>(g_get_monotonic_time() - t0));
    if (x == 0) {
      g_print("REPLACE_BURN_SINK\n");
    }
    fflush(stdout);
    return 0;
  }

  static long field_long(const std::string& text, const char* key) {
    const std::string needle = std::string(key) + " ";
    const auto at = text.find(needle);
    if (at == std::string::npos) {
      return -1;
    }
    return std::strtol(text.c_str() + at + needle.size(), nullptr, 10);
  }

  static void test_throttled_replace(const std::string& dir) {
    std::cout << "round7 throttled replace begin\n";
    if (argv0.empty()) {
      expect(false, "replace throttle test knows its executable");
      return;
    }
    const std::string path = make_replace_fixture(dir + "/replace-throttle.txt");
    const std::vector<std::pair<std::string, std::string>> base_env = {
        {"LUNDUKE_EDIT_TEST_REPLACE_CHILD", "1"},
        {"LUNDUKE_EDIT_TEST", "1"},
    };
    auto full_env = base_env;
    const ToolRun full =
        run_tool({argv0, path}, true, full_env, 90000);
    const long gap = field_long(full.output, "REPLACE_GAP");
    const long slice = field_long(full.output, "REPLACE_SLICE");
    const long progress = field_long(full.output, "REPLACE_PROGRESS");
    const long undo = field_long(full.output, "REPLACE_UNDO");
    const long burn_user = field_long(full.output, "user=");
    // REPLACE_BURN user= is not "user=" at the start of a token search that
    // might hit something else. Parse the burn line directly.
    long b_user = -1, b_sys = -1, b_wall = -1;
    const auto burn_at = full.output.find("REPLACE_BURN ");
    if (burn_at != std::string::npos) {
      std::sscanf(full.output.c_str() + burn_at,
                  "REPLACE_BURN user=%ld sys=%ld wall=%ld", &b_user, &b_sys,
                  &b_wall);
    }
    const double ratio =
        b_wall > 0 ? static_cast<double>(b_user + b_sys) / static_cast<double>(b_wall)
                   : 1.0;
    std::cout << "round7 throttled replace gap_us=" << gap
              << " slice_us=" << slice << " progress=" << progress
              << " undo=" << undo << " burn_ratio=" << ratio << "\n";
    if (!full.ok || gap < 0 || gap >= 800000 || undo != 1 || ratio > 0.55) {
      std::cerr << full.output;
    }
    expect(full.ok, "throttled Replace All child exits cleanly");
    expect(gap >= 0 && gap < 2000000,
           "throttled Replace All is not stuck for seconds");
    // cpulimit stops the process inside a slice, so the slice's wall time
    // includes that stop. The unthrottled run asserts the 500 ms budget.
    expect(slice >= 0 && slice < 1500000,
           "throttled Replace All slice is not a multi-second stall");
    expect(progress >= 2, "throttled Replace All posts progress");
    expect(undo == 1, "throttled Replace All is one undo of the original");
    expect(b_wall >= 1000000 && ratio <= 0.55,
           "throttled Replace All is really under cpulimit -l 25");

    auto cancel_env = base_env;
    cancel_env.push_back({"LUNDUKE_EDIT_TEST_REPLACE_CANCEL", "1"});
    const ToolRun cancelled = run_tool({argv0, path}, true, cancel_env, 90000);
    const long unchanged = field_long(cancelled.output, "REPLACE_UNCHANGED");
    const long cancel_gap = field_long(cancelled.output, "REPLACE_GAP");
    std::cout << "round7 throttled cancel unchanged=" << unchanged
              << " gap_us=" << cancel_gap << "\n";
    if (unchanged != 1) {
      std::cerr << cancelled.output;
    }
    expect(cancelled.ok && unchanged == 1,
           "throttled Replace All cancel leaves the buffer unchanged");
    expect(cancel_gap >= 0 && cancel_gap < 2000000,
           "throttled cancel is not stuck for seconds");
    std::cout << "round7 throttled replace end\n";
    (void)burn_user;
  }

  static int run_dnd_child(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const char* display = g_getenv("DISPLAY");
    if (display == nullptr || display[0] == '\0') {
      std::cerr << "GUI tests require a display; refusing to skip\n";
      return 1;
    }
    const char* want = g_getenv("LUNDUKE_EDIT_TEST_DND_PATH");
    if (want == nullptr || want[0] == '\0') {
      std::cerr << "dnd child needs a path\n";
      return 2;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    g_print("DND_CHILD_UP\n");
    fflush(stdout);
    g_setenv("LUNDUKE_EDIT_TEST", "1", TRUE);
    if (g_getenv("GDK_BACKEND") == nullptr) {
      g_setenv("GDK_BACKEND", "x11", FALSE);
    }
    Gsv::init();
    auto app = Application::create();
    struct DndWait {
      Application* app;
      std::string path;
      bool printed{false};
    };
    auto* wait = new DndWait{app.get(), want, false};
    g_timeout_add(50, +[](gpointer data) -> gboolean {
      auto* state = static_cast<DndWait*>(data);
      MainWindow* window = nullptr;
      for (auto* candidate : state->app->get_windows()) {
        window = dynamic_cast<MainWindow*>(candidate);
        if (window != nullptr) {
          break;
        }
      }
      if (window == nullptr) {
        return G_SOURCE_CONTINUE;
      }
      window->set_default_size(720, 480);
      if (!state->printed && window->get_mapped() && window->get_window()) {
        const unsigned long xid =
            gdk_x11_window_get_xid(window->get_window()->gobj());
        g_print("DND_EDIT xid=%lu\n", xid);
        fflush(stdout);
        state->printed = true;
      }
      if (window->file_path_ == state->path &&
          window->buffer()->get_char_count() > 0) {
        g_print("DND_OPENED\n");
        fflush(stdout);
        state->app->quit();
        return G_SOURCE_REMOVE;
      }
      return G_SOURCE_CONTINUE;
    }, wait);
    g_timeout_add(20000, +[](gpointer data) -> gboolean {
      auto* state = static_cast<DndWait*>(data);
      g_print("DND_TIMEOUT\n");
      fflush(stdout);
      state->app->quit();
      return G_SOURCE_REMOVE;
    }, wait);
    return app->run(argc, argv);
  }

  static std::string dnd_helper_path() {
    const auto slash = argv0.find_last_of('/');
    if (slash == std::string::npos) {
      return "edit-dnd-source";
    }
    return argv0.substr(0, slash + 1) + "edit-dnd-source";
  }

  static bool command_ok(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    return rc == 0;
  }

  static void test_xdnd(const std::string& dir) {
    std::cout << "round7 xdnd begin\n";
    if (access("/usr/bin/xdotool", X_OK) != 0 ||
        access("/usr/bin/xfwm4", X_OK) != 0) {
      expect(false, "XDND test needs xdotool and xfwm4 on PATH");
      return;
    }
    const std::string helper = dnd_helper_path();
    if (access(helper.c_str(), X_OK) != 0) {
      expect(false, "XDND drag source was built next to the test binary");
      return;
    }
    const std::string path = dir + "/xdnd-drop.txt";
    write_bytes(path, "dropped file content\n");
    // pgrep would see an xfwm4 on some other DISPLAY and skip this one.
    // Start a manager here and wait until this screen advertises it.
    pid_t wm_pid = -1;
    if (std::system("xprop -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1") !=
        0) {
      const pid_t wm = fork();
      if (wm == 0) {
        setpgid(0, 0);
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
          dup2(fd, STDOUT_FILENO);
          dup2(fd, STDERR_FILENO);
          if (fd > 2) {
            close(fd);
          }
        }
        execl("/usr/bin/xfwm4", "xfwm4", "--replace", static_cast<char*>(nullptr));
        _exit(127);
      }
      if (wm > 0) {
        setpgid(wm, wm);
        remember_child(wm);
        wm_pid = wm;
      }
      for (int i = 0; i < 80; ++i) {
        if (std::system(
                "xprop -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1") == 0) {
          break;
        }
        g_usleep(50 * 1000);
      }
    }
    expect(std::system(
               "xprop -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1") == 0,
           "a window manager is running on this display for XDND");

    bool opened = false;
    for (int attempt = 0; attempt < 3 && !opened; ++attempt) {
      const ToolRun editor = [&]() {
        // The editor is started below together with the source; this
        // placeholder keeps the attempt loop readable.
        return ToolRun{};
      }();
      (void)editor;
      int edit_fds[2];
      int src_fds[2];
      if (pipe(edit_fds) != 0 || pipe(src_fds) != 0) {
        expect(false, "XDND pipes");
        if (wm_pid > 0) {
          kill_child_tree(wm_pid);
        }
        return;
      }
      const pid_t edit_pid = fork();
      if (edit_pid == 0) {
        setpgid(0, 0);
        dup2(edit_fds[1], STDOUT_FILENO);
        dup2(edit_fds[1], STDERR_FILENO);
        close(edit_fds[0]);
        close(edit_fds[1]);
        close(src_fds[0]);
        close(src_fds[1]);
        unset_inherited_test_env();
        setenv("LUNDUKE_EDIT_TEST_DND_CHILD", "1", 1);
        setenv("LUNDUKE_EDIT_TEST_DND_PATH", path.c_str(), 1);
        setenv("LUNDUKE_EDIT_TEST", "1", 1);
        // xdotool injects core XTest events. GTK3 listens to XInput2
        // unless this is set, and those drags never reach the dest.
        setenv("GDK_CORE_DEVICE_EVENTS", "1", 1);
        execl(argv0.c_str(), argv0.c_str(), static_cast<char*>(nullptr));
        _exit(127);
      }
      if (edit_pid > 0) {
        setpgid(edit_pid, edit_pid);
        remember_child(edit_pid);
      }
      const pid_t src_pid = fork();
      if (src_pid == 0) {
        setpgid(0, 0);
        dup2(src_fds[1], STDOUT_FILENO);
        dup2(src_fds[1], STDERR_FILENO);
        close(src_fds[0]);
        close(src_fds[1]);
        close(edit_fds[0]);
        close(edit_fds[1]);
        setenv("DND_SOURCE_PATH", path.c_str(), 1);
        setenv("GDK_CORE_DEVICE_EVENTS", "1", 1);
        setenv("GDK_BACKEND", "x11", 1);
        execl(helper.c_str(), helper.c_str(), static_cast<char*>(nullptr));
        _exit(127);
      }
      if (src_pid > 0) {
        setpgid(src_pid, src_pid);
        remember_child(src_pid);
      }
      close(edit_fds[1]);
      close(src_fds[1]);
      unsigned long edit_xid = 0;
      unsigned long src_xid = 0;
      std::string edit_pending;
      std::string src_pending;
      std::string edit_log;
      std::string src_log;
      bool edit_done = false;
      const gint64 start = g_get_monotonic_time();
      bool dragged = false;
      while (!edit_done && (g_get_monotonic_time() - start) / 1000 < 20000) {
        pollfd pfds[2];
        pfds[0].fd = edit_fds[0];
        pfds[0].events = POLLIN;
        pfds[1].fd = src_fds[0];
        pfds[1].events = POLLIN;
        poll(pfds, 2, 100);
        auto slurp = [](int fd, std::string& pending) {
          char buf[512];
          const ssize_t n = read(fd, buf, sizeof buf);
          if (n > 0) {
            pending.append(buf, static_cast<std::size_t>(n));
          }
        };
        if (pfds[0].revents & (POLLIN | POLLHUP)) {
          slurp(edit_fds[0], edit_pending);
        }
        if (pfds[1].revents & (POLLIN | POLLHUP)) {
          slurp(src_fds[0], src_pending);
        }
        auto take_lines = [](std::string& pending, std::string& log,
                             const char* key, unsigned long& xid) {
          bool opened_now = false;
          std::size_t nl = 0;
          while ((nl = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            log += line;
            log += '\n';
            if (line.compare(0, std::strlen(key), key) == 0) {
              std::sscanf(line.c_str() + std::strlen(key), "%lu", &xid);
            }
            if (line == "DND_OPENED") {
              opened_now = true;
            }
          }
          return opened_now;
        };
        if (take_lines(edit_pending, edit_log, "DND_EDIT xid=", edit_xid)) {
          opened = true;
          break;
        }
        take_lines(src_pending, src_log, "DND_READY xid=", src_xid);
        if (!dragged && edit_xid != 0 && src_xid != 0) {
          dragged = true;
          command_ok("xdotool windowmove " + std::to_string(src_xid) + " 40 60");
          command_ok("xdotool windowmove " + std::to_string(edit_xid) +
                     " 420 60");
          command_ok("xdotool windowraise " + std::to_string(edit_xid));
          command_ok("xdotool windowraise " + std::to_string(src_xid));
          g_usleep(200 * 1000);
          command_ok("xdotool mousemove --window " + std::to_string(src_xid) +
                     " 80 50");
          g_usleep(100 * 1000);
          command_ok("xdotool mousedown 1");
          g_usleep(50 * 1000);
          for (int step = 0; step < 8; ++step) {
            command_ok("xdotool mousemove_relative --sync 6 0");
            g_usleep(30 * 1000);
          }
          for (int step = 0; step < 25; ++step) {
            command_ok("xdotool mousemove_relative --sync 16 4");
            g_usleep(30 * 1000);
          }
          command_ok("xdotool mousemove --window " + std::to_string(edit_xid) +
                     " --sync 200 180");
          g_usleep(300 * 1000);
          command_ok("xdotool mouseup 1");
          g_usleep(500 * 1000);
        }
      }
      kill_child_tree(edit_pid);
      kill_child_tree(src_pid);
      close(edit_fds[0]);
      close(src_fds[0]);
      if (!opened) {
        std::cerr << "xdnd attempt " << attempt << " edit=[" << edit_log
                  << "] src=[" << src_log << "]\n";
      }
    }
    expect(opened, "a real XDND text/uri-list drop opens the file");
    if (wm_pid > 0) {
      kill_child_tree(wm_pid);
    }
    std::cout << "round7 xdnd end\n";
  }

  struct ProdProbe {
    Display* dpy{nullptr};
    Atom wm_protocols{0};
    Atom net_ping{0};
    Atom net_wm_name{0};
    Atom utf8{0};
    Window root{0};
    long seq{1};
    XErrorHandler previous_handler{nullptr};

    bool open() {
      // xvfb-run's display is not always ready on the first try. Use the
      // DISPLAY and XAUTHORITY it exported, and retry instead of failing
      // the leg.
      const char* name = std::getenv("DISPLAY");
      for (int attempt = 0; attempt < 40; ++attempt) {
        dpy = XOpenDisplay(name);
        if (dpy != nullptr) {
          break;
        }
        poll(nullptr, 0, attempt < 10 ? 25 : 50);
      }
      if (dpy == nullptr) {
        std::cerr << "XOpenDisplay failed DISPLAY="
                  << (name != nullptr ? name : "(null)") << " XAUTHORITY="
                  << (std::getenv("XAUTHORITY") != nullptr
                          ? std::getenv("XAUTHORITY")
                          : "(null)")
                  << "\n";
        return false;
      }
      root = DefaultRootWindow(dpy);
      wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
      net_ping = XInternAtom(dpy, "_NET_WM_PING", False);
      net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
      utf8 = XInternAtom(dpy, "UTF8_STRING", False);
      XSelectInput(dpy, root, SubstructureNotifyMask | StructureNotifyMask);
      previous_handler =
          XSetErrorHandler(+[](Display*, XErrorEvent*) { return 0; });
      return true;
    }

    void close() {
      if (previous_handler != nullptr) {
        XSetErrorHandler(previous_handler);
        previous_handler = nullptr;
      }
      if (dpy != nullptr) {
        XCloseDisplay(dpy);
        dpy = nullptr;
      }
    }

    std::string title_of(Window window) const {
      if (dpy == nullptr || window == 0) {
        return {};
      }
      Atom actual = 0;
      int format = 0;
      unsigned long n = 0;
      unsigned long after = 0;
      unsigned char* data = nullptr;
      if (XGetWindowProperty(dpy, window, net_wm_name, 0, 1024, False, utf8,
                             &actual, &format, &n, &after, &data) == Success &&
          data != nullptr) {
        std::string text(reinterpret_cast<char*>(data), n);
        XFree(data);
        return text;
      }
      if (data != nullptr) {
        XFree(data);
      }
      char* name = nullptr;
      if (XFetchName(dpy, window, &name) && name != nullptr) {
        std::string text(name);
        XFree(name);
        return text;
      }
      return {};
    }

    Window find_editor() const {
      if (dpy == nullptr) {
        return 0;
      }
      Window root_ret = 0;
      Window parent = 0;
      Window* children = nullptr;
      unsigned count = 0;
      if (!XQueryTree(dpy, root, &root_ret, &parent, &children, &count)) {
        return 0;
      }
      Window best = 0;
      int best_area = 0;
      for (unsigned i = 0; i < count; ++i) {
        XClassHint hint {};
        if (XGetClassHint(dpy, children[i], &hint) == 0) {
          continue;
        }
        const bool match =
            (hint.res_name != nullptr &&
             std::strstr(hint.res_name, "lunduke-edit") != nullptr) ||
            (hint.res_class != nullptr &&
             std::strstr(hint.res_class, "lunduke") != nullptr);
        if (hint.res_name != nullptr) {
          XFree(hint.res_name);
        }
        if (hint.res_class != nullptr) {
          XFree(hint.res_class);
        }
        if (!match) {
          continue;
        }
        XWindowAttributes attr {};
        if (XGetWindowAttributes(dpy, children[i], &attr) == 0 ||
            attr.map_state != IsViewable || attr.width < 200 ||
            attr.height < 150) {
          continue;
        }
        const int area = attr.width * attr.height;
        if (area > best_area) {
          best = children[i];
          best_area = area;
        }
      }
      if (children != nullptr) {
        XFree(children);
      }
      return best;
    }

    Window transient_owner(Window window) const {
      if (dpy == nullptr || window == 0) {
        return 0;
      }
      const Atom atom = XInternAtom(dpy, "WM_TRANSIENT_FOR", True);
      if (atom == None) {
        return 0;
      }
      Atom actual = 0;
      int format = 0;
      unsigned long n = 0;
      unsigned long after = 0;
      unsigned char* data = nullptr;
      Window owner = 0;
      if (XGetWindowProperty(dpy, window, atom, 0, 1, False, AnyPropertyType,
                             &actual, &format, &n, &after, &data) == Success &&
          data != nullptr && n >= 1 && format == 32) {
        owner = *reinterpret_cast<Window*>(data);
      }
      if (data != nullptr) {
        XFree(data);
      }
      return owner;
    }

    // Dialogs are root children when nothing is reparenting them, and
    // children of a frame when a window manager is. Match the title on
    // either, and also a window that is transient for the editor.
    Window find_title(const char* needle) const {
      if (dpy == nullptr || needle == nullptr) {
        return 0;
      }
      Window found = 0;
      const auto visit = [&](auto&& self, Window window, int depth) -> void {
        if (found != 0 || window == 0 || depth > 4) {
          return;
        }
        const std::string text = title_of(window);
        XWindowAttributes attr {};
        const bool viewable = XGetWindowAttributes(dpy, window, &attr) != 0 &&
                              attr.map_state == IsViewable;
        if (viewable && text.find(needle) != std::string::npos) {
          found = window;
          return;
        }
        Window root_ret = 0;
        Window parent = 0;
        Window* children = nullptr;
        unsigned count = 0;
        if (!XQueryTree(dpy, window, &root_ret, &parent, &children, &count)) {
          return;
        }
        for (unsigned i = 0; i < count && found == 0; ++i) {
          self(self, children[i], depth + 1);
        }
        if (children != nullptr) {
          XFree(children);
        }
      };
      visit(visit, root, 0);
      return found;
    }

    // Title match first. If a window manager ate the name, a viewable
    // window that is transient for the editor still counts.
    Window find_related(Window editor, const char* needle) const {
      if (Window named = find_title(needle)) {
        return named;
      }
      if (dpy == nullptr || editor == 0 || needle == nullptr) {
        return 0;
      }
      Window found = 0;
      const auto visit = [&](auto&& self, Window window, int depth) -> void {
        if (found != 0 || window == 0 || depth > 4) {
          return;
        }
        XWindowAttributes attr {};
        const bool viewable = XGetWindowAttributes(dpy, window, &attr) != 0 &&
                              attr.map_state == IsViewable;
        const Window owner = transient_owner(window);
        if (viewable && owner == editor) {
          const std::string text = title_of(window);
          if (text.find(needle) != std::string::npos) {
            found = window;
            return;
          }
        }
        Window root_ret = 0;
        Window parent = 0;
        Window* children = nullptr;
        unsigned count = 0;
        if (!XQueryTree(dpy, window, &root_ret, &parent, &children, &count)) {
          return;
        }
        for (unsigned i = 0; i < count && found == 0; ++i) {
          self(self, children[i], depth + 1);
        }
        if (children != nullptr) {
          XFree(children);
        }
      };
      visit(visit, root, 0);
      return found;
    }

    // Round-trip through the editor's main loop. The reply is the
    // _NET_WM_PING the toolkit echoes to the root window.
    double ping(Window window, int timeout_ms, pid_t pid = -1,
                const std::function<void()>& tick = {}) {
      if (dpy == nullptr || window == 0) {
        return static_cast<double>(timeout_ms);
      }
      XEvent ev {};
      ev.xclient.type = ClientMessage;
      ev.xclient.window = window;
      ev.xclient.message_type = wm_protocols;
      ev.xclient.format = 32;
      const long token = ++seq;
      ev.xclient.data.l[0] = static_cast<long>(net_ping);
      ev.xclient.data.l[1] = token;
      ev.xclient.data.l[2] = static_cast<long>(window);
      const gint64 t0 = g_get_monotonic_time();
      XSendEvent(dpy, window, False, NoEventMask, &ev);
      XFlush(dpy);
      while (true) {
        if (tick) {
          tick();
        }
        if (pid > 0) {
          int status = 0;
          if (waitpid(pid, &status, WNOHANG) == pid) {
            return -2;
          }
        }
        XWindowAttributes attr {};
        if (XGetWindowAttributes(dpy, window, &attr) == 0) {
          return -2;
        }
        const double elapsed =
            static_cast<double>(g_get_monotonic_time() - t0) / 1000.0;
        if (elapsed >= timeout_ms) {
          return elapsed;
        }
        while (XPending(dpy) != 0) {
          XEvent got {};
          XNextEvent(dpy, &got);
          if (got.type == ClientMessage &&
              got.xclient.message_type == wm_protocols &&
              static_cast<Atom>(got.xclient.data.l[0]) == net_ping &&
              got.xclient.data.l[1] == token) {
            return static_cast<double>(g_get_monotonic_time() - t0) / 1000.0;
          }
        }
        pollfd pfd {};
        pfd.fd = ConnectionNumber(dpy);
        pfd.events = POLLIN;
        const int remain = timeout_ms - static_cast<int>(elapsed);
        // Short waits so a cancel key can go out on time while this
        // round trip is still measuring a longer stall.
        const int slice = remain > 50 ? 50 : remain;
        poll(&pfd, 1, slice > 0 ? slice : 0);
      }
    }
  };

  // A key command that dies after the press leaves that key down. The
  // next window then sees Escape autorepeat and treats it as Cancel.
  static void release_stuck_keys() {
    std::system(
        "/usr/bin/xdotool keyup Escape Return alt ctrl shift super "
        ">/dev/null 2>&1");
  }

  static bool xdotool_cmd(const std::string& args) {
    const std::string cmd =
        "/usr/bin/xdotool " + args + " >/tmp/xdotool-prod.log 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
      std::cerr << "xdotool rc=" << rc << " args=[" << args << "]\n";
    }
    return rc == 0;
  }

  static std::string production_binary_path() {
    const auto slash = argv0.find_last_of('/');
    const std::string dir =
        (slash == std::string::npos) ? std::string(".") : argv0.substr(0, slash);
    return dir + "/lunduke-edit";
  }

  static pid_t spawn_production(const std::string& bin, const std::string& path,
                                bool throttle) {
    const pid_t pid = fork();
    if (pid < 0) {
      return -1;
    }
    if (pid == 0) {
      setpgid(0, 0);
      unset_inherited_test_env();
      unsetenv("LUNDUKE_EDIT_TEST_ARGV_CHILD");
      unsetenv("LUNDUKE_EDIT_TEST_REPLACE_CHILD");
      unsetenv("LUNDUKE_EDIT_TEST_DND_CHILD");
      setenv("GDK_BACKEND", "x11", 1);
      const bool have_taskset = access("/usr/bin/taskset", X_OK) == 0;
      if (throttle && have_taskset) {
        execl("/usr/bin/taskset", "taskset", "-c", "0", "/usr/bin/cpulimit",
              "-f", "-q", "-c", "1", "-l", "25", "--", bin.c_str(), path.c_str(),
              static_cast<char*>(nullptr));
      } else if (throttle) {
        execl("/usr/bin/cpulimit", "cpulimit", "-f", "-q", "-c", "1", "-l", "25",
              "--", bin.c_str(), path.c_str(), static_cast<char*>(nullptr));
      } else {
        execl(bin.c_str(), bin.c_str(), path.c_str(), static_cast<char*>(nullptr));
      }
      _exit(127);
    }
    setpgid(pid, pid);
    remember_child(pid);
    return pid;
  }

  static void stop_production(pid_t pid) {
    kill_child_tree(pid);
  }

  static void wait_for_editor_gone(ProdProbe& probe) {
    for (int i = 0; i < 80; ++i) {
      if (probe.find_editor() == 0) {
        poll(nullptr, 0, 80);
        if (probe.find_editor() == 0) {
          return;
        }
      }
      poll(nullptr, 0, 25);
    }
  }

  struct ProdStats {
    double map_ms{-1};
    double done_ms{-1};
    double max_gap_ms{0};
    double replace_gap_ms{0};
    double escape_at_ms{-1};
    double honor_ms{-1};
    double gap_at_ms{-1};
    bool gap_after_undo{false};
    bool gap_after_keys{false};
    bool honored{false};
    bool mapped{false};
    bool titled{false};
    bool dirty{false};
    bool escaped{false};
    bool undo_ok{false};
    bool apply_seen{false};
    int pings{0};
    int max_height{0};
  };

  enum class ProdGeom { Default, Tall, Maximize, ResizeLoad, ResizeApply };

  static void shape_window(ProdProbe& probe, Window editor, int width,
                           int height, bool maximize) {
    // xvfb-run has no window manager, and xdotool's resize is an EWMH
    // request that nothing handles there. Move and size the client
    // directly so a tall or maximized leg is actually that size.
    if (probe.dpy != nullptr) {
      if (maximize) {
        width = XDisplayWidth(probe.dpy, DefaultScreen(probe.dpy));
        height = XDisplayHeight(probe.dpy, DefaultScreen(probe.dpy));
      }
      XMoveResizeWindow(probe.dpy, editor, 0, 0,
                        static_cast<unsigned>(width),
                        static_cast<unsigned>(height));
      XFlush(probe.dpy);
    }
    const std::string id = std::to_string(static_cast<unsigned long>(editor));
    xdotool_cmd("windowmove " + id + " 0 0");
    xdotool_cmd("windowsize " + id + " " + std::to_string(width) + " " +
                std::to_string(height));
    if (!maximize || probe.dpy == nullptr) {
      // Set the size again after xdotool. Without a window manager the
      // EWMH request does not change the window, and it must stay at
      // the size just applied.
      if (probe.dpy != nullptr) {
        XMoveResizeWindow(probe.dpy, editor, 0, 0,
                          static_cast<unsigned>(width),
                          static_cast<unsigned>(height));
        XFlush(probe.dpy);
      }
      return;
    }
    Display* dpy = probe.dpy;
    Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom vert = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    Atom horz = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    XEvent ev {};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = editor;
    ev.xclient.message_type = state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 1;
    ev.xclient.data.l[1] = static_cast<long>(vert);
    ev.xclient.data.l[2] = static_cast<long>(horz);
    ev.xclient.data.l[3] = 1;
    // Delivered to the window manager. This connection does not select
    // SubstructureRedirectMask; that mask belongs to xfwm4.
    XSendEvent(dpy, probe.root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
  }

  // Percent from a title that mirrors "Replacing… N%". -1 when the
  // apply status is not in the title.
  static int replacing_percent(const std::string& title) {
    const auto pos = title.find("Replacing");
    if (pos == std::string::npos) {
      return -1;
    }
    const auto pct = title.find('%', pos);
    if (pct == std::string::npos) {
      return -1;
    }
    int value = -1;
    for (std::size_t i = pos; i < pct; ++i) {
      if (title[i] >= '0' && title[i] <= '9') {
        value = 0;
        while (i < pct && title[i] >= '0' && title[i] <= '9') {
          value = value * 10 + (title[i] - '0');
          ++i;
        }
      }
    }
    return value;
  }

  // Focus the window, then deliver the key to that id. windowactivate
  // --sync waits for a window manager; with none it returns at once.
  // key itself has no --sync. XSetInputFocus runs first so key --window
  // uses XTest instead of a ClientMessage GTK drops.
  static void send_key(ProdProbe& probe, Window window, const char* key) {
    if (probe.dpy == nullptr || window == 0 || key == nullptr) {
      return;
    }
    XRaiseWindow(probe.dpy, window);
    XSetInputFocus(probe.dpy, window, RevertToParent, CurrentTime);
    XFlush(probe.dpy);
    const std::string id = std::to_string(static_cast<unsigned long>(window));
    xdotool_cmd("windowactivate --sync " + id);
    xdotool_cmd("key --window " + id + " --clearmodifiers " + key);
    release_stuck_keys();
  }

  // Drive the shipped editor from outside the process. Gaps are
  // _NET_WM_PING round trips after the real window maps.
  static ProdStats watch_production(ProdProbe& probe, pid_t pid,
                                    const std::string& base, bool replace,
                                    bool cancel, double gap_limit_ms,
                                    int deadline_ms, double escape_after_ms,
                                    ProdGeom geom, const char* needle,
                                    const char* replacement) {
    ProdStats stats;
    const gint64 start = g_get_monotonic_time();
    Window editor = 0;
    gint64 mapped_at = 0;
    bool keys_sent = false;
    bool replace_armed = false;
    gint64 apply_seen_at = 0;
    bool undo_sent = false;
    int undo_tries = 0;
    gint64 last_undo_key = 0;
    int escape_sends = 0;
    bool shaped = false;
    bool shaped_late = false;
    gint64 last_resize = 0;
    int max_height = 0;
    // Measure past the pass/fail line so a timeout is not mistaken for
    // the real stall. The expect below still uses gap_limit_ms.
    const int ping_timeout = std::max(4000, static_cast<int>(gap_limit_ms) + 150);
    release_stuck_keys();
    auto elapsed_ms = [&]() {
      return static_cast<double>(g_get_monotonic_time() - start) / 1000.0;
    };
    auto since_map = [&]() {
      return static_cast<double>(g_get_monotonic_time() - mapped_at) / 1000.0;
    };

    while (elapsed_ms() < deadline_ms) {
      int status = 0;
      if (waitpid(pid, &status, WNOHANG) == pid) {
        // A command-line open that was cancelled destroys its window and
        // exits. That can beat the next ping, so treat a still-untitled
        // exit as cancel success instead of a lost Escape.
        if (stats.escaped && !stats.titled) {
          if (stats.honor_ms < 0 && stats.escape_at_ms >= 0) {
            stats.honor_ms = since_map() - stats.escape_at_ms;
          }
          if (stats.honor_ms >= 0 && stats.honor_ms <= 300.0) {
            stats.honored = true;
          }
          if (stats.done_ms < 0) {
            stats.done_ms = elapsed_ms();
          }
        }
        break;
      }
      if (editor == 0) {
        editor = probe.find_editor();
        if (editor != 0) {
          stats.mapped = true;
          mapped_at = g_get_monotonic_time();
          stats.map_ms = elapsed_ms();
        } else {
          poll(nullptr, 0, 20);
          continue;
        }
      }
      if (editor != 0 && probe.dpy != nullptr) {
        Window geom_root = 0;
        int gx = 0;
        int gy = 0;
        unsigned gw = 0;
        unsigned gh = 0;
        unsigned gb = 0;
        unsigned gd = 0;
        if (XGetGeometry(probe.dpy, editor, &geom_root, &gx, &gy, &gw, &gh,
                         &gb, &gd) != 0) {
          max_height = std::max(max_height, static_cast<int>(gh));
        }
      }
      if (editor != 0 && geom == ProdGeom::Tall && !shaped) {
        shape_window(probe, editor, 1280, 770, false);
        shaped = true;
      } else if (editor != 0 && geom == ProdGeom::Maximize && !shaped) {
        shape_window(probe, editor, 1280, 770, true);
        shaped = true;
      } else if (editor != 0 && geom == ProdGeom::ResizeLoad && !replace) {
        if (!shaped && since_map() >= 40.0) {
          shape_window(probe, editor, 1000, 620, false);
          shaped = true;
        } else if (shaped && !shaped_late && since_map() >= 200.0) {
          shape_window(probe, editor, 1280, 770, true);
          shaped_late = true;
        }
      } else if (editor != 0 && geom == ProdGeom::ResizeApply) {
        if (!shaped) {
          shape_window(probe, editor, 1200, 500, false);
          shaped = true;
          last_resize = g_get_monotonic_time();
        } else if (keys_sent &&
                   g_get_monotonic_time() - last_resize > 250000) {
          if ((shaped_late = !shaped_late)) {
            shape_window(probe, editor, 1280, 770, true);
          } else {
            shape_window(probe, editor, 1200, 500, false);
          }
          last_resize = g_get_monotonic_time();
        }
      }
      // Escape is sent from inside the ping wait. A full-timeout poll would
      // otherwise run past the cancel point, and one key during the commit
      // slice is too late. Repeats every 200 ms cover about 1 s while the
      // title is still Untitled.
      const double rtt = probe.ping(editor, ping_timeout, pid, [&]() {
        if (replace || !cancel || escape_sends >= 8 ||
            since_map() < escape_after_ms) {
          return;
        }
        const bool due =
            !stats.escaped ||
            since_map() >= stats.escape_at_ms + escape_sends * 200.0;
        if (!due) {
          return;
        }
        const std::string early = probe.title_of(editor);
        if (early.find(base) != std::string::npos) {
          return;
        }
        if (probe.dpy != nullptr) {
          XRaiseWindow(probe.dpy, editor);
          XSetInputFocus(probe.dpy, editor, RevertToParent, CurrentTime);
          XFlush(probe.dpy);
        }
        xdotool_cmd("key --window " + std::to_string(static_cast<unsigned long>(editor)) +
                    " --clearmodifiers Escape");
        release_stuck_keys();
        if (!stats.escaped) {
          stats.escape_at_ms = since_map();
        }
        stats.escaped = true;
        ++escape_sends;
      });
      if (rtt >= 0 && rtt < 30.0) {
        // A tight ping loop otherwise spins the CPU and crowds the editor.
        poll(nullptr, 0, 15);
      }
      if (rtt < 0) {
        // The process exited mid-ping. A command-line open that was
        // cancelled closes its window; that is not a stalled main loop.
        if (stats.escaped && !stats.titled) {
          if (stats.honor_ms < 0 && stats.escape_at_ms >= 0) {
            stats.honor_ms = since_map() - stats.escape_at_ms;
          }
          if (stats.honor_ms >= 0 && stats.honor_ms <= 300.0) {
            stats.honored = true;
          }
          stats.done_ms = elapsed_ms();
          break;
        }
        break;
      }
      ++stats.pings;
      if (rtt > stats.max_gap_ms) {
        stats.max_gap_ms = rtt;
        stats.gap_at_ms = elapsed_ms();
        stats.gap_after_undo = undo_sent;
        stats.gap_after_keys = keys_sent;
      }
      if (keys_sent && rtt > stats.replace_gap_ms) {
        stats.replace_gap_ms = rtt;
      }
      const std::string title = probe.title_of(editor);
      const bool has_file = title.find(base) != std::string::npos;
      const bool has_star = title.find(" *") != std::string::npos;
      if (has_file) {
        stats.titled = true;
      }
      // Cancel legs judge the title after the apply and its restore have
      // finished. A star seen while "Replacing" or "Restoring" is up is
      // not the final buffer.
      if (has_star && !(replace && cancel)) {
        stats.dirty = true;
      }
      if (stats.escaped && !has_file && stats.honor_ms < 0 &&
          stats.escape_at_ms >= 0) {
        stats.honor_ms = since_map() - stats.escape_at_ms;
      }
      if (stats.escaped && !has_file && stats.escape_at_ms >= 0 &&
          since_map() >= stats.escape_at_ms + 300.0) {
        stats.honored = true;
      }

      if (!replace) {
        if (cancel) {
          if (stats.escaped && since_map() > stats.escape_at_ms + 4000.0) {
            stats.done_ms = elapsed_ms();
            break;
          }
          if (!stats.escaped && has_file) {
            stats.done_ms = elapsed_ms();
            break;
          }
        } else if (has_file) {
          stats.done_ms = elapsed_ms();
          // One more ping after the title flips, so a post-load draw is
          // included in the gap.
          const double after = probe.ping(editor, ping_timeout, pid);
          if (after >= 0) {
            ++stats.pings;
            if (after > stats.max_gap_ms) {
              stats.max_gap_ms = after;
            }
          }
          break;
        }
        continue;
      }

      if (!keys_sent) {
        if (!has_file) {
          continue;
        }
        send_key(probe, editor, "ctrl+f");
        poll(nullptr, 0, 250);
        Window find_dlg = probe.find_related(editor, "Find");
        if (find_dlg == 0) {
          // Do not type into the document. Retry until Find & Replace exists.
          continue;
        }
        const std::string find_text = needle != nullptr ? needle : "line ";
        const std::string repl_text =
            replacement != nullptr ? replacement : "row ";
        send_key(probe, find_dlg, "ctrl+a");
        xdotool_cmd("type --delay 5 --clearmodifiers '" + find_text + "'");
        send_key(probe, find_dlg, "alt+w");
        poll(nullptr, 0, 80);
        xdotool_cmd("type --delay 5 --clearmodifiers '" + repl_text + "'");
        send_key(probe, find_dlg, "alt+l");
        keys_sent = true;
        replace_armed = true;
        continue;
      }

      // The worst-case undo warning is shown before counting. Accept it
      // by name or by transient-for, or Replace All never starts.
      if (Window undo_dlg = probe.find_related(editor, "very large undo")) {
        send_key(probe, undo_dlg, "alt+r");
        poll(nullptr, 0, 40);
        continue;
      }
      if (cancel) {
        const int pct = replacing_percent(title);
        if (!stats.apply_seen && pct >= 96) {
          stats.apply_seen = true;
          apply_seen_at = g_get_monotonic_time();
        }
        const double since_apply =
            stats.apply_seen
                ? static_cast<double>(g_get_monotonic_time() - apply_seen_at) /
                      1000.0
                : 0.0;
        const bool in_apply = pct >= 96 && pct < 100;
        if (stats.apply_seen && in_apply && since_apply >= escape_after_ms &&
            escape_sends < 8) {
          const bool due = !stats.escaped ||
                           since_apply >= stats.escape_at_ms + escape_sends * 200.0;
          if (due) {
            Window target = probe.find_related(editor, "Find");
            if (target == 0) {
              target = editor;
            }
            send_key(probe, target, "Escape");
            if (!stats.escaped) {
              stats.escaped = true;
              stats.escape_at_ms = since_apply;
            }
            ++escape_sends;
          }
        }
        const bool replacing = title.find("Replacing") != std::string::npos;
        const bool restoring = title.find("Restoring") != std::string::npos;
        // The title read above is from before this iteration's key. Once
        // Escape has been delivered, wait until both progress strings are
        // gone and then take the star from that settled title.
        if (stats.escaped && escape_sends > 0 && !replacing && !restoring &&
            since_apply > stats.escape_at_ms + 150.0) {
          stats.dirty = has_star;
          stats.done_ms = elapsed_ms();
          break;
        }
        if (!stats.escaped && has_star && !replacing && !restoring) {
          stats.dirty = true;
          stats.done_ms = elapsed_ms();
          break;
        }
        continue;
      }

      if (has_star && !undo_sent) {
        // The result dialog and Find & Replace are still mapped. A bare
        // Ctrl+Z goes to whichever of those has the keyboard, and the
        // document never sees it. Dismiss them, then undo in the editor.
        // "very large undo" was already handled above; this "Replace All"
        // title is the result dialog.
        if (Window done = probe.find_related(editor, "Replace All")) {
          send_key(probe, done, "Return");
          poll(nullptr, 0, 40);
          continue;
        }
        if (Window find = probe.find_related(editor, "Find")) {
          send_key(probe, find, "Escape");
          poll(nullptr, 0, 40);
          continue;
        }
        const std::string id = std::to_string(static_cast<unsigned long>(editor));
        xdotool_cmd("mousemove --window " + id + " 280 200 click 1");
        send_key(probe, editor, "ctrl+z");
        undo_sent = true;
        last_undo_key = g_get_monotonic_time();
        ++undo_tries;
        continue;
      }
      // The first key can land before the text view has focus. A few
      // retries cover that without repeating for the whole undo.
      if (has_star && undo_sent && undo_tries < 6 &&
          g_get_monotonic_time() - last_undo_key > 400000) {
        send_key(probe, editor, "ctrl+z");
        last_undo_key = g_get_monotonic_time();
        ++undo_tries;
      }
      if (undo_sent && !has_star && has_file) {
        stats.undo_ok = true;
        stats.done_ms = elapsed_ms();
        break;
      }
      (void)replace_armed;
    }
    stats.max_height = max_height;
    return stats;
  }

  static void write_repeated(const std::string& path, const std::string& piece,
                             std::size_t bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::size_t wrote = 0;
    while (wrote < bytes) {
      const std::size_t n = std::min(piece.size(), bytes - wrote);
      out.write(piece.data(), static_cast<std::streamsize>(n));
      wrote += n;
    }
  }

  static void test_production_responsiveness(const std::string& dir) {
    std::cout << "production responsiveness begin\n";
    const char* display = g_getenv("DISPLAY");
    if (display == nullptr || display[0] == '\0') {
      expect(false, "production stall test has a display");
      return;
    }
    if (access("/usr/bin/xdotool", X_OK) != 0) {
      expect(false, "production stall test requires /usr/bin/xdotool");
      return;
    }
    const bool have_cpulimit = access("/usr/bin/cpulimit", X_OK) == 0;
    const std::string bin = production_binary_path();
    if (access(bin.c_str(), X_OK) != 0) {
      expect(false, "production binary lunduke-edit is beside the test");
      std::cerr << "missing " << bin << "\n";
      return;
    }
    const std::string open_path = dir + "/prod-open-12mb.txt";
    const std::string replace_path = dir + "/prod-replace-12mb.txt";
    const std::string short_path = dir + "/prod-short-12mb.txt";
    write_repeated(open_path, "foo bar baz\n", 12000000);
    write_repeated(replace_path,
                   "line dog sit quick ipsum fox value line\n", 12000000);
    write_repeated(short_path, "row\n", 12000000);
    expect(read_bytes(open_path).size() == 12000000,
           "production open fixture is 12000000 bytes");
    expect(read_bytes(replace_path).size() == 12000000,
           "production replace fixture is 12000000 bytes");
    expect(read_bytes(short_path).size() == 12000000,
           "production short-line fixture is 12000000 bytes");

    struct Leg {
      const char* name;
      bool throttle;
      bool replace;
      bool cancel;
      double gap_limit;
      int deadline_ms;
      double escape_after_ms;
      double escape_min_ms;
      ProdGeom geom;
      const char* path;
      const char* needle;
      const char* replacement;
    };
    // Unthrottled gap is 200 ms. cpulimit -l 25, pinned to one CPU, is
    // 1000 ms. Escape during an unthrottled open must take effect within
    // 300 ms, including a press during the last part of a short-line load.
    const Leg legs[] = {
        {"open-work-default", false, false, false, 200.0, 60000, 0, 0,
         ProdGeom::Default, open_path.c_str(), nullptr, nullptr},
        {"open-work-tall", false, false, false, 200.0, 60000, 0, 0,
         ProdGeom::Tall, open_path.c_str(), nullptr, nullptr},
        {"open-work-max", false, false, false, 200.0, 60000, 0, 0,
         ProdGeom::Maximize, open_path.c_str(), nullptr, nullptr},
        {"open-work-resize", false, false, false, 200.0, 60000, 0, 0,
         ProdGeom::ResizeLoad, open_path.c_str(), nullptr, nullptr},
        {"open-short-default", false, false, false, 200.0, 120000, 0, 0,
         ProdGeom::Default, short_path.c_str(), nullptr, nullptr},
        {"open-short-tall", false, false, false, 200.0, 120000, 0, 0,
         ProdGeom::Tall, short_path.c_str(), nullptr, nullptr},
        {"open-short-resize", false, false, false, 200.0, 120000, 0, 0,
         ProdGeom::ResizeLoad, short_path.c_str(), nullptr, nullptr},
        {"open-escape-work", false, false, true, 200.0, 20000, 60.0, 40.0,
         ProdGeom::Default, open_path.c_str(), nullptr, nullptr},
        {"open-escape-short", false, false, true, 200.0, 30000, 400.0, 300.0,
         ProdGeom::Tall, short_path.c_str(), nullptr, nullptr},
        {"open-work-cpulimit", true, false, false, 1000.0, 180000, 0, 0,
         ProdGeom::Default, open_path.c_str(), nullptr, nullptr},
        {"open-work-tall-cpulimit", true, false, false, 1000.0, 180000, 0, 0,
         ProdGeom::Tall, open_path.c_str(), nullptr, nullptr},
        {"open-work-resize-cpulimit", true, false, false, 1000.0, 180000, 0, 0,
         ProdGeom::ResizeLoad, open_path.c_str(), nullptr, nullptr},
        {"open-short-cpulimit", true, false, false, 1000.0, 240000, 0, 0,
         ProdGeom::Default, short_path.c_str(), nullptr, nullptr},
        {"open-short-tall-cpulimit", true, false, false, 1000.0, 240000, 0, 0,
         ProdGeom::Tall, short_path.c_str(), nullptr, nullptr},
        {"open-escape-cpulimit", true, false, true, 1000.0, 90000, 400.0, 300.0,
         ProdGeom::Default, open_path.c_str(), nullptr, nullptr},
        {"open-escape-short-cpulimit", true, false, true, 1000.0, 120000, 600.0,
         400.0, ProdGeom::Default, short_path.c_str(), nullptr, nullptr},
        {"replace-work-default", false, true, false, 200.0, 180000, 0, 0,
         ProdGeom::Default, replace_path.c_str(), "line ", "row "},
        {"replace-work-tall", false, true, false, 200.0, 180000, 0, 0,
         ProdGeom::Tall, replace_path.c_str(), "line ", "row "},
        {"replace-work-max", false, true, false, 200.0, 180000, 0, 0,
         ProdGeom::Maximize, replace_path.c_str(), "line ", "row "},
        {"replace-work-resize", false, true, false, 200.0, 180000, 0, 0,
         ProdGeom::ResizeApply, replace_path.c_str(), "line ", "row "},
        {"replace-short-default", false, true, false, 200.0, 300000, 0, 0,
         ProdGeom::Default, short_path.c_str(), "row", "ROW"},
        {"replace-short-tall", false, true, false, 200.0, 300000, 0, 0,
         ProdGeom::Tall, short_path.c_str(), "row", "ROW"},
        {"replace-short-resize", false, true, false, 200.0, 300000, 0, 0,
         ProdGeom::ResizeApply, short_path.c_str(), "row", "ROW"},
        {"replace-escape-work", false, true, true, 200.0, 90000, 80.0, 40.0,
         ProdGeom::Default, replace_path.c_str(), "line ", "row "},
        {"replace-work-cpulimit", true, true, false, 1000.0, 300000, 0, 0,
         ProdGeom::Default, replace_path.c_str(), "line ", "row "},
        {"replace-work-tall-cpulimit", true, true, false, 1000.0, 300000, 0, 0,
         ProdGeom::Tall, replace_path.c_str(), "line ", "row "},
        {"replace-work-resize-cpulimit", true, true, false, 1000.0, 300000, 0, 0,
         ProdGeom::ResizeApply, replace_path.c_str(), "line ", "row "},
        {"replace-short-cpulimit", true, true, false, 1000.0, 420000, 0, 0,
         ProdGeom::Default, short_path.c_str(), "row", "ROW"},
        {"replace-short-tall-cpulimit", true, true, false, 1000.0, 420000, 0, 0,
         ProdGeom::Tall, short_path.c_str(), "row", "ROW"},
        {"replace-escape-cpulimit", true, true, true, 1000.0, 240000, 80.0,
         40.0, ProdGeom::Default, replace_path.c_str(), "line ", "row "},
        // Same bounds as replace-work-tall-cpulimit. CI runs it once.
        // LUNDUKE_EDIT_PROD_STRESS repeats it; the local gate uses 10.
        {"replace-work-tall-stress", true, true, false, 1000.0, 300000, 0, 0,
         ProdGeom::Tall, replace_path.c_str(), "line ", "row "},
    };

    const char* only = g_getenv("LUNDUKE_EDIT_PROD_FILTER");
    for (const Leg& leg : legs) {
      if (only != nullptr && std::strstr(leg.name, only) == nullptr) {
        continue;
      }
      int repeats = 1;
      if (std::strcmp(leg.name, "replace-work-tall-stress") == 0) {
        if (const char* n = g_getenv("LUNDUKE_EDIT_PROD_STRESS")) {
          repeats = std::atoi(n);
          if (repeats < 1) {
            repeats = 1;
          }
        }
      }
      for (int rep = 0; rep < repeats; ++rep) {
      if (leg.throttle && !have_cpulimit) {
        expect(false, "production stall test requires /usr/bin/cpulimit");
        std::cout << "production " << leg.name << " cpulimit missing\n";
        continue;
      }
      ProdProbe probe;
      expect(probe.open(), "production stall test opens its own X display");
      if (probe.dpy == nullptr) {
        return;
      }
      const std::string path = leg.path;
      const auto slash = path.find_last_of('/');
      const std::string base =
          (slash == std::string::npos) ? path : path.substr(slash + 1);
      const pid_t pid = spawn_production(bin, path, leg.throttle);
      expect(pid > 0, "production editor starts");
      if (pid <= 0) {
        probe.close();
        continue;
      }
      const ProdStats stats = watch_production(
          probe, pid, base, leg.replace, leg.cancel, leg.gap_limit,
          leg.deadline_ms, leg.escape_after_ms, leg.geom, leg.needle,
          leg.replacement);
      stop_production(pid);
      wait_for_editor_gone(probe);
      probe.close();
      std::cout << "production " << leg.name << " map_ms=" << stats.map_ms
                << " done_ms=" << stats.done_ms
                << " max_gap_ms=" << stats.max_gap_ms
                << " gap_at_ms=" << stats.gap_at_ms
                << " gap_after_keys=" << stats.gap_after_keys
                << " gap_after_undo=" << stats.gap_after_undo
                << " replace_gap_ms=" << stats.replace_gap_ms
                << " pings=" << stats.pings << " titled=" << stats.titled
                << " dirty=" << stats.dirty << " escaped=" << stats.escaped
                << " escape_at_ms=" << stats.escape_at_ms
                << " honor_ms=" << stats.honor_ms
                << " honored=" << stats.honored
                << " undo_ok=" << stats.undo_ok
                << " apply_seen=" << stats.apply_seen
                << " height=" << stats.max_height << "\n";
      if (leg.geom == ProdGeom::Tall || leg.geom == ProdGeom::Maximize ||
          leg.geom == ProdGeom::ResizeLoad || leg.geom == ProdGeom::ResizeApply) {
        expect(stats.max_height >= 700,
               "production window actually reaches a tall size");
      }
      expect(stats.mapped && stats.map_ms >= 0 &&
                 stats.map_ms < (leg.throttle ? 15000.0 : 5000.0),
             "production window maps");
      // A cancel that lands before the first ping reply still kept the
      // main loop alive. Zero pings is a pass only for that honored exit.
      // The gap bound is unchanged.
      const bool honored_without_pings =
          leg.cancel && stats.honored && stats.pings == 0;
      expect((stats.pings > 0 || honored_without_pings) &&
                 stats.max_gap_ms <= leg.gap_limit,
             "production main loop stays inside the stall bound");
      if (!leg.replace && !leg.cancel) {
        expect(stats.titled, "production open finishes and retitles");
        expect(stats.done_ms > stats.map_ms, "production open reports load time");
      }
      if (!leg.replace && leg.cancel) {
        expect(stats.escaped && stats.escape_at_ms >= leg.escape_min_ms,
               "Escape during open is pressed while the file is still loading");
        expect(!stats.titled, "Escape during open cancels before the file loads");
        if (!leg.throttle) {
          expect(stats.honored && stats.honor_ms >= 0 && stats.honor_ms <= 300.0,
                 "Escape during open is honored within 300 ms");
        }
      }
      if (leg.replace && !leg.cancel) {
        expect(stats.dirty, "Replace All marks the buffer dirty");
        expect(stats.undo_ok, "Replace All is one undo step");
      }
      if (leg.replace && leg.cancel) {
        expect(stats.apply_seen,
               "Escape during Replace All waits until the apply has started");
        expect(stats.escaped && stats.escape_at_ms >= leg.escape_min_ms,
               "Escape during Replace All is pressed while it is still running");
        expect(!stats.dirty, "Escape during Replace All leaves the buffer clean");
      }
      if (repeats > 1) {
        std::cout << "production " << leg.name << " rep=" << (rep + 1)
                  << "/" << repeats << " max_gap_ms=" << stats.max_gap_ms
                  << "\n";
      }
    }
    }
    std::cout << "production responsiveness end\n";
  }

  static int run() {
    failures = 0;
    g_setenv("LUNDUKE_EDIT_TEST", "1", TRUE);
    g_setenv("GDK_BACKEND", "x11", FALSE);

    test_parse_and_save_as_path();

    const std::string dir = "/tmp/lunduke-edit-tests";
    g_mkdir_with_parents(dir.c_str(), 0700);

    // Before this process registers org.lunduke.LundukeEdit. The production
    // binary is a single instance and would otherwise hand the file here.
    if (g_getenv("LUNDUKE_EDIT_SKIP_PRODUCTION") == nullptr) {
      test_production_responsiveness(dir);
    }
    if (g_getenv("LUNDUKE_EDIT_PRODUCTION_ONLY") != nullptr) {
      return failures;
    }

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
    test_gutter_trailing_newline(*w);
    test_save_open_undo(*w, dir);
    test_find_replace(*w);
    test_open_many(*app.get(), dir);
    test_review_fixes(*app.get(), *w, dir);
    test_hostile_review(*app.get(), *w, dir);
    test_round3(*app.get(), *w, dir);
    test_round4(*app.get(), *w, dir, startup_face);
    test_round5(*app.get(), *w, dir);
    test_round6(*app.get(), *w, dir);
    test_round7_status_and_replace(*w, dir);
    test_argv_large_open(dir);
    test_throttled_replace(dir);
    test_xdnd(dir);

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
    g_unsetenv("LUNDUKE_EDIT_TEST_COMMIT_STEP");
    kill_live_children();

    return failures;
  }
};

int EditChecks::failures = 0;
std::string EditChecks::argv0;

}  // namespace lundukeedit

int main(int argc, char** argv) {
  lundukeedit::EditChecks::argv0 = (argc > 0 && argv[0] != nullptr) ? argv[0] : "";
  if (g_getenv("LUNDUKE_EDIT_TEST_REPLACE_CHILD") != nullptr) {
    return lundukeedit::EditChecks::run_replace_child(argc, argv);
  }
  if (g_getenv("LUNDUKE_EDIT_TEST_DND_CHILD") != nullptr) {
    return lundukeedit::EditChecks::run_dnd_child(argc, argv);
  }
  if (g_getenv("LUNDUKE_EDIT_TEST_ARGV_CHILD") != nullptr) {
    return lundukeedit::EditChecks::run_argv_child(argc, argv);
  }
  // Children inherit this bus. Without it each editor autolaunches a
  // session dbus-daemon that double-forks out of the process group.
  const char* bus = g_getenv("DBUS_SESSION_BUS_ADDRESS");
  if ((bus == nullptr || bus[0] == '\0') &&
      g_getenv("LUNDUKE_EDIT_DBUS_WRAP") == nullptr) {
    std::vector<char*> args;
    args.push_back(const_cast<char*>("dbus-run-session"));
    args.push_back(const_cast<char*>("--"));
    for (int i = 0; i < argc; ++i) {
      args.push_back(argv[i]);
    }
    args.push_back(nullptr);
    setenv("LUNDUKE_EDIT_DBUS_WRAP", "1", 1);
    execvp("dbus-run-session", args.data());
    std::cerr << "dbus-run-session failed to start\n";
    return 1;
  }
  std::atexit(kill_live_children);
  const char* display = g_getenv("DISPLAY");
  if (display == nullptr || display[0] == '\0') {
    std::cerr << "GUI tests require a display; refusing to skip\n";
    return 1;
  }
  const int failures = lundukeedit::EditChecks::run();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "ok\n";
  return 0;
}
