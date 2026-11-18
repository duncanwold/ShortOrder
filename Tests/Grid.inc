// v0.6.3: the grid, the golden renders' companion for what they don't reach: 44.1 / 48 / 96 kHz on the synthetic phrases (every
// wave, the 16 Chord choices, Sustain x Note Release, Octave / Retune / window moved), the host's bypass moved mid-phrase, and
// the investigation takes in Recordings/ (48 kHz). The settings are the old frozen-copy equivalence grids' (v0510- / v0511-
// equivalence), on the engine as it plays. Each run's output is hashed (SHA-256, golden::sha256).
//
//   GTTTests --grid-write <file>   write the hashes (a local baseline: the cleanup keeps it in Tests/baseline/, gitignored)
//   GTTTests --grid <file>         re-render and compare (non-zero exit on any mismatch)
//
// Included by OfflineTests.cpp, after Golden.inc.

namespace grid
{
struct Run
{
    juce::String name;
    EngineParams p;
    int input = 0;
    double bypassFrom = -1.0, bypassTo = -1.0; // seconds: the host's bypass on in [from, to)
};

std::vector<Run> synthRuns()
{
    const char* inputNames[4] = { "phrase", "separate plucks", "medium plucks", "memory phrase" };
    const std::pair<const char*, Wave> waves[8] = { { "Saw", Wave::Saw },          { "Square", Wave::Square }, { "NES Lead", Wave::Pulse25 },
                                                    { "Triangle", Wave::Triangle }, { "Sine", Wave::Sine },     { "Half-Sine", Wave::HalfSine },
                                                    { "Organ", Wave::Organ },       { "Folded", Wave::Folded } };
    std::vector<Run> runs;
    for (int k = 0; k < kChordChoices; ++k)
        for (int input = 0; input < 4; ++input)
            runs.push_back ({ "Chord " + juce::String (k) + ", defaults (" + inputNames[input] + ")", chordChoiceParams (k), input });
    for (const auto& [name, w] : waves)
        for (int choice : { 0, freezeChoice (3), harmonizeChoice (0), harmonizeChoice (7) })
            for (int input : { 0, 3 })
            {
                auto p = chordChoiceParams (choice);
                p.wave = w;
                runs.push_back ({ juce::String (name) + ", Chord " + juce::String (choice) + " (" + inputNames[input] + ")", p, input });
            }
    for (float sus : { 0.0f, 75.0f, 100.0f })
        for (auto [relName, rel] : { std::pair { "Fast", NoteRelease::Fast }, { "Medium", NoteRelease::Medium }, { "Long", NoteRelease::Long } })
            for (int choice : { 0, freezeChoice (3), harmonizeChoice (4) })
                for (int input : { 0, 3 })
                {
                    auto p = chordChoiceParams (choice);
                    p.sustainPercent = sus;
                    p.holdMs = noteReleaseMsFor (rel);
                    runs.push_back ({ juce::String::formatted ("Chord %d, Sustain %.0f%%, %s (", choice, sus, relName) + inputNames[input] + ")", p, input });
                }
    auto moved = [&] (const char* name, std::function<void (EngineParams&)> set) {
        for (int choice : { 0, freezeChoice (3), harmonizeChoice (9) })
            for (int input : { 0, 1 })
            {
                auto p = chordChoiceParams (choice);
                set (p);
                runs.push_back ({ juce::String (name) + juce::String::formatted (", Chord %d (", choice) + inputNames[input] + ")", p, input });
            }
    };
    moved ("Octave Sub, Half-Sine", [] (EngineParams& p) { p.octave = Octave::Sub; p.wave = Wave::HalfSine; });
    moved ("Octave Triple Sub, Square", [] (EngineParams& p) { p.octave = Octave::TripleSub; p.wave = Wave::Square; });
    moved ("Retune Fast, Organ", [] (EngineParams& p) { p.retuneSpeed = RetuneSpeed::Fast; p.wave = Wave::Organ; });
    moved ("Retune Slow, Sine", [] (EngineParams& p) { p.retuneSpeed = RetuneSpeed::Slow; p.wave = Wave::Sine; });
    moved ("window minimum", [] (EngineParams& p) { p.windowHz = (float) derivedWindowMinHz(); });
    moved ("window top, Triangle", [] (EngineParams& p) { p.windowHz = (float) kWindowMaxHz; p.wave = Wave::Triangle; });
    moved ("Note Release Fast, Sustain 100%", [] (EngineParams& p) { p.holdMs = noteReleaseMsFor (NoteRelease::Fast); p.sustainPercent = 100.0f; });
    for (int choice : { 0, harmonizeChoice (0) })
        for (int input : { 0, 3 })
        {
            Run r { juce::String::formatted ("bypass 1.0 .. 2.5 s, Chord %d (", choice) + inputNames[input] + ")", chordChoiceParams (choice), input };
            r.bypassFrom = 1.0;
            r.bypassTo = 2.5;
            runs.push_back (r);
        }
    return runs;
}

std::vector<Run> takeRuns (int numTakes)
{
    std::vector<Run> runs;
    for (int t = 0; t < numTakes; ++t)
    {
        const auto& name = blips::takes()[(size_t) t].name;
        auto add = [&] (const juce::String& what, std::function<void (EngineParams&)> set) {
            EngineParams p;
            set (p);
            runs.push_back ({ name + ", " + what, p, t });
        };
        add ("defaults", [] (EngineParams&) {});
        add ("Retune Fast, Sustain 0", [] (EngineParams& p) { p.retuneSpeed = RetuneSpeed::Fast; p.sustainPercent = 0.0f; });
        add ("Note Release Fast, Folded", [] (EngineParams& p) { p.holdMs = noteReleaseMsFor (NoteRelease::Fast); p.wave = Wave::Folded; });
        add ("Note Release Long, Sustain 100%, Freeze 3", [] (EngineParams& p) {
            p.holdMs = noteReleaseMsFor (NoteRelease::Long);
            p.sustainPercent = 100.0f;
            setChordChoice (p, freezeChoice (3));
        });
        add ("Harmonize G, Retune Slow", [] (EngineParams& p) {
            setChordChoice (p, harmonizeChoice (7));
            p.retuneSpeed = RetuneSpeed::Slow;
        });
    }
    return runs;
}

std::vector<float> renderRun (const std::vector<float>& in, double fs, const Run& r)
{
    Engine e;
    e.prepare (fs, 64);
    std::vector<float> out (in.size());
    auto p = r.p;
    for (size_t start = 0; start < in.size(); start += 64)
    {
        const double t = (double) start / fs;
        p.bypass = t >= r.bypassFrom && t < r.bypassTo;
        e.process (in.data() + start, out.data() + start, (int) std::min ((size_t) 64, in.size() - start), p);
    }
    return out;
}

// Every run's line: "<rate> | <name> | <hash>", in a fixed order.
juce::StringArray renderAll()
{
    struct Job
    {
        double fs;
        const std::vector<float>* in;
        Run run;
    };
    std::vector<Job> jobs;
    static std::vector<std::vector<float>> synth[3];
    const double rates[3] = { 44100.0, 48000.0, 96000.0 };
    const auto runs = synthRuns();
    for (int k = 0; k < 3; ++k)
    {
        synth[k] = { regressionPhrase (rates[k]), separatePlucks (rates[k]), zonePlucks (0.5f, rates[k]), memoryPhrase (rates[k]) };
        for (const auto& r : runs)
            jobs.push_back ({ rates[k], &synth[k][(size_t) r.input], r });
    }
    const int numTakes = (int) blips::takes().size();
    for (const auto& r : takeRuns (numTakes))
        jobs.push_back ({ kFs, &blips::takes()[(size_t) r.input].x, r });
    std::vector<juce::String> lines (jobs.size());
    sputter::parallelFor ((int) jobs.size(), [&] (int i) {
        const auto& j = jobs[(size_t) i];
        lines[(size_t) i] = juce::String ((int) j.fs) + " | " + j.run.name + " | " + golden::sha256 (renderRun (*j.in, j.fs, j.run));
    });
    juce::StringArray out;
    for (auto& l : lines)
        out.add (l);
    return out;
}

int write (const juce::File& f)
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto lines = renderAll();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (lines.joinIntoString ("\n") + "\n");
    std::printf ("  wrote %d hashes to %s (%.1f s)\n", lines.size(), f.getFullPathName().toRawUTF8(),
                 std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
    return 0;
}

int compare (const juce::File& f)
{
    const auto t0 = std::chrono::steady_clock::now();
    juce::StringArray ref;
    ref.addLines (f.loadFileAsString().trimEnd());
    const auto now = renderAll();
    int bad = 0;
    if (ref.size() != now.size())
    {
        std::printf ("  [FAIL] the baseline has %d runs, this build %d\n", ref.size(), now.size());
        ++bad;
    }
    for (int i = 0; i < std::min (ref.size(), now.size()); ++i)
        if (ref[i] != now[i])
        {
            std::printf ("  [FAIL] %s\n     now %s\n", ref[i].toRawUTF8(), now[i].toRawUTF8());
            ++bad;
        }
    std::printf ("  %d of %d grid runs match bit for bit (%.1f s)\n", now.size() - bad, ref.size(),
                 std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
    return bad == 0 ? 0 : 1;
}
} // namespace grid
