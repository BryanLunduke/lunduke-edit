// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUNDUKEEDIT_APPLICATION_HPP
#define LUNDUKEEDIT_APPLICATION_HPP

#include <giomm/file.h>
#include <glibmm/refptr.h>
#include <gtkmm/application.h>
#include <sigc++/connection.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lundukeedit {

class MainWindow;
struct EditChecks;

class Application : public Gtk::Application {
public:
  static Glib::RefPtr<Application> create();
  ~Application() override;

  // The focused editor, or another visible one when nothing has been focused.
  MainWindow* main_window() const;

  // Confirm every open document, then quit the process.
  bool confirm_quit();

  // Last window that actually received focus. A Gio open reuses it only when
  // it is still an empty untitled buffer.
  void note_window_focus(MainWindow* window);

  const std::vector<std::string>& recents();
  void remember_recent(const std::string& path);

  // Preferences new windows inherit. The open charset is how the next file
  // is decoded; a new document itself always starts as UTF-8.
  void set_font(const std::string& desc);
  const std::string& font() const { return font_; }
  void set_wrap_text(bool on);
  bool wrap_text() const { return wrap_text_; }
  void set_tab_width(int spaces);
  int tab_width() const { return tab_width_; }
  void set_open_charset(const std::string& charset, bool prefer_utf8);
  const std::string& open_charset() const { return open_charset_; }
  bool prefer_utf8() const { return prefer_utf8_; }

protected:
  Application();

  void on_startup() override;
  void on_activate() override;
  void on_open(const Gio::Application::type_vec_files& files,
               const Glib::ustring& hint) override;

private:
  friend struct EditChecks;

  MainWindow* create_window();
  void on_window_hidden(MainWindow* window);
  void destroy_window_now(MainWindow* window);
  void open_files(const std::vector<std::string>& paths);
  void ensure_recents_loaded();
  void report_non_native(const std::vector<Glib::ustring>& uris);
  static void install_css();

  MainWindow* focused_{nullptr};
  std::unordered_set<MainWindow*> live_;
  std::unordered_map<MainWindow*, sigc::connection> pending_delete_;
  bool destroying_{false};

  std::vector<std::string> recents_;
  bool recents_loaded_{false};
  static constexpr int kMaxRecents = 8;

  std::string font_{"Monospace 11"};
  bool wrap_text_{false};
  int tab_width_{4};
  std::string open_charset_{"UTF-8"};
  bool prefer_utf8_{true};
};

}  // namespace lundukeedit

#endif
