#include "Theme.h"
#include "BinaryData.h"

namespace theme
{
namespace
{
juce::Typeface::Ptr& cached()
{
    static juce::Typeface::Ptr t = juce::Typeface::createSystemTypefaceFor (BinaryData::DidactGothicRegular_ttf, (size_t) BinaryData::DidactGothicRegular_ttfSize);
    return t;
}
} // namespace

juce::Typeface::Ptr typeface() { return cached(); }
bool typefaceLoaded() { return cached() != nullptr; }

juce::Font font (float size)
{
    if (auto t = cached())
        return juce::Font (juce::FontOptions (t).withPointHeight (size));
    return juce::Font (juce::FontOptions().withPointHeight (size)); // the system sans
}

float trackedWidth (const juce::String& text, float size, float tracking)
{
    const auto f = font (size);
    return juce::GlyphArrangement::getStringWidth (f, text) + tracking * (float) juce::jmax (0, text.length() - 1);
}

void drawTracked (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, float size, float tracking, juce::Colour colour,
                  juce::Justification just)
{
    g.setColour (colour);
    const auto f = font (size);
    if (tracking <= 0.0f)
    {
        g.setFont (f);
        g.drawText (text, area, just, false);
        return;
    }
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (f, text, 0.0f, 0.0f);
    for (int k = 0; k < glyphs.getNumGlyphs(); ++k)
        glyphs.getGlyph (k).moveBy (tracking * (float) k, 0.0f);
    const float w = trackedWidth (text, size, tracking);
    float x = area.getX();
    if (just.testFlags (juce::Justification::horizontallyCentred))
        x = area.getCentreX() - w / 2.0f;
    else if (just.testFlags (juce::Justification::right))
        x = area.getRight() - w;
    const float y = area.getCentreY() + (f.getAscent() - f.getDescent()) / 2.0f;
    glyphs.moveRangeOfGlyphs (0, -1, x, y);
    glyphs.draw (g);
}

void drawLed (juce::Graphics& g, juce::Point<float> c, const Led& led)
{
    const float r = led.diameter / 2.0f;
#if GTT_PAINT_PROBE
    const float reach = r + (led.glowRadius > 0.0f && led.glowAlpha > 0.0f ? led.glowRadius : 0.0f);
    ledDrawn (g, juce::Rectangle<float> (2.0f * reach, 2.0f * reach).withCentre (c));
#endif
    if (led.glowRadius > 0.0f && led.glowAlpha > 0.0f)
    {
        // A halo from the dot's edge out to glowRadius past it, fading to nothing (a CSS box-shadow's blur is a Gaussian; this is
        // its look: strongest at the edge, gone by the blur radius).
        const float outer = r + led.glowRadius;
        juce::ColourGradient halo (led.colour.withAlpha (led.glowAlpha), c, led.colour.withAlpha (0.0f), c.translated (outer, 0.0f), true);
        const double edge = (double) (r / outer);
        halo.clearColours();
        halo.addColour (0.0, led.colour.withAlpha (led.glowAlpha));
        halo.addColour (edge, led.colour.withAlpha (led.glowAlpha * 0.9f));
        halo.addColour (edge + (1.0 - edge) * 0.45, led.colour.withAlpha (led.glowAlpha * 0.3f));
        halo.addColour (1.0, led.colour.withAlpha (0.0f));
        g.setGradientFill (halo);
        g.fillEllipse (juce::Rectangle<float> (2.0f * outer, 2.0f * outer).withCentre (c));
    }
    const auto dot = juce::Rectangle<float> (led.diameter, led.diameter).withCentre (c);
    g.setColour (led.colour);
    g.fillEllipse (dot);
    if (led.inset)
    {
        // Inset shadow, offset 1 px down: darker at the top inside edge, nothing below the middle.
        juce::ColourGradient shade (juce::Colours::black.withAlpha (0.22f), c.x, dot.getY(), juce::Colours::black.withAlpha (0.0f), c.x, dot.getY() + r * 0.9f, false);
        g.setGradientFill (shade);
        g.fillEllipse (dot);
    }
    if (led.ringWidth > 0.0f)
    {
        g.setColour (led.ringColour);
        g.drawEllipse (dot.reduced (led.ringWidth / 2.0f), led.ringWidth);
    }
}

void drawModule (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill)
{
    const auto box = r.reduced (0.5f);
    if (! fill.isTransparent())
    {
        g.setColour (fill);
        g.fillRoundedRectangle (box, 4.0f);
    }
    g.setColour (kBorder);
    g.drawRoundedRectangle (box, 4.0f, 1.0f);
}
} // namespace theme
