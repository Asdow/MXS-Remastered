"""The 450 four-stroke engine sound pack, from a sound mod's recordings (the YZ450 mod).

A sound mod made for another game: steady recordings of the engine at a few RPMs, which that game
pitch-shifts and cross-fades across the rev range (engine.scl): an idle, on the throttle low / mid /
mid-high / high, off it low / mid / high, and the rev limiter. MX Simulator plays an engine from a
soundtable instead (see make_engine_sounds.py): a row of recordings for each of a set of RPMs, the
game shifting each only between its neighbours. So each recording is measured (its RPM, from how
often the engine fires: a four-stroke single fires once every two revolutions) and, for every row
of the stock 450 four-stroke's soundtable, the recording nearest that row's RPM is resampled to it.

The methods are the 250F's (make_250f.py), which this leaves alone - copied rather than shared, so
tuning one pack can never change the other:
  - idle rows: a seamless loop of whole engine cycles, cut where the recording loops best;
  - every row above: every steady cycle of the recording, each cut at an upward zero crossing at
    the same point of the firing, so any cycle of any cell follows any other without a pop (the game
    changes its mix of cells from one cycle to the next);
  - the limiter: the cycles cut all together, as even as the limiter's own beat (cut one at a time
    they came out uneven, and stretched to one length the pitch jerked: cutouts).

Off the throttle: the mod's own off-throttle recordings (from a YZ250F, as the mod has them) where
they're nearest; below them the idle, quieter and with its top end softened.

    python make_450f.py "C:\\Games\\MX Simulator" "V:\\mx sim sound mods\\4t 450s\\YZ450" [out_dir]

Writes <out_dir>/MXS_REMASTERED_engine_default450f.pack (installed from the mod's panel like the others) and
preview_default450f.wav: the sound mod's own recordings, then each row on the throttle and off it.
Needs soundfile (pip install soundfile).
"""
import glob
import os
import sys
import wave

import numpy as np
from scipy import signal

import saf

RATE = 44100
ENGINE = 'default450f'
LOOP_CYCLES = 48      # engine cycles in each idle loop (cut from the recording where it loops best)
OFF_GAIN = 0.5        # off the throttle from the idle: this much quieter (-6 dB) ...
OFF_TOP_HZ = 3000.0   # ... and its top end softened above this
REAL_OFF_GAIN = 0.7   # off the throttle from the mod's own off recordings: this much of the on level
LOOP_BELOW_RPM = 2500.0  # idle rows below this play the idle loop as it is (as the 250F's); above, its
                         # cycles cut at zero crossings, next to the on-low rows' cycles
LOOP_SNAP = 0.15      # ...each cut moved up to this much of a cycle to reach one (the idle's crossings are
                      # sparse: at 0.03 only 20 of 48 found one; at 0.15 all do, cycles off 3.9% typically)
CYCLE_TOLERANCE = 0.2  # a cycle is kept within this of its neighbours' length
CUT_SNAP = 0.12       # a cut moves at most this much of a cycle to reach a zero crossing (0.06 kept too
                      # few: offhigh 16 of ~98; 0.12 keeps 62, the rhythm still even)
THIN_EVERY = 15       # (the limiter's rows: unused now)
ORDER_LENGTH = 1024   # riding cells: a play order this long (see wander_order)
RUN_MIN, RUN_MAX = 12, 40  # ...of natural runs this many cycles long
JUMP_MIN = 10         # ...each jumping at least this far, to the cycle most like the one it replaces
FIRE_AT = 0.38        # where the firing sits in each cycle cut
BRIGHTEN_DB = 0.0     # tone: none to start with (the 250F's dark recordings needed +9 dB; these may not)
BRIGHTEN_FROM_HZ = 800.0
BRIGHTEN_TO_HZ = 4000.0
DEEPEN_DB = 0.0
DEEPEN_FULL_HZ = 120.0
DEEPEN_TO_HZ = 350.0

# Loudness: the riding rows rise RISE_DB_PER_OCTAVE with every doubling of the RPM, as the YZ450 mod
# does in MX Bikes (fitted to a video of it, accelerating: 6.0 dB). Set to the stock 450's level for
# each row, they went up and down instead (-3.1 dB at 4560, then +5.7 at 5772): accelerating, the
# sound didn't build, and the RPM wobbling across a row pumped it. The curve has the stock riding rows'
# median level at RISE_MIDDLE_RPM (the pack as loud overall as before). The idle and the limiter keep
# their sound, turned to the curve: the idle to what it gives at the idle's top row, the limiter to
# full throttle at the top row (as MX Bikes has them: its idle well under its top revs).
RISE_DB_PER_OCTAVE = 6.0
RISE_MIDDLE_RPM = 6494.0
RIDING_FROM_RPM = 3500.0  # (the rows from here up are riding rows; below, the idle's)
# Riding, two recordings each way, low and high, with one seam between them (as the 250F's): the game
# mixes neighbouring rows across the whole gap between them, and between two rows of different
# recordings that's two engines at once, firing out of step. With a recording for every two or three
# rows (onlow / onmid / onmidhigh / onhigh) most of the rev range was such a mix: accelerating
# through it "sounds like my engine is broken". The YZ450 video matches onhigh pitched down from 6k
# up (0.98-0.99 at every 1000 rpm), so onhigh and offhigh take the high rows.
# Riding rows, the way other MX Simulator sound mods made from MX Bikes recordings do it (and the stock
# sounds): many rows, each cell a single engine cycle. Played as loops of their cycles, the riding rows
# were fine revving standing still but "not smooth at all ... like my engine is broken" accelerating
# on the move. A single cycle can't loop or stumble: every engine cycle plays the same waveform at the
# engine's pitch. Each is the average of the recording's cycles at that row's pitch, each lined up on
# its firing (FIRE_AT before it) - the engine's note, the cycle-to-cycle rasp averaged out - and the
# rows close together (RIDING_STEP apart) so the tone changes as gradually as the pitch.
RIDING_STEP = 1.04        # each riding row this much higher than the one before (3604..11702: 32 rows)
# Which recording plays where, as MX Bikes plays them (the YZ450 mod's engine.scl, worked out at the
# pitch heard, and the video of it): up to each RPM, that recording.
RIDING_ON_MAP = ((4800.0, 'onlow'), (6800.0, 'onmid'), (8700.0, 'onmidhigh'), (1e9, 'onhigh'))
RIDING_OFF_MAP = ((5800.0, 'offlow'), (7400.0, 'offmid'), (1e9, 'offhigh'))

