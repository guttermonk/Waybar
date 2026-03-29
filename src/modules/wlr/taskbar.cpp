#include "modules/wlr/taskbar.hpp"

#include <fmt/core.h>
#include <gdkmm/monitor.h>
#include <gio/gdesktopappinfo.h>
#include <giomm/desktopappinfo.h>
#include <gtkmm/icontheme.h>
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <dirent.h>

#include <algorithm>
#include <numeric>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <utility>
#include <unordered_set>

#include "gdkmm/general.h"
#include "modules/hyprland/backend.hpp"
#include "glibmm/error.h"
#include "glibmm/fileutils.h"
#include "glibmm/refptr.h"
#include "util/format.hpp"
#include "util/gtk_icon.hpp"
#include "util/rewrite_string.hpp"
#include "util/string.hpp"

namespace waybar::modules::wlr {

/* Task class implementation */

static const std::unordered_set<std::string> KNOWN_TERMINALS = {
    "kitty", "alacritty", "foot", "wezterm", "wezterm-gui",
    "gnome-terminal", "xterm", "urxvt", "st", "konsole",
    "termite", "tilix", "rxvt", "mlterm"
};

static const std::unordered_set<std::string> KNOWN_SHELLS = {
    "bash", "zsh", "fish", "sh", "dash", "ksh", "tcsh", "csh", "nu", "elvish"
};

// Case-insensitive terminal check — app_id values differ across compositors
// (e.g. Hyprland reports "Alacritty" while the binary is "alacritty").
static bool isKnownTerminal(const std::string &id) {
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
    static const std::unordered_set<std::string> SKIP_PROCS = { "kitten" };

    struct ProcInfo {
        pid_t pid;
        std::string comm;
        pid_t ppid;
        pid_t pgrp;
        pid_t tpgid; // foreground pgrp of the controlling terminal
    };

    // Single pass: read every /proc/[pid]/stat entry we can access.
    std::vector<ProcInfo> all_procs;
    {
        DIR *proc_dir = opendir("/proc");
        if (!proc_dir) return "";
        struct dirent *pent;
        while ((pent = readdir(proc_dir)) != nullptr) {
            bool is_numeric = true;
            for (const char *c = pent->d_name; *c; ++c) {
                if (!isdigit(static_cast<unsigned char>(*c))) { is_numeric = false; break; }
            }
            if (!is_numeric) continue;

            std::string stat_path = "/proc/" + std::string(pent->d_name) + "/stat";
            FILE *f = fopen(stat_path.c_str(), "r");
            if (!f) continue;

            ProcInfo info;
            char comm_buf[256] = {};
            int ppid = 0, pgrp = 0, session = 0, tty_nr = 0, tpgid = 0;
            int ret = fscanf(f, "%d (%255[^)]) %*c %d %d %d %d %d",
                             &info.pid, comm_buf, &ppid, &pgrp, &session, &tty_nr, &tpgid);
            fclose(f);
            if (ret < 7) continue;

            info.comm  = comm_buf;
            info.ppid  = static_cast<pid_t>(ppid);
            info.pgrp  = static_cast<pid_t>(pgrp);
            info.tpgid = static_cast<pid_t>(tpgid);
            all_procs.push_back(std::move(info));
        }
        closedir(proc_dir);
    }

    // Collect direct children of the terminal PID.
    std::vector<const ProcInfo *> children;
    for (const auto &p : all_procs) {
        if (p.ppid == terminal_pid) children.push_back(&p);
    }

    if (children.empty()) {
        spdlog::warn("fgicon: no children found for pid {}", terminal_pid);
        return "";
    }

    for (const auto *child : children) {
        spdlog::warn("fgicon: child pid={} comm='{}' pgrp={} tpgid={}",
                     child->pid, child->comm, child->pgrp, child->tpgid);

        // Skip the terminal's own helper processes and nested terminals.
        if (SKIP_PROCS.count(child->comm) || isKnownTerminal(child->comm)) continue;

        if (KNOWN_SHELLS.count(child->comm)) {
            // The child is a shell. tpgid tells us what is in the foreground.
            pid_t fg_pgrp = child->tpgid;
            if (fg_pgrp <= 0 || fg_pgrp == child->pgrp) {
                // Shell itself is in the foreground — nothing interesting running.
                spdlog::warn("fgicon: shell '{}' is in foreground, skipping", child->comm);
                continue;
            }
            // Find the process in the foreground group.
            for (const auto &p : all_procs) {
                if (p.pgrp == fg_pgrp && !KNOWN_SHELLS.count(p.comm)) {
                    spdlog::warn("fgicon: fg process is '{}' (pgrp {})", p.comm, fg_pgrp);
                    return p.comm;
                }
            }
        } else {
            // Non-shell direct child of the terminal (e.g. kitty running yazi directly).
            spdlog::warn("fgicon: direct non-shell child is '{}'", child->comm);
            return child->comm;
        }
    }

    spdlog::warn("fgicon: no foreground app found for terminal pid {}", terminal_pid);
    return "";
}

// Search all installed .desktop files for one whose Exec= basename matches
// proc_name. Handles apps where the binary name differs from the desktop file
// name, e.g. hx (helix) or vi (neovim).
static Glib::RefPtr<Gio::DesktopAppInfo> findAppInfoByExec(const std::string &proc_name) {
    // Build the exec→desktop map once. Calling Gio::AppInfo::get_all() on
    // every title-change event emits a GIO "changed" signal that causes GTK
    // to invalidate its icon theme cache, wiping all taskbar icons at once.
    static std::unordered_map<std::string, Glib::RefPtr<Gio::DesktopAppInfo>> s_cache;
    static bool s_built = false;
    if (!s_built) {
        s_built = true;
        for (const auto &app : Gio::AppInfo::get_all()) {
            auto desktop = Glib::RefPtr<Gio::DesktopAppInfo>::cast_dynamic(app);
            if (!desktop) continue;
            std::string exec = app->get_executable();
            auto slash = exec.rfind('/');
            if (slash != std::string::npos) exec = exec.substr(slash + 1);
            if (s_cache.find(exec) == s_cache.end())
                s_cache[exec] = desktop;
        }
    }
    auto it = s_cache.find(proc_name);
    return it != s_cache.end() ? it->second : Glib::RefPtr<Gio::DesktopAppInfo>{};
}

uint32_t Task::global_id = 1;  // Start from 1 so 0 can be used as "no window" sentinel

static void tl_handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                            const char *title) {
  return static_cast<Task *>(data)->handle_title(title);
}

