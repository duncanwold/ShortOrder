#include "PluginEditor.h"
#include "Latency.h"
#include "Tab.h"
#include "ChordName.h"

namespace
{
// Editors open in this process (message thread only). The first installs the shared look and feel as JUCE's default (so popups and
// tooltips take it); the last one hands the default back.
int openEditors = 0;

// Sharps, middle C = C4. Also below MIDI 0, which Triple Sub can reach (A-1 = 13.75 Hz).
juce::String noteName (int midi) { return faceplate::noteName (midi); }

// Readout::chordTones: the notes, live first (none while the voice is closed).
std::vector<int> unpackChordTones (uint64_t packed)
{
    std::vector<int> notes;
    for (int k = 0; k < 8; ++k)
        if (((packed >> (8 * k)) & 0xffu) != 0xffu)
            notes.push_back ((int) ((packed >> (8 * k)) & 0xffu));
    return notes;
}

// The faceplate's own parameters: everything else a layout has (what a show flag adds) goes in the plain strip.
const juce::StringArray kFaceplateParams { ParamIDs::latency,   ParamIDs::retuneSpeed, ParamIDs::sustain,   ParamIDs::release,     ParamIDs::wave,
                                           ParamIDs::octave,   ParamIDs::chordType,   ParamIDs::chordSize, ParamIDs::chordKey, ParamIDs::bypass };

// docs/history/SPEC-v0.6.md section 1 (px): the page margin, the panel's padding, the rows and the gaps between them.
constexpr int kMargin = 12, kPadY = 18, kPadX = 26, kHeaderH = 31, kHeroH = 296, kGap = 16, kBottomH = 219, kHeroGap = 22, kTraceH = 120;
constexpr int kLineH = 20;
} // namespace

GTTEditor::GTTEditor (GTTProcessor& p, const ShowFlags& flags)
    : AudioProcessorEditor (&p), gttProcessor (p), params (p.apvts), show (flags), latencyModule (*p.apvts.getParameter (ParamIDs::latency)),
      noteShapeModule (p.apvts), voiceModule (p.apvts), chordModule (p.apvts)
{
    setOpaque (true); // the page fills it; with every module opaque too, a module's repaint never repaints the page behind it
    if (openEditors++ == 0)
        juce::LookAndFeel::setDefaultLookAndFeel (&*lookAndFeel);
    setLookAndFeel (&*lookAndFeel);
    ++gttProcessor.scopeFifo().readers; // the processor feeds the low-range detector while an editor is open

    for (juce::Component* c : { (juce::Component*) &headerBar, (juce::Component*) &latencyModule, (juce::Component*) &neckView, (juce::Component*) &noteShapeModule,
                                (juce::Component*) &voiceModule, (juce::Component*) &chordModule })
    {
        addAndMakeVisible (c);
        // Each module is drawn into an image and redrawn only when it changes (its own repaint(): a new note, a parameter, the
        // HELD LED). The neck's 132 glowing LEDs and the text would otherwise be redrawn on every frame a host repaints.
        c->setBufferedToImage (true);
    }
    headerBar.setTitle ("shortorder");
    neckView.setTitle ("playable zone on your guitar neck");

    if (show.trace)
    {
        // Only with the flag: the editor reads the trace (and so the engine pushes it) only while it's shown.
        trace = std::make_unique<TraceView>();
        addAndMakeVisible (*trace);
        ++gttProcessor.traceFifo().readers;
    }
    // The controls a show flag adds, in a plain strip (only if there are any).
    auto s = std::make_unique<ControlColumns> (p.apvts, show, kFaceplateParams);
    if (s->controlCount() > 0)
    {
        strip = std::move (s);
        addAndMakeVisible (*strip);
        strip->sendLookAndFeelChange();
    }
    if (show.advanced)
        for (auto* l : { &debugLine, &settingsLine })
        {
            l->setFont (theme::font (12.0f));
            l->setColour (juce::Label::textColourId, theme::kText);
            l->setColour (juce::Label::backgroundColourId, theme::kPanel);
            l->setColour (juce::Label::outlineColourId, theme::kBorder);
            l->setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (*l);
        }

    timerCallback();
    setResizable (false, false);
    setSize (kWidth, naturalHeight());
    startTimerHz (30);
}

