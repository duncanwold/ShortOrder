#pragma once

// The pitch trace (v0.5.11; hidden since v0.6 behind gtt::kShowTrace, its code kept and built by the snapshot tool with the flag
// on). v0.4's fretboard and v0.5.4's HELD light went with v0.6's faceplate (the LED neck and the HELD LED, Faceplate.h). Black on white, except the
// fretboard's three status colours and its chord-tone blue. Message thread only; they read the audio thread's
// state through gtt::TraceFifo (lock-free) and the Readout atomics.

#include <juce_gui_basics/juce_gui_basics.h>
#include "DSP/Trace.h"
#include "Tab.h"
#include "DSP/Readout.h"

namespace display
{
const juce::Colour kWhite (0xffffffff);
const juce::Colour kBlack (0xff000000);
const juce::Colour kLightGrey (0xffe6e6e6);
const juce::Colour kMidGrey (0xffb0b0b0);
const juce::Colour kDarkGrey (0xff505050);
const juce::Colour kGreen (0xff2e8b3e);
const juce::Colour kYellow (0xffd4a017);
const juce::Colour kRed (0xffc8352e);
const juce::Colour kBlue (0xff2563c9); // chord tones (v0.5): apart from the status colours and the black "playing now"
} // namespace display

// v0.5.11: the pitch trace, in place of v0.4's scope: the last kSeconds, scrolling. The guitar's detected pitch as a thin grey line
// where the detector has one (bends, wobble, slides); the synth's sounding pitch on top in the chord-tone blue, its thickness and
// brightness following the voice's level, so it fades with the release (Octave shows as the offset between the two); the chord's
// other tones (Freeze, Harmonize) in a lighter blue. Note names up the side at every E and A (the open strings' notes), and the note
// sounding now at the right end of its line; the range follows the playing, moving only when the lines leave it (or have moved far
// from it). Everything is on the voice's timeline (gtt::TraceFrame), so the input is drawn L after it was played, in line with the
// output it became. Message thread only.
class TraceView : public juce::Component
{
public:
    static constexpr double kSeconds = 4.0;  // across the strip
    static constexpr double kMinSpan = 14.0; // semitones shown at least
    TraceView();

    void append (const gtt::TraceFrame&); // the editor pulls the engine's frames (gtt::TraceFifo)
    void update (double sampleRate);      // 30 fps: the vertical range, then repaint
    void paint (juce::Graphics&) override;

    // A level (the voice's, mean |x|) as the line's weight, 0 (not drawn) .. 1: kSilentDb .. kFullDb.
    static float levelAlpha (float level);
    static constexpr float kSilentDb = -60.0f, kFullDb = -18.0f;

    // Snapshot tool: what's in view (frames; points of each line drawn), the range, and the legend's text.
    struct Counts
    {
        int frames = 0, input = 0, output = 0, chord = 0;
    };
    Counts counts() const;
    double viewLow() const { return lo; }
    double viewHigh() const { return hi; }
    static juce::String legendText() { return "GUITAR SYNTH CHORD"; }

private:
    template <typename Fn>
    void forEachInView (Fn&& fn) const; // oldest first
    int newestIndex() const;
    void drawLegend (juce::Graphics&, juce::Rectangle<float> plot) const;

    static constexpr int kRing = 1 << 12; // ~11 s of frames at 48 kHz (one per 128-sample hop)
    static constexpr double kMargin = 1.5, kZoomInSlack = 10.0, kEase = 0.2;
    static constexpr int kWeightSteps = 8;  // the sounding lines' weights (level) drawn in this many steps
    static constexpr int kSteadyFrames = 8; // the input's runs that count toward the range: 8 hops (21 ms at 48 kHz) or more
    static constexpr float kAxisWidth = 26.0f, kNoteWidth = 30.0f, kLegendHeight = 11.0f;
    std::vector<gtt::TraceFrame> ring;
    int write = 0, filled = 0;
    double fs = 48000.0;
    double lo = 57.0, hi = 76.0, targetLo = 57.0, targetHi = 76.0; // MIDI shown (eased toward the target)
};
