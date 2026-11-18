#include "Faceplate.h"
#include "Params.h"
#include "ChordName.h"
#include "ChordSplit.h"
#include "Latency.h"
#include <array>

using namespace theme;

namespace faceplate
{
juce::String noteName (int midi)
{
    static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int octave = (midi >= 0 ? midi / 12 : (midi - 11) / 12) - 1;
    return names[((midi % 12) + 12) % 12] + juce::String (octave);
}

void drawModuleTitle (juce::Graphics& g, juce::Rectangle<int> module, const juce::String& title)
{
    drawTracked (g, title, module.reduced (kPadX, kPadY).removeFromTop (kTitleH).toFloat(), kTitleSize, kTracking, kText);
}
} // namespace faceplate

using faceplate::noteName;

namespace
{
// Readout::chordTones: the notes, live first (none while the voice is closed).
std::vector<int> unpackTones (uint64_t packed)
{
    std::vector<int> notes;
    for (int k = 0; k < 8; ++k)
        if (((packed >> (8 * k)) & 0xffu) != 0xffu)
            notes.push_back ((int) ((packed >> (8 * k)) & 0xffu));
    return notes;
}

// A small right- or left-pointing triangle (the captions' and the ABOVE BOARD arrow), drawn as a path so it doesn't depend on the font.
void fillTriangle (juce::Graphics& g, juce::Rectangle<float> r, bool right)
{
    juce::Path t;
    if (right)
        t.addTriangle (r.getX(), r.getY(), r.getRight(), r.getCentreY(), r.getX(), r.getBottom());
    else
        t.addTriangle (r.getRight(), r.getY(), r.getX(), r.getCentreY(), r.getRight(), r.getBottom());
    g.fillPath (t);
}
} // namespace

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------
HeaderStatus HeaderBar::statusFor (bool sounding, int played, const gtt::NoteDisplay& d)
{
    if (d.aboveBoard >= 0)
        return { HeaderStatus::Led::Orange, "above board " + noteName (d.aboveBoard) };
    if (d.belowRange >= 0 || d.hint == gtt::PitchHint::TooLow)
        return { HeaderStatus::Led::Orange, juce::String::fromUTF8 ("too low \xe2\x80\x93 try octave") };
    if (sounding)
        return { HeaderStatus::Led::Green, "tracking " + noteName (played) };
    return {};
}

void HeaderBar::setStatus (const HeaderStatus& s)
{
    if (s == current)
        return;
    current = s;
    repaint (statusArea()); // the status strip only (the header is opaque, so nothing behind it repaints)
}

juce::Rectangle<float> HeaderBar::statusLedBounds() const
{
    const auto status = statusArea().toFloat();
    return juce::Rectangle<float> (kLedDiameter, kLedDiameter).withCentre ({ status.getRight() - kLedInset, status.getCentreY() });
}

juce::Rectangle<float> HeaderBar::statusTextArea() const { return statusArea().toFloat().withRight (statusLedBounds().getX() - kLedGap); }

void HeaderBar::paint (juce::Graphics& g)
{
    GTT_COUNT_PAINT ("header");
    g.fillAll (kPanel);
    auto r = getLocalBounds().toFloat();
    r.removeFromRight ((float) kStatusWidth); // the status strip (drawn below)
    // shortorder / the 8ms guitar synthesizer, on one baseline, 10 px apart.
    const auto f = font (kTitleSize);
    const float baseline = r.getCentreY() + (f.getAscent() - f.getDescent()) / 2.0f;
    float x = r.getX();
    auto word = [&] (const juce::String& t, juce::Colour c, float tracking) {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (f, t, x, baseline);
        for (int k = 0; k < glyphs.getNumGlyphs(); ++k)
            glyphs.getGlyph (k).moveBy (tracking * (float) k, 0.0f);
        g.setColour (c);
        glyphs.draw (g);
        x += trackedWidth (t, kTitleSize, tracking) + 10.0f;
    };
    word ("shortorder", kText, 0.5f);
    word ("/", kTertiary, 0.0f);
    word ("the 8ms guitar synthesizer", kText, 0.0f);

    // The status (v0.6.1): the LED at its fixed spot at the right edge, the text right-aligned, ending 8 px before it.
    g.setColour (kSecondary);
    g.setFont (font (kStatusSize));
    g.drawText (current.text, statusTextArea(), juce::Justification::centredRight, false);
    const auto c = statusLedBounds().getCentre();
    switch (current.led)
    {
        case HeaderStatus::Led::Green: drawLed (g, c, { kLedGreen, kLedDiameter, 5.0f, 0.7f }); break;
        case HeaderStatus::Led::Orange: drawLed (g, c, { kLedOrange, kLedDiameter, 6.0f, 0.8f }); break;
        case HeaderStatus::Led::Off: drawLed (g, c, { kLedOff, kLedDiameter, 0.0f, 0.0f, true }); break;
    }
}

// ---------------------------------------------------------------------------
// LatencyKnob
// ---------------------------------------------------------------------------
namespace
{
constexpr float kKnobOffsetX = 20.0f; // the 200 px drawing sits 20 px in, so the right-hand labels aren't clipped
}

