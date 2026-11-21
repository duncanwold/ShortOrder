#pragma once

#include "DspUtils.h"
#include "Macro.h"
#include "Readout.h"
#include "PitchDetector.h"
#include "PitchTimeline.h"
#include "Quantizer.h"
#include "SynthSource.h"
#include "Sustain.h"
#include "Dynamics.h"
#include "Onset.h"
#include "Chord.h"
#include "Headroom.h"
#include "Trace.h"
#include <functional>

namespace gtt
{

// Plain parameter snapshot, filled once per block by the processor (or a test). The defaults are the
// plugin's. The fields marked fixed have no parameter since v0.5.2: the processor always sets the fixed
// value, and tests set others (v0.5.1's settings, which the tests written then still run: v051Defaults).
struct EngineParams
{
    // NOTE DETECTION
    float windowHz = defaultWindowHz();           // "Latency": the window L = 2 * round(fs / windowHz); the voice plays D = L / 2 behind
    float sustainPercent = kDefaultSustainPercent; // "Sustain": how much of the guitar's fade the level ignores while detected
    float dynamicsPercent = 100.0f * kDynamics;   // fixed (v0.5.2): 100 = notes as played (bypassed), 0 = every note at the same level
    float holdMs = kDefaultNoteReleaseMs;         // "Note Release": keep the last note this long after pitch is lost (the choice's time, noteReleaseMsFor)
    RetuneSpeed retuneSpeed = RetuneSpeed::Medium;

    // VOICE
    Octave octave = Octave::Main;
    Wave wave = Wave::Saw;
    float filterCutoffHz = kLowPassCutoffHz; // fixed (v0.5.4): the Low Pass Filter at its default

    // CHORD: the Chord selector (v0.5.4) sets the mode and, for Stack, the key (chordChoiceFor)
    ChordMode chordMode = ChordMode::Off;
    Triad triad = Triad::Major;   // Stack, Key Off only (not reachable from the plugin since v0.5.4)
    Voicing voicing = kVoicing;   // fixed (v0.5.2): Stack
    int key = kKeyOff;            // Stack: 0 .. 11 = C .. B (the major key's tonic); Off (chromatic) for tests only
    Scale scale = kScale;         // fixed (v0.5.2): Stack, Key on
    float chordLevelPercent = 100.0f * kChordLevel; // fixed (v0.5.2): both modes, the other tones against the played note
    int memoryNotes = kDefaultMemoryNotes; // "Memory Notes" (v0.5.4): Memory, the notes heard at once (the live note included), 2 .. 4

    // ADVANCED: Confidence and Hysteresis used instead of the fixed ones when manual is on.
    // Defaults = the fixed values, so switching Manual on at defaults changes nothing.
    // manual is the value in effect: the processor passes it through manualInEffect().
    bool manual = false;
    float confidenceThreshold = kConfidence;
    float hysteresisCents = kHysteresisCents;

    // OUTPUT
    bool bypass = false;
};

// "2+ NOTES?" hint (display only; no effect on the sound). It infers a chord from low
// confidence on a clearly sounding input, so it can also fire on noisy single notes, heavy
// string noise, or a single note below the window's range -- hence the question mark.
// Tuned on synthetic plucks (tests chord-hint / probe-chord): two-note intervals sit at
// confidence ~0.5..0.94, in-range single notes at >= 0.99; octaves read as one note.
struct ChordHint
{
    static constexpr float kMinRmsDb = -30.0f;      // input "clearly sounding" above this (analysis window RMS)
    static constexpr float kMaxConfidence = 0.85f;  // ...and the detector's confidence below this (spec's first guess: 0.5)
    static constexpr double kOnMs = 50.0;           // for longer than this: flag
    static constexpr double kOffMs = 200.0;         // quiet or confident for this long: clear

