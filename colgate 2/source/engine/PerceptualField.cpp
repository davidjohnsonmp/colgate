// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PerceptualField.h"

namespace colgate
{

namespace
{
// Measured against reference renders: three songs in Basic, five in Master. See
// docs/DESIGN.md, and tools/calibration for how these numbers were found.
constexpr ModeTuning kBasic {
    -0.0777f, -0.0907f, 1.4000f,
    -2.7461f, -5.6964f,
    { 4.0377f, 1.0790f, 0.0f, -0.0333f, 0.3268f, 0.8862f, 3.1371f, 4.1586f },
    { -1.1053f, 0.2670f, 0.0f, 0.8489f, -2.0529f },
    3.3975f,
    1.0642f,
    0.1541f, 0.3710f, 3.613f,
    0.5437f,
    0.12f, 0.04f,
    0.03f, 0.08f,
    1.2488f, 0.1926f,
    { 0.3207f, -0.1412f, 0.2313f, -0.1368f, 2.8262f, 4.2697f }
};

constexpr ModeTuning kMaster {
    -0.0506f, -0.0746f, 1.7775f,
    -2.3766f, -5.9291f,
    { 3.4769f, 0.6549f, 0.0f, -0.1284f, -0.4844f, 0.2559f, 3.0230f, 4.9126f },
    { -0.3209f, 0.6296f, 0.0f, 0.4049f, -0.8201f },
    1.2878f,
    1.1012f,
    0.1374f, 0.2924f, 2.000f,
    0.3187f,
    0.12f, 0.04f,
    0.03f, 0.08f,
    1.1721f, 0.1926f,
    { 0.1744f, -0.2059f, 0.0744f, -0.2894f, 2.0445f, 3.7704f }
};

constexpr float kRecoverKnotZ[8] = { 2.0f, 8.0f, 16.0f, 26.0f, 33.0f, 36.0f, 38.0f, 39.5f };
constexpr float kTameKnotZ[5] = { 2.0f, 8.0f, 16.0f, 26.0f, 38.0f };
constexpr float kFadeKnotZ[7] = { 28.0f, 33.0f, 36.0f, 38.0f, 39.5f, 40.5f, 41.3f };
constexpr float kFadeKnotValue[7] = { 1.0f, 1.0f, 0.95f, 0.85f, 0.62f, 0.2f, 0.0f };

constexpr float kPivotZ = 14.83f;        // 900 Hz
constexpr float kBiasLineDb = 3.0f;      // per unit of Bias, where the line moves to
constexpr float kBiasKnee = 2.0f;        // and how much softer that side's knee gets
constexpr float kBrightenBend = 0.25f;   // how soon Brighten's tilt levels off
constexpr float kFieldLoudnessExponent = 0.35f;
constexpr float kLoudnessFloor = 7.94e-11f;
constexpr float kMaxKeepDb = 24.0f;
constexpr float kGainLimitDb = 30.0f;
constexpr double kSlowFieldSeconds = 2.0;
constexpr int kNearReach = 6;

template <int N>
float interpolate (const float (&x)[N], const float* y, float at) noexcept
{
    if (at <= x[0])
        return y[0];
    for (int i = 0; i + 1 < N; ++i)
        if (at <= x[i + 1])
            return y[i] + (at - x[i]) / (x[i + 1] - x[i]) * (y[i + 1] - y[i]);
    return y[N - 1];
}

inline float above (float x, float softnessSquared) noexcept
{
    return 0.5f * (x + std::sqrt (x * x + softnessSquared));
}

inline float below (float x, float softnessSquared) noexcept
{
    return 0.5f * (x - std::sqrt (x * x + softnessSquared));
}
} // namespace

const ModeTuning& tuningFor (Mode mode) noexcept
{
    return mode == Mode::master ? kMaster : kBasic;
}

float keepLoudnessDb (const float* energy, const float* gainDb, const float* mask, int count,
                      float exponent, float startDb) noexcept
{
    const float floorPowered = std::pow (kLoudnessFloor, exponent);

    float wanted = 0.0f;
    std::array<float, kMaxBands> gained;
    for (int k = 0; k < count; ++k)
    {
        const float e = mask != nullptr ? mask[k] * energy[k] : energy[k];
        wanted += std::pow (e + kLoudnessFloor, exponent) - floorPowered;
        gained[(size_t) k] = e * dbToPow (gainDb[k]);
    }
    if (wanted < 1.0e-5f)
        return 0.0f;

    float db = startDb;
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        const float scale = dbToPow (db);
        float value = 0.0f, slope = 0.0f;
        for (int k = 0; k < count; ++k)
        {
            const float e = scale * gained[(size_t) k];
            const float total = e + kLoudnessFloor;
            const float powered = std::pow (total, exponent);
            value += powered - floorPowered;
            slope += exponent * powered * (e / total);
        }
        if (slope < 1.0e-9f)
            return 0.0f;
        const float step = (wanted - value) / (slope * 0.2302585093f);
        db = std::max (-kMaxKeepDb, std::min (kMaxKeepDb, db + std::max (-8.0f, std::min (8.0f, step))));
        if (std::abs (step) < 0.002f)
            break;
    }
    return db;
}

