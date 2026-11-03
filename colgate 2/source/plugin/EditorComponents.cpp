// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EditorComponents.h"
#include "SessionBypass.h"

namespace colgate
{

using namespace juce;

// ---------------------------------------------------------------------------
// ParameterControl

ParameterControl::ParameterControl (RangedAudioParameter& parameter, String unitText, int decimalPlaces, float dragPixelsForRange)
    : param (parameter),
      unit (std::move (unitText)),
      decimals (decimalPlaces),
      attachment (parameter, [this] (float v) {
          value = v;
          if (! approximatelyEqual (v, 0.0f))
              lastNonZero = v;
          repaint();
      }),
      dragPixels (dragPixelsForRange)
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);
    attachment.sendInitialUpdate();
}

ParameterControl::~ParameterControl() = default;

float ParameterControl::clampToRange (float v) const
{
    const auto& range = param.getNormalisableRange();
    return jlimit (range.start, range.end, v);
}

void ParameterControl::setFineMode (bool on)
{
    fine = on;
    repaint();
}

String ParameterControl::valueText() const
{
    const int shown = fine ? jmax (decimals, 1) : decimals;
    if (shown == 0)
    {
        const int rounded = roundToInt (value);
        return String (rounded);
    }
    return String (value, shown);
}

String ParameterControl::displayText() const
{
    return valueText().replaceCharacter ('-', (juce_wchar) 0x2212);
}

void ParameterControl::notifyHighlight()
{
    const bool now = isHighlighted();
    if (now != lastHighlight)
    {
        lastHighlight = now;
        if (onHighlightChanged)
            onHighlightChanged (now);
    }
    repaint();
}

void ParameterControl::mouseEnter (const MouseEvent&) { hovered = true; notifyHighlight(); }
void ParameterControl::mouseExit (const MouseEvent&) { hovered = false; notifyHighlight(); }
void ParameterControl::focusGained (FocusChangeType) { notifyHighlight(); }
void ParameterControl::focusLost (FocusChangeType) { notifyHighlight(); }

void ParameterControl::mouseDown (const MouseEvent& e)
{
    if (e.mods.isAltDown())
    {
        // Quick A/B of one parameter.
        attachment.setValueAsCompleteGesture (! approximatelyEqual (value, 0.0f) ? 0.0f : lastNonZero);
        return;
    }

    dragging = true;
    dragStartValue = value;
    dragOriginY = e.getScreenY();
    attachment.beginGesture();
    notifyHighlight();
}

void ParameterControl::mouseDrag (const MouseEvent& e)
{
    if (! dragging)
        return;

    const auto& range = param.getNormalisableRange();
    const float span = range.end - range.start;
    const bool slower = e.mods.isShiftDown() != fine;
    const float perPixel = span / dragPixels * (slower ? 0.125f : 1.0f);

    // Re-anchor, so pressing or releasing Shift mid-drag doesn't make it jump.
    const int moved = dragOriginY - e.getScreenY();
    dragOriginY = e.getScreenY();
    dragStartValue = clampToRange (dragStartValue + (float) moved * perPixel);
    attachment.setValueAsPartOfGesture (dragStartValue);
}

void ParameterControl::mouseUp (const MouseEvent&)
{
    if (dragging)
    {
        dragging = false;
        attachment.endGesture();
        notifyHighlight();
    }
}

void ParameterControl::mouseDoubleClick (const MouseEvent& e)
{
    if (! e.mods.isAltDown())
        beginTyping (valueText());
}

void ParameterControl::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& wheel)
{
    const auto& range = param.getNormalisableRange();
    const float span = range.end - range.start;
    const bool slower = e.mods.isShiftDown() != fine;
    const float step = span * wheel.deltaY * (slower ? 0.02f : 0.1f);
    attachment.setValueAsCompleteGesture (clampToRange (value + step));
}

bool ParameterControl::keyPressed (const KeyPress& key)
{
    const float smallest = param.getNormalisableRange().interval > 0.0f ? param.getNormalisableRange().interval : 0.1f;

    if (key.isKeyCode (KeyPress::upKey))
    {
        attachment.setValueAsCompleteGesture (clampToRange (value + smallest));
        return true;
    }
    if (key.isKeyCode (KeyPress::downKey))
    {
        attachment.setValueAsCompleteGesture (clampToRange (value - smallest));
        return true;
    }

    const auto character = key.getTextCharacter();
    if (CharacterFunctions::isDigit (character) || character == '-' || character == '.')
    {
        beginTyping (String::charToString (character));
        return true;
    }
    return false;
}

void ParameterControl::beginTyping (const String& initial)
{
    typing = std::make_unique<TextEditor>();
    typing->setFont (theme::strong (17.0f));
    typing->setJustification (Justification::centred);
    typing->setColour (TextEditor::backgroundColourId, theme::panelLift);
    typing->setColour (TextEditor::textColourId, theme::ink);
    typing->setColour (TextEditor::highlightColourId, theme::lavender.withAlpha (0.4f));
    typing->setColour (TextEditor::focusedOutlineColourId, theme::lavender);
    typing->setColour (TextEditor::outlineColourId, theme::rule);
    typing->setColour (CaretComponent::caretColourId, theme::ink);
    typing->setInputRestrictions (8, "0123456789.-");
    typing->setText (initial, false);
    typing->setBounds (editorBounds());
    typing->addListener (this);
    addAndMakeVisible (*typing);
    typing->grabKeyboardFocus();
    typing->moveCaretToEnd();
}

void ParameterControl::finishTyping (bool commit)
{
    if (typing == nullptr)
        return;

    if (commit)
    {
        const auto text = typing->getText().trim();
        if (text.isNotEmpty())
            attachment.setValueAsCompleteGesture (clampToRange (text.getFloatValue()));
    }

    // The editor may be mid-callback, so remove it once the stack has unwound.
    auto* doomed = typing.release();
    doomed->removeListener (this);
    doomed->setVisible (false);
    MessageManager::callAsync ([doomed] { delete doomed; });
    grabKeyboardFocus();
    repaint();
}

void ParameterControl::textEditorReturnKeyPressed (TextEditor&) { finishTyping (true); }
void ParameterControl::textEditorEscapeKeyPressed (TextEditor&) { finishTyping (false); }
void ParameterControl::textEditorFocusLost (TextEditor&) { finishTyping (false); }

