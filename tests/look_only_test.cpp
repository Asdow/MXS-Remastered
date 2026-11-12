// look_only_test.cpp
//   The controller looks with the first person mod off (camera::LookOnlyUpdate): riding along +x
//   with the game's first-person camera leaning and pitching, the look keys turn only that camera,
//   about WorldUp, its eye left where the game put it; ahead, the view is left untouched. The sky's
//   preview matches the frame's turn, a pause or the mod coming back on starts the head ahead
//   again, and replay::LooksLikeReplay() tells the game's replay camera from riding.
//   Runs in real time (the camera times frames with the clock), ~8 s.
//
//   Build: cl /EHsc /std:c++17 /W4 tests\look_only_test.cpp src\camera.cpp src\bike.cpp src\config.cpp
//          src\log.cpp src\replay.cpp user32.lib

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../src/bike.h"
#include "../src/camera.h"
#include "../src/config.h"
#include "../src/replay.h"

namespace mxcam {
bool IsDumping() { return false; }  // the frame dump lives in hooks.cpp, not linked here
}

using namespace mxcam;

namespace {

constexpr double kPi = 3.14159265358979;
constexpr double kDeg = kPi / 180.0;

int g_failures = 0;
void Check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

struct V { double x, y, z; };
V operator+(V a, V b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V operator-(V a, V b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V operator*(V a, double s) { return { a.x * s, a.y * s, a.z * s }; }
V Cross(V a, V b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
double Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double Length(V a) { return std::sqrt(Dot(a, a)); }
V Normalize(V a) { return a * (1.0 / Length(a)); }

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

void Mul(const double a[16], const double b[16], double out[16])
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = sum;
        }
}

struct Camera {
    V eye, forward, right, up;
};
Camera Read(const double m[16])
{
    Camera c;
    c.right   = { m[0], m[4], m[8] };
    c.up      = { m[1], m[5], m[9] };
    const V back{ m[2], m[6], m[10] };
    c.forward = back * -1.0;
    const V t{ m[12], m[13], m[14] };
    c.eye = (c.right * t.x + c.up * t.y + back * t.z) * -1.0;
    return c;
}

// The signed turn (deg, + = right) about WorldUp from a's heading to b's.
double TurnDeg(V a, V b)
{
    a.y = 0;
    b.y = 0;
    a = Normalize(a);
    b = Normalize(b);
    const V right = Cross(a, V{ 0, 1, 0 });
    return std::atan2(Dot(b, right), Dot(b, a)) / kDeg;
}

double PitchDeg(V forward) { return std::asin(std::clamp(Normalize(forward).y, -1.0, 1.0)) / kDeg; }

double Clock()
{
    static LARGE_INTEGER frequency, start;
    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - start.QuadPart) / frequency.QuadPart;
}

// The game camera at time t: riding along +x at 15 m/s, weaving, leaning and pitching.
void GameView(double t, double m[16], double* yaw = nullptr)
{
    const double y = 0.3 * std::sin(t * 1.3), pitch = -8 * kDeg + 4 * kDeg * std::sin(t * 2.1),
                 roll = 25 * kDeg * std::sin(t * 1.7);
    View({ 15.0 * t, 1.2 + 0.2 * std::sin(t * 3.0), 0.5 * std::sin(t) }, y, pitch, roll, m);
    if (yaw)
        *yaw = y;
}

struct Frame {
    bool   turned = false;   // a correction was returned
    double turnDeg = 0;      // the view's heading vs the game camera's
    double pitchDiff = 0;    // the view's pitch vs what turning the game camera about WorldUp gives
    double eyeMoved = 0;     // how far the corrected eye is from the game's
    double previewDiff = 0;  // largest element difference between the sky's preview and the frame
};

// One frame as the hooks see it with the mod off: the sky (rotation only), then the camera.
Frame Step(double t, camera::Look look)
{
    camera::SetLook(look);
    camera::FrameTick();
    double view[16];
    GameView(t, view);
    double sky[16];
    std::copy(view, view + 16, sky);
    sky[12] = sky[13] = sky[14] = 0;
    double preview[16];
    const bool previewed = camera::LookOnlyPreview(sky, preview);
    Frame f;
    double correction[16];
    f.turned = camera::LookOnlyUpdate(view, correction);
    if (!f.turned)
        for (int i = 0; i < 16; ++i)
            correction[i] = (i % 5 == 0) ? 1.0 : 0.0;
    for (int i = 0; i < 16; ++i)
        f.previewDiff = std::fmax(f.previewDiff, previewed ? std::fabs(preview[i] - correction[i]) : 1.0);
    double corrected[16];
    Mul(correction, view, corrected);
    const Camera raw = Read(view), out = Read(corrected);
    f.turnDeg = TurnDeg(raw.forward, out.forward);
    f.pitchDiff = std::fabs(PitchDeg(out.forward) - PitchDeg(raw.forward));
    f.eyeMoved = Length(out.eye - raw.eye);
    return f;
}

double Wrap(double degrees)
{
    while (degrees > 180) degrees -= 360;
    while (degrees < -180) degrees += 360;
    return degrees;
}

}  // namespace

