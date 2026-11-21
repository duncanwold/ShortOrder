// 1.1 (notes start sooner): the latency measurements (sines from silence, the RC takes, the flatpicking passage)
// for this build's engine. Probes only (on request), outputs to build/test-output/v11/:
//
//   probe-v11-sine    the lab's Test 1: sines from silence (every note of the default's zone, E4 +40 c, each detent end's lowest note;
//                     4 phases x abrupt / 2 ms fade x 4 points of the hop cycle)
//   probe-v11-takes   the lab's Test 2: the seven RC takes through RC1 QA's own measurements (rc1::analyse, rc1::delaysFor and the
//                     event measures) at the noted settings, as heard, and noted with Release Fast (GTT_V11_DETENT=<k>: all at detent k)
//   probe-v11-knob    the lab's knob table: every detent's readout, the voice's delay D, and sines from silence
//   probe-v11-clips   the listening clips (Listen - 1.1/ in the worktree; GTT_V11_TAG names this build's, e.g. 1.0.1 or 1.1)
//   probe-v11-snaps   the fresh-start snap in every golden render (GTT_V11_REF=<dir>: against renders without it)
//
// The lab read a hop's readiness (its newest input sample) and the voice's T from two fields it added to HopInfo and VoiceProbe; here
// they come from what the engine already reports: a reading's window ends L/2 - 1 after its stamp, and the voice plays the newest
// timeline entry at or before its time (PitchTimeline::lookup), whose T the hop reported.

namespace v11
{

juce::File dir()
{
    auto d = outDir.getChildFile ("v11");
    d.createDirectory();
    return d;
}

juce::String noteName (int midi) { return juce::String (pitchClassName (midi)) + juce::String (midi / 12 - 1); }
juce::String ms (double v, const char* f = "%.1f") { return v < 0.0 ? juce::String ("-") : juce::String::formatted (f, v); }

struct Stats
{
    double med = -1, lo = -1, hi = -1, p10 = -1, p90 = -1;
    int n = 0, missing = 0;
};
Stats statsOf (std::vector<double> v)
{
    Stats s;
    const size_t all = v.size();
    v.erase (std::remove_if (v.begin(), v.end(), [] (double x) { return x < 0.0; }), v.end());
    s.missing = (int) (all - v.size());
    s.n = (int) v.size();
    if (v.empty())
        return s;
    std::sort (v.begin(), v.end());
    s.med = v[v.size() / 2];
    s.lo = v.front();
    s.hi = v.back();
    s.p10 = v[(size_t) (0.1 * (double) (v.size() - 1))];
    s.p90 = v[(size_t) (0.9 * (double) (v.size() - 1))];
    return s;
}
juce::String statsText (const Stats& s) { return s.n == 0 ? juce::String ("-") : juce::String::formatted ("%.1f [%.1f .. %.1f]", s.med, s.lo, s.hi); }

// --- One run, instrumented -------------------------------------------------------------------------------------------------------
struct Run
{
    std::vector<float> out;
    std::vector<HopInfo> hops;
    std::vector<VoiceProbe> voice; // every sample (voice time = input time)
    int delay = 0, L = 0, hop = 0;
};
Run run (const std::vector<float>& x, EngineParams p, double fs = kFs, int block = 64)
{
    Run r;
    Engine e;
    e.prepare (fs, block);
    e.hopListener = [&] (const HopInfo& h) { r.hops.push_back (h); };
    r.voice.reserve (x.size());
    e.voiceListener = [&] (const VoiceProbe& v) { r.voice.push_back (v); };
    r.out.resize (x.size());
    for (size_t s = 0; s < x.size(); s += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, x.size() - s);
        e.process (x.data() + s, r.out.data() + s, n, p);
    }
    r.delay = e.currentLatencySamples();
    r.L = e.readout().windowLength.load();
    r.hop = e.hopSize();
    return r;
}

