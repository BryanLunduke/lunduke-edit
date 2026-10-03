// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUNDUKEEDIT_MAIN_WINDOW_HPP
#define LUNDUKEEDIT_MAIN_WINDOW_HPP

#include "find_replace_dialog.hpp"
#include "line_gutter.hpp"

#include <gtkmm/applicationwindow.h>
#include <gtkmm/box.h>
#include <gtkmm/checkmenuitem.h>
#include <gtkmm/frame.h>
#include <gtkmm/label.h>
#include <gtkmm/menu.h>
#include <gtkmm/menubar.h>
#include <gtkmm/menuitem.h>
#include <gtkmm/radiomenuitem.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separatormenuitem.h>
#include <gtkmm/statusbar.h>
#include <gtkmm/pagesetup.h>
#include <gtkmm/printoperation.h>
#include <gtkmm/printsettings.h>

#include <gtksourceviewmm.h>
#include <pangomm/layout.h>

#include <string>
#include <vector>

namespace lundukeedit {

class Application;
struct EditChecks;

class MainWindow : public Gtk::ApplicationWindow {
public:
  explicit MainWindow(Application& app);

  void load_seed_sample();
  bool open_file(const std::string& path);
  // Asks before dropping unsaved edits. False means the caller must keep
  // the current buffer (Cancel, or Save that did not succeed).
  bool confirm_discard_or_save();

protected:
  bool on_delete_event(GdkEventAny* event) override;
  bool on_key_press_event(GdkEventKey* event) override;

private:
  friend struct EditChecks;
  void build_ui();
  void build_menus();
  void apply_css();
  void update_title();
  void update_status();
  void update_undo_redo_sensitivity();
  void set_dirty(bool dirty);
  void refresh_dirty_from_buffer();
  void on_modified_changed();
  bool save_document();
  bool save_as_dialog();
  bool save_to_path(const std::string& path);
  void report_error(const Glib::ustring& primary,
                    const Glib::ustring& secondary);
  static std::string ensure_save_as_path(std::string path);
  static bool parse_go_to_line(const std::string& text, int& line);
  int display_column_at(const Gtk::TextIter& iter) const;
  Glib::ustring current_basename() const;
  Glib::RefPtr<Gsv::Buffer> buffer();

  // File
  void on_new();
  void on_open();
  void on_open_recent(const std::string& path);
  void on_save();
  void on_save_as();
  void on_page_setup();
  void on_print();
  void on_exit();

  void on_begin_print(const Glib::RefPtr<Gtk::PrintContext>& context);
  void on_draw_page(const Glib::RefPtr<Gtk::PrintContext>& context, int page_nr);
  void rebuild_recents_menu();
  void remember_recent(const std::string& path);

  // Edit
  void on_undo();
  void on_redo();
  void on_cut();
  void on_copy();
  void on_paste();
  void on_select_all();

  // Search
  void on_find();
  void on_find_next();
  void on_go_to_line();

  // Text / Font / View / Encoding
  void on_toggle_wrap();
  void on_tab_width();
  void on_font();
  void on_toggle_line_numbers();
  void on_encoding_utf8();
  void on_encoding_latin1();
  void set_encoding(const std::string& encoding);
  void on_about();

  void on_buffer_changed();
  void on_cursor_moved(const Gtk::TextBuffer::iterator& loc,
                       const Glib::RefPtr<Gtk::TextBuffer::Mark>& mark);
  void apply_tab_width(int spaces);
  void apply_font(const Pango::FontDescription& desc);

  enum class ReplaceResult { Replaced, Found, NotFound, Blocked };

