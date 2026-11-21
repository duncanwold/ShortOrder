// The blips investigation (branch investigate-blips, docs/reports/BLIPS-REPORT.md): what happens at a pick. Included by OfflineTests.cpp
// (it uses its helpers). Probes only: nothing here is a pass / fail invariant.

namespace blips
{
// Every take in Recordings/ (WAV and AIFF), mono: the channels' mean. For a multichannel file, a line on how its channels
// compare (correlation, levels), so we know both carry the guitar.
struct Take
{
    juce::String name;
    std::vector<float> x;
};
const std::vector<Take>& takes()
{
    static const std::vector<Take> all = [] {
        std::vector<Take> out;
        auto dir = juce::File::getCurrentWorkingDirectory().getChildFile ("Recordings");
        if (! dir.isDirectory())
            dir = outDir.getParentDirectory().getParentDirectory().getChildFile ("Recordings");
        auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav;*.aif;*.aiff");
        files.sort();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        for (auto& f : files)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
            if (reader == nullptr || std::abs (reader->sampleRate - kFs) > 1.0)
                continue;
            juce::AudioBuffer<float> b ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&b, 0, (int) reader->lengthInSamples, 0, true, true);
            std::vector<float> x ((size_t) b.getNumSamples());
            for (int c = 0; c < b.getNumChannels(); ++c)
                for (int i = 0; i < b.getNumSamples(); ++i)
                    x[(size_t) i] += b.getSample (c, i) / (float) b.getNumChannels();
            if (b.getNumChannels() == 2)
            {
                double ll = 0.0, rr = 0.0, lr = 0.0, diff = 0.0;
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const double l = b.getSample (0, i), r = b.getSample (1, i);
                    ll += l * l;
                    rr += r * r;
                    lr += l * r;
                    diff = std::max (diff, std::abs (l - r));
                }
                const double n = std::max (1, b.getNumSamples());
                info (juce::String::formatted ("%s: 2 channels, L %.1f dB RMS, R %.1f dB RMS, correlation %.6f, largest L - R %.2e: the mean of the two is used",
                                               f.getFileName().toRawUTF8(), toDb (std::sqrt (ll / n)), toDb (std::sqrt (rr / n)), lr / std::sqrt (ll * rr + 1e-30), diff));
            }
            out.push_back ({ f.getFileNameWithoutExtension().replace (" ", "_"), std::move (x) });
        }
        return out;
    }();
    return all;
}

// One run of the current engine with every hop and every sample's probe.
struct Run
{
    std::vector<float> out;
    std::vector<HopInfo> hops;
    std::vector<VoiceProbe> probes; // per sample, voice time = index
    std::vector<int64_t> onsets;    // voice (= input) time
    int latency = 0;
};
// The probes measure against v0.5.9: the blip fixes (and v0.5.11's pick glide) off unless the setup sets them (or engineDefaults:
// the engine as it starts).
Run run (const std::vector<float>& in, const EngineParams& p, const std::function<void (Engine&)>& setup = {}, bool engineDefaults = false)
{
    Run r;
    Engine e;
    e.prepare (kFs, 64);
    if (! engineDefaults)
    {
        e.setPickSwitches (PickSwitches {});
        e.setPickGlide (false); // v0.5.11's pick glide: v0.5.9 had none
        e.setV062Fixes (false); // nor v0.6.2's fixes
    }
    if (setup)
        setup (e);
    r.probes.reserve (in.size());
    e.hopListener = [&r] (const HopInfo& h) { r.hops.push_back (h); };
    e.voiceListener = [&r] (const VoiceProbe& v) {
        r.probes.push_back (v);
        if (v.onset)
            r.onsets.push_back (v.t);
    };
    r.out = runEngine (e, in, p);
    r.latency = e.currentLatencySamples();
    return r;
}

// The output's pitch, cycle by cycle (a sine voice: upward zero crossings, interpolated), in voice time (output index - D, the voice's delay).
struct Cycle
{
    double t = 0.0; // the cycle's middle, samples
    double midi = 0.0;
    float amp = 0.0f;
};
std::vector<Cycle> cycles (const std::vector<float>& y, int latency)
{
    std::vector<Cycle> c;
    double last = -1.0;
    float amp = 0.0f;
    for (size_t n = 1; n < y.size(); ++n)
    {
        amp = std::max (amp, std::abs (y[n]));
        if (y[n - 1] < 0.0f && y[n] >= 0.0f)
        {
            const double cross = (double) (n - 1) + y[n - 1] / (double) (y[n - 1] - y[n]);
            if (last >= 0.0 && amp > 1.0e-4f)
            {
                const double period = cross - last;
                if (period > kFs / 2000.0 && period < kFs / 50.0)
                    c.push_back ({ 0.5 * (cross + last) - latency, midiFromHz (kFs / period), amp });
            }
            last = cross;
            amp = 0.0f;
        }
    }
    return c;
}

// Picks found in the input itself (analysis only; the engine's onset detector needs a 3 dB rise in level, which a compressed
// or hot take's picks don't make): the pick's click, as a jump in the energy of the second difference (above ~5 kHz the string
// has little energy, the pick a lot), 1 ms frames, 9 dB over the mean of the previous 20 ms and over an absolute floor; 40 ms
// apart at least. Voice time = input time.
std::vector<int64_t> inputPicks (const std::vector<float>& x)
{
    const size_t frame = (size_t) (0.001 * kFs);
    std::vector<double> e;
    for (size_t s = 2; s + frame <= x.size(); s += frame)
    {
        double sum = 0.0;
        for (size_t i = s; i < s + frame; ++i)
        {
            const double d = (double) x[i] - 2.0 * x[i - 1] + x[i - 2];
            sum += d * d;
        }
        e.push_back (sum / (double) frame);
    }
    std::vector<int64_t> out;
    const size_t back = 20;
    for (size_t k = back; k < e.size(); ++k)
    {
        double mean = 0.0;
        for (size_t j = k - back; j < k; ++j)
            mean += e[j];
        mean /= (double) back;
        const bool jump = e[k] > 8.0 * mean && e[k] > 1.0e-7; // 9 dB; floor: the 2nd difference at about -70 dB
        if (jump && (out.empty() || (int64_t) (k * frame) - out.back() > (int64_t) (0.04 * kFs)))
            out.push_back ((int64_t) (2 + k * frame));
    }
    return out;
}

// A pick: an onset of the shared detector (voice time), its note (the input's own pitch 80 .. 250 ms later, by the measuring
// detector: the median of its voiced hops, rounded), and where the next one starts.
struct Pick
{
    int64_t t = 0;
    int64_t end = 0;     // the next pick (or the take's end)
    double truth = 0.0;  // the input's pitch (midi, unrounded median)
    int note = -1;       // rounded; -1: unpitched
};
std::vector<Pick> picks (const std::vector<float>& in, const std::vector<int64_t>& onsets)
{
    const auto measured = measureHops (in, "blips/measure-tmp");
    std::vector<Pick> out;
    for (size_t k = 0; k < onsets.size(); ++k)
    {
        if (! out.empty() && onsets[k] - out.back().t < (int64_t) (0.03 * kFs))
            continue; // the same pluck
        Pick p;
        p.t = onsets[k];
        out.push_back (p);
    }
    for (size_t k = 0; k < out.size(); ++k)
    {
        auto& p = out[k];
        p.end = k + 1 < out.size() ? out[k + 1].t : (int64_t) in.size();
        std::vector<double> m;
        for (const auto& h : measured)
            if (h.detector.voiced && h.time >= p.t + (int64_t) (0.08 * kFs) && h.time <= std::min (p.t + (int64_t) (0.25 * kFs), p.end - (int64_t) (0.02 * kFs)))
                m.push_back (h.midi);
        if (m.size() >= 3)
        {
            std::sort (m.begin(), m.end());
            p.truth = m[m.size() / 2];
            p.note = (int) std::lround (p.truth);
        }
    }
    return out;
}

