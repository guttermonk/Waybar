#pragma once

#include <gdk/gdk.h>
#include <glibmm/iochannel.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <wayland-client.h>

#include <atomic>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "AModule.hpp"
#include "bar.hpp"
#include "client.hpp"
#include "ext-workspace-v1-client-protocol.h"
#include "giomm/desktopappinfo.h"
#ifdef HAVE_HYPRLAND
#include "modules/hyprland/backend.hpp"
#endif
#include "util/icon_loader.hpp"
#include "util/json.hpp"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

namespace waybar::modules::wlr {

struct widget_geometry {
  int x, y, w, h;
};

class Taskbar;

class Task {
 public:
  Task(const waybar::Bar&, const Json::Value&, Taskbar*, struct zwlr_foreign_toplevel_handle_v1*,
       struct wl_seat*);
  ~Task();

 public:
  enum State {
    MAXIMIZED = (1 << 0),
    MINIMIZED = (1 << 1),
    ACTIVE = (1 << 2),
    FULLSCREEN = (1 << 3),
    INVALID = (1 << 4)
  };
  // made public so TaskBar can reorder based on configuration.
  Gtk::Button button;
  struct widget_geometry minimize_hint;

 private:
  static uint32_t global_id;

 private:
  const waybar::Bar& bar_;
  const Json::Value& config_;
  Taskbar* tbar_;
  struct zwlr_foreign_toplevel_handle_v1* handle_;
  struct wl_seat* seat_;

  uint32_t id_;

  Gtk::Box content_;
  Gtk::Image icon_;
  Gtk::Label text_before_;
  Gtk::Label text_after_;
  Glib::RefPtr<Gio::DesktopAppInfo> app_info_;
  bool button_visible_ = false;
  bool ignored_ = false;
  bool squashed_ = false;
  /* Whether the toplevel is on this bar's output, per the protocol's
   * output_enter/output_leave events */
  bool on_bar_output_ = false;
  bool size_allocate_connected_ = false;
  bool app_id_lookup_pending_ = false;
  bool title_lookup_pending_ = false;
  /* The app_id or title that app_info_ was last looked up from */
  std::string lookup_source_;
  /* The entry this task's own app_id/title resolved to, and whether app_info_
   * currently holds a terminal's foreground program instead. A terminal keeps
   * its own entry here so the icon can go back to it when that program exits. */
  Glib::RefPtr<Gio::DesktopAppInfo> own_app_info_;
  bool fg_icon_applied_ = false;

  bool with_icon_ = false;
  bool with_name_ = false;
  std::string format_before_;
  std::string format_after_;

  std::string format_tooltip_;

  std::string name_;
  std::string title_;
  std::string app_id_;
  uint32_t state_ = 0;
  struct ext_workspace_handle_v1* workspace_ = nullptr;

  int32_t drag_start_x;
  int32_t drag_start_y;
  int32_t drag_start_button = -1;

 private:
  std::string repr() const;
  std::string state_string(bool = false) const;
  void set_minimize_hint();
  void on_button_size_allocated(Gtk::Allocation& alloc);
  void hide_if_ignored();
  void hide_if_duplicate();
  void update_duplicates();
  void lookup_app_info();
  void show_button();
  void hide_button();
  bool tryUpdateIconFromTerminalFg();

 public:
  /* Getter functions */
  uint32_t id() const { return id_; }
  std::string title() const { return title_; }
  std::string app_id() const { return app_id_; }
  uint32_t state() const { return state_; }
  bool maximized() const { return state_ & MAXIMIZED; }
  bool minimized() const { return state_ & MINIMIZED; }
  bool active() const { return state_ & ACTIVE; }
  bool fullscreen() const { return state_ & FULLSCREEN; }
  bool visible() const { return button_visible_; }
  struct ext_workspace_handle_v1* workspace() const { return workspace_; }
  void set_workspace(struct ext_workspace_handle_v1* workspace) { workspace_ = workspace; }

 public:
  /* Callbacks for the wlr protocol */
  void handle_title(const char*);
  void handle_app_id(const char*);
  void handle_output_enter(struct wl_output*);
  void handle_output_leave(struct wl_output*);
  void handle_state(struct wl_array*);
  void handle_done();
  void handle_closed();

  /* Callbacks for Gtk events */
  bool handle_clicked(GdkEventButton*);
  bool handle_button_release(GdkEventButton*);
  bool handle_motion_notify(GdkEventMotion*);
  void handle_drag_data_get(const Glib::RefPtr<Gdk::DragContext>& context,
                            Gtk::SelectionData& selection_data, guint info, guint time);
  void handle_drag_data_received(const Glib::RefPtr<Gdk::DragContext>& context, int x, int y,
                                 Gtk::SelectionData selection_data, guint info, guint time);

 public:
  bool operator==(const Task&) const;
  bool operator!=(const Task&) const;

 public:
  void update();
  /* Rasterize the icon again from the already-resolved desktop entry. Public so
   * Taskbar can refresh every task at once when the icon theme changes. */
  void reload_icon();

