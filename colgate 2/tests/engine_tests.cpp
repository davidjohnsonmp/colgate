// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The engine's test suite. Each test states a behaviour the plug-in promises,
// and checks it on the audio coming out instead of on internal variables
// wherever that is possible.
#include "TestSignals.h"
#include "ColgateEngine.h"
#include "Fft.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>

using namespace colgate;
using namespace colgate_test;

// ---------------------------------------------------------------------------
// Tiny test harness and an allocation counter for the realtime check.

static std::atomic<long> allocationCount { 0 };

void* operator new (std::size_t size)
{
    ++allocationCount;
    if (void* p = std::malloc (size))
        return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
int failures = 0;
int checks = 0;

void check (bool ok, const char* what, double value = 0.0, double limit = 0.0)
{
    ++checks;
    if (! ok)
        ++failures;
    std::printf ("    %s %-68s %10.3f  (limit %.3f)\n", ok ? "ok  " : "FAIL", what, value, limit);
}

void atMost (double value, double limit, const char* what) { check (value <= limit, what, value, limit); }
void atLeast (double value, double limit, const char* what) { check (value >= limit, what, value, limit); }

struct Test
{
    const char* name;
    std::function<void()> body;
};

std::unique_ptr<ColgateEngine> makeEngine (double rate, int channels, const EngineParameters& p, Quality q = Quality::normal)
{
    auto e = std::make_unique<ColgateEngine>();
    e->setParameters (p);
    e->prepare (rate, channels);
    e->setQuality (q);
    e->reset();
    return e;
}

using Channels = std::vector<Buffer>;

Channels render (ColgateEngine& engine, Channels audio, const Channels* sidechain = nullptr, int block = 256)
{
    const int length = (int) audio[0].size();
    const int channels = (int) audio.size();
    for (int s = 0; s < length; s += block)
    {
        const int n = std::min (block, length - s);
        float* main[kMaxChannels] = {};
        const float* side[kMaxChannels] = {};
        for (int c = 0; c < channels; ++c)
        {
            main[c] = audio[(size_t) c].data() + s;
            if (sidechain != nullptr)
                side[c] = (*sidechain)[(size_t) c].data() + s;
        }
        engine.process (main, sidechain != nullptr ? side : nullptr, n);
    }
    return audio;
}

Channels alignOutput (const Channels& out, int latency)
{
    Channels aligned;
    for (const auto& c : out)
        aligned.emplace_back (c.begin() + latency, c.end());
    return aligned;
}

double differenceDb (const Buffer& a, const Buffer& b, int start, int end)
{
    double sum = 1.0e-30;
    for (int n = start; n < end; ++n)
    {
        const double d = (double) a[(size_t) n] - b[(size_t) n];
        sum += d * d;
    }
    return 10.0 * std::log10 (sum / (end - start));
}

EngineParameters settings (float recover, float tame, float bias = 0, float brighten = 0, float boost = 0)
{
    EngineParameters p;
    p.recoverPercent = recover;
    p.tamePercent = tame;
    p.biasPercent = bias;
    p.brightenPercent = brighten;
    p.boostDb = boost;
    return p;
}

const double kRate = 48000.0;
} // namespace

// ---------------------------------------------------------------------------

static void testFft()
{
    for (int size : { 128, 256, 512, 2048, 4096 })
    {
        SplitFft fft;
        fft.prepare (size);
        Random random ((uint64_t) size);
        std::vector<float> re ((size_t) size), im ((size_t) size), originalRe, originalIm;
        for (int n = 0; n < size; ++n)
        {
            re[(size_t) n] = random.uniform();
            im[(size_t) n] = random.uniform();
        }
        originalRe = re;
        originalIm = im;
        fft.forward (re.data(), im.data());

        double error = 0.0;
        for (int k : { 0, 1, 3, size / 2, size - 2 })
        {
            double xr = 0, xi = 0;
            for (int n = 0; n < size; ++n)
            {
                const double a = -2.0 * kPi * k * n / size;
                xr += originalRe[(size_t) n] * std::cos (a) - originalIm[(size_t) n] * std::sin (a);
                xi += originalRe[(size_t) n] * std::sin (a) + originalIm[(size_t) n] * std::cos (a);
            }
            error = std::max (error, std::hypot (xr - re[(size_t) k], xi - im[(size_t) k]));
        }

        fft.forward (im.data(), re.data());
        double roundTrip = 0.0;
        for (int n = 0; n < size; ++n)
            roundTrip = std::max (roundTrip, (double) std::abs (re[(size_t) n] / size - originalRe[(size_t) n]));

        const std::string label = "FFT size " + std::to_string (size) + " against a direct DFT";
        atMost (error, 2.0e-2, label.c_str());
        atMost (roundTrip, 1.0e-5, "round trip through the inverse");
    }
}

static void testIso226()
{
    const auto p1k = iso226Interpolate (1000.0);
    atMost (std::abs (iso226Spl ((float) p1k.alpha, (float) p1k.lu, (float) p1k.tf, 40.0f) - 40.0f), 0.05, "40 phon at 1 kHz is 40 dB SPL");
    atMost (std::abs (iso226Spl ((float) p1k.alpha, (float) p1k.lu, (float) p1k.tf, 80.0f) - 80.0f), 0.05, "80 phon at 1 kHz is 80 dB SPL");
    const auto p100 = iso226Interpolate (100.0);
    atMost (std::abs (iso226Spl ((float) p100.alpha, (float) p100.lu, (float) p100.tf, 40.0f) - 64.37f), 0.1, "40 phon at 100 Hz is 64.4 dB SPL (ISO 226:2003 table)");
}

static void testZeroIsExactBypass()
{
    for (double rate : { 16000.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0 })
    {
        for (int channels : { 1, 2 })
        {
            const int length = (int) (rate * 1.0);
            Channels in;
            in.push_back (pinkNoise (0.1, length, 3));
            if (channels > 1)
                in.push_back (pinkNoise (0.1, length, 4));
            auto engine = makeEngine (rate, channels, settings (0, 0));
            const int latency = engine->latencySamples();
            const auto out = render (*engine, in);
            bool exact = true;
            for (int c = 0; c < channels; ++c)
                for (int n = latency; n < length; ++n)
                    exact = exact && out[(size_t) c][(size_t) n] == in[(size_t) c][(size_t) (n - latency)];
            ++checks;
            if (! exact)
                ++failures;
            std::printf ("    %s all-zero controls are bit-exact at %6.0f Hz, %d ch, latency %d = %.1f ms\n", exact ? "ok  " : "FAIL", rate, channels, latency, 1000.0 * latency / rate);
        }
    }
}

