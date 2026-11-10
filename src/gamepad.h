// gamepad.h
//   Reads a game controller for looking around, which MX Simulator has no controls for. Only
//   Windows' own controller APIs are used - XInput (Xbox-style pads), the joystick API (any other
//   pad, and the extra buttons some pads have) and Windows.Gaming.Input (the only one that shows
//   an Xbox Elite controller's paddles) - nothing in the game is read or changed, and the game
//   still gets every press as usual.
#pragma once

namespace mxcam {
namespace gamepad {

// One control to bind an action to: a button, a trigger, one way of one stick axis, a d-pad
// direction, a paddle, or a button / axis direction of another kind of controller. 0 is none.
// Kept in the ini by name (ControlKey / ParseControl).
using Control = int;
constexpr Control kNone = 0;

// The d-pad directions (any controller's: an Xbox pad's d-pad, or another pad's hat): the
// default looks. (Xbox button bits 2, 3 and 1; see gamepad.cpp.)
constexpr Control kDPadLeft  = (1 << 8) | 2;
constexpr Control kDPadRight = (1 << 8) | 3;
constexpr Control kDPadDown  = (1 << 8) | 1;

// Reads the controllers (once a frame, before IsDown). Cheap: slots with nothing plugged in are
// only searched again once a second, from a thread of its own.
void Poll();

// Whether a control is held as of the last Poll(). A stick or trigger counts past half way.
bool IsDown(Control control);

// An axis's position as of the last Poll(), numbered as MX Simulator's controls file numbers them:
// an Xbox-style pad's 0 / 1 left stick X / Y and 2 / 3 right stick X / Y (-1..1), 4 / 5 left / right
// trigger (0..1); another controller's axes through the joystick API (-1..1). 0 with none.
double XInputAxis(int axis);
double JoyAxis(int axis);

// Binding: StartCapture() notes what's held now; PollCapture() (after each Poll) then gives the
// first control pressed since that wasn't held (a held one counts once it's been let go), or 0.
void StartCapture();
Control PollCapture();

// For the settings panel ("D-PAD LEFT", "RIGHT STICK LEFT", "PADDLE 1", "BUTTON 17"...) and the
// ini ("DPAD_LEFT", "RS_X-", "PADDLE1", "JOY_BUTTON17"...). ParseControl takes the ini's name
// (any case) and gives kNone for anything else.
const char* ControlName(Control control);
const char* ControlKey(Control control);
Control ParseControl(const char* key);

// Short status for the settings panel: "XBOX PAD", "PAD" (another controller) or "NO PAD".
const char* StatusText();

}  // namespace gamepad
}  // namespace mxcam
