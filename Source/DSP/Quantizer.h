#pragma once

#include "Macro.h"

namespace gtt
{

// What the engine knows about the voice when a hop arrives (beyond the detector's estimate).
struct NoteContext
{
    bool voiceOpen = false;   // the voice gate won't have fully closed when this hop's entry plays: a note arriving glides in from the held pitch
    bool onset = false;       // an onset was found since the last hop
    int unvoicedHops = 0;     // tests only (investigate-blips): unvoiced hops just before this one
    // v0.5.11, the pick glide: a pick arriving within kPickGlideWindowMs of the last real note (the engine's), whose pitch
    // (T then) it glides in from even if the voice has closed in between.
    bool pickGlide = false;
    double pickFromT = 0.0;
};

// Tests only (investigate-blips): what a pick may do to the glide. All off: the quantizer as it is.
struct PickRules
{
    float freshAfterGapMs = -1.0f;  // >= 0: latch fresh (no glide in) when a pick's note arrives after at least this long unvoiced
    float snapArrivalSemis = 0.0f;  // > 0: a note's arrival (the Always latch, or the first change after an onset) snaps if it's this far
    float snapArrivalMs = 25.0f;    //      from T, within this long of the onset
    int stableHops = 0;             // > 0: after an onset, T holds until N has been the same for this many hops, then snaps to it
};

// Chromatic quantizer, updated once per detector hop.
//  - N changes only when |midi - N| > 0.5 + hysteresis/100; first voiced hop after being lost: N = round(midi).
//  - T glides to N with a one-pole (time constant = retune) in semitones; retune 0 = instant.
//    The glide (Always, the only mode since v0.6.3): a note after the voice gate has fully closed latches fresh (T = N); one
//    arriving while it's open glides in from the held T. v0.5.11: so does a pick
//    arriving within kPickGlideWindowMs of the last real note (NoteContext::pickGlide), from that note's T, whether the gate
//    has closed or not.
//  - Onset snap: within kOnsetSnapMs of a note's start (a fresh latch, an Always latch, an onset), a
//    change of N by kOnsetSnapSemitones or more snaps T to N. The note's arrival (the Always latch,
//    or the first change after an onset) is its glide and never snaps; only a second jump does.
//  - r = 2^((T - midi) / 12).
// When unvoiced, N, T and r are frozen (the Tune path decides what to do with them).
class Quantizer
{
public:
    void reset()
    {
        hasNoteFlag = false;
        wasVoiced = false;
        arrivalPending = false;
        sinceStart = kLong;
        N = 57;
        T = 57.0;
        r = 1.0;
        lastMidi = 57.0;
        sinceOnset = kLong;
        stableCount = 0;
        holdingForStable = false;
    }
    PickRules rules; // tests only