// The lab's measure: from the onset (input sample n0) to the first correct and confident reading, to the voice's gate opening, and to
// the voice sounding the right note (output time); the attack's wrong notes before it.
struct Measure
{
    double readyMs = -1, openMs = -1, soundMs = -1, errCents = 0, switchMs = -1;
    int wrongHops = 0;
    double wrongMs = 0.0;
    double glideMs = 0.0; // before the right note: the gate open with T between notes (more than 5 c from one): a glide in
};
Measure measure (const Run& r, int64_t n0, double trueMidi, int note, double fs = kFs)
{
    Measure m;
    for (const auto& h : r.hops)
    {
        const int64_t ready = h.time + r.L / 2 - 1; // the newest sample of the reading's window (entryTime = newest + 1 - L/2)
        if (ready < n0 || ! h.detector.voiced)
            continue;
        if (std::abs (h.midi - trueMidi) < 0.5)
        {
            if (m.readyMs < 0.0)
            {
                m.readyMs = 1000.0 * (double) (ready + 1 - n0) / fs;
                m.errCents = 100.0 * (h.midi - trueMidi);
            }
        }
        else if (m.readyMs < 0.0)
            ++m.wrongHops;
    }
    const double perSample = 1000.0 / fs;
    size_t j = 0;
    bool any = false;
    for (const auto& v : r.voice)
    {
        while (j + 1 < r.hops.size() && r.hops[j + 1].time <= v.t)
            ++j;
        any = any || (! r.hops.empty() && r.hops[0].time <= v.t);
        const float target = any ? (float) r.hops[j].target : 57.0f; // PitchTimeline::lookup (the entry's T is a float)
        const int64_t outT = v.t + r.delay;
        if (outT < n0)
            continue;
        if (v.gate > 0.0f && m.openMs < 0.0)
            m.openMs = (double) (outT - n0) * perSample;
        if (v.gate > 0.0f && v.note == note && m.switchMs < 0.0)
            m.switchMs = (double) (outT - n0) * perSample;
        if (v.gate > 0.0f && v.note == note && std::abs (target - (float) note) < 0.5f)
        {
            m.soundMs = (double) (outT - n0) * perSample;
            break;
        }
        if (v.gate > 0.0f && v.note != note)
            m.wrongMs += perSample;
        if (v.gate > 0.0f && std::abs (target - std::round (target)) > 0.05f)
            m.glideMs += perSample;
    }
    return m;
}

std::vector<float> toneFromSilence (double hz, double phaseDeg, double fadeMs, int64_t n0, double seconds, float amp = 0.2f, double fs = kFs)
{
    std::vector<float> x ((size_t) (seconds * fs), 0.0f);
    const auto fade = (int64_t) std::lround (fadeMs * 0.001 * fs);
    for (int64_t n = n0; n < (int64_t) x.size(); ++n)
    {
        const double t = (double) (n - n0);
        const double g = fade > 0 && t < (double) fade ? t / (double) fade : 1.0;
        x[(size_t) n] = (float) (amp * g * std::sin (2.0 * M_PI * hz * t / fs + phaseDeg * M_PI / 180.0));
    }
    return x;
}

// The theory from silence (YIN): the first window clear of silence is stamped at its centre, L/2 after the onset, and sounds D later;
// the hop adds up to one hop.
void theory (EngineParams p, double fs, double& lo, double& hi, int* Lout = nullptr, int* Dout = nullptr, int* hopOut = nullptr)
{
    Engine e;
    e.prepare (fs, 64);
    std::vector<float> z (64, 0.0f), o (64);
    e.process (z.data(), o.data(), 64, p);
    const int L = e.readout().windowLength.load(), D = e.currentLatencySamples(), hop = e.hopSize();
    lo = 1000.0 * (L / 2 + D) / fs;
    hi = lo + 1000.0 * hop / fs;
    if (Lout)
        *Lout = L;
    if (Dout)
        *Dout = D;
    if (hopOut)
        *hopOut = hop;
}
int lowestNoteOf (EngineParams p, double fs = kFs) { return lowestReliableNote (fs, clampWindow (p.windowHz)); }

