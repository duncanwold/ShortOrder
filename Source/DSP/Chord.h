#pragma once

#include "DspUtils.h"
#include "SynthSource.h"
#include <array>

namespace gtt
{

// ---------------------------------------------------------------------------
// v0.5 chords. The played (live) note is always the existing voice (SynthSource's Tone); the chord
// engine adds the other tones and scales the sum. Chord Off with no tone still fading leaves the
// voice exactly as v0.4.6 (the engine doesn't call it at all).
// ---------------------------------------------------------------------------

// Choice order = the parameters' (Params.cpp).
enum class ChordMode { Off = 0, Stack = 1, Memory = 2 };
enum class Triad { Major = 0, Minor = 1 };
enum class Voicing { Bottom = 0, Top = 1, Open = 2 };
enum class Scale { Major = 0, NaturalMinor = 1, Dorian = 2, Mixolydian = 3 };
constexpr int kKeyOff = -1; // Key: Off, or 0 .. 11 = C .. B

constexpr int kMemoryNotesMin = 2, kMemoryNotesMax = 4; // Memory Notes (v0.5.4): the notes heard at once, the live note included
constexpr int kDefaultMemoryNotes = 3;
constexpr int kMaxChordTones = kMemoryNotesMax + 1; // the live note included, with Memory's brief extra tone before a new note commits
constexpr double kMemoryMinNoteMs = 80.0;  // Memory: a note joins once it has been live (and voiced) this long
constexpr double kMemoryClearMs = 150.0;   // Memory / Freeze: memory clears after this long with no pitch, whatever Note Release (v0.5.4: 250; v0.5.6: 150)
constexpr double kToneFadeMs = 20.0;       // tones entering / leaving, a tone's level moving between live and Chord Level
// Fixed in v0.5.2 (the parameters are gone; the code for the other values stays, for tests and to bring
// a control back): Stack's Open voicing, the Major scale (a minor key is its relative major), Chord Level 80%.
constexpr Voicing kVoicing = Voicing::Open;
constexpr Scale kScale = Scale::Major;
constexpr float kChordLevel = 0.8f;
// Gain law: the sum may be up to +3 dB (twice the power) over the played note alone, no more.
constexpr float kChordMaxPower = 2.0f;

// The Chord selector (v0.5.4: Triad and Key merged into it; v0.5.6: Memory Notes too). Off, Freeze 2 / 3 / 4 (Memory
// with that many notes heard, the live one included), then Harmonize (Stack) in each of the twelve keys,
// C maj / A min .. B maj / G# min (the major key's tonic, 0 .. 11). The chromatic Stack (Key Off) has no option; its
// code stays. "Freeze" and "Harmonize" are the player's names (labels, readout); the code keeps Memory and Stack.
constexpr int kChordChoices = 16;
constexpr int kFirstFreezeChoice = 1, kFirstHarmonizeChoice = kFirstFreezeChoice + (kMemoryNotesMax - kMemoryNotesMin + 1);
struct ChordChoice
{
    ChordMode mode = ChordMode::Off;
    int key = kKeyOff;
    int memoryNotes = kDefaultMemoryNotes; // Freeze's note count (the default elsewhere: Memory Notes is inert outside Memory)
};
inline ChordChoice chordChoiceFor (int index)
{
    if (index <= 0)
        return {};
    if (index < kFirstHarmonizeChoice)
        return { ChordMode::Memory, kKeyOff, kMemoryNotesMin + index - kFirstFreezeChoice };
    return { ChordMode::Stack, std::min (index - kFirstHarmonizeChoice, 11), kDefaultMemoryNotes };
}

// The two harmony intervals (semitones from the played note) for a Stack chord.
struct StackIntervals
{
    int a = 0, b = 0;
};

// Key Off: fixed intervals. Bottom: root position, played note at the bottom; Top: the 3rd and 5th an
// octave down (the played note on top); Open: the 5th above and the 3rd an octave up (a 10th).
inline StackIntervals placeTriad (int third, int fifth, Voicing v)
{
    switch (v)
    {
        case Voicing::Bottom: return { third, fifth };
        case Voicing::Open: return { fifth, third + 12 };
        case Voicing::Top:
        default: return { third - 12, fifth - 12 };
    }
}

inline StackIntervals chromaticIntervals (Triad t, Voicing v) { return placeTriad (t == Triad::Minor ? 3 : 4, 7, v); }

// Semitones from the key.
inline const std::array<int, 7>& scaleSteps (Scale s)
{
    static constexpr std::array<int, 7> major { 0, 2, 4, 5, 7, 9, 11 }, naturalMinor { 0, 2, 3, 5, 7, 8, 10 }, dorian { 0, 2, 3, 5, 7, 9, 10 },
        mixolydian { 0, 2, 4, 5, 7, 9, 10 };
    switch (s)
    {
        case Scale::NaturalMinor: return naturalMinor;
        case Scale::Dorian: return dorian;
        case Scale::Mixolydian: return mixolydian;
        case Scale::Major:
        default: return major;
    }
}

// The scale degree whose triad a played note uses: its own if it's in the scale; otherwise the nearest
// scale note's (equidistant: the one below, which in these seven-note scales is every out-of-scale
// note). The played note itself is never moved; only the interval sizes are borrowed.
inline int harmonyDegree (int note, int key, Scale scale)
{
    const auto& steps = scaleSteps (scale);
    const int pc = ((note - key) % 12 + 12) % 12;
    int below = 0, above = 0, downBest = 12, upBest = 12;
    for (int d = 0; d < 7; ++d)
    {
        const int down = (pc - steps[(size_t) d] + 12) % 12; // semitones down to this scale note (0: pc is in the scale)
        const int up = (steps[(size_t) d] - pc + 12) % 12;   // semitones up to it
        if (down < downBest)
        {
            downBest = down;
            below = d;
        }
        if (up < upBest)
        {
            upBest = up;
            above = d;
        }
    }
    return downBest <= upBest ? below : above;
}

// Stack's two harmony intervals. Key Off: Triad's fixed intervals. Key on: the scale's own triad on the
// played note's degree (its notes at degrees d + 2 and d + 4), so the quality follows the key (C major:
// C, Dm, Em, F, G, Am, B dim), placed by Voicing.
inline StackIntervals stackIntervals (int note, Triad t, Voicing v, int key, Scale scale)
{
    if (key < 0)
        return chromaticIntervals (t, v);
    const auto& steps = scaleSteps (scale);
    const int d = harmonyDegree (note, key, scale);
    auto above = [&] (int k) { return steps[(size_t) ((d + k) % 7)] + ((d + k) >= 7 ? 12 : 0) - steps[(size_t) d]; };
    return placeTriad (above (2), above (4), v);
}

struct ChordSettings
{
    ChordMode mode = ChordMode::Off;
    Triad triad = Triad::Major;
    Voicing voicing = kVoicing;
    int key = kKeyOff;
    Scale scale = kScale;
    float level = kChordLevel; // Chord Level, 0..1
    float retuneMs = 0.0f;                            // Retune Speed's glide, for every tone
    int memoryNotes = kDefaultMemoryNotes;            // Memory Notes, kMemoryNotesMin .. kMemoryNotesMax
};

// The chord's other tones (up to kMaxChordTones - 1, plus any still fading out), their levels and the
// gain law. All state is allocated in place (no heap): slots are copies of the live Tone.
//  - Every other tone is a full copy of the voice (Wave, Octave, Low Pass Filter) with its
//    own phase. It follows the shared level x Chord Level.
//  - Tones entering or leaving fade over 20 ms. A tone's pitch is phase-continuous, so it can glide
//    or jump (with the Retune Speed glide) without a click.
//  - Stack: two harmony tones, each the live T plus its interval (stackIntervals), the interval gliding
//    with Retune Speed when it changes (so a slide moves the whole chord in parallel).
//  - Memory: the last Memory Notes notes played (each live and voiced for kMemoryMinNoteMs), the live
//    note included, in their own octaves. The live note is the voice; when it moves on from a remembered
//    note, that note carries on in a copy of the voice (mid-cycle, no restart) whose level slides from the
//    live level to Chord Level over 20 ms, and the voice, now on the new note, fades in over 5 ms (both
//    start from the same point of the cycle, so without it the sum would jump). One more note drops the
//    oldest once it commits (until then, the remembered notes all still sound: one tone more than Memory
//    Notes for at most 80 ms). Replaying a remembered note makes it live again (its copy fades out) and
//    moves it to the newest.
//  - Memory clear (v0.5.4): after kMemoryClearMs with no pitch, whatever Note Release. If the voice is
//    still sounding then (Note Release Medium, Long), its tones ring on through the release; the next note
//    starts a fresh chord and they fade out over 20 ms under it. If the voice has closed first (Instant),
//    a note within the clear time rejoins the held chord: its remembered tones fade back in over 20 ms.
//    (Up to v0.5.3 memory cleared when the voice gate closed: setLegacyMemoryClear, tests only.)
//  - Gain: P = 1 (the live tone) + sum of each other tone's relative amplitude squared; the sum is scaled by
//    min(1, sqrt(2 / P)), so it is never more than +3 dB over a single note (Chord Level 100%, 3 tones:
//    -1.8 dB on each; the default 70% with 3 tones: P = 1.98, no change).
class ChordVoice
{
public:
    static constexpr int kSlots = 8; // up to kMaxChordTones - 1 sounding plus any fading out

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        fadeLen = msToSamples (fs, kToneFadeMs);
        liveInLen = msToSamples (fs, 5.0);
        commitLen = msToSamples (fs, kMemoryMinNoteMs);
        clearLen = msToSamples (fs, clearMs);
        for (auto& s : slots)
            s.tone.prepare (fs);
        reset();
    }

