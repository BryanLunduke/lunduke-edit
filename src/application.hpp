// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUNDUKEEDIT_APPLICATION_HPP
#define LUNDUKEEDIT_APPLICATION_HPP

#include <giomm/file.h>
#include <glibmm/refptr.h>
#include <gtkmm/application.h>

#include <string>
#include <vector>

namespace lundukeedit {

class MainWindow;
struct EditChecks;

class Application : public Gtk::Application {
public:
  static Glib::RefPtr<Application> create();

  MainWindow* main_window() const { return window_; }

  // Confirm every open document, then quit the process.
  bool confirm_quit();

protected:
  Application();

  void on_startup() override;
  void on_activate() override;
  void on_open(const Gio::Application::type_vec_files& files,
               const Glib::ustring& hint) override;

private:
  friend struct EditChecks;

  bool ensure_window();
  MainWindow* create_window();
  void forget_window(MainWindow* window);
  void open_files(const std::vector<std::string>& paths);

  MainWindow* window_{nullptr};
};

}  // namespace lundukeedit

#endif
