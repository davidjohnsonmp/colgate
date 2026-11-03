// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ColgateEngine.h"

#if defined(__SSE2__) || defined(_M_X64)
 #include <xmmintrin.h>
#endif

namespace colgate
{

namespace
{
constexpr double kTwoPi = 6.28318530717958647692;

// Tiny floating-point values slow down audio code badly on some CPUs. Hosts
// usually handle this, but the engine must also behave when driven directly.
class ScopedFlushToZero
{
public:
    ScopedFlushToZero() noexcept
    {
#if defined(__SSE2__) || defined(_M_X64)
        saved = _mm_getcsr();
        _mm_setcsr (saved | 0x8040);
#elif defined(__aarch64__)
        asm volatile ("mrs %0, fpcr" : "=r" (saved));
        asm volatile ("msr fpcr, %0" : : "r" (saved | (1ull << 24)));
#endif
    }
    ~ScopedFlushToZero() noexcept
    {
#if defined(__SSE2__) || defined(_M_X64)
        _mm_setcsr (saved);
#elif defined(__aarch64__)
        asm volatile ("msr fpcr, %0" : : "r" (saved));
#endif
    }

private:
#if defined(__SSE2__) || defined(_M_X64)
    unsigned int saved = 0;
#elif defined(__aarch64__)
    unsigned long long saved = 0;
#endif
};

inline float smoothStepRange (float x, float edge0, float edge1) noexcept
{
    const float t = std::max (0.0f, std::min (1.0f, (x - edge0) / (edge1 - edge0)));
    return t * t * (3.0f - 2.0f * t);
}

// One dB of amplitude gain, in nepers.
constexpr float kNepersPerDb = 0.1151292546f;

// How the brightness meter weighs bands: as specific loudness, Stevens's power law.
constexpr float kLoudnessExponent = 0.4f;
constexpr float kLoudnessFloor = 7.76e-11f; // threshold of hearing at 1 kHz, as power re full scale

// Boost keeps a different measure from Recover and Tame. In the reference, its
// large gains hold the sum of (band power)^0.5 steady across songs.
constexpr float kBoostLoudnessExponent = 0.5f;
constexpr float kBoostMeasuredAtDb = 30.0f;
constexpr float kBoostLimitDb = 40.0f; // above this, a quarter of the rate. Past anything measured.
constexpr float kBoostTrebleKnotZ[7] = { 26.0f, 30.0f, 33.0f, 35.0f, 37.0f, 38.5f, 40.3f };
constexpr double kBoostFlatHz = 1700.0;
} // namespace

ColgateEngine::ColgateEngine() = default;

void ColgateEngine::prepare (double sampleRate, int numChannels)
{
    rate = sampleRate;
    channels = std::max (1, std::min (numChannels, kMaxChannels));

    // Everything scales by a power of two from the 48 kHz layout, so time spans
    // stay about the same at any rate.
    const int exponent = (int) std::lround (std::log2 (sampleRate / 48000.0));
    if (exponent >= 0)
    {
        scale = 1 << exponent;
        scaleDiv = 1;
    }
    else
    {
        scale = 1;
        scaleDiv = 1 << std::min (2, -exponent);
    }

    windowLength = 256 * scale / scaleDiv;
    cepstrumHalf = 1024 * scale / scaleDiv;
    fftSize = 2048 * scale / scaleDiv;
    baseHop = 128 * scale / scaleDiv;

    // A minimum-phase filter has nothing before its peak, so one window is all
    // the delay the audio path needs.
    latency = windowLength;

    analysisDecimation = scale;
    analysisRate = sampleRate / analysisDecimation;
    analysisHop = baseHop / analysisDecimation;

    layout.prepare (analysisRate);

    hopSeconds = (double) baseHop / sampleRate;
    phaseSpace.prepare (analysisRate, analysisHop, layout, channels);
    fieldStage.prepare (layout, hopSeconds);
    fft.prepare (fftSize);

    window.resize ((size_t) windowLength);
    for (int n = 0; n < windowLength; ++n)
        window[(size_t) n] = (float) (0.5 * (1.0 - std::cos (kTwoPi * n / windowLength)));

    const auto powerOfTwoAbove = [] (int v) {
        int p = 1;
        while (p < v)
            p <<= 1;
        return p;
    };

    const int inputSize = powerOfTwoAbove (windowLength + 2 * baseHop);
    const int outputSize = powerOfTwoAbove (fftSize + 2 * baseHop);
    const int drySize = powerOfTwoAbove (latency + 1);
    inputMask = inputSize - 1;
    outputMask = outputSize - 1;
    dryMask = drySize - 1;

    for (int c = 0; c < kMaxChannels; ++c)
    {
        inputRing[(size_t) c].assign ((size_t) inputSize, 0.0f);
        outputRing[(size_t) c].assign ((size_t) outputSize, 0.0f);
        dryRing[(size_t) c].assign ((size_t) drySize, 0.0f);
        analysisBuffer[(size_t) c].assign ((size_t) analysisHop, 0.0f);
        if (analysisDecimation > 1)
            analysisLowpass[(size_t) c].design (0.42 * analysisRate, sampleRate);
    }
    spectrumRe.assign ((size_t) fftSize, 0.0f);
    spectrumIm.assign ((size_t) fftSize, 0.0f);
    responseRe.assign ((size_t) fftSize / 2 + 1, 1.0f);
    responseIm.assign ((size_t) fftSize / 2 + 1, 0.0f);

    buildResponseBasis();

    const auto alphaFor = [this] (double seconds) { return (float) (1.0 - std::exp (-hopSeconds / seconds)); };
    for (int m = 0; m < 2; ++m)
    {
        const auto& tuning = tuningFor ((Mode) m);
        attackAlpha[m] = alphaFor (tuning.attackSeconds);
        releaseAlpha[m] = alphaFor (tuning.releaseSeconds);
        loudnessAttackAlpha[m] = alphaFor (tuning.loudnessAttackSeconds);
        loudnessReleaseAlpha[m] = alphaFor (tuning.loudnessReleaseSeconds);
    }

    parameterAlpha = alphaFor (0.020);
    gainSmoothAlpha = alphaFor (0.005);
    meterAlpha = alphaFor (0.120);

    wetStep = 1.0f / (float) std::max (1.0, 0.012 * sampleRate);
    gainStep = 1.0f / (float) std::max (1.0, 0.020 * sampleRate);

    reset();
}

void ColgateEngine::buildResponseBasis()
{
    // Raised-cosine bumps on the ERB scale sum to exactly one everywhere, so any
    // curve is a weighted sum of them in the log domain. In that domain the phase
    // of a minimum-phase filter is linear too: it is the Hilbert transform of the
    // log magnitude, computed here by folding the cepstrum. So each band can carry
    // its own log magnitude and phase, and adding them up gives the exact filter.
    //
    // Limiting the cepstrum to cepstrumHalf keeps the impulse response inside the
    // transform. Its cost is that nothing narrower than about 45 Hz either side can
    // be formed in the deep bass. Longer gave no better match with the reference.
    const int bins = fftSize / 2 + 1;
    const float binHz = (float) rate / (float) fftSize;
    const int count = layout.count;

    knotBins.clear();
    double nextZ = -1.0;
    for (int b = 0; b < bins; ++b)
    {
        const double z = erbRate ((double) b * binHz);
        if (z >= nextZ || b == bins - 1)
        {
            knotBins.push_back (b);
            nextZ = z + 0.2;
        }
    }
    const int knots = (int) knotBins.size();

    basisLog.assign ((size_t) (count * knots), 0.0f);
    basisPhase.assign ((size_t) (count * knots), 0.0f);
    knotLog.assign ((size_t) knots, 0.0f);
    knotPhase.assign ((size_t) knots, 0.0f);
    displayBasis.assign ((size_t) (count * kDisplayPoints), 0.0f);

    std::vector<float> re ((size_t) fftSize), im ((size_t) fftSize);

    for (int k = 0; k < count; ++k)
    {
        std::fill (re.begin(), re.end(), 0.0f);
        std::fill (im.begin(), im.end(), 0.0f);

        for (int b = 0; b < bins; ++b)
        {
            const float z = (float) erbRate (std::max ((float) b * binHz, 0.1f));
            const float u = (z - layout.z[(size_t) k]) / layout.dz;
            float v = 0.0f;
            if ((k == 0 && u <= 0.0f) || (k == count - 1 && u >= 0.0f))
                v = 1.0f;
            else if (std::abs (u) < 1.0f)
                v = 0.5f * (1.0f + std::cos (3.14159265f * u));
            re[(size_t) b] = v;
            if (b > 0 && b < fftSize / 2)
                re[(size_t) (fftSize - b)] = v;
        }

        // To the cepstrum, keep the causal half with a soft limit, and back.
        fft.forward (im.data(), re.data());
        for (int n = 0; n < fftSize; ++n)
        {
            float v = 0.0f;
            if (n == 0)
                v = 1.0f;
            else if (n < cepstrumHalf)
                v = 2.0f * 0.5f * (1.0f + std::cos (3.14159265f * (float) n / (float) cepstrumHalf));
            re[(size_t) n] *= v / (float) fftSize;
            im[(size_t) n] = 0.0f;
        }
        fft.forward (re.data(), im.data());

        for (int i = 0; i < knots; ++i)
        {
            const size_t bin = (size_t) knotBins[(size_t) i];
            basisLog[(size_t) (k * knots + i)] = re[bin] * kNepersPerDb;
            basisPhase[(size_t) (k * knots + i)] = im[bin] * kNepersPerDb;
        }

        for (int i = 0; i < kDisplayPoints; ++i)
        {
            const double position = std::min ((double) (bins - 1), displayPointHz (i) / (double) binHz);
            const int bin = std::min (bins - 2, (int) position);
            const float t = (float) (position - bin);
            displayBasis[(size_t) (k * kDisplayPoints + i)] = (1.0f - t) * re[(size_t) bin] + t * re[(size_t) bin + 1];
        }
    }
}

void ColgateEngine::applyPendingQuality() noexcept
{
    activeQuality = wantedQuality();
    hopsPerAnalysis = activeQuality;
    hop = std::max (1, baseHop / activeQuality);
}

void ColgateEngine::reset() noexcept
{
    applyPendingQuality();

    for (int c = 0; c < kMaxChannels; ++c)
    {
        std::fill (inputRing[(size_t) c].begin(), inputRing[(size_t) c].end(), 0.0f);
        std::fill (outputRing[(size_t) c].begin(), outputRing[(size_t) c].end(), 0.0f);
        std::fill (dryRing[(size_t) c].begin(), dryRing[(size_t) c].end(), 0.0f);
        analysisLowpass[(size_t) c].reset();
    }
    std::fill (responseRe.begin(), responseRe.end(), 1.0f);
    std::fill (responseIm.begin(), responseIm.end(), 0.0f);
    for (auto& a : meterValues.curveDb)
        a.store (0.0f, std::memory_order_relaxed);

    raw.fill (0.0f);
    energy.fill (0.0f);
    weightedEnergy.fill (0.0f);
    shapeDb.fill (0.0f);
    gainTargetDb.fill (0.0f);
    gainNowDb.fill (0.0f);
    specificIn.fill (0.0f);
    specificOut.fill (0.0f);
    energyPrimed = false;

    phaseSpace.reset();
    fieldStage.reset();

    inputPos = hopPos = hopCounter = outputPos = dryPos = displayCounter = 0;
    analysisFill = analysisPhase = 0;

    smoothRecover = target.recoverPercent * 0.01f;
    smoothTame = target.tamePercent * 0.01f;
    smoothBias = target.biasPercent * 0.01f;
    smoothBrighten = target.brightenPercent * 0.01f;
    smoothBoost = target.boostDb;
    currentBoostCurve = 1.0e9f;
    lastLow = lastHigh = -1.0f;

    boostKeepDb = 0.0f;
    currentBoostMode = -1;
    settleHops = 0;
    brightnessSmooth = recoverSmooth = tameSmooth = biasSmooth = 0.0f;
    outputGain = target.bypass ? 1.0f : dbToAmp (target.gainDb);

    lastActive = target.recoverPercent != 0.0f || target.tamePercent != 0.0f || target.boostDb != 0.0f;
    wetMix = (lastActive && ! target.bypass) ? 1.0f : 0.0f;

    updateRangeMask();
    updateBoostCurve (smoothBoost);
}

void ColgateEngine::updateRangeMask() noexcept
{
    const float low = target.rangeLowHz;
    const float high = target.rangeHighHz;
    if (low == lastLow && high == lastHigh)
        return;
    lastLow = low;
    lastHigh = high;

    // A soft edge one and a half ERB wide. A limiter parked at the end of its
    // travel is out of the picture entirely.
    const float zA = (float) erbRate (std::min (low, high));
    const float zB = (float) erbRate (std::max (low, high));
    const bool aAtEdge = std::min (low, high) <= 20.5f;
    const bool bAtEdge = std::max (low, high) >= 19500.0f;
    const bool reversed = low > high;

    for (int k = 0; k < layout.count; ++k)
    {
        const float z = layout.z[(size_t) k];
        const float aboveA = aAtEdge ? 1.0f : smoothStepRange (z, zA - 0.75f, zA + 0.75f);
        const float belowB = bAtEdge ? 1.0f : smoothStepRange (z, zB + 0.75f, zB - 0.75f);
        const float between = aboveA * belowB;
        rangeMask[(size_t) k] = reversed ? 1.0f - between : between;
    }
    fieldStage.setRangeMask (rangeMask.data());
}

void ColgateEngine::updateBoostCurve (float boostDb) noexcept
{
    const int modeIndex = (int) target.mode;
    if (std::abs (boostDb - currentBoostCurve) < 0.005f && modeIndex == currentBoostMode)
        return;
    currentBoostCurve = boostDb;
    currentBoostMode = modeIndex;

    if (boostDb == 0.0f)
    {
        boostCurve.fill (0.0f);
        return;
    }

    // Boost shows the spectral balance a listener would hear if the volume changed
    // by this many dB. A contour moved up by B phon needs less than B dB of extra
    // level in the bass and the top, because contours bunch up there, so to hear
    // what B dB louder would sound like at the same volume, each band gets the
    // difference. The reference follows that shape closely, only somewhat larger,
    // and it keeps rising above the standard's 12.5 kHz. Only Boost +30 and +50 have
    // been measured, so other values and all negative ones follow from the same rule.
    constexpr float referencePhon = 50.0f;
    const float raised = std::max (0.0f, std::min (100.0f, referencePhon + boostDb));
    const auto& tuning = tuningFor (target.mode);
    const auto flat = iso226Interpolate (kBoostFlatHz);
    const auto differenceAt = [&] (float a, float lu, float tf) {
        return (raised - referencePhon) - (iso226Spl (a, lu, tf, raised) - iso226Spl (a, lu, tf, referencePhon));
    };
    const float flatDifference = differenceAt ((float) flat.alpha, (float) flat.lu, (float) flat.tf);
    const float growth = std::pow (std::abs (boostDb) / kBoostMeasuredAtDb, tuning.boostGrowth);
    const float* fade = fieldStage.fade();

    for (int k = 0; k < layout.count; ++k)
    {
        const float z = layout.z[(size_t) k];
        float treble = 0.0f;
        if (z > kBoostTrebleKnotZ[0])
        {
            float previousZ = kBoostTrebleKnotZ[0], previousDb = 0.0f;
            treble = tuning.boostTrebleDb[5];
            for (int i = 0; i < 6; ++i)
            {
                const float nextZ = kBoostTrebleKnotZ[i + 1], nextDb = tuning.boostTrebleDb[(size_t) i];
                if (z <= nextZ)
                {
                    treble = previousDb + (z - previousZ) / (nextZ - previousZ) * (nextDb - previousDb);
                    break;
                }
                previousZ = nextZ;
                previousDb = nextDb;
            }
        }
        const float contour = differenceAt (layout.isoAlpha[(size_t) k], layout.isoLu[(size_t) k], layout.isoTf[(size_t) k]) - flatDifference;
        const float wanted = growth * (tuning.boostScale * contour + treble * boostDb / kBoostMeasuredAtDb);
        const float size = std::abs (wanted);
        const float limited = size <= kBoostLimitDb ? size : kBoostLimitDb + 0.25f * (size - kBoostLimitDb);
        boostCurve[(size_t) k] = fade[k] * (wanted < 0.0f ? -limited : limited);
    }
}

void ColgateEngine::runAnalysisHop() noexcept
{
    const int count = layout.count;
    const int modeIndex = (int) target.mode;

    // Parameters move once per hop, so a static setting is exactly static.
    smoothRecover += parameterAlpha * (target.recoverPercent * 0.01f - smoothRecover);
    smoothTame += parameterAlpha * (target.tamePercent * 0.01f - smoothTame);
    smoothBias += parameterAlpha * (target.biasPercent * 0.01f - smoothBias);
    smoothBrighten += parameterAlpha * (target.brightenPercent * 0.01f - smoothBrighten);
    smoothBoost += parameterAlpha * (target.boostDb - smoothBoost);

    if (target.recoverPercent == 0.0f && smoothRecover < 1.0e-4f) smoothRecover = 0.0f;
    if (target.tamePercent == 0.0f && smoothTame < 1.0e-4f) smoothTame = 0.0f;
    if (target.boostDb == 0.0f && std::abs (smoothBoost) < 1.0e-3f) smoothBoost = 0.0f;

    updateRangeMask();
    updateBoostCurve (smoothBoost);

    // Analysis always runs, so nothing has to warm up when the controls move.
    const float* inputs[kMaxChannels] = { analysisBuffer[0].data(), analysisBuffer[1].data() };
    phaseSpace.pushHop (inputs);

    // One curve for both channels, decided on their combined power.
    const float perChannel = 1.0f / (float) channels;
    for (int k = 0; k < count; ++k)
    {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c)
            sum += phaseSpace.bandEnergy (c)[k];
        raw[(size_t) k] = std::max (sum * perChannel, 0.0f);
    }