static void tl_handle_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                             const char *app_id) {
  return static_cast<Task *>(data)->handle_app_id(app_id);
}

static void tl_handle_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                                   struct wl_output *output) {
  return static_cast<Task *>(data)->handle_output_enter(output);
}

static void tl_handle_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                                   struct wl_output *output) {
  return static_cast<Task *>(data)->handle_output_leave(output);
}

static void tl_handle_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                            struct wl_array *state) {
  return static_cast<Task *>(data)->handle_state(state);
}

static void tl_handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
  return static_cast<Task *>(data)->handle_done();
}

static void tl_handle_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                             struct zwlr_foreign_toplevel_handle_v1 *parent) {
  /* This is explicitly left blank */
}

static void tl_handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
  return static_cast<Task *>(data)->handle_closed();
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

Task::Task(const waybar::Bar &bar, const Json::Value &config, Taskbar *tbar,
           struct zwlr_foreign_toplevel_handle_v1 *tl_handle, struct wl_seat *seat)
    : bar_{bar},
      config_{config},
      tbar_{tbar},
      handle_{tl_handle},
      seat_{seat},
      id_{global_id++},
      content_{bar.orientation, 0} {
  zwlr_foreign_toplevel_handle_v1_add_listener(handle_, &toplevel_handle_impl, this);

  button.set_relief(Gtk::RELIEF_NONE);

  content_.add(text_before_);
  content_.add(icon_);
  content_.add(text_after_);

  content_.show();
  button.add(content_);

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

void Task::handle_title(const char *title) {
  if (title_.empty()) {
    spdlog::debug(fmt::format("Task ({}) setting title to {}", id_, title_));
  } else {
    spdlog::debug(fmt::format("Task ({}) overwriting title '{}' with '{}'", id_, title_, title));
  }
  title_ = title;
  hide_if_ignored();

  if (!with_icon_ && !with_name_) {
    return;
  }

  // For terminal emulators re-evaluate the foreground process on every title
  // change — the title changing is the signal that a new program started.
  if (isKnownTerminal(app_id_)) {
    if (tryUpdateIconFromTerminalFg()) return;
    // fg detection failed — fall back to the terminal's own icon
    app_info_ = IconLoader::get_app_info_from_app_id_list(app_id_);
    name_ = app_info_ ? app_info_->get_display_name() : app_id_.c_str();
  } else {
    if (app_info_) return; // non-terminal: icon already set by handle_app_id
    app_info_ = IconLoader::get_app_info_from_app_id_list(title_);
    name_ = app_info_ ? app_info_->get_display_name() : title;
  }

  if (!with_icon_) {
    return;
  }

  int icon_size = config_["icon-size"].isInt() ? config_["icon-size"].asInt() : 16;
  if (tbar_->icon_loader().image_load_icon(icon_, app_info_, icon_size))
    icon_.show();
  else
    spdlog::debug("Couldn't find icon for {}", app_id_.empty() ? title : app_id_.c_str());
}

void Task::set_minimize_hint() {
  zwlr_foreign_toplevel_handle_v1_set_rectangle(handle_, bar_.surface, minimize_hint.x,
                                                minimize_hint.y, minimize_hint.w, minimize_hint.h);
}

void Task::hide_if_ignored() {
  if (tbar_->ignore_list().count(app_id_) || tbar_->ignore_list().count(title_)) {
    ignored_ = true;
    if (button_visible_) {
      auto output = gdk_wayland_monitor_get_wl_output(bar_.output->monitor->gobj());
      handle_output_leave(output);
    }
  } else {
    bool is_was_ignored = ignored_;
    ignored_ = false;
    if (is_was_ignored) {
      auto output = gdk_wayland_monitor_get_wl_output(bar_.output->monitor->gobj());
      handle_output_enter(output);
    }
  }
}

void Task::handle_app_id(const char *app_id) {
  if (app_id_.empty()) {
    spdlog::debug(fmt::format("Task ({}) setting app_id to {}", id_, app_id));
  } else {
    spdlog::debug(fmt::format("Task ({}) overwriting app_id '{}' with '{}'", id_, app_id_, app_id));
  }
  app_id_ = app_id;
  hide_if_ignored();

  auto ids_replace_map = tbar_->app_ids_replace_map();
  if (ids_replace_map.count(app_id_)) {
    auto replaced_id = ids_replace_map[app_id_];
    spdlog::debug(
        fmt::format("Task ({}) [{}] app_id was replaced with {}", id_, app_id_, replaced_id));
    app_id_ = replaced_id;
  }

  if (!with_icon_ && !with_name_) {
    return;
  }

  app_info_ = IconLoader::get_app_info_from_app_id_list(app_id_);
  name_ = app_info_ ? app_info_->get_display_name() : app_id;

  if (!with_icon_) {
    return;
  }

  int icon_size = config_["icon-size"].isInt() ? config_["icon-size"].asInt() : 16;
  if (tbar_->icon_loader().image_load_icon(icon_, app_info_, icon_size))
    icon_.show();
  else
    spdlog::debug("Couldn't find icon for {}", app_id_);

  // For terminals that were already open when Waybar started, try immediately
  // to show the foreground process icon (title may not have arrived yet, but
  // it's worth attempting with whatever we have).
  if (isKnownTerminal(app_id_)) {
    tryUpdateIconFromTerminalFg();
  }
}

// Query Hyprland IPC for the PID of the window matching this task, then walk
// /proc to find what is running in the terminal's foreground.
// Multiplexers (tmux, zellij, …) are returned as-is so their own icon is shown.
// Returns true and updates icon_/app_info_/name_ on success.
bool Task::tryUpdateIconFromTerminalFg() {
  spdlog::warn("fgicon: tryUpdateIconFromTerminalFg called for app_id='{}' title='{}'", app_id_, title_);
  if (!with_icon_ && !with_name_) {
    spdlog::warn("fgicon: skipping — with_icon_={} with_name_={}", with_icon_, with_name_);
    return false;
  }
  const char* his = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (his == nullptr) {
    spdlog::warn("fgicon: HYPRLAND_INSTANCE_SIGNATURE not set");
    return false;
  }
  // Lazily initialize IPC if it hasn't been set up yet (e.g. when
  // sort-by-hyprland-workspaces is not enabled in the config).
  if (!hyprland::gIPC) {
    spdlog::warn("fgicon: gIPC is null, initializing");
    hyprland::modulesReady = true;
    hyprland::gIPC = std::make_unique<hyprland::IPC>();
  }

  try {
    Json::Value clients = hyprland::gIPC->getSocket1JsonReply("clients");
    if (!clients.isArray()) {
      spdlog::warn("fgicon: clients IPC reply is not an array");
      return false;
    }
    spdlog::warn("fgicon: got {} clients from IPC", clients.size());

    // Match this task to a Hyprland client by class + title to get its PID.
    // Fall back to class-only if no exact title match exists — the title seen
    // by the wlr protocol and by Hyprland IPC can diverge slightly in timing.
    pid_t term_pid = -1;
    pid_t class_only_pid = -1;
    for (Json::ArrayIndex i = 0; i < clients.size(); ++i) {
      const auto &client = clients[i];
      spdlog::warn("fgicon: client[{}] class='{}' title='{}'", i,
                   client["class"].asString(), client["title"].asString());
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
      spdlog::warn("fgicon: no matching client found for app_id='{}'", app_id_);
      return false;
    }
    spdlog::warn("fgicon: matched pid={}", term_pid);

    std::string fg_name = getForegroundProcessName(term_pid);
    if (fg_name.empty()) return false;

    // Apply fg-process-mapping: lets users remap wrapper scripts or processes
    // with no .desktop file to a known app-id.
    // e.g. "fg-process-mapping": { "zide": "zellij", "zellij": "zellij" }
    const auto &fg_map = tbar_->fg_process_map();
    auto fg_map_it = fg_map.find(fg_name);
    if (fg_map_it != fg_map.end()) {
      spdlog::warn("fgicon: remapping '{}' → '{}' via fg-process-mapping", fg_name, fg_map_it->second);
      fg_name = fg_map_it->second;
    }

    spdlog::warn("fgicon: terminal {} fg process is '{}'", app_id_, fg_name);

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
        if (tbar_->icon_loader().image_load_icon(icon_, app_info_, icon_size))
          icon_.show();
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
    spdlog::warn("fgicon: no desktop entry or icon found for '{}'", fg_name);
  } catch (const std::exception &e) {
    spdlog::warn("fgicon: exception: {}", e.what());
  }
  return false;
}

void Task::on_button_size_allocated(Gtk::Allocation &alloc) {
  gtk_widget_translate_coordinates(GTK_WIDGET(button.gobj()), GTK_WIDGET(bar_.window.gobj()), 0, 0,
                                   &minimize_hint.x, &minimize_hint.y);
  minimize_hint.w = button.get_width();
  minimize_hint.h = button.get_height();
}

void Task::handle_output_enter(struct wl_output *output) {
  if (ignored_) {
    spdlog::debug("{} is ignored", repr());
    return;
  }

  spdlog::debug("{} entered output {}", repr(), (void *)output);

  if (!button_visible_ && (tbar_->all_outputs() || tbar_->show_output(output))) {
    /* The task entered the output of the current bar make the button visible */
    button.signal_size_allocate().connect_notify(
        sigc::mem_fun(this, &Task::on_button_size_allocated));
    tbar_->add_button(button);
    button.show();
    button_visible_ = true;
    spdlog::debug("{} now visible on {}", repr(), bar_.output->name);
  }
}

void Task::handle_output_leave(struct wl_output *output) {
  spdlog::debug("{} left output {}", repr(), (void *)output);

  if (button_visible_ && !tbar_->all_outputs() && tbar_->show_output(output)) {
    /* The task left the output of the current bar, make the button invisible */
    tbar_->remove_button(button);
    button.hide();
    button_visible_ = false;
    spdlog::debug("{} now invisible on {}", repr(), bar_.output->name);
  }
}

void Task::handle_state(struct wl_array *state) {
  state_ = 0;
  size_t size = state->size / sizeof(uint32_t);
  for (size_t i = 0; i < size; ++i) {
    auto entry = static_cast<uint32_t *>(state->data)[i];
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
    spdlog::info("Task::handle_done: window became active - id={}, app_id={}, title={}", 
                 id_, app_id_, title_);
    tbar_->notifyActiveChanged(id_);
  } else if (!(state_ & ACTIVE)) {
    button.get_style_context()->remove_class("active");
  }

  if (state_ & FULLSCREEN) {
    button.get_style_context()->add_class("fullscreen");
  } else if (!(state_ & FULLSCREEN)) {
    button.get_style_context()->remove_class("fullscreen");
  }

  if (config_["active-first"].isBool() && config_["active-first"].asBool() && active())
    tbar_->move_button(button, 0);

  tbar_->dp.emit();
}

void Task::handle_closed() {
  spdlog::debug("{} closed", repr());
  zwlr_foreign_toplevel_handle_v1_destroy(handle_);
  handle_ = nullptr;
  if (button_visible_) {
    tbar_->remove_button(button);
    button_visible_ = false;
  }
  tbar_->remove_task(id_);
}

bool Task::handle_clicked(GdkEventButton *bt) {
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

bool Task::handle_motion_notify(GdkEventMotion *mn) {
  if (drag_start_button == -1) return false;

  if (button.drag_check_threshold(drag_start_x, drag_start_y, mn->x, mn->y)) {
    /* start drag in addition to other assigned action */
    auto target_list = Gtk::TargetList::create(target_entries);
    auto refptr = Glib::RefPtr<Gtk::TargetList>(target_list);
    auto drag_context =
        button.drag_begin(refptr, Gdk::DragAction::ACTION_MOVE, drag_start_button, (GdkEvent *)mn);
  }

  return false;
}

void Task::handle_drag_data_get(const Glib::RefPtr<Gdk::DragContext> &context,
                                Gtk::SelectionData &selection_data, guint info, guint time) {
  spdlog::debug("drag_data_get");
  void *button_addr = (void *)&this->button;

  selection_data.set("WAYBAR_TOPLEVEL", 32, (const guchar *)&button_addr, sizeof(gpointer));
}

void Task::handle_drag_data_received(const Glib::RefPtr<Gdk::DragContext> &context, int x, int y,
                                     Gtk::SelectionData selection_data, guint info, guint time) {
  spdlog::debug("drag_data_received");
  gpointer handle = *(gpointer *)selection_data.get_data();
  auto dragged_button = (Gtk::Button *)handle;

  if (dragged_button == &this->button) return;

  auto parent_of_dragged = dragged_button->get_parent();
  auto parent_of_dest = this->button.get_parent();

  if (parent_of_dragged != parent_of_dest) return;

  auto box = (Gtk::Box *)parent_of_dragged;

  auto position_prop = box->child_property_position(this->button);
  auto position = position_prop.get_value();

  box->reorder_child(*dragged_button, position);
}

bool Task::operator==(const Task &o) const { return o.id_ == id_; }

bool Task::operator!=(const Task &o) const { return o.id_ != id_; }

void Task::update() {
  bool markup = config_["markup"].isBool() ? config_["markup"].asBool() : false;
  std::string title = title_;
  std::string name = name_;
  std::string app_id = app_id_;
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

    if (markup)
      button.set_tooltip_markup(txt);
    else
      button.set_tooltip_text(txt);
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

void Task::activate() {
  // When running under Hyprland, use its own focuswindow dispatch rather than
  // the wlr foreign-toplevel activate call. The wlr path causes Hyprland to
  // warp the cursor to the centre of the screen on workspace switches; the
  // dispatch path uses the same code as keyboard shortcuts and leaves the
  // cursor where it is.
  if (hyprland::gIPC && std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
    try {
      Json::Value clients = hyprland::gIPC->getSocket1JsonReply("clients");
      if (clients.isArray()) {
        // Try exact class+title match first, then fall back to class only.
        std::string addr;
        std::string class_only_addr;
        for (Json::ArrayIndex i = 0; i < clients.size(); ++i) {
          const auto &c = clients[i];
          spdlog::warn("activate: checking client class='{}' title='{}' addr='{}'",
                       c["class"].asString(), c["title"].asString(), c["address"].asString());
          if (c["class"].asString() == app_id_) {
            if (c["title"].asString() == title_) {
              addr = c["address"].asString();
              break;
            } else if (class_only_addr.empty()) {
              class_only_addr = c["address"].asString();
            }
          }
        }
        if (addr.empty()) addr = class_only_addr;
        if (!addr.empty()) {
          std::string cmd = "dispatch focuswindow address:" + addr;
          spdlog::warn("activate: dispatching '{}' for app_id='{}' title='{}'",
                       cmd, app_id_, title_);
          std::string reply = hyprland::IPC::getSocket1Reply(cmd);
          spdlog::warn("activate: reply='{}'", reply);
          return;
        }
        spdlog::warn("activate: no address found for app_id='{}' title='{}', falling back to wlr",
                     app_id_, title_);
      }
    } catch (const std::exception &e) {
      spdlog::warn("Task::activate: Hyprland dispatch failed: {}", e.what());
    }
  }
  // Fallback for non-Hyprland compositors.
  spdlog::warn("activate: using wlr protocol for app_id='{}' title='{}'", app_id_, title_);
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
static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
                          const char *interface, uint32_t version) {
  if (std::strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
    static_cast<Taskbar *>(data)->register_manager(registry, name, version);
  } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
    static_cast<Taskbar *>(data)->register_seat(registry, name, version);
  }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
  /* Nothing to do here */
}

static const wl_registry_listener registry_listener_impl = {.global = handle_global,
                                                            .global_remove = handle_global_remove};

Taskbar::Taskbar(const std::string &id, const waybar::Bar &bar, const Json::Value &config)
    : waybar::AModule(config, "taskbar", id, false, false),
      bar_(bar),
      box_{bar.orientation, 0},
      manager_{nullptr},
      seat_{nullptr} {
  box_.set_name("taskbar");
  if (!id.empty()) {
    box_.get_style_context()->add_class(id);
  }
  box_.get_style_context()->add_class(MODULE_CLASS);
  box_.get_style_context()->add_class("empty");
  event_box_.add(box_);

  struct wl_display *display = Client::inst()->wl_display;
  struct wl_registry *registry = wl_display_get_registry(display);

  wl_registry_add_listener(registry, &registry_listener_impl, this);
  wl_display_roundtrip(display);

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
    for (auto &c : config_["icon-theme"]) {
      icon_loader_.add_custom_icon_theme(c.asString());
    }
  } else if (config_["icon-theme"].isString()) {
    icon_loader_.add_custom_icon_theme(config_["icon-theme"].asString());
  }

  // Load ignore-list
  if (config_["ignore-list"].isArray()) {
    for (auto &app_name : config_["ignore-list"]) {
      ignore_list_.emplace(app_name.asString());
    }
  }

  // Load app_id remappings
  if (config_["app_ids-mapping"].isObject()) {
    const Json::Value &mapping = config_["app_ids-mapping"];
    const std::vector<std::string> app_ids = config_["app_ids-mapping"].getMemberNames();
    for (auto &app_id : app_ids) {
      app_ids_replace_map_.emplace(app_id, mapping[app_id].asString());
    }
  }

  // Load fg-process-mapping: maps a detected foreground process name to an
  // app-id used for icon/name lookup. Useful for wrapper scripts (zide → zellij)
  // or apps with no .desktop file (zellij → zellij icon name).
  // Example config: "fg-process-mapping": { "zide": "zellij", "zellij": "zellij" }
  if (config_["fg-process-mapping"].isObject()) {
    const Json::Value &mapping = config_["fg-process-mapping"];
    for (auto &key : config_["fg-process-mapping"].getMemberNames()) {
      fg_process_map_.emplace(key, mapping[key].asString());
    }
  }

  for (auto &t : tasks_) {
    t->handle_app_id(t->app_id().c_str());
  }

  // Initialize Hyprland IPC unconditionally when running under Hyprland so
  // that terminal foreground icon detection works regardless of other settings.
  {
    const char* his = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (his != nullptr) {
      hyprland::modulesReady = true;
      if (!hyprland::gIPC) {
        hyprland::gIPC = std::make_unique<hyprland::IPC>();
      }
    }
  }

  // Register for Hyprland events if sorting by Hyprland workspaces
  if (config_["sort-by-hyprland-workspaces"].asBool()) {
    const char* his = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (his != nullptr) {
      hyprland::gIPC->registerForIPC("movewindow", this);
      hyprland::gIPC->registerForIPC("openwindow", this);
      hyprland::gIPC->registerForIPC("closewindow", this);
      hyprland::gIPC->registerForIPC("configreloaded", this);
      registered_for_hyprland_events_ = true;
      spdlog::debug("Taskbar registered for Hyprland IPC events");
    }
  }

  // Setup control socket for keyboard navigation
  setupControlSocket();
}

Taskbar::~Taskbar() {
  cleanupControlSocket();
  if (registered_for_hyprland_events_ && hyprland::gIPC) {
    hyprland::gIPC->unregisterForIPC(this);
  }
  if (manager_) {
    struct wl_display *display = Client::inst()->wl_display;
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
}

void Taskbar::update() {
  for (auto &t : tasks_) {
    t->update();
  }

  if (config_["sort-by-app-id"].asBool()) {
    std::stable_sort(tasks_.begin(), tasks_.end(),
                     [](const std::unique_ptr<Task> &a, const std::unique_ptr<Task> &b) {
                       return a->app_id() < b->app_id();
                     });

    for (unsigned long i = 0; i < tasks_.size(); i++) {
      move_button(tasks_[i]->button, i);
    }
  }

  if (config_["sort-by-hyprland-workspaces"].asBool() && hyprland::gIPC) {
    // Check if Hyprland is running
    const char* his = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (his != nullptr) {
      try {
        Json::Value clients = hyprland::gIPC->getSocket1JsonReply("clients");
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
                    [](const ClientInfo &a, const ClientInfo &b) {
                      if (a.workspace_id != b.workspace_id) return a.workspace_id < b.workspace_id;
                      if (a.x != b.x) return a.x < b.x;
                      return a.y < b.y;
                    });

          // Build a map from (class, title) to list of sort indices (for windows with same class/title)
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
              size_t &used = usedCount[key];
              if (used < it->second.size()) {
                taskSortIndex[t] = it->second[used];
                ++used;
              }
            }
          }

          // Sort tasks by their assigned sort index
          std::vector<size_t> taskOrder(tasks_.size());
          std::iota(taskOrder.begin(), taskOrder.end(), 0);
          std::stable_sort(taskOrder.begin(), taskOrder.end(),
                           [&taskSortIndex](size_t a, size_t b) {
                             return taskSortIndex[a] < taskSortIndex[b];
                           });

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

  AModule::update();
}

void Taskbar::onEvent(const std::string &ev) {
  // Hyprland window event received - trigger update to re-sort
  spdlog::debug("Taskbar received Hyprland event: {}", ev);
  dp.emit();
}

void Taskbar::setupControlSocket() {
  const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
  std::string display_name = wayland_display ? wayland_display : "wayland-0";
  socket_path_ = fmt::format("/tmp/waybar-taskbar-{}.sock", display_name);

  // Remove existing socket file if it exists
  unlink(socket_path_.c_str());

  socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    spdlog::error("Failed to create taskbar control socket: {}", strerror(errno));
    return;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    spdlog::error("Failed to bind taskbar control socket: {}", strerror(errno));
    close(socket_fd_);
    socket_fd_ = -1;
    return;
  }

  if (listen(socket_fd_, 5) < 0) {
    spdlog::error("Failed to listen on taskbar control socket: {}", strerror(errno));
    close(socket_fd_);
    socket_fd_ = -1;
    return;
  }

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
        Glib::signal_idle().connect_once([this, line]() {
          handleCommand(line);
        });
      }
    }
  }
}