    float minRmsDb = kMinRmsDb;
    float maxConfidence = kMaxConfidence;
    double onMs = kOnMs, offMs = kOffMs;
};

// The ADVANCED panel (Manual, Confidence, Hysteresis) in the editor. Hidden for now; the code
// stays, and flipping this brings it back. While it's hidden, Manual counts as off whatever its
// saved value, so a session saved with Manual on can't change detection with no visible control.
constexpr bool kShowAdvanced = false;
inline bool manualInEffect (bool savedManual, bool showAdvanced = kShowAdvanced) { return showAdvanced && savedManual; }

// There is no output gain (Output was removed in v0.6.3): the channel fader after the plugin is the level control. Bypass has
// no control in the editor either: it's the host's own bypass (GTTProcessor::getBypassParameter).

// The four detection settings in effect: Window and Hold as set; Confidence and Hysteresis fixed,
// or the ADVANCED ones in Manual. The engine resolves this once per block; the editor uses it to
// show the effective values.
inline DetectionSettings effectiveDetection (const EngineParams& p)
{
    return { p.windowHz, p.manual ? p.confidenceThreshold : kConfidence, p.manual ? p.hysteresisCents : kHysteresisCents, p.holdMs };
}

// Per-hop diagnostics (tests only; the plugin leaves the listener empty).
struct HopInfo
{
    int64_t time = 0; // window centre, absolute samples
    DetectorResult detector;
    double midi = 0.0;
    int noteN = 0;
    double target = 0.0;
    double ratio = 1.0;
    bool hasNote = false;
    bool twoPlusNotes = false; // the "2+ NOTES?" hint after this hop
    // Tests only (investigate-blips): what happened at this hop.
    bool onset = false, voiceOpen = false, lockHeld = false, muted = false, revivalBlocked = false, edgeLag = false;
    double rawMidi = 0.0; // the detector's reading before the harmonic lock
    int event = 0;        // Quantizer::Event
};

// The sputter fix's parts (v0.5.5, from the investigation on the investigate-sputter branch, docs/reports/SPUTTER-REPORT.md) and
// the investigation's other mitigations. All off in a default-constructed TailSwitches (v0.5.4's behaviour);
// sputterFix() below is v0.5.5's, which the engine uses unless a test sets another (setTailSwitches, setSputterFix).
// The fix's parts are 1, 5r (harmonics only), 6 (with liftOnPick), 7, 9 and 10; the others are tests only.
struct TailSwitches
{
    float graceMs = 0.0f;        // 1: a dropout shorter than this doesn't start the release (the level holds, the gate stays open)
    float graceInstantMs = -1.0f; // 1: at Note Release Instant (no hold) the grace is this instead (< 0: graceMs)
    float keepConfidence = 0.0f; // 2: > 0: sticky voicing, the confidence a voiced note needs to stay voiced
    float keepRmsDb = -60.0f;    // 2: ... and the window RMS (dBFS) it needs (the detector's own floor is -60)
    float endBelowDb = 0.0f;     // 3: < 0: the note ends (unvoiced) once the window RMS is below this, until an onset or 6 dB above it
    float taperTopDb = 0.0f;     // 4: < 0: Sustain tapers to 0 as the envelope follower (mean |x|, dBFS) falls from this ...
    float taperRangeDb = 20.0f;  //    ... over this many dB
    float lockBelowDb = 0.0f;    // 5: < 0: no note changes while the window RMS is below this (the held note stays)
    // Relative to the note's own reference (NoteReference, the note's peak level) instead of an absolute level:
    float taperUnderRefDb = 0.0f;  // 4r: > 0: Sustain tapers to 0 as the follower falls from this far under the reference, over taperRangeDb
    float lockUnderRefDb = 0.0f;   // 5r: > 0: no note changes (without an onset) while the follower is this far under the reference
    float oneWayUnderDb = -1.0f;   // 6: >= 0: while the follower is this far under the reference, pitch back after the release has
                                   //    started (without an onset) stays at the level the release reached (Sustain only)
    float lockPersistMs = 0.0f;    // 5r: > 0: a softer lock: only harmonic jumps (+-12, +-19, +-24) are refused; another note is
                                   //    taken once it has persisted this long (a slide arrives late instead of not at all)
    bool lockHarmonicsOnly = false; // 5r: only harmonic jumps are refused, every other note change passes (lockPersistMs under a hop)
    float pickRestartUnderDb = 0.0f; // 8: > 0: while the follower is this far under the reference, a pick after a gap restarts the
                                     //    note's reference (it plays at its own level) instead of inheriting it under Glide Always
    float reviveRiseDb = 0.0f;     // 7: > 0: after the voice gate has closed in a note's tail (its last voiced hop 20 dB+ under the
                                   //    reference), for 1 s a hop is voiced only if its window RMS is this far above that last voiced hop's
    bool reviveLevelAhead = false; // 7: the last voiced hop is in the tail by the undelayed level (as the lock), not the voice's delayed one
    bool liftOnPick = false;       // 6: a real pick (as the revival guard's) also lifts the one-way cap (v0.5.5's chug fix, kLiftOnPick)
    float reviveWakeRiseDb = 0.0f; // 7: > 0: a pick (an onset rising this far above the quietest since pitch was lost) passes the guard
    bool lockOnPitch = false;      // 5r: the harmonic test measures the jump between detected pitches (the last unlocked voiced hop's,
                                   //     within 35 c of 12 / 19 / 24 semitones), not between quantized notes
    float muteDropDb = 0.0f;       // 9: > 0: a mute ends the note: once the undelayed level falls this far within 50 ms while the voice
                                   //    is open, every hop is unvoiced until the level is back within muteWakeUnderRefDb of the
                                   //    reference (a pick), or 1 s after the last voiced hop
    float muteWakeUnderRefDb = 25.0f;
    float muteWakeRiseDb = 0.0f;   // 9: > 0: an onset whose level reaches this far above the quietest since the mute also wakes it
    bool muteSkipsGrace = false;   // 9: a mute starts the release at once (no grace for that dropout)
    bool edgeLagUnvoiced = false;  // 10: a reading at the shortest lag searched (above kDetectorMaxHz: the dip is the range's edge,
                                   //     not a period; YIN finds it in a mute's transient and in noise) is unvoiced
};

// What happens at a pick (docs/reports/BLIPS-REPORT.md): v0.5.10's two blip fixes (the fall lock, the mute gate's pass; blipFix() below,
// the engine's default) and the investigation's other mitigations (tests only). All off (a default-constructed
// PickSwitches) is v0.5.9.
struct PickSwitches
{
    PickRules rules;               // the quantizer's (fresh latch after a gap, arrival snap, stable hops)
    float fallLockDb = 0.0f;       // > 0: no note change (without an onset) while the undelayed level is this far under its own
                                   //      largest of the last 50 ms (a damp, a mute coming): the held note stays
    bool fallOnWindowRms = false;  //      ... measured on the detector's window RMS (the hops' own) instead of the undelayed level
    float fallWindowMs = 50.0f;    //      ... over this long
    float fallPassConfidence = 0.0f; // > 0: ... unless the reading's confidence is at least this (a real note arriving as the level
                                     //      falls, e.g. a pull-off to a quieter note, still changes at once)
    float fallMinSemis = 0.0f;     // > 0: ... and only for a change at least this many semitones from the held note (smaller ones,
                                   //      the legato ornaments', always pass)
    float muteKeepConfidence = 0.0f; // > 0: the mute gate doesn't unvoice a reading this sure (a pull-off falling 20 dB isn't a mute),
    int muteKeepHops = 1;            //      and this many such hops in a row end the mute
    float onsetConfidence = 0.0f;  // > 0: the first onsetHops hops from an onset need this confidence to be voiced
    int onsetHops = 0;
    int ignoreHopsAfterOnset = 0;  // > 0: the hops from an onset (this many) are unvoiced
};

// v0.5.10's blip fixes (docs/reports/BLIPS-REPORT.md), from the constants in Macro.h.
inline PickSwitches blipFix()
{
    PickSwitches p;
    p.fallLockDb = kFallLockDropDb;
    p.fallOnWindowRms = true;
    p.fallWindowMs = (float) kFallLockWindowMs;
    p.fallPassConfidence = kFallLockPassConfidence;
    p.fallMinSemis = (float) kFallLockMinSemitones;
    p.muteKeepConfidence = kMuteGatePassConfidence;
    p.muteKeepHops = 1;
    return p;
}

// v0.5.5's sputter fix (docs/reports/SPUTTER-REPORT.md's "fix2"), from the constants in Macro.h.
inline TailSwitches sputterFix()
{
    TailSwitches s;
    s.graceMs = (float) kReleaseGraceMs;
    s.graceInstantMs = (float) kReleaseGraceInstantMs;
    s.oneWayUnderDb = kTailUnderReferenceDb;
    s.lockUnderRefDb = kTailUnderReferenceDb;
    s.lockHarmonicsOnly = true;
    s.reviveRiseDb = kReviveRiseDb;
    s.reviveWakeRiseDb = kReviveWakeRiseDb;
    s.reviveLevelAhead = true;
    s.muteDropDb = kMuteDropDb;
    s.muteWakeRiseDb = kMuteWakeRiseDb;
    s.muteWakeUnderRefDb = kMuteWakeUnderRefDb;
    s.muteSkipsGrace = true;
    s.edgeLagUnvoiced = true;
    s.liftOnPick = kLiftOnPick;
    return s;
}

// Tests only: the voice on the delayed timeline, one sample (Engine::voiceListener).
struct VoiceProbe
{
    int64_t t = 0;
    bool voiced = false, open = false, onset = false, graced = false, capped = false; // capped: Sustain's one-way cap holds the level down
    int note = 0;
    float gate = 0.0f, env = 0.0f, ref = 0.0f, level = 0.0f, dynamicsGain = 1.0f, headroomGain = 1.0f, predictedPeak = 0.0f;
    int memoryHeld = 0;
    float foldDrive = 1.0f;                // v0.5.8: Folded's drive this sample (1 with another wave)
};

// The whole mono chain: detector -> quantizer -> synth voice -> headroom gain ->
// safety clip -> bypass. No JUCE processor or editor dependencies.
class Engine
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    // Mono in -> mono out. in and out may alias.
    void process (const float* in, float* out, int numSamples, const EngineParams& p);

