#include "modules/wlr/taskbar.hpp"

#include <dirent.h>
#include <fmt/core.h>
#include <gdkmm/monitor.h>
#include <gio/gdesktopappinfo.h>
#include <giomm/desktopappinfo.h>
#include <gtkmm/icontheme.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "gdkmm/general.h"
#ifdef HAVE_HYPRLAND
#include "modules/hyprland/backend.hpp"
#endif
#include "glibmm/error.h"
#include "glibmm/fileutils.h"
#include "glibmm/refptr.h"
#include "modules/wlr/taskbar_dnd.hpp"
#include "util/format.hpp"
#include "util/gtk_icon.hpp"
#include "util/rewrite_string.hpp"
#include "util/string.hpp"
#include "util/tooltip.hpp"

namespace waybar::modules::wlr {

/* Task class implementation */

static const std::unordered_set<std::string> KNOWN_TERMINALS = {
    "kitty", "alacritty", "foot",    "wezterm", "wezterm-gui", "gnome-terminal", "xterm",
    "urxvt", "st",        "konsole", "termite", "tilix",       "rxvt",           "mlterm"};

static const std::unordered_set<std::string> KNOWN_SHELLS = {
    "bash", "zsh", "fish", "sh", "dash", "ksh", "tcsh", "csh", "nu", "elvish"};

// Case-insensitive terminal check — app_id values differ across compositors
// (e.g. Hyprland reports "Alacritty" while the binary is "alacritty").
static bool isKnownTerminal(const std::string& id) {
  std::string lower = id;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return KNOWN_TERMINALS.count(lower) > 0;
}

// Walk /proc to find the foreground process name for the given terminal PID.
// Returns "" when only a shell is in the foreground (fall back to terminal icon).
// Returns the multiplexer name (e.g. "tmux") when one is running so its own
// icon is used. Returns the app name (e.g. "yazi") for everything else.
//
// Uses only /proc/[pid]/stat — no tcgetpgrp, no ioctl, no permission issues.
// stat field layout: pid (comm) state ppid pgrp session tty_nr tpgid ...
//   tpgid (field 8) is the foreground process group of the controlling terminal.
static std::string getForegroundProcessName(pid_t terminal_pid) {
  // Processes to ignore when scanning children of the terminal.
  // "kitten" is kitty's own internal helper, not a real application.
  static const std::unordered_set<std::string> SKIP_PROCS = {"kitten"};

  struct ProcInfo {
    pid_t pid;
    std::string comm;
    pid_t ppid;
    pid_t pgrp;
    pid_t tpgid;  // foreground pgrp of the controlling terminal
  };

  // Single pass: read every /proc/[pid]/stat entry we can access.
  std::vector<ProcInfo> all_procs;
  {
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) return "";
    struct dirent* pent;
    while ((pent = readdir(proc_dir)) != nullptr) {
      bool is_numeric = true;
      for (const char* c = pent->d_name; *c; ++c) {
        if (!isdigit(static_cast<unsigned char>(*c))) {
          is_numeric = false;
          break;
        }
      }
      if (!is_numeric) continue;

      std::string stat_path = "/proc/" + std::string(pent->d_name) + "/stat";
      FILE* f = fopen(stat_path.c_str(), "r");
      if (!f) continue;

      ProcInfo info;
      char comm_buf[256] = {};
      int ppid = 0, pgrp = 0, session = 0, tty_nr = 0, tpgid = 0;
      int ret = fscanf(f, "%d (%255[^)]) %*c %d %d %d %d %d", &info.pid, comm_buf, &ppid, &pgrp,
                       &session, &tty_nr, &tpgid);
      fclose(f);
      if (ret < 7) continue;

      info.comm = comm_buf;
      info.ppid = static_cast<pid_t>(ppid);
      info.pgrp = static_cast<pid_t>(pgrp);
      info.tpgid = static_cast<pid_t>(tpgid);
      all_procs.push_back(std::move(info));
    }
    closedir(proc_dir);
  }

  // Collect direct children of the terminal PID.
  std::vector<const ProcInfo*> children;
  for (const auto& p : all_procs) {
    if (p.ppid == terminal_pid) children.push_back(&p);
  }

  if (children.empty()) {
    spdlog::debug("fgicon: no children found for pid {}", terminal_pid);
    return "";
  }

  for (const auto* child : children) {
    spdlog::debug("fgicon: child pid={} comm='{}' pgrp={} tpgid={}", child->pid, child->comm,
                  child->pgrp, child->tpgid);

    // Skip the terminal's own helper processes and nested terminals.
    if (SKIP_PROCS.count(child->comm) || isKnownTerminal(child->comm)) continue;

    if (KNOWN_SHELLS.count(child->comm)) {
      // The child is a shell. tpgid tells us what is in the foreground.
      pid_t fg_pgrp = child->tpgid;
      if (fg_pgrp <= 0 || fg_pgrp == child->pgrp) {
        // Shell itself is in the foreground — nothing interesting running.
        spdlog::debug("fgicon: shell '{}' is in foreground, skipping", child->comm);
        continue;
      }
      // Find the process in the foreground group.
      for (const auto& p : all_procs) {
        if (p.pgrp == fg_pgrp && !KNOWN_SHELLS.count(p.comm)) {
          spdlog::debug("fgicon: fg process is '{}' (pgrp {})", p.comm, fg_pgrp);
          return p.comm;
        }
      }
    } else {
      // Non-shell direct child of the terminal (e.g. kitty running yazi directly).
      spdlog::debug("fgicon: direct non-shell child is '{}'", child->comm);
      return child->comm;
    }
  }

  spdlog::debug("fgicon: no foreground app found for terminal pid {}", terminal_pid);
  return "";
}

// Search all installed .desktop files for one whose Exec= basename matches
// proc_name. Handles apps where the binary name differs from the desktop file
// name, e.g. hx (helix) or vi (neovim).
static Glib::RefPtr<Gio::DesktopAppInfo> findAppInfoByExec(const std::string& proc_name) {
  // Build the exec→desktop map once. Calling Gio::AppInfo::get_all() on
  // every title-change event emits a GIO "changed" signal that causes GTK
  // to invalidate its icon theme cache, wiping all taskbar icons at once.
  static std::unordered_map<std::string, Glib::RefPtr<Gio::DesktopAppInfo>> s_cache;
  static bool s_built = false;
  if (!s_built) {
    s_built = true;
    for (const auto& app : Gio::AppInfo::get_all()) {
      auto desktop = Glib::RefPtr<Gio::DesktopAppInfo>::cast_dynamic(app);
      if (!desktop) continue;
      std::string exec = app->get_executable();
      auto slash = exec.rfind('/');
      if (slash != std::string::npos) exec = exec.substr(slash + 1);
      if (s_cache.find(exec) == s_cache.end()) s_cache[exec] = desktop;
    }
  }
  auto it = s_cache.find(proc_name);
  return it != s_cache.end() ? it->second : Glib::RefPtr<Gio::DesktopAppInfo>{};
}

// Load an app icon as a software (window-independent) Cairo surface.
//
// The shared IconLoader::image_load_icon() passes image.get_window() to
// gdk_cairo_surface_create_from_pixbuf(), which ties the surface to that
// specific GdkWindow. When a screencopy tool (Satty, grim, etc.) causes GTK
// to reset its style/surface state, those window-backed surfaces go stale
// and every icon shows the broken-image symbol.
//
// This helper is intentionally scoped to the taskbar module. It mirrors the
// IconLoader lookup logic but passes a null window so the resulting surface is
// a plain software image that survives any window lifecycle event.
static bool load_icon_software(Gtk::Image& image, const Glib::RefPtr<Gio::DesktopAppInfo>& app_info,
                               const IconLoader& loader, int size) {
  if (!app_info) return false;

  int scaled_size = size * image.get_scale_factor();

  // Determine icon name: try startup WM class in each custom theme first,
  // then fall back to the icon declared in the desktop entry.
  //
  // The WM-class probe must only accept an icon the custom theme itself ships.
  // lookup_icon() follows the theme's Inherits= chain, so an unguarded probe
  // succeeds through a parent theme and quietly discards the entry's Icon=,
  // which is the icon the custom theme was most likely chosen to provide.
  std::string icon_name;
  std::string wm_class = app_info->get_startup_wm_class();
  for (const auto& [theme_name, theme] : loader.custom_themes()) {
    if (IconLoader::icon_is_native_to_theme(theme, theme_name, wm_class, scaled_size)) {
      icon_name = wm_class;
      break;
    }
  }
  if (icon_name.empty() && app_info->get_icon()) icon_name = app_info->get_icon()->to_string();
  if (icon_name.empty()) icon_name = "unknown";

  // Load pixbuf: custom themes first, then system default (includes hicolor).
  Glib::RefPtr<Gdk::Pixbuf> pixbuf;
  for (const auto& [theme_name, theme] : loader.custom_themes()) {
    try {
      pixbuf = theme->load_icon(icon_name, scaled_size, Gtk::ICON_LOOKUP_FORCE_SIZE);
      if (pixbuf) break;
    } catch (...) {
    }
  }
  if (!pixbuf) {
    try {
      pixbuf = Gtk::IconTheme::get_default()->load_icon(icon_name, scaled_size,
                                                        Gtk::ICON_LOOKUP_FORCE_SIZE);
    } catch (...) {
    }
  }
  if (!pixbuf) return false;

  if (pixbuf->get_width() != scaled_size) {
    int w = scaled_size * pixbuf->get_width() / pixbuf->get_height();
    pixbuf = pixbuf->scale_simple(w, scaled_size, Gdk::InterpType::INTERP_BILINEAR);
  }

  // Null window → CAIRO_SURFACE_TYPE_IMAGE (software), never goes stale.
  auto surface = Gdk::Cairo::create_surface_from_pixbuf(pixbuf, image.get_scale_factor(), {});
  image.set(surface);
  return true;
}

uint32_t Task::global_id = 1;  // Start from 1 so 0 can be used as "no window" sentinel

