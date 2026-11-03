// replay.cpp
//   Recognising a replay (see replay.h), and the chase and helmet cameras.
//
//   The chase camera needs to know where the rider is this frame, but the terrain (whose upload
//   is the camera) is drawn before the rider. The game's replay camera is aimed at the rider, so
//   the rider is placed relative to it: the rider's offset from the replay camera is measured on
//   the bones each frame in the replay camera's own axes (it orbits the rider, so in world space
//   that offset keeps turning), smoothed, and applied to the next frame's replay camera.
//   Everything that sets the chase camera's orientation (heading, slope, ease-in) is worked out
//   once a frame is drawn, so the next frame's orientation is already known when its sky is.
//
//   The helmet camera sits on the rider's head, which is also only known once the rider is
//   drawn. It is placed the same way: the mount's offset from the replay camera (in its axes) and
//   its rotation are measured each frame and carried one frame forward at the rate they last
//   changed, so the camera stays on the head instead of trailing a frame behind.
//
//   The head is a model of its own, not part of the skinned body: the game draws it (helmet and
//   all) right after the body, rigidly, and it turns and tilts on its own - riders look into the
//   corners and hold their heads up while the body leans. Its -Z is where the face looks. The
//   camera turns with it. Where the eyes sit inside it isn't known, so the eye the torso bone
//   gives is carried into the head model's frame and averaged there: on average the camera sits
//   where the torso puts the eyes, but it moves and turns rigidly with the helmet.
//   The chest mount rides on the torso bone instead, placed from the middle of the shoulders.
//   The camera works from last frame's head carried forward, and stabilised, while the game
//   draws the head where it really is this frame. Right under the lens the smallest difference
//   shows, so the helmet would wobble in the picture; it is drawn where the camera put it
//   instead, held still under the camera like a real mount.

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "config.h"
#include "log.h"
#include "mathlib.h"
#include "replay.h"

