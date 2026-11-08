"""The 250 four-stroke engine sound pack, from a sound mod's recordings.

A sound mod made for another game: a few steady recordings of the engine, which that game pitch-shifts
across the rev range (*_onidle.wav: low revs, *_onhigh.wav: high revs, *_limiter.wav: on the rev
limiter). MX Simulator plays an engine from a soundtable instead (see make_engine_sounds.py): a row
of recordings for each of a set of RPMs, the game shifting each only between its neighbours. So each
recording here is measured (its RPM, from how often the engine fires: a four-stroke single fires once
every two revolutions), cut into a seamless loop of whole engine cycles, and for every row of the
stock 250 four-stroke's soundtable resampled to that row's RPM: the low-revs recording for the rows
below the middle of the two, the high-revs one above, the limiter for the stock limiter cell.

The sound mod has no recording off the throttle: those cells play the same loops, quieter and with
their top end softened, the way an engine sounds coasting.

    python make_250f.py "C:\\Games\\MX Simulator" "V:\\mx sim sound mods\\FC250" [out_dir]

Writes <out_dir>/mxsr_engine_default250f.pack (installed from the mod's panel like the others) and
preview_default250f.wav: the sound mod's own recordings, then each row on the throttle and off it.
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
ENGINE = 'default250f'
LOOP_CYCLES = 48      # engine cycles in each loop (cut from the recording where it loops best)
OFF_GAIN = 0.5        # off the throttle: this much quieter (-6 dB) ...
OFF_TOP_HZ = 3000.0   # ... and its top end softened above this
LOOP_BELOW_RPM = 2500.0  # the rows below this (idle) play the loop above; the limiter cell too
MOST_CYCLES = 100000  # the rows above idle, and the limiter: every steady cycle of the recording, cut at zero crossings
THIN_EVERY = 15       # every other row leaves out one cycle in this many (so neighbouring rows never repeat in step)
BRIGHTEN_DB = 9.0     # the recordings' top end lifted this much (from 0 at BRIGHTEN_FROM_HZ to all of it at BRIGHTEN_TO_HZ)
BRIGHTEN_FROM_HZ = 800.0
BRIGHTEN_TO_HZ = 4000.0
DEEPEN_DB = 4.0       # and its low end (the thump of the firing) lifted this much: all of it from 40 Hz
DEEPEN_FULL_HZ = 120.0  # up to here, easing to none at DEEPEN_TO_HZ (and to none below 20 Hz, a rumble no one hears)
DEEPEN_TO_HZ = 350.0
FIRE_AT = 0.38        # where the firing sits in each of those cycles (where it sits in the limiter's)


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
    # First a rough rate (YIN-like, from the envelope's repeat), then pulses at least half that apart.
    ac = np.correlate(env - env.mean(), env - env.mean(), 'full')[len(env) - 1:]
    lo, hi = int(RATE / 200), int(RATE / 8)
    rough = RATE / (lo + np.argmax(ac[lo:hi]))
    peaks, _ = signal.find_peaks(env, distance=int(RATE / rough * 0.6))
    return RATE / np.median(np.diff(peaks))


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
    # The end eased into what came just before the start, so the loop runs on into itself.
    ramp = np.sin(np.linspace(0.0, np.pi / 2, fade)) ** 2
    loop[-fade:] = loop[-fade:] * (1.0 - ramp) + x[start - fade:start] * ramp
    return loop, cycles, score


def to_length(loop, length):
    """The loop resampled to `length` samples (as a loop: it still runs on into itself)."""
    return signal.resample(loop, length)


def soften(loop, top_hz):
    """Its top end rolled off above top_hz (on the loop as a loop)."""
    spectrum = np.fft.rfft(loop)
    f = np.fft.rfftfreq(len(loop), 1.0 / RATE)
    gain = 1.0 / np.sqrt(1.0 + (f / top_hz) ** 4)  # (2nd-order roll-off)
    return np.fft.irfft(spectrum * gain, len(loop))


def zero_cut_cycles(x, hz, most, fire_at, tolerance=0.15):
    """The recording's engine cycles, each cut at an upward zero crossing at the same point of the
    firing (the firing fire_at of the way into every cycle), as [(start, end)] fractional sample
    positions, consecutive cycles sharing their crossing; at most `most`, from the steadiest stretch.

    The game mixes the cells around the bike's RPM and load and changes that mix from one cycle to
    the next. Where a cycle's cut isn't at a zero crossing, every change of the mix jumps: a pop each
    cycle while riding (the constant crackle). Cut at zero crossings, and all at the same point of the
    firing, any cycle of any cell follows any other cleanly (as the stock patches do)."""
    # Where each cycle starts, from the phase of the firing rate's fundamental (steady even where the
    # recording's loudness isn't).
    band = signal.butter(2, [hz * 0.7 / (RATE / 2), hz * 1.35 / (RATE / 2)], 'band', output='sos')
    phase = np.unwrap(np.angle(signal.hilbert(signal.sosfiltfilt(band, x))))
    t = np.flatnonzero(np.diff(np.floor(phase / (2 * np.pi))) > 0).astype(float)
    gaps = np.diff(t)
    steady = np.abs(gaps / signal.medfilt(gaps, 9) - 1.0) < tolerance
    # Where the firing sits in those cycles (the loudness, averaged over every steady cycle).
    env = signal.filtfilt(*signal.butter(2, 300.0 / (RATE / 2)), np.abs(x))
    bins = 200
    folded = np.zeros(bins)
    for i in np.flatnonzero(steady):
        folded += np.interp(t[i] + gaps[i] * np.arange(bins) / bins, np.arange(len(x)), env)
    fire = np.argmax(folded) / bins
    want = t[:-1] + ((fire - fire_at) % 1.0) * gaps  # where each cut should fall
    # The upward zero crossings (fractional), and which of them sits at the cut most often.
    j = np.flatnonzero((x[:-1] < 0) & (x[1:] >= 0))
    zc = j + (-x[j]) / (x[j + 1] - x[j])
    near = np.clip(np.searchsorted(zc, want), 1, len(zc) - 1)
    pick = np.where(np.abs(zc[near - 1] - want) < np.abs(zc[near] - want), near - 1, near)
    offset = np.median((zc[pick] - want) / gaps)
    pick = np.clip(np.searchsorted(zc, want + offset * gaps), 1, len(zc) - 1)
    pick = np.where(np.abs(zc[pick - 1] - want - offset * gaps) < np.abs(zc[pick] - want - offset * gaps), pick - 1, pick)
    cuts = zc[pick]
    lengths = np.diff(cuts)
    good = steady[:-1] & steady[1:] & (np.abs(lengths / np.median(gaps) - 1.0) < tolerance)
    cycles = [(cuts[i], cuts[i + 1]) for i in np.flatnonzero(good)]
    # The steadiest `most` of them (the fewest left out between the first and the last).
    if len(cycles) > most:
        starts = np.array([a for a, _ in cycles])
        span = starts[most - 1:] - starts[:len(starts) - most + 1]
        first = int(np.argmin(span))
        cycles = cycles[first:first + most]
    return cycles, fire, offset


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
    chosen all together so the cycles come out as even as the engine's beat, as [(start, end)].

    Its waveform changes shape over a pattern of a few firings (the limiter's rhythm), so the zero
    crossing nearest each firing isn't the same crossing every time; cut one cycle at a time there,
    the cycles came out up to 30% long or short, and stretched back to one length the pitch jerked:
    heard as cutouts. Here the crossings are picked as a whole (the run of them whose spacing keeps
    closest to the waveform's own period, through the recording), starting near where the firing
    sits fire_at of the way into a cycle; the loop is then the longest stretch of those cycles, all
    within most_off of the period, whose next cycle is most like its first (so it runs on into
    itself, its rhythm unbroken)."""
    p0 = RATE / hz
    # The waveform's period through the recording (30 ms windows, every 5 ms), smoothed.
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
    # Where the firing sits (the loudness folded over the period), and so where the first cut goes.
    env = signal.filtfilt(*signal.butter(2, 300.0 / (RATE / 2)), np.abs(x))
    beat = np.cumsum(1.0 / period)
    starts = np.flatnonzero(np.diff(np.floor(beat)) > 0)
    bins = 200
    folded = np.zeros(bins)
    for a, b in zip(starts[:-1], starts[1:]):
        folded += np.interp(a + (b - a) * np.arange(bins) / bins, np.arange(len(x)), env)
    first_fire = starts[1] + np.argmax(folded) / bins * (starts[2] - starts[1])
    want = first_fire - fire_at * p0
    # The crossings, chosen together: the least squared difference of each cycle from the period.
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
    # (The path that reaches furthest into the recording, at the least cost per cycle.)
    reached = np.flatnonzero(np.isfinite(cost) & (back >= 0))
    end = max(reached, key=lambda e: (round(zc[e] / p0), -cost[e]))
    path = [end]
    while back[path[-1]] >= 0:
        path.append(back[path[-1]])
    cuts = zc[path[::-1]]
    even = np.abs(np.diff(cuts) / period[cuts[:-1].astype(int)] - 1.0) < most_off
    # The loop: the stretch of even cycles whose next cycle is most like its first.
    size = int(round(p0))
    shape = [np.interp(a + (b - a) * np.arange(size) / size, np.arange(len(x)), x) for a, b in zip(cuts[:-1], cuts[1:])]
    best = None
    for s in range(len(shape)):
        for count in range(least, len(shape) - s):
            if not even[s:s + count + 1].all():
                break
            alike = np.corrcoef(shape[s], shape[s + count])[0, 1]
            score = alike + 0.0005 * count  # (alike first; then the longer)
            if best is None or score > best[0]:
                best = (score, s, count, alike)
    _, s, count, alike = best
    return [(cuts[i], cuts[i + 1]) for i in range(s, s + count)], alike


