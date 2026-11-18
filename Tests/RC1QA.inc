// RC1 QA (docs/reports/QA-REPORT-RC1.md): the release-candidate takes in Recordings/Release Candidate Recordings/, analysed. Probes only:
// nothing here is a pass / fail invariant, and nothing changes the engine. Uses golden:: (the takes, the settings set) and the
// blips / sputter investigations' tools. Outputs to build/test-output/rc1/, clips to Recordings/RC1-QA/clips/ (local).
//
//   probe-rc1-match   the wet bounces against renders of the dry takes: alignment, envelopes, pitch, the input gain
//   probe-rc1         the QA analysis (every take at its noted settings and across the settings set) and the clips

namespace rc1
{
using golden::kTakes;
using golden::kNumTakes;

double peakDb (const std::vector<float>& x)
{
    float m = 0.0f;
    for (float v : x)
        m = std::max (m, std::abs (v));
    return toDb (m);
}
double rmsDb (const std::vector<float>& x, size_t from = 0, size_t to = SIZE_MAX)
{
    to = std::min (to, x.size());
    double s = 0.0;
    for (size_t i = from; i < to; ++i)
        s += (double) x[i] * x[i];
    return toDb (std::sqrt (s / (double) std::max<size_t> (1, to - from)));
}

// RMS envelope in frames of `frame` samples (linear).
std::vector<double> envelope (const std::vector<float>& x, int frame)
{
    std::vector<double> e (x.size() / (size_t) frame);
    for (size_t k = 0; k < e.size(); ++k)
    {
        double s = 0.0;
        for (size_t i = k * (size_t) frame; i < (k + 1) * (size_t) frame; ++i)
            s += (double) x[i] * x[i];
        e[k] = std::sqrt (s / frame);
    }
    return e;
}

// The lag (in frames) of b against a that maximises the normalised correlation of their envelopes: b[k + lag] ~ a[k].
int bestLag (const std::vector<double>& a, const std::vector<double>& b, int maxLag, double* score = nullptr)
{
    int best = 0;
    double bestC = -2.0;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (size_t k = 0; k < a.size(); ++k)
        {
            const int64_t j = (int64_t) k + lag;
            if (j < 0 || j >= (int64_t) b.size())
                continue;
            ab += a[k] * b[(size_t) j];
            aa += a[k] * a[k];
            bb += b[(size_t) j] * b[(size_t) j];
        }
        const double c = ab / std::sqrt (aa * bb + 1e-30);
        if (c > bestC)
        {
            bestC = c;
            best = lag;
        }
    }
    if (score != nullptr)
        *score = bestC;
    return best;
}

// The sample offset of wet against a render of the same dry take (wet[n + offset] ~ render[n]): envelopes at 1 ms over +-1.5 s,
// then the raw signals within +-2 ms of it.
struct Alignment
{
    int64_t offset = 0;
    double envCorr = 0.0, sampleCorr = 0.0;
};
Alignment align (const std::vector<float>& render, const std::vector<float>& wet)
{
    Alignment a;
    const int frame = 48;
    const auto er = envelope (render, frame), ew = envelope (wet, frame);
    const int coarse = bestLag (er, ew, 1500, &a.envCorr);
    double best = -2.0;
    int64_t bestOff = (int64_t) coarse * frame;
    for (int64_t off = (int64_t) coarse * frame - 96; off <= (int64_t) coarse * frame + 96; ++off)
    {
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (size_t n = 0; n < render.size(); n += 2)
        {
            const int64_t j = (int64_t) n + off;
            if (j < 0 || j >= (int64_t) wet.size())
                continue;
            ab += (double) render[n] * wet[(size_t) j];
            aa += (double) render[n] * render[n];
            bb += (double) wet[(size_t) j] * wet[(size_t) j];
        }
        const double c = ab / std::sqrt (aa * bb + 1e-30);
        if (c > best)
        {
            best = c;
            bestOff = off;
        }
    }
    a.offset = bestOff;
    a.sampleCorr = best;
    return a;
}

// wet shifted into dry time (render time): out[n] = wet[n + offset], zero outside.
std::vector<float> shifted (const std::vector<float>& wet, int64_t offset, size_t length)
{
    std::vector<float> y (length, 0.0f);
    for (size_t n = 0; n < length; ++n)
    {
        const int64_t j = (int64_t) n + offset;
        if (j >= 0 && j < (int64_t) wet.size())
            y[n] = wet[(size_t) j];
    }
    return y;
}

// How alike two outputs are (both in dry time): envelopes (10 ms, dB, the second gain-matched to the first by the median
// difference over frames where either is within 40 dB of its peak) and pitch (the measuring detector at 0.9 confidence or more).
struct Similarity
{
    double gainDb = 0.0;          // b's gain against a (median)
    double envMedianDb = 0.0;     // median |difference| after the gain match
    double env90Db = 0.0;         // 90th percentile
    double envOver6Pct = 0.0;     // frames more than 6 dB apart, % of the frames compared
    double onlyA = 0.0, onlyB = 0.0; // seconds sounding (within 40 dB of the peak) in one and 50 dB+ under in the other
    double pitchAgreePct = 0.0;   // hops voiced in both: % within 50 c
    double pitchBothS = 0.0;      // seconds voiced in both
    double sampleCorr = 0.0;
};
Similarity similarity (const std::vector<float>& a, const std::vector<float>& b, bool pitch = true)
{
    Similarity s;
    const int frame = 480;
    const auto ea = envelope (a, frame), eb = envelope (b, frame);
    const size_t n = std::min (ea.size(), eb.size());
    double pa = 0.0, pb = 0.0;
    for (size_t k = 0; k < n; ++k)
    {
        pa = std::max (pa, ea[k]);
        pb = std::max (pb, eb[k]);
    }
    std::vector<double> d;
    for (size_t k = 0; k < n; ++k)
        if (ea[k] > pa * 0.01 || eb[k] > pb * 0.01)
            d.push_back (toDb ((float) (eb[k] + 1e-9)) - toDb ((float) (ea[k] + 1e-9)));
    if (! d.empty())
    {
        auto sorted = d;
        std::sort (sorted.begin(), sorted.end());
        s.gainDb = sorted[sorted.size() / 2];
        std::vector<double> ad;
        int over6 = 0;
        for (double v : d)
        {
            ad.push_back (std::abs (v - s.gainDb));
            over6 += std::abs (v - s.gainDb) > 6.0 ? 1 : 0;
        }
        std::sort (ad.begin(), ad.end());
        s.envMedianDb = ad[ad.size() / 2];
        s.env90Db = ad[(size_t) (0.9 * (double) (ad.size() - 1))];
        s.envOver6Pct = 100.0 * over6 / (double) ad.size();
    }
    const double g = std::pow (10.0, s.gainDb / 20.0);
    for (size_t k = 0; k < n; ++k)
    {
        if (ea[k] > pa * 0.01 && eb[k] < ea[k] * g * 0.00316)
            s.onlyA += 0.01;
        if (eb[k] > pb * 0.01 && eb[k] < ea[k] * g / 0.00316 && false)
            s.onlyB += 0.01;
        if (eb[k] > pb * 0.01 && ea[k] * g < eb[k] * 0.00316)
            s.onlyB += 0.01;
    }
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
    {
        ab += (double) a[i] * b[i];
        aa += (double) a[i] * a[i];
        bb += (double) b[i] * b[i];
    }
    s.sampleCorr = ab / std::sqrt (aa * bb + 1e-30);
    if (pitch)
    {
        const auto ha = measureHops (a, "rc1/measure-tmp-a"), hb = measureHops (b, "rc1/measure-tmp-b");
        int both = 0, agree = 0;
        for (size_t k = 0; k < std::min (ha.size(), hb.size()); ++k)
        {
            const auto &x = ha[k].detector, &y = hb[k].detector;
            if (x.voiced && y.voiced && x.confidence >= 0.9f && y.confidence >= 0.9f)
            {
                ++both;
                agree += std::abs (ha[k].midi - hb[k].midi) < 0.5 ? 1 : 0;
            }
        }
        s.pitchBothS = both * (double) hopFor (kFs) / kFs;
        s.pitchAgreePct = both > 0 ? 100.0 * agree / both : 0.0;
    }
    return s;
}

std::vector<float> gained (const std::vector<float>& x, double db)
{
    const float g = (float) std::pow (10.0, db / 20.0);
    std::vector<float> y (x.size());
    for (size_t i = 0; i < x.size(); ++i)
        y[i] = x[i] * g;
    return y;
}

const std::vector<float>& wet (int take)
{
    static std::vector<float> loaded[kNumTakes];
    static std::once_flag once[kNumTakes];
    std::call_once (once[take], [take] { loaded[take] = golden::readChannel (golden::takesDir().getChildFile (kTakes[take].wet)); });
    return loaded[take];
}

juce::String simText (const Similarity& s)
{
    return juce::String::formatted ("env: median %.2f dB, 90%% %.2f dB, >6 dB %.1f%% | only render %.2f s, only wet %.2f s | pitch agree %.1f%% of %.1f s | corr %.3f",
                                    s.envMedianDb, s.env90Db, s.envOver6Pct, s.onlyA, s.onlyB, s.pitchAgreePct, s.pitchBothS, s.sampleCorr);
}

void probeMatch()
{
    outDir.getChildFile ("rc1").createDirectory();
    for (int t = 0; t < kNumTakes; ++t)
        golden::dry (t), wet (t);
    struct Row
    {
        Alignment al;
        Similarity noted;
        std::vector<std::pair<double, Similarity>> gains;
    };
    std::vector<Row> rows (kNumTakes);
    const std::vector<double> gainsDb = { -24, -18, -12, -9, -6, -3, 0, 3, 6 };
    sputter::parallelFor (kNumTakes, [&] (int t) {
        const auto& d = golden::dry (t);
        const auto p = golden::notedParams (t);
        const auto r = golden::render (d, kFs, p);
        auto& row = rows[(size_t) t];
        row.al = align (r, wet (t));
        const auto w = shifted (wet (t), row.al.offset, d.size());
        row.noted = similarity (r, w);
        for (double g : gainsDb)
            row.gains.push_back ({ g, similarity (golden::render (gained (d, g), kFs, p), w, false) });
    });
    for (int t = 0; t < kNumTakes; ++t)
    {
        const auto& row = rows[(size_t) t];
        info (juce::String::formatted ("%s: dry %.2f s peak %.2f dBFS RMS %.1f dBFS; wet %.2f s peak %.2f dBFS RMS %.1f dBFS", kTakes[t].id,
                                       golden::dry (t).size() / kFs, peakDb (golden::dry (t)), rmsDb (golden::dry (t)), wet (t).size() / kFs,
                                       peakDb (wet (t)), rmsDb (wet (t))));
        info (juce::String::formatted ("  aligned: wet = dry time %+.4f s (%lld samples), envelope corr %.4f, sample corr %.4f; wet's gain re. render %+.2f dB",
                                       row.al.offset / kFs, (long long) row.al.offset, row.al.envCorr, row.al.sampleCorr, row.noted.gainDb));
        info ("  noted settings: " + simText (row.noted));
        for (const auto& [g, s] : row.gains)
            info (juce::String::formatted ("  input %+5.1f dB: gain %+6.2f dB, ", g, s.gainDb) + simText (s));
    }
}
// The sample correlation of a render with the wet take, its offset refined around `near` (+-96 samples, every 4th sample).
double corrAt (const std::vector<float>& r, const std::vector<float>& w, int64_t near, int64_t* found = nullptr, int stride = 4, double s0 = 0.0,
               double s1 = 1.0e9, int span = 96)
{
    double best = -2.0;
    int64_t bestOff = near;
    const size_t end = std::min (r.size(), (size_t) (s1 * kFs));
    for (int64_t off = near - span; off <= near + span; ++off)
    {
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (size_t n = (size_t) std::max<int64_t> ((int64_t) (s0 * kFs), -off); n < end; n += (size_t) stride)
        {
            const int64_t j = (int64_t) n + off;
            if (j >= (int64_t) w.size())
                break;
            ab += (double) r[n] * w[(size_t) j];
            aa += (double) r[n] * r[n];
            bb += (double) w[(size_t) j] * w[(size_t) j];
        }
        const double c = ab / std::sqrt (aa * bb + 1e-30);
        if (c > best)
        {
            best = c;
            bestOff = off;
        }
    }
    if (found != nullptr)
        *found = bestOff;
    return best;
}

juce::String paramsText (const EngineParams& p, double gainDb)
{
    return juce::String::formatted ("%s, octave %d, retune %s, release %.0f ms, sustain %.0f%%, latency %.2f ms (%.1f Hz), input %+.1f dB",
                                    golden::waveName (p.wave), -(int) p.octave,
                                    p.retuneSpeed == RetuneSpeed::Fast ? "fast" : p.retuneSpeed == RetuneSpeed::Medium ? "medium" : "slow",
                                    (double) p.holdMs, (double) p.sustainPercent, latencyMs (kFs, p.windowHz), (double) p.windowHz, gainDb);
}

