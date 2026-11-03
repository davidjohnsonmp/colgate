// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Deterministic test material and independent measurements. Everything here is
// synthetic, so the tests run the same on every machine without audio files.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace colgate_test
{

constexpr double kPi = 3.14159265358979323846;

using Buffer = std::vector<float>;

struct Random
{
    uint64_t state;
    explicit Random (uint64_t seed) : state (seed * 0x9E3779B97F4A7C15ull + 1) {}
    float uniform()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return (float) ((double) (state >> 11) / 9007199254740992.0) * 2.0f - 1.0f;
    }
};

inline Buffer sine (double hz, double amplitude, int length, double rate, double phase = 0.0)
{
    Buffer out ((size_t) length);
    for (int n = 0; n < length; ++n)
        out[(size_t) n] = (float) (amplitude * std::sin (2.0 * kPi * hz * n / rate + phase));
    return out;
}

// Paul Kellet's pink-noise filter, scaled to the requested RMS.
inline Buffer pinkNoise (double rmsTarget, int length, uint64_t seed)
{
    Random random (seed);
    Buffer out ((size_t) length);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, sum = 0;
    for (int n = 0; n < length; ++n)
    {
        const double w = random.uniform();
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        const double p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        out[(size_t) n] = (float) p;
        sum += p * p;
    }
    const double scale = rmsTarget / std::sqrt (sum / length);
    for (auto& v : out)
        v = (float) (v * scale);
    return out;
}

inline double rmsDb (const float* x, int length)
{
    double sum = 1.0e-30;
    for (int n = 0; n < length; ++n)
        sum += (double) x[n] * x[n];
    return 10.0 * std::log10 (sum / length);
}

inline double rmsDb (const Buffer& x, int start = 0, int end = -1)
{
    const int stop = end < 0 ? (int) x.size() : end;
    return rmsDb (x.data() + start, stop - start);
}

inline double peakDb (const Buffer& x, int start = 0, int end = -1)
{
    const int stop = end < 0 ? (int) x.size() : end;
    double peak = 1.0e-30;
    for (int n = start; n < stop; ++n)
        peak = std::max (peak, (double) std::abs (x[(size_t) n]));
    return 20.0 * std::log10 (peak);
}