static void tl_handle_title(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                            const char* title) {
  return static_cast<Task*>(data)->handle_title(title);
}

static void tl_handle_app_id(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                             const char* app_id) {
  return static_cast<Task*>(data)->handle_app_id(app_id);
}

static void tl_handle_output_enter(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                                   struct wl_output* output) {
  return static_cast<Task*>(data)->handle_output_enter(output);
}

static void tl_handle_output_leave(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                                   struct wl_output* output) {
  return static_cast<Task*>(data)->handle_output_leave(output);
}

static void tl_handle_state(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                            struct wl_array* state) {
  return static_cast<Task*>(data)->handle_state(state);
}

static void tl_handle_done(void* data, struct zwlr_foreign_toplevel_handle_v1* handle) {
  return static_cast<Task*>(data)->handle_done();
}

static void tl_handle_parent(void* data, struct zwlr_foreign_toplevel_handle_v1* handle,
                             struct zwlr_foreign_toplevel_handle_v1* parent) {
  /* This is explicitly left blank */
}

static void tl_handle_closed(void* data, struct zwlr_foreign_toplevel_handle_v1* handle) {
  return static_cast<Task*>(data)->handle_closed();
}

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_handle_impl = {
    .title = tl_handle_title,
    .app_id = tl_handle_app_id,
    .output_enter = tl_handle_output_enter,
    .output_leave = tl_handle_output_leave,
    .state = tl_handle_state,
    .done = tl_handle_done,
    .closed = tl_handle_closed,
    .parent = tl_handle_parent,
};

static const std::vector<Gtk::TargetEntry> target_entries = {
    Gtk::TargetEntry("WAYBAR_TOPLEVEL", Gtk::TARGET_SAME_APP, 0)};

Task::Task(const waybar::Bar& bar, const Json::Value& config, Taskbar* tbar,
           struct zwlr_foreign_toplevel_handle_v1* tl_handle, struct wl_seat* seat)
    : bar_{bar},
      config_{config},
      tbar_{tbar},
      handle_{tl_handle},
      seat_{seat},
      id_{global_id++},
      content_{bar.orientation, 0} {
  zwlr_foreign_toplevel_handle_v1_add_listener(handle_, &toplevel_handle_impl, this);

  button.set_relief(Gtk::RELIEF_NONE);

  /* When "expand" is enabled the buttons stretch to fill the taskbar and the
   * titles ellipsize to fit within the available space. This only makes sense
   * on a horizontal bar; on a vertical bar the box grows along the vertical
   * axis, so ellipsizing/forcing width_chars(1) would truncate every title to
   * "…". Keep the historical behavior (buttons sized to their content) as the
   * default and when the bar is vertical. */
  bool expand = config_["expand"].isBool() && config_["expand"].asBool();
  bool horizontal = bar.orientation == Gtk::ORIENTATION_HORIZONTAL;

  if (expand && horizontal) {
    button.set_hexpand(true);
    content_.set_hexpand(true);
    text_before_.set_ellipsize(Pango::ELLIPSIZE_END);
    text_before_.set_single_line_mode(true);
    text_before_.set_width_chars(1);
    text_before_.set_xalign(0.0);
    text_after_.set_ellipsize(Pango::ELLIPSIZE_END);
    text_after_.set_single_line_mode(true);
    text_after_.set_width_chars(1);
    text_after_.set_xalign(0.0);

    content_.pack_start(text_before_, true, true, 0);
    content_.pack_start(icon_, false, false, 0);
    content_.pack_start(text_after_, true, true, 0);
  } else {
    content_.add(text_before_);
    content_.add(icon_);
    content_.add(text_after_);
  }

  if (config_["justify"].isString()) {
    auto justify_str = config_["justify"].asString();
    if (justify_str == "left") {
      content_.set_halign(Gtk::ALIGN_START);
    } else if (justify_str == "right") {
      content_.set_halign(Gtk::ALIGN_END);
    } else if (justify_str == "center") {
      content_.set_halign(Gtk::ALIGN_CENTER);
    }
  }

  content_.show();
  button.add(content_);

  // Apply optional label truncation (ellipsize).
  if (config_["truncate"].isBool() && config_["truncate"].asBool()) {
    text_before_.set_single_line_mode(true);
    text_before_.set_ellipsize(Pango::ELLIPSIZE_END);
    text_before_.set_line_wrap(false);

    text_after_.set_single_line_mode(true);
    text_after_.set_ellipsize(Pango::ELLIPSIZE_END);
    text_after_.set_line_wrap(false);
  }

  format_before_.clear();
  format_after_.clear();

  if (config_["format"].isString()) {
    /* The user defined a format string, use it */
    auto format = config_["format"].asString();
    if (format.find("{name}") != std::string::npos) {
      with_name_ = true;
    }

    auto parts = split(format, "{icon}", 1);
    format_before_ = parts[0];
    if (parts.size() > 1) {
      with_icon_ = true;
      format_after_ = parts[1];
    }
  } else {
    /* The default is to only show the icon */
    with_icon_ = true;
  }

  if (app_id_.empty()) {
    handle_app_id("unknown");
  }

  /* Strip spaces at the beginning and end of the format strings */
  format_tooltip_.clear();
  if (!config_["tooltip"].isBool() || config_["tooltip"].asBool()) {
    if (config_["tooltip-format"].isString())
      format_tooltip_ = config_["tooltip-format"].asString();
    else
      // Default to {name} so fg process display names show in tooltips.
      // Users can override with tooltip-format: "{title}" to get the raw
      // window title back.
      format_tooltip_ = "{name}";
  }

  /* Handle click events if configured */
  if (config_["on-click"].isString() || config_["on-click-middle"].isString() ||
      config_["on-click-right"].isString()) {
  }

  button.add_events(Gdk::BUTTON_PRESS_MASK);
  button.signal_button_release_event().connect(sigc::mem_fun(*this, &Task::handle_clicked), false);

  button.signal_motion_notify_event().connect(sigc::mem_fun(*this, &Task::handle_motion_notify),
                                              false);

  button.drag_source_set(target_entries, Gdk::BUTTON1_MASK, Gdk::ACTION_MOVE);
  button.drag_dest_set(target_entries, Gtk::DEST_DEFAULT_ALL, Gdk::ACTION_MOVE);

  button.signal_drag_data_get().connect(sigc::mem_fun(*this, &Task::handle_drag_data_get), false);
  button.signal_drag_data_received().connect(sigc::mem_fun(*this, &Task::handle_drag_data_received),
                                             false);

  /* Re-rasterize the icon if the bar ends up on an output with a different scale
   * than the one GTK guessed while the widget was not yet realized. */
  icon_.property_scale_factor().signal_changed().connect([this] {
    spdlog::debug("Task ({}) scale factor changed, reloading icon", id_);
    reload_icon();
  });
}

Task::~Task() {
  if (handle_) {
    zwlr_foreign_toplevel_handle_v1_destroy(handle_);
    handle_ = nullptr;
  }
  if (button_visible_) {
    tbar_->remove_button(button);
    button_visible_ = false;
  }
}

std::string Task::repr() const {
  std::stringstream ss;
  ss << "Task (" << id_ << ") " << title_ << " [" << app_id_ << "] <" << (active() ? "A" : "a")
     << (maximized() ? "M" : "m") << (minimized() ? "I" : "i") << (fullscreen() ? "F" : "f") << ">";

  return ss.str();
}

std::string Task::state_string(bool shortened) const {
  std::stringstream ss;
  if (shortened)
    ss << (minimized() ? "m" : "") << (maximized() ? "M" : "") << (active() ? "A" : "")
       << (fullscreen() ? "F" : "");
  else
    ss << (minimized() ? "minimized " : "") << (maximized() ? "maximized " : "")
       << (active() ? "active " : "") << (fullscreen() ? "fullscreen " : "");

  std::string res = ss.str();
  if (shortened || res.empty())
    return res;
  else
    return res.substr(0, res.size() - 1);
}

void Task::handle_title(const char* title) {
  if (title_.empty()) {
    spdlog::debug(fmt::format("Task ({}) setting title to {}", id_, title_));
  } else {
    spdlog::debug(fmt::format("Task ({}) overwriting title '{}' with '{}'", id_, title_, title));
  }
  title_ = title;
  hide_if_ignored();
  hide_if_duplicate();
  update_duplicates();

  // Only marks the lookup pending; lookup_app_info() does the work once per
  // batch, including re-resolving a terminal's foreground program.
  if (with_icon_ || with_name_) {
    title_lookup_pending_ = true;
  }
}

void Task::set_minimize_hint() {
  zwlr_foreign_toplevel_handle_v1_set_rectangle(handle_, bar_.surface, minimize_hint.x,
                                                minimize_hint.y, minimize_hint.w, minimize_hint.h);
}

void Task::hide_if_duplicate() {
  const auto& squash_list = tbar_->squash_list();
  bool contains_app =
      squash_list.contains("*") || squash_list.contains(title_) || squash_list.contains(app_id_);

  // Only an instance that has a button counts, or the last button of the app gets squashed too
  squashed_ = contains_app && std::ranges::any_of(tbar_->tasks(), [this](auto&& task) {
                return &task != this && task.button_visible_ &&
                       (task.app_id_ == app_id_ || task.title_ == title_);
              });

  if (squashed_) {
    hide_button();
  } else if (!ignored_ && (tbar_->all_outputs() || on_bar_output_)) {
    show_button();
  }
}

void Task::update_duplicates() {
  for (auto&& task : tbar_->tasks()) {
    if (&task != this && task.squashed_) {
      task.hide_if_duplicate();
    }
  }
}

void Task::hide_if_ignored() {
  if (tbar_->ignore_list().count(app_id_) || tbar_->ignore_list().count(title_)) {
    ignored_ = true;
    hide_button();
    return;
  }

  if (ignored_) {
    /* The app_id/title changed to a value that is no longer ignored */
    ignored_ = false;
    if (!squashed_ && (tbar_->all_outputs() || on_bar_output_)) {
      show_button();
    }
  }
}

