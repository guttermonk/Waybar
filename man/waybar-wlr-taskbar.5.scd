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

*bar-css-states*: ++
	typeof: bool ++
	default: false ++
	If set to true, application state is exposed as CSS classes on the Waybar
	window. Maximized and fullscreen state is aggregated across applications
	known to belong to the active workspace. See *Bar state style* below.

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

*active-only*: ++
	typeof: bool ++
	default: false ++
	If set to true, only the currently active application button is shown.
	Other applications remain tracked and reappear when activated.

*sort-by-app-id*: ++
	typeof: bool ++
	default: false ++
	If set to true, group tasks by their app_id. Cannot be used with 'active-first'.

*homogeneous*: ++
	typeof: bool ++
	default: false ++
	If set to true, distribute every task button evenly across the taskbar's allocated width. Buttons will automatically resize so that 'N' visible tasks each take '1/N' of the width.

*justify*: ++
	typeof: string ++
	The alignment of the text within the module's box, allowing options 'left', 'right', or 'center' to define the positioning.

*expand*: ++
	typeof: bool ++
	default: false ++
	If set to true, task buttons stretch to fill the available space in the taskbar and long titles are ellipsized to fit. Only takes effect on a horizontal bar; on a vertical bar the buttons keep their content-based size. If set to false, buttons are sized to their content.

*truncate*: ++
	typeof: bool ++
	default: false ++
	If set to true, the task button text will be ellipsized (truncated with …) when the available button width is smaller than the label text.
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

*squash-list*: ++
	typeof: array ++
	List of app_id/titles whose multiple instances are collapsed into a single button. When more than one instance of a listed app is open, only one button is shown; when one instance closes, the next hidden instance reappears. The special value '\*' matches all applications.

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

As with all format replacements in Waybar, a length limit such as *{title:.15}* is
measured in bytes, not characters. This may produce invalid text if a multi-byte
character is split, or (when *markup* is true) if the truncation cuts through a
value that must be escaped in XML.

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

An expression must match the format output *fully* to trigger its replacement.

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
- *#taskbar.empty*
- *#taskbar button*
- *#taskbar button.maximized*
- *#taskbar button.minimized*
- *#taskbar button.active*
- *#taskbar button.fullscreen*

# Bar state style

When *bar-css-states* is enabled, the following classes are added to
*window#waybar*:

- *window#waybar.toplevel-active*
- *window#waybar.toplevel-maximized*
- *window#waybar.toplevel-minimized*
- *window#waybar.toplevel-fullscreen*

The active, minimized classes describe the active application. The maximized
and fullscreen classes are set if any non-minimized application known to belong
to the active workspace has that state.

Workspace membership is learned when an application is activated and requires
the compositor to support *ext-workspace-v1*. Before an application has been
activated during the current Waybar session, its workspace may be unknown. On
compositors without *ext-workspace-v1*, these classes fall back to the active
application's state.

For example:

```
window#waybar {
	background-color: rgba(0, 0, 0, 0.5);
}

window#waybar.toplevel-maximized {
	background-color: rgba(0, 0, 0, 1);
}
```
- *#taskbar button.keyboard-selected*
