// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Tests of the real plug-in class: parameters, state, bus layouts and the UI.
// `ColgatePluginTests snapshot <folder>` also renders the UI to PNG files.
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SessionBypass.h"
#include "TestSignals.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>

#include <cstdio>

using namespace colgate;

namespace
{
int failures = 0;
int checks = 0;

void check (bool ok, const char* what)
{
    ++checks;
    if (! ok)
        ++failures;
    std::printf ("    %s %s\n", ok ? "ok  " : "FAIL", what);
}

void setParameter (ColgateProcessor& p, const char* id, float value)
{
    auto* parameter = p.parameters().getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

float getParameter (ColgateProcessor& p, const char* id)
{
    return p.parameters().getRawParameterValue (id)->load();
}

juce::AudioBuffer<float> demoBuffer (double rate, double seconds, int channels)
{
    const auto demo = colgate_test::makeDemoMix (rate, seconds, channels);
    juce::AudioBuffer<float> buffer (channels, (int) demo.mix[0].size());
    for (int c = 0; c < channels; ++c)
        buffer.copyFrom (c, 0, demo.mix[(size_t) c].data(), buffer.getNumSamples());
    return buffer;
}

void run (ColgateProcessor& p, juce::AudioBuffer<float>& audio, int block)
{
    juce::MidiBuffer midi;
    const int busChannels = p.getTotalNumInputChannels();
    juce::AudioBuffer<float> work (juce::jmax (busChannels, audio.getNumChannels()), block);
    for (int s = 0; s + block <= audio.getNumSamples(); s += block)
    {
        work.clear();
        for (int c = 0; c < audio.getNumChannels(); ++c)
            work.copyFrom (c, 0, audio, c, s, block);
        p.processBlock (work, midi);
        for (int c = 0; c < audio.getNumChannels(); ++c)
            audio.copyFrom (c, s, work, c, 0, block);
    }
}

void testLayouts()
{
    std::printf ("\nBus layouts\n");
    ColgateProcessor p;
    using Set = juce::AudioChannelSet;

    juce::AudioProcessor::BusesLayout stereo;
    stereo.inputBuses.add (Set::stereo());
    stereo.inputBuses.add (Set::disabled());
    stereo.outputBuses.add (Set::stereo());
    check (p.isBusesLayoutSupported (stereo), "stereo in, stereo out");

    auto mono = stereo;
    mono.inputBuses.getReference (0) = Set::mono();
    mono.outputBuses.getReference (0) = Set::mono();
    check (p.isBusesLayoutSupported (mono), "mono in, mono out");

    auto mixed = stereo;
    mixed.outputBuses.getReference (0) = Set::mono();
    check (! p.isBusesLayoutSupported (mixed), "stereo in, mono out is refused");

    auto surround = stereo;
    surround.inputBuses.getReference (0) = Set::create5point1();
    surround.outputBuses.getReference (0) = Set::create5point1();
    check (! p.isBusesLayoutSupported (surround), "surround is refused");

    auto side = stereo;
    side.inputBuses.getReference (1) = Set::stereo();
    check (p.isBusesLayoutSupported (side), "stereo sidechain");
    side.inputBuses.getReference (1) = Set::mono();
    check (p.isBusesLayoutSupported (side), "mono sidechain");
}

void testState()
{
    std::printf ("\nState\n");
    ColgateProcessor a;
    setParameter (a, ids::recover, 42.0f);
    setParameter (a, ids::tame, 13.0f);
    setParameter (a, ids::bias, -30.0f);
    setParameter (a, ids::brighten, 25.0f);
    setParameter (a, ids::boost, 3.5f);
    setParameter (a, ids::gain, -2.0f);
    setParameter (a, ids::rangeLow, 200.0f);
    setParameter (a, ids::rangeHigh, 5000.0f);
    setParameter (a, ids::sidechain, 1.0f);
    setParameter (a, ids::qualityRealtime, 2.0f);
    setParameter (a, ids::mode, 1.0f);

    juce::MemoryBlock saved;
    a.getStateInformation (saved);

    ColgateProcessor b;
    b.setStateInformation (saved.getData(), (int) saved.getSize());

    const auto near = [] (float x, float y, float tolerance) { return std::abs (x - y) <= tolerance; };
    check (near (getParameter (b, ids::recover), 42.0f, 0.06f), "Recover comes back");
    check (near (getParameter (b, ids::tame), 13.0f, 0.06f), "Tame comes back");
    check (near (getParameter (b, ids::bias), -30.0f, 0.06f), "Bias comes back");
    check (near (getParameter (b, ids::brighten), 25.0f, 0.06f), "Brighten comes back");
    check (near (getParameter (b, ids::boost), 3.5f, 0.06f), "Boost comes back");
    check (near (getParameter (b, ids::gain), -2.0f, 0.06f), "Gain comes back");
    check (near (getParameter (b, ids::rangeLow), 200.0f, 1.0f), "first limiter comes back");
    check (near (getParameter (b, ids::rangeHigh), 5000.0f, 5.0f), "second limiter comes back");
    check (getParameter (b, ids::sidechain) > 0.5f, "Sidechain comes back");
    check (near (getParameter (b, ids::qualityRealtime), 2.0f, 0.01f), "quality comes back with the session");
    check (near (getParameter (b, ids::mode), 1.0f, 0.01f), "the mode comes back with the session");

    // Bad data must be ignored, not crash.
    const char junk[] = "this is not a saved state";
    b.setStateInformation (junk, (int) sizeof (junk));
    check (near (getParameter (b, ids::recover), 42.0f, 0.06f), "junk state data is ignored");
}

void testPresets()
{
    std::printf ("\nPresets\n");
    ColgateProcessor p;
    check (p.getNumPrograms() == kNumFactoryPresets, "every preset is exposed to the host");
    for (int i = 0; i < p.getNumPrograms(); ++i)
    {
        p.setCurrentProgram (i);
        const auto& preset = kFactoryPresets[i];
        const bool ok = std::abs (getParameter (p, ids::recover) - preset.recover) < 0.06f
                        && std::abs (getParameter (p, ids::tame) - preset.tame) < 0.06f
                        && std::abs (getParameter (p, ids::boost) - preset.boost) < 0.06f
                        && std::abs (getParameter (p, ids::rangeLow) - preset.rangeLow) < 1.0f
                        && std::abs (getParameter (p, ids::mode) - (float) preset.mode) < 0.01f;
        check (ok && p.getProgramName (i) == preset.name, preset.name);
    }
    p.setCurrentProgram (0);
    check (juce::approximatelyEqual (getParameter (p, ids::recover), 0.0f) && getParameter (p, ids::rangeHigh) > 19000.0f, "Clean slate clears everything");
}

void testAudio()
{
    std::printf ("\nAudio through the real class\n");
    const double rate = 48000.0;
    const int block = 512;
    for (int channels : { 1, 2 })
    {
        ColgateProcessor p;
        juce::AudioProcessor::BusesLayout layout;
        const auto set = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        layout.inputBuses.add (set);
        layout.inputBuses.add (juce::AudioChannelSet::disabled());
        layout.outputBuses.add (set);
        check (p.setBusesLayout (layout), "layout accepted");
        p.prepareToPlay (rate, block);
        check (p.getLatencySamples() == 256, "latency is reported to the host");

        auto clean = demoBuffer (rate, 1.0, channels);
        auto copy = clean;
        run (p, copy, block);

        const int latency = p.getLatencySamples();
        bool exact = true;
        for (int c = 0; c < channels; ++c)
            for (int n = latency; n < copy.getNumSamples() - block; ++n)
                exact = exact && juce::exactlyEqual (copy.getSample (c, n), clean.getSample (c, n - latency));
        check (exact, channels == 1 ? "mono default state is bit-exact" : "stereo default state is bit-exact");

        setParameter (p, ids::recover, 60.0f);
        setParameter (p, ids::tame, 60.0f);
        auto processed = demoBuffer (rate, 3.0, channels);
        run (p, processed, block);
        bool finite = true;
        for (int c = 0; c < channels; ++c)
            for (int n = 0; n < processed.getNumSamples(); ++n)
                finite = finite && std::isfinite (processed.getSample (c, n));
        check (finite, "processing stays finite");

        double difference = 0.0;
        auto reference = demoBuffer (rate, 3.0, channels);
        for (int n = latency + 48000; n < processed.getNumSamples() - block; ++n)
            difference += std::abs (processed.getSample (0, n) - reference.getSample (0, n - latency));
        check (difference > 1.0, "and audibly does something");
    }

    {
        // A stereo sidechain with the sidechain switch on.
        ColgateProcessor p;
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (juce::AudioChannelSet::stereo());
        layout.inputBuses.add (juce::AudioChannelSet::stereo());
        layout.outputBuses.add (juce::AudioChannelSet::stereo());
        check (p.setBusesLayout (layout), "sidechain layout accepted");
        p.prepareToPlay (rate, block);
        setParameter (p, ids::recover, 100.0f);
        setParameter (p, ids::tame, 100.0f);
        setParameter (p, ids::sidechain, 1.0f);

        auto main = demoBuffer (rate, 2.0, 2);
        auto side = demoBuffer (rate, 2.0, 2);
        juce::AudioBuffer<float> work (4, block);
        juce::MidiBuffer midi;
        bool finite = true;
        for (int s = 0; s + block <= main.getNumSamples(); s += block)
        {
            work.copyFrom (0, 0, main, 0, s, block);
            work.copyFrom (1, 0, main, 1, s, block);
            work.copyFrom (2, 0, side, 0, s, block);
            work.copyFrom (3, 0, side, 1, s, block);
            p.processBlock (work, midi);
            for (int n = 0; n < block; ++n)
                finite = finite && std::isfinite (work.getSample (0, n));
        }
        check (finite, "sidechain processing stays finite");
    }

    {
        ColgateProcessor p;
        p.prepareToPlay (rate, block);
        setParameter (p, ids::recover, 100.0f);
        setParameter (p, ids::tame, 100.0f);

        // Session bypass: this instance reacts without its own parameter moving.
        SessionBypass::set (true);
        auto clean = demoBuffer (rate, 1.0, 2);
        auto copy = clean;
        run (p, copy, block);
        SessionBypass::set (false);
        const int latency = p.getLatencySamples();
        double worst = 0.0;
        for (int n = 24000; n < copy.getNumSamples() - block; ++n)
            worst = std::max (worst, (double) std::abs (copy.getSample (0, n) - clean.getSample (0, n - latency)));
        check (worst < 1.0e-9, "session bypass passes audio exactly, latency intact");
        check (getParameter (p, ids::bypass) < 0.5f, "and leaves the instance's own Bypass alone");
    }
}

void testEditor()
{
    std::printf ("\nEditor\n");
    ColgateProcessor p;
    p.prepareToPlay (48000.0, 512);

    for (int i = 0; i < 2; ++i)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        check (editor != nullptr, "the editor opens");
        check (editor->getWidth() > 0 && editor->getHeight() > 0, "with a sensible size");
        if (auto* typed = dynamic_cast<ColgateEditor*> (editor.get()))
            typed->refreshForSnapshot();
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
        check (image.isValid(), "and draws");
    }

    setParameter (p, ids::mode, 1.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
    check (editor->createComponentSnapshot (editor->getLocalBounds()).isValid(), "and draws in Master mode");
    setParameter (p, ids::mode, 0.0f);
}

void testModes()
{
    std::printf ("\nBasic and Master\n");
    const auto render = [] (float mode) {
        ColgateProcessor p;
        p.prepareToPlay (48000.0, 512);
        setParameter (p, ids::recover, 50.0f);
        setParameter (p, ids::tame, 50.0f);
        setParameter (p, ids::mode, mode);
        auto audio = demoBuffer (48000.0, 3.0, 2);
        run (p, audio, 512);
        check (p.getLatencySamples() == 256, "the latency doesn't depend on the mode");
        return audio;
    };
    const auto basic = render (0.0f);
    const auto master = render (1.0f);
    double difference = 0.0, level = 1.0e-30;
    for (int n = 48000; n < basic.getNumSamples(); ++n)
    {
        const double d = (double) basic.getSample (0, n) - master.getSample (0, n);
        difference += d * d;
        level += (double) basic.getSample (0, n) * basic.getSample (0, n);
    }
    const double db = 10.0 * std::log10 (difference / level + 1.0e-30);
    std::printf ("    Master differs from Basic by %.1f dB\n", db);
    check (db > -80.0 && db < -6.0, "the mode parameter reaches the engine, and the two stay close");
}

void snapshot (const juce::File& folder)
{
    folder.createDirectory();
    const auto save = [&folder] (juce::Component& c, const juce::String& name, float scale = 2.0f) {
        const auto image = c.createComponentSnapshot (c.getLocalBounds(), true, scale);
        juce::PNGImageFormat png;
        auto file = folder.getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream out (file);
        png.writeImageToStream (image, out);
        std::printf ("wrote %s\n", file.getFullPathName().toRawUTF8());
    };

    const auto feed = [] (ColgateProcessor& p, double seconds) {
        auto audio = demoBuffer (48000.0, seconds, 2);
        run (p, audio, 512);
    };

    {
        ColgateProcessor p;
        p.prepareToPlay (48000.0, 512);
        setParameter (p, ids::recover, 75.0f);
        setParameter (p, ids::tame, 60.0f);
        setParameter (p, ids::brighten, -12.0f);
        setParameter (p, ids::boost, 1.5f);
        feed (p, 3.0);
        ColgateEditor editor (p);
        editor.refreshForSnapshot();
        feed (p, 0.2);
        editor.refreshForSnapshot();
        save (editor, "ui-main.png");
    }
    {
        ColgateProcessor p;
        p.prepareToPlay (48000.0, 512);
        setParameter (p, ids::recover, 15.0f);
        setParameter (p, ids::tame, 15.5f);
        setParameter (p, ids::boost, 0.5f);
        setParameter (p, ids::mode, 1.0f);
        feed (p, 3.0);
        ColgateEditor editor (p);
        editor.refreshForSnapshot();
        feed (p, 0.2);
        editor.refreshForSnapshot();
        save (editor, "ui-master.png");
    }
    {
        ColgateProcessor p;
        p.prepareToPlay (48000.0, 512);
        setParameter (p, ids::recover, 90.0f);
        setParameter (p, ids::tame, 90.0f);
        setParameter (p, ids::rangeLow, 200.0f);
        setParameter (p, ids::rangeHigh, 5000.0f);
        p.setCurrentProgram (5);
        feed (p, 3.0);
        ColgateEditor editor (p);
        editor.refreshForSnapshot();
        save (editor, "ui-limiters.png");
    }
    {
        ColgateProcessor p;
        p.prepareToPlay (48000.0, 512);
        setParameter (p, ids::boost, 15.0f);
        feed (p, 2.0);
        ColgateEditor editor (p);
        editor.refreshForSnapshot();
        save (editor, "ui-boost.png");
    }
    {
        ColgateProcessor p;
        p.prepareToPlay (48000.0, 512);
        setParameter (p, ids::recover, 40.0f);
        feed (p, 1.0);
        SessionBypass::set (true);
        ColgateEditor editor (p);
        editor.refreshForSnapshot();
        save (editor, "ui-session-bypass.png");
        SessionBypass::set (false);
    }
    {
        ColgateProcessor p;
        p.prepareToPlay (48000.0, 512);
        InfoPage page (p, UserSettings {});
        page.setBounds (0, 0, ColgateEditor::kBaseWidth, ColgateEditor::kBaseHeight);
        save (page, "ui-info.png");
    }
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    if (argc > 2 && juce::String (argv[1]) == "snapshot")
    {
        snapshot (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
        return 0;
    }

    testLayouts();
    testState();
    testPresets();
    testModes();
    testAudio();

    if (juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr)
        testEditor();
    else
        std::printf ("\nNo display here, so the editor tests were skipped.\n");

    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
