// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Published psychoacoustic formulas and the band layout every other stage of
// the engine shares:
//   - ERB-rate and ERB bandwidth: Glasberg & Moore (1990)
//   - Equal-loudness contours: ISO 226:2003
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace colgate
{

constexpr int kMaxBands = 64;
constexpr int kMaxChannels = 2;

// A full-scale sine is assumed to play at this SPL. It puts pink noise at
// -20 dBFS at 83 dB SPL, the usual mix-room calibration. The plug-in cannot
// know the real monitor level, so the model assumes a sensible one.
constexpr float kReferenceSplDb = 103.0f;

inline float dbToAmp (float db) noexcept { return std::exp (db * 0.1151292546f); }
inline float dbToPow (float db) noexcept { return std::exp (db * 0.2302585093f); }
inline float powToDb (float p) noexcept { return 4.342944819f * std::log (p); }

inline double erbRate (double hz) { return 21.4 * std::log10 (1.0 + 0.00437 * hz); }
inline double erbRateToHz (double z) { return (std::pow (10.0, z / 21.4) - 1.0) / 0.00437; }

// ISO 226:2003 tabulated parameters, reference frequencies 20 Hz to 12.5 kHz.
struct Iso226Point
{
    double alpha = 0.25, lu = 0.0, tf = 2.4;
};

inline Iso226Point iso226Interpolate (double hz)
{
    static constexpr int count = 29;
    static constexpr double freq[count] = { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500,
                                            630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
                                            10000, 12500 };
    static constexpr double af[count] = { 0.532, 0.506, 0.480, 0.455, 0.432, 0.409, 0.387, 0.367, 0.349, 0.330,
                                          0.315, 0.301, 0.288, 0.276, 0.267, 0.259, 0.253, 0.250, 0.246, 0.244,
                                          0.243, 0.243, 0.243, 0.242, 0.242, 0.245, 0.254, 0.271, 0.301 };
    static constexpr double lu[count] = { -31.6, -27.2, -23.0, -19.1, -15.9, -13.0, -10.3, -8.1, -6.2, -4.5,
                                          -3.1, -2.0, -1.1, -0.4, 0.0, 0.3, 0.5, 0.0, -2.7, -4.1,
                                          -1.0, 1.7, 2.5, 1.2, -2.1, -7.1, -11.2, -10.7, -3.1 };
    static constexpr double tf[count] = { 78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9,
                                          14.4, 11.4, 8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7,
                                          -1.3, -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3 };

    const double f = std::min (std::max (hz, freq[0]), freq[count - 1]);
    int i = 0;
    while (i < count - 2 && freq[i + 1] < f)
        ++i;
    const double t = (std::log (f) - std::log (freq[i])) / (std::log (freq[i + 1]) - std::log (freq[i]));

    Iso226Point p;
    p.alpha = af[i] + t * (af[i + 1] - af[i]);
    p.lu = lu[i] + t * (lu[i + 1] - lu[i]);
    p.tf = tf[i] + t * (tf[i + 1] - tf[i]);
    return p;
}

// Sound pressure level (dB SPL) of the contour with the given loudness level.
// The standard's validity range is 20 to 90 phon, but the formula stays smooth
// across the 0 to 100 phon this plug-in visits at extreme Boost settings.
inline float iso226Spl (float alpha, float lu, float tf, float phon) noexcept
{
    const float af = 4.47e-3f * (std::pow (10.0f, 0.025f * phon) - 1.15f)
                     + std::pow (0.4f * std::pow (10.0f, (tf + lu) / 10.0f - 9.0f), alpha);
    return (10.0f / alpha) * std::log10 (std::max (af, 1.0e-9f)) - lu + 94.0f;
}

// Bands are spaced 0.8 ERB apart from about 30 Hz to about 18.5 kHz, limited to
// what the sample rate can carry.
struct BandLayout
{
    int count = 0;
    float z0 = 0.0f, dz = 0.8f;
    std::array<float, kMaxBands> hz {}, z {};
    std::array<float, kMaxBands> earPower {};    // linear power weighting from the 70 phon contour
    std::array<float, kMaxBands> isoAlpha {}, isoLu {}, isoTf {};

    void prepare (double analysisRate)
    {
        const double topHz = std::min (18500.0, 0.46 * analysisRate);
        z0 = (float) erbRate (30.0);
        const double zTop = erbRate (topHz);
        count = std::min (kMaxBands, (int) std::floor ((zTop - z0) / dz) + 1);

        const auto contour1k = iso226Interpolate (1000.0);
        const float spl1k = iso226Spl ((float) contour1k.alpha, (float) contour1k.lu, (float) contour1k.tf, 70.0f);

        for (int k = 0; k < count; ++k)
        {
            const double zk = (double) z0 + (double) dz * (double) k;
            const double f = erbRateToHz (zk);
            hz[(size_t) k] = (float) f;
            z[(size_t) k] = (float) zk;

            const auto p = iso226Interpolate (f);
            isoAlpha[(size_t) k] = (float) p.alpha;
            isoLu[(size_t) k] = (float) p.lu;
            isoTf[(size_t) k] = (float) p.tf;

            const float spl = iso226Spl ((float) p.alpha, (float) p.lu, (float) p.tf, 70.0f);
            earPower[(size_t) k] = dbToPow (spl1k - spl);
        }
    }
};

} // namespace colgate