namespace mxcam {
namespace replay {
namespace {

// Detection. The replay camera looks down at exactly atan(5 / 10) with a perfectly level
// horizon, whichever way it faces as it orbits the rider. Held to a twentieth of a degree for
// kEnterSeconds, that can't be a ride. Once in a replay the camera may rise and look down more
// steeply (it seems to lift over the terrain) but stays level; it ends when the horizon tilts,
// or when the camera jumps somewhere it no longer looks the replay way.
constexpr double kReplayPitchDeg = 26.565;
constexpr double kReplayDistance = 11.18;   // from the rider: sqrt(5^2 + 10^2)
constexpr double kAngleTolerance = 0.05;    // degrees
constexpr double kEnterSeconds   = 0.3;
constexpr double kJumpDistance   = 8.0;     // m in one frame

// Chase camera.
constexpr double kSlopeFollow    = 0.5;     // fraction of the slope ridden that tilts the camera with it
constexpr double kMaxSlopeDeg    = 25.0;
constexpr double kTurnRate       = 4.0;     // 1/s: how fast it swings round behind the rider
constexpr double kOffsetRate     = 2.0;     // 1/s: smoothing of the rider's offset from the replay camera
constexpr double kEaseSeconds    = 0.8;     // from the game's replay view to the chase view
constexpr double kMinSpeed       = 1.5;     // m/s: slower than this the heading is held

// Helmet camera. The rider's torso is one bone, on the body's midline (the highest one); the
// game's own first-person eye sits this far up and forward of the bone's centre, in units of the
// skeleton's half-span (its outermost arm bone), so the rider's size doesn't matter. A capture
// had +0.04 +2.19 -0.82 with a half-span of 1.46.
constexpr double kEyeUp          = 1.5;
constexpr double kEyeForward     = 0.56;
// The head model: its origin was 0.4 to 1.0 from the middle of the shoulders in captures, and
// its right axis within about 35 degrees of the line between them.
constexpr double kHeadReach      = 0.9;     // half-spans from the middle of the shoulders
constexpr double kHeadAlign      = 0.4;     // least cosine between its right axis and the shoulders'
constexpr int    kEyeSamples     = 1000;    // frames the eye's place in the head model is averaged over
constexpr double kHeadDrawMatch  = 1.0;     // m: the head model drawn this near where the camera put it is ours
constexpr double kHeadMatch      = 2.0;     // m: the skeleton has to be this close to the rider followed
constexpr double kMountJump      = 2.0;     // m in one frame: a seek or another rider, not carried forward
constexpr double kMaxStepDeg     = 20.0;    // a bigger turn in one frame isn't carried forward either
constexpr double kMaxSmoothing   = 0.25;    // s: the rotation's time constant at full stabilisation
constexpr int    kHeadlessFrames = 30;      // frames without the head before the game's view returns

constexpr int kMaxRiders = 64;
constexpr int kMaxBones  = 256;

struct Detection {
    bool   haveLast      = false;
    Vec3   lastPos;
    double lastTime      = 0;
    double signatureTime = 0;  // how long the camera has looked exactly the replay way
    bool   active        = false;
};

// This frame's replay camera.
struct ReplayCamera {
    Quat rot;
    Vec3 pos;
    Vec3 right, up, back;
};

struct Chase {
    bool   haveRider     = false;
    Vec3   offset;               // rider minus the replay camera, in its axes (right, up, back), smoothed
    bool   haveLastRider = false;
    Vec3   lastRider;
    double lastTime      = -1;
    Vec3   velocity;
    bool   haveHeading   = false;
    bool   snapHeading   = false;  // take the next heading as it is (after a seek or a change of rider)
    Vec3   heading;                // horizontal, unit
    double slope         = 0;      // radians, + = climbing
    double ease          = 0;      // 0..1, linear; the view blends by its smoothstep
    // Prepared for the next frame.
    Quat   rot;
    Vec3   eyeOffset;              // eye minus the rider, world space
};

// A rider's head, from one skinned draw (and the head model drawn after it).
struct Head {
    Vec3   centre;          // the middle of the skeleton's bones, as in g_riders
    Vec3   eye;             // world space, from the torso bone
    Quat   rot;             // the torso's axes: x right, y up, z back
    bool   haveShoulders = false;
    Vec3   shoulders;       // between the shoulder joints
    Vec3   across;          // from the left shoulder to the right, unit
    double reach = 0;       // how far from the shoulders the head model may be
    bool   model = false;   // the head model was found
    Vec3   modelPos;        // its origin, world space
    Quat   modelRot;        // its axes: x right, y up, z back (the face looks down -Z)
};

struct Helmet {
    bool   have      = false;      // the next frame's view is prepared
    bool   onModel   = false;      // it rides on the head model (not the torso)
    int    anchor    = -1;         // what it rides on: 0 the torso (no head model), 1 the head model, 2 the chest
    int    headless  = 0;          // frames in a row without the rider's head
    Vec3   lastCentre;             // the rider followed last frame
    Vec3   eyeInModel;             // the eye in the head model's frame, averaged
    int    eyeSamples = 0;
    bool   haveLast  = false;      // offset and target hold last frame's mount
    Vec3   offset;                 // the mount minus the replay camera, in its axes
    Quat   target;                 // the mount's rotation (tilted, levelled), unstabilised
    // Prepared for the next frame.
    Quat   rot;
    Vec3   eyeOffset;              // the eye minus the replay camera, in its axes
    Vec3   mountInModel;           // the camera's place in the head model's frame
    Quat   cameraInModel;          // and its rotation there, before stabilisation
    // This frame's view, and where the head model is drawn to sit still under it.
    double view[16] = {};
    Vec3   drawPos;
    Quat   drawRot;
};

Detection    g_detect;
ReplayCamera g_camera;
Chase        g_chase;
Helmet       g_helmet;
Mode         g_mode = Mode::Off;
Vec3         g_riders[kMaxRiders];  // the middle of each skinned mesh's bones this frame
int          g_riderCount = 0;
Head         g_heads[kMaxRiders];   // each skinned draw's head this frame
int          g_headCount = 0;
int          g_pendingHead = -1;    // the head whose body was just drawn: the next model may be its head

constexpr int kHelmetCamera = 2;  // Config::replayCamera
constexpr int kChestMount   = 3;  // Config::helmetMount

Vec3 WorldUp()
{
    const Config& cfg = GetConfig();
    return Normalize(Vec3{ cfg.worldUp[0], cfg.worldUp[1], cfg.worldUp[2] });
}

double LowPassAlpha(double ratePerSecond, double dt) { return 1.0 - std::exp(-ratePerSecond * dt); }

double SmoothStep(double edge0, double edge1, double x)
{
    const double t = Clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double Blend() { return SmoothStep(0.0, 1.0, g_chase.ease); }

// Camera axes and position of a rigid view matrix; false if it isn't one.
bool Decompose(const double view[16], Vec3& right, Vec3& up, Vec3& back, Vec3& pos)
{
    right = { view[0], view[4], view[8] };
    up    = { view[1], view[5], view[9] };
    back  = { view[2], view[6], view[10] };
    if (std::fabs(Length(right) - 1.0) > 0.01 || std::fabs(Length(up) - 1.0) > 0.01 ||
        std::fabs(Length(back) - 1.0) > 0.01 || Dot(Cross(right, up), back) < 0.9)
        return false;
    back  = Normalize(back);
    right = Normalize(Cross(up, back));
    up    = Cross(back, right);
    const Vec3 t{ view[12], view[13], view[14] };
    pos = -(right * t.x + up * t.y + back * t.z);
    return true;
}

// From the replay camera's axes to world space and back.
Vec3 ToWorld(Vec3 local) { return g_camera.right * local.x + g_camera.up * local.y + g_camera.back * local.z; }
Vec3 ToCamera(Vec3 world) { return { Dot(world, g_camera.right), Dot(world, g_camera.up), Dot(world, g_camera.back) }; }

// out(r, c) = dot(new axis r, raw axis c): the rotation taking the raw view to rot.
void RotationCorrection(Quat rot, Vec3 rawRight, Vec3 rawUp, Vec3 rawBack, double out[16])
{
    const Vec3 newAxes[3] = { Rotate(rot, { 1, 0, 0 }), Rotate(rot, { 0, 1, 0 }), Rotate(rot, { 0, 0, 1 }) };
    const Vec3 rawAxes[3] = { rawRight, rawUp, rawBack };
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r)
            out[c * 4 + r] = (r < 3 && c < 3) ? Dot(newAxes[r], rawAxes[c]) : (r == c ? 1.0 : 0.0);
    }
}

// The eye-space correction from the raw view to a camera at eye with rotation q.
bool ViewCorrection(const double view[16], Quat q, Vec3 eye, double correction[16], double rotationOnly[16],
                    double used[16] = nullptr)
{
    const Vec3 R = Rotate(q, { 1, 0, 0 }), U = Rotate(q, { 0, 1, 0 }), B = Rotate(q, { 0, 0, 1 });
    const double newView[16] = {
        R.x, U.x, B.x, 0,
        R.y, U.y, B.y, 0,
        R.z, U.z, B.z, 0,
        -Dot(R, eye), -Dot(U, eye), -Dot(B, eye), 1,
    };
    double rawInverse[16];
    if (!AffineInverse(view, rawInverse))
        return false;
    MatMul(newView, rawInverse, correction);
    RotationCorrection(q, g_camera.right, g_camera.up, g_camera.back, rotationOnly);
    if (used)
        std::copy(newView, newView + 16, used);
    return true;
}

Quat Conjugate(Quat q) { return { -q.x, -q.y, -q.z, q.w }; }

void Stop(const char* why)
{
    if (g_detect.active)
        Log("replay: over (%s), back to the helmet cam", why);
    g_detect.active = false;
    g_detect.signatureTime = 0;
    g_mode = Mode::Off;
}

// Which skinned mesh is the rider the replay camera follows: the one nearest where it was last
// frame, or else nearest the point the camera looks at. Returns the rider's offset from the
// camera, in the camera's axes.
Vec3 MeasureOffset()
{
    const Chase& c = g_chase;
    const Vec3 lookAt{ 0, 0, -kReplayDistance };
    const auto Nearest = [](Vec3 around, double within) {
        int best = -1;
        for (int i = 0; i < g_riderCount; ++i) {
            const double distance = Length(g_riders[i] - around);
            if (distance < within) {
                within = distance;
                best = i;
            }
        }
        return best;
    };
    int rider = c.haveRider ? Nearest(g_camera.pos + ToWorld(c.offset), 3.0) : -1;
    if (rider < 0)
        rider = Nearest(g_camera.pos + ToWorld(lookAt), 8.0);
    if (rider >= 0)
        return ToCamera(g_riders[rider] - g_camera.pos);
    // No bones this frame (or none near): keep the last offset, or aim where the camera looks.
    return c.haveRider ? c.offset : lookAt;
}

// The helmet camera's next frame, from the head of the rider the replay camera follows
// (followed: where that rider is this frame).
void PrepareHelmet(Vec3 followed, double dt)
{
    const Config& cfg = GetConfig();
    Helmet& h = g_helmet;
    int found = -1;
    double nearest = kHeadMatch;
    for (int i = 0; i < g_headCount; ++i) {
        const double distance = Length(g_heads[i].centre - followed);
        if (distance < nearest) {
            nearest = distance;
            found = i;
        }
    }
    g_headCount = 0;
    if (cfg.replayCamera != kHelmetCamera) {
        h = Helmet{};
        return;
    }
    if (found < 0) {
        // Keep the last view a moment (nothing is carried forward), then give the game's back.
        h.haveLast = false;
        if (++h.headless >= kHeadlessFrames && h.have) {
            h.have = false;
            Log("replay: lost the rider's head, back to the game's replay camera");
        }
        return;
    }
    h.headless = 0;

    // The head: the head model, with the eye's place in it learnt from the torso, or else the
    // torso itself.
    const Head& head = g_heads[found];
    const bool sameRider = h.eyeSamples > 0 && Length(head.centre - h.lastCentre) < kMountJump;
    h.lastCentre = head.centre;
    // The chest mount rides on the torso (it turns with the body, not the head), placed from the
    // middle of the shoulders; the others on the head model.
    const bool chest = cfg.helmetMount == kChestMount;
    const bool onModel = head.model && !chest;
    Quat axes = head.rot;
    Vec3 eye = chest && head.haveShoulders ? head.shoulders : head.eye;
    if (head.model) {
        const Vec3 sample = Rotate(Conjugate(head.modelRot), head.eye - head.modelPos);
        if (!sameRider) {
            h.eyeInModel = sample;
            h.eyeSamples = 1;
        } else {
            h.eyeSamples = h.eyeSamples < kEyeSamples ? h.eyeSamples + 1 : kEyeSamples;
            h.eyeInModel = h.eyeInModel + (sample - h.eyeInModel) / h.eyeSamples;
        }
        if (onModel) {
            axes = head.modelRot;
            eye = head.modelPos + Rotate(head.modelRot, h.eyeInModel);
        }
    }
    const int anchor = chest ? 2 : (onModel ? 1 : 0);
    if (anchor != h.anchor || !h.have) {
        static const char* const kAnchors[] = { "torso (no head model found)", "helmet (its head model)", "chest" };
        Log("replay: filming from the rider's %s", kAnchors[anchor]);
    }
    h.anchor = anchor;
    h.onModel = onModel;

    // The mount, on the head's (chest mount: the torso's) axes: above, forward and (side mounts)
    // out to the side of the eyes (chest mount: the middle of the shoulders), angled down by the tilt.
    const bool side = cfg.helmetMount == 1 || cfg.helmetMount == 2;
    const Vec3 local = chest ? Vec3{ 0, cfg.chestHeight, -cfg.chestForward }
                     : side  ? Vec3{ cfg.helmetMount == 1 ? -cfg.sideOut : cfg.sideOut, cfg.sideHeight, -cfg.sideForward }
                             : Vec3{ 0, cfg.helmetHeight, -cfg.helmetForward };
    const Vec3 up = Rotate(axes, { 0, 1, 0 }), back = Rotate(axes, { 0, 0, 1 });
    const Vec3 mount = eye + Rotate(axes, local);
    const double tilt = (chest ? cfg.chestTiltDeg : side ? cfg.sideTiltDeg : cfg.helmetTiltDeg) * kDegToRad;
    const Vec3 forward = -(back * std::cos(tilt) + up * std::sin(tilt));
    const Vec3 mountUp = up * std::cos(tilt) - back * std::sin(tilt);
    Quat target = QuatFromBasis(Cross(forward, mountUp), mountUp, -forward);
    const Vec3 levelRight = Cross(forward, WorldUp());
    if (cfg.helmetLevel > 0 && Length(levelRight) > 1e-3) {  // the same direction, with the head's lean taken out
        const Vec3 right = Normalize(levelRight);
        target = Slerp(target, QuatFromBasis(right, Cross(right, forward), -forward), cfg.helmetLevel);
    }
    h.mountInModel = h.eyeInModel + local;
    h.cameraInModel = Conjugate(axes) * target;
    const Vec3 offset = ToCamera(mount - g_camera.pos);

    // Carried one frame forward at the rate it last changed, unless it jumped (a seek, or the
    // replay camera moved on to another rider).
    const bool carry = h.haveLast && Length(offset - h.offset) < kMountJump &&
                       AngleBetween(target, h.target) < kMaxStepDeg * kDegToRad;
    const Vec3 nextOffset = carry ? offset + (offset - h.offset) : offset;
    const Quat nextTarget = carry ? Normalize(target * Conjugate(h.target) * target) : target;
    const double smoothing = cfg.helmetSmoothing * kMaxSmoothing;
    if (!h.have || !carry || smoothing <= 0)
        h.rot = nextTarget;
    else
        h.rot = Slerp(h.rot, nextTarget, LowPassAlpha(1.0 / smoothing, dt));
    h.eyeOffset = nextOffset;
    h.offset = offset;
    h.target = target;
    h.haveLast = true;
    h.have = true;
}

}  // namespace

void ObserveSkeleton(const float* centres, const float* positions, const float* rotations, bool transposed, int bones)
{
    if (!g_detect.active || GetConfig().replayCamera != kHelmetCamera || !centres || !positions || !rotations ||
        bones <= 0 || bones > kMaxBones || g_headCount >= kMaxRiders)
        return;
    // The torso: the highest bone on the midline (the arms and legs are out to the sides).
    double halfSpan = 0;
    for (int b = 0; b < bones; ++b)
        halfSpan = std::fmax(halfSpan, std::fabs(double(centres[b * 3])));
    int torso = -1, midline = 0;
    for (int b = 0; b < bones; ++b) {
        if (std::fabs(centres[b * 3]) > 0.05 * halfSpan)
            continue;
        ++midline;
        if (torso < 0 || centres[b * 3 + 1] > centres[torso * 3 + 1])
            torso = b;
    }
    if (halfSpan < 1e-3 || torso < 0 || midline == bones)
        return;  // not a rider

    // The shoulders: the arm bones nearest the midline (the upper arms), above the hips.
    int left = -1, right = -1;
    for (int b = 0; b < bones; ++b) {
        const double x = centres[b * 3];
        if (std::fabs(x) <= 0.05 * halfSpan || centres[b * 3 + 1] <= centres[torso * 3 + 1] + 0.25 * halfSpan)
            continue;
        if (x < 0 && (left < 0 || x > centres[left * 3]))
            left = b;
        if (x > 0 && (right < 0 || x < centres[right * 3]))
            right = b;
    }

    // Its rotation's columns are the head's axes in world space.
    const float* r = rotations + torso * 9;
    const auto Column = [&](int c) {
        return transposed ? Vec3{ r[c], r[3 + c], r[6 + c] } : Vec3{ r[c * 3], r[c * 3 + 1], r[c * 3 + 2] };
    };
    Vec3 up = Column(1), back = Column(2);
    if (std::fabs(Length(up) - 1.0) > 0.2 || std::fabs(Length(back) - 1.0) > 0.2)
        return;  // scaled or flattened: not a plain rotation
    back = Normalize(back);
    const Vec3 side = Normalize(Cross(up, back));
    up = Cross(back, side);

    const auto Position = [&](int b) { return Vec3{ positions[b * 3], positions[b * 3 + 1], positions[b * 3 + 2] }; };
    Vec3 sum;
    for (int b = 0; b < bones; ++b)
        sum = sum + Position(b);
    const Vec3 centre = sum / bones;
    for (int i = 0; i < g_headCount; ++i) {
        if (Length(g_heads[i].centre - centre) < 1e-3) {
            g_pendingHead = i;  // the same rider again (it takes several draws)
            return;
        }
    }
    g_pendingHead = g_headCount;
    Head& head = g_heads[g_headCount++];
    head = Head{};
    head.centre = centre;
    head.eye = Position(torso) + up * (kEyeUp * halfSpan) - back * (kEyeForward * halfSpan);
    head.rot = QuatFromBasis(side, up, back);
    if (left >= 0 && right >= 0 && Length(Position(right) - Position(left)) > 1e-3) {
        head.haveShoulders = true;
        head.shoulders = (Position(left) + Position(right)) * 0.5;
        head.across = Normalize(Position(right) - Position(left));
        head.reach = kHeadReach * halfSpan;
    }
}

bool ObserveModelview(const float* m, float out[16])
{
    const int pending = g_pendingHead;
    g_pendingHead = -1;
    if (!g_detect.active || !m || pending < 0 || pending >= g_headCount)
        return false;
    Head& head = g_heads[pending];
    if (head.model || !head.haveShoulders)
        return false;  // already found (the body's later passes), or nothing to look for it by
    // The model's axes and origin in world space, through this frame's raw view.
    const Vec3 x = ToWorld({ m[0], m[1], m[2] }), y = ToWorld({ m[4], m[5], m[6] }), z = ToWorld({ m[8], m[9], m[10] });
    if (std::fabs(Length(x) - 1.0) > 0.05 || std::fabs(Length(y) - 1.0) > 0.05 || std::fabs(Length(z) - 1.0) > 0.05 ||
        Dot(Cross(x, y), z) < 0.9)
        return false;  // not a rigid model
    const Vec3 origin = g_camera.pos + ToWorld({ m[12], m[13], m[14] });
    if (Length(origin - head.shoulders) > head.reach || Dot(x, head.across) < kHeadAlign)
        return false;  // something else
    const Vec3 back = Normalize(z), right = Normalize(Cross(y, back));
    head.model = true;
    head.modelPos = origin;
    head.modelRot = QuatFromBasis(right, Cross(back, right), back);

    // The helmet camera's own rider: draw the head where the camera put it.
    const Helmet& h = g_helmet;
    if (!out || g_mode != Mode::Helmet || !h.onModel || Length(origin - h.drawPos) > kHeadDrawMatch)
        return false;
    const Vec3 R = Rotate(h.drawRot, { 1, 0, 0 }), U = Rotate(h.drawRot, { 0, 1, 0 }), B = Rotate(h.drawRot, { 0, 0, 1 });
    const Vec3 P = h.drawPos;
    const double model[16] = { R.x, R.y, R.z, 0, U.x, U.y, U.z, 0, B.x, B.y, B.z, 0, P.x, P.y, P.z, 1 };
    double modelview[16];
    MatMul(h.view, model, modelview);
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<float>(modelview[i]);
    return true;
}

void ObserveBones(const float* positions, int count)
{
    if (!g_detect.active || !positions || count <= 0 || count > kMaxBones || g_riderCount >= kMaxRiders)
        return;
    Vec3 sum;
    for (int i = 0; i < count; ++i)
        sum = sum + Vec3{ positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2] };
    g_riders[g_riderCount++] = sum / count;
}

