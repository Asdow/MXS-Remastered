// thunder.cpp - see thunder.h.
//
//   The weather's sound: the rain (a recording of steady rain, looped: rain_sound.h), the snow's
//   storm (a recording of a blizzard's wind, looped: snow_sound.h), your tires through the snow (a
//   soft, even hush made from a sled's slide over snow, looped: snow_tires_sound.h - louder and
//   brighter the faster you ride) and the lightning's strikes. A strike: recordings of real ones (lightning_sounds.h - three, cut from
//   "Thunder Sound Effect No Copyright.mp3" at their cracks), one played for each strike as late as it
//   was far, a little different each time (pitch; and far off deeper, duller, quieter). Mixed as they
//   play, a few at once if they overlap, on a thread of its own.
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
#include "lightning_sounds.h"
#include "rain_sound.h"
#include "snow_sound.h"
#include "snow_tires_sound.h"
#include "log.h"

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
#endif

namespace mxcam {
namespace thunder {
namespace {

constexpr int    kRate    = 44100;
constexpr int    kBlock   = 512;   // samples a buffer (~12 ms)
constexpr int    kBuffers = 4;     // queued (~46 ms ahead: the tires' sound follows landings quickly)
constexpr int    kMaxPlaying = 6;
constexpr double kTwoPi   = 6.283185307179586;

std::atomic<float> g_volume{ 0.0f };      // the strikes' (Config::lightningSound, as heard)
std::atomic<float> g_rainVolume{ 0.0f };  // the rain's (Config::rainSound and how hard it rains)
std::atomic<float> g_snowVolume{ 0.0f };  // the snowstorm's (Config::snowSound, the snowfall and its wind)
std::atomic<float> g_tiresVolume{ 0.0f }; // your tires in the snow (Config::snowTires, with how you're rolling)
std::atomic<float> g_tiresBright{ 0.0f }; // and how bright (0..1: brighter riding faster)
std::atomic<bool>  g_stop{ false };
std::mutex         g_pendingLock;
struct Pending { double metres, delay; };
Pending g_pending[8];
int     g_pendingCount = 0;
uint32_t g_seedNext = 0x6C8E9CF5u;

// One-pole low-pass: a = 1 - e^(-2 pi f / rate).
double Pole(double hz) { return 1.0 - std::exp(-kTwoPi * hz / kRate); }

// ---- One strike: one of the recorded strikes (lightning_sounds.h) -------------------------------
// Each plays one of them (never the same twice in a row), each time a little higher or lower; a far
// one a little deeper still, duller (the air takes its edge off) and quieter.
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

int g_lastClip = -1;

struct Clap {
    bool        on = false;
    long long   wait = 0;      // samples until it's heard
    const Clip* clip = nullptr;
    double      at = 0;        // where in the clip (its samples)
    double      step = 0;      // clip samples a sample played (its rate against ours, and its pitch)
    double      gain = 0;
    double      dull = 1;      // the air's low-pass (a pole), and its state
    double      dull1 = 0, dull2 = 0;

    void Start(double metres, double delay, uint32_t seed)
    {
        Rng rng{ seed ? seed : 1u };
        const int count = static_cast<int>(sizeof kClips / sizeof kClips[0]);
        int pick = static_cast<int>(rng() * count) % count;
        if (pick == g_lastClip && count > 1)
            pick = (pick + 1) % count;
        g_lastClip = pick;
        const double km = std::fmax(metres, 100.0) / 1000.0;
        const double distant = std::fmin(std::fmax((km - 0.5) / 5.5, 0.0), 1.0);  // 0 close .. 1 at 6 km
        *this = Clap{};
        on = true;
        clip = &kClips[pick];
        wait = static_cast<long long>(std::fmax(delay, 0.0) * kRate);
        step = static_cast<double>(kClipRate) / kRate * rng.Range(0.93, 1.06) * (1.0 - 0.1 * distant);
        dull = Pole(std::fmin(std::fmax(9000.0 * std::pow(0.8 / km, 1.1), 1000.0), 10000.0));
        gain = std::fmin(std::fmax(1.5 / (0.5 + km), 0.25), 1.0);
    }