// How the output's pitch behaves after a pick, against the note played (cents re. the rounded note; the first 200 ms, or up
// to the next pick).
struct Blip
{
    bool sounded = false;
    double firstMs = 0.0;   // the first output cycle, after the pick
    double startC = 0.0;    // the first cycle's deviation
    double maxC = 0.0;      // the largest deviation, signed
    double over50Ms = 0.0;  // how long more than 50 c off
    double over25Ms = 0.0;
    double over100Ms = 0.0;
    double settleMs = 0.0;  // from the first cycle until it's within 25 c for good (in the span)
    bool blip() const { return sounded && over50Ms >= 5.0; }
};
Blip measure (const std::vector<Cycle>& c, const Pick& p, double spanMs = 200.0)
{
    Blip b;
    if (p.note < 0)
        return b;
    const double from = (double) p.t, to = std::min ((double) p.t + spanMs * 0.001 * kFs, (double) p.end - 0.005 * kFs);
    double lastOff = -1.0, prevT = -1.0;
    for (const auto& x : c)
    {
        if (x.t < from || x.t > to)
            continue;
        const double dev = 100.0 * (x.midi - p.note);
        const double dt = prevT < 0.0 ? 0.0 : std::min (x.t - prevT, 0.01 * kFs) / kFs * 1000.0;
        prevT = x.t;
        if (! b.sounded)
        {
            b.sounded = true;
            b.firstMs = (x.t - from) / kFs * 1000.0;
            b.startC = dev;
        }
        if (std::abs (dev) > std::abs (b.maxC))
            b.maxC = dev;
        if (std::abs (dev) > 50.0)
            b.over50Ms += dt;
        if (std::abs (dev) > 25.0)
        {
            b.over25Ms += dt;
            lastOff = x.t;
        }
        if (std::abs (dev) > 100.0)
            b.over100Ms += dt;
    }
    if (b.sounded)
        b.settleMs = lastOff < 0.0 ? 0.0 : (lastOff - from) / kFs * 1000.0 - b.firstMs;
    return b;
}

const char* eventName (int e)
{
    static const char* const names[] = { "", "FRESH", "ALWAYS", "ARRIVE", "change", "SNAP" };
    return names[std::clamp (e, 0, 5)];
}

// The per-hop timeline around every pick (-30 .. +200 ms) of a take at the defaults (Sine, so the output's pitch is read
// cycle by cycle as well), to build/test-output/blips/<take>-timeline.txt; the summary per pick to the console.
void timeline (const Take& take, const EngineParams& p, const juce::String& tag, const std::function<void (Engine&)>& setup = {})
{
    auto params = p;
    params.wave = Wave::Sine;
    const auto r = run (take.x, params, setup);
    const auto c = cycles (r.out, r.latency);
    // The picks: the input's own (inputPicks), plus the engine's onsets that aren't within 30 ms of one (and the gap noise's
    // onsets fall out: picks() leaves them unpitched, and they're skipped below).
    auto marks = inputPicks (take.x);
    for (auto o : r.onsets)
    {
        bool near = false;
        for (auto m : marks)
            near = near || std::abs (m - o) < (int64_t) (0.03 * kFs);
        if (! near)
            marks.push_back (o);
    }
    std::sort (marks.begin(), marks.end());
    const auto ps = picks (take.x, marks);
    auto file = outDir.getChildFile ("blips").getChildFile (take.name + "-" + tag + "-timeline.txt");
    file.deleteFile();
    auto os = file.createOutputStream();
    *os << "Timeline around every pick: " << take.name << " (" << tag << "). Times re. the pick (voice time = input time; the output plays it L = "
        << r.latency << " samples later).\n"
        << "Hop columns: ms, raw detector (note+cents, conf, voiced, rms dB, guard), midi after the lock, N, T (the output pitch), quantizer event,\n"
        << "flags (ONSET open LOCK MUTE REVIVE EDGE), then at that sample: gate, level (dB), reference (dB), Dynamics gain (dB), graced, capped.\n\n";
    auto noteText = [] (double midi) {
        if (midi <= 0.0)
            return juce::String ("---");
        const int n = (int) std::lround (midi);
        return juce::MidiMessage::getMidiNoteName (n, true, true, 4) + juce::String::formatted ("%+.0f", 100.0 * (midi - n));
    };
    for (size_t k = 0; k < ps.size(); ++k)
    {
        const auto& pk = ps[k];
        if (pk.note < 0)
            continue; // noise in a gap: nothing played
        const auto b = measure (c, pk);
        *os << juce::String::formatted ("Pick %zu at %.3f s (output %.3f s): note %s (input %.2f)", k + 1, pk.t / kFs, (pk.t + r.latency) / kFs,
                                        pk.note >= 0 ? juce::MidiMessage::getMidiNoteName (pk.note, true, true, 4).toRawUTF8() : "unpitched", pk.truth);
        if (b.sounded)
            *os << juce::String::formatted ("; output: first cycle %.1f ms, starts %+.0f c, largest %+.0f c, >50 c %.1f ms, >25 c %.1f ms, settles %.1f ms%s",
                                            b.firstMs, b.startC, b.maxC, b.over50Ms, b.over25Ms, b.settleMs, b.blip() ? "  BLIP" : "");
        *os << "\n";
        for (const auto& h : r.hops)
        {
            const double ms = (double) (h.time - pk.t) / kFs * 1000.0;
            if (ms < -30.0 || ms > 200.0 || h.time >= pk.end + (int64_t) (0.03 * kFs))
                continue;
            const auto& v = r.probes[(size_t) std::clamp<int64_t> (h.time, 0, (int64_t) r.probes.size() - 1)];
            *os << juce::String::formatted ("  %+6.1f  %-8s %.2f %s %5.1f %s | %-8s N %-4s T %-8s %-6s %s%s%s%s%s%s | gate %.2f lvl %6.1f ref %6.1f dyn %+5.1f%s%s\n", ms,
                                            noteText (h.detector.f0 > 0.0f ? midiFromHz (h.detector.f0) : 0.0).toRawUTF8(), h.detector.confidence, h.detector.voiced ? "V" : "-",
                                            toDb (h.detector.rms), h.detector.guardRejected ? "GUARD" : "     ", noteText (h.midi).toRawUTF8(),
                                            juce::MidiMessage::getMidiNoteName (h.noteN, true, true, 4).toRawUTF8(), noteText (h.target).toRawUTF8(), eventName (h.event),
                                            h.onset ? "ONSET " : "", h.voiceOpen ? "open " : "", h.lockHeld ? "LOCK " : "", h.muted ? "MUTE " : "", h.revivalBlocked ? "REVIVE " : "",
                                            h.edgeLag ? "EDGE " : "", v.gate, toDb (v.level), toDb (v.ref), toDb (v.dynamicsGain), v.graced ? " graced" : "", v.capped ? " capped" : "");
        }
        *os << "\n";
        info (juce::String::formatted ("%s pick %2zu at %6.3f s: %-4s", take.name.substring (0, 24).toRawUTF8(), k + 1, pk.t / kFs,
                                       pk.note >= 0 ? juce::MidiMessage::getMidiNoteName (pk.note, true, true, 4).toRawUTF8() : "--")
              + (b.sounded ? juce::String::formatted ("first %5.1f ms, starts %+5.0f c, largest %+5.0f c, >50 c %5.1f ms, settles %5.1f ms%s", b.firstMs, b.startC, b.maxC, b.over50Ms,
                                                      b.settleMs, b.blip() ? "  BLIP" : "")
                           : juce::String ("silent")));
    }
}
} // namespace blips

void probeBlipsTimeline()
{
    outDir.getChildFile ("blips").createDirectory();
    for (const auto& t : blips::takes())
        blips::timeline (t, EngineParams {}, "defaults");
    info ("timelines in " + outDir.getChildFile ("blips").getFullPathName());
}

