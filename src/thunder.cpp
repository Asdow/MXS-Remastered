// thunder.cpp - see thunder.h.
//
//   The sound of a lightning strike, worked out the way it comes about: the bolt's channel, jagged,
//   each short piece of it a sudden shock to the air (an N-wave), all of them reaching you one after
//   another as their distances differ. The nearest part's arrives first - the crack - and the rest of
//   the channel's straight after, close together - the boom, and its rumble rolling on as the
//   further parts arrive. The air softens it with distance, the ground beside you and the hills
//   further off echo it. No noise is played (nothing like wind or rain): only the strike.
#include "thunder.h"

#include <windows.h>
#ifdef _WIN32
#include <mmsystem.h>
#else
#include <dlfcn.h>
#include <thread>
#endif

#include <atomic>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

#include "hooks.h"
#include "log.h"

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
#endif

namespace mxcam {
namespace thunder {
namespace {

constexpr int    kRate    = 44100;
constexpr int    kBlock   = 1024;  // samples a buffer (~23 ms)
constexpr int    kBuffers = 4;     // queued (~93 ms ahead)
constexpr int    kMaxPlaying = 6;
constexpr double kTwoPi   = 6.283185307179586;

std::atomic<float> g_volume{ 0.0f };
std::atomic<bool>  g_stop{ false };
std::mutex         g_pendingLock;
struct Pending { double metres, delay; };
Pending g_pending[8];
int     g_pendingCount = 0;
uint32_t g_seedNext = 0x6C8E9CF5u;

// One-pole low-pass: a = 1 - e^(-2 pi f / rate).
double Pole(double hz) { return 1.0 - std::exp(-kTwoPi * hz / kRate); }

// ---- The sound of one strike -------------------------------------------------------------------
// The channel: a jagged path from the cloud base down to the ground (a random walk, its pieces a few
// metres long, a few branches off it). Each piece gives off one N-wave - the pressure jumps up, falls
// steadily below zero, jumps back - loudest straight out from its side. They reach you each as late
// as its piece was far: the nearest first (the crack), then the rest of the channel in quick, dense
// succession (the boom and its roll), sharper close by, longer and softer far off (the air stretches
// them and takes their edges off). Once off the ground under you, and a couple of times off hills
// further away, quieter and duller. Rendered whole when it's struck (well ahead of when it's heard).
constexpr double kSound = 343.0;  // m/s

struct V3 { double x, y, z; };
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double Len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V3 Unit(V3 a) { const double l = Len(a); return l > 1e-9 ? a * (1.0 / l) : V3{ 0, 0, -1 }; }

struct Rng {
    uint32_t s;
    double operator()()  // 0..1
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (s & 0xFFFFFF) / 16777216.0;
    }
    double Range(double a, double b) { return a + (b - a) * (*this)(); }
};

struct Piece { V3 mid, dir; double length, weight; };

// Down from `from` toward `to` in steps, wandering (each step's direction off the straight way by a
// random tilt, as a real channel's is, about 16 degrees on average).
void Walk(V3 from, V3 to, double weight, bool branches, Rng& rng, std::vector<Piece>& out)
{
    V3 p = from;
    to.z = std::fmax(to.z, 0.0);  // (never below the ground)
    for (int n = 0; n < 1500 && p.z > to.z + 1.0; ++n) {
        const V3 aim = Unit(to - p);
        const V3 dir = Unit(aim + V3{ rng.Range(-0.42, 0.42), rng.Range(-0.42, 0.42), rng.Range(-0.3, 0.3) });
        const double step = rng.Range(4.0, 14.0);
        V3 q = p + dir * step;
        if (q.z < to.z)
            q.z = to.z;
        out.push_back(Piece{ (p + q) * 0.5, Unit(q - p), Len(q - p), weight });
        if (branches && rng() < 0.012 && p.z > 150.0) {
            const double out1 = rng.Range(0.0, 6.2831853), reach = rng.Range(150.0, 500.0);
            Walk(p, p + V3{ std::cos(out1) * reach, std::sin(out1) * reach, -rng.Range(0.6, 1.2) * reach }, weight * 0.45,
                 false, rng, out);
        }
        p = q;
    }
}

// Two one-pole high-passes in a row: below `hz` out (what speakers can't play only muddies it).
void HighPass(std::vector<float>& x, double hz)
{
    const double a = Pole(hz);
    double s1 = 0, s2 = 0;
    for (float& v : x) {
        s1 += (v - s1) * a;
        const double h1 = v - s1;
        s2 += (h1 - s2) * a;
        v = static_cast<float>(h1 - s2);
    }
}

// Two one-pole low-passes in a row over the buffer.
void LowPass(std::vector<float>& x, double hz)
{
    const double a = Pole(hz);
    double s1 = 0, s2 = 0;
    for (float& v : x) {
        s1 += (v - s1) * a;
        s2 += (s1 - s2) * a;
        v = static_cast<float>(s2);
    }
}

void Render(double metres, uint32_t seed, std::vector<float>& out)
{
    Rng rng{ seed ? seed : 1u };
    const double D = std::fmax(metres, 300.0);
    const double base = rng.Range(1100.0, 1600.0);
    const V3 ear{ 0.0, 0.0, 1.6 }, earImage{ 0.0, 0.0, -1.6 };  // (and as the ground under you mirrors it)
    // The channel: from the cloud base above (a little aside) down to the ground D away; a short run
    // inside the cloud at its top.
    const double side = rng.Range(-0.15, 0.15) * D;
    const V3 ground{ D, rng.Range(-0.05, 0.05) * D, 0.0 };
    const V3 top{ D + rng.Range(-0.2, 0.2) * base, side, base };
    std::vector<Piece> pieces;
    pieces.reserve(2000);
    Walk(top, ground, 1.0, true, rng, pieces);
    {
        const double turn = rng.Range(0.0, 6.2831853), reach = rng.Range(300.0, 900.0);
        const V3 end = top + V3{ std::cos(turn) * reach, std::sin(turn) * reach, 0.0 };
        V3 p = top;
        for (int n = 0; n < 200 && Len(end - p) > 15.0; ++n) {
            const V3 dir = Unit(Unit(end - p) + V3{ rng.Range(-0.5, 0.5), rng.Range(-0.5, 0.5), rng.Range(-0.25, 0.25) });
            const V3 q = p + dir * rng.Range(5.0, 15.0);
            pieces.push_back(Piece{ (p + q) * 0.5, dir, Len(q - p), 0.3 });
            p = q;
        }
    }
    double nearest = 1e30, furthest = 0;
    for (const Piece& c : pieces) {
        nearest = std::fmin(nearest, Len(c.mid - ear));
        furthest = std::fmax(furthest, Len(c.mid - ear));
    }
    const double echoMax = 1.6;
    out.assign(static_cast<size_t>(((furthest - nearest) / kSound + echoMax + 0.6) * kRate), 0.0f);
    std::vector<float> dry(out.size(), 0.0f);
    // Each piece's N-wave (and its reflection off the ground beside you).
    for (const Piece& c : pieces) {
        for (int image = 0; image < 2; ++image) {
            const V3 to = (image ? earImage : ear) - c.mid;
            const double r = Len(to);
            const V3 n = to * (1.0 / r);
            const double along = c.dir.x * n.x + c.dir.y * n.y + c.dir.z * n.z;
            const double across = 1.0 - along * along;  // sin^2 of the angle off the piece
            const double amp = c.weight * c.length / r * (0.15 + 0.85 * across * across) * (image ? 0.6 : 1.0);
            const double half = 0.0016 + 0.0032 * std::sqrt(r / 1000.0);  // s: the N-wave's half length
            const double edge = std::fmin(0.0004 + 0.0012 * r / 1000.0, 0.45 * half);  // its jumps' rise
            const double at = (r - nearest) / kSound + 0.05;
            const int first = static_cast<int>(at * kRate);
            const int count = static_cast<int>((2.0 * half + 2.0 * edge) * kRate) + 2;
            for (int i = 0; i < count && first + i < static_cast<int>(dry.size()); ++i) {
                const double t = i / static_cast<double>(kRate) - edge;  // 0 .. 2 half: the N-wave itself
                double p;
                if (t < 0.0)
                    p = (t + edge) / edge;                      // the jump up
                else if (t < 2.0 * half)
                    p = 1.0 - t / half;                         // down through zero
                else
                    p = -1.0 + (t - 2.0 * half) / edge;         // the jump back
                dry[first + i] += static_cast<float>(amp * p);
            }
        }
    }
    // The air: the further, the duller.
    LowPass(dry, std::fmin(std::fmax(5000.0 * std::pow(1000.0 / D, 0.8), 350.0), 6000.0));
    // Echoes off hills: later, quieter, duller still.
    std::vector<float> dull = dry;
    LowPass(dull, 220.0);
    out = dry;
    const int echoes = 2 + static_cast<int>(rng() * 2.0);
    for (int e = 0; e < echoes; ++e) {
        const int lag = static_cast<int>(rng.Range(0.35, echoMax) * kRate);
        const float gain = static_cast<float>(rng.Range(0.12, 0.25));
        for (size_t i = 0; i + lag < out.size(); ++i)
            out[i + lag] += dull[i] * gain;
    }
    HighPass(out, 45.0);
    // Scaled so the loudest moment is 1 (how loud it's played: its distance, in Clap), the crack's
    // very top squeezed so the boom after it carries.
    float peak = 1e-9f;
    for (float v : out)
        peak = std::fmax(peak, std::fabs(v));
    const double squeeze = 2.5, squeezeTop = std::tanh(squeeze);
    for (float& v : out)
        v = static_cast<float>(std::tanh(squeeze * v / peak) / squeezeTop);
    // A fade at the very end, never a click.
    const size_t fade = std::min<size_t>(out.size(), kRate / 2);
    for (size_t i = 0; i < fade; ++i)
        out[out.size() - 1 - i] *= static_cast<float>(i) / fade;
}

struct Clap {
    bool      on = false;
    long long wait = 0;          // samples until it's heard
    size_t    at = 0;
    double    gain = 0;
    std::vector<float> sound;

