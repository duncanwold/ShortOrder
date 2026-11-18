// RC2 (v0.6.2): the RC1 QA report's easy fixes (docs/reports/QA-REPORT-RC1.md issues 1, 3, 10 and 12) and the note-start prototype (issue 6b),
// measured on the release-candidate takes against v0.6.1 (every v0.6.2 switch off = v0.6.1, `v0511-equivalence`). Report:
// docs/reports/RC2-REPORT.md. Clips to Recordings/RC2/clips/ (local). Uses rc1:: (the analysis), golden:: (the takes) and blips::.
//
//   probe-rc2-edge-map   where the detector's dip sits against the window's longest lag, on synthetic notes around every detent's floor
//                        and on the takes' edge readings (fix 1's design)
//   probe-rc2            v0.6.1 against this build on every RC take at the noted and as-heard settings: the QA analysis's counts, each
//                        fix's acceptance figures, the spots that changed; and the clips for GTT_RC2_FIX (1 .. 4) in
//                        Recordings/RC2/clips/<issue>/ (before = v0.6.1, after = this build, dry = the guitar, at the as-heard settings
//                        and input gain). Output: build/test-output/rc2/fix<n>.md
//   probe-rc2-regress    v0.6.1 against this build on the investigation takes in Recordings/ (blips-dry, hammer-ons, pull-offs, trills
//                        and the sputter takes): the blips score at the defaults and three other settings, and where the output differs

