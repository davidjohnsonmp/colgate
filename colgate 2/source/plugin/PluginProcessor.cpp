// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SessionBypass.h"

namespace colgate
{

juce::AudioProcessorValueTreeState::ParameterLayout ColgateProcessor::createLayout()
{
    using namespace juce;

    const auto percent = [] (const char* id, const char* name, float low, float high) {
        return std::make_unique<AudioParameterFloat> (
            ParameterID { id, 1 }, name, NormalisableRange<float> (low, high, 0.1f), 0.0f,
            AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction ([] (float v, int) {
                return String (v, 1) + " %";
            }));
    };
    const auto decibels = [] (const char* id, const char* name, float low, float high) {
        return std::make_unique<AudioParameterFloat> (
            ParameterID { id, 1 }, name, NormalisableRange<float> (low, high, 0.1f), 0.0f,
            AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction ([] (float v, int) {
                return String (v, 1) + " dB";
            }));
    };
    const auto frequency = [] (const char* id, const char* name, float initial) {
        NormalisableRange<float> range (20.0f, 20000.0f);
        range.setSkewForCentre (632.0f);
        return std::make_unique<AudioParameterFloat> (
            ParameterID { id, 1 }, name, range, initial,
            AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction ([] (float v, int) {
                return v >= 1000.0f ? String (v / 1000.0f, 2) + " kHz" : String (roundToInt (v)) + " Hz";
            }));
    };

    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (percent (ids::recover, "Recover", 0.0f, 200.0f));
    layout.add (percent (ids::tame, "Tame", 0.0f, 200.0f));
    layout.add (percent (ids::bias, "Bias", -100.0f, 100.0f));
    layout.add (percent (ids::brighten, "Brighten", -100.0f, 100.0f));
    layout.add (decibels (ids::boost, "Boost", -50.0f, 50.0f));
    layout.add (decibels (ids::gain, "Gain", -24.0f, 24.0f));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ids::mode, 1 }, "Mode", StringArray { "Basic", "Master" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ids::bypass, 1 }, "Bypass", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ids::sidechain, 1 }, "Sidechain", false));
    layout.add (frequency (ids::rangeLow, "Range limiter 1", 20.0f));
    layout.add (frequency (ids::rangeHigh, "Range limiter 2", 20000.0f));

    const StringArray qualities { "Normal", "Better", "Best" };
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ids::qualityRealtime, 1 }, "Realtime quality", qualities, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ids::qualityOffline, 1 }, "Offline quality", qualities, 1));
    return layout;
}

ColgateProcessor::ColgateProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      state (*this, nullptr, "ColgateState", createLayout())
{
    recover = state.getRawParameterValue (ids::recover);
    tame = state.getRawParameterValue (ids::tame);
    bias = state.getRawParameterValue (ids::bias);
    brighten = state.getRawParameterValue (ids::brighten);
    boost = state.getRawParameterValue (ids::boost);
    gain = state.getRawParameterValue (ids::gain);
    mode = state.getRawParameterValue (ids::mode);
    rangeLow = state.getRawParameterValue (ids::rangeLow);
    rangeHigh = state.getRawParameterValue (ids::rangeHigh);
    sidechain = state.getRawParameterValue (ids::sidechain);
    bypass = state.getRawParameterValue (ids::bypass);
    qualityRealtime = state.getRawParameterValue (ids::qualityRealtime);
    qualityOffline = state.getRawParameterValue (ids::qualityOffline);
    bypassParameter = dynamic_cast<juce::AudioParameterBool*> (state.getParameter (ids::bypass));

    engine.prepare (48000.0, 2);
    setLatencySamples (engine.latencySamples());
}

