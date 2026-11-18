#include "Displays.h"

using namespace display;

// ---------------------------------------------------------------------------
// Pitch trace (v0.5.11)
// ---------------------------------------------------------------------------
namespace
{
const juce::Colour kChordLine = kBlue.interpolatedWith (kWhite, 0.5f); // the chord's other tones: the fretboard's chord blue, lighter
juce::String traceNoteName (int midi)
{
    static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int octave = (midi >= 0 ? midi / 12 : (midi - 11) / 12) - 1;
    return names[((midi % 12) + 12) % 12] + juce::String (octave);
}
} // namespace

TraceView::TraceView() : ring ((size_t) kRing) {}

float TraceView::levelAlpha (float level)
{
    if (level <= 0.0f)
        return 0.0f;
    const float db = 20.0f * std::log10 (level);
    return std::clamp ((db - kSilentDb) / (kFullDb - kSilentDb), 0.0f, 1.0f);
}

void TraceView::append (const gtt::TraceFrame& f)
{
    ring[(size_t) write] = f;
    write = (write + 1) & (kRing - 1);
    filled = std::min (kRing, filled + 1);
}

int TraceView::newestIndex() const { return (write - 1 + kRing) & (kRing - 1); }

template <typename Fn>
void TraceView::forEachInView (Fn&& fn) const
{
    if (filled == 0)
        return;
    const int64_t newest = ring[(size_t) newestIndex()].time, span = (int64_t) (kSeconds * fs);
    for (int k = filled - 1; k >= 0; --k)
    {
        const auto& f = ring[(size_t) ((write - 1 - k + 2 * kRing) & (kRing - 1))];
        if (newest - f.time <= span)
            fn (f);
    }
}

void TraceView::update (double sampleRate)
{
    fs = sampleRate;
    // The notes in view: the input where it has a pitch, the output and the chord tones where they sound.
    // The input counts only in runs of kSteadyFrames or more (a damp's or a mute's few junk readings would stretch the range).
    double mn = 1e9, mx = -1e9, runLo = 1e9, runHi = -1e9;
    int run = 0;
    const gtt::TraceFrame* prev = nullptr;
    forEachInView ([&] (const gtt::TraceFrame& f) {
        auto take = [&] (double m) {
            mn = std::min (mn, m);
            mx = std::max (mx, m);
        };
        const bool joined = f.inMidi >= 0.0f && prev != nullptr && prev->inMidi >= 0.0f && std::abs (f.inMidi - prev->inMidi) < 1.5f;
        run = f.inMidi < 0.0f ? 0 : joined ? run + 1 : 1;
        if (run == 1)
        {
            runLo = runHi = f.inMidi;
        }
        else if (run > 1)
        {
            runLo = std::min (runLo, (double) f.inMidi);
            runHi = std::max (runHi, (double) f.inMidi);
        }
        if (run >= kSteadyFrames)
        {
            take (runLo);
            take (runHi);
        }
        prev = &f;
        if (f.outMidi >= 0.0f && levelAlpha (f.level) > 0.0f)
            take (f.outMidi);
        for (int k = 0; k < gtt::TraceFrame::kChordTones; ++k)
            if (f.chordMidi[(size_t) k] >= 0.0f && levelAlpha (f.chordLevel[(size_t) k]) > 0.0f)
                take (f.chordMidi[(size_t) k]);
    });
    if (mx >= mn)
    {
        // Retarget only when the lines leave the view, or the view is much wider than they need (they've scrolled away): the axis
        // doesn't move on every note.
        const double needLo = mn - kMargin, needHi = mx + kMargin;
        const double span = std::max (kMinSpan, needHi - needLo);
        if (needLo < targetLo || needHi > targetHi || targetHi - targetLo > span + kZoomInSlack)
        {
            const double mid = 0.5 * (needLo + needHi);
            targetLo = mid - 0.5 * span;
            targetHi = mid + 0.5 * span;
        }
    }
    lo += kEase * (targetLo - lo);
    hi += kEase * (targetHi - hi);
    repaint();
}

