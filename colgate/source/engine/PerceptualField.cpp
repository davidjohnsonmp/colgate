// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PerceptualField.h"

namespace colgate
{

namespace
{
// Measured against reference renders of three songs. See docs/DESIGN.md, and
// tools/calibration for how these numbers were found.
constexpr ModeTuning kBasic {
    -0.0837f, -0.0890f,
    -2.9283f, -4.9438f,
    { 3.6583f, 1.0112f, 0.0f, -0.2961f, -0.0966f, 0.3752f, 2.4170f, 3.3199f },
    { -1.0088f, 0.1707f, 0.0f, 0.8553f, -1.2050f },
    1.7436f,
    1.0477f,
    0.1555f, 0.3348f, 4.035f,
    0.5341f,
    0.12f, 0.04f,
    1.2493f,
    { 0.2946f, -0.1656f, 0.1990f, -0.1701f, 2.8074f, 4.2898f }
};

constexpr ModeTuning kMaster {
    -0.0952f, -0.0647f,
    -3.0632f, -4.9572f,
    { 3.5971f, 0.9539f, 0.0f, -0.6533f, -1.1404f, -0.3345f, 2.0982f, 3.6181f },
    { -0.3035f, 0.3719f, 0.0f, 0.8489f, -0.0265f },
    1.0178f,
    1.0891f,
    0.1470f, 0.2549f, 2.000f,
    0.1747f,
    0.12f, 0.04f,
    2.1755f,
    { 0.4592f, -0.2155f, 0.3111f, -0.5014f, 3.8981f, 6.7962f }
};

constexpr float kRecoverKnotZ[8] = { 2.0f, 8.0f, 16.0f, 26.0f, 33.0f, 36.0f, 38.0f, 39.5f };
constexpr float kTameKnotZ[5] = { 2.0f, 8.0f, 16.0f, 26.0f, 38.0f };
constexpr float kFadeKnotZ[7] = { 28.0f, 33.0f, 36.0f, 38.0f, 39.5f, 40.5f, 41.3f };
constexpr float kFadeKnotValue[7] = { 1.0f, 1.0f, 0.95f, 0.85f, 0.62f, 0.2f, 0.0f };

constexpr float kPivotZ = 14.83f;        // 900 Hz
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

void PerceptualField::compute (const float* energy, const Controls& controls, Mode mode, float* gainDb) noexcept
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

    float squares = 0.0f, votes = 1.0e-6f;
    for (int k = 0; k < count; ++k)
    {
        float nearSum = 0.0f;
        const float* nearRow = nearKernel[(size_t) k].data();
        for (int j = std::max (0, k - kNearReach); j <= std::min (count - 1, k + kNearReach); ++j)
            nearSum += nearRow[j] * relative[(size_t) j];

        const float zk = z[(size_t) k];
        const float level = t.own * relative[(size_t) k] + t.near * nearSum * nearNorm[(size_t) k];
        float forRecover = level + t.offsetRecoverDb + interpolate (kRecoverKnotZ, t.recoverStaticDb.data(), zk);
        float forTame = level + t.offsetTameDb + interpolate (kTameKnotZ, t.tameStaticDb.data(), zk);

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

        const float middle = 0.5f * (forRecover + forTame);
        squares += mask[(size_t) k] * middle * middle;
        votes += mask[(size_t) k];
    }
    primed = true;

    // Bias moves the line between the classes, relative to how widely the field
    // is spread right now. With equal Recover and Tame it has no effect at all, as
    // in the reference, because the line only enters through their difference.
    const float spread = std::sqrt (squares / votes);
    const float line = -controls.bias * (1.0f + 1.4f * spread);

    const float scaleRecover = amountScale (controls.recover, t.amountExponent);
    const float scaleTame = amountScale (controls.tame, t.amountExponent);
    const float scaleMean = 0.5f * (scaleRecover + scaleTame);
    const float scaleHalfDifference = 0.5f * (scaleRecover - scaleTame);
    const float soft2 = t.softnessDb * t.softnessDb;

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
        const float upLine = above (fr - line, soft2);
        const float downLine = below (ft - line, soft2);

        const float lifted = scaleMean * up + scaleHalfDifference * upLine;
        const float cut = scaleMean * down - scaleHalfDifference * downLine;

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

    keepGuess = keepLoudnessDb (energy, faded.data(), mask.data(), count, kFieldLoudnessExponent, keepGuess);
    for (int k = 0; k < count; ++k)
        gainDb[k] = mask[(size_t) k] * topFade[(size_t) k] * (shape[(size_t) k] + keepGuess);
}

} // namespace colgate