    if (! energyPrimed)
    {
        energy = raw;
        loudnessEnergy = raw;
        energyPrimed = true;
    }

    const float attack = attackAlpha[modeIndex];
    const float release = releaseAlpha[modeIndex];
    const float loudnessAttack = loudnessAttackAlpha[modeIndex];
    const float loudnessRelease = loudnessReleaseAlpha[modeIndex];
    for (int k = 0; k < count; ++k)
    {
        // Slow up, fast down. An attack gets through before the gain reacts.
        const float difference = raw[(size_t) k] - energy[(size_t) k];
        energy[(size_t) k] += (difference > 0.0f ? attack : release) * difference;

        const float loudnessDifference = raw[(size_t) k] - loudnessEnergy[(size_t) k];
        loudnessEnergy[(size_t) k] += (loudnessDifference > 0.0f ? loudnessAttack : loudnessRelease) * loudnessDifference;
    }

    const bool unmasking = smoothRecover > 0.0f || smoothTame > 0.0f;
    if (unmasking)
    {
        PerceptualField::Controls controls;
        controls.recover = smoothRecover;
        controls.tame = smoothTame;
        controls.bias = smoothBias;
        controls.brighten = smoothBrighten;
        fieldStage.compute (energy.data(), loudnessEnergy.data(), controls, target.mode, shapeDb.data());
    }
    else
    {
        shapeDb.fill (0.0f);
    }

