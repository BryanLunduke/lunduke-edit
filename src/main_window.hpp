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
#include <sigc++/connection.h>

#include <gtksourceviewmm.h>
#include <pangomm/layout.h>

#include <cstddef>
#include <string>
#include <vector>

typedef struct _GtkTextView GtkTextView;

namespace lundukeedit {

class Application;
struct EditChecks;

class MainWindow : public Gtk::ApplicationWindow {
public:
  explicit MainWindow(Application& app);
  ~MainWindow() override;

  void load_seed_sample();
  bool open_file(const std::string& path);
  // Asks before dropping unsaved edits. False means the caller must keep
  // the current buffer (Cancel, or Save that did not succeed).
  bool confirm_discard_or_save();

  // True when this window is a clean untitled document with no text.
  // A second-instance open may reuse it only while it is also focused.
  bool is_empty_untitled() const;

  void rebuild_recents_menu();

  enum class NewlineStyle { Lf, Crlf, Cr };

protected:
  bool on_delete_event(GdkEventAny* event) override;
  bool on_key_press_event(GdkEventKey* event) override;
  bool on_focus_in_event(GdkEventFocus* event) override;

private:
  friend struct EditChecks;
  void build_ui();
  void build_menus();
  void update_title();
  void update_status();
  void update_cursor_status();
  void update_bytes_status();
  std::size_t cached_save_bytes() const;
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

  void on_text_inserted(const Gtk::TextBuffer::iterator& pos,
                        const Glib::ustring& text, int bytes);
  void on_text_erased(const Gtk::TextBuffer::iterator& start,
                      const Gtk::TextBuffer::iterator& end);
  void note_loaded_text(const Glib::ustring& text);
  void force_wrap_off();
  bool confirm_large_open(const std::string& path);
  bool clipboard_paste_allowed();
  void handle_paste_clipboard(GtkTextView* view);

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
  int next_print_end(int offset);
  double measure_print_chunk(const Glib::RefPtr<Gtk::PrintContext>& context,
                             int width_pango, int start, int end,
                             double min_height);
  void configure_print_layout(const Glib::RefPtr<Pango::Layout>& layout,
                              int width_pango) const;
  void draw_print_layout(const Cairo::RefPtr<Cairo::Context>& cr,
                         const Glib::RefPtr<Pango::Layout>& layout,
                         double& y) const;
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
  void set_open_preference(bool utf8);
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
  void set_find_count(int n, bool capped);
  void start_find_all(const FindOptions& opts, FindReplaceDialog* dlg);
  void start_replace_all(const FindOptions& opts, FindReplaceDialog* dlg);
  bool on_find_idle();
  bool pump_find_highlight();
  bool pump_replace();
  bool step_search(bool backward, int& cursor_off, int& match_start,
                   int& match_end);
  void finish_find_scan(bool show_result);
  void cancel_find_scan();
  void end_find_user_action();
  bool confirm_huge_undo(std::size_t bytes);
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

  struct FindScan {
    enum class Kind { None, FindAll, ReplaceAll };
    Kind kind{Kind::None};
    bool active{false};
    bool cancel{false};
    bool finishing{false};
    bool started{false};
    bool counting{false};
    bool capped{false};
    bool user_action_open{false};
    FindOptions opts{};
    int cursor_off{0};
    int select_start{-1};
    int select_end{-1};
    int count{0};
    std::size_t undo_bytes{0};
    FindReplaceDialog* dlg{nullptr};
  };

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
  Gtk::Frame status_find_frame_;
  Gtk::Frame status_bytes_frame_;
  Gtk::Label status_pos_{"Ln 1, Col 1"};
  Gtk::Label status_mode_{"Insert"};
  Gtk::Label status_enc_{"UTF-8"};
  Gtk::Label status_find_;
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
  // Menu preference for the next Open. Independent of the document encoding.
  // Auto-detected Latin-1 on one file does not change this. An explicit
  // Text menu choice does.
  std::string open_charset_{"UTF-8"};
  bool prefer_utf8_{true};
  NewlineStyle newline_style_{NewlineStyle::Lf};
  // UTF-8 bytes and LF count in the buffer. Status "bytes" is the size
  // save_to_path would write, derived from these plus encoding and newlines.
  std::size_t utf8_bytes_{0};
  std::size_t newline_count_{0};
  bool dirty_{false};
  bool seeding_{false};
  bool overwrite_{false};
  bool suppress_wrap_pref_{false};
  bool find_highlights_on_{false};
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

  FindScan find_scan_;
  sigc::connection find_idle_;

  Glib::RefPtr<Gtk::PrintSettings> print_settings_;
  Glib::RefPtr<Gtk::PageSetup> page_setup_;
  // Char offsets where each page after the first begins.
  std::vector<int> print_page_breaks_;
};

}  // namespace lundukeedit

#endif