// The take itself, every 10 ms: the input's level (peak and RMS over 10 ms) and its pitch (the engine's detector at the
// default window, raw, with confidence), to see what was played.
void probeBlipsProfile()
{
    for (const auto& t : blips::takes())
    {
        if (t.name != "blips-dry")
            continue;
        Engine e;
        e.prepare (kFs, 64);
        std::vector<HopInfo> hops;
        e.hopListener = [&hops] (const HopInfo& h) { hops.push_back (h); };
        runEngine (e, t.x, EngineParams {});
        const size_t w = (size_t) (0.01 * kFs);
        size_t hk = 0;
        for (size_t s = 0; s + w <= t.x.size(); s += w)
        {
            float pk = 0.0f;
            double sq = 0.0;
            for (size_t i = s; i < s + w; ++i)
            {
                pk = std::max (pk, std::abs (t.x[i]));
                sq += (double) t.x[i] * t.x[i];
            }
            while (hk + 1 < hops.size() && hops[hk + 1].time <= (int64_t) (s + w / 2))
                ++hk;
            const auto& h = hops[hk];
            const double m = h.detector.f0 > 0.0f ? midiFromHz (h.detector.f0) : 0.0;
            info (juce::String::formatted ("%6.3f s  peak %6.1f dB  rms %6.1f dB   %-5s %+4.0f c  conf %.2f %s", (double) s / kFs, toDb (pk), toDb (std::sqrt (sq / (double) w)),
                                           m > 0 ? juce::MidiMessage::getMidiNoteName ((int) std::lround (m), true, true, 4).toRawUTF8() : "--", m > 0 ? 100.0 * (m - std::lround (m)) : 0.0,
                                           h.detector.confidence, h.detector.voiced ? "V" : "-"));
        }
    }
}

// What the input holds around a pick: 512-sample frames (Hann, 10.7 ms) every 2.7 ms, the strongest spectral peaks (Hz, dB).
void spectrumAround (const std::vector<float>& x, double from, double to)
{
    constexpr int order = 11, n = 1 << order;
    juce::dsp::FFT fft (order);
    for (double t = from; t < to; t += 128.0 / kFs)
    {
        std::vector<float> buf (2 * n, 0.0f);
        const size_t s = (size_t) (t * kFs);
        for (int k = 0; k < 512 && s + (size_t) k < x.size(); ++k)
            buf[(size_t) k] = x[s + (size_t) k] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * k / 511.0));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        std::vector<std::pair<float, int>> peaks;
        for (int k = 2; k < n / 2 - 1; ++k)
            if (buf[(size_t) k] > buf[(size_t) k - 1] && buf[(size_t) k] >= buf[(size_t) k + 1])
                peaks.push_back ({ buf[(size_t) k], k });
        std::sort (peaks.rbegin(), peaks.rend());
        juce::String line;
        for (size_t j = 0; j < std::min<size_t> (6, peaks.size()); ++j)
            line << juce::String::formatted ("%5.0f Hz %5.1f dB   ", peaks[j].second * kFs / n, toDb (peaks[j].first / 128.0));
        info (juce::String::formatted ("%.4f s (frame centre %.4f): ", t, t + 256.0 / kFs) + line);
    }
}
void probeBlipsSpectrum()
{
    for (const auto& t : blips::takes())
        if (t.name == "blips-dry")
            for (double at : { 0.652, 1.369, 3.062 })
            {
                info (juce::String::formatted ("-- around the pick at %.3f s", at));
                spectrumAround (t.x, at - 0.02, at + 0.05);
            }
}

// blips.aif read as an output (it looks like one: L = R exactly, a near-pure sine, F#4 to the cent): its pitch cycle by cycle,
// every excursion more than 25 c from F#4 with its shape (time, the notes it passes, how long).
void probeBlipsRecordedShape()
{
    for (const auto& t : blips::takes())
    {
        if (t.name != "blips-dry")
            continue;
        const auto c = blips::cycles (t.x, 0);
        size_t k = 0;
        int count = 0;
        double total = 0.0;
        while (k < c.size())
        {
            if (std::abs (c[k].midi - 66.0) <= 0.25)
            {
                ++k;
                continue;
            }
            size_t j = k;
            while (j < c.size() && std::abs (c[j].midi - 66.0) > 0.25)
                ++j;
            const double from = c[k].t / kFs, to = (j < c.size() ? c[j].t : c.back().t) / kFs;
            juce::String path;
            int lastNote = -1000;
            double maxDev = 0.0;
            for (size_t i = k; i < j; ++i)
            {
                const int n = (int) std::lround (c[i].midi);
                maxDev = std::abs (c[i].midi - 66.0) > std::abs (maxDev) ? c[i].midi - 66.0 : maxDev;
                if (n != lastNote)
                {
                    path << juce::MidiMessage::getMidiNoteName (n, true, true, 4) << juce::String::formatted ("%+.0f", 100.0 * (c[i].midi - n)) << " ";
                    lastNote = n;
                }
            }
            ++count;
            total += to - from;
            info (juce::String::formatted ("%.3f .. %.3f s (%5.1f ms, %3zu cycles), largest %+6.2f st: ", from, to, 1000.0 * (to - from), j - k, maxDev) + path.trimEnd().substring (0, 160));
            k = j;
        }
        info (juce::String::formatted ("%d excursions from F#4, %.0f ms in all", count, 1000.0 * total));
    }
}
void probeBlipsRecordedCycles()
{
    for (const auto& t : blips::takes())
        if (t.name == "blips-dry")
        {
            const auto c = blips::cycles (t.x, 0);
            for (double at : { 1.610, 0.645, 3.505 })
            {
                juce::String line;
                for (const auto& x : c)
                    if (x.t >= at * kFs && x.t < (at + 0.075) * kFs)
                        line << juce::String::formatted ("%.1f:%+.2f ", (x.t / kFs - at) * 1000.0, x.midi - 66.0);
                info (juce::String::formatted ("from %.3f s (ms: semitones re. F#4): ", at) + line);
            }
        }
}

