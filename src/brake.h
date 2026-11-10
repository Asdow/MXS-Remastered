// brake.h
//   Brake squeal (OTHER tab, SOUND MODS: Config::brakeSqueal): MX Simulator has no brake sound, so
//   the squeal of your discs when you brake hard is made here and played through Windows' own audio
//   output (waveOut), beside the game's sound - nothing of the game's is changed or read but its
//   settings file. When: the game's own brake controls (front_brake_key / rear_brake_key in its
//   settings, read the way the dab indicator reads the dab key: the keyboard, XInput, the joystick
//   API) pressed hard, while your bike is moving and actually slowing down (not the brake held in
//   the air or standing still). The sound is synthesised as it plays: no sound file.
#pragma once

namespace mxcam {
namespace brake {

// Once a frame (after gamepad::Poll when NeedsGamepad): reads the brakes and your speed (m/s, if
// known) and sets how hard the squeal plays. Starts the audio output when Config::brakeSqueal is
// turned up and stops it when it's turned off.
void Update(bool haveSpeed, double speed);

// Whether a brake is bound to a controller (gamepad::Poll must then run every frame).
bool NeedsGamepad();

}  // namespace brake
}  // namespace mxcam
