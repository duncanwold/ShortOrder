#pragma once

// v0.6: the faceplate's shared parts (docs/history/SPEC-v0.6.md section 1): one look and feel (Didact Gothic everywhere, the key dropdown, the
// popup menu, the sustain knob's value bubble), one button class for every segmented row and grid (RackButton), a row of them bound to
// a choice parameter (ChoiceRow), and the small knob (SmallKnob, Sustain). Message thread only.

#include <juce_audio_processors/juce_audio_processors.h>
#include "Theme.h"

class RackLookAndFeel : public juce::LookAndFeel_V4
{
public:
    RackLookAndFeel();

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    // The key dropdown: 28 px, the control border, the panel fill, radius 3, a small chevron.
    void drawComboBox (juce::Graphics&, int width, int height, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    // The sustain knob (SmallKnob): face, rim, a 3 px pointer; and its value bubble while dragging.
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float pos, float start, float end, juce::Slider&) override;
    juce::Font getSliderPopupFont (juce::Slider&) override;
    int getSliderPopupPlacement (juce::Slider&) override;
    void drawBubble (juce::Graphics&, juce::BubbleComponent&, const juce::Point<float>& tip, const juce::Rectangle<float>& body) override;

    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
};

// One button of a segmented row or grid: radius 3, a 1 px control-border outline; selected = filled with the text colour, its
// label (or icon) in the panel colour; unselected = the panel fill, the label in #3a3833. Optionally an icon (a stroked path in a
// 36 x 20 box: the wave buttons) in place of the label, and an LED left of the label (Freeze's HELD). Dimmed (a row that doesn't
// apply): the dimmed text, the selected one filled light grey, not clickable (the row's opacity is the row's).
class RackButton : public juce::Button
{
public:
    explicit RackButton (const juce::String& label, float fontSize = 12.0f);

    void setIcon (const juce::Path& viewBox36x20) { icon = viewBox36x20; hasIcon = true; repaint(); }
    void setLed (bool shown, bool lit);
    void setDimmed (bool);
    bool isDimmed() const { return dimmed; }
    bool ledShown() const { return hasLed; }
    bool ledLit() const { return ledOn; }
    const juce::String& label() const { return text; }

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    juce::String text;
    float fontSize;
    juce::Path icon;
    bool hasIcon = false, hasLed = false, ledOn = false, dimmed = false;
};

// A choice parameter as a row (or grid) of RackButtons, one per choice, in the given labels' order (index = the parameter's index).
// Clicking one sets the parameter as one complete gesture (juce::ParameterAttachment); the host's changes show.
class ChoiceRow : public juce::Component
{
public:
    ChoiceRow (juce::RangedAudioParameter&, const juce::StringArray& labels, int columns, float fontSize = 12.0f, juce::UndoManager* = nullptr);

    void resized() override;
    // Dimmed (docs/history/SPEC-v0.6.md section 5: Chord Size outside Freeze): every button dimmed and unclickable, the row at opacity 0.6.
    void setDimmed (bool);
    bool isDimmed() const { return dimmed; }
    RackButton& button (int k) { return *buttons[(size_t) k]; }
    const RackButton& button (int k) const { return *buttons[(size_t) k]; }
    int size() const { return (int) buttons.size(); }
    int selected() const { return current; }
    void setGap (int g) { gap = g; resized(); }

private:
    void show (float value);
    juce::RangedAudioParameter& param;
    std::vector<std::unique_ptr<RackButton>> buttons;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    int columns = 1, current = -1, gap = 4;
    bool dimmed = false;
};

// The small knob (Sustain): a juce::Slider drawn by RackLookAndFeel, ±135°, vertical drag, double-click for the default, the value
// in a bubble while dragging; attached to its parameter (a slider attachment: gestures as the host expects).
class SmallKnob : public juce::Slider
{
public:
    SmallKnob (juce::RangedAudioParameter&, juce::UndoManager* = nullptr);

private:
    juce::SliderParameterAttachment attachment;
};
