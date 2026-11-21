// v0.6: the Chord split (the three Chord parameters read into exactly the EngineParams of v0.5.11's 16-way Chord choice) and the
// latency knob's detents. Included by OfflineTests.cpp.

// The Chord split (v0.6): Chord (Off / Freeze / Harmonize) x Chord Size (2 / 3 / 4 notes) x Key (12) read, through
// legacyChordIndexFor, into exactly the EngineParams of v0.5.11's 16 choices: all 1 + 3 + 12 old choices are reached, each by the
// combinations the spec names (Freeze + size n = Freeze n, whatever the key; Harmonize + key k = Harmonize k, whatever the size;
// Off whatever both), and a v0.5.11 index restores onto the three and back. Audio: on a Freeze phrase (the memory phrase) and a
// Harmonize phrase (the regression phrase), every combination = the old choice, bit for bit.
void testChordSplit()
{
    auto paramsFor = [] (const ChordChoice& c) {
        EngineParams p;
        p.chordMode = c.mode;
        p.key = c.key;
        p.memoryNotes = c.memoryNotes;
        return p;
    };
    auto same = [] (const ChordChoice& a, const ChordChoice& b) { return a.mode == b.mode && a.key == b.key && a.memoryNotes == b.memoryNotes; };
    bool tableOk = true, roundOk = true;
    std::set<int> reached;
    juce::String table;
    for (int type = 0; type < kChordTypeChoices; ++type)
        for (int size = 0; size < kChordSizeChoices; ++size)
            for (int key = 0; key < kChordKeyChoices; ++key)
            {
                const int legacy = legacyChordIndexFor (type, size, key);
                const int want = type == 0 ? 0 : type == 1 ? freezeChoice (kMemoryNotesMin + size) : harmonizeChoice (key);
                tableOk = tableOk && legacy == want && same (chordChoiceForSplit (type, size, key), chordChoiceFor (want));
                reached.insert (legacy);
            }
    for (int k = 0; k < kChordChoices; ++k)
    {
        const auto s = chordSplitFor (k);
        const auto c = chordChoiceFor (k);
        roundOk = roundOk && legacyChordIndexFor (s.type, s.sizeIndex, s.keyIndex) == k && same (chordChoiceForSplit (s.type, s.sizeIndex, s.keyIndex), c);
        // Off keeps the defaults; Freeze keeps the default key, Harmonize the default size.
        roundOk = roundOk && (s.type == 1 || s.sizeIndex == kDefaultChordSize) && (s.type == 2 || s.keyIndex == kDefaultChordKey);
        table << "\n      " << k << " -> " << (s.type == 0 ? "Off" : s.type == 1 ? "Freeze" : "Harmonize") << ", " << (kMemoryNotesMin + s.sizeIndex) << " notes, "
              << keyLabel (s.keyIndex) << " -> mode " << (int) c.mode << " key " << c.key << " notes " << c.memoryNotes;
    }
    check (tableOk && (int) reached.size() == kChordChoices, juce::String::formatted ("Chord x Chord Size x Key: all %d combinations read as v0.5.11's choices, every one of the %d reached",
                                                                                     kChordTypeChoices * kChordSizeChoices * kChordKeyChoices, (int) reached.size()));
    check (roundOk, "each v0.5.11 Chord index restores onto the three and reads back as itself (Off / Freeze keep the default key, Off / Harmonize the default size)" + table);
    check (kDefaultChordSize == kDefaultMemoryNotes - kMemoryNotesMin && kDefaultChordKey == 0 && legacyChordIndexFor (0, kDefaultChordSize, kDefaultChordKey) == 0,
           "defaults: Chord Off, 3 notes, C maj / A min (= v0.5.11's default, Off)");

    // Audio: every combination on both phrases = the old choice's EngineParams through the engine, sample for sample.
    const auto freezeIn = memoryPhrase (kFs), harmonizeIn = regressionPhrase (kFs);
    std::vector<std::future<int>> diffs;
    for (int type = 0; type < kChordTypeChoices; ++type)
        for (int size = 0; size < kChordSizeChoices; ++size)
            for (int key = 0; key < kChordKeyChoices; key += (type == 2 ? 1 : 5))
                for (const auto* in : { &freezeIn, &harmonizeIn })
                    diffs.push_back (std::async (std::launch::async, [=, &paramsFor] {
                        const int legacy = type == 0 ? 0 : type == 1 ? freezeChoice (kMemoryNotesMin + size) : harmonizeChoice (key);
                        Engine a, b;
                        a.prepare (kFs, 64);
                        b.prepare (kFs, 64);
                        return compareSamples (runEngine (a, *in, paramsFor (chordChoiceForSplit (type, size, key))), runEngine (b, *in, chordChoiceParams (legacy))).values;
                    }));
    int bad = 0;
    for (auto& d : diffs)
        bad += d.get() > 0 ? 1 : 0;
    check (bad == 0, juce::String::formatted ("audio: %d renders (each combination on a Freeze phrase and a Harmonize phrase) identical to the old choice", (int) diffs.size()));
}