// --- The lab's Test 1: sines from silence ----------------------------------------------------------------------------------------
struct SineCase
{
    juce::String label;
    double midi;
    int note;
    double windowHz;
};
void probeSine()
{
    std::vector<SineCase> cases;
    const double def = defaultWindowHz();
    for (int n = std::min (59, lowestNoteOf (EngineParams {})); n <= kTopBoardNote; ++n)
        cases.push_back ({ noteName (n), (double) n, n, def });
    cases.push_back ({ "E4 +40c", 64.4, 64, def });
    {
        EngineParams slow, fast;
        slow.windowHz = (float) latencyDetentHz (0);
        fast.windowHz = (float) latencyDetentHz (kLatencyDetents - 1);
        const int ls = lowestNoteOf (slow), lf = lowestNoteOf (fast);
        cases.push_back ({ "slowest " + noteName (ls), (double) ls, ls, slow.windowHz });
        cases.push_back ({ "fastest " + noteName (lf), (double) lf, lf, fast.windowHz });
    }
    int hop0 = 128;
    {
        double a, b;
        theory (EngineParams {}, kFs, a, b, nullptr, nullptr, &hop0);
    }
    std::vector<std::vector<Measure>> rows (cases.size());
    sputter::parallelFor ((int) cases.size(), [&] (int i) {
        for (int off = 0; off < 4; ++off)
            for (double fade : { 0.0, 2.0 })
                for (double ph : { 0.0, 90.0, 180.0, 270.0 })
                {
                    const int64_t n0 = (int64_t) (0.25 * kFs) + off * hop0 / 4;
                    EngineParams p;
                    p.windowHz = (float) cases[(size_t) i].windowHz;
                    const auto r = run (toneFromSilence (hzFromMidi (cases[(size_t) i].midi), ph, fade, n0, 0.6), p);
                    rows[(size_t) i].push_back (measure (r, n0, cases[(size_t) i].midi, cases[(size_t) i].note));
                }
    });
    double tlo, thi;
    int L, D, hop;
    theory (EngineParams {}, kFs, tlo, thi, &L, &D, &hop);
    juce::String md;
    md << "# Sines from silence (Test 1, this build)\n\n";
    md << juce::String::formatted ("At the default: L = %d, D = %d, hop = %d samples; theory %.2f .. %.2f ms. Lowest reliable note %s.\n\n", L, D, hop, tlo, thi,
                                   noteName (lowestNoteOf (EngineParams {})).toRawUTF8());
    md << "Each pitch at start phases 0 / 90 / 180 / 270 deg, abrupt and with a 2 ms fade-in, the onset at four points of the hop cycle (32 runs). ready: the first "
          "correct reading's last input sample; open: the gate opens (output time); sound: the voice on the right note, T within 50 c. A neighbour glide: the "
          "right note sounds with T between notes on the way (a wrong first note gliding in, not snapping).\n\n";
    md << "| note | theory ms | ready ms | open ms | sound ms | err c | wrong hops | wrong ms | neighbour glides |\n|---|---|---|---|---|---|---|---|---|\n";
    std::vector<double> allOpen, allSound, allReady;
    int early = 0, late = 0, wrongs = 0, none = 0, glides = 0, runs = 0;
    double worstGlide = 0.0, worstWrong = 0.0;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        EngineParams p;
        p.windowHz = (float) cases[i].windowHz;
        double lo, hi;
        theory (p, kFs, lo, hi);
        std::vector<double> rd, op, so, er;
        int wh = 0, gl = 0;
        double wm = 0;
        for (const auto& m : rows[i])
        {
            rd.push_back (m.readyMs);
            op.push_back (m.openMs);
            so.push_back (m.soundMs);
            er.push_back (m.errCents + 1000.0);
            wh += m.wrongHops;
            wm += m.wrongMs;
            const bool glide = m.glideMs > 0.0;
            gl += glide ? 1 : 0;
            if (cases[i].windowHz == def) // the medians: the default setting's notes; the counts (as the lab's): every run
            {
                allOpen.push_back (m.openMs);
                allSound.push_back (m.soundMs);
                allReady.push_back (m.readyMs);
            }
            ++runs;
            glides += glide ? 1 : 0;
            if (glide)
                worstGlide = std::max (worstGlide, m.soundMs - m.openMs);
            worstWrong = std::max (worstWrong, m.soundMs >= 0.0 && m.openMs >= 0.0 ? m.soundMs - m.openMs : 0.0);
            if (m.soundMs < 0)
                ++none;
            else if (m.openMs > hi + 0.05)
                ++late;
            else if (m.openMs < lo - 0.05)
                ++early;
            wrongs += (m.wrongHops > 0 || m.wrongMs > 0) ? 1 : 0;
        }
        const auto e = statsOf (er);
        md << "| " << cases[i].label << " | " << juce::String::formatted ("%.1f .. %.1f", lo, hi) << " | " << statsText (statsOf (rd)) << " | "
           << statsText (statsOf (op)) << " | " << statsText (statsOf (so)) << " | "
           << (e.n ? juce::String::formatted ("%+.1f [%+.1f .. %+.1f]", e.med - 1000, e.lo - 1000, e.hi - 1000) : juce::String ("-")) << " | " << wh << " | "
           << juce::String (wm, 1) << " | " << gl << " |\n";
    }
    const auto so = statsOf (allSound);
    juce::String summary = juce::String::formatted (
        "default (L %d / D %d / hop %d, theory %.1f .. %.1f ms): open %s, sound %s, ready %s (the default's notes); every run: early / late vs theory %d / %d of %d; runs with a wrong note "
        "before the right one %d (longest from the gate opening to the right note %.1f ms); neighbour glides %d (longest %.1f ms); no note %d\n",
        L, D, hop, tlo, thi, statsText (statsOf (allOpen)).toRawUTF8(), statsText (so).toRawUTF8(), statsText (statsOf (allReady)).toRawUTF8(), early, late,
        runs, wrongs, worstWrong, glides, worstGlide, none);
    dir().getChildFile ("sine.md").replaceWithText (md + "\n" + summary);
    std::printf ("%s", summary.toRawUTF8());
}

