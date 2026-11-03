// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Runs a WAV file through the engine, for measuring and comparing outside a DAW.
// The output is trimmed of the plug-in's latency, so it lines up with the input.
//
//   colgate_render in.wav out.wav --recover 50 --tame 50 --bias 0 --brighten 0
//                  --boost 0 --gain 0 --low 20 --high 20000 --quality normal --mode basic
#include "ColgateEngine.h"
#include "WavFile.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace colgate;
using namespace colgate_tools;

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: %s in.wav out.wav [--recover N] [--tame N] [--bias N] [--brighten N] [--boost dB] [--gain dB] [--low Hz] [--high Hz] [--quality normal|better|best] [--mode basic|master]\n", argv[0]);
        return 2;
    }

    Audio audio;
    if (! readWav (argv[1], audio) || audio.channels < 1 || audio.channels > 2)
    {
        std::fprintf (stderr, "Could not read %s as a mono or stereo PCM or float WAV\n", argv[1]);
        return 1;
    }

    EngineParameters p;
    Quality quality = Quality::normal;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        const std::string option = argv[i];
        const std::string value = argv[i + 1];
        const float number = (float) std::atof (value.c_str());
        if (option == "--recover") p.recoverPercent = number;
        else if (option == "--tame") p.tamePercent = number;
        else if (option == "--bias") p.biasPercent = number;
        else if (option == "--brighten") p.brightenPercent = number;
        else if (option == "--boost") p.boostDb = number;
        else if (option == "--gain") p.gainDb = number;
        else if (option == "--low") p.rangeLowHz = number;
        else if (option == "--high") p.rangeHighHz = number;
        else if (option == "--mode") p.mode = value == "master" ? Mode::master : Mode::basic;
        else if (option == "--quality") quality = value == "best" ? Quality::best : (value == "better" ? Quality::better : Quality::normal);
        else
        {
            std::fprintf (stderr, "Unknown option %s\n", option.c_str());
            return 2;
        }
    }

    auto engine = std::make_unique<ColgateEngine>();
    engine->setParameters (p);
    engine->prepare (audio.rate, audio.channels);
    engine->setQuality (quality);
    engine->reset();

    const int latency = engine->latencySamples();
    const int length = (int) audio.samples[0].size();
    for (auto& channel : audio.samples)
        channel.resize ((size_t) (length + latency), 0.0f);

    const int total = length + latency;
    const int block = 512;
    for (int s = 0; s < total; s += block)
    {
        const int n = std::min (block, total - s);
        float* pointers[kMaxChannels] = {};
        for (int c = 0; c < audio.channels; ++c)
            pointers[c] = audio.samples[(size_t) c].data() + s;
        engine->process (pointers, nullptr, n);
    }

    for (auto& channel : audio.samples)
        channel.erase (channel.begin(), channel.begin() + latency);

    if (! writeWav (argv[2], audio))
    {
        std::fprintf (stderr, "Could not write %s\n", argv[2]);
        return 1;
    }
    return 0;
}
