#pragma once

// v0.6: the ShortOrder faceplate's colours, type and the drawing every part shares (docs/history/SPEC-v0.6.md section 1; the mockups in Design/).
// Message thread only.

#include <juce_gui_basics/juce_gui_basics.h>

namespace theme
{
// The spec's colour table.
const juce::Colour kPage (0xffe4e0d8);          // behind the panel
const juce::Colour kPanel (0xfff3f1ec);         // the panel, unselected buttons
const juce::Colour kChordPanel (0xffece9e3);    // the chord module's background
const juce::Colour kReadout (0xffebe8e1);       // the ms box
const juce::Colour kBorder (0xffd3cec4);        // module outlines
const juce::Colour kControlBorder (0xffc9c4b9); // buttons, select, now box; the knob's ticks past the pointer
const juce::Colour kKnobRim (0xffbdb8ad);       // knob outline, ms box border
const juce::Colour kKnobSkirt (0xffd9d5cc);
const juce::Colour kKnobFace (0xffe9e6df);
const juce::Colour kText (0xff1c1c1c);          // labels, pointer, selected button fill
const juce::Colour kSecondary (0xff6e6a63);     // small captions
const juce::Colour kTertiary (0xff8a857b);      // fret numbers, the "/" in the header, the nut line
const juce::Colour kButtonText (0xff3a3833);    // unselected button text
const juce::Colour kDimmed (0xffa8a397);        // controls that don't apply in the current mode
const juce::Colour kDimmedFill (0xffdedad2);    // a dimmed row's selected value (Chord Size outside Freeze)
const juce::Colour kLedGreen (0xff2fbf52);      // playable
const juce::Colour kLedOff (0xffd6d2c9);        // too low
const juce::Colour kLedOrange (0xfff26b1d);     // you
const juce::Colour kLedBlue (0xff2f7bf5);       // chord tone, HELD
const juce::Colour kHeldOff (0xffcfcac0);       // the HELD LED unlit

// Didact Gothic (SIL OFL, Resources/Fonts/), embedded; the system sans if it can't be loaded. size: the CSS font-size in px
// (JUCE's point height), so the faceplate's sizes are the mockup's.
juce::Typeface::Ptr typeface();
bool typefaceLoaded();
juce::Font font (float size);

// Text with letter spacing (the module titles' ~1 px tracking), left-aligned at x on a baseline-free box (vertically centred).
void drawTracked (juce::Graphics&, const juce::String&, juce::Rectangle<float> area, float size, float tracking, juce::Colour,
                  juce::Justification = juce::Justification::centredLeft);
float trackedWidth (const juce::String&, float size, float tracking);

// An LED: a dot of diameter d centred at c.
//  - glow: a soft radial halo behind the dot (CSS box-shadow 0 0 6..9 px, the dot's colour at 0.7..0.85), not a hard ring;
//  - inset: the unlit look (CSS inset 0 1px 2px rgba(0,0,0,0.25)): the dot, then a darker crescent at its top edge;
//  - ring: an outline of the given width in ringColour round the dot (the played note below range: orange round off-grey).
struct Led
{
    juce::Colour colour;
    float diameter = 10.0f;
    float glowRadius = 0.0f, glowAlpha = 0.0f; // 0: no glow
    bool inset = false;
    juce::Colour ringColour {};
    float ringWidth = 0.0f;
};
void drawLed (juce::Graphics&, juce::Point<float> centre, const Led&);
inline Led playableLed() { return { kLedGreen, 10.0f, 6.0f, 0.7f }; }
inline Led tooLowLed() { return { kLedOff, 10.0f, 0.0f, 0.0f, true }; }
inline Led youLed (float d = 14.0f) { return { kLedOrange, d, 9.0f, 0.85f }; }
inline Led chordToneLed (float d = 14.0f) { return { kLedBlue, d, 9.0f, 0.85f }; }
inline Led belowRangeLed() { return { kLedOff, 14.0f, 0.0f, 0.0f, false, kLedOrange, 2.0f }; }
inline Led heldLed (bool lit) { return lit ? Led { kLedBlue, 8.0f, 6.0f, 0.9f } : Led { kHeldOff, 8.0f }; }

// v0.6.1: the paint probe (GTTSnapshot only, GTT_PAINT_PROBE): each faceplate paint() reports itself, so the snapshot tool can count
// what a status change repaints. Nothing in the plugin.
#if GTT_PAINT_PROBE
void countPaint (const char* who);
void ledDrawn (juce::Graphics&, juce::Rectangle<float> reach); // every LED drawn: the dot and its glow's whole extent
#define GTT_COUNT_PAINT(who) theme::countPaint (who)
#else
#define GTT_COUNT_PAINT(who)
#endif

// A module's outline (radius 4, 1 px border), filled if fill isn't transparent.
void drawModule (juce::Graphics&, juce::Rectangle<float>, juce::Colour fill = juce::Colours::transparentBlack);
} // namespace theme