    // The signal as the ear weighs it, for the two gains below and the meter.
    const float floorPowered = std::exp (kLoudnessExponent * std::log (kLoudnessFloor));
    for (int k = 0; k < count; ++k)
    {
        const float e = energy[(size_t) k] * layout.earPower[(size_t) k];
        weightedEnergy[(size_t) k] = e;
        specificIn[(size_t) k] = std::exp (kLoudnessExponent * std::log (e + kLoudnessFloor)) - floorPowered;
    }

    boostKeepDb = smoothBoost != 0.0f ? keepLoudnessDb (loudnessEnergy.data(), boostCurve.data(), nullptr, count, kBoostLoudnessExponent, boostKeepDb) : 0.0f;
    const float* fade = fieldStage.fade();

    bool anyTarget = false;
    for (int k = 0; k < count; ++k)
    {
        const float total = shapeDb[(size_t) k] + boostCurve[(size_t) k] + fade[k] * boostKeepDb;
        gainTargetDb[(size_t) k] += gainSmoothAlpha * (total - gainTargetDb[(size_t) k]);
        if (std::abs (gainTargetDb[(size_t) k]) < 1.0e-4f && total == 0.0f)
            gainTargetDb[(size_t) k] = 0.0f;
        anyTarget = anyTarget || gainTargetDb[(size_t) k] != 0.0f;

        const float e = weightedEnergy[(size_t) k] * dbToPow (total);
        specificOut[(size_t) k] = std::exp (kLoudnessExponent * std::log (e + kLoudnessFloor)) - floorPowered;
    }

