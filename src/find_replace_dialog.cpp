// SPDX-License-Identifier: GPL-3.0-or-later

#include "find_replace_dialog.hpp"

#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/sizegroup.h>

namespace lundukeedit {

namespace {
enum {
  RESP_FIND = 1,
  RESP_FIND_ALL = 2,
  RESP_REPLACE = 3,
  RESP_REPLACE_ALL = 4,
  RESP_DONT_FIND = 5,
};

Gtk::Box* make_opt_row(Gtk::Widget& a, Gtk::Widget& b) {
  auto* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 16));
  row->set_valign(Gtk::ALIGN_CENTER);
  a.set_valign(Gtk::ALIGN_CENTER);
  b.set_valign(Gtk::ALIGN_CENTER);
  row->pack_start(a, Gtk::PACK_SHRINK);
  row->pack_start(b, Gtk::PACK_SHRINK);
  return row;
}
}  // namespace

FindReplaceDialog::FindReplaceDialog(Gtk::Window& parent,
                                     const FindOptions& initial)
    : Gtk::Dialog("Find & Replace", parent, false /* non-modal friendly */) {
  set_transient_for(parent);
  set_modal(true);
  set_resizable(false);
  set_default_response(RESP_FIND);

  search_entry_.set_text(initial.search_for);
  replace_entry_.set_text(initial.replace_with);
  search_entry_.set_activates_default(true);
  search_entry_.set_width_chars(36);
  search_entry_.set_hexpand(true);
  replace_entry_.set_hexpand(true);

  start_at_top_.set_active(initial.start_at_top);
  wrap_around_.set_active(initial.wrap_around);
  search_backwards_.set_active(initial.search_backwards);
  search_selection_only_.set_active(initial.search_selection_only);
  extend_selection_.set_active(initial.extend_selection);
  case_sensitive_.set_active(initial.case_sensitive);
  entire_word_.set_active(initial.entire_word);

  auto* content = get_content_area();
  content->set_spacing(10);
  content->set_border_width(12);

  // Left fields + right buttons share vertical spacing and per-row height bands.
  constexpr int kBandSpacing = 14;
  auto* outer = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 14));
  content->pack_start(*outer, Gtk::PACK_EXPAND_WIDGET);

  auto* left = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, kBandSpacing));
  auto* buttons =
      Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, kBandSpacing));
  buttons->set_valign(Gtk::ALIGN_START);
  outer->pack_start(*left, Gtk::PACK_EXPAND_WIDGET);
  outer->pack_start(*buttons, Gtk::PACK_SHRINK);

  auto* search_label = Gtk::manage(new Gtk::Label("Search For:", true));
  search_label->set_halign(Gtk::ALIGN_START);
  search_label->set_valign(Gtk::ALIGN_CENTER);

  find_btn_ = Gtk::manage(new Gtk::Button("_Find", true));
  find_all_btn_ = Gtk::manage(new Gtk::Button("Find _All", true));
  replace_btn_ = Gtk::manage(new Gtk::Button("_Replace", true));
  replace_all_btn_ = Gtk::manage(new Gtk::Button("Replace A_ll", true));
  dont_find_btn_ = Gtk::manage(new Gtk::Button("_Don't Find", true));
  cancel_btn_ = Gtk::manage(new Gtk::Button("_Cancel", true));

  for (auto* b : {find_btn_, find_all_btn_, replace_btn_, replace_all_btn_,
                  dont_find_btn_, cancel_btn_}) {
    b->set_size_request(120, -1);
    b->set_valign(Gtk::ALIGN_CENTER);
    b->set_halign(Gtk::ALIGN_FILL);
  }

  auto band = [&](Gtk::Widget& left_w, Gtk::Button& btn) {
    left_w.set_valign(Gtk::ALIGN_CENTER);
    auto sg = Gtk::SizeGroup::create(Gtk::SIZE_GROUP_VERTICAL);
    sg->add_widget(left_w);
    sg->add_widget(btn);
    size_groups_.push_back(sg);
    left->pack_start(left_w, Gtk::PACK_SHRINK);
    buttons->pack_start(btn, Gtk::PACK_SHRINK);
  };

  // Six matched bands (BBEdit-style column), then Replace With below.
  band(*search_label, *find_btn_);
  band(search_entry_, *find_all_btn_);
  band(*make_opt_row(start_at_top_, search_selection_only_), *replace_btn_);
  band(*make_opt_row(wrap_around_, extend_selection_), *replace_all_btn_);
  band(*make_opt_row(search_backwards_, entire_word_), *dont_find_btn_);
  case_sensitive_.set_halign(Gtk::ALIGN_START);
  band(case_sensitive_, *cancel_btn_);

  auto* replace_label = Gtk::manage(new Gtk::Label("Replace With:", true));
  replace_label->set_halign(Gtk::ALIGN_START);
  replace_label->set_margin_top(2);
  left->pack_start(*replace_label, Gtk::PACK_SHRINK);
  left->pack_start(replace_entry_, Gtk::PACK_SHRINK);

  find_btn_->signal_clicked().connect([this]() {
    if (on_action) {
      on_action(Action::Find, collect());
    }
  });
  find_all_btn_->signal_clicked().connect([this]() {
    if (on_action) {
      on_action(Action::FindAll, collect());
    }
  });
  replace_btn_->signal_clicked().connect([this]() {
    if (on_action) {
      on_action(Action::Replace, collect());
    }
  });
  replace_all_btn_->signal_clicked().connect([this]() {
    if (on_action) {
      on_action(Action::ReplaceAll, collect());
    }
  });
  dont_find_btn_->signal_clicked().connect([this]() {
    response(RESP_DONT_FIND);
  });
  cancel_btn_->signal_clicked().connect([this]() {
    response(Gtk::RESPONSE_CANCEL);
  });


  set_default(*find_btn_);

  show_all_children();
  search_entry_.grab_focus();
  search_entry_.select_region(0, -1);
}

FindOptions FindReplaceDialog::options() const { return collect(); }

FindOptions FindReplaceDialog::collect() const {
  FindOptions o;
  o.search_for = search_entry_.get_text();
  o.replace_with = replace_entry_.get_text();
  o.start_at_top = start_at_top_.get_active();
  o.wrap_around = wrap_around_.get_active();
  o.search_backwards = search_backwards_.get_active();
  o.search_selection_only = search_selection_only_.get_active();
  o.extend_selection = extend_selection_.get_active();
  o.case_sensitive = case_sensitive_.get_active();
  o.entire_word = entire_word_.get_active();
  return o;
}

void FindReplaceDialog::on_response(int response_id) {
  if (response_id == RESP_DONT_FIND || response_id == Gtk::RESPONSE_CANCEL ||
      response_id == Gtk::RESPONSE_DELETE_EVENT) {
    hide();
  }
  Gtk::Dialog::on_response(response_id);
}

}  // namespace lundukeedit