// What Duncan probably had set: coordinate search from the noted settings, scored by the sample correlation with the wet take.
struct Inferred
{
    EngineParams p;
    double gainDb = 0.0, corr = 0.0;
    int64_t offset = 0;
    std::vector<juce::String> log;
};
Inferred infer (int t, const EngineParams& start, double s0 = 0.0, double s1 = 1.0e9)
{
    Inferred best;
    best.p = start;
    const auto& d = golden::dry (t);
    const auto& w = wet (t);
    const auto r0 = golden::render (d, kFs, best.p);
    const auto al = align (r0, w);
    best.offset = al.offset;
    // Every take that matched exactly did so at offset 0 (the exports start together; only their ends differ), and a wide search on a
    // periodic wave finds a period's alias: so the search stays within 96 samples of 0.
    corrAt (r0, w, 0, &best.offset, 4, s0, s1, 96);
    auto score = [&] (const EngineParams& p, double g) {
        const auto r = golden::render (g == 0.0 ? d : gained (d, g), kFs, p);
        return corrAt (r, w, best.offset, nullptr, 8, s0, s1);
    };
    best.corr = score (best.p, 0.0);
    best.log.push_back (juce::String::formatted ("noted: corr %.4f", best.corr));
    auto tryAll = [&] (const juce::String& what, const std::vector<std::pair<EngineParams, double>>& cands) {
        std::vector<double> c (cands.size());
        sputter::parallelFor ((int) cands.size(), [&] (int i) { c[(size_t) i] = score (cands[(size_t) i].first, cands[(size_t) i].second); });
        const auto k = (size_t) (std::max_element (c.begin(), c.end()) - c.begin());
        juce::String line = what + ":";
        for (size_t i = 0; i < c.size(); ++i)
            line << juce::String::formatted (" %.3f", c[i]);
        best.log.push_back (line);
        if (c[k] > best.corr + 1e-4)
        {
            best.p = cands[k].first;
            best.gainDb = cands[k].second;
            best.corr = c[k];
        }
    };
    for (int round = 0; round < 2; ++round)
    {
        std::vector<std::pair<EngineParams, double>> c;
        for (Wave wv : { Wave::Saw, Wave::Square, Wave::Pulse25, Wave::Triangle, Wave::Sine, Wave::HalfSine, Wave::Organ, Wave::Folded })
            for (int o = 0; o < 4; ++o)
            {
                auto p = best.p;
                p.wave = wv;
                p.octave = (Octave) o;
                c.push_back ({ p, best.gainDb });
            }
        tryAll ("wave x octave (Saw.. Folded, each 0 -1 -2 -3)", c);
        c.clear();
        for (int k = 0; k < kLatencyDetents; ++k)
        {
            auto p = best.p;
            p.windowHz = latencyDetentHz (k);
            c.push_back ({ p, best.gainDb });
        }
        tryAll ("latency detents 0 .. 14", c);
        c.clear();
        for (int rs = 0; rs < 3; ++rs)
        {
            auto p = best.p;
            p.retuneSpeed = (RetuneSpeed) rs;
            c.push_back ({ p, best.gainDb });
        }
        tryAll ("retune fast medium slow", c);
        c.clear();
        for (int nr = 0; nr < 3; ++nr)
        {
            auto p = best.p;
            p.holdMs = noteReleaseMsFor ((NoteRelease) nr);
            c.push_back ({ p, best.gainDb });
        }
        tryAll ("release fast medium long", c);
        c.clear();
        for (float sus : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
        {
            auto p = best.p;
            p.sustainPercent = sus;
            c.push_back ({ p, best.gainDb });
        }
        tryAll ("sustain 0 25 50 75 100", c);
        c.clear();
        if (best.p.chordMode == ChordMode::Memory)
        {
            for (int n : { 2, 3, 4 })
            {
                auto p = best.p;
                p.memoryNotes = n;
                c.push_back ({ p, best.gainDb });
            }
            tryAll ("freeze 2 3 4", c);
            c.clear();
        }
        for (double g = -24.0; g <= 9.0; g += 1.5)
            c.push_back ({ best.p, g });
        tryAll ("input gain -24 .. +9 dB, 1.5 dB steps", c);
    }
    // The gain, finer, and the final offset at full resolution.
    {
        std::vector<std::pair<EngineParams, double>> c;
        for (double g = best.gainDb - 1.5; g <= best.gainDb + 1.5; g += 0.25)
            c.push_back ({ best.p, g });
        tryAll ("input gain, 0.25 dB steps", c);
    }
    const auto rb = golden::render (gained (d, best.gainDb), kFs, best.p);
    best.corr = corrAt (rb, w, best.offset, &best.offset, 1, s0, s1);
    return best;
}

std::vector<Inferred>& inferred()
{
    static std::vector<Inferred> all;
    static std::once_flag once;
    std::call_once (once, [] {
        all.resize (kNumTakes);
        // Starting points: the noted settings, with the wave and octave the spectral search found (probe-rc1-segments); the harmonize
        // take is fitted on its first 10 s (its wave changes after that: see harmonizeWaves()).
        sputter::parallelFor (kNumTakes, [] (int t) {
            auto p = golden::notedParams (t);
            const juce::String id = kTakes[t].id;
            double s0 = 0.0, s1 = 1.0e9;
            p.octave = id == "bass" ? Octave::TripleSub : id == "harmonize" ? Octave::DoubleSub : Octave::Sub;
            p.wave = id == "flatpick" || id == "freeze3" || id == "harmonics" || id == "harmonize" ? Wave::Pulse25 : Wave::Saw;
            if (id == "harmonize")
            {
                s0 = 0.5;
                s1 = 10.5;
            }
            if (id == "flatpick" || id == "fingerpick") // they part from every render later on (probe-rc1-settings-detail): fit the start
            {
                s0 = 0.5;
                s1 = id == "flatpick" ? 20.0 : 3.0;
            }
            all[(size_t) t] = infer (t, p, s0, s1);
        });
    });
    return all;
}

void probeSettings()
{
    for (int t = 0; t < kNumTakes; ++t)
    {
        const auto& x = inferred()[(size_t) t];
        info (juce::String (kTakes[t].id) + ":");
        for (const auto& l : x.log)
            info ("  " + l);
        info (juce::String::formatted ("  => corr %.4f at offset %lld: ", x.corr, (long long) x.offset) + paramsText (x.p, x.gainDb)
              + (x.p.chordMode == ChordMode::Off ? "" : x.p.chordMode == ChordMode::Memory ? ", freeze " + juce::String (x.p.memoryNotes)
                                                                                           : ", harmonize " + juce::String (keyLabel (x.p.key))));
    }
}
// Where a render and the wet take part: each second, the sample correlation, the envelope difference (dB, after the overall gain)
// and the pitch difference (semitones, median over hops voiced in both).
void perSecond (const std::vector<float>& r, const std::vector<float>& wAligned, juce::String& out)
{
    const auto hr = measureHops (r, "rc1/measure-tmp-r"), hw = measureHops (wAligned, "rc1/measure-tmp-w");
    const double gain = similarity (r, wAligned, false).gainDb;
    const size_t sec = (size_t) kFs;
    for (size_t s0 = 0; s0 + sec <= r.size(); s0 += sec)
    {
        double ab = 0, aa = 0, bb = 0;
        for (size_t n = s0; n < s0 + sec; ++n)
        {
            ab += (double) r[n] * wAligned[n];
            aa += (double) r[n] * r[n];
            bb += (double) wAligned[n] * wAligned[n];
        }
        std::vector<double> pd;
        for (size_t k = 0; k < std::min (hr.size(), hw.size()); ++k)
            if (hr[k].time >= (int64_t) s0 && hr[k].time < (int64_t) (s0 + sec) && hr[k].detector.voiced && hw[k].detector.voiced
                && hr[k].detector.confidence > 0.9f && hw[k].detector.confidence > 0.9f)
                pd.push_back (hw[k].midi - hr[k].midi);
        std::sort (pd.begin(), pd.end());
        out << juce::String::formatted ("    %3d s: corr %6.3f  level %+6.1f dB  pitch %s\n", (int) (s0 / sec), ab / std::sqrt (aa * bb + 1e-30),
                                        toDb ((float) std::sqrt (bb / (aa + 1e-30))) - gain,
                                        pd.empty() ? "-" : juce::String::formatted ("%+.2f st (%d hops)", pd[pd.size() / 2], (int) pd.size()).toRawUTF8());
    }
}

void probeSettingsDetail()
{
    juce::String out;
    for (int t = 0; t < kNumTakes; ++t)
    {
        const auto& x = inferred()[(size_t) t];
        const auto r = golden::render (gained (golden::dry (t), x.gainDb), kFs, x.p);
        out << kTakes[t].id << ": " << paramsText (x.p, x.gainDb) << juce::String::formatted (", corr %.4f\n", x.corr);
        perSecond (r, shifted (wet (t), x.offset, r.size()), out);
    }
    outDir.getChildFile ("rc1").getChildFile ("settings-per-second.txt").replaceWithText (out);
    std::printf ("%s", out.toRawUTF8());
}
// Magnitude spectra (2048-point Hann frames every 1024 samples, 0 .. 8 kHz, dB clipped 60 dB under the frame's peak): phase-proof,
// so two outputs whose oscillators drifted apart still compare as alike.
struct Spectra
{
    static constexpr int kOrder = 11, kN = 1 << kOrder, kHop = 1024, kBins = (int) (8000.0 * kN / 48000.0);
    std::vector<std::array<float, kBins>> frames;
    std::vector<float> peak; // the frame's peak (dB), to skip silence
};
Spectra spectra (const std::vector<float>& x)
{
    Spectra s;
    juce::dsp::FFT fft (Spectra::kOrder);
    std::vector<float> buf (2 * Spectra::kN), win (Spectra::kN);
    for (int i = 0; i < Spectra::kN; ++i)
        win[(size_t) i] = 0.5f - 0.5f * (float) std::cos (2.0 * M_PI * i / Spectra::kN);
    for (size_t start = 0; start + Spectra::kN <= x.size(); start += Spectra::kHop)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < Spectra::kN; ++i)
            buf[(size_t) i] = x[start + (size_t) i] * win[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        std::array<float, Spectra::kBins> f;
        float top = -300.0f;
        for (int b = 0; b < Spectra::kBins; ++b)
        {
            f[(size_t) b] = toDb (buf[(size_t) b] + 1e-12f);
            top = std::max (top, f[(size_t) b]);
        }
        for (auto& v : f)
            v = std::max (v, top - 60.0f) - top;
        s.frames.push_back (f);
        s.peak.push_back (top);
    }
    return s;
}
// How alike two outputs' spectra are over frames [from, to): the mean correlation of the clipped dB spectra, over frames where
// both sound (within 50 dB of the louder's peak over the range).
double spectralMatch (const Spectra& a, const Spectra& b, size_t from, size_t to, int* used = nullptr)
{
    to = std::min ({ to, a.frames.size(), b.frames.size() });
    float pa = -300.0f, pb = -300.0f;
    for (size_t k = from; k < to; ++k)
    {
        pa = std::max (pa, a.peak[k]);
        pb = std::max (pb, b.peak[k]);
    }
    double sum = 0.0;
    int n = 0;
    for (size_t k = from; k < to; ++k)
    {
        if (a.peak[k] < pa - 50.0f || b.peak[k] < pb - 50.0f)
            continue;
        double ma = 0, mb = 0;
        for (int i = 0; i < Spectra::kBins; ++i)
        {
            ma += a.frames[k][(size_t) i];
            mb += b.frames[k][(size_t) i];
        }
        ma /= Spectra::kBins;
        mb /= Spectra::kBins;
        double ab = 0, aa = 0, bb = 0;
        for (int i = 0; i < Spectra::kBins; ++i)
        {
            const double x = a.frames[k][(size_t) i] - ma, y = b.frames[k][(size_t) i] - mb;
            ab += x * y;
            aa += x * x;
            bb += y * y;
        }
        sum += ab / std::sqrt (aa * bb + 1e-30);
        ++n;
    }
    if (used != nullptr)
        *used = n;
    return n > 0 ? sum / n : 0.0;
}

juce::String chordText (const EngineParams& p)
{
    return p.chordMode == ChordMode::Off ? juce::String ("off") : p.chordMode == ChordMode::Memory ? "freeze " + juce::String (p.memoryNotes)
                                                                                                  : "harmonize " + juce::String (keyLabel (p.key));
}

// Per 2 s segment of each take: the wave x octave x chord choice whose render's spectra best match the wet take's (the inferred
// sustain / release / latency / gain held). A setting changed mid-take shows as a change of winner.
void probeSegments()
{
    juce::String out;
    for (int t = 0; t < kNumTakes; ++t)
    {
        const auto& x = inferred()[(size_t) t];
        const auto in = gained (golden::dry (t), x.gainDb);
        const auto w = shifted (wet (t), x.offset, in.size());
        const auto sw = spectra (w);
        std::vector<EngineParams> cands;
        for (Wave wv : { Wave::Saw, Wave::Square, Wave::Pulse25, Wave::Triangle, Wave::Sine, Wave::HalfSine, Wave::Organ, Wave::Folded })
            for (int o = 0; o < 4; ++o)
                for (int c = 0; c < kChordChoices; ++c)
                {
                    auto p = x.p;
                    p.wave = wv;
                    p.octave = (Octave) o;
                    setChordChoice (p, c);
                    cands.push_back (p);
                }
        const size_t segFrames = (size_t) (2.0 * kFs / Spectra::kHop);
        const size_t segs = sw.frames.size() / segFrames + 1;
        std::vector<std::vector<double>> score (cands.size(), std::vector<double> (segs, 0.0));
        sputter::parallelFor ((int) cands.size(), [&] (int i) {
            const auto sr = spectra (golden::render (in, kFs, cands[(size_t) i]));
            for (size_t g = 0; g < segs; ++g)
                score[(size_t) i][g] = spectralMatch (sr, sw, g * segFrames, (g + 1) * segFrames);
        });
        out << kTakes[t].id << " (inferred: " << paramsText (x.p, x.gainDb) << ", " << chordText (x.p) << ")\n";
        for (size_t g = 0; g < segs; ++g)
        {
            std::vector<size_t> order (cands.size());
            std::iota (order.begin(), order.end(), 0);
            std::sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return score[a][g] > score[b][g]; });
            if (score[order[0]][g] <= 0.0)
                continue;
            out << juce::String::formatted ("  %5.1f s:", g * 2.0);
            for (int r = 0; r < 3; ++r)
            {
                const auto& p = cands[order[(size_t) r]];
                out << juce::String::formatted ("  %.3f %s %d %s |", score[order[(size_t) r]][g], golden::waveName (p.wave), -(int) p.octave, chordText (p).toRawUTF8());
            }
            out << "\n";
        }
    }
    outDir.getChildFile ("rc1").getChildFile ("segments.txt").replaceWithText (out);
    std::printf ("%s", out.toRawUTF8());
}
} // namespace rc1

