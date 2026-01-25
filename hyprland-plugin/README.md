# Taskbar Switcher Hyprland Plugin

A minimal Hyprland plugin that detects Alt key release for the Waybar taskbar window switcher.

## Why This Plugin?

Hyprland's built-in `bindr` (bind on release) doesn't work reliably for detecting modifier key releases, especially in submaps. This plugin hooks directly into Hyprland's keyboard event system to properly detect when the Alt key is released.

## Features

- Detects Alt+Tab to enter switcher mode
- Detects Alt key release to activate the selected window
- Detects Escape to cancel the switcher
- Detects Enter to confirm selection
- Sends commands to Waybar's taskbar socket at `/tmp/waybar-taskbar-$WAYLAND_DISPLAY.sock`

## Building

### Prerequisites

- Hyprland development headers (`hyprland-devel` or `hyprland-dev`)
- pkg-config
- g++ with C++23 support
- pixman and libdrm development headers

#### NixOS

```bash
nix-shell -p hyprland.dev pkg-config gcc pixman libdrm
```

#### Arch Linux

```bash
sudo pacman -S hyprland-headers base-devel
```

#### Fedora

```bash
sudo dnf install hyprland-devel gcc-c++ pkgconfig pixman-devel libdrm-devel
```

### Compile

```bash
cd hyprland-plugin
make
```

This will produce `taskbar-switcher.so`.

## Installation

### Manual

```bash
mkdir -p ~/.local/lib/hyprland
cp taskbar-switcher.so ~/.local/lib/hyprland/
```

### System-wide

```bash
sudo make install
```

## Usage

Add to your Hyprland config (`~/.config/hypr/hyprland.conf`):

```ini
plugin = ~/.local/lib/hyprland/taskbar-switcher.so
```

Or if installed system-wide:

```ini
plugin = /usr/lib/hyprland/taskbar-switcher.so
```

### Hyprland Keybindings

You still need keybindings for Alt+Tab to send commands to the taskbar. Add these to your Hyprland config:

```ini
# Alt+Tab to cycle forward
bind = ALT, Tab, exec, echo "next" | socat - UNIX-CONNECT:/tmp/waybar-taskbar-$WAYLAND_DISPLAY.sock

# Alt+Shift+Tab to cycle backward
bind = ALT SHIFT, Tab, exec, echo "prev" | socat - UNIX-CONNECT:/tmp/waybar-taskbar-$WAYLAND_DISPLAY.sock
```

The plugin will automatically:
- Detect when you press Alt+Tab and enter "switcher mode"
- Send "activate" when you release Alt
- Send "clear" if you press Escape
- Send "activate" if you press Enter

### Waybar CSS

Style the selected taskbar item:

```css
#taskbar button.keyboard-selected {
    background: rgba(255, 255, 255, 0.3);
    border: 2px solid #88c0d0;
    border-radius: 4px;
}
```

## Troubleshooting

### Plugin not loading

Check Hyprland logs:
```bash
hyprctl plugin list
```

### Socket not found

Make sure Waybar is running with the taskbar module. The socket is created at `/tmp/waybar-taskbar-$WAYLAND_DISPLAY.sock`.

### Commands not being received

Test the socket manually:
```bash
echo "next" | socat - UNIX-CONNECT:/tmp/waybar-taskbar-$WAYLAND_DISPLAY.sock
```

## License

MIT License - Same as Waybar