void Task::handle_app_id(const char* app_id) {
  if (app_id_.empty()) {
    spdlog::debug(fmt::format("Task ({}) setting app_id to {}", id_, app_id));
  } else {
    spdlog::debug(fmt::format("Task ({}) overwriting app_id '{}' with '{}'", id_, app_id_, app_id));
  }
  app_id_ = app_id;
  hide_if_ignored();
  hide_if_duplicate();
  update_duplicates();

  auto ids_replace_map = tbar_->app_ids_replace_map();
  if (ids_replace_map.count(app_id_)) {
    auto replaced_id = ids_replace_map[app_id_];
    spdlog::debug(
        fmt::format("Task ({}) [{}] app_id was replaced with {}", id_, app_id_, replaced_id));
    app_id_ = replaced_id;
  }

  if ((!with_icon_ && !with_name_) || app_id_ == lookup_source_) {
    return;
  }

  name_ = app_id;
  app_id_lookup_pending_ = true;
  title_lookup_pending_ = false;
}

// Lookups cost file probes and an index search, and clients choose how often their app_id and
// title change: they run from update(), once per batch of events, and never twice for one string
void Task::lookup_app_info() {
  bool looked_up = false;
  // A terminal gets its icon from whatever program is in its foreground, which
  // the cache below cannot speak for: the terminal's own desktop entry is
  // already resolved, so neither branch would look anything up, yet the answer
  // changes every time the title does. Track that separately.
  bool recheck_terminal_fg = false;
  if (app_id_lookup_pending_) {
    app_id_lookup_pending_ = false;
    lookup_source_ = app_id_;
    looked_up = true;
    recheck_terminal_fg = true;
    app_info_ = IconLoader::get_app_info_from_app_id_list(app_id_);
    if (app_info_) {
      name_ = app_info_->get_display_name();
    }
  }
  if (title_lookup_pending_) {
    title_lookup_pending_ = false;
    recheck_terminal_fg = true;
    if (!app_info_ && title_ != lookup_source_) {
      lookup_source_ = title_;
      looked_up = true;
      app_info_ = IconLoader::get_app_info_from_app_id_list(title_);
      name_ = app_info_ ? app_info_->get_display_name() : title_;
    }
  }

  // The entry the task's own app_id/title resolved to, kept so the terminal
  // branch below can put it back once the foreground program is gone.
  if (looked_up) {
    own_app_info_ = app_info_;
  }

  // Runs once per batch of events like the lookups above, rather than inside
  // the Wayland handlers: this costs a Hyprland IPC round trip and a walk of
  // /proc, and a terminal rewriting its title on every prompt would otherwise
  // starve the main loop -- the same failure the lookup batching fixed.
  if (recheck_terminal_fg && isKnownTerminal(app_id_)) {
    if (tryUpdateIconFromTerminalFg()) {
      fg_icon_applied_ = true;
      return;  // the helper set icon_, app_info_ and name_ itself
    }
    // Nothing in the foreground any more -- the program exited and the shell is
    // back, or it has no icon of its own. Restore the terminal's own entry, or
    // the program's icon stays on the button for as long as the terminal lives.
    if (fg_icon_applied_) {
      fg_icon_applied_ = false;
      app_info_ = own_app_info_;
      name_ = app_info_ ? app_info_->get_display_name() : app_id_;
      looked_up = true;  // the icon below is the foreground program's, reload it
    }
  }

  if (!looked_up || !with_icon_) {
    return;
  }

  int icon_size = config_["icon-size"].isInt() ? config_["icon-size"].asInt() : 16;
  if (tbar_->icon_loader().image_load_icon(icon_, app_info_, icon_size))
    icon_.show();
  else
    spdlog::debug("Couldn't find icon for {}", lookup_source_);
}

// Query Hyprland IPC for the PID of the window matching this task, then walk
// /proc to find what is running in the terminal's foreground.
// Multiplexers (tmux, zellij, …) are returned as-is so their own icon is shown.
// Returns true and updates icon_/app_info_/name_ on success.
bool Task::tryUpdateIconFromTerminalFg() {
#ifndef HAVE_HYPRLAND
  // The PID of the window behind a task is only reachable through Hyprland's
  // IPC here, so without Hyprland support there is nothing to resolve and the
  // caller keeps the icon the desktop entry gave it.
  return false;
#else
  spdlog::debug("fgicon: tryUpdateIconFromTerminalFg called for app_id='{}' title='{}'", app_id_,
                title_);
  if (!with_icon_ && !with_name_) {
    spdlog::debug("fgicon: skipping — with_icon_={} with_name_={}", with_icon_, with_name_);
    return false;
  }
  const char* his = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (his == nullptr) {
    spdlog::debug("fgicon: HYPRLAND_INSTANCE_SIGNATURE not set");
    return false;
  }
  // IPC::inst() constructs the connection on first use, so there is nothing to
  // initialize here even when sort-by-hyprland-workspaces is off.
  try {
    Json::Value clients = hyprland::IPC::inst().getSocket1JsonReply("clients");
    if (!clients.isArray()) {
      spdlog::debug("fgicon: clients IPC reply is not an array");
      return false;
    }
    spdlog::debug("fgicon: got {} clients from IPC", clients.size());

    // Match this task to a Hyprland client by class + title to get its PID.
    // Fall back to class-only if no exact title match exists — the title seen
    // by the wlr protocol and by Hyprland IPC can diverge slightly in timing.
    pid_t term_pid = -1;
    pid_t class_only_pid = -1;
    for (Json::ArrayIndex i = 0; i < clients.size(); ++i) {
      const auto& client = clients[i];
      spdlog::debug("fgicon: client[{}] class='{}' title='{}'", i, client["class"].asString(),
                    client["title"].asString());
      if (client["class"].asString() == app_id_) {
        if (client["title"].asString() == title_) {
          term_pid = static_cast<pid_t>(client["pid"].asInt());
          break;
        } else if (class_only_pid < 0) {
          class_only_pid = static_cast<pid_t>(client["pid"].asInt());
        }
      }
    }
    if (term_pid <= 0) term_pid = class_only_pid;
    if (term_pid <= 0) {
      spdlog::debug("fgicon: no matching client found for app_id='{}'", app_id_);
      return false;
    }
    spdlog::debug("fgicon: matched pid={}", term_pid);

    std::string fg_name = getForegroundProcessName(term_pid);
    if (fg_name.empty()) return false;

    // Apply fg-process-mapping: lets users remap wrapper scripts or processes
    // with no .desktop file to a known app-id.
    // e.g. "fg-process-mapping": { "zide": "zellij", "zellij": "zellij" }
    const auto& fg_map = tbar_->fg_process_map();
    auto fg_map_it = fg_map.find(fg_name);
    if (fg_map_it != fg_map.end()) {
      spdlog::debug("fgicon: remapping '{}' → '{}' via fg-process-mapping", fg_name,
                    fg_map_it->second);
      fg_name = fg_map_it->second;
    }

    spdlog::debug("fgicon: terminal {} fg process is '{}'", app_id_, fg_name);

    int icon_size = config_["icon-size"].isInt() ? config_["icon-size"].asInt() : 16;

    // Try to resolve a .desktop entry for the foreground process.
    // First attempt: match by app-id (looks for <fg_name>.desktop).
    // Second attempt: scan all desktop files for one whose Exec= basename
    //   matches the process name — catches cases like hx → Helix.desktop.
    auto fg_info = IconLoader::get_app_info_from_app_id_list(fg_name);
    if (!fg_info) fg_info = findAppInfoByExec(fg_name);
    if (fg_info) {
      app_info_ = fg_info;
      name_ = fg_info->get_display_name();
      if (with_icon_) {
        if (load_icon_software(icon_, app_info_, tbar_->icon_loader(), icon_size)) icon_.show();
      }
      return true;
    }

    // No .desktop entry — try the process name directly as an icon name
    if (with_icon_) {
      auto icon_theme = Gtk::IconTheme::get_default();
      if (icon_theme->has_icon(fg_name)) {
        icon_.set_from_icon_name(fg_name, Gtk::ICON_SIZE_INVALID);
        icon_.set_pixel_size(icon_size);
        icon_.show();
        name_ = fg_name;
        return true;
      }
    }
    spdlog::debug("fgicon: no desktop entry or icon found for '{}'", fg_name);
  } catch (const std::exception& e) {
    spdlog::debug("fgicon: exception: {}", e.what());
  }
  return false;
#endif  // HAVE_HYPRLAND
}

void Task::on_button_size_allocated(Gtk::Allocation& alloc) {
  gtk_widget_translate_coordinates(GTK_WIDGET(button.gobj()), GTK_WIDGET(bar_.window.gobj()), 0, 0,
                                   &minimize_hint.x, &minimize_hint.y);
  minimize_hint.w = button.get_width();
  minimize_hint.h = button.get_height();
}

void Task::handle_output_enter(struct wl_output* output) {
  spdlog::debug("{} entered output {}", repr(), (void*)output);

  if (tbar_->show_output(output)) {
    on_bar_output_ = true;
  }

  if (ignored_) {
    spdlog::debug("{} is ignored", repr());
    return;
  }
  hide_if_duplicate();
  if (squashed_) {
    spdlog::debug("{} was squashed", repr());
    return;
  }

  if (tbar_->all_outputs() || on_bar_output_) {
    /* The task entered the output of the current bar, make the button visible */
    show_button();
  }
}

void Task::handle_output_leave(struct wl_output* output) {
  spdlog::debug("{} left output {}", repr(), (void*)output);

  if (tbar_->show_output(output)) {
    on_bar_output_ = false;
  }

  if (!tbar_->all_outputs() && !on_bar_output_) {
    /* The task left the output of the current bar, make the button invisible */
    hide_button();
    update_duplicates();
  }
}

