#pragma once

#include "DspUtils.h"
#include <array>

namespace gtt
{

// One per detector hop, stamped with the absolute sample time at the centre of
// its analysis window. period/ratio/target are the values the Tune path should
// use at that time; when unvoiced they hold the last voiced values (Hold).
struct PitchEntry
{
    int64_t time = 0;
    bool voiced = false;
    bool hasNote = false;   // a note has been latched at least once
    float period = 0.0f;    // samples (held)
    float ratio = 1.0f;     // shift ratio r (held)
    float target = 57.0f;   // T in MIDI semitones (held)
    int note = 57;          // the quantizer's note N (held)
    bool pick = false;      // a real pick at this hop (an onset rising kReviveWakeRiseDb above the quietest since pitch was lost)
    bool muted = false;     // the sputter fix's mute gate is on at this hop, so the voice skips the grace (TailSwitches::muteSkipsGrace)
    float inMidi = -1.0f;   // v0.5.11, display only (the pitch trace): the detector's raw pitch (MIDI) where the hop is voiced, else -1
    float inConfidence = 0.0f;
};

// Small FIFO of per-hop estimates so the delayed Tune path can look up the
// estimate for the exact sample it is processing.
class PitchTimeline
{
public:
    static constexpr int kCapacity = 128;

    void reset (const PitchEntry& initial)
    {
        count = 0;
        head = 0;
        fallback = initial;
    }

    void push (const PitchEntry& e)
    {
        head = (head + 1) % kCapacity;
        entries[(size_t) head] = e;
        count = std::min (count + 1, kCapacity);
    }

    // Latest entry whose time <= t. Before the oldest entry: the oldest. Empty: fallback.
    const PitchEntry& lookup (int64_t t) const
    {
        if (count == 0)
            return fallback;
        int idx = head;
        for (int k = 0; k < count; ++k)
        {
            const auto& e = entries[(size_t) idx];
            if (e.time <= t)
                return e;
            if (k + 1 < count)
                idx = (idx - 1 + kCapacity) % kCapacity;
        }
        return entries[(size_t) idx];
    }

    const PitchEntry& newest() const { return count == 0 ? fallback : entries[(size_t) head]; }
    bool empty() const { return count == 0; }

private:
    std::array<PitchEntry, kCapacity> entries {};
    int head = 0, count = 0;
    PitchEntry fallback;
};

} // namespace gtt
