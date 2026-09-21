// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUNDUKEEDIT_FIND_REPLACE_DIALOG_HPP
#define LUNDUKEEDIT_FIND_REPLACE_DIALOG_HPP

#include <gtkmm/checkbutton.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/window.h>

#include <functional>

namespace lundukeedit {

struct FindOptions {
  Glib::ustring search_for;
  Glib::ustring replace_with;
  bool start_at_top{false};
  bool wrap_around{true};
  bool search_backwards{false};
  bool search_selection_only{false};
  bool extend_selection{false};
  bool case_sensitive{false};
  bool entire_word{false};
};

// BBEdit Lite–inspired Find & Replace dialog (single-document; no grep/multi-file).
class FindReplaceDialog : public Gtk::Dialog {
public:
  enum class Action {
    Find,
    FindAll,
    Replace,
    ReplaceAll,
    DontFind,
    Cancel,
  };

  FindReplaceDialog(Gtk::Window& parent, const FindOptions& initial);

  FindOptions options() const;

  // Called for Find / Find All / Replace / Replace All while the dialog stays open.
  // Return true if the action succeeded (for Find: a match was selected).
  std::function<bool(Action, const FindOptions&)> on_action;

protected:
  void on_response(int response_id) override;

private:
  FindOptions collect() const;

  Gtk::Entry search_entry_;
  Gtk::Entry replace_entry_;

  Gtk::CheckButton start_at_top_{"Start at Top"};
  Gtk::CheckButton wrap_around_{"Wrap Around"};
  Gtk::CheckButton search_backwards_{"Search Backwards"};
  Gtk::CheckButton search_selection_only_{"Search Selection Only"};
  Gtk::CheckButton extend_selection_{"Extend Selection"};
  Gtk::CheckButton case_sensitive_{"Case Sensitive"};
  Gtk::CheckButton entire_word_{"Match Entire Words"};

  Gtk::Button* find_btn_{nullptr};
  Gtk::Button* find_all_btn_{nullptr};
  Gtk::Button* replace_btn_{nullptr};
  Gtk::Button* replace_all_btn_{nullptr};
  Gtk::Button* dont_find_btn_{nullptr};
  Gtk::Button* cancel_btn_{nullptr};
};

}  // namespace lundukeedit

#endif