// v0.7: the latency parameter's host text (Latency.h, Params.cpp): the faceplate's ms with the unit, the voice's delay D (1.1: "4.1 ms"
// at the default, 48 kHz, also the rate before one is known; 1.0.x showed L, "8.3 ms"); at every detent and every common rate the text
// reads back as that detent's value exactly (text -> value -> text the same), every detent's text is its own; a value between detents
// reads back as the same text.
void testLatencyText()
{
    check (latencyHostText (48000.0, defaultWindowHz()) == "4.1 ms" && latencyHostText (0.0, defaultWindowHz()) == "4.1 ms",
           juce::String ("the default reads \"") + latencyHostText (48000.0, defaultWindowHz()) + "\" at 48 kHz and before a rate is known (\""
               + latencyHostText (0.0, defaultWindowHz()) + "\")");

    bool roundTrip = true, unique = true, faceplate = true;
    juce::String table = "\n      k   Hz        host text @ 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz";
    for (int k = 0; k < kLatencyDetents; ++k)
    {
        const float hz = latencyDetentHz (k);
        table << juce::String::formatted ("\n      %-2d %8.3f  ", k, hz);
        for (double fs : kCommonSampleRates)
        {
            const auto text = latencyHostText (fs, hz);
            const double ms = juce::String (text).getDoubleValue();
            const auto back = (float) latencyHzForMs (fs, ms);
            roundTrip = roundTrip && back == hz && latencyHostText (fs, back) == text;
            faceplate = faceplate && text == latencyMsText (fs, hz) + " ms";
            for (int j = 0; j < k; ++j)
                unique = unique && latencyHostText (fs, latencyDetentHz (j)) != text;
            table << " " << juce::String (text).paddedLeft (' ', 7);
        }
    }
    check (roundTrip, "text -> value at every detent and every common rate: exactly the detent's value, and its text again" + table);
    check (unique, "every detent's text is its own at every common rate");
    check (faceplate, "the host text is the faceplate's readout (latencyMsText) with \" ms\"");

    // Between detents (and typed values): the window whose D is that many ms; the same text back; clamped to the range.
    bool between = true;
    for (double fs : kCommonSampleRates)
        for (int k = 0; k + 1 < kLatencyDetents; ++k)
        {
            const double hz = std::sqrt ((double) latencyDetentHz (k) * latencyDetentHz (k + 1));
            const auto text = latencyHostText (fs, hz);
            const double back = latencyHzForMs (fs, juce::String (text).getDoubleValue());
            // (1.1: the voice's delay has half L's resolution, so a text between detents can be a detent's own, which reads back as
            // that detent's stored value: within the parameter's range as it stores it, a float.)
            between = between && latencyHostText (fs, back) == text && back >= (float) derivedWindowMinHz() && back <= (float) kWindowMaxHz;
        }
    const bool clamped = latencyHzForMs (48000.0, 50.0) == derivedWindowMinHz() && latencyHzForMs (48000.0, 1.0) == kWindowMaxHz
                         && latencyHzForMs (48000.0, 0.0) == kWindowMaxHz;
    check (between && clamped, "between detents: the text reads back as the same text; 50 ms / 1 ms / 0 clamp to the range's ends");
}