namespace rc2
{
using golden::kTakes;
using golden::kNumTakes;
using rc1::noteNameOf;
using rc1::hz;

// A steady pluck (Karplus-Strong, 1.2 s, 0.5 peak, 4 s decay) at hz.
std::vector<float> pluckAt (double hz, double fs = kFs, uint32_t seed = 3)
{
    return ksPluck ([hz] (double) { return hz; }, 1.2, 0.5f, ksLossFor (hz, 4.0), seed, fs);
}

// Fix 1's design: the edge reading's signature (DetectorResult::atLongEdge, beyondCents) on synthetic notes from 40 c under each
// detent's floor (fs / tauMax) to 60 c over it, and on the takes.
void probeEdgeMap()
{
    for (double fs : { 48000.0, 44100.0, 96000.0 })
        for (int k : { 0, 1, 4, 9, 14 })
        {
            const double w = (double) latencyDetentHz (k);
            const double floorHz = trackableFloorHz (fs, w);
            const int lowest = lowestReliableNote (fs, w);
            const double lowestC = 100.0 * (midiFromHz (hzFromMidi (lowest)) - midiFromHz (floorHz));
            juce::String line = juce::String::formatted ("@ %.1f kHz detent %2d (floor %.1f Hz, lowest reliable %s at +%.0f c):", fs / 1000.0, k, floorHz,
                                                         noteNameOf (lowest).toRawUTF8(), lowestC);
            for (int c = -40; c <= 60; c += 4)
            {
                const double f = floorHz * std::pow (2.0, c / 1200.0);
                const auto x = pluckAt (f, fs);
                Engine e;
                e.prepare (fs, 64);
                EngineParams p;
                p.windowHz = (float) w;
                int n = 0, voiced = 0, edge = 0, edgeVoiced = 0;
                std::vector<float> beyond;
                e.hopListener = [&] (const HopInfo& h) {
                    if (h.time < 0.05 * fs || h.time > 1.0 * fs)
                        return;
                    ++n;
                    voiced += h.detector.voiced;
                    if (h.detector.atLongEdge)
                    {
                        ++edge;
                        edgeVoiced += h.detector.voiced;
                        beyond.push_back (h.detector.beyondCents);
                    }
                };
                runEngine (e, x, p);
                std::sort (beyond.begin(), beyond.end());
                const float med = beyond.empty() ? -1.0f : beyond[beyond.size() / 2];
                line << juce::String::formatted ("\n      %+3d c (%s): voiced %3.0f%%, at the edge %3.0f%% (voiced %3.0f%%), beyond median %5.1f c", c,
                                                 hz (midiFromHz (f)).toRawUTF8(), 100.0 * voiced / std::max (1, n), 100.0 * edge / std::max (1, n),
                                                 100.0 * edgeVoiced / std::max (1, n), (double) med);
            }
            info (line);
        }

    // The takes: every edge reading at the as-heard settings, against the guitar's pitch (the measuring detector).
    for (int t = 0; t < kNumTakes; ++t)
    {
        const auto h = rc1::heard (t);
        const auto in = rc1::gained (golden::dry (t), h.gainDb);
        const auto tr = rc1::truthFor (in);
        Engine e;
        e.prepare (kFs, 64);
        std::vector<HopInfo> hops;
        e.hopListener = [&] (const HopInfo& x) { hops.push_back (x); };
        runEngine (e, in, h.p);
        const double floorMidi = midiFromHz (trackableFloorHz (kFs, clampWindow (h.p.windowHz)));
        struct Bin
        {
            int n = 0, voiced = 0;
            std::vector<double> guitar; // the guitar's pitch re. the floor (cents), where the measuring detector has one
        };
        std::map<int, Bin> byBeyond; // beyond (10 c bins)
        int edges = 0;
        for (const auto& x : hops)
        {
            if (! x.detector.atLongEdge || x.detector.rms < 1.0e-3f)
                continue;
            ++edges;
            const int bin = (int) std::floor (x.detector.beyondCents / 10.0f);
            auto& b = byBeyond[bin];
            ++b.n;
            b.voiced += x.detector.voiced;
            // The guitar's own pitch at this hop's window centre, if the measuring detector has one.
            const int k = tr.at ((double) x.time);
            if (k >= 0 && tr.ok[(size_t) k])
                b.guitar.push_back (100.0 * (tr.hops[(size_t) k].midi - floorMidi));
        }
        juce::String line = juce::String::formatted ("%-10s %d edge readings (window RMS over -60 dB); by the vertex past the edge:", kTakes[t].id, edges);
        for (auto& binEntry : byBeyond)
        {
            const int bin = binEntry.first;
            auto& b = binEntry.second;
            std::sort (b.guitar.begin(), b.guitar.end());
            auto q = [&] (double f) { return b.guitar.empty() ? 0.0 : b.guitar[(size_t) (f * (double) (b.guitar.size() - 1))]; };
            line << juce::String::formatted ("\n      %3d .. %3d c: %4d (voiced %4d); the guitar re. the floor (%d hops): median %+.0f c [10%% %+.0f, 90%% %+.0f]", bin * 10,
                                             bin * 10 + 10, b.n, b.voiced, (int) b.guitar.size(), q (0.5), q (0.1), q (0.9));
        }
        info (line);
    }
}

// Fix 1's test: a note at each detent's lowest reliable note plays, in tune and 30 c flat, at 44.1 / 48 / 96 kHz (every hop from 50 ms
// to 1 s voiced on that note, as with the rule off); a note 20 .. 60 c under the default's floor (issue 1's A#3 +38 c is 21 c under it)
// doesn't play the floor's note (B3); and the note just under the floor at the top detent (G#4, the floor G#4 +39 c), which the floor's
// reading plays in tune, plays as in v0.6.1.
void testBottomEdge()
{
    int worstVoiced = 100, cases = 0, unvoicedByRule = 0;
    juce::String bad;
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (int k = 0; k < kLatencyDetents; ++k)
            for (double flat : { 0.0, -30.0 })
            {
                const double w = (double) latencyDetentHz (k);
                const int lowest = lowestReliableNote (fs, w);
                const double f = hzFromMidi (lowest + flat / 100.0);
                const auto x = pluckAt (f, fs);
                Engine e;
                e.prepare (fs, 64);
                EngineParams p;
                p.windowHz = (float) w;
                int n = 0, onNote = 0;
                juce::String strays;
                e.hopListener = [&] (const HopInfo& h) {
                    if (h.edgeLag && h.detector.atLongEdge)
                        strays << juce::String::formatted (" %.3f s (%.1f c past, conf %.3f)", h.time / fs, (double) h.detector.beyondCents, (double) h.detector.confidence);
                    if (h.time < 0.05 * fs || h.time > 1.0 * fs)
                        return;
                    ++n;
                    onNote += h.detector.voiced && h.noteN == lowest;
                };
                const auto out = runEngine (e, x, p);
                Engine off;
                off.prepare (fs, 64);
                off.setBottomEdge (false);
                int offOnNote = 0;
                off.hopListener = [&] (const HopInfo& h) { offOnNote += h.time >= 0.05 * fs && h.time <= 1.0 * fs && h.detector.voiced && h.noteN == lowest; };
                const auto ref = runEngine (off, x, p);
                double earlyDb = 0.0, lateDb = 0.0; // the output's 10 ms RMS against the rule off's: before / from 100 ms
                const size_t win = (size_t) (0.01 * fs);
                for (size_t i = 0; i + win <= out.size(); i += win)
                {
                    const double a = rms (out, i, i + win), b = rms (ref, i, i + win);
                    if (b > 1e-4)
                        (i < (size_t) (0.1 * fs) ? earlyDb : lateDb) = std::max (i < (size_t) (0.1 * fs) ? earlyDb : lateDb, std::abs (toDb (std::max (a, 1e-9) / b)));
                }
                if (strays.isNotEmpty())
                    info (juce::String::formatted ("%.1f kHz detent %d %s %+.0f c: past the threshold at", fs / 1000.0, k, noteNameOf (lowest).toRawUTF8(), flat) + strays
                          + juce::String::formatted ("; the output within %.2f dB of the rule off's in its first 100 ms, %.3f dB after", earlyDb, lateDb));
                if (onNote + 1 < offOnNote || lateDb > 0.5)
                    bad << juce::String::formatted ("\n      %.1f kHz detent %d, %s %+.0f c: %d hops on it (rule off %d), %.2f dB off the rule off's output after 100 ms", fs / 1000.0, k,
                                                    noteNameOf (lowest).toRawUTF8(), flat, onNote, offOnNote, lateDb);
                const int share = (int) std::floor (100.0 * onNote / std::max (1, n));
                worstVoiced = std::min (worstVoiced, share);
                unvoicedByRule += e.bottomEdges();
                ++cases;
                if (share < 95)
                    bad << juce::String::formatted ("\n      %.1f kHz detent %d, %s %+.0f c: %d%% of the hops voiced on it", fs / 1000.0, k, noteNameOf (lowest).toRawUTF8(), flat, share);
            }
    check (bad.isEmpty(), juce::String::formatted ("the lowest reliable note, in tune and 30 c flat, plays at every detent at 44.1 / 48 / 96 kHz as with the rule off "
                                                   "(%d cases: at least 95%% of the hops from 50 ms to 1 s voiced on it, the worst %d%%; at most one hop fewer than with the "
                                                   "rule off; the output within 0.5 dB of it from 100 ms; %d hops unvoiced by the rule in all)", cases, worstVoiced, unvoicedByRule) + bad);

    // Under the floor: 20 .. 60 c (B3 -41 c is the default's floor at 48 kHz; issue 1's A#3 +38 c is 21 c under it). With the rule
    // off it reads at the edge and plays B3.
    for (bool on : { false, true })
    {
        juce::String line;
        int played = 0;
        for (double under : { 20.0, 30.0, 40.0, 60.0 })
        {
            const double fs = kFs, w = (double) defaultWindowHz();
            const double f = trackableFloorHz (fs, w) * std::pow (2.0, -under / 1200.0);
            const auto x = pluckAt (f, fs);
            Engine e;
            e.prepare (fs, 64);
            e.setBottomEdge (on);
            int n = 0, edgeNote = 0;
            const int edge = (int) std::lround (midiFromHz (trackableFloorHz (fs, w)));
            e.hopListener = [&] (const HopInfo& h) {
                if (h.time < 0.05 * fs || h.time > 1.0 * fs)
                    return;
                ++n;
                edgeNote += h.detector.voiced && h.noteN == edge;
            };
            runEngine (e, x, EngineParams {});
            played += edgeNote;
            line << juce::String::formatted (" %.0f c under: %d%%;", under, (int) std::lround (100.0 * edgeNote / std::max (1, n)));
        }
        if (on)
            check (played == 0, "notes 20 .. 60 c under the default's floor never play its lowest note (B3) with the rule (share of hops voiced on B3:" + line + ")");
        else
            info ("without the rule (v0.6.1) the same notes play B3 on this share of their hops:" + line);
    }
    // The top detent: G#4 in tune (39 c under the floor) and 20 c sharp read at the floor, which rounds to G#4: they play, as in v0.6.1.
    juce::String top;
    bool same = true;
    for (double cents : { 0.0, 20.0 })
    {
        int shares[2] = { 0, 0 };
        for (bool on : { false, true })
        {
            const auto x = pluckAt (hzFromMidi (68.0 + cents / 100.0), kFs);
            Engine e;
            e.prepare (kFs, 64);
            e.setBottomEdge (on);
            EngineParams p;
            p.windowHz = (float) kWindowMaxHz;
            int n = 0, g = 0;
            e.hopListener = [&] (const HopInfo& h) {
                if (h.time < 0.05 * kFs || h.time > 1.0 * kFs)
                    return;
                ++n;
                g += h.detector.voiced && h.noteN == 68;
            };
            runEngine (e, x, p);
            shares[on] = (int) std::lround (100.0 * g / std::max (1, n));
        }
        same = same && shares[1] >= shares[0] - 1;
        top << juce::String::formatted (" G#4 %+.0f c: %d%% -> %d%%;", cents, shares[0], shares[1]);
    }
    check (same, "at the top detent G#4, under the floor but rounding to the floor's own note, plays as in v0.6.1 (hops voiced on G#4, v0.6.1 -> the rule:" + top + ")");
}

// --- v0.6.1 against this build --------------------------------------------------------------------------------------------------
void asV061 (Engine& e) { e.setV062Fixes (false); }

std::vector<float> renderOne (const std::vector<float>& in, const EngineParams& p, bool v061, std::vector<float>* level = nullptr, int* latency = nullptr)
{
    Engine e;
    e.prepare (kFs, 64);
    if (v061)
        asV061 (e);
    if (level != nullptr)
    {
        level->assign (in.size(), 0.0f);
        e.voiceListener = [level] (const VoiceProbe& v) {
            if (v.t >= 0 && v.t < (int64_t) level->size())
                (*level)[(size_t) v.t] = v.level;
        };
    }
    auto out = runEngine (e, in, p);
    if (latency != nullptr)
        *latency = e.currentLatencySamples();
    return out;
}

// The below-zone stretches (rc1::belowZone's), summed: what the output does and what the header says.
struct BelowSummary
{
    double ms = 0, audMs = 0, tuneMs = 0, octaveMs = 0, otherMs = 0;
    int notes = 0;
    std::map<juce::String, double> header; // ms
};
BelowSummary belowSummary (const rc1::Analysis& a, const rc1::Truth& tr, const rc1::Level& in)
{
    BelowSummary s;
    for (const auto& e : rc1::belowZone (a, tr, in))
    {
        ++s.notes;
        s.ms += (e.t1 - e.t0) * 1000.0;
        // the event's text has the numbers: "... output sounds X ms (in tune T, octave off O, other R); header: a P%, b Q%"
        const auto t = e.text;
        auto num = [&] (const char* key) { return t.fromFirstOccurrenceOf (key, false, false).getDoubleValue(); };
        s.audMs += num ("output sounds ");
        s.tuneMs += num ("in tune ");
        s.octaveMs += num ("octave off ");
        s.otherMs += num ("other ");
        const auto hdr = t.fromFirstOccurrenceOf ("header: ", false, false);
        for (const auto& part : juce::StringArray::fromTokens (hdr, ",", ""))
        {
            const auto p = part.trim();
            const auto name = p.upToLastOccurrenceOf (" ", false, false);
            const double pct = p.fromLastOccurrenceOf (" ", false, false).getDoubleValue();
            s.header[name.startsWith ("tracking") ? juce::String ("tracking ...") : name] += pct / 100.0 * (e.t1 - e.t0) * 1000.0;
        }
    }
    return s;
}

// One-sample steps in the voice's level (VoiceProbe::level, before the gate; input time): 0.3 dB or more in one sample, the level
// over -60 dB, and 5x the largest change of the 8 samples on either side (a step, not a fast but smooth rise or fall).
struct Step
{
    double t;
    double db;
};
std::vector<Step> levelSteps (const std::vector<float>& level)
{
    std::vector<Step> v;
    auto change = [&] (size_t i) { return level[i - 1] > 1.0e-6f && level[i] > 0.0f ? toDb (level[i] / level[i - 1]) : 0.0; };
    for (size_t i = 10; i + 9 < level.size(); ++i)
    {
        if (level[i - 1] <= 1.0e-3f || level[i] <= 0.0f)
            continue;
        const double d = change (i);
        if (std::abs (d) < 0.3)
            continue;
        double around = 0.0;
        for (size_t k = i - 8; k <= i + 8; ++k)
            if (k != i)
                around = std::max (around, std::abs (change (k)));
        if (std::abs (d) >= 5.0 * around)
            v.push_back ({ (double) i / kFs, d });
    }
    return v;
}

// Where two renders differ audibly (output time): 1 ms frames where the 10 ms RMS differs by 1 dB or more (the louder within 40 dB of
// the take's peak), or the Sine twins' pitch by 20 c or more (both audible, within 40 dB of their peak), or one sounds and the other
// doesn't. Merged when under 30 ms apart.
struct Spot
{
    double t0, t1;       // output time, seconds
    double levelDb = 0.0, cents = 0.0, energy = 0.0;
};
std::vector<Spot> changedSpots (const std::vector<float>& a, const std::vector<float>& b, const std::vector<blips::Cycle>& ca,
                                const std::vector<blips::Cycle>& cb, int latency)
{
    const auto ea = rc1::levelOf (a).db, eb = rc1::levelOf (b).db;
    float peak = -200.0f;
    for (float v : ea)
        peak = std::max (peak, v);
    for (float v : eb)
        peak = std::max (peak, v);
    const size_t n = std::min (ea.size(), eb.size());
    auto pitchFrames = [n, latency] (const std::vector<blips::Cycle>& c) {
        std::vector<float> f (n, -1.0f);
        float pk = 0.0f;
        for (const auto& x : c)
            pk = std::max (pk, x.amp);
        for (size_t i = 1; i < c.size(); ++i)
        {
            if (c[i].amp < pk * 0.01f)
                continue;
            const auto f0 = (int64_t) ((c[i - 1].t + latency) / 48.0), f1 = (int64_t) ((c[i].t + latency) / 48.0);
            for (int64_t k = std::max<int64_t> (0, f0); k <= f1 && k < (int64_t) n; ++k)
                f[(size_t) k] = (float) c[i].midi;
        }
        return f;
    };
    const auto pa = pitchFrames (ca), pb = pitchFrames (cb);
    std::vector<Spot> v;
    for (size_t k = 0; k < n; ++k)
    {
        const bool loud = std::max (ea[k], eb[k]) > peak - 40.0f;
        const double dl = loud ? std::abs (ea[k] - eb[k]) : 0.0;
        double dc = 0.0;
        if (pa[k] > 0.0f && pb[k] > 0.0f)
            dc = 100.0 * std::abs (pa[k] - pb[k]);
        if (dl < 1.0 && dc < 20.0)
            continue;
        const double t = (double) k * 0.001;
        if (! v.empty() && t - v.back().t1 < 0.03)
            v.back().t1 = t + 0.001;
        else
            v.push_back ({ t, t + 0.001 });
        auto& s = v.back();
        s.levelDb = std::max (s.levelDb, dl);
        s.cents = std::max (s.cents, dc);
        s.energy += std::min (dl, 40.0) + std::min (dc, 1200.0) / 10.0;
    }
    return v;
}

// Clips: 2 s around `at` (input time): the guitar (peak at -3 dBFS), and the two renders at output time with one gain between them
// (the louder's peak at -3 dBFS), so their levels compare.
void writeClips (const juce::File& dir, const juce::String& base, const std::vector<float>& dry, const std::vector<float>& before,
                 const std::vector<float>& after, double at, int latency)
{
    dir.createDirectory();
    auto cut = [] (const std::vector<float>& x, double centre) {
        std::vector<float> y;
        for (auto i = (int64_t) ((centre - 1.0) * kFs); i < (int64_t) ((centre + 1.0) * kFs); ++i)
            y.push_back (i >= 0 && i < (int64_t) x.size() ? x[(size_t) i] : 0.0f);
        return y;
    };
    auto peak = [] (const std::vector<float>& y) {
        float p = 0.0f;
        for (float v : y)
            p = std::max (p, std::abs (v));
        return p;
    };
    auto d = cut (dry, at), b = cut (before, at + latency / kFs), a = cut (after, at + latency / kFs);
    const float gd = peak (d) > 0.0f ? 0.708f / peak (d) : 1.0f, gr = std::max (peak (a), peak (b)) > 0.0f ? 0.708f / std::max (peak (a), peak (b)) : 1.0f;
    for (auto& v : d)
        v *= gd;
    for (auto& v : b)
        v *= gr;
    for (auto& v : a)
        v *= gr;
    auto write = [&] (const juce::String& suffix, const std::vector<float>& y) {
        const auto f = dir.getChildFile (base + "-" + suffix + ".wav");
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (f.createOutputStream().release());
        if (stream == nullptr)
            return;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (kFs).withNumChannels (1).withBitsPerSample (24));
        if (writer == nullptr)
            return;
        const float* chans[1] = { y.data() };
        writer->writeFromFloatArrays (chans, 1, (int) y.size());
    };
    write ("dry", d);
    write ("before", b);
    write ("after", a);
}

