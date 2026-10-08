// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUNDUKEEDIT_MAIN_WINDOW_HPP
#define LUNDUKEEDIT_MAIN_WINDOW_HPP

#include "find_replace_dialog.hpp"
#include "line_gutter.hpp"

#include <giomm/asyncresult.h>
#include <gdkmm/cursor.h>
#include <gdkmm/dragcontext.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/box.h>
#include <gtkmm/clipboard.h>
#include <gtkmm/selectiondata.h>
#include <gtkmm/checkmenuitem.h>
#include <gtkmm/frame.h>
#include <gtkmm/label.h>
#include <gtkmm/menu.h>
#include <gtkmm/menubar.h>
#include <gtkmm/menuitem.h>
#include <gtkmm/radiomenuitem.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/texttag.h>
#include <gtkmm/separatormenuitem.h>
#include <gtkmm/statusbar.h>
#include <gtkmm/pagesetup.h>
#include <gtkmm/printoperation.h>
#include <gtkmm/printsettings.h>
#include <sigc++/connection.h>

#include <gtksourceviewmm.h>
#include <pangomm/layout.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
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
  // discard_already_confirmed is true after File → Open / Open Recent
  // already asked about unsaved changes. A second open of this window's
  // path re-reads the file when it changed on disk.
  bool open_file(const std::string& path, bool discard_already_confirmed = false);
  bool open_file_body(const std::string& path);
  // Continue is close, quit, and File → New / Open. ReloadDisk is the
  // question after the user already chose Reload on a file that changed.
  enum class DiscardKind { Continue, ReloadDisk };
  // Asks before dropping unsaved edits. False means the caller must keep
  // the current buffer (Cancel, or Save that did not succeed).
  bool confirm_discard_or_save(DiscardKind kind = DiscardKind::Continue);

  // True when this window is a clean untitled document with no text.
  // A second-instance open may reuse it only while it is also focused.
  bool is_empty_untitled() const;

  // True when this window is showing the same file as path (symlinks
  // included). A second open presents that window instead of loading again.
  bool edits_path(const std::string& path) const;

  void rebuild_recents_menu();

  // Test seams. Null unless a behavior test installs them.
  static ssize_t (*test_write_hook_)(int fd, const void* buf, std::size_t n,
                                     bool inplace_copy);
  static int (*test_dir_fsync_hook_)(int fd);
  static std::function<void(MainWindow*)> test_during_large_confirm_;
  static std::function<const char*(MainWindow*)> test_discard_choice_;

  // Affirmative response on the stat-failure question. Enter runs Save As.
  static constexpr int kPromptSaveAs = 100;

  enum class NewlineStyle { Lf, Crlf, Cr };