LatencyKnob::LatencyKnob (juce::RangedAudioParameter& window, juce::UndoManager* um) : param (window)
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    setRepaintsOnMouseActivity (false);
    attachment = std::make_unique<juce::ParameterAttachment> (param, [this] (float v) { show (v); }, um);
    attachment->sendInitialUpdate();
}

void LatencyKnob::setSampleRate (double rate)
{
    if (std::equal_to<double>() (rate, fs))
        return;
    fs = rate;
    show ((float) value);
}

double LatencyKnob::position() const { return gtt::latencyKnobPosition (value); }

juce::String LatencyKnob::describe (double rate, double hz)
{
    return "latency " + juce::String (gtt::latencyMsText (rate, hz)) + " ms, lowest note " + noteName (gtt::lowestReliableNote (rate, gtt::clampWindow (hz)));
}

juce::StringArray LatencyKnob::labelTexts() const
{
    juce::StringArray out;
    for (int k : gtt::kLatencyLabelDetents)
        out.add (gtt::latencyMsText (fs, gtt::latencyDetentHz (k)));
    return out;
}

void LatencyKnob::show (float v)
{
    value = v;
    const auto text = describe (fs, value);
    setTitle (text);
    setDescription (text);
    repaint();
}

void LatencyKnob::paint (juce::Graphics& g)
{
    GTT_COUNT_PAINT ("knob");
    g.addTransform (juce::AffineTransform::translation (kKnobOffsetX, 0.0f));
    const juce::Point<float> c (kCentre, kCentre);
    const double pos = position();
    auto at = [&c] (double deg, float r) {
        const double a = juce::degreesToRadians (deg);
        return juce::Point<float> (c.x + r * (float) std::sin (a), c.y - r * (float) std::cos (a));
    };
    // 15 ticks at r 64 -> 78 (major, every other one, 2.5 px) and r 64 -> 73 (minor, 1.5 px): up to the pointer in the text colour.
    for (int k = 0; k < gtt::kLatencyDetents; ++k)
    {
        const bool major = k % 2 == 0, on = (double) k <= pos + 1e-6;
        const double deg = gtt::latencyKnobAngle (k);
        g.setColour (on ? kText : kControlBorder);
        juce::Path tick;
        tick.startNewSubPath (at (deg, 64.0f));
        tick.lineTo (at (deg, major ? 78.0f : 73.0f));
        g.strokePath (tick, juce::PathStrokeType (major ? 2.5f : 1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    // The real latency at detents 0, 4, 8, 12 and 14, at r 92.
    g.setColour (kSecondary);
    g.setFont (font (11.0f));
    for (int k : gtt::kLatencyLabelDetents)
    {
        const auto p = at (gtt::latencyKnobAngle (k), 92.0f);
        g.drawText (gtt::latencyMsText (fs, gtt::latencyDetentHz (k)), juce::Rectangle<float> (40.0f, 14.0f).withCentre (p), juce::Justification::centred, false);
    }
    // Skirt r 58, face r 52 with a 1.5 px rim, a 5 px pointer to r 46, the hub (r 5).
    g.setColour (kKnobSkirt);
    g.fillEllipse (juce::Rectangle<float> (116.0f, 116.0f).withCentre (c));
    g.setColour (kKnobFace);
    g.fillEllipse (juce::Rectangle<float> (104.0f, 104.0f).withCentre (c));
    g.setColour (kKnobRim);
    g.drawEllipse (juce::Rectangle<float> (104.0f, 104.0f).withCentre (c), 1.5f);
    juce::Path pointer;
    pointer.startNewSubPath (c);
    pointer.lineTo (at (gtt::latencyKnobAngle (pos), 46.0f));
    g.setColour (kText);
    g.strokePath (pointer, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.fillEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre (c));
}

int LatencyKnob::detentAtAngle (juce::Point<float> p) const
{
    const auto d = p - juce::Point<float> (kCentre + kKnobOffsetX, kCentre);
    const double deg = std::clamp (juce::radiansToDegrees (std::atan2 ((double) d.x, (double) -d.y)), -135.0, 135.0);
    return std::clamp ((int) std::lround ((deg + 135.0) / (270.0 / (gtt::kLatencyDetents - 1))), 0, gtt::kLatencyDetents - 1);
}

void LatencyKnob::setDetent (int k, bool asGesture)
{
    k = std::clamp (k, 0, gtt::kLatencyDetents - 1);
    const float hz = gtt::latencyDetentHz (k);
    if (asGesture)
    {
        attachment->setValueAsCompleteGesture (hz);
        return;
    }
    if (k == lastSet)
        return;
    lastSet = k;
    attachment->setValueAsPartOfGesture (hz); // the first move snaps a between-detent value
}

void LatencyKnob::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.position;
    const float r = p.getDistanceFrom ({ kCentre + kKnobOffsetX, kCentre });
    attachment->beginGesture();
    inGesture = true;
    lastSet = -1;
    startPosition = position();
    if (r > 60.0f) // the tick ring and the labels: jump there and follow the mouse round
    {
        drag = Drag::Angle;
        setDetent (detentAtAngle (p), false);
    }
    else
        drag = r < 14.0f ? Drag::Vertical : Drag::Undecided;
}

void LatencyKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::Undecided)
    {
        const auto offset = e.getOffsetFromDragStart().toFloat();
        if (offset.getDistanceFromOrigin() < 4.0f)
            return;
        // Round the knob (tangential) follows the angle; toward or away from the centre is a vertical drag.
        auto radial = e.mouseDownPosition - juce::Point<float> (kCentre + kKnobOffsetX, kCentre);
        radial /= juce::jmax (1.0f, radial.getDistanceFromOrigin());
        const float along = std::abs (offset.x * radial.x + offset.y * radial.y), across = std::abs (offset.x * radial.y - offset.y * radial.x);
        drag = across > along ? Drag::Angle : Drag::Vertical;
    }
    if (drag == Drag::Angle)
        setDetent (detentAtAngle (e.position), false);
    else if (drag == Drag::Vertical)
        setDetent ((int) std::lround (startPosition - (double) e.getDistanceFromDragStartY() / kPixelsPerDetent), false);
}