juce::File clipsRoot() { return golden::repoDir().getChildFile ("Recordings").getChildFile ("RC2").getChildFile ("clips"); }

// One analysed run and what the report needs from it (the Analysis itself is dropped: it's large).
struct Summary
{
    std::map<juce::String, std::pair<int, double>> kinds; // event kind -> count, ms
    std::vector<rc1::Event> events;
    std::vector<rc1::Delay> delays;
    BelowSummary below;
    std::vector<Step> steps;
    std::vector<float> out; // the render (as heard only)
    std::vector<blips::Cycle> cyc; // its Sine twin's cycles (as heard only)
    int latency = 0;
    // fix 1: the fingerpick's A#3 (25.27 .. 26.74 s): blocks playing B3, blocks whose header says tracking B3, blocks in the span
    int b3Blocks = 0, b3Tracking = 0, spanBlocks = 0;
    // voiced hops at the edge (DetectorResult::atLongEdge) and those that moved the note
    int edgeVoiced = 0, edgeMoves = 0;
    juce::String edgeMoveTimes;
    // fix 3: time the guitar plays a note in the zone (the measuring detector, at or above the lowest reliable note, within 45 dB of the
    // take's level) and the header says too low (ms), of the time it plays one in the zone
    double inZoneMs = 0.0, inZoneTooLowMs = 0.0;
    // fix 3: the below-zone frames (the guitar under the lowest reliable note, as rc1::belowZone's stretches) by what the header's rule
    // saw: too low; tracking with the low detector hearing nothing single; tracking with it hearing a note in the zone; other
    double belowTooLow = 0, belowLowSilent = 0, belowLowInZone = 0, belowOther = 0;
    std::map<juce::String, int> belowTexts;
};

