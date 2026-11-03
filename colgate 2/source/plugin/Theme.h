// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The look. Plum, because a mixing room is dim and this should be easy to sit
// next to. Mint means "going up", apricot means "going down", and those two
// colours are used for nothing else, so the graph can be read at a glance.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BinaryData.h"

namespace colgate::theme
{

inline const juce::Colour frame { 0xff1d1426 };
inline const juce::Colour panel { 0xff2a1f36 };
inline const juce::Colour panelLift { 0xff3a2c49 };
inline const juce::Colour graph { 0xff231930 };
inline const juce::Colour rule { 0xff463757 };
inline const juce::Colour ruleStrong { 0xff6a5880 };
inline const juce::Colour ink { 0xfff4eef6 };
inline const juce::Colour inkDim { 0xffae9dbb };
inline const juce::Colour lavender { 0xffbca8dc };
inline const juce::Colour mint { 0xff7de8c5 };
inline const juce::Colour apricot { 0xffffb27a };
inline const juce::Colour alarm { 0xffff6470 };

inline juce::Typeface::Ptr regularFace()
{
    static auto face = juce::Typeface::createSystemTypefaceFor (ColgateData::ColgateFaceRegular_ttf, ColgateData::ColgateFaceRegular_ttfSize);
    return face;
}

inline juce::Typeface::Ptr boldFace()
{
    static auto face = juce::Typeface::createSystemTypefaceFor (ColgateData::ColgateFaceBold_ttf, ColgateData::ColgateFaceBold_ttfSize);
    return face;
}

inline juce::Font text (float height) { return juce::Font (juce::FontOptions (regularFace()).withHeight (height)); }
inline juce::Font strong (float height) { return juce::Font (juce::FontOptions (boldFace()).withHeight (height)); }

// A flat line with one blip in it: the hiccup.
inline juce::Path hiccupMark (juce::Rectangle<float> area)
{
    juce::Path p;
    const float y = area.getBottom();
    const float h = area.getHeight();
    const float x = area.getX();
    const float w = area.getWidth();
    p.startNewSubPath (x, y);
    p.lineTo (x + w * 0.70f, y);
    p.quadraticTo (x + w * 0.735f, y, x + w * 0.75f, y - h * 0.55f);
    p.quadraticTo (x + w * 0.765f, y - h * 1.15f, x + w * 0.785f, y - h * 0.55f);
    p.quadraticTo (x + w * 0.80f, y + h * 0.28f, x + w * 0.82f, y);
    p.lineTo (x + w, y);
    return p;
}

inline juce::String formatHz (float hz)
{
    return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz"
                         : juce::String (juce::roundToInt (hz)) + " Hz";
}

} // namespace colgate::theme