void LatencyKnob::mouseUp (const juce::MouseEvent&)
{
    if (inGesture)
        attachment->endGesture();
    inGesture = false;
    drag = Drag::None;
}

void LatencyKnob::mouseDoubleClick (const juce::MouseEvent&)
{
    // The default, B3 (8.3 ms). The second click's press has opened a gesture; the value goes in it (closed on release).
    if (inGesture)
    {
        lastSet = -1;
        attachment->setValueAsPartOfGesture (gtt::latencyDetentHz (gtt::kDefaultLatencyDetent));
        lastSet = gtt::kDefaultLatencyDetent;
        drag = Drag::None;
    }
    else
        setDetent (gtt::kDefaultLatencyDetent, true);
}

void LatencyKnob::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (wheel.isInertial)
        return;
    const float d = (wheel.isReversed ? -wheel.deltaY : wheel.deltaY);
    int steps = 0;
    if (! wheel.isSmooth)
        steps = d > 0.0f ? 1 : d < 0.0f ? -1 : 0;
    else
    {
        wheelAccumulator += d;
        steps = (int) (wheelAccumulator / 0.12f);
        wheelAccumulator -= (float) steps * 0.12f;
    }
    if (steps == 0)
        return;
    // From a value between detents, the first notch goes to the next detent that way.
    const double pos = position();
    int k = steps > 0 ? (int) std::floor (pos + 1e-6) + steps : (int) std::ceil (pos - 1e-6) + steps;
    k = std::clamp (k, 0, gtt::kLatencyDetents - 1);
    if (! std::equal_to<float>() (gtt::latencyDetentHz (k), (float) value))
        setDetent (k, true);
}

// ---------------------------------------------------------------------------
// MsReadout
// ---------------------------------------------------------------------------
void MsReadout::setText (const juce::String& t)
{
    if (t == number)
        return;
    number = t;
    repaint();
}

void MsReadout::paint (juce::Graphics& g)
{
    GTT_COUNT_PAINT ("ms readout");
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (kReadout);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (kKnobRim);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);

    // Each figure in a cell as wide as the widest figure (tabular), the point in its own width.
    const auto big = font (44.0f), small = font (20.0f);
    float cell = 0.0f;
    for (auto c : juce::String ("0123456789"))
        cell = std::max (cell, juce::GlyphArrangement::getStringWidth (big, juce::String::charToString (c)));
    float numberW = 0.0f;
    for (auto c : number)
        numberW += juce::CharacterFunctions::isDigit (c) ? cell : juce::GlyphArrangement::getStringWidth (big, juce::String::charToString (c));
    const float gap = juce::GlyphArrangement::getStringWidth (big, " ") * 0.8f, msW = juce::GlyphArrangement::getStringWidth (small, "ms");
    float x = r.getCentreX() - (numberW + gap + msW) / 2.0f;
    const float baseline = r.getCentreY() + (big.getAscent() - big.getDescent()) / 2.0f;
    g.setColour (kText);
    for (auto c : number)
    {
        const auto s = juce::String::charToString (c);
        const float w = juce::GlyphArrangement::getStringWidth (big, s), slot = juce::CharacterFunctions::isDigit (c) ? cell : w;
        juce::GlyphArrangement glyph;
        glyph.addLineOfText (big, s, x + (slot - w) / 2.0f, baseline);
        glyph.draw (g);
        x += slot;
    }
    juce::GlyphArrangement ms;
    ms.addLineOfText (small, "ms", x + gap, baseline);
    g.setColour (kSecondary);
    ms.draw (g);
}

// ---------------------------------------------------------------------------
// LatencyModule
// ---------------------------------------------------------------------------
LatencyModule::LatencyModule (juce::RangedAudioParameter& window) : dial (window)
{
    setOpaque (true);
    addAndMakeVisible (dial);
    addAndMakeVisible (ms);
}

void LatencyModule::update (double fs)
{
    dial.setSampleRate (fs);
    if (! std::equal_to<double>() (fs, rate) || ! std::equal_to<double>() (dial.hz(), shownHz))
    {
        rate = fs;
        shownHz = dial.hz();
        ms.setText (gtt::latencyMsText (fs, shownHz));
    }
}

namespace
{
constexpr int kLatencyPadY = 12, kLatencyTitleH = 18, kCaptionsW = 220, kCaptionsH = 15;
}