void Taskbar::handleCommand(const std::string &cmd) {
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
      spdlog::info("  Task[{}]: id={}, title={}, active={}", 
                   i, tasks_[i]->id(), tasks_[i]->title(), tasks_[i]->active());
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
    selection_index_ = (selection_index_ - 1 + static_cast<int>(tasks_.size())) % static_cast<int>(tasks_.size());
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

static void tm_handle_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
                               struct zwlr_foreign_toplevel_handle_v1 *tl_handle) {
  return static_cast<Taskbar *>(data)->handle_toplevel_create(tl_handle);
}

static void tm_handle_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager) {
  return static_cast<Taskbar *>(data)->handle_finished();
}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_impl = {
    .toplevel = tm_handle_toplevel,
    .finished = tm_handle_finished,
};

void Taskbar::register_manager(struct wl_registry *registry, uint32_t name, uint32_t version) {
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

  manager_ = static_cast<struct zwlr_foreign_toplevel_manager_v1 *>(
      wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, version));

  if (manager_)
    zwlr_foreign_toplevel_manager_v1_add_listener(manager_, &toplevel_manager_impl, this);
  else
    spdlog::debug("Failed to register manager");
}

void Taskbar::register_seat(struct wl_registry *registry, uint32_t name, uint32_t version) {
  if (seat_) {
    spdlog::warn("Register seat again although already existing!");
    return;
  }
  version = std::min<uint32_t>(version, wl_seat_interface.version);

  seat_ = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, version));
}

