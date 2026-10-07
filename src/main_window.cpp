// SPDX-License-Identifier: GPL-3.0-or-later

#include "main_window.hpp"
#include "application.hpp"

#include <giomm/file.h>
#include <glib.h>
#include <glibmm/convert.h>
#include <glibmm/fileutils.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/aboutdialog.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/filechooserdialog.h>
#include <gtkmm/fontchooserdialog.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/pagesetup.h>
#include <gtkmm/printoperation.h>
#include <gtkmm/printsettings.h>
#include <gtkmm/radiobutton.h>
#include <gtkmm/stock.h>
#include <gtkmm/stylecontext.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lundukeedit {
namespace {

using NewlineStyle = MainWindow::NewlineStyle;

const char* kVersion = "0.9";
constexpr const char* kAppId = "org.lunduke.LundukeEdit";
constexpr const char* kFallbackIcon = "accessories-text-editor";

// Prefer shipped hicolor app id (Paint pattern); fall back to freedesktop
// text-editor name when Bob's artwork is not installed yet.
Glib::ustring resolve_app_icon_name() {
  auto theme = Gtk::IconTheme::get_default();
  if (theme && theme->has_icon(kAppId)) {
    return kAppId;
  }
  return kFallbackIcon;
}

std::string format_bytes(std::size_t n) {
  std::string digits = std::to_string(n);
  std::string out;
  const int len = static_cast<int>(digits.size());
  for (int i = 0; i < len; ++i) {
    if (i > 0 && (len - i) % 3 == 0) {
      out.push_back(',');
    }
    out.push_back(digits[static_cast<std::size_t>(i)]);
  }
  out += (n == 1) ? " byte" : " bytes";
  return out;
}

constexpr std::size_t kDefaultMaxOpenBytes = 32u * 1024u * 1024u;
constexpr std::size_t kDefaultMaxPasteBytes = 32u * 1024u * 1024u;
constexpr std::size_t kReserveCapBytes = 32u * 1024u * 1024u;
constexpr int kMaxColumnWalk = 4096;
constexpr int kLongLineChars = 4000;
constexpr int kPrintChunkChars = 4000;
constexpr int kDefaultMaxFindHits = 10000;
constexpr int kDefaultFindChunk = 200;
constexpr std::size_t kDefaultHugeUndoBytes = 8u * 1024u * 1024u;

std::size_t env_size(const char* name, std::size_t fallback) {
  const char* value = g_getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value) {
    return fallback;
  }
  return static_cast<std::size_t>(parsed);
}

int env_int(const char* name, int fallback) {
  const char* value = g_getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || parsed <= 0) {
    return fallback;
  }
  return static_cast<int>(parsed);
}

std::size_t max_open_bytes() {
  return env_size("LUNDUKE_EDIT_TEST_MAX_OPEN", kDefaultMaxOpenBytes);
}

std::size_t max_paste_bytes() {
  return env_size("LUNDUKE_EDIT_TEST_MAX_PASTE", kDefaultMaxPasteBytes);
}

int max_find_hits() {
  return env_int("LUNDUKE_EDIT_TEST_MAX_HITS", kDefaultMaxFindHits);
}

int find_chunk_size() {
  return env_int("LUNDUKE_EDIT_TEST_CHUNK", kDefaultFindChunk);
}

std::size_t huge_undo_limit() {
  return env_size("LUNDUKE_EDIT_TEST_HUGE_BYTES", kDefaultHugeUndoBytes);
}

std::size_t count_newlines(const char* data, std::size_t len) {
  return static_cast<std::size_t>(
      std::count(data, data + len, '\n'));
}

bool has_long_line(const char* data, std::size_t len, std::size_t limit) {
  std::size_t run = 0;
  for (std::size_t i = 0; i < len; ++i) {
    if (data[i] == '\n') {
      run = 0;
    } else if (++run >= limit) {
      return true;
    }
  }
  return false;
}

// Buffer text uses LF. The style recorded here is written back on save.
NewlineStyle normalize_newlines(std::string& raw) {
  std::size_t crlf = 0;
  std::size_t lf = 0;
  std::size_t cr = 0;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\r') {
      if (i + 1 < raw.size() && raw[i + 1] == '\n') {
        ++crlf;
        ++i;
      } else {
        ++cr;
      }
    } else if (raw[i] == '\n') {
      ++lf;
    }
  }
  NewlineStyle style = NewlineStyle::Lf;
  if (crlf > 0 && crlf >= lf && crlf >= cr) {
    style = NewlineStyle::Crlf;
  } else if (cr > 0 && cr > lf) {
    style = NewlineStyle::Cr;
  }
  if (crlf == 0 && cr == 0) {
    return NewlineStyle::Lf;
  }
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\r') {
      out.push_back('\n');
      if (i + 1 < raw.size() && raw[i + 1] == '\n') {
        ++i;
      }
    } else {
      out.push_back(raw[i]);
    }
  }
  raw.swap(out);
  return style;
}