void LatencyModule::resized()
{
    auto r = getLocalBounds().reduced (14, kLatencyPadY);
    r.removeFromTop (kLatencyTitleH + 2);
    dial.setBounds (r.removeFromTop (LatencyKnob::kHeight).withSizeKeepingCentre (LatencyKnob::kWidth + 40, LatencyKnob::kHeight));
    r.removeFromTop (kCaptionsH + 6);
    ms.setBounds (r.removeFromTop (MsReadout::kHeight).withSizeKeepingCentre (MsReadout::kWidth, MsReadout::kHeight));
}

void LatencyModule::paint (juce::Graphics& g)
{
    GTT_COUNT_PAINT ("latency");
    g.fillAll (kPanel);
    drawModule (g, getLocalBounds().toFloat());
    auto r = getLocalBounds().reduced (14, kLatencyPadY);
    drawTracked (g, "set your latency", r.removeFromTop (kLatencyTitleH).toFloat(), 15.0f, 1.0f, kText, juce::Justification::centred);
    // The captions under the knob: ◂ more range  ...  faster ▸ (the triangles drawn).
    auto row = juce::Rectangle<int> (getWidth() / 2 - kCaptionsW / 2, dial.getBottom(), kCaptionsW, kCaptionsH).toFloat();
    const auto f = font (12.0f);
    g.setFont (f);
    g.setColour (kSecondary);
    const float tri = 6.0f;
    fillTriangle (g, juce::Rectangle<float> (tri * 0.8f, tri).withCentre ({ row.getX() + 3.0f, row.getCentreY() }), false);
    g.drawText ("more range", row.withTrimmedLeft (tri + 4.0f), juce::Justification::centredLeft, false);
    g.drawText ("faster", row.withTrimmedRight (tri + 4.0f), juce::Justification::centredRight, false);
    fillTriangle (g, juce::Rectangle<float> (tri * 0.8f, tri).withCentre ({ row.getRight() - 3.0f, row.getCentreY() }), true);
}

// ---------------------------------------------------------------------------
// NeckView
// ---------------------------------------------------------------------------
NeckLed NeckView::ledFor (int note, int lowestReliable, int playingNote, int belowRangeNote, const std::vector<int>& chordTones)
{
    if (playingNote >= 0 && note == playingNote)
        return NeckLed::You; // tracked, so orange even under the lowest reliable note
    if (belowRangeNote >= 0 && note == belowRangeNote)
        return NeckLed::YouBelow;
    if (std::find (chordTones.begin(), chordTones.end(), note) != chordTones.end())
        return NeckLed::ChordTone;
    return note >= lowestReliable ? NeckLed::Playable : NeckLed::TooLow;
}

void NeckView::update (int lowestReliable, int playingNote, int belowRangeNote, int aboveBoardNote, const std::vector<int>& freezeTones, bool freezeLegend)
{
    if (lowestReliable == lowest && playingNote == playing && belowRangeNote == belowRange && aboveBoardNote == aboveBoard && freezeTones == tones
        && freezeLegend == chordLegend)
        return;
    // v0.6.1: repaint only what changed: each LED whose state moved (with its glow), the arrow, the legend.
    std::array<NeckLed, 6 * (gtt::kFretCount + 1)> before;
    for (int str = 0; str < 6; ++str)
        for (int f = 0; f <= gtt::kFretCount; ++f)
            before[(size_t) (str * (gtt::kFretCount + 1) + f)] = ledAt (str, f);
    const auto markerBefore = aboveBoardMarker();
    const bool legendBefore = chordLegend;
    lowest = lowestReliable;
    playing = playingNote;
    belowRange = belowRangeNote;
    aboveBoard = aboveBoardNote;
    tones = freezeTones;
    chordLegend = freezeLegend;

    juce::RectangleList<int> dirty;
    for (int str = 0; str < 6; ++str)
        for (int f = 0; f <= gtt::kFretCount; ++f)
            if (ledAt (str, f) != before[(size_t) (str * (gtt::kFretCount + 1) + f)])
                dirty.add (ledArea (str, f));
    if (markerBefore != aboveBoardMarker())
    {
        dirty.add (markerBefore.expanded (1.0f).getSmallestIntegerContainer());
        dirty.add (aboveBoardMarker().expanded (1.0f).getSmallestIntegerContainer());
    }
    if (legendBefore != chordLegend)
        dirty.add (legendArea());
    for (auto r : dirty)
        if (! r.isEmpty())
            repaint (r);
}

juce::Rectangle<int> NeckView::ledArea (int string, int fret) const
{
    return juce::Rectangle<float> (2.0f * kLedReach, 2.0f * kLedReach).withCentre (cellCentre (string, fret)).getSmallestIntegerContainer();
}

NeckLed NeckView::ledAt (int string, int fret) const { return ledFor (gtt::positionNote (string, fret), lowest, playing, belowRange, tones); }

juce::StringArray NeckView::legend() const
{
    juce::StringArray l { "playable", "too low", "you" };
    if (chordLegend)
        l.add ("chord tone");
    return l;
}

juce::String NeckView::hintText() { return "faster latency shrinks the playable zone. low notes come back with octave, a bigger sound with chord."; }

namespace
{
constexpr float kNeckTitleH = 18.0f, kNeckGap = 8.0f, kBoxPadTop = 12.0f, kBoxPadSide = 10.0f, kBoxPadBottom = 6.0f, kLegendH = 14.0f;
}

