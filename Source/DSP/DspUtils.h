#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace gtt
{

// ---------------------------------------------------------------------------
// Spec constants
// ---------------------------------------------------------------------------
constexpr double kA4Hz = 440.0;
constexpr double kDetectorMaxHz = 1400.0;
constexpr double kWindowMaxHz = 424.26;   // 300 Hz x 2^(6/12): six frets above v0.4's top; lowest reliable A4, 4.7 ms
constexpr double kV04WindowMaxHz = 300.0; // v0.4 .. v0.4.3's top; the default window is still the middle of that range
constexpr double kMaxPlannedSampleRate = 96000.0;            // preallocate for at least this
constexpr float kHoldMaxMs = 2000.0f;        // the engine's clamp on Hold (Note Release); unchanged since v0.3
constexpr double kSubsonicHz = 30.0;        // the readout flags a voice below this

// The values are the engine's own; the Wave parameter lists them in its own order (waveForChoice in Macro.h). 5 was
// Pulse 12.5% (removed in v0.6.3).
enum class Wave { Saw = 0, Square = 1, Triangle = 2, Sine = 3, Pulse25 = 4, HalfSine = 6, Organ = 7, Folded = 8 };
enum class Octave { Main = 0, Sub = 1, DoubleSub = 2, TripleSub = 3 };
enum class RetuneSpeed { Fast = 0, Medium = 1, Slow = 2 };
// Note Release's options (v0.5.9: Fast 0, Medium 500, Long 1500 ms; noteReleaseMsFor). NoteReleaseV058 is v0.5.6 .. v0.5.8's five,
// kept for the tests (the sputter fix and Freeze's clear were measured at those times; the engine takes any time up to kHoldMaxMs).
enum class NoteRelease { Fast = 0, Medium = 1, Long = 2 };
enum class NoteReleaseV058 { Instant = 0, Fast = 1, Medium = 2, Long = 3, ExtraLong = 4 };
// The wave levels (Oscillator.h's waveScale). The engine plays Current; the tests also ask for V051 (v0.5.1's,
// without v0.5.2's +10 dB on Triangle and Sine) and MeanAbs (v0.1 .. v0.5's, before the loudness match).
enum class WaveLevels { Current = 0, V051 = 1, MeanAbs = 2 };

// 1, 1/2, 1/4, 1/8: exact powers of two, so the octaves are exactly in tune with the note.
inline double octaveRatio (Octave o) { return 1.0 / (double) (1 << std::clamp ((int) o, 0, 3)); }

inline int hopFor (double fs) { return std::max (16, (int) std::lround (128.0 * fs / 48000.0)); }

// What a window can track. Measured offline (tests: probe-reliable): saws and plucks
// track steadily from the window's own frequency (fs / tauMax) upward and fail below it.
// "Lowest reliable" leaves a flat margin so a note can be played slightly flat (or with
// slide vibrato) without dropping out.
constexpr double kReliableFlatMarginCents = 25.0;
inline double trackableFloorHz (double fs, double windowHz) { return fs / std::lround (fs / windowHz); }
inline int lowestReliableNote (double fs, double windowHz)
{
    const double floorMidi = 69.0 + 12.0 * std::log2 (trackableFloorHz (fs, windowHz) / kA4Hz);
    return (int) std::ceil (floorMidi + kReliableFlatMarginCents / 100.0 - 1.0e-9);
}

// The playable zone's lowest Window setting, derived from the rule above: the highest window
// (lowest latency) on a 0.01 Hz grid at which the lowest reliable note is G3 (the open G
// string) at every common sample rate. 192.99 Hz; 10.4 ms of latency at 48 kHz.
constexpr int kPlayableLowestNote = 55; // G3
// The highest window (on a 0.01 Hz grid, from 300 Hz down) whose lowest reliable note is lowestNote or lower at every
// common sample rate.
inline double windowForLowestNote (int lowestNote)
{
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    for (int k = 30000; k > 0; --k)
    {
        const double w = k / 100.0;
        bool all = true;
        for (double fs : rates)
            all = all && lowestReliableNote (fs, w) <= lowestNote;
        if (all)
            return w;
    }
    return 55.0;
}
inline double derivedWindowMinHz()
{
    static const double hz = windowForLowestNote (kPlayableLowestNote);
    return hz;
}

// The low-range display detector's window (v0.5.4, editor side only; LowRange.h): lowest reliable note C2, by
// the same rule.
constexpr int kLowRangeLowestNote = 36; // C2
inline double lowRangeWindowHz()
{
    static const double hz = windowForLowestNote (kLowRangeLowestNote);
    return hz;
}

// The Window range is the playable zone: derivedWindowMinHz() .. kWindowMaxHz.
// tauMax = round(fs / windowHz); pipeline latency L = 2 * tauMax.
inline double clampWindow (double hz) { return std::clamp (hz, derivedWindowMinHz(), kWindowMaxHz); }
inline int tauMaxFor (double fs, double windowHz) { return (int) std::lround (fs / clampWindow (windowHz)); }
inline int latencyFor (double fs, double windowHz) { return 2 * tauMaxFor (fs, windowHz); }
// Buffers are sized for the longest window at max(fs, 96 kHz). minWindowHz below the playable
// zone is only for the offline tests' measuring detector.
inline int maxTauFor (double fs, double minWindowHz = derivedWindowMinHz())
{
    return (int) std::lround (std::max (fs, kMaxPlannedSampleRate) / minWindowHz);
}
inline int nearestNote (double hz) { return (int) std::lround (69.0 + 12.0 * std::log2 (hz / kA4Hz)); }

inline float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }
inline double midiFromHz (double hz) { return 69.0 + 12.0 * std::log2 (hz / kA4Hz); }
inline double hzFromMidi (double midi) { return kA4Hz * std::pow (2.0, (midi - 69.0) / 12.0); }
inline int msToSamples (double fs, double ms) { return std::max (1, (int) std::lround (fs * ms / 1000.0)); }
inline int holdSamplesFor (double fs, float holdMs) { return (int) std::lround (std::clamp (holdMs, 0.0f, kHoldMaxMs) * 0.001 * fs); }
constexpr double kCommonSampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
inline float onePoleCoef (double fs, double cutoffHz) { return (float) (1.0 - std::exp (-2.0 * M_PI * cutoffHz / fs)); }
inline float timeConstantCoef (double fs, double ms) { return (float) (1.0 - std::exp (-1000.0 / (ms * fs))); }