static void testBiasBrightenNeedRecoverOrTame()
{
    Channels in { pinkNoise (0.1, 96000, 5), pinkNoise (0.1, 96000, 6) };
    auto a = makeEngine (kRate, 2, settings (0, 0));
    auto b = makeEngine (kRate, 2, settings (0, 0, 100, -100, 0));
    const auto outA = render (*a, in);
    const auto outB = render (*b, in);
    bool same = outA[0] == outB[0] && outA[1] == outB[1];
    check (same, "Bias and Brighten do nothing while Recover and Tame are zero");
}

static void testLatencyAndMinimumPhase()
{
    // A silent sidechain keeps the gains perfectly static, so the wet path can
    // be measured as an ordinary filter.
    for (float boost : { 12.0f, 40.0f, -30.0f })
    {
        EngineParameters p = settings (0, 0, 0, 0, boost);
        p.useSidechain = true;
        auto engine = makeEngine (kRate, 1, p);
        const int latency = engine->latencySamples();
        const int length = 48000;

        Channels silence { Buffer ((size_t) length, 0.0f) };
        Channels impulse { Buffer ((size_t) length, 0.0f) };
        const int at = 24000;
        impulse[0][(size_t) at] = 1.0f;
        const auto out = render (*engine, impulse, &silence);

        double before = 1.0e-30, total = 1.0e-30, peak = 0.0;
        int peakAt = 0;
        for (int n = at - 4000; n < at + 8000; ++n)
        {
            const double v = out[0][(size_t) n];
            const double power = v * v;
            total += power;
            if (n < at + latency)
                before += power;
            if (std::abs (v) > peak)
            {
                peak = std::abs (v);
                peakAt = n;
            }
        }
        atMost (std::abs (peakAt - (at + latency)), 8, "the impulse response peaks at the reported latency (samples)");
        atMost (10.0 * std::log10 (before / total), -55.0, "nothing comes out before it: no pre-ringing (dB of its energy)");

        // Minimum phase: the measured phase is the Hilbert transform of the log
        // magnitude. The real cepstrum of a minimum-phase response is causal, so
        // nothing should be left in its anti-causal half.
        const int size = 8192;
        SplitFft fft;
        fft.prepare (size);
        std::vector<float> re ((size_t) size, 0.0f), im ((size_t) size, 0.0f);
        for (int n = 0; n < size / 2; ++n)
            re[(size_t) n] = out[0][(size_t) (at + latency + n)];
        fft.forward (re.data(), im.data());
        std::vector<float> logRe ((size_t) size), logIm ((size_t) size);
        double previous = 0.0, unwrap = 0.0;
        for (int b = 0; b <= size / 2; ++b)
        {
            const double phase = std::atan2 ((double) im[(size_t) b], (double) re[(size_t) b]);
            double d = phase - previous;
            d -= 2.0 * kPi * std::round (d / (2.0 * kPi));
            unwrap += d;
            previous = phase;
            logRe[(size_t) b] = (float) std::log (std::hypot ((double) re[(size_t) b], (double) im[(size_t) b]) + 1.0e-9);
            logIm[(size_t) b] = (float) unwrap;
            if (b > 0 && b < size / 2)
            {
                logRe[(size_t) (size - b)] = logRe[(size_t) b];
                logIm[(size_t) (size - b)] = -logIm[(size_t) b];
            }
        }
        logIm[(size_t) (size / 2)] = 0.0f;
        fft.forward (logIm.data(), logRe.data());
        double causal = 1.0e-30, anticausal = 1.0e-30;
        for (int n = 1; n < size / 2; ++n)
        {
            causal += (double) logRe[(size_t) n] * logRe[(size_t) n];
            anticausal += (double) logRe[(size_t) (size - n)] * logRe[(size_t) (size - n)];
        }
        atMost (10.0 * std::log10 (anticausal / causal), -30.0, "EQ is minimum phase: anti-causal cepstrum energy (dB)");
    }

    // The graph must tell the truth.
    EngineParameters p = settings (0, 0, 0, 0, 12);
    p.useSidechain = true;
    const int length = 48000;
    Channels silence { Buffer ((size_t) length, 0.0f) };
    for (double hz : { 150.0, 1000.0, 5000.0, 12000.0 })
    {
        auto e = makeEngine (kRate, 1, p);
        Channels tone { sine (hz, 0.1, length, kRate) };
        const auto toneOut = render (*e, tone, &silence);
        const double measured = toneDb (toneOut[0], hz, kRate, 12000, length) - toneDb (tone[0], hz, kRate, 12000, length);
        const double shown = e->curveDbAt (hz);
        atMost (std::abs (measured - shown), 0.25, "EQ graph matches the measured gain (dB error)");

        const int nearest = (int) std::lround ((kDisplayPoints - 1) * std::log (hz / 20.0) / std::log (1000.0));
        const double published = e->meters().curveDb[(size_t) nearest].load();
        atMost (std::abs (published - shown), 0.5, "the copy the UI reads agrees (dB error)");
    }
}

static void testBlockSizeIndependence()
{
    const auto demo = makeDemoMix (kRate, 3.0, 2);
    auto reference = makeEngine (kRate, 2, settings (60, 60, 10, -10, 2));
    const auto a = render (*reference, demo.mix, nullptr, 512);
    for (int block : { 1, 37, 64, 128, 1000, 4096 })
    {
        auto engine = makeEngine (kRate, 2, settings (60, 60, 10, -10, 2));
        const auto b = render (*engine, demo.mix, nullptr, block);
        const std::string label = "block size " + std::to_string (block) + " changes nothing (difference, dB)";
        atMost (differenceDb (a[0], b[0], 0, (int) a[0].size()), -130.0, label.c_str());
    }
}

static void testResetIsDeterministic()
{
    const auto demo = makeDemoMix (kRate, 2.0, 2);
    auto engine = makeEngine (kRate, 2, settings (80, 40));
    const auto a = render (*engine, demo.mix);
    engine->reset();
    const auto b = render (*engine, demo.mix);
    atMost (differenceDb (a[0], b[0], 0, (int) a[0].size()), -130.0, "reset() returns to exactly the same state (difference, dB)");
}

static void testStemsNullThroughSidechain()
{
    // The stem mastering promise: stems processed with the sum on the sidechain
    // add up to what processing the sum would have given.
    const auto demo = makeDemoMix (kRate, 4.0, 2);
    EngineParameters p = settings (70, 70, 15, 20, 3);
    p.useSidechain = true;

    auto whole = makeEngine (kRate, 2, p);
    const auto mixOut = render (*whole, demo.mix, &demo.mix);

    Channels sum (2, Buffer (demo.mix[0].size(), 0.0f));
    for (const auto& stem : demo.stems)
    {
        auto engine = makeEngine (kRate, 2, p);
        const auto stemOut = render (*engine, stem, &demo.mix);
        for (int c = 0; c < 2; ++c)
            for (size_t n = 0; n < sum[0].size(); ++n)
                sum[(size_t) c][n] += stemOut[(size_t) c][n];
    }
    const double level = rmsDb (mixOut[0]);
    atMost (differenceDb (mixOut[0], sum[0], 0, (int) sum[0].size()) - level, -90.0, "sum of processed stems nulls against the processed mix (dB)");
    atMost (differenceDb (mixOut[1], sum[1], 0, (int) sum[0].size()) - level, -90.0, "same on the right channel (dB)");

    // Also: with the sidechain on, the gains follow the sidechain alone.
    p.boostDb = 0;
    auto first = makeEngine (kRate, 2, p);
    auto second = makeEngine (kRate, 2, p);
    Channels quiet = demo.stems[0];
    render (*first, quiet, &demo.mix);
    render (*second, demo.stems[2], &demo.mix);
    double worst = 0.0;
    for (int k = 0; k < first->numBands(); ++k)
        worst = std::max (worst, (double) std::abs (first->stateGainDb()[k] - second->stateGainDb()[k]));
    atMost (worst, 1.0e-3, "gains ignore the main input while the sidechain is on (dB)");
}

