#pragma once

#include "DspUtils.h"

namespace gtt
{

// The note's level: Sustain's reference (and, from v0.5.1, what Dynamics evens out). The running peak
// of the OnsetDetector's fast level for the current note (the envelope follower's 3 ms attack, a 10 ms
// release; the envelope follower's own 120 ms release would still show the previous note's level when
// a quieter one starts).
//  - It restarts at that level at a new note (the voice gate had fully closed), and outside the
//    inheritance window below at a note change or an onset. After a restart it follows the level both
//    ways for 30 ms (the new note settling, past the pick's transient), then only rises.
//  - Inheritance: inside the window where Retune glides, a note change keeps the reference, and so does
//    an onset (the level carries on; a louder note can still raise it): any note while the voice gate is
//    open, plucks included; only a note after the gate has fully closed restarts. It depends on the gate,
//    not on Retune Speed.
//  - Pitch lost (v0.5.4): the running peak carries on, so a louder sound the detector can't pitch (a strum, a
//    note below the window) raises the reference at once, as a louder note does, and Dynamics doesn't boost it
//    by the quiet note's gain. (Up to v0.5.3 it was frozen while pitch was lost: setLegacyHold, tests only.)
// The engine runs it on every sample (it never changes the output on its own), so Sustain or Dynamics
// coming on mid-note gets the note's own level.
class NoteReference
{
public:
    static constexpr double kSettleMs = 30.0;
    static constexpr double kLostSmoothingMs = 2.0; // v0.5.4: the level that raises the reference while pitch is lost

    enum class Start
    {
        None,    // the reference carries on
        Fresh,   // a new note (after the gate closed, or the first one): no crossfade needed
        Restart, // a jump while sounding: users cross it over 5 ms
        Jump     // v0.6.2's fix 4: pitch back after a gap moved the reference itself in one sample (the running peak meeting the fast
                 // level, or a settle still under way): users cross the level over 5 ms; nothing else about the note changes
    };

    void prepare (double fs)
    {
        settleLen = msToSamples (fs, kSettleMs);
        lostCoef = timeConstantCoef (fs, kLostSmoothingMs);
        reset();
    }

    void reset()
    {
        active = gap = onsetPending = false;
        settle = 0;
        started = Start::None;
        ref = lost = 0.0f;
        lastNote = 0;
    }

    // One sample on the delayed timeline. fast, onset: the OnsetDetector's level and onset for this sample.
    // voiceWasClosed: the voice gate had fully closed (so this is a new note).
    void process (float fast, bool onset, bool voiced, bool voiceWasClosed, int note)
    {
        started = Start::None;
        onsetPending = onsetPending || onset;
        if (! voiced)
        {
            // The fast level is measured over one period of the last detected note, which on a sound with no (or
            // another) period ripples; smoothed over 2 ms, its peaks overstate the sound's level less (by about 1 dB on
            // a low note, 3.5 dB on a strum, against 2.5 / 4.4 dB unsmoothed), and the reference still rises within
            // the burst's first milliseconds.
            lost = gap ? lost + lostCoef * (fast - lost) : fast;
            gap = true;
            if (active && lost > ref && ! legacyHold)
                ref = lost;
            return;
        }
        const bool back = gap;   // pitch back after a gap (fix 4)
        const float before = ref;
        if (! active || voiceWasClosed)
        {
            restart (fast);
            active = true;
            started = Start::Fresh;
        }
        else if (gap && restartOnOnset && onsetPending)
        {
            restart (fast);
            started = Start::Restart;
        }
        onsetPending = gap = false;
        lastNote = note;

        if (settle > 0)
        {
            --settle;
            ref = fast;
        }
        else
        {
            ref = std::max (ref, fast);
        }
        if (reportJumps && back && started == Start::None && before > 0.0f && std::abs (std::log (ref / before)) > kJumpLog)
            started = Start::Jump;
    }

    float level() const { return ref; }
    Start start() const { return started; } // what this sample's process() did
    bool running() const { return active; }
    void setLegacyHold (bool on) { legacyHold = on; } // tests only: frozen while pitch is lost, as up to v0.5.3
    void setRestartOnOnset (bool on) { restartOnOnset = on; } // tests only (investigate-sputter): an onset in a gap restarts
    void setReportJumps (bool on) { reportJumps = on; } // v0.6.2's fix 4 (off: v0.6.1, no Start::Jump)

private:
    void restart (float fast)
    {
        ref = fast;
        settle = settleLen;
    }

    int settleLen = 1, settle = 0, lastNote = 0;
    bool active = false, gap = false, onsetPending = false, legacyHold = false, restartOnOnset = false, reportJumps = true;
    static constexpr float kJumpLog = 0.0115f; // 0.1 dB
    float ref = 0.0f, lost = 0.0f, lostCoef = 1.0f;
    Start started = Start::None;
};

} // namespace gtt
