// timeofday.h
//   The day / night cycle behind the Time of day slider: where the sun (and the moon) are at a
//   given hour, and how the picture is graded for it. No OpenGL here, so it can be tested alone.
//
//   The game has one fixed sun. Its direction is kept as the noon position: the sun rises 90
//   degrees to one side of it at 06:00, peaks at 12:00 and sets on the other side at 18:00. The
//   moon is up opposite it at night. Light is the direction the game's shaders should light from:
//   the sun by day, the moon by night.
#pragma once

#include "mathlib.h"

namespace mxcam {
namespace timeofday {

struct Sky {
    Vec3   sun;              // world, toward the sun (below the horizon at night)
    Vec3   moon;             // world, toward the moon
    Vec3   light;            // world, the light the scene is lit by
    double sunElevationDeg = 0;
    double day    = 1;       // 0..1 full daylight
    double golden = 0;       // 0..1 low sun: warm light, orange sky near the sun
    double night  = 0;       // 0..1 dark: stars, moonlight
    Vec3   tint{ 1, 1, 1 };  // multiplies the lit scene (linear colour)
    double exposure   = 1;   // multiplies the lit scene
    double desaturate = 0;   // 0..1 towards grey (night vision)
    double haze       = 0;   // 0..1 how much the distance fades into the sky colour
    double nightLevel = 1;   // how bright the night is (the night sky follows it too)
};

// hours: 0..24 (wraps). gameSun: the game's own sun direction (world, toward it). up: world up.
// nightLevel: 0..2, how much moonlight the night keeps (1 = the default).
Sky Compute(double hours, Vec3 gameSun, Vec3 up, double nightLevel = 1.0);

// The neutral grading the game's own lighting gets (the slider set to GAME).
Sky Neutral(Vec3 gameSun, Vec3 up);

}  // namespace timeofday
}  // namespace mxcam