Mode Update(const double view[16], double contentTime, double correction[16], double rotationOnly[16])
{
    Detection& d = g_detect;
    Vec3 right, up, back, pos;
    if (!Decompose(view, right, up, back, pos)) {
        Stop("not a camera");
        d.haveLast = false;
        return g_mode;
    }
    const Vec3 worldUp = WorldUp();
    const double dt = d.haveLast ? Clamp(contentTime - d.lastTime, 0.0, 0.1) : 0.0;
    const double tolerance = std::sin(kAngleTolerance * kDegToRad);
    const bool level = std::fabs(Dot(right, worldUp)) < tolerance;
    const bool replayAngle = level && std::fabs(Dot(back, worldUp) - std::sin(kReplayPitchDeg * kDegToRad)) < tolerance;
    const bool jumped = d.haveLast && Length(pos - d.lastPos) > kJumpDistance;
    if (d.active && !level)
        Stop("the horizon tilted");
    else if (d.active && jumped && !replayAngle)
        Stop("the camera jumped");
    d.signatureTime = replayAngle ? d.signatureTime + dt : 0.0;
    d.haveLast = true;
    d.lastPos  = pos;
    d.lastTime = contentTime;
    g_camera = { QuatFromBasis(right, up, back), pos, right, up, back };

    const int camera = GetConfig().replayCamera;
    if (!d.active && d.signatureTime >= kEnterSeconds) {
        d.active = true;
        g_chase = Chase{};
        g_helmet = Helmet{};
        static const char* const kNames[] = { "game's replay", "3rd person", "helmet" };
        Log("replay: detected, showing the %s camera", kNames[Clamp(camera, 0, 2)]);
    }
    if (!d.active)
        return g_mode = Mode::Off;

    // The helmet camera cuts straight in once the rider's head has been found.
    if (camera == kHelmetCamera && g_helmet.have) {
        Helmet& h = g_helmet;
        const Vec3 eye = pos + ToWorld(h.eyeOffset);
        if (!ViewCorrection(view, h.rot, eye, correction, rotationOnly, h.view))
            return g_mode = Mode::Game;
        // The head this camera sits on, rigidly: its model's pose under the (stabilised) camera.
        h.drawRot = Normalize(h.rot * Conjugate(h.cameraInModel));
        h.drawPos = eye - Rotate(h.drawRot, h.mountInModel);
        return g_mode = Mode::Helmet;
    }
    if (camera != 1 || g_chase.ease <= 0)
        return g_mode = Mode::Game;

    // Eases from the game's view to the chase view: rotation and position together.
    const Chase& c = g_chase;
    const double w = Blend();
    const Vec3 chaseEye = pos + ToWorld(c.offset) + c.eyeOffset;
    const Vec3 eye = pos + (chaseEye - pos) * w;
    const Quat q = Slerp(g_camera.rot, c.rot, w);
    return g_mode = ViewCorrection(view, q, eye, correction, rotationOnly) ? Mode::Chase : Mode::Game;
}