inline int nextPow2 (int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

// Safety clip: linear below +/-0.9, tanh knee above (continuous slope, ceiling 1.0).
inline float safetyClip (float x)
{
    const float a = std::abs (x);
    if (a <= 0.9f)
        return x;
    const float y = 0.9f + 0.1f * std::tanh ((a - 0.9f) / 0.1f);
    return x < 0.0f ? -y : y;
}

// ---------------------------------------------------------------------------
// Linear ramp (per-sample smoother). Ramp length can differ per target change,
// which is how asymmetric fades (e.g. 15 ms out / 5 ms in) are done.
// ---------------------------------------------------------------------------
struct LinearRamp
{
    float current = 0.0f, target = 0.0f, step = 0.0f;
    int remaining = 0;

    void snap (float v) { current = target = v; step = 0.0f; remaining = 0; }

    void setTarget (float t, int rampSamples)
    {
        if (std::equal_to<float>() (t, target))
            return;
        target = t;
        remaining = std::max (1, rampSamples);
        step = (target - current) / (float) remaining;
    }

    float next()
    {
        if (remaining > 0)
        {
            current += step;
            if (--remaining == 0)
                current = target;
        }
        return current;
    }

    bool isSmoothing() const { return remaining > 0; }
};

// ---------------------------------------------------------------------------
// Mono history ring addressed by absolute sample index (int64). Capacity is a
// power of two. Negative indices read zeros from the cleared buffer.
// ---------------------------------------------------------------------------
class History
{
public:
    void prepare (int minCapacity)
    {
        buf.assign ((size_t) nextPow2 (minCapacity), 0.0f);
        mask = (int64_t) buf.size() - 1;
        count = 0;
    }

    void reset()
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        count = 0;
    }

    void push (float x) { buf[(size_t) (count++ & mask)] = x; }

    // Index of the newest sample written.
    int64_t newest() const { return count - 1; }
    int64_t size() const { return count; }

    float at (int64_t idx) const { return buf[(size_t) (idx & mask)]; }

    float atFrac (double pos) const
    {
        const double fl = std::floor (pos);
        const auto i = (int64_t) fl;
        const float frac = (float) (pos - fl);
        const float a = at (i), b = at (i + 1);
        return a + frac * (b - a);
    }

    int capacity() const { return (int) buf.size(); }

private:
    std::vector<float> buf;
    int64_t mask = 0;
    int64_t count = 0;
};

} // namespace gtt