static void testLoudnessAndDynamics()
{
    const auto demo = makeDemoMix (kRate, 14.0, 2);
    const int start = (int) kRate * 2;
    const int end = (int) kRate * 13;
    const double lufsIn = integratedLufs (demo.mix, kRate, start, end);

    const auto contourIn = loudnessContour (demo.mix, kRate, 0.4, start, end);
    const auto fastIn = loudnessContour (demo.mix, kRate, 0.05, start, end);

    // Loudness here is BS.1770, which isn't what the reference keeps constant: it
    // moved 0.4 to 1.6 LU at 50/50 and up to 2.5 LU on Boost in its own renders.
    struct Case
    {
        const char* name;
        EngineParameters p;
        double limit;
    };
    const Case cases[] = {
        { "Recover 15 Tame 15", settings (15, 15), 0.75 },
        { "Recover 50 Tame 50", settings (50, 50), 2.5 },
        { "Recover 100 Tame 100", settings (100, 100), 4.5 },
        { "Recover 100", settings (100, 0), 2.5 },
        { "Tame 100", settings (0, 100), 2.5 },
        { "Boost +15", settings (0, 0, 0, 0, 15), 2.5 },
        { "Boost -15", settings (0, 0, 0, 0, -15), 2.5 },
        { "Recover 50 Tame 50 Brighten +50", settings (50, 50, 0, 50), 3.0 },
    };

    for (const auto& c : cases)
    {
        auto engine = makeEngine (kRate, 2, c.p);
        const auto out = alignOutput (render (*engine, demo.mix), engine->latencySamples());
        const double delta = integratedLufs (out, kRate, start, end) - lufsIn;

        const auto contourOut = loudnessContour (out, kRate, 0.4, start, end);
        const auto fastOut = loudnessContour (out, kRate, 0.05, start, end);

        const auto deviation = [] (const std::vector<double>& a, const std::vector<double>& b) {
            double mean = 0.0, squares = 0.0;
            const size_t n = std::min (a.size(), b.size());
            for (size_t i = 0; i < n; ++i)
            {
                const double d = b[i] - a[i];
                mean += d;
                squares += d * d;
            }
            mean /= (double) n;
            return std::sqrt (std::max (0.0, squares / (double) n - mean * mean));
        };

        std::printf ("  %s\n", c.name);
        atMost (std::abs (delta), c.limit, "programme loudness change (LU)");
        atMost (deviation (contourIn, contourOut), 1.2, "wobble of the 400 ms loudness contour (dB)");
        atMost (deviation (fastIn, fastOut), 2.2, "wobble of the 50 ms loudness contour (dB)");
    }
}

static void testTransientsSurvive()
{
    const auto demo = makeDemoMix (kRate, 8.0, 2);
    const int start = (int) kRate;
    const int end = (int) kRate * 7;

    const auto crest = [&] (const Buffer& x) { return peakDb (x, start, end) - rmsDb (x, start, end); };
    const double crestIn = crest (demo.mix[0]);
    auto engine = makeEngine (kRate, 2, settings (50, 50));
    const auto out = alignOutput (render (*engine, demo.mix), engine->latencySamples());
    atMost (std::abs (crest (out[0]) - crestIn), 1.5, "crest factor of the mix changes little at 50/50 (dB)");
}

static void testMaskedToneUncovered()
{
    // The manual's own example. One tone masks a quieter one above it.
    const int length = (int) (kRate * 4.0);
    const double lowHz = 800.0, highHz = 1200.0;
    Channels in { Buffer ((size_t) length) };
    const auto loud = sine (lowHz, 0.5, length, kRate);
    const auto soft = sine (highHz, 0.5 * std::pow (10.0, -32.0 / 20.0), length, kRate);
    for (int n = 0; n < length; ++n)
        in[0][(size_t) n] = loud[(size_t) n] + soft[(size_t) n];

    const int from = (int) kRate, to = length - 4096;
    const double ratioIn = toneDb (in[0], highHz, kRate, from, to) - toneDb (in[0], lowHz, kRate, from, to);

    const auto run = [&] (EngineParameters p) {
        auto engine = makeEngine (kRate, 1, p);
        const auto out = alignOutput (render (*engine, in), engine->latencySamples());
        struct R
        {
            double ratio, low, high;
        };
        return R { toneDb (out[0], highHz, kRate, from, to) - toneDb (out[0], lowHz, kRate, from, to),
                   toneDb (out[0], lowHz, kRate, from, to) - toneDb (in[0], lowHz, kRate, from, to),
                   toneDb (out[0], highHz, kRate, from, to) - toneDb (in[0], highHz, kRate, from, to) };
    };

    const auto both = run (settings (100, 100));
    std::printf ("    both: masked tone %+.1f dB, masker %+.1f dB\n", both.high, both.low);
    atLeast (both.ratio - ratioIn, 3.0, "Recover and Tame move the masked tone out of the masker (dB)");

    const auto recoverOnly = run (settings (100, 0));
    atLeast (recoverOnly.high - recoverOnly.low, 1.5, "Recover alone lifts the masked tone relative to the masker (dB)");

    const auto tameOnly = run (settings (0, 100));
    atLeast (tameOnly.high - tameOnly.low, 0.5, "Tame alone pulls the masker down relative to the masked tone (dB)");

    const auto darker = run (settings (100, 100, 0, -100));
    std::printf ("    Brighten -100: masked tone %+.1f dB, masker %+.1f dB\n", darker.high, darker.low);
    atLeast (darker.ratio - ratioIn, 0.5, "the tone is still uncovered at Brighten -100 (dB)");

    // The range of balances in the manual's example: with two tones further
    // apart, Brighten decides which of them does the moving.
    const double farHz = 5000.0;
    Channels wide { Buffer ((size_t) length) };
    const auto softFar = sine (farHz, 0.5 * std::pow (10.0, -40.0 / 20.0), length, kRate);
    for (int n = 0; n < length; ++n)
        wide[0][(size_t) n] = loud[(size_t) n] + softFar[(size_t) n];
    const auto balance = [&] (float brighten) {
        auto engine = makeEngine (kRate, 1, settings (100, 100, 0, brighten));
        const auto out = alignOutput (render (*engine, wide), engine->latencySamples());
        return toneDb (out[0], farHz, kRate, from, to) - toneDb (out[0], lowHz, kRate, from, to);
    };
    const double dark = balance (-100), neutral = balance (0), bright = balance (100);
    std::printf ("    high tone over low tone: Brighten -100 %+.1f, 0 %+.1f, +100 %+.1f dB\n", dark, neutral, bright);
    atLeast (neutral - dark, 1.0, "Brighten down favours the low tone (dB)");
    atLeast (bright - neutral, 1.0, "Brighten up favours the high tone (dB)");
}