namespace rc1
{
juce::String noteNameOf (int midi) { return juce::String (pitchClassName (midi)) + juce::String (midi / 12 - 1); }
juce::String hz (double midi) { return noteNameOf ((int) std::lround (midi)) + juce::String::formatted ("%+.0fc", 100.0 * (midi - std::round (midi))); }

// --- The ground truth: the measuring detector (55 Hz window, no latency limit) on the dry take ---------------------------------
struct Truth
{
    std::vector<HopInfo> hops;
    std::vector<char> ok;   // voiced at 0.9+ and part of a run of 4+ hops within half a semitone of each other
    int64_t first = 0;
    int hop = 128;
    // The truth hop nearest input time t (-1: none).
    int at (double t) const
    {
        const auto k = (int64_t) std::llround ((t - (double) first) / hop);
        return k < 0 || k >= (int64_t) hops.size() ? -1 : (int) k;
    }
    bool voicedAt (double t) const
    {
        const int k = at (t);
        return k >= 0 && ok[(size_t) k];
    }
    // Every truth pitch (midi) in [t0, t1].
    void range (double t0, double t1, double& lo, double& hi, bool& any) const
    {
        any = false;
        const auto k0 = (int64_t) std::llround ((t0 - (double) first) / hop);
        if (k0 >= (int64_t) hops.size())
            return;
        for (int k = (int) std::max<int64_t> (0, k0); k < (int) hops.size() && hops[(size_t) k].time <= t1; ++k)
            if (ok[(size_t) k])
            {
                lo = any ? std::min (lo, hops[(size_t) k].midi) : hops[(size_t) k].midi;
                hi = any ? std::max (hi, hops[(size_t) k].midi) : hops[(size_t) k].midi;
                any = true;
            }
    }
};
Truth truthFor (const std::vector<float>& x)
{
    Truth t;
    t.hops = measureHops (x, "rc1/truth-tmp");
    t.hop = hopFor (kFs);
    t.first = t.hops.empty() ? 0 : t.hops.front().time;
    t.ok.assign (t.hops.size(), 0);
    size_t k = 0;
    while (k < t.hops.size())
    {
        auto good = [&] (size_t i) { return t.hops[i].detector.voiced && t.hops[i].detector.confidence >= 0.9f; };
        if (! good (k))
        {
            ++k;
            continue;
        }
        size_t e = k + 1;
        while (e < t.hops.size() && good (e) && std::abs (t.hops[e].midi - t.hops[e - 1].midi) < 0.5)
            ++e;
        if (e - k >= 4)
            for (size_t i = k; i < e; ++i)
                t.ok[i] = 1;
        k = e;
    }
    return t;
}

// 10 ms RMS of the input, every ms (dB), centred: the guitar's level at input time.
struct Level
{
    std::vector<float> db; // per ms
    float ref = -100.0f;   // the take's playing level: the 95th percentile of the frames above -70 dB
    float at (double t) const
    {
        const auto k = (int64_t) (t / 48.0);
        return k < 0 || k >= (int64_t) db.size() ? -200.0f : db[(size_t) k];
    }
};
Level levelOf (const std::vector<float>& x)
{
    Level l;
    const size_t step = 48, half = 240;
    std::vector<double> sq (x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); ++i)
        sq[i + 1] = sq[i] + (double) x[i] * x[i];
    for (size_t c = 0; c < x.size(); c += step)
    {
        const size_t a = c > half ? c - half : 0, b = std::min (x.size(), c + half);
        l.db.push_back (toDb ((float) std::sqrt ((sq[b] - sq[a]) / (double) std::max<size_t> (1, b - a)) + 1e-12f));
    }
    std::vector<float> loud;
    for (float v : l.db)
        if (v > -70.0f)
            loud.push_back (v);
    std::sort (loud.begin(), loud.end());
    if (! loud.empty())
        l.ref = loud[(size_t) (0.95 * (double) (loud.size() - 1))];
    return l;
}

// --- One instrumented run ------------------------------------------------------------------------------------------------------
struct Block // the Readout and the header status, once per 64-sample block (output time = index * 64)
{
    uint64_t tones = ~uint64_t { 0 };
    int held = 0, noteN = 0, played = 0;
    bool voiced = false, open = false, hasNote = false, two = false;
};
struct Lite // the voice every 16 samples (input time = index * 16)
{
    float gate = 0.0f, level = 0.0f, headroom = 1.0f;
    bool voiced = false, open = false;
    int note = 0;
};
struct Status
{
    double t; // output time
    juce::String text;
};
struct Frame // RC2: every editor frame (30 fps): what the header's rule saw
{
    double t; // output time
    bool mainVoiced, lowVoiced, sounding;
    int lowNote, lowest;
    juce::String text;
};
struct Analysis
{
    EngineParams p;
    std::vector<float> out;      // the render at p
    std::vector<float> sine;     // the same settings with Sine, Octave Main, Chord Off: the live voice's pitch, cycle by cycle
    std::vector<float> sineFull; // Sine with the chord as set (the clicks)
    std::vector<blips::Cycle> cyc;
    std::vector<HopInfo> hops;
    std::vector<Block> blocks;
    std::vector<Lite> lite;
    std::vector<int64_t> onsets;
    std::vector<Status> status;  // the header's status as the editor shows it (30 fps), changes only
    std::vector<Frame> frames;   // RC2: every frame
    int latency = 0;
    float minHeadroom = 1.0f;
    double headroomSeconds = 0.0; // time with the headroom gain under -0.5 dB
};

// RC2 (v0.6.2): what each analysis engine gets after prepare() (RC2.inc sets it to measure v0.6.1 against v0.6.2; empty: the engine as
// it starts). Set it only between runs (the analysis renders in parallel).
std::function<void (Engine&)> engineSetup;
bool headerV061 = false; // RC2: the header as v0.6.1 showed it (noteDisplayFor's lowWinsOverVoiced off)

std::vector<float> renderWith (const std::vector<float>& in, EngineParams p, Analysis* a)
{
    Engine e;
    e.prepare (kFs, 64);
    if (engineSetup)
        engineSetup (e);
    std::vector<float> out (in.size());
    LowRangeDetector low;
    low.prepare (kFs);
    size_t sinceFrame = 0, frameStart = 0;
    const size_t frame = (size_t) (kFs / 30.0);
    if (a != nullptr)
    {
        a->lite.reserve (in.size() / 16 + 1);
        e.hopListener = [a] (const HopInfo& h) { a->hops.push_back (h); };
        e.voiceListener = [a] (const VoiceProbe& v) {
            if (v.onset)
                a->onsets.push_back (v.t);
            a->minHeadroom = std::min (a->minHeadroom, v.headroomGain);
            if (v.headroomGain < 0.944f)
                a->headroomSeconds += 1.0 / kFs;
            if ((v.t & 15) == 0 && v.t >= 0)
            {
                const auto k = (size_t) (v.t >> 4);
                if (a->lite.size() <= k)
                    a->lite.resize (k + 1);
                a->lite[k] = { v.gate, v.level, v.headroomGain, v.voiced, v.open, v.note };
            }
        };
    }
    juce::String lastStatus = "-";
    for (size_t start = 0; start < in.size(); start += 64)
    {
        const int n = (int) std::min ((size_t) 64, in.size() - start);
        e.process (in.data() + start, out.data() + start, n, p);
        if (a == nullptr)
            continue;
        auto& r = e.readout();
        Block b;
        b.tones = r.chordTones.load();
        b.held = r.memoryHeld.load();
        b.noteN = r.noteN.load();
        b.played = r.outNote.load();
        b.voiced = r.voiced.load();
        b.open = r.voiceOpen.load();
        b.hasNote = r.hasNote.load();
        b.two = r.twoPlusNotes.load();
        a->blocks.push_back (b);
        // The editor: 30 fps, the low-range detector fed what arrived since the last frame, the header status (HeaderBar::statusFor).
        sinceFrame += (size_t) n;
        if (sinceFrame >= frame)
        {
            low.push (in.data() + frameStart, (int) (start + (size_t) n - frameStart));
            frameStart = start + (size_t) n;
            sinceFrame = 0;
            const int lowest = lowestReliableNote (kFs, clampWindow (p.windowHz));
            const auto& lr = low.reading();
            const auto d = noteDisplayFor (b.voiced, b.noteN, lr.voiced, lr.note, lowest, b.two, ! headerV061);
            const bool sounding = b.hasNote && (b.voiced || b.open);
            juce::String s;
            if (d.aboveBoard >= 0)
                s = "above board " + juce::String (noteNameOf (d.aboveBoard));
            else if (d.belowRange >= 0 || d.hint == PitchHint::TooLow)
                s = "too low";
            else if (sounding)
                s = "tracking " + juce::String (noteNameOf (b.played));
            a->frames.push_back ({ (double) (start + (size_t) n), b.voiced, lr.voiced, sounding, lr.note, lowest, s });
            if (s != lastStatus)
            {
                a->status.push_back ({ (double) (start + (size_t) n), s });
                lastStatus = s;
            }
        }
    }
    if (a != nullptr)
        a->latency = e.currentLatencySamples();
    return out;
}

Analysis analyse (const std::vector<float>& in, const EngineParams& p)
{
    Analysis a;
    a.p = p;
    a.out = renderWith (in, p, &a);
    auto ps = p;
    ps.wave = Wave::Sine;
    ps.octave = Octave::Main;
    ps.chordMode = ChordMode::Off;
    a.sine = renderWith (in, ps, nullptr);
    a.cyc = blips::cycles (a.sine, a.latency);
    if (p.chordMode != ChordMode::Off)
    {
        auto pf = p;
        pf.wave = Wave::Sine;
        a.sineFull = renderWith (in, pf, nullptr);
    }
    return a;
}

// --- Events ----------------------------------------------------------------------------------------------------------------------
struct Event
{
    juce::String kind;
    double t0 = 0.0, t1 = 0.0; // input (dry file) time, seconds
    double ms = 0.0;           // how long it's audible
    double size = 0.0;         // kind-specific (cents, dB, ratio)
    juce::String text;
};

double retuneMsOf (const EngineParams& p) { return p.retuneSpeed == RetuneSpeed::Fast ? 0.0 : p.retuneSpeed == RetuneSpeed::Medium ? 40.0 : 120.0; }

// Envelope of an output, 10 ms RMS every ms, at output time.
std::vector<float> envDb (const std::vector<float>& y) { return levelOf (y).db; }

