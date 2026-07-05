#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/event/EventBus.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <string>

inline HANDLE PHANDLE = nullptr;

static bool                g_switcherActive = false;
static CHyprSignalListener g_keyListener;

static void sendToTaskbarSocket(const char* command) {
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
    if (!waylandDisplay)
        waylandDisplay = "wayland-1";

    std::string socketPath = "/tmp/waybar-taskbar-" + std::string(waylandDisplay) + ".sock";

    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0)
        return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        ssize_t result = write(sockfd, command, strlen(command));
        (void)result;
    }

    close(sockfd);
}

// The 0.55 keyboard.key event only carries the SKeyEvent, not the keyboard
// that emitted it. Pick the active keyboard out of InputManager for xkb
// lookups; modifier state is shared across keyboards by default, so any
// keyboard with a live xkb_state will report the same effective mods.
static SP<IKeyboard> getActiveKeyboard() {
    if (!g_pInputManager)
        return nullptr;

    SP<IKeyboard> fallback;
    for (const auto& kb : g_pInputManager->m_keyboards) {
        if (!kb || !kb->m_xkbState)
            continue;
        if (kb->m_active)
            return kb;
        if (!fallback)
            fallback = kb;
    }
    return fallback;
}

static void onKeyPress(IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
    const auto keyboard = getActiveKeyboard();
    if (!keyboard)
        return;

    const auto state = keyboard->m_xkbState;
    if (!state)
        return;

    const uint32_t     keycode   = event.keycode + 8; // xkbcommon expects +8 from libinput
    const bool         released  = event.state == WL_KEYBOARD_KEY_STATE_RELEASED;
    const xkb_keysym_t keysym    = xkb_state_key_get_one_sym(state, keycode);
    const bool         altActive = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) == 1;

    // Alt+Tab press → enter switcher mode
    if (!released && (keysym == XKB_KEY_Tab || keysym == XKB_KEY_ISO_Left_Tab) && altActive) {
        g_switcherActive = true;
    }

    // Alt release while switching → commit
    if (released && g_switcherActive && (keysym == XKB_KEY_Alt_L || keysym == XKB_KEY_Alt_R)) {
        g_switcherActive = false;
        sendToTaskbarSocket("activate\n");
    }

    // Escape while switching → cancel
    if (!released && g_switcherActive && keysym == XKB_KEY_Escape) {
        g_switcherActive = false;
        sendToTaskbarSocket("clear\n");
        info.cancelled = true;
    }

    // Enter while switching → commit
    if (!released && g_switcherActive && (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter)) {
        g_switcherActive = false;
        sendToTaskbarSocket("activate\n");
        info.cancelled = true;
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    g_keyListener = Event::bus()->m_events.input.keyboard.key.listen(
        [](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
            onKeyPress(event, info);
        }
    );

    return {"taskbar-switcher", "Detects Alt key release for taskbar window switcher", "Waybar", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_keyListener.reset();
}
