// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUNDUKEEDIT_APPLICATION_HPP
#define LUNDUKEEDIT_APPLICATION_HPP

#include <giomm/file.h>
#include <glibmm/refptr.h>
#include <gtkmm/application.h>

namespace lundukeedit {

class MainWindow;

class Application : public Gtk::Application {
public:
  static Glib::RefPtr<Application> create();

  MainWindow* main_window() const { return window_; }

protected:
  Application();

  void on_startup() override;
  void on_activate() override;
  void on_open(const Gio::Application::type_vec_files& files,
               const Glib::ustring& hint) override;

private:
  bool ensure_window();

  MainWindow* window_{nullptr};
};

}  // namespace lundukeedit

#endif
