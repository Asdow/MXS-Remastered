// lobby_wheels.cpp
//   See lobby_wheels.h. Each frame's rigid draws are grouped by their mesh; each copy of a mesh is
//   matched to the nearest copy of the same mesh the frame before, and the turn between the two is
//   measured in the copy's own axes. A wheel turns about the same line of its own (its axle) every
//   frame it rolls, by up to a radian a frame; scenery doesn't turn at all, and a fork or a swingarm
//   only swings a little, about axes that keep changing. A mesh whose copies have done that steadily
//   is taken for a wheel (and stays one); its vertices give its axle's centre and its tire's radius,
//   so each copy's lowest point - its contact - is known, and its path is handed on as segments.
#include "lobby_wheels.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "hooks.h"
#include "log.h"

namespace mxcam {
namespace lobby {
namespace {

constexpr int    kMaxCopies     = 96;     // a mesh drawn more often than this a frame is scenery
constexpr double kMatchRange    = 4.0;    // game units a copy moves between frames, at most (~1.2 m)
constexpr double kSpinMin       = 0.03;   // rad a frame below which a copy isn't turning
constexpr double kSpinMax       = 1.5;    // ...and above which it's no wheel's roll (or a mismatch)
constexpr double kStatsSeconds  = 10.0;   // the turning statistics forget over this long
constexpr double kWheelTurning  = 0.5;    // a wheel: its copies turning at least this share of the frames,
constexpr double kWheelSameAxis = 0.95;   // ...about the same axis of their own this steadily,
constexpr double kWheelSamples  = 120.0;  // ...over at least this many turns (weighted, see kStatsSeconds)
constexpr double kReach         = 330.0;  // game units (~100 m) from the camera: wheels further off are left out
constexpr size_t kMaxSegments   = 4096;   // held for the marks, at most (oldest dropped)
constexpr int    kLogLinesMax   = 24;
// In the air, from how the contact moves (not from the ground): its height over the last
// kAirWindow fitted with a parabola; falling faster than kAirFall of the game's gravity for
// kAirOnFrames frames on end is a flight, slower than kAirLand for kAirOffFrames frames a landing.
// (The terrain the mod captures lies half a metre and more under the riding surface in berms: every
// test against it read wheels leant over in corners as in the air, and cut the rut apex to exit.)
constexpr double kGravityUnits  = 9.81 * 3.2808;  // the game's gravity, game units (feet) a second squared
constexpr double kAirWindow     = 0.2;    // s
constexpr double kAirFall       = 0.6;
constexpr double kAirLand       = 0.3;
constexpr int    kAirOnFrames   = 2;
constexpr int    kAirOffFrames  = 3;
constexpr int    kHistory       = 48;     // contact heights kept (covers kAirWindow up to 240 fps)

struct Copy {
    double m[16];                         // its model in the world (column-major)
    bool   haveContact = false;
    double contact[3] = {};
    // Its contact's recent heights (and times), carried from the copy it was matched to:
    int    samples = 0;
    double height[kHistory] = {}, time[kHistory] = {};
    bool   air = false;
    int    toward = 0;                    // frames on end pointing the other way (the hysteresis)
    unsigned id = 0;                      // which wheel: carried on from the copy it was matched to
};
unsigned g_nextId = 1;

// The contact's vertical acceleration over the last kAirWindow (a least-squares parabola through
// its heights), game units a second squared; false with too few samples.
bool FittedFall(const Copy& c, double now, double& accel)
{
    double s[5] = {}, r[3] = {};
    int n = 0;
    for (int k = 0; k < c.samples; ++k) {
        const double t = c.time[k] - now;
        if (t < -kAirWindow)
            continue;
        const double y = c.height[k];
        double p = 1;
        for (int e = 0; e < 5; ++e, p *= t)
            s[e] += p;
        r[0] += y;
        r[1] += y * t;
        r[2] += y * t * t;
        ++n;
    }
    if (n < 5)
        return false;
    const double m[3][3] = { { s[0], s[1], s[2] }, { s[1], s[2], s[3] }, { s[2], s[3], s[4] } };
    const auto Det = [](const double a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    };
    const double det = Det(m);
    if (std::fabs(det) < 1e-18)
        return false;
    const double mc[3][3] = { { s[0], s[1], r[0] }, { s[1], s[2], r[1] }, { s[2], s[3], r[2] } };
    accel = 2.0 * Det(mc) / det;
    return true;
}

struct Mesh {
    unsigned program = 0;
    int      count = 0;
    std::vector<Copy> now, last;          // this frame's copies (world at FrameEnd) and last frame's
    std::vector<float> raws;              // this frame's View * Model, 16 a copy
    // Turning, weighted to the last kStatsSeconds or so:
    double   matched = 0, turning = 0;
    double   axis[3] = {};                // the turning copies' axes in their own space (sign folded)
    bool     wheel = false;               // taken for a wheel (for good)
    // Its shape, once it's a wheel (its own model's space):
    int      shape = 0;                   // 0 not asked, 1 asked, 2 known, -1 couldn't be read
    double   centre[3] = {}, axle[3] = {}, radius = 0;
};
std::unordered_map<uint64_t, Mesh> g_meshes;
std::vector<Segment> g_segments;
double g_lastTime = -1;
int    g_logLines = 0;

uint64_t KeyOf(unsigned program, unsigned vao, unsigned elementBuffer, const DrawCall& d)
{
    uint64_t h = 1469598103934665603ull;
    const auto Mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    Mix(program);
    Mix(vao);
    Mix(elementBuffer);
    Mix(static_cast<uint64_t>(d.kind));
    Mix(static_cast<uint64_t>(d.count));
    Mix(static_cast<uint64_t>(d.first));
    Mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.indices)));
    return h;
}