  bool find_match(const FindOptions& opts, bool from_next);
  int count_matches(const FindOptions& opts);
  ReplaceResult replace_current(const FindOptions& opts);
  int replace_all(const FindOptions& opts);
  void clear_find_highlights();
  void highlight_all_matches(const FindOptions& opts);
  bool is_entire_word(const Gtk::TextIter& start,
                      const Gtk::TextIter& end) const;
  Gtk::TextSearchFlags search_flags(const FindOptions& opts) const;
  // False when Search Selection Only is set and there is no selection.
  // Does not widen an empty selection to the whole buffer.
  bool get_search_bounds(const FindOptions& opts, Gtk::TextIter& begin,
                         Gtk::TextIter& end);
  void ensure_find_marks();
  void pin_selection_only_range();
  void clear_selection_only_range();
  void clear_extend_anchor();
  bool selection_matches_needle(const FindOptions& opts,
                                const Gtk::TextIter& a,
                                const Gtk::TextIter& b) const;

  Application& app_;

  Gtk::Box root_{Gtk::ORIENTATION_VERTICAL};
  Gtk::MenuBar menubar_;
  Gtk::Box editor_row_{Gtk::ORIENTATION_HORIZONTAL};
  Gtk::ScrolledWindow scrolled_;
  Gsv::View text_view_;
  LineGutter* gutter_{nullptr};

  Gtk::Box status_box_{Gtk::ORIENTATION_HORIZONTAL, 0};
  Gtk::Frame status_pos_frame_;
  Gtk::Frame status_mode_frame_;
  Gtk::Frame status_enc_frame_;
  Gtk::Frame status_bytes_frame_;
  Gtk::Label status_pos_{"Ln 1, Col 1"};
  Gtk::Label status_mode_{"Insert"};
  Gtk::Label status_enc_{"UTF-8"};
  Gtk::Label status_bytes_{"0 bytes"};

  Gtk::CheckMenuItem* wrap_item_{nullptr};
  Gtk::CheckMenuItem* line_numbers_item_{nullptr};
  Gtk::MenuItem* undo_item_{nullptr};
  Gtk::MenuItem* redo_item_{nullptr};
  Gtk::Menu* recents_menu_{nullptr};
  Gtk::RadioMenuItem* enc_utf8_item_{nullptr};
  Gtk::RadioMenuItem* enc_latin1_item_{nullptr};

  std::string file_path_;
  std::string encoding_{"UTF-8"};
  // Encoding that matches the bytes last loaded or successfully saved.
  std::string saved_encoding_{"UTF-8"};
  bool encoding_dirty_{false};
  // Menu preference. Auto-detected Latin-1 on one file does not force the
  // next file to skip the UTF-8 check. An explicit Text menu choice does.
  bool prefer_utf8_{true};
  bool dirty_{false};
  bool seeding_{false};
  bool overwrite_{false};
  int tab_width_{4};
  Pango::FontDescription font_desc_;

  FindOptions find_opts_;
  Glib::RefPtr<Gtk::TextTag> find_tag_;
  // Pinned range for Search Selection Only (survives match reselection).
  Glib::RefPtr<Gtk::TextBuffer::Mark> sel_only_start_mark_;
  Glib::RefPtr<Gtk::TextBuffer::Mark> sel_only_end_mark_;
  bool sel_only_range_valid_{false};
  // Anchor for Extend Selection growth across successive Finds.
  Glib::RefPtr<Gtk::TextBuffer::Mark> extend_anchor_mark_;
  bool extend_anchor_valid_{false};
  // The needle from the last successful find, even if Extend Selection
  // has grown the visible selection past it.
  Glib::RefPtr<Gtk::TextBuffer::Mark> last_match_start_;
  Glib::RefPtr<Gtk::TextBuffer::Mark> last_match_end_;
  bool last_match_valid_{false};

  Glib::RefPtr<Gtk::PrintSettings> print_settings_;
  Glib::RefPtr<Gtk::PageSetup> page_setup_;
  Glib::RefPtr<Pango::Layout> print_layout_;
  std::vector<int> print_page_breaks_;  // line index starts for each page after 0

  static constexpr int kMaxRecents = 8;
  std::vector<std::string> recents_;
};

}  // namespace lundukeedit

#endif