static void testNeutralOnAloneSignals()
{
    const int length = (int) (kRate * 3.0);
    struct Amount
    {
        float percent;
        double level, clean;
    };
    // The 100% limits only guard against this getting worse. No render has measured a
    // lone sine, and the fitted field pulls one down at twice the measured amounts.
    for (const auto& amount : { Amount { 15.0f, 1.5, -48.0 }, Amount { 100.0f, 8.5, -33.0 } })
    {
        for (double hz : { 110.0, 1000.0, 6000.0 })
        {
            Channels in { sine (hz, 0.3, length, kRate) };
            auto engine = makeEngine (kRate, 1, settings (amount.percent, amount.percent));
            const auto out = alignOutput (render (*engine, in), engine->latencySamples());
            const int from = (int) kRate, to = length - 4096;
            atMost (std::abs (toneDb (out[0], hz, kRate, from, to) - toneDb (in[0], hz, kRate, from, to)), amount.level, "a lone sine keeps most of its level (dB)");

            // Everything further than 20 Hz from the tone, measured with a Hann
            // window over a long transform.
            const int size = 1 << 16;
            SplitFft fft;
            fft.prepare (size);
            std::vector<float> re ((size_t) size), im ((size_t) size, 0.0f);
            for (int n = 0; n < size; ++n)
                re[(size_t) n] = out[0][(size_t) (from + n)] * (float) (0.5 - 0.5 * std::cos (2.0 * kPi * n / size));
            fft.forward (re.data(), im.data());
            double inside = 1.0e-30, outside = 1.0e-30;
            for (int b = 1; b < size / 2; ++b)
            {
                const double power = (double) re[(size_t) b] * re[(size_t) b] + (double) im[(size_t) b] * im[(size_t) b];
                if (std::abs (b * kRate / size - hz) <= 20.0)
                    inside += power;
                else
                    outside += power;
            }
            atMost (10.0 * std::log10 (outside / inside), amount.clean, "a lone sine stays clean: everything else, dB below it");
        }
    }

    Channels noise { pinkNoise (0.1, (int) (kRate * 8.0), 11), pinkNoise (0.1, (int) (kRate * 8.0), 12) };
    auto engine = makeEngine (kRate, 2, settings (100, 100));
    render (*engine, noise);
    double worst = 0.0;
    for (int k = 2; k < engine->numBands() - 3; ++k)
        worst = std::max (worst, (double) std::abs (engine->stateGainDb()[k]));
    atMost (worst, 6.0, "pink noise is not reshaped: largest band gain at 100/100 (dB)");
}

static void testRecoverTameBiasSemantics()
{
    const auto demo = makeDemoMix (kRate, 5.0, 2);
    const auto meters = [&] (EngineParameters p) {
        auto engine = makeEngine (kRate, 2, p);
        render (*engine, demo.mix);
        struct M
        {
            float recover, tame, bias;
        };
        return M { engine->meters().recoverDb.load(), engine->meters().tameDb.load(), engine->meters().biasBalance.load() };
    };

    const auto r = meters (settings (100, 0));
    atLeast (r.recover, 0.2, "Recover alone shows on the Recover meter (dB)");
    atMost (std::abs (r.tame), 1.0e-3, "Recover alone never cuts (Tame meter, dB)");

    const auto t = meters (settings (0, 100));
    atMost (t.tame, -0.2, "Tame alone shows on the Tame meter (dB)");
    atMost (std::abs (t.recover), 1.0e-3, "Tame alone never lifts (Recover meter, dB)");

    double previous = -2.0;
    for (float bias : { -100.0f, -30.0f, 0.0f, 30.0f, 100.0f })
    {
        const auto b = meters (settings (50, 50, bias));
        check (b.bias > previous, "the Bias meter rises with the Bias control", b.bias, previous);
        previous = b.bias;
    }

    double lastBrightness = -100.0;
    for (float brighten : { -100.0f, -50.0f, 0.0f, 50.0f, 100.0f })
    {
        auto engine = makeEngine (kRate, 2, settings (50, 50, 0, brighten));
        render (*engine, demo.mix);
        const double b = engine->meters().brightnessShift.load();
        check (b > lastBrightness, "the brightness meter rises with Brighten", b, lastBrightness);
        lastBrightness = b;
    }
}

static void testBoost()
{
    const int length = (int) (kRate * 2.0);
    const auto curve = [&] (float boost, double hz) {
        EngineParameters p = settings (0, 0, 0, 0, boost);
        p.useSidechain = true;
        auto engine = makeEngine (kRate, 1, p);
        Channels silence { Buffer ((size_t) length, 0.0f) };
        Channels tone { sine (hz, 0.1, length, kRate) };
        const auto out = render (*engine, tone, &silence);
        return toneDb (out[0], hz, kRate, 24000, length) - toneDb (tone[0], hz, kRate, 24000, length);
    };

    atLeast (curve (15, 60) - curve (15, 3000), 3.0, "Boost up emphasises bass over mids (dB)");
    atMost (curve (-15, 60) - curve (-15, 3000), -3.0, "Boost down does the opposite (dB)");
    atLeast (curve (15, 60), 2.0, "the bass really goes up (dB)");
    atMost (curve (15, 3000), 0.5, "the mids don't (dB)");

    for (Mode mode : { Mode::basic, Mode::master })
        for (float boost : { -50.0f, 50.0f })
        {
            EngineParameters p = settings (0, 0, 0, 0, boost);
            p.mode = mode;
            auto engine = makeEngine (kRate, 2, p);
            const auto demo = makeDemoMix (kRate, 4.0, 2);
            const auto out = render (*engine, demo.mix);
            double peak = 0.0;
            for (float v : out[0])
                peak = std::max (peak, (double) std::abs (v));
            atMost (peak, 8.0, "extreme Boost stays finite and bounded (peak)");
        }

    // With signal present, the rest of the spectrum makes room for the lift. In the
    // reference, Master at +30 pulled the mids down about 3 to 5 dB and at +50 about
    // 11 to 13 dB, with the songs' own balance deciding which.
    const auto midGain = [&] (Mode mode, float boost) {
        EngineParameters p = settings (0, 0, 0, 0, boost);
        p.mode = mode;
        auto engine = makeEngine (kRate, 2, p);
        Channels noise { pinkNoise (0.1, (int) kRate * 4, 91), pinkNoise (0.1, (int) kRate * 4, 92) };
        render (*engine, noise);
        return (double) engine->curveDbAt (2000.0);
    };
    for (Mode mode : { Mode::basic, Mode::master })
    {
        const double at30 = midGain (mode, 30), at50 = midGain (mode, 50);
        atMost (at30, -2.0, "Boost +30 on noise pulls the mids down (dB)");
        atLeast (at30, -9.0, "but not by more than the reference does (dB)");
        atMost (at50, at30 - 2.0, "and Boost +50 pulls them further (dB)");
    }
}

