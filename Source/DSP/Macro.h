#pragma once

#include "DspUtils.h"

namespace gtt
{

// The four detection settings in effect: Window (Latency / Minimum Pitch) and Hold (Note Release)
// are direct controls; Confidence and Hysteresis are fixed (or the ADVANCED parameters, in Manual).
struct DetectionSettings
{
    float windowHz = 300.0f;
    float confidence = 0.55f;
    float hysteresisCents = 0.0f;
    float holdMs = 1500.0f;
};

// The fixed Confidence threshold and Hysteresis: v0.4.3's Deliberate (v0.4.2's Deliberation at
// 75%), exact in float.
constexpr float kConfidence = 0.8125f;
constexpr float kHysteresisCents = 11.25f;

// Latency / Minimum Pitch default: 240.62 Hz (8.29 ms, lowest reliable B3), the geometric middle of
// v0.4's range (minimum .. 300 Hz), computed as v0.4's main control did at 50% so the value is
// bit-identical to v0.4.2's. It no longer sits in the middle since the top moved to 424.26 Hz.
inline float defaultWindowHz() { return (float) (kV04WindowMaxHz * std::pow (derivedWindowMinHz() / kV04WindowMaxHz, 0.5)); }
// Note Release (v0.5.2: a choice) -> the hold time. Each is the engine's Note Release at that time: the
// pitch hold, the 15 ms gate fade (Fast has only that, and the shorter grace, kReleaseGraceInstantMs), and with
// Sustain on the fade from the current level. v0.5.9: Fast / Medium / Long = 0 / 500 / 1500 ms (v0.5.8's Instant,
// Medium, and a Long between its Long and Extra Long).
constexpr float noteReleaseMsFor (NoteRelease r)
{
    switch (r)
    {
        case NoteRelease::Fast: return 0.0f;
        case NoteRelease::Long: return 1500.0f;
        case NoteRelease::Medium:
        default: return 500.0f;
    }
}
// v0.5.6 .. v0.5.8's options: Instant / Fast / Medium / Long / Extra Long = 0 / 250 / 500 / 1000 / 2000 ms (tests only).
constexpr float noteReleaseMsFor (NoteReleaseV058 r)
{
    switch (r)
    {
        case NoteReleaseV058::Instant: return 0.0f;
        case NoteReleaseV058::Medium: return 500.0f;
        case NoteReleaseV058::Long: return 1000.0f;
        case NoteReleaseV058::ExtraLong: return kHoldMaxMs;
        case NoteReleaseV058::Fast:
        default: return 250.0f;
    }
}
// Organ (v0.5.8): a drawbar organ's three sines, phase-locked on one phase: the fundamental, the octave (2x) and the 12th
// (3x) at these levels (relative; the sum is normalized to peak at +/-1, Oscillator.h). By ear: tune here.
constexpr double kOrganLevel1 = 1.0, kOrganLevel2 = 0.5, kOrganLevel3 = 0.35;

// Folded (v0.5.8): a sine through a wavefolder (Oscillator.h), driven by the pick's level (v0.5.9: the level chain after Sustain,
// before the Dynamics gain, so Dynamics evens out the loudness while the brightness follows the picking at full range; v0.5.8
// took it after Dynamics, at -24 dB, 1.6 dB per dB, up to 5). The drive is 1 (a plain sine) at and below kFoldSoftDb (mean |x|,
// as Dynamics' kDynamicsTargetDb), and above it rises kFoldDbPerDb dB per dB of level, up to kFoldMaxDrive:
// drive = (level / kFoldSoftDb)^kFoldDbPerDb. On the test plucks (test folded) a soft pick (0.08 peak) stays at 1, a medium one
// (0.5) reaches about 3 and a hard one (0.9) the top. kFoldMaxDrive is where the three stages stop getting steadily brighter
// (above about 6.25 the centroid dips). Smoothed over kFoldDriveSmoothMs (one pole), every sample.
constexpr float kFoldSoftDb = -27.0f;
constexpr float kFoldDbPerDb = 1.2f;
constexpr float kFoldMaxDrive = 6.0f;
constexpr double kFoldDriveSmoothMs = 2.0;
// The folder's three parallel stages (Oscillator.h's foldStages): at these fractions of the drive, each weighted by its own.
constexpr double kFoldStageScale[3] = { 1.0, 0.7, 0.5 };
constexpr double kFoldStageNorm = 1.0 * 1.0 + 0.7 * 0.7 + 0.5 * 0.5; // sum of scale^2: a sine of amplitude 1 at drive 1
inline const float kFoldSoftLevel = dbToGain (kFoldSoftDb);
inline float foldDriveFor (float level) { return std::clamp (std::pow (std::max (level, 0.0f) / kFoldSoftLevel, kFoldDbPerDb), 1.0f, kFoldMaxDrive); }

// The Wave parameter's order: Saw, Square, NES Lead (Pulse 25%), Triangle, Sine, Half-Sine, Organ, Folded (waveLabel). The engine's
// Wave values are their own (DspUtils.h), so a choice index maps through this.
constexpr int kWaveChoices = 8;
inline Wave waveForChoice (int index)
{
    static constexpr Wave order[kWaveChoices] = { Wave::Saw,  Wave::Square,   Wave::Pulse25, Wave::Triangle,
                                                  Wave::Sine, Wave::HalfSine, Wave::Organ,   Wave::Folded };
    return order[std::clamp (index, 0, kWaveChoices - 1)];
}
// The name the player sees (v0.5.11: Pulse 25% is "NES Lead").
inline const char* waveLabel (Wave w)
{
    switch (w)
    {
        case Wave::Saw: return "Saw";
        case Wave::Square: return "Square";
        case Wave::Pulse25: return "NES Lead";
        case Wave::Triangle: return "Triangle";
        case Wave::Sine: return "Sine";
        case Wave::HalfSine: return "Half-Sine";
        case Wave::Organ: return "Organ";
        case Wave::Folded: return "Folded";
    }
    return "Saw";
}
constexpr NoteRelease kDefaultNoteRelease = NoteRelease::Medium; // v0.5.9 (v0.5.2 .. v0.5.8: Fast, 250 ms)
constexpr float kDefaultNoteReleaseMs = noteReleaseMsFor (kDefaultNoteRelease);
constexpr float kDefaultSustainPercent = 75.0f; // v0.5.6 (0 up to v0.5.5: the level follows the guitar, as v0.4.3)

// Fixed in v0.5.2 (found by ear; the parameter is gone, the code behind it stays, so it can come back as a
// control). Dynamics: 50%, halving the dB between notes (Dynamics.h: T and the +18 dB cap). (Glide was fixed
// at Always then too: a note arriving while the voice gate is still open glides in from the previous pitch
// and inherits the note's level. Its other mode, Legato, was removed in v0.6.3.)
constexpr float kDynamics = 0.5f;

// Fixed in v0.5.4 (the parameter is gone; the filter stays in the path): the Low Pass Filter at its
// default, where the wave levels (Oscillator.h) were measured.
constexpr float kLowPassCutoffHz = 5000.0f;

// Retune Speed -> glide time constant (starting values, to tune by ear).
inline float retuneMsFor (RetuneSpeed s)
{
    switch (s)
    {
        case RetuneSpeed::Medium: return 40.0f;
        case RetuneSpeed::Slow: return 120.0f;
        case RetuneSpeed::Fast:
        default: return 0.0f; // hard snap
    }
}

// Onset snap: in the first kOnsetSnapMs of a note, a quantized note change of kOnsetSnapSemitones
// or more snaps instead of gliding (the detector's onset octave error stays a blip at any Retune
// Speed). Kept here so it can become a hidden "Glitch" switch.
constexpr double kOnsetSnapMs = 25.0;
constexpr int kOnsetSnapSemitones = 7;

// 1.1, the fresh-start snap: a note from silence (the quantizer's fresh latch) whose
// note changes within kFreshSnapMs of the latch (entry time) snaps there (Retune instant) instead of gliding. The first sure reading
// can come from a window still holding a little silence, a neighbour up to ~50 c off, and without the snap that wrong first note
// glided 35-40 ms into the right one. Only a note from silence's own correction: a glide-in, a pick glide or a legato change in a
// phrase never latches fresh, and an onset ends the window (1.1 narrows the rule here: with the window running on through a new
// pick, 2 of the pick-glide test's 180 Release Fast picks into a damped note snapped, after the damped note re-latched fresh for one
// hop), so each keeps its Retune scoop.
constexpr float kFreshSnapMs = 15.0f;

// The sputter fix (v0.5.5; docs/reports/SPUTTER-REPORT.md's "fix2"): the end of long held notes, and mutes. Engine::sputterFix
// builds it from these; Engine::setSputterFix (false) is v0.5.4 (tests only).
// 1. Grace: a pitch dropout shorter than this doesn't start Sustain's release fade, and the gate's Note Release
//    hold starts after it.
constexpr double kReleaseGraceMs = 40.0;
// ... and at Note Release Instant (v0.5.5, stage 3 of its prompt): the shortest that keeps Instant's sputter fixed. 0 / 10 / 15 /
// 20 / 30 / 40 ms leave 141 / 36 / 25 / 9 / 7 / 4 audible dips on the synthetic long notes at Sustain 100% (v0.5.4: 326) and
// 4 / 1 / 1 / 0 / 0 / 0 on the real takes; mutes then end at most 0 / 10 / 15 / 20 / 30 / 40 ms later than v0.5.4's.
constexpr double kReleaseGraceInstantMs = 20.0;
// 2, 3. The tail: the envelope follower this far under the note's reference (NoteReference, its peak level). There,
//    pitch back after the release has started doesn't restore the held level without a new pick (the one-way
//    release), and no 12, 19 or 24 semitone jump away from the picked note is taken without one (the harmonic lock).
constexpr float kTailUnderReferenceDb = 20.0f;
constexpr float kRepickRiseDb = 6.0f;       // a re-pick: an onset, and the input up this much ...
constexpr double kRepickWindowMs = 20.0;    // ... within this long of it (noise in a gap fires onsets too)
constexpr double kRepickProtectMs = 100.0;  // after a re-pick, its own dropout isn't capped for this long
constexpr double kLockAfterOnsetMs = 100.0; // the lock leaves a note's first 100 ms alone; the note then is the picked one
// ... and a real pick (the one that wakes the mute gate: an onset rising kMuteWakeRiseDb above the quietest since pitch was
//    lost, within kMuteWakeWindowMs of it) lifts the one-way release too, whatever the envelope follower does (v0.5.5, the
//    chug fix: a soft chug straight after a hard one doesn't raise the follower 6 dB, its 120 ms release still shows the
//    hard one, so without this it stayed capped, 8-52 dB under v0.5.4).
constexpr bool kLiftOnPick = true;
// 4. Revival: once the gate has closed in a note's tail (its last voiced hop 20 dB under the reference, by the undelayed
//    level), for kReviveWindowMs a hop counts as voiced only if its window RMS is kReviveRiseDb above the last voiced hop's,
//    or a pick starts a note: an onset rising kReviveWakeRiseDb above the quietest since pitch was lost (within
//    kMuteWakeWindowMs of it). (v0.5.5 additions to the report's fix2: without them a soft pick after a loud decayed note,
//    or a loud sound rising into range after a quiet one, was kept silent at Instant.)
constexpr float kReviveRiseDb = 6.0f;
constexpr double kReviveWindowMs = 1000.0;
constexpr float kReviveWakeRiseDb = 25.0f;
// 5. Mute gate: the undelayed level falling kMuteDropDb within kMuteWindowMs while the voice is open ends the note at
//    once (no grace), until a pick: an onset whose level rises kMuteWakeRiseDb above the quietest since the mute
//    (within kMuteWakeWindowMs of it), or the level back within kMuteWakeUnderRefDb of the reference, or
//    kReviveWindowMs after the last voiced hop.
constexpr float kMuteDropDb = 20.0f;
constexpr double kMuteWindowMs = 50.0;
constexpr float kMuteWakeRiseDb = 25.0f;
constexpr double kMuteWakeWindowMs = 20.0;
constexpr float kMuteWakeUnderRefDb = 25.0f;
// 6. Edge rule: a reading above kDetectorMaxHz (the detector's shortest lag: the range's edge, not a period) is
//    unvoiced. The top of a 21-fret guitar is C#6, 1109 Hz.

// The blip fixes (v0.5.10; docs/reports/BLIPS-REPORT.md, "Revised recommendation"). Engine's blipFix() builds them from these;
// Engine::setBlipFixes (tests only) turns each off (both off is v0.5.9).
// 7. Fall lock: while the detector's window RMS is kFallLockDropDb or more under its highest of the last kFallLockWindowMs
//    (the level falling away: the pick damping the string before a re-pick, where the free string rings at another pitch
//    for 30-40 ms and reads at 0.81-0.95), a note change with no onset is refused (the held note stays), unless the
//    reading's confidence is kFallLockPassConfidence or more, or the change is under kFallLockMinSemitones (the damp's junk
//    is 5+ semitones away; hammer-ons, pull-offs and trills are mostly 1-3).
constexpr float kFallLockDropDb = 9.0f;
constexpr double kFallLockWindowMs = 50.0;
constexpr float kFallLockPassConfidence = 0.97f;
constexpr int kFallLockMinSemitones = 3;
// 8. The mute gate's pass: the mute gate (5) doesn't unvoice a reading at kMuteGatePassConfidence or more, and such a
//    reading ends a mute in progress (a quiet pull-off can fall 25-35 dB in a few ms, which tripped it since v0.5.5 and
//    froze the pitch between the two notes while it faded).
constexpr float kMuteGatePassConfidence = 0.97f;

// The pick glide (v0.5.11, docs/history/SPEC-v0.5.11.md section 2). Glide Always glides a note in only while the voice gate is open; at Note
// Release Fast (0 ms since v0.5.9) it closes 20 ms (the grace) + 15 ms after the last voiced hop, and none of the grace at a mute,
// so a pick after the damp before it (30-40 ms, which trips the mute gate) latched fresh. A pick (an onset) arriving within
// kPickGlideWindowMs of the last real note glides in from that note's pitch at Retune Speed, the gate closed or not. A real note's
// hop: voiced, not held by a lock, and the window RMS within kFallLockDropDb of its highest of the last kFallLockWindowMs (not in a
// damp, where the free string's junk reads), or voiced on the same note as the last real hop (the note carrying on into the damp). Measured from that hop to the pick's first hop: blips-dry 56 ms median, 67 at 90%,
// 72 at most (probe-pick-glide). The level is the pick's own, as before; only the pitch glides in.
constexpr double kPickGlideWindowMs = 100.0;

// v0.6.2 (docs/reports/RC2-REPORT.md; docs/reports/QA-REPORT-RC1.md issues 1 and 3), on by default; Engine::setBottomEdge / setGlideToReal / setBlendFromLevel
// (tests only) turn each off, and all off is v0.6.1 (`v0511-equivalence`).
// Fix 2, glides that don't freeze (QA issue 3): the quantizer freezes N and T while pitch is lost, so a glide interrupted by a pick's
// dropout, a damp or a muted end stopped part-way and the voice held an in-between pitch (20 .. 50 c off any note, at full level
// under Sustain) until the next voiced hop: 145 such stretches on the RC takes at Retune Medium. Now, when pitch is lost with T more
// than kGlideToRealMinCents from N and the voice open, and the last real note (the pick glide's: a voiced hop not in a damp,
// Engine::realNote) is one of the two notes the glide is between (N, or the note before it), N becomes that note and T carries on
// gliding to it at Retune Speed on every unvoiced hop while the voice is open: a glide to a real note is finished, and one toward a
// damp's junk (issue 5) goes back to where it came from. Otherwise (the real note is some older one) T stays, as up to v0.6.1. Measured against two other targets (N always: the glide finished as if pitch weren't lost;
// N when it had a real hop or a 0.97+ reading, else the real note), this one had the least wrong-note time (docs/reports/RC2-REPORT.md). At Retune
// Fast T is always N, so it never engages.
constexpr double kGlideToRealMinCents = 5.0;
// Fix 1, the bottom edge rule (mirrors the edge rule, 6, at the window's other end; Engine::runHop): a note under the window's lowest
// frequency (fs / tauMax, the floor) reads at the detector's longest lag, confidently (up to 0.99), as the floor's pitch: at the
// default the floor is B3 -41 c, so a guitar's A#3 +38 c (21 c under it) played B3. A reading whose dip is at the longest lag and still
// falling there (PitchDetector's atLongEdge) is unvoiced when the dip's own pitch (the parabola's vertex, unclamped: beyondCents
// under the floor; on synthetic plucks within ~1 c of the truth 4 .. 12 c under, over-reading further down) rounds to another note
// than the floor's reading does. So a note just under the floor that rounds to the same note still plays as in v0.6.1 (G#4 at the
// top detent, whose floor is G#4 +39 c; the lowest reliable note 30 c flat, at most ~5 c under the floor), and one that would play
// the wrong note doesn't (test `bottom-edge`).

} // namespace gtt