// --- The lab's Test 2: the RC takes, through RC1 QA's own measurements -----------------------------------------------------------
struct TakeSetting
{
    juce::String name;
    bool heard;
    std::function<EngineParams (int take)> params;
};
// GTT_V11_DETENT=<k>: every setting at the latency knob's detent k instead of its own latency (the table at the slowest, default and
// fastest detents).
EngineParams atDetent (EngineParams p)
{
    const auto k = juce::SystemStats::getEnvironmentVariable ("GTT_V11_DETENT", {});
    if (k.isNotEmpty())
        p.windowHz = latencyDetentHz (k.getIntValue());
    return p;
}
std::vector<TakeSetting> takeSettings()
{
    return {
        { "noted", false, [] (int t) { return golden::settingsFor (t)[0].p; } },
        { "as heard", true, [] (int t) { return rc1::heard (t).p; } },
        { "noted, Release Fast", false, [] (int t) {
             auto p = golden::settingsFor (t)[0].p;
             p.holdMs = noteReleaseMsFor (NoteRelease::Fast);
             return p;
         } },
    };
}

struct TakeInput
{
    std::vector<float> in, heardIn;
    rc1::Truth tr, heardTr;
    rc1::Level lv, heardLv;
    std::vector<blips::Pick> ps;
};
std::vector<TakeInput>& takeInputs()
{
    static std::vector<TakeInput> v;
    if (v.empty())
    {
        v.resize ((size_t) golden::kNumTakes);
        for (int t = 0; t < golden::kNumTakes; ++t)
            golden::dry (t);
        sputter::parallelFor (golden::kNumTakes, [&] (int t) {
            auto& x = v[(size_t) t];
            x.in = golden::dry (t);
            x.tr = rc1::truthFor (x.in);
            x.lv = rc1::levelOf (x.in);
            x.ps = blips::takePicks (x.in);
            x.heardIn = rc1::gained (x.in, rc1::heard (t).gainDb);
            x.heardTr = rc1::truthFor (x.heardIn);
            x.heardLv = rc1::levelOf (x.heardIn);
        });
    }
    return v;
}

struct TakeRow
{
    juce::String setting;
    std::vector<double> open, takeMedian, switches;
    int fresh = 0, picks = 0;
    std::map<juce::String, std::pair<int, double>> ev; // kind -> (count, ms)
    int delay = 0;
};

TakeRow takeRow (const TakeSetting& ts)
{
    auto& inputs = takeInputs();
    std::vector<rc1::Result> res ((size_t) golden::kNumTakes);
    std::vector<std::vector<double>> switches ((size_t) golden::kNumTakes);
    sputter::parallelFor (golden::kNumTakes, [&] (int t) {
        const auto& x = inputs[(size_t) t];
        const auto p = atDetent (ts.params (t));
        const auto& in = ts.heard ? x.heardIn : x.in;
        const auto& tr = ts.heard ? x.heardTr : x.tr;
        const auto& lvl = ts.heard ? x.heardLv : x.lv;
        auto& r = res[(size_t) t];
        r.setting = ts.name;
        r.p = p;
        const auto a = rc1::analyse (in, p);
        auto add = [&] (std::vector<rc1::Event> v) { r.ev.insert (r.ev.end(), v.begin(), v.end()); };
        add (rc1::wrongNotes (a, tr));
        add (rc1::pickBlips (a, tr, x.ps, &r.blips));
        add (rc1::dropouts (a, tr, lvl));
        add (rc1::hangs (a, lvl));
        add (rc1::frozenGlides (a));
        add (rc1::clicks (a));
        add (rc1::glideIns (tr, x.ps, r.blips));
        add (rc1::belowZone (a, tr, lvl));
        add (rc1::levelJumps (a, lvl, tr));
        if (p.chordMode == ChordMode::Memory)
            add (rc1::freezeEvents (a, tr, r.freezeLog));
        if (p.chordMode == ChordMode::Stack)
            add (rc1::harmonizeEvents (a, r.harmLog));
        r.delays = rc1::delaysFor (a, in, x.ps);
        r.latency = a.latency;
        // Within a phrase: a pick to another note with the voice already open, to the voice's note becoming the pick's (output time).
        for (const auto& pk : x.ps)
        {
            if (pk.note < 0)
                continue;
            const auto k0 = (size_t) std::max<int64_t> (0, pk.t / 16);
            if (k0 >= a.lite.size() || a.lite[k0].gate <= 0.0f || a.lite[k0].note == pk.note)
                continue;
            for (size_t k = k0; k < a.lite.size() && (int64_t) (k * 16) < std::min (pk.end, pk.t + (int64_t) (0.2 * kFs)); ++k)
                if (a.lite[k].gate > 0.0f && a.lite[k].note == pk.note)
                {
                    switches[(size_t) t].push_back (((double) k * 16.0 + a.latency - (double) pk.t) / 48.0);
                    break;
                }
        }
    });
    TakeRow row;
    row.setting = ts.name;
    for (auto& sw : switches)
        row.switches.insert (row.switches.end(), sw.begin(), sw.end());
    for (const auto& r : res)
    {
        std::vector<double> o;
        for (const auto& d : r.delays)
        {
            ++row.picks;
            if (d.fromSilence)
            {
                ++row.fresh;
                row.open.push_back (d.open);
                o.push_back (d.open);
            }
        }
        row.takeMedian.push_back (statsOf (o).med);
        for (const auto& e : r.ev)
        {
            auto& c = row.ev[e.kind];
            ++c.first;
            c.second += e.ms;
        }
        row.delay = r.latency;
    }
    return row;
}