 public:
  /* Interaction with the tasks */
  void maximize(bool);
  void minimize(bool);
  void activate();
  void fullscreen(bool);
  void close();
};

using TaskPtr = std::unique_ptr<Task>;

/* The Hyprland-specific behaviour (workspace-aware sorting, activation via
 * Hyprland's own dispatch, foreground-process icons) is only built when Waybar
 * is built with Hyprland support. Upstream gates the whole modules/hyprland
 * tree behind -DHAVE_HYPRLAND, so without this guard the taskbar fails to build
 * anywhere that support is off -- including the FreeBSD CI job. */
#ifdef HAVE_HYPRLAND
class Taskbar : public waybar::AModule, public hyprland::EventHandler {
#else
class Taskbar : public waybar::AModule {
#endif
 public:
  struct WorkspaceState {
    Taskbar* taskbar;
    struct ext_workspace_handle_v1* handle;
    uint32_t state = 0;
  };

  Taskbar(const std::string&, const waybar::Bar&, const Json::Value&);
  ~Taskbar();
  void update();
#ifdef HAVE_HYPRLAND
  void onEvent(const std::string& ev) override;
#endif

  /* A redraw is queued on dp, which a burst of events must not flood */
  bool update_queued_ = false;

  /* Set when the icon theme changed and every task icon needs rasterizing
   * again. Deferred to update() rather than done in the signal handler: a theme
   * change touches every task at once, and GTK emits the signal more than once
   * per change. Plain bool, like update_queued_ above -- both are only ever
   * touched on the main loop. */
  bool icon_theme_dirty_ = false;

 private:
  const waybar::Bar& bar_;
  Gtk::Box box_;
  std::vector<TaskPtr> tasks_;

  IconLoader icon_loader_;
  std::unordered_set<std::string> ignore_list_;
  std::unordered_set<std::string> squash_list_;
  std::map<std::string, std::string> app_ids_replace_map_;
  std::map<std::string, std::string> fg_process_map_;

  struct zwlr_foreign_toplevel_manager_v1* manager_;
  struct ext_workspace_manager_v1* workspace_manager_;
  struct wl_seat* seat_;
  std::vector<struct ext_workspace_group_handle_v1*> workspace_groups_;
  std::vector<std::unique_ptr<WorkspaceState>> workspaces_;
  struct ext_workspace_handle_v1* current_workspace_ = nullptr;
#ifdef HAVE_HYPRLAND
  bool registered_for_hyprland_events_ = false;
#endif

  /* Keyboard navigation: a selection that the control socket moves through the
   * task list, so the bar can act as the visible half of a window switcher.
   * icon_theme_dirty_ lives up beside update_queued_ -- both are main-loop only. */
  int selection_index_ = -1;
  uint32_t current_active_id_ = 0;   // currently active window
  uint32_t previous_active_id_ = 0;  // previous active window, for MRU switching
  int socket_fd_ = -1;
  std::string socket_path_;
  std::thread socket_thread_;
  std::atomic<bool> socket_running_{false};

  void setupControlSocket();
  void cleanupControlSocket();
  void socketListener();
  void handleCommand(const std::string& cmd);
  void selectNext();
  void selectPrev();
  void activateSelected();
  void clearSelection();
  void updateSelection(int old_index);

 public:
  void notifyActiveChanged(uint32_t new_active_id);
  void notifyIconThemeChanged();
  /* Callbacks for global registration */
  void register_manager(struct wl_registry*, uint32_t name, uint32_t version);
  void register_workspace_manager(struct wl_registry*, uint32_t name, uint32_t version);
  void register_seat(struct wl_registry*, uint32_t name, uint32_t version);

  /* Callbacks for the wlr protocol */
  void handle_toplevel_create(struct zwlr_foreign_toplevel_handle_v1*);
  void handle_finished();
  void handle_workspace_group_create(struct ext_workspace_group_handle_v1*);
  void handle_workspace_group_removed(struct ext_workspace_group_handle_v1*);
  void handle_workspace_create(struct ext_workspace_handle_v1*);
  void handle_workspace_done();
  void handle_workspace_finished();
  void handle_workspace_removed(struct ext_workspace_handle_v1*);

 public:
  void add_button(Gtk::Button&);
  void move_button(Gtk::Button&, int);
  bool reorder_task(uint32_t dragged_id, uint32_t destination_id);
  void remove_button(Gtk::Button&);
  void remove_task(uint32_t);
  void assign_current_workspace(Task&);
  void update_bar_css_classes();
  void notify_icon_theme_changed();

  bool show_output(struct wl_output*) const;
  bool all_outputs() const;

  const IconLoader& icon_loader() const;
  const std::unordered_set<std::string>& ignore_list() const;
  const std::unordered_set<std::string>& squash_list() const;
  const std::map<std::string, std::string>& app_ids_replace_map() const;
  const std::map<std::string, std::string>& fg_process_map() const;

  auto tasks() {
    return tasks_ | std::views::transform([](auto& task) -> Task& { return *task; });
  }

 private:
  void set_bar_css_class(const std::string&, bool);
};

} /* namespace waybar::modules::wlr */