namespace blips
{
// Scoring a whole output against the picks: for each pick, the audible output (cycles no more than 30 dB under the note's own
// level, its loudest cycle 30 .. 150 ms after the pick) from 60 ms before it (the damp: re. the previous note) to 200 ms after
// (re. this note), clipped to the neighbours. A blip: 5 ms or more of audible output over 50 c off.
struct PickScore
{
    double preMs = 0.0, postMs = 0.0;   // audible > 50 c, before / after the pick
    double preC = 0.0, postC = 0.0;     // the largest deviation (signed), audible
    double startC = 0.0;                // the first audible cycle after the pick
    double firstMs = -1.0;              // when it sounds (ms after the pick; < 0: never in the span)
    bool blip() const { return preMs + postMs >= 5.0; }
};
struct Score
{
    std::vector<PickScore> picks;
    int blips = 0, pre = 0, post = 0, scored = 0;
    double ms = 0.0, worstC = 0.0, meanFirstMs = 0.0;
};
Score score (const std::vector<float>& out, const std::vector<Pick>& ps, int latency, const std::vector<Cycle>* given = nullptr)
{
    const auto c = given != nullptr ? *given : cycles (out, latency);
    Score s;
    for (size_t k = 0; k < ps.size(); ++k)
    {
        PickScore r;
        const auto& p = ps[k];
        if (p.note < 0)
        {
            s.picks.push_back (r);
            continue;
        }
        const int prevNote = k > 0 && ps[k - 1].note >= 0 ? ps[k - 1].note : p.note;
        const double from = std::max ((double) p.t - 0.06 * kFs, k > 0 ? (double) ps[k - 1].t + 0.06 * kFs : 0.0);
        const double to = std::min ((double) p.t + 0.2 * kFs, (double) p.end - 0.005 * kFs);
        float loud = 0.0f;
        for (const auto& x : c)
            if (x.t >= p.t + 0.03 * kFs && x.t <= p.t + 0.15 * kFs)
                loud = std::max (loud, x.amp);
        const float floor = loud * (float) dbToGain (-30.0f);
        double prevT = -1.0;
        int firstSeen = 0;
        double sumFirst = 0.0;
        for (const auto& x : c)
        {
            if (x.t < from || x.t > to)
                continue;
            const double dt = prevT < 0.0 ? 0.0 : std::min (x.t - prevT, 0.01 * kFs) / kFs * 1000.0;
            prevT = x.t;
            if (x.amp < floor || loud <= 0.0f)
                continue;
            const bool after = x.t >= (double) p.t;
            const double dev = 100.0 * (x.midi - (after ? p.note : prevNote));
            if (after && r.firstMs < 0.0)
            {
                r.firstMs = (x.t - p.t) / kFs * 1000.0;
                r.startC = dev;
            }
            if (std::abs (dev) > 50.0)
                (after ? r.postMs : r.preMs) += dt;
            auto& m = after ? r.postC : r.preC;
            if (std::abs (dev) > std::abs (m))
                m = dev;
        }
        juce::ignoreUnused (firstSeen, sumFirst);
        ++s.scored;
        if (r.blip())
            ++s.blips;
        s.pre += r.preMs >= 5.0 ? 1 : 0;
        s.post += r.postMs >= 5.0 ? 1 : 0;
        s.ms += r.preMs + r.postMs;
        s.worstC = std::max ({ s.worstC, std::abs (r.preC), std::abs (r.postC) });
        s.meanFirstMs += std::max (0.0, r.firstMs);
        s.picks.push_back (r);
    }
    if (s.scored > 0)
        s.meanFirstMs /= s.scored;
    return s;
}
juce::String scoreText (const Score& s)
{
    return juce::String::formatted ("%2d / %2d picks blip (%2d in the damp before, %2d after), %6.0f ms audibly > 50 c off, worst %+5.0f c, first sound %.1f ms",
                                    s.blips, s.scored, s.pre, s.post, s.ms, s.worstC, s.meanFirstMs);
}

// The picks of a take (the input's own clicks, plus the engine's onsets away from them), with their notes.
std::vector<Pick> takePicks (const std::vector<float>& x)
{
    const auto r = run (x, EngineParams {});
    auto marks = inputPicks (x);
    for (auto o : r.onsets)
    {
        bool near = false;
        for (auto m : marks)
            near = near || std::abs (m - o) < (int64_t) (0.03 * kFs);
        if (! near)
            marks.push_back (o);
    }
    std::sort (marks.begin(), marks.end());
    return picks (x, marks);
}
const Take& dryTake()
{
    for (const auto& t : takes())
        if (t.name == "blips-dry")
            return t;
    jassertfalse;
    return takes().front();
}
} // namespace blips

namespace blips
{
// What a mitigation costs elsewhere: how long the output's pitch is more than 50 c from the input's own (the measuring
// detector at a 150 Hz window, voiced at 0.9 confidence or more), on the four slide / glide takes and the regression phrase
// (slides, glides, plucks). The output lags a slide by Retune Speed anyway; the cost is the change against no mitigation.
struct Variant
{
    juce::String name;
    std::function<void (Engine&)> setup;
    std::function<void (EngineParams&)> params;
};
double trackingMs (const std::vector<float>& in, const Variant& v)
{
    static std::map<const float*, std::vector<HopInfo>> cache;
    auto& m = cache[in.data()];
    if (m.empty())
        m = measureHops (in, "blips/measure-150", 150.0);
    EngineParams p;
    p.wave = Wave::Sine;
    if (v.params)
        v.params (p);
    const auto r = run (in, p, v.setup);
    const auto c = cycles (r.out, r.latency);
    double ms = 0.0, prevT = -1.0;
    size_t h = 0;
    for (const auto& x : c)
    {
        const double dt = prevT < 0.0 ? 0.0 : std::min (x.t - prevT, 0.01 * kFs) / kFs * 1000.0;
        prevT = x.t;
        while (h + 1 < m.size() && m[h + 1].time <= (int64_t) x.t)
            ++h;
        if (h < m.size() && m[h].detector.voiced && m[h].detector.confidence >= 0.9f && std::abs (x.midi - m[h].midi) > 0.5 && x.amp > 1.0e-3f)
            ms += dt;
    }
    return ms;
}
void compare (const std::vector<Variant>& variants)
{
    const auto& dry = dryTake().x;
    const auto ps = takePicks (dry);
    std::vector<const std::vector<float>*> others;
    for (const auto& t : takes())
        if (! t.name.startsWith ("blips"))
            others.push_back (&t.x);
    static const auto phrase = regressionPhrase (kFs);
    others.push_back (&phrase);
    std::vector<double> base (others.size());
    for (size_t k = 0; k < others.size(); ++k)
        base[k] = trackingMs (*others[k], variants.front());
    for (const auto& v : variants)
    {
        EngineParams p;
        p.wave = Wave::Sine;
        if (v.params)
            v.params (p);
        const auto r = run (dry, p, v.setup);
        const auto s = score (r.out, ps, r.latency);
        double pre = 0.0, post = 0.0;
        for (const auto& k : s.picks)
        {
            pre += k.preMs;
            post += k.postMs;
        }
        juce::String cost;
        double extra = 0.0;
        for (size_t k = 0; k < others.size(); ++k)
        {
            const double d = trackingMs (*others[k], v) - base[k];
            extra += d;
            cost << juce::String::formatted ("%+.0f ", d);
        }
        info (juce::String::formatted ("%-46s %2d / %2d blip (%2d before, %2d after), off %4.0f + %4.0f ms, worst %+5.0f c, first %4.1f ms | cost %+5.0f ms (",
                                       v.name.toRawUTF8(), s.blips, s.scored, s.pre, s.post, pre, post, s.worstC, s.meanFirstMs, extra)
              + cost.trimEnd() + ")");
    }
}
PickSwitches with (const std::function<void (PickSwitches&)>& f)
{
    PickSwitches p;
    f (p);
    return p;
}
Variant pick (const juce::String& name, const std::function<void (PickSwitches&)>& f, const std::function<void (EngineParams&)>& params = {})
{
    const auto sw = with (f);
    return { name, [sw] (Engine& e) { e.setPickSwitches (sw); }, params };
}
} // namespace blips

void probeBlipsMitigations()
{
    using namespace blips;
    info ("blips-dry at the defaults (Retune Medium, Sustain 75%): blips, ms audibly > 50 c off before / after the pick, first sound; cost: the change in ms > 50 c off the input's pitch on the four slide takes and the regression phrase (one figure each)");
    std::vector<Variant> v;
    v.push_back ({ "none (v0.5.9 defaults)", {}, {} });
    v.push_back ({ "Retune Fast", {}, [] (EngineParams& p) { p.retuneSpeed = RetuneSpeed::Fast; } });
    // 1. Snap a pick's arrival.
    for (float semis : { 1.0f, 2.0f, 7.0f })
        for (float ms : { 15.0f, 25.0f, 40.0f })
            v.push_back (pick (juce::String::formatted ("snap arrival >= %.0f st within %.0f ms", semis, ms), [=] (PickSwitches& p) {
                p.rules.snapArrivalSemis = semis;
                p.rules.snapArrivalMs = ms;
            }));
    // 2. No glide until stable.
    for (int n : { 2, 3, 4 })
        v.push_back (pick (juce::String::formatted ("hold until N stable %d hops, then snap", n), [=] (PickSwitches& p) { p.rules.stableHops = n; }));
    // 3. Always: a pick after a gap latches fresh.
    for (float g : { 0.0f, 5.0f, 10.0f, 20.0f })
        v.push_back (pick (juce::String::formatted ("fresh latch on a pick after >= %.0f ms unvoiced", g), [=] (PickSwitches& p) { p.rules.freshAfterGapMs = g; }));
    // 4. Stricter confidence after an onset.
    for (float c : { 0.9f, 0.95f })
        v.push_back (pick (juce::String::formatted ("confidence %.2f for 4 hops from an onset", c), [=] (PickSwitches& p) {
            p.onsetConfidence = c;
            p.onsetHops = 4;
        }));
    // 5. Ignore the first hops.
    for (int n : { 1, 2 })
        v.push_back (pick (juce::String::formatted ("ignore %d hop(s) from an onset", n), [=] (PickSwitches& p) { p.ignoreHopsAfterOnset = n; }));
    // 6. The damp itself: the fall lock, an earlier mute gate, the lock on every jump in the tail.
    for (float db : { 6.0f, 10.0f, 15.0f })
        v.push_back (pick (juce::String::formatted ("fall lock %.0f dB", db), [=] (PickSwitches& p) { p.fallLockDb = db; }));
    for (float db : { 10.0f, 15.0f })
    {
        auto t = sputterFix();
        t.muteDropDb = db;
        v.push_back ({ juce::String::formatted ("mute gate at %.0f dB (not 20)", db), [t] (Engine& e) { e.setTailSwitches (t); }, {} });
    }
    for (float db : { 20.0f, 25.0f, 30.0f })
    {
        auto t = sputterFix();
        t.lockHarmonicsOnly = false;
        t.lockUnderRefDb = db;
        v.push_back ({ juce::String::formatted ("tail lock on every jump, %.0f dB under", db), [t] (Engine& e) { e.setTailSwitches (t); }, {} });
    }
    // Combinations.
    for (float db : { 6.0f, 10.0f })
        v.push_back (pick (juce::String::formatted ("fall lock %.0f dB + fresh latch after a gap", db), [=] (PickSwitches& p) {
            p.fallLockDb = db;
            p.rules.freshAfterGapMs = 0.0f;
        }));
    compare (v);
}