// Amplitude at one frequency by correlation, immune to everything else nearby.
inline double toneDb (const Buffer& x, double hz, double rate, int start, int end)
{
    double re = 0, im = 0, norm = 0;
    for (int n = start; n < end; ++n)
    {
        const double u = (n - start) / (double) (end - start - 1);
        const double w = 0.5 * (1.0 - std::cos (2.0 * kPi * u));
        const double phase = 2.0 * kPi * hz * n / rate;
        re += w * x[(size_t) n] * std::cos (phase);
        im += w * x[(size_t) n] * std::sin (phase);
        norm += w;
    }
    return 20.0 * std::log10 (2.0 * std::sqrt (re * re + im * im) / norm + 1.0e-20);
}

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double tick (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// ITU-R BS.1770 K-weighting at any sample rate (de Man's analogue-prototype
// form of the published 48 kHz coefficients).
inline void kWeighting (double rate, Biquad& shelf, Biquad& highpass)
{
    {
        const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan (kPi * f0 / rate);
        const double vh = std::pow (10.0, g / 20.0), vb = std::pow (vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan (kPi * f0 / rate);
        const double a0 = 1.0 + k / q + k * k;
        highpass.b0 = 1.0;
        highpass.b1 = -2.0;
        highpass.b2 = 1.0;
        highpass.a1 = 2.0 * (k * k - 1.0) / a0;
        highpass.a2 = (1.0 - k / q + k * k) / a0;
    }
}

// BS.1770-4 gated programme loudness in LUFS.
inline double integratedLufs (const std::vector<Buffer>& channels, double rate, int start = 0, int end = -1)
{
    const int stop = end < 0 ? (int) channels[0].size() : end;
    const int count = stop - start;
    std::vector<double> power ((size_t) count, 0.0);

    for (const auto& channel : channels)
    {
        Biquad shelf, highpass;
        kWeighting (rate, shelf, highpass);
        for (int n = 0; n < count; ++n)
        {
            const double y = highpass.tick (shelf.tick (channel[(size_t) (start + n)]));
            power[(size_t) n] += y * y;
        }
    }

    const int block = (int) (0.4 * rate);
    const int step = (int) (0.1 * rate);
    std::vector<double> blocks;
    for (int s = 0; s + block <= count; s += step)
    {
        double sum = 0;
        for (int n = s; n < s + block; ++n)
            sum += power[(size_t) n];
        blocks.push_back (sum / block);
    }
    if (blocks.empty())
        return -100.0;

    const auto lufs = [] (double p) { return -0.691 + 10.0 * std::log10 (p + 1.0e-30); };
    double sum = 0;
    int kept = 0;
    for (double b : blocks)
        if (lufs (b) > -70.0)
        {
            sum += b;
            ++kept;
        }
    if (kept == 0)
        return -100.0;
    const double threshold = lufs (sum / kept) - 10.0;
    sum = 0;
    kept = 0;
    for (double b : blocks)
        if (lufs (b) > -70.0 && lufs (b) > threshold)
        {
            sum += b;
            ++kept;
        }
    return kept > 0 ? lufs (sum / kept) : -100.0;
}

// Short-window K-weighted loudness over time, used to compare dynamics.
inline std::vector<double> loudnessContour (const std::vector<Buffer>& channels, double rate, double blockSeconds, int start, int end)
{
    const int count = end - start;
    std::vector<double> power ((size_t) count, 0.0);
    for (const auto& channel : channels)
    {
        Biquad shelf, highpass;
        kWeighting (rate, shelf, highpass);
        for (int n = 0; n < count; ++n)
        {
            const double y = highpass.tick (shelf.tick (channel[(size_t) (start + n)]));
            power[(size_t) n] += y * y;
        }
    }
    const int block = (int) (blockSeconds * rate);
    std::vector<double> out;
    for (int s = 0; s + block <= count; s += block / 2)
    {
        double sum = 1.0e-30;
        for (int n = s; n < s + block; ++n)
            sum += power[(size_t) n];
        out.push_back (10.0 * std::log10 (sum / block));
    }
    return out;
}

// A small synthetic arrangement. It has what a real mix has and a test needs:
// transients, a bass that masks upward, sustained harmonic parts, a voice-like
// lead with vibrato, and top-end air, all with some stereo placement. Stems are
// kept separate for the sidechain null test.
struct DemoMix
{
    std::vector<std::vector<Buffer>> stems; // stem, channel
    std::vector<Buffer> mix;
};

namespace detail
{
inline float softClip (double x) { return (float) std::tanh (x); }

inline void addPanned (std::vector<Buffer>& stem, int n, double value, double pan)
{
    const double angle = 0.25 * kPi * (pan + 1.0);
    stem[0][(size_t) n] += (float) (value * std::cos (angle));
    if (stem.size() > 1)
        stem[1][(size_t) n] += (float) (value * std::sin (angle));
}
} // namespace detail

inline DemoMix makeDemoMix (double rate, double seconds, int numChannels, uint64_t seed = 7)
{
    const int length = (int) (rate * seconds);
    const int stemCount = 5;
    DemoMix demo;
    demo.stems.assign ((size_t) stemCount, std::vector<Buffer> ((size_t) numChannels, Buffer ((size_t) length, 0.0f)));

    const double beat = 60.0 / 110.0;
    Random random (seed);

    // Drums: kick on 1 and 3, snare on 2 and 4, hats on eighths.
    {
        auto& kick = demo.stems[0];
        auto& snare = demo.stems[1];
        double hatLp = 0.0;
        for (int n = 0; n < length; ++n)
        {
            const double t = n / rate;
            const double position = std::fmod (t, 2.0 * beat);
            const double kPhase = 2.0 * kPi * (48.0 * position + (150.0 - 48.0) * 0.02 * (1.0 - std::exp (-position / 0.02)));
            const double kickValue = 0.75 * std::exp (-position / 0.14) * std::sin (kPhase)
                                     + 0.25 * std::exp (-position / 0.003) * random.uniform();
            detail::addPanned (kick, n, kickValue, 0.0);

            const double snarePos = std::fmod (t + beat, 2.0 * beat);
            const double snareValue = 0.3 * std::exp (-snarePos / 0.05) * std::sin (2.0 * kPi * 190.0 * snarePos)
                                      + 0.32 * std::exp (-snarePos / 0.11) * random.uniform();
            detail::addPanned (snare, n, snareValue, 0.0);

            const double hatPos = std::fmod (t, 0.5 * beat);
            const double white = random.uniform();
            hatLp += 0.45 * (white - hatLp);
            const double hat = 0.12 * std::exp (-hatPos / 0.018) * (white - hatLp);
            detail::addPanned (snare, n, hat, 0.3);
        }
    }

    // Bass: filtered saw over a little progression.
    {
        auto& bass = demo.stems[2];
        const double notes[4] = { 55.0, 55.0, 43.65, 49.0 };
        double phase = 0.0, lp1 = 0.0, lp2 = 0.0;
        for (int n = 0; n < length; ++n)
        {
            const double t = n / rate;
            const int bar = (int) (t / (4.0 * beat)) % 4;
            const double position = std::fmod (t, beat);
            phase += notes[bar] / rate;
            phase -= std::floor (phase);
            const double saw = 2.0 * phase - 1.0;
            const double cutoff = 1.0 - std::exp (-2.0 * kPi * (300.0 + 500.0 * std::exp (-position / 0.08)) / rate);
            lp1 += cutoff * (saw - lp1);
            lp2 += cutoff * (lp1 - lp2);
            detail::addPanned (bass, n, 0.5 * detail::softClip (1.6 * lp2) * std::min (1.0, position / 0.005), 0.0);
        }
    }

    // Pad: detuned chord with a soft top, spread wide.
    {
        auto& pad = demo.stems[3];
        const double chords[4][3] = { { 220.0, 261.63, 329.63 }, { 220.0, 261.63, 329.63 },
                                      { 174.61, 220.0, 261.63 }, { 196.0, 246.94, 293.66 } };
        double phases[6] = { 0.1, 0.33, 0.57, 0.21, 0.74, 0.92 };
        double lpL = 0.0, lpR = 0.0;
        const double coefficient = 1.0 - std::exp (-2.0 * kPi * 2600.0 / rate);
        for (int n = 0; n < length; ++n)
        {
            const double t = n / rate;
            const int bar = (int) (t / (4.0 * beat)) % 4;
            double left = 0.0, right = 0.0;
            for (int v = 0; v < 3; ++v)
            {
                phases[v] += chords[bar][v] * 1.003 / rate;
                phases[v + 3] += chords[bar][v] * 0.997 / rate;
                phases[v] -= std::floor (phases[v]);
                phases[v + 3] -= std::floor (phases[v + 3]);
                left += 2.0 * phases[v] - 1.0;
                right += 2.0 * phases[v + 3] - 1.0;
            }
            lpL += coefficient * (left - lpL);
            lpR += coefficient * (right - lpR);
            pad[0][(size_t) n] += (float) (0.075 * (lpL + 0.3 * lpR));
            if (numChannels > 1)
                pad[1][(size_t) n] += (float) (0.075 * (lpR + 0.3 * lpL));
            else
                pad[0][(size_t) n] += (float) (0.075 * (lpR + 0.3 * lpL));
        }
    }

    // Lead: a voice-like source through three formants, with vibrato, breaks
    // between phrases, and a breath of noise on top.
    {
        auto& lead = demo.stems[4];
        const double melody[8] = { 440.0, 523.25, 493.88, 440.0, 392.0, 440.0, 329.63, 392.0 };
        double phase = 0.0;
        double fz[3][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
        const double formants[3][2] = { { 650.0, 90.0 }, { 1500.0, 130.0 }, { 2800.0, 190.0 } };
        double fa1[3], fa2[3], fb0[3];
        for (int f = 0; f < 3; ++f)
        {
            const double w = 2.0 * kPi * formants[f][0] / rate;
            const double r = std::exp (-kPi * formants[f][1] / rate);
            fa1[f] = -2.0 * r * std::cos (w);
            fa2[f] = r * r;
            fb0[f] = 1.0 - r;
        }
        for (int n = 0; n < length; ++n)
        {
            const double t = n / rate;
            const int step = (int) (t / beat) % 8;
            const double position = std::fmod (t, beat);
            const double hz = melody[step] * (1.0 + 0.012 * std::sin (2.0 * kPi * 5.2 * t));
            phase += hz / rate;
            phase -= std::floor (phase);
            const double pulse = std::pow (std::abs (std::sin (kPi * phase)), 12.0) - 0.08;
            const double breath = 0.05 * random.uniform();
            const double source = pulse + breath;
            double out = 0.0;
            for (int f = 0; f < 3; ++f)
            {
                const double y = fb0[f] * source - fa1[f] * fz[f][0] - fa2[f] * fz[f][1];
                fz[f][1] = fz[f][0];
                fz[f][0] = y;
                out += y * (f == 0 ? 1.0 : (f == 1 ? 0.7 : 0.5));
            }
            const double envelope = std::min (1.0, position / 0.03) * std::min (1.0, std::max (0.0, (beat * 0.82 - position) / 0.05));
            detail::addPanned (lead, n, 0.8 * out * envelope, -0.15);
        }
    }

    demo.mix.assign ((size_t) numChannels, Buffer ((size_t) length, 0.0f));
    for (const auto& stem : demo.stems)
        for (int c = 0; c < numChannels; ++c)
            for (int n = 0; n < length; ++n)
                demo.mix[(size_t) c][(size_t) n] += stem[(size_t) c][(size_t) n];

    // Bring the whole thing to a sensible mix level.
    double peak = 0.0;
    for (const auto& channel : demo.mix)
        for (float v : channel)
            peak = std::max (peak, (double) std::abs (v));
    const float trim = (float) (0.7 / peak);
    for (auto& channel : demo.mix)
        for (auto& v : channel)
            v *= trim;
    for (auto& stem : demo.stems)
        for (auto& channel : stem)
            for (auto& v : channel)
                v *= trim;
    return demo;
}

} // namespace colgate_test
