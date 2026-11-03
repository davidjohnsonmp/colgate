#!/usr/bin/env python3
# Colgate by Hiccup Audio
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Fits Colgate's Basic and Master tunings to reference renders, and scores them.

    python3 tools/calibration/calibrate.py SONG_FOLDER [SONG_FOLDER ...] --build BUILD_DIR [--score]

Each song folder holds one or more 48 kHz WAVs with "original" in the name, and
the renders of that song, in any subfolders. Names and folders say what a render
is, with words or underscores in any order:

    ref 1/master mode/recover 15 tame 15 bias 15 brighten 15 boost 30.wav
    ref 1/recover_50.wav

A render counts as Master if its name or folder says "master", and otherwise as
Basic. A render identical to its original is reported and left out, since that
means the plug-in wasn't processing.

Prints what each song scores with the constants fitted on the other songs, then
the constants for source/engine/PerceptualField.cpp, fitted on all of them. With
--score it instead runs the built engine and compares the actual audio.

Needs numpy and scipy. See README.md beside this file.
"""
import argparse, glob, json, os, re, subprocess, tempfile, warnings
import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as sg
from scipy.optimize import least_squares

warnings.filterwarnings('ignore')

RATE = 48000
HOP = 128
FRAME, STEP = 2048, 256
FPS = RATE / HOP
RECOVER_Z = np.array([2.0, 8.0, 16.0, 26.0, 33.0, 36.0, 38.0, 39.5])
TAME_Z = np.array([2.0, 8.0, 16.0, 26.0, 38.0])
FADE_Z = np.array([28.0, 33.0, 36.0, 38.0, 39.5, 40.5, 41.3])
FADE = np.array([1.0, 1.0, 0.95, 0.85, 0.62, 0.2, 0.0])
BOOST_Z = np.array([26.0, 30.0, 33.0, 35.0, 37.0, 38.5, 40.3])
FIELD_EXPONENT, BOOST_EXPONENT = 0.35, 0.5
FLOOR_DB, POWER_FLOOR = 60.0, 7.94e-11
ATTACK, RELEASE, SLOW_FIELD_SECONDS = 0.12, 0.04, 2.0
BRIGHTEN_BEND = 0.25
FIT_BANDS = slice(2, 49)


def erb(hz): return 21.4 * np.log10(1 + 0.00437 * hz)
def from_erb(z): return (10 ** (z / 21.4) - 1) / 0.00437


Z0 = erb(30.0)
HZ = from_erb(Z0 + 0.8 * np.arange(int(np.floor((erb(18500.0) - Z0) / 0.8)) + 1))
Z = erb(HZ)
B = len(HZ)
PIVOT_Z = erb(900.0)
TOP = np.interp(Z, FADE_Z, FADE)
t = np.clip((Z - 23.0) / 7.0, 0, 1)
TREBLE = t * t * (3 - 2 * t)


def load(path):
    rate, x = wavfile.read(path)
    if rate != RATE:
        raise SystemExit(f'{path}: expected {RATE} Hz, found {rate}')
    x = x.astype(np.float64) if x.dtype.kind == 'f' else x.astype(np.float64) / float(np.iinfo(x.dtype).max)
    return x if x.ndim == 2 else x[:, None]


def parse_setting(name):
    name = name.lower().replace('birhgten', 'brighten').replace('birghten', 'brighten').replace('_', ' ')
    def value(word):
        m = re.search(word + r'\s*(-?\d+)', name)
        return int(m.group(1)) if m else 0
    return tuple(value(w) for w in ('recover', 'tame', 'bias', 'brighten', 'boost'))


def find_song(folder):
    """Returns {mode: (original path, {setting: path})}, keeping only real renders."""
    originals, renders = {}, {'basic': {}, 'master': {}}
    for path in sorted(glob.glob(os.path.join(folder, '**', '*.wav'), recursive=True)):
        relative = os.path.relpath(path, folder).lower()
        mode = 'master' if 'master' in relative else 'basic'
        if 'original' in os.path.basename(relative):
            originals[mode] = path
        elif any(w in relative for w in ('recover', 'tame')):
            renders[mode][parse_setting(os.path.basename(relative))] = path
    song = {}
    for mode, found in renders.items():
        original = originals.get(mode) or originals.get('basic') or originals.get('master')
        if not found or original is None:
            continue
        x = load(original)
        kept = {}
        for setting, path in found.items():
            y = load(path)
            if len(y) == len(x) and np.mean((y - x)[len(x) // 4:] ** 2) < 1e-14:
                print(f'  left out, identical to the original: {path}')
            else:
                kept[setting] = path
        if kept:
            song[mode] = (original, kept)
    return song


def delay_of(a, b):
    n = 2 ** int(np.ceil(np.log2(len(a) + len(b))))
    c = np.fft.irfft(np.fft.rfft(b[:, 0], n) * np.conj(np.fft.rfft(a[:, 0], n)))
    i = int(np.argmax(np.abs(c)))
    return i - n if i > n // 2 else i


def aligned(x, y):
    d = delay_of(x, y)
    pad = np.zeros((abs(d), y.shape[1]))
    return np.vstack([y[d:], pad]) if d >= 0 else np.vstack([pad, y[:d]])


def band_matrix():
    f = np.fft.rfftfreq(FRAME, 1 / RATE)
    z = erb(np.maximum(f, 1.0))
    m = np.zeros((B, len(f)))
    for k, c in enumerate(Z):
        u = np.abs(z - c) / 0.8
        m[k] = np.where(u < 1, 0.5 * (1 + np.cos(np.pi * u)), 0)
    m[0, f < HZ[0]] = 1
    m[-1, f > HZ[-1]] = 1
    m[:, f < 15] = 0
    return m


MATRIX = band_matrix()
WINDOW = np.hanning(FRAME + 1)[:-1]


def band_power(x):
    n = (len(x) - FRAME) // STEP + 1
    out = np.empty((n, FRAME // 2 + 1))
    for i in range(n):
        out[i] = np.abs(np.fft.rfft(WINDOW * x[i * STEP:i * STEP + FRAME])) ** 2
    return (MATRIX @ out.T).T


def smooth_energy(raw):
    up, down = [1 - np.exp(-1 / (FPS * s)) for s in (ATTACK, RELEASE)]
    out, state = np.zeros_like(raw), raw[:40].mean(0)
    for i, row in enumerate(raw):
        d = row - state
        state = state + np.where(d > 0, up, down) * d
        out[i] = state
    return out


def keep_loudness(energy, gain, exponent):
    """Broadband dB holding the sum of power^exponent over the bands."""
    wanted = ((energy + POWER_FLOOR) ** exponent - POWER_FLOOR ** exponent).sum(1)
    db, gained = np.zeros(len(energy)), energy * 10 ** (gain / 10)
    for _ in range(4):
        scale = 10 ** (db / 10)[:, None]
        total = gained * scale + POWER_FLOOR
        powered = total ** exponent
        value = (powered - POWER_FLOOR ** exponent).sum(1)
        slope = (exponent * powered * (gained * scale / total)).sum(1)
        db = db + np.clip((wanted - value) / (slope * 0.2302585 + 1e-15), -8, 8)
    return db


def iso226(hz, phon):
    freq = [20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500]
    af = [.532, .506, .480, .455, .432, .409, .387, .367, .349, .330, .315, .301, .288, .276, .267, .259, .253, .250, .246, .244, .243, .243, .243, .242, .242, .245, .254, .271, .301]
    lu = [-31.6, -27.2, -23.0, -19.1, -15.9, -13.0, -10.3, -8.1, -6.2, -4.5, -3.1, -2.0, -1.1, -.4, 0, .3, .5, 0, -2.7, -4.1, -1.0, 1.7, 2.5, 1.2, -2.1, -7.1, -11.2, -10.7, -3.1]
    tf = [78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9, 14.4, 11.4, 8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7, -1.3, -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3]
    lf = np.log(np.clip(hz, 20, 12500))
    a, l, tt = [np.interp(lf, np.log(freq), v) for v in (af, lu, tf)]
    return 10 / a * np.log10(4.47e-3 * (10 ** (.025 * phon) - 1.15) + (.4 * 10 ** ((tt + l) / 10 - 9)) ** a) - l + 94


def contour_difference(boost, reference=50.0):
    d = lambda hz: boost - (iso226(hz, reference + boost) - iso226(hz, reference))
    return d(HZ) - d(np.array([1700.0]))[0]


class Clip:
    """One song in one mode: features of the original, measured gain per render."""
    def __init__(self, name, original, renders, build):
        self.name = name
        x = load(original)
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, 'bands.f32')
            subprocess.check_call([os.path.join(build, 'colgate_bands'), original, path])
            raw = np.fromfile(path, dtype=np.float32).reshape(-1, B)
        power = [band_power(x[:, c]) for c in range(x.shape[1])]
        total = sum(power)
        self.active = total > total.mean(0) * 0.01
        n = len(total)
        # A target frame is centred FRAME / 2 samples in. The analyser has heard
        # everything up to there, and nothing after it.
        self.energy = smooth_energy(raw)[np.clip((STEP * np.arange(n) + FRAME // 2) // HOP, 0, len(raw) - 1)]
        level = 10 * np.log10(self.energy + 1e-13)
        whole = 10 * np.log10(self.energy.sum(1, keepdims=True) + 1e-13)
        self.rel = np.maximum(level, whole - FLOOR_DB) - whole
        d = np.arange(B)[:, None] - np.arange(B)[None, :]
        kernel = np.exp(-0.5 * d ** 2.0)
        self.near = self.rel @ (kernel / kernel.sum(1, keepdims=True)).T
        self.targets = {}
        for setting, path in renders.items():
            y = aligned(x, load(path))
            self.targets[setting] = np.mean([10 * np.log10((band_power(y[:, c]) + 1e-14) / (power[c] + 1e-14))
                                             for c in range(x.shape[1])], axis=0)
        self.step = max(3, int(round(n / 800)))
        self.frames = np.arange(40, n - 20, self.step)


NAMES = (['own', 'near', 'offset_recover', 'offset_tame']
         + [f'recover{i}' for i in (0, 1, 3, 4, 5, 6, 7)] + [f'tame{i}' for i in (0, 1, 3, 4)]
         + ['softness', 'exponent', 'tilt_low', 'tilt_high', 'tilt_knee', 'calm'])
START = [-.09, -.08, -3, -5, 3.5, 1, -.5, -.5, .2, 2, 3, -.5, .3, .8, -.5, 1.5, 1, .15, .3, 4, .4]
LOW = [-1, -1, -12, -12, -10, -10, -10, -10, -10, -10, -10, -10, -10, -10, -10, 0, .5, -.2, -.2, 2, -1]
HIGH = [1, 1, 12, 12, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 4, 1.4, 2, 2, 60, 3]


def named(x): return dict(zip(NAMES, x))


def shape_of(p, clip, setting, frames=None):
    recover, tame, _, brighten, _ = setting
    rel, near = (clip.rel, clip.near) if frames is None else (clip.rel[frames], clip.near[frames])
    level = p['own'] * rel + p['near'] * near
    amount = lambda v: (v / 50.0) ** p['exponent'] if v > 0 else 0.0
    s_recover, s_tame = amount(recover), amount(tame)
    b = brighten / 100.0

    def field(offset, zs, values):
        f = level + offset + np.interp(Z, zs, values)
        if b != 0:
            a = 1 - np.exp(-(clip.step if frames is not None else 1) * STEP / RATE / SLOW_FIELD_SECONDS)
            slow, _ = sg.lfilter([a], [1, a - 1], f, axis=0, zi=f[:1] * (1 - a))
            f = slow + (f - slow) * (1 - p['calm'] * b * TREBLE)
        return f

    soft = p['softness'] ** 2 + 1e-9
    out = 0.0
    if s_recover > 0:
        f = field(p['offset_recover'], RECOVER_Z, [p['recover0'], p['recover1'], 0.0, p['recover3'], p['recover4'], p['recover5'], p['recover6'], p['recover7']])
        out = out + s_recover * .5 * (f + np.sqrt(f * f + soft))
    if s_tame > 0:
        f = field(p['offset_tame'], TAME_Z, [p['tame0'], p['tame1'], 0.0, p['tame3'], p['tame4']])
        out = out + s_tame * .5 * (f - np.sqrt(f * f + soft))
    if b != 0:
        fp = Z - PIVOT_Z
        tilt = np.where(fp < 0, p['tilt_low'] * fp, p['tilt_high'] * p['tilt_knee'] * np.tanh(fp / p['tilt_knee']))
        bent = np.sign(b) * (1 - np.exp(-abs(b) / BRIGHTEN_BEND)) / (1 - np.exp(-1 / BRIGHTEN_BEND))
        out = out + .5 * (s_recover + s_tame) * bent * tilt
    return out


def boost_of(q, setting):
    return q[0] * contour_difference(setting[4]) + np.interp(Z, BOOST_Z, np.r_[0.0, q[1:]], left=0.0) * setting[4] / 30.0


def predict(p, q, clip, setting, frames=None):
    energy = clip.energy if frames is None else clip.energy[frames]
    shape = shape_of(p, clip, setting, frames)
    out = TOP * (shape + keep_loudness(energy, TOP * shape, FIELD_EXPONENT)[:, None])
    if setting[4] != 0:
        b = boost_of(q, setting)
        out = out + TOP * (b + keep_loudness(energy, TOP * b, BOOST_EXPONENT)[:, None])
    return out


def fit_field(clips):
    def residuals(x):
        p, out = named(x), []
        for c in clips:
            for setting, y in c.targets.items():
                if setting[4] == 0:
                    d = (predict(p, None, c, setting, c.frames) - y[c.frames])[:, FIT_BANDS]
                    out.append(d[c.active[c.frames][:, FIT_BANDS]])
        return np.concatenate(out)
    return named(least_squares(residuals, START, bounds=(LOW, HIGH), max_nfev=30, diff_step=2e-3).x)


def fit_boost(p, clips):
    with_boost = [c for c in clips if any(s[4] != 0 for s in c.targets)]
    if not with_boost:
        return None

    def residuals(q):
        out = []
        for c in with_boost:
            for setting, y in c.targets.items():
                if setting[4] != 0:
                    d = (predict(p, q, c, setting, c.frames) - y[c.frames])[:, 1:49]
                    out.append(d[c.active[c.frames][:, 1:49]])
        return np.concatenate(out)
    return least_squares(residuals, [1.5, 0, -.2, .2, -.2, 3, 5], max_nfev=15, diff_step=1e-2).x


def score(p, q, clip, setting):
    if setting[4] != 0 and q is None:
        return None
    y, g = clip.targets[setting], predict(p, q, clip, setting)
    m = clip.active.copy()
    m[:, :1 if setting[4] else 2] = False
    m[:, 49:] = False
    m[:40] = False
    return 1 - ((y[m] - g[m]) ** 2).sum() / ((y[m] - y[m].mean()) ** 2).sum(), np.sqrt(((y[m] - g[m]) ** 2).mean())


def constants(p, q):
    r = [p[f'recover{i}'] for i in (0, 1)] + [0.0] + [p[f'recover{i}'] for i in (3, 4, 5, 6, 7)]
    m = [p['tame0'], p['tame1'], 0.0, p['tame3'], p['tame4']]
    array = lambda v: '{ ' + ', '.join(f'{x:.4f}f' for x in v) + ' }'
    q = q if q is not None else [1.25, 0, 0, 0, 0, 0, 0]
    return (f"    {p['own']:.4f}f, {p['near']:.4f}f,\n    {p['offset_recover']:.4f}f, {p['offset_tame']:.4f}f,\n"
            f"    {array(r)},\n    {array(m)},\n    {p['softness']:.4f}f,\n    {p['exponent']:.4f}f,\n"
            f"    {p['tilt_low']:.4f}f, {p['tilt_high']:.4f}f, {p['tilt_knee']:.3f}f,\n    {p['calm']:.4f}f,\n"
            f"    {ATTACK}f, {RELEASE}f,\n    {q[0]:.4f}f,\n    {array(q[1:])}")


def run_fit(songs, build):
    for mode in ['basic', 'master']:
        clips = [Clip(name, *song[mode], build) for name, song in songs.items() if mode in song]
        if not clips:
            continue
        print(f'\n{mode}: each song scored with constants fitted on the others')
        if len(clips) > 1:
            for held in clips:
                others = [c for c in clips if c is not held]
                p = fit_field(others)
                q = fit_boost(p, others)
                for setting in sorted(held.targets):
                    s = score(p, q, held, setting)
                    print(f'  {held.name:16} {str(setting):26} ' + (f'R2 {s[0]:.3f}   error {s[1]:.3f} dB' if s else 'no Boost render to fit from'))
        p = fit_field(clips)
        q = fit_boost(p, clips)
        print(f'{mode}: fitted on every song, for PerceptualField.cpp\n{constants(p, q)}')


def lufs(x):
    b1, a1 = [1.53512485958697, -2.69169618940638, 1.19839281085285], [1, -1.69065929318241, 0.73248077421585]
    b2, a2 = [1, -2, 1], [1, -1.99004745483398, 0.99007225036621]
    p = sum(sg.lfilter(b2, a2, sg.lfilter(b1, a1, x[:, c])) ** 2 for c in range(x.shape[1]))
    size, step = int(.4 * RATE), int(.1 * RATE)
    blocks = np.array([p[i:i + size].mean() for i in range(0, len(p) - size, step)])
    loud = -0.691 + 10 * np.log10(blocks + 1e-30)
    gate = loud > -70
    gate &= loud > -0.691 + 10 * np.log10(blocks[gate].mean()) - 10
    return -0.691 + 10 * np.log10(blocks[gate].mean())


def fractional_delay(a, b):
    n = 2 ** int(np.ceil(np.log2(len(a))))
    A, C = np.fft.rfft(a[:, 0], n), np.fft.rfft(b[:, 0], n)
    f = np.fft.rfftfreq(n, 1 / RATE)
    m = (f > 150) & (f < 5000)
    c = C[m] * np.conj(A[m])
    w, om = np.abs(c), 2 * np.pi * f[m]
    return -(w * np.angle(c) * om).sum() / (w * om ** 2).sum() * RATE


def advanced(y, samples):
    f = np.fft.rfftfreq(len(y), 1 / RATE)
    return np.fft.irfft(np.fft.rfft(y, axis=0) * np.exp(2j * np.pi * f * samples / RATE)[:, None], len(y), axis=0)


def run_score(songs, build):
    print('song             mode    setting                      R2   curve  corr  total    dry   null   LUFS ref  col')
    for name, song in songs.items():
        for mode, (original, renders) in song.items():
            x = load(original)
            for s, path in sorted(renders.items()):
                y = aligned(x, load(path))
                n = len(x) - 4096
                a, b = x[:n], y[:n]
                with tempfile.TemporaryDirectory() as folder:
                    out = os.path.join(folder, 'out.wav')
                    subprocess.check_call([os.path.join(build, 'colgate_render'), original, out, '--recover', str(s[0]), '--tame', str(s[1]),
                                           '--bias', str(s[2]), '--brighten', str(s[3]), '--boost', str(s[4]), '--mode', mode])
                    c = load(out)[:n]
                total = lambda v: sum(band_power(v[:, k]) for k in range(v.shape[1]))
                power = total(a)
                gr, gc = [10 * np.log10((total(v) + 1e-14) / (power + 1e-14)) for v in (b, c)]
                weight = power / power.sum(0, keepdims=True)
                mr, mc = (gr * weight).sum(0), (gc * weight).sum(0)
                act = power > power.mean(0) * 0.02
                act[:40] = False
                dr, dc = gr - mr, gc - mc
                rms = lambda v: np.sqrt((v[act] ** 2).mean())
                shifted = advanced(b, fractional_delay(c, b))
                edge = 2048
                null = 10 * np.log10(((shifted - c)[edge:-edge] ** 2).mean() / ((shifted - a)[edge:-edge] ** 2).mean())
                r2 = 1 - ((gr - gc)[act] ** 2).sum() / ((gr[act] - gr[act].mean()) ** 2).sum()
                print('{:16} {:7} {:26} {:5.3f} {:5.2f}  {:4.2f}  {:5.2f}  {:5.2f}  {:+5.1f}  {:+6.2f} {:+6.2f}'.format(
                    name, mode, str(s), r2, np.sqrt(((mr - mc)[2:-2] ** 2).mean()), np.corrcoef(dr[act], dc[act])[0, 1], rms(gr - gc), rms(gr),
                    null, lufs(b) - lufs(a), lufs(c) - lufs(a)), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('folders', nargs='+', help='one folder per song')
    parser.add_argument('--build', required=True, help='folder holding colgate_render and colgate_bands')
    parser.add_argument('--score', action='store_true', help="compare the engine's audio instead of fitting")
    args = parser.parse_args()
    songs = {}
    for folder in args.folders:
        name = os.path.basename(os.path.normpath(folder))
        songs[name] = find_song(folder)
        if not songs[name]:
            raise SystemExit(f'Found no usable renders in {folder}')
    (run_score if args.score else run_fit)(songs, args.build)
