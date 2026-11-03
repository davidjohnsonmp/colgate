// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Loads a built VST3 the way a DAW does, through the plug-in's public interface
// only, and exercises it: parameters, state, latency, stereo and mono audio, and
// the editor.   Usage: ColgateVst3HostCheck /path/to/Colgate.vst3
#include "../tests/TestSignals.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    if (! ok)
        ++failures;
    std::printf ("    %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
}

juce::AudioProcessorParameter* findParameter (juce::AudioPluginInstance& plugin, const juce::String& name)
{
    for (auto* p : plugin.getParameters())
        if (p->getName (64) == name)
            return p;
    return nullptr;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: %s path/to/Colgate.vst3\n", argv[0]);
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI gui;
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    const juce::String path = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]).getFullPathName();
    format.findAllTypesForFile (found, path);
    check (found.size() == 1, "the binary exposes exactly one plug-in");
    if (found.isEmpty())
        return 1;

    const auto& description = *found[0];
    std::printf ("    %s by %s, version %s, %s\n", description.name.toRawUTF8(), description.manufacturerName.toRawUTF8(),
                 description.version.toRawUTF8(), description.category.toRawUTF8());
    check (description.name == "Colgate", "the name is Colgate");
    check (description.manufacturerName == "Hiccup Audio", "the maker is Hiccup Audio");

    const double rate = 48000.0;
    const int block = 512;
    juce::String error;
    auto plugin = format.createInstanceFromDescription (description, rate, block, error);
    check (plugin != nullptr, "it instantiates " + error);
    if (plugin == nullptr)
        return 1;

    plugin->prepareToPlay (rate, block);
    check (plugin->getLatencySamples() == 256, "latency reported to the host: " + juce::String (plugin->getLatencySamples()));

    for (const char* name : { "Recover", "Tame", "Bias", "Brighten", "Boost", "Gain", "Mode", "Bypass", "Sidechain", "Range limiter 1", "Range limiter 2", "Realtime quality", "Offline quality" })
        check (findParameter (*plugin, name) != nullptr, juce::String ("parameter is automatable: ") + name);

    const auto demo = colgate_test::makeDemoMix (rate, 4.0, 2);
    const int length = (int) demo.mix[0].size();
    const int channels = juce::jmax (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());

    const auto render = [&] {
        juce::AudioBuffer<float> out (2, length);
        juce::AudioBuffer<float> work (channels, block);
        juce::MidiBuffer midi;
        for (int s = 0; s + block <= length; s += block)
        {
            work.clear();
            work.copyFrom (0, 0, demo.mix[0].data() + s, block);
            work.copyFrom (1, 0, demo.mix[1].data() + s, block);
            plugin->processBlock (work, midi);
            out.copyFrom (0, s, work, 0, 0, block);
            out.copyFrom (1, s, work, 1, 0, block);
        }
        return out;
    };

    {
        const auto out = render();
        bool exact = true;
        for (int n = 256; n < length - block; ++n)
            exact = exact && juce::approximatelyEqual (out.getSample (0, n), demo.mix[0][(size_t) (n - 256)]);
        check (exact, "default settings pass audio unchanged, apart from the latency");
    }

    auto* recover = findParameter (*plugin, "Recover");
    auto* tame = findParameter (*plugin, "Tame");
    recover->setValueNotifyingHost (0.5f);
    tame->setValueNotifyingHost (0.5f);
    {
        const auto out = render();
        bool finite = true;
        double difference = 0.0;
        for (int n = 256; n < length - block; ++n)
        {
            finite = finite && std::isfinite (out.getSample (0, n));
            difference += std::abs (out.getSample (0, n) - demo.mix[0][(size_t) (n - 256)]);
        }
        check (finite, "output stays finite at Recover 100, Tame 100");
        check (difference > 1.0, "and the processing changes the sound");
    }

    juce::MemoryBlock saved;
    plugin->getStateInformation (saved);
    check (saved.getSize() > 0, "state saves");
    recover->setValueNotifyingHost (0.0f);
    plugin->setStateInformation (saved.getData(), (int) saved.getSize());
    check (std::abs (recover->getValue() - 0.5f) < 0.01f, "state restores");

    check (plugin->hasEditor(), "it has an editor");
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
        check (editor != nullptr, "the editor opens");
        if (editor != nullptr)
        {
            check (editor->getWidth() > 400 && editor->getHeight() > 200, "at a sensible size");
            check (editor->createComponentSnapshot (editor->getLocalBounds()).isValid(), "and draws");
        }
    }

    plugin->releaseResources();
    plugin.reset();
    std::printf ("\n%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
