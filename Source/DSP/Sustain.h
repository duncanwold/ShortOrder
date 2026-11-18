#pragma once

#include "DspUtils.h"
#include "NoteReference.h"

namespace gtt
{

// Sustain: how much of the guitar's fade the synth's level copies while its note is detected.
// The engine bypasses this at Sustain 0, where the level is the envelope follower's (v0.4.3).
//  - Reference: the note's level from NoteReference (the running peak of the fast level for the note,
//    restarting at a new note, and outside the Glide mode's inheritance window at a note change or an
//    onset; see NoteReference.h).
//  - Detected: out_dB = ref_dB + (1 - s) (in_dB - ref_dB), in = the envelope follower capped at the
//    reference (so a rise lifts the reference instead). 100% holds the reference.
//  - Pitch lost: a dB-linear fade from the current output level for Note Release (60 dB over it,
//    but never faster than 60 dB per 100 ms; at 0 there's none), while the voice gate holds the
//    pitch for the same time and then closes with its 15 ms fade. Pitch back before the gate has
//    closed (the same note, or an inherited one): back to the held level (same reference) over 5 ms.
//  - Every jump of the target (a restart while sounding, recovery, Sustain turned on or set to 0) is
//    crossed over 5 ms, the voice's fade-in time.
class SustainLevel
{
public:
    static constexpr double kFastestReleaseMs = 100.0; // per 60 dB
    static constexpr float kReleaseDepthDb = 60.0f;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        blendLen = msToSamples (fs, 5.0);
        reset();
    }

    void reset()
    {
        used = releasing = false;
        out = from = releaseGain = 0.0f;
        releaseFor = -1;
        cap = kNoCap;
        caps = 0;
        blend.snap (1.0f);
    }

    // One sample on the delayed timeline, after the reference's process() for it (the engine runs the
    // reference on every sample, so it's already the note's when Sustain comes on). env: the envelope
    // follower. amount: 0 < s <= 1. sinceLost: samples since pitch was lost (0 while voiced).
    float process (const NoteReference& reference, float env, float amount, bool voiced, int sinceLost, int releaseSamples)
    {
        if (! used) // Sustain just came on (or first use): from the guitar's level
        {
            used = true;
            releasing = false;
            out = env;
            if (reference.start() != NoteReference::Start::Fresh)
                startBlend();
        }

        float target;
        // fix 4: a jump of the reference at pitch's return (Start::Jump) is the same note: only the level is crossed (below)
        const auto start = reference.start() == NoteReference::Start::Jump ? NoteReference::Start::None : reference.start();
        if (start != NoteReference::Start::None)
            cap = kNoCap;
        if (voiced)
        {
            if (fromLevel && reference.start() == NoteReference::Start::Jump && ! (releasing && ! oneWay))
                startBlend(); // fix 4: from the level sounding to the new target (pitch back during a release blends anyway, below)
            if (start == NoteReference::Start::Restart)
                startBlend();
            else if (start == NoteReference::Start::None && releasing)
            {
                if (oneWay)
                {
                    if (fromLevel && blend.isSmoothing())
                        startBlend(); // v0.6.2's fix 4: the cap is the level now, so a blend under way restarts from it
                    cap = std::min (cap, out); // the one-way release: stay at the level the release reached
                    ++caps;
                }
                else
                    startBlend(); // pitch back during the release: to the held level
            }
            releasing = false;

            // The follower's level, but never above the reference: after a quieter note its 120 ms
            // release would otherwise keep the previous note's level for a few hundred ms.
            const float ref = reference.level();
            if (amount >= 1.0f || env >= ref)
                target = ref;
            else
                target = env > 0.0f ? ref * std::exp ((1.0f - amount) * std::log (env / ref)) : 0.0f;
            target = std::min (target, cap);
        }
        else
        {
            if (! releasing)
            {
                releasing = true;
                releaseGain = out;
                // v0.6.2's fix 4 (QA issue 12): the release starts from the level sounding now, so a 5 ms blend under way (a re-pick's
                // lift, pitch back) ends here. Up to v0.6.1 it carried on from the level it started at toward the release's, which
                // is the level now: a one-sample drop (0.8 dB at fingerpick 1.345 s; up to 5 dB where the release follows a lift).
                if (fromLevel)
                    blend.snap (1.0f);
            }
            if (sinceLost <= releaseSamples)
                releaseGain *= releaseStep (releaseSamples);
            target = releaseGain;
        }
        return out = blended (target);
    }

    // The one-way release (v0.5.5's sputter fix, part 2; the engine sets it in a note's tail): while on, pitch back after the
    // release has started stays at the level the release reached (no comeback to the held level) until a new note, a
    // restart or liftCap() (a re-pick), crossed over 5 ms. capped(): for tests.
    void setOneWay (bool on) { oneWay = on; }
    void setBlendFromLevel (bool on) { fromLevel = on; } // v0.6.2's fix 4 (tests: off is v0.6.1)
    bool capped() const { return used && cap < kNoCap; }
    int capsSet() const { return caps; } // tests only: recoveries the one-way release capped
    void liftCap()
    {
        if (cap < kNoCap)
        {
            cap = kNoCap;
            startBlend();
        }
    }

    // Sustain set to 0 while in use: back to the follower level over 5 ms.
    bool inUse() const { return used || blend.isSmoothing(); }
    float handBack (float env)
    {
        if (used)
        {
            used = false;
            startBlend();
        }
        return out = blended (env);
    }

private:
    void startBlend()
    {
        from = out;
        blend.snap (0.0f);
        blend.setTarget (1.0f, blendLen);
    }

    float blended (float target)
    {
        if (! blend.isSmoothing())
            return target;
        return from + blend.next() * (target - from);
    }

    // Per-sample gain of the dB-linear release.
    float releaseStep (int releaseSamples)
    {
        if (releaseSamples != releaseFor)
        {
            releaseFor = releaseSamples;
            const double len = std::max ((double) releaseSamples, fs * kFastestReleaseMs / 1000.0);
            step = (float) std::pow (10.0, -kReleaseDepthDb / 20.0 / len);
        }
        return step;
    }

    double fs = 48000.0;
    int blendLen = 1;

    static constexpr float kNoCap = 1.0e9f;
    bool used = false, releasing = false, oneWay = false, fromLevel = true;
    float cap = kNoCap;
    int caps = 0;
    float out = 0.0f, from = 0.0f, releaseGain = 0.0f, step = 1.0f;
    int releaseFor = -1;
    LinearRamp blend;
};

} // namespace gtt