void PerceptualField::prepare (const BandLayout& layout, double hopSeconds)
{
    count = layout.count;
    slowAlpha = (float) (1.0 - std::exp (-hopSeconds / kSlowFieldSeconds));
    reset();

    for (int k = 0; k < count; ++k)
    {
        z[(size_t) k] = layout.z[(size_t) k];
        mask[(size_t) k] = 1.0f;
        const float t = std::max (0.0f, std::min (1.0f, (z[(size_t) k] - 23.0f) / 7.0f));
        trebleWeight[(size_t) k] = t * t * (3.0f - 2.0f * t);
        topFade[(size_t) k] = interpolate (kFadeKnotZ, kFadeKnotValue, z[(size_t) k]);

        for (int j = 0; j < count; ++j)
            nearKernel[(size_t) k][(size_t) j] = std::exp (-0.5f * (float) ((k - j) * (k - j)));
    }

    setRangeMask (mask.data());
}

void PerceptualField::reset() noexcept
{
    primed = false;
    keepGuess = 0.0f;
    slowRecover.fill (0.0f);
    slowTame.fill (0.0f);
    recoverGain.fill (0.0f);
    tameGain.fill (0.0f);
}

void PerceptualField::setRangeMask (const float* newMask) noexcept
{
    for (int k = 0; k < count; ++k)
        mask[(size_t) k] = newMask[k];

    for (int k = 0; k < count; ++k)
    {
        float sum = 1.0e-6f;
        for (int j = std::max (0, k - kNearReach); j <= std::min (count - 1, k + kNearReach); ++j)
            sum += nearKernel[(size_t) k][(size_t) j] * mask[(size_t) j];
        nearNorm[(size_t) k] = 1.0f / sum;
    }
}