    lastActive = anyTarget;
    publishMeters();
    publishDisplayCurve();
}

void ColgateEngine::publishMeters() noexcept
{
    const int count = layout.count;

    const bool unmasking = smoothRecover > 0.0f || smoothTame > 0.0f;
    const float* lifted = fieldStage.recoverPart();
    const float* cut = fieldStage.tamePart();
    float positive = 0.0f, negative = 0.0f, weightSum = 1.0e-9f;
    float centroidIn = 0.0f, sumIn = 1.0e-12f, centroidOut = 0.0f, sumOut = 1.0e-12f;

    for (int k = 0; k < count; ++k)
    {
        const float n = specificIn[(size_t) k];
        weightSum += n;
        if (unmasking)
        {
            positive += n * lifted[k];
            negative += n * cut[k];
        }

        centroidIn += n * layout.z[(size_t) k];
        sumIn += n;
        const float m = specificOut[(size_t) k];
        centroidOut += m * layout.z[(size_t) k];
        sumOut += m;
    }

    recoverSmooth += meterAlpha * (positive / weightSum - recoverSmooth);
    tameSmooth += meterAlpha * (negative / weightSum - tameSmooth);
    biasSmooth += meterAlpha * ((unmasking ? fieldStage.recoverShare() : 0.0f) - biasSmooth);
    brightnessSmooth += meterAlpha * (centroidOut / sumOut - centroidIn / sumIn - brightnessSmooth);

    meterValues.recoverDb.store (recoverSmooth, std::memory_order_relaxed);
    meterValues.tameDb.store (tameSmooth, std::memory_order_relaxed);
    meterValues.biasBalance.store (biasSmooth, std::memory_order_relaxed);
    meterValues.brightnessShift.store (brightnessSmooth, std::memory_order_relaxed);
}