# The riding rows (on the throttle and off it, above the idle) are made as the idle rows are: each
# recording a seamless loop of nearly all of it, turned so its firing sits FIRE_AT into a cycle, sliced
# into its cycles in their own order (loop_cycles_even). They were made of the cycles that cut cleanly
# at a firing, about half of each recording with a jump at every gap, in loops half as long: heard as
# rasp, and as looping.

# Calmed (see calm()): recording -> (how far its note stands above its rasp, dB; its treble trimmed, dB).
# The 4-6k rows were "super raspy": the on-low recording (an EXC450's) has 9 dB more cycle-to-cycle
# roughness than the YZ450F's recordings above it (note-vs-rasp -7.5 dB; theirs +1.3) and 4 dB more
# top end (the mod played it at a quarter of the others' volume). Off the throttle, low and mid to
# offhigh's +3.4 dB. The recordings above 6k, fine as they are, aren't touched.
CALM = {'onlow': (1.3, -4.0), 'offlow': (3.4, 0.0), 'offmid': (3.4, 0.0)}
TRIM_FROM_HZ = 1500.0  # the treble trim's shelf: none below, all of it above TRIM_TO_HZ
TRIM_TO_HZ = 5000.0

# The recordings: (name, file pattern, on / off / limiter). Their RPMs are measured.
RECORDINGS = [
    ('idle',      '*idle*.wav',       'on'),
    ('onlow',     '*onlow*.wav',      'on'),
    ('onmid',     '*onmid.wav',       'on'),
    ('onmidhigh', '*onmidhigh*.wav',  'on'),
    ('onhigh',    '*onhigh*.wav',     'on'),
    ('offlow',    '*offlow*.wav',     'off'),
    ('offmid',    '*offmid*.wav',     'off'),
    ('offhigh',   '*offhigh*.wav',    'off'),
    ('limiter',   '*limiter*.wav',    'limiter'),
]


def read(path):
    import soundfile as sf
    x, rate = sf.read(path, always_2d=True)
    x = x.mean(axis=1)
    if rate != RATE:
        x = signal.resample_poly(x, RATE, rate)
    return x


def firing_rate(x):
    """How often the engine fires (Hz): the median spacing of the loud pulses (each a firing)."""
    env = signal.filtfilt(*signal.butter(2, 60.0 / (RATE / 2)), np.abs(x))
    e = env - env.mean()
    ac = signal.fftconvolve(e, e[::-1])[len(e) - 1:]  # (the envelope's repeat: a rough rate first)
    lo, hi = int(RATE / 200), int(RATE / 8)
    rough = RATE / (lo + np.argmax(ac[lo:hi]))
    peaks, _ = signal.find_peaks(env, distance=int(RATE / rough * 0.6))
    return RATE / np.median(np.diff(peaks))


def idle_rate(x):
    """The idle's firing rate (Hz), from how its loudness repeats: the shortest repeat at least half
    as strong as the strongest. A big single idles lumpily (every other firing a little different):
    firing_rate() counted extra pulses in it, 2650 rpm for the YZ450's ~2170, which would have
    pitched every idle row 18% low."""
    env = signal.filtfilt(*signal.butter(2, 60.0 / (RATE / 2)), np.abs(x))
    e = env - env.mean()
    ac = signal.fftconvolve(e, e[::-1])[len(e) - 1:]
    ac /= ac[0]
    lo, hi = int(RATE / 200), int(RATE / 8)
    peaks, _ = signal.find_peaks(ac[lo:hi])
    peaks = peaks + lo
    p = peaks[ac[peaks] >= 0.5 * ac[peaks].max()][0]
    a, b, c = ac[p - 1], ac[p], ac[p + 1]
    return RATE / (p + 0.5 * (a - c) / (a - 2 * b + c))


def best_loop(x, period, cycles):
    """A seamless loop of `cycles` engine cycles (period: samples a cycle): where the recording's
    waveform after `cycles` cycles best matches where it started, the end cross-faded into the
    start."""
    want = cycles * period
    if want + 4 * period > len(x):
        cycles = max(4, int((len(x) - 4 * period) / period))
        want = cycles * period
    w = int(period)
    fade = int(min(0.01 * RATE, period * 0.5))  # 10 ms (half a cycle at most)
    best = None
    for start in range(int(fade + period), int(len(x) - want - 2 * period), max(1, int(period / 4))):
        ref = x[start:start + w]
        for end in range(int(start + want - period * 0.5), int(start + want + period * 0.5)):
            seg = x[end:end + w]
            score = np.dot(ref, seg) / (np.sqrt(np.dot(ref, ref) * np.dot(seg, seg)) + 1e-12)
            if best is None or score > best[0]:
                best = (score, start, end)
    score, start, end = best
    loop = x[start:end].copy()
    ramp = np.sin(np.linspace(0.0, np.pi / 2, fade)) ** 2
    loop[-fade:] = loop[-fade:] * (1.0 - ramp) + x[start - fade:start] * ramp
    return loop, cycles, score