    double Sample()
    {
        if (wait > 0) {
            --wait;
            return 0.0;
        }
        const int i = static_cast<int>(at);
        if (!clip || i + 1 >= clip->count) {
            on = false;
            return 0.0;
        }
        const double f = at - i;
        const double v = (clip->samples[i] * (1.0 - f) + clip->samples[i + 1] * f) / 32768.0;
        at += step;
        dull1 += (v - dull1) * dull;
        dull2 += (dull1 - dull2) * dull;
        return dull2 * gain;
    }
};

// Loud moments (strikes on top of each other) squeezed softly above 0.8, never clipping.
double Limit(double s)
{
    const double a = std::fabs(s);
    return a < 0.8 ? s : std::copysign(0.8 + 0.2 * std::tanh((a - 0.8) / 0.2), s);
}

Clap g_playing[kMaxPlaying];
double g_focus = 1.0;

// ---- A looped recording: the rain's (rain_sound.h), the snowstorm's (snow_sound.h) or the snow
// under your tires (snow_tires_sound.h); each one's end blends into its start ---------------------
struct Loop {
    const int16_t* samples;
    int    count;
    int    rate;
    double ease = 0.00004;  // how fast its level follows its volume (a pole a sample: ~0.6 s; the tires' ~60 ms, a soft fade)
    double at = 0;
    double level = 0;  // following its volume (in and out smoothly, never a click)