int evCount (const TakeRow& r, const juce::String& kind)
{
    auto it = r.ev.find (kind);
    return it == r.ev.end() ? 0 : it->second.first;
}
juce::String evText (const TakeRow& r, const juce::String& kind)
{
    auto it = r.ev.find (kind);
    return it == r.ev.end() ? juce::String ("0") : juce::String::formatted ("%d (%.0f)", it->second.first, it->second.second);
}

void probeTakes()
{
    const auto t0 = std::chrono::steady_clock::now();
    juce::String md;
    md << "# The RC takes (Test 2, this build)\n\n";
    md << "| setting | D ms | fresh notes | from silence: median [10 .. 90%] worst | per-take medians | within a phrase: note switch, median [10 .. 90%] (n) | wrong (ms) "
          "| junk (ms) | blips <3 / 3-4 / 5+ st | dropouts (ms) | hangs | frozen glides (ms) | clicks | below: junk of | level jumps / dips | stale / pick glides |\n";
    md << "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    juce::String perTake;
    for (const auto& ts : takeSettings())
    {
        const auto r = takeRow (ts);
        const auto st = statsOf (r.open), tm = statsOf (r.takeMedian), po = statsOf (r.switches);
        md << "| " << ts.name << " | " << juce::String (1000.0 * r.delay / kFs, 2) << " | " << r.fresh << " | "
           << (st.n ? juce::String::formatted ("%.1f [%.1f .. %.1f] %.1f", st.med, st.p10, st.p90, st.hi) : juce::String ("-")) << " | "
           << (tm.n ? juce::String::formatted ("%.1f .. %.1f", tm.lo, tm.hi) : juce::String ("-")) << " | "
           << (po.n ? juce::String::formatted ("%.1f [%.1f .. %.1f] (%d)", po.med, po.p10, po.p90, po.n) : juce::String ("-")) << " | " << evText (r, "wrong")
           << " | " << evText (r, "junk") << " | " << evCount (r, "blip-under-3st") << " / " << evCount (r, "blip-3-4st") << " / " << evCount (r, "blip-5st+")
           << " | " << evText (r, "dropout") << " | " << evCount (r, "hang") << " | " << evText (r, "frozen-glide") << " | " << evCount (r, "click") << " | "
           << evCount (r, "below-junk") << " of " << evCount (r, "below-junk") + evCount (r, "below") << " | " << evCount (r, "level-jump") << " / "
           << evCount (r, "level-dip") << " | " << evCount (r, "stale-glide") << " / " << evCount (r, "glide-in") << " |\n";
        perTake << "- " << ts.name << " (flatpick, fingerpick, bass, below, freeze3, harmonics, harmonize):";
        for (double v : r.takeMedian)
            perTake << " " << ms (v);
        perTake << "\n";
        std::printf ("  %s: from silence %s (%d fresh), per-take medians %s\n", ts.name.toRawUTF8(), statsText (st).toRawUTF8(), r.fresh,
                     tm.n ? juce::String::formatted ("%.1f .. %.1f", tm.lo, tm.hi).toRawUTF8() : "-");
    }
    md << "\nPer-take medians from silence (ms):\n\n" << perTake;
    md << juce::String::formatted ("\n(%.0f s)\n", std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
    dir().getChildFile ("takes.md").replaceWithText (md);
    std::printf ("%s\n", md.toRawUTF8());
}

// --- The lab's knob table: the 15 detents ------------------------------------------------------------------------------------------
void probeKnob()
{
    juce::String md = "# The 15 latency detents (this build)\n\nreadout: the faceplate's and the host's text at 48 kHz (latencyMsText); D: the voice's delay "
                      "(within a phrase: the level, and a legato note once detected); from silence: sines from silence, every note from the detent's lowest to "
                      "C#6, 4 phases x 4 hop offsets, abrupt: median [min .. max] of the voice sounding the right note.\n\n"
                      "| detent | Hz | L ms | readout | D ms | from silence ms | lowest |\n|---|---|---|---|---|---|---|\n";
    for (int d = 0; d < kLatencyDetents; ++d)
    {
        EngineParams p;
        p.windowHz = (float) latencyDetentHz (d);
        const int lowest = lowestNoteOf (p);
        std::vector<int> notes;
        for (int n = lowest; n <= kTopBoardNote; ++n)
            notes.push_back (n);
        std::vector<std::vector<double>> opens (notes.size());
        int L = 0, D = 0, hop = 128;
        double lo, hi;
        theory (p, kFs, lo, hi, &L, &D, &hop);
        sputter::parallelFor ((int) notes.size(), [&] (int i) {
            for (int off = 0; off < 4; ++off)
                for (double ph : { 0.0, 90.0, 180.0, 270.0 })
                {
                    const int64_t n0 = (int64_t) (0.25 * kFs) + off * hop / 4;
                    const auto r = run (toneFromSilence (hzFromMidi (notes[(size_t) i]), ph, 0.0, n0, 0.45), p);
                    opens[(size_t) i].push_back (measure (r, n0, notes[(size_t) i], notes[(size_t) i]).soundMs);
                }
        });
        std::vector<double> all;
        for (auto& o : opens)
            all.insert (all.end(), o.begin(), o.end());
        const auto st = statsOf (all);
        md << "| " << d << " | " << juce::String (latencyDetentHz (d), 2) << " | " << juce::String (1000.0 * L / kFs, 2) << " | " << latencyMsText (kFs, p.windowHz)
           << " | " << juce::String (1000.0 * D / kFs, 2) << " | " << statsText (st) << " | " << noteName (lowest) << " |\n";
    }
    dir().getChildFile ("knob.md").replaceWithText (md);
    std::printf ("%s\n", md.toRawUTF8());
}

// --- The listening clips -----------------------------------------------------------------------------------------------------------
void writeStereo (const juce::File& f, const std::vector<float>& l, const std::vector<float>& r, double fs)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (f.createOutputStream().release());
    if (stream == nullptr)
        return;
    auto options = juce::AudioFormatWriterOptions().withSampleRate (fs).withNumChannels (2).withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    if (writer == nullptr)
        return;
    const float* ch[2] = { l.data(), r.data() };
    writer->writeFromFloatArrays (ch, 2, (int) std::min (l.size(), r.size()));
}
std::vector<float> cut (const std::vector<float>& x, size_t a, size_t n, float peakDb = -3.0f)
{
    std::vector<float> y (n, 0.0f);
    float pk = 0.0f;
    for (size_t i = 0; i < n && a + i < x.size(); ++i)
    {
        y[i] = x[a + i];
        pk = std::max (pk, std::abs (y[i]));
    }
    if (pk > 0.0f)
        for (auto& v : y)
            v *= std::pow (10.0f, peakDb / 20.0f) / pk;
    const size_t f = (size_t) (0.01 * kFs);
    for (size_t i = 0; i < f && i < n; ++i)
    {
        y[i] *= (float) i / (float) f;
        y[n - 1 - i] *= (float) i / (float) f;
    }
    return y;
}

