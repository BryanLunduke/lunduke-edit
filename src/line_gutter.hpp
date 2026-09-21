// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUNDUKEEDIT_LINE_GUTTER_HPP
#define LUNDUKEEDIT_LINE_GUTTER_HPP

#include <gtkmm/drawingarea.h>
#include <gtkmm/textview.h>

namespace lundukeedit {

// Narrow left gutter that paints 1-based line numbers, scrolled in sync
// with an associated Gtk::TextView (no GtkSourceView required).
class LineGutter : public Gtk::DrawingArea {
public:
  explicit LineGutter(Gtk::TextView& text_view);

  void set_visible_gutter(bool visible);
  bool gutter_visible() const { return visible_; }

  void refresh();

protected:
  bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
  void on_size_allocate(Gtk::Allocation& allocation) override;

private:
  void on_buffer_changed();
  void on_vadj_changed();
  void update_width();

  Gtk::TextView& text_view_;
  bool visible_{true};
  int digit_width_{8};
  sigc::connection buffer_changed_;
  sigc::connection vadj_changed_;
  sigc::connection mark_set_;
};

}  // namespace lundukeedit

#endif