bool Enabled()
{
    const Config& cfg = GetConfig();
    return (cfg.marksEveryBike && (cfg.tireMarks > 0.0 || cfg.tireDirt > 0.0)) ||
           (cfg.headlightEveryBike && cfg.headlight > 0.0 && cfg.timeOfDay >= 0.0);
}

void Invert(const double v[16], double out[16])  // a rigid view (rotation + translation), maybe scaled
{
    const double s2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    const double k = s2 > 1e-12 ? 1.0 / s2 : 1.0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[c * 4 + r] = v[r * 4 + c] * k;
    for (int r = 0; r < 3; ++r)
        out[12 + r] = -(out[r] * v[12] + out[4 + r] * v[13] + out[8 + r] * v[14]);
    out[3] = out[7] = out[11] = 0;
    out[15] = 1;
}

void Mul(const double a[16], const double b[16], double out[16])
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

// The turn from copy a to copy b, in a's own axes: its angle (rad) and axis (unit, sign folded so
// the largest component is positive). False if they're not rotations alike in scale.
bool TurnBetween(const double a[16], const double b[16], double& angle, double axis[3])
{
    double A[9], B[9];
    for (int c = 0; c < 3; ++c) {
        double la = 0, lb = 0;
        for (int r = 0; r < 3; ++r) {
            la += a[c * 4 + r] * a[c * 4 + r];
            lb += b[c * 4 + r] * b[c * 4 + r];
        }
        la = std::sqrt(la);
        lb = std::sqrt(lb);
        if (la < 1e-9 || lb < 1e-9 || std::fabs(la / lb - 1.0) > 0.05)
            return false;
        for (int r = 0; r < 3; ++r) {
            A[c * 3 + r] = a[c * 4 + r] / la;
            B[c * 3 + r] = b[c * 4 + r] / lb;
        }
    }
    double R[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            R[i][j] = A[i * 3] * B[j * 3] + A[i * 3 + 1] * B[j * 3 + 1] + A[i * 3 + 2] * B[j * 3 + 2];
    const double cosine = std::fmax(-1.0, std::fmin(1.0, (R[0][0] + R[1][1] + R[2][2] - 1.0) * 0.5));
    angle = std::acos(cosine);
    double x = R[2][1] - R[1][2], y = R[0][2] - R[2][0], z = R[1][0] - R[0][1];
    const double len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-9) {
        axis[0] = axis[1] = axis[2] = 0;
        return true;
    }
    x /= len;
    y /= len;
    z /= len;
    const double big = std::fabs(x) >= std::fabs(y) && std::fabs(x) >= std::fabs(z) ? x : std::fabs(y) >= std::fabs(z) ? y : z;
    const double sign = big < 0 ? -1.0 : 1.0;
    axis[0] = x * sign;
    axis[1] = y * sign;
    axis[2] = z * sign;
    return true;
}

// A copy's contact with the ground: the lowest point of its tire, straight below its axle's centre
// (across the axle) by the tire's radius.
bool ContactOf(const Mesh& mesh, const double m[16], const double up[3], double out[3])
{
    double c[3], a[3];
    for (int r = 0; r < 3; ++r) {
        c[r] = m[r] * mesh.centre[0] + m[4 + r] * mesh.centre[1] + m[8 + r] * mesh.centre[2] + m[12 + r];
        a[r] = m[r] * mesh.axle[0] + m[4 + r] * mesh.axle[1] + m[8 + r] * mesh.axle[2];
    }
    const double scale = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (scale < 1e-9)
        return false;
    for (double& v : a)
        v /= scale;
    const double along = a[0] * up[0] + a[1] * up[1] + a[2] * up[2];
    double down[3] = { -(up[0] - a[0] * along), -(up[1] - a[1] * along), -(up[2] - a[2] * along) };
    const double len = std::sqrt(down[0] * down[0] + down[1] * down[1] + down[2] * down[2]);
    if (len < 0.3)  // (the wheel on its side: no contact to speak of)
        return false;
    for (int r = 0; r < 3; ++r)
        out[r] = c[r] + down[r] / len * mesh.radius * scale;
    return true;
}

}  // namespace