std::string apply_newline_style(const std::string& in, NewlineStyle style) {
  if (style == NewlineStyle::Lf) {
    return in;
  }
  std::string out;
  out.reserve(in.size() + (style == NewlineStyle::Crlf ? in.size() / 8 : 0));
  for (char c : in) {
    if (c == '\n') {
      if (style == NewlineStyle::Crlf) {
        out.push_back('\r');
        out.push_back('\n');
      } else {
        out.push_back('\r');
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool write_all_fd(int fd, const std::string& bytes, std::string& error) {
  const char* p = bytes.data();
  std::size_t left = bytes.size();
  while (left > 0) {
    const ssize_t n = ::write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = std::string("Could not write file: ") + std::strerror(errno);
      return false;
    }
    if (n == 0) {
      error = "Could not write file.";
      return false;
    }
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  if (::fsync(fd) != 0) {
    error = std::string("Could not flush file to disk: ") + std::strerror(errno);
    return false;
  }
  return true;
}

bool fsync_parent_dir(const std::string& dir, std::string& error) {
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0) {
    error = std::string("Could not flush directory: ") + std::strerror(errno);
    return false;
  }
  if (::fsync(dfd) != 0 && errno != EINVAL && errno != EROFS) {
    error = std::string("Could not flush directory: ") + std::strerror(errno);
    ::close(dfd);
    return false;
  }
  ::close(dfd);
  return true;
}

// Hard-linked files share an inode. Replacing that inode would leave the
// other names pointing at the old bytes, so the new text is written into
// the existing file.
bool write_in_place(const std::string& dest, const std::string& bytes,
                    std::string& error) {
  const int fd = ::open(dest.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    error = std::string("Could not write file: ") + std::strerror(errno);
    return false;
  }
  if (!write_all_fd(fd, bytes, error)) {
    ::close(fd);
    return false;
  }
  if (::ftruncate(fd, static_cast<off_t>(bytes.size())) != 0) {
    error = std::string("Could not write file: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  if (::fsync(fd) != 0) {
    error = std::string("Could not flush file to disk: ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  ::close(fd);
  return true;
}

// Write bytes to a temp file in the same directory and rename over the
// destination only after the temp file is flushed. A symlink is resolved
// first so the link itself is not replaced. Hard links are updated in place.
bool replace_file_contents(const std::string& path, const std::string& bytes,
                           std::string& error) {
  std::string dest = path;
  struct stat listed {};
  if (::lstat(path.c_str(), &listed) == 0 && S_ISLNK(listed.st_mode)) {
    char* canon = ::realpath(path.c_str(), nullptr);
    if (canon == nullptr) {
      error = std::string("Could not resolve symlink: ") + std::strerror(errno);
      return false;
    }
    dest.assign(canon);
    std::free(canon);
  }

  struct stat st {};
  const bool have_stat = (::stat(dest.c_str(), &st) == 0);
  if (have_stat && !S_ISREG(st.st_mode)) {
    error = "Could not replace file: not a regular file.";
    return false;
  }
  if (have_stat && st.st_nlink > 1) {
    return write_in_place(dest, bytes, error);
  }

  const std::string dir = Glib::path_get_dirname(dest);
  const std::string base = Glib::path_get_basename(dest);
  const std::string pattern = Glib::build_filename(dir, "." + base + ".XXXXXX");
  std::vector<char> tmpl(pattern.begin(), pattern.end());
  tmpl.push_back('\0');

  const int fd = mkstemp(tmpl.data());
  if (fd < 0) {
    error = std::string("Could not write temporary file: ") +
            std::strerror(errno);
    return false;
  }
  const std::string tmp_path(tmpl.data());

  const mode_t mode = have_stat ? static_cast<mode_t>(st.st_mode & 0777)
                                : static_cast<mode_t>(0644);
  if (fchmod(fd, mode) != 0) {
    // Keep going; the bytes matter more than the mode bit.
  }
  if (have_stat) {
    // Restore the previous owner when the process is allowed to.
    if (fchown(fd, st.st_uid, st.st_gid) != 0) {
      // EPERM is normal for an unprivileged user. The new inode stays ours.
    }
  }
  if (!write_all_fd(fd, bytes, error)) {
    ::close(fd);
    unlink(tmp_path.c_str());
    return false;
  }
  if (::close(fd) != 0) {
    error = std::string("Could not write temporary file: ") +
            std::strerror(errno);
    unlink(tmp_path.c_str());
    return false;
  }

  if (std::rename(tmp_path.c_str(), dest.c_str()) != 0) {
    error = std::string("Could not replace file: ") + std::strerror(errno);
    unlink(tmp_path.c_str());
    return false;
  }
  if (!fsync_parent_dir(dir, error)) {
    return false;
  }
  return true;
}

enum class ReadStatus { Ok, Failed, TooLarge };

// open + fstat + read until EOF. st_size is only a reserve hint. A reported
// size of 0 (/proc, /sys, some FUSE) is not treated as an empty file.
// Reading stops at max_bytes unless unlimited is set, so a sparse st_size
// cannot force a huge allocation.
ReadStatus read_file_fully(const std::string& path, std::string& raw,
                           std::string& error, std::size_t max_bytes,
                           bool unlimited) {
  raw.clear();
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error = path;
    return ReadStatus::Failed;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    error = path;
    return ReadStatus::Failed;
  }
  try {
    if (st.st_size > 0) {
      auto hint = static_cast<std::size_t>(st.st_size);
      if (hint > kReserveCapBytes) {
        hint = kReserveCapBytes;
      }
      raw.reserve(hint);
    }
    char buf[65536];
    while (true) {
      const ssize_t n = ::read(fd, buf, sizeof buf);
      if (n == 0) {
        break;
      }
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        raw.clear();
        ::close(fd);
        error = path;
        return ReadStatus::Failed;
      }
      const auto got = static_cast<std::size_t>(n);
      if (!unlimited && (got > max_bytes || raw.size() > max_bytes - got)) {
        raw.clear();
        ::close(fd);
        error = path;
        return ReadStatus::TooLarge;
      }
      raw.append(buf, got);
    }
  } catch (const std::bad_alloc&) {
    raw.clear();
    ::close(fd);
    error = "Not enough memory to open this file.";
    return ReadStatus::Failed;
  }
  ::close(fd);
  return ReadStatus::Ok;
}

}  // namespace

std::string MainWindow::ensure_save_as_path(std::string path) {
  // The file chooser already confirmed this path, including overwrite.
  // Rewriting the extension afterward writes a different file than the one
  // the user confirmed (notes.txt.txt must stay notes.txt.txt; "report" must
  // not become report.txt and clobber an existing file).
  return path;
}

bool MainWindow::parse_go_to_line(const std::string& text, int& line) {
  if (text.empty()) {
    return false;
  }
  try {
    std::size_t consumed = 0;
    const int value = std::stoi(text, &consumed);
    if (consumed != text.size()) {
      return false;
    }
    line = value;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

MainWindow::MainWindow(Application& app) : app_(app) {
  set_title("Untitled — Lunduke Edit");
  set_default_size(640, 420);
  set_border_width(0);
  // Reinforce default icon for WMs that ignore gtk_window_set_default_icon_name.
  set_icon_name(resolve_app_icon_name());

  font_desc_ = Pango::FontDescription(
      app_.font().empty() ? "Monospace 11" : app_.font());
  tab_width_ = app_.tab_width();
  prefer_utf8_ = app_.prefer_utf8();
  open_charset_ = app_.open_charset().empty() ? "UTF-8" : app_.open_charset();
  // A fresh document is UTF-8 even when the next Open uses another charset.
  encoding_ = "UTF-8";
  saved_encoding_ = "UTF-8";

  auto buf = Gsv::Buffer::create();
  buf->set_max_undo_levels(100);
  text_view_.set_buffer(buf);
  find_tag_ = buf->create_tag("lunduke-find-hit");
  find_tag_->property_background() = "#c4d8f0";

  // gtkmm's connect() defaults to after=true. insert-text and delete-range
  // run their default handlers first in that case, so "changed" updates the
  // status before the cache moves and delete-range has already removed the
  // text we need to measure. Run these before the default handlers.
  buf->signal_insert().connect(
      sigc::mem_fun(*this, &MainWindow::on_text_inserted), false);
  buf->signal_erase().connect(
      sigc::mem_fun(*this, &MainWindow::on_text_erased), false);

  build_ui();
  build_menus();
  apply_font(font_desc_);
  apply_tab_width(tab_width_);

  text_view_.set_wrap_mode(app_.wrap_text() ? Gtk::WRAP_WORD_CHAR
                                            : Gtk::WRAP_NONE);
  text_view_.set_monospace(true);
  text_view_.set_accepts_tab(true);
  text_view_.set_left_margin(4);
  text_view_.set_right_margin(4);
  text_view_.set_top_margin(2);
  text_view_.set_bottom_margin(2);
  text_view_.set_show_line_marks(false);
  text_view_.set_auto_indent(false);
  text_view_.set_highlight_current_line(false);

  buf->signal_changed().connect(
      sigc::mem_fun(*this, &MainWindow::on_buffer_changed));
  buf->signal_modified_changed().connect(
      sigc::mem_fun(*this, &MainWindow::on_modified_changed));
  buf->signal_mark_set().connect(
      sigc::mem_fun(*this, &MainWindow::on_cursor_moved));
  // Undo/redo menu sensitivity must track GtkSourceView undo-manager state.
  // signal_changed() alone is unreliable (keyboard undo via View bindings,
  // and can-undo often notifies after changed). Use property notify + menu map.
  buf->property_can_undo().signal_changed().connect(
      sigc::mem_fun(*this, &MainWindow::update_undo_redo_sensitivity));
  buf->property_can_redo().signal_changed().connect(
      sigc::mem_fun(*this, &MainWindow::update_undo_redo_sensitivity));

  if (!prefer_utf8_ && enc_latin1_item_) {
    seeding_ = true;
    enc_latin1_item_->set_active(true);
    seeding_ = false;
    encoding_ = "UTF-8";
    saved_encoding_ = "UTF-8";
  }

  property_is_active().signal_changed().connect([this]() {
    if (get_visible() && is_active()) {
      app_.note_window_focus(this);
    }
  });

  void (*paste_cb)(GtkTextView*, gpointer) =
      [](GtkTextView* view, gpointer user) {
        static_cast<MainWindow*>(user)->handle_paste_clipboard(view);
      };
  g_signal_connect(text_view_.gobj(), "paste-clipboard", G_CALLBACK(paste_cb),
                   this);

  show_all_children();
  status_find_frame_.hide();
  update_status();
  update_undo_redo_sensitivity();
  rebuild_recents_menu();
}

MainWindow::~MainWindow() {
  find_idle_.disconnect();
  end_find_user_action();
}

Glib::RefPtr<Gsv::Buffer> MainWindow::buffer() {
  return Glib::RefPtr<Gsv::Buffer>::cast_static(text_view_.get_buffer());
}

void MainWindow::build_ui() {
  add(root_);
  root_.pack_start(menubar_, Gtk::PACK_SHRINK);

  gutter_ = Gtk::manage(new LineGutter(text_view_));
  editor_row_.pack_start(*gutter_, Gtk::PACK_SHRINK);

  scrolled_.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
  scrolled_.add(text_view_);
  // Packing into the scrolled window replaces the text view's vadjustment.
  // Rebind the gutter to the adjustment that actually scrolls.
  gutter_->follow_view_adjustment();
  editor_row_.pack_start(scrolled_, Gtk::PACK_EXPAND_WIDGET);
  root_.pack_start(editor_row_, Gtk::PACK_EXPAND_WIDGET);

  status_box_.set_border_width(2);
  status_box_.set_spacing(2);

  status_pos_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_mode_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_enc_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_find_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_bytes_frame_.set_shadow_type(Gtk::SHADOW_IN);

  status_pos_.set_halign(Gtk::ALIGN_START);
  status_pos_.set_margin_start(6);
  status_pos_.set_margin_end(6);
  status_pos_.set_margin_top(2);
  status_pos_.set_margin_bottom(2);

  status_mode_.set_halign(Gtk::ALIGN_CENTER);
  status_mode_.set_margin_start(10);
  status_mode_.set_margin_end(10);
  status_mode_.set_margin_top(2);
  status_mode_.set_margin_bottom(2);

  status_enc_.set_halign(Gtk::ALIGN_CENTER);
  status_enc_.set_margin_start(8);
  status_enc_.set_margin_end(8);
  status_enc_.set_margin_top(2);
  status_enc_.set_margin_bottom(2);

  status_find_.set_halign(Gtk::ALIGN_CENTER);
  status_find_.set_margin_start(8);
  status_find_.set_margin_end(8);
  status_find_.set_margin_top(2);
  status_find_.set_margin_bottom(2);

  status_bytes_.set_halign(Gtk::ALIGN_END);
  status_bytes_.set_margin_start(6);
  status_bytes_.set_margin_end(6);
  status_bytes_.set_margin_top(2);
  status_bytes_.set_margin_bottom(2);

  status_pos_frame_.add(status_pos_);
  status_mode_frame_.add(status_mode_);
  status_enc_frame_.add(status_enc_);
  status_find_frame_.add(status_find_);
  status_bytes_frame_.add(status_bytes_);

  status_box_.pack_start(status_pos_frame_, Gtk::PACK_EXPAND_WIDGET);
  status_box_.pack_start(status_mode_frame_, Gtk::PACK_SHRINK);
  status_box_.pack_start(status_enc_frame_, Gtk::PACK_SHRINK);
  status_box_.pack_start(status_find_frame_, Gtk::PACK_SHRINK);
  status_box_.pack_start(status_bytes_frame_, Gtk::PACK_SHRINK);

  root_.pack_start(status_box_, Gtk::PACK_SHRINK);
}

void MainWindow::build_menus() {
  add_accel_group(Gtk::AccelGroup::create());

  // ---- File ----
  auto* file_menu = Gtk::manage(new Gtk::Menu());
  auto* file_item = Gtk::manage(new Gtk::MenuItem("_File", true));
  file_item->set_submenu(*file_menu);
  menubar_.append(*file_item);

  auto* new_i = Gtk::manage(new Gtk::MenuItem("_New", true));
  new_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_new));
  new_i->add_accelerator("activate", get_accel_group(), GDK_KEY_n,
                         Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*new_i);

  auto* open_i = Gtk::manage(new Gtk::MenuItem("_Open…", true));
  open_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_open));
  open_i->add_accelerator("activate", get_accel_group(), GDK_KEY_o,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*open_i);

  auto* recent_item = Gtk::manage(new Gtk::MenuItem("Open _Recent", true));
  recents_menu_ = Gtk::manage(new Gtk::Menu());
  recent_item->set_submenu(*recents_menu_);
  file_menu->append(*recent_item);

  file_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* save_i = Gtk::manage(new Gtk::MenuItem("_Save", true));
  save_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_save));
  save_i->add_accelerator("activate", get_accel_group(), GDK_KEY_s,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*save_i);

  auto* save_as_i = Gtk::manage(new Gtk::MenuItem("Save _As…", true));
  save_as_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_save_as));
  save_as_i->add_accelerator("activate", get_accel_group(), GDK_KEY_s,
                             Gdk::CONTROL_MASK | Gdk::SHIFT_MASK,
                             Gtk::ACCEL_VISIBLE);
  file_menu->append(*save_as_i);

  file_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* page_setup_i = Gtk::manage(new Gtk::MenuItem("Page Set_up…", true));
  page_setup_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_page_setup));
  file_menu->append(*page_setup_i);

  auto* print_i = Gtk::manage(new Gtk::MenuItem("_Print…", true));
  print_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_print));
  print_i->add_accelerator("activate", get_accel_group(), GDK_KEY_p,
                           Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*print_i);

  file_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* exit_i = Gtk::manage(new Gtk::MenuItem("E_xit", true));
  exit_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_exit));
  exit_i->add_accelerator("activate", get_accel_group(), GDK_KEY_q,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*exit_i);

  // ---- Edit ----
  auto* edit_menu = Gtk::manage(new Gtk::Menu());
  auto* edit_item = Gtk::manage(new Gtk::MenuItem("_Edit", true));
  edit_item->set_submenu(*edit_menu);
  menubar_.append(*edit_item);
  // Refresh Undo/Redo when the Edit menu is opened so items always match
  // buffer->can_undo() / can_redo() (covers keyboard undo/redo paths).
  edit_menu->signal_map().connect(
      sigc::mem_fun(*this, &MainWindow::update_undo_redo_sensitivity));

  undo_item_ = Gtk::manage(new Gtk::MenuItem("_Undo", true));
  undo_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_undo));
  undo_item_->add_accelerator("activate", get_accel_group(), GDK_KEY_z,
                              Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*undo_item_);

  redo_item_ = Gtk::manage(new Gtk::MenuItem("_Redo", true));
  redo_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_redo));
  redo_item_->add_accelerator("activate", get_accel_group(), GDK_KEY_z,
                              Gdk::CONTROL_MASK | Gdk::SHIFT_MASK,
                              Gtk::ACCEL_VISIBLE);
  // Also Ctrl+Y
  redo_item_->add_accelerator("activate", get_accel_group(), GDK_KEY_y,
                              Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*redo_item_);

  edit_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* cut_i = Gtk::manage(new Gtk::MenuItem("Cu_t", true));
  cut_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_cut));
  cut_i->add_accelerator("activate", get_accel_group(), GDK_KEY_x,
                         Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*cut_i);

  auto* copy_i = Gtk::manage(new Gtk::MenuItem("_Copy", true));
  copy_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_copy));
  copy_i->add_accelerator("activate", get_accel_group(), GDK_KEY_c,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*copy_i);

  auto* paste_i = Gtk::manage(new Gtk::MenuItem("_Paste", true));
  paste_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_paste));
  paste_i->add_accelerator("activate", get_accel_group(), GDK_KEY_v,
                           Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*paste_i);

  edit_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* sel_i = Gtk::manage(new Gtk::MenuItem("Select _All", true));
  sel_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_select_all));
  sel_i->add_accelerator("activate", get_accel_group(), GDK_KEY_a,
                         Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*sel_i);

  edit_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  // Notepad-like: Find under Edit (Search menu keeps a duplicate entry).
  auto* edit_find_i = Gtk::manage(new Gtk::MenuItem("_Find…", true));
  edit_find_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_find));
  edit_find_i->add_accelerator("activate", get_accel_group(), GDK_KEY_f,
                               Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*edit_find_i);

  // ---- Search ----
  auto* search_menu = Gtk::manage(new Gtk::Menu());
  auto* search_item = Gtk::manage(new Gtk::MenuItem("_Search", true));
  search_item->set_submenu(*search_menu);
  menubar_.append(*search_item);

  auto* find_i = Gtk::manage(new Gtk::MenuItem("_Find…", true));
  find_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_find));
  find_i->add_accelerator("activate", get_accel_group(), GDK_KEY_f,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  search_menu->append(*find_i);

  auto* find_next_i = Gtk::manage(new Gtk::MenuItem("Find _Next", true));
  find_next_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_find_next));
  find_next_i->add_accelerator("activate", get_accel_group(), GDK_KEY_F3,
                               Gdk::ModifierType(0), Gtk::ACCEL_VISIBLE);
  search_menu->append(*find_next_i);

  auto* goto_i = Gtk::manage(new Gtk::MenuItem("_Go to Line…", true));
  goto_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_go_to_line));
  goto_i->add_accelerator("activate", get_accel_group(), GDK_KEY_g,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  search_menu->append(*goto_i);

  // ---- Text ----
  auto* text_menu = Gtk::manage(new Gtk::Menu());
  auto* text_item = Gtk::manage(new Gtk::MenuItem("_Text", true));
  text_item->set_submenu(*text_menu);
  menubar_.append(*text_item);

  wrap_item_ = Gtk::manage(new Gtk::CheckMenuItem("_Wrap Text", true));
  wrap_item_->set_active(app_.wrap_text());
  wrap_item_->signal_toggled().connect(
      sigc::mem_fun(*this, &MainWindow::on_toggle_wrap));
  text_menu->append(*wrap_item_);

  line_numbers_item_ =
      Gtk::manage(new Gtk::CheckMenuItem("Show _Line Numbers", true));
  line_numbers_item_->set_active(true);
  line_numbers_item_->signal_toggled().connect(
      sigc::mem_fun(*this, &MainWindow::on_toggle_line_numbers));
  text_menu->append(*line_numbers_item_);

  auto* tab_i = Gtk::manage(new Gtk::MenuItem("_Tab Width…", true));
  tab_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_tab_width));
  text_menu->append(*tab_i);

  auto* font_under_text = Gtk::manage(new Gtk::MenuItem("_Font…", true));
  font_under_text->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_font));
  text_menu->append(*font_under_text);

  text_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* enc_label = Gtk::manage(new Gtk::MenuItem("Encoding"));
  enc_label->set_sensitive(false);
  text_menu->append(*enc_label);

  Gtk::RadioMenuItem::Group enc_group;
  enc_utf8_item_ =
      Gtk::manage(new Gtk::RadioMenuItem(enc_group, "_UTF-8", true));
  enc_utf8_item_->set_active(true);
  enc_utf8_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_encoding_utf8));
  text_menu->append(*enc_utf8_item_);

  enc_latin1_item_ = Gtk::manage(
      new Gtk::RadioMenuItem(enc_group, "_Latin-1 (ISO-8859-1)", true));
  enc_latin1_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_encoding_latin1));
  text_menu->append(*enc_latin1_item_);

  // ---- Font (top-level) ----
  auto* font_menu = Gtk::manage(new Gtk::Menu());
  auto* font_top = Gtk::manage(new Gtk::MenuItem("F_ont", true));
  font_top->set_submenu(*font_menu);
  menubar_.append(*font_top);

  auto* font_i = Gtk::manage(new Gtk::MenuItem("_Font…", true));
  font_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_font));
  font_menu->append(*font_i);

  // ---- Help ----
  auto* help_menu = Gtk::manage(new Gtk::Menu());
  auto* help_item = Gtk::manage(new Gtk::MenuItem("_Help", true));
  help_item->set_submenu(*help_menu);
  menubar_.append(*help_item);

  auto* about_i = Gtk::manage(new Gtk::MenuItem("_About Lunduke Edit", true));
  about_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_about));
  help_menu->append(*about_i);
}