def zero_ends(y, cycles, width=24):
    """Every cut between cycles brought to exactly zero (a correction over a millisecond or so, of
    the few counts the resampling and softening leave there)."""
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
    """Its top end lifted (on the loop as a loop): the sound mod's recordings are dark, and pitched
    down for the lower rows darker still; this takes the muffle off without touching the body."""
    spectrum = np.fft.rfft(loop)
    f = np.fft.rfftfreq(len(loop), 1.0 / RATE)
    t = np.clip(np.log2(np.maximum(f, 1.0) / BRIGHTEN_FROM_HZ) / np.log2(BRIGHTEN_TO_HZ / BRIGHTEN_FROM_HZ), 0.0, 1.0)
    shaped = 0.5 - 0.5 * np.cos(np.pi * t)  # (a smooth shelf)
    return np.fft.irfft(spectrum * 10.0 ** (db * shaped / 20.0), len(loop))


def deepen(loop, db=DEEPEN_DB):
    """Its low end lifted (on the loop as a loop), for a deeper note: a shelf from DEEPEN_TO_HZ
    down, all of it below DEEPEN_FULL_HZ (and none below 20 Hz). Added on top: the row's level is
    set from the sound before this, so the rest of it stays as loud as it was."""
    spectrum = np.fft.rfft(loop)
    f = np.fft.rfftfreq(len(loop), 1.0 / RATE)
    low = np.clip(np.log2(DEEPEN_TO_HZ / np.maximum(f, 1.0)) / np.log2(DEEPEN_TO_HZ / DEEPEN_FULL_HZ), 0.0, 1.0)
    sub = np.clip((f - 20.0) / 20.0, 0.0, 1.0)
    return np.fft.irfft(spectrum * 10.0 ** (db * (0.5 - 0.5 * np.cos(np.pi * low)) * sub / 20.0), len(loop))