bool NoteRigidDraw(unsigned program, unsigned vao, unsigned elementBuffer, const DrawCall& draw, const float raw[16])
{
    if (!Enabled())
        return false;
    Mesh& m = g_meshes[KeyOf(program, vao, elementBuffer, draw)];
    if (m.raws.size() >= 16 * kMaxCopies)
        return false;
    m.program = program;
    m.count = draw.count;
    m.raws.insert(m.raws.end(), raw, raw + 16);
    if (m.wheel && m.shape == 0) {
        m.shape = 1;
        return true;
    }
    return false;
}

bool IsWheel(unsigned program, unsigned vao, unsigned elementBuffer, const DrawCall& draw)
{
    if (!Enabled())
        return false;
    const auto found = g_meshes.find(KeyOf(program, vao, elementBuffer, draw));
    return found != g_meshes.end() && found->second.wheel;
}

bool KnowsWheels()
{
    if (!Enabled())
        return false;
    for (const auto& kv : g_meshes)
        if (kv.second.wheel && kv.second.shape == 2)
            return true;
    return false;
}

void GiveBounds(unsigned program, unsigned vao, unsigned elementBuffer, const DrawCall& draw, bool ok,
                const float lo[3], const float hi[3])
{
    const auto found = g_meshes.find(KeyOf(program, vao, elementBuffer, draw));
    if (found == g_meshes.end())
        return;
    Mesh& m = found->second;
    double n = std::sqrt(m.axis[0] * m.axis[0] + m.axis[1] * m.axis[1] + m.axis[2] * m.axis[2]);
    if (!ok || n < 1e-9) {
        m.shape = -1;
        if (g_logLines++ < kLogLinesMax)
            Log("lobby wheels: program %u, %d indices: a wheel, but its vertices couldn't be read (no marks from it)", m.program, m.count);
        return;
    }
    for (int k = 0; k < 3; ++k) {
        m.axle[k] = m.axis[k] / n;
        m.centre[k] = 0.5 * (static_cast<double>(lo[k]) + hi[k]);
    }
    // The tire's radius: half its extent across the axle (the largest, measured along the model's
    // own axes, weighted by how far each lies from the axle).
    double radius = 0;
    for (int k = 0; k < 3; ++k) {
        const double across = std::sqrt(std::fmax(0.0, 1.0 - m.axle[k] * m.axle[k]));
        radius = std::fmax(radius, 0.5 * (static_cast<double>(hi[k]) - lo[k]) * across);
    }
    m.radius = radius;
    m.shape = radius > 1e-6 ? 2 : -1;
    if (g_logLines++ < kLogLinesMax)
        Log("lobby wheels: program %u, %d indices is a wheel: axle along its own (%.2f, %.2f, %.2f), tire radius %.3f units; "
            "its copies leave tire marks", m.program, m.count, m.axle[0], m.axle[1], m.axle[2], m.radius);
}

