#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "Faceplate.h"
#include "Displays.h"
#include "Controls.h"
#include "DSP/LowRange.h"

// v0.6: the ShortOrder faceplate (docs/history/SPEC-v0.6.md), 1000 x 640, fixed. A 12 px page margin round one panel; in it, top to bottom: the
// header (the name, the status LED in place of readout line 1), the hero row (set your latency; the LED neck) and the bottom row
// (note shape, voice, chord with the HELD LED and the now box). What the show flags add goes in a taller window: the pitch trace
// (gtt::kShowTrace) inside the panel under the hero row; the debug and settings lines (ADVANCED shown) and a plain strip of the
// controls a flag adds (ControlColumns: ADVANCED) under the panel.
class GTTEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    // show: which hidden parts to build (the processor's flags by default; the snapshot tool turns them all on).
    explicit GTTEditor (GTTProcessor&, const ShowFlags& show);
    explicit GTTEditor (GTTProcessor& p) : GTTEditor (p, p.showFlags()) {}
    ~GTTEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // One 30 fps update (public for the offscreen snapshot tool).
    void refresh() { timerCallback(); }

    // For the snapshot tool.
    const HeaderBar& header() const { return headerBar; }
    LatencyModule& latency() { return latencyModule; }
    const NeckView& neck() const { return neckView; }
    const NoteShapeModule& noteShape() const { return noteShapeModule; }
    const VoiceModule& voice() const { return voiceModule; }
    ChordModule& chord() { return chordModule; }
    const TraceView* traceView() const { return trace.get(); }
    const ControlColumns* flagStrip() const { return strip.get(); }
    const gtt::LowRangeReading& lowRangeReading() const { return lowRange.reading(); }
    juce::StringArray readoutLines() const; // the debug and settings lines on screen (ADVANCED shown only)
    juce::Rectangle<int> panelBounds() const { return panel; }
    // Every child's bounds by name, to check the layout has no overlaps (snapshot tool).
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> layoutBoxes() const;
    int naturalHeight() const;

    // The pure readout formats. Line 1 (playing) and line 2 (latency) are gone from the screen in v0.6 (the header status and the
    // hero say them); the debug and settings lines show only with ADVANCED. Kept as functions: the header reads line 1's states.
    static juce::String formatPitchLine (const gtt::Readout&, const gtt::LowRangeReading& low = {}, int lowestReliable = 0);
    static juce::String formatDebugLine (const gtt::Readout&, const gtt::LowRangeReading& low = {}, int lowestReliable = 0);
    static juce::String formatWindowLine (double sampleRate, const gtt::DetectionSettings& effective, float gainReductionDb = 0.0f);
    static juce::String formatSettingsLine (const gtt::EngineParams& p, const gtt::DetectionSettings& effective);
    static juce::String formatChordLine (const gtt::EngineParams& p, uint64_t tones, int held = 0);

    static constexpr int kWidth = 1000, kHeight = 640;

private:
    void timerCallback() override;

    GTTProcessor& gttProcessor;
    ParamPointers params;
    const ShowFlags show;
    // Shared across editor instances and installed as JUCE's default look and feel (Didact Gothic everywhere, popups included).
    // Declared before the children so it outlives them.
    juce::SharedResourcePointer<RackLookAndFeel> lookAndFeel;
    HeaderBar headerBar;
    LatencyModule latencyModule;
    NeckView neckView;
    NoteShapeModule noteShapeModule;
    VoiceModule voiceModule;
    ChordModule chordModule;
    std::unique_ptr<TraceView> trace;      // only with gtt::kShowTrace (ShowFlags::trace)
    std::unique_ptr<ControlColumns> strip; // only when a flag adds a control
    juce::Label debugLine, settingsLine;   // only with ADVANCED shown
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> panel;
    // v0.5.4: the display's low-range pitch detector, on the guitar input from the processor's ScopeFifo (message thread only;
    // prepared when the sample rate changes). Feeds the neck's ring and the header's "too low".
    gtt::LowRangeDetector lowRange;
    double lowRangeRate = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GTTEditor)
};