void MainWindow::load_seed_sample() {
  // Fresh window / first launch: blank untitled document (no demo text).
  seeding_ = true;
  auto buf = buffer();
  buf->begin_not_undoable_action();
  clear_find_highlights();
  buf->set_text("");
  buf->end_not_undoable_action();
  buf->set_modified(false);
  file_path_.clear();
  encoding_ = "UTF-8";
  saved_encoding_ = "UTF-8";
  encoding_dirty_ = false;
  newline_style_ = NewlineStyle::Lf;
  note_loaded_text("");
  last_match_valid_ = false;
  set_dirty(false);
  seeding_ = false;
  update_title();
  update_status();
  update_undo_redo_sensitivity();
  if (gutter_) {
    gutter_->refresh();
  }
  buf->place_cursor(buf->begin());
  update_status();
}

bool MainWindow::confirm_large_open(const std::string& path) {
  if (g_getenv("LUNDUKE_EDIT_TEST") != nullptr) {
    return g_getenv("LUNDUKE_EDIT_TEST_LARGE") != nullptr;
  }
  Gtk::MessageDialog dlg(
      *this,
      "This file is larger than 32 MiB.",
      false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_NONE, true);
  dlg.set_secondary_text(
      "Opening \"" + path +
      "\" may use a lot of memory. Open it anyway?");
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Open", Gtk::RESPONSE_ACCEPT);
  dlg.set_default_response(Gtk::RESPONSE_CANCEL);
  return dlg.run() == Gtk::RESPONSE_ACCEPT;
}

bool MainWindow::open_file(const std::string& path) {
  std::string raw;
  std::string read_error;
  const std::size_t cap = max_open_bytes();
  ReadStatus status = ReadStatus::Failed;
  try {
    status = read_file_fully(path, raw, read_error, cap, false);
    if (status == ReadStatus::TooLarge) {
      if (!confirm_large_open(path)) {
        report_error("File not opened.",
                     "The file is larger than 32 MiB.");
        return false;
      }
      status = read_file_fully(path, raw, read_error, cap, true);
    }
  } catch (const std::bad_alloc&) {
    report_error("Not enough memory to open this file.", path);
    return false;
  }
  if (status != ReadStatus::Ok) {
    report_error("Could not open file.",
                 read_error.empty() ? path : read_error);
    return false;
  }

  NewlineStyle newlines = NewlineStyle::Lf;
  Glib::ustring text;
  std::string loaded_encoding = "UTF-8";
  try {
    newlines = normalize_newlines(raw);
    if (prefer_utf8_) {
      if (g_utf8_validate(raw.data(), static_cast<gssize>(raw.size()),
                          nullptr)) {
        text = Glib::ustring(raw);
        loaded_encoding = "UTF-8";
      } else {
        // Bytes are not valid UTF-8. Keep them as Latin-1 so a later save
        // writes the original bytes back instead of UTF-8 (0xE9 must not
        // become C3 A9 while the buffer still looks clean).
        text = Glib::convert(raw, "UTF-8", "ISO-8859-1");
        loaded_encoding = "ISO-8859-1";
      }
    } else {
      const std::string charset =
          open_charset_.empty() ? "ISO-8859-1" : open_charset_;
      text = Glib::convert(raw, "UTF-8", charset);
      loaded_encoding = charset;
    }
  } catch (const Glib::ConvertError& e) {
    report_error("Encoding error while opening.", e.what());
    return false;
  } catch (const std::bad_alloc&) {
    report_error("Not enough memory to open this file.", path);
    return false;
  }

  const bool long_line =
      has_long_line(text.data(), text.bytes(), kLongLineChars);

  seeding_ = true;
  // Document encoding follows the file. The Text menu stays on the open
  // charset so the next file is not forced to this detection.
  encoding_ = loaded_encoding;
  newline_style_ = newlines;
  auto buf = buffer();
  try {
    buf->begin_not_undoable_action();
    clear_find_highlights();
    buf->set_text(text);
    buf->end_not_undoable_action();
  } catch (const std::bad_alloc&) {
    try {
      buf->end_not_undoable_action();
    } catch (...) {
    }
    seeding_ = false;
    report_error("Not enough memory to open this file.", path);
    return false;
  }
  note_loaded_text(text);
  buf->set_modified(false);
  file_path_ = path;
  saved_encoding_ = encoding_;
  encoding_dirty_ = false;
  last_match_valid_ = false;
  set_dirty(false);
  seeding_ = false;
  if (long_line) {
    force_wrap_off();
  }
  update_title();
  update_status();
  update_undo_redo_sensitivity();
  if (gutter_) {
    gutter_->refresh();
  }
  remember_recent(path);
  return true;
}

Glib::ustring MainWindow::current_basename() const {
  if (file_path_.empty()) {
    return "Untitled";
  }
  const auto pos = file_path_.find_last_of('/');
  if (pos == std::string::npos) {
    return file_path_;
  }
  return file_path_.substr(pos + 1);
}

void MainWindow::update_title() {
  Glib::ustring title = current_basename();
  if (dirty_) {
    title += " *";
  }
  title += " — Lunduke Edit";
  set_title(title);
}

void MainWindow::set_dirty(bool dirty) {
  dirty_ = dirty;
  update_title();
}

void MainWindow::update_cursor_status() {
  auto buf = text_view_.get_buffer();
  if (!buf) {
    return;
  }
  auto iter = buf->get_iter_at_mark(buf->get_insert());
  const int line = iter.get_line() + 1;
  const int col = display_column_at(iter);
  status_pos_.set_text("Ln " + std::to_string(line) + ", Col " +
                       std::to_string(col));
}

void MainWindow::update_bytes_status() {
  status_bytes_.set_text(format_bytes(cached_save_bytes()));
  status_mode_.set_text(overwrite_ ? "Overwrite" : "Insert");
  status_enc_.set_text(encoding_ == "ISO-8859-1" ? "Latin-1" : encoding_);
}

void MainWindow::update_status() {
  update_cursor_status();
  update_bytes_status();
}

std::size_t MainWindow::cached_save_bytes() const {
  std::size_t n = 0;
  if (encoding_ == "UTF-8") {
    n = utf8_bytes_;
  } else if (auto buf = text_view_.get_buffer()) {
    const int chars = buf->get_char_count();
    if (chars > 0) {
      n = static_cast<std::size_t>(chars);
    }
  }
  if (newline_style_ == NewlineStyle::Crlf) {
    n += newline_count_;
  }
  return n;
}

void MainWindow::note_loaded_text(const Glib::ustring& text) {
  utf8_bytes_ = text.bytes();
  newline_count_ = count_newlines(text.data(), text.bytes());
}

void MainWindow::on_text_inserted(const Gtk::TextBuffer::iterator& pos,
                                  const Glib::ustring& text, int /*bytes*/) {
  utf8_bytes_ += text.bytes();
  newline_count_ += count_newlines(text.data(), text.bytes());
  if (text_view_.get_wrap_mode() != Gtk::WRAP_NONE &&
      (has_long_line(text.data(), text.bytes(), kLongLineChars) ||
       pos.get_chars_in_line() >= kLongLineChars)) {
    force_wrap_off();
  }
}

void MainWindow::on_text_erased(const Gtk::TextBuffer::iterator& start,
                                const Gtk::TextBuffer::iterator& end) {
  const Glib::ustring gone = start.get_text(end);
  const std::size_t bytes = gone.bytes();
  const std::size_t newlines = count_newlines(gone.data(), bytes);
  utf8_bytes_ = (bytes > utf8_bytes_) ? 0 : utf8_bytes_ - bytes;
  newline_count_ =
      (newlines > newline_count_) ? 0 : newline_count_ - newlines;
}

