// SPDX-License-Identifier: GPL-3.0-or-later

#include "main_window.hpp"
#include "application.hpp"

#include <gtkmm/aboutdialog.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/filechooserdialog.h>
#include <gtkmm/fontchooserdialog.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/radiobutton.h>
#include <gtkmm/stock.h>
#include <gtkmm/stylecontext.h>

#include <fstream>
#include <sstream>
#include <vector>

namespace lundukeedit {
namespace {

const char* kSampleText =
    "LUNDUKE EDIT 1.0.\n"
    "\n"
    "This is a toy mashup of Windows 95 Notepad,\n"
    "Macintosh SimpleText, and BBEdit Lite.\n"
    "\n"
    "Just enough features to be useful,\n"
    "but still light, fast, and late-1996.\n"
    "\n"
    "For fictional use only.\n"
    "— Philip\n";

std::string format_bytes(std::size_t n) {
  // Thousand separators like the mockup ("1,024 bytes").
  std::string digits = std::to_string(n);
  std::string out;
  const int len = static_cast<int>(digits.size());
  for (int i = 0; i < len; ++i) {
    if (i > 0 && (len - i) % 3 == 0) {
      out.push_back(',');
    }
    out.push_back(digits[static_cast<std::size_t>(i)]);
  }
  out += (n == 1) ? " byte" : " bytes";
  return out;
}

}  // namespace

MainWindow::MainWindow(Application& app) : app_(app) {
  set_title("Untitled — Lunduke Edit");
  set_default_size(640, 420);
  set_border_width(0);
  set_icon_name("accessories-text-editor");
  Gtk::Window::set_default_icon_name("accessories-text-editor");

  font_desc_ = Pango::FontDescription("Monospace 11");

  build_ui();
  build_menus();
  apply_css();
  apply_font(font_desc_);
  apply_tab_width(tab_width_);

  text_view_.set_wrap_mode(Gtk::WRAP_WORD_CHAR);
  text_view_.set_monospace(true);
  text_view_.set_accepts_tab(true);
  text_view_.set_left_margin(4);
  text_view_.set_right_margin(4);
  text_view_.set_top_margin(2);
  text_view_.set_bottom_margin(2);

  auto buf = text_view_.get_buffer();
  buf->signal_changed().connect(
      sigc::mem_fun(*this, &MainWindow::on_buffer_changed));
  buf->signal_mark_set().connect(
      sigc::mem_fun(*this, &MainWindow::on_cursor_moved));

  show_all_children();
  update_status();
}

void MainWindow::build_ui() {
  add(root_);
  root_.pack_start(menubar_, Gtk::PACK_SHRINK);

  gutter_ = Gtk::manage(new LineGutter(text_view_));
  editor_row_.pack_start(*gutter_, Gtk::PACK_SHRINK);

  scrolled_.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
  scrolled_.add(text_view_);
  editor_row_.pack_start(scrolled_, Gtk::PACK_EXPAND_WIDGET);
  root_.pack_start(editor_row_, Gtk::PACK_EXPAND_WIDGET);

  // Status bar: three recessed frames like the mockup.
  status_box_.set_border_width(2);
  status_box_.set_spacing(2);

  status_pos_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_mode_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_bytes_frame_.set_shadow_type(Gtk::SHADOW_IN);

  status_pos_.set_halign(Gtk::ALIGN_START);
  status_pos_.set_margin_start(6);
  status_pos_.set_margin_end(6);
  status_pos_.set_margin_top(2);
  status_pos_.set_margin_bottom(2);

  status_mode_.set_halign(Gtk::ALIGN_CENTER);
  status_mode_.set_margin_start(10);
  status_mode_.set_margin_end(10);
  status_mode_.set_margin_top(2);
  status_mode_.set_margin_bottom(2);

  status_bytes_.set_halign(Gtk::ALIGN_END);
  status_bytes_.set_margin_start(6);
  status_bytes_.set_margin_end(6);
  status_bytes_.set_margin_top(2);
  status_bytes_.set_margin_bottom(2);

  status_pos_frame_.add(status_pos_);
  status_mode_frame_.add(status_mode_);
  status_bytes_frame_.add(status_bytes_);

  status_box_.pack_start(status_pos_frame_, Gtk::PACK_EXPAND_WIDGET);
  status_box_.pack_start(status_mode_frame_, Gtk::PACK_SHRINK);
  status_box_.pack_start(status_bytes_frame_, Gtk::PACK_SHRINK);

  root_.pack_start(status_box_, Gtk::PACK_SHRINK);
}

void MainWindow::build_menus() {
  auto add_item = [](Gtk::Menu& menu, const Glib::ustring& label,
                     const sigc::slot<void>& slot,
                     const Glib::ustring& accel = {},
                     Gtk::Window* win = nullptr) {
    auto* item = Gtk::manage(new Gtk::MenuItem(label, true));
    item->signal_activate().connect(slot);
    menu.append(*item);
    if (!accel.empty() && win) {
      item->add_accelerator("activate", win->get_accel_group(),
                            gdk_keyval_from_name(accel.c_str()),
                            Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
    }
    return item;
  };

  // Ensure we have an accel group.
  add_accel_group(Gtk::AccelGroup::create());

  // ---- File ----
  auto* file_menu = Gtk::manage(new Gtk::Menu());
  auto* file_item = Gtk::manage(new Gtk::MenuItem("_File", true));
  file_item->set_submenu(*file_menu);
  menubar_.append(*file_item);

  auto* new_i = Gtk::manage(new Gtk::MenuItem("_New", true));
  new_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_new));
  new_i->add_accelerator("activate", get_accel_group(), GDK_KEY_n,
                         Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*new_i);