static void testRangeLimiters()
{
    const auto demo = makeDemoMix (kRate, 3.0, 2);

    EngineParameters inside = settings (150, 150);
    inside.rangeLowHz = 300.0f;
    inside.rangeHighHz = 3000.0f;
    auto engine = makeEngine (kRate, 2, inside);
    render (*engine, demo.mix);
    const auto& layout = engine->bandLayout();

    double outside = 0.0, within = 0.0;
    for (int k = 0; k < layout.count; ++k)
    {
        const double hz = layout.hz[(size_t) k];
        const double g = std::abs (engine->stateGainDb()[k]);
        if (hz < 150.0 || hz > 6000.0)
            outside = std::max (outside, g);
        else if (hz > 400.0 && hz < 2500.0)
            within = std::max (within, g);
    }
    atMost (outside, 0.02, "nothing happens outside the limiters (dB)");
    atLeast (within, 0.3, "something happens between them (dB)");

    // Only at the edges: the demo mix's tones cut single bands deeply in between.
    double steepest = 0.0;
    for (int k = 1; k < layout.count; ++k)
    {
        const double hz = layout.hz[(size_t) k];
        if ((hz > 200.0 && hz < 420.0) || (hz > 2000.0 && hz < 4500.0))
            steepest = std::max (steepest, (double) std::abs (engine->stateGainDb()[k] - engine->stateGainDb()[k - 1]));
    }
    atMost (steepest, 6.5, "the transition is smooth (largest step between bands at the edges, dB)");

    EngineParameters reversed = settings (150, 150);
    reversed.rangeLowHz = 3000.0f;
    reversed.rangeHighHz = 300.0f;
    auto second = makeEngine (kRate, 2, reversed);
    render (*second, demo.mix);
    double excluded = 0.0, kept = 0.0;
    for (int k = 0; k < layout.count; ++k)
    {
        const double hz = layout.hz[(size_t) k];
        const double g = std::abs (second->stateGainDb()[k]);
        if (hz > 600.0 && hz < 1500.0)
            excluded = std::max (excluded, g);
        else if (hz < 200.0 || hz > 5000.0)
            kept = std::max (kept, g);
    }
    atMost (excluded, 0.02, "reversed limiters exclude the range between them (dB)");
    atLeast (kept, 0.3, "and process the rest (dB)");
}

static void testStereo()
{
    const int length = (int) (kRate * 3.0);
    const auto demo = makeDemoMix (kRate, 3.0, 1);
    Channels dual { demo.mix[0], demo.mix[0] };
    auto engine = makeEngine (kRate, 2, settings (80, 80, 0, 10, 3));
    const auto out = render (*engine, dual);
    atMost (differenceDb (out[0], out[1], 0, length) - rmsDb (out[0]), -80.0, "dual mono stays dual mono (L-R, dB below signal)");

    Channels oneSide { demo.mix[0], Buffer ((size_t) length, 0.0f) };
    auto second = makeEngine (kRate, 2, settings (80, 80));
    const auto panned = render (*second, oneSide);
    atMost (rmsDb (panned[1]) - rmsDb (panned[0]), -90.0, "a hard-panned signal stays on its side (dB)");

    // The reference applies one curve to both channels. So does Colgate, which
    // means that any level difference between them passes through unchanged.
    const auto wide = makeDemoMix (kRate, 4.0, 2);
    Channels scaled { wide.mix[0], wide.mix[0] };
    for (auto& v : scaled[1])
        v *= 0.25f;
    auto third = makeEngine (kRate, 2, settings (100, 100, 0, 20));
    const auto scaledOut = render (*third, scaled);
    Buffer rescaled = scaledOut[1];
    for (auto& v : rescaled)
        v *= 4.0f;
    atMost (differenceDb (scaledOut[0], rescaled, 0, (int) rescaled.size()) - rmsDb (scaledOut[0]), -70.0, "one curve for both channels: a level difference passes unchanged (dB below signal)");

    Channels mono { pinkNoise (0.1, length, 21) };
    auto fourth = makeEngine (kRate, 1, settings (60, 60));
    const auto monoOut = render (*fourth, mono);
    atMost (std::abs (rmsDb (monoOut[0], 48000, length) - rmsDb (mono[0], 48000, length)), 1.5, "mono works too (level change, dB)");
}

static void testRobustness()
{
    const auto allFinite = [] (const Channels& x) {
        for (const auto& c : x)
            for (float v : c)
                if (! std::isfinite (v))
                    return false;
        return true;
    };

    const EngineParameters extremes[] = { settings (200, 200, 100, 100, 50), settings (200, 200, -100, -100, -50),
                                          settings (200, 0, 100, -100, 50), settings (0, 200, -100, 100, -50) };

    for (double rate : { 8000.0, 11025.0, 16000.0, 22050.0, 32000.0, 44100.0, 48000.0, 64000.0, 88200.0, 96000.0, 176400.0, 192000.0, 352800.0, 384000.0 })
    {
        const int length = (int) (rate * 0.6);
        const auto demo = makeDemoMix (rate, 0.6, 2);

        Channels square (2, Buffer ((size_t) length));
        Channels dc (2, Buffer ((size_t) length, 0.8f));
        Channels silence (2, Buffer ((size_t) length, 0.0f));
        for (int n = 0; n < length; ++n)
            square[0][(size_t) n] = square[1][(size_t) n] = (n / 100) % 2 == 0 ? 1.0f : -1.0f;

        bool ok = true;
        for (auto p : extremes)
            for (Mode mode : { Mode::basic, Mode::master })
            {
                p.mode = mode;
                auto engine = makeEngine (rate, 2, p);
                ok = ok && allFinite (render (*engine, demo.mix));
                ok = ok && allFinite (render (*engine, square));
                ok = ok && allFinite (render (*engine, dc));
                ok = ok && allFinite (render (*engine, silence));
            }
        ++checks;
        if (! ok)
            ++failures;
        std::printf ("    %s extreme settings stay finite at %6.0f Hz\n", ok ? "ok  " : "FAIL", rate);
    }

    // Bad input must not poison the engine.
    const int length = 96000;
    Channels poison { pinkNoise (0.1, length, 31), pinkNoise (0.1, length, 32) };
    poison[0][20000] = std::numeric_limits<float>::quiet_NaN();
    poison[1][20001] = std::numeric_limits<float>::infinity();
    poison[0][20002] = -std::numeric_limits<float>::infinity();
    auto engine = makeEngine (kRate, 2, settings (100, 100));
    const auto out = render (*engine, poison);
    check (allFinite (out), "NaN and infinity on the input never reach the output");
    atLeast (rmsDb (out[0], 60000, length), -40.0, "and the audio keeps flowing afterwards (dB)");

    // Long silence must not run into denormal slowdowns.
    {
        auto idle = makeEngine (kRate, 2, settings (100, 100));
        Channels burst { pinkNoise (0.1, 48000, 41), pinkNoise (0.1, 48000, 42) };
        render (*idle, burst);
        Channels quiet (2, Buffer (480000, 0.0f));
        const auto t0 = std::chrono::steady_clock::now();
        render (*idle, quiet);
        const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        atLeast (10.0 / seconds, 5.0, "ten seconds of decay into silence stays fast (times realtime)");
    }
}