// Wrong notes: audible output cycles more than 75 c outside every pitch the guitar had in the trailing glide window (3 x Retune
// + 40 ms; the guitar's pitch at 0.9+ confidence, the measuring detector), 8 ms or more. And junk: the guitar unpitched, the
// output on a note that's neither the last nor the next one it played.
std::vector<Event> wrongNotes (const Analysis& a, const Truth& tr)
{
    std::vector<Event> ev;
    float peak = 0.0f;
    for (const auto& c : a.cyc)
        peak = std::max (peak, c.amp);
    const float floorAbs = peak * (float) dbToGain (-40.0f);
    const double W = (3.0 * retuneMsOf (a.p) + 40.0) * 0.001 * kFs;
    struct Off
    {
        double t, dt, dev;
        bool junk;
        double outMidi, truthMidi;
    };
    std::vector<Off> offs;
    // local loudness: the cycles' max over +-150 ms (two pointers)
    size_t lo = 0, hi = 0;
    std::deque<size_t> dq;
    for (size_t i = 0; i < a.cyc.size(); ++i)
    {
        const auto& c = a.cyc[i];
        while (hi < a.cyc.size() && a.cyc[hi].t <= c.t + 0.15 * kFs)
        {
            while (! dq.empty() && a.cyc[dq.back()].amp <= a.cyc[hi].amp)
                dq.pop_back();
            dq.push_back (hi++);
        }
        while (a.cyc[lo].t < c.t - 0.15 * kFs)
            ++lo;
        while (! dq.empty() && dq.front() < lo)
            dq.pop_front();
        const float local = dq.empty() ? c.amp : a.cyc[dq.front()].amp;
        if (c.amp < floorAbs || c.amp < local * (float) dbToGain (-30.0f))
            continue;
        const double dt = i > 0 ? std::min (c.t - a.cyc[i - 1].t, 0.01 * kFs) / kFs * 1000.0 : 0.0;
        double l = 0, h = 0;
        bool any = false;
        tr.range (c.t - W, c.t + 0.008 * kFs, l, h, any);
        if (any)
        {
            const double off = c.midi < l ? l - c.midi : c.midi > h ? c.midi - h : 0.0;
            if (off > 0.75)
                offs.push_back ({ c.t, dt, c.midi < l ? c.midi - l : c.midi - h, false, c.midi, c.midi < l ? l : h });
        }
        else
        {
            // unpitched input: the last and next pitches the guitar had (within 2 s / 60 ms)
            double last = -1.0, next = -1.0;
            for (int k = tr.at (c.t); k >= 0 && tr.hops[(size_t) k].time > c.t - 2.0 * kFs; --k)
                if (tr.ok[(size_t) k])
                {
                    last = tr.hops[(size_t) k].midi;
                    break;
                }
            for (int k = std::max (0, tr.at (c.t)); k < (int) tr.hops.size() && tr.hops[(size_t) k].time < c.t + 0.06 * kFs; ++k)
                if (tr.ok[(size_t) k])
                {
                    next = tr.hops[(size_t) k].midi;
                    break;
                }
            const bool nearLast = last > 0 && std::abs (c.midi - last) <= 0.75, nearNext = next > 0 && std::abs (c.midi - next) <= 0.75;
            // between the two (a glide from one to the other) is fine too
            const bool between = last > 0 && next > 0 && c.midi >= std::min (last, next) - 0.75 && c.midi <= std::max (last, next) + 0.75;
            if (! nearLast && ! nearNext && ! between && (last > 0 || next > 0))
                offs.push_back ({ c.t, dt, c.midi - (last > 0 ? last : next), true, c.midi, last > 0 ? last : next });
        }
    }
    for (size_t i = 0; i < offs.size();)
    {
        size_t j = i + 1;
        double ms = offs[i].dt;
        while (j < offs.size() && offs[j].t - offs[j - 1].t < 0.006 * kFs && offs[j].junk == offs[i].junk)
            ms += offs[j++].dt;
        if (ms >= 8.0)
        {
            std::vector<double> devs;
            for (size_t k = i; k < j; ++k)
                devs.push_back (offs[k].dev);
            std::sort (devs.begin(), devs.end());
            const double med = devs[devs.size() / 2];
            const size_t mid = (i + j) / 2;
            Event e;
            e.kind = offs[i].junk ? "junk" : "wrong";
            e.t0 = offs[i].t / kFs;
            e.t1 = offs[j - 1].t / kFs;
            e.ms = ms;
            e.size = 100.0 * med;
            const int iv = (int) std::lround (med);
            const char* cls = std::abs (iv) == 12 || std::abs (iv) == 24 ? "octave" : std::abs (iv) == 7 || std::abs (iv) == 19 ? "fifth" : "other";
            e.text = juce::String::formatted ("%s: plays %s where the guitar %s %s (%+.0f c, %s), %.0f ms", offs[i].junk ? "junk while the guitar is unpitched" : "wrong note",
                                              hz (offs[mid].outMidi).toRawUTF8(), offs[i].junk ? "last / next played" : "plays",
                                              hz (offs[mid].truthMidi).toRawUTF8(), 100.0 * med, cls, ms);
            ev.push_back (e);
        }
        i = j;
    }
    return ev;
}

// Blips at picks. BLIPS-REPORT's measure (5 ms audible > 50 c off the note) is kept as `offMs` but it counts the intended glide
// from the previous note and the detection delay on takes where the notes change, so a blip here is junk: 5 ms or more of
// audible output (within 30 dB of the note's level) outside the span between the previous note and the new one (+-50 c), from
// 60 ms before the pick to 200 ms after it. `blip-3-4st`: the junk 2.5 .. 4.5 semitones from the nearer of the two (the fall
// lock's gap, PARKING-LOT).
std::vector<Event> pickBlips (const Analysis& a, const Truth& tr, const std::vector<blips::Pick>& ps, blips::Score* out = nullptr)
{
    std::vector<Event> ev;
    const auto s = blips::score (a.sine, ps, a.latency, &a.cyc);
    for (size_t k = 0; k < ps.size(); ++k)
    {
        const auto& p = ps[k];
        if (p.note < 0)
            continue;
        // the previous note: the last truth pitch before the pick (within 3 s), else the previous pick's
        double prev = -1.0;
        for (int h = tr.at ((double) p.t - 0.005 * kFs); h >= 0 && tr.hops[(size_t) h].time > p.t - 3.0 * kFs; --h)
            if (tr.ok[(size_t) h])
            {
                prev = tr.hops[(size_t) h].midi;
                break;
            }
        const double N = p.note, P = prev < 0 ? N : std::round (prev);
        const double lo = std::min (P, N) - 0.5, hi = std::max (P, N) + 0.5;
        const double from = std::max ((double) p.t - 0.06 * kFs, k > 0 ? (double) ps[k - 1].t + 0.06 * kFs : 0.0);
        const double to = std::min ((double) p.t + 0.2 * kFs, (double) p.end - 0.005 * kFs);
        float loud = 0.0f;
        for (const auto& x : a.cyc)
            if (x.t >= p.t + 0.03 * kFs && x.t <= p.t + 0.15 * kFs)
                loud = std::max (loud, x.amp);
        if (loud <= 0.0f)
            continue;
        double junkMs = 0.0, worst = 0.0, prevT = -1.0, firstJunk = -1.0;
        std::vector<double> junkMidi;
        for (const auto& x : a.cyc)
        {
            if (x.t < from || x.t > to)
                continue;
            const double dt = prevT < 0.0 ? 0.0 : std::min (x.t - prevT, 0.01 * kFs) / kFs * 1000.0;
            prevT = x.t;
            if (x.amp < loud * (float) dbToGain (-30.0f))
                continue;
            if (x.midi < lo || x.midi > hi)
            {
                junkMs += dt;
                const double d = x.midi < lo ? x.midi - (lo + 0.5) : x.midi - (hi - 0.5);
                if (std::abs (d) > std::abs (worst))
                    worst = d;
                if (firstJunk < 0.0)
                    firstJunk = x.t;
                junkMidi.push_back (x.midi);
            }
        }
        if (junkMs < 5.0)
            continue;
        Event e;
        std::sort (junkMidi.begin(), junkMidi.end());
        const double jm = junkMidi[junkMidi.size() / 2];
        // where the junk sits re. the note it interrupts (the previous one before the pick, the new one after): the fall lock lets
        // changes under 3 semitones through (kFallLockMinSemitones), refuses the rest unless the reading is 0.97+ sure
        const bool before = firstJunk < (double) p.t;
        const double re = std::abs (jm - (before ? P : N));
        e.kind = re < 2.5 ? "blip-under-3st" : re <= 4.5 ? "blip-3-4st" : "blip-5st+";
        e.size = re;
        e.t0 = firstJunk / kFs;
        e.t1 = to / kFs;
        e.ms = junkMs;
        e.text = juce::String::formatted ("pick of %s at %.3f s (from %s): %.0f ms of junk outside %s .. %s, mostly %s (%+.1f st re. the %s note), worst %+.0f c past the span (%s the pick)",
                                          noteNameOf (p.note).toRawUTF8(), p.t / kFs, prev < 0 ? "silence" : noteNameOf ((int) P).toRawUTF8(), junkMs,
                                          noteNameOf ((int) std::min (P, N)).toRawUTF8(), noteNameOf ((int) std::max (P, N)).toRawUTF8(), hz (jm).toRawUTF8(),
                                          jm - (before ? P : N), before ? "previous" : "new", 100.0 * worst, before ? "before" : "after");
        ev.push_back (e);
    }
    if (out != nullptr)
        *out = s;
    return ev;
}

// Dropouts: the guitar plays a note in the zone (the truth voiced, at or above the window's lowest reliable note, the input within
// 35 dB of the take's playing level) and the voice gate is closed, 20 ms or more.
std::vector<Event> dropouts (const Analysis& a, const Truth& tr, const Level& in)
{
    std::vector<Event> ev;
    const int lowest = lowestReliableNote (kFs, clampWindow (a.p.windowHz));
    double start = -1.0;
    double note = 0.0;
    for (size_t k = 0; k < a.lite.size(); k += 3) // every 48 samples (1 ms)
    {
        const double t = (double) k * 16.0;
        const int h = tr.at (t);
        const bool playing = h >= 0 && tr.ok[(size_t) h] && tr.hops[(size_t) h].midi >= lowest - 0.5 && in.at (t) >= in.ref - 35.0f;
        const bool silent = a.lite[k].gate < 0.01f;
        if (playing && silent)
        {
            if (start < 0.0)
            {
                start = t;
                note = tr.hops[(size_t) h].midi;
            }
        }
        else if (start >= 0.0)
        {
            if (t - start >= 0.02 * kFs)
            {
                Event e;
                e.kind = "dropout";
                e.t0 = start / kFs;
                e.t1 = t / kFs;
                e.ms = (t - start) / kFs * 1000.0;
                e.text = juce::String::formatted ("silent for %.0f ms while the guitar plays %s at %.0f dB re. the take's level", e.ms, hz (note).toRawUTF8(),
                                                  in.at (start) - in.ref);
                ev.push_back (e);
            }
            start = -1.0;
        }
    }
    return ev;
}

// Frozen glides: Retune's glide only moves on voiced hops (Quantizer: "When unvoiced, N, T and r are frozen"), so pitch lost
// mid-glide leaves the voice on an in-between pitch. Runs of 10 ms or more where the hop is unvoiced, the voice audible (its level
// within 20 dB of its peak), and T more than 30 c from N.
std::vector<Event> frozenGlides (const Analysis& a)
{
    std::vector<Event> ev;
    float peak = 0.0f;
    for (const auto& l : a.lite)
        peak = std::max (peak, l.gate * l.level);
    size_t k = 0;
    while (k < a.hops.size())
    {
        auto frozen = [&] (size_t i) {
            const auto& x = a.hops[i];
            const auto li = (size_t) std::max<int64_t> (0, x.time) / 16;
            const float lv = li < a.lite.size() ? a.lite[li].gate * a.lite[li].level : 0.0f;
            return x.hasNote && ! x.detector.voiced && std::abs (x.target - x.noteN) > 0.3 && lv > peak * 0.1f;
        };
        if (! frozen (k))
        {
            ++k;
            continue;
        }
        size_t e = k;
        while (e < a.hops.size() && frozen (e))
            ++e;
        const double ms = (double) (a.hops[e - 1].time - a.hops[k].time + a.latency * 0 + 128) / kFs * 1000.0;
        if (ms >= 10.0)
        {
            Event x;
            x.kind = "frozen-glide";
            x.t0 = a.hops[k].time / kFs;
            x.t1 = a.hops[e - 1].time / kFs;
            x.ms = ms;
            x.size = 100.0 * (a.hops[k].target - a.hops[k].noteN);
            x.text = juce::String::formatted ("pitch lost mid-glide: the voice holds %s (%+.0f c off its note %s) for %.0f ms", hz (a.hops[k].target).toRawUTF8(), x.size,
                                              noteNameOf (a.hops[k].noteN).toRawUTF8(), ms);
            ev.push_back (x);
        }
        k = e;
    }
    return ev;
}

// Hanging notes: the output audible (within 50 dB of its peak) while the guitar has stopped (the input 50 dB under the take's
// level) for longer than Note Release + the grace + the fade + 300 ms.
std::vector<Event> hangs (const Analysis& a, const Level& in)
{
    std::vector<Event> ev;
    const auto& y = a.p.chordMode != ChordMode::Off ? a.sineFull : a.sine;
    const auto o = envDb (y);
    float peak = -200.0f;
    for (float v : o)
        peak = std::max (peak, v);
    const double allowMs = a.p.holdMs + 40.0 + 15.0 + 300.0;
    double quietSince = -1.0;
    bool reported = false;
    for (size_t k = 0; k < o.size(); ++k)
    {
        const double tOut = (double) k * 48.0, t = tOut - a.latency; // input time
        const bool quiet = in.at (t) < in.ref - 50.0f;
        if (! quiet)
        {
            quietSince = -1.0;
            reported = false;
            continue;
        }
        if (quietSince < 0.0)
            quietSince = t;
        if (o[k] > peak - 50.0f && ! reported && (t - quietSince) / kFs * 1000.0 > allowMs)
        {
            size_t e = k;
            while (e < o.size() && o[e] > peak - 50.0f && in.at ((double) e * 48.0 - a.latency) < in.ref - 50.0f)
                ++e;
            Event x;
            x.kind = "hang";
            x.t0 = quietSince / kFs;
            x.t1 = ((double) e * 48.0 - a.latency) / kFs;
            x.ms = (x.t1 - x.t0) * 1000.0;
            x.text = juce::String::formatted ("the output sounds %.0f ms after the guitar stopped (allowed %.0f)", x.ms, allowMs);
            ev.push_back (x);
            reported = true;
        }
    }
    return ev;
}