void MainWindow::force_wrap_off() {
  if (text_view_.get_wrap_mode() == Gtk::WRAP_NONE &&
      (wrap_item_ == nullptr || !wrap_item_->get_active())) {
    return;
  }
  suppress_wrap_pref_ = true;
  text_view_.set_wrap_mode(Gtk::WRAP_NONE);
  if (wrap_item_ && wrap_item_->get_active()) {
    wrap_item_->set_active(false);
  }
  suppress_wrap_pref_ = false;
}

bool MainWindow::is_empty_untitled() const {
  if (dirty_ || !file_path_.empty()) {
    return false;
  }
  auto buf = text_view_.get_buffer();
  return buf && buf->get_char_count() == 0;
}

bool MainWindow::on_focus_in_event(GdkEventFocus* event) {
  app_.note_window_focus(this);
  return Gtk::ApplicationWindow::on_focus_in_event(event);
}

void MainWindow::update_undo_redo_sensitivity() {
  auto buf = buffer();
  if (undo_item_) {
    undo_item_->set_sensitive(buf && buf->can_undo());
  }
  if (redo_item_) {
    redo_item_->set_sensitive(buf && buf->can_redo());
  }
}

void MainWindow::on_buffer_changed() {
  // Dirty state follows the undo save point (signal_modified_changed),
  // not every change. Undo back to the saved text must clear the marker.
  if (find_highlights_on_) {
    find_highlights_on_ = false;
    clear_find_highlights();
  }
  set_find_count(-1, false);
  update_bytes_status();
  if (text_view_.get_wrap_mode() != Gtk::WRAP_NONE) {
    auto buf = text_view_.get_buffer();
    if (buf) {
      auto iter = buf->get_iter_at_mark(buf->get_insert());
      if (iter.get_chars_in_line() >= kLongLineChars) {
        force_wrap_off();
      }
    }
  }
  if (gutter_) {
    gutter_->queue_draw();
  }
}

void MainWindow::on_modified_changed() { refresh_dirty_from_buffer(); }

void MainWindow::refresh_dirty_from_buffer() {
  if (seeding_) {
    return;
  }
  auto buf = buffer();
  const bool text_dirty = buf && buf->get_modified();
  set_dirty(text_dirty || encoding_dirty_);
}

int MainWindow::display_column_at(const Gtk::TextIter& iter) const {
  const int line_off = iter.get_line_offset();
  // A multi-megabyte line must not be walked on every cursor motion.
  // Past the cap, the column is the character index (tabs are not expanded).
  if (line_off > kMaxColumnWalk) {
    return line_off + 1;
  }
  Gtk::TextIter line_start = iter;
  line_start.set_line_offset(0);
  const int tab = std::max(1, tab_width_);
  int col = 0;
  for (auto it = line_start; it.compare(iter) < 0; it.forward_char()) {
    if (it.get_char() == '\t') {
      col += tab - (col % tab);
    } else {
      col += 1;
    }
  }
  return col + 1;
}

void MainWindow::report_error(const Glib::ustring& primary,
                              const Glib::ustring& secondary) {
  if (g_getenv("LUNDUKE_EDIT_TEST") != nullptr) {
    return;
  }
  Gtk::MessageDialog dlg(*this, primary, false, Gtk::MESSAGE_ERROR,
                         Gtk::BUTTONS_OK, true);
  dlg.set_secondary_text(secondary);
  dlg.run();
}

void MainWindow::on_cursor_moved(
    const Gtk::TextBuffer::iterator& /*loc*/,
    const Glib::RefPtr<Gtk::TextBuffer::Mark>& mark) {
  if (mark == text_view_.get_buffer()->get_insert()) {
    update_cursor_status();
  }
}

bool MainWindow::confirm_discard_or_save() {
  if (!dirty_) {
    return true;
  }
  // Tests set this so a modal dialog does not block. Unset in normal use.
  if (const char* choice = g_getenv("LUNDUKE_EDIT_TEST_DISCARD")) {
    if (std::strcmp(choice, "cancel") == 0) {
      return false;
    }
    if (std::strcmp(choice, "discard") == 0) {
      return true;
    }
    if (std::strcmp(choice, "save") == 0) {
      return save_document();
    }
  }
  Gtk::MessageDialog dlg(*this, "Save changes before continuing?", false,
                         Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_NONE, true);
  dlg.set_secondary_text("\"" + current_basename() +
                         "\" has unsaved changes.");
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Don't Save", Gtk::RESPONSE_REJECT);
  dlg.add_button("_Save", Gtk::RESPONSE_ACCEPT);
  dlg.set_default_response(Gtk::RESPONSE_ACCEPT);
  const int resp = dlg.run();
  if (resp == Gtk::RESPONSE_CANCEL || resp == Gtk::RESPONSE_DELETE_EVENT) {
    return false;
  }
  if (resp == Gtk::RESPONSE_ACCEPT) {
    return save_document();
  }
  return true;
}

void MainWindow::on_new() {
  if (!confirm_discard_or_save()) {
    return;
  }
  seeding_ = true;
  auto buf = buffer();
  buf->begin_not_undoable_action();
  clear_find_highlights();
  buf->set_text("");
  buf->end_not_undoable_action();
  buf->set_modified(false);
  file_path_.clear();
  encoding_ = "UTF-8";
  saved_encoding_ = "UTF-8";
  encoding_dirty_ = false;
  newline_style_ = NewlineStyle::Lf;
  note_loaded_text("");
  last_match_valid_ = false;
  set_dirty(false);
  seeding_ = false;
  update_title();
  update_status();
  update_undo_redo_sensitivity();
}

void MainWindow::on_open() {
  if (!confirm_discard_or_save()) {
    return;
  }
  Gtk::FileChooserDialog dlg(*this, "Open File",
                             Gtk::FILE_CHOOSER_ACTION_OPEN);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Open", Gtk::RESPONSE_ACCEPT);
  auto filter = Gtk::FileFilter::create();
  filter->set_name("Text files");
  filter->add_mime_type("text/plain");
  filter->add_pattern("*.txt");
  filter->add_pattern("*.md");
  filter->add_pattern("*");
  dlg.add_filter(filter);
  if (dlg.run() != Gtk::RESPONSE_ACCEPT) {
    return;
  }
  auto file = dlg.get_file();
  if (!file || !file->is_native() || file->get_path().empty()) {
    report_error("Cannot open this location.",
                 "Only local files can be opened.");
    return;
  }
  open_file(file->get_path());
}

void MainWindow::on_open_recent(const std::string& path) {
  if (!confirm_discard_or_save()) {
    return;
  }
  open_file(path);
}

bool MainWindow::save_to_path(const std::string& path) {
  const Glib::ustring text = text_view_.get_buffer()->get_text();
  std::string out_bytes;
  try {
    if (encoding_ == "UTF-8") {
      out_bytes.assign(text.data(), text.bytes());
    } else {
      out_bytes = Glib::convert(text, encoding_, "UTF-8");
    }
    out_bytes = apply_newline_style(out_bytes, newline_style_);
  } catch (const Glib::ConvertError& e) {
    report_error("Encoding error while saving.", e.what());
    return false;
  } catch (const std::bad_alloc&) {
    report_error("Not enough memory to save this file.", path);
    return false;
  }

  std::string write_error;
  if (!replace_file_contents(path, out_bytes, write_error)) {
    report_error("Could not save file.",
                 write_error.empty() ? path : write_error);
    return false;
  }
  file_path_ = path;
  saved_encoding_ = encoding_;
  encoding_dirty_ = false;
  // Records an undo save point so undo/redo back to this text clears
  // the buffer's modified flag.
  buffer()->set_modified(false);
  set_dirty(false);
  update_status();
  remember_recent(path);
  return true;
}

bool MainWindow::save_document() {
  if (file_path_.empty()) {
    return save_as_dialog();
  }
  return save_to_path(file_path_);
}

void MainWindow::on_save() { save_document(); }

bool MainWindow::save_as_dialog() {
  Gtk::FileChooserDialog dlg(*this, "Save As",
                             Gtk::FILE_CHOOSER_ACTION_SAVE);
  dlg.set_do_overwrite_confirmation(true);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Save", Gtk::RESPONSE_ACCEPT);
  auto filter = Gtk::FileFilter::create();
  filter->set_name("Text files");
  filter->add_mime_type("text/plain");
  filter->add_pattern("*.txt");
  filter->add_pattern("*.md");
  filter->add_pattern("*");
  dlg.add_filter(filter);
  if (!file_path_.empty()) {
    dlg.set_filename(file_path_);
  } else {
    dlg.set_current_name("Untitled.txt");
  }
  if (dlg.run() != Gtk::RESPONSE_ACCEPT) {
    return false;
  }
  auto file = dlg.get_file();
  if (!file || !file->is_native() || file->get_path().empty()) {
    report_error("Cannot save to this location.",
                 "Only local files can be saved.");
    return false;
  }
  return save_to_path(ensure_save_as_path(file->get_path()));
}

void MainWindow::on_save_as() { save_as_dialog(); }

void MainWindow::on_exit() {
  if (!app_.confirm_quit()) {
    return;
  }
  // hide() leaves the process registered as org.lunduke.LundukeEdit.
  app_.quit();
}

bool MainWindow::on_delete_event(GdkEventAny* /*event*/) {
  if (!confirm_discard_or_save()) {
    // Keep the window. GTK must not hide or destroy it.
    return true;
  }
  // Hide, then the application deletes the C++ window after this returns.
  hide();
  return true;
}

bool MainWindow::on_key_press_event(GdkEventKey* event) {
  if (event->keyval == GDK_KEY_Insert) {
    overwrite_ = !overwrite_;
    text_view_.set_overwrite(overwrite_);
    update_status();
    return true;
  }
  return Gtk::ApplicationWindow::on_key_press_event(event);
}

void MainWindow::on_undo() {
  auto buf = buffer();
  if (buf && buf->can_undo()) {
    buf->undo();
    refresh_dirty_from_buffer();
    update_undo_redo_sensitivity();
    update_status();
  }
}

void MainWindow::on_redo() {
  auto buf = buffer();
  if (buf && buf->can_redo()) {
    buf->redo();
    refresh_dirty_from_buffer();
    update_undo_redo_sensitivity();
    update_status();
  }
}

void MainWindow::on_cut() {
  auto clip = Gtk::Clipboard::get();
  text_view_.get_buffer()->cut_clipboard(clip);
  update_undo_redo_sensitivity();
}

void MainWindow::on_copy() {
  auto clip = Gtk::Clipboard::get();
  text_view_.get_buffer()->copy_clipboard(clip);
}

bool MainWindow::clipboard_paste_allowed() {
  auto clip = Gtk::Clipboard::get();
  const Glib::ustring text = clip->wait_for_text();
  const std::size_t cap = max_paste_bytes();
  if (text.bytes() > cap) {
    report_error("Paste is too large.",
                 "A single paste is limited to " + format_bytes(cap) + ".");
    return false;
  }
  return true;
}

void MainWindow::handle_paste_clipboard(GtkTextView* view) {
  if (!clipboard_paste_allowed()) {
    g_signal_stop_emission_by_name(view, "paste-clipboard");
  }
}

void MainWindow::on_paste() {
  g_signal_emit_by_name(text_view_.gobj(), "paste-clipboard");
  update_undo_redo_sensitivity();
}

void MainWindow::on_select_all() {
  auto buf = text_view_.get_buffer();
  buf->select_range(buf->begin(), buf->end());
}

void MainWindow::remember_recent(const std::string& path) {
  app_.remember_recent(path);
}