juce::Rectangle<float> NeckView::contentArea() const { return getLocalBounds().toFloat().reduced ((float) kGlowMargin, 0.0f); }

juce::Rectangle<float> NeckView::boxArea() const
{
    const float h = 1.0f + kBoxPadTop + 6.0f * kCellH + 5.0f * kRowGap + kRowGap + kFretRowH + kBoxPadBottom + 1.0f;
    const auto c = contentArea();
    return { c.getX(), kNeckTitleH + kNeckGap, c.getWidth(), h };
}

juce::Rectangle<int> NeckView::legendArea() const
{
    const auto box = boxArea();
    return juce::Rectangle<float> (0.0f, box.getBottom(), (float) getWidth(), 2.0f * kNeckGap + kLegendH).getSmallestIntegerContainer();
}

juce::Point<float> NeckView::legendLedCentre (int k) const
{
    // The legend's items in a row: an LED, 6 px, the label, 20 px.
    const auto lf = font (12.0f);
    const auto items = legend();
    float x = contentArea().getX();
    auto diameter = [] (int i) { return i < 2 ? 10.0f : 12.0f; }; // playable, too low; you, chord tone
    for (int i = 0; i < k && i < items.size(); ++i)
        x += diameter (i) + 6.0f + juce::GlyphArrangement::getStringWidth (lf, items[i]) + 20.0f;
    const float d = diameter (k);
    return { x + d / 2.0f, boxArea().getBottom() + kNeckGap + kLegendH / 2.0f };
}

juce::Point<float> NeckView::cellCentre (int string, int fret) const
{
    const auto box = boxArea();
    const int row = 5 - string; // high e at the top
    const float x0 = box.getX() + 1.0f + kBoxPadSide + kLabelW;
    return { x0 + ((float) fret + 0.5f) * kCellW, box.getY() + 1.0f + kBoxPadTop + (float) row * (kCellH + kRowGap) + kCellH / 2.0f };
}

juce::Rectangle<float> NeckView::aboveBoardMarker() const
{
    if (aboveBoard < 0)
        return {};
    const auto c = cellCentre (5, gtt::kFretCount);
    return juce::Rectangle<float> (9.0f, 12.0f).withCentre ({ c.x + kCellW / 2.0f + kArrowW / 2.0f, c.y });
}

void NeckView::paint (juce::Graphics& g)
{
    GTT_COUNT_PAINT ("neck");
    g.fillAll (kPanel);
    const auto content = contentArea();
    drawTracked (g, "playable zone on your guitar neck", content.withHeight (kNeckTitleH), 15.0f, 1.0f, kText);
    const auto box = boxArea();
    drawModule (g, box);

    // String letters, the nut, the LEDs (glows first, so a neighbour's glow never covers a dot).
    g.setFont (font (13.0f));
    for (int s = 0; s < 6; ++s)
    {
        const auto c = cellCentre (s, 0);
        g.setColour (kSecondary);
        g.drawText (gtt::kStringNames[(size_t) s], juce::Rectangle<float> (box.getX() + 1.0f + kBoxPadSide, c.y - kCellH / 2.0f, kLabelW, kCellH),
                    juce::Justification::centredLeft, false);
    }
    g.setColour (kTertiary);
    for (int s = 0; s < 6; ++s)
    {
        const auto c = cellCentre (s, 0);
        g.fillRect (juce::Rectangle<float> (c.x + kCellW / 2.0f - 2.0f, c.y - kCellH / 2.0f, 2.0f, kCellH));
    }
    for (int s = 0; s < 6; ++s)
        for (int f = 0; f <= gtt::kFretCount; ++f)
        {
            if (! g.clipRegionIntersects (ledArea (s, f))) // a partial repaint (one LED changed) draws only what touches it
                continue;
            Led led;
            switch (ledAt (s, f))
            {
                case NeckLed::Playable: led = playableLed(); break;
                case NeckLed::TooLow: led = tooLowLed(); break;
                case NeckLed::You: led = youLed(); break;
                case NeckLed::YouBelow: led = belowRangeLed(); break;
                case NeckLed::ChordTone: led = chordToneLed(); break;
            }
            drawLed (g, cellCentre (s, f), led);
        }

    // ABOVE BOARD: an orange arrow past the last fret on the high e string.
    if (aboveBoard >= 0)
    {
        g.setColour (kLedOrange);
        fillTriangle (g, aboveBoardMarker(), true);
    }

    // Fret numbers under the strings: 0, 3, 6 .. 21.
    g.setColour (kTertiary);
    g.setFont (font (10.0f));
    const float fy = cellCentre (0, 0).y + kCellH / 2.0f + kRowGap;
    for (int f = 0; f <= gtt::kFretCount; f += 3)
        g.drawText (juce::String (f), juce::Rectangle<float> (cellCentre (0, f).x - kCellW / 2.0f, fy, kCellW, kFretRowH), juce::Justification::centred, false);

    // The legend: playable, too low, you (and chord tone in Freeze).
    const float ly = box.getBottom() + kNeckGap + kLegendH / 2.0f;
    const auto lf = font (12.0f);
    g.setFont (lf);
    const auto items = legend();
    for (int k = 0; k < items.size(); ++k)
    {
        const Led led = k == 0 ? playableLed() : k == 1 ? tooLowLed() : k == 2 ? Led { kLedOrange, 12.0f, 8.0f, 0.8f } : Led { kLedBlue, 12.0f, 8.0f, 0.8f };
        const float d = led.diameter;
        const auto c = legendLedCentre (k);
        drawLed (g, c, led);
        const float x = c.x + d / 2.0f + 6.0f;
        const float w = juce::GlyphArrangement::getStringWidth (lf, items[k]);
        g.setColour (kSecondary);
        g.drawText (items[k], juce::Rectangle<float> (x, ly - kLegendH / 2.0f, w + 2.0f, kLegendH), juce::Justification::centredLeft, false);
    }

    // The hint, octave and chord in the text colour.
    juce::AttributedString hint;
    const auto hf = font (13.0f);
    hint.append ("faster latency shrinks the playable zone. low notes come back with ", hf, kSecondary);
    hint.append ("octave", hf, kText);
    hint.append (", a bigger sound with ", hf, kSecondary);
    hint.append ("chord", hf, kText);
    hint.append (".", hf, kSecondary);
    hint.setLineSpacing (0.0f);
    const float hy = ly + kLegendH / 2.0f + kNeckGap;
    hint.draw (g, { content.getX(), hy, content.getWidth(), (float) getHeight() - hy });
}

