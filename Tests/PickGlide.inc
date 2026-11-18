// v0.5.11: picks that glide in from the note before them (docs/history/SPEC-v0.5.11.md section 2). The diagnosis (probe-pick-glide) and the
// test (pick-glide). Included by OfflineTests.cpp after BlipsProbes.inc (it uses blips::).

namespace pickglide
{
// Picked notes, each one ringing right up to the next pick, which ends it in one of these ways (a pick on the same string).
enum class Join
{
    Ring,  // the new fret stops the old note at the pick (1 ms)
    Damp,  // the pick touches the string first: the old note falls dampDb over 4 ms, dampMs before the pick
    Stop,  // the old note is muted (10 ms fade) gapMs before the pick: a genuine stop
};
struct Phrase
{
    juce::String name;
    std::vector<float> x;
    std::vector<int64_t> picks; // input samples
    std::vector<int> notes;     // MIDI, one per pick
    std::vector<int64_t> lastOfPrevious; // where the previous note's level starts falling (the damp or the stop; the pick for Ring)
};
const int kNotes[] = { 64, 67, 62, 69, 71, 64, 72, 67, 60, 65 }; // E4 G4 D4 A4 B4 E4 C5 G4 C4 F4: steps of 2 .. 8 semitones, up and down

Phrase phrase (Join join, double noteMs, double dampMs, float dampDb, double gapMs, uint32_t seed = 31)
{
    Phrase ph;
    const int n = (int) std::size (kNotes);
    const double fs = kFs;
    const size_t noteLen = (size_t) (noteMs * 0.001 * fs), gapLen = join == Join::Stop ? (size_t) (gapMs * 0.001 * fs) : 0;
    ph.x.assign ((size_t) n * (noteLen + gapLen) + (size_t) fs, 0.0f);
    size_t at = (size_t) (0.1 * fs);
    std::vector<int64_t> falls;
    for (int k = 0; k < n; ++k)
    {
        const double hz = hzFromMidi (kNotes[k]);
        const bool last = k == n - 1;
        const size_t len = last ? (size_t) (0.8 * fs) : noteLen;
        auto y = ksPluck ([hz] (double) { return hz; }, (double) len / fs, 0.5f, ksLossFor (hz, 3.0), seed + (uint32_t) k, fs);
        int64_t fallAt = (int64_t) (at + len);
        if (! last)
        {
            if (join == Join::Damp)
            {
                const size_t d = std::min (len, (size_t) (dampMs * 0.001 * fs)), ramp = (size_t) (0.004 * fs);
                fallAt = (int64_t) (at + len - d);
                const float floor = dbToGain (-dampDb);
                for (size_t i = len - d; i < len; ++i)
                {
                    const float p = std::min (1.0f, (float) (i - (len - d)) / (float) ramp);
                    y[i] *= 1.0f + (floor - 1.0f) * p;
                }
            }
            const size_t fade = (size_t) ((join == Join::Stop ? 0.010 : 0.001) * fs);
            for (size_t i = 0; i < fade && i < len; ++i)
                y[len - 1 - i] *= (float) i / (float) fade;
            if (join == Join::Stop)
                fallAt = (int64_t) (at + len - fade);
        }
        for (size_t i = 0; i < len && at + i < ph.x.size(); ++i)
            ph.x[at + i] += y[i];
        ph.picks.push_back ((int64_t) at);
        ph.notes.push_back (kNotes[k]);
        falls.push_back (fallAt);
        at += len + (last ? 0 : gapLen);
    }
    ph.lastOfPrevious.push_back (-1);
    for (size_t k = 1; k < ph.picks.size(); ++k)
        ph.lastOfPrevious.push_back (falls[k - 1]);
    const auto noise = sputter::noiseFloor (ph.x.size(), -70.0f, seed);
    for (size_t k = 0; k < ph.x.size(); ++k)
        ph.x[k] += noise[k];
    const char* joins[] = { "ringing into the next pick", "damped", "stopped" };
    ph.name = juce::String (joins[(int) join]);
    if (join == Join::Damp)
        ph.name << juce::String::formatted (" %.0f ms before the pick, -%.0f dB", dampMs, dampDb);
    if (join == Join::Stop)
        ph.name << juce::String::formatted (", %.0f ms of silence before the pick", gapMs);
    ph.name << juce::String::formatted (" (%.0f ms notes)", noteMs);
    return ph;
}

// What the output did at one pick (a Sine voice, cycle by cycle): how long it spent between the previous note and the new one
// (more than 50 c from both) before it reached the new one, and where it started (the first sounding cycle after the pick, in
// semitones re. the new note).
struct Glide
{
    double betweenMs = 0.0; // > a few ms: glided in
    double startSt = 0.0;   // where the output was at the pick's first cycle (re. the new note)
    double reachMs = -1.0;  // the new note within 50 c, after the pick
    bool glided() const { return betweenMs >= 8.0; }
};
Glide glideAt (const std::vector<blips::Cycle>& cyc, int64_t pick, int64_t next, int from, int to)
{
    Glide g;
    const double lo = std::min (from, to) + 0.5, hi = std::max (from, to) - 0.5;
    bool first = true;
    double prevT = -1.0;
    for (const auto& c : cyc)
    {
        if (c.t < (double) pick || c.t >= (double) next)
            continue;
        if (first)
        {
            g.startSt = c.midi - to;
            first = false;
        }
        if (std::abs (c.midi - to) <= 0.5)
        {
            g.reachMs = 1000.0 * (c.t - (double) pick) / kFs;
            break;
        }
        if (c.midi > lo && c.midi < hi && prevT >= 0.0)
            g.betweenMs += 1000.0 * (c.t - prevT) / kFs;
        prevT = c.t;
    }
    return g;
}

// The engine's own view at a pick (the current engine, hop by hop): what closed the glide window.
struct HopView
{
    int event = -1;          // Quantizer::Event at the hop the new note arrives (its N first equal to the new note)
    bool open = false;       // NoteContext::voiceOpen at that hop
    bool muted = false;      // the mute gate tripped between the previous note's fall and the arrival
    int lockHeld = 0;        // hops the fall / harmonic lock held between them
    int unvoiced = 0;        // unvoiced hops just before the arrival
    double sinceVoicedMs = 0.0; // from the previous note's last voiced hop to the arrival
    double fromTarget = 0.0; // T just before the arrival, re. the previous note (semitones)
};
HopView hopView (const std::vector<HopInfo>& hops, int64_t fall, int64_t pick, int64_t next, int from, int to)
{
    HopView v;
    int64_t lastFrom = -1;
    double lastT = 0.0;
    int run = 0;
    for (const auto& h : hops)
    {
        if (h.time < fall - (int64_t) (0.03 * kFs) || h.time >= next)
            continue;
        if (h.time < pick + (int64_t) (0.2 * kFs) && h.event != (int) Quantizer::Event::None && h.noteN == to && h.time >= pick - (int64_t) (0.005 * kFs))
        {
            v.event = h.event;
            v.open = h.voiceOpen;
            v.unvoiced = run;
            v.sinceVoicedMs = lastFrom >= 0 ? 1000.0 * (double) (h.time - lastFrom) / kFs : -1.0;
            v.fromTarget = lastT - from;
            return v;
        }
        if (h.time >= fall - (int64_t) (0.005 * kFs))
        {
            v.muted = v.muted || h.muted;
            v.lockHeld += h.lockHeld ? 1 : 0;
        }
        if (h.detector.voiced && h.noteN == from && ! h.lockHeld)
            lastFrom = h.time;
        run = h.detector.voiced ? 0 : run + 1;
        lastT = h.target;
    }
    return v;
}
// The output's cycles (blips::cycles) without the ones that touch silence: where the voice gate has closed the output is exactly
// 0, and the cycle across the gap (or the half cycle into it) isn't a pitch (at Note Release Fast every damp closes the voice).
std::vector<blips::Cycle> soundingCycles (const std::vector<float>& y, int latency)
{
    std::vector<int> silentUpTo (y.size() + 1, 0); // running count of silent samples (|y| < 1e-7, 16 in a row)
    std::vector<char> silent (y.size(), 0);
    int run = 0;
    for (size_t n = 0; n < y.size(); ++n)
    {
        run = std::abs (y[n]) < 1.0e-7f ? run + 1 : 0;
        if (run >= 16)
            for (int k = 0; k < 16 && k <= (int) n; ++k)
                silent[n - (size_t) k] = 1;
    }
    for (size_t n = 0; n < y.size(); ++n)
        silentUpTo[n + 1] = silentUpTo[n] + silent[n];
    std::vector<blips::Cycle> out;
    for (const auto& c : blips::cycles (y, latency))
    {
        const double mid = c.t + latency, half = kFs / hzFromMidi (c.midi) * 0.5 + 0.002 * kFs; // the cycle and 2 ms either side
        const auto a = (size_t) std::clamp (mid - half, 0.0, (double) y.size()), b = (size_t) std::clamp (mid + half, 0.0, (double) y.size());
        if (silentUpTo[b] - silentUpTo[a] == 0)
            out.push_back (c);
    }
    return out;
}

const char* eventName (int e)
{
    switch (e)
    {
        case 1: return "fresh latch";
        case 2: return "Always latch";
        case 3: return "arrival";
        case 4: return "change";
        case 5: return "snap";
        case 6: return "pick glide";
        default: return "none";
    }
}

EngineParams sineAt (float holdMs)
{
    EngineParams p;
    p.wave = Wave::Sine;
    p.holdMs = holdMs;
    return p;
}

// One phrase through one engine setup: how many picks (after the first) glided in, and the hop view of each.
struct Score
{
    int glided = 0, picks = 0;
    juce::String detail;
};
Score score (const Phrase& ph, const std::vector<float>& out, int latency, const std::vector<HopInfo>* hops = nullptr, bool verbose = false)
{
    Score s;
    const auto cyc = soundingCycles (out, latency);
    for (size_t k = 1; k < ph.picks.size(); ++k)
    {
        const int64_t next = k + 1 < ph.picks.size() ? ph.picks[k + 1] : (int64_t) ph.x.size();
        const auto g = glideAt (cyc, ph.picks[k], next, ph.notes[k - 1], ph.notes[k]);
        ++s.picks;
        s.glided += g.glided() ? 1 : 0;
        if (verbose && hops != nullptr)
        {
            const auto v = hopView (*hops, ph.lastOfPrevious[k], ph.picks[k], next, ph.notes[k - 1], ph.notes[k]);
            s.detail << juce::String::formatted ("\n        %5.3f s %s -> %s: %s (between %4.1f ms, start %+5.1f st, reached %5.1f ms); hop: %s, voice %s, %s, locks %d, %d unvoiced, %.0f ms since the old note's last voiced hop, T %+.2f st off the old note",
                                                 (double) ph.picks[k] / kFs, sputter::noteName (ph.notes[k - 1]).toRawUTF8(), sputter::noteName (ph.notes[k]).toRawUTF8(),
                                                 g.glided() ? "GLIDES" : "fresh ", g.betweenMs, g.startSt, g.reachMs, eventName (v.event), v.open ? "open" : "CLOSED",
                                                 v.muted ? "MUTED" : "no mute", v.lockHeld, v.unvoiced, v.sinceVoicedMs, v.fromTarget);
        }
    }
    return s;
}
} // namespace pickglide

