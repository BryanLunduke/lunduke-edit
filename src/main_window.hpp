// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUNDUKEEDIT_MAIN_WINDOW_HPP
#define LUNDUKEEDIT_MAIN_WINDOW_HPP

#include "line_gutter.hpp"

#include <gtkmm/applicationwindow.h>
#include <gtkmm/box.h>
#include <gtkmm/checkmenuitem.h>
#include <gtkmm/frame.h>
#include <gtkmm/label.h>
#include <gtkmm/menubar.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separatormenuitem.h>
#include <gtkmm/statusbar.h>
#include <gtkmm/textview.h>

#include <string>

namespace lundukeedit {

class Application;

class MainWindow : public Gtk::ApplicationWindow {
public:
  explicit MainWindow(Application& app);

  void load_seed_sample();
  bool open_file(const std::string& path);

protected:
  bool on_delete_event(GdkEventAny* event) override;
  bool on_key_press_event(GdkEventKey* event) override;

private:
  void build_ui();
  void build_menus();
  void apply_css();
  void update_title();
  void update_status();
  void set_dirty(bool dirty);
  bool confirm_discard_or_save();
  bool save_to_path(const std::string& path);
  Glib::ustring current_basename() const;

  // File
  void on_new();
  void on_open();
  void on_save();
  void on_save_as();
  void on_exit();

  // Edit
  void on_undo();
  void on_cut();
  void on_copy();
  void on_paste();
  void on_select_all();

  // Search
  void on_find();
  void on_find_next();

  // Text / Font / View
  void on_toggle_wrap();
  void on_tab_width();
  void on_font();
  void on_toggle_line_numbers();
  void on_about();

  void on_buffer_changed();
  void on_cursor_moved(const Gtk::TextBuffer::iterator& loc,
                       const Glib::RefPtr<Gtk::TextBuffer::Mark>& mark);
  void apply_tab_width(int spaces);
  void apply_font(const Pango::FontDescription& desc);
  bool find_text(bool from_next);

  Application& app_;

  Gtk::Box root_{Gtk::ORIENTATION_VERTICAL};
  Gtk::MenuBar menubar_;
  Gtk::Box editor_row_{Gtk::ORIENTATION_HORIZONTAL};
  Gtk::ScrolledWindow scrolled_;
  Gtk::TextView text_view_;
  LineGutter* gutter_{nullptr};

  Gtk::Box status_box_{Gtk::ORIENTATION_HORIZONTAL, 0};
  Gtk::Frame status_pos_frame_;
  Gtk::Frame status_mode_frame_;
  Gtk::Frame status_bytes_frame_;
  Gtk::Label status_pos_{"Ln 1, Col 1"};
  Gtk::Label status_mode_{"Insert"};
  Gtk::Label status_bytes_{"0 bytes"};

  Gtk::CheckMenuItem* wrap_item_{nullptr};
  Gtk::CheckMenuItem* line_numbers_item_{nullptr};

  std::string file_path_;
  bool dirty_{false};
  bool seeding_{false};
  bool overwrite_{false};
  int tab_width_{4};
  Pango::FontDescription font_desc_;

  Glib::ustring find_needle_;
  bool find_case_sensitive_{false};
};

}  // namespace lundukeedit

#endif