TraceView::Counts TraceView::counts() const
{
    Counts c;
    forEachInView ([&] (const gtt::TraceFrame& f) {
        ++c.frames;
        c.input += f.inMidi >= 0.0f ? 1 : 0;
        c.output += f.outMidi >= 0.0f && levelAlpha (f.level) > 0.0f ? 1 : 0;
        for (int k = 0; k < gtt::TraceFrame::kChordTones; ++k)
            c.chord += f.chordMidi[(size_t) k] >= 0.0f && levelAlpha (f.chordLevel[(size_t) k]) > 0.0f ? 1 : 0;
    });
    return c;
}

void TraceView::drawLegend (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const auto font = juce::FontOptions ("Menlo", 9.5f, juce::Font::plain);
    const char* const names[3] = { "GUITAR", "SYNTH", "CHORD" };
    const juce::Colour colours[3] = { kMidGrey, kBlue, kChordLine };
    const float thick[3] = { 1.0f, 2.5f, 2.0f };
    float width = 4.0f;
    for (auto* n : names)
        width += 15.0f + juce::GlyphArrangement::getStringWidth (juce::Font (font), n) + 10.0f;
    const juce::Rectangle<float> box (plot.getX() + 2.0f, getLocalBounds().toFloat().getY() + 1.5f, width, kLegendHeight);
    g.setColour (kWhite.withAlpha (0.85f));
    g.fillRoundedRectangle (box, 2.0f);
    g.setFont (font);
    float x = box.getX() + 4.0f;
    for (int k = 0; k < 3; ++k)
    {
        g.setColour (colours[k]);
        g.fillRect (juce::Rectangle<float> (x, box.getCentreY() - thick[k] * 0.5f, 12.0f, thick[k]));
        g.setColour (kDarkGrey);
        const float w = juce::GlyphArrangement::getStringWidth (juce::Font (font), names[k]);
        g.drawText (names[k], juce::Rectangle<float> (x + 15.0f, box.getY(), w + 2.0f, box.getHeight()), juce::Justification::centredLeft);
        x += 15.0f + w + 10.0f;
    }
}

