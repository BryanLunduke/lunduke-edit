// SPDX-License-Identifier: GPL-3.0-or-later

#include "application.hpp"
#include "main_window.hpp"

#include <gdkmm/screen.h>
#include <glib.h>
#include <glibmm/convert.h>
#include <glibmm/fileutils.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/recentmanager.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/window.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace lundukeedit {
namespace {

constexpr const char* kAppId = "org.lunduke.LundukeEdit";

std::string recents_file_path() {
  const std::string dir =
      Glib::build_filename(Glib::get_user_config_dir(), "lunduke-edit");
  g_mkdir_with_parents(dir.c_str(), 0700);
  return Glib::build_filename(dir, "recents.txt");
}

std::string read_fd_all(int fd) {
  if (lseek(fd, 0, SEEK_SET) < 0) {
    return {};
  }
  std::string data;
  char buf[4096];
  while (true) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n == 0) {
      break;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    data.append(buf, static_cast<std::size_t>(n));
  }
  return data;
}

bool write_fd_all(int fd, const std::string& data) {
  const char* p = data.data();
  std::size_t left = data.size();
  while (left > 0) {
    const ssize_t n = ::write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  return true;
}

std::vector<std::string> split_recent_lines(const std::string& data) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < data.size()) {
    const std::size_t nl = data.find('\n', start);
    const std::size_t end = (nl == std::string::npos) ? data.size() : nl;
    if (end > start) {
      std::string line = data.substr(start, end - start);
      if (line.find('\r') == std::string::npos) {
        lines.push_back(std::move(line));
      }
    }
    if (nl == std::string::npos) {
      break;
    }
    start = nl + 1;
  }
  return lines;
}

}  // namespace

Glib::RefPtr<Application> Application::create() {
  return Glib::RefPtr<Application>(new Application());
}

Application::Application()
    : Gtk::Application("org.lunduke.LundukeEdit",
                       Gio::APPLICATION_HANDLES_OPEN) {}

Application::~Application() {
  destroying_ = true;
  for (auto& entry : pending_delete_) {
    entry.second.disconnect();
  }
  pending_delete_.clear();
  const std::vector<MainWindow*> windows(live_.begin(), live_.end());
  live_.clear();
  focused_ = nullptr;
  for (auto* window : windows) {
    delete window;
  }
}

