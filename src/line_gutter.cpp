// SPDX-License-Identifier: GPL-3.0-or-later

#include "line_gutter.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace lundukeedit {

LineGutter::LineGutter(Gtk::TextView& text_view) : text_view_(text_view) {
  set_size_request(36, -1);
  set_hexpand(false);
  set_vexpand(true);

  auto vadj = text_view_.get_vadjustment();
  if (vadj) {
    vadj_changed_ = vadj->signal_value_changed().connect(
        sigc::mem_fun(*this, &LineGutter::on_vadj_changed));
    vadj->signal_changed().connect(
        sigc::mem_fun(*this, &LineGutter::on_vadj_changed));
  }

  auto buf = text_view_.get_buffer();
  buffer_changed_ = buf->signal_changed().connect(
      sigc::mem_fun(*this, &LineGutter::on_buffer_changed));
  mark_set_ = buf->signal_mark_set().connect(
      [this](const Gtk::TextBuffer::iterator&,
             const Glib::RefPtr<Gtk::TextBuffer::Mark>&) {
        queue_draw();
      });

  text_view_.signal_size_allocate().connect_notify(
      [this](Gtk::Allocation&) { queue_draw(); });

  update_width();
}

void LineGutter::set_visible_gutter(bool visible) {
  visible_ = visible;
  set_visible(visible);
  if (visible) {
    update_width();
  }
}

void LineGutter::refresh() {
  update_width();
  queue_draw();
}

void LineGutter::on_buffer_changed() {
  update_width();
  queue_draw();
}

void LineGutter::on_vadj_changed() { queue_draw(); }

void LineGutter::on_size_allocate(Gtk::Allocation& allocation) {
  Gtk::DrawingArea::on_size_allocate(allocation);
  queue_draw();
}

void LineGutter::update_width() {
  auto buf = text_view_.get_buffer();
  const int lines = std::max(1, buf->get_line_count());
  int digits = 1;
  for (int n = lines; n >= 10; n /= 10) {
    ++digits;
  }

  auto layout = create_pango_layout("0");
  auto desc = text_view_.get_style_context()->get_font(
      text_view_.get_style_context()->get_state());
  layout->set_font_description(desc);
  int tw = 0, th = 0;
  layout->get_pixel_size(tw, th);
  digit_width_ = std::max(6, tw);

  const int pad = 10;
  const int width = digits * digit_width_ + pad;
  set_size_request(width, -1);
}

bool LineGutter::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
  const int width = get_allocated_width();
  const int height = get_allocated_height();

  // Classic light-gray gutter.
  cr->set_source_rgb(0.90, 0.90, 0.90);
  cr->rectangle(0, 0, width, height);
  cr->fill();

  // Right edge separator.
  cr->set_source_rgb(0.70, 0.70, 0.70);
  cr->set_line_width(1.0);
  cr->move_to(width - 0.5, 0);
  cr->line_to(width - 0.5, height);
  cr->stroke();

  auto buf = text_view_.get_buffer();
  auto vadj = text_view_.get_vadjustment();
  const double scroll_y = vadj ? vadj->get_value() : 0.0;

  Gdk::Rectangle visible_rect;
  text_view_.get_visible_rect(visible_rect);

  Gtk::TextIter start;
  text_view_.get_iter_at_location(start, visible_rect.get_x(),
                                  visible_rect.get_y());
  start.set_line_offset(0);

  auto font_desc = text_view_.get_style_context()->get_font(
      text_view_.get_style_context()->get_state());

  cr->set_source_rgb(0.35, 0.35, 0.40);

  Gtk::TextIter iter = start;
  // Walk a few lines above the visible top in case of partial lines.
  if (iter.get_line() > 0) {
    iter.backward_line();
  }

  const int last_line = buf->get_line_count() - 1;
  while (true) {
    Gdk::Rectangle loc;
    text_view_.get_iter_location(iter, loc);

    // Convert buffer coords -> widget coords for the text view, then
    // map Y into the gutter (same scroll offset).
    int wx = 0, wy = 0;
    text_view_.buffer_to_window_coords(Gtk::TEXT_WINDOW_TEXT, loc.get_x(),
                                       loc.get_y(), wx, wy);

    // wy is already relative to the text window (scrolled). Gutter shares
    // the same vertical space as the scrolled text allocation.
    const int y = wy;

    if (y > height + loc.get_height()) {
      break;
    }

    if (y + loc.get_height() >= 0) {
      const int line_no = iter.get_line() + 1;
      auto layout = create_pango_layout(std::to_string(line_no));
      layout->set_font_description(font_desc);
      int tw = 0, th = 0;
      layout->get_pixel_size(tw, th);
      const int x = width - tw - 6;
      const int text_y = y + std::max(0, (loc.get_height() - th) / 2);
      cr->move_to(x, text_y);
      layout->show_in_cairo_context(cr);
    }

    if (iter.get_line() >= last_line) {
      break;
    }
    if (!iter.forward_line()) {
      break;
    }
  }

  (void)scroll_y;
  return true;
}

}  // namespace lundukeedit