void Taskbar::handle_toplevel_create(struct zwlr_foreign_toplevel_handle_v1 *tl_handle) {
  tasks_.push_back(std::make_unique<Task>(bar_, config_, this, tl_handle, seat_));
}

void Taskbar::handle_finished() {
  zwlr_foreign_toplevel_manager_v1_destroy(manager_);
  manager_ = nullptr;
}

void Taskbar::add_button(Gtk::Button &bt) {
  box_.pack_start(bt, false, false);
  box_.get_style_context()->remove_class("empty");
}

void Taskbar::move_button(Gtk::Button &bt, int pos) { box_.reorder_child(bt, pos); }

void Taskbar::remove_button(Gtk::Button &bt) {
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
                         [id](const TaskPtr &p) { return p->id() == id; });

  if (it == std::end(tasks_)) {
    spdlog::warn("Can't find task with id {}", id);
    return;
  }

  tasks_.erase(it);
}

bool Taskbar::show_output(struct wl_output *output) const {
  return output == gdk_wayland_monitor_get_wl_output(bar_.output->monitor->gobj());
}

bool Taskbar::all_outputs() const {
  return config_["all-outputs"].isBool() && config_["all-outputs"].asBool();
}

const IconLoader &Taskbar::icon_loader() const { return icon_loader_; }

const std::unordered_set<std::string> &Taskbar::ignore_list() const { return ignore_list_; }

const std::map<std::string, std::string> &Taskbar::app_ids_replace_map() const {
  return app_ids_replace_map_;
}

const std::map<std::string, std::string> &Taskbar::fg_process_map() const {
  return fg_process_map_;
}

} /* namespace waybar::modules::wlr */