Summary summarise (const std::vector<float>& in, const rc1::Truth& tr, const rc1::Level& lv, const std::vector<blips::Pick>& ps, const EngineParams& p,
                   bool keep, const juce::String& take)
{
    Summary s;
    const auto a = rc1::analyse (in, p);
    std::vector<rc1::Event> ev;
    auto add = [&] (std::vector<rc1::Event> v) { ev.insert (ev.end(), v.begin(), v.end()); };
    blips::Score bs;
    add (rc1::wrongNotes (a, tr));
    add (rc1::pickBlips (a, tr, ps, &bs));
    add (rc1::dropouts (a, tr, lv));
    add (rc1::hangs (a, lv));
    add (rc1::frozenGlides (a));
    add (rc1::clicks (a));
    add (rc1::glideIns (tr, ps, bs));
    add (rc1::belowZone (a, tr, lv));
    add (rc1::levelJumps (a, lv, tr));
    juce::String chordLog;
    if (p.chordMode == ChordMode::Memory)
        add (rc1::freezeEvents (a, tr, chordLog));
    if (p.chordMode == ChordMode::Stack)
        add (rc1::harmonizeEvents (a, chordLog));
    // Fix 2's own measure: frozen-glide's runs (unvoiced, audible, T more than 30 c from N, 10 ms or more) in which T also stays put (moves
    // less than 0.5 c a hop): the freeze itself, without a glide still under way.
    {
        float peak = 0.0f;
        for (const auto& l : a.lite)
            peak = std::max (peak, l.gate * l.level);
        size_t k = 0;
        while (k < a.hops.size())
        {
            auto still = [&] (size_t i) {
                const auto& x = a.hops[i];
                const auto li = (size_t) std::max<int64_t> (0, x.time) / 16;
                const float lev = li < a.lite.size() ? a.lite[li].gate * a.lite[li].level : 0.0f;
                return i > 0 && x.hasNote && ! x.detector.voiced && std::abs (x.target - x.noteN) > 0.3 && lev > peak * 0.1f
                       && std::abs (x.target - a.hops[i - 1].target) < 0.005;
            };
            if (! still (k))
            {
                ++k;
                continue;
            }
            size_t e = k;
            while (e < a.hops.size() && still (e))
                ++e;
            const double ms = (double) (a.hops[e - 1].time - a.hops[k].time + 128) / kFs * 1000.0;
            if (ms >= 10.0)
            {
                rc1::Event x;
                x.kind = "frozen-still";
                x.t0 = a.hops[k].time / kFs;
                x.t1 = a.hops[e - 1].time / kFs;
                x.ms = ms;
                x.text = juce::String::formatted ("T held at %s, %+.0f c off N, for %.0f ms", rc1::hz (a.hops[k].target).toRawUTF8(), 100.0 * (a.hops[k].target - a.hops[k].noteN), ms);
                ev.push_back (x);
            }
            k = e;
        }
    }
    for (const auto& e : ev)
    {
        auto& k = s.kinds[e.kind];
        ++k.first;
        k.second += e.ms;
    }
    s.events = ev;
    s.delays = rc1::delaysFor (a, in, ps);
    s.below = belowSummary (a, tr, lv);
    s.latency = a.latency;
    if (keep)
    {
        s.out = a.out;
        s.cyc = a.cyc;
    }
    // fix 1's spot
    if (take == "fingerpick")
        for (size_t k = 0; k < a.blocks.size(); ++k)
        {
            const double t = (double) k * 64.0 - a.latency; // input time
            if (t < 25.27 * kFs || t > 26.74 * kFs)
                continue;
            ++s.spanBlocks;
            const auto& b = a.blocks[k];
            const bool sounding = b.hasNote && (b.voiced || b.open);
            s.b3Blocks += sounding && b.played == 59;
        }
    if (take == "fingerpick")
        for (size_t k = 0; k < a.status.size(); ++k)
        {
            const double t0 = a.status[k].t - a.latency, t1 = k + 1 < a.status.size() ? a.status[k + 1].t - a.latency : 1.0e12;
            if (a.status[k].text == "tracking B3")
                s.b3Tracking += (int) std::max (0.0, (std::min (t1, 26.74 * kFs) - std::max (t0, 25.27 * kFs)) / 64.0);
        }
    {
        const int lowest = lowestReliableNote (kFs, clampWindow (p.windowHz));
        size_t si = 0;
        juce::String shown;
        for (size_t k = 0; k < tr.hops.size(); ++k)
        {
            const double t = (double) tr.hops[k].time, tOut = t + a.latency;
            while (si < a.status.size() && a.status[si].t <= tOut)
                shown = a.status[si++].text;
            if (! tr.ok[k] || tr.hops[k].midi < lowest - 0.5 || lv.at (t) < lv.ref - 45.0f)
                continue;
            s.inZoneMs += 128.0 / kFs * 1000.0;
            if (shown.startsWith ("too low"))
                s.inZoneTooLowMs += 128.0 / kFs * 1000.0;
        }
    }
    for (const auto& e : rc1::belowZone (a, tr, lv))
        for (const auto& f : a.frames)
        {
            const double t = f.t - a.latency;
            if (t < e.t0 * kFs || t > e.t1 * kFs)
                continue;
            s.belowTexts[f.text.upToFirstOccurrenceOf (" ", false, false) + (f.text.startsWith ("tracking") ? (f.lowVoiced && f.lowNote < f.lowest ? " (low below)" : f.lowVoiced ? " (low in zone)" : " (low none)") : "")] += 1;
            if (f.text.startsWith ("too low"))
                s.belowTooLow += 1;
            else if (f.text.startsWith ("tracking") && ! f.lowVoiced)
                s.belowLowSilent += 1;
            else if (f.text.startsWith ("tracking") && f.lowNote >= f.lowest)
                s.belowLowInZone += 1;
            else
                s.belowOther += 1;
        }
    int prevN = -1;
    for (const auto& x : a.hops)
    {
        if (x.detector.atLongEdge && x.detector.voiced)
        {
            ++s.edgeVoiced;
            if (x.noteN != prevN)
            {
                ++s.edgeMoves;
                if (s.edgeMoveTimes.length() < 160)
                    s.edgeMoveTimes << juce::String::formatted (" %.2f", x.time / kFs);
            }
        }
        if (x.hasNote)
            prevN = x.noteN;
    }
    // the level's one-sample steps (the setting's own render, again with every sample of the voice)
    std::vector<float> level;
    renderOne (in, p, (bool) rc1::engineSetup, &level);
    s.steps = levelSteps (level);
    return s;
}