void FrameEnd(const double* rawView, double now)
{
    if (!Enabled()) {
        if (!g_meshes.empty()) {
            g_meshes.clear();
            g_segments.clear();
        }
        g_lastTime = -1;
        return;
    }
    const double dt = g_lastTime < 0 ? 0.0 : std::fmax(0.0, std::fmin(0.25, now - g_lastTime));
    g_lastTime = now;
    const double forget = std::exp(-dt / kStatsSeconds);
    double toWorld[16];
    const bool haveView = rawView != nullptr;
    if (haveView)
        Invert(rawView, toWorld);
    const double* up = GetConfig().worldUp;
    for (auto& kv : g_meshes) {
        Mesh& m = kv.second;
        m.last.swap(m.now);
        m.now.clear();
        m.matched *= forget;
        m.turning *= forget;
        if (!m.wheel)
            for (double& v : m.axis)
                v *= forget;
        const int copies = static_cast<int>(m.raws.size() / 16);
        if (!haveView || copies == 0 || copies >= kMaxCopies) {
            m.raws.clear();
            m.last.clear();
            continue;
        }
        for (int i = 0; i < copies; ++i) {
            double mv[16];
            for (int k = 0; k < 16; ++k)
                mv[k] = m.raws[static_cast<size_t>(i) * 16 + k];
            Copy c;
            Mul(toWorld, mv, c.m);
            const double dx = c.m[12] - toWorld[12], dy = c.m[13] - toWorld[13], dz = c.m[14] - toWorld[14];
            if (m.shape == 2 && dx * dx + dy * dy + dz * dz < kReach * kReach)
                c.haveContact = ContactOf(m, c.m, up, c.contact);
            m.now.push_back(c);
        }
        m.raws.clear();
        // Each copy against the nearest copy of the frame before.
        for (Copy& c : m.now) {
            const Copy* best = nullptr;
            double bestD = kMatchRange * kMatchRange;
            for (const Copy& p : m.last) {
                const double dx = c.m[12] - p.m[12], dy = c.m[13] - p.m[13], dz = c.m[14] - p.m[14];
                const double d = dx * dx + dy * dy + dz * dz;
                if (d < bestD) {
                    bestD = d;
                    best = &p;
                }
            }
            c.id = best ? best->id : g_nextId++;
            // Its contact's height history, carried on from the copy it was, and whether it's in the
            // air (see kAirFall).
            if (c.haveContact) {
                if (best && best->haveContact) {
                    c.samples = std::min(best->samples, kHistory - 1);
                    const int drop = best->samples - c.samples;  // (the oldest, if it was full)
                    std::copy(best->height + drop, best->height + best->samples, c.height);
                    std::copy(best->time + drop, best->time + best->samples, c.time);
                    c.air = best->air;
                    c.toward = best->toward;
                }
                c.height[c.samples] = c.contact[0] * up[0] + c.contact[1] * up[1] + c.contact[2] * up[2];
                c.time[c.samples] = now;
                ++c.samples;
                double accel;
                if (FittedFall(c, now, accel)) {
                    const bool other = c.air ? accel > -kAirLand * kGravityUnits : accel < -kAirFall * kGravityUnits;
                    c.toward = other ? c.toward + 1 : 0;
                    if (c.toward >= (c.air ? kAirOffFrames : kAirOnFrames)) {
                        c.air = !c.air;
                        c.toward = 0;
                    }
                }
            }
            if (!best)
                continue;
            // Its path over the ground, for the marks.
            if (c.haveContact && best->haveContact) {
                if (g_segments.size() >= kMaxSegments)
                    g_segments.erase(g_segments.begin(), g_segments.begin() + kMaxSegments / 4);
                Segment s;
                for (int k = 0; k < 3; ++k) {
                    s.a[k] = best->contact[k];
                    s.b[k] = c.contact[k];
                }
                s.radius = m.radius;
                s.grounded = !best->air && !c.air;
                s.id = c.id;
                g_segments.push_back(s);
            }
            if (m.wheel)
                continue;  // (its statistics have done their job)
            m.matched += 1;
            double angle, axis[3];
            if (!TurnBetween(best->m, c.m, angle, axis) || angle < kSpinMin || angle > kSpinMax)
                continue;
            m.turning += 1;
            for (int k = 0; k < 3; ++k)
                m.axis[k] += axis[k];
        }
        if (!m.wheel && m.turning >= kWheelSamples) {
            const double n = std::sqrt(m.axis[0] * m.axis[0] + m.axis[1] * m.axis[1] + m.axis[2] * m.axis[2]);
            if (m.turning >= kWheelTurning * m.matched && n >= kWheelSameAxis * m.turning)
                m.wheel = true;  // (its shape is asked for at its next draw)
        }
    }
}

void TakeSegments(std::vector<Segment>& out)
{
    out.clear();
    out.swap(g_segments);
}

void CurrentWheels(const double rawView[16], std::vector<WheelNow>& out)
{
    out.clear();
    if (!Enabled() || !rawView)
        return;
    double toWorld[16];
    Invert(rawView, toWorld);
    for (const auto& kv : g_meshes) {
        const Mesh& m = kv.second;
        if (!m.wheel || m.shape != 2)
            continue;
        const size_t copies = m.raws.size() / 16;
        for (size_t i = 0; i < copies; ++i) {
            double mv[16], w[16];
            for (int k = 0; k < 16; ++k)
                mv[k] = m.raws[i * 16 + k];
            Mul(toWorld, mv, w);
            WheelNow n;
            double scale = 0;
            for (int r = 0; r < 3; ++r) {
                n.centre[r] = w[r] * m.centre[0] + w[4 + r] * m.centre[1] + w[8 + r] * m.centre[2] + w[12 + r];
                n.axle[r] = w[r] * m.axle[0] + w[4 + r] * m.axle[1] + w[8 + r] * m.axle[2];
                scale += n.axle[r] * n.axle[r];
            }
            scale = std::sqrt(scale);
            if (scale < 1e-9)
                continue;
            for (double& v : n.axle)
                v /= scale;
            n.radius = m.radius * scale;
            n.mesh = kv.first;
            out.push_back(n);
        }
    }
}

}  // namespace lobby
}  // namespace mxcam