void ColgateEngine::publishDisplayCurve() noexcept
{
    if (++displayCounter < 6)
        return;
    displayCounter = 0;

    const int count = layout.count;
    for (int i = 0; i < kDisplayPoints; ++i)
    {
        float db = 0.0f;
        for (int k = 0; k < count; ++k)
            db += gainNowDb[(size_t) k] * displayBasis[(size_t) (k * kDisplayPoints + i)];
        meterValues.curveDb[(size_t) i].store (db, std::memory_order_relaxed);
    }
}

void ColgateEngine::runHop() noexcept
{
    // Quality changes the overlap only, never the latency or the filter. Frames of
    // two overlaps don't add up to one, so the switch waits until the wet path is
    // faded out, and the wet path stays out until the old frames have played.
    if (wantedQuality() != activeQuality && hopCounter == 0 && wetMix <= 0.0f)
    {
        applyPendingQuality();
        settleHops = (lastActive ? windowLength / hop + 1 : 0);
    }
    else if (settleHops > 0)
    {
        --settleHops;
    }

    const int count = layout.count;
    const int knots = (int) knotBins.size();
    const int half = fftSize / 2;

    // Glide to the gains from the latest analysis, reaching them exactly as the
    // next analysis hop arrives.
    const float glide = 1.0f / (float) (hopsPerAnalysis - hopCounter);
    std::fill (knotLog.begin(), knotLog.end(), 0.0f);
    std::fill (knotPhase.begin(), knotPhase.end(), 0.0f);
    bool flat = true;
    for (int k = 0; k < count; ++k)
    {
        auto& now = gainNowDb[(size_t) k];
        now += glide * (gainTargetDb[(size_t) k] - now);
        if (now == 0.0f)
            continue;
        flat = false;
        const float* bl = basisLog.data() + (size_t) (k * knots);
        const float* bp = basisPhase.data() + (size_t) (k * knots);
        float* outLog = knotLog.data();
        float* outPhase = knotPhase.data();
        for (int i = 0; i < knots; ++i)
        {
            outLog[i] += now * bl[i];
            outPhase[i] += now * bp[i];
        }
    }

    float* hr = responseRe.data();
    float* hi = responseIm.data();
    if (flat)
    {
        std::fill (hr, hr + half + 1, 1.0f);
        std::fill (hi, hi + half + 1, 0.0f);
    }
    else
    {
        // exp (log magnitude + j phase), by bin, between the knots. Short series
        // are plenty: both are scaled down into range first, then squared back up.
        for (int i = 0; i + 1 < knots; ++i)
        {
            const int first = knotBins[(size_t) i];
            const int last = knotBins[(size_t) i + 1];
            const float stepInv = 1.0f / (float) (last - first);
            const float l0 = knotLog[(size_t) i];
            const float dl = (knotLog[(size_t) i + 1] - l0) * stepInv;
            const float p0 = knotPhase[(size_t) i];
            const float dp = (knotPhase[(size_t) i + 1] - p0) * stepInv;
            for (int b = first; b < last; ++b)
            {
                const float offset = (float) (b - first);
                const float y = (l0 + dl * offset) * 0.125f;
                float m = 1.0f + y * (1.0f + y * (0.5f + y * (0.16666667f + y * (0.041666667f + y * 0.0083333333f))));
                m = m * m;
                m = m * m;
                m = m * m;
                const float p = 0.25f * std::max (-6.2f, std::min (6.2f, p0 + dp * offset));
                const float p2 = p * p;
                float c = 1.0f - p2 * (0.5f - p2 * (0.041666667f - p2 * (0.0013888889f - p2 * 0.000024801587f)));
                float s = p * (1.0f - p2 * (0.16666667f - p2 * (0.0083333333f - p2 * 0.0001984127f)));
                float c2 = c * c - s * s;
                float s2 = 2.0f * s * c;
                c = c2 * c2 - s2 * s2;
                s = 2.0f * s2 * c2;
                hr[b] = m * c;
                hi[b] = m * s;
            }
        }
        const float y = knotLog[(size_t) knots - 1];
        hr[half] = std::exp (y);
        hi[half] = 0.0f;
        hi[0] = 0.0f;
    }

    // Frame.
    float* re = spectrumRe.data();
    float* im = spectrumIm.data();
    std::fill (re, re + fftSize, 0.0f);
    std::fill (im, im + fftSize, 0.0f);

    const int start = (inputPos - windowLength) & inputMask;
    const float* in0 = inputRing[0].data();
    const float* in1 = channels > 1 ? inputRing[1].data() : nullptr;
    for (int n = 0; n < windowLength; ++n)
    {
        const int i = (start + n) & inputMask;
        re[n] = window[(size_t) n] * in0[i];
        if (in1 != nullptr)
            im[n] = window[(size_t) n] * in1[i];
    }

    fft.forward (re, im);

    // Both channels share one complex transform and one real filter, so the
    // filter applies directly: H at positive bins, its conjugate at negative ones.
    re[0] *= hr[0];
    im[0] *= hr[0];
    re[half] *= hr[half];
    im[half] *= hr[half];
    for (int b = 1; b < half; ++b)
    {
        const int m = fftSize - b;
        const float ar = re[b], ai = im[b], cr = re[m], ci = im[m];
        re[b] = ar * hr[b] - ai * hi[b];
        im[b] = ar * hi[b] + ai * hr[b];
        re[m] = cr * hr[b] + ci * hi[b];
        im[m] = ci * hr[b] - cr * hi[b];
    }

    // Inverse by swapping the real and imaginary parts.
    fft.forward (im, re);

    // Hann windows at this hop sum to windowLength / (2 * hop).
    const float norm = (2.0f * (float) hop / (float) windowLength) / (float) fftSize;
    float* out0 = outputRing[0].data();
    float* out1 = channels > 1 ? outputRing[1].data() : nullptr;
    for (int n = 0; n < fftSize; ++n)
    {
        const int i = (outputPos + n) & outputMask;
        out0[i] += re[n] * norm;
        if (out1 != nullptr)
            out1[i] += im[n] * norm;
    }
}