void MainWindow::rebuild_recents_menu() {
  if (!recents_menu_) {
    return;
  }
  for (auto* child : recents_menu_->get_children()) {
    recents_menu_->remove(*child);
  }
  const auto& recents = app_.recents();
  if (recents.empty()) {
    auto* empty = Gtk::manage(new Gtk::MenuItem("(No recent files)"));
    empty->set_sensitive(false);
    recents_menu_->append(*empty);
  } else {
    for (const auto& path : recents) {
      auto* item = Gtk::manage(new Gtk::MenuItem(path));
      item->signal_activate().connect(
          [this, path]() { on_open_recent(path); });
      recents_menu_->append(*item);
    }
  }
  recents_menu_->show_all();
}

void MainWindow::set_encoding(const std::string& encoding) {
  if (encoding_ == encoding) {
    update_status();
    return;
  }
  encoding_ = encoding;
  if (!seeding_) {
    encoding_dirty_ = (encoding_ != saved_encoding_);
    refresh_dirty_from_buffer();
  }
  update_status();
}

void MainWindow::set_open_preference(bool utf8) {
  prefer_utf8_ = utf8;
  open_charset_ = utf8 ? "UTF-8" : "ISO-8859-1";
  app_.set_open_charset(open_charset_, prefer_utf8_);
}

void MainWindow::on_encoding_utf8() {
  if (enc_utf8_item_ && enc_utf8_item_->get_active()) {
    if (seeding_) {
      return;
    }
    set_open_preference(true);
    set_encoding("UTF-8");
  }
}

void MainWindow::on_encoding_latin1() {
  if (enc_latin1_item_ && enc_latin1_item_->get_active()) {
    if (seeding_) {
      return;
    }
    set_open_preference(false);
    set_encoding("ISO-8859-1");
  }
}

Gtk::TextSearchFlags MainWindow::search_flags(const FindOptions& opts) const {
  Gtk::TextSearchFlags flags = Gtk::TEXT_SEARCH_TEXT_ONLY;
  if (!opts.case_sensitive) {
    flags |= Gtk::TEXT_SEARCH_CASE_INSENSITIVE;
  }
  return flags;
}

bool MainWindow::is_entire_word(const Gtk::TextIter& start,
                                const Gtk::TextIter& end) const {
  if (start.editable() && !start.starts_word() && !start.inside_word()) {
    // starts at non-word is ok if previous isn't word char — use starts_word
  }
  auto s = start;
  auto e = end;
  // Word boundary: start is start-of-word or start-of-buffer / non-word before
  bool left_ok = s.starts_word() || s.is_start();
  if (!left_ok) {
    auto prev = s;
    if (prev.backward_char()) {
      gunichar c = prev.get_char();
      left_ok = !g_unichar_isalnum(c) && c != '_' && c != '-';
    } else {
      left_ok = true;
    }
  }
  bool right_ok = e.ends_word() || e.is_end();
  if (!right_ok) {
    gunichar c = e.get_char();
    right_ok = (c == 0) || (!g_unichar_isalnum(c) && c != '_' && c != '-');
  }
  return left_ok && right_ok;
}

void MainWindow::ensure_find_marks() {
  auto buf = text_view_.get_buffer();
  if (!sel_only_start_mark_) {
    sel_only_start_mark_ =
        buf->create_mark("lunduke-sel-only-start", buf->begin(), true);
    sel_only_end_mark_ =
        buf->create_mark("lunduke-sel-only-end", buf->begin(), false);
  }
  if (!extend_anchor_mark_) {
    extend_anchor_mark_ =
        buf->create_mark("lunduke-extend-anchor", buf->begin(), true);
  }
  if (!last_match_start_) {
    last_match_start_ =
        buf->create_mark("lunduke-last-match-start", buf->begin(), true);
    last_match_end_ =
        buf->create_mark("lunduke-last-match-end", buf->begin(), false);
  }
}

void MainWindow::pin_selection_only_range() {
  auto buf = text_view_.get_buffer();
  Gtk::TextIter a, b;
  if (!buf->get_selection_bounds(a, b) || a == b) {
    return;
  }
  ensure_find_marks();
  buf->move_mark(sel_only_start_mark_, a);
  buf->move_mark(sel_only_end_mark_, b);
  sel_only_range_valid_ = true;
}

void MainWindow::clear_selection_only_range() {
  sel_only_range_valid_ = false;
}

void MainWindow::clear_extend_anchor() {
  extend_anchor_valid_ = false;
}

bool MainWindow::selection_matches_needle(const FindOptions& opts,
                                          const Gtk::TextIter& a,
                                          const Gtk::TextIter& b) const {
  Glib::ustring selected = a.get_text(b);
  Glib::ustring needle = opts.search_for;
  if (!opts.case_sensitive) {
    selected = selected.casefold();
    needle = needle.casefold();
  }
  return selected == needle;
}

bool MainWindow::get_search_bounds(const FindOptions& opts, Gtk::TextIter& begin,
                                   Gtk::TextIter& end) {
  auto buf = text_view_.get_buffer();
  if (opts.search_selection_only) {
    // Prefer the range pinned when Find opened / Selection Only engaged so a
    // successful match (which reselection) does not shrink the search scope.
    if (sel_only_range_valid_ && sel_only_start_mark_ && sel_only_end_mark_) {
      begin = buf->get_iter_at_mark(sel_only_start_mark_);
      end = buf->get_iter_at_mark(sel_only_end_mark_);
      if (begin < end) {
        return true;
      }
    }
    Gtk::TextIter sel_a, sel_b;
    if (buf->get_selection_bounds(sel_a, sel_b) && sel_a != sel_b) {
      begin = sel_a;
      end = sel_b;
      ensure_find_marks();
      buf->move_mark(sel_only_start_mark_, sel_a);
      buf->move_mark(sel_only_end_mark_, sel_b);
      sel_only_range_valid_ = true;
      return true;
    }
    // No selection: do not fall through to the whole buffer.
    begin = buf->begin();
    end = begin;
    return false;
  }
  begin = buf->begin();
  end = buf->end();
  return true;
}

bool MainWindow::find_match(const FindOptions& opts, bool from_next) {
  if (opts.search_for.empty()) {
    return false;
  }
  auto buf = text_view_.get_buffer();
  const auto flags = search_flags(opts);

  if (!opts.search_selection_only) {
    clear_selection_only_range();
  }
  if (!opts.extend_selection) {
    clear_extend_anchor();
  }

  Gtk::TextIter range_begin, range_end;
  if (!get_search_bounds(opts, range_begin, range_end)) {
    return false;
  }

  Gtk::TextIter start;
  Gtk::TextIter sel_a, sel_b;
  const bool have_sel =
      buf->get_selection_bounds(sel_a, sel_b) && sel_a != sel_b;
  // Extend continues past an existing selection/anchor so the span can grow.
  // With no selection yet, Start at Top still applies (FR-B01 / first hit).
  // When Extend is off, Start at Top is unchanged (FR-D03).
  const bool extending_existing =
      opts.extend_selection && (have_sel || extend_anchor_valid_);
  const bool honor_start_at_top =
      opts.start_at_top && !from_next && !extending_existing;

  if (honor_start_at_top) {
    // Searching backward from the start of the range finds nothing.
    // "Start at top" in that direction begins at the end of the range.
    start = opts.search_backwards ? range_end : range_begin;
  } else {
    start = buf->get_iter_at_mark(buf->get_insert());

    if (opts.extend_selection && have_sel) {
      // Grow forward from selection end (or backward from selection start).
      start = opts.search_backwards ? sel_a : sel_b;
      if (!extend_anchor_valid_) {
        ensure_find_marks();
        buf->move_mark(extend_anchor_mark_,
                       opts.search_backwards ? sel_b : sel_a);
        extend_anchor_valid_ = true;
      }
    } else {
      if (opts.search_selection_only) {
        if (start < range_begin || start > range_end) {
          start = opts.search_backwards ? range_end : range_begin;
        }
      }
      if (from_next || (!opts.start_at_top) || opts.extend_selection) {
        // Move past the current selection only when it is exactly the needle.
        // Stepping an extra character when it is not makes forward_search
        // miss a match that starts at the cursor (the second "x" in "xx",
        // and Wrap Around then jumps to an earlier hit).
        if (have_sel && selection_matches_needle(opts, sel_a, sel_b)) {
          start = opts.search_backwards ? sel_a : sel_b;
        }
      }
    }

    if (opts.search_selection_only) {
      if (start < range_begin) {
        start = range_begin;
      }
      if (start > range_end) {
        start = range_end;
      }
    }
  }

  auto try_search = [&](const Gtk::TextIter& from, bool wrap_pass) -> bool {
    Gtk::TextIter match_start, match_end;
    Gtk::TextIter cursor = from;
    const int guard = buf->get_char_count() + 2;
    for (int i = 0; i < guard; ++i) {
      bool found = false;
      if (opts.search_backwards) {
        found = cursor.backward_search(opts.search_for, flags, match_start,
                                       match_end, range_begin);
      } else {
        found = cursor.forward_search(opts.search_for, flags, match_start,
                                      match_end, range_end);
      }
      if (!found) {
        return false;
      }
      if (!opts.search_selection_only ||
          (match_start >= range_begin && match_end <= range_end)) {
        if (!opts.entire_word || is_entire_word(match_start, match_end)) {
          if (opts.extend_selection) {
            ensure_find_marks();
            if (!extend_anchor_valid_) {
              // Anchor at the far end opposite the search direction so growth
              // keeps the original hit while adding new matches.
              Gtk::TextIter cur_a, cur_b;
              if (buf->get_selection_bounds(cur_a, cur_b) && cur_a != cur_b) {
                buf->move_mark(extend_anchor_mark_,
                               opts.search_backwards ? cur_b : cur_a);
              } else {
                buf->move_mark(extend_anchor_mark_, match_start);
              }
              extend_anchor_valid_ = true;
            }
            Gtk::TextIter anchor = buf->get_iter_at_mark(extend_anchor_mark_);
            Gtk::TextIter ext_a =
                anchor < match_start ? anchor : match_start;
            Gtk::TextIter ext_b = anchor > match_end ? anchor : match_end;
            Gtk::TextIter cur_a, cur_b;
            if (buf->get_selection_bounds(cur_a, cur_b) && cur_a != cur_b) {
              if (cur_a < ext_a) {
                ext_a = cur_a;
              }
              if (cur_b > ext_b) {
                ext_b = cur_b;
              }
            }
            buf->select_range(ext_a, ext_b);
          } else {
            buf->select_range(match_start, match_end);
          }
          ensure_find_marks();
          buf->move_mark(last_match_start_, match_start);
          buf->move_mark(last_match_end_, match_end);
          last_match_valid_ = true;
          text_view_.scroll_to(match_start);
          update_status();
          return true;
        }
      }
      cursor = opts.search_backwards ? match_start : match_end;
      if (opts.search_backwards) {
        if (!cursor.backward_char()) {
          break;
        }
      }
      (void)wrap_pass;
    }
    return false;
  };

  if (try_search(start, false)) {
    return true;
  }
  if (opts.wrap_around) {
    Gtk::TextIter wrap_from = opts.search_backwards ? range_end : range_begin;
    return try_search(wrap_from, true);
  }
  return false;
}

int MainWindow::count_matches(const FindOptions& opts) {
  if (opts.search_for.empty()) {
    return 0;
  }
  auto buf = text_view_.get_buffer();
  const auto flags = search_flags(opts);
  Gtk::TextIter range_begin, range_end;
  if (!get_search_bounds(opts, range_begin, range_end)) {
    return 0;
  }

  int count = 0;
  Gtk::TextIter cursor = range_begin;
  Gtk::TextIter match_start, match_end;
  while (cursor.forward_search(opts.search_for, flags, match_start, match_end,
                               range_end)) {
    if (!opts.entire_word || is_entire_word(match_start, match_end)) {
      ++count;
    }
    cursor = match_end;
    if (match_start == match_end) {
      if (!cursor.forward_char()) {
        break;
      }
    }
  }
  return count;
}

