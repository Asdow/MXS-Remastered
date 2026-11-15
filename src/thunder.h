// thunder.h
//   The snow's sounds (WEATHER tab: Config::snowSound - a recording of a blizzard's wind, looped;
//   Config::snowTires - a soft hush made from a sled's slide on snow, as your tires roll through it), the
//   rain's (Config::rainSound - a recording of steady rain, looped) and its
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

// Once a frame: how loud the strikes are (0..1, Config::lightningSound; 0 with no lightning), the
// rain (0..1: Config::rainSound, with how hard it rains; 0 with no rain), the snowstorm (0..1:
// Config::snowSound, with the snowfall and its wind; 0 with no snow) and your tires in the snow
// (tires 0..1: Config::snowTires, 0 with no snow; rolling 0..1: how much of it is heard now, by how
// you're riding; bright 0..1: how bright it sounds, brighter riding faster). Starts the audio
// output when any of them is turned up and stops it when none is.
void Update(double strikes, double rain, double snow, double tires, double rolling, double bright);

}  // namespace thunder
}  // namespace mxcam