    void reset()
    {
        for (auto& s : slots)
        {
            s.role = Role::Free;
            s.presence.snap (0.0f);
            s.liveWeight.snap (0.0f);
        }
        mode = ChordMode::Off;
        clearMemory();
        unvoicedRun = 0;
        awaitingNote = true;
        liveIn.snap (1.0f);
        level.snap (settings.level);
        g = 1.0f;
    }

    // Once per block.
    void setVoice (const VoiceSettings& v)
    {
        for (auto& s : slots)
            s.tone.setVoice (v);
    }
    void setSettings (const ChordSettings& c)
    {
        settings = c;
        settings.level = std::clamp (c.level, 0.0f, 1.0f);
        settings.memoryNotes = std::clamp (c.memoryNotes, kMemoryNotesMin, kMemoryNotesMax);
        level.setTarget (settings.level, fadeLen);
        glideCoef = settings.retuneMs > 0.0f ? (float) (1.0 - std::exp (-1000.0 / (settings.retuneMs * fs))) : 1.0f;
    }

    // True while the engine must run the chord path: a mode on, or a tone still fading out.
    bool active() const
    {
        if (settings.mode != ChordMode::Off || mode != ChordMode::Off)
            return true;
        for (const auto& s : slots)
            if (s.role != Role::Free)
                return true;
        return false;
    }