void Task::show_button() {
  if (button_visible_) {
    return;
  }
  if (!size_allocate_connected_) {
    button.signal_size_allocate().connect_notify(
        sigc::mem_fun(this, &Task::on_button_size_allocated));
    size_allocate_connected_ = true;
  }
  tbar_->add_button(button);
  if (!config_["active-only"].asBool() || active()) {
    button.show();
  }
  button_visible_ = true;
  spdlog::debug("{} now visible on {}", repr(), bar_.output->name);
  tbar_->update_bar_css_classes();
}

void Task::hide_button() {
  if (!button_visible_) {
    return;
  }
  tbar_->remove_button(button);
  button.hide();
  button_visible_ = false;
  spdlog::debug("{} now invisible on {}", repr(), bar_.output->name);
  tbar_->update_bar_css_classes();
}

void Task::handle_state(struct wl_array* state) {
  state_ = 0;
  size_t size = state->size / sizeof(uint32_t);
  for (size_t i = 0; i < size; ++i) {
    auto entry = static_cast<uint32_t*>(state->data)[i];
    if (entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED) state_ |= MAXIMIZED;
    if (entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) state_ |= MINIMIZED;
    if (entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) state_ |= ACTIVE;
    if (entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) state_ |= FULLSCREEN;
  }
}

void Task::handle_done() {
  spdlog::debug("{} changed", repr());

  if (state_ & MAXIMIZED) {
    button.get_style_context()->add_class("maximized");
  } else if (!(state_ & MAXIMIZED)) {
    button.get_style_context()->remove_class("maximized");
  }

  if (state_ & MINIMIZED) {
    button.get_style_context()->add_class("minimized");
  } else if (!(state_ & MINIMIZED)) {
    button.get_style_context()->remove_class("minimized");
  }

  if (state_ & ACTIVE) {
    button.get_style_context()->add_class("active");
    spdlog::info("Task::handle_done: window became active - id={}, app_id={}, title={}", id_,
                 app_id_, title_);
    tbar_->notifyActiveChanged(id_);
  } else if (!(state_ & ACTIVE)) {
    button.get_style_context()->remove_class("active");
  }

  if (state_ & FULLSCREEN) {
    button.get_style_context()->add_class("fullscreen");
  } else if (!(state_ & FULLSCREEN)) {
    button.get_style_context()->remove_class("fullscreen");
  }

  if (button_visible_ && config_["active-only"].asBool()) {
    if (active()) {
      button.show();
    } else {
      button.hide();
    }
  }

  if (active()) {
    tbar_->assign_current_workspace(*this);
  }

  tbar_->update_bar_css_classes();

  if (config_["active-first"].isBool() && config_["active-first"].asBool() && active())
    tbar_->move_button(button, 0);

  if (!tbar_->update_queued_) {
    tbar_->update_queued_ = true;
    tbar_->dp.emit();
  }
}

void Task::handle_closed() {
  spdlog::debug("{} closed", repr());
  zwlr_foreign_toplevel_handle_v1_destroy(handle_);
  handle_ = nullptr;
  if (button_visible_) {
    tbar_->remove_button(button);
    button_visible_ = false;
  }

  update_duplicates();

  tbar_->remove_task(id_);
}

bool Task::handle_clicked(GdkEventButton* bt) {
  /* filter out additional events for double/triple clicks */
  if (bt->type == GDK_BUTTON_PRESS) {
    /* save where the button press occurred in case it becomes a drag */
    drag_start_button = bt->button;
    drag_start_x = bt->x;
    drag_start_y = bt->y;
  }

  std::string action;
  if (config_["on-click"].isString() && bt->button == 1)
    action = config_["on-click"].asString();
  else if (config_["on-click-middle"].isString() && bt->button == 2)
    action = config_["on-click-middle"].asString();
  else if (config_["on-click-right"].isString() && bt->button == 3)
    action = config_["on-click-right"].asString();

  if (action.empty())
    return true;
  else if (action == "activate")
    activate();
  else if (action == "minimize") {
    set_minimize_hint();
    minimize(!minimized());
  } else if (action == "minimize-raise") {
    set_minimize_hint();
    if (minimized())
      minimize(false);
    else if (active())
      minimize(true);
    else
      activate();
  } else if (action == "maximize")
    maximize(!maximized());
  else if (action == "fullscreen")
    fullscreen(!fullscreen());
  else if (action == "close")
    close();
  else
    spdlog::warn("Unknown action {}", action);

  drag_start_button = -1;
  return true;
}

bool Task::handle_motion_notify(GdkEventMotion* mn) {
  if (drag_start_button == -1) return false;

  if (button.drag_check_threshold(drag_start_x, drag_start_y, mn->x, mn->y)) {
    /* start drag in addition to other assigned action */
    auto target_list = Gtk::TargetList::create(target_entries);
    auto refptr = Glib::RefPtr<Gtk::TargetList>(target_list);
    auto drag_context =
        button.drag_begin(refptr, Gdk::DragAction::ACTION_MOVE, drag_start_button, (GdkEvent*)mn);
  }

  return false;
}

void Task::handle_drag_data_get(const Glib::RefPtr<Gdk::DragContext>& context,
                                Gtk::SelectionData& selection_data, guint info, guint time) {
  spdlog::debug("drag_data_get");
  selection_data.set("WAYBAR_TOPLEVEL", 32, reinterpret_cast<const guchar*>(&id_), sizeof(id_));
}

void Task::handle_drag_data_received(const Glib::RefPtr<Gdk::DragContext>& context, int x, int y,
                                     Gtk::SelectionData selection_data, guint info, guint time) {
  spdlog::debug("drag_data_received");
  auto dragged_id = dnd::decode_task_id(selection_data.get_data(), selection_data.get_length());
  if (!dragged_id) {
    spdlog::warn("Ignoring malformed taskbar drag payload");
    return;
  }

  if (!tbar_->reorder_task(*dragged_id, id_)) {
    spdlog::debug("Ignoring stale taskbar drag from task {} to task {}", *dragged_id, id_);
  }
}

bool Task::operator==(const Task& o) const { return o.id_ == id_; }

bool Task::operator!=(const Task& o) const { return o.id_ != id_; }

void Task::update() {
  if (app_id_lookup_pending_ || title_lookup_pending_) {
    lookup_app_info();
  }

  bool markup = config_["markup"].isBool() ? config_["markup"].asBool() : false;
  std::string title = toSingleLine(title_);
  std::string name = toSingleLine(name_);
  std::string app_id = toSingleLine(app_id_);
  if (markup) {
    title = Glib::Markup::escape_text(title);
    name = Glib::Markup::escape_text(name);
    app_id = Glib::Markup::escape_text(app_id);
  }
  if (!format_before_.empty()) {
    auto txt =
        fmt::format(fmt::runtime(format_before_), fmt::arg("title", title), fmt::arg("name", name),
                    fmt::arg("app_id", app_id), fmt::arg("state", state_string()),
                    fmt::arg("short_state", state_string(true)));

    txt = waybar::util::rewriteString(txt, config_["rewrite"]);

    if (markup)
      text_before_.set_markup(txt);
    else
      text_before_.set_label(txt);
    text_before_.show();
  }
  if (!format_after_.empty()) {
    auto txt =
        fmt::format(fmt::runtime(format_after_), fmt::arg("title", title), fmt::arg("name", name),
                    fmt::arg("app_id", app_id), fmt::arg("state", state_string()),
                    fmt::arg("short_state", state_string(true)));

    txt = waybar::util::rewriteString(txt, config_["rewrite"]);

    if (markup)
      text_after_.set_markup(txt);
    else
      text_after_.set_label(txt);
    text_after_.show();
  }

  if (!format_tooltip_.empty()) {
    auto txt =
        fmt::format(fmt::runtime(format_tooltip_), fmt::arg("title", title), fmt::arg("name", name),
                    fmt::arg("app_id", app_id), fmt::arg("state", state_string()),
                    fmt::arg("short_state", state_string(true)));

    txt = waybar::util::rewriteString(txt, config_["rewrite"]);

    if (markup) {
      waybar::util::set_tooltip_markup(button, txt);
    } else {
      waybar::util::set_tooltip_markup(button, Glib::Markup::escape_text(txt));
    }
  }
}

void Task::maximize(bool set) {
  if (set)
    zwlr_foreign_toplevel_handle_v1_set_maximized(handle_);
  else
    zwlr_foreign_toplevel_handle_v1_unset_maximized(handle_);
}

void Task::minimize(bool set) {
  if (set)
    zwlr_foreign_toplevel_handle_v1_set_minimized(handle_);
  else
    zwlr_foreign_toplevel_handle_v1_unset_minimized(handle_);
}

/* Rasterize the icon again from the entry this task already resolved. The
 * pixbuf is produced once and then owned by the Gtk::Image, so anything that
 * invalidates it -- a scale change, the icon theme changing, or the bar's
 * layer-shell surface being recreated -- needs an explicit reload or the task
 * keeps showing the stale image.
 *
 * Loads through load_icon_software() rather than IconLoader, so the surface is
 * window-independent and survives the GTK window/style resets that screencopy
 * tools trigger. Upstream's reload (PR #5493) uses the plain IconLoader path;
 * this is the fork's stronger version. */
void Task::reload_icon() {
  if (!with_icon_ || !tbar_) return;
  int icon_size = config_["icon-size"].isInt() ? config_["icon-size"].asInt() : 16;
  const auto& loader = tbar_->icon_loader();
  icon_.clear();  // release previous surface reference before loading new one
  if (app_info_) {
    if (load_icon_software(icon_, app_info_, loader, icon_size)) icon_.show();
  } else {
    auto fallback = IconLoader::get_app_info_from_app_id_list(app_id_);
    if (fallback && load_icon_software(icon_, fallback, loader, icon_size)) icon_.show();
  }
}

