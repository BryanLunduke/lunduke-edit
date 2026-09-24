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

  // Left fields + right buttons: per-child margins so label→entry can stay tight
  // while checkbox/button bands use the wider dialog spacing.
  constexpr int kBandSpacing = 14;
  constexpr int kLabelEntryGap = 3;
  auto* outer = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 14));
  content->pack_start(*outer, Gtk::PACK_EXPAND_WIDGET);

  auto* left = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
  auto* buttons =
      Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
  buttons->set_valign(Gtk::ALIGN_FILL);
  buttons->set_vexpand(true);
  outer->pack_start(*left, Gtk::PACK_EXPAND_WIDGET);
  outer->pack_start(*buttons, Gtk::PACK_SHRINK);

  auto* search_label = Gtk::manage(new Gtk::Label("Search For:", true));
  search_label->set_halign(Gtk::ALIGN_START);

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

  // Second checkbox column shares one left edge (under Match Entire Words).
  auto col1_sg = Gtk::SizeGroup::create(Gtk::SIZE_GROUP_HORIZONTAL);
  col1_sg->add_widget(start_at_top_);
  col1_sg->add_widget(wrap_around_);
  col1_sg->add_widget(search_backwards_);
  size_groups_.push_back(col1_sg);

  auto band = [&](Gtk::Widget& left_w, Gtk::Widget& right_w, int margin_top) {
    left_w.set_margin_top(margin_top);
    right_w.set_margin_top(margin_top);
    left_w.set_valign(Gtk::ALIGN_CENTER);
    right_w.set_valign(Gtk::ALIGN_CENTER);
    auto sg = Gtk::SizeGroup::create(Gtk::SIZE_GROUP_VERTICAL);
    sg->add_widget(left_w);
    sg->add_widget(right_w);
    size_groups_.push_back(sg);
    left->pack_start(left_w, Gtk::PACK_SHRINK);
    buttons->pack_start(right_w, Gtk::PACK_SHRINK);
  };

  // Label alone on the left; empty pad shifts the whole button column down so
  // Find lines up with the Search For entry (not the label).
  auto* label_pad = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
  band(*search_label, *label_pad, 0);

  // Find ↔ Search For entry; tight gap under the label (not kBandSpacing).
  band(search_entry_, *find_btn_, kLabelEntryGap);

  // Remaining buttons stay size-grouped (banded) with left rows. Column shift
  // places Find All on the first checkbox row; Replace / Replace All / Don't
  // Find keep vertical banding with the rows beside them. Gap from entry to
  // first checkbox is plain kBandSpacing (no stretched search-block slack).
  band(*make_opt_row(start_at_top_, search_selection_only_), *find_all_btn_,
       kBandSpacing);
  band(*make_opt_row(wrap_around_, extend_selection_), *replace_btn_,
       kBandSpacing);
  band(*make_opt_row(search_backwards_, entire_word_), *replace_all_btn_,
       kBandSpacing);

  case_sensitive_.set_halign(Gtk::ALIGN_START);
  band(case_sensitive_, *dont_find_btn_, kBandSpacing);

  // Replace With: tight label→entry (same gap as Search For).
  auto* replace_label = Gtk::manage(new Gtk::Label("Replace With:", true));
  replace_label->set_halign(Gtk::ALIGN_START);
  auto* replace_block =
      Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, kLabelEntryGap));
  replace_block->pack_start(*replace_label, Gtk::PACK_SHRINK);
  replace_block->pack_start(replace_entry_, Gtk::PACK_SHRINK);
  replace_block->set_margin_top(kBandSpacing);
  left->pack_start(*replace_block, Gtk::PACK_SHRINK);

  // Cancel at bottom-right, below Replace With — not beside Case Sensitive.
  auto* btn_spacer =
      Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
  btn_spacer->set_vexpand(true);
  buttons->pack_start(*btn_spacer, Gtk::PACK_EXPAND_WIDGET);
  cancel_btn_->set_margin_top(kBandSpacing);
  buttons->pack_start(*cancel_btn_, Gtk::PACK_SHRINK);

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