def loop_cycles(loop, count, within=0.03):
    """The loop's `count` cycles, each boundary moved to the nearest upward zero crossing (within
    `within` of a cycle), as [(start, end)] in the loop played twice over (the last cycle runs on
    into the first). Sliced evenly, the idle loop's cycles began wherever they fell - up to 70% of
    full scale - and next to the on-low rows (other recordings, cut at zero) the game's cross-fades
    would tick there."""
    n = len(loop)
    period = n / count
    j = np.flatnonzero((loop[:-1] < 0) & (loop[1:] >= 0))
    zc = j + (-loop[j]) / (loop[j + 1] - loop[j])
    cuts = []
    for k in range(count):
        want = k * period
        d = np.abs(zc - want)
        i = int(np.argmin(d))
        cuts.append(zc[i] if d[i] < within * period else want)
    cuts.append(cuts[0] + n)
    return [(cuts[k], cuts[k + 1]) for k in range(count)]


def loop_cycles_even(loop, count, within=0.15):
    """The loop's cycles, every cut on an upward zero crossing within `within` of a cycle from where an
    even slicing puts it, the cuts chosen together (around the loop) for the evenest lengths, as
    [(start, end)] in the loop played twice over. (Each cut at its own nearest crossing, a few cycles
    came out a quarter long or short; stretched to one length, each a jerk in pitch.)"""
    n = len(loop)
    period = n / count
    j = np.flatnonzero((loop[:-1] < 0) & (loop[1:] >= 0))
    zc = j + (-loop[j]) / (loop[j + 1] - loop[j])
    cand = []
    for k in range(count):
        c = zc[np.abs(zc - k * period) < within * period]
        if k == 0:  # (the first cut may also sit just before the loop's end, wrapped round)
            c = np.concatenate([c, zc[np.abs(zc - n) < within * period] - n])
        cand.append(c if len(c) else np.array([k * period]))
    best = None
    for c0 in cand[0]:
        cost = np.array([((c - c0) / period - 1.0) ** 2 for c in cand[1]])
        back = []
        for k in range(2, count):
            prev = cand[k - 1]
            step = ((cand[k][:, None] - prev[None, :]) / period - 1.0) ** 2 + cost[None, :]
            back.append(np.argmin(step, axis=1))
            cost = step.min(axis=1)
        total = cost + ((c0 + n - cand[count - 1]) / period - 1.0) ** 2
        i = int(np.argmin(total))
        if best is None or total[i] < best[0]:
            path = [i]
            for b in reversed(back):
                path.append(int(b[path[-1]]))
            path = path[::-1]
            best = (total[i], [c0] + [cand[k + 1][path[k]] for k in range(count - 1)])
    cuts = best[1] + [best[1][0] + n]
    if cuts[0] < 0:  # (the first cut wrapped round to just before the loop's end: all of them a loop on,
        cuts = [c + n for c in cuts]  # inside the loop played twice - before, the waveform there was
                                      # extrapolated: a spike, and the cell turned down to near silence)
    return [(cuts[k], cuts[k + 1]) for k in range(count)]

def to_length(loop, length):
    """The loop resampled to `length` samples (as a loop: it still runs on into itself)."""
    return signal.resample(loop, length)


def soften(loop, top_hz):
    """Its top end rolled off above top_hz (on the loop as a loop)."""
    spectrum = np.fft.rfft(loop)
    f = np.fft.rfftfreq(len(loop), 1.0 / RATE)
    gain = 1.0 / np.sqrt(1.0 + (f / top_hz) ** 4)
    return np.fft.irfft(spectrum * gain, len(loop))


def firing_cut_cycles(x, hz, fire_at, tolerance, snap):
    """The recording's engine cycles, each cut at the upward zero crossing nearest fire_at of a cycle
    before its firing (within `snap` of a cycle), as [(start, end)] fractional sample positions,
    consecutive cycles sharing their cut; cycles more than `tolerance` off their neighbours' length,
    or with no crossing near a cut, are left out.

    The 250F's cutter (make_250f.py) finds the cycles from the phase of the firing rate's tone. These
    recordings are limited hard (15-42% of the samples flat against full scale), the tone is weak and
    its phase wanders: the cuts drifted off the firings, the firing landing anywhere in a cycle - an
    engine stumbling (spread of where the firing sits 0.5-0.9; the 250F's low rows 0.1-0.3) - and in
    a different place for each recording, so rows from two recordings mixed fired twice a cycle.
    Cut at the firings themselves, every cycle of every recording has its firing in one place."""
    period = RATE / hz
    env60 = signal.filtfilt(*signal.butter(2, 60.0 / (RATE / 2)), np.abs(x))
    env300 = signal.filtfilt(*signal.butter(2, 300.0 / (RATE / 2)), np.abs(x))
    peaks, _ = signal.find_peaks(env60, distance=int(period * 0.6))
    w = int(0.2 * period)  # each firing at the sharper envelope's peak near the smooth one's
    fire = np.array([max(0, p - w) + int(np.argmax(env300[max(0, p - w):p + w])) for p in peaks], float)
    gaps = np.diff(fire)
    local = signal.medfilt(gaps, 9) if len(gaps) >= 9 else np.full_like(gaps, np.median(gaps))
    j = np.flatnonzero((x[:-1] < 0) & (x[1:] >= 0))
    zc = j + (-x[j]) / (x[j + 1] - x[j])
    cuts = []
    for i in range(len(gaps)):
        want = fire[i + 1] - fire_at * gaps[i]  # (the cut before firing i + 1)
        k = np.searchsorted(zc, want)
        near = [zc[q] for q in (k - 1, k) if 0 <= q < len(zc)]
        best = min(near, key=lambda c: abs(c - want)) if near else None
        cuts.append(best if best is not None and abs(best - want) < snap * gaps[i] else None)
    even = np.abs(gaps / local - 1.0) < tolerance
    typical = np.median(gaps)
    cycles = []
    for i in range(len(cuts) - 1):
        a, b = cuts[i], cuts[i + 1]
        if a is not None and b is not None and even[i] and even[i + 1] and abs((b - a) / typical - 1.0) < tolerance:
            cycles.append((a, b))
    return cycles