void MainWindow::clear_find_highlights() {
  auto buf = text_view_.get_buffer();
  if (find_tag_ && buf) {
    buf->remove_tag(find_tag_, buf->begin(), buf->end());
  }
}

void MainWindow::set_find_count(int n, bool capped) {
  if (n < 0) {
    status_find_.set_text("");
    if (status_find_frame_.get_mapped() || status_find_frame_.get_visible()) {
      status_find_frame_.hide();
    }
    return;
  }
  std::string text = std::to_string(n);
  if (capped) {
    text += "+";
  }
  text += (n == 1 && !capped) ? " match" : " matches";
  status_find_.set_text(text);
  status_find_frame_.show();
}

void MainWindow::end_find_user_action() {
  if (!find_scan_.user_action_open) {
    return;
  }
  find_scan_.user_action_open = false;
  if (auto buf = buffer()) {
    buf->end_user_action();
  }
}

void MainWindow::cancel_find_scan() {
  find_scan_.cancel = true;
  find_idle_.disconnect();
  end_find_user_action();
  find_scan_.active = false;
  find_scan_.dlg = nullptr;
  find_scan_.finishing = false;
}

bool MainWindow::confirm_huge_undo(std::size_t bytes) {
  if (const char* choice = g_getenv("LUNDUKE_EDIT_TEST_HUGE_UNDO")) {
    return std::strcmp(choice, "allow") == 0;
  }
  if (g_getenv("LUNDUKE_EDIT_TEST") != nullptr) {
    return true;
  }
  Gtk::MessageDialog dlg(
      find_scan_.dlg ? static_cast<Gtk::Window&>(*find_scan_.dlg)
                     : static_cast<Gtk::Window&>(*this),
      "Replace All would store a very large undo record.", false,
      Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_NONE, true);
  dlg.set_secondary_text(
      "Continuing keeps about " + format_bytes(bytes) +
      " so the replacement can be undone. Continue?");
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Replace", Gtk::RESPONSE_ACCEPT);
  dlg.set_default_response(Gtk::RESPONSE_CANCEL);
  return dlg.run() == Gtk::RESPONSE_ACCEPT;
}

void MainWindow::finish_find_scan(bool show_result) {
  if (find_scan_.finishing) {
    return;
  }
  find_scan_.finishing = true;
  find_idle_.disconnect();
  end_find_user_action();

  auto buf = buffer();
  const bool cancelled = find_scan_.cancel;
  const int count = find_scan_.count;
  const bool capped = find_scan_.capped;
  const auto kind = find_scan_.kind;
  FindReplaceDialog* dlg = find_scan_.dlg;

  if (kind == FindScan::Kind::FindAll && buf && find_scan_.select_start >= 0) {
    auto a = buf->get_iter_at_offset(find_scan_.select_start);
    auto b = buf->get_iter_at_offset(find_scan_.select_end);
    buf->select_range(a, b);
    text_view_.scroll_to(a);
    update_cursor_status();
  }
  if (kind == FindScan::Kind::FindAll) {
    set_find_count(count, capped);
  }

  find_scan_.active = false;
  find_scan_.dlg = nullptr;

  if (show_result && !cancelled && dlg != nullptr &&
      g_getenv("LUNDUKE_EDIT_TEST") == nullptr) {
    if (kind == FindScan::Kind::FindAll) {
      Glib::ustring message =
          "Found " + std::to_string(count) + (count == 1 ? " match." : " matches.");
      if (capped) {
        message = "Highlighted the first " + std::to_string(count) +
                  " matches. The document has more.";
      }
      Gtk::MessageDialog info(*dlg, message, false, Gtk::MESSAGE_INFO,
                              Gtk::BUTTONS_OK, true);
      info.run();
    } else if (kind == FindScan::Kind::ReplaceAll) {
      Gtk::MessageDialog info(
          *dlg,
          "Replaced " + std::to_string(count) +
              (count == 1 ? " occurrence." : " occurrences."),
          false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
      info.run();
    }
  }
  update_undo_redo_sensitivity();
  update_status();
  if (kind == FindScan::Kind::FindAll) {
    // update_status does not touch the match label; put it back if a
    // cursor update ran above. set_find_count is idempotent.
    set_find_count(count, capped);
  }
}

bool MainWindow::step_search(bool backward, int& cursor_off, int& match_start,
                             int& match_end) {
  auto buf = buffer();
  if (!buf || find_scan_.opts.search_for.empty()) {
    return false;
  }
  Gtk::TextIter range_begin, range_end;
  if (!get_search_bounds(find_scan_.opts, range_begin, range_end)) {
    return false;
  }
  const auto flags = search_flags(find_scan_.opts);
  const int guard_chars = buf->get_char_count() + 2;
  Gtk::TextIter cursor = buf->get_iter_at_offset(cursor_off);
  if (cursor < range_begin) {
    cursor = range_begin;
  }
  if (cursor > range_end) {
    cursor = range_end;
  }
  for (int i = 0; i < guard_chars; ++i) {
    Gtk::TextIter ms, me;
    bool found = false;
    if (backward) {
      found = cursor.backward_search(find_scan_.opts.search_for, flags, ms, me,
                                     range_begin);
    } else {
      found = cursor.forward_search(find_scan_.opts.search_for, flags, ms, me,
                                    range_end);
    }
    if (!found) {
      return false;
    }
    const bool in_range = !find_scan_.opts.search_selection_only ||
                          (ms >= range_begin && me <= range_end);
    const bool word_ok =
        !find_scan_.opts.entire_word || is_entire_word(ms, me);
    if (in_range && word_ok) {
      match_start = ms.get_offset();
      match_end = me.get_offset();
      return true;
    }
    if (backward) {
      cursor = ms;
      if (!cursor.backward_char()) {
        return false;
      }
    } else {
      cursor = me;
      if (ms == me && !cursor.forward_char()) {
        return false;
      }
    }
    cursor_off = cursor.get_offset();
  }
  return false;
}

bool MainWindow::pump_find_highlight() {
  if (!find_scan_.active || find_scan_.cancel) {
    finish_find_scan(false);
    return false;
  }
  auto buf = buffer();
  if (!buf) {
    finish_find_scan(false);
    return false;
  }
  Gtk::TextIter range_begin, range_end;
  if (!get_search_bounds(find_scan_.opts, range_begin, range_end)) {
    finish_find_scan(true);
    return false;
  }
  const bool backward = find_scan_.opts.search_backwards;
  if (!find_scan_.started) {
    find_scan_.started = true;
    find_scan_.cursor_off =
        backward ? range_end.get_offset() : range_begin.get_offset();
    clear_find_highlights();
    find_highlights_on_ = false;
    find_scan_.select_start = -1;
  }

  const int cap = max_find_hits();
  const int chunk = find_chunk_size();
  int cursor_off = find_scan_.cursor_off;
  for (int n = 0; n < chunk; ++n) {
    if (find_scan_.count >= cap) {
      int extra_s = 0;
      int extra_e = 0;
      int peek = cursor_off;
      if (step_search(backward, peek, extra_s, extra_e)) {
        find_scan_.capped = true;
      }
      find_scan_.cursor_off = cursor_off;
      finish_find_scan(true);
      return false;
    }
    int ms = 0;
    int me = 0;
    if (!step_search(backward, cursor_off, ms, me)) {
      find_scan_.cursor_off = cursor_off;
      finish_find_scan(true);
      return false;
    }
    if (find_tag_) {
      buf->apply_tag(find_tag_, buf->get_iter_at_offset(ms),
                     buf->get_iter_at_offset(me));
      find_highlights_on_ = true;
    }
    if (find_scan_.select_start < 0) {
      // First hit in search order: the last match when searching backward.
      find_scan_.select_start = ms;
      find_scan_.select_end = me;
    }
    ++find_scan_.count;
    if (backward) {
      // The next backward search must start at this match. Starting one
      // character earlier hides a match that ends at match_start, so
      // adjacent hits such as the letters in "aaaa" are skipped.
      if (ms == me) {
        Gtk::TextIter next = buf->get_iter_at_offset(ms);
        if (!next.backward_char()) {
          find_scan_.cursor_off = next.get_offset();
          finish_find_scan(true);
          return false;
        }
        cursor_off = next.get_offset();
      } else {
        cursor_off = ms;
      }
    } else {
      Gtk::TextIter next = buf->get_iter_at_offset(me);
      if (ms == me && !next.forward_char()) {
        find_scan_.cursor_off = next.get_offset();
        finish_find_scan(true);
        return false;
      }
      cursor_off = next.get_offset();
    }
  }
  find_scan_.cursor_off = cursor_off;
  return true;
}

bool MainWindow::pump_replace() {
  if (!find_scan_.active || find_scan_.cancel) {
    finish_find_scan(false);
    return false;
  }
  auto buf = buffer();
  if (!buf || find_scan_.opts.search_for.empty()) {
    finish_find_scan(true);
    return false;
  }

  const std::size_t per = find_scan_.opts.search_for.bytes() +
                          find_scan_.opts.replace_with.bytes();
  const std::size_t limit = huge_undo_limit();

  if (find_scan_.counting) {
    Gtk::TextIter range_begin, range_end;
    if (!find_scan_.started) {
      if (!get_search_bounds(find_scan_.opts, range_begin, range_end)) {
        finish_find_scan(true);
        return false;
      }
      find_scan_.started = true;
      find_scan_.cursor_off = range_begin.get_offset();
      find_scan_.count = 0;
      find_scan_.undo_bytes = 0;
    }
    const int chunk = find_chunk_size();
    int cursor_off = find_scan_.cursor_off;
    for (int n = 0; n < chunk; ++n) {
      int ms = 0;
      int me = 0;
      if (!step_search(false, cursor_off, ms, me)) {
        const std::size_t estimated = find_scan_.undo_bytes;
        find_scan_.counting = false;
        find_scan_.started = false;
        find_scan_.cursor_off = 0;
        find_scan_.count = 0;
        if (estimated >= limit && limit > 0) {
          find_idle_.disconnect();
          if (!confirm_huge_undo(estimated) || find_scan_.cancel) {
            finish_find_scan(false);
            return false;
          }
          find_idle_ = Glib::signal_idle().connect(
              sigc::mem_fun(*this, &MainWindow::on_find_idle));
          return false;
        }
        return true;
      }
      find_scan_.undo_bytes += per;
      ++find_scan_.count;
      if (find_scan_.undo_bytes >= limit && limit > 0) {
        const std::size_t estimated = find_scan_.undo_bytes;
        find_scan_.counting = false;
        find_scan_.started = false;
        find_scan_.count = 0;
        find_idle_.disconnect();
        if (!confirm_huge_undo(estimated) || find_scan_.cancel) {
          finish_find_scan(false);
          return false;
        }
        find_idle_ = Glib::signal_idle().connect(
            sigc::mem_fun(*this, &MainWindow::on_find_idle));
        return false;
      }
      Gtk::TextIter next = buf->get_iter_at_offset(me);
      if (ms == me && !next.forward_char()) {
        find_scan_.counting = false;
        find_scan_.started = false;
        find_scan_.count = 0;
        return true;
      }
      cursor_off = next.get_offset();
    }
    find_scan_.cursor_off = cursor_off;
    return true;
  }

  if (!find_scan_.user_action_open) {
    buf->begin_user_action();
    find_scan_.user_action_open = true;
    if (!find_scan_.started) {
      Gtk::TextIter range_begin, range_end;
      if (!get_search_bounds(find_scan_.opts, range_begin, range_end)) {
        finish_find_scan(true);
        return false;
      }
      find_scan_.started = true;
      find_scan_.cursor_off = range_begin.get_offset();
      find_scan_.count = 0;
    }
  }

  const int repl_chars = static_cast<int>(g_utf8_strlen(
      find_scan_.opts.replace_with.data(),
      static_cast<gssize>(find_scan_.opts.replace_with.bytes())));
  const int chunk = find_chunk_size();
  int cursor_off = find_scan_.cursor_off;
  for (int n = 0; n < chunk; ++n) {
    int ms = 0;
    int me = 0;
    if (!step_search(false, cursor_off, ms, me)) {
      find_scan_.cursor_off = cursor_off;
      finish_find_scan(true);
      return false;
    }
    buf->erase(buf->get_iter_at_offset(ms), buf->get_iter_at_offset(me));
    buf->insert(buf->get_iter_at_offset(ms), find_scan_.opts.replace_with);
    cursor_off = ms + repl_chars;
    ++find_scan_.count;
  }
  find_scan_.cursor_off = cursor_off;
  return true;
}

bool MainWindow::on_find_idle() {
  if (!find_scan_.active || find_scan_.finishing) {
    return false;
  }
  if (find_scan_.kind == FindScan::Kind::ReplaceAll) {
    return pump_replace();
  }
  return pump_find_highlight();
}

void MainWindow::start_find_all(const FindOptions& opts, FindReplaceDialog* dlg) {
  cancel_find_scan();
  find_scan_ = FindScan{};
  find_scan_.kind = FindScan::Kind::FindAll;
  find_scan_.active = true;
  find_scan_.opts = opts;
  find_scan_.dlg = dlg;
  if (opts.search_for.empty()) {
    finish_find_scan(true);
    return;
  }
  find_idle_ = Glib::signal_idle().connect(
      sigc::mem_fun(*this, &MainWindow::on_find_idle));
}

void MainWindow::start_replace_all(const FindOptions& opts,
                                   FindReplaceDialog* dlg) {
  cancel_find_scan();
  find_scan_ = FindScan{};
  find_scan_.kind = FindScan::Kind::ReplaceAll;
  find_scan_.active = true;
  find_scan_.opts = opts;
  find_scan_.dlg = dlg;
  if (opts.search_for.empty()) {
    finish_find_scan(true);
    return;
  }
  const std::size_t per =
      opts.search_for.bytes() + opts.replace_with.bytes();
  const std::size_t limit = huge_undo_limit();
  const std::size_t needle = std::max<std::size_t>(opts.search_for.bytes(), 1);
  const std::size_t max_matches = utf8_bytes_ / needle + 1;
  const bool maybe_huge =
      limit > 0 && per > 0 && max_matches > limit / per;
  find_scan_.counting = maybe_huge;
  find_idle_ = Glib::signal_idle().connect(
      sigc::mem_fun(*this, &MainWindow::on_find_idle));
}

void MainWindow::highlight_all_matches(const FindOptions& opts) {
  cancel_find_scan();
  find_scan_ = FindScan{};
  find_scan_.kind = FindScan::Kind::FindAll;
  find_scan_.active = true;
  find_scan_.opts = opts;
  if (opts.search_for.empty()) {
    finish_find_scan(false);
    return;
  }
  while (find_scan_.active && pump_find_highlight()) {
  }
}

MainWindow::ReplaceResult MainWindow::replace_current(const FindOptions& opts) {
  auto buf = text_view_.get_buffer();
  auto matches_needle = [&](const Gtk::TextIter& a, const Gtk::TextIter& b) {
    return a != b && selection_matches_needle(opts, a, b) &&
           (!opts.entire_word || is_entire_word(a, b));
  };
  auto replace_span = [&](int start_off, int end_off) {
    buf->begin_user_action();
    auto a = buf->get_iter_at_offset(start_off);
    auto b = buf->get_iter_at_offset(end_off);
    buf->erase(a, b);
    a = buf->get_iter_at_offset(start_off);
    buf->insert(a, opts.replace_with);
    buf->end_user_action();
    last_match_valid_ = false;
    update_undo_redo_sensitivity();
    find_match(opts, true);
  };

  Gtk::TextIter sel_a, sel_b;
  const bool have_sel =
      buf->get_selection_bounds(sel_a, sel_b) && sel_a != sel_b;
  if (have_sel && matches_needle(sel_a, sel_b)) {
    replace_span(sel_a.get_offset(), sel_b.get_offset());
    return ReplaceResult::Replaced;
  }

  // Extend Selection grows the selection past the needle. Replace the match
  // that was found, not the grown span, and do not search again (that would
  // grow the selection further without replacing).
  if (last_match_valid_ && last_match_start_ && last_match_end_) {
    auto a = buf->get_iter_at_mark(last_match_start_);
    auto b = buf->get_iter_at_mark(last_match_end_);
    if (matches_needle(a, b)) {
      replace_span(a.get_offset(), b.get_offset());
      return ReplaceResult::Replaced;
    }
  }

  if (opts.extend_selection && have_sel && !matches_needle(sel_a, sel_b)) {
    return ReplaceResult::Blocked;
  }

  if (find_match(opts, false)) {
    return ReplaceResult::Found;
  }
  return ReplaceResult::NotFound;
}

int MainWindow::replace_all(const FindOptions& opts) {
  if (opts.search_for.empty()) {
    return 0;
  }
  auto buf = text_view_.get_buffer();
  const auto flags = search_flags(opts);
  Gtk::TextIter range_begin, range_end;
  if (!get_search_bounds(opts, range_begin, range_end)) {
    return 0;
  }

  // Collect offsets first so indices stay valid while we edit from end.
  struct Hit {
    int start_off;
    int end_off;
  };
  std::vector<Hit> hits;
  Gtk::TextIter cursor = range_begin;
  Gtk::TextIter match_start, match_end;
  while (cursor.forward_search(opts.search_for, flags, match_start, match_end,
                               range_end)) {
    if (!opts.entire_word || is_entire_word(match_start, match_end)) {
      hits.push_back({match_start.get_offset(), match_end.get_offset()});
    }
    cursor = match_end;
    if (match_start == match_end) {
      if (!cursor.forward_char()) {
        break;
      }
    }
  }

  buf->begin_user_action();
  for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
    auto a = buf->get_iter_at_offset(it->start_off);
    auto b = buf->get_iter_at_offset(it->end_off);
    buf->erase(a, b);
    a = buf->get_iter_at_offset(it->start_off);
    buf->insert(a, opts.replace_with);
  }
  buf->end_user_action();
  clear_find_highlights();
  update_undo_redo_sensitivity();
  update_status();
  return static_cast<int>(hits.size());
}