void EndFrame(double contentTime, bool captured)
{
    if (!captured) {
        Stop("no camera");
        g_detect.haveLast = false;
    }
    g_pendingHead = -1;
    if (!g_detect.active) {
        g_riderCount = 0;
        g_headCount = 0;
        return;
    }

    const Config& cfg = GetConfig();
    Chase& c = g_chase;
    const Vec3 worldUp = WorldUp();
    const double dt = c.lastTime >= 0 ? Clamp(contentTime - c.lastTime, 0.0, 0.1) : 0.0;
    c.lastTime = contentTime;

    // ---- Where the rider is, relative to the replay camera -------------------------------
    const Vec3 measured = MeasureOffset();
    g_riderCount = 0;
    PrepareHelmet(g_camera.pos + ToWorld(measured), dt);
    if (!c.haveRider || Length(measured - c.offset) > 3.0)
        c.offset = measured;  // first frame, or the replay camera moved on to another rider
    else
        c.offset = c.offset + (measured - c.offset) * LowPassAlpha(kOffsetRate, dt);
    c.haveRider = true;

    // ---- How the rider moves ---------------------------------------------------------------
    const Vec3 rider = g_camera.pos + ToWorld(c.offset);
    if (!c.haveLastRider || Length(rider - c.lastRider) > 5.0) {  // first frame, a seek or another rider
        c.velocity = Vec3{};
        c.snapHeading = true;
    } else if (dt > 0) {
        c.velocity = c.velocity + ((rider - c.lastRider) / dt - c.velocity) * LowPassAlpha(6.0, dt);
    }
    c.lastRider = rider;
    c.haveLastRider = true;

    Vec3 velocity = c.velocity;
    Vec3 flat = velocity - worldUp * Dot(velocity, worldUp);
    const double speed = Length(flat);
    if (speed > kMinSpeed) {
        // A bike can't turn round on the spot: the replay is being played backwards.
        if (c.haveHeading && !c.snapHeading && Dot(flat, c.heading) < -0.5 * speed) {
            velocity = -velocity;
            flat = -flat;
        }
        const Vec3 direction = flat / speed;
        const double slope = Clamp(std::atan2(Dot(velocity, worldUp), speed), -kMaxSlopeDeg * kDegToRad, kMaxSlopeDeg * kDegToRad);
        if (!c.haveHeading || c.snapHeading) {
            c.heading = direction;
            c.slope = slope;
            c.haveHeading = true;
            c.snapHeading = false;
        } else {
            const double weight = SmoothStep(kMinSpeed, 4.0, speed);
            c.heading = Normalize(c.heading + (direction - c.heading) * (LowPassAlpha(kTurnRate, dt) * weight));
            c.slope += (slope - c.slope) * (LowPassAlpha(2.0, dt) * weight);
        }
    }

    // ---- Ease in, and the next frame's view ------------------------------------------------
    // Until the rider has moved (a replay starting at the gate) the game's view stays.
    c.ease = (cfg.replayCamera == 1 && c.haveHeading) ? std::fmin(1.0, c.ease + dt / kEaseSeconds) : 0.0;
    if (c.haveHeading) {
        // Behind and above the rider, tilted with part of the slope being ridden: downhill the
        // ground behind is higher, so the camera rises with it (and pitches down to match).
        const double tilt = kSlopeFollow * c.slope;
        const Vec3 along  = c.heading * std::cos(tilt) + worldUp * std::sin(tilt);
        const Vec3 tiltUp = worldUp * std::cos(tilt) - c.heading * std::sin(tilt);
        c.eyeOffset = tiltUp * cfg.replayHeight - along * cfg.replayDistance;
        const double pitch = tilt - cfg.replayPitchDeg * kDegToRad;
        const Vec3 forward = c.heading * std::cos(pitch) + worldUp * std::sin(pitch);
        const Vec3 right = Normalize(Cross(forward, worldUp));
        c.rot = QuatFromBasis(right, Cross(right, forward), -forward);
    }
}