def to_sw(x):
    return np.clip(np.round(x), -32768, 32767).astype('<i2').tobytes()


def from_sw(data):
    return np.frombuffer(data[:len(data) // 2 * 2], '<i2').astype(np.float64)


def stock_tables(game_dir):
    """The stock 250 four-stroke soundtable and recordings, from the game's archives."""
    files = {}
    for path in sorted(glob.glob(os.path.join(game_dir, '*.saf'))):
        if os.path.basename(path).lower().startswith('mxsr_'):
            continue
        try:
            files.update(saf.extract(path, lambda n: (n == ENGINE + '.soundtable' or n.startswith(ENGINE + '/')) and n not in files))
        except ValueError:
            pass
    return files


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    game_dir, mod_dir = sys.argv[1], sys.argv[2]
    out_dir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'mxcam_sounds')
    os.makedirs(out_dir, exist_ok=True)
    from make_engine_sounds import parse_soundtable

    def one(pattern):
        found = glob.glob(os.path.join(mod_dir, pattern))
        if not found:
            sys.exit('no %s in %s' % (pattern, mod_dir))
        return found[0]
    sources = {}
    for kind, pattern in (('low', '*onidle*.wav'), ('high', '*onhigh*.wav'), ('limiter', '*limiter*.wav')):
        x = read(one(pattern))
        hz = firing_rate(x)
        rpm = hz * 120.0
        loop, cycles, score = best_loop(x, RATE / hz, LOOP_CYCLES)
        if kind == 'limiter':
            cut, offset = even_cut_cycles(x, hz, FIRE_AT)  # (offset here: how alike its wrap is)
        else:
            cut, fire, offset = zero_cut_cycles(x, hz, MOST_CYCLES, FIRE_AT)
        sources[kind] = {'rpm': rpm, 'loop': loop, 'cycles': cycles, 'raw': x, 'cut': cut}
        print('%-7s %-24s fires %.1f times a second: %5.0f rpm; loop of %d cycles (%.2f s), its ends matching %.2f; '
              '%d cycles cut at zero crossings (%.1f s; crossing %+.2f of a cycle from the cut)' % (
                  kind, os.path.basename(one(pattern)), hz, rpm, cycles, len(loop) / RATE, score,
                  len(cut), (cut[-1][1] - cut[0][0]) / RATE, offset))

    files = stock_tables(game_dir)
    rpms, loads, cells = parse_soundtable(files[ENGINE + '.soundtable'].decode('latin1'))
    split = np.sqrt(sources['low']['rpm'] * sources['high']['rpm'])  # (low revs below, high above)

    def stock_level(r, suffixes):
        """How loud the stock row is (RMS), from its whole-cycle recordings with those suffixes."""
        levels = []
        for cell in cells[r * len(loads):(r + 1) * len(loads)]:
            if cell['cycles'] == 1 and cell['file'] in files and os.path.splitext(cell['file'])[0][-1] in suffixes:
                c = from_sw(files[cell['file']])
                levels.append(np.sqrt(np.mean(c ** 2)))
        return np.median(levels) if levels else None

    folder = 'mxsr_' + ENGINE
    made = {}   # (row, kind) -> (name, cycles, samples)
    pack_files = []
    lines = ['SOUNDTABLE %d %d' % (len(rpms), len(loads))] + ['%d' % r for r in rpms] + [' '.join('%g' % l for l in loads)]
    on_level = None
    for r, rpm in enumerate(rpms):
        level = stock_level(r, 'ab') or on_level
        on_level = level
        for column in range(len(loads)):
            cell = cells[r * len(loads) + column]
            suffix = os.path.splitext(cell['file'])[0][-1]
            if cell['cycles'] != 1:
                kind = 'limiter'                      # (the stock limiter loop's cell)
            elif suffix == 'c':
                kind = 'off'
            else:
                kind = 'on'
            key = (r, kind)
            if key not in made:
                source = sources['limiter'] if kind == 'limiter' else sources['low' if rpm < split else 'high']
                if kind != 'limiter' and rpm < LOOP_BELOW_RPM:
                    # (Idle, where the revs and load sit still: the loop, as it was.)
                    count = source['cycles']
                    length = int(round(count * 120.0 * RATE / rpm))
                    y = to_length(source['loop'], length)
                    if kind == 'off':
                        y = soften(y, OFF_TOP_HZ)
                    y = brighten(y)
                else:
                    # Every other row a few cycles fewer: two neighbouring rows (which the game plays
                    # mixed) then come round to their starts at different times, never in step.
                    cut = source['cut']
                    if r % 2 and kind != 'limiter':
                        cut = [c for i, c in enumerate(cut) if i % THIN_EVERY != THIN_EVERY // 2]
                    count = len(cut)
                    y = zero_cut_row(source['raw'], cut, int(round(120.0 * RATE / rpm)))
                    if kind == 'off':
                        y = soften(y, OFF_TOP_HZ)
                    y = zero_ends(brighten(y), count)
                # As loud as the stock row (off the throttle: OFF_GAIN of that), never past full scale.
                gain = (level * (OFF_GAIN if kind == 'off' else 1.0)) / (np.sqrt(np.mean(y ** 2)) + 1e-9)
                y = deepen(y)
                if kind == 'limiter' or rpm >= LOOP_BELOW_RPM:
                    y = zero_ends(y, count)  # (its cuts kept at zero)
                y *= gain
                y *= min(1.0, 32000.0 / (np.abs(y).max() + 1e-9))
                name = '%s/fc250-%05d-%s-%02d.sw' % (folder, int(rpm), kind, count)
                pack_files.append((name, to_sw(y)))
                made[key] = (name, count, y)
            name, cycles, _ = made[key]
            # (No play-order line: the game plays a patch's cycles in turn by default. A play-order
            # line belongs to the patch line after it, not before.)
            lines.append('%s %d @%s' % (cell['lead'], cycles, name))
    soundtable = ('\n'.join(lines) + '\n').encode('latin1')
    archive = [(ENGINE + '.soundtable', soundtable)] + pack_files
    pack_path = os.path.join(out_dir, 'mxsr_engine_%s.pack' % ENGINE)
    saf.write(pack_path, archive)
    print('%s: %d rows (%s below %.0f rpm, the high-revs one above), %d recordings -> %s (%.1f MB)' % (
        ENGINE, len(rpms), 'the low-revs recording', split, len(pack_files), pack_path, sum(len(d) for _, d in archive) / 1e6))

    # The preview: the sound mod's own recordings (2 s each), then each row on the throttle and off it (1 s each).
    out = []
    for kind in ('low', 'high', 'limiter'):
        raw = sources[kind]['raw']
        out += [raw[:2 * RATE] / (np.abs(raw).max() + 1e-9) * 0.7 * 32767, np.zeros(RATE // 3)]
    for kind in ('on', 'off'):
        for r in range(len(rpms)):
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
