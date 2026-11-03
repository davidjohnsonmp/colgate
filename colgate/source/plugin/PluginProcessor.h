// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ColgateEngine.h"
#include "Parameters.h"

#include <array>
#include <atomic>

namespace colgate
{

// One channel's peak since the UI last looked.
class PeakMeter
{
public:
    void push (const float* samples, int count) noexcept
    {
        float peak = 0.0f;
        for (int i = 0; i < count; ++i)
            peak = std::max (peak, std::abs (samples[i]));
        float seen = level.load (std::memory_order_relaxed);
        while (peak > seen && ! level.compare_exchange_weak (seen, peak, std::memory_order_relaxed))
        {
        }
    }

    float take() noexcept { return level.exchange (0.0f, std::memory_order_relaxed); }

private:
    std::atomic<float> level { 0.0f };
};

class ColgateProcessor final : public juce::AudioProcessor
{
public:
    ColgateProcessor();
    ~ColgateProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Colgate"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return kNumFactoryPresets; }
    int getCurrentProgram() override { return currentPreset; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destination) override;
    void setStateInformation (const void* data, int size) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParameter; }

    juce::AudioProcessorValueTreeState& parameters() noexcept { return state; }
    const EngineMeters& engineMeters() const noexcept { return engine.meters(); }
    PeakMeter& inputPeak (int channel) noexcept { return inputPeaks[(size_t) channel]; }
    PeakMeter& outputPeak (int channel) noexcept { return outputPeaks[(size_t) channel]; }
    int activeChannels() const noexcept { return numChannels.load (std::memory_order_relaxed); }
    bool isShowingBypass() const noexcept;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void render (juce::AudioBuffer<float>&, bool forceBypass) noexcept;
    EngineParameters currentParameters (bool forceBypass) const noexcept;
    Quality wantedQuality() const noexcept;

    juce::AudioProcessorValueTreeState state;
    ColgateEngine engine;

    std::atomic<float>* recover = nullptr;
    std::atomic<float>* tame = nullptr;
    std::atomic<float>* bias = nullptr;
    std::atomic<float>* brighten = nullptr;
    std::atomic<float>* boost = nullptr;
    std::atomic<float>* gain = nullptr;
    std::atomic<float>* mode = nullptr;
    std::atomic<float>* rangeLow = nullptr;
    std::atomic<float>* rangeHigh = nullptr;
    std::atomic<float>* sidechain = nullptr;
    std::atomic<float>* bypass = nullptr;
    std::atomic<float>* qualityRealtime = nullptr;
    std::atomic<float>* qualityOffline = nullptr;
    juce::AudioParameterBool* bypassParameter = nullptr;

    std::array<PeakMeter, kMaxChannels> inputPeaks, outputPeaks;
    std::atomic<int> numChannels { 2 };
    bool wasOffline = false;
    int currentPreset = 0;

    static constexpr int kScratchSamples = 2048;
    std::array<std::array<float, kScratchSamples>, kMaxChannels> sidechainScratch {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ColgateProcessor)
};

} // namespace colgate