void TraceView::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.fillAll (kWhite);
    g.setColour (kMidGrey);
    g.drawRect (area, 1.0f);
    const auto plot = area.reduced (4.0f, 3.0f).withTrimmedLeft (kAxisWidth).withTrimmedRight (kNoteWidth).withTrimmedTop (kLegendHeight); // the legend's strip above
    if (plot.getHeight() < 4.0f || hi <= lo)
        return;
    auto yAt = [&] (double m) { return (float) (plot.getBottom() - (m - lo) / (hi - lo) * plot.getHeight()); };

    // Gridlines and names at every E and A (the open strings' notes; at most one or two per octave, so it stays uncluttered).
    g.setFont (juce::FontOptions ("Menlo", 9.5f, juce::Font::plain));
    for (int m = (int) std::ceil (lo); m <= (int) std::floor (hi); ++m)
    {
        const int pc = ((m % 12) + 12) % 12;
        if (pc != 4 && pc != 9)
            continue;
        const float y = yAt (m);
        g.setColour (kLightGrey);
        g.drawHorizontalLine ((int) std::lround (y), plot.getX(), plot.getRight());
        g.setColour (kDarkGrey);
        g.drawText (traceNoteName (m), juce::Rectangle<float> (area.getX() + 3.0f, y - 6.0f, kAxisWidth - 2.0f, 12.0f), juce::Justification::centredLeft);
    }
    if (filled == 0)
    {
        drawLegend (g, plot);
        return;
    }

    const int64_t newest = ring[(size_t) newestIndex()].time;
    const double span = kSeconds * fs, gapSamples = 0.012 * fs; // a break wider than 12 ms (4 hops at 48 kHz): not joined
    auto xAt = [&] (int64_t t) { return (float) (plot.getRight() - (double) (newest - t) / span * plot.getWidth()); };

    g.saveState();
    g.reduceClipRegion (plot.toNearestInt());
    // The input: the guitar's detected pitch, thin grey, joined only between consecutive readings.
    {
        juce::Path p;
        const gtt::TraceFrame* prev = nullptr;
        forEachInView ([&] (const gtt::TraceFrame& f) {
            if (f.inMidi >= 0.0f)
            {
                if (prev != nullptr && prev->inMidi >= 0.0f && (double) (f.time - prev->time) < gapSamples && std::abs (f.inMidi - prev->inMidi) < 1.5f)
                    p.lineTo (xAt (f.time), yAt (f.inMidi));
                else
                    p.startNewSubPath (xAt (f.time), yAt (f.inMidi));
            }
            prev = &f;
        });
        g.setColour (kMidGrey);
        g.strokePath (p, juce::PathStrokeType (1.0f));
    }
    // A sounding line: its thickness and paleness follow the level (so it fades with the release). Drawn as runs at the same weight
    // (the level in kWeightSteps steps), each stroked once, opaque (paler toward white; overlaps don't darken), with round joints.
    auto line = [&] (juce::Colour c, float maxThick, auto pitchOf, auto levelOf) {
        const gtt::TraceFrame* prev = nullptr;
        juce::Path run;
        int runStep = -1;
        auto flush = [&] {
            if (runStep > 0 && ! run.isEmpty())
            {
                const float a = (float) runStep / (float) kWeightSteps;
                g.setColour (c.interpolatedWith (kWhite, 0.8f * (1.0f - a)));
                g.strokePath (run, juce::PathStrokeType (0.8f + (maxThick - 0.8f) * a, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            run.clear();
            runStep = -1;
        };
        forEachInView ([&] (const gtt::TraceFrame& f) {
            if (prev != nullptr)
            {
                const float m0 = pitchOf (*prev), m1 = pitchOf (f);
                const float a = std::min (levelAlpha (levelOf (*prev)), levelAlpha (levelOf (f)));
                const int step = (int) std::ceil (a * (float) kWeightSteps);
                if (m0 >= 0.0f && m1 >= 0.0f && step > 0 && (double) (f.time - prev->time) < gapSamples && std::abs (m1 - m0) < 1.0f)
                {
                    if (step != runStep)
                    {
                        flush();
                        runStep = step;
                        run.startNewSubPath (xAt (prev->time), yAt (m0));
                    }
                    run.lineTo (xAt (f.time), yAt (m1));
                }
                else
                {
                    flush();
                }
            }
            prev = &f;
        });
        flush();
    };
    for (int k = 0; k < gtt::TraceFrame::kChordTones; ++k)
        line (kChordLine, 3.0f, [k] (const gtt::TraceFrame& f) { return f.chordMidi[(size_t) k]; }, [k] (const gtt::TraceFrame& f) { return f.chordLevel[(size_t) k]; });
    line (kBlue, 3.5f, [] (const gtt::TraceFrame& f) { return f.outMidi; }, [] (const gtt::TraceFrame& f) { return f.level; });
    g.restoreState();

    drawLegend (g, plot); // on top, on a white backing

    // The note sounding now, at the right end of its line.
    const auto& last = ring[(size_t) newestIndex()];
    if (last.outMidi >= 0.0f && levelAlpha (last.level) > 0.0f)
    {
        const float y = std::clamp (yAt (last.outMidi), plot.getY() + 6.0f, plot.getBottom() - 6.0f);
        g.setColour (kBlue);
        g.setFont (juce::FontOptions ("Menlo", 10.0f, juce::Font::bold));
        g.drawText (traceNoteName ((int) std::lround (last.outMidi)), juce::Rectangle<float> (plot.getRight() + 3.0f, y - 6.0f, kNoteWidth - 3.0f, 12.0f),
                    juce::Justification::centredLeft);
    }
}