void probeBlipsFallLock()
{
    using namespace blips;
    std::vector<Variant> v;
    v.push_back ({ "none (v0.5.9 defaults)", {}, {} });
    for (float db : { 6.0f, 9.0f, 12.0f })
        v.push_back (pick (juce::String::formatted ("fall lock window RMS %.1f dB / 50 ms alone", db), [=] (PickSwitches& p) {
            p.fallLockDb = db;
            p.fallOnWindowRms = true;
        }));
    for (bool rmsBased : { false, true })
        for (float ms : { 30.0f, 50.0f, 80.0f })
            for (float db : { 3.0f, 4.5f, 6.0f, 9.0f, 12.0f })
                v.push_back (pick (juce::String::formatted ("fall lock %s %.1f dB / %.0f ms + fresh", rmsBased ? "window RMS" : "level ahead", db, ms), [=] (PickSwitches& p) {
                    p.fallLockDb = db;
                    p.fallOnWindowRms = rmsBased;
                    p.fallWindowMs = ms;
                    p.rules.freshAfterGapMs = 0.0f;
                }));
    compare (v);
}

void probeBlipsRemaining()
{
    using namespace blips;
    PickSwitches sw;
    sw.fallLockDb = 6.0f;
    sw.rules.freshAfterGapMs = 0.0f;
    outDir.getChildFile ("blips").createDirectory();
    timeline (dryTake(), EngineParams {}, "fix", [sw] (Engine& e) { e.setPickSwitches (sw); });
}

// Every pick of blips-dry at the defaults, measured (task 4), and classified by what the hops show: the note the damp's junk
// reading reached (N before the pick), how far T had glided toward it when the new note arrived, and the arrival (Glide Always
// latch, or fresh). Then the WAVs to listen to (Saw, the plugin's defaults), with the worst picks' times.
void probeBlipsTable()
{
    using namespace blips;
    const auto& dry = dryTake().x;
    const auto ps = takePicks (dry);
    EngineParams sine;
    sine.wave = Wave::Sine;
    const auto r = run (dry, sine);
    const auto s = score (r.out, ps, r.latency);
    struct Row
    {
        double t = 0.0, worst = 0.0;
        juce::String text;
    };
    std::vector<Row> rows;
    int junkPicks = 0, alwaysFalls = 0;
    for (size_t k = 0; k < ps.size(); ++k)
    {
        const auto& p = ps[k];
        if (p.note < 0)
            continue;
        const auto& sc = s.picks[k];
        // The hops from 60 ms before the pick to its note's arrival.
        int junk = -1, arrival = 0;
        double tAtArrival = 0.0, junkConf = 0.0, junkRmsDb = 0.0, fallDb = 0.0, peakRms = 0.0;
        for (const auto& h : r.hops)
        {
            if (h.time < p.t - (int64_t) (0.06 * kFs) || h.time > p.end)
                continue;
            if (h.time < p.t)
            {
                peakRms = std::max (peakRms, (double) h.detector.rms);
                if (h.detector.voiced && h.noteN != p.note && junk < 0)
                {
                    junk = h.noteN;
                    junkConf = h.detector.confidence;
                    junkRmsDb = toDb (h.detector.rms);
                    fallDb = toDb (h.detector.rms / std::max (peakRms, 1e-9));
                }
            }
            else if (arrival == 0 && h.detector.voiced && h.noteN == p.note)
            {
                arrival = h.event;
                tAtArrival = h.target;
            }
        }
        if (junk >= 0)
            ++junkPicks;
        if (arrival == (int) Quantizer::Event::AlwaysLatch && std::abs (tAtArrival - p.note) > 0.5)
            ++alwaysFalls;
        Row row;
        row.t = p.t / kFs;
        row.worst = std::max (std::abs (sc.preC), std::abs (sc.postC)) * (sc.blip() ? 1.0 : 0.0);
        row.text = juce::String::formatted ("%6.3f s (output %6.3f): ", p.t / kFs, (p.t + r.latency) / kFs)
                   + (junk >= 0 ? juce::String::formatted ("damp read %-4s (%+3d st, conf %.2f, window %5.1f dB, %5.1f dB under the note's last 60 ms)",
                                                           juce::MidiMessage::getMidiNoteName (junk, true, true, 4).toRawUTF8(), junk - p.note, junkConf, junkRmsDb, fallDb)
                                : juce::String ("no junk reading in the damp                                                         "))
                   + juce::String::formatted (" | arrival %-6s from T %+6.2f st | audible: before %+5.0f c for %4.1f ms, after starts %+5.0f c, %4.1f ms over 50 c%s",
                                              eventName (arrival), tAtArrival - p.note, sc.preC, sc.preMs, sc.startC, sc.postMs, sc.blip() ? "  BLIP" : "");
        rows.push_back (row);
    }
    for (const auto& row : rows)
        info (row.text);
    info (juce::String::formatted ("%d of %zu picks: a junk note in the damp; %d arrivals glided in (Glide Always) from more than half a semitone off", junkPicks, rows.size(), alwaysFalls));
    auto worst = rows;
    std::sort (worst.begin(), worst.end(), [] (const Row& a, const Row& b) { return a.worst > b.worst; });
    juce::String w;
    for (size_t k = 0; k < std::min<size_t> (6, worst.size()); ++k)
        w << juce::String::formatted ("%.2f s (%+.0f c)  ", worst[k].t + r.latency / kFs, worst[k].worst);
    info ("the worst, in the output: " + w.trimEnd());

    // The WAVs: the plugin's defaults (Saw, Retune Medium, Sustain 75%).
    outDir.getChildFile ("blips").createDirectory();
    writeWav ("blips/0-dry-input", dry);
    auto render = [&] (const juce::String& name, const PickSwitches& sw, const std::function<void (EngineParams&)>& set = {}) {
        EngineParams p;
        if (set)
            set (p);
        writeWav ("blips/" + name, run (dry, p, [sw] (Engine& e) { e.setPickSwitches (sw); }).out);
    };
    PickSwitches none, fix, fixFresh, levelAhead;
    fix.fallLockDb = 9.0f;
    fix.fallOnWindowRms = true;
    fix.fallPassConfidence = 0.97f;
    fixFresh = fix;
    fixFresh.rules.freshAfterGapMs = 0.0f;
    levelAhead.fallLockDb = 4.5f;
    levelAhead.rules.freshAfterGapMs = 0.0f;
    render ("1-before-v059-defaults", none);
    render ("2-fix-fall-lock-9dB-conf-0.97", fix);
    render ("3-alt-fall-lock-plus-fresh-pick", fixFresh);
    render ("4-alt-level-ahead-4.5dB-plus-fresh-pick", levelAhead);
    render ("5-ref-retune-fast-no-fix", none, [] (EngineParams& p) { p.retuneSpeed = RetuneSpeed::Fast; });
    info ("WAVs in " + outDir.getChildFile ("blips").getFullPathName());
}