void Task::activate() {
  // When running under Hyprland, use its own focuswindow dispatch rather than
  // the wlr foreign-toplevel activate call. The wlr path causes Hyprland to
  // warp the cursor to the centre of the screen on workspace switches; the
  // dispatch path uses the same code as keyboard shortcuts and leaves the
  // cursor where it is.
#ifdef HAVE_HYPRLAND
  if (std::getenv("HYPRLAND_INSTANCE_SIGNATURE") != nullptr) {
    try {
      Json::Value clients = hyprland::IPC::inst().getSocket1JsonReply("clients");
      if (clients.isArray()) {
        // Try exact class+title match first, then fall back to class only.
        std::string addr;
        std::string class_only_addr;
        Json::Value matched_client;
        Json::Value class_only_client;
        for (Json::ArrayIndex i = 0; i < clients.size(); ++i) {
          const auto& c = clients[i];
          spdlog::debug("activate: checking client class='{}' title='{}' addr='{}'",
                        c["class"].asString(), c["title"].asString(), c["address"].asString());
          if (c["class"].asString() == app_id_) {
            if (c["title"].asString() == title_) {
              addr = c["address"].asString();
              matched_client = c;
              break;
            } else if (class_only_addr.empty()) {
              class_only_addr = c["address"].asString();
              class_only_client = c;
            }
          }
        }
        if (addr.empty()) {
          addr = class_only_addr;
          matched_client = class_only_client;
        }
        if (!addr.empty()) {
          int win_ws = matched_client["workspace"]["id"].asInt();
          // Special workspaces carry negative ids, and `dispatch workspace -99`
          // is read as a relative offset (99 workspaces back) rather than a
          // target, so it lands on an ordinary workspace instead. Skip the
          // workspace dispatch for those and let focuswindow below handle it: it
          // pulls the special workspace up on the window's own monitor, which is
          // not a workspace crossing, so there is no cursor warp to avoid here.
          if (win_ws >= 0) {
            // A special workspace is an overlay on top of the monitor's ordinary
            // workspace, and neither `dispatch workspace N` nor focuswindow
            // dismisses it, so the window we are activating would stay buried
            // under it. Close it first. Only the focused monitor is considered:
            // togglespecialworkspace acts on that monitor, so aiming it at a
            // special workspace open elsewhere would open a second copy here
            // instead of closing that one.
            try {
              Json::Value monitors = hyprland::IPC::inst().getSocket1JsonReply("monitors");
              for (Json::ArrayIndex i = 0; monitors.isArray() && i < monitors.size(); ++i) {
                if (!monitors[i]["focused"].asBool()) continue;
                std::string special = monitors[i]["specialWorkspace"]["name"].asString();
                if (special.empty()) break;
                // The dispatcher wants the bare name: "special:magic" -> "magic",
                // and the unnamed default reports itself as "special:special".
                const auto colon = special.find(':');
                if (colon != std::string::npos) special = special.substr(colon + 1);
                spdlog::debug("activate: closing special workspace '{}'", special);
                hyprland::IPC::getSocket1Reply("dispatch togglespecialworkspace " + special);
                break;
              }
            } catch (...) {
            }

            try {
              Json::Value activeWs = hyprland::IPC::inst().getSocket1JsonReply("activeworkspace");
              int cur_ws = activeWs["id"].asInt();
              if (win_ws != cur_ws) {
                // Different workspace: use only `dispatch workspace N` — this
                // mirrors what keyboard shortcuts do and respects
                // cursor.warp_on_change_workspace. `focuswindow` has its own
                // unconditional cursor warp when crossing workspaces that no
                // setting can suppress without cursor.no_warps = true.
                spdlog::debug("activate: switching workspace {} → {} (workspace-only dispatch)",
                              cur_ws, win_ws);
                hyprland::IPC::getSocket1Reply("dispatch workspace " + std::to_string(win_ws));
                return;
              }
            } catch (...) {
            }
          }

          // Same workspace, or a special workspace: focus the specific window.
          std::string cmd = "dispatch focuswindow address:" + addr;
          spdlog::debug("activate: dispatching '{}' for app_id='{}' title='{}'", cmd, app_id_,
                        title_);
          std::string reply = hyprland::IPC::getSocket1Reply(cmd);
          spdlog::debug("activate: reply='{}'", reply);
          return;
        }
        spdlog::debug("activate: no address found for app_id='{}' title='{}', falling back to wlr",
                      app_id_, title_);
      }
    } catch (const std::exception& e) {
      spdlog::warn("Task::activate: Hyprland dispatch failed: {}", e.what());
    }
  }
#endif  // HAVE_HYPRLAND
  // Fallback for non-Hyprland compositors.
  spdlog::debug("activate: using wlr protocol for app_id='{}' title='{}'", app_id_, title_);
  zwlr_foreign_toplevel_handle_v1_activate(handle_, seat_);
}

void Task::fullscreen(bool set) {
  if (zwlr_foreign_toplevel_handle_v1_get_version(handle_) <
      ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_SET_FULLSCREEN_SINCE_VERSION) {
    spdlog::warn("Foreign toplevel manager server does not support for set/unset fullscreen.");
    return;
  }

  if (set)
    zwlr_foreign_toplevel_handle_v1_set_fullscreen(handle_, nullptr);
  else
    zwlr_foreign_toplevel_handle_v1_unset_fullscreen(handle_);
}

void Task::close() { zwlr_foreign_toplevel_handle_v1_close(handle_); }

/* Taskbar class implementation */
static void handle_global(void* data, struct wl_registry* registry, uint32_t name,
                          const char* interface, uint32_t version) {
  if (std::strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
    static_cast<Taskbar*>(data)->register_manager(registry, name, version);
  } else if (std::strcmp(interface, ext_workspace_manager_v1_interface.name) == 0) {
    static_cast<Taskbar*>(data)->register_workspace_manager(registry, name, version);
  } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
    static_cast<Taskbar*>(data)->register_seat(registry, name, version);
  }
}

static void handle_global_remove(void* data, struct wl_registry* registry, uint32_t name) {
  /* Nothing to do here */
}

static const wl_registry_listener registry_listener_impl = {.global = handle_global,
                                                            .global_remove = handle_global_remove};

Taskbar::Taskbar(const std::string& id, const waybar::Bar& bar, const Json::Value& config)
    : waybar::AModule(config, "taskbar", id, false, false),
      bar_(bar),
      box_{bar.orientation, 0},
      manager_{nullptr},
      workspace_manager_{nullptr},
      seat_{nullptr} {
  box_.set_name("taskbar");
  if (!id.empty()) {
    box_.get_style_context()->add_class(id);
  }
  box_.get_style_context()->add_class(MODULE_CLASS);
  box_.get_style_context()->add_class("empty");
  event_box_.add(box_);

  // wlr/taskbar interprets on-click* config values as built-in actions, handled
  // per-task in Task::handle_clicked. Register the recognized action names so
  // AModule dispatches them via doAction() instead of also running them as shell
  // commands (issue #3284). Values that are not built-in actions are left alone
  // and still run as user shell commands.
  const auto is_builtin_action = [](const std::string& v) {
    return v == "activate" || v == "minimize" || v == "minimize-raise" || v == "maximize" ||
           v == "fullscreen" || v == "close";
  };
  for (const auto* event : {"on-click", "on-click-middle", "on-click-right"}) {
    if (config_[event].isString() && is_builtin_action(config_[event].asString())) {
      eventActionMap_.insert({event, config_[event].asString()});
    }
  }

  // Make task buttons distribute evenly across the available width.
  if (config_["homogeneous"].isBool() && config_["homogeneous"].asBool()) {
    box_.set_homogeneous(true);
    box_.set_hexpand(true);
  }

  struct wl_display* display = Client::inst()->wl_display;
  struct wl_registry* registry = wl_display_get_registry(display);

  wl_registry_add_listener(registry, &registry_listener_impl, this);
  wl_display_roundtrip(display);
  // The listener points at this module and must not fire for globals announced after it is gone
  wl_registry_destroy(registry);

  if (!manager_) {
    spdlog::error("Failed to register as toplevel manager");
    return;
  }
  if (!seat_) {
    spdlog::error("Failed to get wayland seat");
    return;
  }

  /* Get the configured icon theme if specified */
  if (config_["icon-theme"].isArray()) {
    for (auto& c : config_["icon-theme"]) {
      icon_loader_.add_custom_icon_theme(c.asString());
    }
  } else if (config_["icon-theme"].isString()) {
    icon_loader_.add_custom_icon_theme(config_["icon-theme"].asString());
  }

  // Load ignore-list
  if (config_["ignore-list"].isArray()) {
    for (auto& app_name : config_["ignore-list"]) {
      ignore_list_.emplace(app_name.asString());
    }
  }

  // Load squash-list
  if (config_["squash-list"].isArray()) {
    for (auto& app_name : config_["squash-list"]) {
      squash_list_.emplace(app_name.asString());
    }
  }

  // Load app_id remappings
  if (config_["app_ids-mapping"].isObject()) {
    const Json::Value& mapping = config_["app_ids-mapping"];
    const std::vector<std::string> app_ids = config_["app_ids-mapping"].getMemberNames();
    for (auto& app_id : app_ids) {
      app_ids_replace_map_.emplace(app_id, mapping[app_id].asString());
    }
  }

  // Load fg-process-mapping: maps a detected foreground process name to an
  // app-id used for icon/name lookup. Useful for wrapper scripts (zide -> zellij)
  // or apps with no .desktop file (zellij -> zellij icon name).
  // Example config: "fg-process-mapping": { "zide": "zellij", "zellij": "zellij" }
  if (config_["fg-process-mapping"].isObject()) {
    const Json::Value& mapping = config_["fg-process-mapping"];
    for (auto& key : config_["fg-process-mapping"].getMemberNames()) {
      fg_process_map_.emplace(key, mapping[key].asString());
    }
  }

  for (auto& t : tasks_) {
    t->handle_app_id(t->app_id().c_str());
  }

#ifdef HAVE_HYPRLAND
  // IPC::inst() is a singleton that connects on first use, so the taskbar no
  // longer has to stand one up before the foreground-icon lookup can run.

  // Register for Hyprland events if sorting by Hyprland workspaces
  if (config_["sort-by-hyprland-workspaces"].asBool()) {
    const char* his = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (his != nullptr) {
      hyprland::IPC::inst().registerForIPC("movewindow", this);
      hyprland::IPC::inst().registerForIPC("openwindow", this);
      hyprland::IPC::inst().registerForIPC("closewindow", this);
      hyprland::IPC::inst().registerForIPC("configreloaded", this);
      registered_for_hyprland_events_ = true;
      spdlog::debug("Taskbar registered for Hyprland IPC events");
    }
  }
#else
  if (config_["sort-by-hyprland-workspaces"].asBool()) {
    spdlog::warn(
        "wlr/taskbar: sort-by-hyprland-workspaces needs a Waybar built with Hyprland support, "
        "ignoring it");
  }
#endif  // HAVE_HYPRLAND

  // Control socket for keyboard navigation; see man waybar-wlr-taskbar.
  setupControlSocket();

  /* Task icons are rasterized once, when the toplevel appears. Nothing else
   * re-reads the theme afterwards, so switching icon theme at runtime leaves
   * every existing task showing an icon from the previous one. A configured
   * icon-theme is its own Gtk::IconTheme, so those need listening to as well. */
  Gtk::IconTheme::get_default()->signal_changed().connect(
      sigc::mem_fun(*this, &Taskbar::notify_icon_theme_changed));
  for (const auto& [custom_theme_name, custom_theme] : icon_loader_.custom_themes()) {
    custom_theme->signal_changed().connect(
        sigc::mem_fun(*this, &Taskbar::notify_icon_theme_changed));
  }

  // Reload icons when the taskbar container is re-realized. This fires when
  // Waybar's own layer-shell surface is destroyed and recreated (e.g. when a
  // fullscreen screencopy tool like Satty briefly unmaps the bar layer). Cairo
  // surfaces created with image.get_window() become stale for the new window.
  box_.signal_realize().connect([this]() {
    spdlog::debug("Taskbar: box_ realized, reloading all task icons");
    for (auto& t : tasks_) {
      t->reload_icon();
    }
    dp.emit();
  });
}

