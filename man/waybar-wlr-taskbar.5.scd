waybar-wlr-taskbar(5)

# NAME

waybar - wlr taskbar module

# DESCRIPTION

The *taskbar* module displays the currently open applications. This module requires
a compositor that implements the foreign-toplevel-manager interface.

# CONFIGURATION

Addressed by *wlr/taskbar*

*all-outputs*: ++
	typeof: bool ++
	default: false ++
	If set to false applications on the waybar's current output will be shown. Otherwise, all applications are shown.

*format*: ++
	typeof: string ++
	default: {icon} ++
	The format, how information should be displayed.

*icon-theme*: ++
	typeof: array|string ++
	The names of the icon-themes that should be used to find an icon. The list will be traversed from left to right. If omitted, the system default will be used.

*icon-size*: ++
	typeof: integer ++
	default: 16 ++
	The size of the icon.

*markup*: ++
	typeof: bool ++
	default: false ++
	If set to true, pango markup will be accepted in format and tooltip-format.

*tooltip*: ++
	typeof: bool ++
	default: true ++
	If set to false no tooltip will be shown.

*tooltip-format*: ++
	typeof: string ++
	default: {title} ++
	The format, how information in the tooltip should be displayed.

*active-first*: ++
	typeof: bool ++
	default: false ++
	If set to true, always reorder the tasks in the taskbar so that the currently active one is first. Otherwise don't reorder.

*sort-by-app-id*: ++
	typeof: bool ++
	default: false ++
	If set to true, group tasks by their app_id. Cannot be used with 'active-first'.

*sort-by-hyprland-workspaces*: ++
	typeof: bool ++
	default: false ++
	If set to true, sort tasks by workspace ID first, then by position within each workspace (top-to-bottom, then left-to-right). This option only works when running under Hyprland. Cannot be used with 'active-first' or 'sort-by-app-id'.

*on-click*: ++
	typeof: string ++
	The action which should be triggered when clicking on the application button with the left mouse button.

*on-click-middle*: ++
	typeof: string ++
	The action which should be triggered when clicking on the application button with the middle mouse button.

*on-click-right*: ++
	typeof: string ++
	The action which should be triggered when clicking on the application button with the right mouse button.

*on-update*: ++
	typeof: string ++
	Command to execute when the module is updated.

*ignore-list*: ++
	typeof: array ++
	List of app_id/titles to be invisible.

*app_ids-mapping*: ++
	typeof: object ++
	Dictionary of app_id to be replaced with

*rewrite*: ++
	typeof: object ++
	Rules to rewrite the module format output. See *rewrite rules*.

# FORMAT REPLACEMENTS

*{icon}*: The icon of the application.

*{name}*: The application name as in desktop file if appropriate desktop files are found, otherwise same as {app_id}

*{title}*: The title of the application.

*{app_id}*: The app_id (== application name) of the application.

*{state}*: The state (minimized, maximized, active, fullscreen) of the application.

*{short_state}*: The state (minimize == m, maximized == M, active == A, fullscreen == F) represented as one character of the application.

# CLICK ACTIONS

*activate*: Bring the application into foreground.

*minimize*: Toggle application's minimized state.

*minimize-raise*: Bring the application into foreground or toggle its minimized state.

*maximize*: Toggle application's maximized state.

*fullscreen*: Toggle application's fullscreen state.

*close*: Close the application.

# REWRITE RULES

*rewrite* is an object where keys are regular expressions and values are
rewrite rules if the expression matches. Rules may contain references to
captures of the expression.

Regular expression and replacement follow ECMA-script rules.

If no expression matches, the format output is left unchanged.

Invalid expressions (e.g., mismatched parentheses) are skipped.

# EXAMPLES

```
"wlr/taskbar": {
	"format": "{icon}",
	"icon-size": 14,
	"icon-theme": "Numix-Circle",
	"tooltip-format": "{title}",
	"on-click": "activate",
	"on-click-middle": "close",
	"ignore-list": [
	    "Alacritty"
	],
	"app_ids-mapping": {
		"firefoxdeveloperedition": "firefox-developer-edition"
	},
	"rewrite": {
		"Firefox Web Browser": "Firefox",
		"Foot Server": "Terminal"
	}
}
```

# KEYBOARD NAVIGATION

The taskbar module creates a Unix socket at */tmp/waybar-taskbar-$WAYLAND_DISPLAY.sock*
that accepts commands for keyboard-based window switching. This allows implementing
alt-tab functionality using compositor keybindings.

*Commands:*

*next*: Select the next task (starts from active window on first press)

*prev*: Select the previous task (starts from active window on first press)

*activate*: Activate the selected window and clear selection

*clear*: Clear the selection without activating

*refresh*: Force a refresh of the taskbar (useful after layout changes)

*Hyprland Configuration Example:*

To implement alt-tab window switching with proper Alt key release detection, you need
the *taskbar-switcher* Hyprland plugin (included in the *hyprland-plugin/* directory).
The plugin detects Alt release events which cannot be reliably handled with native
Hyprland keybindings.

First, load the plugin in your Hyprland config:

```
plugin = /path/to/taskbar-switcher.so
```

Then add keybindings for Alt+Tab navigation:

```
# Alt+Tab to cycle forward
bind = ALT, Tab, exec, echo "next" | socat - UNIX-CONNECT:/tmp/waybar-taskbar-$WAYLAND_DISPLAY.sock

# Alt+Shift+Tab to cycle backward
bind = ALT SHIFT, Tab, exec, echo "prev" | socat - UNIX-CONNECT:/tmp/waybar-taskbar-$WAYLAND_DISPLAY.sock
```

The plugin automatically handles:
- Detecting Alt+Tab press to enter switcher mode
- Sending "activate" when Alt is released
- Sending "clear" when Escape is pressed
- Sending "activate" when Enter is pressed

See the *hyprland-plugin/README.md* for build instructions and more details.

*CSS Styling Example:*

Style the keyboard selection indicator in your Waybar CSS:

```
#taskbar button.keyboard-selected {
    background: rgba(255, 255, 255, 0.3);
    border: 2px solid #88c0d0;
    border-radius: 4px;
}
```

# Style

- *#taskbar*
- *#taskbar button*
- *#taskbar button.maximized*
- *#taskbar button.minimized*
- *#taskbar button.active*
- *#taskbar button.fullscreen*
- *#taskbar button.keyboard-selected*
