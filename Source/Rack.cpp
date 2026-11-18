#include "Rack.h"

using namespace theme;

// ---------------------------------------------------------------------------
// RackLookAndFeel
// ---------------------------------------------------------------------------
RackLookAndFeel::RackLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, kPage);
    setColour (juce::Label::textColourId, kText);
    setColour (juce::ComboBox::backgroundColourId, kPanel);
    setColour (juce::ComboBox::textColourId, kText);
    setColour (juce::ComboBox::outlineColourId, kControlBorder);
    setColour (juce::ComboBox::arrowColourId, kText);
    setColour (juce::PopupMenu::backgroundColourId, kPanel);
    setColour (juce::PopupMenu::textColourId, kText);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, kText);
    setColour (juce::PopupMenu::highlightedTextColourId, kPanel);
    setColour (juce::TooltipWindow::backgroundColourId, kPanel);
    setColour (juce::TooltipWindow::textColourId, kText);
    setColour (juce::TooltipWindow::outlineColourId, kControlBorder);
    setColour (juce::BubbleComponent::backgroundColourId, kPanel);
    setColour (juce::BubbleComponent::outlineColourId, kControlBorder);
    setColour (juce::Slider::textBoxTextColourId, kText);
    setColour (juce::Slider::textBoxBackgroundColourId, kPanel);
    setColour (juce::Slider::textBoxOutlineColourId, kControlBorder);
    setColour (juce::Slider::backgroundColourId, kKnobSkirt);
    setColour (juce::Slider::trackColourId, kTertiary);
    setColour (juce::Slider::thumbColourId, kText);
    setColour (juce::ToggleButton::textColourId, kText);
    setColour (juce::ToggleButton::tickColourId, kText);
    setColour (juce::ToggleButton::tickDisabledColourId, kDimmed);
    setColour (juce::TextEditor::backgroundColourId, kPanel);
    setColour (juce::TextEditor::textColourId, kText);
    setColour (juce::TextEditor::outlineColourId, kControlBorder);
    setColour (juce::TextButton::buttonColourId, kPanel);
    setColour (juce::TextButton::textColourOffId, kButtonText);
}

juce::Typeface::Ptr RackLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    if (auto t = typeface())
        return t;
    return LookAndFeel_V4::getTypefaceForFont (f);
}

void RackLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    GTT_COUNT_PAINT ("key box");
    const auto r = juce::Rectangle<float> ((float) width, (float) height).reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, 3.0f, 1.0f);
    // A small chevron at the right, as a native select's.
    const float cx = (float) width - 12.0f, cy = (float) height / 2.0f;
    juce::Path chevron;
    chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
    chevron.lineTo (cx, cy + 2.0f);
    chevron.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font RackLookAndFeel::getComboBoxFont (juce::ComboBox&) { return font (13.0f); }

void RackLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (1, 1, box.getWidth() - 22, box.getHeight() - 2);
    label.setBorderSize ({ 0, 6, 0, 0 });
    label.setFont (getComboBoxFont (box));
}

juce::Font RackLookAndFeel::getPopupMenuFont() { return font (13.0f); }

void RackLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (kPanel);
    g.setColour (kControlBorder);
    g.drawRect (0, 0, width, height);
}

void RackLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos, float start, float end, juce::Slider& s)
{
    GTT_COUNT_PAINT ("sustain knob");
    // The mockup's sustain knob (54 x 54 viewBox): face r 24 with a 1.5 px rim, a 3 px pointer from the centre to r 19.
    const float size = (float) juce::jmin (width, height), scale = size / 54.0f;
    const auto c = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).getCentre();
    const float alpha = s.isEnabled() ? 1.0f : 0.5f;
    g.setColour (kKnobFace.withMultipliedAlpha (alpha));
    g.fillEllipse (juce::Rectangle<float> (48.0f * scale, 48.0f * scale).withCentre (c));
    g.setColour (kKnobRim.withMultipliedAlpha (alpha));
    g.drawEllipse (juce::Rectangle<float> (48.0f * scale, 48.0f * scale).withCentre (c), 1.5f * scale);
    const float angle = start + pos * (end - start);
    juce::Path pointer;
    pointer.startNewSubPath (c);
    pointer.lineTo (c.x + 19.0f * scale * std::sin (angle), c.y - 19.0f * scale * std::cos (angle));
    g.setColour (kText.withMultipliedAlpha (alpha));
    g.strokePath (pointer, juce::PathStrokeType (3.0f * scale, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font RackLookAndFeel::getSliderPopupFont (juce::Slider&) { return font (12.0f); }
int RackLookAndFeel::getSliderPopupPlacement (juce::Slider&) { return juce::BubbleComponent::right; }

void RackLookAndFeel::drawBubble (juce::Graphics& g, juce::BubbleComponent&, const juce::Point<float>&, const juce::Rectangle<float>& body)
{
    g.setColour (kPanel);
    g.fillRoundedRectangle (body, 3.0f);
    g.setColour (kControlBorder);
    g.drawRoundedRectangle (body.reduced (0.5f), 3.0f, 1.0f);
}

void RackLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto r = juce::Rectangle<float> ((float) width, (float) height);
    g.setColour (kPanel);
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (kControlBorder);
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
    g.setColour (kText);
    g.setFont (font (12.0f));
    g.drawText (text, r, juce::Justification::centred, false);
}

juce::Rectangle<int> RackLookAndFeel::getTooltipBounds (const juce::String& text, juce::Point<int> pos, juce::Rectangle<int> parent)
{
    const int w = (int) std::ceil (juce::GlyphArrangement::getStringWidth (font (12.0f), text)) + 14, h = 22;
    return juce::Rectangle<int> (pos.x > parent.getCentreX() ? pos.x - (w + 12) : pos.x + 12, pos.y > parent.getCentreY() ? pos.y - (h + 6) : pos.y + 6, w, h)
        .constrainedWithin (parent);
}

// ---------------------------------------------------------------------------
// RackButton
// ---------------------------------------------------------------------------
RackButton::RackButton (const juce::String& label, float size) : juce::Button (label), text (label), fontSize (size)
{
    setClickingTogglesState (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void RackButton::setLed (bool shown, bool lit)
{
    if (hasLed != shown || ledOn != lit)
    {
        hasLed = shown;
        ledOn = lit;
        repaint();
    }
}

void RackButton::setDimmed (bool d)
{
    if (dimmed == d)
        return;
    dimmed = d;
    setInterceptsMouseClicks (! d, false); // not clickable
    setMouseCursor (d ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
    repaint();
}

void RackButton::paintButton (juce::Graphics& g, bool, bool)
{
    GTT_COUNT_PAINT ("button");
    const bool on = getToggleState();
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    // The mockup's three looks: selected (dark), unselected, and (dimmed) its selected value in light grey.
    const auto fill = dimmed ? (on ? kDimmedFill : kPanel) : (on ? kText : kPanel);
    const auto border = ! dimmed && on ? kText : kControlBorder;
    const auto ink = dimmed ? kDimmed : (on ? kPanel : kButtonText);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (border);
    g.drawRoundedRectangle (r, 3.0f, 1.0f);

    if (hasIcon)
    {
        // The mockup's 36 x 18 icon of a 0 0 36 20 viewBox, centred; stroke 1.8, round joins and caps.
        const float w = 36.0f, h = 18.0f;
        const auto box = juce::Rectangle<float> (w, h).withCentre (r.getCentre());
        auto p = icon;
        p.applyTransform (juce::AffineTransform::scale (w / 36.0f, h / 20.0f).translated (box.getX(), box.getY()));
        g.setColour (on && ! dimmed ? kPanel : kText);
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }

    const auto f = font (fontSize);
    const float textW = juce::GlyphArrangement::getStringWidth (f, text);
    const float ledW = hasLed ? 8.0f + 6.0f : 0.0f; // the LED, then a 6 px gap
    float x = r.getCentreX() - (textW + ledW) / 2.0f;
    if (hasLed)
    {
        drawLed (g, { x + 4.0f, r.getCentreY() }, heldLed (ledOn));
        x += ledW;
    }
    g.setColour (ink);
    g.setFont (f);
    g.drawText (text, juce::Rectangle<float> (x, r.getY(), textW + 2.0f, r.getHeight()), juce::Justification::centredLeft, false);
}

// ---------------------------------------------------------------------------
// ChoiceRow
// ---------------------------------------------------------------------------
ChoiceRow::ChoiceRow (juce::RangedAudioParameter& p, const juce::StringArray& labels, int cols, float fontSize, juce::UndoManager* um)
    : param (p), columns (juce::jmax (1, cols))
{
    for (int k = 0; k < labels.size(); ++k)
    {
        auto b = std::make_unique<RackButton> (labels[k], fontSize);
        b->setTitle (labels[k]);
        b->onClick = [this, k] {
            if (attachment != nullptr)
                attachment->setValueAsCompleteGesture ((float) k);
        };
        addAndMakeVisible (*b);
        buttons.push_back (std::move (b));
    }
    attachment = std::make_unique<juce::ParameterAttachment> (param, [this] (float v) { show (v); }, um);
    attachment->sendInitialUpdate();
}

void ChoiceRow::show (float value)
{
    current = juce::jlimit (0, (int) buttons.size() - 1, (int) std::lround (value));
    for (int k = 0; k < (int) buttons.size(); ++k)
        buttons[(size_t) k]->setToggleState (k == current, juce::dontSendNotification);
}

void ChoiceRow::setDimmed (bool d)
{
    if (dimmed == d)
        return;
    dimmed = d;
    for (auto& b : buttons)
        b->setDimmed (d);
    setAlpha (d ? 0.6f : 1.0f);
}

void ChoiceRow::resized()
{
    const int n = (int) buttons.size(), rows = (n + columns - 1) / columns;
    const auto r = getLocalBounds();
    const float cellW = ((float) r.getWidth() - (float) (gap * (columns - 1))) / (float) columns;
    const float cellH = ((float) r.getHeight() - (float) (gap * (rows - 1))) / (float) rows;
    for (int k = 0; k < n; ++k)
    {
        const int col = k % columns, row = k / columns;
        const int x0 = juce::roundToInt ((float) col * (cellW + (float) gap)), x1 = juce::roundToInt ((float) col * (cellW + (float) gap) + cellW);
        const int y0 = juce::roundToInt ((float) row * (cellH + (float) gap)), y1 = juce::roundToInt ((float) row * (cellH + (float) gap) + cellH);
        buttons[(size_t) k]->setBounds (x0, y0, x1 - x0, y1 - y0);
    }
}

// ---------------------------------------------------------------------------
// SmallKnob
// ---------------------------------------------------------------------------
SmallKnob::SmallKnob (juce::RangedAudioParameter& p, juce::UndoManager* um)
    : juce::Slider (juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox), attachment (p, *this, um)
{
    setRotaryParameters (juce::MathConstants<float>::pi * 2.0f - juce::degreesToRadians (135.0f), juce::MathConstants<float>::pi * 2.0f + juce::degreesToRadians (135.0f), true);
    setMouseDragSensitivity (180);
    setDoubleClickReturnValue (true, (double) p.convertFrom0to1 (p.getDefaultValue()));
    setPopupDisplayEnabled (true, false, nullptr);
    setTextValueSuffix ("%");
    setNumDecimalPlacesToDisplay (0);
    setTitle (p.getName (100));
}
