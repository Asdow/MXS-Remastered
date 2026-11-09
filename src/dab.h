// dab.h
//   The dab indicator (OTHER tab, Config::dabIndicator): whether your rider sits (dabs) or stands,
//   from the game's own controls. The game keeps them in the config file in its personal folder:
//   which key or button sits / dabs (dab_key), Dab When Stopped (auto_dab), Sticky Sit/Dab
//   (sticky_dab) and Max Dab Speed (max_dab_speed, only logged: the key sits you at any speed).
//   That key or button is read here the way the look controls are (the keyboard, XInput, the joystick API): nothing of the game's is read but
//   its config file, and the game still gets every press.
#pragma once

namespace mxcam {
namespace dab {

// Once a frame, with your speed (m/s) if it's known: whether your rider sits now.
void Update(bool haveSpeed, double speed);

// The last Update's answer.
bool Sitting();

// For the trace (F11): the dab key held, and what it was read as.
bool KeyDown();
const char* BindingText();

// Whether the dab key is a controller's (gamepad::Poll must run every frame).
bool NeedsGamepad();

}  // namespace dab
}  // namespace mxcam