static void testRealtimeSafety()
{
    const auto demo = makeDemoMix (kRate, 2.0, 2);
    auto engine = makeEngine (kRate, 2, settings (100, 100, 20, 20, 5));
    Channels work = demo.mix;
    const long before = allocationCount.load();
    render (*engine, work);
    // render() itself copied the buffers once by value before the count started
    // being relevant, so compare only the processing call pattern.
    const long allocations = allocationCount.load() - before;
    atMost ((double) allocations, 3.0, "no allocation inside process(), only the test's own buffer copies");

    EngineParameters moved = settings (20, 180, -40, 40, -12);
    moved.rangeLowHz = 100.0f;
    moved.rangeHighHz = 8000.0f;
    engine->setParameters (moved);
    const long middle = allocationCount.load();
    float* main[2] = { work[0].data(), work[1].data() };
    engine->process (main, nullptr, 1024);
    atMost ((double) (allocationCount.load() - middle), 0.0, "moving every control allocates nothing");
}

static void testQualityModes()
{
    const auto demo = makeDemoMix (kRate, 4.0, 2);
    auto normal = makeEngine (kRate, 2, settings (50, 50), Quality::normal);
    auto better = makeEngine (kRate, 2, settings (50, 50), Quality::better);
    auto best = makeEngine (kRate, 2, settings (50, 50), Quality::best);
    check (normal->latencySamples() == best->latencySamples(), "quality never changes the latency");

    const auto timed = [&] (ColgateEngine& e) {
        const auto t0 = std::chrono::steady_clock::now();
        auto out = render (e, demo.mix);
        std::printf ("    %.1f times realtime\n", 4.0 / std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
        return out;
    };
    const auto a = timed (*normal);
    const auto b = timed (*better);
    const auto c = timed (*best);
    const int n = (int) a[0].size();
    const double level = rmsDb (a[0], 48000, n);
    atMost (differenceDb (a[0], b[0], 48000, n) - level, -25.0, "Better is close to Normal: difference (dB)");
    atMost (differenceDb (b[0], c[0], 48000, n) - level, -25.0, "Best is close to Better: difference (dB)");
}

static void testModes()
{
    const auto demo = makeDemoMix (kRate, 6.0, 2);
    const auto gains = [&] (Mode mode, float amount, float bias = 0.0f, float brighten = 0.0f) {
        EngineParameters p = settings (amount, amount, bias, brighten);
        p.mode = mode;
        auto engine = makeEngine (kRate, 2, p);
        std::vector<double> average ((size_t) engine->numBands(), 0.0);
        Channels work = demo.mix;
        int frames = 0;
        const int block = 128;
        for (int s = 0; s + block <= (int) work[0].size(); s += block)
        {
            float* main[2] = { work[0].data() + s, work[1].data() + s };
            engine->process (main, nullptr, block);
            if (s < (int) kRate)
                continue;
            for (int k = 0; k < engine->numBands(); ++k)
                average[(size_t) k] += engine->stateGainDb()[k];
            ++frames;
        }
        for (auto& v : average)
            v /= frames;
        return average;
    };

    const auto correlation = [] (const std::vector<double>& a, const std::vector<double>& b) {
        double ma = 0, mb = 0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            ma += a[i];
            mb += b[i];
        }
        ma /= (double) a.size();
        mb /= (double) a.size();
        double ab = 0, aa = 0, bb = 0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            ab += (a[i] - ma) * (b[i] - mb);
            aa += (a[i] - ma) * (a[i] - ma);
            bb += (b[i] - mb) * (b[i] - mb);
        }
        return ab / std::sqrt (aa * bb + 1.0e-30);
    };

    const auto basic = gains (Mode::basic, 50);
    const auto master = gains (Mode::master, 50);
    double largest = 0.0, difference = 0.0;
    for (size_t k = 0; k < basic.size(); ++k)
    {
        largest = std::max (largest, std::abs (basic[k]));
        difference = std::max (difference, std::abs (basic[k] - master[k]));
    }
    atLeast (correlation (basic, master), 0.97, "Master has the same overall shape as Basic (correlation)");
    atLeast (difference, 0.05, "but they are different curves (largest difference, dB)");
    atMost (difference, 1.5, "by a fine margin (largest difference, dB)");

    // Measured in the reference: gain grows with amount, a little faster than
    // proportionally at low settings.
    for (Mode mode : { Mode::basic, Mode::master })
    {
        const auto small = gains (mode, 15);
        const auto big = gains (mode, 50);
        double ab = 0, bb = 1.0e-30;
        for (size_t k = 0; k < small.size(); ++k)
        {
            ab += small[k] * big[k];
            bb += big[k] * big[k];
        }
        const double ratio = ab / bb;
        atLeast (ratio, 0.22, "15/15 moves about a third as much as 50/50 (ratio)");
        atMost (ratio, 0.40, "15/15 moves about a third as much as 50/50 (ratio)");

        // Also measured: with Recover equal to Tame, Bias only relabels the bands.
        const auto biased = gains (mode, 50, 40.0f);
        double worst = 0.0;
        for (size_t k = 0; k < big.size(); ++k)
            worst = std::max (worst, std::abs (big[k] - biased[k]));
        atMost (worst, 0.01, "with Recover equal to Tame, Bias leaves the curve alone (dB)");
    }

    // Master always updates at the Best rate, whatever Quality says.
    EngineParameters p = settings (50, 50);
    p.mode = Mode::master;
    auto engine = makeEngine (kRate, 2, p, Quality::normal);
    check (engine->activeOverlapFactor() == 4, "Master always runs at the Best overlap");
    auto second = makeEngine (kRate, 2, settings (50, 50), Quality::normal);
    check (second->activeOverlapFactor() == 1, "Basic follows the Quality setting");
    check (engine->latencySamples() == second->latencySamples(), "both modes have the same latency");

    // Switching modes while playing must not click, even though the overlap changes.
    const int length = 96000;
    Channels tone { sine (440.0, 0.25, length, kRate), sine (440.0, 0.25, length, kRate) };
    EngineParameters state = settings (60, 60);
    auto toggled = makeEngine (kRate, 2, state);
    float biggest = 0.0f, last = 0.0f;
    int switches = 0;
    for (int s = 0; s + 256 <= length; s += 256)
    {
        if (s > 24000 && (s / 256) % 30 == 0)
        {
            state.mode = state.mode == Mode::basic ? Mode::master : Mode::basic;
            toggled->setParameters (state);
            ++switches;
        }
        float* main[2] = { tone[0].data() + s, tone[1].data() + s };
        toggled->process (main, nullptr, 256);
        for (int n = 0; n < 256; ++n)
        {
            if (s > 12000)
                biggest = std::max (biggest, std::abs (tone[0][(size_t) (s + n)] - last));
            last = tone[0][(size_t) (s + n)];
        }
    }
    const float own = (float) (0.25 * 2.0 * kPi * 440.0 / kRate);
    atLeast (switches, 5, "the mode really was switched while playing");
    atMost (biggest / own, 1.8, "switching modes doesn't click (largest step vs the sine's own)");
}

