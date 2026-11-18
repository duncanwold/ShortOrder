#pragma once

// Chord names for the readout. Pure; no JUCE.

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace gtt
{

inline const char* pitchClassName (int midi)
{
    static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((midi % 12) + 12) % 12];
}

// The Key choice (v0.5.2): each major key with its relative minor (3 semitones below), spelled as usual
// (Db, Eb, Ab, Bb majors; their minors follow). key: 0 .. 11 = the major key's tonic, C .. B, which is
// what Stack uses (the major scale on it: the relative minor's triads are the same).
inline const char* keyMajorName (int key)
{
    static const char* const names[12] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    return names[((key % 12) + 12) % 12];
}
inline const char* keyMinorName (int key)
{
    static const char* const names[12] = { "A", "Bb", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#" };
    return names[((key % 12) + 12) % 12];
}
// The parameter's label: "C maj / A min".
inline std::string keyLabel (int key) { return std::string (keyMajorName (key)) + " maj / " + keyMinorName (key) + " min"; }

// Semitones above the lower note, within an octave (0 never occurs: two pitch classes differ).
inline const char* intervalName (int semitones)
{
    static const char* const names[12] = { "8ve", "m2", "M2", "m3", "M3", "4th", "tritone", "5th", "m6", "M6", "m7", "M7" };
    return names[((semitones % 12) + 12) % 12];
}

// A set of notes (MIDI, any order, duplicates allowed) named by its pitch classes, the lowest note as
// the bass:
//  - three pitch classes forming a major, minor, diminished or augmented triad: the root and quality,
//    with a slash for an inversion, then the pitch classes from the bass up: "Em/G (G B E)", "C (C E G)",
//    "B\xc2\xb0 (B D F)", "Caug (C E G#)" (augmented is symmetric: the bass is its root);
//  - two: the interval from the bass, "A+E (5th)", "E+A (4th)";
//  - one: the note, "A" ("A (8ve)" if it's in more than one octave);
//  - anything else (other three-note sets, four or more): the pitch classes from the bass up, "C D E".
// Empty: "".
inline std::string chordName (std::vector<int> notes)
{
    if (notes.empty())
        return {};
    std::sort (notes.begin(), notes.end());
    std::vector<int> pcs; // pitch classes, from the bass up, each once
    for (int n : notes)
    {
        const int pc = ((n % 12) + 12) % 12;
        if (std::find (pcs.begin(), pcs.end(), pc) == pcs.end())
            pcs.push_back (pc);
    }
    auto list = [&pcs] {
        std::string s;
        for (size_t k = 0; k < pcs.size(); ++k)
            s += (k == 0 ? "" : " ") + std::string (pitchClassName (pcs[k]));
        return s;
    };

    const int bass = pcs[0];
    if (pcs.size() == 1)
        return std::string (pitchClassName (bass)) + (notes.size() > 1 ? " (8ve)" : "");
    if (pcs.size() == 2)
        return std::string (pitchClassName (bass)) + "+" + pitchClassName (pcs[1]) + " (" + intervalName (pcs[1] - bass) + ")";
    if (pcs.size() == 3)
    {
        struct Quality
        {
            int third, fifth;
            const char* suffix;
        };
        static const Quality qualities[4] = { { 4, 7, "" }, { 3, 7, "m" }, { 3, 6, "\xc2\xb0" }, { 4, 8, "aug" } };
        for (int k = 0; k < 3; ++k) // the bass first, so an augmented triad takes it as its root
        {
            const int root = pcs[(size_t) k];
            std::array<int, 2> up { (pcs[(size_t) ((k + 1) % 3)] - root + 12) % 12, (pcs[(size_t) ((k + 2) % 3)] - root + 12) % 12 };
            std::sort (up.begin(), up.end());
            for (const auto& q : qualities)
                if (up[0] == q.third && up[1] == q.fifth)
                    return std::string (pitchClassName (root)) + q.suffix + (root != bass ? std::string ("/") + pitchClassName (bass) : std::string()) + " (" + list() + ")";
        }
    }
    return list();
}

} // namespace gtt