// Clicks: on the Sine render (the chord as set), the second difference's 0.5 ms peak 10x (20 dB) over the median of the 20 ms
// around it, and over -66 dB (absolute).
std::vector<Event> clicks (const Analysis& a)
{
    std::vector<Event> ev;
    const auto& y = a.p.chordMode != ChordMode::Off ? a.sineFull : a.sine;
    const size_t fr = 24;
    std::vector<float> m (y.size() / fr, 0.0f);
    for (size_t k = 0; k < m.size(); ++k)
        for (size_t i = std::max<size_t> (2, k * fr); i < (k + 1) * fr; ++i)
            m[k] = std::max (m[k], std::abs (y[i] - 2.0f * y[i - 1] + y[i - 2]));
    double last = -1.0;
    for (size_t k = 20; k + 20 < m.size(); ++k)
    {
        std::vector<float> around;
        for (size_t j = k - 20; j <= k + 20; ++j)
            if (j + 1 < k || j > k + 1)
                around.push_back (m[j]);
        std::nth_element (around.begin(), around.begin() + (long) around.size() / 2, around.end());
        const float med = around[around.size() / 2];
        if (m[k] > 10.0f * med && m[k] > 5.0e-4f)
        {
            const double t = ((double) k * fr - a.latency) / kFs;
            if (t - last > 0.005)
            {
                Event e;
                e.kind = "click";
                e.t0 = e.t1 = t;
                e.size = toDb (m[k] / std::max (med, 1e-9f));
                e.text = juce::String::formatted ("a step in the Sine render: the 2nd difference %.0f dB over its surroundings (%.1e)", e.size, (double) m[k]);
                ev.push_back (e);
            }
            last = t;
        }
    }
    return ev;
}

// Stale glides: a pick whose first audible cycle starts 1.5 semitones or more off its note (it glides in), sorted by how long the
// guitar had been unpitched before it (the pick glide is meant for 100 ms; Glide Always for the whole Note Release).
std::vector<Event> glideIns (const Truth& tr, const std::vector<blips::Pick>& ps, const blips::Score& s)
{
    std::vector<Event> ev;
    for (size_t k = 0; k < ps.size(); ++k)
    {
        const auto& r = s.picks[k];
        if (ps[k].note < 0 || r.firstMs < 0.0 || std::abs (r.startC) < 150.0)
            continue;
        double lastVoiced = -1.0;
        for (int h = tr.at ((double) ps[k].t - 0.01 * kFs); h >= 0 && tr.hops[(size_t) h].time > ps[k].t - 3.0 * kFs; --h)
            if (tr.ok[(size_t) h])
            {
                lastVoiced = (double) tr.hops[(size_t) h].time;
                break;
            }
        const double gapMs = lastVoiced < 0 ? 3000.0 : ((double) ps[k].t - lastVoiced) / kFs * 1000.0;
        Event e;
        e.kind = gapMs > 150.0 ? "stale-glide" : "glide-in";
        e.t0 = ps[k].t / kFs;
        e.t1 = e.t0 + 0.2;
        e.ms = r.postMs;
        e.size = r.startC;
        e.text = juce::String::formatted ("pick of %s glides in from %+.0f c; the guitar unpitched for %.0f ms before it; %.0f ms audibly > 50 c off",
                                          noteNameOf (ps[k].note).toRawUTF8(), r.startC, gapMs, r.postMs);
        ev.push_back (e);
    }
    return ev;
}

// Below the zone: stretches of 50 ms or more where the guitar plays under the window's lowest reliable note. What the output does
// (silent, in tune, other) and what the header says.
std::vector<Event> belowZone (const Analysis& a, const Truth& tr, const Level& in)
{
    std::vector<Event> ev;
    const int lowest = lowestReliableNote (kFs, clampWindow (a.p.windowHz));
    auto statusAt = [&] (double tOut) {
        juce::String s;
        for (const auto& x : a.status)
            if (x.t <= tOut)
                s = x.text;
            else
                break;
        return s;
    };
    size_t k = 0;
    while (k < tr.hops.size())
    {
        auto below = [&] (size_t i) { return tr.ok[i] && tr.hops[i].midi < lowest - 0.5 && in.at ((double) tr.hops[i].time) > in.ref - 45.0f; };
        if (! below (k))
        {
            ++k;
            continue;
        }
        size_t e = k + 1, gap = 0;
        while (e < tr.hops.size() && (below (e) || gap < 8))
        {
            gap = below (e) ? 0 : gap + 1;
            ++e;
        }
        e -= gap;
        const double t0 = (double) tr.hops[k].time, t1 = (double) tr.hops[e - 1].time;
        if (t1 - t0 >= 0.05 * kFs)
        {
            std::vector<double> notes;
            for (size_t i = k; i < e; ++i)
                if (tr.ok[i])
                    notes.push_back (tr.hops[i].midi);
            std::sort (notes.begin(), notes.end());
            const double note = notes[notes.size() / 2];
            // the output over the stretch (Sine twin, at Octave Main): audible cycles, in tune (within 50 c of the guitar), other
            double aud = 0, tune = 0, other = 0, octave = 0;
            float peak = 0.0f;
            for (const auto& c : a.cyc)
                peak = std::max (peak, c.amp);
            for (size_t i = 1; i < a.cyc.size(); ++i)
            {
                const auto& c = a.cyc[i];
                if (c.t < t0 || c.t > t1 || c.amp < peak * (float) dbToGain (-40.0f))
                    continue;
                const double dt = std::min (c.t - a.cyc[i - 1].t, 0.01 * kFs);
                aud += dt;
                const double d = c.midi - note;
                if (std::abs (d) <= 0.5)
                    tune += dt;
                else if (std::abs (std::abs (d) - 12.0) <= 0.5 || std::abs (std::abs (d) - 24.0) <= 0.5)
                    octave += dt;
                else
                    other += dt;
            }
            std::map<juce::String, double> shown;
            for (double t = t0; t <= t1; t += 0.033 * kFs)
                shown[statusAt (t + a.latency)] += 33.0;
            juce::String st;
            for (const auto& [s, ms] : shown)
                st << (st.isEmpty() ? "" : ", ") << (s.isEmpty() ? juce::String ("(blank)") : s) << juce::String::formatted (" %.0f%%", 100.0 * ms / ((t1 - t0) / kFs * 1000.0 + 33.0));
            Event x;
            x.kind = other > 0.008 * kFs ? "below-junk" : "below";
            x.t0 = t0 / kFs;
            x.t1 = t1 / kFs;
            x.ms = other / kFs * 1000.0;
            x.size = note;
            x.text = juce::String::formatted ("%s for %.0f ms (lowest reliable %s): output sounds %.0f ms (in tune %.0f, octave off %.0f, other %.0f); header: ",
                                              hz (note).toRawUTF8(), (t1 - t0) / kFs * 1000.0, noteNameOf (lowest).toRawUTF8(), aud / kFs * 1000.0,
                                              tune / kFs * 1000.0, octave / kFs * 1000.0, other / kFs * 1000.0)
                     + st;
            ev.push_back (x);
        }
        k = e;
    }
    return ev;
}

std::vector<int> unpack (uint64_t tones)
{
    std::vector<int> v;
    for (int i = 0; i < 8; ++i)
    {
        const int b = (int) ((tones >> (8 * i)) & 0xff);
        if (b != 0xff)
            v.push_back (b);
    }
    return v;
}
juce::String notesText (const std::vector<int>& v)
{
    juce::String s;
    for (int n : v)
        s << (s.isEmpty() ? "" : " ") << noteNameOf (n);
    return s;
}

// Freeze: what's held (the chord's other tones, as the Readout has them), when it changes, whether each held note was played, and
// each clear's timing against the voice's last voiced sample.
std::vector<Event> freezeEvents (const Analysis& a, const Truth& tr, juce::String& log)
{
    std::vector<Event> ev;
    std::vector<int> lastOthers;
    int lastHeld = 0;
    double lastVoicedT = -1.0;
    for (size_t b = 0; b < a.blocks.size(); ++b)
    {
        const double tOut = (double) (b + 1) * 64.0, t = tOut - a.latency;
        for (size_t k = (size_t) std::max (0.0, (t - 64.0) / 16.0); k < a.lite.size() && (double) k * 16.0 <= t; ++k)
            if (a.lite[k].voiced)
                lastVoicedT = (double) k * 16.0;
        const auto tones = unpack (a.blocks[b].tones);
        std::vector<int> others (tones.size() > 1 ? tones.begin() + 1 : tones.end(), tones.end());
        std::sort (others.begin(), others.end());
        if (others != lastOthers)
        {
            log << juce::String::formatted ("  %7.3f s  live %-4s  others %s  (held %d)\n", t / kFs, tones.empty() ? "-" : noteNameOf (tones[0]).toRawUTF8(),
                                            notesText (others).toRawUTF8(), a.blocks[b].held);
            for (int n : others)
                if (std::find (lastOthers.begin(), lastOthers.end(), n) == lastOthers.end())
                {
                    // was it played? a truth run on that note of 60 ms or more in the last 10 s
                    int run = 0, bestRun = 0;
                    for (int h = std::max (0, tr.at (t - 10.0 * kFs)); h >= 0 && h < (int) tr.hops.size() && tr.hops[(size_t) h].time <= t; ++h)
                    {
                        run = tr.ok[(size_t) h] && std::lround (tr.hops[(size_t) h].midi) == n ? run + 1 : 0;
                        bestRun = std::max (bestRun, run);
                    }
                    if (bestRun * 128.0 / kFs < 0.06)
                    {
                        Event e;
                        e.kind = "freeze-unplayed";
                        e.t0 = e.t1 = t / kFs;
                        e.size = n;
                        e.text = juce::String::formatted ("Freeze holds %s, which the guitar didn't play for 60 ms in the 10 s before (longest %.0f ms)",
                                                          noteNameOf (n).toRawUTF8(), bestRun * 128.0 / kFs * 1000.0);
                        ev.push_back (e);
                    }
                }
            lastOthers = others;
        }
        if (lastHeld > 0 && a.blocks[b].held == 0)
        {
            const double gap = lastVoicedT < 0 ? -1.0 : (t - lastVoicedT) / kFs * 1000.0;
            // did the guitar have a pitch during that gap?
            double guitarMs = 0.0;
            for (int h = std::max (0, tr.at (lastVoicedT)); h >= 0 && h < (int) tr.hops.size() && tr.hops[(size_t) h].time <= t; ++h)
                guitarMs += tr.ok[(size_t) h] ? 128.0 / kFs * 1000.0 : 0.0;
            Event e;
            e.kind = guitarMs >= 50.0 ? "freeze-clear-while-playing" : "freeze-clear";
            e.t0 = e.t1 = t / kFs;
            e.size = gap;
            e.ms = guitarMs;
            e.text = juce::String::formatted ("Freeze clears %.1f ms after the voice's last voiced sample; the guitar had a pitch for %.0f ms of that gap", gap, guitarMs);
            ev.push_back (e);
            log << juce::String::formatted ("  %7.3f s  CLEAR (%.1f ms after the last voiced sample; the guitar pitched %.0f ms of it)\n", t / kFs, gap, guitarMs);
        }
        lastHeld = a.blocks[b].held;
    }
    return ev;
}

// Harmonize: each chord (live + two others) against the key's own triad on that degree.
std::vector<Event> harmonizeEvents (const Analysis& a, juce::String& log)
{
    std::vector<Event> ev;
    const int key = a.p.key;
    static const int steps[7] = { 0, 2, 4, 5, 7, 9, 11 };
    auto degree = [&] (int n) {
        const int pc = ((n - key) % 12 + 12) % 12;
        for (int d = 0; d < 7; ++d)
            if (steps[d] == pc)
                return d;
        return -1;
    };
    std::vector<int> last;
    double since = 0.0;
    for (size_t b = 0; b < a.blocks.size(); ++b)
    {
        const double t = ((double) (b + 1) * 64.0 - a.latency) / kFs;
        const auto tones = unpack (a.blocks[b].tones);
        if (tones == last)
            continue;
        last = tones;
        since = t;
        if (tones.size() < 3)
            continue;
        const int live = tones[0], d = degree (live);
        std::set<int> pcs;
        for (int n : tones)
            pcs.insert (n % 12);
        std::set<int> want;
        if (d >= 0)
            for (int s : { 0, 2, 4 })
                want.insert ((key + steps[(d + s) % 7]) % 12);
        const auto name = chordName (tones);
        bool inKey = true;
        for (int pc : pcs)
            inKey = inKey && degree (pc) >= 0;
        log << juce::String::formatted ("  %7.3f s  %-4s %s  %s%s\n", t, noteNameOf (live).toRawUTF8(), name.c_str(), d < 0 ? "OUT OF KEY (played)" : pcs == want ? "diatonic" : "WRONG",
                                        inKey ? "" : " [tones outside the key]");
        if (d < 0 || pcs != want)
        {
            Event e;
            e.kind = d < 0 ? "harmonize-out-of-key" : "harmonize-wrong";
            e.t0 = e.t1 = t;
            e.size = live;
            e.text = juce::String::formatted ("%s played: Harmonize sounds %s%s", noteNameOf (live).toRawUTF8(), name.c_str(),
                                              d < 0 ? " (the played note isn't in the key; it borrows the scale note below's intervals)" : " (not the key's triad)");
            ev.push_back (e);
        }
    }
    juce::ignoreUnused (since);
    return ev;
}

