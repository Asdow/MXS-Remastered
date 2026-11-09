// dab.cpp - see dab.h.
//
//   The game writes a key or button binding as "name=(k)<what>" and an axis as
//   "name=(<from>,<to>)<what>", where <what> is:
//     XInputGamepad<n> button <b>    an Xbox-style pad's button: <b> is its bit in XInput's wButtons
//                                    (LB 8, RB 9, A 12, B 13, X 14, Y 15...)
//     XInputGamepad<n> axis <a>      0 / 1 the left stick's X / Y, 2 / 3 the right's, 4 / 5 the
//                                    left / right trigger; <from> is at rest, <to> all the way
//     <device> button <b>, axis <a>  another controller (DirectInput): read through the joystick
//                                    API, the same buttons in the same order
//     Mouse Left / Middle / Right    the mouse's buttons
//     a key's name                   "t", ",", "Space", "Left Shift", "Keypad 5", "F1"...
#include "dab.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "gamepad.h"
#include "log.h"
#include "paths.h"

namespace mxcam {
namespace dab {
namespace {

constexpr double kStopped   = 0.9;      // m/s (2 mph): slower than this you're stopped (Dab When Stopped)

struct Binding {
    int    vk = 0;                          // a key or a mouse button
    gamepad::Control control = gamepad::kNone;  // a controller's button, trigger or stick direction
};

struct GameSettings {
    Binding key;
    char    text[160] = "";   // the binding as the game wrote it
    bool    autoDab = false, sticky = false;
    double  maxSpeed = 40.0;  // mph
};
GameSettings g_game;
FILETIME     g_written{};
double       g_checked = -1;
bool         g_sitting = false, g_down = false, g_wasDown = false, g_latched = false;

// Named keys as the game writes them.
struct NamedKey { const char* name; int vk; };
const NamedKey kKeys[] = {
    { "Esc", VK_ESCAPE }, { "Backspace", VK_BACK }, { "Tab", VK_TAB }, { "Enter", VK_RETURN },
    { "Left Ctrl", VK_LCONTROL }, { "Right Ctrl", VK_RCONTROL }, { "Left Shift", VK_LSHIFT },
    { "Right Shift", VK_RSHIFT }, { "Left Alt", VK_LMENU }, { "Right Alt", VK_RMENU }, { "Space", VK_SPACE },
    { "Caps Lock", VK_CAPITAL }, { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "Home", VK_HOME },
    { "End", VK_END }, { "Page Up", VK_PRIOR }, { "Page Down", VK_NEXT }, { "Up", VK_UP }, { "Down", VK_DOWN },
    { "Left", VK_LEFT }, { "Right", VK_RIGHT }, { "Pause", VK_PAUSE }, { "Num Lock", VK_NUMLOCK },
    { "Keypad /", VK_DIVIDE }, { "Keypad *", VK_MULTIPLY }, { "Keypad -", VK_SUBTRACT }, { "Keypad +", VK_ADD },
    { "Keypad Enter", VK_RETURN }, { "Keypad .", VK_DECIMAL },
    { "Mouse Left", VK_LBUTTON }, { "Mouse Middle", VK_MBUTTON }, { "Mouse Right", VK_RBUTTON },
};

// The gamepad module's controls (gamepad.cpp: (kind << 8) | index).
constexpr gamepad::Control XButton(int bit) { return (1 << 8) | bit; }
constexpr gamepad::Control XTrigger(int t) { return (2 << 8) | t; }
constexpr gamepad::Control XAxis(int axis, bool negative) { return (3 << 8) | (axis * 2 + (negative ? 1 : 0)); }
constexpr gamepad::Control JoyButton(int b) { return (5 << 8) | b; }
constexpr gamepad::Control JoyAxis(int axis, bool negative) { return (6 << 8) | (axis * 2 + (negative ? 1 : 0)); }

// "<what> button 3" / "<what> axis 1": the number, and the device part before it.
bool Ends(const char* what, const char* word, int& number, size_t& deviceLength)
{
    const char* at = strstr(what, word);
    const char* last = nullptr;
    while (at) {
        last = at;
        at = strstr(at + 1, word);
    }
    if (!last || !isdigit(static_cast<unsigned char>(last[strlen(word)])))
        return false;
    number = atoi(last + strlen(word));
    deviceLength = static_cast<size_t>(last - what);
    return true;
}

Binding Parse(const char* value)
{
    Binding b;
    double from = 0, to = 1;
    const char* what = nullptr;
    if (strncmp(value, "(k)", 3) == 0) {
        what = value + 3;
    } else if (value[0] == '(' && sscanf_s(value, "(%lf,%lf)", &from, &to) == 2) {
        what = strchr(value, ')');
        what = what ? what + 1 : nullptr;
    }
    if (!what || !*what)
        return b;
    const bool axisBinding = value[1] != 'k';
    int n = 0;
    size_t device = 0;
    if (Ends(what, " button ", n, device)) {
        b.control = strncmp(what, "XInputGamepad", 13) == 0 ? XButton(n) : JoyButton(n);
        return b;
    }
    if (Ends(what, " axis ", n, device)) {
        const bool negative = to < from;
        if (strncmp(what, "XInputGamepad", 13) == 0)
            b.control = n >= 4 ? XTrigger(n - 4) : XAxis(n, negative);
        else
            b.control = JoyAxis(n, negative);
        return b;
    }
    if (axisBinding)
        return b;
    for (const NamedKey& k : kKeys)
        if (_stricmp(what, k.name) == 0) {
            b.vk = k.vk;
            return b;
        }
    if (strncmp(what, "Keypad ", 7) == 0 && isdigit(static_cast<unsigned char>(what[7])) && !what[8]) {
        b.vk = VK_NUMPAD0 + (what[7] - '0');
        return b;
    }
    if (what[0] == 'F' && isdigit(static_cast<unsigned char>(what[1]))) {
        const int f = atoi(what + 1);
        if (f >= 1 && f <= 24) {
            b.vk = VK_F1 + f - 1;
            return b;
        }
    }
    if (what[0] && !what[1]) {
        const unsigned char c = static_cast<unsigned char>(what[0]);
        if (isalpha(c))
            b.vk = toupper(c);
        else if (isdigit(c))
            b.vk = c;
        else {
            const SHORT scan = VkKeyScanA(static_cast<CHAR>(c));
            if (scan != -1)
                b.vk = scan & 0xFF;
        }
    }
    return b;
}

// The game's config, read again when it changes (it's written when the game's settings are saved).
void ReadGameSettings(double now)
{
    if (g_checked >= 0 && now - g_checked < 1.0)
        return;
    g_checked = now;
    if (!paths::PersonalFolder()[0])
        return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sconfig", paths::PersonalFolder());
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info) || CompareFileTime(&info.ftLastWriteTime, &g_written) == 0)
        return;
    g_written = info.ftLastWriteTime;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f)
        return;
    GameSettings s;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "dab_key=", 8) == 0) {
            strncpy_s(s.text, line + 8, _TRUNCATE);
            s.key = Parse(line + 8);
        } else if (strncmp(line, "auto_dab=", 9) == 0) {
            s.autoDab = _stricmp(line + 9, "yes") == 0;
        } else if (strncmp(line, "sticky_dab=", 11) == 0) {
            s.sticky = _stricmp(line + 11, "yes") == 0;
        } else if (strncmp(line, "max_dab_speed=", 14) == 0) {
            s.maxSpeed = atof(line + 14);
        }
    }
    fclose(f);
    g_game = s;
    Log("dab: the game's dab key is %s (%s), Dab When Stopped %s, Sticky Sit/Dab %s, Max Dab Speed %.0f mph",
        s.text[0] ? s.text : "not set", s.key.vk ? "a key" : s.key.control ? "a controller" : "NOT READ",
        s.autoDab ? "on" : "off", s.sticky ? "on" : "off", s.maxSpeed);
}