// ---------------------------------------------------------------------------
// Note shape
// ---------------------------------------------------------------------------
namespace
{
juce::RangedAudioParameter& paramRef (juce::AudioProcessorValueTreeState& s, const char* id)
{
    auto* p = s.getParameter (id);
    jassert (p != nullptr);
    return *p;
}
constexpr int kCaptionH = 14;
void drawCaption (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, juce::Colour c = kSecondary)
{
    g.setColour (c);
    g.setFont (font (12.0f));
    g.drawText (text, r, juce::Justification::centredLeft, false);
}
} // namespace

NoteShapeModule::NoteShapeModule (juce::AudioProcessorValueTreeState& s)
    : sustain (paramRef (s, ParamIDs::sustain)), retune (paramRef (s, ParamIDs::retuneSpeed), { "fast", "med", "slow" }, 3),
      release (paramRef (s, ParamIDs::release), { "fast", "med", "long" }, 3)
{
    setOpaque (true);
    addAndMakeVisible (sustain);
    addAndMakeVisible (retune);
    addAndMakeVisible (release);
    retune.setTitle ("retune");
    release.setTitle ("release");
}

namespace
{
constexpr int kKnobSize = 54, kRowH = 24, kRowW = 142;
}

void NoteShapeModule::resized()
{
    auto r = getLocalBounds().reduced (faceplate::kPadX, faceplate::kPadY);
    r.removeFromTop (faceplate::kTitleH + 8);
    // The knob and the two rows side by side, toward the top (centred against each other, as the mockup's flex row).
    const int column = kCaptionH + 3 + kRowH + 8 + kCaptionH + 3 + kRowH, knobColumn = kKnobSize + 4 + kCaptionH;
    const int top = r.getY();
    sustain.setBounds (r.getX(), top + (column - knobColumn) / 2, kKnobSize, kKnobSize);
    const int x = r.getX() + kKnobSize + 14;
    retune.setBounds (x, top + kCaptionH + 3, kRowW, kRowH);
    release.setBounds (x, top + kCaptionH + 3 + kRowH + 8 + kCaptionH + 3, kRowW, kRowH);
}

void NoteShapeModule::paint (juce::Graphics& g)
{
    GTT_COUNT_PAINT ("note shape");
    g.fillAll (kPanel);
    drawModule (g, getLocalBounds().toFloat());
    faceplate::drawModuleTitle (g, getLocalBounds(), "note shape");
    g.setColour (kSecondary);
    g.drawText ("sustain", juce::Rectangle<int> (sustain.getX() - 10, sustain.getBottom() + 4, kKnobSize + 20, kCaptionH), juce::Justification::centred, false);
    drawCaption (g, { retune.getX(), retune.getY() - 3 - kCaptionH, kRowW, kCaptionH }, "retune");
    drawCaption (g, { release.getX(), release.getY() - 3 - kCaptionH, kRowW, kCaptionH }, "release");
}

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------
namespace
{
juce::StringArray waveNames()
{
    juce::StringArray names;
    for (int k = 0; k < gtt::kWaveChoices; ++k)
        names.add (VoiceModule::waveName (gtt::waveForChoice (k)));
    return names;
}
juce::Path pathFrom (std::initializer_list<std::pair<float, float>> points)
{
    juce::Path p;
    bool first = true;
    for (auto [x, y] : points)
    {
        if (first)
            p.startNewSubPath (x, y);
        else
            p.lineTo (x, y);
        first = false;
    }
    return p;
}
} // namespace

juce::String VoiceModule::waveName (gtt::Wave w)
{
    switch (w)
    {
        case gtt::Wave::Saw: return "saw";
        case gtt::Wave::Square: return "square";
        case gtt::Wave::Pulse25: return "nes lead";
        case gtt::Wave::Triangle: return "triangle";
        case gtt::Wave::Sine: return "sine";
        case gtt::Wave::HalfSine: return "half-sine";
        case gtt::Wave::Organ: return "organ";
        case gtt::Wave::Folded: return "folded";
    }
    return {};
}