// Level: the output (the setting's own wave) rising 9 dB or more within 10 ms with the input not rising 6 dB (at input time), or
// falling 12 dB with the guitar still playing the same note (input down less than 6 dB).
std::vector<Event> levelJumps (const Analysis& a, const Level& in, const Truth& tr)
{
    // the engine's own level (gate x level, the probes every 16 samples, input time), in dB: phase-proof at any octave
    std::vector<Event> ev;
    auto lv = [&] (size_t k) { return (float) toDb (a.lite[k].gate * a.lite[k].level + 1e-9f); };
    float peak = -200.0f;
    for (size_t k = 0; k < a.lite.size(); ++k)
        peak = std::max (peak, lv (k));
    double last = -1.0;
    const size_t d = 30; // 10 ms
    for (size_t k = d; k < a.lite.size(); k += 3)
    {
        const double t = (double) k * 16.0;
        const float now = lv (k), before = lv (k - d), dOut = now - before;
        // the input's largest rise into t over the last 30 ms (a pick explains a jump: the new note's level arrives with its pitch)
        float lowest = in.at (t);
        for (double u = t - 1440.0; u < t; u += 48.0)
            lowest = std::min (lowest, in.at (u));
        const float dIn = in.at (t) - lowest;
        if (before < peak - 35.0f || a.lite[k - d].gate < 0.99f)
            continue; // from silence or in the gate's fades: that's latency, not a jump
        const bool rise = dOut >= 9.0f && dIn < 3.0f;
        const int h0 = tr.at (t), h1 = tr.at (t - 480.0);
        const bool fall = dOut <= -12.0f && dIn > -6.0f && h0 >= 0 && h1 >= 0 && tr.ok[(size_t) h0] && tr.ok[(size_t) h1]
                          && std::lround (tr.hops[(size_t) h0].midi) == std::lround (tr.hops[(size_t) h1].midi);
        if ((rise || fall) && t / kFs - last > 0.05)
        {
            Event e;
            e.kind = rise ? "level-jump" : "level-dip";
            e.t0 = e.t1 = t / kFs;
            e.size = dOut;
            e.text = juce::String::formatted ("the voice's level %s %.1f dB in 10 ms (from %.0f dB re. its peak); the input rose %.1f dB at most in the 30 ms before", rise ? "rises" : "falls",
                                              std::abs (dOut), before - peak, dIn);
            ev.push_back (e);
            last = t / kFs;
        }
    }
    return ev;
}

// Latency, per pick: the input's rise (its 1 ms envelope reaching 20 dB under its 100 ms max) to the output's (the Sine twin), and
// to the first cycle within 50 c of the note.
struct Delay
{
    double t, sound, pitch;
    bool fromSilence;
    int note;
    double open = -1.0; // from silence: the voice gate's first open sample after the input's rise, at output time (+ L), ms
};
std::vector<Delay> delaysFor (const Analysis& a, const std::vector<float>& input, const std::vector<blips::Pick>& ps)
{
    std::vector<Delay> d;
    auto env1 = [] (const std::vector<float>& x) {
        std::vector<float> e (x.size() / 48);
        for (size_t k = 0; k < e.size(); ++k)
        {
            double s = 0.0;
            for (size_t i = k * 48; i < (k + 1) * 48; ++i)
                s += (double) x[i] * x[i];
            e[k] = (float) std::sqrt (s / 48.0);
        }
        return e;
    };
    const auto ei = env1 (input), eo = env1 (a.sine);
    for (const auto& p : ps)
    {
        if (p.note < 0)
            continue;
        const auto k0 = (size_t) std::max<int64_t> (0, p.t / 48 - 5);
        const auto k1 = std::min (ei.size(), (size_t) ((p.end) / 48));
        if (k1 <= k0 + 20)
            continue;
        float mi = 0.0f;
        for (size_t k = k0; k < std::min (k1, k0 + 100); ++k)
            mi = std::max (mi, ei[k]);
        size_t ri = k0;
        while (ri < k1 && ei[ri] < mi * 0.1f)
            ++ri;
        // the output: from the input's rise, at output time
        const size_t o0 = ri + (size_t) (a.latency / 48) - 3;
        float mo = 0.0f;
        for (size_t k = o0; k < std::min (eo.size(), o0 + 150); ++k)
            mo = std::max (mo, eo[k]);
        if (mo <= 0.0f)
            continue;
        // the output's rise: its first frame above 20 dB under its max, after being under it (an open voice: the level's own rise)
        size_t ro = o0;
        const bool openBefore = eo[std::min (eo.size() - 1, o0)] >= mo * 0.1f;
        if (! openBefore)
            while (ro < eo.size() && eo[ro] < mo * 0.1f)
                ++ro;
        double pitchAt = -1.0;
        for (const auto& c : a.cyc)
            if (c.t >= (double) ri * 48.0 && c.t <= (double) p.end && std::abs (c.midi - p.note) <= 0.5 && c.amp > mo * 0.1f)
            {
                pitchAt = c.t + a.latency;
                break;
            }
        double open = -1.0;
        if (! openBefore)
            for (size_t k = (size_t) (ri * 48 / 16 > 30 ? ri * 48 / 16 - 30 : 0); k < a.lite.size() && (double) k * 16.0 < (double) p.end; ++k)
                if (a.lite[k].gate > 0.0f && (k == 0 || a.lite[k - 1].gate <= 0.0f))
                {
                    open = ((double) k * 16.0 + a.latency - (double) ri * 48.0) / 48.0;
                    break;
                }
        d.push_back ({ p.t / kFs, openBefore ? -1.0 : ((double) ro - (double) ri), pitchAt < 0 ? -1.0 : (pitchAt - (double) ri * 48.0) / 48.0, ! openBefore, p.note, open });
    }
    return d;
}

struct Heard;
Heard heard (int t);
std::vector<float> renderSwitched (const std::vector<float>& in, const EngineParams& a, const EngineParams& b, double atS);
EngineParams heardAfterSwitch();

// --- The sweep -------------------------------------------------------------------------------------------------------------------
struct Result
{
    juce::String setting;
    EngineParams p;
    std::vector<Event> ev;
    blips::Score blips;
    std::vector<Delay> delays;
    float minHeadroomDb = 0.0f;
    double headroomSeconds = 0.0;
    juce::String freezeLog, harmLog;
    std::vector<float> out; // kept for the clips (noted and as-heard only)
    int latency = 0;
};

Result evaluate (const std::vector<float>& in, const Truth& tr, const Level& lv, const std::vector<blips::Pick>& ps, const juce::String& name,
                 const EngineParams& p, bool keep)
{
    Result r;
    r.setting = name;
    r.p = p;
    const auto a = analyse (in, p);
    auto add = [&] (std::vector<Event> v) { r.ev.insert (r.ev.end(), v.begin(), v.end()); };
    add (wrongNotes (a, tr));
    add (pickBlips (a, tr, ps, &r.blips));
    add (dropouts (a, tr, lv));
    add (hangs (a, lv));
    add (frozenGlides (a));
    add (clicks (a));
    add (glideIns (tr, ps, r.blips));
    add (belowZone (a, tr, lv));
    add (levelJumps (a, lv, tr));
    if (p.chordMode == ChordMode::Memory)
        add (freezeEvents (a, tr, r.freezeLog));
    if (p.chordMode == ChordMode::Stack)
        add (harmonizeEvents (a, r.harmLog));
    r.delays = delaysFor (a, in, ps);
    r.minHeadroomDb = toDb (a.minHeadroom);
    r.headroomSeconds = a.headroomSeconds;
    r.latency = a.latency;
    if (keep)
        r.out = a.out;
    return r;
}

int count (const Result& r, const juce::String& kind, double* ms = nullptr)
{
    int n = 0;
    double total = 0.0;
    for (const auto& e : r.ev)
        if (e.kind == kind)
        {
            ++n;
            total += e.ms;
        }
    if (ms != nullptr)
        *ms = total;
    return n;
}

juce::String median (std::vector<double> v, const char* fmt = "%.1f")
{
    v.erase (std::remove_if (v.begin(), v.end(), [] (double x) { return x < 0.0; }), v.end());
    if (v.empty())
        return "-";
    std::sort (v.begin(), v.end());
    return juce::String::formatted (fmt, v[v.size() / 2]) + juce::String::formatted (" [%.1f .. %.1f]", v.front(), v.back());
}

// Clips: 2 s of the dry take, the wet bounce (aligned to it) and the render, each peak-normalised to -3 dBFS, in
// Recordings/RC1-QA/clips/.
void writeClip (const juce::File& f, const std::vector<float>& x, double centre)
{
    const auto a = (int64_t) ((centre - 1.0) * kFs), b = (int64_t) ((centre + 1.0) * kFs);
    std::vector<float> y;
    for (int64_t i = a; i < b; ++i)
        y.push_back (i >= 0 && i < (int64_t) x.size() ? x[(size_t) i] : 0.0f);
    float pk = 0.0f;
    for (float v : y)
        pk = std::max (pk, std::abs (v));
    if (pk > 0.0f)
        for (auto& v : y)
            v *= 0.708f / pk;
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
}


// A take whose wet part shifts mid-way (an edit in one export): the best offset of each 4 s stretch (envelopes at 1 ms over
// +-1 s, then the samples within 2 ms).
void probeOffsets()
{
    for (int t = 0; t < kNumTakes; ++t)
    {
        const auto& x = inferred()[(size_t) t];
        const auto r = golden::render (gained (golden::dry (t), x.gainDb), kFs, x.p);
        const auto& w = wet (t);
        juce::String line = juce::String (kTakes[t].id) + ":";
        for (double s0 = 0.0; s0 + 4.0 <= r.size() / kFs; s0 += 4.0)
        {
            std::vector<float> part (r.begin() + (long) (s0 * kFs), r.begin() + (long) ((s0 + 4.0) * kFs));
            std::vector<float> wpart;
            const int64_t a = (int64_t) ((s0 - 1.0) * kFs);
            for (int64_t i = a; i < a + (int64_t) (6.0 * kFs); ++i)
                wpart.push_back (i >= 0 && i < (int64_t) w.size() ? w[(size_t) i] : 0.0f);
            const auto al = align (part, wpart);
            line << juce::String::formatted (" %.0f s: %+lld (%.3f)", s0, (long long) (al.offset - kFs), al.sampleCorr);
        }
        info (line);
    }
}
// A close look at a spot: 10 ms frames of the dry take, the render (as heard) and the wet, in dB, and their local correlation.
void probeSpot()
{
    const auto spec = juce::SystemStats::getEnvironmentVariable ("GTT_SPOT", "flatpick 27.4 29.0");
    const auto parts = juce::StringArray::fromTokens (spec, " ", "");
    int t = 0;
    for (int k = 0; k < kNumTakes; ++k)
        if (parts[0] == kTakes[k].id)
            t = k;
    const double from = parts[1].getDoubleValue(), to = parts[2].getDoubleValue();
    const auto& x = inferred()[(size_t) t];
    const auto in = gained (golden::dry (t), x.gainDb);
    const auto r = golden::render (in, kFs, x.p);
    const auto& w = wet (t);
    for (double s = from; s < to; s += 0.01)
    {
        const auto a = (size_t) (s * kFs), b = a + 480;
        double ab = 0, aa = 0, bb = 0, dd = 0;
        for (size_t n = a; n < b; ++n)
        {
            ab += (double) r[n] * w[n];
            aa += (double) r[n] * r[n];
            bb += (double) w[n] * w[n];
            dd += (double) in[n] * in[n];
        }
        info (juce::String::formatted ("%.2f s  dry %6.1f  render %6.1f  wet %6.1f dB  corr %6.3f", s, toDb ((float) std::sqrt (dd / 480)), toDb ((float) std::sqrt (aa / 480)),
                                       toDb ((float) std::sqrt (bb / 480)), ab / std::sqrt (aa * bb + 1e-30)));
    }
}
// The input gain, finely, over a stretch: GTT_GAIN="take from to lo hi step".
void probeGain()
{
    const auto parts = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("GTT_GAIN", "flatpick 20 50 -3 0 0.1"), " ", "");
    int t = 0;
    for (int k = 0; k < kNumTakes; ++k)
        if (parts[0] == kTakes[k].id)
            t = k;
    const double s0 = parts[1].getDoubleValue(), s1 = parts[2].getDoubleValue(), lo = parts[3].getDoubleValue(), hi = parts[4].getDoubleValue(),
                 step = parts[5].getDoubleValue();
    const auto& x = inferred()[(size_t) t];
    std::vector<double> gs;
    for (double g = lo; g <= hi + 1e-9; g += step)
        gs.push_back (g);
    std::vector<double> c (gs.size());
    sputter::parallelFor ((int) gs.size(), [&] (int i) {
        const auto r = golden::render (gained (golden::dry (t), gs[(size_t) i]), kFs, x.p);
        c[(size_t) i] = corrAt (r, wet (t), 0, nullptr, 2, s0, s1, 2);
        if (parts.size() >= 8) // and the render's level over [a, b] (dB re. its own peak)
            c[(size_t) i] = rmsDb (r, (size_t) (parts[6].getDoubleValue() * kFs), (size_t) (parts[7].getDoubleValue() * kFs)) - peakDb (r);
    });
    juce::String line;
    for (size_t i = 0; i < gs.size(); ++i)
        line << juce::String::formatted ("%+.2f: %.4f  ", gs[i], c[i]);
    info (line);
}
// What Duncan heard, as the probes above found it (probe-rc1-settings, -segments, -gain): the settings and the input gain (the gain
// that entered the plugin, re. the dry export's level) that reproduce each wet take sample for sample (correlation 1.0000 over the
// whole take; the harmonize take over its first 10 s, after which its wave changes: heardWaves()).
struct Heard
{
    EngineParams p;
    double gainDb;
    juce::String note;
};
Heard heard (int t)
{
    const juce::String id = kTakes[t].id;
    EngineParams p;
    p.octave = Octave::Sub;
    if (id == "flatpick")
        return { (p.wave = Wave::Pulse25, p), -1.80, "plateau -1.80 dB only" };
    if (id == "fingerpick")
        return { p, -0.90, "plateau -0.90 dB only" };
    if (id == "bass")
        return { (p.octave = Octave::TripleSub, p), -3.2, "plateau -3.6 .. -2.8 dB" };
    if (id == "below")
        return { p, -8.75, "plateau -9.2 .. -8.3 dB" };
    if (id == "freeze3")
    {
        p = golden::freeze (4);
        p.wave = Wave::Pulse25;
        p.octave = Octave::Sub;
        return { p, 0.05, "plateau -0.2 .. +0.3 dB" };
    }
    if (id == "harmonics")
    {
        p = golden::freeze (4);
        p.wave = Wave::Pulse25;
        p.octave = Octave::Sub;
        return { p, 0.3, "plateau -0.3 .. +0.9 dB" };
    }
    p = golden::harmonize (11);
    p.wave = Wave::Pulse25;
    p.octave = Octave::DoubleSub;
    return { p, -1.7, "plateau -2.0 .. -1.4 dB (first 10 s)" };
}

