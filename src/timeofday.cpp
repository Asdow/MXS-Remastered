// timeofday.cpp
#include "timeofday.h"

namespace mxcam {
namespace timeofday {
namespace {

constexpr double kNoonElevationDeg = 62.0;  // how high the sun climbs
constexpr double kMoonElevationDeg = 38.0;  // the moon's height at midnight

double Smoothstep(double edge0, double edge1, double x)
{
    const double t = Clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double Mix(double a, double b, double t) { return a + (b - a) * t; }
Vec3 Mix(Vec3 a, Vec3 b, double t) { return a + (b - a) * t; }

// Any horizontal unit vector, if the game's sun is straight overhead (or missing).
Vec3 Horizontal(Vec3 v, Vec3 up)
{
    Vec3 h = v - up * Dot(v, up);
    if (Length(h) < 1e-6)
        h = Cross(up, std::fabs(up.x) < 0.9 ? Vec3{ 1, 0, 0 } : Vec3{ 0, 0, 1 });
    return Normalize(h);
}

Vec3 FromAngles(Vec3 horizontal, Vec3 up, double elevationDeg)
{
    const double e = elevationDeg * kDegToRad;
    return Normalize(horizontal * std::cos(e) + up * std::sin(e));
}

}  // namespace

Sky Compute(double hours, Vec3 gameSun, Vec3 up, double nightLevel)
{
    up = Normalize(up);
    hours = std::fmod(hours, 24.0);
    if (hours < 0)
        hours += 24.0;

    // The sun swings 15 degrees an hour around the up axis, through the game's sun at noon.
    const Vec3 noon = Horizontal(gameSun, up);
    const Quat swing = AxisAngle(up, (hours - 12.0) * 15.0 * kDegToRad);
    const Vec3 sunHorizontal = Rotate(swing, noon);
    const double elevation = kNoonElevationDeg * std::sin(2.0 * kPi * (hours - 6.0) / 24.0);

    Sky sky;
    sky.sunElevationDeg = elevation;
    sky.sun  = FromAngles(sunHorizontal, up, elevation);
    // The moon: on the other side of the sky, highest at midnight, never quite on the horizon.
    const double moonElevation = std::fmax(10.0, kMoonElevationDeg * Clamp(-elevation / kNoonElevationDeg, 0.0, 1.0) + 10.0);
    sky.moon = FromAngles(sunHorizontal * -1.0, up, moonElevation);

    sky.day    = Smoothstep(-4.0, 10.0, elevation);
    sky.night  = 1.0 - Smoothstep(-14.0, -3.0, elevation);
    sky.golden = Smoothstep(-6.0, 1.0, elevation) * (1.0 - Smoothstep(6.0, 24.0, elevation));

    // Lit by the sun until it's a few degrees under the horizon (grazing, so the ground still
    // catches some), then by the moon.
    const Vec3 grazingSun = FromAngles(sunHorizontal, up, std::fmax(elevation, 3.0));
    sky.light = Normalize(Mix(grazingSun, sky.moon, Smoothstep(-3.0, -9.0, elevation)));

    // Grading: full brightness by day, dimming through dusk to blue moonlight. nightLevel scales
    // how much of the scene the moon leaves visible (1 = 2% of daylight).
    const double brightness = Smoothstep(-13.0, 8.0, elevation);
    sky.nightLevel = Clamp(nightLevel, 0.0, 2.0);
    sky.exposure   = Mix(0.02 * sky.nightLevel, 1.0, brightness * brightness);
    const Vec3 moonlight{ 0.55, 0.72, 1.15 };
    const Vec3 sunset{ 1.22, 0.9, 0.66 };
    sky.tint       = Mix(moonlight, Vec3{ 1, 1, 1 }, Smoothstep(-10.0, 0.0, elevation));
    sky.tint       = Mix(sky.tint, sunset, sky.golden);
    sky.desaturate = 0.6 * sky.night;
    sky.haze       = 0.75 * (1.0 - sky.day);
    return sky;
}

Sky Neutral(Vec3 gameSun, Vec3 up)
{
    Sky sky;
    sky.sun = sky.light = Normalize(gameSun);
    sky.moon = Normalize(gameSun * -1.0);
    const double s = Clamp(Dot(sky.sun, Normalize(up)), -1.0, 1.0);
    sky.sunElevationDeg = std::asin(s) * kRadToDeg;
    return sky;
}

}  // namespace timeofday
}  // namespace mxcam