    void update (bool voiced, double midi, float hysteresisCents, float retuneMs, double hopSeconds, const NoteContext& c = {})
    {
        if (c.onset)
        {
            sinceStart = 0.0; // a pick: a note starts here, and its first change is its arrival
            arrivalPending = true;
            sinceOnset = 0.0;
            holdingForStable = rules.stableHops > 0;
            stableCount = 0;
        }
        const double heldT = T;
        const bool nearOnset = sinceOnset < rules.snapArrivalMs * 0.001;
        if (voiced)
        {
            lastEvent = Event::None;
            const bool freshPick = rules.freshAfterGapMs >= 0.0f && sinceOnset < 0.1 && ! wasVoiced
                                   && c.unvoicedHops * hopSeconds >= rules.freshAfterGapMs * 0.001 - 1.0e-9;
            const bool glideIn = c.voiceOpen && ! freshPick;
            // v0.5.11: a pick following a real note glides in from that note, whether the voice has closed (it would latch fresh) or not
            // (the held T may have moved toward the damp's junk since).
            const bool pickIn = c.pickGlide && ! freshPick;
            if (! hasNoteFlag || (! wasVoiced && ! glideIn && ! pickIn))
            {
                lastEvent = Event::FreshLatch;
                N = (int) std::lround (midi);
                T = N; // fresh note: no glide in from the previous one
                hasNoteFlag = true;
                sinceStart = 0.0;
                arrivalPending = false;
            }
            else if (! wasVoiced)
            {
                lastEvent = pickIn ? Event::PickGlide : Event::AlwaysLatch;
                if (pickIn)
                    T = c.pickFromT; // from the last real note's pitch (T, held since, may have moved on with nothing to play)
                N = (int) std::lround (midi); // Always, the voice still open: T glides in from where it's held
                if (rules.snapArrivalSemis > 0.0f && arrivalPending && nearOnset && std::abs (N - T) >= rules.snapArrivalSemis)
                {
                    T = N;
                    lastEvent = Event::Snap;
                }
                sinceStart = 0.0;
                arrivalPending = false;
            }
            else if (std::abs (midi - N) > 0.5 + hysteresisCents / 100.0)
            {
                const int next = (int) std::lround (midi);
                bool snap = ! arrivalPending && sinceStart < kOnsetSnapMs * 0.001 && std::abs (next - N) >= kOnsetSnapSemitones;
                if (rules.snapArrivalSemis > 0.0f && arrivalPending && nearOnset && std::abs (next - T) >= rules.snapArrivalSemis)
                    snap = true;
                lastEvent = snap ? Event::Snap : (arrivalPending ? Event::Arrival : Event::Change);
                arrivalPending = false;
                N = next;
                if (snap)
                    T = N;
            }

            if (lastEvent == Event::FreshLatch)
                holdingForStable = false; // from silence: nothing to hold
            if (holdingForStable)
            {
                stableCount = N == stableNote ? stableCount + 1 : 1;
                stableNote = N;
                if (stableCount >= rules.stableHops)
                {
                    T = N;
                    holdingForStable = false;
                }
                else
                {
                    T = heldT;
                }
            }
            else if (retuneMs <= 0.0f)
                T = N;
            else
                T += (N - T) * (1.0 - std::exp (-hopSeconds / (retuneMs * 0.001)));

            lastMidi = midi;
            r = std::clamp (std::pow (2.0, (T - midi) / 12.0), 0.5, 2.0);
        }
        if (! voiced)
            lastEvent = Event::None;
        wasVoiced = voiced;
        if (sinceOnset < kLong)
            sinceOnset += hopSeconds;
        if (sinceStart < kLong)
            sinceStart += hopSeconds;
    }

    // v0.6.2's fix 2 (Macro.h, kGlideToRealMinCents): an unvoiced hop with the voice open, after pitch was lost mid-glide: N becomes
    // the last real note and T glides on to it at Retune Speed (instead of staying frozen where the glide stopped).
    void glideToReal (int note, float retuneMs, double hopSeconds)
    {
        N = note;
        if (retuneMs <= 0.0f)
            T = N;
        else
            T += (N - T) * (1.0 - std::exp (-hopSeconds / (retuneMs * 0.001)));
        r = std::clamp (std::pow (2.0, (T - lastMidi) / 12.0), 0.5, 2.0);
    }

    // Tests only (investigate-blips): what the last update did to N.
    enum class Event { None, FreshLatch, AlwaysLatch, Arrival, Change, Snap, PickGlide };
    Event event() const { return lastEvent; }
    bool hasNote() const { return hasNoteFlag; }
    int note() const { return N; }
    double target() const { return T; }
    double ratio() const { return r; }
    double midi() const { return lastMidi; }

private:
    static constexpr double kLong = 1.0e9;
    bool hasNoteFlag = false, wasVoiced = false, arrivalPending = false;
    double sinceStart = kLong; // seconds since the current note's start (hop time)
    int N = 57;
    double T = 57.0, r = 1.0, lastMidi = 57.0;
    Event lastEvent = Event::None;
    double sinceOnset = kLong;
    int stableCount = 0, stableNote = -1;
    bool holdingForStable = false;
};

} // namespace gtt