static void testRecoverAndTameAdd()
{
    // The clearest thing the renders showed: 50/50 is the sum of 50/0 and 0/50.
    const auto demo = makeDemoMix (kRate, 4.0, 2);
    for (Mode mode : { Mode::basic, Mode::master })
    {
        const auto gains = [&] (float recover, float tame) {
            EngineParameters p = settings (recover, tame);
            p.mode = mode;
            auto engine = makeEngine (kRate, 2, p);
            render (*engine, demo.mix);
            return std::vector<float> (engine->stateGainDb(), engine->stateGainDb() + engine->numBands());
        };
        const auto both = gains (50, 50), recover = gains (50, 0), tame = gains (0, 50);
        double squares = 0.0;
        for (size_t k = 0; k < both.size(); ++k)
        {
            const double d = (double) both[k] - recover[k] - tame[k];
            squares += d * d;
        }
        atMost (std::sqrt (squares / (double) both.size()), 0.15, "50/50 is the sum of 50/0 and 0/50 (dB, RMS over bands)");

        // Each lifts some of the spectrum and cuts the rest, to keep its balance.
        const auto count = [] (const std::vector<float>& v, bool up) {
            int n = 0;
            for (float g : v)
                n += up ? (g > 0.2f) : (g < -0.2f);
            return n;
        };
        atLeast (count (recover, true), 3, "Recover alone lifts some bands");
        atLeast (count (recover, false), 3, "and dips others to hold the balance");
        atLeast (count (tame, true), 3, "Tame alone lifts some bands to hold the balance");
        atLeast (count (tame, false), 3, "and cuts others");
    }

    // What the broadband gain holds constant.
    auto engine = makeEngine (kRate, 2, settings (50, 50));
    render (*engine, demo.mix);
    const float* energy = engine->smoothedEnergy();
    const float* gain = engine->stateGainDb();
    double before = 0.0, after = 0.0;
    for (int k = 0; k < engine->numBands(); ++k)
    {
        before += std::pow ((double) energy[k], 0.35);
        after += std::pow ((double) energy[k] * std::pow (10.0, gain[k] / 10.0), 0.35);
    }
    atMost (std::abs (10.0 * std::log10 (after / before)) / 0.35, 1.0, "the sum over bands of power^0.35 holds (dB of broadband gain)");

    // And with the limiters in, Bias only acts through a difference.
    const auto curve = [&] (float recover, float tame, float bias) {
        auto e = makeEngine (kRate, 2, settings (recover, tame, bias));
        render (*e, demo.mix);
        return std::vector<float> (e->stateGainDb(), e->stateGainDb() + e->numBands());
    };
    const auto a = curve (80, 20, 0), b = curve (80, 20, 60);
    double moved = 0.0;
    for (size_t k = 0; k < a.size(); ++k)
        moved = std::max (moved, (double) std::abs (a[k] - b[k]));
    atLeast (moved, 0.3, "with Recover and Tame apart, Bias does move the curve (dB)");
}

static void testPhaseSpaceResolution()
{
    // Two partials closer than a Hann spectrogram of the same span can separate.
    const double rate = 48000.0;
    const double low = 1000.0, high = 1188.0;
    const int length = 24000;
    Channels tone { Buffer ((size_t) length) };
    const auto a = sine (low, 0.3, length, rate);
    const auto b = sine (high, 0.3, length, rate, 1.0);
    for (int n = 0; n < length; ++n)
        tone[0][(size_t) n] = a[(size_t) n] + b[(size_t) n];

    auto engine = makeEngine (rate, 1, settings (0, 0));
    const float* slice = nullptr;
    std::vector<double> averaged;
    const int block = 128;
    int frames = 0;
    for (int s = 0; s + block <= length; s += block)
    {
        float* main[1] = { tone[0].data() + s };
        engine->process (main, nullptr, block);
        slice = engine->analyzer().spectrum (1, 0);
        const int size = engine->analyzer().spectrumSize (1);
        if (averaged.empty())
            averaged.assign ((size_t) size, 0.0);
        if (s > 12000)
        {
            for (int i = 0; i < size; ++i)
                averaged[(size_t) i] += slice[i];
            ++frames;
        }
    }
    const double binHz = engine->analyzer().binHz (1);
    const auto at = [&] (double hz) { return averaged[(size_t) std::lround (hz / binHz)] / frames; };
    const double peakValue = 0.5 * (at (low) + at (high));
    const double valley = std::max (std::abs (at (0.5 * (low + high))), 1.0e-12);
    const double phaseSpaceDip = 10.0 * std::log10 (peakValue / valley);

    // The same 512-sample span as a Hann spectrogram.
    const int span = 512;
    const auto hannPower = [&] (double hz) {
        double re = 0, im = 0;
        for (int n = 0; n < span; ++n)
        {
            const double w = 0.5 * (1.0 - std::cos (2.0 * kPi * n / span));
            const double phase = 2.0 * kPi * hz * n / rate;
            re += w * tone[0][(size_t) (12000 + n)] * std::cos (phase);
            im += w * tone[0][(size_t) (12000 + n)] * std::sin (phase);
        }
        return re * re + im * im;
    };
    const double spectrogramDip = 10.0 * std::log10 (0.5 * (hannPower (low) + hannPower (high)) / std::max (hannPower (0.5 * (low + high)), 1.0e-12));
    std::printf ("    valley between partials 188 Hz apart: phase-space %.1f dB, Hann spectrogram %.1f dB\n", phaseSpaceDip, spectrogramDip);
    atLeast (phaseSpaceDip - spectrogramDip, 3.0, "the distribution resolves close partials better than a spectrogram (dB)");
}

