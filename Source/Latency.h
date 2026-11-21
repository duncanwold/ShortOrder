#pragma once

// v0.6: the "set your latency" knob's detents (docs/history/SPEC-v0.6.md section 3). Pure; no JUCE.
//
// 15 detents, one per semitone of lowest reliable note, G3 + k (k = 0 .. 14, G3 .. A4), on the `window` parameter (its ID, range and
// default unchanged): k = 0 is exactly its minimum (derivedWindowMinHz, 192.99 Hz), k = 4 exactly its default (defaultWindowHz,
// 240.62 Hz, B3), k = 14 exactly its top (kWindowMaxHz, 424.26 Hz); between them equal ratios per step (4 steps G3 .. B3, 10 steps
// B3 .. A4). The knob turns right toward k = 14 (lower latency). Test `latency-detents` checks every detent's lowest reliable note
// is G3 + k at every common rate (none needed nudging).

#include <cmath>
#include <cstdio>
#include <string>
#include "DSP/DspUtils.h"
#include "DSP/Macro.h"

namespace gtt
{
constexpr int kLatencyDetents = 15;
constexpr int kDefaultLatencyDetent = 4;              // B3, the parameter's default
constexpr int kLatencyLabelDetents[] = { 0, 4, 8, 12, 14 }; // the latency labels round the knob

// The detent's window (Hz), as the parameter stores it (float).
inline float latencyDetentHz (int k)
{
    const double lo = derivedWindowMinHz(), mid = (double) defaultWindowHz(), hi = kWindowMaxHz;
    if (k <= 0)
        return (float) lo;
    if (k == kDefaultLatencyDetent)
        return defaultWindowHz();
    if (k >= kLatencyDetents - 1)
        return (float) hi;
    if (k < kDefaultLatencyDetent)
        return (float) (lo * std::pow (mid / lo, (double) k / kDefaultLatencyDetent));
    return (float) (mid * std::pow (hi / mid, (double) (k - kDefaultLatencyDetent) / (kLatencyDetents - 1 - kDefaultLatencyDetent)));
}

// Where a window value sits on the knob, in detents (0 .. 14, continuous): the inverse of the detents' geometric steps, so a value
// a host has automated between detents shows at its true position.
inline double latencyKnobPosition (double hz)
{
    const double lo = derivedWindowMinHz(), mid = (double) defaultWindowHz(), hi = kWindowMaxHz;
    hz = clampWindow (hz);
    if (hz <= mid)
        return kDefaultLatencyDetent * std::log (hz / lo) / std::log (mid / lo);
    return kDefaultLatencyDetent + (kLatencyDetents - 1 - kDefaultLatencyDetent) * std::log (hz / mid) / std::log (hi / mid);
}

inline int nearestLatencyDetent (double hz)
{
    const int k = (int) std::lround (latencyKnobPosition (hz));
    return k < 0 ? 0 : k > kLatencyDetents - 1 ? kLatencyDetents - 1 : k;
}

// The knob's angle for a position (degrees, 0 = up, clockwise): the 270 degree sweep, -135 (k = 0) .. +135 (k = 14).
inline double latencyKnobAngle (double position) { return -135.0 + position * 270.0 / (kLatencyDetents - 1); }

// The real latency (ms): the voice's delay D / fs at that rate (voiceDelayFor, L / 2; 1.1: up to 1.0.x the voice, and so the
// readout and the host text, were a whole window L behind, twice this).
inline double latencyMs (double fs, double hz) { return 1000.0 * voiceDelayFor (fs, hz) / fs; }
// One decimal place, as the readout and the labels show it: "8.3".
inline std::string oneDecimalText (double ms)
{
    char buf[16];
    std::snprintf (buf, sizeof (buf), "%.1f", std::round (ms * 10.0) / 10.0);
    return buf;
}
inline std::string latencyMsText (double fs, double hz) { return oneDecimalText (latencyMs (fs, hz)); }

// v0.7: the host's text for the latency parameter: the faceplate's ms with the unit, "4.1 ms" at the default (48 kHz; 1.0.x: "8.3 ms"). The rate is
// the processor's; before one is known (0), 48 kHz, as the editor does.
constexpr double kHostTextFallbackRate = 48000.0;
inline double hostTextRate (double fs) { return fs > 0.0 ? fs : kHostTextFallbackRate; }
inline std::string latencyHostText (double fs, double hz) { return latencyMsText (hostTextRate (fs), hz) + " ms"; }

// Back from a typed ms value to the parameter's Hz: a value showing as a detent's text (at this rate) is that detent's value exactly,
// so the text round-trips at every detent; any other is the window 1000 / ms Hz (D = tauMax = that many ms, to the sample), within the range.
inline double latencyHzForMs (double fs, double ms)
{
    fs = hostTextRate (fs);
    const auto typed = oneDecimalText (ms);
    for (int k = 0; k < kLatencyDetents; ++k)
        if (latencyMsText (fs, latencyDetentHz (k)) == typed)
            return latencyDetentHz (k);
    return ms > 0.0 ? clampWindow (1000.0 / ms) : kWindowMaxHz;
}
} // namespace gtt