juce::Path VoiceModule::waveIcon (gtt::Wave w)
{
    // The mockup's waveDefs (SVG path data in a 0 0 36 20 viewBox), point for point.
    switch (w)
    {
        case gtt::Wave::Saw: return pathFrom ({ { 2, 16 }, { 16, 4 }, { 16, 16 }, { 30, 4 }, { 30, 16 }, { 34, 12 } });
        case gtt::Wave::Square: return pathFrom ({ { 2, 16 }, { 2, 4 }, { 11, 4 }, { 11, 16 }, { 20, 16 }, { 20, 4 }, { 29, 4 }, { 29, 16 }, { 34, 16 } });
        case gtt::Wave::Pulse25: return pathFrom ({ { 2, 16 }, { 2, 4 }, { 6, 4 }, { 6, 16 }, { 18, 16 }, { 18, 4 }, { 22, 4 }, { 22, 16 }, { 34, 16 } });
        case gtt::Wave::Triangle: return pathFrom ({ { 2, 16 }, { 9, 4 }, { 16, 16 }, { 23, 4 }, { 30, 16 }, { 34, 9 } });
        case gtt::Wave::Sine:
        {
            juce::Path p;
            p.startNewSubPath (2, 10);
            p.cubicTo (6, 0, 10, 0, 13, 10);
            p.cubicTo (16, 20, 20, 20, 24, 10);
            p.cubicTo (27, 0, 31, 0, 34, 10);
            return p;
        }
        case gtt::Wave::HalfSine:
        {
            juce::Path p;
            p.startNewSubPath (2, 16);
            p.cubicTo (5, 1, 10, 1, 13, 16);
            p.lineTo (22, 16);
            p.cubicTo (25, 1, 30, 1, 33, 16);
            p.lineTo (34, 16);
            return p;
        }
        case gtt::Wave::Organ:
        {
            juce::Path p;
            p.startNewSubPath (2, 10);
            p.cubicTo (4, 2, 7, 5, 9, 10);
            p.cubicTo (11, 15, 13, 4, 16, 10);
            p.cubicTo (19, 16, 21, 6, 24, 10);
            p.cubicTo (26, 14, 29, 3, 34, 10);
            return p;
        }
        case gtt::Wave::Folded:
            return pathFrom ({ { 2, 10 }, { 6, 3 }, { 8, 8 }, { 10, 3 }, { 14, 17 }, { 16, 12 }, { 18, 17 }, { 22, 3 }, { 24, 8 }, { 26, 3 }, { 30, 17 }, { 34, 10 } });
    }
    return {};
}

VoiceModule::VoiceModule (juce::AudioProcessorValueTreeState& s)
    : waves (paramRef (s, ParamIDs::wave), waveNames(), 4),
      octaves (paramRef (s, ParamIDs::octave), { "0", juce::String::fromUTF8 ("\xe2\x88\x92" "1"), juce::String::fromUTF8 ("\xe2\x88\x92" "2"), juce::String::fromUTF8 ("\xe2\x88\x92" "3") }, 4, 13.0f)
{
    setOpaque (true);
    for (int k = 0; k < waves.size(); ++k)
    {
        const auto w = gtt::waveForChoice (k);
        waves.button (k).setIcon (waveIcon (w));
        waves.button (k).setTooltip (waveName (w));
    }
    waveRows = (waves.size() + 3) / 4;
    addAndMakeVisible (waves);
    addAndMakeVisible (octaves);
    waves.setTitle ("wave");
    octaves.setTitle ("octave");
}

namespace
{
constexpr int kGridButtonH = 30;
}

void VoiceModule::resized()
{
    auto r = getLocalBounds().reduced (faceplate::kPadX, faceplate::kPadY);
    r.removeFromTop (faceplate::kTitleH + 6);
    waves.setBounds (r.removeFromTop (waveRows * kGridButtonH + (waveRows - 1) * 4));
    r.removeFromTop (6 + 2 + faceplate::kTitleH + 6);
    octaves.setBounds (r.removeFromTop (kGridButtonH));
}

void VoiceModule::paint (juce::Graphics& g)
{
    GTT_COUNT_PAINT ("voice");
    g.fillAll (kPanel);
    drawModule (g, getLocalBounds().toFloat());
    faceplate::drawModuleTitle (g, getLocalBounds(), "voice");
    drawTracked (g, "octave", juce::Rectangle<int> (waves.getX(), waves.getBottom() + 8, waves.getWidth(), faceplate::kTitleH).toFloat(), faceplate::kTitleSize,
                 faceplate::kTracking, kText);
}

// ---------------------------------------------------------------------------
// Chord
// ---------------------------------------------------------------------------
namespace
{
juce::StringArray keyLabels()
{
    juce::StringArray k;
    for (int i = 0; i < gtt::kChordKeyChoices; ++i)
        k.add (juce::String (gtt::keyLabel (i)));
    return k;
}
} // namespace