// The harmonize take's wave, segment by segment (2 s): Octave -2, Harmonize B, the gain above, each wave's spectra against the wet.
void probeHeardWaves()
{
    const int t = kNumTakes - 1;
    const auto h = heard (t);
    const auto in = gained (golden::dry (t), h.gainDb);
    const auto sw = spectra (wet (t));
    std::vector<EngineParams> cands;
    for (Wave wv : { Wave::Saw, Wave::Square, Wave::Pulse25, Wave::Triangle, Wave::Sine, Wave::HalfSine, Wave::Organ, Wave::Folded })
        for (int o = 0; o < 4; ++o)
        {
            auto p = h.p;
            p.wave = wv;
            p.octave = (Octave) o;
            cands.push_back (p);
        }
    std::vector<std::vector<float>> outs (cands.size());
    std::vector<Spectra> sp (cands.size());
    sputter::parallelFor ((int) cands.size(), [&] (int i) {
        outs[(size_t) i] = golden::render (in, kFs, cands[(size_t) i]);
        sp[(size_t) i] = spectra (outs[(size_t) i]);
    });
    const size_t seg = (size_t) (1.0 * kFs / Spectra::kHop);
    for (size_t g = 0; g * seg < sw.frames.size(); ++g)
    {
        std::vector<std::pair<double, size_t>> sc;
        for (size_t i = 0; i < cands.size(); ++i)
            sc.push_back ({ spectralMatch (sp[i], sw, g * seg, (g + 1) * seg), i });
        std::sort (sc.rbegin(), sc.rend());
        // and the sample correlation of the best three over the segment
        juce::String line = juce::String::formatted ("%4.0f s:", (double) g * seg * Spectra::kHop / kFs);
        for (int r = 0; r < 3; ++r)
        {
            const auto& p = cands[sc[(size_t) r].second];
            const double c = corrAt (outs[sc[(size_t) r].second], wet (t), 0, nullptr, 2, (double) g * seg * Spectra::kHop / kFs,
                                     (double) (g + 1) * seg * Spectra::kHop / kFs, 0);
            line << juce::String::formatted ("  %s %d: spec %.3f corr %.3f |", golden::waveName (p.wave), -(int) p.octave, sc[(size_t) r].first, c);
        }
        info (line);
    }
}
// A render whose settings change once, at a block boundary (the harmonize take: its wave changes in the silence at 11 s).
std::vector<float> renderSwitched (const std::vector<float>& in, const EngineParams& a, const EngineParams& b, double atS)
{
    Engine e;
    e.prepare (kFs, 64);
    std::vector<float> out (in.size());
    for (size_t start = 0; start < in.size(); start += 64)
    {
        const int n = (int) std::min ((size_t) 64, in.size() - start);
        e.process (in.data() + start, out.data() + start, n, (double) start < atS * kFs ? a : b);
    }
    return out;
}
EngineParams heardAfterSwitch()
{
    auto p = heard (kNumTakes - 1).p;
    p.wave = Wave::Folded;
    return p;
}
constexpr double kHarmonizeSwitchS = 11.0;