static void testBypassAndGain()
{
    const int length = 96000;
    Channels in { sine (440.0, 0.25, length, kRate) };

    EngineParameters p = settings (80, 80, 0, 0, 6);
    p.gainDb = 6.0f;
    p.bypass = true;
    auto engine = makeEngine (kRate, 1, p);
    const int latency = engine->latencySamples();
    const auto out = render (*engine, in);
    atMost (differenceDb (Buffer (out[0].begin() + latency, out[0].end()), in[0], 24000, length - latency), -140.0, "Bypass is the exact delayed input, without Gain (dB)");

    EngineParameters g = settings (0, 0);
    g.gainDb = 6.0f;
    auto gained = makeEngine (kRate, 1, g);
    const auto gainedOut = alignOutput (render (*gained, in), latency);
    atMost (std::abs (rmsDb (gainedOut[0], 24000, length - latency) - rmsDb (in[0], 24000, length - latency) - 6.0), 0.01, "Gain +6 dB is +6 dB");

    // Toggling Bypass mid-stream must not click.
    Channels noisy { sine (440.0, 0.25, length, kRate) };
    auto toggled = makeEngine (kRate, 1, settings (60, 60, 0, 0, 4));
    float biggestStep = 0.0f;
    float last = 0.0f;
    EngineParameters state = settings (60, 60, 0, 0, 4);
    for (int s = 0; s + 256 <= length; s += 256)
    {
        if (s > 24000 && (s / 256) % 40 == 0)
        {
            state.bypass = ! state.bypass;
            toggled->setParameters (state);
        }
        float* main[1] = { noisy[0].data() + s };
        toggled->process (main, nullptr, 256);
        if (s > 12000)
            for (int n = 0; n < 256; ++n)
            {
                biggestStep = std::max (biggestStep, std::abs (noisy[0][(size_t) (s + n)] - last));
                last = noisy[0][(size_t) (s + n)];
            }
        else
            last = noisy[0][(size_t) (s + 255)];
    }
    const float sineStep = (float) (0.25 * 2.0 * kPi * 440.0 / kRate);
    atMost (biggestStep / sineStep, 1.6, "toggling Bypass doesn't click (largest step vs the sine's own)");
}

static void testAutomationIsSmooth()
{
    const int length = 96000;
    Channels in { sine (440.0, 0.3, length, kRate), sine (660.0, 0.3, length, kRate) };
    auto engine = makeEngine (kRate, 2, settings (0, 0));
    float last = 0.0f;
    float biggest = 0.0f;
    for (int s = 0; s + 64 <= length; s += 64)
    {
        const float t = (float) s / (float) length;
        engine->setParameters (settings (200.0f * t, 200.0f * (1.0f - t), 200.0f * t - 100.0f, 100.0f - 200.0f * t, 20.0f * std::sin (20.0f * t)));
        float* main[2] = { in[0].data() + s, in[1].data() + s };
        engine->process (main, nullptr, 64);
        if (s > 4096)
            for (int n = 0; n < 64; ++n)
            {
                biggest = std::max (biggest, std::abs (in[0][(size_t) (s + n)] - last));
                last = in[0][(size_t) (s + n)];
            }
        else
            last = in[0][(size_t) (s + 63)];
    }
    const float own = (float) (0.3 * 2.0 * kPi * 440.0 / kRate);
    atMost (biggest / own, 4.0, "wild automation stays free of clicks (largest step vs the sine's own)");
}

static void testSteadyInputMakesSteadyGains()
{
    // A held chord must not make the EQ wander, or it would be audible as a warble.
    const int length = (int) (kRate * 5.0);
    Channels chord { Buffer ((size_t) length, 0.0f) };
    for (double f : { 196.0, 246.94, 293.66, 392.0 })
        for (int h = 1; h <= 8; ++h)
        {
            const auto partial = sine (f * h, 0.12 / (h * h), length, kRate, h * 0.3);
            for (int n = 0; n < length; ++n)
                chord[0][(size_t) n] += partial[(size_t) n];
        }

    auto engine = makeEngine (kRate, 1, settings (200, 200));
    std::array<double, kMaxBands> sum {}, squares {};
    int frames = 0;
    for (int s = 0; s + 128 <= length; s += 128)
    {
        float* main[1] = { chord[0].data() + s };
        engine->process (main, nullptr, 128);
        if (s < (int) (kRate * 2.0))
            continue;
        for (int k = 0; k < engine->numBands(); ++k)
        {
            const double g = engine->stateGainDb()[k];
            sum[(size_t) k] += g;
            squares[(size_t) k] += g * g;
        }
        ++frames;
    }
    double worst = 0.0;
    for (int k = 0; k < engine->numBands(); ++k)
    {
        const double mean = sum[(size_t) k] / frames;
        worst = std::max (worst, std::sqrt (std::max (0.0, squares[(size_t) k] / frames - mean * mean)));
    }
    atMost (worst, 2.0, "a held chord at 200/200 gives steady gains: worst band wobble (dB)");
}

int main (int argc, char** argv)
{
    const Test tests[] = {
        { "FFT", testFft },
        { "ISO 226", testIso226 },
        { "Zero controls are an exact bypass", testZeroIsExactBypass },
        { "Bias and Brighten need Recover or Tame", testBiasBrightenNeedRecoverOrTame },
        { "Latency, minimum phase, truthful graph", testLatencyAndMinimumPhase },
        { "Block size independence", testBlockSizeIndependence },
        { "Determinism", testResetIsDeterministic },
        { "Stem mastering null through the sidechain", testStemsNullThroughSidechain },
        { "Loudness and dynamics are preserved", testLoudnessAndDynamics },
        { "Transients survive", testTransientsSurvive },
        { "A masked tone gets uncovered", testMaskedToneUncovered },
        { "Neutral on signals that mask nothing", testNeutralOnAloneSignals },
        { "Recover, Tame, Bias and Brighten mean what they say", testRecoverTameBiasSemantics },
        { "Boost follows the equal-loudness contours", testBoost },
        { "Frequency range limiters", testRangeLimiters },
        { "Stereo and mono", testStereo },
        { "Robustness", testRobustness },
        { "Realtime safety", testRealtimeSafety },
        { "Quality modes", testQualityModes },
        { "Basic and Master", testModes },
        { "Recover and Tame add up", testRecoverAndTameAdd },
        { "Phase-space resolution", testPhaseSpaceResolution },
        { "Bypass and Gain", testBypassAndGain },
        { "Automation", testAutomationIsSmooth },
        { "Steady input, steady gains", testSteadyInputMakesSteadyGains },
    };

    const char* filter = argc > 1 ? argv[1] : nullptr;
    for (const auto& test : tests)
    {
        if (filter != nullptr && std::strstr (test.name, filter) == nullptr)
            continue;
        std::printf ("\n%s\n", test.name);
        test.body();
    }

    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
