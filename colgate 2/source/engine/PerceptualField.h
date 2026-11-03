// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The decision stage: how far each band should move, from where it stands in the
// spectrum at this moment.
//
// Each band's level is taken relative to the signal's total level, so absolute
// level plays no part. A band that stands above its immediate neighbourhood is
// pulled down and one that sits below it is lifted:
//
//     level_k = own L_k + near L_k(1)
//
// Recover and Tame read that through two different static curves, each with a soft
// knee at zero, and Tame moves further for the same level. Recover only ever acts
// above its own zero and Tame only below its own. Their zeros aren't the same across
// the spectrum, so in the bass the two can work on one band at once:
//
//     gain_k = recover * above (level_k + staticR_k) + tame * below (scale level_k + staticT_k)
//
// One broadband gain then holds the sum over the bands of power^0.35 constant. That
// is why Tame alone lifts the treble and Recover alone dips the mids.
//
// The form and every number in ModeTuning come from measuring reference renders
// (see docs/DESIGN.md). In them 50/50 is the sum of 50/0 and 0/50, to 0.13 dB at
// worst, which is what the sum above and the loudness rule give.
#pragma once

#include "Psychoacoustics.h"

#include <array>

namespace colgate
{

enum class Mode
{
    basic = 0,
    master = 1
};

struct ModeTuning
{
    // Weights of a band's own relative level and of its neighbourhood's, and how
    // much further Tame moves than Recover for the same level.
    float own, near, tameLevelScale;

    // Where zero lies for each control, in dB of the level above.
    float offsetRecoverDb, offsetTameDb;

    // Static curves by ERB rate, at z = 2, 8, 16, 26, 33, 36, 38, 39.5 for Recover,
    // and at z = 2, 8, 16, 26, 38 for Tame.
    std::array<float, 8> recoverStaticDb;
    std::array<float, 5> tameStaticDb;

    // Width of the knee at each control's zero, in dB.
    float softnessDb;

    // Gain follows (amount / 50) ^ amountExponent.
    float amountExponent;

    // Brighten: a tilt about 900 Hz, with its treble side leaning over at a knee.
    float tiltLowPerErb, tiltHighPerErb, tiltKneeErb;

    // Positive Brighten calms the field's movement in the treble.
    float brightenTrebleSlope;

    // A band's energy rises slowly and falls fast.
    float attackSeconds, releaseSeconds;

    // The broadband gains read energy that rises fast and falls more slowly.
    float loudnessAttackSeconds, loudnessReleaseSeconds;

    // Boost: this many times the equal-loudness contour difference, plus a treble
    // correction at Boost +30, at z = 30, 33, 35, 37, 38.5, 40.3. All of it grows
    // faster than the contours do, by (Boost / 30) ^ boostGrowth.
    float boostScale, boostGrowth;
    std::array<float, 6> boostTrebleDb;
};

const ModeTuning& tuningFor (Mode mode) noexcept;

// Field gain per unit of amount. An amount of 0.5 (50%) is unity.
inline float amountScale (float amount, float exponent) noexcept
{
    return amount <= 0.0f ? 0.0f : std::pow (2.0f * amount, exponent);
}

// The broadband gain in dB that keeps the sum of (band power)^exponent where it
// was. The mask, which may be null, says which bands count. startDb is a first
// guess, so passing the previous answer makes it converge at once.
float keepLoudnessDb (const float* energy, const float* gainDb, const float* mask, int count,
                      float exponent, float startDb) noexcept;

class PerceptualField
{
public:
    void prepare (const BandLayout& layout, double hopSeconds);
    void reset() noexcept;

    // Which bands take part, 0 to 1, from the range limiters. Excluded bands are
    // neither moved nor counted. Allocates nothing, but only call it on a change.
    void setRangeMask (const float* mask) noexcept;

    struct Controls
    {
        float recover = 0.0f;  // 0..2, where 1 is 100%
        float tame = 0.0f;     // 0..2
        float bias = 0.0f;     // -1..1
        float brighten = 0.0f; // -1..1
    };

    // energy is smoothed band power. Writes the gain in dB per band.
    void compute (const float* energy, const float* loudnessEnergy, const Controls& controls, Mode mode, float* gainDb) noexcept;

    // How the bands split between the classes at the last compute().
    float recoverShare() const noexcept { return share; }

    // Each control's own part of the last gain, before the broadband gain.
    const float* recoverPart() const noexcept { return recoverGain.data(); }
    const float* tamePart() const noexcept { return tameGain.data(); }

    // Scales everything, by frequency: the top of the spectrum is eased off.
    const float* fade() const noexcept { return topFade.data(); }

    static constexpr float kFloorDb = 60.0f;

private:
    using BandArray = std::array<float, kMaxBands>;

    int count = 0;
    BandArray z {}, relative {}, mask {}, trebleWeight {}, topFade {}, nearNorm {};
    BandArray fieldRecover {}, fieldTame {}, slowRecover {}, slowTame {};
    BandArray recoverGain {}, tameGain {}, shape {}, faded {};
    std::array<std::array<float, kMaxBands>, kMaxBands> nearKernel {};
    float slowAlpha = 0.001f;
    float keepGuess = 0.0f;
    float share = 0.0f;
    bool primed = false;
};

} // namespace colgate