def firing_at(loop, count):
    """Where the firing sits in the loop's cycles (0..1 of a cycle; the loudness folded over them)."""
    env = signal.filtfilt(*signal.butter(2, 300.0 / (RATE / 2)), np.abs(np.tile(loop, 2)))[:len(loop)]
    length = len(loop) / count
    bins = 200
    folded = np.zeros(bins)
    for k in range(count):
        folded += np.interp(k * length + length * np.arange(bins) / bins, np.arange(len(loop)), env)
    return np.argmax(folded) / bins


def zero_cut_row(x, cycles, length):
    """Each cycle stretched to `length` samples (its crossings at the very ends), one after another:
    the whole recording resampled first to the row's pitch (so nothing aliases), then each cycle
    eased by the little its own length differs."""
    from fractions import Fraction
    from scipy.interpolate import CubicSpline
    typical = np.median([b - a for a, b in cycles])
    ratio = Fraction(length / typical).limit_denominator(2000)
    y = signal.resample_poly(x, ratio.numerator, ratio.denominator)
    scale = ratio.numerator / ratio.denominator
    spline = CubicSpline(np.arange(len(y)), y)
    out = []
    for a, b in cycles:
        a, b = a * scale, b * scale
        out.append(spline(a + (b - a) * np.arange(length) / length))
    return np.concatenate(out)


def even_cut_cycles(x, hz, fire_at, most_off=0.06, least=24):
    """For the limiter: a seamless loop of its cycles, each cut at an upward zero crossing, the cuts
    chosen all together so the cycles come out as even as the engine's beat. (make_250f.py has the
    why.)"""
    p0 = RATE / hz
    hop, w = 220, 1323
    centres, periods = [], []
    for s in range(0, len(x) - w, hop):
        seg = x[s:s + w] - x[s:s + w].mean()
        ac = signal.fftconvolve(seg, seg[::-1])[w - 1:]
        lo, hi = int(p0 * 0.8), int(p0 * 1.25)
        centres.append(s + w / 2)
        periods.append(lo + int(np.argmax(ac[lo:hi])))
    period = np.interp(np.arange(len(x)), centres, np.convolve(signal.medfilt(periods, 5), np.ones(5) / 5, 'same'))
    j = np.flatnonzero((x[:-1] < 0) & (x[1:] >= 0))
    zc = j + (-x[j]) / (x[j + 1] - x[j])
    env = signal.filtfilt(*signal.butter(2, 300.0 / (RATE / 2)), np.abs(x))
    beat = np.cumsum(1.0 / period)
    starts = np.flatnonzero(np.diff(np.floor(beat)) > 0)
    bins = 200
    folded = np.zeros(bins)
    for a, b in zip(starts[:-1], starts[1:]):
        folded += np.interp(a + (b - a) * np.arange(bins) / bins, np.arange(len(x)), env)
    first_fire = starts[1] + np.argmax(folded) / bins * (starts[2] - starts[1])
    want = first_fire - fire_at * p0
    n = len(zc)
    cost = np.full(n, np.inf)
    back = np.full(n, -1)
    for i in np.flatnonzero(np.abs(zc - want) < 0.15 * p0):
        cost[i] = 0.0
    for b in range(n):
        p = period[int(zc[b])]
        for a in range(b - 1, -1, -1):
            length = zc[b] - zc[a]
            if length > 1.25 * p:
                break
            if length < 0.75 * p or not np.isfinite(cost[a]):
                continue
            c = cost[a] + (length / p - 1.0) ** 2
            if c < cost[b]:
                cost[b], back[b] = c, a
    reached = np.flatnonzero(np.isfinite(cost) & (back >= 0))
    end = max(reached, key=lambda e: (round(zc[e] / p0), -cost[e]))
    path = [end]
    while back[path[-1]] >= 0:
        path.append(back[path[-1]])
    cuts = zc[path[::-1]]
    even = np.abs(np.diff(cuts) / period[cuts[:-1].astype(int)] - 1.0) < most_off
    size = int(round(p0))
    shape = [np.interp(a + (b - a) * np.arange(size) / size, np.arange(len(x)), x) for a, b in zip(cuts[:-1], cuts[1:])]
    best = None
    for s in range(len(shape)):
        for count in range(least, len(shape) - s):
            if not even[s:s + count + 1].all():
                break
            alike = np.corrcoef(shape[s], shape[s + count])[0, 1]
            score = alike + 0.0005 * count
            if best is None or score > best[0]:
                best = (score, s, count, alike)
    _, s, count, alike = best
    return [(cuts[i], cuts[i + 1]) for i in range(s, s + count)], alike