void PerceptualField::compute (const float* energy, const float* loudnessEnergy, const Controls& controls, Mode mode, float* gainDb) noexcept
{
    const auto& t = tuningFor (mode);

    float total = 1.0e-13f;
    for (int k = 0; k < count; ++k)
        total += mask[(size_t) k] * energy[k];
    const float totalDb = powToDb (total);

    // Relative level, with a floor: a band this far under the rest of the signal
    // holds nothing to reveal, so it asks for no more than the floor gives.
    for (int k = 0; k < count; ++k)
    {
        const float level = powToDb (energy[k] + 1.0e-13f) - totalDb;
        relative[(size_t) k] = mask[(size_t) k] * std::max (level, -kFloorDb);
    }

    const float brighten = controls.brighten;

    for (int k = 0; k < count; ++k)
    {
        float nearSum = 0.0f;
        const float* nearRow = nearKernel[(size_t) k].data();
        for (int j = std::max (0, k - kNearReach); j <= std::min (count - 1, k + kNearReach); ++j)
            nearSum += nearRow[j] * relative[(size_t) j];

        const float zk = z[(size_t) k];
        const float level = t.own * relative[(size_t) k] + t.near * nearSum * nearNorm[(size_t) k];
        float forRecover = level + t.offsetRecoverDb + interpolate (kRecoverKnotZ, t.recoverStaticDb.data(), zk);
        float forTame = t.tameLevelScale * level + t.offsetTameDb + interpolate (kTameKnotZ, t.tameStaticDb.data(), zk);

        // Positive Brighten calms the treble's movement around its own average,
        // and only that, so the tilt alone decides which way the balance goes.
        auto& slowR = slowRecover[(size_t) k];
        auto& slowT = slowTame[(size_t) k];
        slowR = primed ? slowR + slowAlpha * (forRecover - slowR) : forRecover;
        slowT = primed ? slowT + slowAlpha * (forTame - slowT) : forTame;
        const float calm = 1.0f - t.brightenTrebleSlope * brighten * trebleWeight[(size_t) k];
        forRecover = slowR + (forRecover - slowR) * calm;
        forTame = slowT + (forTame - slowT) * calm;

        fieldRecover[(size_t) k] = forRecover;
        fieldTame[(size_t) k] = forTame;
    }
    primed = true;

    const float scaleRecover = amountScale (controls.recover, t.amountExponent);
    const float scaleTame = amountScale (controls.tame, t.amountExponent);
    const float scaleMean = 0.5f * (scaleRecover + scaleTame);
    const float shared = std::min (scaleRecover, scaleTame);
    const float soft2 = t.softnessDb * t.softnessDb;

    // Bias moves the line between the classes and softens one side's knee. Measured
    // with Recover alone, this explains only about a fifth of what Bias does there.
    // The shared part removes it entirely with Recover equal to Tame, as in the
    // reference, because then only the unbiased curves are left.
    const float line = -controls.bias * kBiasLineDb;
    const float wider = std::exp (kBiasKnee * controls.bias);
    const float softRecover2 = soft2 * wider * wider;
    const float softTame2 = soft2 / (wider * wider);

    // Levels off well before full travel, but stays smooth through zero.
    const float bent = (1.0f - std::exp (-std::abs (brighten) / kBrightenBend)) / (1.0f - std::exp (-1.0f / kBrightenBend));
    const float tiltAmount = scaleMean * (brighten < 0.0f ? -bent : bent);

    float recoverBands = 0.0f, tameBands = 0.0f;
    for (int k = 0; k < count; ++k)
    {
        const float fr = fieldRecover[(size_t) k];
        const float ft = fieldTame[(size_t) k];
        const float up = above (fr, soft2);
        const float down = below (ft, soft2);
        const float upLine = above (fr - line, softRecover2);
        const float downLine = below (ft - line, softTame2);

        const float lifted = scaleRecover * upLine + shared * (up - upLine);
        const float cut = scaleTame * downLine + shared * (down - downLine);

        const float fromPivot = z[(size_t) k] - kPivotZ;
        const float tilt = fromPivot < 0.0f ? t.tiltLowPerErb * fromPivot
                                            : t.tiltHighPerErb * t.tiltKneeErb * std::tanh (fromPivot / t.tiltKneeErb);
        const float g = lifted + cut + tiltAmount * tilt;

        recoverGain[(size_t) k] = mask[(size_t) k] * lifted;
        tameGain[(size_t) k] = mask[(size_t) k] * cut;
        shape[(size_t) k] = kGainLimitDb * std::tanh (g / kGainLimitDb);
        faded[(size_t) k] = topFade[(size_t) k] * shape[(size_t) k];

        if (fr + ft - 2.0f * line > 0.0f)
            recoverBands += mask[(size_t) k];
        else
            tameBands += mask[(size_t) k];
    }

    const float bandsTotal = recoverBands + tameBands;
    share = bandsTotal > 0.0f ? (recoverBands - tameBands) / bandsTotal : 0.0f;

    keepGuess = keepLoudnessDb (loudnessEnergy, faded.data(), mask.data(), count, kFieldLoudnessExponent, keepGuess);
    for (int k = 0; k < count; ++k)
        gainDb[k] = mask[(size_t) k] * topFade[(size_t) k] * (shape[(size_t) k] + keepGuess);
}

} // namespace colgate