struct FixSpots
{
    int issue;
    const char* name;
    std::vector<std::pair<const char*, double>> spots; // take, input time (the QA report's)
};
FixSpots spotsForFix (int fix)
{
    switch (fix)
    {
        case 1: return { 1, "bottom-edge", { { "fingerpick", 25.40 }, { "fingerpick", 26.30 }, { "below", 7.41 }, { "below", 9.54 }, { "below", 10.18 }, { "below", 10.66 }, { "below", 14.63 }, { "below", 16.55 } } };
        case 2: return { 3, "frozen-glide", { { "flatpick", 2.69 }, { "flatpick", 6.78 }, { "flatpick", 7.86 }, { "flatpick", 9.83 } } };
        case 3: return { 0, "", {} }; // issue 10: the UI only, the audio is v0.6.1's bit for bit (no clips)
        // the QA report's spots, then every one-sample step v0.6.1 has on the takes as heard (levelSteps); no automatic spots (the
        // largest changes since v0.6.1 are fixes 1 and 2's pitch)
        case 4: return { 12, "level-tick", { { "fingerpick", 1.345 }, { "fingerpick", 13.26 }, { "fingerpick", 23.34 }, { "fingerpick", 34.92 }, { "freeze3", 1.62 },
                                              { "fingerpick", 6.017 }, { "fingerpick", 6.524 }, { "bass", 1.889 }, { "bass", 26.956 }, { "below", 12.468 },
                                              { "below", 14.631 }, { "below", 17.303 }, { "freeze3", 22.697 }, { "harmonics", 2.697 }, { "harmonics", 2.993 },
                                              { "harmonize", 2.748 }, { "harmonize", 13.121 }, { "harmonize", 17.639 }, { "harmonize", 39.967 } } };
        default: return { 0, "", {} };
    }
}

juce::String countsCell (const Summary& s, const juce::String& kind)
{
    const auto it = s.kinds.find (kind);
    if (it == s.kinds.end())
        return "0";
    return juce::String::formatted ("%d (%.0f ms)", it->second.first, it->second.second);
}

