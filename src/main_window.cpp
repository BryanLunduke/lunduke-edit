// SPDX-License-Identifier: GPL-3.0-or-later

#include "main_window.hpp"
#include "application.hpp"

#include <giomm/file.h>
#include <gtkmm/recentmanager.h>
#include <glib.h>
#include <glibmm/convert.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
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

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace lundukeedit {
namespace {

const char* kVersion = "0.2";

const char* kSampleText =
    "LUNDUKE EDIT 0.2.\n"
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

std::string recents_path() {
  const std::string dir =
      Glib::build_filename(Glib::get_user_config_dir(), "lunduke-edit");
  g_mkdir_with_parents(dir.c_str(), 0700);
  return Glib::build_filename(dir, "recents.txt");
}

}  // namespace

MainWindow::MainWindow(Application& app) : app_(app) {
  set_title("Untitled — Lunduke Edit");
  set_default_size(640, 420);
  set_border_width(0);
  set_icon_name("accessories-text-editor");
  Gtk::Window::set_default_icon_name("accessories-text-editor");

  font_desc_ = Pango::FontDescription("Monospace 11");

  auto buf = Gsv::Buffer::create();
  buf->set_max_undo_levels(100);
  text_view_.set_buffer(buf);
  find_tag_ = buf->create_tag("lunduke-find-hit");
  find_tag_->property_background() = "#c4d8f0";

  // Load persisted recents.
  try {
    const std::string path = recents_path();
    if (Glib::file_test(path, Glib::FILE_TEST_EXISTS)) {
      std::ifstream in(path);
      std::string line;
      while (std::getline(in, line) &&
             static_cast<int>(recents_.size()) < kMaxRecents) {
        if (!line.empty()) {
          recents_.push_back(line);
        }
      }
    }
  } catch (...) {
  }

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
  text_view_.set_show_line_marks(false);
  text_view_.set_auto_indent(false);
  text_view_.set_highlight_current_line(false);

  buf->signal_changed().connect(
      sigc::mem_fun(*this, &MainWindow::on_buffer_changed));
  buf->signal_mark_set().connect(
      sigc::mem_fun(*this, &MainWindow::on_cursor_moved));
  // Undo manager state changes with edits; refresh on changed + idle.
  buf->signal_changed().connect(
      sigc::mem_fun(*this, &MainWindow::update_undo_redo_sensitivity));

  show_all_children();
  update_status();
  update_undo_redo_sensitivity();
  rebuild_recents_menu();
}

Glib::RefPtr<Gsv::Buffer> MainWindow::buffer() {
  return Glib::RefPtr<Gsv::Buffer>::cast_static(text_view_.get_buffer());
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

  status_box_.set_border_width(2);
  status_box_.set_spacing(2);

  status_pos_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_mode_frame_.set_shadow_type(Gtk::SHADOW_IN);
  status_enc_frame_.set_shadow_type(Gtk::SHADOW_IN);
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

  status_enc_.set_halign(Gtk::ALIGN_CENTER);
  status_enc_.set_margin_start(8);
  status_enc_.set_margin_end(8);
  status_enc_.set_margin_top(2);
  status_enc_.set_margin_bottom(2);

  status_bytes_.set_halign(Gtk::ALIGN_END);
  status_bytes_.set_margin_start(6);
  status_bytes_.set_margin_end(6);
  status_bytes_.set_margin_top(2);
  status_bytes_.set_margin_bottom(2);

  status_pos_frame_.add(status_pos_);
  status_mode_frame_.add(status_mode_);
  status_enc_frame_.add(status_enc_);
  status_bytes_frame_.add(status_bytes_);

  status_box_.pack_start(status_pos_frame_, Gtk::PACK_EXPAND_WIDGET);
  status_box_.pack_start(status_mode_frame_, Gtk::PACK_SHRINK);
  status_box_.pack_start(status_enc_frame_, Gtk::PACK_SHRINK);
  status_box_.pack_start(status_bytes_frame_, Gtk::PACK_SHRINK);

  root_.pack_start(status_box_, Gtk::PACK_SHRINK);
}

void MainWindow::build_menus() {
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

  auto* recent_item = Gtk::manage(new Gtk::MenuItem("Open _Recent", true));
  recents_menu_ = Gtk::manage(new Gtk::Menu());
  recent_item->set_submenu(*recents_menu_);
  file_menu->append(*recent_item);

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

  undo_item_ = Gtk::manage(new Gtk::MenuItem("_Undo", true));
  undo_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_undo));
  undo_item_->add_accelerator("activate", get_accel_group(), GDK_KEY_z,
                              Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*undo_item_);

  redo_item_ = Gtk::manage(new Gtk::MenuItem("_Redo", true));
  redo_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_redo));
  redo_item_->add_accelerator("activate", get_accel_group(), GDK_KEY_z,
                              Gdk::CONTROL_MASK | Gdk::SHIFT_MASK,
                              Gtk::ACCEL_VISIBLE);
  // Also Ctrl+Y
  redo_item_->add_accelerator("activate", get_accel_group(), GDK_KEY_y,
                              Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  edit_menu->append(*redo_item_);

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

  auto* goto_i = Gtk::manage(new Gtk::MenuItem("_Go to Line…", true));
  goto_i->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_go_to_line));
  goto_i->add_accelerator("activate", get_accel_group(), GDK_KEY_g,
                          Gdk::CONTROL_MASK, Gtk::ACCEL_VISIBLE);
  search_menu->append(*goto_i);

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

  text_menu->append(*Gtk::manage(new Gtk::SeparatorMenuItem()));

  auto* enc_label = Gtk::manage(new Gtk::MenuItem("Encoding"));
  enc_label->set_sensitive(false);
  text_menu->append(*enc_label);

  Gtk::RadioMenuItem::Group enc_group;
  enc_utf8_item_ =
      Gtk::manage(new Gtk::RadioMenuItem(enc_group, "_UTF-8", true));
  enc_utf8_item_->set_active(true);
  enc_utf8_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_encoding_utf8));
  text_menu->append(*enc_utf8_item_);

  enc_latin1_item_ = Gtk::manage(
      new Gtk::RadioMenuItem(enc_group, "_Latin-1 (ISO-8859-1)", true));
  enc_latin1_item_->signal_activate().connect(
      sigc::mem_fun(*this, &MainWindow::on_encoding_latin1));
  text_menu->append(*enc_latin1_item_);

  // ---- Font (top-level) ----
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
  auto buf = buffer();
  buf->begin_not_undoable_action();
  buf->set_text(kSampleText);
  buf->end_not_undoable_action();
  set_dirty(false);
  file_path_.clear();
  file_path_ = "/tmp/readme.txt";
  save_to_path(file_path_);
  set_dirty(false);
  seeding_ = false;
  update_title();
  update_status();
  update_undo_redo_sensitivity();
  if (gutter_) {
    gutter_->refresh();
  }
  auto iter = buf->get_iter_at_line_offset(6, 13);
  if (!iter) {
    iter = buf->begin();
  }
  buf->place_cursor(iter);
  update_status();
}