ChordModule::ChordModule (juce::AudioProcessorValueTreeState& s)
    : modes (paramRef (s, ParamIDs::chordType), { "off", "freeze", "harmonize" }, 3, 13.0f), sizes (paramRef (s, ParamIDs::chordSize), { "2", "3", "4" }, 3),
      keyAttachment (paramRef (s, ParamIDs::chordKey), key)
{
    setOpaque (true);
    // The ComboBox attachment needs the items before it can show the value: add them, then re-send.
    key.addItemList (keyLabels(), 1);
    keyAttachment.sendInitialUpdate();
    key.setTitle ("key");
    modes.button (1).setLed (true, false);
    modes.setTitle ("chord");
    sizes.setTitle ("size");
    addAndMakeVisible (modes);
    addAndMakeVisible (sizes);
    addAndMakeVisible (key);
}

juce::String ChordModule::nowText (const gtt::EngineParams& p, uint64_t tones, int held)
{
    const auto notes = unpackTones (tones);
    const auto name = notes.empty() ? juce::String() : juce::String::fromUTF8 (gtt::chordName (notes).c_str());
    switch (p.chordMode)
    {
        case gtt::ChordMode::Off: return juce::String::fromUTF8 ("\xe2\x80\x94");
        case gtt::ChordMode::Memory:
            if (held > 0)
                return (name.isEmpty() ? juce::String() : name + juce::String::fromUTF8 (" \xc2\xb7 ")) + juce::String (held) + " held";
            return juce::String (p.memoryNotes) + juce::String::fromUTF8 (" notes \xc2\xb7 play to fill");
        case gtt::ChordMode::Stack: return name.isNotEmpty() ? name : (p.key >= 0 ? juce::String (gtt::keyLabel (p.key)) : juce::String());
    }
    return {};
}

void ChordModule::update (const gtt::EngineParams& p, uint64_t tones, int held)
{
    const bool freeze = p.chordMode == gtt::ChordMode::Memory, harmonize = p.chordMode == gtt::ChordMode::Stack;
    sizes.setDimmed (! freeze);
    if (key.isEnabled() != harmonize)
    {
        key.setEnabled (harmonize);
        key.setAlpha (harmonize ? 1.0f : 0.45f);
    }
    modes.button (1).setLed (true, freeze && held > 0); // HELD: exactly v0.5.11's HELD light (lit while memory holds a note)
    const auto line = nowText (p, tones, held);
    const bool off = p.chordMode == gtt::ChordMode::Off;
    if (off != nowOff || freeze != sizeLive || harmonize != keyLive)
    {
        // A mode change: the captions and the now box.
        nowLine = line;
        nowOff = off;
        sizeLive = freeze;
        keyLive = harmonize;
        repaint();
    }
    else if (line != nowLine)
    {
        nowLine = line;
        repaint (nowBox); // v0.6.1: a new chord or count repaints the now box only
    }
}

namespace
{
constexpr int kModeH = 30, kSizeW = 116, kSizeRowH = 28, kNowH = 31;
}

void ChordModule::resized()
{
    auto r = getLocalBounds().reduced (faceplate::kPadX, faceplate::kPadY);
    r.removeFromTop (faceplate::kTitleH + 6);
    modes.setBounds (r.removeFromTop (kModeH));
    r.removeFromTop (6);
    auto block = r.removeFromTop (kCaptionH + 3 + kSizeRowH);
    auto size = block.removeFromLeft (kSizeW);
    block.removeFromLeft (10);
    sizeCaption = size.removeFromTop (kCaptionH);
    keyCaption = block.removeFromTop (kCaptionH);
    size.removeFromTop (3);
    block.removeFromTop (3);
    sizes.setBounds (size);
    key.setBounds (block);
    r.removeFromTop (6);
    nowBox = r.removeFromTop (kNowH);
}

void ChordModule::paint (juce::Graphics& g)
{
    GTT_COUNT_PAINT ("chord");
    g.fillAll (kPanel);
    drawModule (g, getLocalBounds().toFloat(), kChordPanel);
    faceplate::drawModuleTitle (g, getLocalBounds(), "chord");
    // The captions take their control's state: the text colour while live, dimmed (and faded as the control) otherwise.
    drawCaption (g, sizeCaption, "size", sizeLive ? kText : kDimmed.withMultipliedAlpha (0.6f));
    drawCaption (g, keyCaption, "key", keyLive ? kText : kDimmed.withMultipliedAlpha (0.45f));

    // The now box: "now" at 11 px, then the line at 15 px, on one baseline.
    const auto box = nowBox.toFloat().reduced (0.5f);
    g.setColour (kPanel);
    g.fillRoundedRectangle (box, 3.0f);
    g.setColour (kControlBorder);
    g.drawRoundedRectangle (box, 3.0f, 1.0f);
    auto inner = nowBox.reduced (10, 5).toFloat();
    const auto big = font (15.0f), small = font (11.0f);
    const float baseline = inner.getCentreY() + (big.getAscent() - big.getDescent()) / 2.0f;
    juce::GlyphArrangement caption, line;
    caption.addLineOfText (small, "now", inner.getX(), baseline);
    g.setColour (kSecondary);
    caption.draw (g);
    const float x = inner.getX() + juce::GlyphArrangement::getStringWidth (small, "now") + 10.0f;
    line.addCurtailedLineOfText (big, nowLine, x, baseline, inner.getRight() - x, true);
    g.setColour (nowOff ? kDimmed : kText);
    line.draw (g);
}