    // One sample, before the live tone renders (so a copy of it starts where it is). live: the voice;
    // liveTarget / liveNote: its T and N; voiced: pitch detected now; voiceClosed: the voice gate had
    // fully closed (the chord has ended); silent: it's closed and stays closed this sample.
    void step (const Tone& live, float liveTarget, int liveNote, bool voiced, bool voiceClosed, bool silent)
    {
        if (settings.mode != mode)
            changeMode (live, liveTarget, liveNote);

        if (mode == ChordMode::Stack)
        {
            const auto iv = intervalsFor (liveNote);
            // The played note isn't gliding (a fresh note, Retune Fast, or arrived): the harmony snaps.
            const bool snap = std::equal_to<float>() (liveTarget, (float) liveNote);
            for (auto& s : slots)
                if (s.role == Role::Harmony && ! s.leaving)
                {
                    const float want = (float) (s.index == 0 ? iv.a : iv.b);
                    s.offset = snap ? want : s.offset + glideCoef * (want - s.offset);
                    s.pitch = liveTarget + s.offset;
                    s.note = liveNote + (s.index == 0 ? iv.a : iv.b);
                }
        }
        else if (mode == ChordMode::Memory)
        {
            if (legacyClear)
                stepLegacyClear (live, liveNote, voiced, voiceClosed);
            else
            {
                // kMemoryClearMs without pitch: memory clears. Tones still sounding ring on (the next note
                // fades them out: none is wanted then).
                if (voiced)
                    unvoicedRun = 0;
                else if (unvoicedRun < clearLen && ++unvoicedRun == clearLen)
                {
                    clearMemory();
                    awaitingNote = true;
                }
                if (voiceClosed) // the voice has ended, so its tones are silent; memory (if still held) waits for the next note
                {
                    for (auto& s : slots)
                        if (s.role == Role::Remembered)
                            s.role = Role::Free;
                    memLive = -1; // the last live note is remembered now; the next note brings every remembered one back in
                    liveVoiced = 0;
                    liveCommitted = false;
                    awaitingNote = true;
                }
                if (voiced || ! awaitingNote)
                {
                    awaitingNote = false;
                    if (memCount > settings.memoryNotes) // Memory Notes turned down: the oldest go
                    {
                        trimMemory (settings.memoryNotes);
                        reconcile (live, -1, 0.0f);
                    }
                    registerLive (live, liveNote, voiced);
                }
            }
            for (auto& s : slots)
                if (s.role == Role::Remembered)
                    s.pitch += glideCoef * ((float) s.note - s.pitch); // a copy made mid-glide finishes it
        }
        lastLiveTarget = liveTarget;
        isSilent = silent;
    }