protected:
  bool on_delete_event(GdkEventAny* event) override;
  bool on_key_press_event(GdkEventKey* event) override;
  bool on_key_release_event(GdkEventKey* event) override;
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
  bool read_clipboard_text(const Glib::RefPtr<Gtk::Clipboard>& clip,
                           Glib::ustring& out);
  void insert_pasted_text(const Glib::ustring& text);
  void insert_primary_paste(const Glib::ustring& text, GdkEventButton* event);
  bool on_text_button_press(GdkEventButton* event);
  bool on_text_button_release(GdkEventButton* event);
  void on_drag_data_received(const Glib::RefPtr<Gdk::DragContext>& context,
                             int x, int y, const Gtk::SelectionData& data,
                             guint info, guint time);
  void on_window_drag_data_received(const Glib::RefPtr<Gdk::DragContext>& context,
                                    int x, int y, const Gtk::SelectionData& data,
                                    guint info, guint time);
  void open_dropped_uris(const std::vector<Glib::ustring>& uris);
  void sync_overwrite_status();
  void clear_document_search_pins();
  void sync_encoding_radios();
  void maybe_restore_wrap();
  bool buffer_has_long_line();
  void note_line_length(int chars_in_line);
  void sync_long_line_window();
  bool apply_bulk_replace(int start_off, int end_off, const Glib::ustring& neu);
  enum class DiskChangeChoice { Cancel, Replace, Reload };
  DiskChangeChoice confirm_file_changed(const std::string& path);
  // Same path opened again. Re-reads when the inode or mtime changed.
  // A dirty buffer is replaced only after the unsaved-changes prompt,
  // unless the caller already confirmed that prompt.
  bool reopen_same_path(const std::string& path, bool discard_already_confirmed);
  void remember_file_identity(const std::string& path);
  void track_inserted_endings(const Gtk::TextIter& pos, const Glib::ustring& text);
  void track_erased_endings(const Gtk::TextIter& start, const Gtk::TextIter& end);
  void snapshot_endings();
  void apply_ending_kinds(const std::vector<char>& kinds);
  std::vector<char> ending_kinds() const;
  void clear_ending_history();
  void restore_buffer_after_failed_load(const Glib::ustring& previous_text,
                                        bool previous_modified,
                                        const std::string& previous_encoding,
                                        NewlineStyle previous_newlines);
  void remember_source_lines(const Glib::ustring& text,
                             const std::vector<char>& kinds);
  void on_find_dialog_hidden();
  void on_open_pref_utf8();
  void on_open_pref_latin1();

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
  int measure_print_tab(const Glib::RefPtr<Gtk::PrintContext>& context) const;
  void configure_print_layout(const Glib::RefPtr<Pango::Layout>& layout,
                              const Glib::RefPtr<Gtk::PrintContext>& context,
                              int width_pango);
  void draw_print_layout(const Cairo::RefPtr<Cairo::Context>& cr,
                         const Glib::RefPtr<Pango::Layout>& layout, double& y,
                         int row_begin, int row_end) const;
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
  // user_chosen is Text → Font (and a face reloaded from disk). The
  // default face keeps the monospace style class.
  void apply_font(const Pango::FontDescription& desc, bool user_chosen = false);
  void install_editor_font(const Pango::FontDescription& desc);
  void apply_editor_font_tag();
  void on_font_tag_inserted(const Gtk::TextBuffer::iterator& pos,
                           const Glib::ustring& text, int bytes);
  void refresh_disk_flags();

  enum class ReplaceResult { Replaced, Found, NotFound, Blocked };
  enum class SearchStep { Miss, Hit, Yield };

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
  SearchStep step_search(bool backward, int& cursor_off, int& match_start,
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
    int slice_steps{0};
    FindReplaceDialog* dlg{nullptr};
    // Replace All collects hits, then commits them as one buffer edit.
    bool collected{false};
    std::vector<std::pair<int, int>> hits;
    int replace_start{0};
    int replace_end{0};
  };

  struct LoadState;
  void begin_load_chrome(const std::string& path);
  void end_load_chrome();
  void update_load_status();
  bool start_async_load(const std::string& path);
  void pump_async_load();
  void fail_async_load(const std::shared_ptr<LoadState>& state,
                       const std::string& primary, const std::string& secondary);
  void abort_async_load(const std::shared_ptr<LoadState>& state);
  void finish_async_load(const std::shared_ptr<LoadState>& state);
  void schedule_load_read(const std::shared_ptr<LoadState>& state);
  void begin_load_stream(const std::shared_ptr<LoadState>& state);
  void on_load_opened(const std::shared_ptr<LoadState>& state,
                      const Glib::RefPtr<Gio::AsyncResult>& result);
  void on_load_chunk(const std::shared_ptr<LoadState>& state,
                     const Glib::RefPtr<Gio::AsyncResult>& result);
  bool on_load_idle(const std::shared_ptr<LoadState>& state);
  bool commit_loaded_text(const std::shared_ptr<LoadState>& state);

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
  Gtk::RadioMenuItem* open_utf8_item_{nullptr};
  Gtk::RadioMenuItem* open_latin1_item_{nullptr};

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
  NewlineStyle saved_newline_style_{NewlineStyle::Lf};
  // Per-line break recorded at load, paired with the UTF-8 line text.
  // Unchanged lines are written back with their own break.
  struct SourceLine {
    std::string text;
    char kind{0};
  };
  std::vector<SourceLine> source_lines_;
  // Snapshots of per-line endings, one per undoable edit, so undo/redo
  // puts each line's ending back. Cleared when the document is replaced.
  std::vector<std::vector<char>> ending_undo_;
  std::vector<std::vector<char>> ending_redo_;
  std::vector<char> pending_kinds_;
  bool have_pending_kinds_{false};
  bool ending_restore_{false};
  bool in_user_action_{false};
  bool ending_snapshotted_{false};
  bool force_replace_{false};
  std::string loaded_bytes_;
  Glib::ustring loaded_text_;
  bool loaded_bytes_valid_{false};
  bool have_file_id_{false};
  std::uint64_t file_dev_{0};
  std::uint64_t file_ino_{0};
  std::int64_t file_mtime_sec_{0};
  std::int64_t file_mtime_nsec_{0};
  // The loaded path is gone (ENOENT) or stat failed for another reason.
  // Close and quit must ask before dropping the buffer.
  bool file_missing_{false};
  bool file_unreadable_{false};
  bool opening_{false};
  bool accepting_cr_{false};
  bool swallow_insert_repeat_{false};
  std::string last_save_error_;
  std::string last_open_error_;
  std::string last_notice_;
  // Error dialogs actually raised. A stat failure must not raise one of
  // these and then repeat itself on the save question and on Save.
  int error_reports_{0};
  Glib::ustring last_error_primary_;
  Glib::ustring last_error_secondary_;
  // Set once the user has been told about the current stat failure, either
  // by the save question or by the one Save / reopen dialog. Later Save
  // and reopen calls for that same failure stay quiet.
  bool disk_error_noted_{false};
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
  bool font_user_chosen_{false};
  Glib::RefPtr<Gtk::CssProvider> font_css_;
  Glib::RefPtr<Gtk::TextTag> font_tag_;
  // Hides the part of a very long line that is outside the caret window so
  // typing does not shape the whole line. Wrap stays off.
  Glib::RefPtr<Gtk::TextTag> long_hidden_tag_;
  bool syncing_long_line_{false};
  bool long_line_present_{false};
  int long_window_line_{-1};
  int long_window_begin_{0};
  int long_window_end_{0};
  std::shared_ptr<LoadState> load_;
  bool loading_{false};
  bool dropping_uris_{false};
  bool load_saved_editable_{true};
  Glib::RefPtr<Gdk::Cursor> load_watch_;
  // Timestamp of the last middle-button press. Later presses in the same
  // double- or triple-click do not insert again.
  std::uint32_t last_middle_paste_time_{0};
  // What the last save/close question would have focused, for tests.
  Glib::ustring last_prompt_primary_;
  Glib::ustring last_prompt_secondary_;
  Glib::ustring last_prompt_accept_;
  int last_prompt_default_{0};

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
  struct PrintSlice {
    Glib::RefPtr<Pango::Layout> layout;
    int row_begin{0};
    int row_end{0};
  };
  struct PrintPage {
    std::vector<PrintSlice> slices;
  };
  std::vector<PrintPage> print_pages_;
  int last_print_tab_pos_{0};
  bool print_tabs_in_pixels_{false};
};

}  // namespace lundukeedit

#endif
