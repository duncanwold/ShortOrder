#pragma once

// Guitar positions (standard tuning) for the readout and the fretboard. Pure; no JUCE.

#include <array>
#include <cmath>
#include <string>

namespace gtt
{

struct TabPosition
{
    int string = 0;         // 0 = low E, 1 = A, 2 = D, 3 = G, 4 = B, 5 = high e
    int fret = 0;
    bool belowLowE = false; // below open low E: the whole neck tracks (marked 0 on low E)
};

inline constexpr std::array<int, 6> kOpenStringMidi { 40, 45, 50, 55, 59, 64 }; // E2 A2 D3 G3 B3 E4
inline constexpr std::array<const char*, 6> kStringNames { "E", "A", "D", "G", "B", "e" };

// The highest-pitched string whose open note is at or below n; fret = n - that open note.
// Frets 0..4 on every string but high e, which can go higher.
inline TabPosition tabPositionFor (int n)
{
    if (n < kOpenStringMidi[0])
        return { 0, 0, true };
    int s = 5;
    while (kOpenStringMidi[(size_t) s] > n)
        --s;
    return { s, n - kOpenStringMidi[(size_t) s], false };
}

// "A string fret 1", or "below open low E (whole neck)".
inline std::string tabPositionText (const TabPosition& t)
{
    if (t.belowLowE)
        return "below open low E (whole neck)";
    return std::string (kStringNames[(size_t) t.string]) + " string fret " + std::to_string (t.fret);
}
// The latency line's wording (v0.5.7): "B string, open", "e string, fret 5".
inline std::string tabPositionLabel (const TabPosition& t)
{
    if (t.belowLowE)
        return "below open low E";
    return std::string (kStringNames[(size_t) t.string]) + " string, " + (t.fret == 0 ? std::string ("open") : "fret " + std::to_string (t.fret));
}

// ---------------------------------------------------------------------------
// Fretboard: what each position will do at the current window.
// ---------------------------------------------------------------------------
constexpr int kFretCount = 21; // Telecaster; positions are frets 0 (open) .. kFretCount
constexpr std::array<int, 8> kSingleInlayFrets { 3, 5, 7, 9, 15, 17, 19, 21 };
constexpr int kDoubleInlayFret = 12;

inline int positionNote (int string, int fret) { return kOpenStringMidi[(size_t) string] + fret; }
// The board's highest note: high e at fret kFretCount (C#6, 1109 Hz). Above it (harmonics, slide): ABOVE BOARD (v0.5.11).
constexpr int kTopBoardNote = 64 + kFretCount;

enum class FretStatus
{
    Reliable,   // green, filled: at or above the lowest reliable note
    InTuneOnly, // yellow, half: at or above the window's own pitch, below lowest reliable -- tracks in tune, not flat
    WontTrack   // red, empty: below the window's own pitch
};

// windowFloorHz: the lowest frequency the window can track (fs / tauMax);
// lowestReliable: the readout's lowest reliable note (MIDI).
inline FretStatus classifyPosition (int midiNote, double windowFloorHz, int lowestReliable)
{
    if (midiNote >= lowestReliable)
        return FretStatus::Reliable;
    const double hz = 440.0 * std::pow (2.0, (midiNote - 69) / 12.0);
    return hz >= windowFloorHz ? FretStatus::InTuneOnly : FretStatus::WontTrack;
}

// ---------------------------------------------------------------------------
// v0.5.4: what the fretboard and line 1 show of the detected note, from the main detector and the low-range display
// detector (LowRange.h).
//  - The main detector has a pitch: PLAYING NOW at its note, as before; above the board's top (C#6, v0.5.11) also ABOVE BOARD:
//    a marker past the last fret, and the note on line 1.
//  - Only the low detector has one, below the main detector's range (under its lowest reliable note): BELOW RANGE at
//    that note, in its own style; below open low E (C2 .. D#2) it can't be drawn, so line 1 says so in text.
//  - The 2+ NOTES? hint: TOO LOW instead while the low detector hears a single note below the range (it fires on a
//    single low note too, since the main detector reads it as unsure); 2+ NOTES? when it doesn't.
//  - v0.6.2 (docs/reports/QA-REPORT-RC1.md issue 10): the low detector hearing a single note below the range wins over a voiced main detector
//    too, whose reading of a note under its window is then its octave-up or its longest lag: BELOW RANGE as well as PLAYING NOW
//    (the header says too low; the neck rings the low note beside the played one). Up to v0.6.1 PLAYING NOW alone
//    (lowWinsOverVoiced = false, tests only).
// ---------------------------------------------------------------------------
constexpr bool kLowRangeWinsOverVoiced = true;
enum class PitchHint { None, TwoPlusNotes, TooLow };

struct NoteDisplay
{
    int playing = -1;            // PLAYING NOW (MIDI), or -1
    int aboveBoard = -1;         // v0.5.11: the playing note when it's above the board's top (kTopBoardNote), or -1
    int belowRange = -1;         // BELOW RANGE (MIDI), or -1
    bool belowFretboard = false; // belowRange is under open low E: text only
    PitchHint hint = PitchHint::None;
};

// lowVoiced: the low detector hears a single note (LowRangeReading::voiced); lowestReliable: the main window's.
inline NoteDisplay noteDisplayFor (bool mainVoiced, int mainNote, bool lowVoiced, int lowNote, int lowestReliable, bool twoPlusNotes,
                                   bool lowWinsOverVoiced = kLowRangeWinsOverVoiced)
{
    NoteDisplay d;
    const bool lowBelow = lowVoiced && lowNote < lowestReliable;
    if (mainVoiced)
    {
        d.playing = mainNote;
        d.aboveBoard = mainNote > kTopBoardNote ? mainNote : -1;
    }
    if (lowBelow && (! mainVoiced || lowWinsOverVoiced))
    {
        d.belowRange = lowNote;
        d.belowFretboard = lowNote < kOpenStringMidi[0];
    }
    if (twoPlusNotes)
        d.hint = lowBelow ? PitchHint::TooLow : PitchHint::TwoPlusNotes;
    return d;
}

} // namespace gtt
