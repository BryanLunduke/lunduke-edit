// SPDX-License-Identifier: GPL-3.0-or-later

#include "application.hpp"
#include "main_window.hpp"

#include <gtkmm/window.h>

namespace lundukeedit {
namespace {

constexpr const char* kAppId = "org.lunduke.LundukeEdit";

}  // namespace

Glib::RefPtr<Application> Application::create() {
  return Glib::RefPtr<Application>(new Application());
}

Application::Application()
    : Gtk::Application("org.lunduke.LundukeEdit",
                       Gio::APPLICATION_HANDLES_OPEN) {}

void Application::on_startup() {
  Gtk::Application::on_startup();
  // WM / title-bar icon (xfwm4 etc.): desktop Icon= alone is not enough.
  Gtk::Window::set_default_icon_name(kAppId);
}

MainWindow* Application::create_window() {
  auto* w = new MainWindow(*this);
  add_window(*w);
  w->signal_hide().connect([this, w]() { forget_window(w); });
  window_ = w;
  return w;
}

void Application::forget_window(MainWindow* window) {
  if (window_ != window) {
    return;
  }
  window_ = nullptr;
  for (auto* other : get_windows()) {
    if (other == window || !other->get_visible()) {
      continue;
    }
    window_ = dynamic_cast<MainWindow*>(other);
    if (window_) {
      break;
    }
  }
}

bool Application::ensure_window() {
  if (window_ && window_->get_visible()) {
    return false;
  }
  if (!window_) {
    for (auto* other : get_windows()) {
      auto* mw = dynamic_cast<MainWindow*>(other);
      if (mw && mw->get_visible()) {
        window_ = mw;
        return false;
      }
    }
  }
  create_window();
  return true;
}

void Application::on_activate() {
  const bool fresh = ensure_window();
  if (fresh) {
    window_->load_seed_sample();
  }
  window_->present();
}

void Application::open_files(const std::vector<std::string>& paths) {
  if (paths.empty()) {
    on_activate();
    return;
  }

  std::size_t index = 0;
  // Reuse the visible window for the first file only after the user agrees
  // to drop unsaved edits. Cancel aborts the whole open: the dirty buffer
  // stays, and none of the requested files are opened.
  if (window_ && window_->get_visible()) {
    if (!window_->confirm_discard_or_save()) {
      return;
    }
    window_->present();
    window_->open_file(paths[0]);
    index = 1;
  } else if (!window_) {
    auto* w = create_window();
    w->present();
    w->open_file(paths[0]);
    index = 1;
  }

  for (; index < paths.size(); ++index) {
    auto* w = create_window();
    w->present();
    w->open_file(paths[index]);
  }
}

void Application::on_open(const Gio::Application::type_vec_files& files,
                          const Glib::ustring& /*hint*/) {
  std::vector<std::string> paths;
  paths.reserve(files.size());
  for (const auto& file : files) {
    if (!file) {
      continue;
    }
    std::string path = file->get_path();
    if (path.empty()) {
      path = file->get_uri();
    }
    if (!path.empty()) {
      paths.push_back(path);
    }
  }
  open_files(paths);
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

}  // namespace lundukeedit
