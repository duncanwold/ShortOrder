#include "Params.h"
#include "ChordName.h"
#include "ChordSplit.h"
#include "Latency.h"

using APF = juce::AudioParameterFloat;
using APB = juce::AudioParameterBool;
using APC = juce::AudioParameterChoice;
using API = juce::AudioParameterInt;

namespace
{
juce::ParameterID pid (const char* id) { return { id, 1 }; }

std::unique_ptr<APF> floatParam (const char* id, const juce::String& name, juce::NormalisableRange<float> range, float def,
                                 const char* label, int decimals)
{
    auto toText = [decimals] (float v, int) { return juce::String (v, decimals); };
    return std::make_unique<APF> (pid (id), name, range, def,
                                  juce::AudioParameterFloatAttributes().withLabel (label).withStringFromValueFunction (toText));
}

// Continuous (interval 0), so defaults are stored exactly, with no snapping to a step.
juce::NormalisableRange<float> continuous (float lo, float hi, float skewCentre = -1.0f)
{
    juce::NormalisableRange<float> r (lo, hi);
    if (skewCentre > 0.0f)
        r.setSkewForCentre (skewCentre);
    return r;
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout (bool withAdvanced, std::function<double()> sampleRate)
{
    const float windowMin = (float) gtt::derivedWindowMinHz(), windowMax = (float) gtt::kWindowMaxHz;

    // v0.7, the 1.0 contract (RELEASE-BRIEF.md, "Parameter contract (frozen at 1.0)"): the IDs and names below are what hosts save
    // and show. The names and choices are the faceplate's labels in title case, and the groups are its sections, in its order.
    // Choice order = the gtt enums (RetuneSpeed, NoteRelease, Octave); Wave is gtt::waveForChoice's, Chord gtt::chordChoiceFor's.
    const auto minus = juce::String::fromUTF8 ("\xe2\x88\x92"); // the faceplate's minus sign (U+2212)
    auto latency = std::make_unique<juce::AudioProcessorParameterGroup> ("main", "Latency", "|");
    // The detector's window (Hz): L = 2 * round(fs / Hz). Skewed about the geometric middle (286.1 Hz), so equal travel is roughly
    // equal frets of the lowest note over the whole 13.6 semitones; the default (240.62 Hz) sits at 30% of the travel, 300 Hz at 56%.
    // The value stays Hz; the host's text is the faceplate's ms at the current rate (v0.7: "8.3 ms" at the default, 48 kHz before a
    // rate is known; gtt::latencyHostText), and typed text is read back as ms (gtt::latencyHzForMs; "240 Hz" is still read as Hz).
    // The unit is in the text, so the label is empty.
    auto rate = [sampleRate] { return sampleRate ? sampleRate() : 0.0; };
    auto latencyToText = [rate] (float hz, int) { return juce::String (gtt::latencyHostText (rate(), hz)); };
    auto latencyFromText = [rate] (const juce::String& text)
    {
        const double v = text.retainCharacters ("0123456789.-").getDoubleValue();
        if (text.containsIgnoreCase ("hz"))
            return (float) gtt::clampWindow (v);
        return (float) gtt::latencyHzForMs (rate(), v);
    };
    latency->addChild (std::make_unique<APF> (pid (ParamIDs::latency), "Latency", continuous (windowMin, windowMax, std::sqrt (windowMin * windowMax)),
                                              gtt::defaultWindowHz(),
                                              juce::AudioParameterFloatAttributes().withStringFromValueFunction (latencyToText)
                                                                                   .withValueFromStringFunction (latencyFromText)));

    auto shape = std::make_unique<juce::AudioProcessorParameterGroup> ("shape", "Note Shape", "|");
    shape->addChild (floatParam (ParamIDs::sustain, "Sustain", continuous (0.0f, 100.0f), gtt::kDefaultSustainPercent, "%", 0));
    shape->addChild (std::make_unique<APC> (pid (ParamIDs::retuneSpeed), "Retune", juce::StringArray { "Fast", "Med", "Slow" }, 1));
    // Release keeps the note's pitch after pitch is lost (and with Sustain on, fades the level over it): Fast / Med / Long =
    // 0 / 500 / 1500 ms (v0.5.9; gtt::noteReleaseMsFor).
    shape->addChild (std::make_unique<APC> (pid (ParamIDs::release), "Release", juce::StringArray { "Fast", "Med", "Long" },
                                            (int) gtt::kDefaultNoteRelease));

    // Voice (v0.5.4: PITCH and TONE merged). The Low Pass Filter is fixed at its default (kLowPassCutoffHz).
    auto voice = std::make_unique<juce::AudioProcessorParameterGroup> ("voice", "Voice", "|");
    // Wave: gtt::waveForChoice maps the index to the engine's Wave.
    juce::StringArray waves;
    for (int k = 0; k < gtt::kWaveChoices; ++k)
        waves.add (gtt::waveLabel (gtt::waveForChoice (k)));
    voice->addChild (std::make_unique<APC> (pid (ParamIDs::wave), "Wave", waves, 0));
    voice->addChild (std::make_unique<APC> (pid (ParamIDs::octave), "Octave", juce::StringArray { "0", minus + "1", minus + "2", minus + "3" }, 0));

    // Chord (v0.6: three parameters in place of v0.5.6's one): Chord (Off / Freeze / Harmonize), Size (2 / 3 / 4 notes, Freeze's
    // note count) and Key (Harmonize's key, named by the major key and its relative minor: C maj / A min .. B maj / G# min). Each keeps
    // its value across mode changes; gtt::legacyChordIndexFor reads the three as v0.5.11's Chord index.
    juce::StringArray sizes, keys;
    for (int n = gtt::kMemoryNotesMin; n <= gtt::kMemoryNotesMax; ++n)
        sizes.add (juce::String (n));
    for (int k = 0; k < gtt::kChordKeyChoices; ++k)
        keys.add (juce::String (gtt::keyLabel (k)));
    jassert (sizes.size() == gtt::kChordSizeChoices);
    auto chord = std::make_unique<juce::AudioProcessorParameterGroup> ("chord", "Chord", "|");
    chord->addChild (std::make_unique<APC> (pid (ParamIDs::chordType), "Chord", juce::StringArray { "Off", "Freeze", "Harmonize" }, 0));
    chord->addChild (std::make_unique<APC> (pid (ParamIDs::chordSize), "Size", sizes, gtt::kDefaultChordSize));
    chord->addChild (std::make_unique<APC> (pid (ParamIDs::chordKey), "Key", keys, gtt::kDefaultChordKey));

    // ADVANCED (v0.7: debug only, not host parameters): Confidence and Hysteresis used instead of the fixed ones when Manual is on.
    // Defaults = the fixed values. In the layout only when the editor shows the panel (gtt::kShowAdvanced, false in every shipping
    // build; the snapshot tool builds a processor with it on), so the plugin's host parameters never include them.
    std::unique_ptr<juce::AudioProcessorParameterGroup> advanced;
    if (withAdvanced)
    {
        advanced = std::make_unique<juce::AudioProcessorParameterGroup> ("advanced", "ADVANCED", "|");
        advanced->addChild (std::make_unique<APB> (pid (ParamIDs::manual), "Manual", false));
        advanced->addChild (floatParam (ParamIDs::confidence, "Confidence Threshold", continuous (0.5f, 0.99f), gtt::kConfidence, "", 3));
        advanced->addChild (floatParam (ParamIDs::hysteresis, "Hysteresis", continuous (0.0f, 40.0f), gtt::kHysteresisCents, "cents", 1));
    }

    // Bypass is the host's bypass (v0.5.9: GTTProcessor::getBypassParameter; no control in the editor), at the top level (v0.7: the
    // "Output" group it sat in is gone).
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::move (latency), std::move (shape), std::move (voice), std::move (chord));
    if (advanced != nullptr)
        layout.add (std::move (advanced));
    layout.add (std::make_unique<APB> (pid (ParamIDs::bypass), "Bypass", false));
    return layout;
}

ParamPointers::ParamPointers (juce::AudioProcessorValueTreeState& s)
    : latency (s.getRawParameterValue (ParamIDs::latency)),
      retuneSpeed (s.getRawParameterValue (ParamIDs::retuneSpeed)),
      sustain (s.getRawParameterValue (ParamIDs::sustain)),
      release (s.getRawParameterValue (ParamIDs::release)),
      wave (s.getRawParameterValue (ParamIDs::wave)),
      octave (s.getRawParameterValue (ParamIDs::octave)),
      chordType (s.getRawParameterValue (ParamIDs::chordType)),
      chordSize (s.getRawParameterValue (ParamIDs::chordSize)),
      chordKey (s.getRawParameterValue (ParamIDs::chordKey)),
      manual (s.getRawParameterValue (ParamIDs::manual)),
      confidence (s.getRawParameterValue (ParamIDs::confidence)),
      hysteresis (s.getRawParameterValue (ParamIDs::hysteresis)),
      bypass (s.getRawParameterValue (ParamIDs::bypass))
{
}

void ParamPointers::read (gtt::EngineParams& p, bool showAdvanced) const
{
    auto b = [] (const std::atomic<float>* a) { return a->load (std::memory_order_relaxed) >= 0.5f; };
    auto f = [] (const std::atomic<float>* a) { return a->load (std::memory_order_relaxed); };
    auto i = [] (const std::atomic<float>* a) { return (int) std::lround (a->load (std::memory_order_relaxed)); };

    p.windowHz = f (latency);
    p.retuneSpeed = (gtt::RetuneSpeed) std::clamp (i (retuneSpeed), 0, 2);
    p.sustainPercent = f (sustain);
    p.dynamicsPercent = 100.0f * gtt::kDynamics;
    p.holdMs = gtt::noteReleaseMsFor ((gtt::NoteRelease) std::clamp (i (release), 0, 2));

    p.octave = (gtt::Octave) std::clamp (i (octave), 0, 3);

    p.wave = gtt::waveForChoice (i (wave));
    p.filterCutoffHz = gtt::kLowPassCutoffHz;

    const auto chord = gtt::chordChoiceForSplit (i (chordType), i (chordSize), i (chordKey)); // v0.6: as v0.5.11's Chord index
    p.chordMode = chord.mode;
    p.key = chord.key;
    p.triad = gtt::Triad::Major;
    p.memoryNotes = chord.memoryNotes;
    p.voicing = gtt::kVoicing;
    p.scale = gtt::kScale;
    p.chordLevelPercent = 100.0f * gtt::kChordLevel;

    // ADVANCED: only in a layout built with it (debug); otherwise Manual off and the fixed values.
    p.manual = manual != nullptr && gtt::manualInEffect (b (manual), showAdvanced);
    p.confidenceThreshold = confidence != nullptr ? f (confidence) : gtt::kConfidence;
    p.hysteresisCents = hysteresis != nullptr ? f (hysteresis) : gtt::kHysteresisCents;

    p.bypass = b (bypass);
}