int main()
{
    Config& cfg = EditConfig();
    cfg.enabled = false;
    cfg.controllerLook = true;
    cfg.lookSideDeg = 90.0;
    cfg.lookTurnSpeed = 12.0;
    camera::Reset();

    printf("Mod off, riding with the game's camera leaning and pitching: ahead, right, back, left, ahead\n");
    struct Phase { camera::Look look; double seconds; double want; const char* name; };
    const Phase phases[] = {
        { camera::Look::Ahead, 0.8, 0,   "ahead" },
        { camera::Look::Right, 0.8, 90,  "right" },
        { camera::Look::Back,  0.8, 180, "back" },
        { camera::Look::Left,  0.8, -90, "left" },
        { camera::Look::Ahead, 1.0, 0,   "ahead again" },
    };
    bool aheadUntouched = true, eyeStill = true, pitchKept = true, previewMatches = true, smooth = true;
    double last = 0, worstStep = 0;
    bool haveLast = false;
    const double begin = Clock();
    double phaseStart = 0;
    for (const Phase& phase : phases) {
        Frame f;
        for (;;) {
            const double t = Clock() - begin;
            if (t > phaseStart + phase.seconds)
                break;
            f = Step(t, phase.look);
            if (phase.want == 0 && &phase == &phases[0] && f.turned)
                aheadUntouched = false;
            eyeStill = eyeStill && f.eyeMoved < 1e-6;
            pitchKept = pitchKept && f.pitchDiff < 0.01;  // (a turn about WorldUp keeps the pitch)
            previewMatches = previewMatches && f.previewDiff < 1e-9;
            if (haveLast)
                worstStep = std::fmax(worstStep, std::fabs(Wrap(f.turnDeg - last)));
            last = f.turnDeg;
            haveLast = true;
            Sleep(7);
        }
        phaseStart += phase.seconds;
        printf("  %-11s view %+7.1f deg from the game camera (%s)\n", phase.name, f.turnDeg,
               f.turned ? "turned" : "untouched");
        const bool there = std::fabs(Wrap(f.turnDeg - phase.want)) < 1.0;
        char what[96];
        snprintf(what, sizeof what, "looking %s turns the view %.0f deg", phase.name, phase.want);
        Check(there, what);
        if (&phase == &phases[4])
            Check(!f.turned, "back ahead, the game's view is left exactly as it is");
    }
    smooth = worstStep < 60.0;  // (left from back is 270 deg the long way round: ~45 deg a frame at first)
    printf("  biggest turn in one frame: %.1f deg\n", worstStep);
    Check(aheadUntouched, "looking ahead, the game's view is never touched");
    Check(eyeStill, "the eye stays where the game put it");
    Check(pitchKept, "the head turns about WorldUp (the view's pitch is the game camera's)");
    Check(previewMatches, "the sky's preview is the frame's own correction");
    Check(smooth, "the head turns over several frames (under 60 deg a frame), never in one jump");

    printf("Holding back while the mod comes back on, then off again\n");
    {
        // A frame of the full camera, then the mod off again with back still held: it eases
        // from straight ahead, it doesn't jump.
        double view[16], correction[16];
        camera::FrameTick();
        GameView(Clock() - begin, view);
        camera::SetLook(camera::Look::Back);
        camera::Update(view, correction);
        Sleep(7);
        const Frame f = Step(Clock() - begin, camera::Look::Back);
        printf("  first look-only frame: %+.1f deg\n", f.turnDeg);
        Check(std::fabs(f.turnDeg) < 45.0, "the look-only head starts ahead and eases in (not 180 deg at once)");
    }

    printf("A pause (loading, alt-tab) while looking right\n");
    {
        for (int i = 0; i < 60; ++i) {
            Step(Clock() - begin, camera::Look::Right);
            Sleep(7);
        }
        Sleep(400);
        const Frame f = Step(Clock() - begin, camera::Look::Right);
        printf("  first frame after the pause: %+.1f deg\n", f.turnDeg);
        Check(std::fabs(f.turnDeg) < 1.0, "after a pause the head starts ahead again");
        camera::Reset();
        const Frame g = Step(Clock() - begin, camera::Look::Ahead);
        Check(!g.turned, "after a reset, looking ahead leaves the view untouched");
    }

    printf("Replays\n");
    {
        bool everRiding = false;
        double t0 = Clock();
        for (double t = 0; t < 0.6; t = Clock() - t0) {  // riding: never a replay
            double view[16];
            GameView(t, view);
            everRiding = everRiding || replay::LooksLikeReplay(view, Clock());
            Sleep(7);
        }
        Check(!everRiding, "riding doesn't look like a replay");
        // The game's replay camera: level, 26.565 deg down, panning slowly.
        bool replayNow = false;
        t0 = Clock();
        for (double t = 0; t < 0.6; t = Clock() - t0) {
            double view[16];
            View({ 3.0 * t, 5.0, 2.0 }, 0.2 * t, -26.565 * kDeg, 0, view);
            replayNow = replay::LooksLikeReplay(view, Clock());
            Sleep(7);
        }
        Check(replayNow, "the game's replay camera is told");
        Check(replay::Current() == replay::Mode::Off, "telling it leaves the mod's replay state alone");
        double view[16];
        GameView(1.0, view);
        Check(!replay::LooksLikeReplay(view, Clock()), "back to riding (the horizon tilts): not a replay");
    }

    printf("Third person: the chase camera 4 m behind and 1.5 m above the rider, looking right, then back\n");
    {
        // Riding along +x (+z is the rider's right). The rider at r, the game camera behind it
        // looking down at it.
        const auto Chase = [&](double t, double m[16], V& rider) {
            rider = V{ 15.0 * t, 0.8, 0.0 };
            View(rider + V{ -4.0, 1.5, 0.0 }, 0.0, -std::atan2(1.5, 4.0), 0.0, m);
        };
        camera::Reset();
        const camera::Look looks[2] = { camera::Look::Right, camera::Look::Back };
        const char* names[2] = { "right", "back" };
        for (int which = 0; which < 2; ++which) {
            double view[16], correction[16] = {};
            V rider;
            bool turned = false;
            const double t0 = Clock();
            while (Clock() - t0 < 1.0) {
                camera::SetLook(looks[which]);
                camera::FrameTick();
                Chase(Clock() - begin, view, rider);
                turned = camera::LookOnlyUpdate(view, correction);
                if (turned) {
                    const V p{ view[0] * rider.x + view[4] * rider.y + view[8] * rider.z + view[12],
                               view[1] * rider.x + view[5] * rider.y + view[9] * rider.z + view[13],
                               view[2] * rider.x + view[6] * rider.y + view[10] * rider.z + view[14] };
                    const double inEye[3] = { p.x, p.y, p.z };
                    camera::OrbitLook(view, inEye, correction);
                }
                Sleep(7);
            }
            double drawn[16];
            Mul(correction, view, drawn);
            const Camera raw = Read(view), out = Read(drawn);
            // Where the rider is on screen: its eye-space position before and after.
            const auto InEye = [&](const double m[16]) {
                return V{ m[0] * rider.x + m[4] * rider.y + m[8] * rider.z + m[12],
                          m[1] * rider.x + m[5] * rider.y + m[9] * rider.z + m[13],
                          m[2] * rider.x + m[6] * rider.y + m[10] * rider.z + m[14] };
            };
            const V before = InEye(view), after = InEye(drawn);
            const V offset = out.eye - rider;
            printf("  looking %-5s camera at (%+.2f, %+.2f, %+.2f) from the rider, facing %+.0f deg; rider moved %.4f on screen\n",
                   names[which], offset.x, offset.y, offset.z, TurnDeg(raw.forward, out.forward), Length(after - before));
            Check(turned, "the look turns the view");
            Check(Length(after - before) < 1e-3, "the rider stays where it was in the view");
            Check(std::fabs(Length(offset) - Length(raw.eye - rider)) < 1e-3 && std::fabs(offset.y - 1.5) < 1e-3,
                  "the camera keeps its distance and height from the rider");
            if (which == 0) {
                Check(offset.z < -3.9 && std::fabs(offset.x) < 0.1, "looking right: the camera is off the rider's left side");
                Check(out.forward.z > 0.9, "looking right: it faces right, across the rider");
            } else {
                Check(offset.x > 3.9 && std::fabs(offset.z) < 0.1, "looking back: the camera is out in front of the rider");
                Check(out.forward.x < -0.9, "looking back: it faces back, at the rider");
            }
        }
        camera::SetLook(camera::Look::Ahead);
    }

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