void MainWindow::on_find() {
  // Capture selection before the dialog takes focus so Search Selection Only
  // still has the user range after Find reselection. Clear the pin when Find
  // opens with no selection so a stale range is not reused.
  {
    auto buf = text_view_.get_buffer();
    Gtk::TextIter a, b;
    if (buf->get_selection_bounds(a, b) && a != b) {
      pin_selection_only_range();
    } else {
      clear_selection_only_range();
    }
  }
  clear_extend_anchor();

  FindReplaceDialog dlg(*this, find_opts_);
  dlg.signal_hide().connect([this]() { cancel_find_scan(); });
  dlg.on_action = [this, &dlg](FindReplaceDialog::Action action,
                               const FindOptions& opts) -> bool {
    find_opts_ = opts;
    if (!opts.search_selection_only) {
      clear_selection_only_range();
    } else if (!sel_only_range_valid_) {
      pin_selection_only_range();
    }
    if (!opts.extend_selection) {
      clear_extend_anchor();
    }
    auto no_selection = [&dlg]() {
      Gtk::MessageDialog miss(dlg, "No text is selected.", false,
                              Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
      miss.set_secondary_text(
          "Search Selection Only needs a selection.");
      miss.run();
    };
    auto bounds_ok = [this, &opts]() {
      Gtk::TextIter begin, end;
      return get_search_bounds(opts, begin, end);
    };

    switch (action) {
      case FindReplaceDialog::Action::Find: {
        if (!bounds_ok()) {
          no_selection();
          return false;
        }
        FindOptions o = opts;
        const bool found = find_match(o, false);
        // Start at Top applies to this search only. The next Find in the
        // dialog continues from the cursor. F3 still forces the flag off.
        if (opts.start_at_top) {
          dlg.clear_start_at_top();
          find_opts_.start_at_top = false;
        }
        if (!found) {
          Gtk::MessageDialog miss(dlg, "Text not found.", false,
                                  Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
          miss.set_secondary_text(opts.search_for);
          miss.run();
          return false;
        }
        return true;
      }
      case FindReplaceDialog::Action::FindAll: {
        if (!bounds_ok()) {
          no_selection();
          return false;
        }
        // One capped scan, a chunk per idle, so Cancel stays responsive.
        // The selected hit follows Search Backwards.
        start_find_all(opts, &dlg);
        return true;
      }
      case FindReplaceDialog::Action::Replace: {
        if (!bounds_ok()) {
          no_selection();
          return false;
        }
        const ReplaceResult result = replace_current(opts);
        if (result == ReplaceResult::Blocked) {
          Gtk::MessageDialog miss(dlg, "Cannot replace this selection.", false,
                                  Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
          miss.set_secondary_text(
              "The selection extends past the match, and that match could "
              "not be replaced.");
          miss.run();
          return false;
        }
        if (result == ReplaceResult::NotFound) {
          Gtk::MessageDialog miss(dlg, "Text not found.", false,
                                  Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
          miss.set_secondary_text(opts.search_for);
          miss.run();
          return false;
        }
        return true;
      }
      case FindReplaceDialog::Action::ReplaceAll: {
        if (!bounds_ok()) {
          no_selection();
          return false;
        }
        // Whole-range replacement. Chunked so Cancel works, with a warning
        // when the grouped undo record would be huge.
        start_replace_all(opts, &dlg);
        return true;
      }
      default:
        return false;
    }
  };

  dlg.run();
  find_opts_ = dlg.options();
  // After first successful find, clear start_at_top for Find Next.
  find_opts_.start_at_top = false;
}

void MainWindow::on_find_next() {
  if (find_opts_.search_for.empty()) {
    on_find();
    return;
  }
  FindOptions o = find_opts_;
  o.start_at_top = false;
  if (!find_match(o, true)) {
    Gtk::MessageDialog miss(*this, "Text not found.", false, Gtk::MESSAGE_INFO,
                            Gtk::BUTTONS_OK, true);
    miss.set_secondary_text(find_opts_.search_for);
    miss.run();
  }
}

void MainWindow::on_go_to_line() {
  Gtk::Dialog dlg("Go to Line", *this, true);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Go", Gtk::RESPONSE_OK);
  dlg.set_default_response(Gtk::RESPONSE_OK);

  auto* box = dlg.get_content_area();
  box->set_spacing(8);
  box->set_border_width(10);
  auto* label = Gtk::manage(new Gtk::Label("Line number:", true));
  label->set_halign(Gtk::ALIGN_START);
  auto* entry = Gtk::manage(new Gtk::Entry());
  entry->set_activates_default(true);
  auto buf = text_view_.get_buffer();
  auto iter = buf->get_iter_at_mark(buf->get_insert());
  entry->set_text(std::to_string(iter.get_line() + 1));
  box->pack_start(*label, Gtk::PACK_SHRINK);
  box->pack_start(*entry, Gtk::PACK_SHRINK);
  dlg.show_all();

  if (dlg.run() == Gtk::RESPONSE_OK) {
    int line = 0;
    if (!parse_go_to_line(entry->get_text().raw(), line)) {
      Gtk::MessageDialog bad(dlg, "Invalid line number.", false,
                             Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
      bad.run();
    } else {
      if (line < 1) {
        line = 1;
      }
      const int max_line = buf->get_line_count();
      if (line > max_line) {
        line = max_line;
      }
      auto dest = buf->get_iter_at_line(line - 1);
      buf->place_cursor(dest);
      text_view_.scroll_to(dest);
      update_status();
    }
  }
}

void MainWindow::on_toggle_wrap() {
  if (!wrap_item_) {
    return;
  }
  text_view_.set_wrap_mode(wrap_item_->get_active() ? Gtk::WRAP_WORD_CHAR
                                                    : Gtk::WRAP_NONE);
  if (!suppress_wrap_pref_) {
    app_.set_wrap_text(wrap_item_->get_active());
  }
  if (gutter_) {
    gutter_->queue_draw();
  }
}

void MainWindow::on_toggle_line_numbers() {
  if (!gutter_ || !line_numbers_item_) {
    return;
  }
  gutter_->set_visible_gutter(line_numbers_item_->get_active());
}

void MainWindow::apply_tab_width(int spaces) {
  tab_width_ = spaces;
  app_.set_tab_width(spaces);
  auto layout = text_view_.create_pango_layout(std::string(spaces, ' '));
  layout->set_font_description(font_desc_);
  int tw = 0, th = 0;
  layout->get_pixel_size(tw, th);
  Pango::TabArray tabs(1, true);
  tabs.set_tab(0, Pango::TAB_LEFT, tw);
  text_view_.set_tabs(tabs);
  text_view_.set_tab_width(static_cast<guint>(spaces));
  (void)th;
}

void MainWindow::on_tab_width() {
  Gtk::Dialog dlg("Tab Width", *this, true);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_OK", Gtk::RESPONSE_OK);
  dlg.set_default_response(Gtk::RESPONSE_OK);

  auto* box = dlg.get_content_area();
  box->set_spacing(6);
  box->set_border_width(10);
  auto* label = Gtk::manage(new Gtk::Label("Spaces per tab:"));
  label->set_halign(Gtk::ALIGN_START);
  box->pack_start(*label, Gtk::PACK_SHRINK);

  Gtk::RadioButton::Group group;
  auto* r2 = Gtk::manage(new Gtk::RadioButton(group, "2"));
  auto* r4 = Gtk::manage(new Gtk::RadioButton(group, "4"));
  auto* r8 = Gtk::manage(new Gtk::RadioButton(group, "8"));
  if (tab_width_ == 2) {
    r2->set_active(true);
  } else if (tab_width_ == 8) {
    r8->set_active(true);
  } else {
    r4->set_active(true);
  }
  box->pack_start(*r2, Gtk::PACK_SHRINK);
  box->pack_start(*r4, Gtk::PACK_SHRINK);
  box->pack_start(*r8, Gtk::PACK_SHRINK);
  dlg.show_all();

  if (dlg.run() == Gtk::RESPONSE_OK) {
    int w = 4;
    if (r2->get_active()) {
      w = 2;
    } else if (r8->get_active()) {
      w = 8;
    }
    apply_tab_width(w);
  }
}

void MainWindow::apply_font(const Pango::FontDescription& desc) {
  font_desc_ = desc;
  app_.set_font(desc.to_string());
  text_view_.override_font(desc);
  apply_tab_width(tab_width_);
  if (gutter_) {
    gutter_->refresh();
  }
}

void MainWindow::on_font() {
  Gtk::FontChooserDialog dlg("Font", *this);
  dlg.set_font_desc(font_desc_);
  if (dlg.run() == Gtk::RESPONSE_OK) {
    apply_font(dlg.get_font_desc());
  }
}


void MainWindow::on_page_setup() {
  if (!print_settings_) {
    print_settings_ = Gtk::PrintSettings::create();
  }
  if (!page_setup_) {
    page_setup_ = Gtk::PageSetup::create();
  }
  page_setup_ =
      Gtk::run_page_setup_dialog(*this, page_setup_, print_settings_);
}

void MainWindow::configure_print_layout(
    const Glib::RefPtr<Pango::Layout>& layout, int width_pango) const {
  layout->set_font_description(font_desc_);
  layout->set_width(width_pango);
  layout->set_wrap(Pango::WRAP_WORD_CHAR);
  Pango::TabArray tabs = text_view_.get_tabs();
  if (tabs.get_size() > 0) {
    layout->set_tabs(tabs);
  }
}

int MainWindow::next_print_end(int offset) {
  auto buf = text_view_.get_buffer();
  if (!buf) {
    return offset;
  }
  const int total = buf->get_char_count();
  if (offset >= total) {
    return total;
  }
  auto iter = buf->get_iter_at_offset(offset);
  auto line_end = iter;
  if (!line_end.ends_line()) {
    line_end.forward_to_line_end();
  }
  int end = line_end.get_offset();
  if (end - offset > kPrintChunkChars) {
    end = offset + kPrintChunkChars;
  } else if (end < total) {
    auto nl = buf->get_iter_at_offset(end);
    const gunichar ch = nl.get_char();
    if (ch == '\n' || ch == '\r') {
      ++end;
    }
  }
  if (end <= offset) {
    end = std::min(total, offset + 1);
  }
  return end;
}

double MainWindow::measure_print_chunk(
    const Glib::RefPtr<Gtk::PrintContext>& context, int width_pango, int start,
    int end, double min_height) {
  auto buf = text_view_.get_buffer();
  if (!buf || end <= start) {
    return min_height;
  }
  const Glib::ustring text = buf->get_iter_at_offset(start).get_text(
      buf->get_iter_at_offset(end));
  auto layout = context->create_pango_layout();
  configure_print_layout(layout, width_pango);
  layout->set_text(text);
  double height = 0.0;
  const int n_lines = layout->get_line_count();
  for (int i = 0; i < n_lines; ++i) {
    auto line = layout->get_line(i);
    Pango::Rectangle ink, logical;
    line->get_extents(ink, logical);
    height += static_cast<double>(logical.get_height()) / Pango::SCALE;
  }
  if (height < 1.0) {
    return min_height;
  }
  return height;
}

void MainWindow::draw_print_layout(const Cairo::RefPtr<Cairo::Context>& cr,
                                   const Glib::RefPtr<Pango::Layout>& layout,
                                   double& y) const {
  // y is the top of the next line box. show_in_cairo_context draws on the
  // baseline; logical.y is that baseline relative to the top (usually
  // negative). Starting at y=0 clips the ascent on every page.
  const int n_lines = layout->get_line_count();
  for (int i = 0; i < n_lines; ++i) {
    auto line = layout->get_line(i);
    Pango::Rectangle ink, logical;
    line->get_extents(ink, logical);
    const double line_height =
        static_cast<double>(logical.get_height()) / Pango::SCALE;
    const double baseline =
        y - static_cast<double>(logical.get_y()) / Pango::SCALE;
    cr->move_to(static_cast<double>(logical.get_x()) / Pango::SCALE, baseline);
    line->show_in_cairo_context(cr);
    y += line_height;
  }
}

void MainWindow::on_begin_print(
    const Glib::RefPtr<Gtk::PrintContext>& context) {
  print_page_breaks_.clear();
  auto buf = buffer();
  if (!buf) {
    return;
  }
  const int total = buf->get_char_count();
  const double page_height = context->get_height();
  const int width =
      static_cast<int>(std::floor(context->get_width() * Pango::SCALE));

  double min_height = 12.0;
  {
    auto sample = context->create_pango_layout();
    configure_print_layout(sample, width);
    sample->set_text("Ag");
    Pango::Rectangle ink, logical;
    sample->get_extents(ink, logical);
    const double h = static_cast<double>(logical.get_height()) / Pango::SCALE;
    if (h > 1.0) {
      min_height = h;
    }
  }

  int offset = 0;
  double used = 0.0;
  bool page_empty = true;
  while (offset < total) {
    const int chunk_end = next_print_end(offset);
    const double height =
        measure_print_chunk(context, width, offset, chunk_end, min_height);
    if (!page_empty && used + height > page_height) {
      print_page_breaks_.push_back(offset);
      used = 0.0;
      page_empty = true;
    }
    used += height;
    page_empty = false;
    if (chunk_end <= offset) {
      break;
    }
    offset = chunk_end;
  }
}

void MainWindow::on_draw_page(const Glib::RefPtr<Gtk::PrintContext>& context,
                              int page_nr) {
  auto buf = buffer();
  if (!buf) {
    return;
  }
  const int total = buf->get_char_count();
  int start = 0;
  if (page_nr > 0 &&
      static_cast<std::size_t>(page_nr - 1) < print_page_breaks_.size()) {
    start = print_page_breaks_[static_cast<std::size_t>(page_nr - 1)];
  }
  int end = total;
  if (static_cast<std::size_t>(page_nr) < print_page_breaks_.size()) {
    end = print_page_breaks_[static_cast<std::size_t>(page_nr)];
  }
  if (end < start) {
    end = start;
  }

  auto cr = context->get_cairo_context();
  cr->set_source_rgb(0.0, 0.0, 0.0);
  const int width =
      static_cast<int>(std::floor(context->get_width() * Pango::SCALE));
  double y = 0.0;
  int offset = start;
  while (offset < end) {
    int chunk_end = next_print_end(offset);
    if (chunk_end > end) {
      chunk_end = end;
    }
    if (chunk_end <= offset) {
      break;
    }
    const Glib::ustring text = buf->get_iter_at_offset(offset).get_text(
        buf->get_iter_at_offset(chunk_end));
    auto layout = context->create_pango_layout();
    configure_print_layout(layout, width);
    layout->set_text(text);
    draw_print_layout(cr, layout, y);
    offset = chunk_end;
  }
}

void MainWindow::on_print() {
  if (!print_settings_) {
    print_settings_ = Gtk::PrintSettings::create();
  }
  if (!page_setup_) {
    page_setup_ = Gtk::PageSetup::create();
  }

  auto op = Gtk::PrintOperation::create();
  op->set_print_settings(print_settings_);
  op->set_default_page_setup(page_setup_);
  op->set_embed_page_setup(true);
  op->set_unit(Gtk::UNIT_POINTS);
  op->set_job_name(current_basename());
  op->set_allow_async(false);

  op->signal_begin_print().connect(
      [this, op](const Glib::RefPtr<Gtk::PrintContext>& context) {
        on_begin_print(context);
        const int n_pages = static_cast<int>(print_page_breaks_.size()) + 1;
        op->set_n_pages(std::max(1, n_pages));
      });
  op->signal_draw_page().connect(
      sigc::mem_fun(*this, &MainWindow::on_draw_page));

  try {
    const auto result =
        op->run(Gtk::PRINT_OPERATION_ACTION_PRINT_DIALOG, *this);
    if (result == Gtk::PRINT_OPERATION_RESULT_APPLY) {
      print_settings_ = op->get_print_settings();
    }
  } catch (const Gtk::PrintError& error) {
    Gtk::MessageDialog err(*this, "Could not print.", false,
                           Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
    err.set_secondary_text(error.what());
    err.run();
  }

  print_page_breaks_.clear();
}

void MainWindow::on_about() {
  Gtk::AboutDialog dlg;
  dlg.set_transient_for(*this);
  dlg.set_program_name("Lunduke Edit");
  dlg.set_version(kVersion);
  dlg.set_copyright("© 2026 The Lunduke Journal");
  dlg.set_website("https://lunduke.com");
  dlg.set_website_label("lunduke.com");
  dlg.set_license_type(Gtk::LICENSE_GPL_3_0);
  dlg.set_comments(
      "A light text editor for the Lunduke Computer Operating System.");
  dlg.set_logo_icon_name(resolve_app_icon_name());
  std::vector<Glib::ustring> authors{"The Lunduke Journal"};
  dlg.set_authors(authors);
  dlg.run();
}

}  // namespace lundukeedit