def zero_ends(y, cycles, width=24):
    """Every cut between cycles brought to exactly zero."""
    length = len(y) // cycles
    bump = np.hanning(2 * width + 1)
    y = y.copy()
    for k in range(cycles):
        b = k * length
        v = y[b]
        idx = (b + np.arange(-width, width + 1)) % len(y)
        y[idx] -= v * bump
    return y


def brighten(loop, db=BRIGHTEN_DB):
    """Its top end lifted (a smooth shelf from BRIGHTEN_FROM_HZ to BRIGHTEN_TO_HZ)."""
    if db == 0:
        return loop
    spectrum = np.fft.rfft(loop)
    f = np.fft.rfftfreq(len(loop), 1.0 / RATE)
    t = np.clip(np.log2(np.maximum(f, 1.0) / BRIGHTEN_FROM_HZ) / np.log2(BRIGHTEN_TO_HZ / BRIGHTEN_FROM_HZ), 0.0, 1.0)
    shaped = 0.5 - 0.5 * np.cos(np.pi * t)
    return np.fft.irfft(spectrum * 10.0 ** (db * shaped / 20.0), len(loop))


def deepen(loop, db=DEEPEN_DB):
    """Its low end lifted (a shelf from DEEPEN_TO_HZ down, all of it below DEEPEN_FULL_HZ, none
    below 20 Hz). Added on top of the row's level."""
    if db == 0:
        return loop
    spectrum = np.fft.rfft(loop)
    f = np.fft.rfftfreq(len(loop), 1.0 / RATE)
    low = np.clip(np.log2(DEEPEN_TO_HZ / np.maximum(f, 1.0)) / np.log2(DEEPEN_TO_HZ / DEEPEN_FULL_HZ), 0.0, 1.0)
    sub = np.clip((f - 20.0) / 20.0, 0.0, 1.0)
    return np.fft.irfft(spectrum * 10.0 ** (db * (0.5 - 0.5 * np.cos(np.pi * low)) * sub / 20.0), len(loop))