// ---------------------------------------------------------------------------
// ValueReadout

ValueReadout::ValueReadout (RangedAudioParameter& parameter, String labelText, String unitText, int decimalPlaces,
                            float dragPixelsForRange, Colour activeColour, bool isPrimary)
    : ParameterControl (parameter, std::move (unitText), decimalPlaces, dragPixelsForRange),
      label (std::move (labelText)),
      active (activeColour),
      primary (isPrimary)
{
    setTitle (label);
}

Rectangle<int> ValueReadout::editorBounds() const
{
    return getLocalBounds().reduced (6, 0).withTrimmedTop (24).withTrimmedBottom (6);
}

void ValueReadout::paint (Graphics& g)
{
    const auto area = getLocalBounds().toFloat();

    if (isHighlighted())
    {
        g.setColour (theme::panelLift);
        g.fillRoundedRectangle (area, 5.0f);
    }

    const bool isZero = std::abs (currentValue()) < 0.05f;
    g.setFont (theme::text (13.0f));
    g.setColour (isHighlighted() ? theme::ink : theme::inkDim);
    g.drawText (label, area.withHeight (24.0f).withTrimmedTop (5.0f).withTrimmedLeft (10.0f), Justification::centredLeft);

    auto number = area.withTrimmedTop (24.0f).withTrimmedLeft (10.0f).withTrimmedBottom (5.0f);
    const auto text = displayText();
    const auto bigFont = theme::strong (primary ? 27.0f : 21.0f);
    const auto unitFont = theme::text (primary ? 14.0f : 13.0f);
    const float textWidth = GlyphArrangement::getStringWidth (bigFont, text);

    g.setFont (bigFont);
    g.setColour (isZero ? theme::inkDim : active);
    g.drawText (text, number, Justification::centredLeft);

    g.setFont (unitFont);
    g.setColour (theme::inkDim);
    g.drawText (unit, number.withTrimmedLeft (textWidth + 3.0f).withTrimmedTop (primary ? 7.0f : 5.0f), Justification::centredLeft);

    if (hasKeyboardFocus (false))
    {
        g.setColour (theme::lavender);
        g.drawRoundedRectangle (area.reduced (0.5f), 5.0f, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// GainFader

GainFader::GainFader (RangedAudioParameter& parameter) : ParameterControl (parameter, "dB", 1, 300.0f)
{
    setTitle ("Gain");
    setTooltip ("Output level. The processing already keeps loudness, so this is for matching peaks or a key element.");
}

Rectangle<int> GainFader::editorBounds() const
{
    return getLocalBounds().removeFromBottom (44).reduced (2, 12);
}

void GainFader::paint (Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    auto footer = area.removeFromBottom (44.0f);

    const auto track = area.reduced (0.0f, 8.0f);
    const float bar = 10.0f;
    const auto column = track.withX (track.getX() + 18.0f).withWidth (bar);

    g.setColour (theme::graph);
    g.fillRoundedRectangle (column, 3.0f);

    const auto yFor = [&track] (float db) { return jmap (db, -24.0f, 24.0f, track.getBottom(), track.getY()); };

    g.setFont (theme::text (11.5f));
    for (float db : { -24.0f, -12.0f, 0.0f, 12.0f, 24.0f })
    {
        const float y = yFor (db);
        g.setColour (theme::rule);
        g.drawHorizontalLine (roundToInt (y), column.getRight() + 4.0f, column.getRight() + 10.0f);
        g.setColour (theme::inkDim);
        g.drawText ((db > 0 ? String ("+") : String()) + String (roundToInt (db)).replaceCharacter ('-', (juce_wchar) 0x2212), Rectangle<float> (column.getRight() + 12.0f, y - 7.0f, 34.0f, 14.0f), Justification::centredLeft);
    }

    const float zeroY = yFor (0.0f);
    const float valueY = yFor (currentValue());
    g.setColour (currentValue() >= 0.0f ? theme::mint : theme::apricot);
    if (std::abs (currentValue()) > 0.04f)
        g.fillRoundedRectangle (column.withY (jmin (zeroY, valueY)).withHeight (jmax (1.0f, std::abs (valueY - zeroY))), 2.0f);

    g.setColour (theme::ink);
    g.fillRoundedRectangle (column.withY (valueY - 1.5f).withHeight (3.0f).expanded (3.0f, 0.0f), 1.5f);

    if (isHighlighted())
    {
        g.setColour (theme::panelLift);
        g.fillRoundedRectangle (footer.reduced (2.0f, 2.0f), 5.0f);
    }

    g.setColour (theme::inkDim);
    g.setFont (theme::text (12.5f));
    g.drawText ("Gain", footer.removeFromBottom (18.0f), Justification::centred);
    g.setColour (theme::ink);
    g.setFont (theme::strong (15.0f));
    g.drawText (displayText() + " dB", footer, Justification::centred);

    if (hasKeyboardFocus (false))
    {
        g.setColour (theme::lavender);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 5.0f, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// EqGraph

namespace
{
constexpr float kLeftAxis = 46.0f;
constexpr float kTopAxis = 8.0f;
constexpr float kBottomAxis = 22.0f;
constexpr float kLowestHz = 20.0f;
constexpr float kHighestHz = 20000.0f;
constexpr float kEdgeRevealPixels = 14.0f;
constexpr float kGrabPixels = 6.0f;
} // namespace

EqGraph::EqGraph (ColgateProcessor& processor, float defaultScaleDb) : owner (processor)
{
    for (size_t i = 0; i < scales.size(); ++i)
        if (std::abs (scales[i] - defaultScaleDb) < 0.01f)
            scaleIndex = (int) i;

    auto& state = owner.parameters();
    lowAttachment = std::make_unique<ParameterAttachment> (*state.getParameter (ids::rangeLow), [this] (float v) {
        rangeLow = v;
        repaint();
    });
    highAttachment = std::make_unique<ParameterAttachment> (*state.getParameter (ids::rangeHigh), [this] (float v) {
        rangeHigh = v;
        repaint();
    });
    lowAttachment->sendInitialUpdate();
    highAttachment->sendInitialUpdate();
}

EqGraph::~EqGraph() = default;

Rectangle<float> EqGraph::plotArea() const
{
    return getLocalBounds().toFloat().withTrimmedLeft (kLeftAxis).withTrimmedBottom (kBottomAxis).withTrimmedTop (kTopAxis);
}

void EqGraph::resized() {}

float EqGraph::xForHz (float hz) const
{
    const auto plot = plotArea();
    return plot.getX() + plot.getWidth() * std::log (hz / kLowestHz) / std::log (kHighestHz / kLowestHz);
}

float EqGraph::hzForX (float x) const
{
    const auto plot = plotArea();
    const float t = jlimit (0.0f, 1.0f, (x - plot.getX()) / plot.getWidth());
    return kLowestHz * std::pow (kHighestHz / kLowestHz, t);
}

float EqGraph::yForDb (float db) const
{
    const auto plot = plotArea();
    return plot.getCentreY() - plot.getHeight() * 0.5f * db / scaleDb();
}

float EqGraph::dbForY (float y) const
{
    const auto plot = plotArea();
    return (plot.getCentreY() - y) / (plot.getHeight() * 0.5f) * scaleDb();
}

float EqGraph::curveAt (float hz) const
{
    const float t = std::log (hz / kLowestHz) / std::log (kHighestHz / kLowestHz) * (float) (kDisplayPoints - 1);
    const int i = jlimit (0, kDisplayPoints - 2, (int) t);
    const float frac = t - (float) i;
    return curve[(size_t) i] + frac * (curve[(size_t) i + 1] - curve[(size_t) i]);
}

void EqGraph::refresh()
{
    const auto& meters = owner.engineMeters();
    bool changed = false;
    for (int i = 0; i < kDisplayPoints; ++i)
    {
        const float now = meters.curveDb[(size_t) i].load (std::memory_order_relaxed);
        changed = changed || std::abs (now - curve[(size_t) i]) > 0.005f;
        curve[(size_t) i] = now;
    }
    if (changed)
        repaint();
}

void EqGraph::setNotice (Notice n)
{
    if (n != notice)
    {
        notice = n;
        repaint();
    }
}

void EqGraph::paint (Graphics& g)
{
    const auto plot = plotArea();
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (theme::graph);
    g.fillRoundedRectangle (plot.expanded (1.0f), 4.0f);

    // Frequency grid.
    g.setFont (theme::text (11.5f));
    const struct { float hz; const char* label; } marks[] = {
        { 20, "20 Hz" }, { 50, "50" }, { 100, "100" }, { 200, "200" }, { 500, "500" }, { 1000, "1 kHz" },
        { 2000, "2k" }, { 5000, "5k" }, { 10000, "10k" }, { 20000, "20k" }
    };
    for (const auto& mark : marks)
    {
        const float x = xForHz (mark.hz);
        g.setColour (theme::rule.withAlpha (0.7f));
        g.drawVerticalLine (roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (theme::inkDim);
        const float labelX = jlimit (bounds.getX(), bounds.getRight() - 44.0f, x - 22.0f);
        g.drawText (mark.label, Rectangle<float> (labelX, plot.getBottom() + 4.0f, 44.0f, 16.0f),
                    mark.hz <= 20.0f ? Justification::centredLeft : (mark.hz >= 20000.0f ? Justification::centredRight : Justification::centred));
    }

    // Gain grid. The axis labels are the clickable, draggable scale control.
    const float scale = scaleDb();
    const int divisions = 4;
    for (int i = -divisions; i <= divisions; ++i)
    {
        const float db = scale * (float) i / (float) divisions;
        const float y = yForDb (db);
        g.setColour (i == 0 ? theme::ruleStrong : theme::rule.withAlpha (0.45f));
        g.drawHorizontalLine (roundToInt (y), plot.getX(), plot.getRight());
        if (i % 2 == 0)
        {
            g.setColour (theme::inkDim);
            const String number = String (std::abs (db), std::fmod (std::abs (db), 1.0f) > 0.01f ? 1 : 0);
            const String text = (db > 0.0f ? String ("+") : (db < 0.0f ? String::charToString ((juce_wchar) 0x2212) : String())) + number;
            g.drawText (text, Rectangle<float> (0.0f, y - 8.0f, kLeftAxis - 8.0f, 16.0f), Justification::centredRight);
        }
    }

    {
        Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (plot.toNearestInt());

        // Curve, as a fill either side of zero: mint where it lifts, apricot where
        // it cuts. This two-colour fill is the one thing to read from across the room.
        Path line;
        for (int i = 0; i < kDisplayPoints; ++i)
        {
            const float x = xForHz ((float) displayPointHz (i));
            const float y = yForDb (curve[(size_t) i]);
            if (i == 0)
                line.startNewSubPath (plot.getX(), y);
            else
                line.lineTo (x, y);
        }

        Path fill = line;
        const float zero = yForDb (0.0f);
        fill.lineTo (plot.getRight(), zero);
        fill.lineTo (plot.getX(), zero);
        fill.closeSubPath();

        {
            Graphics::ScopedSaveState above (g);
            g.reduceClipRegion (plot.withBottom (zero).toNearestInt());
            g.setGradientFill (ColourGradient (theme::mint.withAlpha (0.55f), 0.0f, plot.getY(), theme::mint.withAlpha (0.12f), 0.0f, zero, false));
            g.fillPath (fill);
        }
        {
            Graphics::ScopedSaveState below (g);
            g.reduceClipRegion (plot.withTop (zero).toNearestInt());
            g.setGradientFill (ColourGradient (theme::apricot.withAlpha (0.12f), 0.0f, zero, theme::apricot.withAlpha (0.55f), 0.0f, plot.getBottom(), false));
            g.fillPath (fill);
        }

        g.setColour (theme::ink);
        g.strokePath (line, PathStrokeType (1.8f, PathStrokeType::curved, PathStrokeType::rounded));

        // Limiters. What they leave out is fogged over.
        const float a = xForHz (jlimit (kLowestHz, kHighestHz, rangeLow));
        const float b = xForHz (jlimit (kLowestHz, kHighestHz, rangeHigh));
        const Colour fog = theme::frame.withAlpha (0.62f);
        g.setColour (fog);
        if (a <= b)
        {
            if (rangeLow > 20.5f) g.fillRect (plot.withRight (a));
            if (rangeHigh < 19500.0f) g.fillRect (plot.withLeft (b));
        }
        else
        {
            g.fillRect (plot.withLeft (b).withRight (a));
        }

        const auto drawLimiter = [&] (float x, float hz, bool lit, bool parked) {
            if (parked && ! lit && ! hovering)
                return;
            g.setColour (lit ? theme::ink : theme::lavender.withAlpha (parked ? 0.45f : 0.9f));
            g.fillRect (Rectangle<float> (x - 0.75f, plot.getY(), 1.5f, plot.getHeight()));
            const auto grip = Rectangle<float> (x - 4.0f, plot.getY() + 6.0f, 8.0f, 24.0f);
            g.fillRoundedRectangle (grip.withX (jlimit (plot.getX(), plot.getRight() - 8.0f, grip.getX())), 3.0f);
            if (lit && ! parked)
            {
                g.setFont (theme::strong (12.0f));
                const auto text = theme::formatHz (hz);
                const float width = GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 10.0f;
                auto tag = Rectangle<float> (x + 8.0f, plot.getY() + 8.0f, width, 18.0f);
                if (tag.getRight() > plot.getRight())
                    tag.setX (x - 8.0f - width);
                g.setColour (theme::panelLift);
                g.fillRoundedRectangle (tag, 3.0f);
                g.setColour (theme::ink);
                g.drawText (text, tag, Justification::centred);
            }
        };

        const bool nearLeftEdge = hovering && mouse.x < plot.getX() + kEdgeRevealPixels;
        const bool nearRightEdge = hovering && mouse.x > plot.getRight() - kEdgeRevealPixels;
        const bool lowParked = rangeLow <= 20.5f;
        const bool highParked = rangeHigh >= 19500.0f;
        if (! lowParked || nearLeftEdge || hoveredLimiter == firstLimiter || draggedLimiter == firstLimiter)
            drawLimiter (a, rangeLow, hoveredLimiter == firstLimiter || draggedLimiter == firstLimiter, lowParked);
        if (! highParked || nearRightEdge || hoveredLimiter == secondLimiter || draggedLimiter == secondLimiter)
            drawLimiter (b, rangeHigh, hoveredLimiter == secondLimiter || draggedLimiter == secondLimiter, highParked);

        if (notice != Notice::none)
        {
            g.setColour (theme::frame.withAlpha (0.55f));
            g.fillRect (plot);
            g.setFont (theme::strong (34.0f));
            g.setColour (notice == Notice::sessionBypassed ? theme::alarm : theme::inkDim);
            g.drawText (notice == Notice::sessionBypassed ? "Session bypassed" : "Bypassed", plot.withHeight (plot.getHeight() * 0.4f), Justification::centred);
        }

        if (probing)
        {
            const float hz = hzForX (mouse.x);
            const float db = curveAt (hz);
            const String top = theme::formatHz (hz);
            const String bottom = (db >= 0 ? String ("+") : String()) + String (db, 2).replaceCharacter ('-', (juce_wchar) 0x2212) + " dB";
            g.setFont (theme::strong (12.5f));
            const float width = jmax (GlyphArrangement::getStringWidth (g.getCurrentFont(), top),
                                      GlyphArrangement::getStringWidth (g.getCurrentFont(), bottom)) + 14.0f;
            auto tag = Rectangle<float> (mouse.x + 12.0f, mouse.y - 40.0f, width, 36.0f);
            if (tag.getRight() > plot.getRight()) tag.setX (mouse.x - 12.0f - width);
            if (tag.getY() < plot.getY()) tag.setY (mouse.y + 12.0f);
            g.setColour (theme::panelLift);
            g.fillRoundedRectangle (tag, 4.0f);
            g.setColour (theme::ink);
            g.drawText (top, tag.withHeight (18.0f).withTrimmedTop (2.0f), Justification::centred);
            g.drawText (bottom, tag.withTrimmedTop (17.0f), Justification::centred);
            g.setColour (theme::ink.withAlpha (0.35f));
            g.drawVerticalLine (roundToInt (mouse.x), plot.getY(), plot.getBottom());
        }
    }
}

int EqGraph::limiterAt (Point<float> position) const
{
    const auto plot = plotArea();
    if (! plot.expanded (2.0f, 0.0f).contains (position))
        return noLimiter;

    const float a = xForHz (jlimit (kLowestHz, kHighestHz, rangeLow));
    const float b = xForHz (jlimit (kLowestHz, kHighestHz, rangeHigh));
    const float da = std::abs (position.x - a);
    const float db = std::abs (position.x - b);

    if (da <= kGrabPixels && da <= db) return firstLimiter;
    if (db <= kGrabPixels) return secondLimiter;

    // Parked limiters can be grabbed anywhere along the edge they're revealed on.
    if (rangeLow <= 20.5f && position.x < plot.getX() + kEdgeRevealPixels) return firstLimiter;
    if (rangeHigh >= 19500.0f && position.x > plot.getRight() - kEdgeRevealPixels) return secondLimiter;
    return noLimiter;
}

void EqGraph::mouseEnter (const MouseEvent& e) { hovering = true; mouse = e.position; repaint(); }
void EqGraph::mouseExit (const MouseEvent&) { hovering = false; hoveredLimiter = noLimiter; repaint(); }

void EqGraph::mouseMove (const MouseEvent& e)
{
    mouse = e.position;
    const int now = limiterAt (e.position);
    if (now != hoveredLimiter)
        hoveredLimiter = now;
    setMouseCursor (now != noLimiter ? MouseCursor::LeftRightResizeCursor : (e.position.x < kLeftAxis ? MouseCursor::UpDownResizeCursor : MouseCursor::NormalCursor));
    repaint();
}

void EqGraph::stepScale (int direction)
{
    const int count = (int) scales.size();
    const int next = (scaleIndex + direction + count) % count;
    if (next != scaleIndex)
    {
        scaleIndex = next;
        if (onScaleChanged)
            onScaleChanged (scaleDb());
        repaint();
    }
}

void EqGraph::mouseDown (const MouseEvent& e)
{
    mouse = e.position;

    if (e.position.x < kLeftAxis)
    {
        draggingAxis = true;
        axisMoved = false;
        axisDragStart = e.position.y;
        axisScaleStart = scaleIndex;
        return;
    }

    draggedLimiter = limiterAt (e.position);
    if (draggedLimiter == firstLimiter)
        lowAttachment->beginGesture();
    else if (draggedLimiter == secondLimiter)
        highAttachment->beginGesture();
    else if (plotArea().contains (e.position))
        probing = true;
    repaint();
}

void EqGraph::mouseDrag (const MouseEvent& e)
{
    mouse = e.position;

    if (draggingAxis)
    {
        const int steps = roundToInt ((e.position.y - axisDragStart) / 26.0f);
        if (steps != 0)
            axisMoved = true;
        const int next = jlimit (0, (int) scales.size() - 1, axisScaleStart + steps);
        if (next != scaleIndex)
        {
            scaleIndex = next;
            if (onScaleChanged)
                onScaleChanged (scaleDb());
            repaint();
        }
        return;
    }

    const float hz = hzForX (e.position.x);
    if (draggedLimiter == firstLimiter)
        lowAttachment->setValueAsPartOfGesture (hz);
    else if (draggedLimiter == secondLimiter)
        highAttachment->setValueAsPartOfGesture (hz);
    repaint();
}

void EqGraph::mouseUp (const MouseEvent&)
{
    if (draggingAxis)
    {
        draggingAxis = false;
        if (! axisMoved)
            stepScale (1);
        return;
    }

    if (draggedLimiter == firstLimiter)
        lowAttachment->endGesture();
    else if (draggedLimiter == secondLimiter)
        highAttachment->endGesture();
    draggedLimiter = noLimiter;
    probing = false;
    repaint();
}

void EqGraph::mouseDoubleClick (const MouseEvent& e)
{
    // A double click sends a limiter back to the end of its travel.
    const int which = limiterAt (e.position);
    if (which == firstLimiter)
        lowAttachment->setValueAsCompleteGesture (20.0f);
    else if (which == secondLimiter)
        highAttachment->setValueAsCompleteGesture (20000.0f);
}

void EqGraph::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& wheel)
{
    if (approximatelyEqual (wheel.deltaY, 0.0f))
        return;
    const int next = jlimit (0, (int) scales.size() - 1, scaleIndex + (wheel.deltaY > 0.0f ? -1 : 1));
    if (next != scaleIndex)
    {
        scaleIndex = next;
        if (onScaleChanged)
            onScaleChanged (scaleDb());
        repaint();
    }
}

// ---------------------------------------------------------------------------
// ActionMeters

ActionMeters::ActionMeters (const EngineMeters& meterSource) : source (meterSource)
{
    setTooltip ("Left: whether Recover (up) or Tame (down) holds more of the range. Right: how much each is doing, on the graph's scale.");
}

void ActionMeters::refresh()
{
    const float r = source.recoverDb.load (std::memory_order_relaxed);
    const float t = source.tameDb.load (std::memory_order_relaxed);
    const float b = source.biasBalance.load (std::memory_order_relaxed);
    if (std::abs (r - recoverDb) > 0.01f || std::abs (t - tameDb) > 0.01f || std::abs (b - balance) > 0.005f)
    {
        recoverDb = r;
        tameDb = t;
        balance = b;
        repaint();
    }
}

void ActionMeters::setScaleDb (float db)
{
    scale = db;
    repaint();
}

void ActionMeters::setHighlights (bool recover, bool tame, bool bias)
{
    if (recover != lightRecover || tame != lightTame || bias != lightBias)
    {
        lightRecover = recover;
        lightTame = tame;
        lightBias = bias;
        repaint();
    }
}

void ActionMeters::paint (Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const float centre = area.getCentreY();
    const float half = area.getHeight() * 0.5f;
    const float barWidth = 7.0f;
    const float gap = 4.0f;

    const auto column = [&] (int index) {
        return Rectangle<float> (area.getX() + (float) index * (barWidth + gap), area.getY(), barWidth, area.getHeight());
    };

    for (int i = 0; i < 2; ++i)
    {
        g.setColour (theme::graph);
        g.fillRoundedRectangle (column (i), 2.0f);
    }

    // Bias: above the line, Recover holds more of the range. Below, Tame does.
    {
        const auto c = column (0);
        const float h = half * jlimit (-1.0f, 1.0f, balance);
        g.setColour (theme::lavender.withAlpha (lightBias ? 1.0f : 0.55f));
        const float top = h >= 0.0f ? centre - h : centre;
        g.fillRoundedRectangle (c.withY (top).withHeight (jmax (1.5f, std::abs (h))), 2.0f);
    }

    // Recover goes up and Tame goes down, sharing one column on the gain axis.
    {
        const auto c = column (1);
        const float up = half * jlimit (0.0f, 1.0f, recoverDb / scale);
        const float down = half * jlimit (0.0f, 1.0f, -tameDb / scale);
        g.setColour (theme::mint.withAlpha (lightRecover ? 1.0f : 0.7f));
        g.fillRoundedRectangle (c.withY (centre - up).withHeight (jmax (1.5f, up)), 2.0f);
        g.setColour (theme::apricot.withAlpha (lightTame ? 1.0f : 0.7f));
        g.fillRoundedRectangle (c.withY (centre).withHeight (jmax (1.5f, down)), 2.0f);
    }
}

// ---------------------------------------------------------------------------
// BrightnessMeter

BrightnessMeter::BrightnessMeter (const EngineMeters& meterSource) : source (meterSource)
{
    setTitle ("Brightness shift");
    setTooltip ("How far the output's perceived brightness has moved from the input's. Centred means equally bright.");
}

void BrightnessMeter::refresh()
{
    const float s = source.brightnessShift.load (std::memory_order_relaxed);
    if (std::abs (s - shift) > 0.003f)
    {
        shift = s;
        repaint();
    }
}

void BrightnessMeter::setHighlight (bool on)
{
    if (on != lit)
    {
        lit = on;
        repaint();
    }
}

void BrightnessMeter::paint (Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const auto bar = area.withSizeKeepingCentre (area.getWidth(), 4.0f);
    g.setColour (theme::graph);
    g.fillRoundedRectangle (bar, 2.0f);

    // Full travel at 4 ERB: a change no one could miss.
    const float t = jlimit (-1.0f, 1.0f, shift / 4.0f);
    const float width = area.getWidth() * 0.5f * std::abs (t);
    const float x = t >= 0.0f ? area.getCentreX() : area.getCentreX() - width;
    g.setColour (theme::lavender.withAlpha (lit ? 1.0f : 0.6f));
    g.fillRoundedRectangle (bar.withX (x).withWidth (jmax (1.5f, width)), 2.0f);

    g.setColour (theme::ruleStrong);
    g.fillRect (Rectangle<float> (area.getCentreX() - 0.5f, area.getY(), 1.0f, area.getHeight()));
}

// ---------------------------------------------------------------------------
// PeakStrip

PeakStrip::PeakStrip (ColgateProcessor& processor) : owner (processor)
{
    setTitle ("Input and output peak levels");
    setTooltip ("Input and output peak levels. Click to reset the peak hold.");
    setMouseCursor (MouseCursor::PointingHandCursor);
}

void PeakStrip::refresh()
{
    const auto settle = [] (float now, float& shown, float& hold) {
        const float db = now > 1.0e-6f ? 20.0f * std::log10 (now) : -120.0f;
        shown = db > shown ? db : jmax (db, shown - 1.5f);
        // Hold only what's worth holding: levels above -12 dBFS.
        if (db > -12.0f && db > hold)
            hold = db;
    };

    for (int c = 0; c < 2; ++c)
    {
        settle (owner.inputPeak (c).take(), inLevel[(size_t) c], inHold[(size_t) c]);
        settle (owner.outputPeak (c).take(), outLevel[(size_t) c], outHold[(size_t) c]);
    }
    repaint();
}

void PeakStrip::mouseDown (const MouseEvent&)
{
    inHold.fill (-120.0f);
    outHold.fill (-120.0f);
    repaint();
}

void PeakStrip::drawSide (Graphics& g, Rectangle<float> area, const float* levels, const float* holds, bool flip)
{
    constexpr float lowDb = -48.0f, highDb = 6.0f;
    const auto xFor = [&] (float db) {
        const float t = jlimit (0.0f, 1.0f, (db - lowDb) / (highDb - lowDb));
        return flip ? area.getRight() - t * area.getWidth() : area.getX() + t * area.getWidth();
    };

    const float rowHeight = 7.0f;
    const float top = area.getY();

    for (int c = 0; c < 2; ++c)
    {
        const auto row = Rectangle<float> (area.getX(), top + (float) c * (rowHeight + 3.0f), area.getWidth(), rowHeight);
        g.setColour (theme::graph);
        g.fillRoundedRectangle (row, 2.0f);

        const float level = levels[c];
        if (level > lowDb)
        {
            const float x = xFor (level);
            const auto bar = flip ? row.withLeft (x) : row.withRight (x);
            g.setColour (theme::lavender);
            g.fillRoundedRectangle (bar, 2.0f);

            if (level > 0.0f)
            {
                g.setColour (theme::alarm);
                const float zero = xFor (0.0f);
                g.fillRect (flip ? bar.withRight (zero) : bar.withLeft (zero));
            }
        }

        if (holds[c] > lowDb)
        {
            g.setColour (holds[c] > 0.0f ? theme::alarm : theme::ink);
            g.fillRect (Rectangle<float> (xFor (holds[c]) - 1.0f, row.getY(), 2.0f, rowHeight));
        }
    }

    const float scaleY = top + 2.0f * rowHeight + 3.0f + 3.0f;
    g.setFont (theme::text (11.0f));
    for (float db : { -48.0f, -36.0f, -24.0f, -12.0f, -6.0f, 0.0f, 6.0f })
    {
        const float x = xFor (db);
        g.setColour (db > 0.0f ? theme::alarm.withAlpha (0.8f) : theme::inkDim);
        const auto label = (db > 0.0f ? String ("+") : String()) + String (roundToInt (db)).replaceCharacter ('-', (juce_wchar) 0x2212);
        g.drawText (label, Rectangle<float> (x - 16.0f, scaleY, 32.0f, 14.0f),
                    approximatelyEqual (db, flip ? 6.0f : -48.0f) ? Justification::centredLeft
                                                                  : (approximatelyEqual (db, flip ? -48.0f : 6.0f) ? Justification::centredRight : Justification::centred));
    }
}

void PeakStrip::paint (Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (0.0f, 4.0f);
    const float labelWidth = 40.0f;
    const float centreGap = 44.0f;
    const float sideWidth = (area.getWidth() - 2.0f * labelWidth - centreGap) * 0.5f;

    g.setFont (theme::text (13.0f));
    g.setColour (theme::inkDim);
    g.drawText ("In", area.removeFromLeft (labelWidth).withHeight (20.0f), Justification::centredLeft);
    g.drawText ("Out", area.removeFromRight (labelWidth).withHeight (20.0f), Justification::centredRight);

    const auto leftSide = area.removeFromLeft (sideWidth);
    const auto rightSide = area.removeFromRight (sideWidth);

    drawSide (g, leftSide, inLevel.data(), inHold.data(), false);
    drawSide (g, rightSide, outLevel.data(), outHold.data(), true);

    g.setFont (theme::text (11.5f));
    g.setColour (theme::inkDim);
    g.drawText ("L", Rectangle<float> (area.getX(), area.getY() - 3.0f, area.getWidth(), 12.0f), Justification::centred);
    g.drawText ("R", Rectangle<float> (area.getX(), area.getY() + 7.0f, area.getWidth(), 12.0f), Justification::centred);
}

// ---------------------------------------------------------------------------
// BypassButton

BypassButton::BypassButton (ColgateProcessor& processor) : owner (processor)
{
    setTitle ("Bypass");
    setTooltip ("Compare with the unprocessed sound, delay matched. Shift-click to bypass every Colgate in this session.");
    setMouseCursor (MouseCursor::PointingHandCursor);
    attachment = std::make_unique<ParameterAttachment> (*owner.parameters().getParameter (ids::bypass), [this] (float v) {
        on = v > 0.5f;
        repaint();
    });
    attachment->sendInitialUpdate();
}

void BypassButton::refresh (int tick)
{
    const bool now = SessionBypass::isActive();
    const bool blinkNow = now && (tick % 2 == 0);
    if (now != session || blinkNow != blink)
    {
        session = now;
        blink = blinkNow;
        repaint();
    }
}

void BypassButton::mouseDown (const MouseEvent& e)
{
    if (SessionBypass::isActive())
    {
        // A second click restores every instance's own state.
        SessionBypass::set (false);
    }
    else if (e.mods.isShiftDown())
    {
        SessionBypass::set (true);
    }
    else
    {
        attachment->setValueAsCompleteGesture (on ? 0.0f : 1.0f);
    }
    repaint();
}

void BypassButton::paint (Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    Colour background = hovered ? theme::panelLift : theme::panel;
    Colour text = theme::inkDim;

    if (on)
    {
        background = theme::lavender;
        text = theme::frame;
    }
    if (session)
    {
        background = blink ? theme::alarm : theme::panel;
        text = blink ? theme::frame : theme::alarm;
    }

    g.setColour (background);
    g.fillRoundedRectangle (area, 5.0f);
    g.setColour (session ? theme::alarm : (on ? theme::lavender : theme::rule));
    g.drawRoundedRectangle (area, 5.0f, 1.0f);
    g.setColour (text);
    g.setFont (theme::strong (15.0f));
    g.drawText ("Bypass", area, Justification::centred);
}

// ---------------------------------------------------------------------------
// ModeSwitch

ModeSwitch::ModeSwitch (ColgateProcessor& processor)
{
    setTitle ("Mode");
    setTooltip ("Basic for tracks, buses and mixes. Master for a finished stereo master: control is finer and the EQ always updates at the Best rate, for the same latency.");
    setMouseCursor (MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);
    attachment = std::make_unique<ParameterAttachment> (*processor.parameters().getParameter (ids::mode), [this] (float v) {
        master = v > 0.5f;
        if (onChange)
            onChange (master);
        repaint();
    });
    attachment->sendInitialUpdate();
}

void ModeSwitch::mouseMove (const MouseEvent& e)
{
    const int side = e.x < getWidth() / 2 ? 0 : 1;
    if (side != hoveredSide)
    {
        hoveredSide = side;
        repaint();
    }
}

void ModeSwitch::mouseExit (const MouseEvent&)
{
    hoveredSide = -1;
    repaint();
}

void ModeSwitch::mouseDown (const MouseEvent& e)
{
    const bool wantMaster = e.x >= getWidth() / 2;
    if (wantMaster != master)
        attachment->setValueAsCompleteGesture (wantMaster ? 1.0f : 0.0f);
}

bool ModeSwitch::keyPressed (const KeyPress& key)
{
    if (key.isKeyCode (KeyPress::leftKey) || key.isKeyCode (KeyPress::rightKey) || key.isKeyCode (KeyPress::spaceKey))
    {
        attachment->setValueAsCompleteGesture (master ? 0.0f : 1.0f);
        return true;
    }
    return false;
}

void ModeSwitch::paint (Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::graph);
    g.fillRoundedRectangle (area, 4.0f);

    const auto left = area.withWidth (area.getWidth() * 0.44f);
    const auto right = area.withTrimmedLeft (left.getWidth());
    const auto drawSide = [&] (Rectangle<float> side, const char* text, bool selected, bool lit) {
        if (selected)
        {
            g.setColour (theme::lavender);
            g.fillRoundedRectangle (side.reduced (1.5f), 3.0f);
        }
        else if (lit)
        {
            g.setColour (theme::panelLift);
            g.fillRoundedRectangle (side.reduced (1.5f), 3.0f);
        }
        g.setColour (selected ? theme::frame : theme::inkDim);
        g.setFont (selected ? theme::strong (12.0f) : theme::text (12.0f));
        g.drawText (text, side, Justification::centred);
    };
    drawSide (left, "Basic", ! master, hoveredSide == 0);
    drawSide (right, "Master", master, hoveredSide == 1);

    g.setColour (hasKeyboardFocus (false) ? theme::lavender : theme::rule);
    g.drawRoundedRectangle (area, 4.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// SidechainButton

SidechainButton::SidechainButton (ColgateProcessor& processor)
{
    setTitle ("Sidechain");
    setTooltip ("Decide the EQ from the sidechain input instead of the main input. Feed every stem the full mix to master stems together.");
    setMouseCursor (MouseCursor::PointingHandCursor);
    attachment = std::make_unique<ParameterAttachment> (*processor.parameters().getParameter (ids::sidechain), [this] (float v) {
        on = v > 0.5f;
        repaint();
    });
    attachment->sendInitialUpdate();
}

void SidechainButton::mouseDown (const MouseEvent&)
{
    attachment->setValueAsCompleteGesture (on ? 0.0f : 1.0f);
}

void SidechainButton::paint (Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (on ? theme::lavender : (hovered ? theme::panelLift : theme::panel));
    g.fillRoundedRectangle (area, 3.0f);
    if (! on)
    {
        g.setColour (theme::ruleStrong);
        g.drawRoundedRectangle (area, 3.0f, 1.0f);
    }
    g.setColour (on ? theme::frame : theme::inkDim);
    g.setFont (theme::text (12.5f));
    g.drawText ("Sidechain", area, Justification::centred);
}

// ---------------------------------------------------------------------------
// PresetButton

PresetButton::PresetButton (ColgateProcessor& processor) : owner (processor)
{
    setTitle ("Presets");
    setMouseCursor (MouseCursor::PointingHandCursor);
}

void PresetButton::refresh()
{
    const int now = owner.getCurrentProgram();
    if (now != shown)
    {
        shown = now;
        repaint();
    }
}

void PresetButton::mouseDown (const MouseEvent&)
{
    PopupMenu menu;
    for (int i = 0; i < kNumFactoryPresets; ++i)
        menu.addItem (i + 1, kFactoryPresets[i].name, true, i == owner.getCurrentProgram());

    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (this),
                        [safe = SafePointer<PresetButton> (this)] (int chosen) {
                            if (safe != nullptr && chosen > 0)
                            {
                                safe->owner.setCurrentProgram (chosen - 1);
                                safe->refresh();
                            }
                        });
}

void PresetButton::paint (Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (hovered ? theme::panelLift : theme::panel);
    g.fillRoundedRectangle (area, 3.0f);
    g.setColour (theme::ruleStrong);
    g.drawRoundedRectangle (area, 3.0f, 1.0f);

    g.setColour (hovered ? theme::ink : theme::inkDim);
    g.setFont (theme::text (12.5f));
    g.drawText (owner.getProgramName (owner.getCurrentProgram()), area.withTrimmedLeft (8.0f).withTrimmedRight (18.0f), Justification::centredLeft);

    Path arrow;
    const float x = area.getRight() - 14.0f;
    arrow.addTriangle (x, area.getCentreY() - 2.0f, x + 8.0f, area.getCentreY() - 2.0f, x + 4.0f, area.getCentreY() + 3.0f);
    g.fillPath (arrow);
}

// ---------------------------------------------------------------------------
// SegmentRow

SegmentRow::SegmentRow (String labelText, StringArray choiceTexts, int selectedIndex)
    : label (std::move (labelText)), choices (std::move (choiceTexts)), selected (selectedIndex)
{
    setTitle (label);
}

void SegmentRow::setSelected (int index)
{
    if (index != selected)
    {
        selected = index;
        repaint();
    }
}

Rectangle<float> SegmentRow::segment (int index) const
{
    const float labelWidth = 130.0f;
    const auto area = getLocalBounds().toFloat().withTrimmedLeft (labelWidth);
    const float width = area.getWidth() / (float) choices.size();
    return { area.getX() + (float) index * width, area.getY(), width, area.getHeight() };
}

void SegmentRow::mouseDown (const MouseEvent& e)
{
    for (int i = 0; i < choices.size(); ++i)
    {
        if (segment (i).contains (e.position))
        {
            setSelected (i);
            if (onChange)
                onChange (i);
            return;
        }
    }
}

void SegmentRow::paint (Graphics& g)
{
    g.setFont (theme::text (13.5f));
    g.setColour (theme::inkDim);
    g.drawText (label, getLocalBounds().toFloat().withWidth (122.0f), Justification::centredRight);

    for (int i = 0; i < choices.size(); ++i)
    {
        const auto s = segment (i).reduced (1.0f, 0.0f);
        g.setColour (i == selected ? theme::lavender : theme::panelLift);
        g.fillRoundedRectangle (s, 3.0f);
        g.setColour (i == selected ? theme::frame : theme::ink);
        g.setFont (i == selected ? theme::strong (13.0f) : theme::text (13.0f));
        g.drawText (choices[i], s, Justification::centred);
    }
}

// ---------------------------------------------------------------------------
// InfoPage

InfoPage::InfoPage (ColgateProcessor& processor, UserSettings initial)
    : owner (processor),
      settings (initial),
      windowRow ("Window size", { "Tiny", "Small", "Normal", "Large", "Huge" }, initial.windowSizeIndex),
      rateRow ("Display rate", { "10 Hz", "15 Hz", "20 Hz", "30 Hz", "60 Hz" }, initial.displayRateIndex),
      scaleRow ("Default scale", { "3 dB", "6 dB", "9 dB", "12 dB", "18 dB", "24 dB" }, initial.defaultScaleIndex),
      realtimeRow ("Realtime audio", { "Normal", "Better", "Best" }, 0),
      offlineRow ("Offline audio", { "Normal", "Better", "Best" }, 1)
{
    for (auto* row : { &windowRow, &rateRow, &scaleRow, &realtimeRow, &offlineRow })
        addAndMakeVisible (*row);

    const auto changed = [this] {
        settings.save();
        if (onSettingsChanged)
            onSettingsChanged (settings);
    };

    windowRow.onChange = [this, changed] (int i) { settings.windowSizeIndex = i; changed(); };
    rateRow.onChange = [this, changed] (int i) { settings.displayRateIndex = i; changed(); };
    scaleRow.onChange = [this, changed] (int i) { settings.defaultScaleIndex = i; changed(); };

    auto& state = owner.parameters();
    realtimeAttachment = std::make_unique<ParameterAttachment> (*state.getParameter (ids::qualityRealtime), [this] (float v) {
        realtimeRow.setSelected (roundToInt (v));
    });
    offlineAttachment = std::make_unique<ParameterAttachment> (*state.getParameter (ids::qualityOffline), [this] (float v) {
        offlineRow.setSelected (roundToInt (v));
    });
    realtimeAttachment->sendInitialUpdate();
    offlineAttachment->sendInitialUpdate();
    realtimeRow.onChange = [this] (int i) { realtimeAttachment->setValueAsCompleteGesture ((float) i); };
    offlineRow.onChange = [this] (int i) { offlineAttachment->setValueAsCompleteGesture ((float) i); };
}

Rectangle<int> InfoPage::card() const
{
    return getLocalBounds().withSizeKeepingCentre (480, 312);
}

void InfoPage::resized()
{
    auto area = card().reduced (28, 0).withTrimmedTop (118);
    const int rowHeight = 25;
    const int gap = 7;
    windowRow.setBounds (area.removeFromTop (rowHeight));
    area.removeFromTop (gap);
    rateRow.setBounds (area.removeFromTop (rowHeight));
    area.removeFromTop (gap);
    scaleRow.setBounds (area.removeFromTop (rowHeight));
    area.removeFromTop (gap + 11);
    realtimeRow.setBounds (area.removeFromTop (rowHeight));
    area.removeFromTop (gap);
    offlineRow.setBounds (area.removeFromTop (rowHeight));
}

void InfoPage::mouseDown (const MouseEvent& e)
{
    if (! card().contains (e.getPosition()) && onClose)
        onClose();
}

void InfoPage::paint (Graphics& g)
{
    g.fillAll (theme::frame.withAlpha (0.72f));

    const auto box = card().toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (box, 8.0f);
    g.setColour (theme::rule);
    g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);

    g.setColour (theme::ink);
    g.setFont (theme::strong (40.0f));
    g.drawText ("colgate", box.withHeight (64.0f).withTrimmedTop (16.0f), Justification::centred);

    const auto mark = Rectangle<float> (box.getCentreX() - 90.0f, box.getY() + 76.0f, 180.0f, 9.0f);
    g.setColour (theme::mint);
    g.strokePath (theme::hiccupMark (mark), PathStrokeType (1.6f, PathStrokeType::curved, PathStrokeType::rounded));

    g.setColour (theme::inkDim);
    g.setFont (theme::text (13.5f));
    g.drawText (String ("Version ") + COLGATE_VERSION_STRING + " by Hiccup Audio", box.withY (box.getY() + 88.0f).withHeight (20.0f), Justification::centred);

    g.setFont (theme::text (12.0f));
    g.drawText ("Master mode always runs at Best.", box.withY (box.getBottom() - 28.0f).withHeight (18.0f), Justification::centred);
}

} // namespace colgate
