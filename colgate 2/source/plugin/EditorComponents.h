// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "PluginProcessor.h"
#include "Settings.h"
#include "Theme.h"

#include <functional>

namespace colgate
{

// Shared behaviour of everything that edits a number: vertical drag, Shift for
// fine moves, Alt-click to flip between zero and the last value, arrow keys on
// the highlighted control, the scroll wheel, and typing a value with Enter.
class ParameterControl : public juce::Component, public juce::SettableTooltipClient, private juce::TextEditor::Listener
{
public:
    ParameterControl (juce::RangedAudioParameter& parameter, juce::String unitText, int decimalPlaces, float dragPixelsForRange);
    ~ParameterControl() override;

    bool isHighlighted() const noexcept { return hovered || dragging || hasKeyboardFocus (true); }

    // Master's finer control: a decimal more on screen, and a slower drag and wheel
    // unless Shift is down.
    void setFineMode (bool on);
    float currentValue() const noexcept { return value; }
    juce::String valueText() const;   // ready to edit
    juce::String displayText() const; // with a real minus sign

    std::function<void (bool)> onHighlightChanged;

    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override;
    void focusLost (FocusChangeType) override;

protected:
    virtual juce::Rectangle<int> editorBounds() const { return getLocalBounds(); }

    juce::RangedAudioParameter& param;
    juce::String unit;
    int decimals;
    bool fine = false;
    bool hovered = false, dragging = false;

private:
    void textEditorReturnKeyPressed (juce::TextEditor&) override;
    void textEditorEscapeKeyPressed (juce::TextEditor&) override;
    void textEditorFocusLost (juce::TextEditor&) override;
    void beginTyping (const juce::String& initial);
    void finishTyping (bool commit);
    void notifyHighlight();
    float clampToRange (float v) const;

    juce::ParameterAttachment attachment;
    float value = 0.0f;
    float lastNonZero = 0.0f;
    float dragStartValue = 0.0f;
    float dragPixels;
    int dragOriginY = 0;
    bool lastHighlight = false;
    std::unique_ptr<juce::TextEditor> typing;
};

// A label and a large number at the top of the window.
class ValueReadout final : public ParameterControl
{
public:
    ValueReadout (juce::RangedAudioParameter& parameter, juce::String labelText, juce::String unitText, int decimalPlaces,
                  float dragPixelsForRange, juce::Colour activeColour, bool isPrimary);
    void paint (juce::Graphics&) override;

private:
    juce::Rectangle<int> editorBounds() const override;
    juce::String label;
    juce::Colour active;
    bool primary;
};

class GainFader final : public ParameterControl
{
public:
    explicit GainFader (juce::RangedAudioParameter& parameter);
    void paint (juce::Graphics&) override;

private:
    juce::Rectangle<int> editorBounds() const override;
};

class EqGraph final : public juce::Component
{
public:
    EqGraph (ColgateProcessor& processor, float defaultScaleDb);
    ~EqGraph() override;

    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    float scaleDb() const noexcept { return scales[(size_t) scaleIndex]; }
    std::function<void (float)> onScaleChanged;

    enum class Notice
    {
        none,
        bypassed,
        sessionBypassed
    };
    void setNotice (Notice n);

    juce::Rectangle<float> plotArea() const;

private:
    float xForHz (float hz) const;
    float hzForX (float x) const;
    float yForDb (float db) const;
    float dbForY (float y) const;
    float curveAt (float hz) const;
    int limiterAt (juce::Point<float> position) const;
    void stepScale (int direction);

    enum Limiter
    {
        noLimiter = -1,
        firstLimiter = 0,
        secondLimiter = 1
    };

    ColgateProcessor& owner;
    std::array<float, kDisplayPoints> curve {};
    static constexpr std::array<float, 6> scales { 3.0f, 6.0f, 9.0f, 12.0f, 18.0f, 24.0f };
    int scaleIndex = 3;
    Notice notice = Notice::none;

    std::unique_ptr<juce::ParameterAttachment> lowAttachment, highAttachment;
    float rangeLow = 20.0f, rangeHigh = 20000.0f;

    bool hovering = false;
    juce::Point<float> mouse;
    int hoveredLimiter = noLimiter, draggedLimiter = noLimiter;
    bool probing = false;
    float axisDragStart = 0.0f;
    int axisScaleStart = 0;
    bool draggingAxis = false, axisMoved = false;
};

// The Bias, Recover and Tame meters, left of the graph on its gain axis.
class ActionMeters final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ActionMeters (const EngineMeters& meterSource);
    void refresh();
    void setScaleDb (float db);
    void setHighlights (bool recover, bool tame, bool bias);
    void paint (juce::Graphics&) override;

private:
    const EngineMeters& source;
    float recoverDb = 0, tameDb = 0, balance = 0, scale = 12.0f;
    bool lightRecover = false, lightTame = false, lightBias = false;
};

// How far the output's perceived brightness has moved from the input's.
class BrightnessMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit BrightnessMeter (const EngineMeters& meterSource);
    void refresh();
    void setHighlight (bool on);
    void paint (juce::Graphics&) override;

private:
    const EngineMeters& source;
    float shift = 0.0f;
    bool lit = false;
};

class PeakStrip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit PeakStrip (ColgateProcessor& processor);
    void refresh();
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void drawSide (juce::Graphics&, juce::Rectangle<float> area, const float* levels, const float* holds, bool flip);
    ColgateProcessor& owner;
    std::array<float, 2> inLevel {}, outLevel {}, inHold {}, outHold {};
};

class BypassButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit BypassButton (ColgateProcessor& processor);
    void refresh (int tick);
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override;

private:
    ColgateProcessor& owner;
    bool hovered = false, on = false, session = false, blink = false;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

// Basic or Master, under Bypass.
class ModeSwitch final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ModeSwitch (ColgateProcessor& processor);
    std::function<void (bool isMaster)> onChange;
    bool isMaster() const noexcept { return master; }
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    bool master = false;
    int hoveredSide = -1;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

class SidechainButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit SidechainButton (ColgateProcessor& processor);
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override;

private:
    bool hovered = false, on = false;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

class PresetButton final : public juce::Component
{
public:
    explicit PresetButton (ColgateProcessor& processor);
    void refresh();
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override;

private:
    ColgateProcessor& owner;
    int shown = -1;
    bool hovered = false;
};

// A row of mutually exclusive choices.
class SegmentRow final : public juce::Component
{
public:
    SegmentRow (juce::String labelText, juce::StringArray choiceTexts, int selectedIndex);
    std::function<void (int)> onChange;
    void setSelected (int index);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> segment (int index) const;
    juce::String label;
    juce::StringArray choices;
    int selected;
};

// Opened by clicking the name: version, window and display settings, quality.
class InfoPage final : public juce::Component
{
public:
    InfoPage (ColgateProcessor& processor, UserSettings initial);
    std::function<void (const UserSettings&)> onSettingsChanged;
    std::function<void()> onClose;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> card() const;
    ColgateProcessor& owner;
    UserSettings settings;
    SegmentRow windowRow, rateRow, scaleRow, realtimeRow, offlineRow;
    std::unique_ptr<juce::ParameterAttachment> realtimeAttachment, offlineAttachment;
};

} // namespace colgate
