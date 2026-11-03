// Colgate by Hiccup Audio
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginEditor.h"
#include "SessionBypass.h"

namespace colgate
{

using namespace juce;

// Everything sits in one component laid out at the base size. Window size is
// then a single scale transform on it, so nothing has to know about it.
class ColgateEditor::Content final : public Component
{
public:
    Content (ColgateProcessor& p, const UserSettings& initialSettings)
        : processor (p),
          recover (param (ids::recover), "Recover", "%", 0, 500.0f, theme::mint, true),
          tame (param (ids::tame), "Tame", "%", 0, 500.0f, theme::apricot, true),
          bias (param (ids::bias), "Bias", "%", 0, 500.0f, theme::ink, false),
          brighten (param (ids::brighten), "Brighten", "%", 0, 500.0f, theme::ink, false),
          boost (param (ids::boost), "Boost", "dB", 1, 700.0f, theme::ink, false),
          gain (param (ids::gain)),
          graph (p, initialSettings.defaultScaleDb()),
          actionMeters (p.engineMeters()),
          brightnessMeter (p.engineMeters()),
          peaks (p),
          bypassButton (p),
          modeSwitch (p),
          sidechainButton (p),
          presets (p),
          currentSettings (initialSettings)
    {
        for (Component* c : std::initializer_list<Component*> { &recover, &tame, &bias, &brighten, &boost, &gain, &graph, &actionMeters, &brightnessMeter, &peaks, &bypassButton, &modeSwitch, &sidechainButton, &presets })
            addAndMakeVisible (c);

        // Meters light up when their control is highlighted.
        const auto update = [this] (bool) {
            actionMeters.setHighlights (recover.isHighlighted(), tame.isHighlighted(), bias.isHighlighted());
            brightnessMeter.setHighlight (brighten.isHighlighted());
        };
        for (auto* control : { &recover, &tame, &bias, &brighten })
            control->onHighlightChanged = update;

        recover.setTooltip ("Lifts what is covered up by other sounds.");
        tame.setTooltip ("Pulls down what is covering up other sounds.");
        bias.setTooltip ("Divides the range: up gives Recover more of it, down gives Tame more of it.");
        brighten.setTooltip ("Uncover by favouring higher frequencies (up) or lower ones (down). Needs Recover or Tame.");
        boost.setTooltip ("The tonal balance you'd hear if the volume changed by this many dB, at the same loudness. Lower it to push a sound back.");
        tooltips.setMillisecondsBeforeTipAppears (900);

        const auto applyMode = [this] (bool isMaster) {
            for (auto* control : { &recover, &tame, &bias, &brighten, &boost })
                control->setFineMode (isMaster);
        };
        modeSwitch.onChange = applyMode;
        applyMode (modeSwitch.isMaster());

        actionMeters.setScaleDb (graph.scaleDb());
        graph.onScaleChanged = [this] (float db) { actionMeters.setScaleDb (db); };
    }

    void refresh()
    {
        graph.refresh();
        actionMeters.refresh();
        brightnessMeter.refresh();
        peaks.refresh();
        presets.refresh();

        ++tick;
        bypassButton.refresh (tick / blinkDivider);

        if (SessionBypass::isActive())
            graph.setNotice (EqGraph::Notice::sessionBypassed);
        else if (processor.isShowingBypass())
            graph.setNotice (EqGraph::Notice::bypassed);
        else
            graph.setNotice (EqGraph::Notice::none);
    }

    void setBlinkDivider (int divider) { blinkDivider = jmax (1, divider); }

    std::function<void (const UserSettings&)> onSettingsChanged;

    void paint (Graphics& g) override
    {
        g.fillAll (theme::frame);

        const auto bounds = getLocalBounds().toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (bounds.withBottom (kHeaderBottom).reduced (8.0f, 8.0f).withTrimmedBottom (-2.0f), 7.0f);
        g.fillRoundedRectangle (bounds.withTop (kHeaderBottom + 6.0f).withBottom (kStripTop - 6.0f).reduced (8.0f, 0.0f), 7.0f);
        g.fillRoundedRectangle (bounds.withTop (kStripTop).reduced (8.0f, 0.0f).withTrimmedBottom (8.0f), 7.0f);

        const auto name = nameArea().toFloat();
        g.setColour (theme::ink);
        g.setFont (theme::strong (31.0f));
        g.drawText ("colgate", name.withHeight (38.0f), Justification::centredLeft);

        const auto mark = Rectangle<float> (name.getX(), name.getY() + 39.0f, 108.0f, 6.0f);
        g.setColour (theme::mint);
        g.strokePath (theme::hiccupMark (mark), PathStrokeType (1.4f, PathStrokeType::curved, PathStrokeType::rounded));

        g.setColour (theme::inkDim);
        g.setFont (theme::text (12.5f));
        g.drawText ("by hiccup audio", name.withY (name.getY() + 46.0f).withHeight (16.0f), Justification::centredLeft);

        // Thin rules between the three groups of controls say which belong together.
        g.setColour (theme::rule);
        const int top = 22, bottom = kHeaderBottom - 16;
        g.drawVerticalLine (kGroupRuleOne, (float) top, (float) bottom);
        g.drawVerticalLine (kGroupRuleTwo, (float) top, (float) bottom);
    }

