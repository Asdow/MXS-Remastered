// thunder.h
//   The rain's sound (WEATHER tab: Config::rainSound - a recording of steady rain, looped) and its
//   lightning's (Config::lightning, Config::lightningSound): each strike postfx draws is heard as late
//   as it was far (sound's 343 m a second) - a recording of a real strike, a little different each
//   time, deeper and duller far off. Played through Windows' own audio output (waveOut; ALSA on
//   Linux), beside the game's sound, as the brake squeal is (brake.h) - nothing of the game's is
//   changed or read.
#pragma once

namespace mxcam {
namespace thunder {

// A strike `metres` away: its sound starts `delay` seconds from now.
void Strike(double metres, double delay);

// Once a frame: how loud the strikes are (0..1, Config::lightningSound; 0 with no lightning) and the
// rain (0..1: Config::rainSound, with how hard it rains; 0 with no rain). Starts the audio output when
// either is heard and stops it when neither is.
void Update(double strikes, double rain);

}  // namespace thunder
}  // namespace mxcam
