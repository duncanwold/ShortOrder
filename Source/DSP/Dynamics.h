#pragma once

#include "DspUtils.h"
#include "NoteReference.h"

namespace gtt
{

constexpr float kDynamicsTargetDb = -18.0f;       // T: the note level (NoteReference, mean |x| in dBFS) Dynamics 0% brings every note to
constexpr float kDynamicsMaxBoostDb = 18.0f;      // the most a quiet note (or noise) is raised

// Dynamics: a per-note gain that narrows the range of note levels, not a running compressor. A note's
// level is its reference (NoteReference: the fast level's running peak for the note, settling 30 ms
// after a restart, then only rising, and kept through inherited notes). With d = Dynamics / 100:
//   gain_dB = (T + d (ref_dB - T)) - ref_dB = (1 - d)(T - ref_dB), at most +kDynamicsMaxBoostDb.
// So 100% is unity (the engine bypasses it), 0% puts every note at T, 50% halves the dB between notes.
// v0.5.2 fixes it at 50% (kDynamics, Macro.h); other values are for tests.
// The gain follows the reference directly (it's already smooth: one-pole followers, a running peak), so
// it's constant through a note once the reference has settled, and only moves when it rises. A restart
// while sounding (a jump of the reference) is crossed over 5 ms. Dynamics itself is smoothed over 20 ms.
// It comes after the follower and Sustain, so they still shape the note.
class DynamicsGain
{
public:
    void prepare (double fs)
    {
        blendLen = msToSamples (fs, 5.0);
        amountLen = msToSamples (fs, 20.0);
        reset();
    }

    void reset()
    {
        amount.snap (1.0f);
        blend.snap (1.0f);
        g = from = 1.0f;
    }

    // v0.6.2's fix 4: a jump of the reference at pitch's return (NoteReference::Start::Jump) is crossed over 5 ms too (off: v0.6.1).
    void setBlendJumps (bool on) { blendJumps = on; }

    // In use: Dynamics below 100%, or still on its way back there.
    bool active (float target) const { return target < 1.0f || amount.isSmoothing() || amount.current < 1.0f; }

    // One sample, after the reference's process() for it. target: Dynamics, 0..1.
    float process (const NoteReference& reference, float target)
    {
        amount.setTarget (target, amountLen);
        const float d = amount.next();
        if (reference.start() == NoteReference::Start::Restart || (reference.start() == NoteReference::Start::Jump && blendJumps))
        {
            from = g;
            blend.snap (0.0f);
            blend.setTarget (1.0f, blendLen);
        }
        if (! reference.running()) // no note yet: unity until one starts
        {
            g = 1.0f;
            return g;
        }
        const float ref = reference.level();
        const float gainDb = ref > 0.0f ? (1.0f - d) * (kDynamicsTargetDb - 20.0f * std::log10 (ref)) : (d < 1.0f ? kDynamicsMaxBoostDb : 0.0f);
        const float want = dbToGain (std::min (gainDb, kDynamicsMaxBoostDb));
        g = blend.isSmoothing() ? from + blend.next() * (want - from) : want;
        return g;
    }

private:
    int blendLen = 1, amountLen = 1;
    LinearRamp amount, blend;
    float g = 1.0f, from = 1.0f;
    bool blendJumps = true;
};

} // namespace gtt