bool GameHasFocus()
{
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

}  // namespace

void Update(bool haveSpeed, double speed)
{
    ReadGameSettings(GetTickCount64() / 1000.0);
    const Binding& b = g_game.key;
    bool down = false;
    if (b.vk)
        down = GameHasFocus() && (GetAsyncKeyState(b.vk) & 0x8000) != 0;
    else if (b.control)
        down = gamepad::IsDown(b.control);
    g_down = down;
    // Sticky Sit/Dab: a press sits, the next stands. As the game's file says, unless it's set here
    // (Config::dabSticky: the game writes its file only now and then, so it can say the old setting).
    const int choice = GetConfig().dabSticky;
    const bool sticky = choice == 0 ? g_game.sticky : choice == 2;
    if (sticky) {
        if (down && !g_wasDown)
            g_latched = !g_latched;
    } else {
        g_latched = false;
    }
    g_wasDown = down;
    // (Held at any speed: the game's Max Dab Speed doesn't stop the key sitting you.)
    const bool held = sticky ? g_latched : down;
    g_sitting = held || (g_game.autoDab && haveSpeed && speed < kStopped);
}

bool Sitting() { return g_sitting; }
bool KeyDown() { return g_down; }
const char* BindingText() { return g_game.text; }
bool NeedsGamepad() { return g_game.key.control != gamepad::kNone; }

}  // namespace dab
}  // namespace mxcam