// Legato changes: the truth (the measuring detector) changing note with no pick from 10 ms before to 60 ms after (the lab's rule):
// (input sample, the new note).
std::vector<std::pair<int64_t, int>> legatoChanges (const TakeInput& x)
{
    std::vector<std::pair<int64_t, int>> legato;
    int last = -1;
    for (size_t k = 0; k < x.tr.hops.size(); ++k)
    {
        if (! x.tr.ok[k])
            continue;
        const int n = (int) std::lround (x.tr.hops[k].midi);
        if (last >= 0 && n != last)
        {
            const double tt = (double) x.tr.hops[k].time;
            bool picked = false;
            for (auto& pk : x.ps)
                picked = picked || (tt - (double) pk.t > -0.01 * kFs && tt - (double) pk.t < 0.06 * kFs);
            if (! picked)
                legato.push_back ({ x.tr.hops[k].time, n });
        }
        last = n;
    }
    return legato;
}
// The legato run: in the flatpicking take, the 6 s with the most legato changes, from the input alone, so every build cuts the same
// stretch.
double legatoStart (const TakeInput& x)
{
    const auto legato = legatoChanges (x);
    double best = 0.0;
    int bestN = -1;
    for (double s0 = 0.0; s0 + 6.0 < (double) x.in.size() / kFs; s0 += 0.25)
    {
        int l = 0;
        for (auto& c : legato)
            l += (double) c.first / kFs >= s0 + 0.3 && (double) c.first / kFs < s0 + 5.7 ? 1 : 0;
        if (l > bestN)
        {
            bestN = l;
            best = s0;
        }
    }
    return best;
}

