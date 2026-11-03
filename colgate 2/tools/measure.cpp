// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Prints what the engine does to the synthetic demo mix across settings. Used
// for tuning, and handy for anyone who changes the model.
#include "../tests/TestSignals.h"
#include "ColgateEngine.h"

#include <chrono>
#include <cstdio>
#include <memory>

using namespace colgate;
using namespace colgate_test;

namespace
{
struct Result
{
    std::vector<Buffer> out;
    std::array<float, kMaxBands> meanDb {}, spreadDb {};
    int bands = 0;
    double realtimeFactor = 0;
    float recoverDb = 0, tameDb = 0, bias = 0, bright = 0;
};

Result run (const std::vector<Buffer>& in, double rate, EngineParameters p, Quality q = Quality::normal)
{
    const int channels = (int) in.size();
    const int length = (int) in[0].size();
    auto engine = std::make_unique<ColgateEngine>();
    engine->setParameters (p);
    engine->prepare (rate, channels);
    engine->setQuality (q);
    engine->reset();

    Result r;
    r.out = in;
    r.bands = engine->numBands();
    std::array<double, kMaxBands> sum {}, squares {};
    int frames = 0;
    double elapsed = 0;

    const int block = 256;
    for (int s = 0; s < length; s += block)
    {
        const int n = std::min (block, length - s);
        float* ptr[2] = { r.out[0].data() + s, channels > 1 ? r.out[1].data() + s : nullptr };
        const auto t0 = std::chrono::steady_clock::now();
        engine->process (ptr, nullptr, n);
        elapsed += std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        if (s > rate)
        {
            for (int k = 0; k < r.bands; ++k)
            {
                const double g = engine->stateGainDb()[k];
                sum[(size_t) k] += g;
                squares[(size_t) k] += g * g;
            }
            ++frames;
        }
    }
    for (int k = 0; k < r.bands; ++k)
    {
        const double mean = sum[(size_t) k] / frames;
        r.meanDb[(size_t) k] = (float) mean;
        r.spreadDb[(size_t) k] = (float) std::sqrt (std::max (0.0, squares[(size_t) k] / frames - mean * mean));
    }
    r.realtimeFactor = length / rate / elapsed;
    r.recoverDb = engine->meters().recoverDb.load();
    r.tameDb = engine->meters().tameDb.load();
    r.bias = engine->meters().biasBalance.load();
    r.bright = engine->meters().brightnessShift.load();
    return r;
}

void report (const char* label, const std::vector<Buffer>& in, double rate, const EngineParameters& p)
{
    const auto r = run (in, rate, p);
    const int start = (int) rate * 2;
    const int end = (int) in[0].size() - 2048;

    // Align output to input before comparing.
    const int latency = 256;
    std::vector<Buffer> aligned;
    for (const auto& c : r.out)
        aligned.emplace_back (c.begin() + latency, c.end());

    const double lufsIn = integratedLufs (in, rate, start, end);
    const double lufsOut = integratedLufs (aligned, rate, start, end);

    std::printf ("%-26s dLUFS %+5.2f  rec %5.2f tame %6.2f bias %+5.2f bright %+5.2f  rt x%-5.0f\n   curve:",
                 label, lufsOut - lufsIn, r.recoverDb, r.tameDb, r.bias, r.bright, r.realtimeFactor);
    const float picks[] = { 40, 80, 160, 320, 640, 1250, 2500, 5000, 8000, 12000, 16000 };
    BandLayout layout;
    layout.prepare (rate);
    const auto nearest = [&layout] (float f) {
        int best = 0;
        float bestDistance = 1.0e9f;
        for (int k = 0; k < layout.count; ++k)
        {
            const float d = std::abs (layout.hz[(size_t) k] - f);
            if (d < bestDistance)
            {
                bestDistance = d;
                best = k;
            }
        }
        return best;
    };
    for (float f : picks)
        std::printf (" %+5.1f", r.meanDb[(size_t) nearest (f)]);
    std::printf ("\n   wobble:");
    for (float f : picks)
        std::printf (" %5.1f ", r.spreadDb[(size_t) nearest (f)]);
    std::printf ("\n");
}
} // namespace

int main()
{
    const double rate = 48000.0;
    const auto demo = makeDemoMix (rate, 10.0, 2);
    std::vector<Buffer> pink;
    pink.push_back (pinkNoise (0.1, (int) (rate * 10.0), 1));
    pink.push_back (pinkNoise (0.1, (int) (rate * 10.0), 2));

    std::printf ("bands at:      40    80   160   320   640  1k25  2k5   5k    8k   12k   16k\n");

    const auto make = [] (float r, float t, float bi, float br, float bo) {
        EngineParameters p;
        p.recoverPercent = r;
        p.tamePercent = t;
        p.biasPercent = bi;
        p.brightenPercent = br;
        p.boostDb = bo;
        return p;
    };

    std::printf ("--- demo mix ---\n");
    report ("R15 T15", demo.mix, rate, make (15, 15, 0, 0, 0));
    report ("R50 T50", demo.mix, rate, make (50, 50, 0, 0, 0));
    report ("R100 T100", demo.mix, rate, make (100, 100, 0, 0, 0));
    report ("R200 T200", demo.mix, rate, make (200, 200, 0, 0, 0));
    report ("R100 T0", demo.mix, rate, make (100, 0, 0, 0, 0));
    report ("R0 T100", demo.mix, rate, make (0, 100, 0, 0, 0));
    report ("R50 T50 bias+100", demo.mix, rate, make (50, 50, 100, 0, 0));
    report ("R50 T50 bias-100", demo.mix, rate, make (50, 50, -100, 0, 0));
    report ("R50 T50 bright+100", demo.mix, rate, make (50, 50, 0, 100, 0));
    report ("R50 T50 bright-100", demo.mix, rate, make (50, 50, 0, -100, 0));
    report ("boost+15", demo.mix, rate, make (0, 0, 0, 0, 15));
    report ("boost-15", demo.mix, rate, make (0, 0, 0, 0, -15));
    std::printf ("--- pink noise ---\n");
    report ("R50 T50", pink, rate, make (50, 50, 0, 0, 0));
    report ("R100 T100", pink, rate, make (100, 100, 0, 0, 0));
    std::printf ("--- demo mix, Master ---\n");
    auto master = make (50, 50, 0, 0, 0);
    master.mode = Mode::master;
    report ("R50 T50", demo.mix, rate, master);
    return 0;
}
