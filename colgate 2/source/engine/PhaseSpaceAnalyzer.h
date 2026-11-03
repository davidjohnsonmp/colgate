// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The analyser's front end: a smoothed pseudo Wigner-Ville distribution.
//
// A spectrogram is the most common time-frequency distribution, but it is only
// one member of Cohen's class. The Wigner-Ville distribution at the heart of
// that class is the time-frequency twin of the Wigner function from the
// phase-space (deformation quantization) formulation of quantum mechanics:
// time and frequency take the place of position and momentum. Its lag product
// spans twice the window of an equally long spectrogram frame, so it separates
// close partials about twice as well for the same decision time. The price is
// oscillating cross-terms between components, which time smoothing removes.
//
// This is public mathematics (Wigner 1932, Moyal 1949, Cohen 1966). It is an
// independent implementation of the idea, not anyone's proprietary transform.
//
// Three lag lengths follow the ear's constant-Q resolution: long for the bass,
// medium for the mids, short for the treble.
#pragma once

#include "Fft.h"
#include "Psychoacoustics.h"

#include <array>
#include <vector>

namespace colgate
{

// Niemitalo's 8th-order all-pass Hilbert pair. Cheap, streaming and within two
// degrees of quadrature across 20 Hz to 20 kHz at 48 kHz.
class AllpassHilbert
{
public:
    void reset() noexcept;
    void process (float x, float& re, float& im) noexcept;

private:
    struct Section
    {
        float c = 0.0f, x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
        float tick (float x) noexcept;
    };

    static constexpr float kA[4] = { 0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f };
    static constexpr float kB[4] = { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f };

    std::array<Section, 4> pathA, pathB;
    float delayed = 0.0f;
    bool initialised = false;
};

// Fourth-section Butterworth low-pass in direct form II transposed. Used to
// band-limit before decimating. Eighth order is plenty for an energy estimate.
class ButterworthLowpass
{
public:
    void design (double cutoffHz, double sampleRate);
    void reset() noexcept;
    float process (float x) noexcept;

private:
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    };
    std::array<Biquad, 4> sections;
};

class PhaseSpaceAnalyzer
{
public:
    static constexpr int kNumResolutions = 3;

    void prepare (double analysisRate, int hopSamples, const BandLayout& layout, int numChannels);
    void reset();

    // Takes one hop of analysis-rate audio per channel and updates all outputs.
    void pushHop (const float* const* input) noexcept;

    const float* bandEnergy (int channel) const noexcept { return rawEnergy[(size_t) channel].data(); }

    // Latest distribution slice at one resolution.
    const float* spectrum (int resolution, int channel) const noexcept
    {
        return resolutions[(size_t) resolution].slice[(size_t) channel].data();
    }

    int spectrumSize (int resolution) const noexcept { return resolutions[(size_t) resolution].bandBins; }
    double binHz (int resolution) const noexcept { return resolutions[(size_t) resolution].binHz; }

private:
    struct BinMap
    {
        int lowBand = 0;
        float highWeight = 0.0f;
    };

    struct Resolution
    {
        int decimation = 1;
        int fftSize = 128;
        int lagCount = 64;
        int bandBins = 0;
        int firstBin = 0;
        double binHz = 0.0;
        SplitFft fft;
        std::vector<float> lagWindow;
        std::vector<float> workRe, workIm;
        // Each buffer holds its samples twice, so a run of lags is contiguous
        // however the write position falls. The accumulators hold the lag
        // products, averaged over time points within the hop: the time kernel
        // of the distribution. Integrating cross-terms away in the lag domain
        // is what keeps the band energies steady and overwhelmingly positive.
        std::array<std::vector<float>, kMaxChannels> zRe, zIm, lagRe, lagIm;
        std::array<int, 8> timeStrides {};
        int timeStrideIndex = 0;
        int timeCounter = 0;
        int timePoints = 0;
        std::array<std::vector<float>, kMaxChannels> slice;
        std::vector<BinMap> binMap;
        std::array<ButterworthLowpass, kMaxChannels> lowpassRe, lowpassIm;
        int decimationCounter = 0;
        int writePos = 0;
    };

    void accumulateLagProducts (Resolution& r) noexcept;
    void computeSlice (Resolution& r) noexcept;
    void accumulateBands (Resolution& r) noexcept;

    int channels = 2;
    int hop = 128;
    int bands = 0;
    std::array<AllpassHilbert, kMaxChannels> hilbert;
    std::array<Resolution, kNumResolutions> resolutions;

    std::array<std::array<float, kMaxBands>, kMaxChannels> rawEnergy {};
};

} // namespace colgate