    void Start(double metres, double delay, uint32_t seed)
    {
        Render(metres, seed, sound);
        on = true;
        at = 0;
        wait = static_cast<long long>(std::fmax(delay, 0.0) * kRate);
        const double km = std::fmax(metres, 100.0) / 1000.0;
        gain = std::fmin(std::fmax(1.5 / (0.5 + km), 0.25), 1.0);
    }

    double Sample()
    {
        if (wait > 0) {
            --wait;
            return 0.0;
        }
        if (at >= sound.size()) {
            on = false;
            return 0.0;
        }
        return sound[at++] * gain;
    }
};

Clap g_playing[kMaxPlaying];
double g_focus = 1.0;

void Fill(short* out, int count, float volume)
{
    {
        std::lock_guard<std::mutex> lock(g_pendingLock);
        for (int i = 0; i < g_pendingCount; ++i) {
            int slot = 0;
            for (int k = 0; k < kMaxPlaying; ++k)
                if (!g_playing[k].on) {
                    slot = k;
                    break;
                } else if (g_playing[k].at > g_playing[slot].at) {
                    slot = k;  // (all busy: the oldest makes way)
                }
            g_seedNext = g_seedNext * 1664525u + 1013904223u;
            g_playing[slot].Start(g_pending[i].metres, g_pending[i].delay, g_seedNext);
        }
        g_pendingCount = 0;
    }
    const double focusTo = GameHasFocus() ? 1.0 : 0.0;
    for (int i = 0; i < count; ++i) {
        g_focus += (focusTo - g_focus) * 0.0002;
        double s = 0.0;
        for (Clap& c : g_playing)
            if (c.on)
                s += c.Sample();
        // Soft-limited: a close strike on top of another rumble stays whole, never clipping hard.
        s = std::tanh(s * 1.1) * volume * g_focus;
        out[i] = static_cast<short>(s * 30000.0);
    }
}

#ifdef _WIN32
HANDLE g_thread = nullptr;

DWORD WINAPI AudioThread(void*)
{
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = kRate * 2;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT device = nullptr;
    if (!ready || waveOutOpen(&device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(ready), 0, CALLBACK_EVENT) !=
                      MMSYSERR_NOERROR) {
        Log("thunder: the sound output couldn't be opened");
        if (ready)
            CloseHandle(ready);
        return 0;
    }
    Log("thunder: sound output open (%d Hz)", kRate);
    static short samples[kBuffers][kBlock];
    WAVEHDR headers[kBuffers]{};
    for (int i = 0; i < kBuffers; ++i) {
        headers[i].lpData = reinterpret_cast<LPSTR>(samples[i]);
        headers[i].dwBufferLength = sizeof samples[i];
        waveOutPrepareHeader(device, &headers[i], sizeof headers[i]);
        Fill(samples[i], kBlock, 0.0f);
        waveOutWrite(device, &headers[i], sizeof headers[i]);
    }
    while (!g_stop.load()) {
        WaitForSingleObject(ready, 50);
        for (int i = 0; i < kBuffers; ++i) {
            if (!(headers[i].dwFlags & WHDR_DONE))
                continue;
            Fill(samples[i], kBlock, g_volume.load());
            waveOutWrite(device, &headers[i], sizeof headers[i]);
        }
    }
    waveOutReset(device);
    for (int i = 0; i < kBuffers; ++i)
        waveOutUnprepareHeader(device, &headers[i], sizeof headers[i]);
    waveOutClose(device);
    CloseHandle(ready);
    Log("thunder: sound output closed");
    return 0;
}

bool Running() { return g_thread != nullptr; }

void StartAudio()
{
    if (g_thread)
        return;
    g_stop = false;
    g_thread = CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);
}

void StopAudio()
{
    if (!g_thread)
        return;
    g_stop = true;
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}

#else
// Linux: the same through ALSA (as the brake squeal's), ALSA pacing the thread.
void AudioThread()
{
    using OpenFn = int (*)(void**, const char*, int, int);
    using SetParamsFn = int (*)(void*, int, int, unsigned, unsigned, int, unsigned);
    using WriteFn = long (*)(void*, const void*, unsigned long);
    using RecoverFn = int (*)(void*, int, int);
    using CloseFn = int (*)(void*);
    void* lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_NOLOAD);
    if (!lib)
        lib = dlopen("libasound.so.2", RTLD_NOW);
    const auto open = lib ? reinterpret_cast<OpenFn>(dlsym(lib, "snd_pcm_open")) : nullptr;
    const auto setParams = lib ? reinterpret_cast<SetParamsFn>(dlsym(lib, "snd_pcm_set_params")) : nullptr;
    const auto write = lib ? reinterpret_cast<WriteFn>(dlsym(lib, "snd_pcm_writei")) : nullptr;
    const auto recover = lib ? reinterpret_cast<RecoverFn>(dlsym(lib, "snd_pcm_recover")) : nullptr;
    const auto close = lib ? reinterpret_cast<CloseFn>(dlsym(lib, "snd_pcm_close")) : nullptr;
    void* pcm = nullptr;
    if (!open || !setParams || !write || !close || open(&pcm, "default", 0, 0) < 0 ||
        setParams(pcm, 2, 3, 1, kRate, 1, kBuffers * kBlock * 1000000u / kRate) < 0) {
        Log("thunder: the sound output couldn't be opened (ALSA)");
        if (pcm && close)
            close(pcm);
        return;
    }
    Log("thunder: sound output open (%d Hz, ALSA)", kRate);
    static short samples[kBlock];
    while (!g_stop.load()) {
        Fill(samples, kBlock, g_volume.load());
        long n = write(pcm, samples, kBlock);
        if (n < 0 && recover)
            n = recover(pcm, static_cast<int>(n), 1);
        if (n < 0) {
            Log("thunder: the sound output stopped (ALSA error %ld)", n);
            break;
        }
    }
    close(pcm);
    Log("thunder: sound output closed");
}