    double Sample(double to, double pitch = 1.0)
    {
        level += (to - level) * ease;
        if (level < 1e-5 && to <= 0.0)
            return 0.0;
        const int i = static_cast<int>(at);
        const double f = at - i;
        const double v = (samples[i] * (1.0 - f) + samples[i + 1 < count ? i + 1 : 0] * f) / 32768.0;
        at += static_cast<double>(rate) / kRate * pitch;
        if (at >= count)
            at -= count;
        return v * level;
    }
};
Loop g_rain{ kRainLoop, static_cast<int>(sizeof kRainLoop / sizeof kRainLoop[0]), kRainRate };
Loop g_snow{ kSnowLoop, static_cast<int>(sizeof kSnowLoop / sizeof kSnowLoop[0]), kSnowRate };
Loop g_tires{ kSnowTiresLoop, static_cast<int>(sizeof kSnowTiresLoop / sizeof kSnowTiresLoop[0]), kSnowTiresRate, 0.0004 };

// Every volume now, as set by Update.
struct Levels { float strikes, rain, snow, tires, tiresBright; };
Levels Now()
{
    return Levels{ g_volume.load(), g_rainVolume.load(), g_snowVolume.load(), g_tiresVolume.load(), g_tiresBright.load() };
}

// The tires' brightness: a gentle low-pass over them (two poles), opening up as you ride faster -
// the brush harder, not faster. Its cutoff eased, never stepping.
double g_tiresCut = 9000.0, g_tiresLp1 = 0.0, g_tiresLp2 = 0.0;
double Brightened(double s, double bright)
{
    const double to = 9000.0 * std::pow(1.8, std::fmin(std::fmax(bright, 0.0), 1.0));  // 9 to ~16 kHz: clear, only airier faster
    g_tiresCut += (to - g_tiresCut) * 0.0005;
    const double a = Pole(g_tiresCut);
    g_tiresLp1 += (s - g_tiresLp1) * a;
    g_tiresLp2 += (g_tiresLp1 - g_tiresLp2) * a;
    return g_tiresLp2;
}

void Fill(short* out, int count, const Levels& v)
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
        s = s * v.strikes + g_rain.Sample(v.rain) + g_snow.Sample(v.snow) + Brightened(g_tires.Sample(v.tires), v.tiresBright);
        // Soft-limited: a close strike on top of the rain stays whole, never clipping hard.
        s = Limit(s) * g_focus;
        out[i] = static_cast<short>(s * 32000.0);
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
        Log("weather sound: the sound output couldn't be opened");
        if (ready)
            CloseHandle(ready);
        return 0;
    }
    Log("weather sound: sound output open (%d Hz)", kRate);
    static short samples[kBuffers][kBlock];
    WAVEHDR headers[kBuffers]{};
    for (int i = 0; i < kBuffers; ++i) {
        headers[i].lpData = reinterpret_cast<LPSTR>(samples[i]);
        headers[i].dwBufferLength = sizeof samples[i];
        waveOutPrepareHeader(device, &headers[i], sizeof headers[i]);
        Fill(samples[i], kBlock, Levels{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f });
        waveOutWrite(device, &headers[i], sizeof headers[i]);
    }
    while (!g_stop.load()) {
        WaitForSingleObject(ready, 50);
        for (int i = 0; i < kBuffers; ++i) {
            if (!(headers[i].dwFlags & WHDR_DONE))
                continue;
            Fill(samples[i], kBlock, Now());
            waveOutWrite(device, &headers[i], sizeof headers[i]);
        }
    }
    waveOutReset(device);
    for (int i = 0; i < kBuffers; ++i)
        waveOutUnprepareHeader(device, &headers[i], sizeof headers[i]);
    waveOutClose(device);
    CloseHandle(ready);
    Log("weather sound: sound output closed");
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
        Log("weather sound: the sound output couldn't be opened (ALSA)");
        if (pcm && close)
            close(pcm);
        return;
    }
    Log("weather sound: sound output open (%d Hz, ALSA)", kRate);
    static short samples[kBlock];
    while (!g_stop.load()) {
        Fill(samples, kBlock, Now());
        long n = write(pcm, samples, kBlock);
        if (n < 0 && recover)
            n = recover(pcm, static_cast<int>(n), 1);
        if (n < 0) {
            Log("weather sound: the sound output stopped (ALSA error %ld)", n);
            break;
        }
    }
    close(pcm);
    Log("weather sound: sound output closed");
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

void Update(double strikes, double rain, double snow, double tires, double rolling, double bright)
{
    if (strikes <= 0.0 && rain <= 0.0 && snow <= 0.0 && tires <= 0.0) {
        g_volume = 0.0f;
        g_rainVolume = 0.0f;
        g_snowVolume = 0.0f;
        g_tiresVolume = 0.0f;
        if (Running()) {
            StopAudio();
            for (Clap& c : g_playing)
                c.on = false;
            g_rain.level = 0.0;
            g_snow.level = 0.0;
            g_tires.level = 0.0;
        }
        return;
    }
    // The sliders as loudness is heard: their squares (100%: as loud as it goes without clipping).
    g_volume = static_cast<float>(strikes * strikes);
    g_rainVolume = static_cast<float>(rain * rain);
    g_snowVolume = static_cast<float>(snow * snow);
    g_tiresVolume = static_cast<float>(tires * tires * std::fmin(std::fmax(rolling, 0.0), 1.0));
    g_tiresBright = static_cast<float>(std::fmin(std::fmax(bright, 0.0), 1.0));
    StartAudio();
}

#ifdef MXCAM_THUNDER_TEST
// For the test (tests/thunder_test.cpp): one strike `metres` away, rendered without a device.
void RenderForTest(double metres, short* out, int count, uint32_t seed)
{
    Clap c;
    c.Start(metres, 0.0, seed);
    for (int i = 0; i < count; ++i)
        out[i] = static_cast<short>(Limit(c.Sample()) * 32000.0);
}

// The rain alone at full volume (as Fill mixes it, once faded in).
void RenderRainForTest(short* out, int count)
{
    Loop r = g_rain;
    r.at = 0.0;
    r.level = 1.0;
    for (int i = 0; i < count; ++i)
        out[i] = static_cast<short>(Limit(r.Sample(1.0)) * 32000.0);
}
#endif

}  // namespace thunder
}  // namespace mxcam