// The latency knob's detents (v0.6, Latency.h): 15, k = 0 / 4 / 14 exactly the window's minimum / default / top, equal ratios in
// between (4 steps G3 .. B3, 10 steps B3 .. A4); each detent's lowest reliable note (the fretboard's) is G3 + k at every common rate,
// also after the parameter's skewed normalised round trip; turning right (k up) lowers the latency at every rate; the readout text at
// 44.1 / 48 / 96 kHz; a value between detents sits at its true position.
void testLatencyDetents()
{
    const double windowMin = derivedWindowMinHz(), windowMax = kWindowMaxHz;
    juce::NormalisableRange<float> range ((float) windowMin, (float) windowMax); // the parameter's range (Params.cpp)
    range.setSkewForCentre ((float) std::sqrt (windowMin * windowMax));
    const float k0 = latencyDetentHz (0), k4 = latencyDetentHz (4), k14 = latencyDetentHz (14);
    const bool exact = k0 == (float) windowMin && k4 == defaultWindowHz() && k14 == (float) windowMax && kLatencyDetents == 15;
    check (exact, juce::String::formatted ("15 detents; k = 0 / 4 / 14 = %.9g / %.9g / %.9g Hz: exactly the window's minimum, default and top", k0, k4, k14));

    bool ratios = true;
    const double low = std::pow ((double) k4 / k0, 0.25), high = std::pow ((double) k14 / k4, 0.1);
    for (int k = 1; k < kLatencyDetents; ++k)
        ratios = ratios && std::abs ((double) latencyDetentHz (k) / latencyDetentHz (k - 1) / (k <= 4 ? low : high) - 1.0) < 1e-6;
    check (ratios, juce::String::formatted ("equal ratios per step: x%.5f (G3 .. B3, 4 steps), x%.5f (B3 .. A4, 10 steps)", low, high));

    bool notes = true, monotone = true, positions = true;
    juce::String table = "\n      k  note     Hz        ms @ 44.1 / 48 / 96 kHz   (lowest reliable at 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz)";
    static const char* const names[15] = { "G3", "G#3", "A3", "A#3", "B3", "C4", "C#4", "D4", "D#4", "E4", "F4", "F#4", "G4", "G#4", "A4" };
    for (int k = 0; k < kLatencyDetents; ++k)
    {
        const float hz = latencyDetentHz (k);
        const float roundTrip = range.convertFrom0to1 (range.convertTo0to1 (hz));
        juce::String lows;
        for (double fs : kCommonSampleRates)
        {
            const int n = lowestReliableNote (fs, hz);
            notes = notes && n == kPlayableLowestNote + k && lowestReliableNote (fs, roundTrip) == n;
            lows << n << " ";
            if (k > 0)
                monotone = monotone && voiceDelayFor (fs, hz) <= voiceDelayFor (fs, latencyDetentHz (k - 1));
        }
        positions = positions && std::abs (latencyKnobPosition (hz) - k) < 1e-5 && nearestLatencyDetent (hz) == k && nearestLatencyDetent (roundTrip) == k;
        table << juce::String::formatted ("\n      %-2d %-4s %8.3f   %s / %s / %s ms   (", k, names[k], hz, latencyMsText (44100.0, hz).c_str(), latencyMsText (48000.0, hz).c_str(),
                                          latencyMsText (96000.0, hz).c_str())
              << lows.trim() << ")";
    }
    check (notes, "every detent's lowest reliable note is G3 + k at every common rate (also after the parameter's normalised round trip); none nudged" + table);
    check (monotone && latencyMs (48000.0, k14) < latencyMs (48000.0, k0), "turning right (k up) lowers the latency at every common rate: 5.2 ms (k = 0) .. 2.4 ms (k = 14) at 48 kHz");
    check (positions, "each detent sits at its own position on the knob (and the nearest detent to its value is itself)");

    // The readout text at 44.1 / 48 / 96 kHz: the default, both ends. 1.1: the voice's delay D (half of 1.0.x's 8.3 / 10.4 / 4.7); at
    // 96 kHz the default's D is 399 samples, 4.16 ms, so it reads 4.2 (at 44.1 and 48 kHz 4.15 ms, 4.1).
    const bool text = latencyMsText (48000.0, k4) == "4.1" && latencyMsText (48000.0, k0) == "5.2" && latencyMsText (48000.0, k14) == "2.4"
                      && latencyMsText (44100.0, k4) == "4.1" && latencyMsText (96000.0, k4) == "4.2" && latencyMsText (44100.0, k14) == "2.4"
                      && latencyMsText (96000.0, k14) == "2.4" && latencyMsText (44100.0, k0) == "5.2" && latencyMsText (96000.0, k0) == "5.2";
    check (text, juce::String ("readout: default ") + latencyMsText (44100.0, k4) + " / " + latencyMsText (48000.0, k4) + " / " + latencyMsText (96000.0, k4)
                     + " ms, fully left " + latencyMsText (44100.0, k0) + " / " + latencyMsText (48000.0, k0) + " / " + latencyMsText (96000.0, k0) + ", fully right "
                     + latencyMsText (44100.0, k14) + " / " + latencyMsText (48000.0, k14) + " / " + latencyMsText (96000.0, k14) + " (44.1 / 48 / 96 kHz)");

    // Between detents (a host's automation): the true position, the true latency and the true lowest note.
    const float between = std::sqrt (latencyDetentHz (6) * latencyDetentHz (7)); // halfway (geometrically) between C#4 and D4
    const double pos = latencyKnobPosition (between);
    check (std::abs (pos - 6.5) < 1e-4 && latencyKnobAngle (pos) > latencyKnobAngle (6) && latencyKnobAngle (pos) < latencyKnobAngle (7)
               && std::abs (latencyKnobAngle (0) + 135.0) < 1e-9 && std::abs (latencyKnobAngle (14) - 135.0) < 1e-9,
           juce::String::formatted ("%.2f Hz (between C#4 and D4) sits at position %.3f (%.1f deg), %s ms, lowest note %d", between, pos, latencyKnobAngle (pos),
                                    latencyMsText (48000.0, between).c_str(), lowestReliableNote (48000.0, between)));
}