// The other takes: their picks at the defaults, without and with the fix.
void probeBlipsOtherTakes()
{
    using namespace blips;
    PickSwitches fix;
    fix.fallLockDb = 9.0f;
    fix.fallOnWindowRms = true;
    fix.fallPassConfidence = 0.97f;
    for (const auto& t : takes())
    {
        if (t.name == "blips")
            continue; // GTT's output, not an input
        const auto ps = takePicks (t.x);
        EngineParams p;
        p.wave = Wave::Sine;
        const auto a = run (t.x, p), b = run (t.x, p, [fix] (Engine& e) { e.setPickSwitches (fix); });
        info (juce::String::formatted ("%-58s without: ", t.name.substring (0, 58).toRawUTF8()) + scoreText (score (a.out, ps, a.latency)));
        info (juce::String::formatted ("%-58s with:    ", "") + scoreText (score (b.out, ps, b.latency)));
    }
}

// The fall lock with a confidence pass (a change during a fall goes through at this confidence or more): blips, the cost, and
// the hammer-on / pull-off cases (synthetic: a pluck, then the string's pitch stepping up or down a whole tone while the level
// steps down 3 .. 24 dB, no onset): how long after the step the voice's note follows it.
std::vector<float> legatoStep (double drop, int step, double fs = kFs)
{
    const double f1 = hzFromMidi (64), f2 = hzFromMidi (64 + step);
    std::vector<float> x ((size_t) (1.2 * fs));
    double ph = 0.0, amp = 0.0;
    for (size_t n = 0; n < x.size(); ++n)
    {
        const double t = (double) n / fs;
        const bool second = t >= 0.5;
        const double target = t < 0.05 ? 0.0 : (second ? 0.5 * dbToGain ((float) -drop) : 0.5) * std::exp (-(t - (second ? 0.5 : 0.05)) * 1.5);
        amp = t < 0.05 ? 0.0 : (n == (size_t) (0.05 * fs) ? target : amp + (target - amp) * (second ? 0.2 : 1.0));
        ph += (second ? f2 : f1) / fs;
        ph -= std::floor (ph);
        double v = 0.0;
        for (int h = 1; h <= 6; ++h)
            v += std::sin (2.0 * M_PI * h * ph) / h;
        x[n] = (float) (amp * v);
    }
    return x;
}
void probeBlipsConfidencePass()
{
    using namespace blips;
    std::vector<Variant> v;
    v.push_back ({ "none (v0.5.9 defaults)", {}, {} });
    std::vector<std::pair<float, float>> grid;
    for (float db : { 6.0f, 9.0f, 12.0f })
        for (float conf : { 0.0f, 0.95f, 0.97f, 0.98f })
        {
            grid.push_back ({ db, conf });
            v.push_back (pick (juce::String::formatted ("fall lock %.0f dB, pass at conf %.2f", db, conf), [=] (PickSwitches& p) {
                p.fallLockDb = db;
                p.fallOnWindowRms = true;
                p.fallPassConfidence = conf;
            }));
        }
    compare (v);
    // Legato steps: when the voice's note follows (ms after the step), none vs each grid point.
    for (int step : { 2, -2 })
    {
        juce::String head = juce::String::formatted ("legato step %+d st, level down", step);
        for (double drop : { 3.0, 6.0, 9.0, 12.0, 18.0, 24.0 })
        {
            const auto in = legatoStep (drop, step);
            auto follow = [&] (const PickSwitches* sw) {
                Engine e;
                e.prepare (kFs, 64);
                e.setPickSwitches (sw != nullptr ? *sw : PickSwitches {});
                double at = -1.0;
                e.hopListener = [&] (const HopInfo& h) {
                    if (at < 0.0 && h.time >= (int64_t) (0.5 * kFs) && h.noteN == 64 + step)
                        at = (h.time - 0.5 * kFs) / kFs * 1000.0;
                };
                runEngine (e, in, EngineParams {});
                return at;
            };
            juce::String line = juce::String::formatted ("%s %4.0f dB: none %5.1f ms", head.toRawUTF8(), drop, follow (nullptr));
            for (auto [db, conf] : grid)
            {
                PickSwitches sw;
                sw.fallLockDb = db;
                sw.fallOnWindowRms = true;
                sw.fallPassConfidence = conf;
                line << juce::String::formatted (" | %.0f/%.2f %5.1f", db, conf, follow (&sw));
            }
            info (line);
        }
    }
}

// The other takes' flagged picks: the input's own pitch (measuring detector, 150 Hz window) against the output's, every 10 ms
// for 160 ms from the pick, to tell a played slide (the output following the input) from a blip.
void probeBlipsOtherPicks()
{
    using namespace blips;
    for (const auto& t : takes())
    {
        if (t.name.startsWith ("blips"))
            continue;
        const auto ps = takePicks (t.x);
        EngineParams p;
        p.wave = Wave::Sine;
        const auto r = run (t.x, p);
        const auto sc = score (r.out, ps, r.latency);
        const auto c = cycles (r.out, r.latency);
        const auto m = measureHops (t.x, "blips/measure-other", 150.0);
        for (size_t k = 0; k < ps.size(); ++k)
        {
            if (ps[k].note < 0 || ! sc.picks[k].blip())
                continue;
            juce::String line;
            for (double ms = 0.0; ms <= 160.0; ms += 10.0)
            {
                const double at = ps[k].t + ms * 0.001 * kFs;
                double in = 0.0, out = 0.0;
                for (const auto& h : m)
                    if (std::abs ((double) h.time - at) < 64.0 && h.detector.voiced)
                        in = h.midi;
                for (const auto& x : c)
                    if (std::abs (x.t - at) < 0.002 * kFs)
                        out = x.midi;
                line << juce::String::formatted ("%3.0f: %5.1f/%5.1f  ", ms, in, out);
            }
            info (juce::String::formatted ("%s pick at %.3f s (ms: input / output midi): ", t.name.substring (0, 30).toRawUTF8(), ps[k].t / kFs) + line);
        }
    }
}

// The stale glide-in (Glide Always from a pitch that has long faded): the fall lock plus a fresh latch on a pick after a long
// unvoiced gap, on every take (blips, and the cost on the slide takes and the regression phrase), and how many picks of the
// regression phrase and separate plucks still glide in (the feature kept for picks inside a short dropout).
void probeBlipsStale()
{
    using namespace blips;
    std::vector<Variant> v;
    auto fixWith = [] (float gapMs) {
        PickSwitches p;
        p.fallLockDb = 9.0f;
        p.fallOnWindowRms = true;
        p.fallPassConfidence = 0.97f;
        p.rules.freshAfterGapMs = gapMs;
        return p;
    };
    v.push_back ({ "none (v0.5.9 defaults)", {}, {} });
    for (float g : { -1.0f, 20.0f, 30.0f, 50.0f, 100.0f })
    {
        const auto sw = fixWith (g);
        v.push_back ({ g < 0.0f ? juce::String ("fix (fall lock 9 dB, conf 0.97)") : juce::String::formatted ("fix + fresh latch after >= %.0f ms unvoiced", g),
                       [sw] (Engine& e) { e.setPickSwitches (sw); }, {} });
    }
    compare (v);
    // Per take: the picks that blip, without and with each.
    for (const auto& t : takes())
    {
        if (t.name == "blips")
            continue;
        const auto ps = takePicks (t.x);
        juce::String line = juce::String::formatted ("%-40s", t.name.substring (0, 40).toRawUTF8());
        for (const auto& var : v)
        {
            EngineParams p;
            p.wave = Wave::Sine;
            const auto r = run (t.x, p, var.setup);
            const auto s = score (r.out, ps, r.latency);
            line << juce::String::formatted (" | %d blip %4.0f ms", s.blips, s.ms);
        }
        info (line);
    }
    // How many of the regression phrase's and separate plucks' note arrivals still glide in (an Always latch), per variant.
    for (const auto& var : v)
    {
        int glides = 0, fresh = 0;
        for (const auto& in : { regressionPhrase (kFs), separatePlucks (kFs) })
        {
            const auto r = run (in, EngineParams {}, var.setup);
            for (const auto& h : r.hops)
            {
                glides += h.event == (int) Quantizer::Event::AlwaysLatch ? 1 : 0;
                fresh += h.event == (int) Quantizer::Event::FreshLatch ? 1 : 0;
            }
        }
        info (juce::String::formatted ("%-46s regression phrase + separate plucks: %d arrivals glide in (Always), %d latch fresh", var.name.toRawUTF8(), glides, fresh));
    }
}