  auto* open_i = Gtk::manage(new Gtk::MenuItem("_Open…", true));
  open_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_open));
  open_i->add_accelerator("activate", get_accel_group(), GDK_KEY_o,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*open_i);

  file_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* save_i = Gtk::manage(new Gtk::MenuItem("_Save", true));
  save_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_save));
  save_i->add_accelerator("activate", get_accel_group(), GDK_KEY_s,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*save_i);

  auto* save_as_i = Gtk::manage(new Gtk::MenuItem("Save _As…", true));
  save_as_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_save_as));
  save_as_i->add_accelerator("activate", get_accel_group(), GDK_KEY_s,
                             Gdk::CONTROL_MASK | Gdk::SHIFT_MASK,
                             Gtk::ACCEL_VISIBLE);
  file_menu->append(*save_as_i);

  file_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* exit_i = Gtk::manage(new Gtk::MenuItem("E_xit", true));
  exit_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_exit));
  exit_i->add_accelerator("activate", get_accel_group(), GDK_KEY_q,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  file_menu->append(*exit_i);

  // ---- Edit ----
  auto* edit_menu = Gtk::manage(new Gtk::Menu());
  auto* edit_item = Gtk::manage(new Gtk::MenuItem("_Edit", true));
  edit_item->set_submenu(*edit_menu);
  menubar_.append(*edit_item);

  auto* undo_i = Gtk::manage(new Gtk::MenuItem("_Undo", true));
  undo_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_undo));
  undo_i->add_accelerator("activate", get_accel_group(), GDK_KEY_z,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*undo_i);

  edit_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* cut_i = Gtk::manage(new Gtk::MenuItem("Cu_t", true));
  cut_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_cut));
  cut_i->add_accelerator("activate", get_accel_group(), GDK_KEY_x,
                         Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*cut_i);

  auto* copy_i = Gtk::manage(new Gtk::MenuItem("_Copy", true));
  copy_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_copy));
  copy_i->add_accelerator("activate", get_accel_group(), GDK_KEY_c,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*copy_i);

  auto* paste_i = Gtk::manage(new Gtk::MenuItem("_Paste", true));
  paste_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_paste));
  paste_i->add_accelerator("activate", get_accel_group(), GDK_KEY_v,
                           Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*paste_i);

  edit_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* sel_i = Gtk::manage(new Gtk::MenuItem("Select _All", true));
  sel_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_select_all));
  sel_i->add_accelerator("activate", get_accel_group(), GDK_KEY_a,
                         Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*sel_i);

  // ---- Search ----
  auto* search_menu = Gtk::manage(new Gtk::Menu());
  auto* search_item = Gtk::manage(new Gtk::MenuItem("_Search", true));
  search_item->set_submenu(*search_menu);
  menubar_.append(*search_item);

  auto* find_i = Gtk::manage(new Gtk::MenuItem("_Find…", true));
  find_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_find));
  find_i->add_accelerator("activate", get_accel_group(), GDK_KEY_f,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  search_menu->append(*find_i);

  auto* find_next_i = Gtk::manage(new Gtk::MenuItem("Find _Next", true));
  find_next_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_find_next));
  find_next_i->add_accelerator("activate", get_accel_group(), GDK_KEY_F3,
                               Gdk::ModifierType(0), Gtk::ACCEL_VISIBLE);
  search_menu->append(*find_next_i);

  // ---- Text ----
  auto* text_menu = Gtk::manage(new Gtk::Menu());
  auto* text_item = Gtk::manage(new Gtk::MenuItem("_Text", true));
  text_item->set_submenu(*text_menu);
  menubar_.append(*text_item);

  wrap_item_ = Gtk::manage(new Gtk::CheckMenuItem("_Wrap Text", true));
  wrap_item_->set_active(true);
  wrap_item_->signal_toggled().connect(
      sigc::mem_fun(*this, &MainWindow::on_toggle_wrap));
  text_menu->append(*wrap_item_);

  line_numbers_item_ =
      Gtk::manage(new Gtk::CheckMenuItem("Show _Line Numbers", true));
  line_numbers_item_->set_active(true);
  line_numbers_item_->signal_toggled().connect(
      sigc::mem_fun(*this, &MainWindow::on_toggle_line_numbers));
  text_menu->append(*line_numbers_item_);

  auto* tab_i = Gtk::manage(new Gtk::MenuItem("_Tab Width…", true));
  tab_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_tab_width));
  text_menu->append(*tab_i);

  auto* font_under_text = Gtk::manage(new Gtk::MenuItem("_Font…", true));
  font_under_text->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_font));
  text_menu->append(*font_under_text);

  // ---- Font (top-level, same dialog) ----
  auto* font_menu = Gtk::manage(new Gtk::Menu());
  auto* font_top = Gtk::manage(new Gtk::MenuItem("F_ont", true));
  font_top->set_submenu(*font_menu);
  menubar_.append(*font_top);

  auto* font_i = Gtk::manage(new Gtk::MenuItem("_Font…", true));
  font_i->signal_activate().connect(sigc::mem_fun(*this, &MainWindow::on_font));
  font_menu->append(*font_i);

  // ---- Help ----
  auto* help_menu = Gtk::manage(new Gtk::Menu());
  auto* help_item = Gtk::manage(new Gtk::MenuItem("_Help", true));
  help_item->set_submenu(*help_menu);
  menubar_.append(*help_item);

  auto* about_i = Gtk::manage(new Gtk::MenuItem("_About Lunduke Edit", true));
  about_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_about));
  help_menu->append(*about_i);

  (void)add_item;
}

