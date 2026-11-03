// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "EditorComponents.h"

namespace colgate
{

class ColgateEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ColgateEditor (ColgateProcessor&);
    ~ColgateEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kBaseWidth = 900;
    static constexpr int kBaseHeight = 540;

    // Pushes the meters and graph forward once. For rendering the UI in tests.
    void refreshForSnapshot();

private:
    class Content;

    void timerCallback() override;

    std::unique_ptr<Content> content;
    UserSettings settings;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ColgateEditor)
};

} // namespace colgate
