// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Preferences that belong to the user, not the session: window size, how often
// the graph redraws, and its default gain scale. Quality lives in the session,
// as ordinary plug-in parameters.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace colgate
{

struct UserSettings
{
    int windowSizeIndex = 2;    // Tiny, Small, Normal, Large, Huge
    int displayRateIndex = 3;   // 10, 15, 20, 30, 60 Hz
    int defaultScaleIndex = 3;  // 3, 6, 9, 12, 18, 24 dB

    static constexpr float windowScales[5] = { 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
    static constexpr int displayRates[5] = { 10, 15, 20, 30, 60 };
    static constexpr float scalesDb[6] = { 3.0f, 6.0f, 9.0f, 12.0f, 18.0f, 24.0f };

    float windowScale() const noexcept { return windowScales[windowSizeIndex]; }
    int displayRate() const noexcept { return displayRates[displayRateIndex]; }
    float defaultScaleDb() const noexcept { return scalesDb[defaultScaleIndex]; }

    static UserSettings load();
    void save() const;
};

} // namespace colgate