// The diagnosis: what closes the glide window between a ringing note and the next pick (docs/history/SPEC-v0.5.11.md section 2).
void probePickGlide()
{
    using namespace pickglide;
    std::vector<Phrase> phrases;
    for (double noteMs : { 300.0, 600.0 })
    {
        phrases.push_back (phrase (Join::Ring, noteMs, 0, 0, 0));
        for (double d : { 15.0, 30.0, 45.0 })
            for (float db : { 15.0f, 25.0f, 35.0f })
                phrases.push_back (phrase (Join::Damp, noteMs, d, db, 0));
        for (double gap : { 30.0, 60.0, 100.0, 150.0, 250.0 })
            phrases.push_back (phrase (Join::Stop, noteMs, 0, 0, gap));
    }
    struct Setup
    {
        const char* name;
        std::function<void (Engine&)> set;
    };
    const Setup setups[] = {
        { "v0.5.11 as v0.5.10 (defaults)", [] (Engine&) {} },
        { "no mute gate", [] (Engine& e) { auto t = sputterFix(); t.muteDropDb = 0.0f; e.setTailSwitches (t); } },
        { "no fall lock", [] (Engine& e) { e.setBlipFixes (false, true); } },
        { "no revival guard", [] (Engine& e) { auto t = sputterFix(); t.reviveRiseDb = 0.0f; e.setTailSwitches (t); } },
        { "no grace", [] (Engine& e) { auto t = sputterFix(); t.graceMs = 0.0f; t.graceInstantMs = 0.0f; e.setTailSwitches (t); } },
        { "sputter fix off (v0.5.4's tail)", [] (Engine& e) { e.setSputterFix (false); } },
    };
    for (auto [relName, rel] : { std::pair { "Fast", NoteRelease::Fast }, { "Medium", NoteRelease::Medium }, { "Long", NoteRelease::Long } })
    {
        const auto p = sineAt (noteReleaseMsFor (rel));
        info (juce::String ("Note Release ") + relName + " (Sine, Sustain 75%, Retune Medium, Glide Always): picks that glide in from the note before, of 9 a phrase");
        for (const auto& ph : phrases)
        {
            juce::String line = "  " + ph.name + ":";
            for (const auto& su : setups)
            {
                Engine e;
                e.prepare (kFs, 64);
                su.set (e);
                const auto out = runEngine (e, ph.x, p);
                const auto s = score (ph, out, e.currentLatencySamples());
                line << juce::String::formatted (" %s %d", su.name, s.glided) << ";";
            }
            info (line);
        }
    }
    // Pick by pick at the default (Medium), with the engine's own hop view.
    for (const auto& ph : phrases)
    {
        Engine e;
        e.prepare (kFs, 64);
        std::vector<HopInfo> hops;
        e.hopListener = [&hops] (const HopInfo& h) { hops.push_back (h); };
        const auto out = runEngine (e, ph.x, sineAt (kDefaultNoteReleaseMs));
        const auto s = score (ph, out, e.currentLatencySamples(), &hops, true);
        info ("  Medium, " + ph.name + juce::String::formatted (": %d of %d glide", s.glided, s.picks) + s.detail);
    }
    // The real takes: every pick (the engine's onsets), at Fast, Medium and Long. For each pick that brings a note (its first hop
    // with a quantizer event), the gap from the last real hop before it (voiced, not held by a lock, the window RMS within 9 dB of
    // its largest of the last 50 ms: before any damp) to that hop, and what the quantizer did.
    for (auto [relName, rel] : { std::pair { "Fast", NoteRelease::Fast }, { "Medium", NoteRelease::Medium }, { "Long", NoteRelease::Long } })
        for (const auto& t : blips::takes())
        {
            if (t.name == "blips")
                continue;
            const auto r = blips::run (t.x, sineAt (noteReleaseMsFor (rel)), {}, true);
            const auto pk = blips::picks (t.x, r.onsets);
            std::map<int, int> counts;
            std::vector<double> gaps;
            juce::String list;
            const int ringHops = (int) std::lround (0.05 * kFs / 128.0);
            for (size_t k = 1; k < pk.size(); ++k)
            {
                const HopInfo* arrival = nullptr;
                int64_t lastReal = -1;
                int lastRealNote = -1;
                bool mutedBetween = false;
                std::vector<float> ring;
                for (const auto& h : r.hops)
                {
                    if (h.time >= pk[k].end)
                        break;
                    float mx = 0.0f;
                    for (size_t q = ring.size() > (size_t) ringHops ? ring.size() - (size_t) ringHops : 0; q < ring.size(); ++q)
                        mx = std::max (mx, ring[q]);
                    ring.push_back (h.detector.rms);
                    if (h.time >= pk[k].t - (int64_t) (0.005 * kFs) && h.event != (int) Quantizer::Event::None)
                    {
                        arrival = &h;
                        break;
                    }
                    if (h.detector.voiced && ! h.lockHeld && h.detector.rms >= mx * dbToGain (-9.0f))
                    {
                        lastReal = h.time;
                        lastRealNote = h.noteN;
                        mutedBetween = false;
                    }
                    mutedBetween = mutedBetween || h.muted;
                }
                if (arrival == nullptr)
                {
                    ++counts[-1];
                    continue;
                }
                ++counts[arrival->event];
                const double gap = lastReal >= 0 ? 1000.0 * (double) (arrival->time - lastReal) / kFs : -1.0;
                if (gap >= 0.0 && gap < 1000.0)
                    gaps.push_back (gap);
                if (arrival->event == (int) Quantizer::Event::FreshLatch || gap > 60.0)
                    list << juce::String::formatted ("\n        %.2f s %s -> %s: %s, %.0f ms after the last real hop%s%s", (double) pk[k].t / kFs,
                                                     lastRealNote >= 0 ? sputter::noteName (lastRealNote).toRawUTF8() : "-", sputter::noteName (arrival->noteN).toRawUTF8(),
                                                     eventName (arrival->event), gap, mutedBetween ? ", muted between" : "", arrival->voiceOpen ? "" : ", voice closed");
            }
            std::sort (gaps.begin(), gaps.end());
            auto pct = [&] (double q) { return gaps.empty() ? -1.0 : gaps[(size_t) std::min ((double) gaps.size() - 1, q * (double) gaps.size())]; };
            info (juce::String::formatted ("  %s, %s: %d picks: fresh %d, Always latch %d, arrival / change %d, snap %d, no new note %d; gap from the last real hop to the pick's note: median %.0f ms, 90%% %.0f, largest under 1 s %.0f",
                                           relName, t.name.toRawUTF8(), (int) pk.size() - 1, counts[1], counts[2], counts[3] + counts[4], counts[5], counts[-1], pct (0.5), pct (0.9),
                                           gaps.empty() ? -1.0 : gaps.back())
                  + list);
        }
}

