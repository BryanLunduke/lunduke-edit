// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only XDND source. Not installed. Offers one text/uri-list file so a
// pointer drag can hit Lunduke Edit's real GTK drop target.

#include <giomm/file.h>
#include <gtkmm/eventbox.h>
#include <gtkmm/label.h>
#include <gtkmm/main.h>
#include <gtkmm/window.h>
#include <gdk/gdkx.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

class DragSource : public Gtk::Window {
 public:
  explicit DragSource(std::string uri) : uri_(std::move(uri)) {
    set_title("dnd-source");
    set_default_size(220, 140);
    set_resizable(false);
    label_.set_text("drag source");
    label_.set_margin_top(40);
    label_.set_margin_bottom(40);
    label_.set_margin_start(20);
    label_.set_margin_end(20);
    // A label has no window of its own. The event box does, so a real
    // button press on the client area starts the drag.
    box_.add(label_);
    add(box_);

    std::vector<Gtk::TargetEntry> targets;
    targets.emplace_back("text/uri-list", Gtk::TargetFlags(0), 0);
    box_.drag_source_set(targets, Gdk::BUTTON1_MASK, Gdk::ACTION_COPY);
    box_.signal_drag_data_get().connect(
        [this](const Glib::RefPtr<Gdk::DragContext>&, Gtk::SelectionData& data,
               guint, guint) {
          std::vector<Glib::ustring> uris;
          uris.emplace_back(uri_);
          data.set_uris(uris);
        });
    box_.signal_drag_begin().connect(
        [](const Glib::RefPtr<Gdk::DragContext>&) {
          std::cout << "DND_DRAG_BEGIN" << std::endl;
        });
    box_.signal_drag_end().connect([](const Glib::RefPtr<Gdk::DragContext>&) {
      std::cout << "DND_DRAG_END" << std::endl;
    });

    signal_map().connect([this]() {
      if (auto win = get_window()) {
        const unsigned long xid = gdk_x11_window_get_xid(win->gobj());
        std::cout << "DND_READY xid=" << xid << std::endl;
      }
    });
    show_all_children();
  }

 private:
  std::string uri_;
  Gtk::EventBox box_;
  Gtk::Label label_;
};

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  if (const char* from_env = g_getenv("DND_SOURCE_PATH")) {
    path = from_env;
  }
  for (int i = 1; i < argc && path.empty(); ++i) {
    if (argv[i] != nullptr && argv[i][0] != '\0' && argv[i][0] != '-') {
      path = argv[i];
    }
  }
  const char* display = g_getenv("DISPLAY");
  if (display == nullptr || display[0] == '\0') {
    std::cerr << "GUI tests require a display; refusing to skip\n";
    return 1;
  }
  if (path.empty()) {
    std::cerr << "dnd source needs a file path\n";
    return 2;
  }
  Gtk::Main kit(argc, argv);
  const std::string uri = Gio::File::create_for_path(path)->get_uri();
  DragSource window(uri);
  window.show();
  kit.run(window);
  return 0;
}
