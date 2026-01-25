{ lib
, stdenv
, hyprland
, pkg-config
, pixman
, libdrm
}:

stdenv.mkDerivation {
  pname = "taskbar-switcher-hyprland-plugin";
  version = "1.0.0";

  src = ./.;

  nativeBuildInputs = [
    pkg-config
  ];

  buildInputs = [
    hyprland
    pixman
    libdrm
  ];

  # Disable cmake - we use a custom g++ build
  dontUseCmakeConfigure = true;
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    
    g++ -shared -fPIC -std=c++23 -O2 \
      $(pkg-config --cflags hyprland pixman-1 libdrm) \
      -o taskbar-switcher.so \
      taskbar-switcher.cpp
    
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    
    mkdir -p $out/lib/hyprland
    cp taskbar-switcher.so $out/lib/hyprland/
    
    runHook postInstall
  '';

  meta = with lib; {
    description = "Hyprland plugin that detects Alt key release for Waybar taskbar window switcher";
    homepage = "https://github.com/Alexays/Waybar";
    license = licenses.mit;
    platforms = platforms.linux;
  };
}