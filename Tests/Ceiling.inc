// v0.5.11: the pitch ceiling (docs/history/SPEC-v0.5.11.md section 3). Since v0.5.5 the edge rule unvoices any reading above 1400 Hz
// (kDetectorMaxHz: the detector's shortest lag, where it pinned on mutes and chirped). What raising it would take, behind a
// test-only switch (Engine::setCeilingHz; the default stays 1400 Hz): the mute chirps that come back, and the highest
// harmonic that then plays. Included by OfflineTests.cpp after PickGlide.inc. Report only, but for the default being unchanged.

namespace ceiling
{
const double kCeilings[] = { 1400.0, 1760.0, 2100.0, 2500.0, 3000.0 }; // the default; A6; B6 (the 7th-fret harmonic on high e) + margin; higher
// Each ceiling also with readings above 1400 Hz voiced only at this confidence (a natural harmonic is nearly a sine: it reads sure).
constexpr float kSureAbove = 0.97f;

// Readings on a take above 1400 Hz (none of them a note: no take has one above C#6): what the default edge rule refuses, and so
// what a raised ceiling lets through. hops: voiced hops above 1400 Hz; runs: runs of them (each a chirp); heardMs: how long the
// output (Sine) sounds above 1400 Hz within 30 dB of the take's loudest output.
struct Chirps
{
    int hops = 0, runs = 0, edge = 0;
    double heardMs = 0.0;
    std::array<int, 5> byHz {};      // the readings above 1400 Hz by pitch: < 1500, < 1760, < 2100, < 2500, higher
    std::array<int, 3> byConf {};    // ... and by confidence: < 0.9, < 0.97, 0.97 or more
    juce::String heardAt;            // where the output was heard above 1400 Hz (s)
};
Chirps chirps (const std::vector<float>& x, double ceilingHz, const EngineParams& p, float sure = 0.0f)
{
    Chirps c;
    Engine e;
    e.prepare (kFs, 64);
    e.setCeilingHz (ceilingHz, sure);
    bool inRun = false;
    e.hopListener = [&] (const HopInfo& h) {
        const bool above = h.detector.voiced && h.detector.f0 > kDetectorMaxHz;
        if (above)
        {
            const float f = h.detector.f0, q = h.detector.confidence;
            ++c.byHz[(size_t) (f < 1500.0f ? 0 : f < 1760.0f ? 1 : f < 2100.0f ? 2 : f < 2500.0f ? 3 : 4)];
            ++c.byConf[(size_t) (q < 0.9f ? 0 : q < 0.97f ? 1 : 2)];
        }
        c.hops += above ? 1 : 0;
        c.runs += above && ! inRun ? 1 : 0;
        inRun = above;
    };
    const auto y = runEngine (e, x, p);
    c.edge = e.tailEdgeLags();
    const auto cyc = blips::cycles (y, e.currentLatencySamples());
    float loud = 0.0f;
    for (const auto& k : cyc)
        loud = std::max (loud, k.amp);
    const double top = midiFromHz (kDetectorMaxHz);
    double lastAt = -1.0;
    for (size_t k = 1; k < cyc.size(); ++k)
        if (cyc[k].midi > top && cyc[k].amp >= loud * dbToGain (-30.0f))
        {
            c.heardMs += 1000.0 * (cyc[k].t - cyc[k - 1].t) / kFs;
            if (cyc[k].t / kFs - lastAt > 0.05)
                c.heardAt << juce::String::formatted (" %.2f s (%.0f Hz)", cyc[k].t / kFs, hzFromMidi (cyc[k].midi));
            lastAt = cyc[k].t / kFs;
        }
    return c;
}

// A natural harmonic: nearly a sine (2nd partial -25 dB, 3rd -35 dB), 2 ms in, a 1.5 s decay, 0.15 peak, over a -70 dB floor.
std::vector<float> harmonic (double hz, uint32_t seed)
{
    std::vector<float> x ((size_t) (1.2 * kFs));
    for (size_t n = 0; n < x.size(); ++n)
    {
        const double t = (double) n / kFs, w = 2.0 * M_PI * hz * t;
        const double env = std::min (1.0, t / 0.002) * std::exp (-t / 1.5 * 6.9);
        x[n] = (float) (0.15 * env * (std::sin (w) + dbToGain (-25.0f) * std::sin (2.0 * w) + dbToGain (-35.0f) * std::sin (3.0 * w)));
    }
    const auto noise = sputter::noiseFloor (x.size(), -70.0f, seed);
    for (size_t n = 0; n < x.size(); ++n)
        x[n] += noise[n];
    return x;
}
// Whether a note plays: from 50 to 600 ms, 80% of the hops voiced on it (the quantizer's N), and the output's pitch on it.
bool plays (const std::vector<float>& x, int note, double ceilingHz, float windowHz, float sure = 0.0f)
{
    Engine e;
    e.prepare (kFs, 64);
    e.setCeilingHz (ceilingHz, sure);
    int on = 0, n = 0;
    e.hopListener = [&] (const HopInfo& h) {
        if (h.time < (int64_t) (0.05 * kFs) || h.time > (int64_t) (0.6 * kFs))
            return;
        ++n;
        on += h.detector.voiced && h.noteN == note && std::abs (h.midi - note) < 0.3 ? 1 : 0;
    };
    EngineParams p;
    p.windowHz = windowHz;
    runEngine (e, x, p);
    return n > 0 && on >= (int) (0.8 * n);
}
} // namespace ceiling