void MainWindow::apply_css() {
  auto css = Gtk::CssProvider::create();
  css->load_from_data(
      "textview text {"
      "  background-color: #f7f4e8;"
      "  color: #1a1a1a;"
      "}"
      "textview {"
      "  background-color: #f7f4e8;"
      "}");
  auto screen = Gdk::Screen::get_default();
  if (screen) {
    Gtk::StyleContext::add_provider_for_screen(
        screen, css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
}

void MainWindow::load_seed_sample() {
  seeding_ = true;
  auto buf = text_view_.get_buffer();
  buf->set_text(kSampleText);
  // Prefer clean buffer so title stays untitled / ready for Save As naming.
  set_dirty(false);
  file_path_.clear();
  // For the screenshot, present as readme.txt like the mockup.
  file_path_ = "/tmp/readme.txt";
  // Write the sample so Save is coherent, keep clean.
  save_to_path(file_path_);
  set_dirty(false);
  seeding_ = false;
  update_title();
  update_status();
  if (gutter_) {
    gutter_->refresh();
  }
  // Place cursor somewhere mid-document for a lively status bar.
  auto iter = buf->get_iter_at_line_offset(6, 13);  // Ln 7, Col 14 (1-based)
  if (!iter) {
    iter = buf->begin();
  }
  buf->place_cursor(iter);
  update_status();
}

bool MainWindow::open_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    Gtk::MessageDialog dlg(*this, "Could not open file.", false,
                           Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
    dlg.set_secondary_text(path);
    dlg.run();
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  seeding_ = true;
  text_view_.get_buffer()->set_text(ss.str());
  file_path_ = path;
  set_dirty(false);
  seeding_ = false;
  update_title();
  update_status();
  if (gutter_) {
    gutter_->refresh();
  }
  return true;
}

Glib::ustring MainWindow::current_basename() const {
  if (file_path_.empty()) {
    return "Untitled";
  }
  const auto pos = file_path_.find_last_of('/');
  if (pos == std::string::npos) {
    return file_path_;
  }
  return file_path_.substr(pos + 1);
}

void MainWindow::update_title() {
  Glib::ustring title = current_basename();
  if (dirty_) {
    title += " *";
  }
  title += " — Lunduke Edit";
  set_title(title);
}

void MainWindow::set_dirty(bool dirty) {
  dirty_ = dirty;
  update_title();
}

void MainWindow::update_status() {
  auto buf = text_view_.get_buffer();
  auto iter = buf->get_iter_at_mark(buf->get_insert());
  const int line = iter.get_line() + 1;
  const int col = iter.get_line_offset() + 1;
  status_pos_.set_text("Ln " + std::to_string(line) + ", Col " +
                       std::to_string(col));
  status_mode_.set_text(overwrite_ ? "Overwrite" : "Insert");

  const Glib::ustring text = buf->get_text();
  status_bytes_.set_text(format_bytes(text.bytes()));
}

void MainWindow::on_buffer_changed() {
  if (!seeding_) {
    set_dirty(true);
  }
  update_status();
  if (gutter_) {
    gutter_->queue_draw();
  }
}

void MainWindow::on_cursor_moved(
    const Gtk::TextBuffer::iterator& /*loc*/,
    const Glib::RefPtr<Gtk::TextBuffer::Mark>& mark) {
  if (mark == text_view_.get_buffer()->get_insert()) {
    update_status();
  }
}

bool MainWindow::confirm_discard_or_save() {
  if (!dirty_) {
    return true;
  }
  Gtk::MessageDialog dlg(*this, "Save changes before continuing?", false,
                         Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_NONE, true);
  dlg.set_secondary_text("\"" + current_basename() +
                         "\" has unsaved changes.");
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Don't Save", Gtk::RESPONSE_REJECT);
  dlg.add_button("_Save", Gtk::RESPONSE_ACCEPT);
  dlg.set_default_response(Gtk::RESPONSE_ACCEPT);
  const int resp = dlg.run();
  if (resp == Gtk::RESPONSE_CANCEL || resp == Gtk::RESPONSE_DELETE_EVENT) {
    return false;
  }
  if (resp == Gtk::RESPONSE_ACCEPT) {
    on_save();
    return !dirty_;
  }
  return true;  // Don't Save
}

void MainWindow::on_new() {
  if (!confirm_discard_or_save()) {
    return;
  }
  seeding_ = true;
  text_view_.get_buffer()->set_text("");
  file_path_.clear();
  set_dirty(false);
  seeding_ = false;
  update_title();
  update_status();
}

void MainWindow::on_open() {
  if (!confirm_discard_or_save()) {
    return;
  }
  Gtk::FileChooserDialog dlg(*this, "Open File",
                             Gtk::FILE_CHOOSER_ACTION_OPEN);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Open", Gtk::RESPONSE_ACCEPT);
  auto filter = Gtk::FileFilter::create();
  filter->set_name("Text files");
  filter->add_mime_type("text/plain");
  filter->add_pattern("*.txt");
  filter->add_pattern("*.md");
  filter->add_pattern("*");
  dlg.add_filter(filter);
  if (dlg.run() == Gtk::RESPONSE_ACCEPT) {
    open_file(dlg.get_filename());
  }
}

bool MainWindow::save_to_path(const std::string& path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    Gtk::MessageDialog dlg(*this, "Could not save file.", false,
                           Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
    dlg.set_secondary_text(path);
    dlg.run();
    return false;
  }
  const Glib::ustring text = text_view_.get_buffer()->get_text();
  out.write(text.data(), static_cast<std::streamsize>(text.bytes()));
  file_path_ = path;
  set_dirty(false);
  update_status();
  return true;
}

void MainWindow::on_save() {
  if (file_path_.empty()) {
    on_save_as();
    return;
  }
  save_to_path(file_path_);
}

void MainWindow::on_save_as() {
  Gtk::FileChooserDialog dlg(*this, "Save As",
                             Gtk::FILE_CHOOSER_ACTION_SAVE);
  dlg.set_do_overwrite_confirmation(true);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Save", Gtk::RESPONSE_ACCEPT);
  if (!file_path_.empty()) {
    dlg.set_filename(file_path_);
  } else {
    dlg.set_current_name("readme.txt");
  }
  if (dlg.run() == Gtk::RESPONSE_ACCEPT) {
    save_to_path(dlg.get_filename());
  }
}

void MainWindow::on_exit() {
  if (confirm_discard_or_save()) {
    hide();
  }
}

bool MainWindow::on_delete_event(GdkEventAny* /*event*/) {
  if (confirm_discard_or_save()) {
    return false;  // allow destroy
  }
  return true;  // block
}

bool MainWindow::on_key_press_event(GdkEventKey* event) {
  if (event->keyval == GDK_KEY_Insert) {
    overwrite_ = !overwrite_;
    text_view_.set_overwrite(overwrite_);
    update_status();
    return true;
  }
  return Gtk::ApplicationWindow::on_key_press_event(event);
}

void MainWindow::on_undo() {
  // Plain GtkTextBuffer has no built-in undo stack (GtkSourceBuffer does).
  // Deferred — see NOTES.md.
}

void MainWindow::on_cut() {
  auto clip = Gtk::Clipboard::get();
  text_view_.get_buffer()->cut_clipboard(clip);
}

void MainWindow::on_copy() {
  auto clip = Gtk::Clipboard::get();
  text_view_.get_buffer()->copy_clipboard(clip);
}

void MainWindow::on_paste() {
  auto clip = Gtk::Clipboard::get();
  text_view_.get_buffer()->paste_clipboard(clip);
}

void MainWindow::on_select_all() {
  auto buf = text_view_.get_buffer();
  buf->select_range(buf->begin(), buf->end());
}

void MainWindow::on_find() {
  Gtk::Dialog dlg("Find", *this, true);
  dlg.add_button("_Close", Gtk::RESPONSE_CLOSE);
  dlg.add_button("Find _Next", Gtk::RESPONSE_ACCEPT);
  dlg.set_default_response(Gtk::RESPONSE_ACCEPT);

  auto* box = dlg.get_content_area();
  box->set_spacing(8);
  box->set_border_width(10);
  auto* label = Gtk::manage(new Gtk::Label("Find what:", true));
  label->set_halign(Gtk::ALIGN_START);
  auto* entry = Gtk::manage(new Gtk::Entry());
  entry->set_text(find_needle_);
  entry->set_activates_default(true);
  box->pack_start(*label, Gtk::PACK_SHRINK);
  box->pack_start(*entry, Gtk::PACK_SHRINK);
  dlg.show_all();

  while (true) {
    const int resp = dlg.run();
    if (resp != Gtk::RESPONSE_ACCEPT) {
      break;
    }
    find_needle_ = entry->get_text();
    if (find_needle_.empty()) {
      continue;
    }
    if (!find_text(false)) {
      Gtk::MessageDialog miss(dlg, "Text not found.", false, Gtk::MESSAGE_INFO,
                              Gtk::BUTTONS_OK, true);
      miss.set_secondary_text(find_needle_);
      miss.run();
    }
  }
}

void MainWindow::on_find_next() {
  if (find_needle_.empty()) {
    on_find();
    return;
  }
  if (!find_text(true)) {
    Gtk::MessageDialog miss(*this, "Text not found.", false, Gtk::MESSAGE_INFO,
                            Gtk::BUTTONS_OK, true);
    miss.set_secondary_text(find_needle_);
    miss.run();
  }
}

bool MainWindow::find_text(bool from_next) {
  auto buf = text_view_.get_buffer();
  Gtk::TextIter start = buf->get_iter_at_mark(buf->get_insert());
  if (from_next) {
    start.forward_char();
  }
  Gtk::TextIter match_start, match_end;
  Gtk::TextSearchFlags flags = Gtk::TEXT_SEARCH_TEXT_ONLY;
  if (!find_case_sensitive_) {
    flags |= Gtk::TEXT_SEARCH_CASE_INSENSITIVE;
  }
  bool found =
      start.forward_search(find_needle_, flags, match_start, match_end);
  if (!found) {
    // Wrap around.
    found = buf->begin().forward_search(find_needle_, flags, match_start,
                                        match_end);
  }
  if (found) {
    buf->select_range(match_start, match_end);
    text_view_.scroll_to(match_start);
    update_status();
    return true;
  }
  return false;
}

void MainWindow::on_toggle_wrap() {
  if (!wrap_item_) {
    return;
  }
  text_view_.set_wrap_mode(wrap_item_->get_active() ? Gtk::WRAP_WORD_CHAR
                                                    : Gtk::WRAP_NONE);
  if (gutter_) {
    gutter_->queue_draw();
  }
}

void MainWindow::on_toggle_line_numbers() {
  if (!gutter_ || !line_numbers_item_) {
    return;
  }
  gutter_->set_visible_gutter(line_numbers_item_->get_active());
}

void MainWindow::apply_tab_width(int spaces) {
  tab_width_ = spaces;
  // Approximate tab stops via pango tabs: width of N spaces in current font.
  auto layout = text_view_.create_pango_layout(std::string(spaces, ' '));
  layout->set_font_description(font_desc_);
  int tw = 0, th = 0;
  layout->get_pixel_size(tw, th);
  Pango::TabArray tabs(1, true);
  tabs.set_tab(0, Pango::TAB_LEFT, tw);
  text_view_.set_tabs(tabs);
  (void)th;
}

void MainWindow::on_tab_width() {
  Gtk::Dialog dlg("Tab Width", *this, true);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_OK", Gtk::RESPONSE_OK);
  dlg.set_default_response(Gtk::RESPONSE_OK);

  auto* box = dlg.get_content_area();
  box->set_spacing(6);
  box->set_border_width(10);
  auto* label = Gtk::manage(new Gtk::Label("Spaces per tab:"));
  label->set_halign(Gtk::ALIGN_START);
  box->pack_start(*label, Gtk::PACK_SHRINK);

  Gtk::RadioButton::Group group;
  auto* r2 = Gtk::manage(new Gtk::RadioButton(group, "2"));
  auto* r4 = Gtk::manage(new Gtk::RadioButton(group, "4"));
  auto* r8 = Gtk::manage(new Gtk::RadioButton(group, "8"));
  if (tab_width_ == 2) {
    r2->set_active(true);
  } else if (tab_width_ == 8) {
    r8->set_active(true);
  } else {
    r4->set_active(true);
  }
  box->pack_start(*r2, Gtk::PACK_SHRINK);
  box->pack_start(*r4, Gtk::PACK_SHRINK);
  box->pack_start(*r8, Gtk::PACK_SHRINK);
  dlg.show_all();

  if (dlg.run() == Gtk::RESPONSE_OK) {
    int w = 4;
    if (r2->get_active()) {
      w = 2;
    } else if (r8->get_active()) {
      w = 8;
    }
    apply_tab_width(w);
  }
}

void MainWindow::apply_font(const Pango::FontDescription& desc) {
  font_desc_ = desc;
  text_view_.override_font(desc);
  apply_tab_width(tab_width_);
  if (gutter_) {
    gutter_->refresh();
  }
}

void MainWindow::on_font() {
  Gtk::FontChooserDialog dlg("Font", *this);
  dlg.set_font_desc(font_desc_);
  if (dlg.run() == Gtk::RESPONSE_OK) {
    apply_font(dlg.get_font_desc());
  }
}

void MainWindow::on_about() {
  Gtk::AboutDialog dlg;
  dlg.set_transient_for(*this);
  dlg.set_program_name("Lunduke Edit");
  dlg.set_version("0.1");
  dlg.set_copyright("© 2026 The Lunduke Journal");
  dlg.set_website("https://lunduke.com");
  dlg.set_website_label("lunduke.com");
  dlg.set_license_type(Gtk::LICENSE_GPL_3_0);
  dlg.set_comments(
      "A light text editor for the Lunduke Computer Operating System.");
  dlg.set_logo_icon_name("accessories-text-editor");
  std::vector<Glib::ustring> authors{"The Lunduke Journal"};
  dlg.set_authors(authors);
  dlg.run();
}

}  // namespace lundukeedit