void probeHeardSwitch()
{
    const int t = kNumTakes - 1;
    const auto h = heard (t);
    const auto in = gained (golden::dry (t), h.gainDb);
    for (double at : { 10.95, 11.0, 11.05, 11.1 })
    {
        const auto r = renderSwitched (in, h.p, heardAfterSwitch(), at);
        info (juce::String::formatted ("switch to Folded at %.2f s: corr 0.5 .. 11 s %.4f, 11 .. 57 s %.4f", at, corrAt (r, wet (t), 0, nullptr, 1, 0.5, 11.0, 0),
                                       corrAt (r, wet (t), 0, nullptr, 1, 11.0, 57.0, 0)));
    }
    // and the gain once more, for the whole take with the switch
    juce::String line;
    for (double g = -2.0; g <= -1.4 + 1e-9; g += 0.1)
        line << juce::String::formatted ("%+.1f: %.4f  ", g, corrAt (renderSwitched (gained (golden::dry (t), g), h.p, heardAfterSwitch(), kHarmonizeSwitchS), wet (t), 0, nullptr, 2, 0.5, 57.0, 0));
    info (line);
}
// What else changed at the switch: a coordinate search on the settings after it, scored over 11.5 .. 57 s.
void probeHeardAfter()
{
    const int t = kNumTakes - 1;
    const auto h = heard (t);
    const auto in = gained (golden::dry (t), h.gainDb);
    auto best = heardAfterSwitch();
    auto score = [&] (const EngineParams& b) { return corrAt (renderSwitched (in, h.p, b, kHarmonizeSwitchS), wet (t), 0, nullptr, 4, 11.5, 57.0, 0); };
    double bestC = score (best);
    auto tryAll = [&] (const char* what, std::vector<EngineParams> c) {
        std::vector<double> sc (c.size());
        sputter::parallelFor ((int) c.size(), [&] (int i) { sc[(size_t) i] = score (c[(size_t) i]); });
        juce::String line = juce::String (what) + ":";
        for (double v : sc)
            line << juce::String::formatted (" %.3f", v);
        info (line);
        const auto k = (size_t) (std::max_element (sc.begin(), sc.end()) - sc.begin());
        if (sc[k] > bestC + 1e-4)
        {
            bestC = sc[k];
            best = c[k];
        }
    };
    for (int round = 0; round < 2; ++round)
    {
        std::vector<EngineParams> c;
        for (Wave w : { Wave::Saw, Wave::Square, Wave::Pulse25, Wave::Triangle, Wave::Sine, Wave::HalfSine, Wave::Organ, Wave::Folded })
            for (int o = 0; o < 4; ++o)
            {
                auto p = best;
                p.wave = w;
                p.octave = (Octave) o;
                c.push_back (p);
            }
        tryAll ("wave x octave", c);
        c.clear();
        for (float sus : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
            c.push_back ((best.sustainPercent = sus, best));
        tryAll ("sustain 0 25 50 75 100", c);
        c.clear();
        for (int r : { 0, 1, 2 })
        {
            auto p = best;
            p.retuneSpeed = (RetuneSpeed) r;
            c.push_back (p);
        }
        tryAll ("retune", c);
        c.clear();
        for (int r : { 0, 1, 2 })
        {
            auto p = best;
            p.holdMs = noteReleaseMsFor ((NoteRelease) r);
            c.push_back (p);
        }
        tryAll ("release", c);
        c.clear();
        for (int k = 0; k < kLatencyDetents; ++k)
        {
            auto p = best;
            p.windowHz = latencyDetentHz (k);
            c.push_back (p);
        }
        tryAll ("latency detents", c);
        c.clear();
        for (int k = 0; k < kChordChoices; ++k)
        {
            auto p = best;
            setChordChoice (p, k);
            c.push_back (p);
        }
        tryAll ("chord choices", c);
    }
    info (juce::String::formatted ("after the switch: corr %.4f, ", bestC) + paramsText (best, h.gainDb) + ", " + chordText (best));
}
// The wet takes against the as-heard renders, sample by sample: the correlation, the residual (after the best gain) re. the
// signal, and the wet's gain re. the render (the bounce's normalisation).
void probeHeard()
{
    for (int t = 0; t < kNumTakes; ++t)
    {
        const auto h = heard (t);
        const auto in = gained (golden::dry (t), h.gainDb);
        const bool harm = juce::String (kTakes[t].id) == "harmonize";
        const auto r = harm ? renderSwitched (in, h.p, heardAfterSwitch(), kHarmonizeSwitchS) : golden::render (in, kFs, h.p);
        const auto& w = wet (t);
        auto stats = [&] (double s0, double s1) {
            const auto a = (size_t) (s0 * kFs), b = std::min ({ r.size(), w.size(), (size_t) (s1 * kFs) });
            double rw = 0, rr = 0, ww = 0;
            for (size_t n = a; n < b; ++n)
            {
                rw += (double) r[n] * w[n];
                rr += (double) r[n] * r[n];
                ww += (double) w[n] * w[n];
            }
            const double g = rw / (rr + 1e-30); // the least-squares gain of the render onto the wet
            double e = 0, peak = 0;
            for (size_t n = a; n < b; ++n)
            {
                const double d = w[n] - g * r[n];
                e += d * d;
                peak = std::max (peak, std::abs (d));
            }
            return juce::String::formatted ("%.1f .. %.1f s: corr %.6f, residual %.1f dB re. the wet (largest sample %.1e), wet = render %+.2f dB", s0, b / kFs,
                                            rw / std::sqrt (rr * ww + 1e-30), toDb ((float) std::sqrt (e / (ww + 1e-30))), peak, toDb ((float) g));
        };
        info (juce::String (kTakes[t].id) + " (" + paramsText (h.p, h.gainDb) + ", " + chordText (h.p) + "; " + h.note + "): " + stats (0.0, 1.0e9));
        if (harm)
        {
            info ("    " + stats (0.0, kHarmonizeSwitchS));
            info ("    " + stats (kHarmonizeSwitchS, 1.0e9) + " (Folded from 11 s)");
        }
    }
}

// The clips docs/reports/QA-REPORT-RC1.md refers to: issue number, a short name, the spot (dry time, s), and whether the render is the as-heard
// one (the wet's settings and input gain) or the noted settings.
struct Clip
{
    int issue;
    const char* name;
    const char* take;
    double at;
    bool heard;
};
std::vector<Clip> clipsFor (const juce::String& take)
{
    static const Clip all[] = {
        { 2, "below-revive", "below", 9.36, true },      { 2, "below-revive", "below", 8.40, true },     { 2, "below-revive", "below", 3.55, true },
        { 1, "bottom-edge", "below", 7.41, true },       { 1, "bottom-edge", "fingerpick", 25.26, true }, { 1, "bottom-edge", "below", 10.66, true },
        { 4, "stale-glide", "flatpick", 38.23, true },   { 4, "stale-glide", "fingerpick", 17.96, true }, { 4, "stale-glide", "harmonize", 34.01, true },
        { 4, "stale-glide", "fingerpick", 29.84, true }, { 5, "damp-fifth", "bass", 4.40, true },         { 3, "damp-junk", "flatpick", 6.78, true },
        { 3, "damp-junk", "flatpick", 7.86, true },      { 6, "latency-from-silence", "flatpick", 1.81, true },
        { 7, "ghost-touch", "flatpick", 28.50, true },   { 8, "out-of-tune-flip", "freeze3", 16.65, true }, { 11, "harmonic-flash", "harmonics", 6.75, true },
        { 11, "harmonic-flash", "harmonics", 11.40, true }, { 9, "harmonize-chromatic", "harmonize", 7.40, true },
        { 9, "harmonize-chromatic", "harmonize", 52.50, false }, { 10, "below-octave-up", "below", 2.95, true }, { 10, "below-octave-up", "below", 8.10, true },
        { 12, "repick-tick", "fingerpick", 1.345, true },
    };
    std::vector<Clip> v;
    for (const auto& c : all)
        if (take == c.take)
            v.push_back (c);
    return v;
}

void probeQA()
{
    const auto dir = outDir.getChildFile ("rc1");
    dir.createDirectory();
    const auto clipDir = golden::repoDir().getChildFile ("Recordings").getChildFile ("RC1-QA").getChildFile ("clips");
    clipDir.createDirectory();
    for (int t = 0; t < kNumTakes; ++t)
        golden::dry (t), wet (t);

    for (int t = 0; t < kNumTakes; ++t)
    {
        const auto& in = golden::dry (t);
        const auto tr = truthFor (in);
        const auto lv = levelOf (in);
        const auto ps = blips::takePicks (in);
        auto settings = golden::settingsFor (t);
        // as Duncan heard it: the settings and input gain that reproduce the wet take (heard())
        const auto h = heard (t);
        settings.push_back ({ "as heard: " + paramsText (h.p, h.gainDb) + ", " + chordText (h.p), h.p });
        std::vector<Result> res (settings.size());
        const auto heardIn = gained (in, h.gainDb);
        const auto heardTruth = truthFor (heardIn);
        const auto heardLv = levelOf (heardIn);
        sputter::parallelFor ((int) settings.size(), [&] (int i) {
            const bool heard = i == (int) settings.size() - 1;
            res[(size_t) i] = evaluate (heard ? heardIn : in, heard ? heardTruth : tr, heard ? heardLv : lv, ps, settings[(size_t) i].name,
                                        settings[(size_t) i].p, i == 0 || heard);
        });

        juce::String out;
        out << "# " << kTakes[t].id << " (" << kTakes[t].file << ")\n";
        int voicedHops = 0;
        for (auto c : tr.ok)
            voicedHops += c;
        out << juce::String::formatted ("%.1f s, %d picks (%d with a note), the guitar pitched %.1f s (measuring detector, 0.9+), playing level %.1f dBFS (10 ms RMS, 95th pct)\n\n",
                                        in.size() / kFs, (int) ps.size(), (int) std::count_if (ps.begin(), ps.end(), [] (const blips::Pick& p) { return p.note >= 0; }),
                                        voicedHops * 128.0 / kFs, lv.ref);
        out << "## Across the settings set\n\n";
        out << "| setting | wrong (ms) | junk (ms) | pick blips (ms) | <3 / 3-4 / 5+ st | dropouts (ms) | hangs | frozen glides (ms) | clicks | stale / pick glides | below: junk | jumps / dips | headroom min dB (s < -0.5) | gate opens ms | first sound (-20 dB) ms | first pitch ms |\n";
        out << "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
        for (const auto& r : res)
        {
            double wms, jms, dms;
            const int w = count (r, "wrong", &wms), j = count (r, "junk", &jms), dr = count (r, "dropout", &dms);
            std::vector<double> snd, pit, opn;
            for (const auto& d : r.delays)
            {
                if (d.fromSilence)
                {
                    snd.push_back (d.sound);
                    opn.push_back (d.open);
                }
                pit.push_back (d.pitch);
            }
            out << "| " << r.setting << " | " << juce::String::formatted ("%d (%.0f)", w, wms) << " | " << juce::String::formatted ("%d (%.0f)", j, jms)
                << " | " << [&] { double a = 0, b = 0, c = 0; const int n = count (r, "blip-under-3st", &a) + count (r, "blip-3-4st", &b) + count (r, "blip-5st+", &c); return juce::String::formatted ("%d (%.0f)", n, a + b + c); }() << " | " << count (r, "blip-under-3st") << " / " << count (r, "blip-3-4st") << " / " << count (r, "blip-5st+") << " | "
                << juce::String::formatted ("%d (%.0f)", dr, dms) << " | " << count (r, "hang") << " | " << [&] { double m = 0; const int n = count (r, "frozen-glide", &m); return juce::String::formatted ("%d (%.0f)", n, m); }() << " | " << count (r, "click") << " | "
                << count (r, "stale-glide") << " / " << count (r, "glide-in") << " | " << count (r, "below-junk") << " of " << count (r, "below-junk") + count (r, "below")
                << " | " << count (r, "level-jump") << " / " << count (r, "level-dip") << " | "
                << juce::String::formatted ("%.1f (%.1f)", r.minHeadroomDb, r.headroomSeconds) << " | " << median (opn) << " | " << median (snd) << " | " << median (pit) << " |\n";
        }
        for (size_t which : { (size_t) 0, settings.size() - 1 })
        {
            const auto& r = res[which];
            out << "\n## Events at " << r.setting << "\n\n";
            auto sorted = r.ev;
            std::sort (sorted.begin(), sorted.end(), [] (const Event& a, const Event& b) { return a.t0 < b.t0; });
            for (const auto& e : sorted)
                out << juce::String::formatted ("- %8.3f s  %-26s ", e.t0, e.kind.toRawUTF8()) << e.text << "\n";
            if (r.freezeLog.isNotEmpty())
                out << "\n### Freeze timeline\n\n```\n" << r.freezeLog << "```\n";
            if (r.harmLog.isNotEmpty())
                out << "\n### Harmonize timeline\n\n```\n" << r.harmLog << "```\n";
            out << "\n### Latency per pick (ms from the input's rise; sound only for picks from silence)\n\n```\n";
            for (const auto& d : r.delays)
                out << juce::String::formatted ("  %7.3f s %-4s open %s  sound %s  pitch %s\n", d.t, noteNameOf (d.note).toRawUTF8(),
                                                d.open < 0 ? "  -  " : juce::String::formatted ("%5.1f", d.open).toRawUTF8(),
                                                d.sound < 0 ? "  -  " : juce::String::formatted ("%5.1f", d.sound).toRawUTF8(),
                                                d.pitch < 0 ? "  -  " : juce::String::formatted ("%5.1f", d.pitch).toRawUTF8());
            out << "```\n";
        }
        dir.getChildFile (juce::String (kTakes[t].id) + ".md").replaceWithText (out);
        std::printf ("%s\n", out.upToFirstOccurrenceOf ("## Events", false, false).toRawUTF8());

        // Clips: the curated list (clipsFor), dry / wet / render around each spot.
        const auto w = shifted (wet (t), 0, in.size()); // the exports start together (every match is at offset 0)
        if (juce::String (kTakes[t].id) == "harmonize") // its wave changed at 11 s: the clips' render does too
            res.back().out = renderSwitched (heardIn, h.p, heardAfterSwitch(), kHarmonizeSwitchS);
        for (const auto& c : clipsFor (kTakes[t].id))
        {
            const auto& r = c.heard ? res.back() : res[0];
            const auto base = juce::String::formatted ("%02d-%s-%s-%06.2f", c.issue, c.name, kTakes[t].id, c.at);
            writeClip (clipDir.getChildFile (base + "-dry.wav"), c.heard ? heardIn : in, c.at);
            writeClip (clipDir.getChildFile (base + "-wet.wav"), w, c.at + r.latency / kFs);
            writeClip (clipDir.getChildFile (base + "-render.wav"), r.out, c.at + r.latency / kFs);
        }
    }
}
// The per-hop timeline of a spot, at the as-heard settings (GTT_SPOT="take from to"): the guitar (the measuring detector), the
// engine's reading (raw, confidence, voiced, the gates' flags), its note N and target T, the event, the input's and the voice's level.
void probeTimeline()
{
    const auto parts = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("GTT_SPOT", "flatpick 27.2 28.0"), " ", "");
    int t = 0;
    for (int k = 0; k < kNumTakes; ++k)
        if (parts[0] == kTakes[k].id)
            t = k;
    const double from = parts[1].getDoubleValue(), to = parts[2].getDoubleValue();
    const auto h = heard (t);
    const auto in = gained (golden::dry (t), h.gainDb);
    auto p = h.p;
    if (parts.size() >= 4 && parts[3] == "noted")
        p = golden::notedParams (t);
    if (const auto w = juce::SystemStats::getEnvironmentVariable ("GTT_WINDOW", "").getDoubleValue(); w > 0.0) // RC2: another window (Hz)
        p.windowHz = (float) w;
    if (juce::SystemStats::getEnvironmentVariable ("GTT_V061", "0") == "1") // RC2: v0.6.1 (every v0.6.2 fix off)
        engineSetup = [] (Engine& e) { e.setV062Fixes (false); };
    const auto a = analyse (in, p);
    engineSetup = {};
    const auto tr = truthFor (in);
    const auto lv = levelOf (in);
    info (juce::String (kTakes[t].id) + ": " + paramsText (p, h.gainDb) + ", " + chordText (p) + juce::String::formatted (", L = %d", a.latency));
    info ("   time s | guitar (conf) | engine raw (conf) V M lock | N    T       ev    | in dB  voice dB gate | onset");
    for (const auto& x : a.hops)
    {
        const double ts = (double) x.time / kFs;
        if (ts < from || ts > to)
            continue;
        const int k = tr.at ((double) x.time);
        const auto li = (size_t) std::max<int64_t> (0, x.time) / 16;
        const auto& l = li < a.lite.size() ? a.lite[li] : Lite {};
        info (juce::String::formatted ("  %7.3f | %-8s %4.2f%s | %-9s %4.2f %c %c %c | %-4s %6.2f %-6s | %5.1f %6.1f %4.2f | %s", ts,
                                       k >= 0 && tr.hops[(size_t) k].detector.voiced ? hz (tr.hops[(size_t) k].midi).toRawUTF8() : "-",
                                       k >= 0 ? (double) tr.hops[(size_t) k].detector.confidence : 0.0, k >= 0 && tr.ok[(size_t) k] ? "*" : " ",
                                       x.rawMidi > 0 ? hz (x.rawMidi).toRawUTF8() : "-", (double) x.detector.confidence, x.detector.voiced ? 'V' : '.',
                                       x.muted ? 'M' : '.', x.lockHeld ? 'L' : '.', x.hasNote ? noteNameOf (x.noteN).toRawUTF8() : "-", x.target,
                                       blips::eventName (x.event), (double) lv.at ((double) x.time), (double) toDb (l.gate * l.level + 1e-9f), (double) l.gate,
                                       x.onset ? "ONSET" : ""));
    }
}
// Sample by sample around a spot (GTT_SPOT="take t [noted]", input time): the voice's level, gate, the Sine render's sample, its
// 2nd difference.
void probeSamples()
{
    const auto parts = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("GTT_SPOT", "fingerpick 1.345"), " ", "");
    int t = 0;
    for (int k = 0; k < kNumTakes; ++k)
        if (parts[0] == kTakes[k].id)
            t = k;
    const double at = parts[1].getDoubleValue();
    const auto h = heard (t);
    const bool noted = parts.size() >= 3 && parts[2] == "noted";
    auto p = noted ? golden::notedParams (t) : h.p;
    p.wave = Wave::Sine;
    p.octave = Octave::Main;
    const auto in = noted ? golden::dry (t) : gained (golden::dry (t), h.gainDb); // RC2: noted = the file's level, as the analysis
    std::vector<VoiceProbe> pr;
    Engine e;
    e.prepare (kFs, 64);
    e.voiceListener = [&] (const VoiceProbe& v) { pr.push_back (v); };
    const auto y = runEngine (e, in, p);
    const int L = e.currentLatencySamples();
    const auto c = (int64_t) (at * kFs);
    const int stride = juce::SystemStats::getEnvironmentVariable ("GTT_STRIDE", "4").getIntValue();
    for (int64_t n = c - 240; n < c + 240; n += stride)
    {
        const auto o = (size_t) (n + L);
        if (o < 2 || o >= y.size())
            continue;
        const auto& v = pr[o];
        info (juce::String::formatted ("%.5f s  level %8.5f  gate %5.3f  env %8.5f ref %8.5f  dyn %6.3f  onset %d  y %+9.5f  d2 %+9.5f", (double) n / kFs, (double) v.level,
                                       (double) v.gate, (double) v.env, (double) v.ref, (double) v.dynamicsGain, (int) v.onset, (double) y[o],
                                       (double) (y[o] - 2.0f * y[o - 1] + y[o - 2])));
    }
}
// The guitar's tuning on each take: the measuring detector's sure hops, cents off the nearest note (median, spread), and how much
// of the playing sits within 15 c of the half-semitone line (where the quantizer's choice depends on the hysteresis).
void probeTuning()
{
    for (int t = 0; t < kNumTakes; ++t)
    {
        const auto tr = truthFor (golden::dry (t));
        std::vector<double> c;
        std::map<int, std::vector<double>> byString; // by note, for the open strings' notes (E A D G B E)
        for (size_t k = 0; k < tr.hops.size(); ++k)
            if (tr.ok[k])
                c.push_back (100.0 * (tr.hops[k].midi - std::round (tr.hops[k].midi)));
        std::sort (c.begin(), c.end());
        int nearEdge = 0;
        for (double v : c)
            nearEdge += std::abs (v) >= 35.0 ? 1 : 0;
        auto q = [&] (double f) { return c[(size_t) (f * (double) (c.size() - 1))]; };
        info (juce::String::formatted ("%-10s %5d sure hops: cents re. the nearest note: median %+5.1f, 10%% %+5.1f, 90%% %+5.1f; %4.1f%% of the playing 35 c or more off (near the line)",
                                       kTakes[t].id, (int) c.size(), q (0.5), q (0.1), q (0.9), 100.0 * nearEdge / (double) c.size()));
        juce::ignoreUnused (byString);
    }
}
// The window's bottom edge: hops whose reading is the detector's longest lag (the window's own lowest frequency, as the top edge
// rule's 1400 Hz is its shortest), and how many the engine voices, at the defaults and the as-heard settings, per take.
void probeEdge()
{
    for (int t = 0; t < kNumTakes; ++t)
        for (int which = 0; which < 2; ++which)
        {
            const auto h = heard (t);
            const auto p = which == 0 ? EngineParams {} : h.p;
            const auto in = which == 0 ? golden::dry (t) : gained (golden::dry (t), h.gainDb);
            Engine e;
            e.prepare (kFs, 64);
            std::vector<HopInfo> hops;
            e.hopListener = [&] (const HopInfo& x) { hops.push_back (x); };
            runEngine (e, in, p);
            const double edgeHz = kFs / (double) (e.currentLatencySamples() / 2); // tauMax = L / 2
            int edge = 0, voiced = 0, changed = 0;
            juce::String where;
            int prevN = -1;
            for (const auto& x : hops)
            {
                const bool atEdge = x.rawMidi > 0 && std::abs (x.rawMidi - midiFromHz (edgeHz)) < 0.05;
                if (atEdge)
                {
                    ++edge;
                    if (x.detector.voiced)
                    {
                        ++voiced;
                        if (x.noteN != prevN)
                        {
                            ++changed;
                            if (where.length() < 200)
                                where << juce::String::formatted (" %.2f", x.time / kFs);
                        }
                    }
                }
                if (x.hasNote)
                    prevN = x.noteN;
            }
            info (juce::String::formatted ("%-10s %-9s edge %.1f Hz (%s): %d hops read at the edge, %d voiced, %d of them moved the note:", kTakes[t].id,
                                           which == 0 ? "defaults" : "as heard", edgeHz, hz (midiFromHz (edgeHz)).toRawUTF8(), edge, voiced, changed) + where);
        }
}
} // namespace rc1