    Readout& readout() { return readoutState; }
    // v0.5.11: the pitch trace's feed (display only). The engine pushes a TraceFrame every hop of the voice's timeline while the
    // feed has readers (an editor open); never otherwise. Set once, before processing (the processor owns the FIFO).
    void setTraceFeed (TraceFifo* feed) { trace = feed; }
    int currentLatencySamples() const { return voiceDelay; } // the voice's delay D (voiceDelayFor): what the output plays behind the input
    int windowLength() const { return windowLen; }           // the detector's window L
    double sampleRate() const { return fs; }
    int hopSize() const { return hop; }
    int windowSwaps() const { return swapCount; } // number of L changes applied (tests)
    bool twoPlusNotes() const { return chordFlag; }
    void setHeadroomGain (bool on) { headroomOn = on; }               // tests only: off = the output stage before v0.5.3 (the voice straight into the clip)
    void setLegacyMemoryClear (bool on) { chord.setLegacyClear (on); } // tests only: on = memory clears when the voice gate closes (up to v0.5.3), not after kMemoryClearMs without pitch
    void setMemoryClearMs (double ms) { chord.setClearMs (ms); }       // tests only, before prepare(): another clear time (v0.5.5's 250 ms)
    void setLegacyNoteReference (bool on) { noteReference.setLegacyHold (on); } // tests only: on = the note's reference frozen while pitch is lost (up to v0.5.3)
    float noteLevel() const { return noteReference.level(); }        // tests only: the note's reference (mean |x|) while in use
    float lastPredictedPeak() const { return predictedPeak; }        // tests only: the last sample's predicted output peak, before the headroom gain
    float headroomGain() const { return headroom.gain(); }           // tests only: the last sample's headroom gain