void ColgateEngine::process (float* const* main, const float* const* sidechain, int numSamples) noexcept
{
    ScopedFlushToZero flush;

    const bool analyseSidechain = target.useSidechain && sidechain != nullptr;
    const float gainTarget = target.bypass ? 1.0f : dbToAmp (target.gainDb);
    const int analysisStride = analysisDecimation;

    int done = 0;
    while (done < numSamples)
    {
        const int chunk = std::min (numSamples - done, hop - hopPos);

        const bool switching = wantedQuality() != activeQuality || settleHops > 0;
        const float wetTarget = (! target.bypass && lastActive && ! switching) ? 1.0f : 0.0f;

        for (int c = 0; c < channels; ++c)
        {
            float* samples = main[c] + done;
            const float* analysed = analyseSidechain ? sidechain[c] + done : samples;

            float* ring = inputRing[(size_t) c].data();
            float* dry = dryRing[(size_t) c].data();
            float* out = outputRing[(size_t) c].data();
            float* anaOut = analysisBuffer[(size_t) c].data();
            auto& lowpass = analysisLowpass[(size_t) c];

            int fill = analysisFill;
            int phase = analysisPhase;
            float mix = wetMix;
            float gain = outputGain;

            for (int i = 0; i < chunk; ++i)
            {
                float x = samples[i];
                if (! std::isfinite (x))
                    x = 0.0f;

                // Feed the analyser.
                float a = analysed[i];
                if (! std::isfinite (a))
                    a = 0.0f;
                if (analysisStride == 1)
                {
                    anaOut[fill++] = a;
                }
                else
                {
                    const float low = lowpass.process (a);
                    if (++phase >= analysisStride)
                    {
                        phase = 0;
                        anaOut[fill++] = low;
                    }
                }

                ring[(inputPos + i) & inputMask] = x;

                const int dryIndex = (dryPos + i) & dryMask;
                const float delayed = dry[(dryIndex - latency) & dryMask];
                dry[dryIndex] = x;

                const int outIndex = (outputPos + hopPos + i) & outputMask;
                const float wet = out[outIndex];
                out[outIndex] = 0.0f;

                if (mix != wetTarget)
                    mix = mix < wetTarget ? std::min (wetTarget, mix + wetStep) : std::max (wetTarget, mix - wetStep);

                const float mixed = mix >= 1.0f ? wet : (mix <= 0.0f ? delayed : delayed + mix * (wet - delayed));

                if (gain != gainTarget)
                {
                    const float difference = gainTarget - gain;
                    gain = std::abs (difference) < 1.0e-5f ? gainTarget : gain + gainStep * 3.0f * difference;
                }
                samples[i] = mixed * gain;
            }

            if (c == channels - 1)
            {
                analysisFill = fill;
                analysisPhase = phase;
                wetMix = mix;
                outputGain = gain;
            }
        }

        inputPos = (inputPos + chunk) & inputMask;
        dryPos = (dryPos + chunk) & dryMask;
        hopPos += chunk;
        done += chunk;

        if (hopPos == hop)
        {
            hopPos = 0;
            outputPos = (outputPos + hop) & outputMask;

            // A whole base hop of analysis samples has arrived on every wrap.
            hopCounter = (hopCounter + 1) % hopsPerAnalysis;
            if (hopCounter == 0)
            {
                analysisFill = 0;
                runAnalysisHop();
            }
            runHop();
        }
    }
}

float ColgateEngine::curveDbAt (double hz) const noexcept
{
    const int knots = (int) knotBins.size();
    const double position = std::max (0.0, std::min ((double) (fftSize / 2), hz * fftSize / rate));
    int i = 0;
    while (i < knots - 2 && knotBins[(size_t) i + 1] < position)
        ++i;
    const float t = (float) ((position - knotBins[(size_t) i]) / (double) (knotBins[(size_t) i + 1] - knotBins[(size_t) i]));

    float nepers = 0.0f;
    for (int k = 0; k < layout.count; ++k)
    {
        const float* b = basisLog.data() + (size_t) (k * knots);
        nepers += gainNowDb[(size_t) k] * ((1.0f - t) * b[i] + t * b[i + 1]);
    }
    return nepers / kNepersPerDb;
}

} // namespace colgate
