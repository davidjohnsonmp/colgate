// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The whole audio engine, free of any plug-in framework so it can be tested on
// its own. Signal flow:
//
//   main in ------------------------> minimum-phase EQ --> Gain --> out
//                                         ^
//   main or sidechain --> analyser --> field --> one gain curve
//
// The analyser never touches the audio. Both channels always share one curve.
// A dry path of the same latency is kept so Bypass and the all-zero state are
// exact.
#pragma once

#include "Fft.h"
#include "PerceptualField.h"
#include "PhaseSpaceAnalyzer.h"
#include "Psychoacoustics.h"

#include <array>
#include <atomic>
#include <vector>

namespace colgate
{

struct EngineParameters
{
    float recoverPercent = 0.0f;   // 0..200
    float tamePercent = 0.0f;      // 0..200
    float biasPercent = 0.0f;      // -100..100
    float brightenPercent = 0.0f;  // -100..100
    float boostDb = 0.0f;          // -50..50
    float gainDb = 0.0f;           // -24..24
    float rangeLowHz = 20.0f;
    float rangeHighHz = 20000.0f;
    bool useSidechain = false;
    bool bypass = false;
    Mode mode = Mode::basic;
};

enum class Quality
{
    normal = 1,
    better = 2,
    best = 4
};

// Points of the EQ curve published for the display, log-spaced 20 Hz to 20 kHz.
constexpr int kDisplayPoints = 160;

inline double displayPointHz (int index) noexcept
{
    return 20.0 * std::pow (1000.0, (double) index / (double) (kDisplayPoints - 1));
}

// Read from the UI thread. All values are safe to read at any time.
struct EngineMeters
{
    std::atomic<float> recoverDb { 0.0f };
    std::atomic<float> tameDb { 0.0f };
    std::atomic<float> biasBalance { 0.0f };     // -1 all Tame .. +1 all Recover
    std::atomic<float> brightnessShift { 0.0f }; // output minus input, in ERB
    std::array<std::atomic<float>, kDisplayPoints> curveDb;

    EngineMeters()
    {
        for (auto& a : curveDb)
            a.store (0.0f, std::memory_order_relaxed);
    }
};

class ColgateEngine
{
public:
    ColgateEngine();

    // Not realtime safe. Call before processing and whenever the format changes.
    void prepare (double sampleRate, int numChannels);

    // Clears all audio history. Realtime safe.
    void reset() noexcept;

    int latencySamples() const noexcept { return latency; }
    double tailSeconds() const noexcept { return (double) (fftSize - windowLength) / rate; }
    int numBands() const noexcept { return layout.count; }
    const BandLayout& bandLayout() const noexcept { return layout; }

    void setParameters (const EngineParameters& p) noexcept { target = p; }

    // Changes how often the EQ updates, never what it does. Master always runs at
    // Best. A change made while the EQ is running ducks to the delay path for a
    // few milliseconds, because two overlaps can't meet cleanly.
    void setQuality (Quality q) noexcept { pendingQuality = (int) q; }
    int activeOverlapFactor() const noexcept { return activeQuality; }

    // main: in-place buffers. sidechain: same channel count, or null.
    void process (float* const* main, const float* const* sidechain, int numSamples) noexcept;

    const EngineMeters& meters() const noexcept { return meterValues; }

    // The realised EQ curve in dB at any frequency. The UI reads the published
    // copy in meters() instead, since this one reads data prepare() rebuilds.
    float curveDbAt (double hz) const noexcept;

    // For tests and tools.
    const PhaseSpaceAnalyzer& analyzer() const noexcept { return phaseSpace; }
    const float* smoothedEnergy() const noexcept { return energy.data(); }
    const float* stateGainDb() const noexcept { return shapeDb.data(); }

private:
    using BandArray = std::array<float, kMaxBands>;

    void buildResponseBasis();
    void runHop() noexcept;
    void runAnalysisHop() noexcept;
    void updateRangeMask() noexcept;
    void updateBoostCurve (float boostDb) noexcept;
    int wantedQuality() const noexcept { return target.mode == Mode::master ? (int) Quality::best : pendingQuality; }
    void publishMeters() noexcept;
    void publishDisplayCurve() noexcept;
    void applyPendingQuality() noexcept;

    double rate = 48000.0;
    int channels = 2;

    // Time structure. See prepare() for how they relate to the sample rate.
    int scale = 1, scaleDiv = 1;
    int windowLength = 256;
    int cepstrumHalf = 1024;
    int fftSize = 2048;
    int baseHop = 128;
    int hop = 128;
    int hopsPerAnalysis = 1;
    int latency = 256;
    int pendingQuality = 1;
    int activeQuality = 1;

    BandLayout layout;
    PhaseSpaceAnalyzer phaseSpace;
    PerceptualField fieldStage;
    SplitFft fft;

    // Each band's log response (nepers per dB of gain) and the phase that makes
    // it minimum phase, sampled at a set of bins that follows the ERB scale.
    std::vector<int> knotBins;
    std::vector<float> basisLog, basisPhase; // band x knot
    std::vector<float> knotLog, knotPhase;
    std::vector<float> displayBasis;         // band x display point
    std::vector<float> responseRe, responseIm;
    int displayCounter = 0;

    std::vector<float> window;
    std::array<std::vector<float>, kMaxChannels> inputRing, outputRing, dryRing;
    std::vector<float> spectrumRe, spectrumIm;
    int inputPos = 0, hopPos = 0, hopCounter = 0, outputPos = 0, dryPos = 0;
    int inputMask = 0, outputMask = 0, dryMask = 0;

    // Analysis input, decimated to about 48 kHz for high sample rates.
    int analysisDecimation = 1;
    int analysisHop = 128;
    int analysisFill = 0;
    int analysisPhase = 0;
    double analysisRate = 48000.0;
    double hopSeconds = 128.0 / 48000.0;
    std::array<ButterworthLowpass, kMaxChannels> analysisLowpass;
    std::array<std::vector<float>, kMaxChannels> analysisBuffer;

    EngineParameters target;
    float smoothRecover = 0, smoothTame = 0, smoothBias = 0, smoothBrighten = 0, smoothBoost = 0;
    float parameterAlpha = 0.1f;
    float currentBoostCurve = 0.0f;
    float lastLow = -1.0f, lastHigh = -1.0f;
    bool lastActive = false;

    float wetMix = 0.0f, wetStep = 0.0f;
    float outputGain = 1.0f, gainStep = 0.0f;

    BandArray raw {}, energy {}, loudnessEnergy {}, weightedEnergy {}, shapeDb {}, gainTargetDb {}, gainNowDb {};
    BandArray rangeMask {}, boostCurve {}, specificIn {}, specificOut {};
    float gainSmoothAlpha = 0.5f;
    float attackAlpha[2] = { 0.05f, 0.05f }, releaseAlpha[2] = { 0.2f, 0.2f };
    float loudnessAttackAlpha[2] = { 0.05f, 0.05f }, loudnessReleaseAlpha[2] = { 0.2f, 0.2f };
    float boostKeepDb = 0.0f;
    int currentBoostMode = -1;
    int settleHops = 0;
    float meterAlpha = 0.05f, brightnessSmooth = 0.0f, recoverSmooth = 0.0f, tameSmooth = 0.0f, biasSmooth = 0.0f;
    bool energyPrimed = false;

    EngineMeters meterValues;
};

} // namespace colgate
