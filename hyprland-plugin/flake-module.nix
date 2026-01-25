# Flake module for the taskbar-switcher Hyprland plugin
# 
# Usage in your flake.nix:
#
# {
#   inputs = {
#     nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
#     home-manager.url = "github:nix-community/home-manager";
#     waybar-taskbar-switcher.url = "path:/path/to/Waybar/hyprland-plugin";
#   };
#
#   outputs = { self, nixpkgs, home-manager, waybar-taskbar-switcher, ... }: {
#     # Import the module in your home-manager config
#     homeConfigurations."user" = home-manager.lib.homeManagerConfiguration {
#       modules = [
#         waybar-taskbar-switcher.homeManagerModules.default
#         {
#           programs.taskbar-switcher.enable = true;
#         }
#       ];
#     };
#   };
# }

{ config, lib, pkgs, ... }:

let
  cfg = config.programs.taskbar-switcher;
  
  taskbar-switcher-plugin = pkgs.callPackage ./default.nix {
    hyprland = config.wayland.windowManager.hyprland.package;
  };
in
{
  options.programs.taskbar-switcher = {
    enable = lib.mkEnableOption "taskbar-switcher Hyprland plugin for Alt-Tab window switching";
    
    package = lib.mkOption {
      type = lib.types.package;
      default = taskbar-switcher-plugin;
      description = "The taskbar-switcher plugin package to use";
    };
  };

  config = lib.mkIf cfg.enable {
    # Add the plugin to Hyprland
    wayland.windowManager.hyprland = {
      plugins = [ cfg.package ];
      
      # Add the keybindings for Alt+Tab
      extraConfig = ''
        # Taskbar switcher keybindings (Alt release handled by plugin)
        bind = ALT, Tab, exec, echo "next" | socat - UNIX-CONNECT:/tmp/waybar-taskbar-$WAYLAND_DISPLAY.sock
        bind = ALT SHIFT, Tab, exec, echo "prev" | socat - UNIX-CONNECT:/tmp/waybar-taskbar-$WAYLAND_DISPLAY.sock
      '';
    };
    
    # Ensure socat is available
    home.packages = [ pkgs.socat ];
  };
}