void testCeiling()
{
    using namespace ceiling;
    // The default is unchanged: the switch at 1400 Hz is the engine as it starts, bit for bit.
    {
        const auto in = regressionPhrase (kFs);
        Engine a, b;
        a.prepare (kFs, 64);
        b.prepare (kFs, 64);
        b.setCeilingHz (kDetectorMaxHz);
        EngineParams p;
        check (compareSamples (runEngine (a, in, p), runEngine (b, in, p)).values == 0 && a.ceilingHz() == kDetectorMaxHz,
               juce::String::formatted ("the ceiling's default is %.0f Hz (kDetectorMaxHz), and the switch at it = the engine as it starts (regression phrase, bit for bit)", kDetectorMaxHz));
    }
    // The detector's range at each ceiling: its shortest lag and the highest pitch it can read.
    {
        juce::String line;
        for (double c : kCeilings)
            for (double fs : { 44100.0, 48000.0, 96000.0 })
            {
                const int tMin = std::max (2, (int) std::floor (fs / c));
                line << juce::String::formatted ("%s%.0f Hz @ %.1f kHz: shortest lag %d (%.0f Hz)", line.isEmpty() ? "" : "; ", c, fs / 1000.0, tMin, fs / tMin);
            }
        info ("the detector's top (the shortest lag searched, floor (fs / ceiling)): " + line);
    }
    // Mute chirps: the real takes (every take in Recordings/ but GTT's own output) and synthetic mutes (a muted E4; palm-muted chugs),
    // at the defaults and Note Release Fast / Long with Sustain 100%.
    struct In
    {
        juce::String name;
        std::vector<float> x;
    };
    std::vector<In> ins;
    for (const auto& t : blips::takes())
        if (t.name != "blips")
            ins.push_back ({ t.name, t.x });
    ins.push_back ({ "synthetic muted E4", mutedPluck() });
    for (double bpm : { 100.0, 180.0 })
    {
        const auto c = chugs (bpm, { { 40, 0.9f }, { 45, 0.5f }, { 52, 0.9f }, { 57, 0.5f }, { 64, 0.9f }, { 64, 0.3f }, { 69, 0.9f } }, 0.04, 7);
        ins.push_back ({ juce::String::formatted ("synthetic chugs %.0f bpm", bpm), c.x });
    }
    std::vector<EngineParams> settings (3);
    settings[1].holdMs = noteReleaseMsFor (NoteRelease::Fast);
    settings[1].sustainPercent = 100.0f;
    settings[2].holdMs = noteReleaseMsFor (NoteRelease::Long);
    settings[2].sustainPercent = 100.0f;
    for (auto& p : settings)
        p.wave = Wave::Sine;
    for (double c : kCeilings)
      for (float sure : { 0.0f, kSureAbove })
    {
        if (c <= kDetectorMaxHz && sure > 0.0f)
            continue;
        Chirps total;
        juce::String per;
        for (const auto& in : ins)
        {
            Chirps sum;
            for (const auto& p : settings)
            {
                const auto r = chirps (in.x, c, p, sure);
                sum.hops += r.hops;
                sum.runs += r.runs;
                sum.edge += r.edge;
                sum.heardMs += r.heardMs;
                for (size_t q = 0; q < 5; ++q)
                    sum.byHz[q] += r.byHz[q];
                for (size_t q = 0; q < 3; ++q)
                    sum.byConf[q] += r.byConf[q];
                if (r.heardAt.isNotEmpty())
                    sum.heardAt << (sum.heardAt.isEmpty() ? "" : ";") << r.heardAt;
            }
            for (size_t q = 0; q < 5; ++q)
                total.byHz[q] += sum.byHz[q];
            for (size_t q = 0; q < 3; ++q)
                total.byConf[q] += sum.byConf[q];
            total.hops += sum.hops;
            total.runs += sum.runs;
            total.heardMs += sum.heardMs;
            if (sum.runs > 0)
                per << juce::String::formatted ("\n      %s: %d chirps, %d hops, %.0f ms heard", in.name.toRawUTF8(), sum.runs, sum.hops, sum.heardMs)
                    << (sum.heardAt.isNotEmpty() ? " at" + sum.heardAt : juce::String());
        }
        info (juce::String::formatted ("ceiling %.0f Hz%s, %d inputs x 3 settings: %d chirps above 1400 Hz (%d hops: by pitch 1400-1500 / -1760 / -2100 / -2500 / higher Hz %d / %d / %d / %d / %d; by confidence < 0.9 / < 0.97 / 0.97+ %d / %d / %d), %.0f ms heard above it",
                                       c, sure > 0.0f ? juce::String::formatted (" (above 1400 Hz only at %.2f+)", sure).toRawUTF8() : "", (int) ins.size(), total.runs, total.hops, total.byHz[0], total.byHz[1], total.byHz[2], total.byHz[3], total.byHz[4], total.byConf[0],
                                       total.byConf[1], total.byConf[2], total.heardMs)
              + per);
    }
    // The highest harmonic that plays: E6 .. E7 (a natural harmonic, nearly a sine; the 7th-fret harmonic on high e is B6), at the
    // default window and the top one.
    for (float w : { defaultWindowHz(), (float) kWindowMaxHz })
    {
        juce::String line;
        for (double c : kCeilings)
          for (float sure : { 0.0f, kSureAbove })
        {
            if (c <= kDetectorMaxHz && sure > 0.0f)
                continue;
            int highest = -1;
            juce::String gaps;
            for (int note = 85; note <= 100; ++note)
            {
                const bool ok = plays (harmonic (hzFromMidi (note), (uint32_t) note), note, c, w, sure);
                if (ok)
                    highest = note;
                else if (hzFromMidi (note) < c)
                    gaps << " " << sputter::noteName (note);
            }
            line << juce::String::formatted ("\n      ceiling %.0f Hz%s: highest %s (%.0f Hz)", c, sure > 0.0f ? " (sure above 1400)" : "", highest >= 0 ? sputter::noteName (highest).toRawUTF8() : "-", highest >= 0 ? hzFromMidi (highest) : 0.0)
                 << (gaps.isNotEmpty() ? "; under the ceiling but not playing:" + gaps : juce::String());
        }
        info (juce::String::formatted ("natural harmonics C#6 .. E7 at window %.0f Hz:", w) + line);
    }
}