    std::function<void (const HopInfo&)> hopListener;
    std::function<void (const VoiceProbe&)> voiceListener; // tests only: every sample of the voice
    // Tests only: the sputter fix's parts and the investigation's other mitigations (default: sputterFix()). Off is v0.5.4.
    void setTailSwitches (const TailSwitches& t)
    {
        tail = t;
        applyTail();
    }
    // Off is v0.5.4, so it takes v0.5.10's blip fixes, v0.5.11's pick glide and v0.6.2's fixes with it.
    void setSputterFix (bool on)
    {
        setTailSwitches (on ? sputterFix() : TailSwitches {});
        if (! on)
        {
            setBlipFixes (false, false);
            setPickGlide (false);
            setV062Fixes (false);
        }
    }
    // Tests only: every v0.6.2 fix on or off (off: v0.6.1).
    void setV062Fixes (bool on)
    {
        setBottomEdge (on);
        setGlideToReal (on);
        setBlendFromLevel (on);
    }
    // Tests only: v0.5.10's blip fixes, each on (blipFix()'s values) or off. Both off is v0.5.9.
    void setBlipFixes (bool fallLock, bool mutePass)
    {
        const auto f = blipFix();
        pickSw.fallLockDb = fallLock ? f.fallLockDb : 0.0f;
        pickSw.fallOnWindowRms = fallLock && f.fallOnWindowRms;
        pickSw.fallWindowMs = f.fallWindowMs;
        pickSw.fallPassConfidence = fallLock ? f.fallPassConfidence : 0.0f;
        pickSw.fallMinSemis = fallLock ? f.fallMinSemis : 0.0f;
        pickSw.muteKeepConfidence = mutePass ? f.muteKeepConfidence : 0.0f;
        pickSw.muteKeepHops = f.muteKeepHops;
    }
    // Tests only: v0.5.11's pick glide (kPickGlideWindowMs) on or off. Off is v0.5.10.
    void setPickGlide (bool on) { pickGlideOn = on; }
    bool pickGlide() const { return pickGlideOn; }
    static inline bool defaultPickGlide = true; // what prepare() starts with (tests: GTT_PICK_GLIDE=0 makes it v0.5.10's)
    int pickGlides() const { return pickGlideCount; } // tests only: picks the pick glide glided in
    // Tests only (v0.5.11, the ceiling investigation): the edge rule's ceiling and the detector's top together (default kDetectorMaxHz,
    // 1400 Hz). After prepare().
    // sureAbove > 0: a reading above kDetectorMaxHz (and under the ceiling) is voiced only at this confidence or more.
    void setCeilingHz (double hz, float sureAbove = 0.0f)
    {
        ceiling = hz;
        ceilingSure = sureAbove;
        detector.setMaxHz (hz);
    }
    double ceilingHz() const { return ceiling; }
    int tailLocks() const { return locks; } // tests only: hops where the pitch lock held the note
    void setPickSwitches (const PickSwitches& p) { pickSw = p; quantizer.rules = p.rules; } // tests only (the blips probes)
    static inline PickSwitches defaultPickSwitches = blipFix(); // what prepare() starts with (tests: GTT_BLIP_FIXES=0 makes it v0.5.9's)
    int tailEdgeLags() const { return edgeLags; } // tests only: hops switch 10 unvoiced
    // v0.6.2 (Macro.h), tests only: each fix on (the default) or off; all off is v0.6.1.
    void setBottomEdge (bool on) { bottomEdgeOn = on; } // fix 1: the bottom edge rule (Macro.h)
    int bottomEdges() const { return bottomEdgeCount; } // tests only: hops fix 1 unvoiced
    void setGlideToReal (bool on) { glideToRealOn = on; } // fix 2: a glide interrupted by lost pitch goes on to the last real note
    // fix 4: Sustain's 5 ms blends start from the level sounding, and a jump of the note's reference when pitch comes back is crossed
    // over 5 ms (by Sustain and Dynamics) instead of in one sample
    void setBlendFromLevel (bool on)
    {
        sustainLevel.setBlendFromLevel (on);
        noteReference.setReportJumps (on);
        dynamicsGain.setBlendJumps (on);
    }
    // Tests only: where each part of the sputter fix engaged (v054-identity).
    struct TailCounts
    {
        int graceDropouts = 0, oneWayCaps = 0, locks = 0, revivals = 0, mutes = 0, edgeLags = 0;
        bool any() const { return graceDropouts + oneWayCaps + locks + revivals + mutes + edgeLags > 0; }
    };
    TailCounts tailCounts() const { return { graceDropouts, sustainLevel.capsSet(), locks, revivalsBlocked, mutes, edgeLags }; }
    ChordHint chordHint; // the 2+ NOTES? thresholds (the constants; tests sweep them)

private:
    // The block's effective settings, resolved once per block from EngineParams.
    struct Settings
    {
        DetectionSettings detection;
        float retuneMs = 0.0f;
        int holdSamples = 0;
        Octave octave = Octave::Main;
        float sustain = 0.0f; // 0..1
        float dynamics = 1.0f; // 0..1
        ChordSettings chord;
        bool folded = false; // Wave Folded (v0.5.8): the drive is worked out each sample
    };
    Settings resolve (const EngineParams& p) const;