void probeRC2()
{
    const int fix = juce::SystemStats::getEnvironmentVariable ("GTT_RC2_FIX", "0").getIntValue();
    const auto dir = outDir.getChildFile ("rc2");
    dir.createDirectory();
    for (int t = 0; t < kNumTakes; ++t)
        golden::dry (t);

    struct Job
    {
        int take;
        bool heard, v061;
    };
    std::vector<Job> jobs;
    for (int t = 0; t < kNumTakes; ++t)
        for (bool heard : { false, true })
            jobs.push_back ({ t, heard, false });
    struct Inputs
    {
        std::vector<float> x;
        rc1::Truth tr;
        rc1::Level lv;
        std::vector<blips::Pick> ps;
        EngineParams p;
    };
    std::vector<Inputs> inputs ((size_t) kNumTakes * 2);
    sputter::parallelFor ((int) inputs.size(), [&] (int i) {
        const int t = i / 2;
        const bool heard = i % 2 == 1;
        const auto h = rc1::heard (t);
        auto& in = inputs[(size_t) i];
        in.x = heard ? rc1::gained (golden::dry (t), h.gainDb) : golden::dry (t);
        in.tr = rc1::truthFor (in.x);
        in.lv = rc1::levelOf (in.x);
        in.ps = blips::takePicks (in.x);
        in.p = heard ? h.p : golden::notedParams (t);
    });
    std::vector<Summary> before (inputs.size()), after (inputs.size());
    for (bool v061 : { true, false })
    {
        rc1::engineSetup = v061 ? std::function<void (Engine&)> (asV061) : std::function<void (Engine&)> {};
        rc1::headerV061 = v061; // and the header as v0.6.1 showed it (fix 3)
        auto& res = v061 ? before : after;
        sputter::parallelFor ((int) inputs.size(), [&] (int i) {
            const auto& in = inputs[(size_t) i];
            res[(size_t) i] = summarise (in.x, in.tr, in.lv, in.ps, in.p, i % 2 == 1, kTakes[i / 2].id);
        });
    }
    rc1::engineSetup = {};
    rc1::headerV061 = false;

    juce::String md;
    md << "# probe-rc2" << (fix > 0 ? juce::String (", fix ") + juce::String (fix) : juce::String()) << ": v0.6.1 -> this build\n\n";
    const char* kinds[] = { "wrong", "junk", "blip-under-3st", "blip-3-4st", "blip-5st+", "frozen-glide", "frozen-still", "stale-glide", "glide-in", "below", "below-junk",
                            "level-jump", "level-dip", "dropout", "hang", "click", "freeze-unplayed", "freeze-clear-while-playing", "harmonize-wrong", "harmonize-out-of-key" };
    for (bool heard : { false, true })
    {
        md << "## " << (heard ? "As heard" : "Noted settings") << ": counts (v0.6.1 -> this build)\n\n| take |";
        for (auto k : kinds)
            md << " " << k << " |";
        md << " level steps |\n|---|";
        for (size_t k = 0; k < std::size (kinds) + 1; ++k)
            md << "---|";
        md << "\n";
        for (int t = 0; t < kNumTakes; ++t)
        {
            const auto& b = before[(size_t) (2 * t + heard)];
            const auto& a = after[(size_t) (2 * t + heard)];
            md << "| " << kTakes[t].id << " |";
            for (auto k : kinds)
            {
                const auto cb = countsCell (b, k), ca = countsCell (a, k);
                md << " " << (cb == ca ? cb : cb + " -> **" + ca + "**") << " |";
            }
            md << " " << (int) b.steps.size() << (b.steps.size() == a.steps.size() ? juce::String() : " -> **" + juce::String ((int) a.steps.size()) + "**") << " |\n";
        }
        md << "\n";
    }

    // Fix 1's and issue 2's figures (as heard; the noted rows are the same in pitch)
    for (bool heard : { false, true })
    {
        md << "## The bottom of the zone (" << (heard ? "as heard" : "noted") << ")\n\n";
        for (int t = 0; t < kNumTakes; ++t)
        {
            const auto& b = before[(size_t) (2 * t + heard)];
            const auto& a = after[(size_t) (2 * t + heard)];
            if (b.edgeVoiced + a.edgeVoiced == 0 && b.below.notes == 0)
                continue;
            md << "- **" << kTakes[t].id << "**: voiced edge readings " << b.edgeVoiced << " -> " << a.edgeVoiced << ", of them moving the note " << b.edgeMoves << " ("
               << b.edgeMoveTimes.trim() << ") -> " << a.edgeMoves << (a.edgeMoves > 0 ? " (" + a.edgeMoveTimes.trim() + ")" : juce::String()) << "\n";
            if (juce::String (kTakes[t].id) == "fingerpick")
                md << juce::String::formatted ("  - 25.27 .. 26.74 s (A#3 +38 c): B3 sounding %.0f%% -> %.0f%% of the span; the header `tracking B3` %.0f%% -> %.0f%%\n",
                                               100.0 * b.b3Blocks / std::max (1, b.spanBlocks), 100.0 * a.b3Blocks / std::max (1, a.spanBlocks),
                                               100.0 * b.b3Tracking / std::max (1, b.spanBlocks), 100.0 * a.b3Tracking / std::max (1, a.spanBlocks));
            for (const auto* x : { &b, &a })
            {
                const auto& s = x->below;
                if (s.notes == 0)
                    continue;
                md << juce::String::formatted ("  - %s: %d below-zone notes, %.1f s: silent %.0f%%, in tune %.0f%%, an octave off %.0f%%, other (held / detuned / edge) %.0f%%; header:",
                                               x == &b ? "v0.6.1" : "this build", s.notes, s.ms / 1000.0, 100.0 * (1.0 - s.audMs / s.ms), 100.0 * s.tuneMs / s.ms,
                                               100.0 * s.octaveMs / s.ms, 100.0 * s.otherMs / s.ms);
                for (const auto& [name, ms] : s.header)
                    md << juce::String::formatted (" %s %.0f%%", name.toRawUTF8(), 100.0 * ms / s.ms);
                md << "\n";
            }
        }
        md << "\n";
    }

    // The events of the kinds named in GTT_RC2_KINDS (default: wrong, frozen-glide) that are new or gone (noted settings; matched by
    // kind and a start within 30 ms).
    {
        const auto kindList = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("GTT_RC2_KINDS", "wrong,frozen-glide"), ",", "");
        md << "## Events new / gone (noted; " << kindList.joinIntoString (", ") << ")\n\n";
        for (int t = 0; t < kNumTakes; ++t)
        {
            const auto& b = before[(size_t) (2 * t)];
            const auto& a = after[(size_t) (2 * t)];
            auto unmatched = [&] (const std::vector<rc1::Event>& x, const std::vector<rc1::Event>& y) {
                juce::String out;
                for (const auto& e : x)
                {
                    if (! kindList.contains (e.kind))
                        continue;
                    const bool found = std::any_of (y.begin(), y.end(), [&] (const rc1::Event& f) { return f.kind == e.kind && std::abs (f.t0 - e.t0) < 0.03; });
                    if (! found)
                        out << juce::String::formatted ("\n    - %.3f s %s: ", e.t0, e.kind.toRawUTF8()) << e.text;
                }
                return out;
            };
            const auto gone = unmatched (b.events, a.events), added = unmatched (a.events, b.events);
            if (gone.isEmpty() && added.isEmpty())
                continue;
            md << "- **" << kTakes[t].id << "**\n  - gone:" << (gone.isEmpty() ? juce::String (" none") : gone) << "\n  - new:" << (added.isEmpty() ? juce::String (" none") : added) << "\n";
        }
        md << "\n";
    }
    // Issue 2's level jumps on the below take, v0.6.1 against this build (noted settings)
    {
        int t = 0;
        for (int k = 0; k < kNumTakes; ++k)
            if (juce::String (kTakes[k].id) == "below")
                t = k;
        md << "## Level jumps on `below` (noted; issue 2)\n\n";
        for (const auto* x : { &before[(size_t) (2 * t)], &after[(size_t) (2 * t)] })
        {
            md << "- " << (x == &before[(size_t) (2 * t)] ? "v0.6.1:" : "this build:");
            for (const auto& e : x->events)
                if (e.kind == "level-jump")
                    md << juce::String::formatted (" %.2f", e.t0);
            md << "\n";
        }
        md << "\n";
    }
    // Fix 3: the below-zone frames by what the header's rule saw
    md << "## Below-zone frames by what the header's rule saw (v0.6.1 -> this build)\n\n";
    for (bool heard : { false, true })
        for (int t = 0; t < kNumTakes; ++t)
        {
            const auto& b = before[(size_t) (2 * t + heard)];
            const auto& a = after[(size_t) (2 * t + heard)];
            const double nb = b.belowTooLow + b.belowLowSilent + b.belowLowInZone + b.belowOther, na = a.belowTooLow + a.belowLowSilent + a.belowLowInZone + a.belowOther;
            if (na <= 0)
                continue;
            for (const auto* x : { &b, &a })
            {
                md << "  - " << (x == &b ? "v0.6.1" : "this build") << ":";
                for (const auto& kv : x->belowTexts)
                    md << " '" << kv.first << "' " << kv.second;
                md << "\n";
            }
            md << juce::String::formatted ("- %s %s, %d frames: too low %.0f%% -> %.0f%%; tracking, the low detector hearing no single note %.0f%% -> %.0f%%; tracking, it hearing a note in the zone %.0f%% -> %.0f%%; other (blank, ...) %.0f%% -> %.0f%%\n",
                                           kTakes[t].id, heard ? "as heard" : "noted", (int) na, 100 * b.belowTooLow / nb, 100 * a.belowTooLow / na, 100 * b.belowLowSilent / nb,
                                           100 * a.belowLowSilent / na, 100 * b.belowLowInZone / nb, 100 * a.belowLowInZone / na, 100 * b.belowOther / nb, 100 * a.belowOther / na);
        }
    md << "\n";
    // Fix 3: the header's too low while the guitar plays in the zone (false alarms)
    md << "## The header says too low while the guitar plays in the zone (v0.6.1 -> this build)\n\n";
    for (bool heard : { false, true })
    {
        md << "- " << (heard ? "as heard:" : "noted:");
        for (int t = 0; t < kNumTakes; ++t)
        {
            const auto& b = before[(size_t) (2 * t + heard)];
            const auto& a = after[(size_t) (2 * t + heard)];
            md << juce::String::formatted (" %s %.0f -> %.0f ms (of %.1f s);", kTakes[t].id, b.inZoneTooLowMs, a.inZoneTooLowMs, a.inZoneMs / 1000.0);
        }
        md << "\n";
    }
    md << "\n";
    // Level steps (as heard)
    md << "## One-sample level steps (as heard; 0.3 dB+, 5x the samples around it)\n\n";
    for (int t = 0; t < kNumTakes; ++t)
    {
        const auto& b = before[(size_t) (2 * t + 1)];
        const auto& a = after[(size_t) (2 * t + 1)];
        if (b.steps.empty() && a.steps.empty())
            continue;
        md << "- " << kTakes[t].id << ": v0.6.1";
        for (const auto& x : b.steps)
            md << juce::String::formatted (" %.4f s (%+.2f dB)", x.t, x.db);
        md << "; this build";
        for (const auto& x : a.steps)
            md << juce::String::formatted (" %.4f s (%+.2f dB)", x.t, x.db);
        md << "\n";
    }

    // Where the as-heard renders differ, and the clips
    const auto fs = spotsForFix (fix);
    const auto clipDir = clipsRoot().getChildFile (juce::String::formatted ("%02d-%s", fs.issue, fs.name));
    md << "\n## Where the as-heard renders differ (level 1 dB+ or pitch 20 c+; the largest, input time)\n\n";
    for (int t = 0; t < kNumTakes; ++t)
    {
        const auto& b = before[(size_t) (2 * t + 1)];
        const auto& a = after[(size_t) (2 * t + 1)];
        auto spots = changedSpots (b.out, a.out, b.cyc, a.cyc, a.latency);
        double total = 0.0;
        for (const auto& x : spots)
            total += x.t1 - x.t0;
        md << "- " << kTakes[t].id << ": " << (int) spots.size() << juce::String::formatted (" spots, %.2f s in all", total);
        auto byEnergy = spots;
        std::sort (byEnergy.begin(), byEnergy.end(), [] (const Spot& x, const Spot& y) { return x.energy > y.energy; });
        for (size_t k = 0; k < std::min<size_t> (12, byEnergy.size()); ++k)
            md << juce::String::formatted ("; %.3f .. %.3f s (%.1f dB, %.0f c)", byEnergy[k].t0 - a.latency / kFs, byEnergy[k].t1 - a.latency / kFs, byEnergy[k].levelDb,
                                           byEnergy[k].cents);
        md << "\n";
        if (fix <= 0 || fs.issue == 0)
            continue;
        // the QA report's spots for this fix, then the take's three largest other changes
        std::vector<double> at;
        for (const auto& [take, when] : fs.spots)
            if (juce::String (take) == kTakes[t].id)
                at.push_back (when);
        for (size_t k = 0; k < (fix == 4 ? (size_t) 0 : std::min<size_t> (3, byEnergy.size())); ++k)
        {
            const double c = 0.5 * (byEnergy[k].t0 + byEnergy[k].t1) - a.latency / kFs;
            if (std::none_of (at.begin(), at.end(), [c] (double x) { return std::abs (x - c) < 1.0; }))
                at.push_back (c);
        }
        for (double c : at)
            writeClips (clipDir, juce::String::formatted ("%s-%06.2f", kTakes[t].id, c), inputs[(size_t) (2 * t + 1)].x, b.out, a.out, c, a.latency);
    }
    if (fix > 0)
        md << "\nClips: `" << clipDir.getFullPathName() << "`\n";
    dir.getChildFile (fix > 0 ? juce::String::formatted ("fix%d.md", fix) : juce::String ("now.md")).replaceWithText (md);
    std::printf ("%s\n", md.toRawUTF8());
}