    // One sample: the other tones at their levels. voiceLevel: the live tone's level.
    float renderOthers (float voiceLevel)
    {
        peakSum = 0.0f;
        if (isSilent) // the voice is closed: nothing sounds, so fades can finish at once
        {
            level.snap (level.target);
            liveIn.snap (liveIn.target);
            for (auto& s : slots)
                if (s.role != Role::Free)
                {
                    s.presence.snap (s.presence.target);
                    s.liveWeight.snap (s.liveWeight.target);
                    if (s.presence.current <= 0.0f)
                        s.role = Role::Free;
                }
            g = 1.0f;
            return 0.0f;
        }

        const float cl = level.next();
        const float base = voiceLevel * cl;
        float power = lg * lg, sum = 0.0f; // the live tone (its fade-in after a hand-off counted, so the gain doesn't jump)
        for (auto& s : slots)
        {
            if (s.role == Role::Free)
                continue;
            const float p = s.presence.next(), w = s.liveWeight.next();
            const float amp = p * (w + (1.0f - w) * cl);
            sum += s.tone.render (s.pitch, p * (w * voiceLevel + (1.0f - w) * base));
            peakSum += s.tone.peak();
            power += amp * amp;
            if (p <= 0.0f && ! s.presence.isSmoothing())
                s.role = Role::Free;
        }
        g = power > kChordMaxPower ? std::sqrt (kChordMaxPower / power) : 1.0f;
        return sum;
    }

    // Memory: the remembered notes held for the next note, besides the one being played (all of memory while
    // nothing is: livePlaying false). 0 outside Memory. For the HELD light.
    int heldNotes (bool livePlaying) const
    {
        if (mode != ChordMode::Memory || settings.mode != ChordMode::Memory)
            return 0;
        int n = 0;
        for (int k = 0; k < memCount; ++k)
            if (! livePlaying || memNotes[(size_t) k] != memLive)
                ++n;
        return n;
    }

    // Tests only: memory clears when the voice gate closes, as up to v0.5.3 (not after kMemoryClearMs without pitch).
    void setLegacyClear (bool on) { legacyClear = on; }
    // Tests only: another clear time (v0.5.5's 250 ms, for the gap measurements). Call before prepare().
    void setClearMs (double ms) { clearMs = ms; }

    float gain() const { return g; } // the gain law's scale on the whole sum, this sample
    float othersPeak() const { return peakSum; } // the bound on renderOthers()'s size, this sample (Tone::peak, summed)
    float liveGain() { return lg = liveIn.next(); } // the live tone's fade-in after a hand-off (1 otherwise); once per sample, first

