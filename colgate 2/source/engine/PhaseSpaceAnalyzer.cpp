// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PhaseSpaceAnalyzer.h"

namespace colgate
{

namespace
{
constexpr double kPi = 3.14159265358979323846;

int nearestPowerOfTwo (double value)
{
    return 1 << std::max (0, (int) std::lround (std::log2 (std::max (value, 1.0))));
}

inline float flushTiny (float v) noexcept { return std::abs (v) < 1.0e-20f ? 0.0f : v; }
} // namespace

// ---------------------------------------------------------------------------

float AllpassHilbert::Section::tick (float x) noexcept
{
    const float y = c * (x + y2) - x2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = flushTiny (y);
    return y;
}

void AllpassHilbert::reset() noexcept
{
    for (int i = 0; i < 4; ++i)
    {
        pathA[(size_t) i] = {};
        pathB[(size_t) i] = {};
        pathA[(size_t) i].c = kA[i] * kA[i];
        pathB[(size_t) i].c = kB[i] * kB[i];
    }
    delayed = 0.0f;
    initialised = true;
}

void AllpassHilbert::process (float x, float& re, float& im) noexcept
{
    float a = x, b = x;
    for (auto& s : pathA)
        a = s.tick (a);
    for (auto& s : pathB)
        b = s.tick (b);

    // Path A delayed by a sample lags path B by 90 degrees, so path B is the
    // in-phase part of the analytic signal.
    re = b;
    im = delayed;
    delayed = a;
}

// ---------------------------------------------------------------------------

void ButterworthLowpass::design (double cutoffHz, double sampleRate)
{
    const double k = std::tan (kPi * std::min (cutoffHz, 0.49 * sampleRate) / sampleRate);
    for (int i = 0; i < 4; ++i)
    {
        const double theta = kPi * (2.0 * i + 1.0) / 16.0;
        const double q = 1.0 / (2.0 * std::sin (theta));
        const double norm = 1.0 / (1.0 + k / q + k * k);
        auto& s = sections[(size_t) i];
        s.b0 = (float) (k * k * norm);
        s.b1 = 2.0f * s.b0;
        s.b2 = s.b0;
        s.a1 = (float) (2.0 * (k * k - 1.0) * norm);
        s.a2 = (float) ((1.0 - k / q + k * k) * norm);
    }
    reset();
}

void ButterworthLowpass::reset() noexcept
{
    for (auto& s : sections)
        s.z1 = s.z2 = 0.0f;
}

float ButterworthLowpass::process (float x) noexcept
{
    for (auto& s : sections)
    {
        const float y = s.b0 * x + s.z1;
        s.z1 = flushTiny (s.b1 * x - s.a1 * y + s.z2);
        s.z2 = flushTiny (s.b2 * x - s.a2 * y);
        x = y;
    }
    return x;
}

// ---------------------------------------------------------------------------

void PhaseSpaceAnalyzer::prepare (double analysisRate, int hopSamples, const BandLayout& layout, int numChannels)
{
    channels = numChannels;
    hop = hopSamples;
    bands = layout.count;

    // Crossovers between lag lengths. Above them the shorter lag's better time
    // resolution matters more than the longer lag's frequency resolution.
    const double lowCross = 500.0;
    const double highCross = std::min (3500.0, 0.3 * analysisRate);

    const int midDecimation = nearestPowerOfTwo (analysisRate / 24000.0);
    const int longDecimation = nearestPowerOfTwo (analysisRate / 6000.0);

    // The distribution is periodic in frequency with period rate/2, so a low
    // partial's skirt wraps around to the very top of the short lag's axis.
    // Stopping just past the top band keeps those bins out.
    const double topHz = std::min (0.47 * analysisRate, 1.08 * layout.hz[(size_t) (layout.count - 1)]);

    struct Setup
    {
        int decimation, fftSize;
        double fromHz, toHz, cutoffHz, timePointsHz;
    };
    const Setup setups[kNumResolutions] = {
        { longDecimation, 256, 15.0, lowCross, 0.2 * analysisRate / longDecimation, 1500.0 },
        { midDecimation, 256, lowCross, highCross, 0.42 * analysisRate / midDecimation * 0.5, 6000.0 },
        { 1, 128, highCross, topHz, 0.0, 12000.0 },
    };

    for (int i = 0; i < kNumResolutions; ++i)
    {
        auto& r = resolutions[(size_t) i];
        const auto& s = setups[i];
        r.decimation = s.decimation;
        r.fftSize = s.fftSize;
        r.lagCount = s.fftSize / 2;
        r.fft.prepare (s.fftSize);

        const double rate = analysisRate / s.decimation;

        // A complex sample at this rate contributes phase 2*w*tau to the lag
        // product, so bin k sits at k * rate / (2 * fftSize).
        r.binHz = rate / (2.0 * s.fftSize);

        r.lagWindow.assign ((size_t) r.lagCount, 0.0f);
        const double sigma = r.lagCount / 3.0;
        for (int t = 0; t < r.lagCount; ++t)
            r.lagWindow[(size_t) t] = (float) std::exp (-0.5 * (t / sigma) * (t / sigma));

        r.workRe.assign ((size_t) s.fftSize, 0.0f);
        r.workIm.assign ((size_t) s.fftSize, 0.0f);

        // Time points only have to resolve cross-terms between components that
        // land in the same band, so a sparse grid removes them at a fraction of
        // the work. The grid is irregular on purpose: on a regular one, partials
        // spaced at exactly its rate would alias to a steady false component at
        // their midpoint, where nothing else could ever average it away.
        const double stride = rate / s.timePointsHz;
        const double spread[8] = { 0.75, 1.25, 1.0, 0.5, 1.5, 1.0, 0.75, 1.25 };
        for (size_t t = 0; t < r.timeStrides.size(); ++t)
            r.timeStrides[t] = std::max (1, (int) std::lround (stride * spread[t]));

        r.firstBin = (int) std::ceil (s.fromHz / r.binHz);
        const int lastBin = std::min (s.fftSize - 1, (int) std::floor (s.toHz / r.binHz));
        r.bandBins = lastBin + 1;

        r.binMap.assign ((size_t) r.bandBins, {});
        for (int b = 0; b < r.bandBins; ++b)
        {
            const double f = std::max (b * r.binHz, 1.0);
            const double pos = (erbRate (f) - layout.z0) / layout.dz;
            BinMap m;
            if (pos <= 0.0)
            {
                m.lowBand = 0;
                m.highWeight = 0.0f;
            }
            else if (pos >= layout.count - 1)
            {
                m.lowBand = layout.count - 1;
                m.highWeight = 0.0f;
            }
            else
            {
                m.lowBand = (int) pos;
                m.highWeight = (float) (pos - m.lowBand);
            }
            r.binMap[(size_t) b] = m;
        }

        for (int c = 0; c < channels; ++c)
        {
            r.zRe[(size_t) c].assign ((size_t) (2 * s.fftSize), 0.0f);
            r.zIm[(size_t) c].assign ((size_t) (2 * s.fftSize), 0.0f);
            r.lagRe[(size_t) c].assign ((size_t) r.lagCount, 0.0f);
            r.lagIm[(size_t) c].assign ((size_t) r.lagCount, 0.0f);
            r.slice[(size_t) c].assign ((size_t) r.bandBins, 0.0f);
            if (s.decimation > 1)
            {
                r.lowpassRe[(size_t) c].design (s.cutoffHz, analysisRate);
                r.lowpassIm[(size_t) c].design (s.cutoffHz, analysisRate);
            }
        }
    }

    reset();
}

void PhaseSpaceAnalyzer::reset()
{
    for (auto& h : hilbert)
        h.reset();

    for (auto& r : resolutions)
    {
        r.decimationCounter = 0;
        r.writePos = 0;
        r.timeCounter = 0;
        r.timeStrideIndex = 0;
        r.timePoints = 0;
        for (int c = 0; c < channels; ++c)
        {
            std::fill (r.zRe[(size_t) c].begin(), r.zRe[(size_t) c].end(), 0.0f);
            std::fill (r.zIm[(size_t) c].begin(), r.zIm[(size_t) c].end(), 0.0f);
            std::fill (r.lagRe[(size_t) c].begin(), r.lagRe[(size_t) c].end(), 0.0f);
            std::fill (r.lagIm[(size_t) c].begin(), r.lagIm[(size_t) c].end(), 0.0f);
            std::fill (r.slice[(size_t) c].begin(), r.slice[(size_t) c].end(), 0.0f);
            r.lowpassRe[(size_t) c].reset();
            r.lowpassIm[(size_t) c].reset();
        }
    }

    for (int c = 0; c < kMaxChannels; ++c)
        rawEnergy[(size_t) c].fill (0.0f);
}

void PhaseSpaceAnalyzer::accumulateLagProducts (Resolution& r) noexcept
{
    const int size = r.fftSize;
    const int lags = r.lagCount;

    // Newest sample sits at writePos - 1 + size in the doubled buffer, and the
    // slice centre is half a buffer behind that.
    const int centre = r.writePos - 1 + size - lags + 1;

    for (int c = 0; c < channels; ++c)
    {
        const float* plusRe = r.zRe[(size_t) c].data() + centre;
        const float* plusIm = r.zIm[(size_t) c].data() + centre;
        const float* minusRe = r.zRe[(size_t) c].data() + centre;
        const float* minusIm = r.zIm[(size_t) c].data() + centre;
        float* lagRe = r.lagRe[(size_t) c].data();
        float* lagIm = r.lagIm[(size_t) c].data();

        // z[centre + t] * conj (z[centre - t])
        for (int t = 0; t < lags; ++t)
        {
            lagRe[t] += plusRe[t] * minusRe[-t] + plusIm[t] * minusIm[-t];
            lagIm[t] += plusIm[t] * minusRe[-t] - plusRe[t] * minusIm[-t];
        }
    }
    ++r.timePoints;
}

void PhaseSpaceAnalyzer::computeSlice (Resolution& r) noexcept
{
    const int size = r.fftSize;
    const int lags = r.lagCount;
    const float average = 1.0f / (float) std::max (1, r.timePoints);

    float* re = r.workRe.data();
    float* im = r.workIm.data();
    std::fill (re, re + size, 0.0f);
    std::fill (im, im + size, 0.0f);

    // Two Hermitian lag sequences, one packed into each part of a complex
    // sequence. Hermitian input has a real transform, so a single FFT returns
    // channel 0 in the real part and channel 1 in the imaginary part.
    for (int c = 0; c < channels; ++c)
    {
        float* lagRe = r.lagRe[(size_t) c].data();
        float* lagIm = r.lagIm[(size_t) c].data();

        const float zero = lagRe[0] * average * r.lagWindow[0];
        if (c == 0)
            re[0] = zero;
        else
            im[0] = zero;

        for (int t = 1; t < lags; ++t)
        {
            const float h = r.lagWindow[(size_t) t] * average;
            const float pr = h * lagRe[t];
            const float pi = h * lagIm[t];

            if (c == 0)
            {
                re[t] = pr;
                im[t] = pi;
                re[size - t] = pr;
                im[size - t] = -pi;
            }
            else
            {
                // j * (a + jb) = -b + ja
                re[t] -= pi;
                im[t] += pr;
                re[size - t] += pi;
                im[size - t] += pr;
            }
        }

        std::fill (lagRe, lagRe + lags, 0.0f);
        std::fill (lagIm, lagIm + lags, 0.0f);
    }
    r.timePoints = 0;

    r.fft.forward (re, im);

    const float norm = 1.0f / (float) size;
    for (int b = 0; b < r.bandBins; ++b)
    {
        r.slice[0][(size_t) b] = re[b] * norm;
        if (channels > 1)
            r.slice[1][(size_t) b] = im[b] * norm;
    }
}

void PhaseSpaceAnalyzer::accumulateBands (Resolution& r) noexcept
{
    for (int c = 0; c < channels; ++c)
    {
        const float* now = r.slice[(size_t) c].data();
        auto& energy = rawEnergy[(size_t) c];

        for (int b = r.firstBin; b < r.bandBins; ++b)
        {
            const auto m = r.binMap[(size_t) b];
            const float w = now[b];
            energy[(size_t) m.lowBand] += (1.0f - m.highWeight) * w;
            energy[(size_t) std::min (m.lowBand + 1, bands - 1)] += m.highWeight * w;
        }
    }
}

void PhaseSpaceAnalyzer::pushHop (const float* const* input) noexcept
{
    for (int c = 0; c < channels; ++c)
        rawEnergy[(size_t) c].fill (0.0f);

    for (auto& r : resolutions)
        r.decimationCounter = 0;

    for (int i = 0; i < hop; ++i)
    {
        float zr[kMaxChannels] = {}, zi[kMaxChannels] = {};
        for (int c = 0; c < channels; ++c)
            hilbert[(size_t) c].process (input[c][i], zr[c], zi[c]);

        for (auto& r : resolutions)
        {
            const bool write = ++r.decimationCounter >= r.decimation;
            for (int c = 0; c < channels; ++c)
            {
                float a = zr[c], b = zi[c];
                if (r.decimation > 1)
                {
                    a = r.lowpassRe[(size_t) c].process (a);
                    b = r.lowpassIm[(size_t) c].process (b);
                }
                if (write)
                {
                    r.zRe[(size_t) c][(size_t) r.writePos] = a;
                    r.zIm[(size_t) c][(size_t) r.writePos] = b;
                    r.zRe[(size_t) c][(size_t) (r.writePos + r.fftSize)] = a;
                    r.zIm[(size_t) c][(size_t) (r.writePos + r.fftSize)] = b;
                }
            }
            if (write)
            {
                r.decimationCounter = 0;
                r.writePos = (r.writePos + 1) & (r.fftSize - 1);
                if (++r.timeCounter >= r.timeStrides[(size_t) r.timeStrideIndex])
                {
                    r.timeCounter = 0;
                    r.timeStrideIndex = (r.timeStrideIndex + 1) & 7;
                    accumulateLagProducts (r);
                }
            }
        }
    }

    for (auto& r : resolutions)
    {
        computeSlice (r);
        accumulateBands (r);
    }

}

} // namespace colgate