Taskbar::~Taskbar() {
  cleanupControlSocket();
#ifdef HAVE_HYPRLAND
  if (registered_for_hyprland_events_) {
    hyprland::IPC::inst().unregisterForIPC(this);
  }
#endif

  if (workspace_manager_) {
    struct wl_display* display = Client::inst()->wl_display;
    ext_workspace_manager_v1_stop(workspace_manager_);
    wl_display_roundtrip(display);

    if (workspace_manager_) {
      spdlog::warn("Workspace manager destroyed before .finished event");
      ext_workspace_manager_v1_destroy(workspace_manager_);
      workspace_manager_ = nullptr;
    }
  }

  // Handle ids are server-allocated: destroyed before `stop` is processed, a late event naming one
  // is a fatal protocol error
  for (auto& workspace : workspaces_) {
    ext_workspace_handle_v1_destroy(workspace->handle);
  }
  workspaces_.clear();
  for (auto* group : workspace_groups_) {
    ext_workspace_group_handle_v1_destroy(group);
  }
  workspace_groups_.clear();

  if (manager_) {
    struct wl_display* display = Client::inst()->wl_display;
    /*
     * Send `stop` request and wait for one roundtrip.
     * This is not quite correct as the protocol encourages us to wait for the .finished event,
     * but it should work with wlroots foreign toplevel manager implementation.
     */
    zwlr_foreign_toplevel_manager_v1_stop(manager_);
    wl_display_roundtrip(display);

    if (manager_) {
      spdlog::warn("Foreign toplevel manager destroyed before .finished event");
      zwlr_foreign_toplevel_manager_v1_destroy(manager_);
      manager_ = nullptr;
    }
  }

  if (config_["bar-css-states"].asBool()) {
    set_bar_css_class("toplevel-active", false);
    set_bar_css_class("toplevel-maximized", false);
    set_bar_css_class("toplevel-minimized", false);
    set_bar_css_class("toplevel-fullscreen", false);
  }
}

void Taskbar::notify_icon_theme_changed() {
  spdlog::debug("Taskbar: icon theme changed, queueing a reload of every task icon");
  icon_theme_dirty_ = true;
  if (!update_queued_) {
    update_queued_ = true;
    dp.emit();
  }
}

void Taskbar::update() {
  update_queued_ = false;
  if (std::exchange(icon_theme_dirty_, false)) {
    spdlog::debug("Taskbar: reloading all task icons (deferred from theme change)");
    for (auto& t : tasks_) {
      t->reload_icon();
    }
  }
  for (auto& t : tasks_) {
    t->update();
  }

  if (config_["sort-by-app-id"].asBool()) {
    std::stable_sort(tasks_.begin(), tasks_.end(),
                     [](const std::unique_ptr<Task>& a, const std::unique_ptr<Task>& b) {
                       return a->app_id() < b->app_id();
                     });

    for (unsigned long i = 0; i < tasks_.size(); i++) {
      move_button(tasks_[i]->button, i);
    }
  }

#ifdef HAVE_HYPRLAND
  if (config_["sort-by-hyprland-workspaces"].asBool()) {
    // Check if Hyprland is running
    const char* his = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (his != nullptr) {
      try {
        Json::Value clients = hyprland::IPC::inst().getSocket1JsonReply("clients");
        if (clients.isArray()) {
          // Build a list of all clients with their sort keys
          // Using a vector to handle multiple windows with same class/title
          struct ClientInfo {
            std::string cls;
            std::string title;
            int workspace_id;
            int x;
            int y;
          };
          std::vector<ClientInfo> allClients;
          for (Json::ArrayIndex i = 0; i < clients.size(); ++i) {
            const auto& client = clients[i];
            ClientInfo info;
            info.cls = client["class"].asString();
            info.title = client["title"].asString();
            info.workspace_id = client["workspace"]["id"].asInt();
            info.x = client["at"][0].asInt();
            info.y = client["at"][1].asInt();
            allClients.push_back(info);
          }

          // Sort clients by workspace, then x, then y
          std::sort(allClients.begin(), allClients.end(),
                    [](const ClientInfo& a, const ClientInfo& b) {
                      if (a.workspace_id != b.workspace_id) return a.workspace_id < b.workspace_id;
                      if (a.x != b.x) return a.x < b.x;
                      return a.y < b.y;
                    });

          // Build a map from (class, title) to list of sort indices (for windows with same
          // class/title)
          std::map<std::pair<std::string, std::string>, std::vector<int>> clientIndices;
          for (size_t i = 0; i < allClients.size(); ++i) {
            auto key = std::make_pair(allClients[i].cls, allClients[i].title);
            clientIndices[key].push_back(static_cast<int>(i));
          }

          // Track how many times we've seen each (class, title) pair during sorting
          std::map<std::pair<std::string, std::string>, size_t> usedCount;

          // Assign a sort index to each task
          std::vector<int> taskSortIndex(tasks_.size(), INT_MAX);
          for (size_t t = 0; t < tasks_.size(); ++t) {
            auto key = std::make_pair(tasks_[t]->app_id(), tasks_[t]->title());
            auto it = clientIndices.find(key);
            if (it != clientIndices.end()) {
              size_t& used = usedCount[key];
              if (used < it->second.size()) {
                taskSortIndex[t] = it->second[used];
                ++used;
              }
            }
          }

          // Sort tasks by their assigned sort index
          std::vector<size_t> taskOrder(tasks_.size());
          std::iota(taskOrder.begin(), taskOrder.end(), 0);
          std::stable_sort(
              taskOrder.begin(), taskOrder.end(),
              [&taskSortIndex](size_t a, size_t b) { return taskSortIndex[a] < taskSortIndex[b]; });

          // Reorder tasks_ according to taskOrder
          std::vector<TaskPtr> sortedTasks;
          sortedTasks.reserve(tasks_.size());
          for (size_t idx : taskOrder) {
            sortedTasks.push_back(std::move(tasks_[idx]));
          }
          tasks_ = std::move(sortedTasks);

          for (unsigned long i = 0; i < tasks_.size(); i++) {
            move_button(tasks_[i]->button, i);
          }
        }
      } catch (const std::exception& e) {
        spdlog::warn("Failed to query Hyprland clients for taskbar sorting: {}", e.what());
      }
    }
  }
#endif  // HAVE_HYPRLAND

  AModule::update();
}

#ifdef HAVE_HYPRLAND
void Taskbar::onEvent(const std::string& ev) {
  // Hyprland window event received - trigger update to re-sort
  spdlog::debug("Taskbar received Hyprland event: {}", ev);
  dp.emit();
}
#endif

void Taskbar::setupControlSocket() {
  const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
  std::string display_name = wayland_display ? wayland_display : "wayland-0";

  // $XDG_RUNTIME_DIR is per-user and already 0700, so a socket there is not
  // reachable by other users at all. /tmp is world-writable and was the old
  // location: anything running as any user could connect and activate windows.
  // The uid in the fallback name keeps two users from colliding on a machine
  // where XDG_RUNTIME_DIR somehow is not set; the peer check in socketListener()
  // is what actually enforces the boundary in either case.
  const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  if (runtime_dir != nullptr && *runtime_dir != '\0') {
    socket_path_ = fmt::format("{}/waybar-taskbar-{}.sock", runtime_dir, display_name);
  } else {
    socket_path_ = fmt::format("/tmp/waybar-taskbar-{}-{}.sock", getuid(), display_name);
    spdlog::warn("XDG_RUNTIME_DIR is not set; taskbar control socket falls back to {}",
                 socket_path_);
  }

  // Remove existing socket file if it exists
  unlink(socket_path_.c_str());

  socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    spdlog::error("Failed to create taskbar control socket: {}", strerror(errno));
    return;
  }

  // Create the socket with 0600 rather than whatever the umask happens to be.
  // Set before bind(), because the permissions of a unix socket are taken from
  // the mode in effect at bind time; a later chmod races against connects.
  mode_t old_umask = umask(0077);

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    spdlog::error("Taskbar control socket path is too long: {}", socket_path_);
    umask(old_umask);
    close(socket_fd_);
    socket_fd_ = -1;
    return;
  }
  strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    umask(old_umask);
    spdlog::error("Failed to bind taskbar control socket: {}", strerror(errno));
    close(socket_fd_);
    socket_fd_ = -1;
    return;
  }

  if (listen(socket_fd_, 5) < 0) {
    umask(old_umask);
    spdlog::error("Failed to listen on taskbar control socket: {}", strerror(errno));
    close(socket_fd_);
    socket_fd_ = -1;
    return;
  }

  umask(old_umask);

  socket_running_ = true;
  socket_thread_ = std::thread(&Taskbar::socketListener, this);
  spdlog::info("Taskbar control socket listening at {}", socket_path_);
}

