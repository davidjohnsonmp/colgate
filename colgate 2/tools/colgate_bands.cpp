// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Writes the analyser's raw band energies for a WAV file, for tools/calibration.
// The file holds little-endian 32-bit floats, one row per 128 samples at 48 kHz:
// the mean across channels of every band, clipped at zero.
//
//   colgate_bands in.wav out.f32
#include "PhaseSpaceAnalyzer.h"
#include "WavFile.h"

#include <cstdio>
#include <memory>

using namespace colgate;

int main (int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf (stderr, "usage: %s in.wav out.f32\n", argv[0]);
        return 2;
    }

    colgate_tools::Audio audio;
    if (! colgate_tools::readWav (argv[1], audio) || audio.channels < 1 || audio.channels > kMaxChannels)
    {
        std::fprintf (stderr, "Couldn't read %s as mono or stereo WAV\n", argv[1]);
        return 1;
    }

    BandLayout layout;
    layout.prepare (audio.rate);
    auto analyser = std::make_unique<PhaseSpaceAnalyzer>();
    const int hop = 128;
    analyser->prepare (audio.rate, hop, layout, audio.channels);
    analyser->reset();

    FILE* out = std::fopen (argv[2], "wb");
    if (out == nullptr)
        return 1;

    std::vector<float> row ((size_t) layout.count);
    for (size_t s = 0; s + (size_t) hop <= audio.samples[0].size(); s += (size_t) hop)
    {
        const float* input[kMaxChannels] = {};
        for (int c = 0; c < audio.channels; ++c)
            input[c] = audio.samples[(size_t) c].data() + s;
        analyser->pushHop (input);
        for (int k = 0; k < layout.count; ++k)
        {
            float sum = 0.0f;
            for (int c = 0; c < audio.channels; ++c)
                sum += analyser->bandEnergy (c)[k];
            row[(size_t) k] = std::max (sum / (float) audio.channels, 0.0f);
        }
        std::fwrite (row.data(), sizeof (float), row.size(), out);
    }
    std::fclose (out);
    return 0;
}
