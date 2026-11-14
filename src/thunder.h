// thunder.h
//   Thunder for the rain's lightning (WEATHER tab: Config::lightning, Config::thunder). Each strike
//   postfx draws is heard here as late as it was far (sound's 343 m a second): a close one a ripping
//   crack and a boom, then a rumble; a far one only a low, long roll. Synthesised as it plays (no sound
//   file) and played through Windows' own audio output (waveOut; ALSA on Linux), beside the game's
//   sound, as the brake squeal is (brake.h) - nothing of the game's is changed or read.
#pragma once

namespace mxcam {
namespace thunder {

// A strike `metres` away: its thunder starts `delay` seconds from now.
void Strike(double metres, double delay);

// Once a frame: on (the rain's lightning on, and heard at all), and how loud (0..1, Config::thunder).
// Starts the audio output when on and stops it when not.
void Update(bool on, double volume);

}  // namespace thunder
}  // namespace mxcam