std::thread g_audio;

bool Running() { return g_audio.joinable(); }

void StartAudio()
{
    if (g_audio.joinable())
        return;
    g_stop = false;
    g_audio = std::thread(AudioThread);
}

void StopAudio()
{
    if (!g_audio.joinable())
        return;
    g_stop = true;
    g_audio.join();
}
#endif

}  // namespace

void Strike(double metres, double delay)
{
    if (!Running())
        return;
    std::lock_guard<std::mutex> lock(g_pendingLock);
    if (g_pendingCount < static_cast<int>(sizeof g_pending / sizeof g_pending[0]))
        g_pending[g_pendingCount++] = Pending{ metres, delay };
}

void Update(bool on, double volume)
{
    if (!on || volume <= 0.0) {
        g_volume = 0.0f;
        if (Running()) {
            StopAudio();
            for (Clap& c : g_playing)
                c.on = false;
        }
        return;
    }
    // The slider as loudness is heard: its square (100%: as loud as it goes without clipping).
    g_volume = static_cast<float>(volume * volume);
    StartAudio();
}

#ifdef MXCAM_THUNDER_TEST
// For the test (tests/thunder_test.cpp): one strike `metres` away, rendered without a device.
void RenderForTest(double metres, short* out, int count, uint32_t seed)
{
    Clap c;
    c.Start(metres, 0.0, seed);
    for (int i = 0; i < count; ++i)
        out[i] = static_cast<short>(std::tanh(c.Sample() * 1.1) * 30000.0);
}
#endif

}  // namespace thunder
}  // namespace mxcam