void Taskbar::cleanupControlSocket() {
  socket_running_ = false;
  if (socket_fd_ >= 0) {
    shutdown(socket_fd_, SHUT_RDWR);
    close(socket_fd_);
    socket_fd_ = -1;
  }
  if (socket_thread_.joinable()) {
    socket_thread_.join();
  }
  if (!socket_path_.empty()) {
    unlink(socket_path_.c_str());
  }
}

void Taskbar::socketListener() {
  while (socket_running_) {
    struct pollfd pfd;
    pfd.fd = socket_fd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, 100);  // 100ms timeout
    if (ret <= 0) continue;

    int client_fd = accept(socket_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (socket_running_) {
        spdlog::error("Error accepting connection: {}", strerror(errno));
      }
      continue;
    }

    // These commands focus and raise windows, so only this user may issue them.
    // The socket lives under $XDG_RUNTIME_DIR and is 0600, but that is a
    // property of the path; this is the check that holds even if the socket ends
    // up somewhere more permissive. SO_PEERCRED is the kernel's own view of who
    // connected and cannot be spoofed by the peer.
    struct ucred cred;
    socklen_t cred_len = sizeof(cred);
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) < 0) {
      spdlog::warn("Rejecting taskbar control connection: no peer credentials ({})",
                   strerror(errno));
      close(client_fd);
      continue;
    }
    if (cred.uid != getuid()) {
      spdlog::warn("Rejecting taskbar control connection from uid {} (pid {})", cred.uid, cred.pid);
      close(client_fd);
      continue;
    }

    char buffer[256];
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
    close(client_fd);

    if (n > 0) {
      buffer[n] = '\0';
      std::string line(buffer);

      // Trim whitespace
      line.erase(0, line.find_first_not_of(" \t\r\n"));
      auto pos = line.find_last_not_of(" \t\r\n");
      if (pos != std::string::npos) {
        line.erase(pos + 1);
      }

      if (!line.empty()) {
        spdlog::debug("Taskbar received command: {}", line);
        // Dispatch to main thread
        Glib::signal_idle().connect_once([this, line]() { handleCommand(line); });
      }
    }
  }
}

void Taskbar::handleCommand(const std::string& cmd) {
  if (cmd == "next") {
    selectNext();
  } else if (cmd == "prev") {
    selectPrev();
  } else if (cmd == "activate") {
    activateSelected();
  } else if (cmd == "clear" || cmd == "cancel") {
    clearSelection();
  } else if (cmd == "refresh") {
    dp.emit();
  } else {
    spdlog::warn("Unknown taskbar command: {}", cmd);
  }
}

void Taskbar::selectNext() {
  if (tasks_.empty()) return;

  int old_index = selection_index_;
  if (selection_index_ < 0) {
    // Start from the previously active window (MRU order)
    selection_index_ = 0;  // fallback to first
    spdlog::info("Taskbar::selectNext: looking for previous_active_id_={}, current_active_id_={}",
                 previous_active_id_, current_active_id_);
    for (size_t i = 0; i < tasks_.size(); ++i) {
      spdlog::info("  Task[{}]: id={}, title={}, active={}", i, tasks_[i]->id(), tasks_[i]->title(),
                   tasks_[i]->active());
      if (tasks_[i]->id() == previous_active_id_) {
        selection_index_ = static_cast<int>(i);
        break;
      }
    }
    spdlog::info("Taskbar::selectNext: selected index {}", selection_index_);
  } else {
    selection_index_ = (selection_index_ + 1) % static_cast<int>(tasks_.size());
  }
  updateSelection(old_index);
}

void Taskbar::selectPrev() {
  if (tasks_.empty()) return;

  int old_index = selection_index_;
  if (selection_index_ < 0) {
    // Start from the previously active window (MRU order)
    selection_index_ = static_cast<int>(tasks_.size()) - 1;  // fallback to last
    for (size_t i = 0; i < tasks_.size(); ++i) {
      if (tasks_[i]->id() == previous_active_id_) {
        selection_index_ = static_cast<int>(i);
        break;
      }
    }
  } else {
    selection_index_ =
        (selection_index_ - 1 + static_cast<int>(tasks_.size())) % static_cast<int>(tasks_.size());
  }
  updateSelection(old_index);
}

void Taskbar::activateSelected() {
  if (selection_index_ >= 0 && selection_index_ < static_cast<int>(tasks_.size())) {
    tasks_[selection_index_]->activate();
  }
  clearSelection();
}

void Taskbar::clearSelection() {
  int old_index = selection_index_;
  selection_index_ = -1;
  updateSelection(old_index);
}

void Taskbar::updateSelection(int old_index) {
  // Remove class from old selection
  if (old_index >= 0 && old_index < static_cast<int>(tasks_.size())) {
    tasks_[old_index]->button.get_style_context()->remove_class("keyboard-selected");
  }
  // Add class to new selection
  if (selection_index_ >= 0 && selection_index_ < static_cast<int>(tasks_.size())) {
    tasks_[selection_index_]->button.get_style_context()->add_class("keyboard-selected");
  }
}

void Taskbar::notifyActiveChanged(uint32_t new_active_id) {
  // Save the current active as previous, then update current
  if (current_active_id_ != 0 && current_active_id_ != new_active_id) {
    spdlog::info("Taskbar: previous_active changing from {} to {}, new current: {}",
                 previous_active_id_, current_active_id_, new_active_id);
    previous_active_id_ = current_active_id_;
  }
  current_active_id_ = new_active_id;
}

static void tm_handle_toplevel(void* data, struct zwlr_foreign_toplevel_manager_v1* manager,
                               struct zwlr_foreign_toplevel_handle_v1* tl_handle) {
  return static_cast<Taskbar*>(data)->handle_toplevel_create(tl_handle);
}

static void tm_handle_finished(void* data, struct zwlr_foreign_toplevel_manager_v1* manager) {
  return static_cast<Taskbar*>(data)->handle_finished();
}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_impl = {
    .toplevel = tm_handle_toplevel,
    .finished = tm_handle_finished,
};

static void workspace_handle_id(void*, struct ext_workspace_handle_v1*, const char*) {}
static void workspace_handle_name(void*, struct ext_workspace_handle_v1*, const char*) {}
static void workspace_handle_coordinates(void*, struct ext_workspace_handle_v1*, struct wl_array*) {
}

static void workspace_handle_state(void* data, struct ext_workspace_handle_v1*, uint32_t state) {
  static_cast<Taskbar::WorkspaceState*>(data)->state = state;
}

static void workspace_handle_capabilities(void*, struct ext_workspace_handle_v1*, uint32_t) {}

static void workspace_handle_removed(void* data, struct ext_workspace_handle_v1* handle) {
  static_cast<Taskbar::WorkspaceState*>(data)->taskbar->handle_workspace_removed(handle);
}

static const struct ext_workspace_handle_v1_listener workspace_handle_impl = {
    .id = workspace_handle_id,
    .name = workspace_handle_name,
    .coordinates = workspace_handle_coordinates,
    .state = workspace_handle_state,
    .capabilities = workspace_handle_capabilities,
    .removed = workspace_handle_removed,
};

static void workspace_group_handle_capabilities(void*, struct ext_workspace_group_handle_v1*,
                                                uint32_t) {}
static void workspace_group_handle_output_enter(void*, struct ext_workspace_group_handle_v1*,
                                                struct wl_output*) {}
static void workspace_group_handle_output_leave(void*, struct ext_workspace_group_handle_v1*,
                                                struct wl_output*) {}
static void workspace_group_handle_workspace_enter(void*, struct ext_workspace_group_handle_v1*,
                                                   struct ext_workspace_handle_v1*) {}
static void workspace_group_handle_workspace_leave(void*, struct ext_workspace_group_handle_v1*,
                                                   struct ext_workspace_handle_v1*) {}

static void workspace_group_handle_removed(void* data,
                                           struct ext_workspace_group_handle_v1* group) {
  static_cast<Taskbar*>(data)->handle_workspace_group_removed(group);
}

static const struct ext_workspace_group_handle_v1_listener workspace_group_impl = {
    .capabilities = workspace_group_handle_capabilities,
    .output_enter = workspace_group_handle_output_enter,
    .output_leave = workspace_group_handle_output_leave,
    .workspace_enter = workspace_group_handle_workspace_enter,
    .workspace_leave = workspace_group_handle_workspace_leave,
    .removed = workspace_group_handle_removed,
};

static void workspace_manager_handle_group(void* data, struct ext_workspace_manager_v1*,
                                           struct ext_workspace_group_handle_v1* group) {
  static_cast<Taskbar*>(data)->handle_workspace_group_create(group);
}

static void workspace_manager_handle_workspace(void* data, struct ext_workspace_manager_v1*,
                                               struct ext_workspace_handle_v1* workspace) {
  static_cast<Taskbar*>(data)->handle_workspace_create(workspace);
}

static void workspace_manager_handle_done(void* data, struct ext_workspace_manager_v1*) {
  static_cast<Taskbar*>(data)->handle_workspace_done();
}

static void workspace_manager_handle_finished(void* data, struct ext_workspace_manager_v1*) {
  static_cast<Taskbar*>(data)->handle_workspace_finished();
}

