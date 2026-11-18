#pragma once

// v0.6: Chord as three parameters (Chord: Off / Freeze / Harmonize; Chord Size: 2 / 3 / 4 notes; Key: the twelve major /
// relative-minor pairs) in place of v0.5.6 ... v0.5.11's single 16-way choice. Pure; no JUCE.
//
// The engine still takes the old choice's EngineParams (chordMode, memoryNotes, key): the three are turned into the old index
// (legacyChordIndexFor) and read by gtt::chordChoiceFor, so every combination plays exactly what that old choice played. The
// inverse (chordSplitFor) sets the three from an old index (GTTSnapshot's checks).

#include "DSP/Chord.h"

namespace gtt
{
enum class ChordType
{
    Off,
    Freeze,   // Memory in the engine
    Harmonize // Stack in the engine
};
constexpr int kChordTypeChoices = 3;
constexpr int kChordSizeChoices = kMemoryNotesMax - kMemoryNotesMin + 1; // 2 / 3 / 4 notes
constexpr int kDefaultChordSize = kDefaultMemoryNotes - kMemoryNotesMin; // index of 3 notes
constexpr int kChordKeyChoices = 12;
constexpr int kDefaultChordKey = 0; // C maj / A min

// The three controls' indices -> v0.5.11's Chord index (0 Off, 1 .. 3 Freeze 2 / 3 / 4, 4 .. 15 Harmonize in C .. B).
// Size matters only in Freeze, Key only in Harmonize.
inline int legacyChordIndexFor (int type, int sizeIndex, int keyIndex)
{
    sizeIndex = std::clamp (sizeIndex, 0, kChordSizeChoices - 1);
    keyIndex = std::clamp (keyIndex, 0, kChordKeyChoices - 1);
    switch ((ChordType) std::clamp (type, 0, kChordTypeChoices - 1))
    {
        case ChordType::Freeze: return kFirstFreezeChoice + sizeIndex;
        case ChordType::Harmonize: return kFirstHarmonizeChoice + keyIndex;
        case ChordType::Off: break;
    }
    return 0;
}

inline ChordChoice chordChoiceForSplit (int type, int sizeIndex, int keyIndex)
{
    return chordChoiceFor (legacyChordIndexFor (type, sizeIndex, keyIndex));
}

// A v0.5.11 Chord index -> the three. What the old choice doesn't say stays at its default (Off: 3 notes, C maj / A min;
// Freeze: the key C maj / A min; Harmonize: 3 notes).
struct ChordSplit
{
    int type = (int) ChordType::Off, sizeIndex = kDefaultChordSize, keyIndex = kDefaultChordKey;
};
inline ChordSplit chordSplitFor (int legacyIndex)
{
    legacyIndex = std::clamp (legacyIndex, 0, kChordChoices - 1);
    ChordSplit s;
    if (legacyIndex >= kFirstHarmonizeChoice)
    {
        s.type = (int) ChordType::Harmonize;
        s.keyIndex = legacyIndex - kFirstHarmonizeChoice;
    }
    else if (legacyIndex >= kFirstFreezeChoice)
    {
        s.type = (int) ChordType::Freeze;
        s.sizeIndex = legacyIndex - kFirstFreezeChoice;
    }
    return s;
}
} // namespace gtt