void probeClips()
{
    const auto tag = juce::SystemStats::getEnvironmentVariable ("GTT_V11_TAG", "this-build");
    const auto out = golden::repoDir().getChildFile ("Listen - 1.1");
    out.createDirectory();
    auto& inputs = takeInputs();
    const int take = 0; // solo note flatpicking
    const auto& x = inputs[(size_t) take];
    juce::String log;
    struct Clip
    {
        juce::String name;
        double start, seconds;
        bool releaseFast;
    };
    // The latency measurements' passage (the listening clips): solo note flatpicking 0 .. 9 s, the defaults with Release Fast (rests start notes from
    // silence). The legato run: the noted settings (Release Medium).
    const double ls = legatoStart (x);
    for (const auto& c : { Clip { "flatpicking", 0.0, 9.0, true }, Clip { "legato", ls, 6.0, false } })
    {
        auto p = golden::settingsFor (take)[0].p;
        if (c.releaseFast)
            p.holdMs = noteReleaseMsFor (NoteRelease::Fast);
        const auto a = rc1::analyse (x.in, p);
        const auto d = rc1::delaysFor (a, x.in, x.ps);
        juce::String starts, switches;
        for (auto& q : d)
            if (q.t >= c.start && q.t < c.start + c.seconds && q.fromSilence && q.open > 0)
                starts << juce::String::formatted ("%.1f ", q.open);
        for (const auto& pk : x.ps)
        {
            if (pk.note < 0 || (double) pk.t / kFs < c.start || (double) pk.t / kFs >= c.start + c.seconds)
                continue;
            const auto k0 = (size_t) std::max<int64_t> (0, pk.t / 16);
            if (k0 >= a.lite.size() || a.lite[k0].gate <= 0.0f || a.lite[k0].note == pk.note)
                continue;
            for (size_t k = k0; k < a.lite.size() && (int64_t) (k * 16) < std::min (pk.end, pk.t + (int64_t) (0.2 * kFs)); ++k)
                if (a.lite[k].gate > 0.0f && a.lite[k].note == pk.note)
                {
                    switches << juce::String::formatted ("%.1f ", ((double) k * 16.0 + a.latency - (double) pk.t) / 48.0);
                    break;
                }
        }
        // The voice's note changes in the clip (output time, s from the clip's start: the gate open and the note changing), for
        // comparing builds: changes-<clip>-<tag>.txt.
        {
            juce::String ch;
            int last = -1;
            for (size_t k = 0; k < a.lite.size(); ++k)
            {
                const double tOut = ((double) k * 16.0 + a.latency) / kFs;
                const int note = a.lite[k].gate > 0.0f ? a.lite[k].note : -1;
                if (note != last && tOut >= c.start && tOut < c.start + c.seconds)
                    ch << juce::String::formatted ("%.4f %d\n", tOut - c.start, note);
                last = note;
            }
            golden::repoDir().getChildFile ("Listen - 1.1").getChildFile ("changes-" + c.name + "-" + tag + ".txt").replaceWithText (ch);
        }
        const size_t a0 = (size_t) (c.start * kFs), n = (size_t) (c.seconds * kFs);
        const auto L = cut (x.in, a0, n), R = cut (a.out, a0, n);
        const auto file = c.name + "-" + tag;
        writeStereo (out.getChildFile (file + ".wav"), L, R, kFs);
        writeStereo (out.getChildFile (file + "-half-speed.wav"), L, R, kFs / 2.0);
        log << file << ".wav: solo note flatpicking " << juce::String::formatted ("%.2f .. %.2f s", c.start, c.start + c.seconds) << (c.releaseFast ? ", Release Fast" : ", Release Medium")
            << "; voice delay " << juce::String (1000.0 * a.latency / kFs, 2) << " ms; notes from silence start "
            << (starts.isEmpty() ? juce::String ("(none)") : starts.trim() + " ms") << " after the pick; picks to a new note with the voice open switch at "
            << (switches.isEmpty() ? juce::String ("(none)") : switches.trim() + " ms") << "\n";
    }
    out.getChildFile ("starts-" + tag + ".txt").replaceWithText (log);
    std::printf ("%s", log.toRawUTF8());
}

} // namespace v11