// v0.5.11's pick glide: every pick into a ringing note glides in from the note before it at Retune Medium (synthetic picks: rung,
// damped; and blips-dry's repicks), at every Note Release; a pick after a stop longer than kPickGlideWindowMs starts fresh at
// Fast (at Medium and Long the voice is still open: it glides, as before); blips-dry stays at 0 blips; the pick glide changes only
// the pitch (the gate, the level and the reference are the same sample for sample).
void testPickGlide()
{
    using namespace pickglide;
    std::vector<Phrase> into, stops;
    for (double noteMs : { 300.0, 600.0 })
    {
        into.push_back (phrase (Join::Ring, noteMs, 0, 0, 0));
        for (double d : { 15.0, 30.0, 45.0 })
            for (float db : { 15.0f, 25.0f, 35.0f })
                into.push_back (phrase (Join::Damp, noteMs, d, db, 0));
        for (double gap : { 30.0, 60.0, 150.0, 250.0 })
            stops.push_back (phrase (Join::Stop, noteMs, 0, 0, gap));
    }
    for (auto [relName, rel] : { std::pair { "Fast", NoteRelease::Fast }, { "Medium", NoteRelease::Medium }, { "Long", NoteRelease::Long } })
    {
        const auto p = sineAt (noteReleaseMsFor (rel));
        int glided = 0, from = 0, total = 0, offGlided = 0;
        juce::String bad;
        for (const auto& ph : into)
        {
            Engine e, off;
            e.prepare (kFs, 64);
            off.prepare (kFs, 64);
            off.setPickGlide (false);
            std::vector<HopInfo> hops;
            e.hopListener = [&hops] (const HopInfo& h) { hops.push_back (h); };
            const auto out = runEngine (e, ph.x, p), outOff = runEngine (off, ph.x, p);
            const double alpha = 1.0 - std::exp (-(double) e.hopSize() / kFs / (retuneMsFor (p.retuneSpeed) * 0.001));
            const auto cyc = soundingCycles (out, e.currentLatencySamples());
            offGlided += score (ph, outOff, off.currentLatencySamples()).glided;
            for (size_t k = 1; k < ph.picks.size(); ++k)
            {
                const int64_t next = k + 1 < ph.picks.size() ? ph.picks[k + 1] : (int64_t) ph.x.size();
                const auto g = glideAt (cyc, ph.picks[k], next, ph.notes[k - 1], ph.notes[k]);
                // Where the glide started: the arrival hop's T before its one-pole step (T = start + (N - start) alpha), for a glide in
                // (an Always latch, a pick glide, an arrival or change while the voice was connected).
                double start = -1000.0;
                int event = -1;
                for (size_t q = 0; q < hops.size(); ++q)
                {
                    const auto& h = hops[q];
                    if (h.time < ph.picks[k] - (int64_t) (0.005 * kFs) || h.time >= next || h.event == (int) Quantizer::Event::None || h.noteN != ph.notes[k])
                        continue;
                    event = h.event;
                    start = h.event == (int) Quantizer::Event::Arrival || h.event == (int) Quantizer::Event::Change ? (q > 0 ? hops[q - 1].target : h.target)
                                                                                                                    : (h.target - h.noteN * alpha) / (1.0 - alpha);
                    break;
                }
                // ... against where the voice was on the previous note: its T at the last voiced hop on that note before the pick (a
                // note still gliding in when the next pick comes glides on from where it got to).
                double voiceWas = ph.notes[k - 1];
                for (const auto& h : hops)
                    if (h.time < ph.picks[k] && h.detector.voiced && h.noteN == ph.notes[k - 1])
                        voiceWas = h.target;
                    else if (h.time >= ph.picks[k])
                        break;
                const bool fromPrev = event != (int) Quantizer::Event::FreshLatch && event != (int) Quantizer::Event::Snap && std::abs (start - voiceWas) <= 0.5;
                ++total;
                glided += g.glided() ? 1 : 0;
                from += g.glided() && fromPrev ? 1 : 0;
                if (! g.glided() || ! fromPrev)
                    bad << juce::String::formatted ("\n      %s, %.3f s: %s, between %.1f ms, the glide started %+.2f st off the voice on the old note (%+.2f st off that note; first sounding cycle %+.1f st off the new one)",
                                                    ph.name.toRawUTF8(), (double) ph.picks[k] / kFs, eventName (event), g.betweenMs, start - voiceWas, start - ph.notes[k - 1], g.startSt);
            }
        }
        check (glided == total && from == total, juce::String::formatted ("Note Release %s: picks into a ringing note (rung, damped 15 / 30 / 45 ms by 15 / 25 / 35 dB): %d of %d glide in (audibly: 8 ms or more between the two notes), %d from the voice's pitch on the previous note (within 50 c; without the pick glide %d glide in)",
                                                                          relName, glided, total, from, offGlided) + bad);
        int stopGlided = 0, stopTotal = 0;
        juce::String stopText;
        for (const auto& ph : stops)
        {
            Engine e;
            e.prepare (kFs, 64);
            const auto sc = score (ph, runEngine (e, ph.x, p), e.currentLatencySamples());
            stopText << (stopText.isEmpty() ? "" : "; ") << ph.name << juce::String::formatted (" %d/%d", sc.glided, sc.picks);
            const bool beyond = ph.name.contains ("150 ms") || ph.name.contains ("250 ms");
            if (rel == NoteRelease::Fast && beyond)
            {
                stopGlided += sc.glided;
                stopTotal += sc.picks;
            }
        }
        if (rel == NoteRelease::Fast)
            check (stopGlided == 0, juce::String::formatted ("Note Release Fast: a pick after a stop longer than the window (%.0f ms) starts fresh: %d of %d glide; ", kPickGlideWindowMs, stopGlided, stopTotal) + stopText);
        else
            info (juce::String ("Note Release ") + relName + ": after a stop (the voice still open, Glide Always as before): " + stopText);
    }

    // blips-dry: no blip with the pick glide (it glides from the real F#4, never the damp's junk), at the defaults and at Fast.
    {
        const auto& dry = blips::dryTake().x;
        const auto ps = blips::takePicks (dry);
        for (auto [relName, rel] : { std::pair { "Medium (the default)", NoteRelease::Medium }, { "Fast", NoteRelease::Fast } })
        {
            const auto r = blips::run (dry, sineAt (noteReleaseMsFor (rel)), {}, true);
            const auto cyc = soundingCycles (r.out, r.latency);
            const auto sc = blips::score (r.out, ps, r.latency, &cyc);
            {
                const auto r0 = blips::run (dry, sineAt (noteReleaseMsFor (rel)), [] (Engine& e) { e.setPickGlide (false); }, true);
                const auto cyc0 = soundingCycles (r0.out, r0.latency);
                info (juce::String ("blips-dry, Note Release ") + relName + ", without the pick glide (v0.5.10): " + blips::scoreText (blips::score (r0.out, ps, r0.latency, &cyc0))
                      + " (the blips test's measure, which also counts the cycle across a closed voice's silent gap: " + blips::scoreText (blips::score (r0.out, ps, r0.latency)) + ")");
            }
            Engine e;
            e.prepare (kFs, 64);
            runEngine (e, dry, sineAt (noteReleaseMsFor (rel)));
            check (sc.blips == 0, juce::String ("blips-dry, Note Release ") + relName + ", the pick glide on: " + blips::scoreText (sc) + juce::String::formatted ("; %d picks glided in by it", e.pickGlides()));
        }
    }

    // Only the pitch: on every take, at every Note Release, the gate, the envelope, the reference and the level are identical with
    // the pick glide on and off.
    for (auto [relName, rel] : { std::pair { "Fast", NoteRelease::Fast }, { "Medium", NoteRelease::Medium }, { "Long", NoteRelease::Long } })
    {
        int levelDiff = 0, glides = 0, n = 0;
        juce::String moved;
        for (const auto& t : blips::takes())
        {
            if (t.name == "blips")
                continue;
            ++n;
            EngineParams p;
            p.holdMs = noteReleaseMsFor (rel);
            std::vector<VoiceProbe> a, b;
            Engine on, off;
            on.prepare (kFs, 64);
            off.prepare (kFs, 64);
            off.setPickGlide (false);
            a.reserve (t.x.size());
            b.reserve (t.x.size());
            on.voiceListener = [&a] (const VoiceProbe& v) { a.push_back (v); };
            off.voiceListener = [&b] (const VoiceProbe& v) { b.push_back (v); };
            const auto ya = runEngine (on, t.x, p), yb = runEngine (off, t.x, p);
            int d = 0;
            for (size_t k = 0; k < a.size() && k < b.size(); ++k)
                d += (a[k].open != b[k].open || a[k].gate != b[k].gate || a[k].env != b[k].env || a[k].ref != b[k].ref || a[k].level != b[k].level) ? 1 : 0;
            levelDiff += d;
            const int od = compareSamples (ya, yb).values;
            glides += on.pickGlides();
            if (od > 0)
                moved << juce::String::formatted ("%s%s (%d glided in)", moved.isEmpty() ? "" : ", ", t.name.toRawUTF8(), on.pickGlides());
        }
        const juce::String text = juce::String::formatted ("Note Release %s, the %d takes at the defaults: the gate, envelope, reference and level identical with and without the pick glide (%d samples differ); %d picks glided in",
                                                           relName, n, levelDiff, glides)
                                  + (moved.isNotEmpty() ? "; the pitch moved on " + moved : juce::String());
        check (levelDiff == 0, text);
    }
}