// The investigation takes in Recordings/ (blips-dry, hammer-ons, pull-offs, trills, the sputter takes; not blips.aif, GTT's own
// output): v0.6.1 against this build at five settings, scored as docs/reports/BLIPS-REPORT.md scores them (the Sine voice: picks that blip, ms
// audibly off) and where the output changes (level 1 dB+, pitch 20 c+).
void probeRegress()
{
    const int fix = juce::SystemStats::getEnvironmentVariable ("GTT_RC2_FIX", "0").getIntValue();
    struct Setting
    {
        const char* name;
        std::function<void (EngineParams&)> set;
    };
    const std::vector<Setting> settings = {
        { "defaults", [] (EngineParams&) {} },
        { "Retune Fast, Sustain 0", [] (EngineParams& p) { p.retuneSpeed = RetuneSpeed::Fast; p.sustainPercent = 0.0f; } },
        { "Retune Slow", [] (EngineParams& p) { p.retuneSpeed = RetuneSpeed::Slow; } },
        { "Release Fast", [] (EngineParams& p) { p.holdMs = noteReleaseMsFor (NoteRelease::Fast); } },
        { "Release Long, Sustain 100%, Freeze 3", [] (EngineParams& p) { p.holdMs = noteReleaseMsFor (NoteRelease::Long); p.sustainPercent = 100.0f; p.chordMode = ChordMode::Memory; p.memoryNotes = 3; } },
    };
    std::vector<const blips::Take*> takes;
    for (const auto& t : blips::takes())
        if (t.name != "blips")
            takes.push_back (&t);
    struct Row
    {
        juce::String text;
    };
    std::vector<Row> rows (takes.size() * settings.size());
    sputter::parallelFor ((int) rows.size(), [&] (int i) {
        const auto& t = *takes[(size_t) i / settings.size()];
        const auto& st = settings[(size_t) i % settings.size()];
        EngineParams p;
        st.set (p);
        p.wave = Wave::Sine; // the blips measure reads the voice's pitch cycle by cycle
        const auto ps = blips::takePicks (t.x);
        int latency = 0;
        const auto before = renderOne (t.x, p, true, nullptr, &latency), after = renderOne (t.x, p, false);
        const auto sb = blips::score (before, ps, latency), sa = blips::score (after, ps, latency);
        const auto cb = blips::cycles (before, latency), ca = blips::cycles (after, latency);
        const auto spots = changedSpots (before, after, cb, ca, latency);
        double total = 0.0;
        for (const auto& x : spots)
            total += x.t1 - x.t0;
        int differ = 0;
        for (size_t k = 0; k < before.size(); ++k)
            differ += before[k] != after[k];
        juce::String r = juce::String::formatted ("| %s | %s | %d (%.0f ms) | %d (%.0f ms) | %d | %d spots, %.2f s", t.name.toRawUTF8(), st.name, sb.blips, sb.ms, sa.blips, sa.ms, differ,
                                                  (int) spots.size(), total);
        auto byEnergy = spots;
        std::sort (byEnergy.begin(), byEnergy.end(), [] (const Spot& x, const Spot& y) { return x.energy > y.energy; });
        for (size_t k = 0; k < std::min<size_t> (4, byEnergy.size()); ++k)
            r << juce::String::formatted ("; %.3f s (%.1f dB, %.0f c)", byEnergy[k].t0 - latency / kFs, byEnergy[k].levelDb, byEnergy[k].cents);
        rows[(size_t) i].text = r + " |";
    });
    // Where fix 1 fired on these takes (the defaults): the hops it unvoiced.
    juce::String fired;
    for (const auto* t : takes)
    {
        Engine e;
        e.prepare (kFs, 64);
        juce::String where;
        int n = 0;
        e.hopListener = [&] (const HopInfo& h) {
            if (h.edgeLag && h.detector.atLongEdge && h.detector.f0 < 1000.0f)
            {
                ++n;
                if (where.length() < 400)
                    where << juce::String::formatted (" %.3f (%s %.2f, %.0f c past)", h.time / kFs, rc1::hz (h.rawMidi).toRawUTF8(), (double) h.detector.confidence,
                                                      (double) h.detector.beyondCents);
            }
        };
        runEngine (e, t->x, EngineParams {});
        if (n > 0)
            fired << "- " << t->name << ": " << n << " hops:" << where << "\n";
    }
    juce::String md;
    md << "# probe-rc2-regress" << (fix > 0 ? juce::String (", fix ") + juce::String (fix) : juce::String()) << ": v0.6.1 -> this build on the investigation takes\n\n";
    md << "| take | setting | blips v0.6.1 | blips now | samples that differ | where it changes (level 1 dB+, pitch 20 c+; the largest, input time) |\n|---|---|---|---|---|---|\n";
    for (const auto& r : rows)
        md << r.text << "\n";
    if (fired.isNotEmpty())
        md << "\nThe bottom edge rule (fix 1) at the defaults:\n\n" << fired;
    const auto dir = outDir.getChildFile ("rc2");
    dir.createDirectory();
    dir.getChildFile (fix > 0 ? juce::String::formatted ("regress-fix%d.md", fix) : juce::String ("regress-now.md")).replaceWithText (md);
    std::printf ("%s\n", md.toRawUTF8());
}

// v0.6.1 against this build, hop by hop, where they differ (GTT_HOPS="take from to", a take in Recordings/ or an RC take's id; the
// defaults, or the RC take's as-heard settings and gain): the reading, voiced, N, T, the event.
void probeHops()
{
    const auto parts = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("GTT_HOPS", "trills 11.40 11.47"), " ", "");
    std::vector<float> x;
    EngineParams p;
    for (const auto& t : blips::takes())
        if (t.name == parts[0])
            x = t.x;
    for (int k = 0; k < kNumTakes; ++k)
        if (parts[0] == kTakes[k].id)
        {
            const auto h = rc1::heard (k);
            const bool noted = parts.size() >= 4 && parts[3] == "noted";
            x = noted ? golden::dry (k) : rc1::gained (golden::dry (k), h.gainDb);
            p = noted ? golden::notedParams (k) : h.p;
        }
    const double from = parts[1].getDoubleValue() * kFs, to = parts[2].getDoubleValue() * kFs;
    std::vector<HopInfo> hb, ha;
    for (bool v061 : { true, false })
    {
        Engine e;
        e.prepare (kFs, 64);
        if (v061)
            asV061 (e);
        auto& hops = v061 ? hb : ha;
        e.hopListener = [&] (const HopInfo& h) { hops.push_back (h); };
        runEngine (e, x, p);
    }
    const char* ev[] = { "", "FRESH", "ALWAYS", "arrive", "change", "SNAP", "PICK" };
    auto cell = [&] (const HopInfo& h) {
        return juce::String::formatted ("%-8s %.2f %s %s%s N %-4s T %6.2f %-6s", rc1::hz (h.rawMidi).toRawUTF8(), (double) h.detector.confidence, h.detector.voiced ? "V" : ".",
                                        h.lockHeld ? "L" : " ", h.edgeLag ? "E" : " ", noteNameOf (h.noteN).toRawUTF8(), h.target, ev[std::clamp (h.event, 0, 6)]);
    };
    info ("    time s | v0.6.1                                          | this build");
    for (size_t k = 0; k < std::min (hb.size(), ha.size()); ++k)
    {
        if (hb[k].time < from || hb[k].time > to)
            continue;
        const auto a = cell (hb[k]), b = cell (ha[k]);
        info (juce::String::formatted ("%9.3f %s | ", hb[k].time / kFs, a == b ? " " : "*") + a + " | " + (a == b ? juce::String ("=") : b));
    }
}