    // The tones sounding (not fading out), live first, as notes in the played note's space (before
    // Octave); for the readout. Returns the count.
    bool slotSounding (int k) const { return slots[(size_t) k].role != Role::Free && ! slots[(size_t) k].leaving; }
    float slotPitch (int k) const { return slots[(size_t) k].pitch; }
    float slotPresence (int k) const { return slots[(size_t) k].presence.current; } // v0.5.11, the pitch trace: 0..1 entering / leaving

    int tones (int liveNote, std::array<int, 8>& out) const
    {
        int n = 0;
        out[(size_t) n++] = liveNote;
        for (const auto& s : slots)
            if (s.role != Role::Free && ! s.leaving && n < (int) out.size())
                out[(size_t) n++] = s.note;
        return n;
    }

private:
    enum class Role { Free, Harmony, Remembered };

    struct Slot
    {
        Tone tone;
        Role role = Role::Free;
        int index = 0;        // Harmony: which interval (0, 1)
        int note = 0;         // the note it plays (played-note space; Remembered: as played)
        float pitch = 0.0f;   // the T it renders at
        float offset = 0.0f;  // Harmony: gliding interval from the live T
        LinearRamp presence;  // 0..1: entering / leaving
        LinearRamp liveWeight;// 1: at the live note's level; 0: at Chord Level
        bool leaving = false;
    };

    void changeMode (const Tone& live, float liveTarget, int liveNote)
    {
        mode = settings.mode;
        for (auto& s : slots)
            if (s.role != Role::Free)
                leave (s);
        clearMemory(); // Memory starts empty; the note playing now joins it after 80 ms
        unvoicedRun = 0;
        awaitingNote = false;
        if (mode == ChordMode::Stack)
        {
            const auto iv = intervalsFor (liveNote);
            for (int k = 0; k < 2; ++k)
            {
                auto& s = enter (live);
                s.role = Role::Harmony;
                s.index = k;
                s.offset = (float) (k == 0 ? iv.a : iv.b);
                s.pitch = liveTarget + s.offset;
                s.note = liveNote + (k == 0 ? iv.a : iv.b);
            }
        }
    }

    // Memory: a note change starts a new live note (the one it left carries on if remembered); one that has
    // been live and voiced for kMemoryMinNoteMs commits.
    void registerLive (const Tone& live, int liveNote, bool voiced)
    {
        if (liveNote != memLive)
        {
            const int previous = memLive;
            memLive = liveNote;
            liveVoiced = 0;
            liveCommitted = false;
            reconcile (live, previous, lastLiveTarget);
        }
        if (voiced && ! liveCommitted && ++liveVoiced >= commitLen)
        {
            commit();
            reconcile (live, -1, 0.0f);
        }
    }

    // Up to v0.5.3 (tests only): memory clears when the voice gate has fully closed (Note Release's end), so
    // the gate decides both.
    void stepLegacyClear (const Tone& live, int liveNote, bool voiced, bool voiceClosed)
    {
        if (voiceClosed) // the live note's release has ended: so has the chord (it was silent already)
        {
            clearMemory();
            for (auto& s : slots)
                if (s.role == Role::Remembered)
                    s.role = Role::Free;
        }
        if (! voiceClosed || voiced)
            registerLive (live, liveNote, voiced);
    }

    // Memory: the live note has lasted kMemoryMinNoteMs. Already remembered (a replay): it moves to the
    // newest. Otherwise it's added, and one past Memory Notes drops the oldest.
    void commit()
    {
        liveCommitted = true;
        int kept = 0;
        for (int k = 0; k < memCount; ++k)
            if (memNotes[(size_t) k] != memLive)
                memNotes[(size_t) kept++] = memNotes[(size_t) k];
        memCount = kept;
        trimMemory (settings.memoryNotes - 1);
        memNotes[(size_t) memCount++] = memLive;
    }

    // Keep the newest n remembered notes.
    void trimMemory (int n)
    {
        const int drop = memCount - std::max (0, n);
        if (drop <= 0)
            return;
        for (int k = 0; k + drop < memCount; ++k)
            memNotes[(size_t) k] = memNotes[(size_t) (k + drop)];
        memCount -= drop;
    }