    void runHop (const Settings& s);
    void setWindow (double windowHz);
    float stepWindowFade (double requestedHz);
    float processVoice (int64_t t, float delayedIn, int holdSamples, const Settings& s);

    double fs = 48000.0;
    int windowLen = 0;  // L, the detector's window
    int64_t freshLatchEntry = -(1 << 30), freshSnapLen = 1; // the fresh-start snap (Macro.h): the last fresh latch's entry time; kFreshSnapMs
    int voiceDelay = 0; // D = voiceDelayForWindow (L): the voice plays the timeline this far behind the newest input sample
    int hop = 128, hopCounter = 0;

    History history;
    PitchDetector detector;
    Quantizer quantizer;
    PitchTimeline timeline;
    float lastVoicedPeriod = 0.0f;
    int64_t lastVoicedEntry = -1;  // time of the newest voiced timeline entry (-1: none yet)
    uint32_t onsetsSeen = 0;       // OnsetDetector::foundCount() at the last hop
    double chordLikeMs = 0.0, notChordLikeMs = 0.0; // the 2+ NOTES? hint's timers (hop time)
    bool chordFlag = false;

    SynthSource synth;
    LinearRamp voiceGate;   // lost pitch: 15 ms out (after the hold), 5 ms in
    OnsetDetector onsets;
    NoteReference noteReference; // the note's level, for Sustain and Dynamics
    SustainLevel sustainLevel;
    DynamicsGain dynamicsGain;
    ChordVoice chord;
    FoldShape foldShape;   // v0.5.8: Folded's fold at the live note's drive, shared by every tone (Tone::shareFold)
    float foldDrive = 1.0f, foldCoef = 1.0f;
    HeadroomGain headroom;          // v0.5.3: keeps the predicted output peak under the ceiling, before the safety clip
    float voicePeak = 0.0f;         // processVoice()'s bound on its own output, this sample
    float predictedPeak = 0.0f;     // the same after the window fade
    int sinceVoiced = kNeverVoiced; // samples on the delayed timeline since the last voiced one
    void applyTail() // the sample lengths of the sputter fix, for this rate
    {
        graceLenHeld = tail.graceMs > 0.0f ? msToSamples (fs, tail.graceMs) : 0;
        graceLenInstant = tail.graceInstantMs < 0.0f ? graceLenHeld : tail.graceInstantMs > 0.0f ? msToSamples (fs, tail.graceInstantMs) : 0;
        graceLen = graceLenHeld;
        liftLen = msToSamples (fs, kRepickWindowMs);
        repickLen = msToSamples (fs, kRepickProtectMs);
        muteHops = std::clamp ((int) std::lround (kMuteWindowMs * 0.001 * fs / hop), 1, (int) muteRing.size());
        muteWakeHops = (int) std::lround (kMuteWakeWindowMs * 0.001 * fs / hop);
        reviveLen = (int64_t) std::lround (kReviveWindowMs * 0.001 * fs);
        realRingLen = std::clamp ((int) std::lround (kFallLockWindowMs * 0.001 * fs / hop), 1, (int) realRing.size());
        pickGlideLen = (int) std::lround (kPickGlideWindowMs * 0.001 * fs);
    }
    static constexpr int kNeverVoiced = 1 << 30;
    // The HELD readout: the live note counts as being played (not held) until pitch has been lost this long, so a
    // pick's brief dropout doesn't flash the light.
    static constexpr double kHeldLiveGraceMs = 50.0;
    int heldGraceLen = 1;
    // Window (L) change: once the knob has been still for kSettleMs, fade the voice
    // out over 20 ms, swap L (and D) everywhere, fade back in over 20 ms. One swap per gesture.
    static constexpr double kSettleMs = 100.0;
    LinearRamp windowFade;
    enum class WindowState { Idle, FadingOut, FadingIn } windowState = WindowState::Idle;
    double windowHz = kWindowMaxHz;
    int pendingTau = 0, settleRemaining = 0, settleLen = 1, swapCount = 0;
    LinearRamp bypassMix;   // 0 = synth, 1 = raw input (10 ms)
    int gateInLen = 1, gateOutLen = 1, xfadeLen = 1, windowFadeLen = 1;
    bool firstBlock = true;
    bool headroomOn = true;