// The one suite test the fix moves (inheritance: Sustain 0, hammerOn (-12 / 0 dB) and pluckGapPluck = v0.5): which hops' notes
// change, and by how long.
void probeBlipsHammerOn()
{
    PickSwitches fix;
    fix.fallLockDb = 9.0f;
    fix.fallOnWindowRms = true;
    fix.fallPassConfidence = 0.97f;
    int k = 0;
    for (const auto& in : { hammerOn (-12.0), hammerOn (0.0), pluckGapPluck (hzFromMidi (64), hzFromMidi (69), 0.04) })
    {
        EngineParams p = v051Defaults();
        const auto a = blips::run (in, p), b = blips::run (in, p, [fix] (Engine& e) { e.setPickSwitches (fix); });
        juce::String line;
        int diff = 0;
        for (size_t h = 0; h < std::min (a.hops.size(), b.hops.size()); ++h)
            if (a.hops[h].noteN != b.hops[h].noteN || std::abs (a.hops[h].target - b.hops[h].target) > 1e-9)
            {
                ++diff;
                if (diff <= 8)
                    line << juce::String::formatted ("%.4f s: N %d -> %d (raw %.2f conf %.2f rms %.1f dB, LOCK %d)  ", a.hops[h].time / kFs, a.hops[h].noteN, b.hops[h].noteN,
                                                     a.hops[h].rawMidi, a.hops[h].detector.confidence, toDb (a.hops[h].detector.rms), (int) b.hops[h].lockHeld);
            }
        info (juce::String::formatted ("input %d: %d hops differ. ", ++k, diff) + line);
    }
}

// Legato takes (hammer-ons, pull-offs, trills): the fix's risk. The input's notes come from the measuring detector (150 Hz
// window, voiced at 0.9 confidence or more): a note is 4+ consecutive hops on one rounded note; a change is a note different
// from the one before it. For each change, when the engine's N reaches the new note (ms after the input's first hop on it;
// voice time), per variant; "missed" if it never does before the next change. Plus the picks' blips and the ms the output is
// more than 50 c off the input.
namespace blips
{
struct Change
{
    int64_t t = 0, end = 0;
    int from = -1, to = -1;
    double rmsDb = 0.0, dropDb = 0.0; // the input window's level at the change, and re. its loudest of the 50 ms before
};
std::vector<Change> inputChanges (const std::vector<float>& x)
{
    const auto m = measureHops (x, "blips/measure-legato", 150.0);
    struct Seg
    {
        int note;
        size_t first, last;
    };
    std::vector<Seg> segs;
    size_t k = 0;
    while (k < m.size())
    {
        if (! (m[k].detector.voiced && m[k].detector.confidence >= 0.9f))
        {
            ++k;
            continue;
        }
        const int n = (int) std::lround (m[k].midi);
        size_t j = k;
        while (j + 1 < m.size() && m[j + 1].detector.voiced && m[j + 1].detector.confidence >= 0.9f && (int) std::lround (m[j + 1].midi) == n)
            ++j;
        if (j - k + 1 >= 4)
            segs.push_back ({ n, k, j });
        k = j + 1;
    }
    std::vector<Change> out;
    for (size_t s = 1; s < segs.size(); ++s)
    {
        if (segs[s].note == segs[s - 1].note)
            continue;
        Change c;
        c.t = m[segs[s].first].time;
        c.from = segs[s - 1].note;
        c.to = segs[s].note;
        c.rmsDb = toDb (m[segs[s].first].detector.rms);
        float peak = 0.0f;
        for (size_t h = 0; h < segs[s].first; ++h)
            if (m[h].time >= c.t - (int64_t) (0.05 * kFs))
                peak = std::max (peak, m[h].detector.rms);
        c.dropDb = toDb (m[segs[s].first].detector.rms / std::max (peak, 1e-9f));
        out.push_back (c);
    }
    for (size_t s = 0; s < out.size(); ++s)
        out[s].end = s + 1 < out.size() ? out[s + 1].t : (int64_t) x.size();
    return out;
}
// ms from the change until the engine's N is the new note (kMissed: never, before the next change).
constexpr double kMissed = -1.0e9;
inline bool isMissed (double d) { return d < -1.0e8; }
std::vector<double> follows (const Run& r, const std::vector<Change>& cs)
{
    std::vector<double> d;
    for (const auto& c : cs)
    {
        double at = kMissed;
        for (const auto& h : r.hops)
            if (h.time >= c.t - (int64_t) (0.03 * kFs) && h.time < c.end && h.noteN == c.to && h.detector.voiced)
            {
                at = (double) (h.time - c.t) / kFs * 1000.0;
                break;
            }
        d.push_back (at);
    }
    return d;
}
} // namespace blips