GTTEditor::~GTTEditor()
{
    stopTimer();
    --gttProcessor.scopeFifo().readers;
    if (trace != nullptr)
        --gttProcessor.traceFifo().readers;
    setLookAndFeel (nullptr);
    if (--openEditors == 0)
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

int GTTEditor::naturalHeight() const
{
    // The faceplate is 1000 x 640; what a show flag adds makes the window taller (the trace inside the panel, the rest under it).
    int h = kHeight + (trace != nullptr ? kGap + kTraceH : 0);
    int below = 0;
    if (show.advanced)
        below += 2 * (kLineH + 2);
    if (strip != nullptr)
        below += strip->preferredHeight();
    return h + (below > 0 ? below + 4 : 0);
}

void GTTEditor::paint (juce::Graphics& g)
{
    GTT_COUNT_PAINT ("editor");
    g.fillAll (theme::kPage);
    const auto r = panel.toFloat().reduced (0.5f);
    g.setColour (theme::kPanel);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (theme::kBorder);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    if (strip != nullptr)
    {
        g.setColour (theme::kPanel);
        g.fillRoundedRectangle (strip->getBounds().toFloat().reduced (0.5f), 4.0f);
    }
}

void GTTEditor::resized()
{
    const int panelH = kHeight - 2 * kMargin + (trace != nullptr ? kGap + kTraceH : 0);
    panel = { kMargin, kMargin, getWidth() - 2 * kMargin, panelH };
    auto r = panel.reduced (1).reduced (kPadX, kPadY);
    headerBar.setBounds (r.removeFromTop (kHeaderH));
    r.removeFromTop (kGap);
    auto hero = r.removeFromTop (kHeroH);
    latencyModule.setBounds (hero.removeFromLeft (LatencyModule::kWidth));
    hero.removeFromLeft (kHeroGap);
    neckView.setBounds (hero.expanded (NeckView::kGlowMargin, 0)); // the drawing stays at hero; the margin holds the LEDs' glow
    if (trace != nullptr)
    {
        r.removeFromTop (kGap);
        trace->setBounds (r.removeFromTop (kTraceH));
    }
    r.removeFromTop (kGap);
    // Note shape, voice, chord: flex 1 / 1.25 / 1.1, 16 px apart.
    auto bottom = r.removeFromTop (kBottomH);
    const float unit = (float) (bottom.getWidth() - 2 * kGap) / 3.35f;
    noteShapeModule.setBounds (bottom.removeFromLeft (juce::roundToInt (unit)));
    bottom.removeFromLeft (kGap);
    voiceModule.setBounds (bottom.removeFromLeft (juce::roundToInt (unit * 1.25f)));
    bottom.removeFromLeft (kGap);
    chordModule.setBounds (bottom);

    // Under the panel (a taller window): the debug and settings lines, then the strip.
    auto below = getLocalBounds().withTop (panel.getBottom() + 4).reduced (kMargin, 0);
    if (show.advanced)
    {
        debugLine.setBounds (below.removeFromTop (kLineH));
        below.removeFromTop (2);
        settingsLine.setBounds (below.removeFromTop (kLineH));
        below.removeFromTop (2);
    }
    if (strip != nullptr)
        strip->setBounds (below.removeFromTop (strip->preferredHeight()));
}

std::vector<std::pair<juce::String, juce::Rectangle<int>>> GTTEditor::layoutBoxes() const
{
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> out { { "header", headerBar.getBounds() },    { "latency", latencyModule.getBounds() },
                                                                     { "neck", neckView.getBounds() },        { "note shape", noteShapeModule.getBounds() },
                                                                     { "voice", voiceModule.getBounds() },    { "chord", chordModule.getBounds() } };
    if (trace != nullptr)
        out.push_back ({ "trace", trace->getBounds() });
    if (show.advanced)
    {
        out.push_back ({ "debug line", debugLine.getBounds() });
        out.push_back ({ "settings line", settingsLine.getBounds() });
    }
    if (strip != nullptr)
        out.push_back ({ "strip", strip->getBounds() });
    return out;
}

juce::StringArray GTTEditor::readoutLines() const
{
    juce::StringArray out;
    for (auto* l : { &debugLine, &settingsLine })
        if (l->getParentComponent() == this && l->isVisible())
            out.add (l->getText());
    return out;
}

void GTTEditor::timerCallback()
{
    const double fs = gttProcessor.getSampleRate() > 0.0 ? gttProcessor.getSampleRate() : 48000.0;
    const auto& readout = gttProcessor.readout();
    const auto relaxed = std::memory_order_relaxed;

    // The effective detection settings, live from the parameters (the knob, not the engine's debounced window swap), so the hero and
    // the neck follow the knob at once.
    gtt::EngineParams p;
    params.read (p);
    const auto effective = gtt::effectiveDetection (p);

    // The guitar input to the low-range display detector; (with the trace shown) the engine's frames to the trace.
    if (! std::equal_to<double>() (lowRangeRate, fs))
    {
        lowRange.prepare (fs);
        lowRangeRate = fs;
    }
    gttProcessor.scopeFifo().pull ([this] (const float* in, const float*, int n) { lowRange.push (in, n); });
    if (trace != nullptr)
    {
        gttProcessor.traceFifo().pull ([this] (const gtt::TraceFrame& f) { trace->append (f); });
        trace->update (fs);
    }

    const double windowHz = gtt::clampWindow (effective.windowHz);
    const int lowestReliable = gtt::lowestReliableNote (fs, windowHz);
    latencyModule.update (fs);

    // Line 1's states (formatPitchLine): the header status and the neck.
    const bool voiced = readout.voiced.load (relaxed);
    const auto& low = lowRange.reading();
    const auto display = gtt::noteDisplayFor (voiced, readout.noteN.load (relaxed), low.voiced, low.note, lowestReliable, readout.twoPlusNotes.load (relaxed));
    const bool sounding = readout.hasNote.load (relaxed) && (voiced || readout.voiceOpen.load (relaxed));
    const int played = readout.outNote.load (relaxed);
    headerBar.setStatus (HeaderBar::statusFor (sounding, played, display));

    const uint64_t tones = readout.chordTones.load (relaxed);
    const int held = readout.memoryHeld.load (relaxed);
    const bool freeze = p.chordMode == gtt::ChordMode::Memory;
    auto chordNotes = unpackChordTones (tones);
    if (! chordNotes.empty())
        chordNotes.erase (chordNotes.begin()); // the live note is the orange one
    // v0.6.1: the neck's orange is the header's note, lit while it sounds (as "tracking E4"): up to v0.6 it took the detector's
    // voiced hops alone, so every pick's dropout blinked it off for a frame or two and repainted the whole neck.
    neckView.update (lowestReliable, sounding ? played : -1, display.belowFretboard ? -1 : display.belowRange, display.aboveBoard,
                     freeze ? chordNotes : std::vector<int> {}, freeze);
    chordModule.update (p, tones, held);

    if (show.advanced)
    {
        // The headroom gain's reduction (line 2's GR up to v0.5.11) on the debug line.
        const double gr = std::round (gttProcessor.takeGainReductionDb() * 10.0) / 10.0;
        debugLine.setText (formatDebugLine (readout, low, lowestReliable) + (gr < 0.0 ? " | GR " + juce::String (gr, 1) + "dB" : juce::String()),
                           juce::dontSendNotification);
        settingsLine.setText (formatSettingsLine (p, effective), juce::dontSendNotification);
    }
}

juce::String GTTEditor::formatPitchLine (const gtt::Readout& r, const gtt::LowRangeReading& low, int lowestReliable)
{
    // v0.5.7: what's sounding, input -> output (the played note as quantized, the voice's note after Octave),
    // and the words that tell the player what to do. The detail is the debug line's (formatDebugLine).
    const auto relaxed = std::memory_order_relaxed;
    const bool voiced = r.voiced.load (relaxed);
    const auto display = gtt::noteDisplayFor (voiced, r.noteN.load (relaxed), low.voiced, low.note, lowestReliable, r.twoPlusNotes.load (relaxed));
    const juce::String hint = display.hint == gtt::PitchHint::TooLow ? " | TOO LOW" : display.hint == gtt::PitchHint::TwoPlusNotes ? " | 2+ NOTES?" : "";
    const juce::String below = display.belowRange < 0 ? juce::String() : " | BELOW RANGE " + noteName (display.belowRange);
    const juce::String above = display.aboveBoard < 0 ? juce::String() : " | ABOVE BOARD " + noteName (display.aboveBoard); // v0.5.11
    // Sounding: pitch detected, or the note held through Note Release while the voice is open.
    if (r.hasNote.load (relaxed) && (voiced || r.voiceOpen.load (relaxed)))
    {
        const int in = r.outNote.load (relaxed), out = r.voiceNote.load (relaxed);
        return "PLAYING " + noteName (in) + (out != in ? juce::String::fromUTF8 (" \xe2\x86\x92 ") + noteName (out) : juce::String()) + above + below + hint;
    }
    return juce::String::fromUTF8 ("PLAYING \xe2\x80\x94") + below + hint;
}

juce::String GTTEditor::formatDebugLine (const gtt::Readout& r, const gtt::LowRangeReading& low, int lowestReliable)
{
    // Up to v0.5.6 this was line 1; since v0.5.7 it's the debug line (ADVANCED shown only).
    const auto relaxed = std::memory_order_relaxed;
    const bool voiced = r.voiced.load (relaxed);
    const bool hasNote = r.hasNote.load (relaxed);
    const auto display = gtt::noteDisplayFor (voiced, r.noteN.load (relaxed), low.voiced, low.note, lowestReliable, r.twoPlusNotes.load (relaxed));
    const juce::String hint = display.hint == gtt::PitchHint::TooLow ? " | TOO LOW" : display.hint == gtt::PitchHint::TwoPlusNotes ? " | 2+ NOTES?" : "";
    // A note only the low-range detector hears: where it is, or (below open low E) that it's off the board.
    const juce::String in = display.belowRange < 0 ? juce::String ("IN ---")
                                                   : "IN --- | BELOW RANGE " + noteName (display.belowRange)
                                                         + (display.belowFretboard ? juce::String::fromUTF8 (" \xe2\x80\x94 below the fretboard")
                                                                                   : " = " + juce::String (gtt::tabPositionText (gtt::tabPositionFor (display.belowRange))));

    // The voice's own pitch: up to three octaves below.
    const float voiceHz = r.voiceHz.load (relaxed);
    const juce::String voice = "VOICE " + noteName (r.voiceNote.load (relaxed)) + " " + juce::String (voiceHz, 1) + "Hz"
                               + (voiceHz < gtt::kSubsonicHz ? " SUBSONIC" : "");
    const juce::String out = "OUT " + noteName (r.outNote.load (relaxed));

    if (voiced)
    {
        const float cents = r.cents.load (relaxed);
        return "IN " + noteName (r.noteN.load (relaxed)) + " " + (cents >= 0.0f ? "+" : "") + juce::String (juce::roundToInt (cents)) + "c "
               + juce::String (r.f0.load (relaxed), 1) + "Hz conf " + juce::String (r.confidence.load (relaxed), 2) + " | " + out + " | " + voice + hint;
    }

    if (hasNote && r.voiceOpen.load (relaxed))
        return in + " | " + out + " | " + voice + " | RELEASE " + juce::String (juce::roundToInt (r.holdRemainingMs.load (relaxed))) + "ms" + hint;
    return in + (hasNote ? " | OUT --- (FADED)" : " | OUT ---") + hint;
}

juce::String GTTEditor::formatWindowLine (double fs, const gtt::DetectionSettings& effective, float gainReductionDb)
{
    const double windowHz = gtt::clampWindow (effective.windowHz);
    const double latMs = gtt::latencyMs (fs, windowHz); // the voice's delay, as the readout
    const int lowest = gtt::lowestReliableNote (fs, windowHz);
    // The headroom gain, while it's reducing (nothing once it rounds to 0.0 dB).
    const double gr = std::round (gainReductionDb * 10.0) / 10.0;
    return "LATENCY " + juce::String (std::round (latMs * 10.0) / 10.0, 1) + " ms | LOWEST NOTE " + noteName (lowest) + " ("
           + gtt::tabPositionLabel (gtt::tabPositionFor (lowest)) + ")" + (gr < 0.0 ? " | GR " + juce::String (gr, 1) + "dB" : juce::String());
}

juce::String GTTEditor::formatSettingsLine (const gtt::EngineParams& p, const gtt::DetectionSettings& effective)
{
    static const char* const speeds[3] = { "FAST", "MEDIUM", "SLOW" };
    static const char* const releases[3] = { "FAST", "MEDIUM", "LONG" };
    // Note Release by its option's name (a time no option has, which only a test can set, shows bare).
    juce::String release;
    for (int k = 0; k < 3; ++k)
        if (std::equal_to<float>() (effective.holdMs, gtt::noteReleaseMsFor ((gtt::NoteRelease) k)))
            release = juce::String (releases[k]) + " ";
    // p.manual is Manual in effect: never on while the ADVANCED panel is hidden. Hysteresis rounds
    // half up (11.25 c reads 11.3c; printf's round-half-even would give 11.2c). DYN is fixed (v0.5.2), shown like CONF and HYST.
    return (p.manual ? juce::String ("MANUAL | ") : juce::String()) + "CONF " + juce::String (effective.confidence, 4) + " | HYST "
           + juce::String (std::round (effective.hysteresisCents * 10.0) / 10.0, 1) + "c | DYN " + juce::String (juce::roundToInt (p.dynamicsPercent)) + "% | "
           + "SUSTAIN "
           + juce::String (juce::roundToInt (p.sustainPercent)) + "% | RELEASE " + release
           + juce::String (juce::roundToInt (effective.holdMs)) + "ms | RETUNE " + speeds[std::clamp ((int) p.retuneSpeed, 0, 2)];
}

juce::String GTTEditor::formatChordLine (const gtt::EngineParams& p, uint64_t tones, int held)
{
    if (p.chordMode == gtt::ChordMode::Off)
        return "CHORD OFF";

    const auto notes = unpackChordTones (tones);
    const juce::String name = notes.empty() ? juce::String ("---") : juce::String::fromUTF8 (gtt::chordName (notes).c_str());
    // Freeze (Memory): its note count, then what memory holds for the next note (the HELD light's count), then the tones
    // sounding, which after a clear can still be the old chord ringing through Note Release.
    if (p.chordMode == gtt::ChordMode::Memory)
        return "CHORD FREEZE " + juce::String (p.memoryNotes) + " | " + (held > 0 ? juce::String (held) + " HELD" : juce::String ("CLEAR"))
               + (notes.empty() ? juce::String() : " | " + name);

    // Harmonize (Stack): the Key by its label (the note names as spelt there, so "Db", not "DB"), or (Key Off, no
    // option since v0.5.4) the chromatic Triad, then the chord sounding. Voicing, Scale and Chord Level are fixed (v0.5.2).
    const juce::String harmony = p.key >= 0 ? juce::String (gtt::keyMajorName (p.key)) + " MAJ / " + gtt::keyMinorName (p.key) + " MIN"
                                            : juce::String ("CHROMATIC ") + (p.triad == gtt::Triad::Minor ? "MINOR" : "MAJOR");
    return "CHORD HARMONIZE " + harmony + " | " + name;
}
