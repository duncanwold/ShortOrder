#pragma once

#include "DspUtils.h"
#include "Oscillator.h"
#include <array>

namespace gtt
{

// Per-block voice settings.
struct VoiceSettings
{
    Wave wave = Wave::Saw;
    Octave octave = Octave::Main;   // voice = snapped note x 1, 1/2, 1/4, 1/8
    float filterCutoffHz = 5000.0f; // 200..8000, fixed (no envelope)
};

// One oscillator voice: the note times the octave ratio, the one-pole LPF.
// Saw/square/triangle are band-limited (Oscillator.h) and pass a one-pole LPF at a fixed cutoff (Low
// Pass Filter, 5 kHz default, as v0.1); sine is plain. Plain data (no heap), so the chord engine can
// copy the playing tone into another slot mid-note (ChordVoice) without allocating.
class Tone
{
public:
    void prepare (double sampleRate);
    void reset();

    // Call once per block.
    void setVoice (const VoiceSettings& v);

    // targetMidi: T (may be fractional while gliding); level: the amplitude (envelope).
    float render (float targetMidi, float level);

    // Folded (v0.5.8): the fold's shape (its drive). The engine shares one, set each sample from the note's level, with the
    // live tone (shareFold), and every copy of it (the chord's tones) shares it too, so they all take the
    // live note's drive. Without one (a Tone on its own, in tests) it's the tone's own, setFoldDrive's (1: a sine).
    void shareFold (const FoldShape* shape) { sharedFold = shape; }
    void setFoldDrive (double drive) { ownFold.set (drive); }
    const FoldShape& foldShape() const { return sharedFold != nullptr ? *sharedFold : ownFold; }

    // The most the last render() could have returned in size, from its gains alone: level x the wave's
    // scale. Every waveform stays within +/-1 and the one-pole LPF can't raise a peak, so it's a bound.
    // The output stage's headroom gain works from it.
    float peak() const { return lastPeak; }

private:
    float filterCoef();

    double fs = 48000.0;
    float lpCoef = 0.0f;              // at the (possibly ramping) cutoff
    LinearRamp cutoffHz;
    float cachedCutoff = -1.0f;

    // The oscillator. Phase-continuous: note and octave changes only change dt.
    double phase = 0.0;
    FoldOversampler fold; // Folded's doubled rate
    const FoldShape* sharedFold = nullptr;
    FoldShape ownFold;
    float lpY = 0.0f;
    Wave wave = Wave::Saw;
    float scale = 2.0f; // waveScale (wave), once per block
    double ratio = 1.0;

    bool firstSettings = true;
    int rampLen = 1;
    float lastTarget = -1.0f;
    double lastRatio = 1.0;
    double dt = 0.0;
    float lastPeak = 0.0f;
};

// The synth voice: one Tone driven by an envelope follower on the (L-delayed) guitar, at full level.
// At default voice settings this is v0.1's (and v0.2's and v0.3's) code path, sample for sample.
class SynthSource
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    // Call once per block before process().
    void setVoice (const VoiceSettings& v) { voice.setVoice (v); }

    // delayedIn: the guitar on the L-delayed timeline. targetMidi: T (may be fractional
    // while gliding). periodSamples: the (held) input period at this point of the timeline.
    float process (float delayedIn, float targetMidi, float periodSamples) { return render (targetMidi, follow (delayedIn, periodSamples)); }

    // process() in two steps, so the engine can shape the level in between (Sustain):
    // the envelope follower's level for this sample, then the oscillator at a level.
    float follow (float delayedIn, float periodSamples);
    float render (float targetMidi, float level) { return voice.render (targetMidi, level); }
    float meanAbsLevel() const { return meanAbs; } // follow()'s input: the last one-period mean |x|

    // The played note's oscillator (the chord engine copies it into its other tones).
    const Tone& tone() const { return voice; }
    void shareFold (const FoldShape* shape) { voice.shareFold (shape); } // Folded's drive (Tone::shareFold)

private:
    double fs = 48000.0;
    float attack = 0.0f, release = 0.0f, env = 0.0f;
    Tone voice;

    // One-period boxcar of |x|: the guitar's mean-absolute level without per-cycle ripple.
    std::vector<float> absRing;
    int absMask = 0, absWrite = 0, boxLen = 1;
    double boxSum = 0.0;
    float meanAbs = 0.0f;
};

} // namespace gtt
