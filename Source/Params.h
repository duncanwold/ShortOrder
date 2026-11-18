#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP/Engine.h"

namespace ParamIDs
{
// v0.7: the IDs frozen at 1.0 (RELEASE-BRIEF.md, "Parameter contract (frozen at 1.0)"): after 1.0 they can't change without breaking
// users' sessions. Host order = declaration order (Params.cpp): latency, sustain, retuneSpeed, release, wave, octave, chordType,
// chordSize, chordKey, bypass. Up to v0.6.3 latency was "window", release "hold" and wave "mainWave".
// LATENCY
inline constexpr const char* latency = "latency"; // named "Latency": the detector's window in Hz (shown as ms, as the faceplate's knob shows it)
// NOTE SHAPE
inline constexpr const char* sustain = "sustain";
inline constexpr const char* retuneSpeed = "retuneSpeed"; // named "Retune"
inline constexpr const char* release = "release"; // named "Release": Fast / Med / Long = 0 / 500 / 1500 ms (v0.5.9)
// VOICE (v0.5.4: PITCH and TONE merged)
inline constexpr const char* wave = "wave"; // named "Wave"
inline constexpr const char* octave = "octave"; // 0, -1, -2, -3 octaves
// CHORD
// v0.6: three parameters in place of v0.5.6 ... v0.5.11's chordMode (gtt::legacyChordIndexFor, ChordSplit.h, reads them as that index).
inline constexpr const char* chordType = "chordType"; // named "Chord": Off, Freeze, Harmonize
inline constexpr const char* chordSize = "chordSize"; // named "Size": 2 / 3 / 4 notes (Freeze)
inline constexpr const char* chordKey = "chordKey";   // named "Key": the twelve major / relative-minor pairs (Harmonize)
// ADVANCED (v0.7: debug only; in the layout only when the panel is shown, gtt::kShowAdvanced, so never host parameters in a shipping build)
inline constexpr const char* manual = "manual"; // overrides the fixed Confidence and Hysteresis (only while the panel is shown)
inline constexpr const char* confidence = "confidence";
inline constexpr const char* hysteresis = "hysteresis";
// The host's bypass (v0.5.9: GTTProcessor::getBypassParameter), not in the editor; top level (v0.7)
inline constexpr const char* bypass = "bypass";
// Removed in v0.5.2, now fixed in code (gtt::kDynamics, kVoicing, kScale, kChordLevel; Glide Always, the only mode since v0.6.3):
// glideMode, dynamics, voicing, scale, chordLevel.
// Removed in v0.5.4: filterCutoff (gtt::kLowPassCutoffHz), triad and key (merged into chordMode).
// Removed in v0.5.6: memoryNotes (merged into chordMode: Freeze 2 / 3 / 4).
// Removed in v0.6: chordMode (split into chordType, chordSize, chordKey).
// Removed in v0.6.3: transpose, and the hidden attack, thickness, chordArp and output.
// v0.7: window, hold and mainWave renamed (latency, release, wave); the "output" group removed.
} // namespace ParamIDs

// Which hidden parts the editor shows: the compile-time flags (gtt::kShowAdvanced in Engine.h, kShowTrace here) by default. The
// parameter layout is the same either way, except that the ADVANCED parameters exist only with `advanced` on (v0.7); the snapshot
// tool builds an editor with every flag on to check where each part lands.
namespace gtt
{
// v0.6: the pitch trace (v0.5.11) is hidden: the editor doesn't build it, never reads TraceFifo, so the engine pushes nothing.
// The snapshot tool builds it with the flag on (ShowFlags::trace) so it doesn't rot.
constexpr bool kShowTrace = false;
} // namespace gtt

struct ShowFlags
{
    bool advanced = gtt::kShowAdvanced,
         trace = gtt::kShowTrace; // v0.6: the pitch trace (editor only)
    static ShowFlags all() { return { true, true }; }
};

// withAdvanced: add the ADVANCED group (manual, confidence, hysteresis); only a processor whose editor shows the panel does.
// sampleRate: the rate the latency parameter's text is in ms at (the processor's; none or 0: 48 kHz).
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout (bool withAdvanced = gtt::kShowAdvanced,
                                                                           std::function<double()> sampleRate = {});

// Cached raw-value atomics; read once per block into a plain EngineParams. Manual comes out as the value in
// effect (off while hidden, and off with the fixed Confidence and Hysteresis when the layout has no ADVANCED group); the show
// flags are for tests. The settings fixed in v0.5.2 (Glide,
// Dynamics, Voicing, Scale, Chord Level) and v0.5.4 (the Low Pass Filter) come out as their fixed values,
// and Chord, Chord Size and Key (v0.6) as v0.5.11's Chord choice: the mode, key and (Freeze) Memory Notes.
struct ParamPointers
{
    explicit ParamPointers (juce::AudioProcessorValueTreeState& apvts);
    void read (gtt::EngineParams& p, bool showAdvanced = gtt::kShowAdvanced) const;

    std::atomic<float>* latency;
    std::atomic<float>* retuneSpeed;
    std::atomic<float>* sustain;
    std::atomic<float>* release;
    std::atomic<float>* wave;
    std::atomic<float>* octave;
    std::atomic<float>* chordType;
    std::atomic<float>* chordSize;
    std::atomic<float>* chordKey;
    std::atomic<float>* manual;     // nullptr unless the layout has the ADVANCED group
    std::atomic<float>* confidence; // (as manual)
    std::atomic<float>* hysteresis; // (as manual)
    std::atomic<float>* bypass;
};