void probeBlipsLegato()
{
    using namespace blips;
    std::vector<Variant> v;
    v.push_back ({ "none (v0.5.9 defaults)", {}, {} });
    auto fall = [&] (float db, float conf, float ms, float semis = 0.0f) {
        v.push_back (pick (juce::String::formatted ("fall %.0f dB/%.0f ms, pass %.2f%s", db, ms, conf, semis > 0.0f ? juce::String::formatted (", >= %.0f st", semis).toRawUTF8() : ""),
                           [=] (PickSwitches& p) {
                               p.fallLockDb = db;
                               p.fallOnWindowRms = true;
                               p.fallWindowMs = ms;
                               p.fallPassConfidence = conf;
                               p.fallMinSemis = semis;
                           }));
    };
    fall (9.0f, 0.97f, 50.0f); // the recommended
    fall (9.0f, 0.0f, 50.0f);
    fall (9.0f, 0.95f, 50.0f);
    fall (6.0f, 0.97f, 50.0f);
    fall (12.0f, 0.97f, 50.0f);
    fall (9.0f, 0.97f, 30.0f);
    fall (9.0f, 0.97f, 50.0f, 3.0f);
    fall (9.0f, 0.97f, 50.0f, 4.0f);
    fall (9.0f, 0.97f, 50.0f, 5.0f);
    fall (9.0f, 0.0f, 50.0f, 4.0f);
    fall (9.0f, 0.0f, 50.0f, 5.0f);
    auto mute = [&] (const juce::String& name, float db, float semis, float conf, int hops) {
        v.push_back (pick (name, [=] (PickSwitches& p) {
            p.fallLockDb = db;
            p.fallOnWindowRms = true;
            p.fallPassConfidence = 0.97f;
            p.fallMinSemis = semis;
            p.muteKeepConfidence = conf;
            p.muteKeepHops = hops;
        }));
    };
    mute ("mute keeps conf 0.97 (alone)", 0.0f, 0.0f, 0.97f, 1);
    mute ("fall >= 3 st + mute keeps 0.97", 9.0f, 3.0f, 0.97f, 1);
    mute ("fall >= 3 st + mute keeps 0.97, 2 hops", 9.0f, 3.0f, 0.97f, 2);
    mute ("fall >= 3 st + mute keeps 0.95", 9.0f, 3.0f, 0.95f, 1);
    info ("columns of the per-change rows: the variants in this order");
    for (const auto& t : takes())
    {
        if (! (t.name.startsWith ("hammer") || t.name.startsWith ("pull") || t.name.startsWith ("trill") || t.name == "blips-dry"))
            continue;
        const auto cs = inputChanges (t.x);
        const auto ps = takePicks (t.x);
        info ("== " + t.name + juce::String::formatted (": %zu note changes in the input, %zu picks", cs.size(), ps.size()));
        std::vector<std::vector<double>> all;
        for (const auto& var : v)
        {
            EngineParams p;
            p.wave = Wave::Sine;
            const auto r = run (t.x, p, var.setup);
            const auto d = follows (r, cs);
            all.push_back (d);
            std::vector<double> got;
            int missed = 0, later = 0;
            double worseMs = 0.0;
            for (size_t k = 0; k < d.size(); ++k)
            {
                if (isMissed (d[k]))
                    ++missed;
                else
                    got.push_back (d[k]);
                if (! isMissed (all.front()[k]))
                {
                    const double dd = (isMissed (d[k]) ? 200.0 : d[k]) - all.front()[k];
                    if (dd > 2.0)
                        ++later;
                    worseMs = std::max (worseMs, dd);
                }
            }
            std::sort (got.begin(), got.end());
            const auto s = score (r.out, ps, r.latency);
            info (juce::String::formatted ("%-40s changes: median %5.1f ms, 90%% %5.1f, max %6.1f, missed %2d, later than none %2d (worst +%5.1f ms) | picks: %2d blip, %5.0f ms > 50 c | tracking %5.0f ms",
                                           var.name.toRawUTF8(), got.empty() ? 0.0 : got[got.size() / 2], got.empty() ? 0.0 : got[got.size() * 9 / 10],
                                           got.empty() ? 0.0 : got.back(), missed, later, worseMs, s.blips, s.ms, trackingMs (t.x, var)));
        }
        // The changes the recommended fix delays, and every change's level.
        for (size_t k = 0; k < cs.size(); ++k)
        {
            const auto& c = cs[k];
            juce::String line = juce::String::formatted ("  %7.3f s %-4s -> %-4s (%+3d st) rms %5.1f dB, %+5.1f dB re. 50 ms: ", c.t / kFs,
                                                         juce::MidiMessage::getMidiNoteName (c.from, true, true, 4).toRawUTF8(),
                                                         juce::MidiMessage::getMidiNoteName (c.to, true, true, 4).toRawUTF8(), c.to - c.from, c.rmsDb, c.dropDb);
            bool differs = false;
            for (size_t j = 0; j < all.size(); ++j)
            {
                line << (isMissed (all[j][k]) ? juce::String ("  miss ") : juce::String::formatted ("%6.1f ", all[j][k]));
                differs = differs || std::abs (all[j][k] - all[0][k]) > 2.0;
            }
            if (differs || isMissed (all[0][k]) || all[0][k] > 30.0)
                info (line);
        }
    }
}

// Every hop of a stretch of a take (GTT_HOPS="take from to", seconds), at the defaults: the raw reading, the note, the flags.
void probeBlipsHops()
{
    using namespace blips;
    const auto spec = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("GTT_HOPS", "trills 3.8 4.7"), " ", "");
    const double from = spec[1].getDoubleValue(), to = spec[2].getDoubleValue();
    for (const auto& t : takes())
    {
        if (t.name != spec[0])
            continue;
        EngineParams p;
        p.wave = Wave::Sine;
        const auto r = run (t.x, p);
        const auto m = measureHops (t.x, "blips/measure-hops", 150.0);
        for (const auto& h : r.hops)
        {
            if (h.time < from * kFs || h.time > to * kFs)
                continue;
            double in = 0.0;
            for (const auto& x : m)
                if (std::abs (x.time - h.time) <= 64 && x.detector.voiced)
                    in = x.midi;
            const auto& v = r.probes[(size_t) std::clamp<int64_t> (h.time, 0, (int64_t) r.probes.size() - 1)];
            info (juce::String::formatted ("%.4f in %6.2f | raw %6.2f conf %.2f %s rms %5.1f %s| N %d T %6.2f %-6s %s%s%s%s%s%s| gate %.2f lvl %6.1f ref %6.1f", h.time / kFs, in,
                                           h.rawMidi, h.detector.confidence, h.detector.voiced ? "V" : "-", toDb (h.detector.rms), h.detector.guardRejected ? "GUARD " : "",
                                           h.noteN, h.target, eventName (h.event), h.onset ? "ONSET " : "", h.voiceOpen ? "open " : "", h.lockHeld ? "LOCK " : "",
                                           h.muted ? "MUTE " : "", h.revivalBlocked ? "REVIVE " : "", h.edgeLag ? "EDGE " : "", v.gate, toDb (v.level), toDb (v.ref)));
        }
    }
}

// The legato takes to listen to (Saw, the plugin's defaults): the dry input, as it is, and the candidates; and for each, the ms
// more than 50 c off the input per second of the take (where a variant's cost or gain sits).
void probeBlipsLegatoWavs()
{
    using namespace blips;
    PickSwitches fall, both;
    fall.fallLockDb = 9.0f;
    fall.fallOnWindowRms = true;
    fall.fallPassConfidence = 0.97f;
    fall.fallMinSemis = 3.0f;
    both = fall;
    both.muteKeepConfidence = 0.97f;
    PickSwitches muteOnly;
    muteOnly.muteKeepConfidence = 0.97f;
    outDir.getChildFile ("blips").createDirectory();
    for (const auto& t : takes())
    {
        const bool legato = t.name.startsWith ("hammer") || t.name.startsWith ("pull") || t.name.startsWith ("trill");
        if (! legato && t.name != "blips-dry")
            continue;
        if (legato)
            writeWav ("blips/" + t.name + "-0-dry", t.x);
        const std::vector<std::pair<juce::String, PickSwitches>> vs = { { "1-before", {} }, { "2-fall-lock-3st", fall }, { "3-fall-lock-3st-plus-mute-keeps-sure", both } };
        for (const auto& [name, sw] : vs)
        {
            const auto swc = sw;
            if (legato || name.startsWith ("3"))
                writeWav ("blips/" + t.name + "-" + name, run (t.x, EngineParams {}, [swc] (Engine& e) { e.setPickSwitches (swc); }).out);
        }
        // Per second: ms > 50 c off the input (Sine), as it is / the fall lock / both / the mute rule alone.
        const auto m = measureHops (t.x, "blips/measure-150", 150.0);
        auto perSecond = [&] (const PickSwitches& sw) {
            EngineParams p;
            p.wave = Wave::Sine;
            const auto r = run (t.x, p, [sw] (Engine& e) { e.setPickSwitches (sw); });
            const auto c = cycles (r.out, r.latency);
            std::vector<double> ms ((size_t) (t.x.size() / kFs) + 1, 0.0);
            double prevT = -1.0;
            size_t h = 0;
            for (const auto& x : c)
            {
                const double dt = prevT < 0.0 ? 0.0 : std::min (x.t - prevT, 0.01 * kFs) / kFs * 1000.0;
                prevT = x.t;
                while (h + 1 < m.size() && m[h + 1].time <= (int64_t) x.t)
                    ++h;
                if (x.t >= 0.0 && h < m.size() && m[h].detector.voiced && m[h].detector.confidence >= 0.9f && std::abs (x.midi - m[h].midi) > 0.5 && x.amp > 1.0e-3f)
                    ms[std::min (ms.size() - 1, (size_t) (x.t / kFs))] += dt;
            }
            return ms;
        };
        const auto a = perSecond ({}), b = perSecond (fall), c = perSecond (both), d = perSecond (muteOnly);
        juce::String line;
        for (size_t s = 0; s < a.size(); ++s)
            line << juce::String::formatted ("%zu s %.0f/%.0f/%.0f/%.0f  ", s, a[s], b[s], c[s], d[s]);
        info (t.name + " ms > 50 c off per second (as it is / fall lock >= 3 st / + mute keeps sure / mute rule alone): " + line);
    }
}
