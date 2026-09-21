// SPDX-License-Identifier: GPL-3.0-or-later

#include "application.hpp"
#include "main_window.hpp"

namespace lundukeedit {

Glib::RefPtr<Application> Application::create() {
  return Glib::RefPtr<Application>(new Application());
}

Application::Application()
    : Gtk::Application("org.lunduke.LundukeEdit",
                       Gio::APPLICATION_HANDLES_OPEN) {}

void Application::on_startup() {
  Gtk::Application::on_startup();
}

bool Application::ensure_window() {
  if (window_) {
    return false;
  }
  window_ = new MainWindow(*this);
  add_window(*window_);
  window_->signal_hide().connect([this]() {
    // Window deletes itself via delete_on_hide; drop our pointer.
    window_ = nullptr;
  });
  return true;
}

void Application::on_activate() {
  const bool fresh = ensure_window();
  if (fresh) {
    window_->load_seed_sample();
  }
  window_->present();
}

void Application::on_open(const Gio::Application::type_vec_files& files,
                          const Glib::ustring& /*hint*/) {
  ensure_window();
  if (!files.empty()) {
    window_->open_file(files.front()->get_path());
  }
  window_->present();
}

}  // namespace lundukeedit
