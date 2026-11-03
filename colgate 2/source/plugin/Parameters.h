// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace colgate::ids
{
inline constexpr const char* recover = "recover";
inline constexpr const char* tame = "tame";
inline constexpr const char* bias = "bias";
inline constexpr const char* brighten = "brighten";
inline constexpr const char* boost = "boost";
inline constexpr const char* gain = "gain";
inline constexpr const char* mode = "mode";
inline constexpr const char* bypass = "bypass";
inline constexpr const char* sidechain = "sidechain";
inline constexpr const char* rangeLow = "rangeLow";
inline constexpr const char* rangeHigh = "rangeHigh";
inline constexpr const char* qualityRealtime = "qualityRealtime";
inline constexpr const char* qualityOffline = "qualityOffline";
} // namespace colgate::ids

namespace colgate
{

struct FactoryPreset
{
    const char* name;
    float recover, tame, bias, brighten, boost;
    float rangeLow = 20.0f, rangeHigh = 20000.0f;
    int mode = 0; // 0 Basic, 1 Master
};

// Starting points, drawn from how people describe using this kind of processor.
inline constexpr FactoryPreset kFactoryPresets[] = {
    { "Clean slate", 0, 0, 0, 0, 0 },
    { "Bus polish", 15, 15, 0, 0, 0 },
    { "Mix bus", 30, 30, 0, 0, 0 },
    { "Master, gentle", 10, 10, 0, 0, 0, 20.0f, 20000.0f, 1 },
    { "Master, open", 60, 60, 0, 0, 0, 20.0f, 20000.0f, 1 },
    { "Lead vocal touch", 8, 25, 0, 0, 0, 200.0f, 5000.0f },
    { "Rough vocal rescue", 70, 80, 0, -10, 0, 120.0f, 9000.0f },
    { "Guitar bus", 20, 25, 0, 0, 0, 90.0f, 10000.0f },
    { "Reverb return", 120, 120, 0, 0, 0 },
    { "Push to the back", 0, 0, 0, 0, -8 },
    { "Bring to the front", 0, 0, 0, 0, 5 },
};

inline constexpr int kNumFactoryPresets = (int) (sizeof (kFactoryPresets) / sizeof (kFactoryPresets[0]));

} // namespace colgate