Mode Current() { return g_mode; }

double AngleToCameraDeg(const double view[16])
{
    Vec3 right, up, back, pos;
    if (!Decompose(view, right, up, back, pos))
        return -1;
    return AngleBetween(g_camera.rot, QuatFromBasis(right, up, back)) * kRadToDeg;
}

bool PreviewRotation(const double view[16], double out[16])
{
    Vec3 right, up, back, pos;
    if (!Corrects(g_mode) || !Decompose(view, right, up, back, pos))
        return false;
    const Quat q = g_mode == Mode::Helmet ? g_helmet.rot : Slerp(QuatFromBasis(right, up, back), g_chase.rot, Blend());
    RotationCorrection(q, right, up, back, out);
    return true;
}

const char* StatusText()
{
    if (!g_detect.active)
        return "NOT IN A REPLAY";
    switch (GetConfig().replayCamera) {
    case 1:
        return g_chase.haveHeading ? "REPLAY - 3RD PERSON CAMERA" : "REPLAY - WAITING FOR THE RIDER TO MOVE";
    case kHelmetCamera:
        return g_helmet.have ? "REPLAY - HELMET CAMERA" : "REPLAY - LOOKING FOR THE RIDER'S HEAD";
    default:
        return "REPLAY - GAME CAMERA";
    }
}

void Describe(char* text, size_t size)
{
    const Chase& c = g_chase;
    const Vec3 rider = g_camera.pos + ToWorld(c.offset);
    snprintf(text, size, "replay,%s,ease %.2f,camera %.2f %.2f %.2f,rider %.2f %.2f %.2f,heading %.1f,speed %.1f,slope %.1f",
             g_mode == Mode::Chase ? "chase" : (g_mode == Mode::Helmet ? "helmet" : "game"), c.ease, g_camera.pos.x, g_camera.pos.y, g_camera.pos.z,
             rider.x, rider.y, rider.z, std::atan2(c.heading.x, -c.heading.z) * kRadToDeg, Length(c.velocity),
             c.slope * kRadToDeg);
    if (g_mode == Mode::Helmet) {
        const size_t used = strlen(text);
        const Helmet& h = g_helmet;
        snprintf(text + used, size - used, ",on %s,eye in head model %.2f %.2f %.2f (%d frames)",
                 h.onModel ? "head model" : "torso", h.eyeInModel.x, h.eyeInModel.y, h.eyeInModel.z, h.eyeSamples);
    }
}

}  // namespace replay
}  // namespace mxcam