    void resized() override
    {
        const int readoutTop = 15;
        const int readoutHeight = kHeaderBottom - 28;
        recover.setBounds (kReadoutLeft, readoutTop, 112, readoutHeight);
        tame.setBounds (recover.getRight() + 2, readoutTop, 112, readoutHeight);
        bias.setBounds (kGroupRuleOne + 9, readoutTop, 88, readoutHeight);
        brighten.setBounds (bias.getRight() + 2, readoutTop, 88, readoutHeight);
        boost.setBounds (kGroupRuleTwo + 9, readoutTop, 104, readoutHeight);

        bypassButton.setBounds (kRightColumn - 8, 16, kRightColumnWidth + 8, 32);
        modeSwitch.setBounds (kRightColumn - 8, 54, kRightColumnWidth + 8, 24);

        const int mainTop = kHeaderBottom + 16;
        const int mainBottom = kStripTop - 16;

        graph.setBounds (54, mainTop, kRightColumn - 54 - 14, mainBottom - mainTop - 16);
        const auto plot = graph.plotArea();

        actionMeters.setBounds (22, graph.getY() + roundToInt (plot.getY()), 18, roundToInt (plot.getHeight()));

        brightnessMeter.setBounds (graph.getX() + roundToInt (plot.getX()), graph.getBottom() + 1, roundToInt (plot.getWidth()), 12);
        sidechainButton.setBounds (graph.getX() + roundToInt (plot.getX()) + 8, graph.getY() + roundToInt (plot.getBottom()) - 30, 74, 22);
        presets.setBounds (graph.getX() + roundToInt (plot.getRight()) - 8 - 148, graph.getY() + roundToInt (plot.getBottom()) - 30, 148, 22);

        gain.setBounds (kRightColumn, mainTop - 4, kRightColumnWidth, mainBottom - mainTop + 8);

        peaks.setBounds (24, kStripTop + 8, getWidth() - 48, 50);
    }

    void mouseMove (const MouseEvent& e) override
    {
        setMouseCursor (nameArea().contains (e.getPosition()) ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (nameArea().contains (e.getPosition()))
            showInfoPage();
    }

    void showInfoPage()
    {
        if (info != nullptr)
            return;
        info = std::make_unique<InfoPage> (processor, currentSettings);
        info->onClose = [this] {
            // Closing happens inside the page's own mouse handler.
            MessageManager::callAsync ([safe = SafePointer<Content> (this)] {
                if (safe != nullptr)
                    safe->info.reset();
            });
        };
        info->onSettingsChanged = [this] (const UserSettings& s) {
            currentSettings = s;
            if (onSettingsChanged)
                onSettingsChanged (s);
        };
        addAndMakeVisible (*info);
        info->setBounds (getLocalBounds());
    }

    void hideInfoPage() { info.reset(); }

private:
    static constexpr int kHeaderBottom = 92;
    static constexpr int kStripTop = 464;
    static constexpr int kReadoutLeft = 190;
    static constexpr int kGroupRuleOne = 424;
    static constexpr int kGroupRuleTwo = 619;
    static constexpr int kRightColumn = 790;
    static constexpr int kRightColumnWidth = 88;

    Rectangle<int> nameArea() const { return { 24, 14, 150, 62 }; }

    RangedAudioParameter& param (const char* id) { return *processor.parameters().getParameter (id); }

    ColgateProcessor& processor;
    ValueReadout recover, tame, bias, brighten, boost;
    GainFader gain;
    EqGraph graph;
    ActionMeters actionMeters;
    BrightnessMeter brightnessMeter;
    PeakStrip peaks;
    BypassButton bypassButton;
    ModeSwitch modeSwitch;
    SidechainButton sidechainButton;
    PresetButton presets;
    TooltipWindow tooltips { this };
    std::unique_ptr<InfoPage> info;
    UserSettings currentSettings;
    int tick = 0;
    int blinkDivider = 8;
};

ColgateEditor::ColgateEditor (ColgateProcessor& p)
    : AudioProcessorEditor (p), settings (UserSettings::load())
{
    setOpaque (true);
    content = std::make_unique<Content> (p, settings);
    addAndMakeVisible (*content);
    content->setBounds (0, 0, kBaseWidth, kBaseHeight);

    content->onSettingsChanged = [this] (const UserSettings& s) {
        settings = s;
        const float scale = settings.windowScale();
        content->setTransform (AffineTransform::scale (scale));
        setSize (roundToInt ((float) kBaseWidth * scale), roundToInt ((float) kBaseHeight * scale));
        content->setBlinkDivider (settings.displayRate() / 4);
        startTimerHz (settings.displayRate());
    };

    const float scale = settings.windowScale();
    content->setTransform (AffineTransform::scale (scale));
    setSize (roundToInt ((float) kBaseWidth * scale), roundToInt ((float) kBaseHeight * scale));
    content->setBlinkDivider (settings.displayRate() / 4);
    startTimerHz (settings.displayRate());
}

ColgateEditor::~ColgateEditor()
{
    stopTimer();
}

void ColgateEditor::paint (Graphics& g)
{
    g.fillAll (theme::frame);
}

void ColgateEditor::resized() {}

void ColgateEditor::timerCallback()
{
    content->refresh();
}

void ColgateEditor::refreshForSnapshot()
{
    content->refresh();
}

} // namespace colgate