void Application::install_css() {
  static bool installed = false;
  if (installed) {
    return;
  }
  auto screen = Gdk::Screen::get_default();
  if (!screen) {
    return;
  }
  auto css = Gtk::CssProvider::create();
  css->load_from_data(
      "textview text {"
      "  background-color: #f7f4e8;"
      "  color: #1a1a1a;"
      "}"
      "textview {"
      "  background-color: #f7f4e8;"
      "}");
  Gtk::StyleContext::add_provider_for_screen(
      screen, css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  installed = true;
}

void Application::on_startup() {
  Gtk::Application::on_startup();
  // WM / title-bar icon (xfwm4 etc.): desktop Icon= alone is not enough.
  Gtk::Window::set_default_icon_name(kAppId);
  // One screen-wide provider. Windows must not add another.
  install_css();
  ensure_recents_loaded();
}

void Application::set_font(const std::string& desc) {
  if (!desc.empty()) {
    font_ = desc;
  }
}

void Application::set_wrap_text(bool on) { wrap_text_ = on; }

void Application::set_tab_width(int spaces) {
  if (spaces > 0) {
    tab_width_ = spaces;
  }
}

void Application::set_open_charset(const std::string& charset,
                                   bool prefer_utf8) {
  open_charset_ = charset.empty() ? "UTF-8" : charset;
  prefer_utf8_ = prefer_utf8;
}

void Application::note_window_focus(MainWindow* window) {
  if (window && live_.count(window) != 0) {
    focused_ = window;
  }
}

MainWindow* Application::main_window() const {
  if (focused_ && focused_->get_visible()) {
    return focused_;
  }
  for (auto* other : get_windows()) {
    auto* mw = dynamic_cast<MainWindow*>(const_cast<Gtk::Window*>(other));
    if (mw && mw->get_visible()) {
      return mw;
    }
  }
  return nullptr;
}

MainWindow* Application::create_window() {
  auto* w = new MainWindow(*this);
  live_.insert(w);
  add_window(*w);
  w->signal_hide().connect([this, w]() { on_window_hidden(w); });
  return w;
}

void Application::on_window_hidden(MainWindow* window) {
  if (focused_ == window) {
    focused_ = nullptr;
  }
  if (destroying_ || live_.count(window) == 0 ||
      pending_delete_.count(window) != 0) {
    return;
  }
  // Deleting inside the hide signal frees the window while GTK is still
  // delivering the event. Idle runs after the close has finished.
  pending_delete_[window] = Glib::signal_idle().connect([this, window]() {
    pending_delete_.erase(window);
    if (live_.erase(window) != 0) {
      delete window;
    }
    return false;
  });
}

void Application::destroy_window_now(MainWindow* window) {
  if (!window || live_.count(window) == 0) {
    return;
  }
  const auto pending = pending_delete_.find(window);
  if (pending != pending_delete_.end()) {
    pending->second.disconnect();
    pending_delete_.erase(pending);
  }
  live_.erase(window);
  if (focused_ == window) {
    focused_ = nullptr;
  }
  delete window;
}

void Application::on_activate() {
  MainWindow* w = nullptr;
  if (focused_ && focused_->get_visible()) {
    w = focused_;
  } else {
    for (auto* other : get_windows()) {
      auto* mw = dynamic_cast<MainWindow*>(other);
      if (mw && mw->get_visible()) {
        w = mw;
        break;
      }
    }
  }
  if (!w) {
    w = create_window();
    w->load_seed_sample();
  }
  w->present();
}

MainWindow* Application::find_window_editing(const std::string& path) const {
  if (path.empty()) {
    return nullptr;
  }
  for (auto* window : live_) {
    if (window && window->get_visible() && window->edits_path(path)) {
      return window;
    }
  }
  return nullptr;
}

void Application::push_reentry() { ++reentry_depth_; }

void Application::pop_reentry() {
  if (reentry_depth_ > 0) {
    --reentry_depth_;
  }
  if (reentry_depth_ == 0) {
    drain_deferred_opens();
  }
}

void Application::defer_open(const std::string& path) {
  if (!path.empty()) {
    deferred_opens_.push_back(path);
  }
}

void Application::drain_deferred_opens() {
  while (!deferred_opens_.empty() && reentry_depth_ == 0) {
    std::vector<std::string> batch;
    batch.swap(deferred_opens_);
    open_files(batch);
  }
}

void Application::open_files(const std::vector<std::string>& paths) {
  if (paths.empty()) {
    on_activate();
    return;
  }
  // A dialog or clipboard wait is already iterating the main context.
  // Opening now would set_text under that dialog. Hold the paths until
  // the nested loop returns.
  if (reentry_depth_ > 0) {
    deferred_opens_.insert(deferred_opens_.end(), paths.begin(), paths.end());
    return;
  }

  ++reentry_depth_;

  std::size_t index = 0;
  if (MainWindow* existing = find_window_editing(paths[0])) {
    existing->present();
    index = 1;
  } else if (focused_ && focused_->get_visible() &&
             focused_->is_empty_untitled()) {
    // Reuse only the focused window, and only while it is still a blank
    // untitled document. Any other document stays where it is.
    if (focused_->open_file(paths[0])) {
      focused_->present();
    }
    index = 1;
  }

  for (; index < paths.size(); ++index) {
    if (MainWindow* existing = find_window_editing(paths[index])) {
      existing->present();
      continue;
    }
    auto* w = create_window();
    if (!w->open_file(paths[index])) {
      destroy_window_now(w);
      continue;
    }
    w->present();
  }

  --reentry_depth_;
  if (reentry_depth_ == 0) {
    drain_deferred_opens();
  }
}

void Application::report_non_native(const std::vector<Glib::ustring>& uris) {
  if (g_getenv("LUNDUKE_EDIT_TEST") != nullptr) {
    return;
  }
  Glib::ustring secondary = "Only local files can be opened.";
  if (!uris.empty()) {
    secondary += "\n\n";
    secondary += uris.front();
  }
  Gtk::MessageDialog dlg("Cannot open this location.", false,
                         Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
  dlg.set_secondary_text(secondary);
  if (MainWindow* w = main_window()) {
    dlg.set_transient_for(*w);
  }
  push_reentry();
  dlg.run();
  pop_reentry();
}

void Application::on_open(const Gio::Application::type_vec_files& files,
                          const Glib::ustring& /*hint*/) {
  std::vector<std::string> paths;
  std::vector<Glib::ustring> rejected;
  paths.reserve(files.size());
  for (const auto& file : files) {
    if (!file) {
      continue;
    }
    // Remote and virtual locations have no local path. Reject them before
    // any window is created for them.
    if (!file->is_native()) {
      rejected.push_back(file->get_uri());
      continue;
    }
    std::string path = file->get_path();
    if (path.empty()) {
      rejected.push_back(file->get_uri());
      continue;
    }
    paths.push_back(std::move(path));
  }
  if (!rejected.empty()) {
    report_non_native(rejected);
  }
  if (!paths.empty()) {
    open_files(paths);
  }
}

bool Application::confirm_quit() {
  std::vector<MainWindow*> mains;
  for (auto* w : get_windows()) {
    if (auto* mw = dynamic_cast<MainWindow*>(w)) {
      mains.push_back(mw);
    }
  }
  for (auto* mw : mains) {
    if (!mw->get_visible()) {
      mw->present();
    }
    if (!mw->confirm_discard_or_save()) {
      return false;
    }
  }
  return true;
}

void Application::ensure_recents_loaded() {
  if (recents_loaded_) {
    return;
  }
  recents_loaded_ = true;
  const int fd = ::open(recents_file_path().c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  if (flock(fd, LOCK_SH) != 0) {
    ::close(fd);
    return;
  }
  const std::string data = read_fd_all(fd);
  flock(fd, LOCK_UN);
  ::close(fd);
  recents_ = split_recent_lines(data);
  if (static_cast<int>(recents_.size()) > kMaxRecents) {
    recents_.resize(static_cast<std::size_t>(kMaxRecents));
  }
}

const std::vector<std::string>& Application::recents() {
  ensure_recents_loaded();
  return recents_;
}

void Application::remember_recent(const std::string& path) {
  // A newline would split into two menu entries. Reject it rather than
  // escaping; the config file is one path per line.
  if (path.empty() || path.find('\n') != std::string::npos ||
      path.find('\r') != std::string::npos) {
    return;
  }
  try {
    const std::string uri = Glib::filename_to_uri(path);
    Gtk::RecentManager::get_default()->add_item(uri);
  } catch (...) {
  }

  const std::string file = recents_file_path();
  const int fd =
      ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd >= 0) {
    if (flock(fd, LOCK_EX) != 0) {
      // A failed lock must not read-modify-write recents.txt.
      ::close(fd);
      return;
    }
    auto list = split_recent_lines(read_fd_all(fd));
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if (static_cast<int>(list.size()) > kMaxRecents) {
      list.resize(static_cast<std::size_t>(kMaxRecents));
    }
    recents_ = std::move(list);
    recents_loaded_ = true;

    std::string out;
    for (const auto& entry : recents_) {
      out.append(entry);
      out.push_back('\n');
    }
    bool wrote = false;
    if (lseek(fd, 0, SEEK_SET) >= 0 && write_fd_all(fd, out) &&
        ::ftruncate(fd, static_cast<off_t>(out.size())) == 0) {
      ::fsync(fd);
      wrote = true;
    }
    flock(fd, LOCK_UN);
    ::close(fd);
    (void)wrote;
  } else {
    recents_.erase(std::remove(recents_.begin(), recents_.end(), path),
                   recents_.end());
    recents_.insert(recents_.begin(), path);
    if (static_cast<int>(recents_.size()) > kMaxRecents) {
      recents_.resize(static_cast<std::size_t>(kMaxRecents));
    }
    recents_loaded_ = true;
  }

  for (auto* w : get_windows()) {
    if (auto* mw = dynamic_cast<MainWindow*>(w)) {
      mw->rebuild_recents_menu();
    }
  }
}

}  // namespace lundukeedit