// The analysis's Sine render around a spot (GTT_SPOT="take seconds [heard]"), v0.6.1 and this build: the largest 2nd difference within
// 20 ms (output time), and the clicks rc1::clicks finds within 50 ms.
void probeClickAt()
{
    const auto parts = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("GTT_SPOT", "below 7.662"), " ", "");
    int t = 0;
    for (int k = 0; k < kNumTakes; ++k)
        if (parts[0] == kTakes[k].id)
            t = k;
    const double at = parts[1].getDoubleValue();
    const bool heard = parts.size() >= 3 && parts[2] == "heard";
    const auto h = rc1::heard (t);
    const auto in = heard ? rc1::gained (golden::dry (t), h.gainDb) : golden::dry (t);
    const auto p = heard ? h.p : golden::notedParams (t);
    for (bool v061 : { true, false })
    {
        rc1::engineSetup = v061 ? std::function<void (Engine&)> (asV061) : std::function<void (Engine&)> {};
        const auto a = rc1::analyse (in, p);
        rc1::engineSetup = {};
        const auto c = (int64_t) (at * kFs) + a.latency;
        float worst = 0.0f;
        int64_t where = 0;
        for (int64_t i = c - (int64_t) (0.02 * kFs); i < c + (int64_t) (0.02 * kFs); ++i)
            if (i >= 2 && i < (int64_t) a.sine.size())
            {
                const float d = std::abs (a.sine[(size_t) i] - 2.0f * a.sine[(size_t) i - 1] + a.sine[(size_t) i - 2]);
                if (d > worst)
                {
                    worst = d;
                    where = i;
                }
            }
        juce::String line = juce::String::formatted ("%s: the largest 2nd difference within 20 ms: %.2e at %.5f s (input time)", v061 ? "v0.6.1" : "this build", (double) worst,
                                                     (double) (where - a.latency) / kFs);
        for (const auto& e : rc1::clicks (a))
            if (std::abs (e.t0 - at) < 0.05)
                line << juce::String::formatted ("; click at %.4f s (%.0f dB)", e.t0, e.size);
        for (int64_t i = where - 3; i <= where + 3; ++i)
            if (i >= 0 && i < (int64_t) a.sine.size())
                line << juce::String::formatted ("\n      %.5f s  %+.5f", (double) (i - a.latency) / kFs, (double) a.sine[(size_t) i]);
        info (line);
        // the voice's stages at the step
        Engine e;
        e.prepare (kFs, 64);
        if (v061)
            asV061 (e);
        auto q = p;
        q.wave = Wave::Sine;
        q.octave = Octave::Main;
        q.chordMode = ChordMode::Off;
        const auto stepAt = where - a.latency;
        juce::String st;
        e.voiceListener = [&] (const VoiceProbe& v) {
            if (v.t >= stepAt - 3 && v.t <= stepAt + 2)
                st << juce::String::formatted ("\n      %.5f s  voiced %d open %d graced %d capped %d onset %d  gate %.4f env %.5f ref %.5f level %.5f dyn %.3f", (double) v.t / kFs,
                                               (int) v.voiced, (int) v.open, (int) v.graced, (int) v.capped, (int) v.onset, (double) v.gate, (double) v.env, (double) v.ref,
                                               (double) v.level, (double) v.dynamicsGain);
        };
        runEngine (e, in, q);
        info ("    the voice:" + st);
    }
}

// Freeze's timeline (rc1::freezeEvents' log) on an investigation take, v0.6.1 against this build (GTT_FREEZE="take from to"; Release
// Long, Sustain 100%, Freeze 3, Sine, as probe-rc2-regress's last row).
void probeFreezeAt()
{
    const auto parts = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("GTT_FREEZE", "pull-offs 12.3 12.6"), " ", "");
    std::vector<float> x;
    for (const auto& t : blips::takes())
        if (t.name == parts[0])
            x = t.x;
    const double from = parts[1].getDoubleValue(), to = parts[2].getDoubleValue();
    EngineParams p;
    p.holdMs = noteReleaseMsFor (NoteRelease::Long);
    p.sustainPercent = 100.0f;
    p.chordMode = ChordMode::Memory;
    p.memoryNotes = 3;
    p.wave = Wave::Sine;
    const auto tr = rc1::truthFor (x);
    for (bool v061 : { true, false })
    {
        rc1::engineSetup = v061 ? std::function<void (Engine&)> (asV061) : std::function<void (Engine&)> {};
        const auto a = rc1::analyse (x, p);
        rc1::engineSetup = {};
        juce::String log;
        const auto ev = rc1::freezeEvents (a, tr, log);
        juce::String out;
        for (const auto& line : juce::StringArray::fromLines (log))
        {
            const double t = line.trim().getDoubleValue();
            if (t >= from && t <= to)
                out << "\n  " << line;
        }
        int unplayed = 0;
        for (const auto& e : ev)
            unplayed += e.kind == "freeze-unplayed";
        info (juce::String (v061 ? "v0.6.1" : "this build") + juce::String::formatted (" (%d held notes the guitar didn't play in the take):", unplayed) + out);
    }
}

// Fix 4's test (QA issue 12): Sustain's level through a release that starts part-way through a 5 ms blend (pitch back after a dropout,
// then lost again 2 ms later), and through the one-way cap set part-way through a re-pick's lift: no one-sample step with the fix
// (the largest step there no bigger than the blend's own), the step v0.6.1 had without it.
void testLevelTick()
{
    const double fs = kFs;
    auto run = [fs] (bool fromLevel, bool capCase, double& worstAt, double& worstElse) {
        NoteReference ref;
        ref.prepare (fs);
        SustainLevel sus;
        sus.prepare (fs);
        sus.setBlendFromLevel (fromLevel);
        const int n = (int) (0.4 * fs), release = msToSamples (fs, 500.0);
        float prev = 0.0f;
        int lost = 0;
        worstAt = worstElse = 0.0;
        // voiced 0 .. 100 ms (a note at 0.1, decaying), lost 100 .. 130 ms, back 130 .. 132 ms (the blend under way), lost again from 132 ms
        for (int i = 0; i < n; ++i)
        {
            const double t = i / fs;
            const bool voiced = t < 0.1 || (t >= 0.13 && t < 0.132) || (capCase && t >= 0.2);
            const float env = 0.1f * (float) std::exp (-t / 0.3);
            ref.process (env, i == 0, voiced, i == 0, 64);
            if (capCase)
            {
                sus.setOneWay (t >= 0.12); // in the tail: the one-way release
                if (t >= 0.199 && t < 0.1995)
                    sus.liftCap(); // a re-pick's lift, 1 ms before pitch comes back (the cap is set part-way through its blend)
            }
            lost = voiced ? 0 : lost + 1;
            const float out = sus.process (ref, env, 0.75f, voiced, lost, release);
            if (i > 0)
            {
                const double d = std::abs (toDb (std::max (out, 1e-9f) / std::max (prev, 1e-9f)));
                const bool at = capCase ? (t >= 0.2 && t < 0.2002) : (t >= 0.132 && t < 0.1322);
                (at ? worstAt : worstElse) = std::max (at ? worstAt : worstElse, d);
            }
            prev = out;
        }
    };
    for (bool capCase : { false, true })
    {
        double atOff, elseOff, atOn, elseOn;
        run (false, capCase, atOff, elseOff);
        run (true, capCase, atOn, elseOn);
        const char* what = capCase ? "the one-way cap set part-way through a re-pick's lift" : "a release starting part-way through a 5 ms blend";
        check (atOn <= std::max (elseOn, 0.01) * 1.5,
               juce::String::formatted ("%s: the largest one-sample step there %.3f dB with the fix (elsewhere %.3f dB); v0.6.1 %.3f dB (elsewhere %.3f dB)", what, atOn, elseOn, atOff, elseOff));
    }
}

} // namespace rc2