bool ColgateProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    if (input != output)
        return false;
    if (input != juce::AudioChannelSet::mono() && input != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.inputBuses.size() > 1)
    {
        const auto side = layouts.getChannelSet (true, 1);
        if (! side.isDisabled() && side != juce::AudioChannelSet::mono() && side != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

Quality ColgateProcessor::wantedQuality() const noexcept
{
    const auto* parameter = isNonRealtime() ? qualityOffline : qualityRealtime;
    switch (juce::roundToInt (parameter->load (std::memory_order_relaxed)))
    {
        case 1: return Quality::better;
        case 2: return Quality::best;
        default: return Quality::normal;
    }
}

void ColgateProcessor::prepareToPlay (double sampleRate, int)
{
    const int channels = juce::jlimit (1, kMaxChannels, getMainBusNumInputChannels());
    numChannels.store (channels, std::memory_order_relaxed);

    engine.setParameters (currentParameters (false));
    engine.prepare (sampleRate, channels);
    engine.setQuality (wantedQuality());
    engine.reset();
    wasOffline = isNonRealtime();
    setLatencySamples (engine.latencySamples());
}

double ColgateProcessor::getTailLengthSeconds() const
{
    return engine.tailSeconds();
}

EngineParameters ColgateProcessor::currentParameters (bool forceBypass) const noexcept
{
    EngineParameters p;
    p.recoverPercent = recover->load (std::memory_order_relaxed);
    p.tamePercent = tame->load (std::memory_order_relaxed);
    p.biasPercent = bias->load (std::memory_order_relaxed);
    p.brightenPercent = brighten->load (std::memory_order_relaxed);
    p.boostDb = boost->load (std::memory_order_relaxed);
    p.gainDb = gain->load (std::memory_order_relaxed);
    p.mode = mode->load (std::memory_order_relaxed) > 0.5f ? Mode::master : Mode::basic;
    p.rangeLowHz = rangeLow->load (std::memory_order_relaxed);
    p.rangeHighHz = rangeHigh->load (std::memory_order_relaxed);
    p.useSidechain = sidechain->load (std::memory_order_relaxed) > 0.5f;
    p.bypass = forceBypass || bypass->load (std::memory_order_relaxed) > 0.5f || SessionBypass::isActive();
    return p;
}

void ColgateProcessor::render (juce::AudioBuffer<float>& buffer, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    auto main = getBusBuffer (buffer, true, 0);
    const int channels = juce::jmin (main.getNumChannels(), numChannels.load (std::memory_order_relaxed));
    const int total = buffer.getNumSamples();

    // Anything beyond the main channels must leave silent.
    for (int c = channels; c < getTotalNumOutputChannels(); ++c)
        if (c < buffer.getNumChannels())
            buffer.clear (c, 0, total);

    if (channels == 0 || total == 0)
        return;

    const auto p = currentParameters (forceBypass);
    engine.setParameters (p);

    // The engine ducks the wet path across a change made while playing. A bounce
    // has to start at its own quality instead, from a clean slate.
    engine.setQuality (wantedQuality());
    const bool offline = isNonRealtime();
    if (offline != wasOffline)
    {
        wasOffline = offline;
        engine.reset();
    }

    for (int c = 0; c < channels; ++c)
        inputPeaks[(size_t) c].push (main.getReadPointer (c), total);

    // Fold the sidechain to the main channel count. With no sidechain bus at
    // all, the engine keeps analysing the main input.
    juce::AudioBuffer<float> side;
    int sideChannels = 0;
    if (p.useSidechain && getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
    {
        side = getBusBuffer (buffer, true, 1);
        sideChannels = side.getNumChannels();
    }

    float* mainPointers[kMaxChannels] = {};
    for (int c = 0; c < channels; ++c)
        mainPointers[c] = main.getWritePointer (c);

    int done = 0;
    while (done < total)
    {
        const int chunk = juce::jmin (total - done, kScratchSamples);
        float* chunkMain[kMaxChannels] = {};
        const float* chunkSide[kMaxChannels] = {};

        for (int c = 0; c < channels; ++c)
            chunkMain[c] = mainPointers[c] + done;

        if (sideChannels > 0)
        {
            if (sideChannels == channels)
            {
                for (int c = 0; c < channels; ++c)
                    chunkSide[c] = side.getReadPointer (c) + done;
            }
            else if (sideChannels == 1)
            {
                chunkSide[0] = chunkSide[1] = side.getReadPointer (0) + done;
            }
            else
            {
                // Stereo sidechain into a mono instance.
                const float* a = side.getReadPointer (0) + done;
                const float* b = side.getReadPointer (1) + done;
                auto& scratch = sidechainScratch[0];
                for (int i = 0; i < chunk; ++i)
                    scratch[(size_t) i] = 0.5f * (a[i] + b[i]);
                chunkSide[0] = scratch.data();
            }
        }

        engine.process (chunkMain, sideChannels > 0 ? chunkSide : nullptr, chunk);
        done += chunk;
    }

    for (int c = 0; c < channels; ++c)
        outputPeaks[(size_t) c].push (main.getReadPointer (c), total);
}

void ColgateProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, false);
}

void ColgateProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Stays latency matched, like the Bypass button.
    render (buffer, true);
}

bool ColgateProcessor::isShowingBypass() const noexcept
{
    return bypass->load (std::memory_order_relaxed) > 0.5f;
}

void ColgateProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, kNumFactoryPresets))
        return;

    currentPreset = index;
    const auto& preset = kFactoryPresets[index];

    const auto assign = [this] (const char* id, float value) {
        if (auto* parameter = state.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };
    assign (ids::recover, preset.recover);
    assign (ids::tame, preset.tame);
    assign (ids::bias, preset.bias);
    assign (ids::brighten, preset.brighten);
    assign (ids::boost, preset.boost);
    assign (ids::rangeLow, preset.rangeLow);
    assign (ids::rangeHigh, preset.rangeHigh);
    assign (ids::mode, (float) preset.mode);
}

const juce::String ColgateProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, kNumFactoryPresets) ? juce::String (kFactoryPresets[index].name) : juce::String();
}

void ColgateProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    auto copy = state.copyState();
    copy.setProperty ("preset", currentPreset, nullptr);
    if (const auto xml = copy.createXml())
        copyXmlToBinary (*xml, destination);
}

void ColgateProcessor::setStateInformation (const void* data, int size)
{
    if (const auto xml = getXmlFromBinary (data, size))
    {
        if (xml->hasTagName (state.state.getType()))
        {
            const auto tree = juce::ValueTree::fromXml (*xml);
            currentPreset = juce::jlimit (0, kNumFactoryPresets - 1, (int) tree.getProperty ("preset", 0));
            state.replaceState (tree);
        }
    }
}

juce::AudioProcessorEditor* ColgateProcessor::createEditor()
{
    return new ColgateEditor (*this);
}

} // namespace colgate

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new colgate::ColgateProcessor();
}