    // The sputter fix (v0.5.5) and the investigation's test-only mitigations.
    TailSwitches tail = sputterFix();
    PickSwitches pickSw = blipFix();
    int unvoicedRun = 0, hopsSinceOnset = 1 << 20, fallRingPos = 0, muteSureRun = 0;
    std::array<float, 64> fallRing {};
    int graceLen = 0, graceLenHeld = 0, graceLenInstant = 0, locks = 0, revivalsBlocked = 0, pendingNote = -1, pendingHops = 0, anchorNote = -1;
    bool lastHopVoiced = false, belowEnd = false, lastVoicedInTail = false;
    float lastVoicedRms = 0.0f, liftEnv = 0.0f;
    int liftWait = 0, liftLen = 1, sinceRepick = 1 << 30, repickLen = 1, sinceOnsetHops = 1 << 20;
    double lastVoicedMidi = 0.0;
    std::array<float, 64> muteRing {};
    int muteHops = 1, muteWakeHops = 1, muteRingPos = 0, mutes = 0, muteWakeWait = 0, edgeLags = 0;
    int64_t reviveLen = 1;
    float gapFloor = 0.0f; // the quietest undelayed level since pitch was lost
    bool pickAtHop = false;
    int64_t lastPickTime = -1;
    int pickWait = 0;      // hops left in which an onset's rise counts as a pick
    int graceDropouts = 0; // tests only: dropouts the grace held over
    bool wasGraced = false;
    float mutedFloor = 0.0f;
    bool muted = false;
    VoiceProbe probe;
    // v0.5.11, the pitch trace (display only): the feed, a hop counter on the voice's timeline, the live level this sample.
    TraceFifo* trace = nullptr;
    int traceCounter = 0;
    float traceLevel = 0.0f;
    void pushTrace (int64_t t, float fade, const Settings& s);
    // v0.5.11, the pick glide: the last real note's hop (entry time, T), the latest onset's hop, and its own window-RMS ring.
    bool pickGlideOn = defaultPickGlide;
    double ceiling = kDetectorMaxHz; // the edge rule's (tests: setCeilingHz)
    bool bottomEdgeOn = true; // v0.6.2's fix 1
    int bottomEdgeCount = 0;
    bool glideToRealOn = true; // v0.6.2's fix 2
    bool glideLost = false;    // pitch was lost mid-glide (fix 2 is moving T on to the last real note)
    int glideFromNote = -1, glideToNote = -1; // fix 2: the quantizer's note before the current one, and the current one
    float ceilingSure = 0.0f;
    int64_t realEntry = -1, lastOnsetEntry = -1;
    double realT = 0.0;
    std::array<float, 64> realRing {};
    int realRingPos = 0, realRingLen = 1, pickGlideLen = 1, pickGlideCount = 0, realNote = -1;

    Readout readoutState;
};

} // namespace gtt