namespace v11
{
// --- The fresh-start snap in the golden renders --------------------------------------------------------------------------------------
// For every golden render: the hops where the snap acted (within kFreshSnapMs of a fresh latch, no onset since, T moved onto N
// exactly: a correction that would have glided at Retune), at output time. GTT_V11_REF=<dir> (renders of the build without the snap, the
// same file names as Tests/golden/renders/): each render's first differing sample, which must be at or after its first snap (and
// a render with no snap must be identical).
void probeSnaps()
{
    const auto js = golden::jobs();
    const auto refDir = juce::File (juce::SystemStats::getEnvironmentVariable ("GTT_V11_REF", {}));
    struct Row
    {
        int snaps = 0;
        double firstSnapS = -1.0, firstDiffS = -1.0;
        juce::String list;
        bool identical = true, haveRef = false;
    };
    std::vector<Row> rows (js.size());
    sputter::parallelFor ((int) js.size(), [&] (int i) {
        const auto& j = js[(size_t) i];
        Engine e;
        e.prepare (j.fs, golden::kBlock);
        std::vector<HopInfo> hops;
        e.hopListener = [&hops] (const HopInfo& h) { hops.push_back (h); };
        const auto& in = golden::inputFor (j);
        std::vector<float> out (in.size());
        for (size_t s = 0; s < in.size(); s += (size_t) golden::kBlock)
        {
            const int n = (int) std::min ((size_t) golden::kBlock, in.size() - s);
            e.process (in.data() + s, out.data() + s, n, j.setting.p);
        }
        const int D = e.currentLatencySamples();
        const auto win = (int64_t) (kFreshSnapMs * 0.001 * j.fs);
        int64_t latch = -(1 << 30);
        int prevN = -1;
        double prevT = 0.0;
        auto& r = rows[(size_t) i];
        for (const auto& h : hops)
        {
            if (h.onset)
                latch = -(1 << 30);
            const auto ev = (Quantizer::Event) h.event;
            // the snap acted: in the window, T moved onto N exactly (at Retune it would have stepped part of the way)
            if (h.time - latch < win && ev != Quantizer::Event::FreshLatch && h.hasNote && std::abs (prevT - h.noteN) > 1.0e-9 && h.target == (double) h.noteN)
            {
                ++r.snaps;
                const double at = (double) (h.time + D) / j.fs;
                if (r.firstSnapS < 0.0)
                    r.firstSnapS = at;
                if (r.snaps <= 6)
                    r.list << juce::String::formatted (" %.3f s %s>%s", at, sputter::noteName (prevN).toRawUTF8(), sputter::noteName (h.noteN).toRawUTF8());
            }
            if (ev == Quantizer::Event::FreshLatch)
                latch = h.time;
            if (h.hasNote)
                prevN = h.noteN;
            prevT = h.target;
        }
        const auto f = refDir.getChildFile (j.file + ".wav");
        if (refDir.isDirectory() && f.existsAsFile())
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
            if (rd != nullptr)
            {
                juce::AudioBuffer<float> b (1, (int) rd->lengthInSamples);
                rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, false);
                std::vector<float> ref (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
                const int d = golden::firstDifference (ref, out);
                r.haveRef = true;
                r.identical = d < 0;
                r.firstDiffS = d < 0 ? -1.0 : (double) d / j.fs;
            }
        }
    });
    juce::String md = "# The fresh-start snap in the golden renders\n\n| render | snaps | first snap (s, output) | first difference from the reference (s) | consistent |\n|---|---|---|---|---|\n";
    int changed = 0, withSnap = 0, bad = 0;
    for (size_t i = 0; i < js.size(); ++i)
    {
        const auto& r = rows[i];
        withSnap += r.snaps > 0 ? 1 : 0;
        changed += r.haveRef && ! r.identical ? 1 : 0;
        // consistent: no snap and identical, or a snap and the first difference no earlier than the first snap's hop (less one hop)
        const bool ok = ! r.haveRef || (r.snaps == 0 ? r.identical : (r.identical || r.firstDiffS >= r.firstSnapS - 0.003));
        bad += ok ? 0 : 1;
        if (r.snaps > 0 || ! r.identical)
            md << "| " << js[i].file << " | " << r.snaps << r.list << " | " << ms (r.firstSnapS, "%.3f") << " | "
               << (r.haveRef ? (r.identical ? juce::String ("identical") : juce::String (r.firstDiffS, 3)) : juce::String ("(no reference)")) << " | "
               << (ok ? "yes" : "NO") << " |\n";
    }
    const auto summary = juce::String::formatted ("%d of %d renders have a fresh-start snap; %d differ from the reference; %d inconsistent\n", withSnap,
                                                  (int) js.size(), changed, bad);
    dir().getChildFile ("snaps.md").replaceWithText (md + "\n" + summary);
    std::printf ("%s", summary.toRawUTF8());
}
} // namespace v11
