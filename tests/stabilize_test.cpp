// stabilize_test.cpp
//   Drives the camera module (camera.cpp) through what the STABILIZATION settings are for, on
//   the game camera and the custom camera, with Steadiness off and at its default:
//     - bumps: a straight full of quick lean and pitch jolts (3-6 Hz, a few degrees), which
//       should hardly reach the view;
//     - a corner: the bike leans 40 deg into a turn and holds it, which the view should follow
//       as far as Horizon lock lets it, soon after the bike;
//     - a wheelie: the nose comes up 25 deg and stays there, likewise for Pitch lock.
//   The game camera is fixed to the rider's head, 1 m above the bike along its up axis.
//   Runs in real time (the camera times frames with the clock), ~40 s.
//
//   Build: cl /EHsc /std:c++17 /W4 tests\stabilize_test.cpp src\camera.cpp src\bike.cpp src\config.cpp src\log.cpp user32.lib

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../src/bike.h"
#include "../src/camera.h"
#include "../src/config.h"

namespace mxcam {
bool IsDumping() { return false; }  // the frame dump lives in hooks.cpp, not linked here
}

using namespace mxcam;

namespace {

constexpr double kPi = 3.14159265358979, kDeg = kPi / 180.0;
constexpr double kSpeed = 15.0;
constexpr double kStart = 2.5;  // s of level riding first: the tracker finds the bike, rest pitch is learnt
constexpr double kEnd = 4.6;

int g_failures = 0;
void Check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

struct V { double x, y, z; };
V operator+(V a, V b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V operator*(V a, double s) { return { a.x * s, a.y * s, a.z * s }; }
V Cross(V a, V b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
double Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V Normalize(V a) { return a * (1.0 / std::sqrt(Dot(a, a))); }

// The view matrix of a camera at eye with the given yaw (0 = +x, + = toward +z), pitch and roll.
void View(V eye, double yaw, double pitch, double roll, double m[16])
{
    const V heading{ std::cos(yaw), 0, std::sin(yaw) }, worldUp{ 0, 1, 0 };
    const V forward = heading * std::cos(pitch) + worldUp * std::sin(pitch);
    const V levelRight = Normalize(Cross(heading, worldUp));
    const V levelUp = Cross(levelRight, forward);
    const V right = levelRight * std::cos(roll) + levelUp * std::sin(roll);
    const V up = Cross(right, forward);
    const V back = forward * -1.0;
    const V axes[3] = { right, up, back };
    for (int r = 0; r < 3; ++r) {
        m[0 * 4 + r] = axes[r].x;
        m[1 * 4 + r] = axes[r].y;
        m[2 * 4 + r] = axes[r].z;
        m[3 * 4 + r] = -Dot(axes[r], eye);
        m[r * 4 + 3] = 0;
    }
    m[15] = 1;
}

// One frame the way the hooks see it: the camera view, then the bike drawn at bike with the
// camera's orientation (the head is fixed to it), which the tracker locks onto.
const camera::Telemetry& Draw(V bike, V eye, double yaw, double pitch, double roll)
{
    double view[16], axes[16];
    View(eye, yaw, pitch, roll, view);
    View({ 0, 0, 0 }, yaw, pitch, roll, axes);
    double model[16] = {};  // the bike's axes as columns, at bike
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            model[c * 4 + r] = axes[r * 4 + c];
    model[12] = bike.x; model[13] = bike.y; model[14] = bike.z; model[15] = 1;
    float viewModel[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += view[k * 4 + r] * model[c * 4 + k];
            viewModel[c * 4 + r] = float(sum);
        }
    camera::FrameTick();
    bike::BeginFrame(view);
    double correction[16];
    camera::Update(view, correction);
    bike::Observe(0, viewModel);
    bike::EndFrame(camera::GameTime(), camera::FrameInterval());
    return camera::LastTelemetry();
}

// 0 -> 1 over `over` seconds from x = 0, eased.
double Ease(double x, double over)
{
    const double u = std::fmin(std::fmax(x / over, 0.0), 1.0);
    return u * u * (3 - 2 * u);
}

enum class Kind { Bumps, Corner, Wheelie };

struct Result {
    double rollIn = 0, rollOut = 0;    // bumps: RMS of the jolts, the bike's and the view's (deg)
    double pitchIn = 0, pitchOut = 0;
    double held = 0;                   // corner / wheelie: the view's lean / pitch once held (deg)
    double reach = 0;                  // s after the bike got there until the view had 90% of it
    bool   tracked = true;
};

Result Ride(Kind kind, bool custom, double steadiness)
{
    Config& cfg = EditConfig();
    cfg.customCamera = custom;
    cfg.steadiness = steadiness;
    cfg.horizonLock = 0.6;
    cfg.pitchLock = 0.5;
    camera::Reset();
    bike::Reset();

    Result result;
    double yaw = 0, lastClock = 0, levelRoll = 0, levelPitch = 0;
    double sumRollIn = 0, sumRollIn2 = 0, sumRollOut = 0, sumRollOut2 = 0;
    double sumPitchIn = 0, sumPitchIn2 = 0, sumPitchOut = 0, sumPitchOut2 = 0, samples = 0;
    double heldSum = 0, heldCount = 0, reachedAt = -1;
    struct Sample { double t, value; };
    static Sample history[4096];
    int count = 0;
    V pos{ 0, 0, 0 };
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (;;) {
        QueryPerformanceCounter(&now);
        const double t = double(now.QuadPart - start.QuadPart) / frequency.QuadPart;
        if (t > kEnd)
            break;
        const double dt = t - lastClock;
        lastClock = t;
        const double e = t - kStart;
        double roll = 0, pitch = 0, turnRate = 0;
        if (e > 0) {
            switch (kind) {
            case Kind::Bumps:
                roll  = 4.0 * kDeg * std::sin(2 * kPi * 5.0 * e) + 2.0 * kDeg * std::sin(2 * kPi * 3.3 * e + 1.0);
                pitch = 3.0 * kDeg * std::sin(2 * kPi * 4.0 * e) + 2.0 * kDeg * std::sin(2 * kPi * 6.0 * e + 2.0);
                break;
            case Kind::Corner:  // leaned in over 0.4 s, turning 60 deg/s once leaned
                roll = -40.0 * kDeg * Ease(e, 0.4);
                turnRate = 60.0 * kDeg * Ease(e, 0.4);
                break;
            case Kind::Wheelie:  // nose up over 0.5 s
                pitch = 25.0 * kDeg * Ease(e, 0.5);
                break;
            }
        }
        yaw += turnRate * dt;
        pos = pos + V{ std::cos(yaw), 0, std::sin(yaw) } * (kSpeed * dt);
        double m[16];
        View({ 0, 0, 0 }, yaw, pitch, roll, m);
        const V up{ m[1], m[5], m[9] };
        const camera::Telemetry& tel = Draw(pos, pos + up * 1.0, yaw, pitch, roll);

        if (e > -0.6 && e < 0) {  // level, just before
            levelRoll = tel.outRoll;
            levelPitch = tel.outPitch;
        }
        if (e > -0.3)
            result.tracked &= tel.bikeTracked;
        if (kind == Kind::Bumps && e > 0.6) {
            const double ri = roll / kDeg, ro = tel.outRoll, pi = pitch / kDeg, po = tel.outPitch;
            sumRollIn += ri;  sumRollIn2 += ri * ri;  sumRollOut += ro;  sumRollOut2 += ro * ro;
            sumPitchIn += pi; sumPitchIn2 += pi * pi; sumPitchOut += po; sumPitchOut2 += po * po;
            samples += 1;
        }
        if (kind != Kind::Bumps && e > 0 && count < 4096) {
            const double value = kind == Kind::Corner ? std::fabs(tel.outRoll - levelRoll) : tel.outPitch - levelPitch;
            history[count++] = { e, value };
            if (e > 1.4) {
                heldSum += value;
                heldCount += 1;
            }
        }
        Sleep(7);
    }
    const auto Rms = [&](double sum, double sum2) { return std::sqrt(std::fmax(sum2 / samples - (sum / samples) * (sum / samples), 0.0)); };
    if (kind == Kind::Bumps) {
        result.rollIn = Rms(sumRollIn, sumRollIn2);
        result.rollOut = Rms(sumRollOut, sumRollOut2);
        result.pitchIn = Rms(sumPitchIn, sumPitchIn2);
        result.pitchOut = Rms(sumPitchOut, sumPitchOut2);
    } else {
        result.held = heldCount > 0 ? heldSum / heldCount : 0;
        const double bikeThere = kind == Kind::Corner ? 0.4 : 0.5;
        for (int i = 0; i < count; ++i) {
            if (history[i].value >= 0.9 * result.held) {
                reachedAt = history[i].t;
                break;
            }
        }
        result.reach = reachedAt >= 0 ? reachedAt - bikeThere : 99;
    }
    return result;
}

}  // namespace

int main()
{
    printf("Horizon lock 60%%, pitch lock 50%%; the head 1 m above the bike, riding at 15 m/s\n");
    for (int custom = 0; custom < 2; ++custom) {
        printf("\n%s camera\n", custom ? "CUSTOM" : "GAME");
        const double settings[2] = { 0.0, 0.6 };
        Result bumps[2], corner[2], wheelie[2];
        for (int i = 0; i < 2; ++i) {
            bumps[i] = Ride(Kind::Bumps, custom != 0, settings[i]);
            corner[i] = Ride(Kind::Corner, custom != 0, settings[i]);
            wheelie[i] = Ride(Kind::Wheelie, custom != 0, settings[i]);
            printf("  steadiness %3.0f%%: bumps reach the view at %4.0f%% (lean %.2f of %.2f deg) and %4.0f%% (pitch %.2f of %.2f deg)\n",
                   settings[i] * 100, 100 * bumps[i].rollOut / bumps[i].rollIn, bumps[i].rollOut, bumps[i].rollIn,
                   100 * bumps[i].pitchOut / bumps[i].pitchIn, bumps[i].pitchOut, bumps[i].pitchIn);
            printf("                    40 deg lean: view leans %.1f deg, 90%% of it %.2f s after the bike;"
                   " 25 deg wheelie: view pitches %.1f deg, 90%% %.2f s after\n",
                   corner[i].held, corner[i].reach, wheelie[i].held, wheelie[i].reach);
            if (!bumps[i].tracked || !corner[i].tracked || !wheelie[i].tracked)
                printf("    (the tracker lost the bike in a ride that counts)\n");
        }
        Check(bumps[0].rollOut > 0.15 * bumps[0].rollIn, "steadiness off: the jolts reach the view (test is meaningful)");
        Check(bumps[1].rollOut < 0.4 * bumps[0].rollOut, "steadiness: the view leans with the jolts less than half as much");
        Check(bumps[1].pitchOut < 0.4 * bumps[0].pitchOut, "steadiness: the view dives with the jolts less than half as much");
        Check(bumps[1].rollOut < 0.08 * bumps[1].rollIn && bumps[1].pitchOut < 0.08 * bumps[1].pitchIn,
              "steadiness: under 8% of the jolts reach the view");
        Check(std::fabs(corner[1].held - corner[0].held) < 1.0, "steadiness: a held lean is followed as far as without it");
        Check(std::fabs(wheelie[1].held - wheelie[0].held) < 1.0, "steadiness: a held wheelie is followed as far as without it");
        Check(std::fabs(corner[1].held - 16.0) < 2.0, "the view keeps 40% of a 40 deg lean (horizon lock 60%)");
        Check(std::fabs(wheelie[1].held - 12.5) < 2.0, "the view keeps 50% of a 25 deg wheelie (pitch lock 50%)");
        Check(corner[1].reach < 0.45 && wheelie[1].reach < 0.45, "steadiness: the view catches up within 0.45 s");
    }
    printf("\n%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