static const struct ext_workspace_manager_v1_listener workspace_manager_impl = {
    .workspace_group = workspace_manager_handle_group,
    .workspace = workspace_manager_handle_workspace,
    .done = workspace_manager_handle_done,
    .finished = workspace_manager_handle_finished,
};

void Taskbar::register_manager(struct wl_registry* registry, uint32_t name, uint32_t version) {
  if (manager_) {
    spdlog::warn("Register foreign toplevel manager again although already existing!");
    return;
  }
  if (version < ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_SET_FULLSCREEN_SINCE_VERSION) {
    spdlog::warn(
        "Foreign toplevel manager server does not have the appropriate version."
        " To be able to use all features, you need at least version 2, but server is version {}",
        version);
  }

  // limit version to a highest supported by the client protocol file
  version = std::min<uint32_t>(version, zwlr_foreign_toplevel_manager_v1_interface.version);

  manager_ = static_cast<struct zwlr_foreign_toplevel_manager_v1*>(
      wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, version));

  if (manager_)
    zwlr_foreign_toplevel_manager_v1_add_listener(manager_, &toplevel_manager_impl, this);
  else
    spdlog::debug("Failed to register manager");
}

void Taskbar::register_workspace_manager(struct wl_registry* registry, uint32_t name,
                                         uint32_t version) {
  if (workspace_manager_) {
    return;
  }

  version = std::min<uint32_t>(version, ext_workspace_manager_v1_interface.version);
  workspace_manager_ = static_cast<struct ext_workspace_manager_v1*>(
      wl_registry_bind(registry, name, &ext_workspace_manager_v1_interface, version));
  ext_workspace_manager_v1_add_listener(workspace_manager_, &workspace_manager_impl, this);
}

void Taskbar::register_seat(struct wl_registry* registry, uint32_t name, uint32_t version) {
  if (seat_) {
    spdlog::warn("Register seat again although already existing!");
    return;
  }
  version = std::min<uint32_t>(version, wl_seat_interface.version);

  seat_ = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, version));
}

void Taskbar::handle_toplevel_create(struct zwlr_foreign_toplevel_handle_v1* tl_handle) {
  tasks_.push_back(std::make_unique<Task>(bar_, config_, this, tl_handle, seat_));
}

void Taskbar::handle_finished() {
  zwlr_foreign_toplevel_manager_v1_destroy(manager_);
  manager_ = nullptr;
}

void Taskbar::handle_workspace_group_create(struct ext_workspace_group_handle_v1* handle) {
  ext_workspace_group_handle_v1_add_listener(handle, &workspace_group_impl, this);
  workspace_groups_.push_back(handle);
}

void Taskbar::handle_workspace_group_removed(struct ext_workspace_group_handle_v1* handle) {
  const auto group = std::find(workspace_groups_.begin(), workspace_groups_.end(), handle);
  if (group != workspace_groups_.end()) {
    ext_workspace_group_handle_v1_destroy(*group);
    workspace_groups_.erase(group);
  }
}

void Taskbar::handle_workspace_create(struct ext_workspace_handle_v1* handle) {
  auto workspace = std::make_unique<WorkspaceState>(WorkspaceState{this, handle});
  ext_workspace_handle_v1_add_listener(handle, &workspace_handle_impl, workspace.get());
  workspaces_.push_back(std::move(workspace));
}

void Taskbar::handle_workspace_done() {
  const auto active_workspace =
      std::find_if(workspaces_.begin(), workspaces_.end(), [](const auto& workspace) {
        return workspace->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
      });
  current_workspace_ =
      active_workspace == workspaces_.end() ? nullptr : (*active_workspace)->handle;
  if (current_workspace_) {
    const auto active_task =
        std::find_if(tasks_.begin(), tasks_.end(), [](const auto& task) { return task->active(); });
    if (active_task != tasks_.end()) {
      (*active_task)->set_workspace(current_workspace_);
    }
  }
  update_bar_css_classes();
}

void Taskbar::handle_workspace_finished() { workspace_manager_ = nullptr; }

void Taskbar::handle_workspace_removed(struct ext_workspace_handle_v1* handle) {
  if (current_workspace_ == handle) {
    current_workspace_ = nullptr;
  }
  for (auto& task : tasks_) {
    if (task->workspace() == handle) {
      task->set_workspace(nullptr);
    }
  }

  const auto workspace =
      std::find_if(workspaces_.begin(), workspaces_.end(),
                   [handle](const auto& workspace) { return workspace->handle == handle; });
  if (workspace != workspaces_.end()) {
    ext_workspace_handle_v1_destroy((*workspace)->handle);
    workspaces_.erase(workspace);
  }
  update_bar_css_classes();
}

void Taskbar::add_button(Gtk::Button& bt) {
  /* When "homogeneous" is enabled, let every child expand and fill so the buttons
   * divide the available width equally. Otherwise, only let the buttons expand to
   * fill the taskbar when "expand" is enabled and the bar is horizontal (see the
   * Task constructor for details). */
  const bool homogeneous = config_["homogeneous"].isBool() && config_["homogeneous"].asBool();
  bool expand = config_["expand"].isBool() && config_["expand"].asBool();
  bool horizontal = bar_.orientation == Gtk::ORIENTATION_HORIZONTAL;
  if (homogeneous) {
    box_.pack_start(bt, true, true);
    bt.set_hexpand(true);
    bt.set_halign(Gtk::ALIGN_FILL);
  } else if (expand && horizontal) {
    box_.pack_start(bt, true, true);
  } else {
    box_.pack_start(bt, false, false);
  }
  box_.get_style_context()->remove_class("empty");
}

void Taskbar::move_button(Gtk::Button& bt, int pos) { box_.reorder_child(bt, pos); }

bool Taskbar::reorder_task(uint32_t dragged_id, uint32_t destination_id) {
  std::vector<uint32_t> task_ids;
  task_ids.reserve(tasks_.size());
  std::transform(tasks_.begin(), tasks_.end(), std::back_inserter(task_ids),
                 [](const TaskPtr& task) { return task->id(); });

  auto positions = dnd::reorder_positions(task_ids, dragged_id, destination_id);
  if (!positions) return false;

  auto* dragged = tasks_[positions->first].get();
  auto* destination = tasks_[positions->second].get();
  if (dragged == destination) return true;
  if (dragged->button.get_parent() != &box_ || destination->button.get_parent() != &box_) {
    return false;
  }

  auto position = box_.child_property_position(destination->button).get_value();
  box_.reorder_child(dragged->button, position);
  return true;
}

void Taskbar::remove_button(Gtk::Button& bt) {
  box_.remove(bt);
  if (box_.get_children().empty()) {
    box_.get_style_context()->add_class("empty");
  }
}

void Taskbar::remove_task(uint32_t id) {
  // Clear stale IDs if the removed task was tracked
  if (previous_active_id_ == id) {
    spdlog::info("Taskbar: clearing previous_active_id_ {} (task closed)", id);
    previous_active_id_ = 0;
  }
  if (current_active_id_ == id) {
    spdlog::info("Taskbar: clearing current_active_id_ {} (task closed)", id);
    current_active_id_ = 0;
  }

  auto it = std::find_if(std::begin(tasks_), std::end(tasks_),
                         [id](const TaskPtr& p) { return p->id() == id; });

  if (it == std::end(tasks_)) {
    spdlog::warn("Can't find task with id {}", id);
    return;
  }

  tasks_.erase(it);
  update_bar_css_classes();
}

void Taskbar::assign_current_workspace(Task& task) {
  if (current_workspace_) {
    task.set_workspace(current_workspace_);
  }
}

void Taskbar::update_bar_css_classes() {
  if (!config_["bar-css-states"].asBool()) {
    return;
  }

  const auto active_task = std::find_if(tasks_.begin(), tasks_.end(), [](const TaskPtr& task) {
    return task->visible() && task->active();
  });

  const bool has_active_task = active_task != tasks_.end();
  const auto on_current_workspace = [this](const TaskPtr& task) {
    if (!current_workspace_) {
      return task->active();
    }
    return task->workspace() == current_workspace_;
  };
  const bool has_maximized_task =
      std::any_of(tasks_.begin(), tasks_.end(), [&on_current_workspace](const TaskPtr& task) {
        return task->visible() && !task->minimized() && on_current_workspace(task) &&
               task->maximized();
      });
  const bool has_fullscreen_task =
      std::any_of(tasks_.begin(), tasks_.end(), [&on_current_workspace](const TaskPtr& task) {
        return task->visible() && !task->minimized() && on_current_workspace(task) &&
               task->fullscreen();
      });
  set_bar_css_class("toplevel-active", has_active_task);
  set_bar_css_class("toplevel-maximized", has_maximized_task);
  set_bar_css_class("toplevel-minimized", has_active_task && (*active_task)->minimized());
  set_bar_css_class("toplevel-fullscreen", has_fullscreen_task);
}

void Taskbar::set_bar_css_class(const std::string& class_name, bool enabled) {
  const auto style = bar_.window.get_style_context();
  if (enabled && !style->has_class(class_name)) {
    spdlog::trace("Adding bar class: {}", class_name);
    style->add_class(class_name);
  } else if (!enabled && style->has_class(class_name)) {
    spdlog::trace("Removing bar class: {}", class_name);
    style->remove_class(class_name);
  }
}

bool Taskbar::show_output(struct wl_output* output) const {
  return output == gdk_wayland_monitor_get_wl_output(bar_.output->monitor->gobj());
}

bool Taskbar::all_outputs() const {
  return config_["all-outputs"].isBool() && config_["all-outputs"].asBool();
}

const IconLoader& Taskbar::icon_loader() const { return icon_loader_; }

const std::unordered_set<std::string>& Taskbar::ignore_list() const { return ignore_list_; }

const std::unordered_set<std::string>& Taskbar::squash_list() const { return squash_list_; }

const std::map<std::string, std::string>& Taskbar::app_ids_replace_map() const {
  return app_ids_replace_map_;
}

const std::map<std::string, std::string>& Taskbar::fg_process_map() const {
  return fg_process_map_;
}

} /* namespace waybar::modules::wlr */