def note_vs_rasp(y, count):
    """How far the part of the sound that repeats every cycle (the note) stands above what differs
    from cycle to cycle (rasp, noise), in dB."""
    c = y[:len(y) // count * count].reshape(count, -1)
    mean = c.mean(axis=0)
    return 10.0 * np.log10((mean ** 2).sum() * count / (((c - mean) ** 2).sum() + 1e-12))


def calm(y, count, target_db, trim_db):
    """Every cycle pulled toward the cycles' average until the note stands target_db above the rasp
    (the note kept, the cycle-to-cycle roughness turned down), and the top end trimmed by trim_db."""
    now = note_vs_rasp(y, count)
    if now < target_db:
        c = y.reshape(count, -1)
        mean = c.mean(axis=0)
        y = (mean + (c - mean) * 10.0 ** ((now - target_db) / 20.0)).reshape(-1)
    if trim_db:
        spectrum = np.fft.rfft(y)
        f = np.fft.rfftfreq(len(y), 1.0 / RATE)
        t = np.clip(np.log2(np.maximum(f, 1.0) / TRIM_FROM_HZ) / np.log2(TRIM_TO_HZ / TRIM_FROM_HZ), 0.0, 1.0)
        y = np.fft.irfft(spectrum * 10.0 ** (trim_db * (0.5 - 0.5 * np.cos(np.pi * t)) / 20.0), len(y))
    return y


def to_sw(x):
    return np.clip(np.round(x), -32768, 32767).astype('<i2').tobytes()


def from_sw(data):
    return np.frombuffer(data[:len(data) // 2 * 2], '<i2').astype(np.float64)


def stock_tables(game_dir):
    """The stock 450 four-stroke soundtable and recordings, from the game's archives."""
    files = {}
    for path in sorted(glob.glob(os.path.join(game_dir, '*.saf'))):
        if os.path.basename(path).lower().startswith(('mxsr_', 'mxs_remastered_')):
            continue
        try:
            files.update(saf.extract(path, lambda n: (n == ENGINE + '.soundtable' or n.startswith(ENGINE + '/')) and n not in files))
        except ValueError:
            pass
    return files


def wander_order(cycles, length, seed):
    """A play order through a recording's cycles that never comes round the same way: runs of
    RUN_MIN-RUN_MAX cycles in their own order, each ending in a jump to one of the few cycles most like
    the cycle that would have come next (at least JUMP_MIN away). The recordings are 1-2 s: played in
    order, every row came round again each second ("looping very fast" while accelerating; the 250F's
    rows run 8 s). Shuffled cycle by cycle (a jump every cycle) it sounded worse; here nearly
    everything heard is the recording as it is, and the jumps are where it hardly changes.
    cycles: [array] the recording's cycles, each at one length. Returns (order, how alike the cycles
    jumped to were to the ones they replaced, on average; and a natural step's)."""
    rng = np.random.default_rng(seed)
    n = len(cycles)
    c = np.array([v - v.mean() for v in cycles])
    c /= np.linalg.norm(c, axis=1, keepdims=True) + 1e-12
    alike = c @ c.T
    order, at, jumps = [], 0, []
    while len(order) < length:
        run = int(rng.integers(RUN_MIN, RUN_MAX + 1))
        order += [(at + k) % n for k in range(run)]
        nxt = (order[-1] + 1) % n
        far = np.array([min((j - nxt) % n, (nxt - j) % n) >= JUMP_MIN for j in range(n)])
        choices = np.flatnonzero(far)
        best = choices[np.argsort(-alike[nxt, choices])[:3]]
        at = int(rng.choice(best))
        jumps.append(alike[nxt, at])
    natural = np.mean([alike[k, (k + 1) % n] for k in range(n)])
    return [int(k) for k in order[:length]], float(np.mean(jumps)), float(natural)


START_SNAP = 0.04  # a riding cell's start moves at most this much of a cycle to reach a zero crossing
# Grit: the averaged cycle alone sounded "a little too clean". Each riding cell is the average plus
# GRIT of one real cycle's difference from it (its roughness), a different cycle for each row, in the
# recording's own order (revving up the rows goes through its cycles as recorded), from its more
# typical half (the least like the average left out). 0 = the average alone, 1 = the real cycle.
GRIT = 0.5


def average_cycle(source, rpm, turn=None, snap=True, row=None):
    """One engine cycle at rpm: the average of the recording's cycles there, each lined up on its
    firing (see RIDING_STEP); turned by the recording's source['turn'] (a fraction of a cycle, see
    line_up), then (snap) on to an upward zero crossing within START_SNAP, so the cell starts at zero
    (with none there, zero_ends sees to it). (Moved on to the nearest crossing wherever it was, the
    cells of two recordings lined up came out a fifth of a cycle apart again: out of phase.)"""
    length = int(round(120.0 * RATE / rpm))
    cycles = source['cut']
    stack = zero_cut_row(source['raw'], cycles, length).reshape(len(cycles), length)
    clean = stack.mean(axis=0)
    y = clean
    if row is not None and GRIT > 0:
        alike = np.array([np.corrcoef(c, clean)[0, 1] for c in stack])
        typical = np.flatnonzero(alike >= np.median(alike))   # (in the recording's order)
        y = clean + GRIT * (stack[typical[row % len(typical)]] - clean)
    turn = source.get('turn', 0.0) if turn is None else turn
    move = int(round(turn * length))
    if snap:
        # (Where the cell starts: from the average, the same for every row - found on the gritty cycle,
        # each row's start moved by its own few percent, and neighbouring rows came out of step.)
        c = np.roll(clean, -move)
        j = np.flatnonzero((c < 0) & (np.roll(c, -1) >= 0))  # (upward crossings, round the cycle)
        if len(j):
            off = np.where(j > length // 2, j - length, j)
            near = off[np.argmin(np.abs(off))]
            if abs(near) <= START_SNAP * length:
                move += int(near)
    return np.roll(y, -move)


def line_up(sources, chain, seams):
    """The recordings of a chain (low to high RPM, seams: where each hands over to the next) turned
    to fire together: each, from the top down, by the turn that makes its cycle most like the next
    one's at their seam. Lined up on their own firings, the recordings' cycles still came out with the
    firing at different points (0.28 / 0.84 / 0.06 of a cycle): at a seam the game mixed two cycles
    0.33-0.74 alike, half cancelling."""
    sources[chain[-1]]['turn'] = 0.0
    for k in range(len(chain) - 2, -1, -1):
        low, high, rpm = sources[chain[k]], sources[chain[k + 1]], seams[k]
        a = average_cycle(low, rpm, 0.0, snap=False)
        b = average_cycle(high, rpm, high['turn'], snap=False)
        b = b[:len(a)] if len(b) >= len(a) else np.pad(b, (0, len(a) - len(b)))
        corr = np.fft.irfft(np.fft.rfft(a) * np.conj(np.fft.rfft(b)), len(a))
        shift = int(np.argmax(corr))
        low['turn'] = shift / len(a)
        alike = np.corrcoef(np.roll(a, -shift), b)[0, 1]
        print('  %-9s turned %.2f of a cycle to fire with %-9s at %5.0f rpm: alike %.2f (it was %.2f)' % (
            chain[k], low['turn'], chain[k + 1], rpm, alike, np.corrcoef(a, b)[0, 1]))


def nearest(sources, names, rpm):
    """Of these recordings, the one whose RPM is nearest (in pitch: by ratio) to rpm."""
    return min(names, key=lambda n: abs(np.log(sources[n]['rpm'] / rpm)))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    game_dir, mod_dir = sys.argv[1], sys.argv[2]
    out_dir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'MXS_REMASTERED_sounds')
    os.makedirs(out_dir, exist_ok=True)
    from make_engine_sounds import parse_soundtable

    sources = {}
    for name, pattern, kind in RECORDINGS:
        found = sorted(glob.glob(os.path.join(mod_dir, pattern)))
        if not found:
            sys.exit('no %s in %s' % (pattern, mod_dir))
        x = read(found[0])
        hz = idle_rate(x) if name == 'idle' else firing_rate(x)
        src = {'rpm': hz * 120.0, 'raw': x, 'kind': kind, 'file': os.path.basename(found[0])}
        if kind != 'limiter':
            if name == 'idle':
                loop, src['cycles'], score = best_loop(x, RATE / hz, LOOP_CYCLES)
            else:
                # Riding: nearly all of the recording; a few cycles fewer where that makes a much better
                # seam (onlow's longest loop met itself at only 0.63).
                most = int(len(x) * hz / RATE) - 5
                tries = [best_loop(x, RATE / hz, c) for c in range(most, max(8, int(most * 0.85)) - 1, -1)]
                loop, src['cycles'], score = max(tries, key=lambda t: t[2] + 0.002 * t[1])
            # Turned so its firing sits FIRE_AT into each cycle, as the other recordings' cuts have
            # it (the rows next to each other then fire together when the game mixes them). A loop
            # turned plays exactly as before.
            shift = (firing_at(loop, src['cycles']) - FIRE_AT) % 1.0
            src['loop'] = np.roll(loop, -int(round(shift * len(loop) / src['cycles'])))
        if kind == 'limiter':
            src['cut'], alike = even_cut_cycles(x, hz, FIRE_AT)
        elif name == 'idle':
            src['cut'] = []  # (the idle rows play its loop)
        else:
            src['cut'] = firing_cut_cycles(x, hz, FIRE_AT, CYCLE_TOLERANCE, CUT_SNAP)  # (averaged, see RIDING_STEP)
        sources[name] = src
        print('%-9s %-22s %5.1f s  fires %5.1f Hz: %5.0f rpm; %3d cycles cut at zero crossings (%.2f s)%s' % (
            name, src['file'], len(x) / RATE, hz, src['rpm'], len(src['cut']),
            (src['cut'][-1][1] - src['cut'][0][0]) / RATE if src['cut'] else 0.0,
            ('; loop of %d cycles (%.2f s), its ends matching %.2f' % (src['cycles'], len(src['loop']) / RATE, score)) if 'loop' in src else ''))

    files = stock_tables(game_dir)
    rpms, loads, cells = parse_soundtable(files[ENGINE + '.soundtable'].decode('latin1'))
    on_names = [n for n, _, k in RECORDINGS if k == 'on']
    off_names = ['idle'] + [n for n, _, k in RECORDINGS if k == 'off']
    # Each riding recording's play order (see wander_order), the same for every cell made from it, so
    # its rows mixed with each other play the same cycle together.
    orders = {}
    for i, (name, _, kind) in enumerate(RECORDINGS):
        if kind in ('on', 'off') and name != 'idle':
            loop, count = sources[name]['loop'], sources[name]['cycles']
            twice = np.concatenate([loop, loop])
            size = 512
            cycles = [np.interp(a + (b - a) * np.arange(size) / size, np.arange(len(twice)), twice)
                      for a, b in loop_cycles_even(loop, count, LOOP_SNAP)]
            orders[name], jump, natural = wander_order(cycles, ORDER_LENGTH, 4500 + i)
            print('  %-9s play order of %d over its %d cycles: jumps to cycles %.2f alike (a natural step %.2f)' % (
                name, ORDER_LENGTH, count, jump, natural))

    def stock_level(r, suffixes):
        levels = []
        for cell in cells[r * len(loads):(r + 1) * len(loads)]:
            if cell['cycles'] == 1 and cell['file'] in files and os.path.splitext(cell['file'])[0][-1] in suffixes:
                c = from_sw(files[cell['file']])
                levels.append(np.sqrt(np.mean(c ** 2)))
        return np.median(levels) if levels else None

    riding = [r for r, rpm in enumerate(rpms) if rpm >= RIDING_FROM_RPM]
    middle = np.median([stock_level(r, 'ab') for r in riding])
    def curve(rpm):  # (the riding rows' level at rpm)
        return middle * 10.0 ** (RISE_DB_PER_OCTAVE * np.log2(rpm / RISE_MIDDLE_RPM) / 20.0)
    idle_top = max(r for r, rpm in enumerate(rpms) if rpm < RIDING_FROM_RPM)
    idle_turn = curve(rpms[idle_top]) / stock_level(idle_top, 'ab')         # the idle, turned to the curve
    limiter_turn = curve(rpms[-1]) / stock_level(len(rpms) - 1, 'ab')     # the limiter: full throttle at the top
    print('  loudness: riding rows %.0f at %.0f rpm to %.0f at %.0f (%+.1f dB); the idle turned %+.1f dB, the limiter %+.1f dB' % (
        curve(rpms[riding[0]]), rpms[riding[0]], curve(rpms[-1]), rpms[-1], 20 * np.log10(curve(rpms[-1]) / curve(rpms[riding[0]])),
        20 * np.log10(idle_turn), 20 * np.log10(limiter_turn)))

    # The rows: the stock's idle rows, then riding rows RIDING_STEP apart up to the stock's top row
    # (which keeps its limiter cell). Each row: (rpm, the stock row it copies the cells of, or None).
    top = len(rpms) - 1
    grid = [rpms[riding[0]]]
    while grid[-1] * RIDING_STEP < rpms[top] * 0.99:
        grid.append(grid[-1] * RIDING_STEP)
    table = ([(rpm, r) for r, rpm in enumerate(rpms) if rpm < RIDING_FROM_RPM] +
             [(rpm, None) for rpm in grid] + [(rpms[top], top)])
    plain = cells[riding[-2] * len(loads):(riding[-2] + 1) * len(loads)]  # (a stock riding row: off, on, on, on)

    def pick(table_map, rpm):
        return next(name for limit, name in table_map if rpm < limit)

    for table_map in (RIDING_ON_MAP, RIDING_OFF_MAP):
        line_up(sources, [name for _, name in table_map], [limit for limit, _ in table_map[:-1]])

    folder = 'mxsr_' + ENGINE
    made = {}   # (row, kind) -> (name, cycles, samples, source)
    pack_files = []
    lines = (['SOUNDTABLE %d %d' % (len(table), len(loads))] + ['%d' % round(rpm) for rpm, _ in table] +
             [' '.join('%g' % l for l in loads)])
    for r, (rpm, stock_row) in enumerate(table):
        row_cells = cells[stock_row * len(loads):(stock_row + 1) * len(loads)] if stock_row is not None else plain
        for column in range(len(loads)):
            cell = row_cells[column]
            suffix = os.path.splitext(cell['file'])[0][-1]
            kind = 'limiter' if cell['cycles'] != 1 else 'off' if suffix == 'c' else 'on'
            key = (r, kind)
            if key not in made:
                if kind == 'limiter':
                    src_name, level, share = 'limiter', stock_level(stock_row, 'ab'), 1.0
                elif rpm < RIDING_FROM_RPM:
                    src_name, level = 'idle', stock_level(stock_row, 'ab')
                    share = OFF_GAIN if kind == 'off' else 1.0
                else:
                    src_name = pick(RIDING_ON_MAP if kind == 'on' else RIDING_OFF_MAP, rpm)
                    level, share = curve(rpm), (REAL_OFF_GAIN if kind == 'off' else 1.0)
                source = sources[src_name]
                if src_name == 'idle':
                    # (Idle, where the revs and load sit still: the loop, as the 250F's. Its lumpy
                    # firing defeats the cycle cutting - the cuts came out ~12% short.)
                    count = source['cycles']
                    loop = source['loop']
                    if rpm < LOOP_BELOW_RPM:
                        y = to_length(loop, int(round(count * 120.0 * RATE / rpm)))
                    else:
                        y = zero_cut_row(np.concatenate([loop, loop]), loop_cycles(loop, count, LOOP_SNAP),
                                         int(round(120.0 * RATE / rpm)))
                    if kind == 'off':
                        y = soften(y, OFF_TOP_HZ)
                    y = brighten(y)
                elif kind != 'limiter':
                    count = 1
                    y = average_cycle(source, rpm, row=r)
                    if src_name in CALM:
                        y = calm(y, 1, *CALM[src_name])
                    y = zero_ends(brighten(y), 1)
                else:
                    cut = source['cut']
                    count = len(cut)
                    y = zero_cut_row(source['raw'], cut, int(round(120.0 * RATE / rpm)))
                    y = zero_ends(brighten(y), count)
                # As loud as meant (the stock row, or the riding curve; off the throttle less), never
                # past full scale.
                gain = (level * share) / (np.sqrt(np.mean(y ** 2)) + 1e-9)
                y = deepen(y)
                if not (src_name == 'idle' and rpm < LOOP_BELOW_RPM):
                    y = zero_ends(y, count)  # (its cuts kept at zero)
                y *= gain
                y *= min(1.0, 32000.0 / (np.abs(y).max() + 1e-9))
                # (The idle and the limiter turned to the loudness curve as a whole.)
                turn = limiter_turn if kind == 'limiter' else idle_turn if src_name == 'idle' else 1.0
                y *= turn
                y *= min(1.0, 32000.0 / (np.abs(y).max() + 1e-9))
                # (Never quieter than meant by more than the limit to full scale can explain: a spike
                # turning a whole cell down to near silence stops the build.)
                meant = level * share * turn
                if np.sqrt(np.mean(y ** 2)) < 0.5 * meant:
                    sys.exit('%d rpm %s: %.0f rms of the %.0f meant - a spike in it?' % (rpm, kind, np.sqrt(np.mean(y ** 2)), meant))
                name = '%s/yz450-%05d-%s-%03d.sw' % (folder, int(round(rpm)), kind, count)
                pack_files.append((name, to_sw(y)))
                made[key] = (name, count, y, src_name)
            name, cycles, _, src = made[key]
            lines.append('%s %d @%s' % (cell['lead'], cycles, name))
    soundtable = ('\n'.join(lines) + '\n').encode('latin1')
    archive = [(ENGINE + '.soundtable', soundtable)] + pack_files
    pack_path = os.path.join(out_dir, 'MXS_REMASTERED_engine_%s.pack' % ENGINE)
    saf.write(pack_path, archive)
    for r, (rpm, _) in enumerate(table):
        parts = []
        for kind in ('on', 'off', 'limiter'):
            if (r, kind) in made:
                _, count, y, src = made[(r, kind)]
                parts.append('%s: %s x%d (%.2f s)' % (kind, src, count, len(y) / RATE))
        print('  %5.0f rpm  %s' % (rpm, '   '.join(parts)))
    print('%s: %d rows, %d recordings -> %s (%.1f MB)' % (
        ENGINE, len(table), len(pack_files), pack_path, sum(len(d) for _, d in archive) / 1e6))

    # The preview: the sound mod's own recordings (up to 2 s each), then each row on the throttle and off it (1 s each).
    out = []
    for name, _, _ in RECORDINGS:
        raw = sources[name]['raw']
        out += [raw[:2 * RATE] / (np.abs(raw).max() + 1e-9) * 0.7 * 32767, np.zeros(RATE // 3)]
    for kind in ('on', 'off'):
        for r in range(len(table)):
            if (r, kind) in made:
                y = made[(r, kind)][2]
                out += [np.tile(y, int(np.ceil(RATE / len(y))))[:RATE], np.zeros(RATE // 8)]
    with wave.open(os.path.join(out_dir, 'preview_%s.wav' % ENGINE), 'wb') as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(to_sw(np.concatenate(out)))


if __name__ == '__main__':
    main()