    void clearMemory()
    {
        memCount = 0;
        memLive = -1;
        liveVoiced = 0;
        liveCommitted = false;
    }

    // Memory: the other tones are every remembered note but the live one. inheritNote: the note the
    // live tone just left (-1: none), which if still wanted carries on in a copy of the live tone,
    // at inheritTarget (its T then), its level sliding from the live note's to Chord Level.
    void reconcile (const Tone& live, int inheritNote, float inheritTarget)
    {
        auto wanted = [this] (int n) {
            if (n == memLive)
                return false;
            for (int k = 0; k < memCount; ++k)
                if (memNotes[(size_t) k] == n)
                    return true;
            return false;
        };
        for (auto& s : slots)
            if (s.role == Role::Remembered && ! s.leaving && ! wanted (s.note))
                leave (s);
        for (int k = 0; k < memCount; ++k)
        {
            const int n = memNotes[(size_t) k];
            if (! wanted (n) || sounding (n))
                continue;
            if (n == inheritNote)
            {
                auto& s = freeSlot();
                s.tone = live;
                s.role = Role::Remembered;
                s.note = n;
                s.pitch = inheritTarget;
                s.leaving = false;
                s.presence.snap (1.0f);
                s.liveWeight.snap (1.0f);
                s.liveWeight.setTarget (0.0f, fadeLen);
                liveIn.snap (0.0f);
                liveIn.setTarget (1.0f, liveInLen);
            }
            else if (Slot* fading = find (n))
            {
                fading->leaving = false; // still fading out: back in from where it is
                fading->presence.setTarget (1.0f, fadeLen);
            }
            else
            {
                auto& s = enter (live);
                s.role = Role::Remembered;
                s.note = n;
                s.pitch = (float) n;
            }
        }
    }

    bool sounding (int note) const
    {
        for (const auto& s : slots)
            if (s.role == Role::Remembered && ! s.leaving && s.note == note)
                return true;
        return false;
    }

    Slot* find (int note)
    {
        for (auto& s : slots)
            if (s.role == Role::Remembered && s.note == note)
                return &s;
        return nullptr;
    }

    StackIntervals intervalsFor (int liveNote) const { return stackIntervals (liveNote, settings.triad, settings.voicing, settings.key, settings.scale); }

    // A new tone: a copy of the live one (so its filter and ramps match), fading in from 0.
    Slot& enter (const Tone& live)
    {
        auto& s = freeSlot();
        s.tone = live;
        s.leaving = false;
        s.presence.snap (0.0f);
        s.presence.setTarget (1.0f, fadeLen);
        s.liveWeight.snap (0.0f);
        return s;
    }

    void leave (Slot& s)
    {
        s.leaving = true;
        s.presence.setTarget (0.0f, fadeLen);
    }

    // A free slot, or (all busy, only possible with fast churn) the quietest one fading out.
    Slot& freeSlot()
    {
        Slot* best = nullptr;
        for (auto& s : slots)
        {
            if (s.role == Role::Free)
                return s;
            if (s.leaving && (best == nullptr || s.presence.current < best->presence.current))
                best = &s;
        }
        return best != nullptr ? *best : slots.back();
    }

    double fs = 48000.0;
    int fadeLen = 1, liveInLen = 1, commitLen = 1, clearLen = 1;
    ChordSettings settings;
    ChordMode mode = ChordMode::Off;
    LinearRamp level;  // Chord Level, smoothed over 20 ms
    LinearRamp liveIn; // the live tone after a hand-off
    float glideCoef = 1.0f, lastLiveTarget = 0.0f, g = 1.0f, lg = 1.0f, peakSum = 0.0f;
    bool isSilent = true;
    std::array<Slot, kSlots> slots;

    // Memory, oldest first; the live note and how long it has been live and voiced; how long pitch has been
    // lost (up to the clear); and whether a note change waits for pitch (after a clear, or the voice closing).
    std::array<int, kMemoryNotesMax> memNotes {};
    int memCount = 0, memLive = -1, liveVoiced = 0, unvoicedRun = 0;
    bool liveCommitted = false, awaitingNote = true, legacyClear = false;
    double clearMs = kMemoryClearMs;
};

} // namespace gtt