bool MainWindow::open_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    Gtk::MessageDialog dlg(*this, "Could not open file.", false,
                           Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
    dlg.set_secondary_text(path);
    dlg.run();
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string raw = ss.str();

  Glib::ustring text;
  try {
    if (encoding_ == "UTF-8") {
      if (g_utf8_validate(raw.data(), static_cast<gssize>(raw.size()),
                          nullptr)) {
        text = Glib::ustring(raw);
      } else {
        // Bytes are not valid UTF-8; interpret as Latin-1 so something readable loads.
        text = Glib::convert_with_fallback(raw, "UTF-8", "ISO-8859-1", "?");
      }
    } else {
      text = Glib::convert_with_fallback(raw, "UTF-8", encoding_, "?");
    }
  } catch (const Glib::ConvertError& e) {
    Gtk::MessageDialog dlg(*this, "Encoding error while opening.", false,
                           Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
    dlg.set_secondary_text(e.what());
    dlg.run();
    return false;
  }

  seeding_ = true;
  auto buf = buffer();
  buf->begin_not_undoable_action();
  clear_find_highlights();
  buf->set_text(text);
  buf->end_not_undoable_action();
  file_path_ = path;
  set_dirty(false);
  seeding_ = false;
  update_title();
  update_status();
  update_undo_redo_sensitivity();
  if (gutter_) {
    gutter_->refresh();
  }
  remember_recent(path);
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
  status_enc_.set_text(encoding_ == "ISO-8859-1" ? "Latin-1" : encoding_);

  const Glib::ustring text = buf->get_text();
  status_bytes_.set_text(format_bytes(text.bytes()));
}

void MainWindow::update_undo_redo_sensitivity() {
  auto buf = buffer();
  if (undo_item_) {
    undo_item_->set_sensitive(buf && buf->can_undo());
  }
  if (redo_item_) {
    redo_item_->set_sensitive(buf && buf->can_redo());
  }
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
  return true;
}

void MainWindow::on_new() {
  if (!confirm_discard_or_save()) {
    return;
  }
  seeding_ = true;
  auto buf = buffer();
  buf->begin_not_undoable_action();
  clear_find_highlights();
  buf->set_text("");
  buf->end_not_undoable_action();
  file_path_.clear();
  set_dirty(false);
  seeding_ = false;
  update_title();
  update_status();
  update_undo_redo_sensitivity();
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

void MainWindow::on_open_recent(const std::string& path) {
  if (!confirm_discard_or_save()) {
    return;
  }
  open_file(path);
}

bool MainWindow::save_to_path(const std::string& path) {
  const Glib::ustring text = text_view_.get_buffer()->get_text();
  std::string out_bytes;
  try {
    if (encoding_ == "UTF-8") {
      out_bytes.assign(text.data(), text.bytes());
    } else {
      out_bytes = Glib::convert(text, encoding_, "UTF-8");
    }
  } catch (const Glib::ConvertError& e) {
    Gtk::MessageDialog dlg(*this, "Encoding error while saving.", false,
                           Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
    dlg.set_secondary_text(e.what());
    dlg.run();
    return false;
  }

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    Gtk::MessageDialog dlg(*this, "Could not save file.", false,
                           Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
    dlg.set_secondary_text(path);
    dlg.run();
    return false;
  }
  out.write(out_bytes.data(),
            static_cast<std::streamsize>(out_bytes.size()));
  file_path_ = path;
  set_dirty(false);
  update_status();
  remember_recent(path);
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
    return false;
  }
  return true;
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
  auto buf = buffer();
  if (buf && buf->can_undo()) {
    buf->undo();
    update_undo_redo_sensitivity();
    update_status();
  }
}

void MainWindow::on_redo() {
  auto buf = buffer();
  if (buf && buf->can_redo()) {
    buf->redo();
    update_undo_redo_sensitivity();
    update_status();
  }
}

void MainWindow::on_cut() {
  auto clip = Gtk::Clipboard::get();
  text_view_.get_buffer()->cut_clipboard(clip);
  update_undo_redo_sensitivity();
}

void MainWindow::on_copy() {
  auto clip = Gtk::Clipboard::get();
  text_view_.get_buffer()->copy_clipboard(clip);
}

void MainWindow::on_paste() {
  auto clip = Gtk::Clipboard::get();
  text_view_.get_buffer()->paste_clipboard(clip);
  update_undo_redo_sensitivity();
}

void MainWindow::on_select_all() {
  auto buf = text_view_.get_buffer();
  buf->select_range(buf->begin(), buf->end());
}

void MainWindow::remember_recent(const std::string& path) {
  if (path.empty()) {
    return;
  }
  recents_.erase(std::remove(recents_.begin(), recents_.end(), path),
                 recents_.end());
  recents_.insert(recents_.begin(), path);
  if (static_cast<int>(recents_.size()) > kMaxRecents) {
    recents_.resize(static_cast<std::size_t>(kMaxRecents));
  }
  try {
    const std::string uri = Glib::filename_to_uri(path);
    Gtk::RecentManager::get_default()->add_item(uri);
  } catch (...) {
  }
  try {
    std::ofstream out(recents_path(), std::ios::trunc);
    for (const auto& p : recents_) {
      out << p << '\n';
    }
  } catch (...) {
  }
  rebuild_recents_menu();
}

void MainWindow::rebuild_recents_menu() {
  if (!recents_menu_) {
    return;
  }
  for (auto* child : recents_menu_->get_children()) {
    recents_menu_->remove(*child);
  }
  if (recents_.empty()) {
    auto* empty = Gtk::manage(new Gtk::MenuItem("(No recent files)"));
    empty->set_sensitive(false);
    recents_menu_->append(*empty);
  } else {
    for (const auto& path : recents_) {
      auto* item = Gtk::manage(new Gtk::MenuItem(path));
      item->signal_activate().connect(
          [this, path]() { on_open_recent(path); });
      recents_menu_->append(*item);
    }
  }
  recents_menu_->show_all();
}

void MainWindow::set_encoding(const std::string& encoding) {
  encoding_ = encoding;
  update_status();
}

void MainWindow::on_encoding_utf8() {
  if (enc_utf8_item_ && enc_utf8_item_->get_active()) {
    set_encoding("UTF-8");
  }
}

void MainWindow::on_encoding_latin1() {
  if (enc_latin1_item_ && enc_latin1_item_->get_active()) {
    set_encoding("ISO-8859-1");
  }
}

Gtk::TextSearchFlags MainWindow::search_flags(const FindOptions& opts) const {
  Gtk::TextSearchFlags flags = Gtk::TEXT_SEARCH_TEXT_ONLY;
  if (!opts.case_sensitive) {
    flags |= Gtk::TEXT_SEARCH_CASE_INSENSITIVE;
  }
  return flags;
}

bool MainWindow::is_entire_word(const Gtk::TextIter& start,
                                const Gtk::TextIter& end) const {
  if (start.editable() && !start.starts_word() && !start.inside_word()) {
    // starts at non-word is ok if previous isn't word char — use starts_word
  }
  auto s = start;
  auto e = end;
  // Word boundary: start is start-of-word or start-of-buffer / non-word before
  bool left_ok = s.starts_word() || s.is_start();
  if (!left_ok) {
    auto prev = s;
    if (prev.backward_char()) {
      gunichar c = prev.get_char();
      left_ok = !g_unichar_isalnum(c) && c != '_' && c != '-';
    } else {
      left_ok = true;
    }
  }
  bool right_ok = e.ends_word() || e.is_end();
  if (!right_ok) {
    gunichar c = e.get_char();
    right_ok = (c == 0) || (!g_unichar_isalnum(c) && c != '_' && c != '-');
  }
  return left_ok && right_ok;
}

void MainWindow::get_search_bounds(const FindOptions& opts, Gtk::TextIter& begin,
                                   Gtk::TextIter& end) {
  auto buf = text_view_.get_buffer();
  if (opts.search_selection_only) {
    Gtk::TextIter sel_a, sel_b;
    if (buf->get_selection_bounds(sel_a, sel_b)) {
      begin = sel_a;
      end = sel_b;
      return;
    }
  }
  begin = buf->begin();
  end = buf->end();
}

bool MainWindow::find_match(const FindOptions& opts, bool from_next) {
  if (opts.search_for.empty()) {
    return false;
  }
  auto buf = text_view_.get_buffer();
  const auto flags = search_flags(opts);

  Gtk::TextIter range_begin, range_end;
  get_search_bounds(opts, range_begin, range_end);

  Gtk::TextIter start;
  if (opts.start_at_top && !from_next) {
    start = range_begin;
  } else {
    start = buf->get_iter_at_mark(buf->get_insert());
    if (opts.search_selection_only) {
      if (start < range_begin || start > range_end) {
        start = opts.search_backwards ? range_end : range_begin;
      }
    }
    if (from_next || (!opts.start_at_top)) {
      // Move past current selection if it is the needle.
      Gtk::TextIter sel_a, sel_b;
      if (buf->get_selection_bounds(sel_a, sel_b) &&
          sel_a.get_text(sel_b) == opts.search_for) {
        start = opts.search_backwards ? sel_a : sel_b;
      } else if (from_next) {
        if (opts.search_backwards) {
          start.backward_char();
        } else {
          start.forward_char();
        }
      }
    }
  }

  auto try_search = [&](const Gtk::TextIter& from, bool wrap_pass) -> bool {
    Gtk::TextIter match_start, match_end;
    Gtk::TextIter cursor = from;
    const int guard = buf->get_char_count() + 2;
    for (int i = 0; i < guard; ++i) {
      bool found = false;
      if (opts.search_backwards) {
        found = cursor.backward_search(opts.search_for, flags, match_start,
                                       match_end, range_begin);
      } else {
        found = cursor.forward_search(opts.search_for, flags, match_start,
                                      match_end, range_end);
      }
      if (!found) {
        return false;
      }
      if (!opts.search_selection_only ||
          (match_start >= range_begin && match_end <= range_end)) {
        if (!opts.entire_word || is_entire_word(match_start, match_end)) {
          if (opts.extend_selection) {
            Gtk::TextIter sel_a, sel_b;
            if (buf->get_selection_bounds(sel_a, sel_b)) {
              if (match_start < sel_a) {
                sel_a = match_start;
              }
              if (match_end > sel_b) {
                sel_b = match_end;
              }
              buf->select_range(sel_a, sel_b);
            } else {
              buf->select_range(match_start, match_end);
            }
          } else {
            buf->select_range(match_start, match_end);
          }
          text_view_.scroll_to(match_start);
          update_status();
          return true;
        }
      }
      cursor = opts.search_backwards ? match_start : match_end;
      if (opts.search_backwards) {
        if (!cursor.backward_char()) {
          break;
        }
      }
      (void)wrap_pass;
    }
    return false;
  };

  if (try_search(start, false)) {
    return true;
  }
  if (opts.wrap_around && !opts.search_selection_only) {
    Gtk::TextIter wrap_from = opts.search_backwards ? range_end : range_begin;
    return try_search(wrap_from, true);
  }
  if (opts.wrap_around && opts.search_selection_only) {
    Gtk::TextIter wrap_from = opts.search_backwards ? range_end : range_begin;
    return try_search(wrap_from, true);
  }
  return false;
}

int MainWindow::count_matches(const FindOptions& opts) {
  if (opts.search_for.empty()) {
    return 0;
  }
  auto buf = text_view_.get_buffer();
  const auto flags = search_flags(opts);
  Gtk::TextIter range_begin, range_end;
  get_search_bounds(opts, range_begin, range_end);

  int count = 0;
  Gtk::TextIter cursor = range_begin;
  Gtk::TextIter match_start, match_end;
  while (cursor.forward_search(opts.search_for, flags, match_start, match_end,
                               range_end)) {
    if (!opts.entire_word || is_entire_word(match_start, match_end)) {
      ++count;
    }
    cursor = match_end;
    if (match_start == match_end) {
      if (!cursor.forward_char()) {
        break;
      }
    }
  }
  return count;
}

void MainWindow::clear_find_highlights() {
  auto buf = text_view_.get_buffer();
  if (find_tag_) {
    buf->remove_tag(find_tag_, buf->begin(), buf->end());
  }
}

void MainWindow::highlight_all_matches(const FindOptions& opts) {
  clear_find_highlights();
  if (opts.search_for.empty() || !find_tag_) {
    return;
  }
  auto buf = text_view_.get_buffer();
  const auto flags = search_flags(opts);
  Gtk::TextIter range_begin, range_end;
  get_search_bounds(opts, range_begin, range_end);

  Gtk::TextIter cursor = range_begin;
  Gtk::TextIter match_start, match_end;
  Gtk::TextIter first_start, first_end;
  bool have_first = false;
  while (cursor.forward_search(opts.search_for, flags, match_start, match_end,
                               range_end)) {
    if (!opts.entire_word || is_entire_word(match_start, match_end)) {
      buf->apply_tag(find_tag_, match_start, match_end);
      if (!have_first) {
        first_start = match_start;
        first_end = match_end;
        have_first = true;
      }
    }
    cursor = match_end;
    if (match_start == match_end) {
      if (!cursor.forward_char()) {
        break;
      }
    }
  }
  if (have_first) {
    buf->select_range(first_start, first_end);
    text_view_.scroll_to(first_start);
  }
}

bool MainWindow::replace_current(const FindOptions& opts) {
  auto buf = text_view_.get_buffer();
  Gtk::TextIter sel_a, sel_b;
  if (buf->get_selection_bounds(sel_a, sel_b)) {
    Glib::ustring selected = sel_a.get_text(sel_b);
    Glib::ustring needle = opts.search_for;
    if (!opts.case_sensitive) {
      selected = selected.casefold();
      needle = needle.casefold();
    }
    if (selected == needle &&
        (!opts.entire_word || is_entire_word(sel_a, sel_b))) {
      buf->erase(sel_a, sel_b);
      // Re-get insert after erase
      auto insert = buf->get_iter_at_mark(buf->get_insert());
      buf->insert(insert, opts.replace_with);
      update_undo_redo_sensitivity();
      return find_match(opts, true) || true;
    }
  }
  return find_match(opts, false);
}

int MainWindow::replace_all(const FindOptions& opts) {
  if (opts.search_for.empty()) {
    return 0;
  }
  auto buf = text_view_.get_buffer();
  const auto flags = search_flags(opts);
  Gtk::TextIter range_begin, range_end;
  get_search_bounds(opts, range_begin, range_end);

  // Collect offsets first so indices stay valid while we edit from end.
  struct Hit {
    int start_off;
    int end_off;
  };
  std::vector<Hit> hits;
  Gtk::TextIter cursor = range_begin;
  Gtk::TextIter match_start, match_end;
  while (cursor.forward_search(opts.search_for, flags, match_start, match_end,
                               range_end)) {
    if (!opts.entire_word || is_entire_word(match_start, match_end)) {
      hits.push_back({match_start.get_offset(), match_end.get_offset()});
    }
    cursor = match_end;
    if (match_start == match_end) {
      if (!cursor.forward_char()) {
        break;
      }
    }
  }

  buf->begin_user_action();
  for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
    auto a = buf->get_iter_at_offset(it->start_off);
    auto b = buf->get_iter_at_offset(it->end_off);
    buf->erase(a, b);
    a = buf->get_iter_at_offset(it->start_off);
    buf->insert(a, opts.replace_with);
  }
  buf->end_user_action();
  clear_find_highlights();
  update_undo_redo_sensitivity();
  update_status();
  return static_cast<int>(hits.size());
}

void MainWindow::on_find() {
  FindReplaceDialog dlg(*this, find_opts_);
  dlg.on_action = [this, &dlg](FindReplaceDialog::Action action,
                               const FindOptions& opts) -> bool {
    find_opts_ = opts;
    switch (action) {
      case FindReplaceDialog::Action::Find: {
        FindOptions o = opts;
        // After the first Find from the dialog, subsequent Finds act as Find Next.
        static thread_local bool dummy = false;
        (void)dummy;
        if (!find_match(o, false)) {
          Gtk::MessageDialog miss(dlg, "Text not found.", false,
                                  Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
          miss.set_secondary_text(opts.search_for);
          miss.run();
          return false;
        }
        // Turn off start-at-top for follow-up finds within same dialog session.
        return true;
      }
      case FindReplaceDialog::Action::FindAll: {
        const int n = count_matches(opts);
        highlight_all_matches(opts);
        Gtk::MessageDialog info(dlg,
                                "Found " + std::to_string(n) +
                                    (n == 1 ? " match." : " matches."),
                                false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK,
                                true);
        info.run();
        status_pos_.set_text(status_pos_.get_text() + "  [" +
                             std::to_string(n) + " matches]");
        return n > 0;
      }
      case FindReplaceDialog::Action::Replace: {
        if (!replace_current(opts)) {
          Gtk::MessageDialog miss(dlg, "Text not found.", false,
                                  Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
          miss.set_secondary_text(opts.search_for);
          miss.run();
          return false;
        }
        return true;
      }
      case FindReplaceDialog::Action::ReplaceAll: {
        const int n = replace_all(opts);
        Gtk::MessageDialog info(dlg,
                                "Replaced " + std::to_string(n) +
                                    (n == 1 ? " occurrence." : " occurrences."),
                                false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK,
                                true);
        info.run();
        return n > 0;
      }
      default:
        return false;
    }
  };

  dlg.run();
  find_opts_ = dlg.options();
  // After first successful find, clear start_at_top for Find Next.
  find_opts_.start_at_top = false;
}

void MainWindow::on_find_next() {
  if (find_opts_.search_for.empty()) {
    on_find();
    return;
  }
  FindOptions o = find_opts_;
  o.start_at_top = false;
  if (!find_match(o, true)) {
    Gtk::MessageDialog miss(*this, "Text not found.", false, Gtk::MESSAGE_INFO,
                            Gtk::BUTTONS_OK, true);
    miss.set_secondary_text(find_opts_.search_for);
    miss.run();
  }
}

void MainWindow::on_go_to_line() {
  Gtk::Dialog dlg("Go to Line", *this, true);
  dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
  dlg.add_button("_Go", Gtk::RESPONSE_OK);
  dlg.set_default_response(Gtk::RESPONSE_OK);

  auto* box = dlg.get_content_area();
  box->set_spacing(8);
  box->set_border_width(10);
  auto* label = Gtk::manage(new Gtk::Label("Line number:", true));
  label->set_halign(Gtk::ALIGN_START);
  auto* entry = Gtk::manage(new Gtk::Entry());
  entry->set_activates_default(true);
  auto buf = text_view_.get_buffer();
  auto iter = buf->get_iter_at_mark(buf->get_insert());
  entry->set_text(std::to_string(iter.get_line() + 1));
  box->pack_start(*label, Gtk::PACK_SHRINK);
  box->pack_start(*entry, Gtk::PACK_SHRINK);
  dlg.show_all();

  if (dlg.run() == Gtk::RESPONSE_OK) {
    try {
      int line = std::stoi(entry->get_text().raw());
      if (line < 1) {
        line = 1;
      }
      const int max_line = buf->get_line_count();
      if (line > max_line) {
        line = max_line;
      }
      auto dest = buf->get_iter_at_line(line - 1);
      buf->place_cursor(dest);
      text_view_.scroll_to(dest);
      update_status();
    } catch (...) {
      Gtk::MessageDialog bad(dlg, "Invalid line number.", false,
                             Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
      bad.run();
    }
  }
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
  auto layout = text_view_.create_pango_layout(std::string(spaces, ' '));
  layout->set_font_description(font_desc_);
  int tw = 0, th = 0;
  layout->get_pixel_size(tw, th);
  Pango::TabArray tabs(1, true);
  tabs.set_tab(0, Pango::TAB_LEFT, tw);
  text_view_.set_tabs(tabs);
  text_view_.set_tab_width(static_cast<guint>(spaces));
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
  dlg.set_version(kVersion);
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
