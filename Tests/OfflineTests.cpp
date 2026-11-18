// ShortOrder (internally gtt; GTT Lab up to v0.6) offline test harness.
//
// Runs synthetic signals through the DSP classes, writes WAVs and per-hop pitch
// CSVs to the output directory, and returns non-zero if any check fails.
//
//   GTTTests [outDir] [--only <name>] [--bench] [--golden | --golden-write] [--grid <file> | --grid-write <file>]

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "DSP/Engine.h"
#include "DSP/Oscillator.h"
#include "DSP/LowRange.h"
#include "Tab.h"
#include "ChordName.h"
#include "ChordSplit.h"
#include "Latency.h"
#include <set>
#include <deque>
#include <numeric>
#include <array>
#include <cstring>

#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <map>
#include <random>
#include <thread>
#include <atomic>

using namespace gtt;

namespace
{
constexpr double kFs = 48000.0;
juce::File outDir;

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------
void writeWav (const juce::String& name, const std::vector<float>& x, double fs = kFs)
{
    auto file = outDir.getChildFile (name + ".wav");
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
    if (stream == nullptr)
        return;
    auto options = juce::AudioFormatWriterOptions().withSampleRate (fs).withNumChannels (1).withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    if (writer == nullptr)
        return;
    const float* chans[1] = { x.data() };
    writer->writeFromFloatArrays (chans, 1, (int) x.size());
}

struct Csv
{
    explicit Csv (const juce::String& name, const juce::String& header)
    {
        auto file = outDir.getChildFile (name + ".csv");
        file.deleteFile();
        out = file.createOutputStream();
        if (out != nullptr)
            *out << header << "\n";
    }
    void row (const juce::String& line)
    {
        if (out != nullptr)
            *out << line << "\n";
    }
    std::unique_ptr<juce::FileOutputStream> out;
};

int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::printf ("    [%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok)
        ++failures;
}

void info (const juce::String& what) { std::printf ("    %s\n", what.toRawUTF8()); }

// ---------------------------------------------------------------------------
// Signal generators
// ---------------------------------------------------------------------------

// Band-limited-ish saw (PolyBLEP) with an arbitrary frequency trajectory.
std::vector<float> sawFromTrajectory (const std::function<double (double)>& hzAt, double seconds, float amp, double fs = kFs)
{
    std::vector<float> x ((size_t) (seconds * fs));
    double phase = 0.0;
    for (size_t n = 0; n < x.size(); ++n)
    {
        const double dt = hzAt ((double) n / fs) / fs;
        double v = 2.0 * phase - 1.0;
        // PolyBLEP
        if (phase < dt)
        {
            const double t = phase / dt;
            v -= t + t - t * t - 1.0;
        }
        else if (phase > 1.0 - dt)
        {
            const double t = (phase - 1.0) / dt;
            v -= t * t + t + t + 1.0;
        }
        x[n] = amp * (float) v;
        phase += dt;
        if (phase >= 1.0)
            phase -= 1.0;
    }
    return x;
}

std::vector<float> steadySaw (double hz, double seconds, float amp = 0.5f)
{
    return sawFromTrajectory ([hz] (double) { return hz; }, seconds, amp);
}

// Karplus-Strong pluck with a fractional (linear-interpolated) delay, so the
// pitch can be exact and can glide. Loop filter: two-point average (0.5 sample
// delay) times a loss factor; the fractional delay compensates for the 0.5.
std::vector<float> ksPluck (const std::function<double (double)>& hzAt, double seconds, float amp, float loss, uint32_t seed = 1,
                            double fs = kFs)
{
    // (v0.7: a negative length is an empty pluck. Converting a negative double to size_t is undefined: arm64 gives 0, x86_64 a huge
    // size, which threw in testGaps, whose phrases have a note lifted before it starts.)
    const size_t n = seconds > 0.0 ? (size_t) (seconds * fs) : 0;
    std::vector<float> y (n, 0.0f);
    std::vector<float> line (8192, 0.0f);
    const size_t mask = line.size() - 1;
    std::minstd_rand rng (seed);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);

    // Excite with a short lowpassed noise burst (one period of the start pitch).
    const int burst = (int) (fs / hzAt (0.0));
    float lp = 0.0f;
    size_t w = 0;
    float prev = 0.0f;
    for (size_t i = 0; i < n; ++i)
    {
        float exc = 0.0f;
        if ((int) i < burst)
        {
            lp += 0.5f * (dist (rng) - lp);
            exc = lp;
        }
        const double period = fs / hzAt ((double) i / fs);
        const double d = period - 0.5; // loop delay excluding the averager
        const double rp = (double) w - d;
        const double fl = std::floor (rp);
        const float frac = (float) (rp - fl);
        const float a = line[(size_t) (int64_t) fl & mask];
        const float b = line[(size_t) ((int64_t) fl + 1) & mask];
        const float delayed = a + frac * (b - a);
        const float fb = loss * 0.5f * (delayed + prev);
        prev = delayed;
        const float v = exc + fb;
        line[w & mask] = v;
        ++w;
        y[i] = v;
    }

    float peak = 0.0f;
    for (auto v : y)
        peak = std::max (peak, std::abs (v));
    if (peak > 0.0f)
        for (auto& v : y)
            v *= amp / peak;
    return y;
}

std::vector<float> whiteNoise (double seconds, float amp, uint32_t seed = 7)
{
    std::vector<float> x ((size_t) (seconds * kFs));
    std::minstd_rand rng (seed);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    for (auto& v : x)
        v = amp * dist (rng);
    return x;
}

std::vector<float> sine (double hz, double seconds, float amp)
{
    std::vector<float> x ((size_t) (seconds * kFs));
    for (size_t n = 0; n < x.size(); ++n)
        x[n] = amp * (float) std::sin (2.0 * M_PI * hz * (double) n / kFs);
    return x;
}

// Run an Engine over a whole signal in fixed-size blocks.
std::vector<float> runEngine (Engine& e, const std::vector<float>& in, const EngineParams& p, int block = 64)
{
    std::vector<float> out (in.size());
    for (size_t start = 0; start < in.size(); start += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, in.size() - start);
        e.process (in.data() + start, out.data() + start, n, p);
    }
    return out;
}

// v0.5.1's engine defaults: Retune Fast, Note Release 350 ms, Sustain 0, Dynamics 100% (bypassed), Voicing Top, Scale Major,
// Chord Level 70% (Glide was Legato too; Always is the only mode since v0.6.3). v0.5.2 removed Dynamics, Voicing, Scale and Chord
// Level as controls (fixed at 50%, Open, Major, 80%) and changed two defaults, but kept the code behind every setting, so the
// engine still runs these. The tests written up to v0.5.1 start from here, so their measurements at the old defaults still
// hold. Tests of v0.5.2 and later start from EngineParams {} (the plugin's defaults).
EngineParams v051Defaults()
{
    EngineParams p;
    p.sustainPercent = 0.0f; // the default up to v0.5.5 (75% since v0.5.6)
    p.retuneSpeed = RetuneSpeed::Fast;
    p.holdMs = 350.0f;
    p.dynamicsPercent = 100.0f;
    p.voicing = Voicing::Top;
    p.scale = Scale::Major;
    p.chordLevelPercent = 70.0f;
    return p;
}

// The reference configuration: v0.3's default detection settings at Window 220 (inside the
// playable zone), as direct (Manual) settings: Confidence 0.85, Hysteresis 15, Hold 2000,
// Retune Fast.
constexpr double kRefWindowHz = 220.0;
EngineParams defaultParams()
{
    EngineParams p = v051Defaults();
    p.manual = true;
    p.windowHz = (float) kRefWindowHz;
    p.confidenceThreshold = 0.85f;
    p.hysteresisCents = 15.0f;
    p.holdMs = 2000.0f;
    return p;
}

// Detection across the playable zone: the top, the default and the minimum window (what the
// chord-hint tests and the benchmark sweep).
struct ZoneSetting
{
    const char* name;
    EngineParams p = v051Defaults();
};
std::vector<ZoneSetting> zoneSettings()
{
    EngineParams top, mid, bottom; // v0.5.2's defaults (detection doesn't depend on what changed)
    top.windowHz = (float) kWindowMaxHz;
    bottom.windowHz = (float) derivedWindowMinHz();
    return { { "424.26 Hz (top)", top }, { "240.62 Hz (default)", mid }, { "192.99 Hz (minimum)", bottom } };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testBypassAndClip()
{
    // Bypass on: output is the raw, undelayed input, exactly.
    {
        Engine e;
        e.prepare (kFs, 64);
        auto p = defaultParams();
        p.bypass = true;
        auto in = steadySaw (220.0, 0.5, 0.5f);
        auto out = runEngine (e, in, p);
        float maxErr = 0.0f;
        for (size_t n = 0; n < in.size(); ++n)
            maxErr = std::max (maxErr, std::abs (out[n] - in[n]));
        check (maxErr <= 0.0f, "Bypass on: output == raw undelayed input (max err " + juce::String (maxErr, 8) + ")");
    }

    // Bypass toggles crossfade over 10 ms: no jumps on a sine.
    {
        Engine e;
        e.prepare (kFs, 64);
        auto in = sine (440.0, 1.5, 0.4f);
        std::vector<float> out (in.size());
        for (size_t start = 0; start < in.size(); start += 64)
        {
            auto p = defaultParams();
            p.wave = Wave::Sine;
            const double t = (double) start / kFs;
            p.bypass = t >= 0.5 && t < 1.0;
            e.process (in.data() + start, out.data() + start, (int) std::min ((size_t) 64, in.size() - start), p);
        }
        writeWav ("bypass_toggle", out);
        float steady = 0.0f, atToggle = 0.0f;
        for (size_t n = 1; n < out.size(); ++n)
        {
            const double t = (double) n / kFs;
            const float d = std::abs (out[n] - out[n - 1]);
            const bool nearToggle = (t > 0.49 && t < 0.53) || (t > 0.99 && t < 1.03);
            if (nearToggle)
                atToggle = std::max (atToggle, d);
            else if (t > 0.2)
                steady = std::max (steady, d);
        }
        check (atToggle < 1.5f * steady, "Bypass toggle crossfade has no jump (max step " + juce::String (atToggle, 4) + " vs steady "
                                             + juce::String (steady, 4) + ")");
    }

    // Safety clip: a hot synth (full-scale input) never exceeds 1.0 (with the headroom gain off, so the clip itself is
    // reached); with it on (v0.5.3) the output stays at the headroom ceiling.
    {
        auto p = defaultParams();
        auto hot = steadySaw (440.0, 1.0, 1.0f);
        float peak[2] = {};
        for (int on = 0; on < 2; ++on)
        {
            Engine e;
            e.prepare (kFs, 64);
            e.setHeadroomGain (on == 1);
            auto out = runEngine (e, hot, p);
            for (auto v : out)
                peak[on] = std::max (peak[on], std::abs (v));
            if (on == 0)
                writeWav ("clip_out", out);
        }
        check (peak[0] <= 1.0f && peak[0] > 0.9f, "Safety clip ceiling: hot synth, headroom gain off, peaks at " + juce::String (peak[0], 4));
        check (peak[1] <= kHeadroomCeiling * 1.0001f, juce::String::formatted ("With the headroom gain: peaks at %.4f (ceiling %.4f)", peak[1], kHeadroomCeiling));
        check (std::equal_to<float>() (safetyClip (0.5f), 0.5f) && std::equal_to<float>() (safetyClip (-0.9f), -0.9f),
               "Safety clip is linear below 0.9");
    }
}

double rms (const std::vector<float>& x, size_t from = 0, size_t to = 0)
{
    if (to == 0 || to > x.size())
        to = x.size();
    double s = 0.0;
    for (size_t n = from; n < to; ++n)
        s += (double) x[n] * x[n];
    return std::sqrt (s / (double) std::max<size_t> (1, to - from));
}

double toDb (double v) { return 20.0 * std::log10 (std::max (v, 1e-12)); }

// ---------------------------------------------------------------------------
// Pitch detection helpers
// ---------------------------------------------------------------------------

// Run a signal through an Engine and collect every detector hop. Writes a CSV.
std::vector<HopInfo> collectHops (const std::vector<float>& in, EngineParams p, const juce::String& csvName, double fs = kFs,
                                  std::vector<float>* audioOut = nullptr, bool sputterFix = true)
{
    Engine e;
    e.prepare (fs, 64);
    e.setSputterFix (sputterFix);
    std::vector<HopInfo> hops;
    hops.reserve (in.size() / 64);
    e.hopListener = [&] (const HopInfo& h) { hops.push_back (h); };
    auto out = runEngine (e, in, p);
    if (audioOut != nullptr)
        *audioOut = std::move (out);

    Csv csv (csvName, "time_s,f0_hz,midi,confidence,voiced,guard_rejected,note_N,target_T,ratio_r");
    for (auto& h : hops)
        csv.row (juce::String ((double) h.time / fs, 5) + "," + juce::String (h.detector.f0, 3) + "," + juce::String (h.midi, 4) + ","
                 + juce::String (h.detector.confidence, 4) + "," + juce::String ((int) h.detector.voiced) + ","
                 + juce::String ((int) h.detector.guardRejected) + "," + juce::String (h.noteN) + "," + juce::String (h.target, 4) + ","
                 + juce::String (h.ratio, 5));
    return hops;
}

EngineParams detectorParams (double windowHz = kRefWindowHz)
{
    auto p = defaultParams();
    p.windowHz = (float) windowHz;
    return p;
}

// The measuring detector: the plugin's YIN (and octave guard), but free to use windows below
// the playable zone, since sub octaves go down to ~27 Hz. Measures an output's pitch; never
// part of the engine. Same hop timing and time stamps as the engine's detector.
constexpr double kMeasureWindowHz = 55.0;
int measureLatency (double windowHz, double fs = kFs) { return 2 * (int) std::lround (fs / windowHz); }

std::vector<HopInfo> measureHops (const std::vector<float>& sig, const juce::String& csvName, double windowHz = kMeasureWindowHz, double fs = kFs)
{
    constexpr double lowestHz = 25.0;
    PitchDetector d;
    d.prepare (fs, 64, lowestHz);
    d.setWindow (windowHz);
    History h;
    h.prepare (4 * maxTauFor (fs, lowestHz));
    const int hop = hopFor (fs), L = d.windowLength();
    std::vector<HopInfo> hops;
    hops.reserve (sig.size() / (size_t) hop + 1);
    for (size_t n = 0; n < sig.size(); ++n)
    {
        h.push (sig[n]);
        if ((n + 1) % (size_t) hop != 0)
            continue;
        HopInfo info;
        info.detector = d.analyse (h, h.newest(), 0.85f);
        info.time = h.newest() + 1 - L / 2;
        info.midi = info.detector.f0 > 0.0f ? midiFromHz (info.detector.f0) : 0.0;
        hops.push_back (info);
    }
    Csv csv (csvName, "time_s,f0_hz,midi,confidence,voiced,guard_rejected");
    for (auto& x : hops)
        csv.row (juce::String ((double) x.time / fs, 5) + "," + juce::String (x.detector.f0, 3) + "," + juce::String (x.midi, 4) + ","
                 + juce::String (x.detector.confidence, 4) + "," + juce::String ((int) x.detector.voiced) + "," + juce::String ((int) x.detector.guardRejected));
    return hops;
}


// Loss per period for a given T60, so KS plucks decay like a real string.
float ksLossFor (double hz, double t60) { return (float) std::pow (10.0, -3.0 / (hz * t60)); }

// ---------------------------------------------------------------------------
// Test 1: steady saw (and KS pluck) -> within +/-3 cents, confidence > 0.9
// ---------------------------------------------------------------------------
void testDetector()
{
    struct Case
    {
        const char* kind;
        double hz;
        double window;
        double fs;
        bool infoOnly = false; // beyond the spec's 110/220/440: reported, not pass/fail
    };
    // The playable zone: its minimum (G3 reliable), Window 220, the default (240.6 Hz), 300 Hz and
    // the top (424.26 Hz, A4 reliable). A tone is only trackable when its period fits the window (f > window).
    const double wMin = derivedWindowMinHz(), wMid = defaultWindowHz();
    const Case cases[] = {
        { "saw", 196.0, wMin, kFs }, { "saw", 220.0, wMin, kFs }, { "saw", 440.0, wMin, kFs },
        { "saw", 233.08, 220.0, kFs }, { "saw", 440.0, 220.0, kFs }, { "saw", 880.0, 220.0, kFs, true },
        { "saw", 261.63, wMid, kFs }, { "saw", 523.25, wMid, kFs },
        { "saw", 329.63, 300.0, kFs }, { "saw", 440.0, 300.0, kFs }, { "saw", 659.26, 300.0, kFs, true }, { "saw", 1046.5, 300.0, kFs, true },
        { "saw", 196.0, wMin, 44100.0 }, { "saw", 196.0, wMin, 96000.0 }, { "saw", 233.08, 220.0, 96000.0 },
        { "ks", 196.0, wMin, kFs }, { "ks", 246.94, wMin, kFs }, { "ks", 440.0, wMin, kFs },
        { "ks", 246.94, 220.0, kFs }, { "ks", 440.0, 220.0, kFs }, { "ks", 329.63, 300.0, kFs },
        { "saw", 440.0, kWindowMaxHz, kFs }, { "saw", 659.26, kWindowMaxHz, kFs, true }, { "saw", 440.0, kWindowMaxHz, 44100.0, true }, { "saw", 440.0, kWindowMaxHz, 96000.0 },
        { "saw", 1108.73, kWindowMaxHz, kFs, true }, { "ks", 440.0, kWindowMaxHz, kFs }, { "ks", 880.0, kWindowMaxHz, kFs },
    };

    for (const auto& c : cases)
    {
        const bool ks = juce::String (c.kind) == "ks";
        const double seconds = 2.0;
        auto sig = ks ? ksPluck ([&] (double) { return c.hz; }, seconds, 0.5f, ksLossFor (c.hz, 3.0), 1, c.fs)
                      : sawFromTrajectory ([&] (double) { return c.hz; }, seconds, 0.5f, c.fs);
        const juce::String name = juce::String ("detector_") + c.kind + "_" + juce::String (c.hz, 0) + "Hz_win" + juce::String (c.window, 0) + "_"
                                  + juce::String (c.fs / 1000.0, 1) + "k";
        auto hops = collectHops (sig, detectorParams (c.window), name, c.fs);
        if (std::abs (c.fs - kFs) < 1.0)
            writeWav (name, sig, c.fs);

        const int L = latencyFor (c.fs, c.window);
        // Steady-state: skip the onset (first 20 ms for saws; plucks after 0.1 s).
        const double from = ks ? 0.1 : 0.02, to = ks ? 1.5 : seconds;
        int onsetOctaveErrors = 0;
        for (auto& h : hops)
            if (h.detector.voiced && (double) h.time / c.fs < 0.05 && std::abs (1200.0 * std::log2 (h.detector.f0 / c.hz)) > 600.0)
                ++onsetOctaveErrors;
        double worstCents = 0.0, sumCents = 0.0;
        float minConf = 1.0f;
        int n = 0, unvoiced = 0;
        for (auto& h : hops)
        {
            const double windowStart = (double) (h.time - L / 2) / c.fs;
            const double windowEnd = (double) (h.time + L / 2) / c.fs;
            if (windowStart < from || windowEnd > to)
                continue;
            ++n;
            if (! h.detector.voiced)
                ++unvoiced;
            const double cents = 1200.0 * std::log2 (h.detector.f0 / c.hz);
            worstCents = std::max (worstCents, std::abs (cents));
            sumCents += cents;
            minConf = std::min (minConf, h.detector.confidence);
        }
        const juce::String line = juce::String::formatted ("%-4s %7.2f Hz window %6.2f Hz @%5.1fk: %d hops, worst %.3f c, mean %+.3f c, min conf %.4f, unvoiced %d",
                                        c.kind, c.hz, c.window, c.fs / 1000.0, n, worstCents, sumCents / std::max (1, n),
                                        minConf, unvoiced)
                                  + (onsetOctaveErrors > 0 ? juce::String::formatted ("; %d octave-off hop(s) at onset", onsetOctaveErrors) : juce::String());
        if (c.infoOnly)
            info ("(info) " + line);
        else
            check (n > 50 && unvoiced == 0 && worstCents < 3.0 && minConf > 0.9f, line);
    }
}

// ---------------------------------------------------------------------------
// Probe: for each window, the lowest tone (cents re. the window frequency) that tracks
// steadily within +/-3 c with confidence > 0.9 -- saw and Karplus-Strong pluck.
// ---------------------------------------------------------------------------
bool tracksSteadily (bool ks, double hz, double window)
{
    auto sig = ks ? ksPluck ([&] (double) { return hz; }, 1.2, 0.5f, ksLossFor (hz, 3.0), 3)
                  : sawFromTrajectory ([&] (double) { return hz; }, 1.2, 0.5f);
    Engine e;
    e.prepare (kFs, 64);
    bool ok = true;
    int n = 0;
    const int L = latencyFor (kFs, window);
    e.hopListener = [&] (const HopInfo& h) {
        const double ws = (double) (h.time - L / 2) / kFs;
        if (ws < (ks ? 0.1 : 0.02) || (double) (h.time + L / 2) / kFs > 1.1)
            return;
        ++n;
        ok = ok && h.detector.voiced && h.detector.confidence > 0.9f && std::abs (1200.0 * std::log2 (h.detector.f0 / hz)) < 3.0;
    };
    runEngine (e, sig, detectorParams (window));
    return ok && n > 20;
}

void probeReliable()
{
    for (double w : { derivedWindowMinHz(), 220.0, (double) defaultWindowHz(), 270.0, 300.0, 360.0, kWindowMaxHz })
    {
        juce::String line = juce::String::formatted ("window %6.2f Hz (tauMax %4d):", w, tauMaxFor (kFs, w));
        for (bool ks : { false, true })
        {
            int lowest = 999;
            for (int c = 60; c >= -40; c -= 2)
                if (tracksSteadily (ks, w * std::pow (2.0, c / 1200.0), w))
                    lowest = c;
                else
                    break;
            line << (ks ? "  ks" : "  saw") << " from " << (lowest == 999 ? juce::String ("--") : juce::String::formatted ("%+d c", lowest));
        }
        info (line);
    }
}

// The readout's LOWEST RELIABLE claim: that note, played 25 c flat, tracks (saw and pluck).
void testReadoutClaim()
{
    for (double w : { derivedWindowMinHz(), 200.0, 220.0, (double) defaultWindowHz(), 260.0, 280.0, 300.0, 340.0, 380.0, kWindowMaxHz })
    {
        const int note = lowestReliableNote (kFs, w);
        const double flatHz = hzFromMidi (note - kReliableFlatMarginCents / 100.0);
        const bool saw = tracksSteadily (false, flatHz, w), ks = tracksSteadily (true, flatHz, w);
        const bool belowInTune = tracksSteadily (false, hzFromMidi (note - 1), w);
        check (saw && ks, juce::String::formatted ("window %6.2f Hz: lowest reliable MIDI %d, played 25 c flat (%.2f Hz), tracks (saw %s, pluck %s);"
                                                   " the note below in tune: %s",
                                                   w, note, flatHz, saw ? "yes" : "NO", ks ? "yes" : "NO", belowInTune ? "tracks" : "fails"));
    }
}

// ---------------------------------------------------------------------------
// Test E: tab conversion (the spec's table), its text and the mini tab.
// ---------------------------------------------------------------------------
void testTab()
{
    struct Row
    {
        int n;
        const char* note;
        const char* expect;
    };
    const Row table[] = {
        { 39, "D#2", "below open low E (whole neck)" }, { 40, "E2", "E string fret 0" }, { 44, "G#2", "E string fret 4" },
        { 45, "A2", "A string fret 0" },  { 46, "A#2", "A string fret 1" },  { 49, "C#3", "A string fret 4" },
        { 58, "A#3", "G string fret 3" }, { 59, "B3", "B string fret 0" },   { 63, "D#4", "B string fret 4" },
        { 64, "E4", "e string fret 0" },  { 69, "A4", "e string fret 5" },   { 71, "B4", "e string fret 7" },
    };
    for (const auto& row : table)
    {
        const auto got = tabPositionText (tabPositionFor (row.n));
        check (got == row.expect, juce::String::formatted ("n %d (%s) -> %s", row.n, row.note, got.c_str()));
    }

    // Every note from far below the guitar to the top of the detector: frets 0..4 except on high e.
    bool rule = true;
    for (int n = 0; n <= 90; ++n)
    {
        const auto t = tabPositionFor (n);
        if (t.belowLowE)
            rule = rule && n < 40 && t.string == 0 && t.fret == 0;
        else
            rule = rule && kOpenStringMidi[(size_t) t.string] + t.fret == n && t.fret >= 0 && (t.string == 5 || t.fret <= 4)
                   && (t.string == 5 || kOpenStringMidi[(size_t) t.string + 1] > n);
    }
    check (rule, "MIDI 0..90: highest string at or below the note, fret 0..4 except on high e, below 40 = whole neck");

    // The readout's lowest reliable note across the Window range, as tab.
    juce::String line;
    for (double w : { derivedWindowMinHz(), 220.0, (double) defaultWindowHz(), 300.0 })
        line << juce::String (w, 2) << " Hz: " << tabPositionText (tabPositionFor (lowestReliableNote (kFs, w))) << "; ";
    info ("lowest reliable @ 48 kHz -- " + line.trimEnd());
}

// ---------------------------------------------------------------------------
// Test O: fretboard classification. Green at or above the lowest reliable note, yellow from
// the window's own pitch up to it, red below the window's pitch; both ends of the range.
// ---------------------------------------------------------------------------
void testFretboard()
{
    using S = FretStatus;
    struct Row
    {
        int string, fret;
        S expect;
    };
    struct Case
    {
        const char* name;
        double floorHz;
        int lowest;
        std::vector<Row> rows;
    };
    // Hand-made values first (a floor at 251 Hz = MIDI 59.22: B3 and up yellow-or-green).
    const Case hand { "floor 251 Hz, lowest reliable C4 (60)", 251.0, 60,
                      { { 5, 0, S::Reliable }, { 4, 1, S::Reliable }, { 4, 0, S::WontTrack }, { 3, 4, S::WontTrack }, { 3, 5, S::Reliable }, { 0, 20, S::Reliable },
                        { 0, 19, S::WontTrack }, { 0, 0, S::WontTrack }, { 1, 14, S::WontTrack }, { 1, 15, S::Reliable } } };
    // A real window with a yellow band: 245 Hz -> floor 244.90 Hz (MIDI 58.85), lowest reliable C4.
    const Case yellow { "Window 245 Hz (floor MIDI 58.85, lowest reliable C4)", trackableFloorHz (kFs, 245.0), lowestReliableNote (kFs, 245.0),
                        { { 4, 0, S::InTuneOnly }, { 3, 4, S::InTuneOnly }, { 0, 19, S::InTuneOnly }, { 3, 3, S::WontTrack }, { 4, 1, S::Reliable },
                          { 2, 8, S::WontTrack }, { 2, 9, S::InTuneOnly }, { 2, 10, S::Reliable } } };
    // The two ends of the playable zone at 48 kHz, from the real rule.
    const double wMin = derivedWindowMinHz();
    const Case slowEnd { "Window minimum (192.99 Hz)", trackableFloorHz (kFs, wMin), lowestReliableNote (kFs, wMin),
                         { { 3, 0, S::Reliable }, { 2, 5, S::Reliable }, { 2, 4, S::WontTrack }, { 0, 14, S::WontTrack }, { 0, 15, S::Reliable }, { 5, 21, S::Reliable } } };
    // At 300 Hz the floor is MIDI 62.37 and lowest reliable 63: no whole note between, so no yellow.
    const Case fastEnd { "Window 300 Hz (floor MIDI 62.37, lowest reliable D#4)", trackableFloorHz (kFs, 300.0), lowestReliableNote (kFs, 300.0),
                         { { 4, 4, S::Reliable }, { 4, 3, S::WontTrack }, { 4, 2, S::WontTrack }, { 3, 7, S::WontTrack }, { 3, 8, S::Reliable }, { 5, 0, S::Reliable }, { 0, 0, S::WontTrack } } };
    // At the top (424.26 Hz) the floor is MIDI 68.39 (424.78 Hz) and lowest reliable A4: no yellow either;
    // green from A4 (high e fret 5, B fret 10, ...) up to the 21st fret.
    const Case topEnd { "Window 424.26 Hz (floor MIDI 68.39, lowest reliable A4)", trackableFloorHz (kFs, kWindowMaxHz), lowestReliableNote (kFs, kWindowMaxHz),
                        { { 5, 5, S::Reliable }, { 5, 4, S::WontTrack }, { 4, 10, S::Reliable }, { 4, 9, S::WontTrack }, { 0, 21, S::WontTrack },
                          { 2, 21, S::Reliable }, { 2, 18, S::WontTrack }, { 5, 21, S::Reliable } } };
    const char* sym[3] = { "green", "yellow", "red" };
    for (const auto* c : { &hand, &yellow, &slowEnd, &fastEnd, &topEnd })
    {
        bool ok = true;
        juce::String line;
        for (const auto& r : c->rows)
        {
            const auto got = classifyPosition (positionNote (r.string, r.fret), c->floorHz, c->lowest);
            ok = ok && got == r.expect;
            line << kStringNames[(size_t) r.string] << r.fret << " " << sym[(int) got] << (got == r.expect ? "" : "(WRONG)") << ", ";
        }
        check (ok, juce::String (c->name) + ": " + line.trimEnd().dropLastCharacters (1));
    }

    // The whole board at every setting: statuses are ordered along each string (red, then
    // yellow, then green, never back), and green starts exactly at the lowest reliable note.
    bool ordered = true;
    for (double w = wMin; w <= kWindowMaxHz; w += 0.5)
    {
        const double floorHz = trackableFloorHz (kFs, w);
        const int lowest = lowestReliableNote (kFs, w);
        for (int string = 0; string < 6; ++string)
            for (int fret = 1; fret <= kFretCount; ++fret)
            {
                const auto a = classifyPosition (positionNote (string, fret - 1), floorHz, lowest), b = classifyPosition (positionNote (string, fret), floorHz, lowest);
                ordered = ordered && (int) b <= (int) a && ((b == S::Reliable) == (positionNote (string, fret) >= lowest));
            }
    }
    check (ordered, "Every Window 192.99..424.26 Hz (0.5 Hz steps): along each string red -> yellow -> green, green from the lowest reliable note");
    check (kFretCount == 21 && kSingleInlayFrets.size() == 8 && kDoubleInlayFret == 12, "21 frets; inlays at 3 5 7 9 15 17 19 21, double at 12");
}

// ---------------------------------------------------------------------------
// Test J: the Window range. The minimum is derived from the lowest-reliable rule so that the lowest
// reliable note is exactly G3 (the open G string) at the knob's lowest setting. The top is
// 300 Hz x 2^(6/12) = 424.26 Hz: lowest reliable A4, and still tracking the top of a 21-fret neck.
// ---------------------------------------------------------------------------

// A steady saw at hz, at this window and rate: is every hop voiced (confidence > 0.9), and how far
// off is the worst one (cents)?
struct SawTrack
{
    bool voiced = false;
    double worstCents = 0.0;
};
SawTrack sawTrackAt (double fs, double hz, double window)
{
    const auto hops = collectHops (sawFromTrajectory ([hz] (double) { return hz; }, 0.6, 0.5f, fs), detectorParams (window),
                                   "range_saw" + juce::String (hz, 0) + "_" + juce::String (fs / 1000.0, 1) + "k", fs);
    const int L = latencyFor (fs, window);
    int n = 0;
    SawTrack r;
    r.voiced = true;
    for (auto& h : hops)
        if ((double) (h.time - L / 2) / fs >= 0.02 && (double) (h.time + L / 2) / fs <= 0.6)
        {
            ++n;
            r.voiced = r.voiced && h.detector.voiced && h.detector.confidence > 0.9f;
            r.worstCents = std::max (r.worstCents, std::abs (1200.0 * std::log2 (h.detector.f0 / hz)));
        }
    r.voiced = r.voiced && n > 20;
    return r;
}

void testWindowMinimum()
{
    const double wMin = derivedWindowMinHz();
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    juce::String line;
    bool g3Everywhere = true, aboveIsHigher = false;
    for (double fs : rates)
    {
        g3Everywhere = g3Everywhere && lowestReliableNote (fs, wMin) == kPlayableLowestNote;
        aboveIsHigher = aboveIsHigher || lowestReliableNote (fs, wMin + 0.01) > kPlayableLowestNote;
        line << juce::String (fs / 1000.0, 1) << "k: tau " << tauMaxFor (fs, wMin) << ", " << juce::String (1000.0 * latencyFor (fs, wMin) / fs, 2) << " ms; ";
    }
    info (juce::String::formatted ("derived minimum window: %.2f Hz (G3 - 25 c = %.2f Hz)", wMin, hzFromMidi (kPlayableLowestNote - 0.25)));
    info ("  at the minimum -- " + line.trimEnd());
    check (g3Everywhere, juce::String::formatted ("Window %.2f Hz: lowest reliable is exactly G3 at 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz", wMin));
    check (aboveIsHigher, juce::String::formatted ("Window %.2f Hz (0.01 Hz higher): lowest reliable rises above G3 at some rate, so the minimum is the lowest-latency G3 window", wMin + 0.01));

    bool zoneOk = true;
    for (double w = wMin; w <= kWindowMaxHz; w += 0.25)
        for (double fs : rates)
            zoneOk = zoneOk && lowestReliableNote (fs, w) >= kPlayableLowestNote;
    check (zoneOk, juce::String::formatted ("Every setting from %.2f to %.2f Hz: lowest reliable at or above G3; latency %.2f .. %.2f ms at 48 kHz", wMin,
                                            kWindowMaxHz, 1000.0 * latencyFor (kFs, kWindowMaxHz) / kFs, 1000.0 * latencyFor (kFs, wMin) / kFs));

    // The top: six semitones above v0.4's 300 Hz. Latency, lowest reliable note and the detector's
    // range at each rate; the detection range must still reach high e fret 21 (C#6, 1108.73 Hz).
    {
        const double wTop = kWindowMaxHz, c6Sharp = hzFromMidi (85);
        bool a4Everywhere = true, rangeOk = true;
        juce::String lats, range;
        for (double fs : rates)
        {
            a4Everywhere = a4Everywhere && lowestReliableNote (fs, wTop) == 69;
            PitchDetector d;
            d.prepare (fs, 64);
            d.setWindow (wTop);
            // Searched lags: tauMin .. tauMax - 1, so f0 from fs / (tauMax - 1) up to fs / tauMin.
            rangeOk = rangeOk && d.windowLength() == latencyFor (fs, wTop) && d.tauMax() == tauMaxFor (fs, wTop) && fs / d.tauMin() >= c6Sharp
                      && fs / (d.tauMax() - 1) <= hzFromMidi (69 - 0.25);
            if (fs == 44100.0 || fs == 48000.0 || fs == 96000.0)
            {
                lats << juce::String (fs / 1000.0, 1) << "k " << juce::String (1000.0 * latencyFor (fs, wTop) / fs, 2) << " ms; ";
                range << juce::String (fs / 1000.0, 1) << "k tau " << d.tauMin() << ".." << d.tauMax() - 1 << " (" << juce::String (fs / (d.tauMax() - 1), 1)
                      << ".." << juce::roundToInt (fs / d.tauMin()) << " Hz); ";
            }
        }
        check (std::abs (wTop - 300.0 * std::exp2 (0.5)) < 0.005 && a4Everywhere,
               juce::String::formatted ("Top %.2f Hz (300 Hz x 2^(6/12)): lowest reliable A4 at every rate; latency ", wTop) + lats.trimEnd());
        check (rangeOk, "Top: L and tauMax as derived, lags cover A4 - 25 c up to C#6 (21-fret high e) at every rate -- " + range.trimEnd());

        // Tracking = every hop voiced and on the right note (within 10 c; the quantizer snaps, so
        // a few cents of detector error don't reach the output). The worst error is reported: the
        // top's 113-sample integration (48 kHz) is a little less exact than 300 Hz's 160.
        bool tracks = true;
        juce::String worst;
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            worst << juce::String (fs / 1000.0, 1) << "k:";
            for (double hz : { hzFromMidi (69 - 0.25), 440.0, 659.26, c6Sharp, hzFromMidi (88) })
            {
                const auto t = sawTrackAt (fs, hz, wTop);
                tracks = tracks && t.voiced && t.worstCents < 10.0;
                worst << " " << juce::String (t.worstCents, 1) << (t.voiced ? "" : "(UNVOICED)");
            }
            worst << " c; ";
        }
        const bool flatPluck = tracksSteadily (true, hzFromMidi (69 - 0.25), wTop);
        const auto gSharp = sawTrackAt (kFs, hzFromMidi (68), wTop);
        check (tracks && flatPluck && ! (gSharp.voiced && gSharp.worstCents < 10.0),
               "Top: saws at A4 - 25 c, A4, E5, C#6 (fret 21) and E6 track on the note at 44.1 / 48 / 96 kHz; plucked A4 - 25 c tracks within 3 c; G#4 in tune doesn't. "
               "Worst error per note -- " + worst.trimEnd());

        // The engine runs at the top and clamps above it.
        Engine e;
        e.prepare (kFs, 64);
        auto p = defaultParams();
        p.windowHz = 1000.0f;
        runEngine (e, std::vector<float> (6400, 0.0f), p);
        check (e.currentLatencySamples() == latencyFor (kFs, wTop) && e.hopSize() == 128,
               juce::String::formatted ("A 1000 Hz request clamps to the top: L = %d samples (%.2f ms); hop %d (every input sample is in at least one window: L >= hop)",
                                        e.currentLatencySamples(), 1000.0 * e.currentLatencySamples() / kFs, e.hopSize()));
    }

    // As played, at the minimum: G3 25 c flat tracks (saw and pluck); F#3 in tune doesn't.
    const double g3Flat = hzFromMidi (kPlayableLowestNote - 0.25);
    const bool saw = tracksSteadily (false, g3Flat, wMin), ks = tracksSteadily (true, g3Flat, wMin);
    const bool fSharp = tracksSteadily (false, hzFromMidi (kPlayableLowestNote - 1), wMin);
    check (saw && ks && ! fSharp, juce::String::formatted ("At the minimum window, G3 played 25 c flat tracks (saw %s, pluck %s); F#3 in tune %s",
                                                            saw ? "yes" : "NO", ks ? "yes" : "NO", fSharp ? "TRACKS" : "doesn't"));

    // The engine clamps into the zone, and buffers are sized for it.
    Engine e;
    e.prepare (kFs, 64);
    auto p = defaultParams();
    p.windowHz = 55.0f;
    std::vector<float> silence (6400, 0.0f);
    runEngine (e, silence, p);
    check (e.currentLatencySamples() == latencyFor (kFs, wMin),
           juce::String::formatted ("A 55 Hz request clamps to the minimum: L = %d samples (%.2f ms)", e.currentLatencySamples(), 1000.0 * e.currentLatencySamples() / kFs));
    info (juce::String::formatted ("preallocated for the longest window at 96 kHz: tauMax %d (v0.3, 55 Hz: %d), engine history %d samples (v0.3: %d)", maxTauFor (kFs),
                                   (int) std::lround (96000.0 / 55.0), nextPow2 (8 * maxTauFor (kFs)), nextPow2 (8 * (int) std::lround (96000.0 / 55.0))));
}

// ---------------------------------------------------------------------------
// Test 3: glide parked at +48 cents -> no note chatter at hysteresis 15
// ---------------------------------------------------------------------------
void testHysteresis()
{
    // At each window: a base note for 0.5 s, glide up to +48 c by 1.0 s, then park with
    // +/-4 c vibrato at 5 Hz. The base note must fit the window (A3 at the minimum window,
    // A4 at 220 and 300 Hz, since A3's period is just longer than a 220 Hz window).
    struct Case
    {
        double window, baseHz;
        int baseNote;
    };
    for (const Case c : { Case { derivedWindowMinHz(), 220.0, 57 }, Case { 220.0, 440.0, 69 }, Case { 300.0, 440.0, 69 } })
    {
        const double parkStart = 1.0, seconds = 4.0;
        auto centsAt = [=] (double t) {
            if (t < 0.5)
                return 0.0;
            if (t < parkStart)
                return 48.0 * (t - 0.5) / 0.5;
            return 48.0 + 4.0 * std::sin (2.0 * M_PI * 5.0 * (t - parkStart));
        };
        auto hzAt = [=] (double t) { return c.baseHz * std::pow (2.0, centsAt (t) / 1200.0); };
        auto sig = sawFromTrajectory (hzAt, seconds, 0.5f);
        const juce::String tag = "win" + juce::String (c.window, 0);
        writeWav ("hysteresis_park_48c_in_" + tag, sig);

        for (float hyst : { 15.0f, 0.0f })
        {
            auto p = detectorParams (c.window);
            p.hysteresisCents = hyst;
            auto hops = collectHops (sig, p, "hysteresis_park_48c_" + tag + "_h" + juce::String (hyst, 0));
            const int L = latencyFor (kFs, c.window);
            int changes = 0, prevN = -1, parkedN = -1;
            for (auto& h : hops)
            {
                if ((double) (h.time - L / 2) / kFs < parkStart)
                    continue;
                if (prevN >= 0 && h.noteN != prevN)
                    ++changes;
                prevN = h.noteN;
                if (parkedN < 0)
                    parkedN = h.noteN;
            }
            if (hyst > 0.0f)
                check (changes == 0 && parkedN == c.baseNote,
                       "Window " + juce::String (c.window, 2) + " Hz, hysteresis 15: parked at +48 c stays put (" + juce::String (changes)
                           + " note changes)");
            else
                check (changes > 0, "Window " + juce::String (c.window, 2) + " Hz, hysteresis 0 (sensitivity check): chatters ("
                                        + juce::String (changes) + " note changes)");
        }
    }
}

// ---------------------------------------------------------------------------
// Test 4: silence and white noise -> unvoiced
// ---------------------------------------------------------------------------
void testUnvoiced()
{
    struct Case
    {
        const char* name;
        std::vector<float> sig;
    };
    std::vector<Case> cases;
    cases.push_back ({ "silence", std::vector<float> ((size_t) (2.0 * kFs), 0.0f) });
    cases.push_back ({ "noise_-40dBFS", whiteNoise (2.0, 0.01f * 1.732f, 3) });
    cases.push_back ({ "noise_-20dBFS", whiteNoise (2.0, 0.1f * 1.732f, 5) });
    cases.push_back ({ "noise_-6dBFS", whiteNoise (2.0, 0.5f * 1.732f, 7) });

    for (auto& c : cases)
    {
        for (double w : { derivedWindowMinHz(), (double) defaultWindowHz(), 300.0, kWindowMaxHz })
        {
            auto hops = collectHops (c.sig, detectorParams (w), juce::String ("unvoiced_") + c.name + "_win" + juce::String (w, 0));
            int voiced = 0;
            float maxConf = 0.0f;
            for (auto& h : hops)
            {
                voiced += h.detector.voiced ? 1 : 0;
                maxConf = std::max (maxConf, h.detector.confidence);
            }
            check (voiced == 0, juce::String::formatted ("%-14s window %6.2f Hz: %d/%d hops voiced, max confidence %.3f", c.name, w, voiced,
                                                         (int) hops.size(), maxConf));
        }
    }
}

// ---------------------------------------------------------------------------
// Quantizer, octave guard and timeline unit checks
// ---------------------------------------------------------------------------
void testQuantizerUnits()
{
    const double hopSec = 128.0 / kFs;

    {
        Quantizer q;
        q.reset();
        q.update (true, 57.2, 15.0f, 0.0f, hopSec);
        const bool first = q.note() == 57 && std::abs (q.ratio() - std::pow (2.0, -0.2 / 12.0)) < 1e-9;
        q.update (true, 57.64, 15.0f, 0.0f, hopSec);
        const bool held = q.note() == 57;
        q.update (true, 57.66, 15.0f, 0.0f, hopSec);
        const bool moved = q.note() == 58 && std::abs (q.target() - 58.0) < 1e-9;
        q.update (false, 0.0, 15.0f, 0.0f, hopSec);
        const bool frozen = q.note() == 58;
        q.update (true, 57.4, 15.0f, 0.0f, hopSec);
        const bool relatched = q.note() == 57;
        check (first && held && moved && frozen && relatched,
               "Quantizer: round on first hop, hold inside 0.65, move beyond it, freeze when lost, re-latch after");
    }
    {
        Quantizer q;
        q.reset();
        q.update (true, 57.0, 15.0f, 50.0f, hopSec);
        double t = 0.0;
        while (t < 0.050 - 1e-9)
        {
            q.update (true, 60.0, 15.0f, 50.0f, hopSec);
            t += hopSec;
        }
        const double expected = 57.0 + 3.0 * (1.0 - std::exp (-t / 0.050));
        check (std::abs (q.target() - expected) < 0.01,
               "Retune 50 ms: T after " + juce::String (t * 1000.0, 1) + " ms = " + juce::String (q.target(), 3) + " (expected "
                   + juce::String (expected, 3) + ")");
    }
    {
        OctaveGuard g;
        const bool a = g.accept (true, 57.0) && ! g.accept (true, 69.0) && g.accept (true, 57.0);
        g.reset();
        const bool b = g.accept (true, 57.0) && ! g.accept (true, 69.0) && g.accept (true, 69.2);
        const bool c = g.accept (false, 0.0) && g.accept (true, 45.0);
        check (a && b && c, "Octave guard: single-hop 12 st jump rejected, 2-hop jump accepted, reset when unvoiced");
    }
    {
        PitchTimeline tl;
        tl.reset (PitchEntry { 0, false, false, 100.0f, 1.0f, 57.0f });
        const bool fallback = tl.lookup (1000).period == 100.0f;
        for (int k = 0; k < 10; ++k)
            tl.push (PitchEntry { 1000 + 128 * k, true, true, (float) k, 1.0f, 57.0f });
        const bool exact = tl.lookup (1000 + 128 * 3).period == 3.0f;
        const bool between = tl.lookup (1000 + 128 * 3 + 64).period == 3.0f;
        const bool before = tl.lookup (0).period == 0.0f;
        const bool after = tl.lookup (1000000).period == 9.0f;
        check (fallback && exact && between && before && after, "Timeline lookup: latest entry at or before t");
    }
}

// ---------------------------------------------------------------------------
// Tune path helpers
// ---------------------------------------------------------------------------

EngineParams voiceParams (Wave wave = Wave::Saw)
{
    auto p = defaultParams();
    p.wave = wave;
    return p;
}

double meanAbs (const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0.0;
    for (size_t n = from; n < to; ++n)
        s += std::abs (x[n]);
    return s / (double) std::max<size_t> (1, to - from);
}

struct PitchStats
{
    int hops = 0, unvoiced = 0;
    double worstCents = 0.0, meanCents = 0.0;
};

// Measure an output signal's pitch against an expected frequency over [from, to) seconds.
PitchStats measureAgainst (const std::vector<float>& sig, double expectedHz, double from, double to, const juce::String& csv,
                           double windowHz = kMeasureWindowHz)
{
    auto hops = measureHops (sig, csv, windowHz);
    const int L = measureLatency (windowHz);
    PitchStats st;
    for (auto& h : hops)
    {
        const double ws = (double) (h.time - L / 2) / kFs, we = (double) (h.time + L / 2) / kFs;
        if (ws < from || we > to)
            continue;
        ++st.hops;
        if (! h.detector.voiced)
        {
            ++st.unvoiced;
            continue;
        }
        const double c = 1200.0 * std::log2 (h.detector.f0 / expectedHz);
        st.worstCents = std::max (st.worstCents, std::abs (c));
        st.meanCents += c;
    }
    st.meanCents /= std::max (1, st.hops - st.unvoiced);
    return st;
}

// ---------------------------------------------------------------------------
// Synth source
// ---------------------------------------------------------------------------
void testSynth()
{
    const int L = latencyFor (kFs, kRefWindowHz);
    const char* waveNames[4] = { "saw", "square", "triangle", "sine" };

    // Snapping: an input 30 cents sharp of A4 comes out on A4.
    {
        auto in = sawFromTrajectory ([] (double) { return 440.0 * std::pow (2.0, 30.0 / 1200.0); }, 2.0, 0.4f);
        for (int w = 0; w < 4; ++w)
        {
            auto p = voiceParams ((Wave) w);
            std::vector<float> out;
            collectHops (in, p, juce::String ("synth_snap_") + waveNames[w] + "_in_hops", kFs, &out);
            writeWav (juce::String ("synth_snap_") + waveNames[w], out);
            auto st = measureAgainst (out, 440.0, 0.3, 2.0, juce::String ("synth_snap_") + waveNames[w] + "_out_hops");
            check (st.hops > 50 && st.unvoiced == 0 && st.worstCents < 3.0,
                   juce::String::formatted ("Synth %-8s: A4+30c input -> output within %.3f c of A4 (mean %+.3f c, %d hops)", waveNames[w],
                                            st.worstCents, st.meanCents, st.hops));
        }
    }

    // Level: mean-absolute output vs mean-absolute input on the aligned timeline.
    {
        struct In
        {
            const char* name;
            std::vector<float> sig;
        };
        std::vector<In> inputs;
        // A single cycle of a pluck, tiled: stationary, with a plucked string's crest factor.
        auto tiledPluckCycle = [] (int period, uint32_t seed) {
            auto pl = ksPluck ([=] (double) { return kFs / period; }, 0.5, 0.5f, 1.0f, seed);
            std::vector<float> x ((size_t) (2.0 * kFs));
            const size_t from = (size_t) (0.2 * kFs);
            for (size_t n = 0; n < x.size(); ++n)
                x[n] = pl[from + n % (size_t) period];
            return x;
        };
        inputs.push_back ({ "pluckcycle262", tiledPluckCycle (183, 21) });
        inputs.push_back ({ "pluckcycle331", tiledPluckCycle (145, 22) });
        inputs.push_back ({ "pluckcycle440", tiledPluckCycle (109, 23) });
        // Decaying plucks are reported only: mean |x| of a pluck falls faster than the
        // spec's 120 ms release can follow, so the synth reads hot while it decays.
        inputs.push_back ({ "ks262_decaying", ksPluck ([] (double) { return 261.63; }, 2.0, 0.5f, ksLossFor (261.63, 8.0), 11) });
        inputs.push_back ({ "ks440_decaying", ksPluck ([] (double) { return 440.0; }, 2.0, 0.5f, ksLossFor (440.0, 8.0), 13) });
        inputs.push_back ({ "saw330", steadySaw (330.0, 2.0, 0.3f) });
        inputs.push_back ({ "sine330", sine (330.0, 2.0, 0.3f) });

        // Each wave's own gain over the mean-|x| scale (v0.5.1's loudness match, v0.5.2's +10 dB on Triangle
        // and Sine) is taken out: what's checked is that the level follows the guitar's.
        double designed[4];
        for (int w = 0; w < 4; ++w)
            designed[w] = toDb ((double) waveScale ((Wave) w) / (double) waveScale ((Wave) w, WaveLevels::MeanAbs));
        info (juce::String::formatted ("each wave's gain over mean |x| = 1: saw %+.2f, square %+.2f, triangle %+.2f, sine %+.2f dB (taken out below)", designed[0], designed[1], designed[2],
                                       designed[3]));
        for (auto& in : inputs)
        {
            juce::String line = juce::String ("level ") + in.name + ":";
            double worst = 0.0;
            for (int w = 0; w < 4; ++w)
            {
                auto p = voiceParams ((Wave) w);
                Engine e;
                e.prepare (kFs, 64);
                e.setHeadroomGain (false); // the level as the voice makes it (Triangle and Sine at +10 dB would reach the headroom gain)
                auto out = runEngine (e, in.sig, p);
                const size_t a = (size_t) (0.2 * kFs), b = (size_t) (1.5 * kFs);
                const double db = toDb (meanAbs (out, a + (size_t) L, b + (size_t) L) / meanAbs (in.sig, a, b)) - designed[w];
                worst = std::max (worst, std::abs (db));
                line << " " << waveNames[w] << " " << juce::String (db, 2) << " dB";
            }
            if (juce::String (in.name).endsWith ("decaying"))
                info (line + " (info: release lag while decaying)");
            else
                check (worst < 1.5, line + " (within 1.5 dB)");
        }
    }

    // Phase continuity across note changes: sine synth on a glide has no jumps.
    {
        auto in = sawFromTrajectory ([] (double t) { return 233.08 * std::pow (2.0, t / 2.0); }, 2.0, 0.4f);
        auto p = voiceParams (Wave::Sine);
        Engine e;
        e.prepare (kFs, 64);
        auto out = runEngine (e, in, p);
        writeWav ("synth_sine_glide", out);
        float peak = 0.0f, maxStep = 0.0f;
        for (size_t n = (size_t) (0.2 * kFs); n < out.size(); ++n)
        {
            peak = std::max (peak, std::abs (out[n]));
            maxStep = std::max (maxStep, std::abs (out[n] - out[n - 1]));
        }
        const float bound = (float) (2.0 * M_PI * 480.0 / kFs) * peak * 1.2f; // max slope of a sine at <= 480 Hz, +20%
        check (maxStep < bound, "Sine synth over a 12-step glide: max sample step " + juce::String (maxStep, 5) + " < slope bound "
                                    + juce::String (bound, 5));
    }

    // Timeline: output starts L after the input (runs on the delayed timeline).
    {
        std::vector<float> in ((size_t) (0.5 * kFs), 0.0f);
        auto tone = steadySaw (440.0, 1.0, 0.4f);
        in.insert (in.end(), tone.begin(), tone.end());
        auto p = voiceParams();
        Engine e;
        e.prepare (kFs, 64);
        auto out = runEngine (e, in, p);
        size_t onset = 0;
        for (size_t n = 0; n < out.size(); ++n)
            if (std::abs (out[n]) > 0.01f)
            {
                onset = n;
                break;
            }
        const double lateMs = ((double) onset - 0.5 * kFs) / kFs * 1000.0;
        info ("synth onset " + juce::String (lateMs, 2) + " ms after the input onset (L = " + juce::String (1000.0 * L / kFs, 2) + " ms)");
        check (lateMs >= 1000.0 * L / kFs - 0.1, "Synth output is not earlier than the L-delayed input");
    }
}

// ---------------------------------------------------------------------------
// Stage 6: align, range floor switching, lost pitch, crossfades
// ---------------------------------------------------------------------------

// Run with parameters chosen per 64-sample block by time; records latency per block.
std::vector<float> runScheduled (Engine& e, const std::vector<float>& in, const std::function<EngineParams (double)>& paramsAt,
                                 std::vector<int>* latencyPerBlock = nullptr)
{
    std::vector<float> out (in.size());
    for (size_t start = 0; start < in.size(); start += 64)
    {
        const int n = (int) std::min ((size_t) 64, in.size() - start);
        e.process (in.data() + start, out.data() + start, n, paramsAt ((double) start / kFs));
        if (latencyPerBlock != nullptr)
            latencyPerBlock->push_back (e.currentLatencySamples());
    }
    return out;
}

float maxStep (const std::vector<float>& x, double from, double to)
{
    float m = 0.0f;
    for (size_t n = std::max<size_t> (1, (size_t) (from * kFs)); n < std::min (x.size(), (size_t) (to * kFs)); ++n)
        m = std::max (m, std::abs (x[n] - x[n - 1]));
    return m;
}

// ---------------------------------------------------------------------------
// Test 6: sweep Window from the top (424.26 Hz) to the minimum under a sustained tone; the 20 ms crossfade hides
// every buffer swap, and a continuous drag coalesces into a single swap.
// ---------------------------------------------------------------------------
void testWindowSweep()
{
    const double toneHz = 494.0; // B4 (+0.4 c): trackable at every window setting, the top's included
    const double b4 = 493.883;

    struct Result
    {
        std::vector<float> out;
        std::vector<int> lat;
        int swaps;
    };
    auto render = [&] (Wave wave, const std::function<float (double)>& windowAt, double seconds) {
        auto in = steadySaw (toneHz, seconds, 0.4f);
        Engine e;
        e.prepare (kFs, 64);
        Result r;
        r.out = runScheduled (e, in, [&] (double t) {
            auto p = voiceParams (wave);
            p.windowHz = windowAt (t);
            return p;
        }, &r.lat);
        r.swaps = e.windowSwaps();
        return r;
    };

    // Click metric: largest sample step within +/-60 ms of each swap vs the steady tone's.
    auto swapClicks = [] (const Result& r, float& worstRatio) {
        const float steady = maxStep (r.out, 0.15, 0.45);
        worstRatio = 0.0f;
        for (size_t k = 1; k < r.lat.size(); ++k)
            if (r.lat[k] != r.lat[k - 1])
            {
                const double t = (double) (k * 64) / kFs;
                worstRatio = std::max (worstRatio, maxStep (r.out, t - 0.06, t + 0.06) / steady);
            }
        return worstRatio < 1.2f;
    };

    // A: stepped -- six settings, 0.5 s each (each move settles, so each one swaps).
    {
        const double steps[] = { kWindowMaxHz, 360.0, 300.0, 250.0, 215.0, derivedWindowMinHz() };
        auto windowAt = [&] (double t) { return (float) steps[std::min<size_t> (5, (size_t) (t / 0.5))]; };
        auto r = render (Wave::Sine, windowAt, 3.2);
        writeWav ("test6_window_steps_sine", r.out);
        writeWav ("test6_window_steps_saw", render (Wave::Saw, windowAt, 3.2).out);

        bool latOk = true;
        juce::String lats;
        for (int k = 0; k < 6; ++k)
        {
            const int got = r.lat[(size_t) ((0.5 * k + 0.45) * kFs / 64)];
            latOk = latOk && got == latencyFor (kFs, steps[k]);
            lats << juce::String (1000.0 * got / kFs, 1) << (k < 5 ? " -> " : " ms");
        }
        float ratio = 0.0f;
        const bool noClicks = swapClicks (r, ratio);
        check (r.swaps == 5 && latOk, "Stepped sweep: 5 swaps, latency follows the window: " + lats);
        check (noClicks, "Stepped sweep: no click at any swap (max step " + juce::String (ratio, 2) + "x the steady tone's)");

        double worst = 0.0;
        bool voiced = true;
        for (int k = 0; k < 6; ++k)
        {
            auto st = measureAgainst (r.out, b4, 0.5 * k + 0.2, 0.5 * k + 0.48, "test6_steps_seg" + juce::String (k));
            worst = std::max (worst, st.worstCents);
            voiced = voiced && st.unvoiced == 0 && st.hops > 0;
        }
        check (voiced && worst < 3.0, "Stepped sweep: output stays on B4 at every window (worst " + juce::String (worst, 2) + " c)");
    }

    // B: continuous drag from the top to the minimum over 3 s, then hold: one swap after it settles.
    {
        auto windowAt = [] (double t) { return (float) (kWindowMaxHz + (derivedWindowMinHz() - kWindowMaxHz) * std::min (1.0, t / 3.0)); };
        auto r = render (Wave::Sine, windowAt, 4.5);
        writeWav ("test6_window_drag_sine", r.out);
        writeWav ("test6_window_drag_saw", render (Wave::Saw, windowAt, 4.5).out);

        size_t swapBlock = 0;
        for (size_t k = 1; k < r.lat.size(); ++k)
            if (r.lat[k] != r.lat[k - 1])
                swapBlock = k;
        float ratio = 0.0f;
        const bool noClicks = swapClicks (r, ratio);
        const double swapAt = (double) (swapBlock * 64) / kFs;
        check (r.swaps == 1 && r.lat.back() == latencyFor (kFs, derivedWindowMinHz()),
               "Continuous drag coalesces into " + juce::String (r.swaps) + " swap, " + juce::String (1000.0 * (swapAt - 3.0), 0)
                   + " ms after the knob stops (100 ms settle + 20 ms fade-out)");
        check (noClicks, "Continuous drag: no click at the swap (max step " + juce::String (ratio, 2) + "x the steady tone's)");
        auto st = measureAgainst (r.out, b4, 3.4, 4.5, "test6_drag_after");
        check (st.unvoiced == 0 && st.worstCents < 3.0, "After the drag the output is on B4 (worst " + juce::String (st.worstCents, 2) + " c)");
    }
}

// ---------------------------------------------------------------------------
// Test A: each Octave setting sounds exactly 1, 1/2, 1/4, 1/8 of the snapped note.
// ---------------------------------------------------------------------------

// Spectral peak near expectHz of x[from, from + len): Hann window, zero-padded FFT,
// Gaussian interpolation of the peak bin.
double spectralPeakHz (const std::vector<float>& x, size_t from, size_t len, double expectHz, double fs = kFs)
{
    constexpr int order = 20;
    juce::dsp::FFT fft (order);
    const int nfft = 1 << order;
    std::vector<float> buf ((size_t) (2 * nfft), 0.0f);
    for (size_t k = 0; k < len; ++k)
        buf[k] = x[from + k] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * (double) k / (double) len));
    fft.performFrequencyOnlyForwardTransform (buf.data());
    const double binHz = fs / nfft;
    const int lo = (int) (expectHz * 0.97 / binHz), hi = (int) (expectHz * 1.03 / binHz);
    int best = lo;
    for (int k = lo; k <= hi; ++k)
        if (buf[(size_t) k] > buf[(size_t) best])
            best = k;
    const double y0 = std::log (buf[(size_t) best - 1]), y1 = std::log (buf[(size_t) best]), y2 = std::log (buf[(size_t) best + 1]);
    return (best + 0.5 * (y0 - y2) / (y0 - 2.0 * y1 + y2)) * binHz;
}

void testOctaves()
{
    const char* names[4] = { "Main", "Sub", "Double Sub", "Triple Sub" };
    auto render = [] (const std::vector<float>& in, Octave o, RetuneSpeed retune) {
        auto p = voiceParams (Wave::Sine);
        p.octave = o;
        p.retuneSpeed = retune;
        Engine e;
        e.prepare (kFs, 64);
        return runEngine (e, in, p);
    };

    // Across a glide: C5 for 0.3 s, glide to C6 over 2 s, hold 0.3 s. Triple Sub spans 65..131 Hz.
    auto hzAt = [] (double t) { return 523.25 * std::pow (2.0, std::clamp ((t - 0.3) / 2.0, 0.0, 1.0)); };
    const auto glide = sawFromTrajectory (hzAt, 2.6, 0.4f);
    for (RetuneSpeed retune : { RetuneSpeed::Fast, RetuneSpeed::Medium })
    {
        const juce::String tag = retune == RetuneSpeed::Fast ? "retuneFast" : "retuneMedium";
        std::vector<HopInfo> hops[4];
        for (int o = 0; o < 4; ++o)
        {
            const auto out = render (glide, (Octave) o, retune);
            writeWav ("testA_" + tag + "_octave" + juce::String (o), out);
            hops[o] = measureHops (out, "testA_" + tag + "_octave" + juce::String (o) + "_hops");
        }
        // The 55 Hz measuring window spans 2 * 873 samples = +/-7 hops. Compare only where the
        // pitch is steady across it (at a note step the detector smears longer periods differently).
        const size_t span = (size_t) (measureLatency (kMeasureWindowHz) / 2 / hopFor (kFs)) + 1;
        double worst[4] = { 0.0, 0.0, 0.0, 0.0 };
        int compared = 0;
        for (size_t k = span; k + span < hops[0].size(); ++k)
        {
            bool ok = true;
            for (int o = 0; o < 4; ++o)
                ok = ok && hops[o][k].detector.voiced;
            for (size_t j = k - span; j <= k + span && ok; ++j)
                ok = std::abs (hops[0][j].midi - hops[0][k].midi) < 0.01;
            if (! ok)
                continue;
            ++compared;
            for (int o = 1; o < 4; ++o)
                worst[o] = std::max (worst[o], std::abs ((hops[0][k].midi - hops[o][k].midi) - 12.0 * o) * 100.0);
        }
        check (compared > 100 && worst[1] < 1.0 && worst[2] < 1.0 && worst[3] < 1.0,
               juce::String::formatted ("%s glide C5->C6: Sub = Main - 12 st within %.3f c, Double Sub - 24 st within %.3f c, Triple Sub - 36 st within %.3f c (%d hops)",
                                        tag.toRawUTF8(), worst[1], worst[2], worst[3], compared));
    }

    // Exactly: a long A4 (input 12 c sharp, snapped to 440 Hz) for every wave; the spectral
    // peak is 440 / 2^k Hz.
    const auto a4 = steadySaw (440.0 * std::pow (2.0, 12.0 / 1200.0), 9.0, 0.4f);
    const char* waveNames[4] = { "saw", "square", "triangle", "sine" };
    for (int w = 0; w < 4; ++w)
    {
        juce::String line;
        double worst = 0.0;
        for (int o = 0; o < 4; ++o)
        {
            auto p = voiceParams ((Wave) w);
            p.octave = (Octave) o;
            Engine e;
            e.prepare (kFs, 64);
            const auto out = runEngine (e, a4, p);
            const double expect = 440.0 * octaveRatio ((Octave) o);
            const double c = 1200.0 * std::log2 (spectralPeakHz (out, (size_t) kFs, (size_t) (8.0 * kFs), expect) / expect);
            worst = std::max (worst, std::abs (c));
            line << names[o] << " " << juce::String (c, 4) << "c  ";
        }
        check (worst < 0.02, juce::String::formatted ("%-8s A4: fundamental at 440 x 1, 1/2, 1/4, 1/8 Hz: ", waveNames[w]) + line.trimEnd());
    }
}

// ---------------------------------------------------------------------------
// Test B: switching Octave while a note sustains is click-free (phase-continuous).
// ---------------------------------------------------------------------------
void testOctaveSwitch()
{
    // A4 sustained; 0.4 s per step through Main, Sub, Double, Triple, Main, Triple, Sub, Main.
    const Octave steps[] = { Octave::Main, Octave::Sub, Octave::DoubleSub, Octave::TripleSub, Octave::Main,
                             Octave::TripleSub, Octave::Sub, Octave::Main };
    constexpr int numSteps = 8;
    constexpr double stepSec = 0.4;
    const auto in = steadySaw (440.0, stepSec * numSteps + 0.1, 0.4f);

    struct Case
    {
        const char* name;
        Wave wave;
    };
    for (const Case c : { Case { "sine", Wave::Sine }, Case { "saw", Wave::Saw }, Case { "triangle", Wave::Triangle }, Case { "square", Wave::Square } })
    {
        Engine e;
        e.prepare (kFs, 64);
        const auto out = runScheduled (e, in, [&] (double t) {
            auto p = voiceParams (c.wave);
            p.octave = steps[std::min (numSteps - 1, (int) (t / stepSec))];
            return p;
        });
        writeWav ("testB_octave_switch_" + juce::String (c.name).replace (", ", "_").replace (" ", ""), out);

        // Same metric as the window sweep: the largest sample step near each switch vs the
        // steady tone's, taken over the whole render away from the switches (so detuned
        // voices' coinciding resets count in both; for a sine it's the Main segments' slope).
        float steady = 0.0f;
        for (int k = 0; k < numSteps; ++k)
            steady = std::max (steady, maxStep (out, std::max (0.15, k * stepSec + 0.02), (k + 1) * stepSec - 0.02));
        float worst = 0.0f;
        for (int k = 1; k < numSteps; ++k)
            worst = std::max (worst, maxStep (out, k * stepSec - 0.02, k * stepSec + 0.02) / steady);
        check (worst < 1.2f, juce::String::formatted ("%-14s Octave steps M>S>D>T>M>T>S>M: max step at a switch %.2fx the steady tone's", c.name, worst));
    }
}

// ---------------------------------------------------------------------------
// Test G: the triangle is band-limited. Spectrum of the bare oscillator at ~1 kHz: largest
// component that isn't a harmonic below Nyquist (i.e. an alias), re. the fundamental.
// ---------------------------------------------------------------------------
struct AliasReport
{
    double worstDb = -300.0, worstHz = 0.0;
    double bandDb[5] = { -300.0, -300.0, -300.0, -300.0, -300.0 }; // 0-5, 5-10, 10-15, 15-20, 20+ kHz
};

AliasReport aliasReport (const std::function<double (double, double)>& osc, double f0, double fs)
{
    constexpr int order = 17;
    const int n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) (2 * n), 0.0f);
    const double dt = f0 / fs;
    double phase = 0.0;
    for (int k = 0; k < n; ++k)
    {
        // 4-term Blackman-Harris: sidelobes below -92 dB.
        const double a = 2.0 * M_PI * k / (n - 1);
        const double w = 0.35875 - 0.48829 * std::cos (a) + 0.14128 * std::cos (2 * a) - 0.01168 * std::cos (3 * a);
        buf[(size_t) k] = (float) (w * osc (phase, dt));
        phase += dt;
        if (phase >= 1.0)
            phase -= 1.0;
    }
    fft.performFrequencyOnlyForwardTransform (buf.data());
    const double binHz = fs / n;
    const int guard = 6; // main-lobe half width is 4 bins
    float fund = 0.0f;
    for (int k = (int) (f0 / binHz) - guard; k <= (int) (f0 / binHz) + guard; ++k)
        fund = std::max (fund, buf[(size_t) k]);
    AliasReport r;
    for (int k = guard; k < n / 2; ++k)
    {
        const double h = std::round (k * binHz / f0);
        const bool harmonic = h >= 1.0 && h * f0 < fs / 2 && std::abs (k - h * f0 / binHz) <= guard;
        if (harmonic)
            continue;
        const double db = toDb (buf[(size_t) k] / fund);
        const int band = std::min (4, (int) (k * binHz / 5000.0));
        r.bandDb[band] = std::max (r.bandDb[band], db);
        if (db > r.worstDb)
        {
            r.worstDb = db;
            r.worstHz = k * binHz;
        }
    }
    return r;
}

void testTriangleAliasing()
{
    auto blTri = [] (double p, double dt) { return oscSample (Wave::Triangle, p, dt); };
    auto naive = [] (double p, double) { return naiveTriangle (p); };
    // At 48 kHz exactly 1 kHz would fold every alias onto a harmonic, so 1 kHz is measured
    // at 44.1 kHz; at 48 kHz, C6 (1046.5 Hz) and the top of the guitar (1400 Hz).
    struct Case
    {
        double f0, fs;
    };
    for (const Case c : { Case { 1000.0, 44100.0 }, Case { 1046.5, 48000.0 }, Case { 1400.0, 48000.0 } })
    {
        // As the voice plays it: through the default 5 kHz one-pole LPF.
        const float coef = onePoleCoef (c.fs, 5000.0);
        float lp = 0.0f;
        auto blFiltered = [&] (double p, double dt) { return (double) (lp += coef * ((float) oscSample (Wave::Triangle, p, dt) - lp)); };

        const auto bl = aliasReport (blTri, c.f0, c.fs), nv = aliasReport (naive, c.f0, c.fs), blf = aliasReport (blFiltered, c.f0, c.fs);
        auto bands = [] (const AliasReport& r) {
            return juce::String::formatted ("%.0f / %.0f / %.0f / %.0f / %.0f", r.bandDb[0], r.bandDb[1], r.bandDb[2], r.bandDb[3], r.bandDb[4]);
        };
        const juce::String tag = juce::String::formatted ("Triangle %6.1f Hz @ %4.1f kHz", c.f0, c.fs / 1000.0);
        info (tag + ", worst alias per 5 kHz band (dB re. fundamental): band-limited " + bands (bl) + "   naive " + bands (nv)
              + "   band-limited through the 5 kHz LPF " + bands (blf));

        // PolyBLAMP removes aliasing across the audible band; its 2-sample kernel leaves a residue
        // close to Nyquist (reported above), still below the naive triangle's.
        const double below10 = std::max (bl.bandDb[0], bl.bandDb[1]), naiveBelow10 = std::max (nv.bandDb[0], nv.bandDb[1]);
        check (below10 < -80.0 && below10 < naiveBelow10 - 20.0,
               tag + juce::String::formatted (": worst alias below 10 kHz %.1f dB (naive %.1f dB)", below10, naiveBelow10));
        check (bl.worstDb < nv.worstDb - 6.0,
               tag + juce::String::formatted (": worst alias anywhere %.1f dB at %.0f Hz (naive %.1f dB; %.1f dB through the 5 kHz LPF)", bl.worstDb, bl.worstHz,
                                              nv.worstDb, blf.worstDb));
    }
    // Level: the band-limited triangle keeps the naive one's waveform away from the corners.
    double maxDev = 0.0;
    const double dt = 1046.5 / 48000.0;
    for (double p = 0.0; p < 1.0; p += 0.001)
        if (std::min ({ p, std::abs (p - 0.5), 1.0 - p }) > dt)
            maxDev = std::max (maxDev, std::abs (oscSample (Wave::Triangle, p, dt) - naiveTriangle (p)));
    check (maxDev == 0.0, "Triangle is the naive waveform exactly, away from the two corners");
}

// ---------------------------------------------------------------------------
// Tests C and D: Note Release (the hold). The references are the same engine at the longest hold and with none
// (runHoldWith).
// ---------------------------------------------------------------------------

// B3 saw, then `gap` seconds of noise at -30 dBFS RMS (unvoiced), then a D4 saw. With
// silentGap the gap is silence (unvoiced at any Confidence, even the glitchy end's 0.55).
std::vector<float> noteGapNote (double gap, bool silentGap = false)
{
    auto in = steadySaw (246.94, 1.0, 0.4f);
    auto noise = silentGap ? std::vector<float> ((size_t) (gap * kFs), 0.0f) : whiteNoise (gap, 0.0316f * 1.732f, 99);
    auto d4 = steadySaw (293.66, 1.0, 0.4f);
    in.insert (in.end(), noise.begin(), noise.end());
    in.insert (in.end(), d4.begin(), d4.end());
    return in;
}

struct HoldRun
{
    std::vector<float> out, refHold, refMute;
    size_t lost = 0, back = 0;  // output samples where the delayed timeline finally loses / regains pitch
    int strayVoiced = 0;        // voiced hops after the first unvoiced one in the gap (each restarts the hold)
    size_t holdLen = 0, fadeOut = 0, fadeIn = 0;
};

// Any configuration, against the same engine and settings at the longest hold the engine takes (refHold: kHoldMaxMs, 2000 ms,
// longer than any Note Release option) and with none (refMute). The sputter fix is off (its grace lengthens the hold by a
// dropout's grace).
void holdEngine (Engine& e)
{
    e.prepare (kFs, 64);
    e.setSputterFix (false);
}
HoldRun runHoldWith (const std::vector<float>& in, double gapStart, double gapEnd, const EngineParams& p)
{
    HoldRun r;
    Engine e;
    holdEngine (e);
    // The hold runs from the start of the gap's final unvoiced run (after any voiced flicker
    // at the edge of the gap) to the first voiced hop of the next note.
    int64_t firstUnvoiced = -1, lostAt = -1, backAt = -1;
    e.hopListener = [&] (const HopInfo& h) {
        const double t = (double) h.time / kFs;
        if (backAt >= 0 || t < gapStart - 0.1)
            return;
        if (! h.detector.voiced)
        {
            if (firstUnvoiced < 0)
                firstUnvoiced = h.time;
            if (lostAt < 0)
                lostAt = h.time;
        }
        else if (firstUnvoiced >= 0)
        {
            if (t >= gapEnd - 0.1)
                backAt = h.time;
            else
            {
                ++r.strayVoiced;
                lostAt = -1;
            }
        }
    };
    r.out = runEngine (e, in, p);

    const auto d = effectiveDetection (p);
    for (float hold : { kHoldMaxMs, 0.0f })
    {
        Engine ref;
        holdEngine (ref);
        auto q = p;
        q.holdMs = hold;
        (hold > 0.0f ? r.refHold : r.refMute) = runEngine (ref, in, q);
    }

    const int L = latencyFor (kFs, d.windowHz);
    r.lost = (size_t) (lostAt + L);
    r.back = (size_t) (backAt + L);
    r.holdLen = (size_t) std::lround (d.holdMs * 0.001 * kFs);
    r.fadeOut = (size_t) msToSamples (kFs, 15.0);
    r.fadeIn = (size_t) msToSamples (kFs, 5.0);
    return r;
}

HoldRun runHold (const std::vector<float>& in, double gapStart, double gapEnd, float holdMs)
{
    auto p = defaultParams();
    p.holdMs = holdMs;
    return runHoldWith (in, gapStart, gapEnd, p);
}

// First sample index in [from, to) where a and b differ in value (to if none).
size_t firstValueDiff (const std::vector<float>& a, const std::vector<float>& b, size_t from = 0, size_t to = SIZE_MAX)
{
    to = std::min ({ to, a.size(), b.size() });
    for (size_t n = from; n < to; ++n)
        if (! std::equal_to<float>() (a[n], b[n]))
            return n;
    return to;
}

// Largest deviation of out from ramp(k) * ref over [start, start + len), relative to ref's peak there.
double rampError (const std::vector<float>& out, const std::vector<float>& ref, size_t start, size_t len, bool rising)
{
    double err = 0.0, peak = 1e-12;
    for (size_t k = 0; k < len; ++k)
    {
        const double g = rising ? (double) (k + 1) / (double) len : 1.0 - (double) (k + 1) / (double) len;
        err = std::max (err, std::abs (out[start + k] - g * ref[start + k]));
        peak = std::max (peak, (double) std::abs (ref[start + k]));
    }
    return err / peak;
}

bool allZero (const std::vector<float>& x, size_t from, size_t to)
{
    for (size_t n = from; n < std::min (to, x.size()); ++n)
        if (x[n] != 0.0f)
            return false;
    return true;
}

void testHold()
{
    // Pitch cut off mid-note for 3 s, longer than the longest hold (the reference's 2000 ms); up to Note Release Long (1500 ms).
    const double gapStart = 1.0, gapEnd = 4.0;
    const auto in = noteGapNote (gapEnd - gapStart);

    for (float holdMs : { 0.0f, 400.0f, 1500.0f })
    {
        const auto r = runHold (in, gapStart, gapEnd, holdMs);
        const juce::String tag = juce::String::formatted ("Hold %4.0f ms", holdMs);
        writeWav ("testC_hold" + juce::String (holdMs, 0), r.out);
        if (r.strayVoiced > 0)
            info (tag + ": WARNING " + juce::String (r.strayVoiced) + " voiced hop(s) inside the noise gap");

        // Sounds exactly like the longest hold until the hold runs out...
        const size_t fadeStart = r.lost + r.holdLen;
        const size_t diff = firstValueDiff (r.out, r.refHold);
        const double holdDb = toDb (rms (r.out, r.lost, fadeStart) / std::max (1e-12, rms (r.out, r.lost - (size_t) (0.3 * kFs), r.lost)));
        check (diff == fadeStart && r.strayVoiced == 0,
               tag + ": identical to the longest hold for " + juce::String (1000.0 * ((double) diff - (double) r.lost) / kFs, 2) + " ms after pitch is lost (expected "
                   + juce::String (holdMs, 0) + " ms)" + (holdMs > 0.0f ? juce::String::formatted (", level %+.1f dB re. the note (envelope follows the input)", holdDb) : juce::String()));

        // ...then fades linearly to silence over 15 ms, and stays silent until pitch returns.
        const double err = rampError (r.out, r.refHold, fadeStart, r.fadeOut, false);
        const bool silent = allZero (r.out, fadeStart + r.fadeOut, r.back);
        check (err < 1e-5 && silent, tag + ": then a 15 ms linear fade (max error " + juce::String (err, 8) + " of peak) and silence until pitch returns");

        // From the end of the fade on (silence, then the 5 ms fade-in), it is Note Release 0's (a mute).
        if (holdMs > 0.0f)
        {
            const size_t d = firstValueDiff (r.out, r.refMute, fadeStart + r.fadeOut);
            check (d == r.out.size(), tag + ": after the fade, sample-identical to Note Release 0 (incl. the fade-in on the next note)");
        }
    }
}

void testHoldRecovery()
{
    // Pitch returns during the hold: no fade at all, so the whole render is the longest hold's.
    {
        const auto in = noteGapNote (0.3);
        for (float holdMs : { 400.0f, 2000.0f })
        {
            const auto r = runHold (in, 1.0, 1.3, holdMs);
            writeWav ("testD_back_during_hold" + juce::String (holdMs, 0), r.out);
            const size_t d = firstValueDiff (r.out, r.refHold);
            const double gapMs = 1000.0 * ((double) r.back - (double) r.lost) / kFs;
            check (d == r.out.size() && r.strayVoiced == 0 && r.back < r.lost + r.holdLen,
                   juce::String::formatted ("Hold %4.0f ms, pitch back after %.0f ms: no fade (sample-identical to the longest hold throughout)", holdMs, gapMs));
        }
    }

    // Pitch returns after the fade: silent, then a 5 ms linear fade-in, then the full note.
    {
        const auto in = noteGapNote (1.0);
        const auto r = runHold (in, 1.0, 2.0, 400.0f);
        writeWav ("testD_back_after_fade", r.out);
        const bool silentBefore = r.lost + r.holdLen + r.fadeOut < r.back && allZero (r.out, r.lost + r.holdLen + r.fadeOut, r.back);
        const double err = rampError (r.out, r.refHold, r.back, r.fadeIn, true);
        const size_t d = firstValueDiff (r.out, r.refHold, r.back + r.fadeIn);
        check (silentBefore && err < 1e-5 && d == r.out.size() && r.strayVoiced == 0,
               "Hold  400 ms, pitch back after " + juce::String (1000.0 * ((double) r.back - (double) r.lost) / kFs, 0)
                   + " ms: 5 ms linear fade-in (max error " + juce::String (err, 8) + " of peak), then identical to the longest hold");
    }
}

// ---------------------------------------------------------------------------
// Comparing renders, and the synthetic phrases many tests (and the grid) play.
// ---------------------------------------------------------------------------

// Counts samples that differ in value (+0.0 vs -0.0 counted separately: same sample value).
struct Diff
{
    int values = 0, signOfZeroOnly = 0;
    size_t first = 0;
};
Diff compareSamples (const std::vector<float>& a, const std::vector<float>& b)
{
    Diff d;
    d.first = a.size();
    for (size_t n = 0; n < std::min (a.size(), b.size()); ++n)
    {
        if (std::memcmp (&a[n], &b[n], sizeof (float)) == 0)
            continue;
        if (std::equal_to<float>() (a[n], b[n]))
            ++d.signOfZeroOnly;
        else if (d.values++ == 0)
            d.first = n;
    }
    d.values += (int) (std::max (a.size(), b.size()) - std::min (a.size(), b.size()));
    return d;
}

// In the playable zone: a pluck that slides B3 -> E4, a gap of noise and silence (0.7 s of
// lost pitch), a loud note, a quiet note, a fast glide G3 -> G5, then a pluck left to ring
// out and die (the hold runs past the end).
std::vector<float> regressionPhrase (double fs)
{
    std::vector<float> x;
    auto add = [&] (std::vector<float> v) { x.insert (x.end(), v.begin(), v.end()); };
    add (ksPluck ([] (double t) { return 246.94 * std::pow (2.0, 5.0 / 12.0 * std::min (1.0, t / 1.5)); }, 2.0, 0.5f, ksLossFor (246.94, 6.0), 5, fs));
    add (whiteNoise (0.4, 0.03f, 6));
    add (std::vector<float> ((size_t) (0.3 * fs), 0.0f));
    add (ksPluck ([] (double) { return 293.66; }, 1.0, 0.9f, ksLossFor (293.66, 3.0), 7, fs));
    add (ksPluck ([] (double) { return 392.0; }, 1.0, 0.05f, ksLossFor (392.0, 3.0), 8, fs));
    add (sawFromTrajectory ([] (double t) { return 196.0 * std::pow (2.0, 2.0 * std::min (1.0, t / 0.6)); }, 1.0, 0.3f, fs));
    add (ksPluck ([] (double) { return 329.63; }, 2.5, 0.5f, ksLossFor (329.63, 0.8), 9, fs));
    return x;
}

// The regression phrase's plucks without its slides, each after a gap, so no note change is connected:
// v0.5.1's level inheritance changes Sustain at connected changes, so the Sustain > 0 measurements use this.
std::vector<float> separatePlucks (double fs)
{
    std::vector<float> x;
    auto add = [&] (std::vector<float> v) { x.insert (x.end(), v.begin(), v.end()); };
    auto gap = [&] (double s) { x.insert (x.end(), (size_t) (s * fs), 0.0f); };
    add (ksPluck ([] (double) { return 246.94; }, 1.5, 0.5f, ksLossFor (246.94, 6.0), 5, fs));
    add (whiteNoise (0.4, 0.03f, 6));
    gap (0.3);
    add (ksPluck ([] (double) { return 293.66; }, 1.0, 0.9f, ksLossFor (293.66, 3.0), 7, fs));
    gap (0.06);
    add (ksPluck ([] (double) { return 392.0; }, 1.0, 0.05f, ksLossFor (392.0, 3.0), 8, fs));
    gap (0.06);
    add (ksPluck ([] (double) { return 329.63; }, 2.5, 0.5f, ksLossFor (329.63, 0.8), 9, fs));
    return x;
}

// ---------------------------------------------------------------------------
// Test H: fixed detection. With Manual off, Confidence and Hysteresis are always 0.8125 / 11.25 c
// (v0.4.3's Deliberate); Latency / Minimum Pitch (Window) and Note Release (Hold) stay as set, and
// Sustain doesn't touch detection. The engine uses them.
// ---------------------------------------------------------------------------
void testFixedDetection()
{
    auto same = [] (float a, float b) { return std::equal_to<float>() (a, b); };
    bool exact = true;
    for (auto [window, hold] : { std::pair { defaultWindowHz(), 350.0f }, std::pair { (float) kWindowMaxHz, 0.0f }, std::pair { (float) derivedWindowMinHz(), 500.0f } })
        for (float sustain : { 0.0f, 50.0f, 100.0f })
        {
            EngineParams p = v051Defaults();
            p.windowHz = window;
            p.holdMs = hold;
            p.sustainPercent = sustain;
            const auto d = effectiveDetection (p);
            exact = exact && same (d.confidence, 0.8125f) && same (d.hysteresisCents, 11.25f) && same (d.windowHz, window) && same (d.holdMs, hold);
        }
    check (exact, "Confidence 0.8125, Hysteresis 11.25 c exactly; Latency / Minimum Pitch and Note Release as set "
                  "(default, 424.26 Hz / 0 ms, minimum / 500 ms; each at Sustain 0 / 50 / 100%)");
    const EngineParams d = v051Defaults();
    check (same (d.confidenceThreshold, kConfidence) && same (d.hysteresisCents, kHysteresisCents) && ! d.manual && same (d.sustainPercent, 0.0f)
               && same (d.holdMs, 350.0f) && same (d.windowHz, defaultWindowHz()),
           "Defaults: Sustain 0%, Note Release 350 ms, Latency / Minimum Pitch 240.62 Hz; ADVANCED Confidence / Hysteresis = the fixed values, Manual off");

    // The engine follows them: identical to the longest hold until exactly Note Release (350 ms, the default) after pitch is lost.
    {
        const auto in = noteGapNote (2.0, true);
        const EngineParams p = v051Defaults();
        const auto run = runHoldWith (in, 1.0, 3.0, p);
        const size_t diff = firstValueDiff (run.out, run.refHold);
        check (diff == run.lost + run.holdLen,
               juce::String::formatted ("Defaults (Confidence 0.8125 / Hysteresis 11.25 c) hold %.1f ms after pitch is lost (expected %.0f)",
                                        1000.0 * ((double) diff - (double) run.lost) / kFs, p.holdMs)
                   + (run.strayVoiced > 0 ? juce::String::formatted ("; %d voiced hop(s) of flicker as the note ends", run.strayVoiced) : juce::String()));
    }
}

// ---------------------------------------------------------------------------
// Test I: Manual override (as with the ADVANCED panel shown: EngineParams::manual is Manual in
// effect). Manual on: the ADVANCED Confidence and Hysteresis are used instead of the fixed ones.
// Manual off: they're ignored. Window, Hold and Sustain are direct either way. At defaults, on = off.
// ---------------------------------------------------------------------------
void testManual()
{
    const auto in = regressionPhrase (kFs);
    auto render = [&] (const EngineParams& p) {
        Engine e;
        e.prepare (kFs, 64);
        return runEngine (e, in, p);
    };
    auto sameAs = [] (const std::vector<float>& a, const std::vector<float>& b) { return compareSamples (a, b).values == 0; };

    EngineParams defaults, manualDefaults;
    manualDefaults.manual = true;
    const auto ref = render (defaults);
    check (sameAs (render (manualDefaults), ref), "At defaults, switching Manual on gives bit-identical output (ADVANCED defaults = the fixed 0.8125 / 11.25 c)");

    EngineParams offOdd = defaults; // Manual off: ADVANCED values far from the fixed ones
    offOdd.confidenceThreshold = 0.99f;
    offOdd.hysteresisCents = 40.0f;
    check (sameAs (render (offOdd), ref), "Manual off: the ADVANCED parameters are ignored (Confidence 0.99, Hysteresis 40 change nothing)");

    // Manual on: the ADVANCED values are what's used, individually.
    auto same = [] (float a, float b) { return std::equal_to<float>() (a, b); };
    EngineParams loose = v051Defaults();
    loose.manual = true;
    loose.confidenceThreshold = 0.55f;
    loose.hysteresisCents = 0.0f;
    const auto dl = effectiveDetection (loose);
    EngineParams confOnly = manualDefaults, hystOnly = manualDefaults;
    confOnly.confidenceThreshold = 0.6f;
    hystOnly.hysteresisCents = 30.0f;
    const auto dc = effectiveDetection (confOnly), dh = effectiveDetection (hystOnly);
    check (same (dl.confidence, 0.55f) && same (dl.hysteresisCents, 0.0f) && same (dc.confidence, 0.6f) && same (dc.hysteresisCents, kHysteresisCents)
               && same (dh.confidence, kConfidence) && same (dh.hysteresisCents, 30.0f) && ! sameAs (render (loose), ref),
           "Manual on: Confidence and Hysteresis take the ADVANCED values, each on its own (0.55 / 0 c; 0.6 alone; 30 c alone), and the output changes");
    {
        const auto run = runHoldWith (noteGapNote (2.0, true), 1.0, 3.0, loose);
        const size_t diff = firstValueDiff (run.out, run.refHold);
        check (diff == run.lost + run.holdLen, "Manual on at 0.55 / 0 c: holds exactly Note Release after pitch is lost");
    }

    // Window, Hold and Sustain aren't Manual's: the same direct values sound the same with Manual on or off.
    EngineParams onDirect = manualDefaults, offDirect = defaults;
    onDirect.windowHz = offDirect.windowHz = 300.0f;
    onDirect.holdMs = offDirect.holdMs = 150.0f;
    onDirect.sustainPercent = offDirect.sustainPercent = 60.0f;
    check (sameAs (render (onDirect), render (offDirect)) && ! sameAs (render (offDirect), ref),
           "Latency / Minimum Pitch 300 Hz, Note Release 150 ms, Sustain 60%: identical with Manual on (at the fixed values) and off, and different from the defaults");
}

// ---------------------------------------------------------------------------
// Test R: direct Latency / Minimum Pitch (Window) and Note Release (Hold). They set the effective
// values whatever Manual and Sustain are, and the engine follows them.
// ---------------------------------------------------------------------------
void testDirectWindowHold()
{
    auto same = [] (float a, float b) { return std::equal_to<float>() (a, b); };
    const float wMin = (float) derivedWindowMinHz();
    bool allSet = true;
    int n = 0;
    for (float window : { wMin, defaultWindowHz(), 271.3f, 300.0f, 377.0f, (float) kWindowMaxHz })
        for (float hold : { 0.0f, 150.0f, 350.0f, 500.0f })
            for (float sustain : { 0.0f, 100.0f })
                for (bool manual : { false, true })
                {
                    EngineParams p = v051Defaults();
                    p.windowHz = window;
                    p.holdMs = hold;
                    p.sustainPercent = sustain;
                    p.manual = manual;
                    const auto d = effectiveDetection (p);
                    allSet = allSet && same (d.windowHz, window) && same (d.holdMs, hold);
                    ++n;
                }
    check (allSet, juce::String::formatted ("%d combinations of Latency / Minimum Pitch, Note Release, Sustain and Manual: effective Window and Hold = as set", n));

    // Latency follows Window.
    juce::String lats;
    bool latOk = true;
    for (float window : { wMin, defaultWindowHz(), 300.0f, (float) kWindowMaxHz })
    {
        EngineParams p = v051Defaults();
        p.windowHz = window;
        Engine e;
        e.prepare (kFs, 64);
        runEngine (e, std::vector<float> (6400, 0.0f), p);
        latOk = latOk && e.currentLatencySamples() == latencyFor (kFs, window);
        lats << juce::String (window, 2) << " Hz: " << juce::String (1000.0 * e.currentLatencySamples() / kFs, 2) << " ms  ";
    }
    check (latOk, "Engine latency follows Latency / Minimum Pitch: " + lats.trimEnd());

    // Note Release: the pitch holds for exactly Note Release after pitch is lost (identical to the longest hold until then). The note is B3, so the window is the minimum.
    for (float hold : { 150.0f, 500.0f })
    {
        const auto in = noteGapNote (2.0, true);
        EngineParams p = v051Defaults();
        p.windowHz = wMin;
        p.holdMs = hold;
        const auto r = runHoldWith (in, 1.0, 3.0, p);
        const size_t diff = firstValueDiff (r.out, r.refHold);
        check (diff == r.lost + r.holdLen,
               juce::String::formatted ("Window minimum, Note Release %3.0f ms: holds %.1f ms after pitch is lost", hold, 1000.0 * ((double) diff - (double) r.lost) / kFs));
    }
}

// ---------------------------------------------------------------------------
// Test T: hidden Manual. With the ADVANCED panel hidden, a session with Manual saved on behaves
// as Manual off; with it shown, Manual overrides Confidence and Hysteresis only. (The plugin
// applies manualInEffect() when it reads the parameters; GTTSnapshot checks that end to end.)
// ---------------------------------------------------------------------------
void testHiddenManual()
{
    info (juce::String ("shipped: kShowAdvanced = ") + (kShowAdvanced ? "true (panel shown)" : "false (panel hidden)"));
    check (! manualInEffect (true, false) && ! manualInEffect (false, false) && manualInEffect (true, true) && ! manualInEffect (false, true),
           "Manual in effect: only when saved on and the panel is shown");

    const auto in = regressionPhrase (kFs);
    auto render = [&] (const EngineParams& p) {
        Engine e;
        e.prepare (kFs, 64);
        return runEngine (e, in, p);
    };
    auto sameAs = [] (const std::vector<float>& a, const std::vector<float>& b) { return compareSamples (a, b).values == 0; };

    // A session saved with Manual on, at ADVANCED values far from the fixed ones, and direct
    // Window 300 / Hold 400.
    auto saved = [] (bool showAdvanced) {
        EngineParams p = v051Defaults();
        p.windowHz = 300.0f;
        p.holdMs = 400.0f;
        p.confidenceThreshold = 0.99f;
        p.hysteresisCents = 40.0f;
        p.manual = manualInEffect (true, showAdvanced);
        return p;
    };
    EngineParams manualOff = saved (true);
    manualOff.manual = false;

    const auto hidden = saved (false), shown = saved (true);
    const auto h = effectiveDetection (hidden), s = effectiveDetection (shown);
    const auto hiddenOut = render (hidden);
    check (sameAs (hiddenOut, render (manualOff)) && std::equal_to<float>() (h.confidence, kConfidence)
               && std::equal_to<float>() (h.hysteresisCents, kHysteresisCents),
           juce::String::formatted ("Panel hidden: Manual saved on = Manual off, bit for bit (Confidence %.4f, Hysteresis %.2f c: the fixed values)", h.confidence,
                                    h.hysteresisCents));
    check (std::abs (s.confidence - 0.99f) < 1e-6f && std::abs (s.hysteresisCents - 40.0f) < 1e-5f && std::equal_to<float>() (s.windowHz, 300.0f)
               && std::equal_to<float>() (s.holdMs, 400.0f) && ! sameAs (render (shown), hiddenOut),
           "Panel shown: Manual overrides Confidence (0.99) and Hysteresis (40 c) only; Latency / Minimum Pitch 300 Hz and Note Release 400 ms as set; the output changes");
}

// ---------------------------------------------------------------------------
// Test K: Retune Speed. Fast / Medium / Slow are glide time constants of 0 / 40 / 120 ms:
// fitted from the quantizer's target T, hop by hop, after a note change within a voiced run.
// (A note that starts after an unvoiced hop -- a hard step, a re-pick -- latches fresh and
// doesn't glide, as since v0.1; so the input is a fast slide, which stays voiced.)
// ---------------------------------------------------------------------------
void testRetuneSpeed()
{
    // A saw sliding B3 -> D4 over 20 ms at t = 1 s.
    std::vector<float> in = sawFromTrajectory ([] (double t) { return 246.94 * std::pow (293.66 / 246.94, std::clamp ((t - 1.0) / 0.02, 0.0, 1.0)); }, 2.0, 0.4f);
    const char* names[3] = { "Fast", "Medium", "Slow" };
    for (int k = 0; k < 3; ++k)
    {
        auto p = defaultParams();
        p.retuneSpeed = (RetuneSpeed) k;
        auto hops = collectHops (in, p, juce::String ("testK_retune_") + names[k]);
        // The hop where N reaches the new note (62) for good, then the glide toward it.
        size_t first = 0;
        bool allVoiced = true;
        for (size_t i = 1; i < hops.size(); ++i)
        {
            if (hops[i].noteN == 62 && hops[i - 1].noteN != 62)
                first = i;
            if ((double) hops[i].time / kFs > 0.5)
                allVoiced = allVoiced && hops[i].detector.voiced;
        }
        double tau = 0.0;
        int fits = 0;
        bool snapped = first > 0 && allVoiced;
        for (size_t i = first; snapped && i < hops.size(); ++i)
            snapped = std::abs (hops[i].target - (double) hops[i].noteN) < 1e-9;
        if (k > 0 && first > 0)
        {
            // T_i = N - (N - T_{i-1}) exp(-hop / tau): tau from each hop while still gliding (>1 c left).
            for (size_t i = first + 1; i < hops.size() && hops[i].noteN == 62 && 62.0 - hops[i].target > 0.01; ++i)
            {
                tau += -(double) hopFor (kFs) / kFs / std::log ((62.0 - hops[i].target) / (62.0 - hops[i - 1].target));
                ++fits;
            }
            tau /= std::max (1, fits);
        }
        const double expect = retuneMsFor ((RetuneSpeed) k);
        const bool ok = k == 0 ? snapped : (first > 0 && allVoiced && fits > 3 && std::abs (1000.0 * tau - expect) < 0.01);
        check (ok, k == 0 ? juce::String ("Retune Speed Fast: hard snap, T = N on every hop through the slide")
                          : juce::String::formatted ("Retune Speed %-6s: glide time constant %.3f ms (expected %.0f; fitted over %d hops)", names[k], 1000.0 * tau,
                                                     expect, fits));
    }
    auto is = [] (float a, float b) { return std::equal_to<float>() (a, b); };
    check (is (retuneMsFor (RetuneSpeed::Fast), 0.0f) && is (retuneMsFor (RetuneSpeed::Medium), 40.0f) && is (retuneMsFor (RetuneSpeed::Slow), 120.0f),
           "Retune Speed table: Fast 0 ms, Medium 40 ms, Slow 120 ms");
}

// ---------------------------------------------------------------------------
// Test P: the 2+ NOTES? hint fires on two decaying plucked tones a few semitones apart and
// not on a single pluck at the same level. A probe reports every interval and both ends of
// the main control (the hint only infers a chord from low confidence).
// ---------------------------------------------------------------------------
struct HintResult
{
    bool fired = false;
    double flaggedMs = 0.0, firstMs = -1.0; // total time flagged; first flag after the onset
};

HintResult runHint (const std::vector<float>& in, const EngineParams& p, const ChordHint& hint = {})
{
    HintResult r;
    Engine e;
    e.prepare (kFs, 64);
    e.chordHint = hint;
    const double hopMs = 1000.0 * hopFor (kFs) / kFs;
    e.hopListener = [&] (const HopInfo& h) {
        if (! h.twoPlusNotes)
            return;
        r.fired = true;
        r.flaggedMs += hopMs;
        if (r.firstMs < 0.0)
            r.firstMs = 1000.0 * (double) h.time / kFs;
    };
    runEngine (e, in, p);
    return r;
}

// Plucks at midi notes (2 s, T60 3 s), summed, at 0.5 peak: the same level for 1 or 2 tones.
std::vector<float> plucks (std::initializer_list<double> notes, uint32_t seed)
{
    std::vector<float> sum ((size_t) (2.0 * kFs), 0.0f);
    for (double n : notes)
    {
        const double hz = hzFromMidi (n);
        const auto pl = ksPluck ([hz] (double) { return hz; }, 2.0, 0.5f, ksLossFor (hz, 3.0), seed++);
        for (size_t i = 0; i < sum.size(); ++i)
            sum[i] += pl[i];
    }
    float peak = 0.0f;
    for (auto v : sum)
        peak = std::max (peak, std::abs (v));
    for (auto& v : sum)
        v *= 0.5f / peak;
    return sum;
}

void testChordHint()
{
    info (juce::String::formatted ("thresholds: RMS > %.0f dBFS and confidence < %.2f for > %.0f ms sets it; quiet or confident for %.0f ms clears it",
                                   ChordHint::kMinRmsDb, ChordHint::kMaxConfidence, ChordHint::kOnMs, ChordHint::kOffMs));
    const EngineParams defaults = v051Defaults();

    // The spec's case: two plucks a few semitones apart vs a single pluck, same level.
    for (double root : { 60.0, 64.0, 69.0 })
    {
        const auto two = runHint (plucks ({ root, root + 3.0 }, 31), defaults);
        const auto two2 = runHint (plucks ({ root, root + 2.0 }, 41), defaults);
        const auto one = runHint (plucks ({ root }, 31), defaults);
        check (two.fired && two2.fired && ! one.fired,
               juce::String::formatted ("Root MIDI %.0f: minor 3rd fires (%.0f ms flagged, first at %.0f ms), major 2nd fires (%.0f ms), single pluck doesn't (%.0f ms)", root,
                                        two.flaggedMs, two.firstMs, two2.flaggedMs, one.flaggedMs));
    }
    // Single notes in the playable range (from the lowest reliable note up) don't fire, at
    // either end of the zone and in the middle. Below the range a single note also reads as unsure.
    for (const auto& z : zoneSettings())
    {
        const auto& p = z.p;
        const int lowest = lowestReliableNote (kFs, p.windowHz);
        int fired = 0, n = 0;
        for (int note = lowest; note <= 84; ++note, ++n)
            fired += runHint (plucks ({ (double) note }, 81 + (uint32_t) note), p).fired ? 1 : 0;
        const bool below = runHint (plucks ({ (double) lowest - 3 }, 9), p).fired;
        check (fired == 0, juce::String::formatted ("%-22s: none of %d single plucks from the lowest reliable note (MIDI %d) to C6 fire; "
                                                    "(info: one 3 semitones below it %s)",
                                                    z.name, n, lowest, below ? "fires" : "doesn't"));
    }

    // Probe: every interval on C4, across the zone (info).
    const char* names[] = { "m2", "M2", "m3", "M3", "P4", "TT", "P5", "m6", "M6", "oct" };
    for (const auto& z : zoneSettings())
    {
        const auto& p = z.p;
        juce::String line;
        for (int k = 1; k <= 12; ++k)
        {
            if (k == 10 || k == 11)
                continue;
            const auto r = runHint (plucks ({ 60.0, 60.0 + k }, 51), p);
            line << names[k == 12 ? 9 : k - 1] << (r.fired ? juce::String::formatted (" %.0fms ", r.flaggedMs) : juce::String (" - "));
        }
        const auto single = runHint (plucks ({ 60.0 }, 51), p);
        info (juce::String::formatted ("%-22s, intervals on C4 (time flagged): ", z.name) + line.trimEnd()
              + (single.fired ? "; single C4 FIRES" : "; single C4 -"));
    }

    // It clears: quiet input, or confidence back, for 200 ms.
    {
        auto in = plucks ({ 64.0, 67.0 }, 61);
        in.resize ((size_t) (1.0 * kFs));
        in.insert (in.end(), (size_t) (0.5 * kFs), 0.0f);  // quiet
        auto single = plucks ({ 64.0 }, 61);
        in.insert (in.end(), single.begin(), single.begin() + (int) (1.0 * kFs));
        Engine e;
        e.prepare (kFs, 64);
        bool atChord = false, atQuiet = true, atSingle = true;
        e.hopListener = [&] (const HopInfo& h) {
            const double t = (double) h.time / kFs;
            if (t > 0.2 && t < 0.6)
                atChord = atChord || h.twoPlusNotes;
            if (t > 1.0 + 0.25 && t < 1.5)
                atQuiet = atQuiet && ! h.twoPlusNotes;
            if (t > 1.6 && t < 2.4)
                atSingle = atSingle && ! h.twoPlusNotes;
        };
        runEngine (e, in, defaults);
        check (atChord && atQuiet && atSingle, "Hint set during an interval, cleared within 250 ms of silence, stays clear for the next single note");
    }
}

// Probe: which confidence threshold separates intervals from single notes. For each
// candidate: how many of 30 intervals (m2..M7 and the octave, roots C4 and G4, at 0 / 50 / 100%)
// fire, and how many of the in-range single plucks do.
void probeChordThreshold()
{
    for (float maxConf : { 0.5f, 0.7f, 0.8f, 0.85f, 0.9f, 0.95f })
    {
        ChordHint hint;
        hint.maxConfidence = maxConf;
        int intervals = 0, intervalsFired = 0, singles = 0, singlesFired = 0, octavesFired = 0;
        juce::String falseSingles;
        for (const auto& z : zoneSettings())
        {
            const auto& p = z.p;
            const int lowest = lowestReliableNote (kFs, p.windowHz);
            for (double root : { 60.0, 67.0 })
            {
                if (root < lowest)
                    continue; // the root itself can't track at this setting
                for (int k = 1; k <= 12; ++k)
                {
                    const bool fired = runHint (plucks ({ root, root + k }, 51 + (uint32_t) k), p, hint).fired;
                    if (k == 12)
                        octavesFired += fired ? 1 : 0;
                    else
                    {
                        ++intervals;
                        intervalsFired += fired ? 1 : 0;
                    }
                }
            }
            for (int n = lowest; n <= 84; n += 2)
            {
                ++singles;
                if (runHint (plucks ({ (double) n }, 71 + (uint32_t) n), p, hint).fired)
                {
                    ++singlesFired;
                    falseSingles << z.name << ":" << n << " ";
                }
            }
        }
        info (juce::String::formatted ("confidence < %.2f: %2d / %d intervals fire, %d octave(s), %d / %d in-range single plucks fire", maxConf, intervalsFired, intervals,
                                       octavesFired, singlesFired, singles)
              + (falseSingles.isNotEmpty() ? " (" + falseSingles.trimEnd() + ")" : juce::String()));
    }
}

void testSwitchingCrossfades()
{
    // Changing Hold while voiced: the voice stays open, no jumps.
    auto in = sine (440.0, 2.0, 0.4f);
    Engine e;
    e.prepare (kFs, 64);
    auto out = runScheduled (e, in, [] (double t) {
        auto p = voiceParams (Wave::Sine);
        p.holdMs = t < 0.7 ? 2000.0f : (t < 1.4 ? 0.0f : 400.0f);
        return p;
    });
    check (std::isfinite (maxStep (out, 0.0, 2.0)) && maxStep (out, 0.6, 1.5) < 1.5f * maxStep (out, 0.2, 0.6),
           "Changing Hold while voiced is seamless");
}

// ---------------------------------------------------------------------------
// Test W (probe): Note Release on a muted pluck, at Sustain 0. Note Release only keeps the pitch; the level
// follows the guitar with the envelope's 120 ms release, so the tail grows with it up to ~350 ms
// and barely after. A plucked E4 (T60 3 s) is muted at 1.0 s (10 ms); the time the output falls
// 40 dB below its peak (20 ms RMS, latency removed) must grow monotonically with Note Release and
// match the v0.4.2 diagnosis: 1.02 / 1.16 / 1.21 s at 0 / 150 / 350 ms.
// ---------------------------------------------------------------------------
std::vector<float> mutedPluck()
{
    const double hz = 329.63;
    auto in = ksPluck ([hz] (double) { return hz; }, 3.0, 0.5f, ksLossFor (hz, 3.0), 3);
    for (size_t n = (size_t) (1.0 * kFs); n < in.size(); ++n)
        in[n] *= (float) std::max (0.0, 1.0 - ((double) n / kFs - 1.0) / 0.01);
    return in;
}

double muteTail40dB (float releaseMs, float sustainPercent = 0.0f, Wave wave = Wave::Saw, std::vector<float>* outCopy = nullptr, bool sputterFix = true)
{
    const auto in = mutedPluck();
    EngineParams p = v051Defaults();
    p.holdMs = releaseMs;
    p.sustainPercent = sustainPercent;
    p.wave = wave;
    Engine e;
    e.prepare (kFs, 64);
    e.setSputterFix (sputterFix);
    const auto out = runEngine (e, in, p);
    if (outCopy != nullptr)
        *outCopy = out;

    // 20 ms RMS every 5 ms; the last frame within 40 dB of the loudest.
    std::vector<double> db;
    for (size_t c = 0; c < out.size(); c += 240)
    {
        double sum = 0.0;
        size_t n = 0;
        for (size_t k = c > 480 ? c - 480 : 0; k < std::min (out.size(), c + 480); ++k, ++n)
            sum += (double) out[k] * out[k];
        db.push_back (10.0 * std::log10 (sum / (double) std::max<size_t> (1, n) + 1e-20));
    }
    const double peak = *std::max_element (db.begin(), db.end());
    double last = 0.0;
    for (size_t f = 0; f < db.size(); ++f)
        if (db[f] > peak - 40.0)
            last = (double) (f * 240) / kFs;
    return last - (double) e.currentLatencySamples() / kFs;
}

// The Note Release options (v0.5.9: Fast / Medium / Long = 0 / 500 / 1500 ms; v0.5.2 .. v0.5.8 had Instant / Fast / Medium / Long
// (/ Extra Long) = 0 / 250 / 500 / 1000 (/ 2000) ms, still reported below as the engine's times). Each holds the pitch for exactly its
// time (against the longest hold, as test R); the muted pluck's tail at Sustain 0 (the level follows the guitar, so past ~350 ms it barely
// grows), 75% and 100% (a fade over Note Release), longer with each option. Fast keeps Instant's 20 ms grace (kReleaseGraceInstantMs),
// Medium and Long the 40 ms one; the default is Medium.
void testNoteReleaseTail()
{
    struct Option
    {
        const char* name;
        float ms;
        double expected0; // s, Sustain 0; < 0: report only
        double graceMs;
    };
    const Option options[] = { { "Fast", noteReleaseMsFor (NoteRelease::Fast), 1.02, kReleaseGraceInstantMs },
                               { "Medium", noteReleaseMsFor (NoteRelease::Medium), -1.0, kReleaseGraceMs },
                               { "Long", noteReleaseMsFor (NoteRelease::Long), -1.0, kReleaseGraceMs } };
    const float times[3] = { 0.0f, 500.0f, 1500.0f };
    check (kDefaultNoteRelease == NoteRelease::Medium && std::equal_to<float>() (EngineParams {}.holdMs, 500.0f),
           juce::String::formatted ("The default is Medium: EngineParams {} holds %.0f ms", EngineParams {}.holdMs));
    double prev0 = 0.0, prev75 = 0.0, prev100 = 0.0;
    bool monotonic = true;
    juce::String line0, line75, line100;
    for (int k = 0; k < 3; ++k)
    {
        const auto& o = options[k];
        const float ms = o.ms;

        // The pitch holds for exactly the option's time after pitch is lost (window minimum: the note is B3).
        EngineParams p = v051Defaults();
        p.windowHz = (float) derivedWindowMinHz();
        p.holdMs = ms;
        const auto r = runHoldWith (noteGapNote (2.0, true), 1.0, 3.0, p);
        const size_t diff = firstValueDiff (r.out, r.refHold);
        check (std::equal_to<float>() (ms, times[k]) && diff == r.lost + r.holdLen,
               juce::String::formatted ("Note Release %-6s = %4.0f ms: holds %.1f ms after pitch is lost", o.name, ms, 1000.0 * ((double) diff - (double) r.lost) / kFs));

        // The sputter fix's grace: how long the level holds after pitch is lost (the longest run of graced samples), at the
        // defaults, on an E4 that turns into noise at the same level for 150 ms and back (pitch lost with no mute, which skips it).
        {
            std::vector<float> in ((size_t) (1.2 * kFs));
            juce::Random rng (59);
            for (size_t n = 0; n < in.size(); ++n)
            {
                const double t = (double) n / kFs, ph = 2.0 * M_PI * 329.63 * t;
                const bool noise = t >= 0.5 && t < 0.65;
                in[n] = noise ? 0.4f * (rng.nextFloat() * 2.0f - 1.0f) : (float) (0.3 * (std::sin (ph) + 0.5 * std::sin (2.0 * ph) + 0.3 * std::sin (3.0 * ph)));
            }
            EngineParams g;
            g.holdMs = ms;
            Engine e;
            e.prepare (kFs, 64);
            int run = 0, longest = 0;
            e.voiceListener = [&] (const VoiceProbe& v) {
                run = v.graced ? run + 1 : 0;
                longest = std::max (longest, run);
            };
            runEngine (e, in, g);
            const double graceMs = 1000.0 * longest / kFs;
            check (std::abs (graceMs - o.graceMs) <= 1000.0 / kFs + 1e-9,
                   juce::String::formatted ("Note Release %-6s: the grace after pitch is lost is %.2f ms (wanted %.0f ms)", o.name, graceMs, o.graceMs));
        }

        const double t0 = muteTail40dB (ms), t75 = muteTail40dB (ms, 75.0f), t100 = muteTail40dB (ms, 100.0f);
        monotonic = monotonic && t0 >= prev0 && (k == 0 || (t75 > prev75 + 0.05 && t100 > prev100 + 0.05));
        prev0 = t0;
        prev75 = t75;
        prev100 = t100;
        line0 << o.name << " " << juce::String (t0, 3) << " s  ";
        line75 << o.name << " " << juce::String (t75, 3) << " s  ";
        line100 << o.name << " " << juce::String (t100, 3) << " s  ";
        if (o.expected0 > 0.0)
        {
            const double off = muteTail40dB (ms, 0.0f, Wave::Saw, nullptr, false);
            check (std::abs (off - o.expected0) <= 0.005, juce::String::formatted ("Note Release %s, Sustain 0, sputter fix off: muted E4 is 40 dB down at %.3f s (diagnosis: %.2f s, within 5 ms)", o.name, off, o.expected0));
            check (t0 - off <= kReleaseGraceInstantMs * 0.001 + 0.005,
                   juce::String::formatted ("Note Release %s, Sustain 0, sputter fix on: %.3f s (%+.0f ms: at most its %.0f ms grace, within 5 ms)", o.name, t0, 1000.0 * (t0 - off), kReleaseGraceInstantMs));
        }
    }
    check (monotonic, "Muted E4, 40 dB down (latency removed) -- Sustain 0: " + line0.trimEnd() + "; Sustain 75% (the default): " + line75.trimEnd() + "; Sustain 100%: " + line100.trimEnd()
                           + " (with Sustain, each option at least 50 ms longer)");
    // v0.5.8's options, as the engine still plays them (reported).
    juce::String old75, old100;
    for (auto [name, r] : { std::pair { "Instant", NoteReleaseV058::Instant }, { "Fast", NoteReleaseV058::Fast }, { "Medium", NoteReleaseV058::Medium },
                            { "Long", NoteReleaseV058::Long }, { "Extra Long", NoteReleaseV058::ExtraLong } })
    {
        old75 << name << " " << juce::String (muteTail40dB (noteReleaseMsFor (r), 75.0f), 3) << " s  ";
        old100 << name << " " << juce::String (muteTail40dB (noteReleaseMsFor (r), 100.0f), 3) << " s  ";
    }
    info ("v0.5.8's options, same tail: Sustain 75%: " + old75.trimEnd() + "; Sustain 100%: " + old100.trimEnd());
    // v0.5.1's default, kept as a pin on the engine (the v0.4.2 diagnosis).
    const double t350 = muteTail40dB (350.0f);
    check (std::abs (t350 - 1.21) <= 0.005, juce::String::formatted ("Engine at 350 ms (v0.5.1's default), Sustain 0: 40 dB down at %.3f s (diagnosis: 1.21 s, within 5 ms)", t350));
}

// ---------------------------------------------------------------------------
// Test X: Sustain. How much of the guitar's fade the level copies while the note is detected; a
// fade from the held level over Note Release once it's lost. (Sustain 0 = v0.4.3: test S.)
// ---------------------------------------------------------------------------
struct SustainRun
{
    std::vector<float> out;
    double lostAt = -1.0;   // output time (s) the delayed timeline loses pitch for good (last voiced hop + L)
    int unvoicedIn[2] = {}; // unvoiced hops inside the window given to renderSustain
};

SustainRun renderSustain (const std::vector<float>& in, float sustainPercent, float releaseMs = 350.0f, Wave wave = Wave::Saw,
                          double spanFrom = -1.0, double spanTo = -1.0)
{
    SustainRun r;
    EngineParams p = v051Defaults();
    p.sustainPercent = sustainPercent;
    p.holdMs = releaseMs;
    p.wave = wave;
    Engine e;
    e.prepare (kFs, 64);
    int64_t lastVoiced = -1;
    e.hopListener = [&] (const HopInfo& h) {
        const double t = (double) h.time / kFs;
        if (h.detector.voiced)
            lastVoiced = h.time;
        if (t >= spanFrom && t < spanTo)
            ++r.unvoicedIn[h.detector.voiced ? 0 : 1];
    };
    r.out = runEngine (e, in, p);
    r.lostAt = (double) (lastVoiced + e.currentLatencySamples()) / kFs;
    return r;
}

// RMS level (dB) of x over [t - w/2, t + w/2) seconds.
double levelDb (const std::vector<float>& x, double t, double w = 0.02)
{
    return toDb (rms (x, (size_t) std::max (0.0, (t - 0.5 * w) * kFs), (size_t) std::max (1.0, (t + 0.5 * w) * kFs)));
}

void testSustain()
{
    const double e4 = 329.63, lat = (double) latencyFor (kFs, defaultWindowHz()) / kFs;

    // Monotonic effect, the dB law, and flat at 100%: a plucked E4 ringing out (T60 6 s).
    {
        const auto in = ksPluck ([e4] (double) { return e4; }, 4.0, 0.5f, ksLossFor (e4, 6.0), 21);
        SustainRun runs[3];
        const float amounts[3] = { 0.0f, 50.0f, 100.0f };
        for (int k = 0; k < 3; ++k)
            runs[k] = renderSustain (in, amounts[k]);
        writeWav ("testX_ring_sustain0", runs[0].out);
        bool monotonic = true, law = true;
        juce::String line, lawLine;
        for (double t : { 0.5, 1.0, 1.5, 2.0, 3.0 })
        {
            double db[3];
            for (int k = 0; k < 3; ++k)
                db[k] = levelDb (runs[k].out, t + lat, 0.05);
            if (t == 1.0 || t == 2.0 || t == 3.0)
            {
                monotonic = monotonic && db[0] <= db[1] + 0.01 && db[1] <= db[2] + 0.01;
                line << juce::String (t, 0) << " s: " << juce::String (db[0], 1) << " / " << juce::String (db[1], 1) << " / " << juce::String (db[2], 1) << " dB  ";
            }
            // 50%: half the dB the guitar has fallen below the held level (while Sustain 0 still sounds).
            if (t <= 2.0)
            {
                const double drop0 = db[2] - db[0], drop50 = db[2] - db[1];
                law = law && std::abs (drop50 - 0.5 * drop0) < 1.0;
                lawLine << juce::String (t, 1) << " s: " << juce::String (drop50, 1) << " of " << juce::String (drop0, 1) << " dB  ";
            }
        }
        check (monotonic, "Ringing E4, output level at Sustain 0 / 50 / 100% is non-decreasing -- " + line.trimEnd());
        check (law, "Sustain 50%: falls half the dB that Sustain 0 does below the held level -- " + lawLine.trimEnd());

        const double to = std::min (runs[2].lostAt - 0.05, 3.9);
        double lo = 1e9, hi = -1e9;
        for (double t = 0.15 + lat; t < to; t += 0.01)
        {
            lo = std::min (lo, levelDb (runs[2].out, t));
            hi = std::max (hi, levelDb (runs[2].out, t));
        }
        check (hi - lo <= 1.0 && to > 2.5,
               juce::String::formatted ("Sustain 100%%: level flat within %.2f dB from 0.15 s until detection is lost (%.2f s), while the guitar falls %.1f dB", hi - lo,
                                        runs[2].lostAt, levelDb (runs[0].out, 0.15 + lat) - levelDb (runs[0].out, to)));
    }

    // Release: Sustain 100%, a muted pluck (as test W). The fade starts at the held level when pitch is
    // lost and lasts Note Release; click-free (sine: no sample step above the steady tone's).
    {
        const float releases[] = { 0.0f, 150.0f, 350.0f, 500.0f };
        double prev = -1.0;
        bool increasing = true, clickFree = true;
        juce::String line, clicks;
        for (float r : releases)
        {
            const double t = muteTail40dB (r, 100.0f);
            increasing = increasing && t > prev;
            prev = t;
            line << juce::String (r, 0) << " ms: " << juce::String (t, 3) << " s  ";

            const auto run = renderSustain (mutedPluck(), 100.0f, r, Wave::Sine);
            const float steady = maxStep (run.out, 0.4, 0.8);
            const float worst = maxStep (run.out, 0.85, 2.5) / steady;
            clickFree = clickFree && worst < 1.2f;
            clicks << juce::String (r, 0) << " ms: " << juce::String (worst, 2) << "x  ";
            if (r == 350.0f)
                writeWav ("testX_muted_sustain100_release350_sine", run.out);
        }
        check (increasing, "Sustain 100%, muted E4: 40 dB down at (latency removed) " + line.trimEnd() + " -- increases with Note Release");
        check (clickFree, "Sustain 100%, muted E4 (sine): largest sample step from the mute on, vs the steady tone's -- " + clicks.trimEnd());
        info (juce::String::formatted ("(Sustain 0, for comparison: 0 / 150 / 350 / 500 ms -> %.3f / %.3f / %.3f / %.3f s)", muteTail40dB (0.0f), muteTail40dB (150.0f),
                                       muteTail40dB (350.0f), muteTail40dB (500.0f)));
    }

    // Flicker: 60 ms of noise at the string's level at 1.0 s (pitch lost; no level change), Note
    // Release 350 ms: back to the held level, click-free.
    {
        auto in = ksPluck ([e4] (double) { return e4; }, 2.0, 0.5f, ksLossFor (e4, 60.0), 27);
        const float noiseAmp = 2.0f * (float) meanAbs (in, (size_t) (0.9 * kFs), (size_t) kFs); // uniform noise: mean |x| = amp / 2
        const auto noise = whiteNoise (0.07, noiseAmp, 11);
        for (size_t k = 0; k < noise.size(); ++k)
        {
            const double t = (double) k / kFs;
            const float g = (float) std::clamp (std::min (t, 0.064 - t) / 0.002, 0.0, 1.0); // 2 ms crossfades, 60 ms of noise
            auto& v = in[(size_t) kFs + k];
            v = (1.0f - g) * v + g * noise[k];
        }
        const auto run = renderSustain (in, 100.0f, 350.0f, Wave::Sine, 1.0, 1.07);
        writeWav ("testX_flicker_sustain100_sine", run.out);
        const double held = levelDb (run.out, 0.7 + lat, 0.3), after = levelDb (run.out, 1.4 + lat, 0.3);
        double dip = 1e9;
        for (double t = 1.0 + lat; t < 1.2 + lat; t += 0.002)
            dip = std::min (dip, levelDb (run.out, t, 0.01));
        const float steady = maxStep (run.out, 0.4, 0.8), worst = maxStep (run.out, 0.9 + lat, 1.3 + lat) / steady;
        check (run.unvoicedIn[1] >= 3 && std::abs (after - held) < 0.5 && worst < 1.2f,
               juce::String::formatted ("Sustain 100%%, 60 ms dropout (%d unvoiced hops): fades to %+.1f dB, back at %+.2f dB re. the held level; max step %.2fx the steady tone's",
                                        run.unvoicedIn[1], dip - held, after - held, worst));
    }

    // Turning Sustain to 0 mid-note hands back to the guitar's level over 5 ms, click-free; and on again.
    {
        const auto in = ksPluck ([e4] (double) { return e4; }, 3.0, 0.5f, ksLossFor (e4, 4.0), 29);
        Engine e;
        e.prepare (kFs, 64);
        const auto out = runScheduled (e, in, [] (double t) {
            EngineParams p = v051Defaults();
            p.wave = Wave::Sine;
            p.sustainPercent = (t < 1.0 || t >= 2.0) ? 100.0f : 0.0f;
            return p;
        });
        const float worst = maxStep (out, 0.9, 2.5) / maxStep (out, 0.4, 0.8);
        check (worst < 1.2f, juce::String::formatted ("Sustain 100 -> 0 -> 100%% mid-note (sine): max step %.2fx the steady tone's", worst));
    }
}

// ---------------------------------------------------------------------------
// Test Y: the envelope follower's one-period box (SynthSource::follow) is exact. Up to v0.4.4 it
// was off by one whenever its length changed (growing added a sample twice and slid off one never
// added; shrinking removed one twice), leaving a permanent offset of about one sample's |x| per
// change. A note decayed to that size then read wrong: the synth died while the guitar rang, or
// kept a floor that didn't fade.
// ---------------------------------------------------------------------------
void testEnvelopeBox()
{
    // Noise decaying from -6 dBFS to -126 dB over 10 s, against a brute-force mean |x| of the newest
    // round(period) samples, while the period jumps, glides and jitters across whole samples.
    const size_t N = (size_t) (10.0 * kFs);
    std::vector<float> x (N), period (N);
    {
        std::minstd_rand rng (91);
        std::uniform_real_distribution<float> u (-1.0f, 1.0f);
        double p = 146.0;
        for (size_t n = 0; n < N; ++n)
        {
            const double t = (double) n / kFs;
            x[n] = 0.5f * u (rng) * (float) std::pow (10.0, -0.6 * t);
            if (n % 128 == 0)
            {
                const int phase = (int) (t / 0.5) % 4;
                if (phase == 0)
                    p = 30.0 + 460.0 * (0.5 + 0.5 * u (rng)); // jumps (new notes), 30 .. 490 samples
                else if (phase == 1)
                    p *= 1.004;                              // a slide down
                else if (phase == 2)
                    p = std::floor (p) + 0.5 + 0.1 * u (rng); // jitter across a whole sample
                else
                    p /= 1.004;                              // a slide up
                p = std::clamp (p, 20.0, 490.0);
            }
            period[n] = (float) p;
        }
    }
    auto worstErrorDb = [&] (double& worstRel) {
        SynthSource syn;
        syn.prepare (kFs, 64);
        syn.setVoice ({});
        worstRel = 0.0;
        double worstDb = 0.0;
        for (size_t n = 0; n < N; ++n)
        {
            syn.follow (x[n], period[n]);
            const int len = (int) std::lround (period[n]);
            double sum = 0.0;
            for (int k = 0; k < len && (size_t) k <= n; ++k)
                sum += std::abs ((double) x[n - (size_t) k]);
            const double truth = sum / len, got = syn.meanAbsLevel();
            worstRel = std::max (worstRel, std::abs (got - truth) / truth);
            if (n % 480 == 0)
                worstDb = std::max (worstDb, std::abs (toDb (got) - toDb (truth)));
        }
        return worstDb;
    };
    double rel = 0.0;
    const double db = worstErrorDb (rel);
    check (rel < 1.0e-5, juce::String::formatted ("Box mean |x| = brute force over 10 s of noise decaying 120 dB, the period jumping, sliding and jittering "
                                                  "(%d s): worst error %.2g relative (%.2g dB)", 10, rel, db));

    // A plucked E4 (period 146 samples, T60 6 s): the box starts at fs / 220 = 218 (as the engine's
    // does) and shrinks to 146 at a detection moment between 10 and 30 ms. Box mean vs the true mean
    // |x| over 41 such moments.
    const auto in = ksPluck ([] (double) { return 329.63; }, 3.0, 0.5f, ksLossFor (329.63, 6.0), 21);
    const double times[] = { 1.0, 1.5, 2.0, 2.5 };
    {
        std::vector<double> err[4];
        for (int k = 0; k <= 40; ++k)
        {
            const size_t switchAt = (size_t) ((0.010 + 0.0005 * k) * kFs);
            SynthSource syn;
            syn.prepare (kFs, 64);
            syn.setVoice ({});
            size_t n = 0;
            for (int i = 0; i < 4; ++i)
            {
                for (; n < (size_t) (times[i] * kFs); ++n)
                    syn.follow (in[n], n < switchAt ? 218.0f : 146.0f);
                err[i].push_back (toDb (syn.meanAbsLevel()) - toDb (meanAbs (in, n - 146, n)));
            }
        }
        juce::String line;
        double worst = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            std::sort (err[i].begin(), err[i].end());
            worst = std::max ({ worst, -err[i].front(), err[i].back() });
            line << juce::String (times[i], 1) << " s: " << juce::String (err[i].front(), 1) << " / " << juce::String (err[i][20], 1) << " / " << juce::String (err[i].back(), 1) << "  ";
        }
        check (worst < 0.01, "Box, plucked E4, 218 -> 146 near the attack: re. the true mean |x|, dB (worst low / median / worst high) -- " + line.trimEnd());
    }

    // The engine at defaults on that pluck: its output level follows the guitar's.
    const double lat = (double) latencyFor (kFs, defaultWindowHz()) / kFs;
    {
        Engine e;
        e.prepare (kFs, 64);
        const auto out = runEngine (e, in, v051Defaults());
        auto outVsIn = [&] (double t) { return levelDb (out, t + lat, 0.05) - toDb (meanAbs (in, (size_t) ((t - 0.025) * kFs), (size_t) ((t + 0.025) * kFs))); };
        juce::String line;
        double worst = 0.0;
        for (double t : times)
        {
            worst = std::max (worst, std::abs (outVsIn (t) - outVsIn (0.5)));
            line << juce::String (t, 1) << " s: " << juce::String (outVsIn (t) - outVsIn (0.5), 1) << "  ";
        }
        check (worst < 3.0, "Engine at defaults on it: output level re. the guitar's, dB (0 at 0.5 s) -- " + line.trimEnd());
    }
}

// ---------------------------------------------------------------------------
// Renders from v0.5.1's defaults (with the sputter fix and v0.5.3 / v0.5.4's output stage and reference off, as the tests
// written then measured) and from the plugin's.
// ---------------------------------------------------------------------------
std::vector<float> renderWith (const std::vector<float>& in, const std::function<void (EngineParams&)>& set, double fs = kFs)
{
    EngineParams p = v051Defaults();
    set (p);
    Engine e;
    e.prepare (fs, 64);
    e.setHeadroomGain (false); // the output stage before v0.5.3
    e.setLegacyMemoryClear (true); // memory clears with the voice gate, as before v0.5.4
    e.setLegacyNoteReference (true); // the note's reference frozen while pitch is lost, as before v0.5.4
    e.setSputterFix (false); // the sputter fix off, as v0.5.4
    return runEngine (e, in, p);
}

// The same from v0.5.2's defaults (the plugin's) instead of v0.5.1's.
std::vector<float> renderAtDefaults (const std::vector<float>& in, const std::function<void (EngineParams&)>& set, double fs = kFs)
{
    EngineParams p;
    set (p);
    Engine e;
    e.prepare (fs, 64);
    return runEngine (e, in, p);
}

// A sine voice's largest sample step vs the steepest a sine at its peak level can move (1 = no click).
float sineStepRatio (const std::vector<float>& out, double hz)
{
    float peak = 0.0f;
    for (auto v : out)
        peak = std::max (peak, std::abs (v));
    return maxStep (out, 0.0, (double) out.size() / kFs) / (peak * (float) (2.0 * M_PI * hz / kFs));
}

// ---------------------------------------------------------------------------
// v0.5 chords
// ---------------------------------------------------------------------------

// One zero-padded FFT of x[from, from + len) (Hann), queried for the peak near each expected tone.
struct Spectrum
{
    Spectrum (const std::vector<float>& x, size_t from, size_t len, double fs = kFs) : binHz (fs / (1 << kOrder)), mags ((size_t) (2 << kOrder), 0.0f)
    {
        juce::dsp::FFT fft (kOrder);
        for (size_t k = 0; k < len; ++k)
            mags[k] = x[from + k] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * (double) k / (double) len));
        fft.performFrequencyOnlyForwardTransform (mags.data());
        norm = 4.0 / (double) len; // a sine of amplitude A peaks at A (Hann: coherent gain 1/2)
    }

    struct Peak
    {
        double hz = 0.0, amp = 0.0;
    };

    // Strongest bin within +/- 1.5% of expectHz (about a quarter tone), interpolated.
    Peak near (double expectHz) const
    {
        const int lo = (int) (expectHz * 0.985 / binHz), hi = (int) (expectHz * 1.015 / binHz);
        int best = lo;
        for (int k = lo; k <= hi; ++k)
            if (mags[(size_t) k] > mags[(size_t) best])
                best = k;
        const double y0 = std::log (mags[(size_t) best - 1]), y1 = std::log (mags[(size_t) best]), y2 = std::log (mags[(size_t) best + 1]);
        return { (best + 0.5 * (y0 - y2) / (y0 - 2.0 * y1 + y2)) * binHz, mags[(size_t) best] * norm };
    }

    static constexpr int kOrder = 20;
    double binHz, norm = 1.0;
    std::vector<float> mags;
};

double centsBetween (double hz, double refHz) { return 1200.0 * std::log2 (hz / refHz); }

// Steady notes, legato (one continuous saw, phase-continuous at each change): {midi, seconds} each.
std::vector<float> legatoSaws (std::initializer_list<std::pair<int, double>> notes, float amp = 0.4f, double fs = kFs)
{
    std::vector<std::pair<double, double>> plan; // end time, hz
    double t = 0.0;
    for (auto [n, len] : notes)
        plan.push_back ({ t += len, hzFromMidi (n) });
    return sawFromTrajectory ([plan] (double at) {
        for (auto [end, hz] : plan)
            if (at < end)
                return hz;
        return plan.back().second;
    }, t, amp, fs);
}

// v0.5.1's defaults otherwise, with the chord settings fixed since v0.5.2 (Voicing Open, Chord Level 80%). Stack in a
// key (the Chord selector's, v0.5.4); E maj / C# min by default, so E4 plays E major (E B G#, as v0.5.3's chromatic Major).
EngineParams chordParams (ChordMode mode, int key = 4)
{
    EngineParams p = v051Defaults();
    p.chordMode = mode;
    p.voicing = kVoicing;
    p.chordLevelPercent = 100.0f * kChordLevel;
    p.key = key;
    p.wave = Wave::Sine; // one partial per tone, so each can be measured on its own
    return p;
}

// Stack in a key (the Chord selector, v0.5.4): E4 in E maj / C# min is E major, in C maj / A min E minor, in the Open
// voicing (fixed since v0.5.2: the 5th above, the 3rd an octave up); each tone at the right pitch (sine voice, one FFT
// over a steady E4; each within 1 c of its note and present at Chord Level 80%). (Notes from B3 up: the default
// window's range.)
void testChordStack()
{
    const auto in = legatoSaws ({ { 64, 1.5 } });
    for (int key : { 4, 0 })
    {
        const auto out = renderWith (in, [key] (EngineParams& p) { p = chordParams (ChordMode::Stack, key); });
        const Spectrum spec (out, (size_t) (0.5 * kFs), (size_t) kFs);
        const auto iv = stackIntervals (64, Triad::Major, kVoicing, key, kScale);
        const int notes[3] = { 64, 64 + iv.a, 64 + iv.b };
        const auto played = spec.near (hzFromMidi (64));
        juce::String line;
        bool ok = true;
        for (int n : notes)
        {
            const auto pk = spec.near (hzFromMidi (n));
            const double c = centsBetween (pk.hz, hzFromMidi (n)), rel = toDb (pk.amp / played.amp);
            ok = ok && std::abs (c) < 1.0 && (n == 64 || std::abs (rel - toDb (kChordLevel)) < 0.5);
            line << juce::String (n - 64) << ": " << juce::String (c, 2) << "c " << juce::String (rel, 1) << "dB  ";
        }
        check (ok && iv.b == (key == 4 ? 16 : 15), juce::String::formatted ("Stack %s, Open on E4 (%s; semitones from E4: cents, level vs played) -- ", keyLabel (key).c_str(),
                                                               key == 4 ? "E major" : "E minor")
                                          + line.trimEnd());
    }
}

void testChordOctave()
{
    const auto in = legatoSaws ({ { 64, 1.5 } });
    for (Octave o : { Octave::Sub, Octave::DoubleSub })
    {
        const auto out = renderWith (in, [o] (EngineParams& p) {
            p = chordParams (ChordMode::Stack);
            p.octave = o;
        });
        const Spectrum spec (out, (size_t) (0.5 * kFs), (size_t) kFs);
        juce::String line;
        bool ok = true;
        for (int n : { 64, 71, 80 })
        {
            const double want = hzFromMidi (n) * octaveRatio (o);
            const auto pk = spec.near (want);
            const double c = centsBetween (pk.hz, want);
            ok = ok && std::abs (c) < 1.0 && pk.amp > 0.05;
            line << juce::String (want, 2) << " Hz: " << juce::String (c, 2) << "c  ";
        }
        check (ok, juce::String (o == Octave::Sub ? "Sub" : "Double Sub") + ", Stack E maj / C# min (Open) on E4: every tone an octave lower per step -- " + line.trimEnd());
    }
}

// The other tones follow the played note's level x Chord Level (80%, fixed since v0.5.2) through a pluck's
// decay; 3 tones are at most +3 dB over one note.
void testChordLevel()
{
    const double e4 = hzFromMidi (64);
    const auto pluck = ksPluck ([e4] (double) { return e4; }, 2.0, 0.5f, ksLossFor (e4, 2.0), 71);
    {
        const auto out = renderWith (pluck, [] (EngineParams& p) { p = chordParams (ChordMode::Stack); });
        juce::String line;
        bool ok = true;
        for (double t : { 0.2, 0.6, 1.0, 1.4 })
        {
            const Spectrum spec (out, (size_t) (t * kFs), (size_t) (0.2 * kFs));
            const double played = spec.near (e4).amp;
            const double third = toDb (spec.near (hzFromMidi (80)).amp / played), fifth = toDb (spec.near (hzFromMidi (71)).amp / played);
            ok = ok && std::abs (third - toDb (kChordLevel)) < 0.3 && std::abs (fifth - toDb (kChordLevel)) < 0.3;
            line << juce::String (t, 1) << " s (" << juce::String (toDb (played), 1) << " dB): " << juce::String (third, 2) << " / " << juce::String (fifth, 2) << " dB;  ";
        }
        writeWav ("chord_stack_open_pluck", out);
        check (ok, juce::String::formatted ("Plucked E4, Stack Open, Chord Level %.0f%% (%.2f dB): 3rd / 5th vs the played note through the decay -- ", 100.0f * kChordLevel, toDb (kChordLevel))
                       + line.trimEnd());
    }
    {
        const auto in = legatoSaws ({ { 64, 1.5 } });
        const double single = rms (renderWith (in, [] (EngineParams&) {}), (size_t) (0.5 * kFs), (size_t) (1.5 * kFs));
        juce::String line;
        bool ok = true;
        for (int key : { 4, 0 })
        {
            const auto out = renderWith (in, [key] (EngineParams& p) {
                p = chordParams (ChordMode::Stack, key);
                p.wave = Wave::Saw;
            });
            const double db = toDb (rms (out, (size_t) (0.5 * kFs), (size_t) (1.5 * kFs)) / single);
            ok = ok && db <= 3.1 && db > 2.0; // the gain law holds the sum at +3 dB (P = 2.28 at 80%), and it does reach it
            line << (key == 4 ? "E major " : "E minor ") << juce::String (db, 2) << " dB;  ";
        }
        check (ok, "Saw, Stack Open at Chord Level 80%: 3 tones vs the played note alone (RMS, at most +3 dB) -- " + line.trimEnd());
    }
}

// Key on: the major scale's triad on each degree (the scale fixed at Major since v0.5.2), placed Open. The
// interval function over every degree in every key, the out-of-scale rule, then the engine on a few notes
// (sine voice, FFT).
void testChordDiatonic()
{
    auto quality = [] (StackIntervals iv) { // the Open voicing back to the triad above the played note
        const StackIntervals root { iv.b - 12, iv.a };
        return root.a == 4 && root.b == 7 ? "" : root.a == 3 && root.b == 7 ? "m" : root.a == 3 && root.b == 6 ? "dim" : root.a == 4 && root.b == 8 ? "aug" : "?";
    };
    static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    // C major: C Dm Em F G Am Bdim; the same pattern on every key.
    {
        const char* const want[7] = { "", "m", "m", "", "", "m", "dim" };
        bool ok = true;
        juce::String line;
        for (int key = 0; key < 12; ++key)
            for (int d = 0; d < 7; ++d)
            {
                const int note = 60 + key + scaleSteps (kScale)[(size_t) d];
                const auto iv = stackIntervals (note, Triad::Minor, kVoicing, key, kScale); // Triad is ignored with a Key
                ok = ok && std::strcmp (quality (iv), want[d]) == 0 && iv.a == 7 - (d == 6 ? 1 : 0);
                if (key == 0)
                    line << names[note % 12] << quality (iv) << " (" << iv.a << ", " << iv.b << ")  ";
            }
        check (ok, "Major scale, every degree in all 12 keys, Open (C: triad, intervals) -- " + line.trimEnd());
    }

    // Out of scale: the nearest scale note's intervals on the played note; equidistant = the one below.
    {
        struct Borrow
        {
            int note;
            int fromDegreeNote;
        };
        const Borrow borrows[] = { { 61, 60 }, { 63, 62 }, { 66, 65 }, { 68, 67 }, { 70, 69 } }; // C# D# F# G# A# in C major
        bool ok = true;
        juce::String line;
        for (auto [note, from] : borrows)
        {
            const auto iv = stackIntervals (note, Triad::Major, kVoicing, 0, kScale), lent = stackIntervals (from, Triad::Major, kVoicing, 0, kScale);
            ok = ok && iv.a == lent.a && iv.b == lent.b;
            line << names[note % 12] << " -> " << names[from % 12] << "'s: " << names[(note + iv.a) % 12] << " " << names[(note + iv.b) % 12] << " (" << names[note % 12] << quality (iv)
                 << ");  ";
        }
        check (ok, "C major, out of scale -- " + line.trimEnd());
    }

    // The engine: C major, each note held 0.6 s legato; each tone's pitch from an FFT.
    {
        struct Case
        {
            int note;
            int a, b;
        };
        const Case cases[] = {
            { 62, 69, 77 }, // Dm: D4, A4 above, F5 a 10th up
            { 64, 71, 79 }, // Em: E4 B4 G5
            { 71, 77, 86 }, // B dim: B4, F5 (a diminished 5th), D6
            { 65, 72, 81 }, // F: F4 C5 A5
            { 66, 73, 82 }, // F# (F's intervals): C#5 A#5
            { 61, 68, 77 }, // C# (C's): G#4 F5
        };
        const auto in = legatoSaws ({ { 62, 0.6 }, { 64, 0.6 }, { 71, 0.6 }, { 65, 0.6 }, { 66, 0.6 }, { 61, 0.6 } });
        Engine e;
        e.prepare (kFs, 64);
        const auto out = runScheduled (e, in, [] (double) {
            auto p = chordParams (ChordMode::Stack);
            p.key = 0;
            return p;
        });
        bool ok = true;
        juce::String line;
        for (int k = 0; k < 6; ++k)
        {
            const auto& c = cases[k];
            const Spectrum spec (out, (size_t) ((0.6 * k + 0.25) * kFs), (size_t) (0.3 * kFs));
            const double played = spec.near (hzFromMidi (c.note)).amp;
            line << names[c.note % 12] << ":";
            for (int n : { c.note, c.a, c.b })
            {
                const auto pk = spec.near (hzFromMidi (n));
                const double cents = centsBetween (pk.hz, hzFromMidi (n));
                ok = ok && std::abs (cents) < 3.0 && pk.amp > 0.5 * played;
                line << " " << names[n % 12] << (n / 12 - 1) << " " << juce::String (cents, 1) << "c";
            }
            line << ";  ";
        }
        check (ok, "Engine, C major, legato D E B F F# C# (FFT per note, each tone within 3 c) -- " + line.trimEnd());
    }
}

// Readout::chordTones unpacked: live first, then the others (sorted).
std::vector<int> unpackTones (uint64_t packed)
{
    std::vector<int> t;
    for (int k = 0; k < 8; ++k)
        if (((packed >> (8 * k)) & 0xffu) != 0xffu)
            t.push_back ((int) ((packed >> (8 * k)) & 0xffu));
    if (t.size() > 2)
        std::sort (t.begin() + 1, t.end());
    return t;
}

juce::String noteText (int n)
{
    static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[n % 12]) + juce::String (n / 12 - 1);
}

juce::String tonesText (const std::vector<int>& t)
{
    if (t.empty())
        return "(none)";
    juce::String s;
    for (size_t k = 0; k < t.size(); ++k)
        s << (k == 0 ? "" : " ") << noteText (t[k]) << (k == 0 ? "*" : "");
    return s;
}

// The output, and the readout's chord tones after every 64-sample block.
struct ChordRun
{
    std::vector<float> out;
    std::vector<uint64_t> tones; // per block
    std::vector<int> held;       // Readout::memoryHeld, per block

    std::vector<int> at (double t) const { return unpackTones (tones[std::min (tones.size() - 1, (size_t) (t * kFs / 64.0))]); }
};

ChordRun runChord (const std::vector<float>& in, const std::function<EngineParams (double)>& paramsAt)
{
    ChordRun r;
    r.out.resize (in.size());
    Engine e;
    e.prepare (kFs, 64);
    for (size_t start = 0; start < in.size(); start += 64)
    {
        const int n = (int) std::min ((size_t) 64, in.size() - start);
        e.process (in.data() + start, r.out.data() + start, n, paramsAt ((double) start / kFs));
        r.tones.push_back (e.readout().chordTones.load());
        r.held.push_back (e.readout().memoryHeld.load());
    }
    return r;
}

std::vector<float> silence (double seconds) { return std::vector<float> ((size_t) (seconds * kFs), 0.0f); }

std::vector<float> joined (std::initializer_list<std::vector<float>> parts)
{
    std::vector<float> x;
    for (const auto& p : parts)
        x.insert (x.end(), p.begin(), p.end());
    return x;
}

// Memory: what's remembered and sounding, from the readout (live note marked *), at points well into
// each note (input time + 0.2 s, past the latency and the detector), and from the sound itself (FFT).
void testChordMemory()
{
    auto memory = [] (double) { return chordParams (ChordMode::Memory); };
    auto expect = [] (const ChordRun& r, double t, std::vector<int> want, const juce::String& what) {
        if (want.size() > 2)
            std::sort (want.begin() + 1, want.end());
        const auto got = r.at (t);
        check (got == want, what + juce::String::formatted (" (at %.2f s): ", t) + tonesText (got) + (got == want ? "" : "   expected " + tonesText (want)));
    };

    // C4, E4, G4 legato, 0.3 s each, then B4: three notes build a 3-tone chord; the 4th drops the oldest.
    {
        const auto r = runChord (legatoSaws ({ { 60, 0.3 }, { 64, 0.3 }, { 67, 0.3 }, { 71, 0.4 } }), memory);
        writeWav ("chord_memory_build_sine", r.out);
        expect (r, 0.2, { 60 }, "First note: a single tone");
        expect (r, 0.5, { 64, 60 }, "Second note");
        expect (r, 0.8, { 67, 60, 64 }, "Three notes held 0.3 s each: a 3-tone chord");
        expect (r, 1.2, { 71, 64, 67 }, "A 4th note drops the oldest (C4)");
        const Spectrum three (r.out, (size_t) (0.68 * kFs), (size_t) (0.2 * kFs)), four (r.out, (size_t) (1.05 * kFs), (size_t) (0.2 * kFs));
        const double g = three.near (hzFromMidi (67)).amp;
        const double c = toDb (three.near (hzFromMidi (60)).amp / g), e = toDb (three.near (hzFromMidi (64)).amp / g);
        const double gone = toDb (four.near (hzFromMidi (60)).amp / four.near (hzFromMidi (71)).amp);
        check (std::abs (c - toDb (kChordLevel)) < 0.5 && std::abs (e - toDb (kChordLevel)) < 0.5 && gone < -60.0,
               juce::String::formatted ("Sound (FFT): with G4 live, C4 %.2f dB and E4 %.2f dB (Chord Level 80%% = %.2f dB); after B4, C4 at %.1f dB", c, e, toDb (kChordLevel), gone));
    }

    // C4, E4, a 50 ms F4 (passing), then G4: F4 sounds while it lasts but is never remembered.
    {
        const auto r = runChord (legatoSaws ({ { 60, 0.3 }, { 64, 0.3 }, { 65, 0.05 }, { 67, 0.45 } }), memory);
        bool everF = false, fLive = false;
        for (size_t b = 0; b < r.tones.size(); ++b)
        {
            const auto t = unpackTones (r.tones[b]);
            const bool live = ! t.empty() && t[0] == 65;
            fLive = fLive || live;
            everF = everF || (! live && std::find (t.begin(), t.end(), 65) != t.end());
        }
        check (fLive && ! everF, juce::String ("A 50 ms passing F4 between E4 and G4: sounded as the live note ") + (fLive ? "yes" : "NO") + ", ever remembered " + (everF ? "YES" : "no"));
        expect (r, 0.95, { 67, 60, 64 }, "After the passing note: C4 E4 G4");
    }

    // C4, E4, G4, then E4 again: no duplicate, E4 live; the order updates (A4 next drops C4, not E4).
    {
        const auto r = runChord (legatoSaws ({ { 60, 0.3 }, { 64, 0.3 }, { 67, 0.3 }, { 64, 0.3 }, { 69, 0.4 } }), memory);
        expect (r, 1.1, { 64, 60, 67 }, "Replaying E4: E4 live, no duplicate");
        expect (r, 1.5, { 69, 64, 67 }, "Then A4 drops C4 (E4 moved to the newest)");
    }

    // A gap shorter than Note Release and the clear (100 ms of silence, Note Release 350 ms; the clear is 150 ms since
    // v0.5.6, the test's 150 ms gap became 100) keeps the chord; a stop (1 s of silence: the gate closes) clears it, and
    // the next note is a single tone.
    {
        const auto chord = legatoSaws ({ { 60, 0.3 }, { 64, 0.3 }, { 67, 0.3 } });
        const auto r = runChord (joined ({ chord, silence (0.10), legatoSaws ({ { 69, 0.4 } }), silence (1.0), legatoSaws ({ { 62, 0.4 } }) }), memory);
        expect (r, 0.9 + 0.07, { 67, 60, 64 }, "During a 100 ms gap: the chord holds");
        expect (r, 1.0 + 0.25, { 69, 64, 67 }, "A4 after the gap: joins the chord (C4 dropped)");
        expect (r, 1.4 + 0.6, {}, "1 s of silence: the chord has faded");
        expect (r, 2.4 + 0.25, { 62 }, "The next note (D4) after a stop: a single tone");
    }
}

// The chord-naming function (readout line 5) on a table of sets.
void testChordNaming()
{
    struct Case
    {
        std::vector<int> notes;
        const char* want;
    };
    const Case cases[] = {
        { {}, "" },
        { { 57 }, "A" },
        { { 57, 69 }, "A (8ve)" },
        { { 57, 64 }, "A+E (5th)" },
        { { 64, 57 }, "A+E (5th)" },            // order doesn't matter: the lowest is the bass
        { { 52, 57 }, "E+A (4th)" },
        { { 60, 61 }, "C+C# (m2)" },
        { { 60, 66 }, "C+F# (tritone)" },
        { { 60, 71 }, "C+B (M7)" },
        { { 60, 64, 67 }, "C (C E G)" },
        { { 64, 67, 72 }, "C/E (E G C)" },      // first inversion
        { { 55, 60, 64 }, "C/G (G C E)" },      // second inversion
        { { 57, 60, 64 }, "Am (A C E)" },
        { { 60, 64, 69 }, "Am/C (C E A)" },
        { { 55, 59, 64 }, "Em/G (G B E)" },
        { { 59, 62, 65 }, "B\xc2\xb0 (B D F)" },
        { { 62, 65, 71 }, "B\xc2\xb0/D (D F B)" },
        { { 60, 64, 68 }, "Caug (C E G#)" },
        { { 64, 68, 72 }, "Eaug (E G# C)" },    // augmented: the bass names it
        { { 48, 64, 67, 72, 76 }, "C (C E G)" }, // doubled notes, spread voicing
        { { 62, 69, 77 }, "Dm (D A F)" },       // open voicing: pitch classes from the bass up
        { { 60, 62, 64 }, "C D E" },            // not a triad
        { { 60, 65, 67 }, "C F G" },            // sus4: not one of the four qualities
        { { 55, 59, 62, 64 }, "G B D E" },      // four pitch classes
    };
    int bad = 0;
    for (const auto& c : cases)
    {
        const auto got = chordName (c.notes);
        if (got != c.want)
        {
            ++bad;
            info (juce::String ("  ") + juce::String::fromUTF8 (got.c_str()) + "   expected " + juce::String::fromUTF8 (c.want));
        }
    }
    check (bad == 0, juce::String::formatted ("Chord naming: %d cases (triads and inversions, dim / aug, intervals, octaves, other sets), %d wrong", (int) std::size (cases), bad));
}

// No clicks: a sine voice on steady input, the largest sample step vs the steepest a sum of its tones
// at the output's peak can move (1 = no click; the highest tone sets the bound).
float chordStepRatio (const std::vector<float>& out, double maxToneHz, double from = 0.0)
{
    float peak = 0.0f;
    for (size_t n = (size_t) (from * kFs); n < out.size(); ++n)
        peak = std::max (peak, std::abs (out[n]));
    return maxStep (out, from, (double) out.size() / kFs) / (peak * (float) (2.0 * M_PI * maxToneHz / kFs));
}

void testChordClicks()
{
    // Stack switched on and off, and between keys (E maj <-> C maj: the triads change), mid-note; legato note changes under it.
    {
        const auto in = legatoSaws ({ { 64, 0.9 }, { 66, 0.6 }, { 67, 0.75 }, { 69, 0.75 } });
        Engine e;
        e.prepare (kFs, 64);
        const auto out = runScheduled (e, in, [] (double t) {
            return chordParams ((int) (t / 0.23) % 3 == 0 ? ChordMode::Off : ChordMode::Stack, (int) (t / 0.17) % 2 == 0 ? 4 : 0);
        });
        writeWav ("chord_clicks_stack_switching_sine", out);
        const float r = chordStepRatio (out, hzFromMidi (69 + 16), 0.05);
        check (r < 1.0f, juce::String::formatted ("Stack on / off every 0.23 s, E maj <-> C maj every 0.17 s, 4 legato notes: largest step %.2fx the bound", r));
    }

    // Key on: legato notes whose triad changes (E -> F -> F# -> G -> B), Key changed mid-note,
    // at Retune Fast and Slow (the harmony jumps or glides; the oscillators never restart).
    for (RetuneSpeed speed : { RetuneSpeed::Fast, RetuneSpeed::Slow })
    {
        const auto in = legatoSaws ({ { 64, 0.5 }, { 65, 0.4 }, { 66, 0.4 }, { 67, 0.5 }, { 71, 0.6 } });
        Engine e;
        e.prepare (kFs, 64);
        const auto out = runScheduled (e, in, [speed] (double t) {
            auto p = chordParams (ChordMode::Stack);
            p.retuneSpeed = speed;
            p.key = t < 1.1 ? 0 : t < 1.6 ? 7 : t < 1.9 ? 9 : 2;
            return p;
        });
        writeWav (juce::String ("chord_clicks_key_sine_") + (speed == RetuneSpeed::Fast ? "fast" : "slow"), out);
        const float r = chordStepRatio (out, hzFromMidi (71 + 16), 0.05);
        check (r < 1.0f, juce::String::formatted ("Key on, Retune %s: legato E F F# G B (triad changes), Key C -> G -> A -> D mid-note: largest step %.2fx the bound",
                                                  speed == RetuneSpeed::Fast ? "Fast" : "Slow", r));
    }

    // Memory: notes joining, a 4th dropping the oldest, a replay, a passing note; then Memory -> Stack ->
    // Off -> Memory mid-note.
    for (RetuneSpeed speed : { RetuneSpeed::Fast, RetuneSpeed::Medium })
    {
        const auto in = legatoSaws ({ { 60, 0.3 }, { 64, 0.3 }, { 67, 0.3 }, { 71, 0.3 }, { 64, 0.3 }, { 65, 0.05 }, { 69, 0.6 }, { 67, 1.2 } });
        Engine e;
        e.prepare (kFs, 64);
        const auto out = runScheduled (e, in, [speed] (double t) {
            auto p = chordParams (t < 2.5 ? ChordMode::Memory : t < 2.8 ? ChordMode::Stack : t < 3.1 ? ChordMode::Off : ChordMode::Memory);
            p.retuneSpeed = speed;
            return p;
        });
        writeWav (juce::String ("chord_clicks_memory_sine_") + (speed == RetuneSpeed::Fast ? "fast" : "medium"), out);
        const float r = chordStepRatio (out, hzFromMidi (67 + 16), 0.05); // Stack Open on G4: B5 is the highest tone
        check (r < 1.0f, juce::String::formatted ("Memory, Retune %s: C E G B E (replay) F (50 ms) A G, then Stack / Off / Memory mid-note: largest step %.2fx the bound",
                                                  speed == RetuneSpeed::Fast ? "Fast" : "Medium", r));
    }
}

// ---------------------------------------------------------------------------
// v0.5.1: Glide, onset snap, level inheritance, Dynamics, wave loudness
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// v0.5.2: fewer controls (Glide, Dynamics, Voicing, Scale, Chord Level fixed), Note Release choice, Key
// labels, Triangle and Sine +10 dB
// ---------------------------------------------------------------------------

// Key labels (v0.5.2): each option names the major key whose tonic Stack uses and its relative minor (3
// semitones below). A minor-key label plays its relative major's scale, which has the natural minor's
// triads: A min gives Am, B dim, C, Dm, Em, F, G.
void testKeyLabels()
{
    auto pitchClass = [] (const std::string& name) { // "C", "C#", "Db"
        static const int letters[7] = { 9, 11, 0, 2, 4, 5, 7 }; // A .. G
        const int base = letters[name[0] - 'A'];
        return (base + (name.size() > 1 ? (name[1] == '#' ? 1 : -1) : 0) + 12) % 12;
    };
    static const char* const table[12] = { "C maj / A min", "Db maj / Bb min", "D maj / B min", "Eb maj / C min", "E maj / C# min", "F maj / D min",
                                           "F# maj / D# min", "G maj / E min", "Ab maj / F min", "A maj / F# min", "Bb maj / G min", "B maj / G# min" };
    {
        bool ok = true;
        juce::String line;
        for (int k = 0; k < 12; ++k)
        {
            ok = ok && keyLabel (k) == table[k] && pitchClass (keyMajorName (k)) == k && pitchClass (keyMinorName (k)) == (k + 9) % 12;
            line << juce::String (k + 1) << " " << keyLabel (k) << " -> " << pitchClassName (k) << ";  ";
        }
        check (ok, "Key option k (1 .. 12): the spec's label, its major tonic = key k - 1, its minor 3 semitones below -- " + line.trimEnd());
    }

    // The triads of each degree, by name, for a minor label (A min: the C major scale from A).
    auto triadName = [] (int note, int key) {
        const auto iv = stackIntervals (note, Triad::Major, kVoicing, key, kScale);
        const auto name = chordName ({ note, note + iv.b - 12, note + iv.a }); // the Open voicing back in root position
        return name.substr (0, name.find (' '));
    };
    {
        const std::string want[7] = { "Am", "B\xc2\xb0", "C", "Dm", "Em", "F", "G" };
        const int notes[7] = { 57, 59, 60, 62, 64, 65, 67 };
        bool ok = true;
        juce::String line;
        for (int d = 0; d < 7; ++d)
        {
            const auto got = triadName (notes[d], 0);
            ok = ok && got == want[d];
            line << juce::String::fromUTF8 (got.c_str()) << " ";
        }
        check (ok, juce::String::fromUTF8 (keyLabel (0).c_str()) + ", the A minor scale's degrees: " + line.trimEnd());
    }

    // Every key, every note (in and out of the scale): the major scale on the label's tonic gives the same
    // triads as the natural minor on its relative minor (v0.5.1's Natural Minor, still in the code).
    {
        int differ = 0;
        for (int k = 0; k < 12; ++k)
            for (int note = 48; note < 84; ++note)
            {
                const auto major = stackIntervals (note, Triad::Major, kVoicing, k, Scale::Major), minor = stackIntervals (note, Triad::Major, kVoicing, (k + 9) % 12, Scale::NaturalMinor);
                differ += major.a != minor.a || major.b != minor.b ? 1 : 0;
            }
        check (differ == 0, juce::String::formatted ("All 12 labels x 36 notes: the major scale = the relative natural minor, note for note (%d differ)", differ));
    }

    // The engine, Key "C maj / A min", playing A4: the chord is Am, Open (A4, E5, C6).
    {
        const auto r = runChord (legatoSaws ({ { 69, 0.6 } }), [] (double) {
            EngineParams p;
            p.chordMode = ChordMode::Stack;
            p.key = 0;
            return p;
        });
        const auto tones = r.at (0.4);
        const auto name = chordName (tones);
        check (name == "Am (A E C)", "Stack, Key C maj / A min, playing A4: " + tonesText (tones) + " = " + juce::String::fromUTF8 (name.c_str()));
    }
}

// Two notes with a gap between them: a pluck of `a`, `gap` s of silence (the pick's dropout, or a stop),
// then a pluck of `b` (KS strings; the first still ringing up to the gap).
std::vector<float> pluckGapPluck (double a, double b, double gap, double firstLen = 0.6, double secondLen = 0.8)
{
    auto x = ksPluck ([a] (double) { return a; }, firstLen, 0.5f, ksLossFor (a, 4.0), 61);
    x.insert (x.end(), (size_t) (gap * kFs), 0.0f);
    const auto y = ksPluck ([b] (double) { return b; }, secondLen, 0.5f, ksLossFor (b, 4.0), 62);
    x.insert (x.end(), y.begin(), y.end());
    return x;
}

// The second note's first voiced hops (input time from `from`): N and T.
juce::String hopsText (const std::vector<HopInfo>& hops, size_t first, int count)
{
    juce::String s;
    for (size_t i = first; i < std::min (hops.size(), first + (size_t) count); ++i)
        s << hops[i].noteN << ":" << juce::String (hops[i].target, 2) << " ";
    return s.trimEnd();
}

size_t firstVoicedFrom (const std::vector<HopInfo>& hops, double from)
{
    for (size_t i = 0; i < hops.size(); ++i)
        if ((double) hops[i].time / kFs >= from && hops[i].detector.voiced)
            return i;
    return hops.size();
}

// Glide (Always since v0.5.2, fixed; Legato is gone as a control): at Retune Medium (the default), a note
// plucked while the voice is still open (the previous note's Note Release) glides in from the previous
// pitch; after the gate has closed it latches fresh.
void testGlide()
{
    const double e4 = hzFromMidi (64), a4 = hzFromMidi (69);
    struct Row
    {
        const char* name;
        double gap;
        bool glides;
    };
    const Row rows[] = {
        { "40 ms dropout (voice open)", 0.04, true },
        { "1 s stop (gate closed)", 1.0, false },
    };
    for (const auto& r : rows)
    {
        EngineParams p; // v0.5.2's defaults: Always, Retune Medium
        std::vector<float> out;
        const auto hops = collectHops (pluckGapPluck (e4, a4, r.gap), p, juce::String ("glide_always_") + juce::String (r.gap, 2), kFs, &out);
        const size_t i = firstVoicedFrom (hops, 0.6 + r.gap);
        bool ok = i + 8 < hops.size() && hops[i].noteN == 69;
        if (ok && r.glides)
        {
            // From the held E4: the first hop a step of the 40 ms glide up from 64, then rising steadily toward 69.
            const double first = 64.0 + 5.0 * (1.0 - std::exp (-(double) hopFor (kFs) / kFs / 0.040));
            ok = std::abs (hops[i].target - first) < 1e-6;
            for (size_t k = i + 1; ok && k < i + 8; ++k)
                ok = hops[k].target > hops[k - 1].target && hops[k].target < 69.0;
        }
        else if (ok)
        {
            ok = std::abs (hops[i].target - 69.0) < 1e-9;
        }
        if (r.gap < 0.1)
            writeWav ("glide_always_dropout", out);
        check (ok, juce::String::formatted ("%-28s: A4 %s -- first hops N:T ", r.name, r.glides ? "glides in from E4" : "latches fresh") + hopsText (hops, i, 5));
    }

    // Always within Note Release 0: the gate's own 15 ms fade still counts as open; a 40 ms dropout doesn't.
    // (The sputter fix's grace keeps the gate open longer, 20 ms at Instant: with it the 6 ms dropout still glides and the
    // 40 ms one, past the grace and the 15 ms fade, closes the gate. Both checked with the fix off and on.) v0.5.11: with the
    // engine's defaults (the fix on) the pick after the 40 ms dropout glides in all the same, by the pick glide (within
    // kPickGlideWindowMs of the E4); after 200 ms it latches fresh. The fix off (v0.5.4) has no pick glide.
    for (double gap : { 0.006, 0.04, 0.2 })
        for (bool fix : { false, true })
        {
            EngineParams p;
            p.holdMs = 0.0f; // Note Release Instant
            const auto hops = collectHops (pluckGapPluck (e4, a4, gap), p, "glide_always_release0", kFs, nullptr, fix);
            const size_t i = firstVoicedFrom (hops, 0.6 + gap);
            const bool glided = i < hops.size() && hops[i].target < 68.0;
            const bool pickGlide = fix && gap >= 0.01 && 1000.0 * gap < kPickGlideWindowMs;
            const bool want = gap < 0.01 || pickGlide;
            check (i < hops.size() && glided == want,
                   juce::String::formatted ("Always, Note Release 0, %3.0f ms dropout, sputter fix %s: %s -- first hops ", 1000.0 * gap, fix ? "on" : "off",
                                            gap < 0.01 ? "glides (gate still fading)" : pickGlide ? "glides in (the gate closed: the pick glide)" : "latches fresh (gate closed)")
                       + hopsText (hops, i, 3));
        }

    // Memory (v0.5.2's defaults, sine voice): C4 (remembered after 80 ms), a 30 ms dropout, then E4. The
    // remembered C4 carries on in a copy of the voice at its own pitch while the live voice fades in over
    // 5 ms (the hand-off) and glides up from C4, so for the first tens of ms the two are nearly in unison
    // and in phase. Level against the settled two-note chord, 5 ms RMS.
    {
        const auto in = joined ({ legatoSaws ({ { 60, 0.4 } }), silence (0.03), legatoSaws ({ { 64, 0.6 } }) });
        const double lat = (double) latencyFor (kFs, defaultWindowHz()) / kFs, change = 0.43 + lat;
        const auto out = renderAtDefaults (in, [] (EngineParams& q) {
            q.chordMode = ChordMode::Memory;
            q.wave = Wave::Sine;
        });
        const double settled = levelDb (out, change + 0.4, 0.1);
        double peak = -200.0;
        for (double t = change; t < change + 0.25; t += 0.0025)
            peak = std::max (peak, levelDb (out, t, 0.005));
        writeWav ("glide_always_memory_handoff_sine", out);
        check (peak - settled < 6.0, juce::String::formatted ("Memory, C4 -> (30 ms dropout) -> E4 gliding in: the in-phase start of the hand-off stays within +6 dB of the settled chord "
                                                              "(loudest 5 ms in the 250 ms after the change %+.1f dB)", peak - settled));
    }
}

// Onset snap: in the first 25 ms of a note, a jump of 7+ semitones snaps (the onset octave error); a
// deliberate jump after that still glides.
void testOnsetSnap()
{
    const double e4 = hzFromMidi (64), e5 = hzFromMidi (76);
    // A 7-semitone slide over 60 ms starting 300 ms into a note glides at Retune Slow. (An instant jump
    // costs the detector a hop or two, after which the note arrives as a new one; so single jumps of
    // N are checked on the quantizer itself below.)
    {
        EngineParams p = v051Defaults();
        p.retuneSpeed = RetuneSpeed::Slow;
        const double b4 = hzFromMidi (71);
        const auto slideIn = joined ({ silence (0.2), sawFromTrajectory ([=] (double t) { return e4 * std::pow (b4 / e4, std::clamp ((t - 0.3) / 0.06, 0.0, 1.0)); }, 0.8, 0.4f) });
        const auto sh = collectHops (slideIn, p, "onset_snap_late_slide");
        double worstLag = 0.0;
        for (const auto& h : sh)
            worstLag = std::max (worstLag, (double) h.noteN - h.target);
        check (worstLag > 1.0, juce::String::formatted ("Slide E4 -> B4 over 60 ms, 300 ms into the note: glides (T up to %.2f semitones behind N)", worstLag));
    }

    // Glide Always: the jump that starts a note is its glide, whatever its size (no cap): E4, a 30 ms
    // dropout, then E5 glides in at Retune Slow.
    {
        EngineParams p = v051Defaults();
        p.retuneSpeed = RetuneSpeed::Slow;
        const auto in = joined ({ sawFromTrajectory ([=] (double) { return e4; }, 0.5, 0.4f), silence (0.03), sawFromTrajectory ([=] (double) { return e5; }, 0.5, 0.4f) });
        const auto hops = collectHops (in, p, "onset_snap_always_octave");
        const size_t i = firstVoicedFrom (hops, 0.53);
        check (i < hops.size() && hops[i].noteN == 76 && hops[i].target < 66.0,
               "Always, E4 then E5 after a 30 ms dropout: the octave glides in (no distance cap) -- " + hopsText (hops, i, 4));
    }

    // The quantizer on its own, Retune Slow, voiced hops: N jumps 64 -> 76 (or 71) after k hops.
    {
        const double hopS = (double) hopFor (kFs) / kFs;
        auto afterJump = [&] (int hopsBefore, int to, NoteContext atStart, bool onsetBeforeJump = false) {
            Quantizer q;
            q.reset();
            q.update (true, 64.0, kHysteresisCents, 120.0f, hopS, atStart);
            for (int k = 1; k < hopsBefore; ++k)
            {
                NoteContext c;
                c.onset = onsetBeforeJump && k == hopsBefore - 1;
                q.update (true, 64.0, kHysteresisCents, 120.0f, hopS, c);
            }
            q.update (true, (double) to, kHysteresisCents, 120.0f, hopS);
            return q.target();
        };
        const int inside = (int) (kOnsetSnapMs * 0.001 / hopS); // the last hop count still inside 25 ms
        const double snapIn = afterJump (inside, 76, {}), glideAfter = afterJump (inside + 1, 76, {}), sixIn = afterJump (2, 70, {}), sevenIn = afterJump (2, 71, {});
        const double afterOnset = afterJump (40, 76, {}, true);
        check (std::abs (snapIn - 76.0) < 1e-9 && glideAfter < 66.0 && sixIn < 65.0 && std::abs (sevenIn - 71.0) < 1e-9 && afterOnset < 66.0,
               juce::String::formatted ("Quantizer, Retune Slow: +12 after %d hops (%.1f ms) snaps (T %.2f), after %d hops glides (T %.2f); +6 early glides (%.2f), +7 snaps (%.2f); "
                                        "the first change after an onset is its arrival and glides (%.2f)",
                                        inside, 1000.0 * inside * hopS, snapIn, inside + 1, glideAfter, sixIn, sevenIn, afterOnset));

        // An onset 1 s into a note starts a new 25 ms window: arrival (the first change) glides, a second 7+ jump snaps.
        Quantizer q;
        q.reset();
        for (int k = 0; k < 400; ++k)
            q.update (true, 64.0, kHysteresisCents, 120.0f, hopS);
        NoteContext onset;
        onset.onset = true;
        q.update (true, 64.0, kHysteresisCents, 120.0f, hopS, onset);
        q.update (true, 57.0, kHysteresisCents, 120.0f, hopS); // arrival: an octave-low read of A4
        const double arrival = q.target();
        q.update (true, 69.0, kHysteresisCents, 120.0f, hopS); // the correction, inside 25 ms
        check (arrival > 63.0 && arrival < 64.0 && std::abs (q.target() - 69.0) < 1e-9,
               juce::String::formatted ("Quantizer: after an onset mid-note, the arrival (64 -> 57) glides (T %.2f), the octave correction (57 -> 69) snaps (T %.2f)", arrival, q.target()));
    }
}


// A saw at E4 decaying 10 dB/s, sliding to F#4 over 10 ms at 1.0 s (fast enough to be a hammer-on,
// slow enough to stay voiced: a connected change), its level stepping by stepDb over the same 10 ms.
std::vector<float> hammerOn (double stepDb, double seconds = 2.0)
{
    const double e4 = hzFromMidi (64), fs4 = hzFromMidi (66);
    auto in = sawFromTrajectory ([=] (double t) { return e4 * std::pow (fs4 / e4, std::clamp ((t - 1.0) / 0.01, 0.0, 1.0)); }, seconds, 0.4f);
    for (size_t n = 0; n < in.size(); ++n)
    {
        const double t = (double) n / kFs;
        in[n] *= (float) (std::pow (10.0, -0.5 * t) * std::pow (10.0, stepDb / 20.0 * std::clamp ((t - 1.0) / 0.01, 0.0, 1.0)));
    }
    return in;
}

// Level inheritance: inside Glide Always's window (the voice gate open; fixed since v0.5.2) a note change,
// plucks included, keeps Sustain's reference. (Retune Fast and Dynamics 100%, as v0.5.1, so the levels
// are Sustain's alone and the v0.5 comparisons hold.)
void testInheritance()
{
    const double lat = (double) latencyFor (kFs, defaultWindowHz()) / kFs;
    auto render = [] (const std::vector<float>& in, float sustain) {
        return renderWith (in, [=] (EngineParams& p) {
            p.sustainPercent = sustain;
        });
    };
    auto unvoicedHops = [] (const std::vector<float>& in, double from, double to) {
        int n = 0;
        for (const auto& h : collectHops (in, v051Defaults(), "inherit_hops"))
            n += (double) h.time / kFs >= from && (double) h.time / kFs < to && ! h.detector.voiced ? 1 : 0;
        return n;
    };

    // Sustain 100%: a hammer-on to a note 12 dB quieter holds the first note's level (v0.5 restarted:
    // the second note held 22 dB lower).
    {
        const auto in = hammerOn (-12.0);
        const int gaps = unvoicedHops (in, 0.9, 1.1);
        const auto out = render (in, 100.0f);
        writeWav ("inherit_hammer_on_sustain100", out);
        const double held1 = levelDb (out, 0.7 + lat, 0.3), held2 = levelDb (out, 1.5 + lat, 0.3);
        check (gaps == 0 && std::abs (held2 - held1) < 1.0,
               juce::String::formatted ("Sustain 100%%, hammer-on to a note 12 dB quieter (%d unvoiced hops): holds %+.2f dB re. the first note's held level", gaps,
                                        held2 - held1));

        // A hammer-on louder than the held level still raises it.
        const auto louder = render (hammerOn (+16.0), 100.0f);
        const double rise = levelDb (louder, 1.5 + lat, 0.3) - levelDb (louder, 0.7 + lat, 0.3);
        check (rise > 4.0, juce::String::formatted ("Sustain 100%%, hammer-on 16 dB up (6 dB over the first note's start): the reference rises, held level %+.1f dB", rise));
    }

    // Sustain 50%: a hammer-on with the guitar's level carrying on (no step) carries on along the same
    // half-speed fade, with no jump at the change (v0.5 restarted there: a jump down).
    {
        const auto in = hammerOn (0.0);
        auto deviation = [&] (const std::vector<float>& out) {
            // Fit the output's dB over 0.6 .. 0.95 s (input time), then the worst departure from it over 1.0 .. 1.4 s.
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            int n = 0;
            for (double t = 0.6; t < 0.95; t += 0.01, ++n)
            {
                const double y = levelDb (out, t + lat, 0.01);
                sx += t;
                sy += y;
                sxx += t * t;
                sxy += t * y;
            }
            const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx), icpt = (sy - slope * sx) / n;
            double worst = 0.0;
            for (double t = 1.0; t < 1.4; t += 0.005)
            {
                const double d = levelDb (out, t + lat, 0.01) - (icpt + slope * t);
                if (std::abs (d) > std::abs (worst))
                    worst = d;
            }
            return std::pair { slope, worst };
        };
        const auto out = render (in, 50.0f);
        const auto [slope, jump] = deviation (out);
        writeWav ("inherit_hammer_on_sustain50", out);
        check (std::abs (jump) < 0.5 && std::abs (slope + 5.0) < 0.5,
               juce::String::formatted ("Sustain 50%%, hammer-on, the guitar fading 10 dB/s: output fades %.2f dB/s and stays within %+.2f dB of that line through the change",
                                        slope, jump));
    }

    // A pluck while the voice is open (a 40 ms dropout) inherits, plucks included; a pluck after the gate
    // has closed (1 s) restarts. The second pluck is 12 dB quieter.
    {
        const auto e4 = ksPluck ([] (double) { return hzFromMidi (64); }, 0.6, 0.5f, ksLossFor (329.63, 4.0), 71);
        const auto a4 = ksPluck ([] (double) { return hzFromMidi (69); }, 1.0, 0.125f, ksLossFor (440.0, 4.0), 72);
        auto twoPlucks = [&] (double gap) { return joined ({ e4, silence (gap), a4 }); };
        // The A4's own held level, played alone (what a restart holds), against the E4's.
        const double e4Held = levelDb (render (e4, 100.0f), 0.4 + lat, 0.2);
        const double fresh = levelDb (render (joined ({ silence (0.6), a4 }), 100.0f), 1.1 + lat, 0.3) - e4Held;
        struct Row
        {
            const char* name;
            double gap;
            bool inherits;
        };
        const Row rows[] = {
            { "pluck after a 40 ms dropout", 0.04, true },
            { "pluck after a 1 s stop     ", 1.0, false },
        };
        for (const auto& r : rows)
        {
            const auto out = render (twoPlucks (r.gap), 100.0f);
            if (r.gap < 0.1)
                writeWav ("inherit_always_pluck_sustain100", out);
            const double d = levelDb (out, 0.6 + r.gap + 0.5 + lat, 0.3) - e4Held;
            const double expect = r.inherits ? 0.0 : fresh;
            check (std::abs (d - expect) < 1.0, juce::String::formatted ("Sustain 100%%, %s: the A4 (picked 12 dB softer) holds %+.1f dB re. the E4's held level (%s: %+.1f dB)", r.name, d,
                                                                         r.inherits ? "inherits" : "restarts, as the A4 alone", expect));
        }
    }

}

// Dynamics, fixed at 50% since v0.5.2 (kDynamics): a per-note gain toward T that halves the dB between
// notes; the note's decay shape is unchanged (no pumping). 100% (a bypass, the reference "as played"
// here) is what v05-defaults compares with v0.5; the other settings of the old control aren't tested.
void testDynamics()
{
    const double e4 = hzFromMidi (64), lat = (double) latencyFor (kFs, defaultWindowHz()) / kFs;
    const float fixed = 100.0f * kDynamics;
    auto pluck = [e4] (float amp, uint32_t seed = 91) { return ksPluck ([e4] (double) { return e4; }, 1.6, amp, ksLossFor (e4, 3.0), seed); };
    auto render = [] (const std::vector<float>& in, float dynamics, float sustain = 0.0f, Wave wave = Wave::Saw) {
        return renderWith (in, [=] (EngineParams& p) {
            p.dynamicsPercent = dynamics;
            p.sustainPercent = sustain;
            p.wave = wave;
        });
    };
    check (std::equal_to<float>() (EngineParams().dynamicsPercent, 50.0f), "The engine's default (the plugin's, fixed) is Dynamics 50%");

    // T sits at a medium pluck: what Dynamics does to one (0.5 peak, E4).
    {
        const auto in = pluck (0.5f);
        const double d = levelDb (render (in, fixed), 0.2 + lat, 0.1) - levelDb (render (in, 100.0f), 0.2 + lat, 0.1);
        check (std::abs (d) < 1.0, juce::String::formatted ("T = %.0f dB (note level, mean |x|): a medium pluck (0.5 peak, E4) moves %+.2f dB", kDynamicsTargetDb, d));
    }

    // A soft (0.08 peak) and a hard (0.9 peak) pluck of E4: the gap between them, as played and at 50%.
    {
        const auto soft = pluck (0.08f, 92), hard = pluck (0.9f, 92); // the same pluck, picked 21 dB harder
        auto gapAt = [&] (float amount) { return levelDb (render (hard, amount), 0.2 + lat, 0.2) - levelDb (render (soft, amount), 0.2 + lat, 0.2); };
        const double played = gapAt (100.0f), now = gapAt (fixed);
        writeWav ("dynamics50_soft", render (soft, fixed));
        writeWav ("dynamics50_hard", render (hard, fixed));
        check (std::abs (now - 0.5 * played) < 1.0, juce::String::formatted ("Soft (0.08) vs hard (0.9) pluck: the gap is halved in dB (%.1f dB, as played %.1f dB)", now, played));

        // The chord's other tones take the level before the swell, so they include the gain: Stack (Key C) is evened out the same.
        auto stack = [] (const std::vector<float>& in, float amount) {
            return renderWith (in, [amount] (EngineParams& p) {
                p.dynamicsPercent = amount;
                p.chordMode = ChordMode::Stack;
                p.key = 0;
            });
        };
        const double stackPlayed = levelDb (stack (hard, 100.0f), 0.2 + lat, 0.2) - levelDb (stack (soft, 100.0f), 0.2 + lat, 0.2);
        const double stackNow = levelDb (stack (hard, fixed), 0.2 + lat, 0.2) - levelDb (stack (soft, fixed), 0.2 + lat, 0.2);
        check (std::abs (stackNow - 0.5 * stackPlayed) < 1.0, juce::String::formatted ("Stack (Key C), soft vs hard: the whole chord's gap halved too (%.1f of %.1f dB)", stackNow, stackPlayed));

        // The cap: a ghost note (0.01 peak, ~ -57 dB) would need +19.5 dB at 50%; it's raised by 18 dB, no more.
        const auto ghost = pluck (0.01f, 94);
        const double raised = levelDb (render (ghost, fixed), 0.2 + lat, 0.2) - levelDb (render (ghost, 100.0f), 0.2 + lat, 0.2);
        check (std::abs (raised - kDynamicsMaxBoostDb) < 0.1, juce::String::formatted ("A ghost note (0.01 peak): raised %+.2f dB (the cap, %.0f dB)", raised, kDynamicsMaxBoostDb));
    }

    // No pumping: the note's decay is the same curve as played, offset by one gain, from the end of the
    // reference's 30 ms settle until detection is lost (Sustain 0 and 50%).
    for (float sustain : { 0.0f, 50.0f })
    {
        const auto in = pluck (0.3f, 95);
        const auto a = render (in, 100.0f, sustain), b = render (in, fixed, sustain);
        double lo = 1e9, hi = -1e9;
        for (double t = 0.06; t < 1.2; t += 0.01)
        {
            const double d = levelDb (b, t + lat, 0.01) - levelDb (a, t + lat, 0.01);
            lo = std::min (lo, d);
            hi = std::max (hi, d);
        }
        check (hi - lo < 0.1, juce::String::formatted ("Sustain %.0f%%: the same decay as played, gain %+.2f .. %+.2f dB from 60 ms to 1.2 s (spread %.3f dB)", sustain, lo, hi, hi - lo));
    }

    // Inherited notes keep their gain (Glide Always): a hard E4, a 40 ms dropout, a soft A4. The A4 is
    // played at the E4's gain, so the two keep the level difference they were picked with; a fresh A4 (after
    // the gate has closed) gets its own gain. And no step at the hand-over on a sine.
    {
        const auto a4 = ksPluck ([] (double) { return hzFromMidi (69); }, 1.0, 0.11f, ksLossFor (440.0, 3.0), 100);
        auto second = [&] (double gap, float amount, Wave wave = Wave::Saw, std::vector<float>* keep = nullptr) {
            const auto out = renderWith (joined ({ pluck (0.9f, 99), silence (gap), a4 }), [=] (EngineParams& p) {
                p.dynamicsPercent = amount;
                p.wave = wave;
            });
            if (keep != nullptr)
                *keep = out;
            return levelDb (out, 1.6 + gap + 0.2 + lat, 0.2) - levelDb (out, 0.2 + lat, 0.2);
        };
        const double inherit = second (0.04, fixed), played = second (0.04, 100.0f), fresh = second (1.0, fixed), freshPlayed = second (1.0, 100.0f);
        check (std::abs (inherit - played) < 1.0 && fresh > freshPlayed + 5.0,
               juce::String::formatted ("Hard E4 then a soft A4: after a 40 ms dropout the A4 keeps the E4's gain, %+.1f dB re. the E4 (as played %+.1f); after a 1 s stop it gets its own, %+.1f dB (as played %+.1f)",
                                        inherit, played, fresh, freshPlayed));
        std::vector<float> sineOut;
        second (0.04, fixed, Wave::Sine, &sineOut);
        writeWav ("dynamics50_inherit_sine", sineOut);
        const float step = sineStepRatio (std::vector<float> (sineOut.begin() + (size_t) (1.5 * kFs), sineOut.end()), hzFromMidi (69));
        check (step < 1.2f, juce::String::formatted ("Sine: max step through the dropout and the inherited A4 %.2fx a sine's", step));
    }

    // A soft note right after a loud one stops (Note Release 0, so the gate closes 15 ms after pitch is lost):
    // the envelope follower's 120 ms release still carries the loud note when the soft one starts, and the
    // soft note's gain lifts that tail too. Loudest 10 ms against the soft note's level at 0.2 s.
    {
        const double d4 = hzFromMidi (62);
        auto loud = ksPluck ([d4] (double) { return d4; }, 0.6, 0.9f, ksLossFor (d4, 3.0), 101);
        for (size_t n = (size_t) (0.5 * kFs); n < loud.size(); ++n)
            loud[n] *= (float) std::max (0.0, 1.0 - ((double) n / kFs - 0.5) / 0.003);
        for (double gapS : { 0.03, 0.15 })
        {
            const auto in = joined ({ loud, silence (gapS), pluck (0.08f, 102) });
            const double start = 0.6 + gapS;
            double over[2];
            for (int k = 0; k < 2; ++k)
            {
                const auto out = renderWith (in, [=] (EngineParams& p) {
                    p.dynamicsPercent = k == 0 ? 100.0f : fixed;
                    p.holdMs = 0.0f;
                });
                double peak = -200.0;
                for (double t = start; t < start + 0.15; t += 0.002)
                    peak = std::max (peak, levelDb (out, t + lat, 0.01));
                over[k] = peak - levelDb (out, start + 0.2 + lat, 0.05);
            }
            info (juce::String::formatted ("Loud D4 stopped, %.0f ms later a soft E4, Note Release 0: its start peaks %+.1f dB (as played) / %+.1f dB (Dynamics 50%%) over its level at 200 ms",
                                           1000.0 * gapS, over[0], over[1]));
        }
    }
}

// The note levels (NoteReference) of plucks across the zone at four strengths: what T is chosen from (a
// medium pluck, 0.5 peak, about unchanged). G3 is below the default window, so it reads nothing.
void probeDynamics()
{
    for (int note : { 55, 59, 64, 69, 76 })
        for (float amp : { 0.05f, 0.15f, 0.5f, 0.9f })
        {
            const double hz = hzFromMidi (note);
            const auto in = ksPluck ([hz] (double) { return hz; }, 1.0, amp, ksLossFor (hz, 4.0), 90 + (uint32_t) note);
            EngineParams p = v051Defaults();
            p.dynamicsPercent = 99.0f;
            Engine e;
            e.prepare (kFs, 64);
            std::vector<float> out (in.size());
            double at[3] = {};
            for (size_t s = 0; s < in.size(); s += 64)
            {
                e.process (in.data() + s, out.data() + s, 64, p);
                const double t = (double) s / kFs;
                if (at[0] == 0.0 && t >= 0.1) at[0] = toDb (e.noteLevel());
                if (at[1] == 0.0 && t >= 0.3) at[1] = toDb (e.noteLevel());
                if (at[2] == 0.0 && t >= 0.8) at[2] = toDb (e.noteLevel());
            }
            info (juce::String::formatted ("note %d amp %.2f: ref %.1f / %.1f / %.1f dB at 0.1 / 0.3 / 0.8 s", note, amp, at[0], at[1], at[2]));
        }
}

// ITU-R BS.1770 loudness (LUFS) of a steady signal: K-weighting (the high shelf, then the RLB high-pass),
// mean square, -0.691 + 10 log10. Coefficients from the standard's analogue prototypes at any rate
// (as libebur128). No gating: the signals here are steady tones.
double lufs (const std::vector<float>& x, size_t from, double fs = kFs)
{
    struct Biquad
    {
        double b0, b1, b2, a1, a2, z1 = 0.0, z2 = 0.0;
        double run (double v)
        {
            const double y = b0 * v + z1;
            z1 = b1 * v - a1 * y + z2;
            z2 = b2 * v - a2 * y;
            return y;
        }
    };
    const double kShelf = std::tan (M_PI * 1681.974450955533 / fs), qShelf = 0.7071752369554196;
    const double vh = std::pow (10.0, 3.999843853973347 / 20.0), vb = std::pow (vh, 0.4996667741545416);
    const double a0s = 1.0 + kShelf / qShelf + kShelf * kShelf;
    Biquad shelf { (vh + vb * kShelf / qShelf + kShelf * kShelf) / a0s, 2.0 * (kShelf * kShelf - vh) / a0s, (vh - vb * kShelf / qShelf + kShelf * kShelf) / a0s,
                   2.0 * (kShelf * kShelf - 1.0) / a0s, (1.0 - kShelf / qShelf + kShelf * kShelf) / a0s };
    const double kHp = std::tan (M_PI * 38.13547087602444 / fs), qHp = 0.5003270373238773;
    const double a0h = 1.0 + kHp / qHp + kHp * kHp;
    Biquad hp { 1.0, -2.0, 1.0, 2.0 * (kHp * kHp - 1.0) / a0h, (1.0 - kHp / qHp + kHp * kHp) / a0h };
    double sum = 0.0;
    for (size_t n = 0; n < x.size(); ++n)
    {
        const double y = hp.run (shelf.run (x[n]));
        if (n >= from)
            sum += y * y;
    }
    return -0.691 + 10.0 * std::log10 (sum / (double) (x.size() - from) + 1e-30);
}

// One Tone (the voice's oscillator, LPF, wave scale) at a fixed level: a steady note, 1 s.
std::vector<float> toneAt (Wave w, int note, Octave octave = Octave::Main, float level = 0.1f, double fs = kFs)
{
    Tone t;
    t.prepare (fs);
    VoiceSettings v;
    v.wave = w;
    v.octave = octave;
    t.setVoice (v);
    std::vector<float> x ((size_t) fs);
    for (auto& s : x)
        s = t.render ((float) note, level);
    return x;
}

// Wave levels. v0.5.1 matched Square, Triangle and Sine to Saw by BS.1770 loudness; v0.5.2 raises Triangle
// and Sine 10 dB over that, by ear. Square within 1 LU of Saw and Triangle / Sine within 1 LU of +10 at
// every note G3 .. E5 (Octave Main, default Low Pass Filter); Triangle and Sine exactly
// +10 dB over v0.5.1 in the engine, Saw and Square = v0.5.1; Saw = v0.5 and the others = v0.5 with the
// mean-|x| levels. (The headroom with the new gains: test headroom.)
// The pulse wave (v0.5.7; NES Lead): Pulse 25%, built as Square (PolyBLEP at both edges, the DC taken off).
//  - Duty: every 4th harmonic missing (bare oscillator at 187.5 Hz, 48 kHz: the harmonics on bins).
//  - Aliases like Square's: the worst component that isn't a harmonic (aliasReport) at 1 kHz / 44.1 kHz, C6, C#6 and 1400 Hz
//    / 48 kHz, re. its own fundamental and re. Square's (both at their loudness-matched level): within 6 dB of Square's up
//    to C#6 (1400 Hz reported).
//  - Within +/-1 everywhere (the headroom gain's bound), peaking at +1.
//  - Loudness within 1 dB of Saw (BS.1770) at every note G3 .. E5, Octave Main, 44.1 / 48 / 96 kHz.
double harmonicAmp (Wave w, double f0, double fs, int h)
{
    constexpr int order = 17;
    const int n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) (2 * n), 0.0f);
    const double dt = f0 / fs;
    double phase = 0.0;
    for (int k = 0; k < n; ++k)
    {
        const double a = 2.0 * M_PI * k / (n - 1);
        const double win = 0.35875 - 0.48829 * std::cos (a) + 0.14128 * std::cos (2 * a) - 0.01168 * std::cos (3 * a);
        buf[(size_t) k] = (float) (win * oscSample (w, phase, dt));
        phase += dt;
        if (phase >= 1.0)
            phase -= 1.0;
    }
    fft.performFrequencyOnlyForwardTransform (buf.data());
    const double binHz = fs / n;
    const int c = (int) std::lround (h * f0 / binHz);
    float m = 0.0f;
    for (int k = c - 4; k <= c + 4; ++k)
        m = std::max (m, buf[(size_t) k]);
    return m;
}

void testPulseWaves()
{
    struct P
    {
        const char* name;
        Wave w;
        int missingEvery;
        float lufsDb;
    };
    const P pulses[] = { { "Pulse 25%", Wave::Pulse25, 4, kPulse25LoudnessDb } };
    for (const auto& pw : pulses)
    {
        // Duty.
        const double f1 = harmonicAmp (pw.w, 187.5, 48000.0, 1);
        double worstMissing = -300.0, weakestPresent = 0.0;
        juce::String line;
        for (int h = 1; h <= 24; ++h)
        {
            const double db = toDb (harmonicAmp (pw.w, 187.5, 48000.0, h) / f1);
            if (h % pw.missingEvery == 0)
                worstMissing = std::max (worstMissing, db);
            else
                weakestPresent = std::min (weakestPresent, db);
            if (h <= 2 * pw.missingEvery)
                line << h << ": " << juce::String (db, 1) << "  ";
        }
        check (worstMissing < -60.0 && weakestPresent > -40.0,
               juce::String::formatted ("%-11s duty: every %dth harmonic missing (at most %.0f dB re. the fundamental; the others %.1f dB or more), harmonics 1 .. %d: ", pw.name,
                                        pw.missingEvery, worstMissing, weakestPresent, 2 * pw.missingEvery)
                   + line.trimEnd());

        // Aliases, against Square's.
        struct Case
        {
            double f0, fs;
        };
        for (const Case c : { Case { 1000.0, 44100.0 }, Case { 1046.5, 48000.0 }, Case { 1108.7, 48000.0 }, Case { 1400.0, 48000.0 } })
        {
            auto osc = [&pw] (double p, double dt) { return oscSample (pw.w, p, dt); };
            auto sq = [] (double p, double dt) { return oscSample (Wave::Square, p, dt); };
            const auto a = aliasReport (osc, c.f0, c.fs), b = aliasReport (sq, c.f0, c.fs);
            // Also re. Square's fundamental with both at their loudness-matched level (waveScale): the pulse's own fundamental
            // is weaker within its spectrum (sin (pi duty)), so re. it alone its aliases read higher.
            const double fundDb = toDb (harmonicAmp (pw.w, c.f0, c.fs, 1) * waveScale (pw.w) / (harmonicAmp (Wave::Square, c.f0, c.fs, 1) * waveScale (Wave::Square)));
            const double below10 = std::max (a.bandDb[0], a.bandDb[1]), sqBelow10 = std::max (b.bandDb[0], b.bandDb[1]);
            // Checked at the guitar's notes (up to C#6, the 21-fret top); 1400 Hz (the detector's edge, nothing a guitar
            // plays) reported.
            const bool like = a.worstDb + fundDb < b.worstDb + 6.0 && below10 + fundDb < sqBelow10 + 6.0;
            const auto text = juce::String::formatted ("%-11s %6.1f Hz @ %4.1f kHz: worst alias %.1f dB re. its fundamental (%.1f re. Square's, both loudness-matched), below 10 kHz %.1f (%.1f); Square %.1f, below 10 kHz %.1f",
                                                       pw.name, c.f0, c.fs / 1000.0, a.worstDb, a.worstDb + fundDb, below10, below10 + fundDb, b.worstDb, sqBelow10);
            if (c.f0 < 1200.0)
                check (like, text);
            else
                info (text + (like ? " (within 6 dB of Square's)" : " (more than 6 dB over Square's)"));
        }

        // The bound.
        double lo = 1e9, hi = -1e9;
        for (double dt : { 55.0 / 48000.0, 440.0 / 48000.0, 1400.0 / 44100.0 })
            for (double p = 0.0; p < 1.0; p += 0.25 * dt)
            {
                const double y = oscSample (pw.w, p, dt);
                lo = std::min (lo, y);
                hi = std::max (hi, y);
            }
        check (hi <= 1.0 && lo >= -1.0 && hi > 0.999, juce::String::formatted ("%-11s within +/-1: %.4f .. %.4f", pw.name, lo, hi));

        // Loudness.
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            double lo2 = 1e9, hi2 = -1e9, sum = 0.0;
            int n = 0;
            for (int note = 55; note <= 76; ++note)
            {
                const auto from = (size_t) (0.1 * fs);
                const double d = lufs (toneAt (pw.w, note, Octave::Main, 0.1f, fs), from, fs) - lufs (toneAt (Wave::Saw, note, Octave::Main, 0.1f, fs), from, fs);
                lo2 = std::min (lo2, d);
                hi2 = std::max (hi2, d);
                sum += d;
                ++n;
            }
            check (lo2 > -1.0 && hi2 < 1.0, juce::String::formatted ("%-11s @ %4.1f kHz, G3 .. E5: %+.2f .. %+.2f LU re. Saw, mean %+.2f (gain %+.2f dB on the mean-|x| scale)", pw.name,
                                                                    fs / 1000.0, lo2, hi2, sum / n, pw.lufsDb));
        }
    }
}

// Loudness re. Saw (BS.1770) of a wave's bare Tone at every note G3 .. E5, Octave Main, at a rate: the lowest, highest and mean.
struct LoudnessSpread
{
    double lo = 1e9, hi = -1e9, mean = 0.0;
};
LoudnessSpread loudnessReSaw (Wave w, double fs)
{
    LoudnessSpread r;
    int n = 0;
    for (int note = 55; note <= 76; ++note)
    {
        const auto from = (size_t) (0.1 * fs);
        const double d = lufs (toneAt (w, note, Octave::Main, 0.1f, fs), from, fs) - lufs (toneAt (Wave::Saw, note, Octave::Main, 0.1f, fs), from, fs);
        r.lo = std::min (r.lo, d);
        r.hi = std::max (r.hi, d);
        r.mean += d;
        ++n;
    }
    r.mean /= n;
    return r;
}

// The mean of a bare oscillator over whole cycles, re. its peak (dB): its DC.
double oscDcDb (Wave w, double f0, double fs)
{
    const double dt = f0 / fs;
    const int cycles = 200, n = (int) std::lround (cycles / dt);
    double phase = 0.0, sum = 0.0;
    for (int k = 0; k < n; ++k)
    {
        sum += oscSample (w, phase, dt);
        phase += dt;
        if (phase >= 1.0)
            phase -= 1.0;
    }
    return toDb (std::abs (sum / n));
}

// One component of a signal (Goertzel, Hann window), as an amplitude.
double componentAmp (const std::vector<float>& x, size_t from, double hz, double fs)
{
    const size_t n = x.size() - from;
    const double w = 2.0 * M_PI * hz / fs;
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (size_t k = 0; k < n; ++k)
    {
        const double win = 0.5 - 0.5 * std::cos (2.0 * M_PI * (double) k / (double) (n - 1));
        re += win * x[from + k] * std::cos (w * (double) k);
        im -= win * x[from + k] * std::sin (w * (double) k);
        wsum += win;
    }
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

// Half-Sine and Organ (v0.5.8).
//  - Half-Sine: the fundamental and even harmonics only (odd ones from the 3rd under -60 dB), each even one within 0.5 dB of
//    the rectified sine's 2 / (pi (4k^2 - 1)) re. 1/2 and below the one before (bare oscillator at 187.5 Hz, 48 kHz: the
//    harmonics on bins); no DC (G3 .. C#6, 44.1 / 48 / 96 kHz: under -60 dB re. its peak); within +/-1, peaking at +1;
//    aliases at 1 kHz / 44.1 kHz, C6, C#6 and 1400 Hz / 48 kHz re. its fundamental, beside the triangle's and its own
//    without PolyBLAMP (reported).
//  - Organ: the fundamental, 2nd and 3rd harmonics at kOrganLevel1 / 2 / 3 (within 0.05 dB), nothing else (under -100 dB),
//    no DC; one phase (the sum of the three sines on it, exactly); within +/-1, peaking at +1; Octave Sub moves the whole
//    tone (a Tone at Octave Sub: the three an octave down at the same levels, within 0.3 dB with the filter).
//  - Loudness: each within 1 dB of Saw (BS.1770) at every note G3 .. E5, Octave Main, 44.1 / 48 / 96 kHz.
void testHalfSineOrgan()
{
    // Half-Sine: the spectrum.
    {
        const double f1 = harmonicAmp (Wave::HalfSine, 187.5, 48000.0, 1);
        double worstOdd = -300.0, worstErr = 0.0;
        bool falling = true;
        double last = 1e9;
        juce::String line;
        for (int h = 2; h <= 24; ++h)
        {
            const double db = toDb (harmonicAmp (Wave::HalfSine, 187.5, 48000.0, h) / f1);
            if (h % 2 == 1)
                worstOdd = std::max (worstOdd, db);
            else
            {
                const int k = h / 2;
                const double want = toDb ((2.0 / (M_PI * (4.0 * k * k - 1.0))) / 0.5);
                worstErr = std::max (worstErr, std::abs (db - want));
                falling = falling && db < last;
                last = db;
                if (h <= 8)
                    line << h << ": " << juce::String (db, 2) << " (" << juce::String (want, 2) << ")  ";
            }
        }
        check (worstOdd < -60.0 && worstErr < 0.5 && falling,
               juce::String::formatted ("Half-Sine: even harmonics only (odd from the 3rd at most %.0f dB re. the fundamental), each within %.2f dB of the rectified sine's and falling: ", worstOdd, worstErr)
                   + line.trimEnd());
    }
    // Organ: the spectrum and the one phase.
    {
        const double f1 = harmonicAmp (Wave::Organ, 187.5, 48000.0, 1);
        const double h2 = toDb (harmonicAmp (Wave::Organ, 187.5, 48000.0, 2) / f1), h3 = toDb (harmonicAmp (Wave::Organ, 187.5, 48000.0, 3) / f1);
        double worstOther = -300.0;
        for (int h = 4; h <= 24; ++h)
            worstOther = std::max (worstOther, toDb (harmonicAmp (Wave::Organ, 187.5, 48000.0, h) / f1));
        const double want2 = toDb (kOrganLevel2 / kOrganLevel1), want3 = toDb (kOrganLevel3 / kOrganLevel1);
        check (std::abs (h2 - want2) < 0.05 && std::abs (h3 - want3) < 0.05 && worstOther < -100.0,
               juce::String::formatted ("Organ: 2nd %.2f dB, 3rd %.2f dB re. the fundamental (set %.2f / %.2f: levels %.2f / %.2f / %.2f), the 4th .. 24th at most %.0f dB", h2, h3, want2,
                                        want3, kOrganLevel1, kOrganLevel2, kOrganLevel3, worstOther));
        double worstFormula = 0.0;
        for (int k = 0; k < 100000; ++k)
        {
            const double p = k / 100000.0, th = 2.0 * M_PI * p;
            const double want = (kOrganLevel1 * std::sin (th) + kOrganLevel2 * std::sin (2.0 * th) + kOrganLevel3 * std::sin (3.0 * th)) / kOrganPeak;
            worstFormula = std::max (worstFormula, std::abs (oscSample (Wave::Organ, p, 0.01) - want));
        }
        check (worstFormula < 1e-12, juce::String::formatted ("Organ: phase-locked (the three sines on the one phase): at most %.1e from the sum, normalized by its peak %.6f", worstFormula, kOrganPeak));
        // Octave Sub: the whole tone an octave down.
        const auto x = toneAt (Wave::Organ, 69, Octave::Sub);
        const double a1 = componentAmp (x, 4800, 220.0, kFs), a2 = toDb (componentAmp (x, 4800, 440.0, kFs) / a1), a3 = toDb (componentAmp (x, 4800, 660.0, kFs) / a1);
        check (std::abs (a2 - want2) < 0.3 && std::abs (a3 - want3) < 0.3,
               juce::String::formatted ("Organ at Octave Sub (A4 played): 220 / 440 / 660 Hz, the 2nd %.2f dB and 3rd %.2f dB re. the 1st (the whole tone an octave down)", a2, a3));
    }
    // DC, the bound and the aliases, for both.
    for (const auto& [name, w] : { std::pair<const char*, Wave> { "Half-Sine", Wave::HalfSine }, std::pair<const char*, Wave> { "Organ", Wave::Organ } })
    {
        double worstDc = -300.0;
        for (double fs : { 44100.0, 48000.0, 96000.0 })
            for (double f0 : { 196.0, 440.0, 1108.7 })
                worstDc = std::max (worstDc, oscDcDb (w, f0, fs));
        double lo = 1e9, hi = -1e9;
        for (double dt : { 55.0 / 48000.0, 440.0 / 48000.0, 1108.7 / 44100.0, 1400.0 / 44100.0 })
            for (double p = 0.0; p < 1.0; p += 0.01 * dt)
            {
                const double y = oscSample (w, p, dt);
                lo = std::min (lo, y);
                hi = std::max (hi, y);
            }
        check (worstDc < -60.0 && hi <= 1.0 && lo >= -1.0 && hi > 0.999,
               juce::String::formatted ("%-9s no DC (at most %.0f dB re. its peak, G3 / A4 / C#6 at 44.1 / 48 / 96 kHz); within +/-1: %.4f .. %.4f", name, worstDc, lo, hi));
    }
    struct Case
    {
        double f0, fs;
    };
    for (const Case c : { Case { 1000.0, 44100.0 }, Case { 1046.5, 48000.0 }, Case { 1108.7, 48000.0 }, Case { 1400.0, 48000.0 } })
    {
        auto half = [] (double p, double dt) { return oscSample (Wave::HalfSine, p, dt); };
        auto naive = [] (double p, double) { return ((p < 0.5 ? std::sin (2.0 * M_PI * p) : 0.0) - kHalfSineDc) / (1.0 - kHalfSineDc); };
        auto tri = [] (double p, double dt) { return oscSample (Wave::Triangle, p, dt); };
        const auto a = aliasReport (half, c.f0, c.fs), b = aliasReport (naive, c.f0, c.fs), t = aliasReport (tri, c.f0, c.fs);
        info (juce::String::formatted ("Half-Sine %6.1f Hz @ %4.1f kHz: worst alias %.1f dB re. its fundamental, below 10 kHz %.1f (without PolyBLAMP %.1f / %.1f; Triangle %.1f / %.1f)", c.f0,
                                       c.fs / 1000.0, a.worstDb, std::max (a.bandDb[0], a.bandDb[1]), b.worstDb, std::max (b.bandDb[0], b.bandDb[1]), t.worstDb,
                                       std::max (t.bandDb[0], t.bandDb[1])));
    }
    // Loudness.
    for (const auto& [name, w, gain] : { std::tuple<const char*, Wave, float> { "Half-Sine", Wave::HalfSine, kHalfSineLoudnessDb }, std::tuple<const char*, Wave, float> { "Organ", Wave::Organ, kOrganLoudnessDb } })
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            const auto r = loudnessReSaw (w, fs);
            check (r.lo > -1.0 && r.hi < 1.0, juce::String::formatted ("%-9s @ %4.1f kHz, G3 .. E5: %+.2f .. %+.2f LU re. Saw, mean %+.2f (gain %+.2f dB on the mean-|x| scale)", name, fs / 1000.0,
                                                                    r.lo, r.hi, r.mean, gain));
        }
}

// The engine as it ships (or with the headroom stage off): the output and the deepest headroom gain.
std::vector<float> renderHeadroom (const std::vector<float>& in, const EngineParams& p, float* deepestDb, bool headroom = true)
{
    Engine e;
    e.prepare (kFs, 64);
    e.setHeadroomGain (headroom);
    std::vector<float> out (in.size());
    float deepest = 0.0f;
    for (size_t k = 0; k < out.size(); k += 64)
    {
        e.process (in.data() + k, out.data() + k, (int) std::min<size_t> (64, out.size() - k), p);
        deepest = std::min (deepest, e.readout().gainReductionDb.load());
    }
    if (deepestDb != nullptr)
        *deepestDb = deepest;
    return out;
}

struct ClipStats;
ClipStats clipStats (const std::vector<float>& y);

void testWaveLoudness()
{
    static const char* const names[4] = { "Saw", "Square", "Triangle", "Sine" };
    const double byEar[4] = { 0.0, 0.0, kTriangleByEarDb, kSineByEarDb };
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (int w = 1; w < 4; ++w)
        {
            double lo = 1e9, hi = -1e9;
            for (int note = 55; note <= 76; ++note)
            {
                const auto from = (size_t) (0.1 * fs);
                const double d = lufs (toneAt ((Wave) w, note, Octave::Main, 0.1f, fs), from, fs) - lufs (toneAt (Wave::Saw, note, Octave::Main, 0.1f, fs), from, fs);
                lo = std::min (lo, d);
                hi = std::max (hi, d);
            }
            check (lo > byEar[w] - 1.0 && hi < byEar[w] + 1.0,
                   juce::String::formatted ("%-8s @ %4.1f kHz, G3 .. E5: %+.2f .. %+.2f LU re. Saw (within 1 of %+.0f)", names[w], fs / 1000.0, lo, hi, byEar[w]));
        }
    // Sub octaves (info: expected and accepted that Sine is thinner down there).
    for (Octave o : { Octave::Sub, Octave::DoubleSub, Octave::TripleSub })
    {
        juce::String line;
        for (int w = 1; w < 4; ++w)
        {
            double lo = 1e9, hi = -1e9;
            for (int note = 55; note <= 76; ++note)
            {
                const double d = lufs (toneAt ((Wave) w, note, o), 4800) - lufs (toneAt (Wave::Saw, note, o), 4800);
                lo = std::min (lo, d);
                hi = std::max (hi, d);
            }
            line << names[w] << juce::String::formatted (" %+.1f .. %+.1f  ", lo, hi);
        }
        info (juce::String::formatted ("Octave %-10s LU re. Saw, G3 .. E5: ", o == Octave::Sub ? "Sub" : (o == Octave::DoubleSub ? "Double Sub" : "Triple Sub")) + line.trimEnd());
    }

    // The gain itself: each wave's scale against v0.5.1's.
    {
        juce::String line;
        bool ok = true;
        for (int w = 0; w < 4; ++w)
        {
            const double d = toDb ((double) waveScale ((Wave) w) / (double) waveScale ((Wave) w, WaveLevels::V051));
            ok = ok && (w < 2 ? std::equal_to<float>() (waveScale ((Wave) w), waveScale ((Wave) w, WaveLevels::V051)) : std::abs (d - 10.0) < 1e-4);
            line << names[w] << juce::String::formatted (" %+.4f dB  ", d);
        }
        check (ok, "Wave scale re. v0.5.1: " + line.trimEnd() + " (Saw and Square the same bits)");
    }

}

// ---------------------------------------------------------------------------
// v0.5.3: the headroom gain before the safety clip
// ---------------------------------------------------------------------------

// A Chord selector index as the plugin reads it: the mode, the key and (v0.5.6, Freeze) Memory Notes.
void setChordChoice (EngineParams& p, int index)
{
    const auto c = chordChoiceFor (index);
    p.chordMode = c.mode;
    p.key = c.key;
    p.memoryNotes = c.memoryNotes;
}
// v0.5.6's options by name: Freeze N (Memory with N notes) and Harmonize in a key (Stack).
constexpr int freezeChoice (int notes) { return kFirstFreezeChoice + notes - kMemoryNotesMin; }
constexpr int harmonizeChoice (int key) { return kFirstHarmonizeChoice + key; }
EngineParams chordChoiceParams (int index) // ... on the engine's defaults
{
    EngineParams p;
    setChordChoice (p, index);
    return p;
}

// Seven plucks over the zone, B3 .. E5, each 0.8 s, 30 ms apart (the v0.5.2 headroom table's input).
std::vector<float> zonePlucks (float amp, double fs = kFs)
{
    std::vector<float> in;
    uint32_t seed = 120;
    for (int note : { 59, 62, 64, 67, 69, 72, 76 })
    {
        const double hz = hzFromMidi (note);
        const auto pl = ksPluck ([hz] (double) { return hz; }, 0.8, amp, ksLossFor (hz, 3.0), seed++, fs);
        in.insert (in.end(), pl.begin(), pl.end());
        in.insert (in.end(), (size_t) (0.03 * fs), 0.0f);
    }
    return in;
}

// Render with v0.5.2's settings, and the deepest headroom gain reduction (dB) on the way. legacy: as v0.5.3 behaved (memory
// clears with the voice gate, the reference frozen while pitch is lost).
std::vector<float> renderV053 (const std::vector<float>& in, const EngineParams& p, double fs = kFs, float* deepestDb = nullptr, Engine* engine = nullptr, bool legacy = true)
{
    Engine local;
    Engine& e = engine != nullptr ? *engine : local;
    e.prepare (fs, 64);
    e.setLegacyMemoryClear (legacy); // memory clears with the voice gate, as before v0.5.4
    e.setLegacyNoteReference (legacy); // the note's reference frozen while pitch is lost, as before v0.5.4
    e.setSputterFix (false); // the sputter fix off, as v0.5.4 (v0.5.4's Dynamics fix measured alone)
    std::vector<float> out (in.size());
    float deepest = 0.0f;
    for (size_t k = 0; k < out.size(); k += 64)
    {
        e.process (in.data() + k, out.data() + k, (int) std::min<size_t> (64, out.size() - k), p);
        deepest = std::min (deepest, e.readout().gainReductionDb.load());
    }
    if (deepestDb != nullptr)
        *deepestDb = deepest;
    return out;
}

// The safety clip's work in an output: samples where it acts (|y| > 0.9, the knee), and the energy it took
// off against the signal before it (recovered exactly by inverting the clip's tanh).
struct ClipStats
{
    double share = 0.0, removedDb = -200.0, rmsDb = -200.0;
};
ClipStats clipStats (const std::vector<float>& y)
{
    ClipStats c;
    size_t n = 0;
    double removed = 0.0, total = 0.0;
    for (float v : y)
    {
        const double a = std::abs ((double) v);
        double pre = a;
        if (a > kSafetyClipKnee)
        {
            ++n;
            pre = kSafetyClipKnee + 0.1 * std::atanh (std::min (0.999999, (a - kSafetyClipKnee) / 0.1));
        }
        removed += (pre - a) * (pre - a);
        total += pre * pre;
    }
    c.share = 100.0 * (double) n / (double) y.size();
    c.removedDb = n > 0 ? toDb (std::sqrt (removed / total)) : -200.0;
    c.rmsDb = toDb (std::sqrt (total / (double) y.size()));
    return c;
}

// Headroom (v0.5.7), as the v0.5.3 table (seven plucks over the zone, B3 .. E5; medium 0.5 / hard 0.9 peak) at the
// plugin's defaults (Sustain 75%), every wave with Chord Off / Harmonize / Freeze: per wave, the safety clip with the headroom
// stage off (samples / energy it takes),
// the deepest headroom gain, and the RMS re. the stage off. Checked: the clip never acts with the stage, and no sample over
// the ceiling by 0.1 dB.
using NamedWave = std::pair<const char*, Wave>;
void headroomTable (const std::vector<NamedWave>& waves)
{
    const auto hard = zonePlucks (0.9f), medium = zonePlucks (0.5f);
    struct Row
    {
        const char* name;
        const std::vector<float>* in;
        int chord;
    };
    const Row rows[] = {
        { "Medium plucks, Chord Off          ", &medium, 0 },
        { "Hard plucks, Chord Off            ", &hard, 0 },
        { "Medium plucks, Harmonize C maj    ", &medium, harmonizeChoice (0) },
        { "Hard plucks, Harmonize C maj      ", &hard, harmonizeChoice (0) },
        { "Medium plucks, Freeze 2           ", &medium, freezeChoice (2) },
        { "Medium plucks, Freeze 3           ", &medium, freezeChoice (3) },
        { "Medium plucks, Freeze 4           ", &medium, freezeChoice (4) },
        { "Hard plucks, Freeze 4             ", &hard, freezeChoice (4) },
        { "Hard plucks, Freeze 3             ", &hard, freezeChoice (3) },
    };
    info ("Per wave: the clip with the headroom stage off (samples / energy off), the deepest headroom gain, RMS re. the stage off:");
    auto text = [] (const ClipStats& c) { return c.share > 0.0 ? juce::String::formatted ("%.2f%% / %.0f dB", c.share, c.removedDb) : juce::String ("-"); };
    bool clean = true, overshootOk = true;
    double worstOver = -200.0;
    for (const auto& r : rows)
    {
        std::vector<std::future<juce::String>> jobs;
        std::mutex m;
        for (const auto& nw : waves)
            jobs.push_back (std::async (std::launch::async, [&, nw] {
                const auto& [name, wave] = nw;
                EngineParams p = chordChoiceParams (r.chord);
                p.wave = wave;
                float deepest = 0.0f;
                const auto now = renderHeadroom (*r.in, p, &deepest), off = renderHeadroom (*r.in, p, nullptr, false);
                const auto a = clipStats (now), b = clipStats (off);
                float peak = 0.0f;
                for (float v : now)
                    peak = std::max (peak, std::abs (v));
                {
                    std::lock_guard<std::mutex> lock (m);
                    clean = clean && a.share == 0.0;
                    if (deepest < 0.0f)
                    {
                        worstOver = std::max (worstOver, toDb (peak / kHeadroomCeiling));
                        overshootOk = overshootOk && peak <= kHeadroomCeiling * dbToGain (0.1f);
                    }
                }
                return juce::String (name) + " " + text (b) + juce::String::formatted (" GR %.1f, %+.1f dB   ", deepest, a.rmsDb - b.rmsDb);
            }));
        juce::String line;
        for (auto& j : jobs)
            line << j.get();
        info ("  " + juce::String (r.name) + "  " + line.trimEnd());
    }
    check (clean, "Every wave: the safety clip never acts with the headroom stage");
    check (overshootOk, juce::String::formatted ("Wherever it engages, no sample over the ceiling by 0.1 dB (closest %+.2f dB)", worstOver));
}
void testHeadroomV057()
{
    headroomTable ({ { "Saw", Wave::Saw }, { "Square", Wave::Square }, { "Pulse 25%", Wave::Pulse25 }, { "Triangle", Wave::Triangle }, { "Sine", Wave::Sine } });
}
// v0.5.8: the same table for the new waves (Saw beside them for reference).
void testHeadroomV058()
{
    headroomTable ({ { "Saw", Wave::Saw }, { "Half-Sine", Wave::HalfSine }, { "Organ", Wave::Organ }, { "Folded", Wave::Folded } });
}

namespace sputter
{
std::vector<std::pair<juce::String, std::vector<float>>> loadRecordings();
}

// Folded (v0.5.8): the engine's output and Folded's drive, every sample (and whether the voice is open).
struct FoldedRun
{
    std::vector<float> out, drive;
    std::vector<bool> open;
};
FoldedRun renderFolded (const std::vector<float>& in, const EngineParams& p, double fs = kFs)
{
    Engine e;
    e.prepare (fs, 64);
    FoldedRun r;
    r.drive.reserve (in.size());
    e.voiceListener = [&r] (const VoiceProbe& v) {
        r.drive.push_back (v.foldDrive);
        r.open.push_back (v.open);
    };
    r.out = runEngine (e, in, p);
    return r;
}
// The spectral centroid (power-weighted, Hz) of 4096 samples from `from` (Hann).
double centroidHz (const std::vector<float>& x, size_t from, double fs = kFs)
{
    constexpr int order = 12, n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf (2 * n, 0.0f);
    for (int k = 0; k < n && from + (size_t) k < x.size(); ++k)
        buf[(size_t) k] = x[from + (size_t) k] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * k / (n - 1)));
    fft.performFrequencyOnlyForwardTransform (buf.data());
    double num = 0.0, den = 0.0;
    for (int k = 2; k < n / 2; ++k)
    {
        const double pw = (double) buf[(size_t) k] * buf[(size_t) k];
        num += pw * k * fs / n;
        den += pw;
    }
    return den > 0.0 ? num / den : 0.0;
}
// The power that isn't at a harmonic of f0 (more than `away` Hz from every one, DC excluded), re. the total (dB): aliases,
// zipper noise, clicks. Blackman-Harris, 2^17 samples from `from`. below: only up to this frequency.
double offHarmonicDb (const std::vector<float>& x, size_t from, double f0, double away, double fs = kFs, double below = 1e9)
{
    constexpr int order = 17, n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf (2 * n, 0.0f);
    for (int k = 0; k < n && from + (size_t) k < x.size(); ++k)
    {
        const double a = 2.0 * M_PI * k / (n - 1);
        buf[(size_t) k] = x[from + (size_t) k] * (float) (0.35875 - 0.48829 * std::cos (a) + 0.14128 * std::cos (2 * a) - 0.01168 * std::cos (3 * a));
    }
    fft.performFrequencyOnlyForwardTransform (buf.data());
    double off = 0.0, total = 0.0;
    for (int k = 8; k < n / 2; ++k)
    {
        const double hz = k * fs / n, pw = (double) buf[(size_t) k] * buf[(size_t) k];
        total += pw;
        const double h = std::max (1.0, std::round (hz / f0));
        if (std::abs (hz - h * f0) > away && hz <= below)
            off += pw;
    }
    return 10.0 * std::log10 (std::max (off, 1e-30) / std::max (total, 1e-30));
}

// Folded (v0.5.8; v0.5.9: the drive from the pick's level before Dynamics, up to kFoldMaxDrive 6): a sine through a wavefolder
// whose drive follows the note's level.
//  - The drive (kFoldSoftDb, kFoldDbPerDb, kFoldMaxDrive): single plucks B3 / E4 / E5 at 0.08 / 0.2 / 0.5 / 0.9 peak at the
//    plugin's defaults (Sustain 75%), the drive's largest and at 100 / 500 ms (reported); checked: a soft pick (0.08) stays a
//    plain sine (drive at most 1.05), a hard one (0.9) reaches at least 5, a medium one (0.5) between.
//  - Brightness: the spectral centroid 30 .. 115 ms into each pluck, soft / medium / hard at B3, E4, E5 (reported); checked: it rises with the pick (soft 0.08 < medium 0.5 < hard 0.9; 0.2 no lower than 0.08) at every
//    note, Chord Off and Harmonize C maj (the chord's tones share the drive), and at E4 the hard pick is at least 4x the soft.
//  - Brightness against the drive: a bare Tone (the 5 kHz filter included) at B3 / E4 / E5, drives 1 .. kFoldMaxDrive in steps
//    of 0.05: the centroid never falls by more than 1% from one step to the next (steadily brighter, no dips).
//  - The decay: at Sustain 0 the centroid falls through a hard E4 (every 25 ms from 0.03 to 0.5 s: never up by more than 1%,
//    and down by half overall); at Sustain 100% it holds (within 3% from 0.1 to 1.2 s).
//  - No zipper noise or clicks as the drive moves: an E4 sine swelling 0.15 .. 0.9 peak and back at 0.25 Hz (Sustain 0, so the
//    drive sweeps its whole range and back every 4 s): the power more than 60 Hz from every harmonic under -60 dB re. the total
//    (Saw beside it). The drive moves every sample, by at most 0.05 in any one.
//  - Loudness of the bare folder against its drive (a Tone at E4, drives 1 .. 6, re. Saw): reported.
//  - Aliases at a hard pick (the bare folder at a 0.9 pluck's drive, and at kFoldMaxDrive) at 1 kHz / 44.1 kHz, C6, C#6 and
//    1400 Hz / 48 kHz: reported against the folder without the doubled rate, and without PolyBLAMP either; checked: the
//    alias power below 10 kHz under -60 dB re. the signal up to C#6.
//  - The bound: the three stages' sum within +/-1 before its clamp (drives 1 .. kFoldMaxDrive + 2, every phase), and the
//    folder's output (the binomial filter included) within +/-1, so Tone::peak still bounds it.
//  - Loudness: within 1 dB of Saw (BS.1770) on medium (0.5 peak) single plucks G3 .. E5 at the defaults (the window at its
//    minimum, so G3 .. A#3 are played too); hard (0.9) reported, v0.5.8 beside it.
//  - CPU: a Tone (Saw, and Folded at a hard pick's drive), and the engine on the bench's phrase with Saw / Folded, Chord Off /
//    Harmonize C maj / Freeze 3 (reported).
//  - The comparison WAVs (build/test-output/folded/): one phrase of soft, medium and hard picks on Triangle, v0.5.8's Folded and
//    this Folded, at Sustain 0 and 75%.
void testFolded()
{
    auto pluckAt = [] (int note, float amp, double seconds = 1.2, double t60 = 3.0) {
        const double hz = hzFromMidi (note);
        auto x = ksPluck ([hz] (double) { return hz; }, seconds, amp, ksLossFor (hz, t60), 77);
        x.insert (x.begin(), (size_t) (0.05 * kFs), 0.0f); // 50 ms of silence first
        return x;
    };
    const size_t start = (size_t) (0.05 * kFs) + (size_t) latencyFor (kFs, defaultWindowHz());
    auto folded = [] (int chord = 0) {
        EngineParams p = chordChoiceParams (chord);
        p.wave = Wave::Folded;
        return p;
    };
    float largestStep = 0.0f;
    auto steps = [&largestStep] (const std::vector<float>& d) {
        for (size_t k = 1; k < d.size(); ++k)
            largestStep = std::max (largestStep, std::abs (d[k] - d[k - 1]));
    };

    // The drive.
    const float amps[4] = { 0.08f, 0.2f, 0.5f, 0.9f };
    float hardDrive = 0.0f;
    bool driveOk = true;
    for (int note : { 59, 64, 76 })
    {
        juce::String line;
        float peaks[4] {};
        for (int a = 0; a < 4; ++a)
        {
            const auto r = renderFolded (pluckAt (note, amps[a]), folded());
            steps (r.drive);
            peaks[a] = *std::max_element (r.drive.begin(), r.drive.end());
            line << juce::String::formatted ("%.2f: max %.2f, %.2f / %.2f   ", amps[a], peaks[a], r.drive[start + (size_t) (0.1 * kFs)], r.drive[start + (size_t) (0.5 * kFs)]);
        }
        hardDrive = std::max (hardDrive, peaks[3]);
        driveOk = driveOk && peaks[0] <= 1.05f && peaks[3] >= 5.0f && peaks[2] > peaks[0] && peaks[2] < peaks[3];
        info (juce::String::formatted ("Folded drive, %s pluck (peak: largest, at 100 / 500 ms): ", juce::MidiMessage::getMidiNoteName (note, true, true, 4).toRawUTF8()) + line.trimEnd());
    }
    check (driveOk, juce::String::formatted ("Folded drive: a soft pick (0.08) at most 1.05 (a plain sine), a hard one (0.9) at least 5 (largest %.2f), a medium one between (drive = (level before Dynamics / %.0f dB)^%.2f, 1 .. %.0f)",
                                             hardDrive, kFoldSoftDb, kFoldDbPerDb, kFoldMaxDrive));

    // Brightness with the pick.
    {
        double soft = 0.0, hard = 0.0;
        for (int note : { 59, 64, 76 })
        {
            juce::String line;
            for (int a : { 0, 2, 3 })
            {
                const auto in = pluckAt (note, amps[a]);
                const double now = centroidHz (renderFolded (in, folded()).out, start + (size_t) (0.03 * kFs));
                line << juce::String::formatted ("%s %.0f Hz   ", a == 0 ? "soft" : a == 2 ? "medium" : "hard", now);
                if (note == 64 && a == 0)
                    soft = now;
                if (note == 64 && a == 3)
                    hard = now;
            }
            info (juce::String::formatted ("Folded centroid 30 .. 115 ms in, %s: ", juce::MidiMessage::getMidiNoteName (note, true, true, 4).toRawUTF8()) + line.trimEnd());
        }
        check (hard >= 4.0 * soft, juce::String::formatted ("Folded at E4: the hard pick's centroid is %.2fx the soft pick's (%.0f / %.0f Hz; at least 4x)", hard / soft, hard, soft));
    }
    for (int chord : { 0, harmonizeChoice (0) })
    {
        bool rising = true;
        juce::String line;
        for (int note : { 59, 64, 76 })
        {
            double c[4] {};
            line << juce::MidiMessage::getMidiNoteName (note, true, true, 4) << ":";
            for (int a = 0; a < 4; ++a)
            {
                const auto r = renderFolded (pluckAt (note, amps[a]), folded (chord));
                c[a] = centroidHz (r.out, start + (size_t) (0.03 * kFs));
                line << " " << juce::String (c[a], 0);
            }
            rising = rising && c[1] >= c[0] - 1.0 && c[2] > c[0] && c[2] > c[1] && c[3] > c[2];
            line << " Hz   ";
        }
        check (rising, juce::String ("Folded brightness rises with the pick (centroid 30 .. 115 ms in at 0.08 / 0.2 / 0.5 / 0.9 peak), ") + (chord == 0 ? "Chord Off: " : "Harmonize C maj: ") + line.trimEnd());
    }

    // Brightness against the drive (a bare Tone, the 5 kHz filter included).
    {
        bool steady = true;
        juce::String line;
        for (int note : { 59, 64, 76 })
        {
            double prev = 0.0, worst = 0.0, worstAt = 0.0;
            line << juce::MidiMessage::getMidiNoteName (note, true, true, 4) << ":";
            for (double drive = 1.0; drive <= kFoldMaxDrive + 1e-9; drive += 0.05)
            {
                Tone t;
                t.prepare (kFs);
                VoiceSettings v;
                v.wave = Wave::Folded;
                t.setVoice (v);
                t.setFoldDrive (drive);
                std::vector<float> x ((size_t) (0.2 * kFs));
                for (auto& sm : x)
                    sm = t.render ((float) note, 0.1f);
                const double c = centroidHz (x, (size_t) (0.1 * kFs));
                if (prev > 0.0 && 1.0 - c / prev > worst)
                {
                    worst = 1.0 - c / prev;
                    worstAt = drive;
                }
                prev = c;
                if (std::abs (drive - std::round (drive)) < 1e-6)
                    line << juce::String::formatted (" %.0f: %.0f", drive, c);
            }
            steady = steady && worst <= 0.01;
            line << juce::String::formatted (" Hz (largest fall %.2f%% at %.2f)   ", 100.0 * worst, worstAt);
        }
        check (steady, "Folded brightness rises steadily with the drive (a bare Tone, drives 1 .. " + juce::String (kFoldMaxDrive, 0) + " in steps of 0.05, never down by more than 1%): " + line.trimEnd());
    }

    // Through the decay.
    {
        const auto in = pluckAt (64, 0.9f, 1.6, 3.0);
        auto p0 = folded (), p100 = folded ();
        p0.sustainPercent = 0.0f;
        p100.sustainPercent = 100.0f;
        const auto a = renderFolded (in, p0), b = renderFolded (in, p100);
        steps (a.drive);
        steps (b.drive);
        double first = 0.0, last = 0.0, prev = 1e9, worstRise = 0.0;
        juce::String line;
        for (double t = 0.03; t <= 0.5001; t += 0.025)
        {
            const double c = centroidHz (a.out, start + (size_t) (t * kFs));
            if (first == 0.0)
                first = c;
            worstRise = std::max (worstRise, c / prev - 1.0);
            prev = last = c;
            if (std::fmod (t + 1e-9, 0.1) < 0.025)
                line << juce::String::formatted ("%.2f s %.0f Hz (drive %.2f)  ", t, c, a.drive[start + (size_t) (t * kFs)]);
        }
        double lo = 1e9, hi = 0.0;
        for (double t : { 0.1, 0.4, 0.7, 1.0, 1.2 })
        {
            const double c = centroidHz (b.out, start + (size_t) (t * kFs));
            lo = std::min (lo, c);
            hi = std::max (hi, c);
        }
        check (worstRise < 0.01 && last < 0.5 * first,
               juce::String::formatted ("Folded at Sustain 0, hard E4: the centroid falls as the note decays (every 25 ms 0.03 .. 0.5 s, never up by more than %.1f%%): ", 100.0 * worstRise) + line.trimEnd());
        check (hi / lo < 1.03, juce::String::formatted ("Folded at Sustain 100%%, hard E4: the centroid holds, %.0f .. %.0f Hz from 0.1 to 1.2 s (drive %.2f)", lo, hi, b.drive[start + (size_t) (0.6 * kFs)]));
    }

    // No zipper noise or clicks as the drive moves.
    {
        const double f0 = hzFromMidi (64);
        std::vector<float> in ((size_t) (8.0 * kFs));
        for (size_t k = 0; k < in.size(); ++k)
        {
            const double t = (double) k / kFs;
            in[k] = (float) ((0.15 + 0.75 * (0.5 - 0.5 * std::cos (2.0 * M_PI * 0.25 * t))) * std::sin (2.0 * M_PI * f0 * t));
        }
        auto p = folded();
        p.sustainPercent = 0.0f;
        auto ps = p;
        ps.wave = Wave::Saw;
        const auto r = renderFolded (in, p), s = renderFolded (in, ps);
        steps (r.drive);
        const auto lo = *std::min_element (r.drive.begin() + (long) (0.5 * kFs), r.drive.end()), hi = *std::max_element (r.drive.begin() + (long) (0.5 * kFs), r.drive.end());
        const double off = offHarmonicDb (r.out, (size_t) (0.5 * kFs), f0, 60.0), offSaw = offHarmonicDb (s.out, (size_t) (0.5 * kFs), f0, 60.0);
        check (off < -60.0, juce::String::formatted ("Folded, the drive swept %.2f .. %.2f and back every 4 s (E4 sine swelling, Sustain 0): power more than 60 Hz from every harmonic %.1f dB re. the total (Saw %.1f)", lo, hi, off, offSaw));
    }
    check (largestStep < 0.05f, juce::String::formatted ("Folded: the drive moves every sample, by at most %.4f in any one in these runs (smoothed over %.0f ms; no steps)", largestStep, kFoldDriveSmoothMs));

    // The real takes (Recordings/, dry DI): where the drive goes on a real guitar, at the defaults.
    for (const auto& [name, take] : sputter::loadRecordings())
    {
        const auto r = renderFolded (take, folded());
        std::vector<float> open;
        for (size_t k = 0; k < r.drive.size(); ++k)
            if (r.open[k])
                open.push_back (r.drive[k]);
        if (open.empty())
            continue;
        std::sort (open.begin(), open.end());
        auto at = [&open] (double q) { return open[(size_t) (q * (double) (open.size() - 1))]; };
        info (juce::String::formatted ("Folded on the real take %s: the drive while the voice is open: median %.2f, 90%% %.2f, largest %.2f", name.toRawUTF8(), at (0.5), at (0.9), open.back()));
    }

    // The bare folder's loudness against its drive.
    {
        juce::String line;
        for (double drive : { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 })
        {
            Tone t;
            t.prepare (kFs);
            VoiceSettings v;
            v.wave = Wave::Folded;
            t.setVoice (v);
            t.setFoldDrive (drive);
            std::vector<float> x ((size_t) kFs);
            for (auto& sm : x)
                sm = t.render (64.0f, 0.1f);
            line << juce::String::formatted ("%.1f: %+.2f  ", drive, lufs (x, 4800) - lufs (toneAt (Wave::Saw, 64), 4800));
        }
        info ("Folded, a bare Tone at E4 against its drive (LU re. Saw at the same level): " + line.trimEnd());
    }

    // Aliases at a hard pick.
    struct Case
    {
        double f0, fs;
    };
    bool aliasOk = true;
    for (float drive : { hardDrive, kFoldMaxDrive })
        for (const Case c : { Case { 1000.0, 44100.0 }, Case { 1046.5, 48000.0 }, Case { 1108.7, 48000.0 }, Case { 1400.0, 48000.0 } })
        {
            FoldShape shape;
            shape.set (drive);
            FoldOversampler os;
            auto twice = [&] (double p, double dt) { return (double) os.next (shape, p, dt); };
            auto once = [&] (double p, double dt) { return foldedSample (shape, p, dt); };
            auto naive = [&] (double p, double) { return foldStages (shape.drive, std::sin (2.0 * M_PI * p)); };
            auto power = [&] (const std::function<double (double, double)>& osc) {
                std::vector<float> y ((size_t) (1 << 17) + 4096);
                double ph = 0.0;
                const double dt = c.f0 / c.fs;
                for (auto& v : y)
                {
                    v = (float) osc (ph, dt);
                    ph += dt;
                    if (ph >= 1.0)
                        ph -= 1.0;
                }
                return std::pair<double, double> { offHarmonicDb (y, 4096, c.f0, 3.0, c.fs), offHarmonicDb (y, 4096, c.f0, 3.0, c.fs, 10000.0) };
            };
            const auto a = aliasReport (twice, c.f0, c.fs);
            const auto pa = power (twice), pb = power (once), pn = power (naive);
            if (c.f0 < 1200.0)
                aliasOk = aliasOk && pa.second < -60.0;
            info (juce::String::formatted ("Folded drive %.2f, %6.1f Hz @ %4.1f kHz: alias power %.1f dB re. the signal, below 10 kHz %.1f (without the doubled rate %.1f / %.1f; naive %.1f / %.1f); worst component %.1f dB re. the fundamental, below 10 kHz %.1f",
                                           drive, c.f0, c.fs / 1000.0, pa.first, pa.second, pb.first, pb.second, pn.first, pn.second, a.worstDb, std::max (a.bandDb[0], a.bandDb[1])));
        }
    check (aliasOk, "Folded at a hard pick's drive and at the most: alias power below 10 kHz under -60 dB re. the signal up to C#6");

    // The bound.
    {
        double stages = 0.0;
        const double top = kFoldMaxDrive + 2.0;
        for (double drive = 1.0; drive <= top; drive += 0.001)
            for (int k = 0; k <= 4000; ++k)
                stages = std::max (stages, std::abs (foldStages (drive, k / 4000.0)));
        check (stages <= 1.0 + 1e-12, juce::String::formatted ("Folded: the three stages' sum peaks at %.6f (drives 1 .. %.0f, every phase), within +/-1 before its clamp", stages, top));
        double worst = 0.0;
        for (double drive : { 1.0, 1.01, 1.5, 2.9, 3.0, 3.01, 4.4, 5.0, 5.5, 6.0 })
            for (double hz : { 98.0, 440.0, 1108.7, 1400.0 })
                for (double fs : { 44100.0, 96000.0 })
                {
                    FoldShape shape;
                    shape.set (drive);
                    FoldOversampler os;
                    double ph = 0.0;
                    const double dt = hz / fs;
                    for (int k = 0; k < 20000; ++k)
                    {
                        worst = std::max (worst, (double) std::abs (os.next (shape, ph, dt)));
                        ph += dt;
                        if (ph >= 1.0)
                            ph -= 1.0;
                    }
                }
        check (worst <= 1.0, juce::String::formatted ("Folded: the folder's output peaks at %.4f (drives 1 .. 6, 98 .. 1400 Hz, 44.1 / 96 kHz; the binomial filter included), within +/-1 as Tone::peak assumes", worst));
    }

    // Loudness, against Saw on single plucks at the defaults (the window at its minimum, for G3).
    double meanFolded[2] {}, meanSaw[2] {};
    for (float amp : { 0.5f, 0.9f })
    {
        double lo = 1e9, hi = -1e9, sum = 0.0;
        int n = 0;
        for (int note = 55; note <= 76; ++note)
        {
            const auto in = pluckAt (note, amp, 0.8, 3.0);
            auto pf = folded();
            pf.windowHz = (float) derivedWindowMinHz();
            auto ps = pf;
            ps.wave = Wave::Saw;
            const size_t from = (size_t) (0.05 * kFs) + (size_t) latencyFor (kFs, derivedWindowMinHz());
            const double saw = lufs (renderFolded (in, ps).out, from), fold = lufs (renderFolded (in, pf).out, from);
            const double d = fold - saw;
            meanFolded[amp < 0.7f ? 0 : 1] += fold / 22.0;
            meanSaw[amp < 0.7f ? 0 : 1] += saw / 22.0;
            lo = std::min (lo, d);
            hi = std::max (hi, d);
            sum += d;
            ++n;
        }
        const auto text = juce::String::formatted ("Folded, %s single plucks (%.1f peak) G3 .. E5 at the defaults: %+.2f .. %+.2f LU re. Saw, mean %+.2f (gain %+.2f dB on the sine's mean-|x| scale)",
                                                   amp < 0.7f ? "medium" : "hard", amp, lo, hi, sum / n, kFoldedLoudnessDb);
        if (amp < 0.7f)
            check (lo > -1.0 && hi < 1.0, text);
        else
            info (text);
    }
    info (juce::String::formatted ("Folded, a hard pick (0.9) against a medium one (0.5), the same plucks: %+.2f LU louder (Saw %+.2f; Dynamics 50%% halves the 5.1 dB between them)",
                                   meanFolded[1] - meanFolded[0], meanSaw[1] - meanSaw[0]));

    // CPU.
    {
        auto toneNs = [] (Wave w, double drive) {
            Tone t;
            t.prepare (48000.0);
            VoiceSettings v;
            v.wave = w;
            t.setVoice (v);
            t.setFoldDrive (drive);
            const int n = 4800000;
            float acc = 0.0f;
            const auto t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < n; ++k)
                acc += t.render (64.0f + 0.000001f * (float) (k % 7), 0.1f);
            const double ns = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / n * 1e9;
            return ns + 0.0 * acc;
        };
        info (juce::String::formatted ("Folded CPU, one Tone at 48 kHz: Saw %.1f ns / sample, Folded at drive %.2f %.1f ns / sample", toneNs (Wave::Saw, 1.0), hardDrive, toneNs (Wave::Folded, hardDrive)));
        std::vector<float> in;
        for (int k = 0; k < 4; ++k)
        {
            const double base = 196.0 * std::pow (2.0, (k % 3) * 2.0 / 12.0);
            auto hz = [base] (double t) { return base * std::pow (2.0, 5.0 / 12.0 * std::sin (M_PI * std::min (t, 3.0) / 3.0)); };
            auto pl = ksPluck (hz, 4.0, 0.9f, ksLossFor (base, 12.0), (uint32_t) (50 + k));
            in.insert (in.end(), pl.begin(), pl.end());
        }
        for (int chord : { 0, harmonizeChoice (0), freezeChoice (3) })
        {
            double pct[2] {};
            for (int w = 0; w < 2; ++w)
            {
                EngineParams p = chordChoiceParams (chord);
                p.wave = w == 0 ? Wave::Saw : Wave::Folded;
                double best = 1e30;
                for (int rep = 0; rep < 3; ++rep)
                {
                    Engine e;
                    e.prepare (kFs, 64);
                    std::vector<float> out (in.size());
                    const auto t0 = std::chrono::steady_clock::now();
                    for (size_t s = 0; s + 64 <= in.size(); s += 64)
                        e.process (in.data() + s, out.data() + s, 64, p);
                    best = std::min (best, std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
                }
                pct[w] = 100.0 * best / ((double) in.size() / kFs);
            }
            info (juce::String::formatted ("Folded CPU, the engine at 48 kHz, 64-sample blocks, hard slide plucks, %s: Saw %.2f%%, Folded %.2f%% of one core",
                                           chord == 0 ? "Chord Off" : (chord == freezeChoice (3) ? "Freeze 3" : "Harmonize C maj"), pct[0], pct[1]));
        }
    }

    // The comparison WAVs: one phrase (B3 E4 G4 B4 E5, 0.7 s each) picked soft (0.08), then medium (0.5), then hard (0.9), then
    // alternating soft / hard, on Triangle and Folded, at Sustain 0 and 75%.
    {
        outDir.getChildFile ("folded").createDirectory();
        std::vector<float> phrase;
        const int notes[5] = { 59, 64, 67, 71, 76 };
        auto add = [&] (int note, float amp) {
            const double hz = hzFromMidi (note);
            auto x = ksPluck ([hz] (double) { return hz; }, 0.7, amp, ksLossFor (hz, 3.0), (uint32_t) (90 + note));
            phrase.insert (phrase.end(), x.begin(), x.end());
        };
        for (float amp : { 0.08f, 0.5f, 0.9f })
            for (int note : notes)
                add (note, amp);
        for (int k = 0; k < 10; ++k)
            add (notes[k % 5], k % 2 == 0 ? 0.08f : 0.9f);
        phrase.insert (phrase.end(), (size_t) (1.0 * kFs), 0.0f);
        writeWav ("folded/phrase-dry", phrase);
        for (float sus : { 0.0f, 75.0f })
        {
            const auto tag = juce::String ("-sustain") + juce::String ((int) sus);
            auto p = folded();
            p.sustainPercent = sus;
            auto tri = p;
            tri.wave = Wave::Triangle;
            writeWav ("folded/1-triangle" + tag, renderFolded (phrase, tri).out);
            writeWav ("folded/2-folded" + tag, renderFolded (phrase, p).out);
        }
        info ("Folded comparison WAVs in " + outDir.getChildFile ("folded").getFullPathName() + ": phrase-dry, 1-triangle, 2-folded (-sustain0, -sustain75): "
              "B3 E4 G4 B4 E5 soft, medium, hard, then alternating soft / hard");
    }
}
// The headroom table (seven plucks B3 .. E5 at the defaults), Stack in a key (C maj / A min) and Memory with Memory Notes.
// Per wave: how often the safety clip still works and what it takes off, and the headroom gain's deepest reduction.
// Targets: no clipping in any row for any wave. And no overshoot: on hard plucks with the stage engaged, no sample past the ceiling by more than 0.1 dB.
void testHeadroom()
{
    static const char* const names[4] = { "Saw", "Square", "Triangle", "Sine" };
    const auto hard = zonePlucks (0.9f), medium = zonePlucks (0.5f);
    struct Row
    {
        const char* name;
        const std::vector<float>* in;
        int chord; // the Chord selector's index
        int memoryNotes;
    };
    const Row rows[] = {
        { "Medium plucks (0.5 peak), Chord Off", &medium, 0, 3 },
        { "Hard plucks (0.9 peak), Chord Off  ", &hard, 0, 3 },
        { "Medium plucks, C maj / A min       ", &medium, harmonizeChoice (0), 3 },
        { "Hard plucks, C maj / A min         ", &hard, harmonizeChoice (0), 3 },
        { "Medium plucks, Memory 2            ", &medium, freezeChoice (4), 2 },
        { "Medium plucks, Memory 3            ", &medium, freezeChoice (4), 3 },
        { "Medium plucks, Memory 4            ", &medium, freezeChoice (4), 4 },
        { "Hard plucks, Memory 4              ", &hard, freezeChoice (4), 4 },
    };
    info ("Per wave: clip (samples / energy off), deepest headroom gain (\"-\": the clip never acts):");
    auto text = [] (const ClipStats& c) { return c.share > 0.0 ? juce::String::formatted ("%.2f%% / %.0f dB", c.share, c.removedDb) : juce::String ("-"); };
    bool clean = true, overshootOk = true;
    double worstOver = -200.0;
    for (const auto& r : rows)
    {
        juce::String line;
        for (int w = 0; w < 4; ++w)
        {
            EngineParams p;
            setChordChoice (p, r.chord);
            p.memoryNotes = r.memoryNotes;
            p.wave = (Wave) w;
            float deepest = 0.0f;
            const auto now = renderV053 (*r.in, p, kFs, &deepest, nullptr, false);
            const auto a = clipStats (now);
            line << names[w] << " " << text (a) << juce::String::formatted (" GR %.1f   ", deepest);
            clean = clean && a.share == 0.0;
            if (r.in == &hard && deepest < 0.0f)
            {
                float peak = 0.0f;
                for (float v : now)
                    peak = std::max (peak, std::abs (v));
                worstOver = std::max (worstOver, toDb (peak / kHeadroomCeiling));
                overshootOk = overshootOk && peak <= kHeadroomCeiling * dbToGain (0.1f);
            }
        }
        info ("  " + juce::String (r.name) + "  " + line.trimEnd());
    }

    check (clean, "The safety clip never acts, in any row, for any wave");
    check (overshootOk, juce::String::formatted ("Hard plucks with the stage engaged: the highest sample is %+.3f dB re. the ceiling (%.4f, %.2f dB under the clip's knee; at most +0.1 dB)", worstOver,
                                                 kHeadroomCeiling, toDb (kHeadroomCeiling / kSafetyClipKnee)));
}

// The headroom gain in motion. No pumping: a held Stack chord on Sine at Sustain 100% (a steady E4) keeps its
// gain constant to 0.1 dB after the first 200 ms. No clicks with the stage engaged (sine voice):
// Memory's tones entering and leaving, the chord mode switched mid-note.
void testHeadroomMotion()
{
    {
        const auto in = legatoSaws ({ { 64, 3.0 } });
        EngineParams p;
        p.wave = Wave::Sine;
        p.chordMode = ChordMode::Stack;
        p.sustainPercent = 100.0f;
        Engine e;
        e.prepare (kFs, 64);
        const double lat = (double) e.currentLatencySamples() / kFs;
        double lo = 1e9, hi = -1e9;
        std::vector<float> out (1);
        for (size_t n = 0; n < in.size(); ++n)
        {
            e.process (in.data() + n, out.data(), 1, p);
            if ((double) n / kFs > lat + 0.2)
            {
                const double g = toDb (e.headroomGain());
                lo = std::min (lo, g);
                hi = std::max (hi, g);
            }
        }
        check (hi < -1.0 && hi - lo <= 0.1,
               juce::String::formatted ("Held Stack chord, Sine, Sustain 100%%: headroom gain %.2f .. %.2f dB from 200 ms to 3 s (spread %.3f dB)", lo, hi, hi - lo));
    }

    auto stepped = [] (const std::vector<float>& in, const std::function<EngineParams (double)>& at, float* deepest) {
        Engine e;
        e.prepare (kFs, 64);
        std::vector<float> out (in.size());
        *deepest = 0.0f;
        for (size_t k = 0; k < in.size(); k += 64)
        {
            e.process (in.data() + k, out.data() + k, (int) std::min<size_t> (64, in.size() - k), at ((double) k / kFs));
            *deepest = std::min (*deepest, e.readout().gainReductionDb.load());
        }
        return out;
    };
    auto sine = [] (EngineParams p) {
        p.wave = Wave::Sine;
        return p;
    };
    {
        float deepest = 0.0f;
        const auto out = stepped (legatoSaws ({ { 60, 0.3 }, { 64, 0.3 }, { 67, 0.3 }, { 71, 0.3 }, { 64, 0.3 }, { 65, 0.05 }, { 69, 0.6 }, { 67, 1.0 } }), [&] (double) {
            EngineParams p;
            p.chordMode = ChordMode::Memory;
            return sine (p);
        }, &deepest);
        writeWav ("headroom_memory_sine", out);
        const float r = chordStepRatio (out, hzFromMidi (71), 0.05);
        check (deepest < -1.0f && r < 1.0f, juce::String::formatted ("Memory (C E G B E F A G), Sine: tones entering and leaving with the gain down to %.1f dB: largest step %.2fx the bound", deepest, r));
    }
    {
        float deepest = 0.0f;
        const auto out = stepped (legatoSaws ({ { 64, 3.0 } }), [&] (double t) {
            EngineParams p;
            p.chordMode = (ChordMode) ((int) (t / 0.37) % 3);
            return sine (p);
        }, &deepest);
        writeWav ("headroom_chord_switch_sine", out);
        const float r = chordStepRatio (out, hzFromMidi (64 + 16), 0.05);
        check (deepest < -1.0f && r < 1.0f, juce::String::formatted ("Chord Off / Stack / Memory switched every 0.37 s mid-note, Sine, gain down to %.1f dB: largest step %.2fx the bound", deepest, r));
    }
}

// v0.5.3 headroom (probe): the predicted peak (Tone::peak, the bound the headroom gain works from) against the
// actual output, per wave and chord: on hard plucks, and how near held notes come to it over time.
void probeHeadroom()
{
    static const char* const names[4] = { "Saw", "Square", "Triangle", "Sine" };
    std::vector<float> hard;
    uint32_t seed = 120;
    for (int note : { 59, 62, 64, 67, 69, 72, 76 })
    {
        const double hz = hzFromMidi (note);
        const auto pl = ksPluck ([hz] (double) { return hz; }, 0.8, 0.9f, ksLossFor (hz, 3.0), seed++);
        hard.insert (hard.end(), pl.begin(), pl.end());
        hard.insert (hard.end(), (size_t) (0.03 * kFs), 0.0f);
    }
    for (int chord : { 0, harmonizeChoice (0), freezeChoice (4) }) // Chord Off, C maj / A min, Memory (4 notes)
    {
        juce::String line;
        for (int w = 0; w < 4; ++w)
        {
            EngineParams p;
            p.wave = (Wave) w;
            setChordChoice (p, chord);
            p.memoryNotes = kMemoryNotesMax;
            Engine e;
            e.prepare (kFs, 64);
            e.setHeadroomGain (false); // the plain output (the clip may still act above its knee)
            double maxPred = 0.0, maxOut = 0.0, ratioMax = 0.0;
            std::vector<float> out (1);
            for (size_t n = 0; n < hard.size(); ++n)
            {
                e.process (hard.data() + n, out.data(), 1, p);
                const double pred = e.lastPredictedPeak();
                maxPred = std::max (maxPred, pred);
                maxOut = std::max (maxOut, (double) std::abs (out[0]));
                if (pred > 1e-4)
                    ratioMax = std::max (ratioMax, std::abs (out[0]) / pred);
            }
            line << names[w] << juce::String::formatted (" pred %.2f out %.2f (max out/pred %.2f)   ", maxPred, maxOut, ratioMax);
        }
        info (juce::String::formatted ("Chord choice %d, hard plucks, headroom gain off: ", chord) + line.trimEnd());
    }

    // Held notes (Sustain 100%, a steady saw input, 6 s): how close the output gets to the bound over time.
    for (int chord : { 0, harmonizeChoice (0) })
    {
        juce::String line;
        for (int w = 0; w < 4; w += 3)
            for (int note : { 64, 69 })
            {
                const auto in = legatoSaws ({ { note, 6.0 } });
                EngineParams p;
                p.wave = (Wave) w;
                setChordChoice (p, chord);
                p.sustainPercent = 100.0f;
                Engine e;
                e.prepare (kFs, 64);
                e.setHeadroomGain (false);
                double ratio = 0.0, firstHalf = 0.0;
                std::vector<float> out (1);
                for (size_t n = 0; n < in.size(); ++n)
                {
                    e.process (in.data() + n, out.data(), 1, p);
                    if (n > (size_t) (0.5 * kFs) && e.lastPredictedPeak() > 1e-4f)
                    {
                        ratio = std::max (ratio, std::abs (out[0]) / (double) e.lastPredictedPeak());
                        if (n < (size_t) (1.5 * kFs))
                            firstHalf = ratio;
                    }
                }
                line << names[w] << " " << note << juce::String::formatted (" %.2f/%.2f  ", firstHalf, ratio);
            }
        info (juce::String::formatted ("Held, Chord choice %d: max |out| / predicted, first 1 s / 5.5 s: ", chord) + line.trimEnd());
    }
}

// The waves' loudness re. Saw, note by note (what v0.5.1's per-wave gains in Oscillator.h were set from; at the current
// levels, so Triangle and Sine read about +10 LU: v0.5.2's gain by ear).
void probeWaveLoudness()
{
    static const char* const names[4] = { "Saw", "Square", "Triangle", "Sine" };
    for (Octave o : { Octave::Main, Octave::Sub, Octave::DoubleSub })
    {
        info (juce::String::formatted ("Octave %d: LUFS re. Saw per note (G3 .. E5), then mean / min / max", (int) o));
        for (int w = 1; w < 4; ++w)
        {
            juce::String line;
            double sum = 0.0, lo = 1e9, hi = -1e9;
            int n = 0;
            for (int note = 55; note <= 76; ++note, ++n)
            {
                const double d = lufs (toneAt ((Wave) w, note, o), 4800) - lufs (toneAt (Wave::Saw, note, o), 4800);
                sum += d;
                lo = std::min (lo, d);
                hi = std::max (hi, d);
                if (note % 3 == 1)
                    line << juce::String (d, 1) << " ";
            }
            info (juce::String::formatted ("  %-8s mean %+.4f  min %+.2f  max %+.2f  | ", names[w], sum / n, lo, hi) + line);
        }
    }
    info (juce::String::formatted ("Saw E4 at level 0.1: %.1f LUFS", lufs (toneAt (Wave::Saw, 64), 4800)));
}

// ---------------------------------------------------------------------------
// v0.5.4: Simplify (Thickness, Low Pass Filter, Triad and Key removed; one Chord selector), Memory Notes, the
// fixed memory clear, HELD, the Dynamics fix, the low-range display detector
// ---------------------------------------------------------------------------

// Legato notes and plucks whose pitch gaps are all short (the picks' own dropouts and 60 ms of silence), within
// the default window's range: Memory builds, drops and replays notes on it.
std::vector<float> memoryPhrase (double fs)
{
    std::vector<float> x;
    auto add = [&] (std::vector<float> v) { x.insert (x.end(), v.begin(), v.end()); };
    auto gap = [&] (double s) { x.insert (x.end(), (size_t) (s * fs), 0.0f); };
    add (legatoSaws ({ { 60, 0.3 }, { 64, 0.3 }, { 67, 0.3 }, { 71, 0.3 }, { 64, 0.3 }, { 65, 0.05 }, { 69, 0.5 } }, 0.4f, fs));
    gap (0.06);
    uint32_t seed = 140;
    for (int note : { 62, 66, 69, 74, 62 })
    {
        const double hz = hzFromMidi (note);
        add (ksPluck ([hz] (double) { return hz; }, 0.45, 0.5f, ksLossFor (hz, 3.0), seed++, fs));
    }
    gap (0.06);
    add (legatoSaws ({ { 67, 0.4 }, { 71, 0.6 } }, 0.3f, fs));
    return x;
}

// The Chord selector (v0.5.6): Off, Freeze 2 / 3 / 4 (Memory with that many notes), then Harmonize (Stack) in each key;
// and through the engine, each option plays what v0.5.5 played at its mode, key and Memory Notes (the memory phrase,
// whose pitch gaps are all under 150 ms, at the defaults).
void testChordSelector()
{
    bool ok = kChordChoices == 16 && chordChoiceFor (0).mode == ChordMode::Off && chordChoiceFor (0).key == kKeyOff;
    juce::String line = "0 Off";
    for (int k = 1; k < kChordChoices; ++k)
    {
        const auto c = chordChoiceFor (k);
        if (k <= 3)
        {
            ok = ok && c.mode == ChordMode::Memory && c.key == kKeyOff && c.memoryNotes == k + 1;
            line << ", " << k << " Freeze " << c.memoryNotes;
        }
        else
        {
            ok = ok && c.mode == ChordMode::Stack && c.key == k - 4;
            line << ", " << k << " Harmonize " << keyLabel (c.key);
        }
    }
    const EngineParams defaults;
    check (ok && defaults.chordMode == chordChoiceFor (0).mode && defaults.key == chordChoiceFor (0).key,
           "Chord index -> mode, note count and key (and the engine's default is index 0, Off): " + line);
}

// Memory at the plugin's defaults with Memory Notes n: a sine voice (one partial per tone, so each can be measured
// on its own).
EngineParams memoryParams (int notes = kDefaultMemoryNotes)
{
    EngineParams p = chordChoiceParams (freezeChoice (3));
    p.memoryNotes = notes;
    p.wave = Wave::Sine;
    return p;
}

// Memory Notes (v0.5.4): at 2 / 3 / 4, five notes played legato (0.3 s each, past the 80 ms commit) leave that many
// tones sounding, the most recent ones; each new note drops the oldest once it commits (until then one tone more
// sounds, never two more); and the sound (FFT over the last note): the held notes at Chord Level, the dropped ones gone.
void testMemoryNotes()
{
    const int played[5] = { 60, 64, 67, 71, 74 };
    const auto in = legatoSaws ({ { 60, 0.3 }, { 64, 0.3 }, { 67, 0.3 }, { 71, 0.3 }, { 74, 0.6 } });
    for (int n = kMemoryNotesMin; n <= kMemoryNotesMax; ++n)
    {
        const auto r = runChord (in, [n] (double) { return memoryParams (n); });
        writeWav (juce::String ("memory_notes_") + juce::String (n), r.out);
        bool ok = true;
        juce::String line;
        for (int k = 0; k < 5; ++k)
        {
            std::vector<int> want { played[k] };
            for (int j = std::max (0, k - n + 1); j < k; ++j)
                want.push_back (played[j]);
            if (want.size() > 2)
                std::sort (want.begin() + 1, want.end());
            const auto got = r.at (0.3 * k + 0.28);
            ok = ok && got == want;
            line << tonesText (got) << (k < 4 ? " / " : "");
        }
        size_t most = 0;
        for (auto t : r.tones)
            most = std::max (most, unpackTones (t).size());
        const Spectrum spec (r.out, (size_t) (1.35 * kFs), (size_t) (0.3 * kFs));
        const double live = spec.near (hzFromMidi (74)).amp;
        juce::String sound;
        bool soundOk = true;
        for (int k = 0; k < 4; ++k)
        {
            const double db = toDb (spec.near (hzFromMidi (played[k])).amp / live);
            const bool held = k >= 4 - (n - 1);
            soundOk = soundOk && (held ? std::abs (db - toDb (kChordLevel)) < 0.5 : db < -60.0);
            sound << noteText (played[k]) << " " << juce::String (db, 1) << " ";
        }
        check (ok && (int) most == n + 1 && soundOk,
               juce::String::formatted ("Memory Notes %d, C4 E4 G4 B4 D5 legato: tones after each note ", n) + line + juce::String::formatted ("; most at once %d (the brief extra before a commit)", (int) most)
                   + "; over D5 (dB re. D5): " + sound.trimEnd());
    }

    // Memory Notes changed while playing (4 -> 2): the oldest go at once (20 ms fade), the newest stay; no click.
    {
        const auto r = runChord (in, [] (double t) { return memoryParams (t < 1.4 ? 4 : 2); });
        const auto got = r.at (1.6);
        check (got == std::vector<int> { 74, 71 } && chordStepRatio (r.out, hzFromMidi (74), 0.05) < 1.0f,
               "Memory Notes 4 -> 2 over D5: " + tonesText (got) + juce::String::formatted (" (want D5* B4); largest step %.2fx the bound", chordStepRatio (r.out, hzFromMidi (74), 0.05)));
    }
}

// The memory clear (v0.5.4; 150 ms since v0.5.6): kMemoryClearMs without pitch, whatever Note Release; the sound is still
// Note Release's.
//  - At every option: a note 100 ms after the chord stops rejoins it; one 330 ms after starts fresh.
//  - Medium and Long (the voice still sounding at the clear; Sustain 100%, so its level holds through the release):
//    the chord rings on past the clear, then the next note (420 ms after the stop) starts a fresh chord: the old
//    tones fade out (20 ms) under it, with no click.
//  - Instant (the voice closed 35 ms after the stop: its 20 ms grace and the 15 ms fade): a note 100 ms later rejoins
//    the held chord, its remembered tones fading back in, with no click.
void testMemoryClear()
{
    const auto chord = legatoSaws ({ { 60, 0.3 }, { 64, 0.3 }, { 67, 0.3 } });
    const auto withGap = [&chord] (double gap) { return joined ({ chord, silence (gap), legatoSaws ({ { 69, 0.5 } }) }); };
    static const char* const names[5] = { "Instant", "Fast", "Medium", "Long", "Extra Long" };
    for (int r = 0; r < 5; ++r)
    {
        juce::String line;
        bool ok = true;
        for (double gap : { 0.10, 0.33 })
        {
            const auto run = runChord (withGap (gap), [r] (double) {
                auto p = memoryParams();
                p.holdMs = noteReleaseMsFor ((NoteReleaseV058) r);
                return p;
            });
            const auto got = run.at (0.9 + gap + 0.3);
            const auto want = gap < 0.001 * kMemoryClearMs ? std::vector<int> { 69, 64, 67 } : std::vector<int> { 69 };
            ok = ok && got == want;
            line << juce::String::formatted ("%.0f ms gap: ", 1000.0 * gap) << tonesText (got) << "  ";
        }
        check (ok, juce::String::formatted ("Note Release %-10s: A4 after the C4 E4 G4 chord, ", names[r]) + line.trimEnd()
                       + juce::String::formatted (" (rejoins under %.0f ms; fresh after)", kMemoryClearMs));
    }

    for (NoteReleaseV058 nr : { NoteReleaseV058::Medium, NoteReleaseV058::Long })
    {
        const double gap = 0.42;
        const auto run = runChord (withGap (gap), [nr] (double) {
            auto p = memoryParams();
            p.holdMs = noteReleaseMsFor (nr);
            p.sustainPercent = 100.0f;
            return p;
        });
        writeWav (juce::String ("memory_clear_") + names[(int) nr], run.out);
        const auto ringing = run.at (0.9 + 0.33);
        const Spectrum rings (run.out, (size_t) ((0.9 + 0.28) * kFs), (size_t) (0.1 * kFs));
        const double g = rings.near (hzFromMidi (67)).amp;
        const double c = toDb (rings.near (hzFromMidi (60)).amp / g), e = toDb (rings.near (hzFromMidi (64)).amp / g);
        const auto fresh = run.at (0.9 + gap + 0.1);
        const Spectrum after (run.out, (size_t) ((0.9 + gap + 0.08) * kFs), (size_t) (0.3 * kFs));
        const double a = after.near (hzFromMidi (69)).amp;
        double gone = -200.0;
        for (int n : { 60, 64, 67 })
            gone = std::max (gone, toDb (after.near (hzFromMidi (n)).amp / a));
        const float step = chordStepRatio (run.out, hzFromMidi (69), 0.05);
        check (ringing == std::vector<int> { 67, 60, 64 } && std::abs (c - toDb (kChordLevel)) < 0.5 && std::abs (e - toDb (kChordLevel)) < 0.5 && fresh == std::vector<int> { 69 }
                   && gone < -60.0 && step < 1.0f,
               juce::String::formatted ("Note Release %s, Sustain 100%%: 330 ms after the stop (memory cleared) still sounding ", names[(int) nr]) + tonesText (ringing)
                   + juce::String::formatted (" (C4 %.2f, E4 %.2f dB re. G4); A4 at 420 ms: ", c, e) + tonesText (fresh)
                   + juce::String::formatted (", the old tones at most %.1f dB re. A4 from 80 ms on; largest step %.2fx the bound", gone, step));
    }

    {
        const double gap = 0.10;
        const auto run = runChord (withGap (gap), [] (double) {
            auto p = memoryParams();
            p.holdMs = noteReleaseMsFor (NoteReleaseV058::Instant);
            return p;
        });
        writeWav ("memory_clear_Instant_rejoin", run.out);
        double gapPeak = 0.0;
        for (size_t n = (size_t) ((0.9 + 0.05) * kFs); n < (size_t) ((0.9 + gap - 0.02) * kFs); ++n)
            gapPeak = std::max (gapPeak, (double) std::abs (run.out[n]));
        const auto rejoined = run.at (0.9 + gap + 0.3);
        const Spectrum spec (run.out, (size_t) ((0.9 + gap + 0.15) * kFs), (size_t) (0.3 * kFs));
        const double a = spec.near (hzFromMidi (69)).amp;
        const double e = toDb (spec.near (hzFromMidi (64)).amp / a), g = toDb (spec.near (hzFromMidi (67)).amp / a);
        const float step = chordStepRatio (run.out, hzFromMidi (69), 0.05);
        check (gapPeak == 0.0 && rejoined == std::vector<int> { 69, 64, 67 } && std::abs (e - toDb (kChordLevel)) < 0.5 && std::abs (g - toDb (kChordLevel)) < 0.5 && step < 1.0f,
               juce::String::formatted ("Note Release Instant: silent in the gap (peak %.1g: the voice closed); A4 100 ms after the stop rejoins: ", gapPeak) + tonesText (rejoined)
                   + juce::String::formatted (" (E4 %.2f, G4 %.2f dB re. A4); largest step %.2fx the bound", e, g, step));
    }
}

// The HELD readout (v0.5.4): Readout::memoryHeld counts the notes memory holds besides the one being played. Building
// C4 E4 G4 it reads 0, 1, 2 (the light off on the first note); after the stop, 3; at every Note Release option the
// memory clears exactly kMemoryClearMs (150 ms since v0.5.6) after the voice's last voiced sample (to the sample,
// VoiceProbe), and the readout (the HELD light) goes out in that same block, also while the chord still rings; and
// it's 0 throughout Chord Off and Stack, and at once when Memory is switched off.
void testHeld()
{
    const auto in = joined ({ legatoSaws ({ { 60, 0.3 }, { 64, 0.3 }, { 67, 0.3 } }), silence (0.6) });
    static const char* const names[5] = { "Instant", "Fast", "Medium", "Long", "Extra Long" };
    for (int r = 0; r < 5; ++r)
    {
        auto p = memoryParams();
        p.holdMs = noteReleaseMsFor ((NoteReleaseV058) r);
        Engine e;
        e.prepare (kFs, 64);
        int64_t n = 0, lastVoiced = -1, clearedAt = -1;
        int prevHeld = 0;
        e.voiceListener = [&] (const VoiceProbe& v) {
            if (v.voiced)
                lastVoiced = n;
            if (prevHeld > 0 && v.memoryHeld == 0 && n > (int64_t) (0.9 * kFs) && clearedAt < 0)
                clearedAt = n;
            prevHeld = v.memoryHeld;
            ++n;
        };
        std::vector<int> held;
        std::vector<float> out (in.size());
        for (size_t k = 0; k < in.size(); k += 64)
        {
            e.process (in.data() + k, out.data() + k, 64, p);
            held.push_back (e.readout().memoryHeld.load());
        }
        auto at = [&] (double t) { return held[(size_t) (t * kFs / 64.0)]; };
        size_t clearedBlock = held.size();
        for (size_t b = (size_t) (0.95 * kFs / 64.0); b < held.size(); ++b)
            if (held[b] == 0)
            {
                clearedBlock = b;
                break;
            }
        const int64_t clearLen = (int64_t) msToSamples (kFs, kMemoryClearMs);
        const bool sameBlock = clearedAt >= 0 && (size_t) (clearedAt / 64) == clearedBlock;
        const bool ok = at (0.25) == 0 && at (0.55) == 1 && at (0.85) == 2 && at (0.9 + 0.12) == 3 && clearedAt - lastVoiced == clearLen && sameBlock;
        check (ok, juce::String::formatted ("Note Release %-10s: held while playing C4 E4 G4: %d %d %d; 120 ms after the stop: %d; memory cleared %.2f ms after the last voiced "
                                            "sample (%.0f ms), the HELD readout out in the same block: %s",
                                            names[r], at (0.25), at (0.55), at (0.85), at (0.9 + 0.12), 1000.0 * (double) (clearedAt - lastVoiced) / kFs, kMemoryClearMs, sameBlock ? "yes" : "NO"));
    }

    // Outside Memory: never set; Memory switched to Off with notes held: 0 from the next block.
    {
        bool never = true;
        for (int choice : { 0, harmonizeChoice (0) })
        {
            const auto run = runChord (in, [choice] (double) {
                auto p = chordChoiceParams (choice);
                p.memoryNotes = 3;
                return p;
            });
            for (int h : run.held)
                never = never && h == 0;
        }
        const auto run = runChord (in, [] (double t) {
            auto p = memoryParams();
            if (t >= 0.8)
                setChordChoice (p, 0);
            return p;
        });
        const size_t switchBlock = (size_t) (0.8 * kFs / 64.0);
        check (never && run.held[switchBlock - 1] == 2 && run.held[switchBlock] == 0,
               juce::String::formatted ("Chord Off and C maj / A min: HELD never set (%s); Memory -> Off with 2 held: %d, then %d from the switch's block", never ? "yes" : "NO",
                                        run.held[switchBlock - 1], run.held[switchBlock]));
    }
}

// The low-range detector's readings over a signal, one per analysis: {input time of the newest sample, reading}.
std::vector<std::pair<double, LowRangeReading>> lowRangeReadings (const std::vector<float>& in, double fs = kFs)
{
    LowRangeDetector d;
    d.prepare (fs);
    std::vector<std::pair<double, LowRangeReading>> out;
    for (size_t k = 0; k + (size_t) d.hopSize() <= in.size(); k += (size_t) d.hopSize())
    {
        d.push (in.data() + k, d.hopSize());
        out.push_back ({ (double) (k + (size_t) d.hopSize()) / fs, d.reading() });
    }
    return out;
}

// Probe: single plucks C2 .. C6 and two-note intervals in the zone, what the low-range detector reports (0.15 .. 0.7 s).
// Probe: where the Dynamics fix changes the regression phrase (Note Release Instant, window minimum): 10 ms windows more
// than 0.5 dB from v0.5.3's (the reference frozen while pitch is lost). One, at 4.71 s: the soft G4 handing over to the
// loud saw, a few unvoiced hops that v0.5.3 played with the soft note's boost.
void probeDynamicsFixWhere()
{
    EngineParams p;
    p.holdMs = 0.0f;
    p.windowHz = (float) derivedWindowMinHz();
    const auto in = regressionPhrase (kFs);
    Engine a, b;
    a.prepare (kFs, 64);
    b.prepare (kFs, 64);
    b.setLegacyNoteReference (true);
    b.setSputterFix (false); // the sputter fix off, as v0.5.4
    const auto now = runEngine (a, in, p), old = runEngine (b, in, p);
    const size_t w = (size_t) (0.01 * kFs);
    for (size_t k = 0; k + w <= now.size(); k += w)
    {
        const double x = rms (now, k, k + w), y = rms (old, k, k + w);
        if (y > 1e-3 && std::abs (toDb (x / y)) > 0.5)
            info (juce::String::formatted ("%.2f s: %+.1f dB (v0.5.3 %.1f dBFS)", (double) k / kFs, toDb (x / y), toDb (y)));
    }
}

void probeLowRange()
{
    auto summary = [] (const std::vector<float>& in) {
        const auto rs = lowRangeReadings (in);
        int voiced = 0, periodic = 0, n = 0;
        std::map<int, int> notes;
        float minConf = 1.0f, fundLo = 0.0f, fundHi = -200.0f;
        for (const auto& [t, r] : rs)
            if (t > 0.15 && t < 0.7)
            {
                ++n;
                minConf = std::min (minConf, r.confidence);
                periodic += r.periodic ? 1 : 0;
                if (r.periodic)
                {
                    fundLo = std::min (fundLo, r.fundamentalDb);
                    fundHi = std::max (fundHi, r.fundamentalDb);
                }
                if (r.voiced)
                {
                    ++voiced;
                    ++notes[r.note];
                }
            }
        juce::String s = juce::String::formatted ("single %d / periodic %d of %d, min conf %.2f, fundamental %.1f .. %.1f dB, notes", voiced, periodic, n, minConf, fundLo, fundHi);
        for (auto [note, c] : notes)
            s << " " << noteText (note) << "x" << c;
        return s;
    };
    info (juce::String::formatted ("window %.2f Hz, L %d samples at 48 kHz", lowRangeWindowHz(), 2 * (int) std::lround (kFs / lowRangeWindowHz())));
    for (int note : { 36, 38, 40, 43, 45, 50, 55, 59, 64, 69, 76, 84 })
    {
        const double hz = hzFromMidi (note);
        info ("  pluck " + noteText (note) + ": " + summary (ksPluck ([hz] (double) { return hz; }, 0.8, 0.5f, ksLossFor (hz, 3.0), (uint32_t) note)));
    }
    const char* names[] = { "m2", "M2", "m3", "M3", "P4", "TT", "P5", "m6", "M6", "oct" };
    const int steps[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 12 };
    for (int root : { 59, 64, 69 })
        for (int k = 0; k < 10; ++k)
            info ("  " + noteText (root) + " + " + names[k] + ": " + summary (plucks ({ (double) root, (double) (root + steps[k]) }, 51 + (uint32_t) k)));
}

// The low-range display detector (v0.5.4), offline on the class (LowRange.h):
//  - its window's lowest reliable note is C2 at every common rate;
//  - single plucks from C2 (and its 25 c flat margin) up to C6 are named right in at least 95% of its readings from
//    60 ms to 0.7 s, at 44.1 / 48 / 96 kHz;
//  - silence and low noise read nothing;
//  - two notes at once in the zone (every interval to the octave) never read as a single note at their common period,
//    an octave or more below the lower one (the fundamental check);
//  - against the main detector (an engine at the default window) on separate plucks in the zone: wherever both are
//    voiced, after the first three readings of each note, the same note (no octave disagreement).
void testLowRange()
{
    bool reliable = true, highest = false;
    for (double fs : kCommonSampleRates)
    {
        reliable = reliable && lowestReliableNote (fs, lowRangeWindowHz()) <= kLowRangeLowestNote;
        highest = highest || lowestReliableNote (fs, lowRangeWindowHz() + 0.01) > kLowRangeLowestNote;
    }
    reliable = reliable && highest;
    check (reliable, juce::String::formatted ("Window %.2f Hz: lowest reliable C2 at every common rate (and the highest such window on the 0.01 Hz grid)", lowRangeWindowHz()));

    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        juce::String bad;
        int worst = 100;
        for (double note : { 35.75, 36.0, 37.0, 38.0, 39.0, 40.0, 43.0, 45.0, 48.0, 50.0, 52.0, 55.0, 59.0, 64.0, 69.0, 76.0, 81.0, 84.0 })
        {
            const double hz = hzFromMidi (note);
            const auto rs = lowRangeReadings (ksPluck ([hz] (double) { return hz; }, 0.8, 0.5f, ksLossFor (hz, 3.0), (uint32_t) (note * 4), fs), fs);
            int right = 0, n = 0;
            for (const auto& [t, r] : rs)
                if (t > 0.06 && t < 0.7)
                {
                    ++n;
                    right += r.voiced && r.note == (int) std::lround (note) ? 1 : 0;
                }
            const int pct = 100 * right / std::max (1, n);
            worst = std::min (worst, pct);
            if (pct < 95)
                bad << " " << noteText ((int) std::lround (note)) << (note != std::round (note) ? "-25c" : "") << " " << pct << "%";
        }
        check (bad.isEmpty(), juce::String::formatted ("@ %4.1f kHz: plucks C2 (and 25 c flat) .. C6 named right in at least %d%% of the readings from 60 ms", fs / 1000.0, worst) + bad);
    }

    {
        int voiced = 0;
        for (const auto& [t, r] : lowRangeReadings (silence (1.0)))
            voiced += r.voiced ? 1 : 0;
        int noisy = 0;
        for (const auto& [t, r] : lowRangeReadings (whiteNoise (1.0, 0.01f * 1.732f, 9)))
            noisy += r.voiced ? 1 : 0;
        check (voiced == 0 && noisy == 0, juce::String::formatted ("Silence: %d readings voiced; white noise at -40 dBFS: %d", voiced, noisy));
    }

    {
        const char* names[] = { "m2", "M2", "m3", "M3", "P4", "TT", "P5", "m6", "M6", "oct" };
        const int steps[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 12 };
        int falseLow = 0;
        juce::String where;
        for (int root : { 59, 62, 64, 67, 69 })
            for (int k = 0; k < 10; ++k)
            {
                int low = 0;
                for (const auto& [t, r] : lowRangeReadings (plucks ({ (double) root, (double) (root + steps[k]) }, 51 + (uint32_t) k)))
                    if (t > 0.06 && t < 1.5 && r.voiced && r.note <= root - 12)
                        ++low;
                if (low > 0)
                {
                    ++falseLow;
                    where << " " << noteText (root) << "+" << names[k] << " (" << low << ")";
                }
            }
        check (falseLow == 0, juce::String::formatted ("Two notes at once (B3 D4 E4 G4 A4 roots, m2 .. octave, 1.5 s): %d of 50 ever read as a single note an octave or more below the lower one "
                                                       "(their common period)",
                                                       falseLow)
                                  + where);
    }

    {
        std::vector<float> in;
        uint32_t seed = 300;
        for (int note : { 59, 60, 62, 64, 65, 67, 69, 71, 72, 74, 76 })
        {
            const double hz = hzFromMidi (note);
            const auto pl = ksPluck ([hz] (double) { return hz; }, 0.6, 0.5f, ksLossFor (hz, 3.0), seed++);
            in.insert (in.end(), pl.begin(), pl.end());
            in.insert (in.end(), (size_t) (0.1 * kFs), 0.0f);
        }
        std::vector<HopInfo> hops;
        Engine e;
        e.prepare (kFs, 64);
        e.hopListener = [&hops] (const HopInfo& h) { hops.push_back (h); };
        runEngine (e, in, EngineParams {});
        LowRangeDetector d;
        d.prepare (kFs);
        int both = 0, same = 0, octave = 0, run = 0;
        for (size_t k = 0; k + (size_t) d.hopSize() <= in.size(); k += (size_t) d.hopSize())
        {
            d.push (in.data() + k, d.hopSize());
            const auto& r = d.reading();
            run = r.voiced ? run + 1 : 0;
            if (run <= 3)
                continue;
            const double centre = (double) (k + (size_t) d.hopSize()) - 0.5 * d.windowLength();
            const HopInfo* nearest = nullptr;
            for (const auto& h : hops)
                if (nearest == nullptr || std::abs ((double) h.time - centre) < std::abs ((double) nearest->time - centre))
                    nearest = &h;
            if (nearest == nullptr || ! nearest->detector.voiced)
                continue;
            ++both;
            const int main = (int) std::lround (nearest->midi);
            same += main == r.note ? 1 : 0;
            octave += std::abs (main - r.note) >= 11 ? 1 : 0;
        }
        check (both > 100 && octave == 0 && same >= both - 2,
               juce::String::formatted ("Plucks B3 .. E5 against the main detector (default window): both voiced at %d readings, the same note at %d, an octave apart at %d", both, same, octave));
    }
}

// The display rules (v0.5.4, Tab.h's noteDisplayFor), as a pure function like the fretboard's classification: PLAYING
// NOW from the main detector; BELOW RANGE only from the low detector, below the window's lowest reliable note, drawn from
// E2 up and text below it; TOO LOW in place of 2+ NOTES? while the low detector hears a single note below the range.
void testDisplayRules()
{
    const int lowest = lowestReliableNote (kFs, defaultWindowHz()); // B3
    struct Case
    {
        const char* name;
        bool mainVoiced;
        int mainNote;
        bool lowVoiced;
        int lowNote;
        bool twoPlus;
        int playing, below;
        bool belowFretboard;
        PitchHint hint;
    };
    const Case cases[] = {
        { "main E4, low E4", true, 64, true, 64, false, 64, -1, false, PitchHint::None },
        { "main E4, low silent", true, 64, false, 0, false, 64, -1, false, PitchHint::None },
        { "main unvoiced, low A2", false, 0, true, 45, false, -1, 45, false, PitchHint::None },
        { "main unvoiced, low E2", false, 0, true, 40, false, -1, 40, false, PitchHint::None },
        { "main unvoiced, low D#2", false, 0, true, 39, false, -1, 39, true, PitchHint::None },
        { "main unvoiced, low C2", false, 0, true, 36, false, -1, 36, true, PitchHint::None },
        { "main unvoiced, low B3 (in range: a dropout)", false, 0, true, lowest, false, -1, -1, false, PitchHint::None },
        { "main unvoiced, low A#3 (just below)", false, 0, true, lowest - 1, false, -1, lowest - 1, false, PitchHint::None },
        { "main unvoiced, low silent", false, 0, false, 0, false, -1, -1, false, PitchHint::None },
        { "2+ NOTES?, low A2: TOO LOW", false, 0, true, 45, true, -1, 45, false, PitchHint::TooLow },
        // v0.6.2 (issue 10): the low detector's single note below the range wins over a voiced main detector too (BELOW RANGE as well)
        { "2+ NOTES?, main A3 (a harmonic), low A2: TOO LOW, main shown, below range too", true, 57, true, 45, true, 57, 45, false, PitchHint::TooLow },
        { "main A4 (the octave-up of A3), low A3: below range A3, main shown", true, 69, true, 57, false, 69, 57, false, PitchHint::None },
        { "main B3 (the window's edge), low A#3: below range A#3, main shown", true, lowest, true, lowest - 1, false, lowest, lowest - 1, false, PitchHint::None },
        { "main E4, low E4: no below range", true, 64, true, 64, false, 64, -1, false, PitchHint::None },
        { "2+ NOTES?, low not single", false, 0, false, 0, true, -1, -1, false, PitchHint::TwoPlusNotes },
        { "2+ NOTES?, low E4 (in range: two notes)", false, 0, true, 64, true, -1, -1, false, PitchHint::TwoPlusNotes },
    };
    bool ok = true;
    juce::String bad;
    for (const auto& c : cases)
    {
        const auto d = noteDisplayFor (c.mainVoiced, c.mainNote, c.lowVoiced, c.lowNote, lowest, c.twoPlus);
        const bool right = d.playing == c.playing && d.belowRange == c.below && d.belowFretboard == c.belowFretboard && d.hint == c.hint;
        ok = ok && right;
        if (! right)
            bad << "; " << c.name;
    }
    // BELOW RANGE's positions on the board: every position of the note (A2: low E fret 5 and the open A).
    int positions = 0;
    for (int string = 0; string < 6; ++string)
        for (int fret = 0; fret <= kFretCount; ++fret)
            positions += positionNote (string, fret) == 45 ? 1 : 0;
    check (ok && positions == 2, juce::String::formatted ("%d cases at the default window (lowest reliable %s); A2's positions on the board: %d", (int) std::size (cases),
                                                          noteText (lowest).toRawUTF8(), positions)
                                     + bad);
}

// The Dynamics fix (v0.5.4): a soft note (G4, 0.05 peak), then within Note Release a loud sound the detector can't pitch:
// a saw below the default window (D3, 0.6 peak) or a strum (E2 A2 D3 G3 together). Over the burst, the gain Dynamics 50%
// applies (the output against the same render at Dynamics 100%, bypassed) is close to what Dynamics gives the burst's
// own level, (1 - 0.5)(T - level), with the level its mean |x|; v0.5.3 (the reference frozen while pitch is lost) gave it
// the soft note's boost. Close: within 1.25 dB on the note, 2 dB on the strum, and only ever lower. The reference is a
// running peak of a level measured over the last detected note's period (NoteReference: 2 ms smoothed while pitch is
// lost), so on an unpitched sound it reads somewhat above the mean |x|, more on a strum, whose level beats between the
// strings. And the onset: the loudest 2 ms of the burst's first 60 ms against its level over 30 .. 200 ms, at most 1 dB
// over (v0.5.3 in brackets): the reference rises with the sound, so its first milliseconds don't keep the soft note's gain.
void testDynamicsFix()
{
    const double g4 = hzFromMidi (67);
    const auto soft = ksPluck ([g4] (double) { return g4; }, 0.6, 0.05f, ksLossFor (g4, 3.0), 31);
    const auto lowSaw = sawFromTrajectory ([] (double) { return hzFromMidi (50); }, 0.6, 0.6f);
    std::vector<float> strum ((size_t) (0.6 * kFs), 0.0f);
    for (int note : { 40, 45, 50, 55 })
    {
        const auto tone = sawFromTrajectory ([note] (double) { return hzFromMidi (note); }, 0.6, 0.25f);
        for (size_t n = 0; n < strum.size(); ++n)
            strum[n] += tone[n];
    }
    struct Burst
    {
        const char* name;
        const std::vector<float>* burst;
    };
    for (const Burst b : { Burst { "saw D3 (below the window)", &lowSaw }, Burst { "strum E2 A2 D3 G3", &strum } })
        for (NoteReleaseV058 nr : { NoteReleaseV058::Fast, NoteReleaseV058::Long })
        {
            const auto in = joined ({ soft, *b.burst });
            auto render = [&in, nr] (float dynamics, bool legacy) {
                EngineParams p;
                p.sustainPercent = 0.0f; // v0.5.4's default, as measured (75% since v0.5.6)
                p.holdMs = noteReleaseMsFor (nr);
                p.dynamicsPercent = dynamics;
                Engine e;
                e.prepare (kFs, 64);
                e.setHeadroomGain (false); // the gain as Dynamics applies it (v0.5.3's burst reaches the clip)
                e.setLegacyNoteReference (legacy);
                e.setSputterFix (false); // v0.5.4's Dynamics fix measured alone
                return runEngine (e, in, p);
            };
            const int L = latencyFor (kFs, defaultWindowHz());
            const size_t from = soft.size() + (size_t) L + (size_t) (0.03 * kFs), to = soft.size() + (size_t) L + (size_t) (0.2 * kFs);
            const auto bypassed = render (100.0f, false);
            const double fixedDb = toDb (rms (render (50.0f, false), from, to) / rms (bypassed, from, to));
            const double oldDb = toDb (rms (render (50.0f, true), from, to) / rms (bypassed, from, to));
            double meanAbs = 0.0;
            for (size_t n = soft.size() + (size_t) (0.03 * kFs); n < soft.size() + (size_t) (0.2 * kFs); ++n)
                meanAbs += std::abs (in[n]);
            meanAbs /= (double) (0.17 * kFs);
            const double want = std::min ((double) kDynamicsMaxBoostDb, 0.5 * (kDynamicsTargetDb - toDb (meanAbs)));
            const auto fixed = render (50.0f, false);
            const auto old = render (50.0f, true);
            auto overshoot = [&] (const std::vector<float>& out) {
                double loudest = 0.0;
                const size_t start = soft.size() + (size_t) L, w2 = (size_t) (0.002 * kFs);
                for (size_t k = start; k + w2 <= start + (size_t) (0.06 * kFs); k += w2)
                    loudest = std::max (loudest, rms (out, k, k + w2));
                return toDb (loudest / rms (out, from, to));
            };
            const double onsetDb = overshoot (fixed);
            check (fixedDb <= want && fixedDb >= want - (b.burst == &strum ? 2.0 : 1.25) && onsetDb <= 1.0,
                   juce::String::formatted ("Soft G4 then a loud %s, Note Release %s: Dynamics gain on it %+.2f dB, its own level's %+.2f dB (mean |x| %.1f dBFS); v0.5.3 gave it %+.2f dB; "
                                            "its onset's loudest 2 ms %+.2f dB re. its level [%+.2f]",
                                            b.name, nr == NoteReleaseV058::Fast ? "Fast" : "Long", fixedDb, want, toDb (meanAbs), oldDb, onsetDb, overshoot (old)));
        }
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// investigate-sputter (branch): the end of long held notes. probe-sputter reports the baseline and the
// candidate fixes on 18 long notes; probe-sputter-full (only with --only) is the whole investigation:
// per-hop CSVs, the settings sweep, the mitigation table and the audio examples. Neither ever fails.
// ---------------------------------------------------------------------------
namespace sputter
{
// A long string note, additive: partials k f0 sqrt(1 + B k^2) (B = 1e-4) with pluck-position amplitudes,
// the upper ones dying faster. Each partial is two polarizations 0.12 + 0.05 k Hz apart, one decaying in
// 0.45 of the other's time (beating, and the two-stage decay of a real string). A 4 ms noise burst at the
// pick. Peak normalized to amp.
struct StringNote
{
    double hz = 329.63;
    double seconds = 8.0;
    double t60 = 8.0;              // the fundamental's slower polarization
    float amp = 0.5f;              // peak (-6 dBFS)
    double muteAt = -1.0;          // > 0: from here every partial dies in kMuteT60 (a palm mute)
    double fundamentalGain = 1.0;  // < 1: a weak fundamental that also dies in half the time (harmonic 2 outlasts it)
    double fadeOutAt = -1.0;       // > 0: a 10 ms fade to silence here (the next note's pick)
    uint32_t seed = 1;
};
constexpr double kMuteT60 = 0.08;

std::vector<float> stringNote (const StringNote& s, double fs = kFs)
{
    const size_t n = (size_t) (s.seconds * fs);
    std::vector<double> y (n, 0.0);
    std::minstd_rand rng (s.seed);
    std::uniform_real_distribution<double> uni (0.0, 1.0);
    const size_t muteN = s.muteAt > 0.0 ? (size_t) (s.muteAt * fs) : n;
    const double muteDecay = std::exp (-6.907755 / (kMuteT60 * fs));
    for (int k = 1; k <= 40; ++k)
    {
        const double fk = k * s.hz * std::sqrt (1.0 + 1.0e-4 * k * k);
        if (fk > std::min (7000.0, 0.45 * fs))
            break;
        double a = std::abs (std::sin (M_PI * k * 0.18)) / k;
        double t60k = s.t60 / (1.0 + 0.25 * (k - 1) + 0.01 * (k - 1) * (k - 1));
        if (k == 1 && s.fundamentalGain < 1.0)
        {
            a *= s.fundamentalGain;
            t60k *= 0.5;
        }
        for (int pol = 0; pol < 2; ++pol)
        {
            const double f = fk + (pol == 0 ? 0.0 : 0.12 + 0.05 * k);
            const double decay = std::exp (-6.907755 / ((pol == 0 ? t60k : 0.45 * t60k) * fs));
            const double ph = 2.0 * M_PI * uni (rng);
            double re = std::cos (ph), im = std::sin (ph);
            const double cr = std::cos (2.0 * M_PI * f / fs), ci = std::sin (2.0 * M_PI * f / fs);
            double g = a * (pol == 0 ? 0.7 : 0.3);
            for (size_t i = 0; i < n; ++i)
            {
                y[i] += g * re;
                const double nr = re * cr - im * ci;
                im = re * ci + im * cr;
                re = nr;
                g *= i < muteN ? decay : muteDecay;
            }
        }
    }
    // The pick: 4 ms of lowpassed noise, and a 1 ms fade-in.
    double peak = 0.0;
    for (auto v : y)
        peak = std::max (peak, std::abs (v));
    std::uniform_real_distribution<double> dist (-1.0, 1.0);
    double lp = 0.0;
    const size_t burst = (size_t) (0.004 * fs), fadeIn = (size_t) (0.001 * fs);
    for (size_t i = 0; i < std::min (burst, n); ++i)
    {
        lp += 0.3 * (dist (rng) - lp);
        y[i] += 0.4 * peak * lp * (1.0 - (double) i / burst);
    }
    for (size_t i = 0; i < std::min (fadeIn, n); ++i)
        y[i] *= (double) i / fadeIn;
    if (s.fadeOutAt > 0.0)
    {
        const size_t from = (size_t) (s.fadeOutAt * fs), len = (size_t) (0.01 * fs);
        for (size_t i = from; i < n; ++i)
            y[i] *= i < from + len ? 1.0 - (double) (i - from) / len : 0.0;
    }
    peak = 0.0;
    for (auto v : y)
        peak = std::max (peak, std::abs (v));
    std::vector<float> out (n);
    for (size_t i = 0; i < n; ++i)
        out[i] = (float) (s.amp * y[i] / peak);
    return out;
}

// The noise floor: pink noise (Kellet's filter) at rmsDb, plus 60 Hz hum (60 / 120 / 180 Hz at 1, 0.5, 0.3)
// 6 dB under it.
std::vector<float> noiseFloor (size_t n, float rmsDb, uint32_t seed, double fs = kFs)
{
    std::vector<double> x (n);
    std::minstd_rand rng (seed);
    std::uniform_real_distribution<double> dist (-1.0, 1.0);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, sum = 0.0;
    for (auto& v : x)
    {
        const double w = dist (rng);
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        v = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        sum += v * v;
    }
    const double pinkGain = dbToGain (rmsDb) / std::sqrt (sum / (double) n);
    const double humAmp = dbToGain (rmsDb - 6.0f) * std::sqrt (2.0 / (1.0 + 0.25 + 0.09));
    std::vector<float> out (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / fs;
        const double hum = std::sin (2 * M_PI * 60 * t) + 0.5 * std::sin (2 * M_PI * 120 * t + 1.0) + 0.3 * std::sin (2 * M_PI * 180 * t + 2.0);
        out[i] = (float) (pinkGain * x[i] + humAmp * hum);
    }
    return out;
}

struct NoteSpec
{
    const char* name;
    int midi;
    double t60;
    double fundamentalGain;
};
const NoteSpec kNotes[] = { { "G3", 55, 10.0, 1.0 }, { "B3", 59, 9.0, 1.0 }, { "B3weakfund", 59, 9.0, 0.25 },
                            { "E4", 64, 8.0, 1.0 },  { "A4", 69, 6.0, 1.0 }, { "E5", 76, 4.5, 1.0 } };
const float kNoiseDbs[] = { -70.0f, -60.0f, -50.0f };

struct Input
{
    juce::String name;
    std::vector<float> x;
    size_t noteStart = 0; // the long note's first sample
    size_t noteEnd = 0;   // ... and its end (0: the end of x; a real take's next pick)
    int note = 0;
    float noiseDb = 0.0f;
    // A real take: the measuring detector's pitch per hop (window centre, smoothed MIDI, -1 unvoiced). When it's
    // there, the voice's pitch is judged against what was played at that moment (slides) rather than against note.
    std::vector<std::pair<int64_t, float>> truth;
};

// The long note (T60 x 1.25 + 1 s, at most 13 s) over the noise floor. memoryPrefix: first two 250 ms notes a
// major third and a fifth above, so Memory holds a three-note chord when the long note starts.
Input makeInput (const NoteSpec& ns, float noiseDb, bool memoryPrefix)
{
    Input in;
    in.note = ns.midi;
    in.noiseDb = noiseDb;
    in.name = juce::String (ns.name) + "-noise" + juce::String ((int) noiseDb) + (memoryPrefix ? "-prefix" : "");
    if (memoryPrefix)
    {
        uint32_t seed = 900;
        for (int up : { 4, 7 })
        {
            StringNote p;
            p.hz = hzFromMidi (ns.midi + up);
            p.seconds = 0.25;
            p.t60 = 3.0;
            p.fadeOutAt = 0.24;
            p.seed = seed++;
            const auto x = stringNote (p);
            in.x.insert (in.x.end(), x.begin(), x.end());
        }
    }
    in.noteStart = in.x.size();
    StringNote s;
    s.hz = hzFromMidi (ns.midi);
    s.t60 = ns.t60;
    s.seconds = std::min (13.0, ns.t60 * 1.25 + 1.0);
    s.fundamentalGain = ns.fundamentalGain;
    s.seed = (uint32_t) (ns.midi * 7 + (ns.fundamentalGain < 1.0 ? 1 : 0));
    const auto x = stringNote (s);
    in.x.insert (in.x.end(), x.begin(), x.end());
    const auto noise = noiseFloor (in.x.size(), noiseDb, (uint32_t) (1000 + ns.midi));
    for (size_t i = 0; i < in.x.size(); ++i)
        in.x[i] += noise[i];
    return in;
}

// One render with everything logged: per hop (detection) and per sample (the voice, on its delayed timeline,
// which is the input's own time axis).
struct HopRec
{
    int64_t time;
    bool voiced, guard, locked; // locked: the tail's pitch lock held the note on this hop
    float confidence, rms, f0;
    int note;
    double target;
};
struct Render
{
    std::vector<HopRec> hops;
    std::vector<float> amp, env, gate, ref, level, dyn, head, out; // amp: the predicted output peak after the headroom gain
    std::vector<uint8_t> onset, open, capped;
    std::vector<int8_t> held;
    std::vector<int> tones; // per 64-sample block: chord tones sounding
    int latency = 0, holdSamples = 0, locks = 0;
};

Render render (const Input& in, const EngineParams& p, const TailSwitches& sw, bool keepOut = false)
{
    Engine e;
    e.prepare (kFs, 64);
    e.setTailSwitches (sw);
    if (sw.muteDropDb <= 0.0f)
    {
        e.setBlipFixes (false, false); // v0.5.4 and the investigation's variants of it: without v0.5.10's blip fixes
        e.setV062Fixes (false);        // ... and v0.6.2's
    }
    e.setPickGlide (false);            // ... and v0.5.11's pick glide (as it has been since v0.5.11, for every switch set)
    Render r;
    const size_t n = in.x.size();
    r.amp.assign (n, 0.0f);
    r.env.assign (n, 0.0f);
    r.gate.assign (n, 0.0f);
    r.ref.assign (n, 0.0f);
    r.level.assign (n, 0.0f);
    r.dyn.assign (n, 1.0f);
    r.head.assign (n, 1.0f);
    r.onset.assign (n, 0);
    r.open.assign (n, 0);
    r.capped.assign (n, 0);
    r.held.assign (n, 0);
    int locksSoFar = 0;
    e.hopListener = [&] (const HopInfo& h) {
        const bool locked = e.tailLocks() != locksSoFar;
        locksSoFar = e.tailLocks();
        r.hops.push_back ({ h.time, h.detector.voiced, h.detector.guardRejected, locked, h.detector.confidence, h.detector.rms, h.detector.f0, h.noteN, h.target });
    };
    e.voiceListener = [&] (const VoiceProbe& v) {
        if (v.t < 0 || v.t >= (int64_t) n)
            return;
        const auto i = (size_t) v.t;
        r.amp[i] = v.predictedPeak * v.headroomGain;
        r.env[i] = v.env;
        r.gate[i] = v.gate;
        r.ref[i] = v.ref;
        r.level[i] = v.level;
        r.dyn[i] = v.dynamicsGain;
        r.head[i] = v.headroomGain;
        r.onset[i] = v.onset;
        r.open[i] = v.open;
        r.capped[i] = v.capped;
        r.held[i] = (int8_t) v.memoryHeld;
    };
    std::vector<float> out (n);
    for (size_t start = 0; start < n; start += 64)
    {
        const int len = (int) std::min ((size_t) 64, n - start);
        e.process (in.x.data() + start, out.data() + start, len, p);
        r.tones.push_back ((int) unpackTones (e.readout().chordTones.load()).size());
    }
    r.latency = e.currentLatencySamples();
    r.holdSamples = holdSamplesFor (kFs, p.holdMs);
    r.locks = e.tailLocks();
    if (keepOut)
        r.out = std::move (out);
    return r;
}

float db (float x) { return 20.0f * std::log10 (std::max (x, 1.0e-10f)); }

constexpr double kTailFromS = 0.15;      // events are counted from this far into the long note (past the pick and settle)
constexpr float kAudibleDipDb = 3.0f;    // a dip and recovery at least this deep is a sputter ...
constexpr float kAudibleRangeDb = 40.0f; // ... when it starts within this many dB of the note's loudest output

struct Result
{
    bool skipped = false;
    int dropouts = 0, dropRms = 0, dropConf = 0, dropShort = 0, dropMid = 0, dropLong = 0; // <= 1 hop, 2 .. 10, > 10 hops
    double dropMsTotal = 0.0, firstDropS = -1.0;
    float firstDropDbfs = 0.0f, firstDropBelowDb = 0.0f;
    int jumpOct = 0, jumpFifth = 0, jumpSemi = 0, jumpOther = 0, guard = 0; // changes away from the played note: octaves, fifths, 1-2 semitones, other
    float maxCents = 0.0f;  // the voice's pitch (T) off the played note, at most (voiced hops of the tail)
    double offMs = 0.0;     // ... and how long it's more than 20 c off
    int louder = 0;         // dropouts after which the output comes back 1 dB+ louder than before the gap
    float maxLouder = 0.0f;
    int falseOnsets = 0;
    int dips1 = 0, dips3 = 0, dips10 = 0;
    float maxDip = 0.0f;
    double firstAudibleS = -1.0;
    float firstAudibleDbfs = 0.0f, firstAudibleBelowDb = 0.0f;
    int levelJumps = 0; // the output rising 3+ dB more than the input in one rise
    int reopens = 0;    // the voice gate fully closed and opened again
    int heldChanges = 0, memoryClears = 0, chordLost = 0;
    double lastVoicedS = 0.0, tailEndS = 0.0;
    float lastVoicedDbfs = 0.0f, lastVoicedBelowDb = 0.0f;
    int locks = 0;
};

// On a real take, what's played at t: note if the measuring detector reads it within 6 hops (16 ms: its window is
// three times as long), so a slide or a fret step isn't an error; else the note it reads at t (-1 unvoiced).
int playedNear (const std::vector<std::pair<int64_t, float>>& truth, int64_t t, int note)
{
    auto it = std::lower_bound (truth.begin(), truth.end(), std::pair<int64_t, float> { t, -2.0f });
    const int c = (int) std::clamp<std::ptrdiff_t> (it - truth.begin(), 0, (std::ptrdiff_t) truth.size() - 1);
    for (int j = std::max (0, c - 6); j <= std::min ((int) truth.size() - 1, c + 6); ++j)
        if (truth[(size_t) j].second >= 0.0f && (int) std::lround (truth[(size_t) j].second) == note)
            return note;
    return truth[(size_t) c].second >= 0.0f ? (int) std::lround (truth[(size_t) c].second) : -1;
}

Result analyse (const Input& in, const Render& r)
{
    Result res;
    res.locks = r.locks;
    const auto n = (int64_t) (in.noteEnd > 0 ? std::min (in.noteEnd, in.x.size()) : in.x.size());
    const int64_t note0 = (int64_t) in.noteStart, from = note0 + (int64_t) (kTailFromS * kFs);
    const double hopMs = 1000.0 * hopFor (kFs) / kFs;

    // Hops of the long note.
    std::vector<const HopRec*> hops;
    float peakRms = 0.0f;
    for (auto& h : r.hops)
        if (h.time >= note0 && h.time < n)
        {
            peakRms = std::max (peakRms, h.rms);
            if (h.time >= from)
                hops.push_back (&h);
        }
    int first = -1, last = -1;
    for (int k = 0; k < (int) hops.size(); ++k)
        if (hops[(size_t) k]->voiced)
        {
            if (first < 0)
                first = k;
            last = k;
        }
    if (first < 0)
    {
        res.skipped = true;
        return res;
    }
    res.lastVoicedS = (double) (hops[(size_t) last]->time - note0) / kFs;
    res.lastVoicedDbfs = db (hops[(size_t) last]->rms);
    res.lastVoicedBelowDb = db (peakRms) - res.lastVoicedDbfs;

    float peakAmp = 0.0f;
    for (int64_t t = note0; t < n; ++t)
        peakAmp = std::max (peakAmp, r.amp[(size_t) t]);
    const float peakAmpDb = db (peakAmp);
    auto ampDb = [&] (int64_t t) { return db (r.amp[(size_t) std::clamp<int64_t> (t, 0, n - 1)]); };

    // Dropouts (unvoiced runs between voiced hops) and the dip each makes in the output.
    for (int k = first + 1; k <= last; ++k)
    {
        if (hops[(size_t) k]->voiced)
            continue;
        int end = k;
        bool rmsCause = false;
        while (! hops[(size_t) end]->voiced)
        {
            rmsCause = rmsCause || hops[(size_t) end]->rms <= kVoicedRmsFloor;
            ++end;
        }
        const int len = end - k;
        ++res.dropouts;
        (rmsCause ? res.dropRms : res.dropConf)++;
        (len <= 1 ? res.dropShort : len <= 10 ? res.dropMid : res.dropLong)++;
        res.dropMsTotal += len * hopMs;
        if (res.firstDropS < 0.0)
        {
            res.firstDropS = (double) (hops[(size_t) k]->time - note0) / kFs;
            res.firstDropDbfs = db (hops[(size_t) k - 1]->rms);
            res.firstDropBelowDb = db (peakRms) - res.firstDropDbfs;
        }
        const int64_t a = hops[(size_t) k]->time, b = hops[(size_t) end]->time;
        const float before = ampDb (a - 1);
        float lo = before;
        for (int64_t t = a; t < std::min (n, b + (int64_t) (0.010 * kFs)); ++t)
            lo = std::min (lo, ampDb (t));
        float after = lo;
        for (int64_t t = b; t < std::min (n, b + (int64_t) (0.025 * kFs)); ++t)
            after = std::max (after, ampDb (t));
        const float depth = std::min (before - lo, after - lo);
        if (after > before + 1.0f && before >= peakAmpDb - kAudibleRangeDb)
        {
            ++res.louder;
            res.maxLouder = std::max (res.maxLouder, after - before);
        }
        if (depth >= 1.0f)
            ++res.dips1;
        if (depth >= kAudibleDipDb && before >= peakAmpDb - kAudibleRangeDb)
        {
            ++res.dips3;
            if (res.firstAudibleS < 0.0)
            {
                res.firstAudibleS = (double) (a - note0) / kFs;
                res.firstAudibleDbfs = db (hops[(size_t) k - 1]->rms);
                res.firstAudibleBelowDb = db (peakRms) - res.firstAudibleDbfs;
            }
        }
        if (depth >= 10.0f && before >= peakAmpDb - kAudibleRangeDb)
            ++res.dips10;
        if (before >= peakAmpDb - kAudibleRangeDb)
            res.maxDip = std::max (res.maxDip, depth);
        k = end;
    }

    // Pitch: changes of the quantized note away from the played one (on a real take, what's played at that moment).
    auto played = [&] (int64_t t, int note) { return in.truth.empty() ? in.note : playedNear (in.truth, t, note); };
    for (int k = first + 1; k <= last; ++k)
    {
        res.guard += hops[(size_t) k]->guard ? 1 : 0;
        if (! hops[(size_t) k]->voiced)
            continue;
        const int now = hops[(size_t) k]->note, playedNote = played (hops[(size_t) k]->time, now);
        if (playedNote < 0)
            continue;
        const double tgt = hops[(size_t) k]->target;
        const float cents = (float) std::abs (100.0 * (tgt - played (hops[(size_t) k]->time, (int) std::lround (tgt))));
        res.maxCents = std::max (res.maxCents, cents);
        res.offMs += cents > 20.0f ? hopMs : 0.0;
        int prev = k - 1;
        while (prev > first && ! hops[(size_t) prev]->voiced)
            --prev;
        const int before = hops[(size_t) prev]->note;
        if (now == before || now == playedNote)
            continue;
        const int d = now - playedNote;
        if (d % 12 == 0)
            ++res.jumpOct;
        else if (d == 7 || d == 19 || d == -5 || d == -17)
            ++res.jumpFifth;
        else if (std::abs (d) <= 2)
            ++res.jumpSemi;
        else
            ++res.jumpOther;
    }

    // The voice: onsets, gate reopenings, HELD, the output's level against the input's, the end of the tail.
    bool closed = false;
    int lastHeld = r.held[(size_t) from];
    for (int64_t t = from; t < n; ++t)
    {
        const auto i = (size_t) t;
        if (t <= hops[(size_t) last]->time)
            res.falseOnsets += r.onset[i];
        if (r.gate[i] <= 0.0f)
            closed = true;
        else if (closed)
        {
            closed = false;
            ++res.reopens;
        }
        if (r.held[i] != lastHeld)
        {
            ++res.heldChanges;
            if (r.held[i] == 0)
            {
                bool voicedLater = false;
                for (auto* h : hops)
                    voicedLater = voicedLater || (h->time > t && h->voiced);
                res.memoryClears += voicedLater ? 1 : 0;
            }
            lastHeld = r.held[i];
        }
        if (db (r.amp[i]) >= peakAmpDb - kAudibleRangeDb)
            res.tailEndS = (double) (t - note0) / kFs;
    }
    {
        // Rises of the output (1 ms frames) that the input doesn't make: dA - max(dE, 0) >= 3 dB.
        const int64_t frame = (int64_t) (0.001 * kFs);
        float prevA = ampDb (from), startA = prevA, startE = db (r.env[(size_t) from]);
        bool rising = false;
        for (int64_t t = from + frame; t < n; t += frame)
        {
            const float a = ampDb (t);
            if (a > prevA + 0.01f)
            {
                if (! rising)
                {
                    rising = true;
                    startA = prevA;
                    startE = db (r.env[(size_t) (t - frame)]);
                }
            }
            else if (rising)
            {
                rising = false;
                const float dA = prevA - startA, dE = db (r.env[(size_t) (t - frame)]) - startE;
                if (dA - std::max (dE, 0.0f) >= kAudibleDipDb && prevA >= peakAmpDb - kAudibleRangeDb)
                    ++res.levelJumps;
            }
            prevA = a;
        }
    }
    {
        // Chord tones lost while the note is still detected later.
        const size_t b0 = (size_t) ((note0 + (int64_t) (0.2 * kFs)) / 64), b1 = (size_t) ((note0 + (int64_t) (1.0 * kFs)) / 64);
        int most = 0;
        for (size_t b = b0; b < std::min (b1, r.tones.size()); ++b)
            most = std::max (most, r.tones[b]);
        for (size_t b = b1; b < r.tones.size() && most > 1; ++b)
            if (r.tones[b] > 0 && r.tones[b] < most)
            {
                bool voicedLater = false;
                for (auto* h : hops)
                    voicedLater = voicedLater || (h->time > (int64_t) (b * 64) && h->voiced);
                res.chordLost = voicedLater ? 1 : 0;
                break;
            }
    }
    return res;
}

// The per-hop timeline of the long note (from 0.5 s before its last voiced hop's 3 s before the end ... the whole note).
void writeHopCsv (const juce::String& name, const Input& in, const Render& r, const EngineParams& p)
{
    Csv csv ("sputter/" + name, "t_s,in_rms_dbfs,in_below_peak_db,confidence,voiced,guard_rejected,f0_hz,note,onset,gate,hold_left_ms,ref_dbfs,level_dbfs,"
                                "dynamics_db,headroom_db,held,out_peak_dbfs,out_rms_dbfs");
    const int64_t note0 = (int64_t) in.noteStart, n = (int64_t) in.x.size();
    float peakRms = 0.0f;
    for (auto& h : r.hops)
        if (h.time >= note0)
            peakRms = std::max (peakRms, h.rms);
    int64_t lastVoiced = -1;
    const int hop = hopFor (kFs);
    for (auto& h : r.hops)
    {
        if (h.voiced)
            lastVoiced = h.time;
        if (h.time < note0 || h.time + hop > n)
            continue;
        const auto i = (size_t) h.time;
        int onset = 0;
        for (size_t j = i; j < i + (size_t) hop; ++j)
            onset |= r.onset[j];
        const double holdLeft = h.voiced ? p.holdMs : std::max (0.0, p.holdMs - 1000.0 * (double) (h.time - lastVoiced) / kFs);
        double sq = 0.0;
        for (size_t j = i + (size_t) r.latency; j < std::min ((size_t) n, i + (size_t) (r.latency + hop)); ++j)
            sq += (double) r.out[j] * r.out[j];
        csv.row (juce::String ((double) (h.time - note0) / kFs, 4) + "," + juce::String (db (h.rms), 1) + "," + juce::String (db (peakRms) - db (h.rms), 1) + ","
                 + juce::String (h.confidence, 3) + "," + juce::String ((int) h.voiced) + "," + juce::String ((int) h.guard) + "," + juce::String (h.f0, 1) + ","
                 + juce::String (h.note) + "," + juce::String (onset) + "," + juce::String (r.gate[i], 3) + "," + juce::String (holdLeft, 0) + ","
                 + juce::String (db (r.ref[i]), 1) + "," + juce::String (db (r.level[i]), 1) + "," + juce::String (db (r.dyn[i]), 1) + ","
                 + juce::String (db (r.head[i]), 1) + "," + juce::String ((int) r.held[i]) + "," + juce::String (db (r.amp[i]), 1) + ","
                 + juce::String (db ((float) std::sqrt (sq / hop)), 1));
    }
}

// A labelled engine setting and a labelled mitigation.
struct Setting
{
    juce::String name;
    EngineParams p;
    bool prefix = false; // Memory: the two-note prefix
};
struct Mitigation
{
    juce::String name;
    TailSwitches sw;
};

EngineParams baseParams (float sustain = 100.0f, NoteReleaseV058 release = NoteReleaseV058::Fast)
{
    EngineParams p;
    p.sustainPercent = sustain;
    p.holdMs = noteReleaseMsFor (release);
    return p;
}

// The recommended fix: a 40 ms grace before the release; in the tail (the input 20 dB under the note's reference) no
// comeback to the held level without a new pick and no harmonic jumps (octave, 12th, two octaves) without one; and no
// revival of a tail once the gate has closed.
TailSwitches recommendedTail()
{
    TailSwitches s;
    s.graceMs = 40.0f;
    s.oneWayUnderDb = 20.0f;
    s.lockUnderRefDb = 20.0f;
    s.reviveRiseDb = 6.0f;
    s.lockPersistMs = 1.0f; // under one hop: only harmonic jumps are refused
    return s;
}

// Batch 2's revision of the fix: a mute (the level falling 20 dB within 50 ms) ends the note at once (no grace) until a pick
// (an onset rising 25 dB above the quietest since, or the level back within 25 dB of the reference); a reading at the
// detector's shortest lag is unvoiced.
TailSwitches fix2Tail()
{
    auto s = recommendedTail();
    s.muteDropDb = 20.0f;
    s.muteWakeRiseDb = 25.0f;
    s.muteSkipsGrace = true;
    s.edgeLagUnvoiced = true;
    return s;
}

std::vector<Mitigation> mitigations()
{
    std::vector<Mitigation> m;
    m.push_back ({ "none", {} });
    for (float g : { 20.0f, 40.0f, 60.0f })
    {
        TailSwitches s;
        s.graceMs = g;
        m.push_back ({ "1 grace " + juce::String ((int) g) + "ms", s });
    }
    for (auto [c, rms] : { std::pair { 0.7f, -60.0f }, { 0.6f, -60.0f }, { 0.5f, -60.0f }, { 0.6f, -66.0f }, { 0.5f, -70.0f } })
    {
        TailSwitches s;
        s.keepConfidence = c;
        s.keepRmsDb = rms;
        m.push_back ({ "2 sticky " + juce::String (c, 1) + " / " + juce::String ((int) rms) + "dB", s });
    }
    for (float f : { -45.0f, -50.0f, -55.0f })
    {
        TailSwitches s;
        s.endBelowDb = f;
        m.push_back ({ "3 end below " + juce::String ((int) f) + "dB", s });
    }
    for (float top : { -35.0f, -40.0f, -45.0f })
    {
        TailSwitches s;
        s.taperTopDb = top;
        m.push_back ({ "4 taper " + juce::String ((int) top) + ".." + juce::String ((int) (top - 20)) + "dB", s });
    }
    for (float l : { -40.0f, -50.0f })
    {
        TailSwitches s;
        s.lockBelowDb = l;
        m.push_back ({ "5 lock below " + juce::String ((int) l) + "dB", s });
    }
    for (float u : { 20.0f, 30.0f })
    {
        TailSwitches s;
        s.taperUnderRefDb = u;
        m.push_back ({ "4r taper " + juce::String ((int) u) + ".." + juce::String ((int) u + 20) + "dB under ref", s });
    }
    for (float u : { 20.0f, 30.0f })
    {
        TailSwitches s;
        s.lockUnderRefDb = u;
        m.push_back ({ "5r lock " + juce::String ((int) u) + "dB under ref", s });
    }
    for (float u : { 0.0f, 10.0f, 20.0f, 30.0f })
    {
        TailSwitches s;
        s.oneWayUnderDb = u;
        m.push_back ({ "6 one-way " + juce::String ((int) u) + "dB under ref", s });
    }
    {
        TailSwitches s;
        s.reviveRiseDb = 6.0f;
        m.push_back ({ "7 revive needs +6dB", s });
    }
    {
        TailSwitches s;
        s.graceMs = 40.0f;
        s.keepConfidence = 0.6f;
        m.push_back ({ "1+2 grace 40 + sticky 0.6", s });
    }
    {
        TailSwitches s;
        s.graceMs = 40.0f;
        s.taperUnderRefDb = 20.0f;
        m.push_back ({ "1+4r grace 40 + taper 20", s });
        s.keepConfidence = 0.6f;
        m.push_back ({ "1+2+4r ... + sticky 0.6", s });
    }
    for (auto [g, u] : { std::pair { 40.0f, 20.0f }, { 60.0f, 20.0f }, { 40.0f, 30.0f } })
    {
        TailSwitches s;
        s.graceMs = g;
        s.oneWayUnderDb = u;
        m.push_back ({ "1+6 grace " + juce::String ((int) g) + " + one-way " + juce::String ((int) u), s });
    }
    for (float ms : { 0.0f, 10.0f, 30.0f })
    {
        TailSwitches s = recommendedTail();
        s.lockPersistMs = ms;
        m.push_back ({ ms > 0.0f ? "RECOMMENDED, lock lets " + juce::String ((int) ms) + "ms through" : juce::String ("RECOMMENDED, hard lock"), s });
    }
    {
        TailSwitches s = recommendedTail();
        s.lockUnderRefDb = 0.0f;
        m.push_back ({ "RECOMMENDED without the lock", s });
    }
    {
        TailSwitches s = recommendedTail();
        s.reviveRiseDb = 0.0f;
        m.push_back ({ "1+6+5r grace 40 + one-way 20 + lock 20", s });
        m.push_back ({ "1+6+5h+7 RECOMMENDED", recommendedTail() });
        auto r8 = recommendedTail();
        r8.pickRestartUnderDb = 30.0f;
        m.push_back ({ "RECOMMENDED + 8 pick restarts 30 dB under", r8 });
        m.push_back ({ "fix2 = RECOMMENDED + 9 mute 20 (no grace; wake on a 25 dB rise) + 10 edge lag", fix2Tail() });
        auto f3 = fix2Tail();
        f3.muteSkipsGrace = false;
        m.push_back ({ "fix2, the grace kept at mutes", f3 });
        auto f4 = recommendedTail();
        f4.lockOnPitch = true;
        m.push_back ({ "RECOMMENDED + lock on pitch", f4 });
        auto f2 = recommendedTail();
        f2.edgeLagUnvoiced = true;
        m.push_back ({ "RECOMMENDED + 10 edge lag", f2 });
        s = recommendedTail();
        s.keepConfidence = 0.7f;
        m.push_back ({ "1+2+6+5r+7 ... + sticky 0.7", s });
    }
    {
        TailSwitches s;
        s.keepConfidence = 0.6f;
        s.endBelowDb = -55.0f;
        m.push_back ({ "2+3 sticky 0.6 + end -55", s });
    }
    return m;
}

struct Totals
{
    int runs = 0, dropouts = 0, dropRms = 0, dropConf = 0, dropShort = 0, dropMid = 0, dropLong = 0, dips1 = 0, dips3 = 0, dips10 = 0;
    int jumpOct = 0, jumpFifth = 0, jumpSemi = 0, jumpOther = 0, falseOnsets = 0, levelJumps = 0, reopens = 0, heldChanges = 0, memoryClears = 0, chordLost = 0;
    int louder = 0;
    float maxLouder = 0.0f, maxCents = 0.0f;
    double offMs = 0.0;
    int runsWithAudible = 0;
    float maxDip = 0.0f;
    double tailEnd = 0.0, lastVoiced = 0.0;
    void add (const Result& r)
    {
        if (r.skipped)
            return;
        ++runs;
        dropouts += r.dropouts;
        dropRms += r.dropRms;
        dropConf += r.dropConf;
        dropShort += r.dropShort;
        dropMid += r.dropMid;
        dropLong += r.dropLong;
        dips1 += r.dips1;
        dips3 += r.dips3;
        dips10 += r.dips10;
        runsWithAudible += r.dips3 > 0 ? 1 : 0;
        jumpOct += r.jumpOct;
        jumpFifth += r.jumpFifth;
        jumpSemi += r.jumpSemi;
        jumpOther += r.jumpOther;
        louder += r.louder;
        maxLouder = std::max (maxLouder, r.maxLouder);
        maxCents = std::max (maxCents, r.maxCents);
        offMs += r.offMs;
        falseOnsets += r.falseOnsets;
        levelJumps += r.levelJumps;
        reopens += r.reopens;
        heldChanges += r.heldChanges;
        memoryClears += r.memoryClears;
        chordLost += r.chordLost;
        maxDip = std::max (maxDip, r.maxDip);
        tailEnd += r.tailEndS;
        lastVoiced += r.lastVoicedS;
    }
};

// Runs fn over [0, count) on every core.
void parallelFor (int count, const std::function<void (int)>& fn)
{
    const int threads = (int) std::max (1u, std::thread::hardware_concurrency());
    std::atomic<int> next { 0 };
    std::vector<std::thread> pool;
    for (int k = 0; k < threads; ++k)
        pool.emplace_back ([&] {
            for (int i; (i = next++) < count;)
                fn (i);
        });
    for (auto& t : pool)
        t.join();
}

// The real takes in Recordings/ (dry DI, mono; in the repo since batch 2). Empty if the folder isn't there.
std::vector<std::pair<juce::String, std::vector<float>>> loadRecordings()
{
    std::vector<std::pair<juce::String, std::vector<float>>> out;
    auto dir = juce::File::getCurrentWorkingDirectory().getChildFile ("Recordings");
    if (! dir.isDirectory())
        dir = outDir.getParentDirectory().getParentDirectory().getChildFile ("Recordings");
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.wav");
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
        out.push_back ({ f.getFileNameWithoutExtension().replace (" ", "_"), std::move (x) });
    }
    return out;
}

struct Inputs
{
    std::vector<Input> plain, prefixed; // kNotes x kNoiseDbs, note-major
};
const Inputs& inputs()
{
    static const Inputs all = [] {
        Inputs in;
        for (auto& ns : kNotes)
            for (float nd : kNoiseDbs)
            {
                in.plain.push_back (makeInput (ns, nd, false));
                in.prefixed.push_back (makeInput (ns, nd, true));
            }
        return in;
    }();
    return all;
}

// Every input through one setting and mitigation (notes below the window's range are skipped).
std::vector<Result> sweep (const Setting& s, const TailSwitches& sw)
{
    const auto& ins = s.prefix ? inputs().prefixed : inputs().plain;
    std::vector<Result> out (ins.size());
    parallelFor ((int) ins.size(), [&] (int i) {
        const auto& in = ins[(size_t) i];
        if (in.note < lowestReliableNote (kFs, s.p.windowHz))
            out[(size_t) i].skipped = true;
        else
            out[(size_t) i] = analyse (in, render (in, s.p, sw));
    });
    return out;
}

Totals total (const std::vector<Result>& rs)
{
    Totals t;
    for (auto& r : rs)
        t.add (r);
    return t;
}

juce::String totalsLine (const juce::String& label, const Totals& t)
{
    return label.paddedRight (' ', 34) + "runs " + juce::String (t.runs).paddedLeft (' ', 2) + " | dropouts " + juce::String (t.dropouts).paddedLeft (' ', 4)
           + " (rms " + juce::String (t.dropRms) + ", conf " + juce::String (t.dropConf) + "; 1 hop " + juce::String (t.dropShort) + ", 2-10 "
           + juce::String (t.dropMid) + ", >10 " + juce::String (t.dropLong) + ") | dips >=1dB " + juce::String (t.dips1).paddedLeft (' ', 4) + " >=3dB "
           + juce::String (t.dips3).paddedLeft (' ', 3) + " >=10dB " + juce::String (t.dips10).paddedLeft (' ', 3) + " max " + juce::String (t.maxDip, 1)
           + "dB | runs w/ audible " + juce::String (t.runsWithAudible) + " | level jumps " + juce::String (t.levelJumps) + " | back louder " + juce::String (t.louder)
           + " (max +" + juce::String (t.maxLouder, 1) + "dB) | reopen " + juce::String (t.reopens) + " | pitch oct/5th/semi/other " + juce::String (t.jumpOct) + "/"
           + juce::String (t.jumpFifth) + "/" + juce::String (t.jumpSemi) + "/" + juce::String (t.jumpOther) + ", max " + juce::String (t.maxCents, 0) + "c, >20c "
           + juce::String (t.offMs / std::max (1, t.runs), 0) + "ms/note | onsets " + juce::String (t.falseOnsets) + " | mean last voiced " + juce::String (t.runs > 0 ? t.lastVoiced / t.runs : 0.0, 2) + "s, tail end "
           + juce::String (t.runs > 0 ? t.tailEnd / t.runs : 0.0, 2) + "s";
}

// Costs of a mitigation: how long after a palm mute the output is 40 dB down (B3, E4, A4 muted at 1 s, mean),
// and how long a soft E4 (0.02 peak, -34 dBFS) sounds (to 40 dB under its loudest output).
struct Cost
{
    double muteMs = 0.0, softS = 0.0;
    double softAfterMuteMs = 0.0; // a soft G4 (0.02 peak) picked 120 ms after a muted E4 (0.5 peak): from the pick to its note sounding
    int earlySlide = 0, lateSlide = 0; // the note sounding after a slide E4 -> G4 (no pick) 0.5 s / 3 s into the note
};
Cost cost (const EngineParams& p, const TailSwitches& sw)
{
    Cost c;
    int count = 0;
    for (int midi : { 59, 64, 69 })
    {
        StringNote s;
        s.hz = hzFromMidi (midi);
        s.t60 = 8.0;
        s.seconds = 3.0;
        s.muteAt = 1.0;
        s.seed = (uint32_t) midi;
        Input in;
        in.x = stringNote (s);
        const auto noise = noiseFloor (in.x.size(), -70.0f, 5);
        for (size_t i = 0; i < in.x.size(); ++i)
            in.x[i] += noise[i];
        in.note = midi;
        const auto r = render (in, p, sw);
        const auto at = (size_t) (1.0 * kFs);
        const float ref = db (r.amp[at]);
        size_t t = at;
        for (size_t k = at; k < r.amp.size(); ++k)
            if (db (r.amp[k]) >= ref - 40.0f)
                t = k;
        c.muteMs += 1000.0 * (double) (t - at) / kFs;
        ++count;
    }
    c.muteMs /= count;
    {
        StringNote s;
        s.hz = hzFromMidi (64);
        s.t60 = 8.0;
        s.seconds = 6.0;
        s.amp = 0.02f;
        Input in;
        in.x = stringNote (s);
        const auto noise = noiseFloor (in.x.size(), -70.0f, 6);
        for (size_t i = 0; i < in.x.size(); ++i)
            in.x[i] += noise[i];
        const auto r = render (in, p, sw);
        float peak = 0.0f;
        for (auto a : r.amp)
            peak = std::max (peak, a);
        size_t t = 0;
        for (size_t k = 0; k < r.amp.size(); ++k)
            if (peak > 0.0f && db (r.amp[k]) >= db (peak) - 40.0f)
                t = k;
        c.softS = (double) t / kFs;
    }
    {
        StringNote loud;
        loud.hz = hzFromMidi (64);
        loud.t60 = 8.0;
        loud.seconds = 1.12;
        loud.muteAt = 1.0;
        loud.seed = 41;
        StringNote soft;
        soft.hz = hzFromMidi (67);
        soft.t60 = 8.0;
        soft.seconds = 1.5;
        soft.amp = 0.02f;
        soft.seed = 42;
        Input in;
        in.x = stringNote (loud);
        const auto b = stringNote (soft);
        in.x.insert (in.x.end(), b.begin(), b.end());
        const auto noise = noiseFloor (in.x.size(), -70.0f, 8);
        for (size_t i = 0; i < in.x.size(); ++i)
            in.x[i] += noise[i];
        const auto r = render (in, p, sw);
        const auto pick = (int64_t) (1.12 * kFs);
        c.softAfterMuteMs = -1.0;
        for (auto& h : r.hops)
            if (h.time >= pick && h.voiced && h.note == 67)
            {
                c.softAfterMuteMs = 1000.0 * (double) (h.time - pick) / kFs;
                break;
            }
    }
    for (double at : { 0.5, 3.0 })
    {
        Input in;
        in.x = ksPluck ([at] (double t) { return 329.63 * std::pow (2.0, 3.0 / 12.0 * std::clamp ((t - at) / 0.3, 0.0, 1.0)); }, at + 1.0, 0.5f,
                        ksLossFor (329.63, 8.0), 31);
        const auto noise = noiseFloor (in.x.size(), -70.0f, 7);
        for (size_t i = 0; i < in.x.size(); ++i)
            in.x[i] += noise[i];
        const auto r = render (in, p, sw);
        int note = 0;
        for (auto& h : r.hops)
            if (h.voiced && h.time < (int64_t) ((at + 0.6) * kFs))
                note = (int) std::lround (h.target);
        (at < 1.0 ? c.earlySlide : c.lateSlide) = note;
    }
    return c;
}

// The audio examples (Sustain 100%): the input, and the output with each listed mitigation.
void writeExamples (const std::vector<Mitigation>& ms)
{
    auto dir = outDir.getChildFile ("sputter");
    struct Example
    {
        const char* note;
        float noiseDb;
        NoteReleaseV058 release;
    };
    for (auto ex : { Example { "B3", -60.0f, NoteReleaseV058::Fast }, Example { "E4", -60.0f, NoteReleaseV058::Fast }, Example { "A4", -50.0f, NoteReleaseV058::Fast },
                     Example { "E4", -70.0f, NoteReleaseV058::Long }, Example { "B3weakfund", -70.0f, NoteReleaseV058::Fast } })
    {
        const Input* in = nullptr;
        for (auto& i : inputs().plain)
            if (i.name == juce::String (ex.note) + "-noise" + juce::String ((int) ex.noiseDb))
                in = &i;
        if (in == nullptr)
            continue;
        const juce::String base = in->name + "-sustain100-" + (ex.release == NoteReleaseV058::Fast ? "fast" : "long");
        writeWav ("sputter/" + in->name + "-input", in->x);
        for (auto& m : ms)
        {
            const auto r = render (*in, baseParams (100.0f, ex.release), m.sw, true);
            writeWav ("sputter/" + base + "-" + m.name, r.out);
        }
    }
    info ("audio examples in " + dir.getFullPathName());
}
} // namespace sputter

// The candidates (probe-sputter's short report, and the audio examples).
std::vector<sputter::Mitigation> sputterCandidates()
{
    using namespace sputter;
    std::vector<Mitigation> m;
    m.push_back ({ "before", {} });
    m.push_back ({ "fix", recommendedTail() });
    TailSwitches s;
    s.graceMs = 40.0f;
    m.push_back ({ "alt-grace40", s });
    s.keepConfidence = 0.6f;
    m.push_back ({ "alt-grace40-sticky06", s });
    s = {};
    s.graceMs = 40.0f;
    s.taperUnderRefDb = 20.0f;
    m.push_back ({ "alt-grace40-taper20", s });
    return m;
}

void probeSputter()
{
    using namespace sputter;
    const char* names[] = { "Instant", "Fast", "Medium", "Long" };
    for (auto [sus, rel] : { std::pair { 100.0f, NoteReleaseV058::Fast }, { 100.0f, NoteReleaseV058::Long }, { 100.0f, NoteReleaseV058::Instant }, { 50.0f, NoteReleaseV058::Fast },
                             { 0.0f, NoteReleaseV058::Instant } })
        for (auto& c : sputterCandidates())
            if (c.name == "before" || c.name == "fix")
            {
                const Setting s { "", baseParams (sus, rel) };
                info (totalsLine (c.name + " (S" + juce::String ((int) sus) + " " + names[(int) rel] + ")", total (sweep (s, c.sw))));
            }
}

// The real takes (Recordings/*.wav; the first batch was deleted, this is batch 2): what's played (picks, glides and
// fretted slides, vibrato, mutes and damps, found from the input's level and the measuring detector's pitch), the
// tail analysis of each picked note across Sustain and Note Release, every slide and mute checked before / with the
// fix / fix + pick restart 30 / grace 40, the values swept, per-hop CSVs and the WAVs. Part of probe-sputter-full;
// probe-sputter-recordings runs it alone.
namespace sputter
{
const juce::String kTakeDir = "sputter/recordings-2";

struct TakeEvent
{
    enum Kind { Pick, Glide, Vibrato, Mute, Damp } kind = Pick;
    int64_t from = 0, to = 0;
    double fromMidi = -1.0, toMidi = -1.0, depthCents = 0.0, rateHz = 0.0;
    float levelDbfs = 0.0f, toDbfs = 0.0f, underPeakDb = 0.0f;
    bool stepped = false; // the pitch steps at each fret (a fretted slide), not a smooth glide (a bend, a bar or a bottleneck)
};

struct Take
{
    juce::String name;
    std::vector<float> x;
    std::vector<std::pair<int64_t, float>> truth;
    std::vector<TakeEvent> events;
    std::vector<Input> notes; // one per pitched pick, to the next pick
    float floorDb = 0.0f, peakDb = 0.0f, preDb = 0.0f;
    int clipped = 0;
    juce::String hum;
};

juce::String noteName (double midi)
{
    const int n = (int) std::lround (midi);
    return juce::String (gtt::pitchClassName (n)) + juce::String (n / 12 - 1);
}

juce::String sec (int64_t t) { return juce::String ((double) t / kFs, 2); }

// What's played in a take.
Take readTake (const juce::String& name, std::vector<float> x)
{
    Take tk;
    tk.name = name;
    tk.x = std::move (x);
    const auto& xs = tk.x;
    const auto hops = measureHops (xs, kTakeDir + "/measure-" + name, 80.0);
    // The measuring detector's pitch, as the median of the voiced hops among the 5 around each (3 of them needed).
    for (size_t k = 0; k < hops.size(); ++k)
    {
        float m = -1.0f;
        if (hops[k].detector.voiced)
        {
            std::vector<double> v;
            for (size_t j = k >= 2 ? k - 2 : 0; j <= std::min (hops.size() - 1, k + 2); ++j)
                if (hops[j].detector.voiced)
                    v.push_back (hops[j].midi);
            if (v.size() >= 3)
            {
                std::sort (v.begin(), v.end());
                m = (float) v[v.size() / 2];
            }
        }
        tk.truth.push_back ({ hops[k].time, m });
    }
    auto truthIdx = [&] (int64_t t) {
        auto it = std::lower_bound (tk.truth.begin(), tk.truth.end(), std::pair<int64_t, float> { t, -2.0f });
        return (size_t) std::clamp<std::ptrdiff_t> (it - tk.truth.begin(), 0, (std::ptrdiff_t) tk.truth.size() - 1);
    };
    auto voicedFrac = [&] (int64_t a, int64_t b) {
        int v = 0, n = 0;
        for (auto& [t, m] : tk.truth)
            if (t >= a && t < b)
            {
                ++n;
                v += m >= 0.0f ? 1 : 0;
            }
        return n > 0 ? (double) v / n : 0.0;
    };

    // Levels: 10 ms RMS frames, and the same smoothed over 3 frames.
    const int64_t frame = (int64_t) (0.01 * kFs);
    std::vector<float> lv (xs.size() / (size_t) frame), sm (lv.size());
    for (size_t k = 0; k < lv.size(); ++k)
    {
        double sq = 0.0;
        for (size_t i = k * (size_t) frame; i < (k + 1) * (size_t) frame; ++i)
            sq += (double) xs[i] * xs[i];
        lv[k] = db ((float) std::sqrt (sq / (double) frame));
    }
    for (size_t k = 0; k < lv.size(); ++k)
        sm[k] = (lv[k > 0 ? k - 1 : 0] + lv[k] + lv[std::min (lv.size() - 1, k + 1)]) / 3.0f;
    auto levelAt = [&] (int64_t t) { return sm[(size_t) std::clamp<int64_t> (t / frame, 0, (int64_t) sm.size() - 1)]; };

    // Peak, clipping, the noise floor (the quietest 0.3 s that isn't digital silence) and hum in the pre-roll.
    for (auto v : xs)
    {
        tk.peakDb = std::max (tk.peakDb, std::abs (v));
        tk.clipped += std::abs (v) > 0.999f ? 1 : 0;
    }
    tk.peakDb = db (tk.peakDb);
    {
        float best = 0.0f;
        const size_t w = (size_t) (0.3 * kFs);
        for (size_t f = 0; f + w < xs.size(); f += (size_t) (0.05 * kFs))
        {
            double sq = 0.0;
            for (size_t i = f; i < f + w; ++i)
                sq += (double) xs[i] * xs[i];
            const float l = db ((float) std::sqrt (sq / (double) w));
            if (l > -150.0f && l < best)
                best = l;
        }
        tk.floorDb = best;
    }

    // Picks: a 10 ms frame above -40 dBFS, 15 dB over the quietest of the 3 before it (one per 150 ms).
    std::vector<int64_t> picks;
    for (size_t k = 3; k < lv.size(); ++k)
    {
        const float lo = std::min ({ lv[k - 1], lv[k - 2], lv[k - 3] });
        if (lv[k] > -40.0f && lv[k] >= lo + 15.0f && (picks.empty() || (int64_t) k * frame - picks.back() > (int64_t) (0.15 * kFs)))
        {
            size_t j = k - 2;
            while (j < k && lv[j] < lo + 6.0f)
                ++j;
            picks.push_back ((int64_t) j * frame);
        }
    }
    {
        double pre = 0.0;
        const size_t n = (size_t) std::max<int64_t> (frame, picks.empty() ? (int64_t) xs.size() : picks.front() - frame);
        for (size_t i = 0; i < n; ++i)
            pre += (double) xs[i] * xs[i];
        tk.preDb = db ((float) std::sqrt (pre / (double) n));
        for (double hz : { 50.0, 60.0, 120.0, 180.0 })
        {
            double re = 0.0, im = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                re += xs[i] * std::cos (2 * M_PI * hz * (double) i / kFs);
                im += xs[i] * std::sin (2 * M_PI * hz * (double) i / kFs);
            }
            tk.hum += juce::String ((int) hz) + " Hz " + juce::String (db ((float) (std::sqrt (re * re + im * im) / (double) n)), 1) + ", ";
        }
    }
    auto nearPick = [&] (int64_t t) {
        for (auto p : picks)
            if (t >= p - (int64_t) (0.01 * kFs) && t < p + (int64_t) (0.03 * kFs))
                return true;
        return false;
    };
    auto pickBefore = [&] (int64_t t) {
        int64_t p = -1;
        for (auto q : picks)
            if (q <= t)
                p = q;
        return p;
    };
    auto pickPeak = [&] (int64_t p) {
        float pk = -200.0f;
        for (int64_t t = p; t < p + (int64_t) (0.15 * kFs); t += frame)
            pk = std::max (pk, levelAt (t));
        return pk;
    };

    // Each pick, and the note it starts (the measuring detector's median 30 .. 150 ms after it; unpitched if it reads
    // nothing for most of the first 300 ms).
    for (size_t i = 0; i < picks.size(); ++i)
    {
        const int64_t p = picks[i], end = i + 1 < picks.size() ? picks[i + 1] : (int64_t) xs.size();
        TakeEvent ev;
        ev.kind = TakeEvent::Pick;
        ev.from = p;
        ev.to = end;
        ev.levelDbfs = pickPeak (p);
        {
            std::vector<float> v; // what it reads in its first 300 ms, for an unpitched pick
            for (auto& [t, m] : tk.truth)
                if (t >= p && t < std::min (end, p + (int64_t) (0.3 * kFs)) && m >= 0.0f)
                    v.push_back (m);
            ev.depthCents = 1000.0 * (double) v.size() * hopFor (kFs) / kFs; // (ms voiced)
            if (! v.empty())
            {
                std::sort (v.begin(), v.end());
                ev.toMidi = v[v.size() / 2];
            }
        }
        if (voicedFrac (p, std::min (end, p + (int64_t) (0.3 * kFs))) >= 0.2)
        {
            std::vector<float> v;
            for (int64_t a = p + (int64_t) (0.03 * kFs); v.empty() && a < end; a += (int64_t) (0.12 * kFs))
                for (auto& [t, m] : tk.truth)
                    if (t >= a && t < a + (int64_t) (0.12 * kFs) && t < end && m >= 0.0f)
                        v.push_back (m);
            if (! v.empty())
            {
                std::sort (v.begin(), v.end());
                ev.fromMidi = v[v.size() / 2];
            }
        }
        tk.events.push_back (ev);
        if (ev.fromMidi >= 0.0)
        {
            Input in;
            in.name = name + "@" + juce::String ((double) p / kFs, 2);
            in.x = xs;
            in.noteStart = (size_t) p;
            in.noteEnd = (size_t) end;
            in.note = (int) std::lround (ev.fromMidi);
            in.noiseDb = tk.floorDb;
            in.truth = tk.truth;
            tk.notes.push_back (std::move (in));
        }
    }

    // Mutes and damps: the level falling 10+ dB in 150 ms (decays here fall 2 dB at most) out of a pitched note; from
    // the steepest 20 ms out to where it falls less than 1.5 dB per 30 ms. A damp keeps ringing (pitched) after it.
    std::vector<TakeEvent> falls;
    for (size_t k = 3; k + 15 < sm.size();)
    {
        if (sm[k] > -50.0f && sm[k] - sm[k + 15] >= 10.0f)
        {
            size_t st = k;
            float best = 0.0f;
            for (size_t j = k; j < k + 15 && j + 2 < sm.size(); ++j)
                if (sm[j] - sm[j + 2] > best)
                {
                    best = sm[j] - sm[j + 2];
                    st = j;
                }
            size_t a = st, b = st + 1;
            while (a >= 3 && sm[a - 3] - sm[a] >= 1.5f)
                --a;
            while (b + 3 < sm.size() && sm[b] - sm[b + 3] >= 1.5f)
                ++b;
            const int64_t ta = (int64_t) a * frame, tb = (int64_t) b * frame;
            if (! falls.empty() && ta - falls.back().to <= (int64_t) (0.05 * kFs)) // the same fall, continued
            {
                falls.back().to = tb;
                falls.back().toDbfs = sm[b];
            }
            else if (sm[a] - sm[b] >= 8.0f && voicedFrac (ta - (int64_t) (0.1 * kFs), ta) >= 0.5)
            {
                TakeEvent ev;
                ev.from = ta;
                ev.to = tb;
                ev.levelDbfs = sm[a];
                ev.toDbfs = sm[b];
                for (size_t j = truthIdx (ta); j > 0 && tk.truth[j].first > ta - (int64_t) (0.1 * kFs); --j)
                    if (tk.truth[j].second >= 0.0f)
                    {
                        ev.fromMidi = tk.truth[j].second;
                        break;
                    }
                const int64_t p = pickBefore (ta);
                ev.underPeakDb = (p >= 0 ? pickPeak (p) : tk.peakDb) - ev.levelDbfs;
                falls.push_back (ev);
            }
            k = b + 1;
            continue;
        }
        ++k;
    }
    for (auto& ev : falls)
    {
        ev.kind = voicedFrac (ev.to, ev.to + (int64_t) (0.1 * kFs)) >= 0.5 && ev.toDbfs > tk.floorDb + 15.0f ? TakeEvent::Damp : TakeEvent::Mute;
        tk.events.push_back (ev);
    }
    auto inFall = [&] (int64_t t) {
        for (auto& f : falls)
            if (t >= f.from - (int64_t) (0.02 * kFs) && t < f.to + (int64_t) (0.1 * kFs))
                return true;
        return false;
    };

    // Glides and slides: stretches where the pitch moves 4.5+ semitones a second (over 6 hops), joined across stills
    // of up to 20 hops (fret steps), extended while it still moves 1+ semitone a second the same way, joined to the
    // next one the same way within 80 ms; 0.8 semitone or more end to end, at most 100 semitones a second (faster is
    // the detector jumping, at a mute or in the noise). Not in a pick's first 30 ms or a mute.
    const int span = 3;
    auto still = [&] (size_t k) { return tk.truth[k].second < 0.0f || nearPick (tk.truth[k].first) || inFall (tk.truth[k].first); };
    std::vector<size_t> moving;
    for (size_t k = span; k + span < tk.truth.size(); ++k)
        if (! still (k) && ! still (k - span) && ! still (k + span) && std::abs (tk.truth[k + span].second - tk.truth[k - span].second) >= 0.012f * 2 * span)
            moving.push_back (k);
    std::vector<std::pair<size_t, size_t>> stretches;
    for (size_t g = 0; g < moving.size();)
    {
        size_t h = g;
        while (h + 1 < moving.size() && moving[h + 1] - moving[h] <= 20 && pickBefore (tk.truth[moving[h + 1]].first) == pickBefore (tk.truth[moving[g]].first))
            ++h;
        size_t a = moving[g] - span, b = moving[h] + span;
        g = h + 1;
        const float dir = tk.truth[b].second >= tk.truth[a].second ? 1.0f : -1.0f;
        while (b + 6 < tk.truth.size() && ! still (b + 1) && ! still (b + 6) && (tk.truth[b + 6].second - tk.truth[b].second) * dir >= 0.016f)
            ++b;
        while (a > 6 && ! still (a - 1) && ! still (a - 6) && (tk.truth[a].second - tk.truth[a - 6].second) * dir >= 0.016f)
            --a;
        if (! stretches.empty())
        {
            auto& [pa, pb] = stretches.back();
            const bool sameWay = (tk.truth[pb].second >= tk.truth[pa].second) == (dir > 0.0f);
            if (sameWay && tk.truth[a].first - tk.truth[pb].first <= (int64_t) (0.08 * kFs) && pickBefore (tk.truth[a].first) == pickBefore (tk.truth[pb].first))
            {
                pb = std::max (pb, b);
                continue;
            }
        }
        stretches.push_back ({ a, b });
    }
    for (auto [a, b] : stretches)
    {
        int stillHops = 0, count = 0;
        for (size_t k = a + 1; k <= b; ++k)
            if (hops[k].detector.voiced && hops[k - 1].detector.voiced)
            {
                ++count;
                stillHops += std::abs (hops[k].midi - hops[k - 1].midi) < 0.006 ? 1 : 0;
            }
        TakeEvent ev;
        ev.kind = TakeEvent::Glide;
        ev.from = tk.truth[a].first;
        ev.to = tk.truth[b].first;
        ev.fromMidi = tk.truth[a].second;
        ev.toMidi = tk.truth[b].second;
        ev.levelDbfs = levelAt (ev.from);
        ev.toDbfs = levelAt (ev.to);
        const int64_t p = pickBefore (ev.from);
        ev.underPeakDb = (p >= 0 ? pickPeak (p) : tk.peakDb) - ev.levelDbfs;
        ev.stepped = count > 0 && stillHops >= 0.3 * count;
        const double dur = (double) (ev.to - ev.from) / kFs, st = std::abs (ev.toMidi - ev.fromMidi);
        if (st >= 0.8 && dur >= 0.03 && st / dur <= 100.0)
            tk.events.push_back (ev);
    }

    // Vibrato: 4+ turns of the pitch in a row (each 0.1 .. 1.5 semitones from the last, 50 .. 300 ms apart; turns found
    // with 0.06 semitone of hysteresis), outside mutes.
    {
        constexpr size_t kBreak = (size_t) -1;
        std::vector<size_t> turns;
        int dir = 0; // 0: not moving yet (ext is where it started), 1 rising, -1 falling
        size_t ext = kBreak;
        for (size_t k = 0; k < tk.truth.size(); ++k)
        {
            if (still (k))
            {
                if (ext != kBreak)
                    turns.push_back (kBreak);
                dir = 0;
                ext = kBreak;
                continue;
            }
            const float m = tk.truth[k].second;
            if (ext == kBreak)
            {
                ext = k;
                continue;
            }
            const float e = tk.truth[ext].second;
            if (dir == 0)
            {
                if (std::abs (m - e) >= 0.06f)
                {
                    dir = m > e ? 1 : -1;
                    ext = k;
                }
            }
            else if ((float) dir * (m - e) > 0.0f)
                ext = k;
            else if (std::abs (m - e) >= 0.06f)
            {
                turns.push_back (ext);
                dir = -dir;
                ext = k;
            }
        }
        std::vector<size_t> run;
        auto flush = [&] {
            if (run.size() >= 4)
            {
                float lo = 1000.0f, hi = -1000.0f;
                for (size_t k = run.front(); k <= run.back(); ++k)
                    if (tk.truth[k].second >= 0.0f)
                    {
                        lo = std::min (lo, tk.truth[k].second);
                        hi = std::max (hi, tk.truth[k].second);
                    }
                TakeEvent ev;
                ev.kind = TakeEvent::Vibrato;
                ev.from = tk.truth[run.front()].first;
                ev.to = tk.truth[run.back()].first;
                ev.fromMidi = ev.toMidi = 0.5 * (lo + hi);
                ev.depthCents = 100.0 * (hi - lo);
                ev.rateHz = (double) (run.size() - 1) / 2.0 / ((double) (ev.to - ev.from) / kFs);
                ev.levelDbfs = levelAt (ev.from);
                ev.toDbfs = levelAt (ev.to);
                const int64_t p = pickBefore (ev.from);
                ev.underPeakDb = (p >= 0 ? pickPeak (p) : tk.peakDb) - ev.levelDbfs;
                tk.events.push_back (ev);
            }
            run.clear();
        };
        for (auto t : turns)
        {
            if (t == kBreak)
            {
                flush();
                continue;
            }
            if (! run.empty())
            {
                const double dt = (double) (tk.truth[t].first - tk.truth[run.back()].first) / kFs;
                const float amp = std::abs (tk.truth[t].second - tk.truth[run.back()].second);
                if (dt < 0.05 || dt > 0.3 || amp < 0.1f || amp > 1.5f)
                    flush();
            }
            run.push_back (t);
        }
        flush();
    }
    std::sort (tk.events.begin(), tk.events.end(), [] (const TakeEvent& a, const TakeEvent& b) { return a.from < b.from; });
    return tk;
}

juce::String describe (const TakeEvent& ev)
{
    const juce::String at = sec (ev.from) + (ev.kind == TakeEvent::Pick ? juce::String() : " .. " + sec (ev.to)) + " s  ";
    switch (ev.kind)
    {
        case TakeEvent::Pick:
            return at + "pick, " + (ev.fromMidi >= 0.0 ? noteName (ev.fromMidi) + " (" + juce::String (100.0 * (ev.fromMidi - std::lround (ev.fromMidi)), 0) + " c)"
                                       : ev.toMidi >= 0.0 ? "unpitched (" + noteName (ev.toMidi) + " for " + juce::String (ev.depthCents, 0) + " ms of its first 300)"
                                                          : juce::String ("unpitched"))
                   + ", " + juce::String (ev.levelDbfs, 1) + " dBFS (10 ms RMS)";
        case TakeEvent::Glide:
        {
            const double st = ev.toMidi - ev.fromMidi, dur = (double) (ev.to - ev.from) / kFs;
            return at + (ev.stepped ? "fretted slide " : "smooth glide ") + noteName (ev.fromMidi) + " -> " + noteName (ev.toMidi) + " (" + (st > 0 ? "+" : "")
                   + juce::String (st, 2) + " st in " + juce::String (dur, 2) + " s, " + juce::String (std::abs (st) / dur, 1) + " st/s), " + juce::String (ev.levelDbfs, 1) + " -> "
                   + juce::String (ev.toDbfs, 1) + " dBFS, " + juce::String (ev.underPeakDb, 1) + " .. " + juce::String (ev.underPeakDb + ev.levelDbfs - ev.toDbfs, 1)
                   + " dB under its pick";
        }
        case TakeEvent::Vibrato:
            return at + "vibrato on " + noteName (ev.fromMidi) + ", " + juce::String (ev.depthCents, 0) + " c peak to peak, " + juce::String (ev.rateHz, 1) + " Hz, "
                   + juce::String (ev.levelDbfs, 1) + " -> " + juce::String (ev.toDbfs, 1) + " dBFS, from " + juce::String (ev.underPeakDb, 1) + " dB under its pick";
        case TakeEvent::Mute:
        case TakeEvent::Damp:
            return at + (ev.kind == TakeEvent::Mute ? "mute" : "damp (rings on)") + " of " + (ev.fromMidi >= 0.0 ? noteName (ev.fromMidi) : juce::String ("?")) + " at "
                   + juce::String (ev.levelDbfs, 1) + " dBFS (" + juce::String (ev.underPeakDb, 1) + " dB under its pick): " + juce::String (ev.toDbfs - ev.levelDbfs, 1) + " dB in "
                   + juce::String (1000.0 * (double) (ev.to - ev.from) / kFs, 0) + " ms";
    }
    return {};
}

// One event in one render (against before's render at the same setting).
struct EventCheck
{
    float heldMinDb = 0.0f;     // Sustain 100%: the level's lowest under the note's held level (reference x Dynamics) while it's played
    float vsBeforeMinDb = 0.0f; // the output's lowest against before's, while it's played
    double cappedMs = 0.0, wrongMs = 0.0, longestWrongMs = 0.0;
    int locks = 0;
    double endMs = -1.0;    // a mute: from its start to the output 40 dB under its level there (-1: not before the next pick, ...
    double untilMs = 0.0;   // ... which is this far from its start)
    float dampDb = 0.0f;    // a damp: the output's level 50 ms after it against just before it
    int64_t heldMinAt = 0;
};

EventCheck check (const Take& tk, const TakeEvent& ev, const Render& r, const Render& before, bool held)
{
    EventCheck c;
    const int64_t n = (int64_t) tk.x.size();
    const double hopMs = 1000.0 * hopFor (kFs) / kFs;
    int64_t nextPick = n, nextFall = n;
    for (auto& e : tk.events)
        if (e.from > ev.from)
        {
            if (e.kind == TakeEvent::Pick)
                nextPick = std::min (nextPick, e.from);
            else if (e.kind == TakeEvent::Mute || e.kind == TakeEvent::Damp)
                nextFall = std::min (nextFall, e.from);
        }
    if (ev.kind == TakeEvent::Pick) // an unpitched (muted) pick: from it to the output 40 dB under its loudest in its first 50 ms
    {
        const int64_t settle = std::min (nextPick, ev.from + (int64_t) (0.05 * kFs));
        float l0 = 0.0f;
        for (int64_t t = ev.from; t < settle; ++t)
            l0 = std::max (l0, r.amp[(size_t) t]);
        c.untilMs = 1000.0 * (double) (nextPick - ev.from) / kFs;
        if (l0 < 1.0e-6f)
            c.endMs = 0.0; // the voice didn't sound
        for (int64_t t = settle; t < nextPick && l0 >= 1.0e-6f; ++t)
            if (r.amp[(size_t) t] < l0 * 0.01f)
            {
                c.endMs = 1000.0 * (double) (t - ev.from) / kFs;
                break;
            }
        return c;
    }
    if (ev.kind == TakeEvent::Mute || ev.kind == TakeEvent::Damp)
    {
        float l0 = 0.0f;
        for (int64_t t = std::max<int64_t> (0, ev.from - (int64_t) (0.02 * kFs)); t <= ev.from; ++t)
            l0 = std::max (l0, r.amp[(size_t) t]);
        if (ev.kind == TakeEvent::Damp)
        {
            c.dampDb = db (r.amp[(size_t) std::min (n - 1, ev.to + (int64_t) (0.05 * kFs))]) - db (l0);
            return c;
        }
        c.untilMs = 1000.0 * (double) (nextPick - ev.from) / kFs;
        for (int64_t t = ev.from; t < nextPick; ++t)
            if (r.amp[(size_t) t] < l0 * 0.01f)
            {
                c.endMs = 1000.0 * (double) (t - ev.from) / kFs;
                break;
            }
        return c;
    }
    int64_t lastPick = -1;
    for (auto& e : tk.events)
        if (e.kind == TakeEvent::Pick && e.from <= ev.from)
            lastPick = e.from;
    const int64_t a = std::max ({ (int64_t) 0, ev.from - (int64_t) (0.01 * kFs), lastPick + (int64_t) (0.04 * kFs) }), b = std::min ({ n, ev.to + (int64_t) (0.15 * kFs), nextFall });
    int run = 0;
    for (auto& h : r.hops)
    {
        if (h.time < a || h.time >= b)
            continue;
        c.locks += h.locked ? 1 : 0;
        const int p = h.voiced ? playedNear (tk.truth, h.time, h.note) : -1;
        if (p >= 0 && p != h.note)
        {
            c.wrongMs += hopMs;
            c.longestWrongMs = std::max (c.longestWrongMs, ++run * hopMs);
        }
        else
            run = 0;
    }
    size_t ti = 0;
    for (int64_t t = a; t < b; t += 48)
    {
        while (ti + 1 < tk.truth.size() && tk.truth[ti + 1].first <= t)
            ++ti;
        const auto i = (size_t) t;
        c.cappedMs += r.capped[i] ? 1.0 : 0.0;
        if (tk.truth[ti].second < 0.0f || ! r.open[i])
            continue;
        if (held && r.ref[i] > 0.0f && db (r.level[i]) - db (r.ref[i] * r.dyn[i]) < c.heldMinDb)
        {
            c.heldMinDb = db (r.level[i]) - db (r.ref[i] * r.dyn[i]);
            c.heldMinAt = t;
        }
        if (r.amp[i] > 0.0f && before.amp[i] > 0.0f)
            c.vsBeforeMinDb = std::min (c.vsBeforeMinDb, db (r.amp[i]) - db (before.amp[i]));
    }
    return c;
}

// A whole take at Sustain 100%: how long the level is 1 dB+ under the held level while a note is played (and where).
double heldDeficitMs (const Take& tk, const Render& r, juce::String* spans = nullptr)
{
    double ms = 0.0;
    int64_t spanFrom = -1;
    float spanMin = 0.0f;
    size_t ti = 0;
    for (int64_t t = 0; t < (int64_t) tk.x.size(); t += 48)
    {
        while (ti + 1 < tk.truth.size() && tk.truth[ti + 1].first <= t)
            ++ti;
        const auto i = (size_t) t;
        const float under = r.ref[i] > 0.0f ? db (r.level[i]) - db (r.ref[i] * r.dyn[i]) : 0.0f;
        if (tk.truth[ti].second >= 0.0f && r.open[i] && r.ref[i] > 0.0f && under < -1.0f)
        {
            ms += 1.0;
            if (spanFrom < 0)
            {
                spanFrom = t;
                spanMin = 0.0f;
            }
            spanMin = std::min (spanMin, under);
        }
        else if (spanFrom >= 0)
        {
            if (spans != nullptr)
                *spans += sec (spanFrom) + " s " + juce::String ((double) (t - spanFrom) / 48.0, 0) + " ms (" + juce::String (spanMin, 1) + " dB); ";
            spanFrom = -1;
        }
    }
    return ms;
}

std::vector<std::pair<juce::String, TailSwitches>> takeCandidates()
{
    auto restart = recommendedTail();
    restart.pickRestartUnderDb = 30.0f;
    TailSwitches grace;
    grace.graceMs = 40.0f;
    return { { "before", {} }, { "fix", recommendedTail() }, { "fix+restart30", restart }, { "grace40", grace }, { "fix2", fix2Tail() } };
}

// Every take through one setting and mitigation: the per-note totals and the slide / mute summary.
struct TakeRun
{
    Totals totals;
    float glideHeldMin = 0.0f, glideVsBeforeMin = 0.0f;
    double glideWrongMs = 0.0, glideLongestMs = 0.0, glideCappedMs = 0.0, heldShortMs = 0.0, muteEndMs = 0.0, muteEndMax = 0.0;
    int glideLocks = 0, mutes = 0, ringOn = 0; // ringOn: mutes the output isn't 40 dB down from before the next pick
    float dampMinDb = 0.0f, dampMaxDb = -200.0f;
};
TakeRun runTakes (const std::vector<Take>& takes, const EngineParams& p, const TailSwitches& sw)
{
    std::vector<Render> rs (takes.size()), bs (takes.size());
    parallelFor ((int) takes.size() * 2, [&] (int i) {
        Input in;
        in.x = takes[(size_t) i / 2].x;
        (i % 2 == 0 ? rs : bs)[(size_t) i / 2] = render (in, p, i % 2 == 0 ? sw : TailSwitches {});
    });
    TakeRun out;
    const bool held = p.sustainPercent >= 100.0f;
    for (size_t k = 0; k < takes.size(); ++k)
    {
        for (auto& in : takes[k].notes)
            out.totals.add (analyse (in, rs[k]));
        if (held)
            out.heldShortMs += heldDeficitMs (takes[k], rs[k]);
        for (auto& ev : takes[k].events)
        {
            if (ev.kind == TakeEvent::Pick && ev.fromMidi >= 0.0)
                continue;
            const auto c = check (takes[k], ev, rs[k], bs[k], held);
            if (ev.kind == TakeEvent::Damp)
            {
                out.dampMinDb = std::min (out.dampMinDb, c.dampDb);
                out.dampMaxDb = std::max (out.dampMaxDb, c.dampDb);
                continue;
            }
            if (ev.kind == TakeEvent::Mute || ev.kind == TakeEvent::Pick)
            {
                if (c.endMs < 0.0)
                {
                    ++out.ringOn;
                    continue;
                }
                ++out.mutes;
                out.muteEndMs += c.endMs;
                out.muteEndMax = std::max (out.muteEndMax, c.endMs);
                continue;
            }
            out.glideHeldMin = std::min (out.glideHeldMin, c.heldMinDb);
            out.glideVsBeforeMin = std::min (out.glideVsBeforeMin, c.vsBeforeMinDb);
            out.glideWrongMs += c.wrongMs;
            out.glideLongestMs = std::max (out.glideLongestMs, c.longestWrongMs);
            out.glideCappedMs += c.cappedMs;
            out.glideLocks += c.locks;
        }
    }
    out.muteEndMs /= std::max (1, out.mutes);
    return out;
}

juce::String takeRunLine (const juce::String& label, const TakeRun& t, bool held)
{
    return totalsLine (label, t.totals) + " || slides+vibrato: " + (held ? "held min " + juce::String (t.glideHeldMin, 1) + " dB, " : juce::String()) + "vs before min "
           + juce::String (t.glideVsBeforeMin, 1) + " dB, capped " + juce::String (t.glideCappedMs, 0) + " ms, wrong note " + juce::String (t.glideWrongMs, 0) + " ms (longest "
           + juce::String (t.glideLongestMs, 0) + "), locks " + juce::String (t.glideLocks) + (held ? " | 1 dB+ under held while played " + juce::String (t.heldShortMs, 0) + " ms" : juce::String())
           + " | mutes: end " + juce::String (t.muteEndMs, 0) + " ms mean, " + juce::String (t.muteEndMax, 0) + " max, " + juce::String (t.ringOn)
           + " ring on to the next pick | damps: output " + juce::String (t.dampMaxDb, 1) + " .. " + juce::String (t.dampMinDb, 1) + " dB";
}
} // namespace sputter

void sputterRecordings()
{
    using namespace sputter;
    const auto loaded = loadRecordings();
    if (loaded.empty())
    {
        info ("no Recordings/*.wav at 48 kHz: the real-take section is skipped");
        return;
    }
    outDir.getChildFile (kTakeDir).createDirectory();
    const juce::String rn[] = { "Instant", "Fast", "Medium", "Long" };
    std::vector<Take> takes (loaded.size());
    parallelFor ((int) loaded.size(), [&] (int i) { takes[(size_t) i] = readTake (loaded[(size_t) i].first, loaded[(size_t) i].second); });
    const auto cands = takeCandidates();

    // 1. What's played.
    info ("== R1. What's played (picks and mutes from the input's level, slides and vibrato from the measuring detector, 80 Hz window) ==");
    for (auto& tk : takes)
    {
        info (tk.name + ": " + juce::String ((double) tk.x.size() / kFs, 2) + " s, peak " + juce::String (tk.peakDb, 1) + " dBFS (" + juce::String (tk.clipped)
              + " samples at full scale), pre-roll " + juce::String (tk.preDb, 1) + " dBFS RMS, quietest 0.3 s " + juce::String (tk.floorDb, 1) + " dBFS RMS; hum in the pre-roll (dBFS): " + tk.hum);
        for (auto& ev : tk.events)
            info ("    " + describe (ev));
    }

    // 2. Each picked note through the engine (Sustain 100%, Fast): the tail analysis per candidate, and before's events.
    info ("== R2. Each picked note (Sustain 100%, Fast, default window): dropouts, dips, pitch against what's played ==");
    for (auto& tk : takes)
    {
        Input whole;
        whole.name = tk.name;
        whole.x = tk.x;
        for (auto& [label, sw] : cands)
        {
            const auto r = render (whole, baseParams(), sw, true);
            writeHopCsv (kTakeDir.fromFirstOccurrenceOf ("/", false, false) + "/hops-" + tk.name + "-" + label, whole, r, baseParams());
            for (auto& in : tk.notes)
            {
                const auto res = analyse (in, r);
                info (in.name + " " + noteName (in.note) + " " + label + ": " + juce::String (res.dropouts) + " dropouts (rms " + juce::String (res.dropRms) + ", conf "
                      + juce::String (res.dropConf) + "; 1 hop " + juce::String (res.dropShort) + ", 2-10 " + juce::String (res.dropMid) + ", >10 " + juce::String (res.dropLong)
                      + "), audible dips " + juce::String (res.dips3) + " (max " + juce::String (res.maxDip, 1) + " dB), back louder " + juce::String (res.louder) + " (+"
                      + juce::String (res.maxLouder, 1) + " dB), reopen " + juce::String (res.reopens) + ", level jumps " + juce::String (res.levelJumps)
                      + ", off the played note oct/5th/semi/other " + juce::String (res.jumpOct) + "/" + juce::String (res.jumpFifth) + "/" + juce::String (res.jumpSemi) + "/"
                      + juce::String (res.jumpOther) + ", T >20 c off " + juce::String (res.offMs, 0) + " ms, onsets " + juce::String (res.falseOnsets) + ", last voiced "
                      + juce::String (res.lastVoicedS, 2) + " s " + juce::String (res.lastVoicedDbfs, 1) + " dBFS (" + juce::String (res.lastVoicedBelowDb, 1)
                      + " under the peak), tail end " + juce::String (res.tailEndS, 2) + " s");
                if (label != "before")
                    continue;
                const int64_t note0 = (int64_t) in.noteStart, end = (int64_t) in.noteEnd;
                float peakRms = 0.0f;
                for (auto& h : r.hops)
                    if (h.time >= note0 && h.time < end)
                        peakRms = std::max (peakRms, h.rms);
                juce::String line;
                int lastNote = -1, shown = 0;
                for (size_t k = 1; k < r.hops.size(); ++k)
                {
                    const auto& h = r.hops[k];
                    if (h.time < note0 + (int64_t) (kTailFromS * kFs) || h.time >= end)
                        continue;
                    if (! h.voiced && r.hops[k - 1].voiced)
                    {
                        size_t e = k;
                        bool rmsCause = false;
                        for (; e < r.hops.size() && ! r.hops[e].voiced && r.hops[e].time < end; ++e)
                            rmsCause = rmsCause || r.hops[e].rms <= kVoicedRmsFloor;
                        if (e < r.hops.size() && r.hops[e].time < end && r.hops[e].voiced)
                        {
                            if (shown++ < 40)
                                line += "drop " + sec (h.time) + "s " + juce::String ((int) (e - k)) + "h " + juce::String (db (r.hops[k - 1].rms), 0) + "dBFS/"
                                        + juce::String (db (peakRms) - db (r.hops[k - 1].rms), 0) + (rmsCause ? " rms" : "") + "; ";
                        }
                        else
                            line += "END " + sec (h.time) + "s " + juce::String (db (r.hops[k - 1].rms), 0) + "dBFS; ";
                    }
                    if (h.voiced && h.note != lastNote)
                    {
                        if (lastNote >= 0)
                            line += "NOTE " + sec (h.time) + "s " + noteName (lastNote) + "->" + noteName (h.note) + "; ";
                        lastNote = h.note;
                    }
                }
                info ("    " + line);
            }
        }
    }

    // 3. Across Sustain and Note Release (default window), totals over the notes, and the slides and mutes.
    info ("== R3. Across Sustain and Note Release (totals over the picked notes; slides, vibrato and mutes) ==");
    for (float sus : { 0.0f, 50.0f, 100.0f })
        for (auto rel : { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Medium, NoteReleaseV058::Long })
            for (auto& [label, sw] : cands)
                info (takeRunLine ("S" + juce::String ((int) sus) + " " + rn[(int) rel] + " " + label, runTakes (takes, baseParams (sus, rel), sw), sus >= 100.0f));

    // 4. Every slide, vibrato, mute and muted pick, per candidate (Sustain 100% at each Note Release; Sustain 50% and 0%, Fast).
    info ("== R4. Each slide, vibrato and mute: held = the level's lowest under the held level while played (Sustain 100%), vs before = the output's lowest "
          "against before's, capped = one-way cap on, wrong = the voice's note off what's played (longest run), locks = hops the pitch lock held; mutes: "
          "from the mute's start to the output 40 dB down; muted (unpitched) picks: from the pick to the output 40 dB under its first 50 ms ==");
    struct Run
    {
        juce::String name;
        EngineParams p;
    };
    std::vector<Run> runs;
    for (auto rel : { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Medium, NoteReleaseV058::Long })
        runs.push_back ({ "S100 " + rn[(int) rel], baseParams (100.0f, rel) });
    runs.push_back ({ "S50 Fast", baseParams (50.0f) });
    runs.push_back ({ "S0 Fast", baseParams (0.0f) });
    runs.push_back ({ "S0 Instant", baseParams (0.0f, NoteReleaseV058::Instant) });
    for (auto& tk : takes)
    {
        Input whole;
        whole.x = tk.x;
        std::vector<Render> rr (runs.size() * cands.size());
        parallelFor ((int) rr.size(), [&] (int i) { rr[(size_t) i] = render (whole, runs[(size_t) i / cands.size()].p, cands[(size_t) i % cands.size()].second); });
        info ("-- " + tk.name);
        for (size_t sI = 0; sI < runs.size(); ++sI)
            if (runs[sI].p.sustainPercent >= 100.0f)
                for (size_t c = 0; c < cands.size(); ++c)
                {
                    juce::String spans;
                    heldDeficitMs (tk, rr[sI * cands.size() + c], &spans);
                    if (spans.isNotEmpty())
                        info ("  " + runs[sI].name + " " + cands[c].first + ": 1 dB+ under the held level while played at " + spans);
                }
        for (auto& ev : tk.events)
        {
            if (ev.kind == TakeEvent::Pick && ev.fromMidi >= 0.0)
                continue;
            info ("  " + describe (ev));
            for (size_t s = 0; s < runs.size(); ++s)
            {
                const bool held = runs[s].p.sustainPercent >= 100.0f;
                juce::String line = "    " + runs[s].name.paddedRight (' ', 12);
                for (size_t c = 0; c < cands.size(); ++c)
                {
                    const auto k = check (tk, ev, rr[s * cands.size() + c], rr[s * cands.size()], held);
                    line += cands[c].first + ": ";
                    if (ev.kind == TakeEvent::Damp)
                        line += "output " + juce::String (k.dampDb, 1) + " dB; ";
                    else if (ev.kind == TakeEvent::Mute || ev.kind == TakeEvent::Pick)
                        line += (k.endMs < 0.0 ? "rings on " + juce::String (k.untilMs, 0) + " ms to the next pick" : "end " + juce::String (k.endMs, 0) + " ms") + "; ";
                    else
                        line += (held ? "held " + juce::String (k.heldMinDb, 1) + (k.heldMinDb < -1.0f ? " at " + juce::String ((double) k.heldMinAt / kFs, 3) + " s" : juce::String()) + ", " : juce::String()) + "vs before " + juce::String (k.vsBeforeMinDb, 1) + " dB, capped "
                                + juce::String (k.cappedMs, 0) + " ms, wrong " + juce::String (k.wrongMs, 0) + " (" + juce::String (k.longestWrongMs, 0) + ") ms, locks "
                                + juce::String (k.locks) + "; ";
                }
                info (line);
            }
        }
    }

    // 5. The values, one at a time from the recommendation (Sustain 100%, Fast, Instant and Long).
    info ("== R5. The values (Sustain 100%; each line changes one value of the recommendation) ==");
    std::vector<std::pair<juce::String, TailSwitches>> variants;
    variants.push_back ({ "before", {} });
    variants.push_back ({ "recommended", recommendedTail() });
    for (float g : { 0.0f, 20.0f, 30.0f, 60.0f })
    {
        auto t = recommendedTail();
        t.graceMs = g;
        variants.push_back ({ "grace " + juce::String ((int) g), t });
    }
    for (float u : { 10.0f, 30.0f, 40.0f, -1.0f })
    {
        auto t = recommendedTail();
        t.oneWayUnderDb = u;
        variants.push_back ({ u < 0.0f ? juce::String ("no one-way") : "one-way " + juce::String ((int) u), t });
    }
    for (float u : { 10.0f, 30.0f, 0.0f })
    {
        auto t = recommendedTail();
        t.lockUnderRefDb = u;
        variants.push_back ({ u > 0.0f ? "lock " + juce::String ((int) u) : juce::String ("no lock"), t });
    }
    for (float u : { 20.0f, 30.0f, 40.0f })
    {
        auto t = recommendedTail();
        t.pickRestartUnderDb = u;
        variants.push_back ({ "+ pick restarts, " + juce::String ((int) u) + " dB", t });
    }
    for (float v : { 3.0f, 10.0f, 0.0f })
    {
        auto t = recommendedTail();
        t.reviveRiseDb = v;
        variants.push_back ({ v > 0.0f ? "revive " + juce::String ((int) v) : juce::String ("no revive guard"), t });
    }
    {
        auto t = recommendedTail();
        t.lockOnPitch = true;
        variants.push_back ({ "+ lock on pitch", t });
    }
    for (float m : { 12.0f, 15.0f, 20.0f, 30.0f })
    {
        auto t = recommendedTail();
        t.muteDropDb = m;
        variants.push_back ({ "+ mute " + juce::String ((int) m), t });
    }
    {
        auto t = recommendedTail();
        t.edgeLagUnvoiced = true;
        variants.push_back ({ "+ edge lag unvoiced", t });
    }
    variants.push_back ({ "fix2", fix2Tail() });
    {
        auto t = fix2Tail();
        t.muteSkipsGrace = false;
        variants.push_back ({ "fix2, the grace kept at mutes", t });
    }
    {
        auto t = fix2Tail();
        t.edgeLagUnvoiced = false;
        variants.push_back ({ "fix2 without the edge lag", t });
    }
    {
        auto t = fix2Tail();
        t.muteWakeRiseDb = 0.0f;
        variants.push_back ({ "fix2, no rise wake", t });
    }
    for (float w : { 15.0f, 35.0f })
    {
        auto t = fix2Tail();
        t.muteWakeUnderRefDb = w;
        variants.push_back ({ "fix2, wake " + juce::String ((int) w), t });
    }
    for (float w : { 20.0f, 30.0f })
    {
        auto t = fix2Tail();
        t.muteWakeRiseDb = w;
        variants.push_back ({ "fix2, wake on a " + juce::String ((int) w) + " dB rise", t });
    }
    {
        auto t = fix2Tail();
        t.pickRestartUnderDb = 30.0f;
        variants.push_back ({ "fix2 + pick restarts, 30 dB", t });
    }
    for (auto rel : { NoteReleaseV058::Fast, NoteReleaseV058::Instant, NoteReleaseV058::Long })
    {
        info ("-- S100 " + rn[(int) rel]);
        for (auto& [label, sw] : variants)
        {
            const auto c = cost (baseParams (100.0f, rel), sw);
            info (takeRunLine (label, runTakes (takes, baseParams (100.0f, rel), sw), true) + " || synthetic: palm mute " + juce::String (c.muteMs, 0)
                  + " ms, soft G4 after a mute sounds " + juce::String (c.softAfterMuteMs, 0) + " ms after its pick, slide E4->G4 at 0.5 s / 3 s ends on "
                  + noteName (c.earlySlide) + " / " + noteName (c.lateSlide));
        }
    }

    // 6. Where each note's tail zone starts (the follower 10 .. 50 dB under the note's reference) against its first dropout.
    info ("== R6. Where the tail zone starts against the first dropout (S100 Fast, before) ==");
    for (auto& tk : takes)
    {
        Input whole;
        whole.x = tk.x;
        const auto r = render (whole, baseParams(), {});
        for (auto& in : tk.notes)
        {
            const auto res = analyse (in, r);
            juce::String line = in.name + " " + noteName (in.note) + ": first dropout " + juce::String (res.firstDropS, 2) + " s; follower under the reference by ";
            for (float u : { 10.0f, 20.0f, 30.0f, 40.0f, 50.0f })
            {
                double at = -1.0;
                for (size_t t = in.noteStart + (size_t) (0.05 * kFs); t < in.noteEnd; ++t)
                    if (r.ref[t] > 0.0f && r.env[t] < r.ref[t] * dbToGain (-u))
                    {
                        at = (double) (t - in.noteStart) / kFs;
                        break;
                    }
                line += juce::String ((int) u) + " dB at " + (at < 0.0 ? juce::String ("never") : juce::String (at, 2) + " s") + ", ";
            }
            info (line);
        }
    }

    // 7. The WAVs: Sustain 100%, Fast.
    for (auto& tk : takes)
    {
        Input whole;
        whole.x = tk.x;
        for (auto& [label, sw] : cands)
            writeWav (kTakeDir + "/" + tk.name + "-sustain100-fast-" + juce::String (label).replace ("+", "-"), render (whole, baseParams(), sw, true).out);
    }
    info ("recording WAVs in " + outDir.getChildFile (kTakeDir).getFullPathName());
}

void probeSputterRecordings() { sputterRecordings(); }

// ---------------------------------------------------------------------------
// v0.5.5: the sputter fix as behaviour. Its tests: v054-identity, sputter, slides, edge-rule (and instant-grace, chugs).
// ---------------------------------------------------------------------------

// The real takes (Recordings/*.wav), read once: what's played in each (picks, slides, mutes).
const std::vector<sputter::Take>& realTakes()
{
    static const std::vector<sputter::Take> takes = [] {
        using namespace sputter;
        outDir.getChildFile (kTakeDir).createDirectory();
        const auto loaded = loadRecordings();
        std::vector<Take> t (loaded.size());
        parallelFor ((int) loaded.size(), [&] (int i) { t[(size_t) i] = readTake (loaded[(size_t) i].first, loaded[(size_t) i].second); });
        return t;
    }();
    return takes;
}

const char* noteReleaseName (NoteReleaseV058 r)
{
    static const char* const names[] = { "Instant", "Fast", "Medium", "Long", "Extra Long" };
    return names[(int) r];
}

// The Freeze clear's risk (v0.5.6: 150 ms, was 250): a slow move between notes while building a chord can clear it.
// A run at Freeze 3 records each clear (memory holding notes, then none: VoiceProbe::memoryHeld) and the voice's pitch
// gaps, with the clear at 150 ms (v0.5.6) and at 250 ms (v0.5.5, Engine::setMemoryClearMs).
struct ClearRun
{
    std::vector<int64_t> clears; // input time (samples): the voice's sample less the latency
    std::vector<std::pair<int64_t, int64_t>> gaps; // input time where pitch was lost, length (samples); between voiced samples
    std::vector<uint8_t> held;                      // memoryHeld per input sample (from the latency on)
    int heldAt (int64_t t) const { return t >= 0 && t < (int64_t) held.size() ? held[(size_t) t] : 0; }
};
ClearRun runClears (const std::vector<float>& in, const EngineParams& p, double clearMs)
{
    ClearRun r;
    Engine e;
    e.setMemoryClearMs (clearMs);
    e.prepare (kFs, 64);
    int64_t n = 0, lastVoiced = -1;
    int prevHeld = 0;
    e.voiceListener = [&] (const VoiceProbe& v) {
        const int64_t L = e.currentLatencySamples();
        if (v.voiced)
        {
            if (lastVoiced >= 0 && n - lastVoiced > 1)
                r.gaps.push_back ({ lastVoiced + 1 - L, n - lastVoiced - 1 });
            lastVoiced = n;
        }
        if (prevHeld > 0 && v.memoryHeld == 0)
            r.clears.push_back (n - L);
        prevHeld = v.memoryHeld;
        if (n >= L)
            r.held.push_back ((uint8_t) v.memoryHeld);
        ++n;
    };
    runEngine (e, in, p);
    return r;
}

// A synthetic arpeggio: plucked segments (Karplus-Strong, 0.5 peak), each with its pitches (a slide glides into the
// next over 50 ms) and an end (a 10 ms fade: the next pick, a lift or a damp; or rings on), over a -70 dB floor.
struct ArpSegment
{
    double start = 0.0, end = 0.0;
    std::vector<std::pair<int, double>> pitches; // MIDI, from (s, absolute)
    bool rings = false;                          // not damped: rings on to the phrase's end
};
struct Arpeggio
{
    std::vector<float> x;
    std::vector<int64_t> picks;
    int64_t lastNote = 0; // where the last note starts (a pick, or a slide's arrival)
};
Arpeggio renderArpeggio (const std::vector<ArpSegment>& segs, double length, uint32_t seed)
{
    Arpeggio a;
    a.x.assign ((size_t) (length * kFs), 0.0f);
    for (const auto& sg : segs)
    {
        const auto pitches = sg.pitches;
        const double t0 = sg.start;
        auto hzAt = [pitches, t0] (double t) {
            const double at = t0 + t;
            double m = pitches[0].first;
            for (size_t k = 1; k < pitches.size(); ++k)
            {
                const double from = pitches[k].second - 0.05;
                if (at >= pitches[k].second)
                    m = pitches[k].first;
                else if (at > from)
                    m = pitches[k - 1].first + (pitches[k].first - pitches[k - 1].first) * (at - from) / 0.05;
            }
            return hzFromMidi (m);
        };
        const double end = sg.rings ? length : sg.end;
        auto y = ksPluck (hzAt, end - sg.start, 0.5f, ksLossFor (hzFromMidi (pitches[0].first), 3.0), seed++, kFs);
        if (! sg.rings)
        {
            const size_t fade = (size_t) (0.01 * kFs);
            for (size_t k = 0; k < fade && k < y.size(); ++k)
                y[y.size() - 1 - k] *= (float) k / (float) fade;
        }
        const auto at = (size_t) (sg.start * kFs);
        for (size_t k = 0; k < y.size() && at + k < a.x.size(); ++k)
            a.x[at + k] += y[k];
        a.picks.push_back ((int64_t) at);
        a.lastNote = std::max (a.lastNote, (int64_t) (pitches.back().second * kFs));
    }
    const auto noise = sputter::noiseFloor (a.x.size(), -70.0f, seed);
    for (size_t k = 0; k < a.x.size(); ++k)
        a.x[k] += noise[k];
    return a;
}

// The arpeggio phrases: G major up from B3 (B3 D4 G4 B4 D5 G5) in 8th notes, picked or slid.
std::vector<std::pair<juce::String, std::vector<ArpSegment>>> arpeggios (double bpm)
{
    const double e = 30.0 / bpm; // an 8th
    const int notes[6] = { 59, 62, 67, 71, 74, 79 };
    std::vector<std::pair<juce::String, std::vector<ArpSegment>>> out;
    auto picked = [&] (std::function<double (int)> endOf, bool rings) {
        std::vector<ArpSegment> v;
        for (int k = 0; k < 6; ++k)
            v.push_back ({ k * e, endOf (k), { { notes[k], k * e } }, rings && k < 5 });
        return v;
    };
    out.push_back ({ "picked, each note stopped by the next pick", picked ([e] (int k) { return (k + 1) * e + (k == 5 ? 3 * e : 0.0); }, false) });
    out.push_back ({ "picked across strings, every note ringing on", picked ([] (int) { return 0.0; }, true) });
    out.push_back ({ "picked across strings, each lifted 30 ms before the next", picked ([e] (int k) { return (k + 1) * e - (k == 5 ? -3 * e : 0.03); }, false) });
    out.push_back ({ "position shift after the 3rd note (lifted an 8th / 3 early)", picked ([e] (int k) { return (k + 1) * e - (k == 2 ? e / 3.0 : k == 5 ? -3 * e : 0.0); }, false) });
    out.push_back ({ "slow position shift after the 3rd note (lifted an 8th / 2 early)", picked ([e] (int k) { return (k + 1) * e - (k == 2 ? e / 2.0 : k == 5 ? -3 * e : 0.0); }, false) });
    auto slid = [&] (double lift) {
        std::vector<ArpSegment> v;
        for (int k = 0; k < 6; k += 2)
            v.push_back ({ k * e, (k + 2) * e - (k == 4 ? -3 * e : lift), { { notes[k], k * e }, { notes[k + 1], (k + 1) * e } }, false });
        return v;
    };
    out.push_back ({ "slid: a pick, a slide to the next note, a pick ...", slid (0.0) });
    out.push_back ({ "slid, the slide re-placed between pairs (lifted an 8th / 3)", slid (e / 3.0) });
    {
        // (One pick slid through all six dies in the synthesis: a Karplus-Strong string slid up two octaves loses its
        // energy within about 1.5 s, which is the synthesis, not the clear. A bottleneck player re-picks too.)
        std::vector<ArpSegment> v (2);
        for (int g = 0; g < 2; ++g)
        {
            v[(size_t) g].start = 3 * g * e;
            v[(size_t) g].end = g == 0 ? 3 * e : 9.0 * e;
            for (int k = 3 * g; k < 3 * g + 3; ++k)
                v[(size_t) g].pitches.push_back ({ notes[k], k * e });
        }
        out.push_back ({ "two picks, each slid through three notes", v });
    }
    return out;
}

void testGaps()
{
    const NoteReleaseV058 rels[] = { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Medium, NoteReleaseV058::Long, NoteReleaseV058::ExtraLong };
    auto params = [] (NoteReleaseV058 rel) {
        auto p = chordChoiceParams (freezeChoice (3));
        p.holdMs = noteReleaseMsFor (rel);
        return p;
    };
    auto ms = [] (int64_t n) { return juce::String (1000.0 * (double) n / kFs, 0); };
    int gentleLost = 0;
    double longestKept = 0.0;
    juce::String gentleText;
    for (double bpm : { 60.0, 90.0, 120.0 })
    {
        int idx = 0;
        for (auto& [name, segs] : arpeggios (bpm))
        {
            const bool gentle = idx == 0 || idx == 2 || idx == 5 || idx == 7;
            ++idx;
            const auto a = renderArpeggio (segs, 9.0 * 30.0 / bpm + 0.8, 500 + (uint32_t) idx); // the last note ends 3 8ths after its start
            std::vector<std::future<std::pair<ClearRun, ClearRun>>> jobs;
            for (auto rel : rels)
                jobs.push_back (std::async (std::launch::async, [&a, rel, &params] { return std::pair { runClears (a.x, params (rel), kMemoryClearMs), runClears (a.x, params (rel), 250.0) }; }));
            juce::String lost150, lost250, heldText;
            int64_t longest = 0;
            bool built = true, kept = true; // built: in memory at the last note at 250 ms (2 or more held); kept: never cleared before it at 150 ms
            for (size_t r = 0; r < jobs.size(); ++r)
            {
                const auto [now, old] = jobs[r].get();
                // The chord at the last note (150 ms in): Freeze 3 holds the 2 notes before it.
                const auto end = a.lastNote + (int64_t) (0.15 * kFs);
                built = built && old.heldAt (end) >= 2;
                kept = kept && now.heldAt (end) >= 2;
                heldText << now.heldAt (end) << "/" << old.heldAt (end) << " ";
                for (auto [from, len] : now.gaps)
                    if (from < a.lastNote)
                        longest = std::max (longest, len);
                auto before = [&a] (const ClearRun& c) {
                    std::vector<int64_t> v;
                    for (auto t : c.clears)
                        if (t < a.lastNote)
                            v.push_back (t);
                    return v;
                };
                const auto l150 = before (now), l250 = before (old);
                kept = kept && l150.empty();
                for (auto t : l150)
                    if (std::find_if (l250.begin(), l250.end(), [t] (int64_t u) { return std::abs (u - t) < (int64_t) (0.12 * kFs); }) == l250.end())
                    {
                        int64_t gap = 0;
                        for (auto [from, len] : now.gaps)
                            if (from <= t && from + len >= t)
                                gap = len;
                        lost150 << noteReleaseName (rels[r]) << " " << juce::String ((double) t / kFs, 2) << " s (gap " << (gap > 0 ? ms (gap) + " ms" : juce::String ("to the next pick")) << ") ";
                        if (gentle)
                            ++gentleLost;
                    }
                for (auto t : l250)
                {
                    int64_t gap = -1;
                    for (auto [from, len] : old.gaps)
                        if (from <= t && from + len >= t)
                            gap = len;
                    lost250 << noteReleaseName (rels[r]) << " " << juce::String ((double) t / kFs, 2) << " s (" << (gap > 0 ? "gap " + ms (gap) + " ms" : juce::String ("pitch never back")) << ") ";
                }
            }
            if (kept)
                longestKept = std::max (longestKept, 1000.0 * (double) longest / kFs);
            const auto line = juce::String::formatted ("%3.0f bpm, ", bpm) + name + ": longest pitch gap before the last note " + ms (longest) + " ms; notes held at the last note, 150 / 250 ms (I F M L XL): "
                              + heldText.trimEnd() + (built ? "" : " (not built even at 250 ms)") + "; lost at 150 ms only: " + (lost150.isEmpty() ? juce::String ("none") : lost150.trimEnd())
                              + "; lost at both: " + (lost250.isEmpty() ? juce::String ("none") : lost250.trimEnd());
            info (line);
            if (gentle && lost150.isNotEmpty())
                gentleText << "\n      " << line;
        }
    }
    check (gentleLost == 0, juce::String::formatted ("Arpeggios with no lift over 30 ms (each note stopped by the next pick or lifted 30 ms early, slides) at 60 / 90 / 120 bpm keep the chord "
                                                     "at 150 ms at every Note Release (lost %d times); the longest pitch gap in a phrase that built and kept its chord %.0f ms", gentleLost, longestKept)
                                + gentleText);

    // The real takes: every pitch gap of 60 ms or more, what came before it (the last event), and the chords lost at 150 ms only.
    using namespace sputter;
    for (auto& tk : realTakes())
    {
        std::vector<std::future<std::pair<ClearRun, ClearRun>>> jobs;
        for (auto rel : rels)
            jobs.push_back (std::async (std::launch::async, [&tk, rel, &params] { return std::pair { runClears (tk.x, params (rel), kMemoryClearMs), runClears (tk.x, params (rel), 250.0) }; }));
        juce::String gapsText, lostText;
        std::vector<std::pair<ClearRun, ClearRun>> res;
        for (auto& j : jobs)
            res.push_back (j.get());
        auto eventBefore = [&tk] (int64_t t) {
            const TakeEvent* best = nullptr;
            for (auto& ev : tk.events)
                if (ev.from <= t + (int64_t) (0.03 * kFs) && (best == nullptr || ev.from > best->from))
                    best = &ev;
            static const char* const kinds[] = { "pick", "glide", "vibrato", "mute", "damp" };
            return best == nullptr ? juce::String ("start") : juce::String (kinds[(int) best->kind]) + " @" + juce::String ((double) best->from / kFs, 2);
        };
        for (auto [at, len] : res[1].first.gaps) // Fast (the default)
            if (len >= (int64_t) (0.06 * kFs))
                gapsText << juce::String ((double) at / kFs, 2) << " s " << ms (len) << " ms (after " << eventBefore (at) << "); ";
        for (size_t r = 0; r < res.size(); ++r)
            for (auto t : res[r].first.clears)
                if (std::find_if (res[r].second.clears.begin(), res[r].second.clears.end(), [t] (int64_t u) { return std::abs (u - t) < (int64_t) (0.12 * kFs); }) == res[r].second.clears.end())
                    lostText << noteReleaseName (rels[r]) << " " << juce::String ((double) t / kFs, 2) << " s (after " << eventBefore (t - (int64_t) msToSamples (kFs, kMemoryClearMs)) << "); ";
        info (tk.name.substring (0, 44) + "...: pitch gaps of 60 ms or more (Fast): " + (gapsText.isEmpty() ? juce::String ("none") : gapsText.trimEnd())
              + " | cleared at 150 ms but not at 250: " + (lostText.isEmpty() ? juce::String ("none") : lostText.trimEnd()));
    }
}

// Chugs (v0.5.5): palm-muted picks on one note at 8th notes. Each is a string note muted from muteAt (every partial then
// dies in 80 ms), with a one-pole low-pass at 1.2 kHz for the damped highs, over a -70 dBFS noise floor. picks: every
// pick's sample (the open notes' too, in the phrase).
struct ChugInput
{
    std::vector<float> x;
    std::vector<int64_t> picks;
};
ChugInput chugs (double bpm, const std::vector<std::pair<int, float>>& notes, double muteAt, uint32_t seed)
{
    using namespace sputter;
    const double eighth = 30.0 / bpm;
    ChugInput c;
    std::vector<std::pair<double, std::vector<float>>> parts; // start (s), samples
    double t = 0.3;
    for (auto [midi, amp] : notes)
    {
        StringNote sn;
        sn.hz = hzFromMidi (std::abs (midi));
        sn.amp = amp;
        sn.t60 = 4.0;
        sn.seed = seed++;
        if (midi > 0) // a chug
        {
            sn.seconds = 0.6;
            sn.muteAt = muteAt;
            auto x = stringNote (sn);
            double lp = 0.0;
            const double a = 1.0 - std::exp (-2.0 * M_PI * 1200.0 / kFs);
            for (auto& v : x)
                v = (float) (lp += a * (v - lp));
            parts.push_back ({ t, x });
            t += eighth;
        }
        else // an open note, ringing four 8ths, then the next pick
        {
            sn.seconds = 4.0 * eighth;
            sn.fadeOutAt = 4.0 * eighth - 0.012;
            parts.push_back ({ t, stringNote (sn) });
            t += 4.0 * eighth;
        }
    }
    c.x.assign ((size_t) ((t + 1.0) * kFs), 0.0f);
    for (auto& [start, x] : parts)
    {
        const auto at = (size_t) (start * kFs);
        c.picks.push_back ((int64_t) at);
        for (size_t i = 0; i < x.size() && at + i < c.x.size(); ++i)
            c.x[at + i] += x[i];
    }
    const auto noise = noiseFloor (c.x.size(), -70.0f, seed);
    for (size_t i = 0; i < c.x.size(); ++i)
        c.x[i] += noise[i];
    return c;
}

// Each pick against v0.5.4: sounded (a voiced hop within 60 ms of it, and the output in its first 50 ms no more than 6 dB
// under v0.5.4's) wherever v0.5.4 sounded it.
constexpr float kChugQuieterDb = 1.0f; // chugs at Sustain 100% Fast / Medium / Long: at most this much under v0.5.4 (testChugs)
struct PickResult
{
    int picks = 0, sounded = 0, swallowed = 0, neither = 0, quieter = 0; // quieter: more than kChugQuieterDb under v0.5.4
    float worstDb = 0.0f;
    juce::String where, quietWhere;
};
PickResult comparePicks (const std::vector<float>& x, const std::vector<int64_t>& picks, const EngineParams& p, const TailSwitches& sw = sputterFix())
{
    using namespace sputter;
    Input in;
    in.x = x;
    const auto fix = render (in, p, sw, true), old = render (in, p, {}, true);
    const auto L = (int64_t) fix.latency;
    auto voicedNear = [] (const Render& r, int64_t t) {
        for (auto& h : r.hops)
            if (h.time >= t && h.time < t + (int64_t) (0.06 * kFs) && h.voiced)
                return true;
        return false;
    };
    auto rmsAt = [&] (const std::vector<float>& y, int64_t t) {
        const auto a = (size_t) std::min<int64_t> ((int64_t) y.size(), t + L), b = (size_t) std::min<int64_t> ((int64_t) y.size(), t + L + (int64_t) (0.05 * kFs));
        return rms (y, a, b);
    };
    PickResult res;
    for (auto t : picks)
    {
        ++res.picks;
        const bool vf = voicedNear (fix, t), vo = voicedNear (old, t);
        if (! vo)
        {
            ++res.neither;
            continue;
        }
        const double rf = rmsAt (fix.out, t), ro = rmsAt (old.out, t);
        const float d = (float) toDb (std::max (rf, 1e-9) / std::max (ro, 1e-9));
        res.worstDb = std::min (res.worstDb, d);
        if (d < -kChugQuieterDb)
        {
            ++res.quieter;
            res.quietWhere << juce::String ((double) t / kFs, 2) << " s (" << juce::String (d, 1) << " dB) ";
        }
        if (vf && d >= -6.0f)
            ++res.sounded;
        else
        {
            ++res.swallowed;
            res.where << juce::String ((double) t / kFs, 2) << " s (" << (vf ? "voiced" : "unvoiced") << ", " << juce::String (d, 1) << " dB) ";
        }
    }
    return res;
}

// The chugs the fix swallows, and the proposal (a real pick lifts the one-way cap): the chug patterns and the real takes'
// picks, then the sputter counts (synthetic, real) with the proposal.
void probeChugLift()
{
    using namespace sputter;
    auto noLift = sputterFix();
    noLift.liftOnPick = false;
    std::vector<std::vector<std::pair<int, float>>> pats;
    for (int midi : { 59, 64 })
    {
        std::vector<std::pair<int, float>> v;
        for (int b = 0; b < 2; ++b)
            for (auto a : { 0.8f, 0.15f, 0.3f, 0.15f, 0.6f, 0.15f, 0.3f, 0.15f })
                v.push_back ({ midi, a });
        pats.push_back (v);
    }
    for (auto [name, sw] : { std::pair { "fix without the pick lift (v0.5.5 stage 3)", noLift }, { "fix (a real pick lifts the cap, v0.5.5)", sputterFix() } })
    {
        juce::String line = juce::String (name) + ": accented chugs swallowed (of 16 per run, Sustain 100% Instant / Fast / Medium / Long): ";
        for (double bpm : { 100.0, 140.0, 180.0 })
            for (double muteAt : { 0.002, 0.04 })
            {
                int sw0 = 0;
                for (size_t k = 0; k < pats.size(); ++k)
                {
                    const auto c = chugs (bpm, pats[k], muteAt, 400 + (uint32_t) k);
                    for (auto rel : { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Medium, NoteReleaseV058::Long })
                        sw0 += comparePicks (c.x, c.picks, baseParams (100.0f, rel), sw).swallowed;
                }
                line << (int) bpm << (muteAt < 0.01 ? " tight " : " loose ") << sw0 << "; ";
            }
        info (line);
        juce::String counts = juce::String (name) + ": synthetic dips / octave jumps / reopenings: ";
        for (auto [sname, sus, rel] : { std::tuple { "S100 Fast", 100.0f, NoteReleaseV058::Fast }, { "S100 Instant", 100.0f, NoteReleaseV058::Instant }, { "S100 Long", 100.0f, NoteReleaseV058::Long },
                                        { "S50 Fast", 50.0f, NoteReleaseV058::Fast }, { "S0 Instant", 0.0f, NoteReleaseV058::Instant } })
        {
            const auto t = total (sweep ({ sname, baseParams (sus, rel) }, sw));
            counts << sname << " " << t.dips3 << "/" << t.jumpOct << "/" << t.reopens << "  ";
        }
        counts << "| real takes: ";
        for (float sus : { 0.0f, 50.0f, 100.0f })
            for (auto rel : { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Medium, NoteReleaseV058::Long })
            {
                const auto t = runTakes (realTakes(), baseParams (sus, rel), sw);
                counts << "S" << (int) sus << noteReleaseName (rel)[0] << " " << t.totals.dips3 << "/" << t.totals.jumpOct << "/" << t.totals.reopens << " ";
            }
        info (counts);
    }
}

// Every chug sounds as its own note with the fix (the mute gate's risk): 100 / 140 / 180 bpm, B3 and E4, tight and loose
// mutes, accented / all-soft / all-hard picks and a phrase alternating chugs with open notes, at every Note Release and
// Sustain 0 / 100%; at Sustain 100% Fast / Medium / Long (the one-way release) no chug more than kChugQuieterDb under
// v0.5.4 in its first 50 ms (a soft chug after a hard one was, before the pick lift); and every pick in the real takes
// (Recordings/, so chug recordings added later are included).
void testChugs()
{
    struct Pattern
    {
        const char* name;
        std::vector<std::pair<int, float>> notes; // MIDI (negative: an open note), peak
    };
    auto run = [] (int midi, std::vector<float> amps, int bars) {
        std::vector<std::pair<int, float>> v;
        for (int b = 0; b < bars; ++b)
            for (auto a : amps)
                v.push_back ({ midi, a });
        return v;
    };
    std::vector<Pattern> patterns;
    for (int midi : { 59, 64 })
    {
        const juce::String n = midi == 59 ? "B3" : "E4";
        patterns.push_back ({ "accented", run (midi, { 0.8f, 0.15f, 0.3f, 0.15f, 0.6f, 0.15f, 0.3f, 0.15f }, 2) });
        patterns.push_back ({ "all soft", run (midi, { 0.12f }, 12) });
        patterns.push_back ({ "all hard", run (midi, { 0.8f }, 12) });
    }
    {
        std::vector<std::pair<int, float>> v;
        for (int rep = 0; rep < 2; ++rep)
        {
            for (auto a : { 0.7f, 0.2f, 0.4f, 0.2f, 0.7f, 0.2f, 0.4f, 0.2f })
                v.push_back ({ 59, a });
            v.push_back ({ -64, 0.6f }); // open E4
            for (auto a : { 0.7f, 0.2f, 0.4f, 0.2f })
                v.push_back ({ 59, a });
            v.push_back ({ -67, 0.3f }); // a soft open G4
        }
        patterns.push_back ({ "phrase: chugs B3 / open E4 / chugs / soft open G4", v });
    }
    for (double bpm : { 100.0, 140.0, 180.0 })
        for (double muteAt : { 0.002, 0.04 })
        {
            std::vector<ChugInput> inputs;
            uint32_t seed = 300;
            for (size_t k = 0; k < patterns.size(); ++k)
                inputs.push_back (chugs (bpm, patterns[k].notes, muteAt, seed += 50));
            juce::String line, bad;
            int total = 0, sounded = 0, swallowed = 0, neither = 0;
            float worst = 0.0f;
            std::vector<std::tuple<size_t, float, NoteReleaseV058>> jobs;
            for (size_t k = 0; k < inputs.size(); ++k)
                for (float sus : { 0.0f, 100.0f })
                    for (auto rel : { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Medium, NoteReleaseV058::Long })
                        jobs.push_back ({ k, sus, rel });
            std::vector<PickResult> results (jobs.size()), unlifted (jobs.size());
            auto noLift = sputterFix();
            noLift.liftOnPick = false;
            sputter::parallelFor ((int) jobs.size() * 2, [&] (int j) {
                auto [k, sus, rel] = jobs[(size_t) j / 2];
                (j % 2 == 0 ? results : unlifted)[(size_t) j / 2] = comparePicks (inputs[k].x, inputs[k].picks, sputter::baseParams (sus, rel), j % 2 == 0 ? sputterFix() : noLift);
            });
            int unliftedSwallowed = 0;
            for (auto& r : unlifted)
                unliftedSwallowed += r.swallowed;
            // Sustain 100% at Fast / Medium / Long (where the one-way release holds a note's level): every chug's first 50 ms
            // within kChugQuieterDb of v0.5.4's.
            int quieter = 0;
            float worstHeld = 0.0f;
            juce::String quietText;
            for (size_t j = 0; j < jobs.size(); ++j)
            {
                const auto& r = results[j];
                auto [k, sus, rel] = jobs[j];
                total += r.picks;
                sounded += r.sounded;
                swallowed += r.swallowed;
                neither += r.neither;
                worst = std::min (worst, r.worstDb);
                const juce::String name = juce::String (k < 6 ? (k < 3 ? "B3 " : "E4 ") : "") + patterns[k].name + ", S" + juce::String ((int) sus) + " " + noteReleaseName (rel);
                if (r.swallowed > 0)
                    bad << "\n      " << name << ": " << r.where;
                if (sus > 0.0f && rel != NoteReleaseV058::Instant)
                {
                    worstHeld = std::min (worstHeld, r.worstDb);
                    if (r.quieter > 0)
                    {
                        quieter += r.quieter;
                        quietText << "\n      " << name << ": " << r.quietWhere;
                    }
                }
            }
            const auto mute = muteAt < 0.01 ? "tight" : "loose";
            check (swallowed == 0 && quieter == 0,
                   juce::String::formatted ("%3.0f bpm, %s mute: %d picks over %d patterns x Sustain 0 / 100%% x 4 Note Release; sounded %d, swallowed %d "
                                            "(without the pick lifting the one-way release: %d), not detected by v0.5.4 either %d; worst first 50 ms vs v0.5.4 %.1f dB, "
                                            "at Sustain 100%% Fast / Medium / Long %.2f dB (%d more than %.0f dB quieter)",
                                            bpm, mute, total, (int) patterns.size(), sounded, swallowed, unliftedSwallowed, neither, worst, worstHeld, quieter, kChugQuieterDb)
                       + bad + quietText);
        }
    // The real takes' picks.
    using namespace sputter;
    for (auto& tk : realTakes())
    {
        std::vector<int64_t> picks;
        for (auto& ev : tk.events)
            if (ev.kind == TakeEvent::Pick)
                picks.push_back (ev.from);
        int swallowed = 0, neither = 0;
        juce::String bad;
        for (float sus : { 0.0f, 100.0f })
            for (auto rel : { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Medium, NoteReleaseV058::Long })
            {
                const auto r = comparePicks (tk.x, picks, baseParams (sus, rel));
                swallowed += r.swallowed;
                neither += r.neither;
                if (r.swallowed > 0)
                    bad << " S" << (int) sus << " " << noteReleaseName (rel) << ": " << r.where;
            }
        check (swallowed == 0, tk.name.substring (0, 40) + juce::String::formatted ("...: %d picks x 8 settings, swallowed %d (not detected by v0.5.4 either %d)", (int) picks.size(), swallowed, neither) + bad);
    }
}

// Instant's grace (v0.5.5): 0 / 10 / 20 ms at Instant only (40 ms elsewhere) against 40: Instant's sputter counts
// (synthetic long notes, real takes) and when mutes end (real mutes, the synthetic palm mute) against v0.5.4.
std::vector<std::pair<juce::String, double>> instantMuteEnds (const TailSwitches& sw, float sustain)
{
    using namespace sputter;
    std::vector<std::pair<juce::String, double>> out;
    for (auto& tk : realTakes())
    {
        Input whole;
        whole.x = tk.x;
        const auto p = baseParams (sustain, NoteReleaseV058::Instant);
        const auto r = render (whole, p, sw), b = render (whole, p, {});
        for (auto& ev : tk.events)
            if (ev.kind == TakeEvent::Mute || (ev.kind == TakeEvent::Pick && ev.fromMidi < 0.0))
            {
                const auto c = check (tk, ev, r, b, false);
                if (c.endMs > 0.0)
                    out.push_back ({ tk.name.substring (0, 12) + "@" + juce::String ((double) ev.from / kFs, 2), c.endMs });
            }
    }
    out.push_back ({ "synthetic palm mute", cost (baseParams (sustain, NoteReleaseV058::Instant), sw).muteMs });
    return out;
}

void probeInstantGrace()
{
    using namespace sputter;
    const auto ref0 = instantMuteEnds ({}, 0.0f), ref100 = instantMuteEnds ({}, 100.0f);
    for (float g : { 0.0f, 10.0f, 15.0f, 20.0f, 30.0f, 40.0f })
    {
        auto sw = sputterFix();
        sw.graceInstantMs = g;
        juce::String line = "Instant grace " + juce::String ((int) g) + " ms: synthetic dips / reopenings ";
        for (float sus : { 0.0f, 50.0f, 100.0f })
        {
            const auto t = total (sweep ({ "", baseParams (sus, NoteReleaseV058::Instant) }, sw));
            line << "S" << (int) sus << " " << t.dips3 << "/" << t.reopens << " ";
        }
        line << "| real takes ";
        for (float sus : { 0.0f, 50.0f, 100.0f })
        {
            const auto t = runTakes (realTakes(), baseParams (sus, NoteReleaseV058::Instant), sw).totals;
            line << "S" << (int) sus << " " << t.dips3 << "/" << t.reopens << " ";
        }
        for (float sus : { 0.0f, 100.0f })
        {
            const auto ends = instantMuteEnds (sw, sus);
            const auto& ref = sus > 0.0f ? ref100 : ref0;
            double worst = -1e9;
            line << "| mutes S" << (int) sus << " (ms after v0.5.4): ";
            for (size_t k = 0; k < ends.size() && k < ref.size(); ++k)
            {
                line << juce::String (ends[k].second - ref[k].second, 0) << " ";
                worst = std::max (worst, ends[k].second - ref[k].second);
            }
            line << "worst +" << juce::String (worst, 0);
        }
        info (line);
    }
    juce::String names = "mutes, in order (v0.5.4 ms at S0 / S100): ";
    for (size_t k = 0; k < ref0.size(); ++k)
        names << ref0[k].first << " " << juce::String (ref0[k].second, 0) << " / " << juce::String (ref100[k].second, 0) << "; ";
    info (names);
}

// The synthetic long notes' audible dips at Sustain 100%, Instant: fix2 (40 ms grace) left 4; v0.5.5's 20 ms Instant grace 9.
constexpr int kSputterInstantDips = 9;

// Instant's grace: mutes at Instant end at most the grace later than v0.5.4 (the spec's target was about 10 ms; 20 ms is
// the shortest grace that keeps the sputter fixed, probe-instant-grace), and Instant's sputter counts stay at the 20 ms
// grace's: synthetic long notes 9 audible dips at most and no reopening, real takes none.
void testInstantGrace()
{
    using namespace sputter;
    for (float sus : { 0.0f, 100.0f })
    {
        const auto ref = instantMuteEnds ({}, sus), now = instantMuteEnds (sputterFix(), sus);
        bool ok = ref.size() == now.size();
        double worst = -1e9;
        juce::String line;
        for (size_t k = 0; ok && k < ref.size(); ++k)
        {
            const double late = now[k].second - ref[k].second;
            worst = std::max (worst, late);
            ok = late <= kReleaseGraceInstantMs + 1.0;
            line << now[k].first << " " << juce::String (now[k].second, 0) << " ms (" << (late >= 0.0 ? "+" : "") << juce::String (late, 0) << "); ";
        }
        check (ok, juce::String::formatted ("Sustain %.0f%%, Instant: every mute ends at most %.0f ms after v0.5.4 (latest %+.0f ms): ", sus, kReleaseGraceInstantMs, worst) + line);
    }
    juce::String line;
    bool ok = true;
    for (float sus : { 0.0f, 50.0f, 100.0f })
    {
        const auto t = total (sweep ({ "", baseParams (sus, NoteReleaseV058::Instant) }, sputterFix()));
        const auto r = runTakes (realTakes(), baseParams (sus, NoteReleaseV058::Instant), sputterFix()).totals;
        ok = ok && t.dips3 <= kSputterInstantDips && t.reopens == 0 && t.jumpOct == 0 && r.dips3 == 0 && r.reopens == 0;
        line << "S" << (int) sus << ": synthetic " << t.dips3 << " / " << t.reopens << ", real " << r.dips3 << " / " << r.reopens << "; ";
    }
    check (ok, "Instant's sputter stays fixed (audible dips / reopenings; synthetic at most " + juce::String (kSputterInstantDips) + " dips, v0.5.4: 303 .. 326 and 95): " + line);
}

// The investigation's before / fix2 comparison as pass / fail: audible dips and octave jumps on the synthetic long
// notes and on the real takes at or below fix2's counts (docs/reports/SPUTTER-REPORT.md), with the engine's default (the fix on).
void testSputter()
{
    using namespace sputter;
    // The engine's default is the investigation's fix2 (its switches) with v0.5.5's two revival additions, its 20 ms
    // grace at Instant and the pick lifting the one-way release, bit for bit.
    {
        auto f = fix2Tail();
        f.reviveWakeRiseDb = kReviveWakeRiseDb;
        f.reviveLevelAhead = true;
        f.graceInstantMs = (float) kReleaseGraceInstantMs;
        f.liftOnPick = true;
        for (auto rel : { NoteReleaseV058::Fast, NoteReleaseV058::Instant })
        {
            const auto& in = inputs().plain[3];
            Engine a, b;
            a.prepare (kFs, 64);
            b.prepare (kFs, 64);
            b.setTailSwitches (f);
            const auto p = baseParams (100.0f, rel);
            const auto d = compareSamples (runEngine (a, in.x, p), runEngine (b, in.x, p)).values;
            check (d == 0, in.name + ", Sustain 100% " + (rel == NoteReleaseV058::Fast ? "Fast" : "Instant") + ": the engine's default = the investigation's fix2 + the revival additions + Instant's grace + the pick lift ("
                               + juce::String (d) + " samples differ)");
        }
    }
    // The revival guard leaves ordinary playing alone: no hop kept unvoiced on the regression phrase, separate plucks,
    // medium plucks or the memory phrase, at every Note Release, Sustain 0 and 100%.
    {
        int blocked = 0;
        juce::String where;
        const std::pair<const char*, std::vector<float>> phrases[] = { { "phrase", regressionPhrase (kFs) }, { "separate plucks", separatePlucks (kFs) },
                                                                      { "medium plucks", zonePlucks (0.5f) }, { "memory phrase", memoryPhrase (kFs) } };
        for (auto& [name, in] : phrases)
            for (float sus : { 0.0f, 100.0f })
                for (auto rel : { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Medium, NoteReleaseV058::Long })
                {
                    Engine e;
                    e.prepare (kFs, 64);
                    runEngine (e, in, baseParams (sus, rel));
                    if (const int b = e.tailCounts().revivals; b > 0)
                    {
                        blocked += b;
                        where << "; " << name << " S" << (int) sus << " " << noteReleaseName (rel) << ": " << b;
                    }
                }
        check (blocked == 0, "Ordinary phrases (4 x Sustain 0 / 100% x every Note Release): " + juce::String (blocked) + " hops kept unvoiced by the revival guard" + where);
    }
    struct Limit
    {
        const char* name;
        float sustain;
        NoteReleaseV058 release;
        int dips;
    };
    // fix2's counts in the report (15 notes x 3 noise floors).
    for (const auto& l : { Limit { "S100 Fast", 100.0f, NoteReleaseV058::Fast, 0 }, Limit { "S100 Instant", 100.0f, NoteReleaseV058::Instant, kSputterInstantDips },
                           Limit { "S100 Long", 100.0f, NoteReleaseV058::Long, 0 }, Limit { "S100 Extra Long", 100.0f, NoteReleaseV058::ExtraLong, 0 },
                           Limit { "S75 Extra Long", 75.0f, NoteReleaseV058::ExtraLong, 0 }, Limit { "S50 Fast", 50.0f, NoteReleaseV058::Fast, 0 } })
    {
        const Setting st { l.name, baseParams (l.sustain, l.release) };
        const auto before = total (sweep (st, {}));
        const auto fix = total (sweep (st, sputterFix()));
        check (fix.dips3 <= l.dips && fix.jumpOct == 0 && fix.reopens == 0,
               juce::String::formatted ("Synthetic long notes, %-15s: audible dips %d (at most %d), octave jumps %d, reopenings %d; v0.5.4: %d / %d / %d", l.name, fix.dips3, l.dips,
                                        fix.jumpOct, fix.reopens, before.dips3, before.jumpOct, before.reopens));
    }
    const auto& takes = realTakes();
    if (takes.empty())
    {
        info ("no Recordings/*.wav: the real takes are skipped");
        return;
    }
    const juce::String rn[] = { "Instant", "Fast", "Medium", "Long", "Extra Long" };
    for (float sus : { 0.0f, 50.0f, 75.0f, 100.0f })
    {
        juce::String line;
        bool ok = true;
        for (auto rel : { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Medium, NoteReleaseV058::Long, NoteReleaseV058::ExtraLong })
        {
            const auto fix = runTakes (takes, baseParams (sus, rel), sputterFix());
            const auto& t = fix.totals;
            ok = ok && t.dips3 == 0 && t.jumpOct == 0 && t.reopens == 0;
            line << rn[(int) rel] << " " << t.dips3 << "/" << t.jumpOct << "/" << t.reopens << "  ";
        }
        check (ok, juce::String::formatted ("Real takes (%d picked notes), Sustain %3.0f%%: audible dips / octave jumps / reopenings, all 0 as fix2: ", (int) [&] {
                                                int n = 0;
                                                for (auto& tk : takes)
                                                    n += (int) tk.notes.size();
                                                return n;
                                            }(), sus)
                       + line.trimEnd());
    }
}

// On every slide, glide and vibrato in the real takes, the fix never lowers the level or blocks a pitch move: no
// one-way cap and no lock during it, the level no lower than v0.5.4's (Sustain 100%: no lower under the held level),
// and the voice's note off what's played no longer than with v0.5.4.
void testSlides()
{
    using namespace sputter;
    const auto& takes = realTakes();
    if (takes.empty())
    {
        info ("no Recordings/*.wav: skipped");
        return;
    }
    struct Run
    {
        juce::String name;
        EngineParams p;
    };
    std::vector<Run> runs;
    const juce::String rn[] = { "Instant", "Fast", "Medium", "Long" };
    for (float sus : { 0.0f, 50.0f, 100.0f })
        for (auto rel : { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Medium, NoteReleaseV058::Long })
            runs.push_back ({ "S" + juce::String ((int) sus) + " " + rn[(int) rel], baseParams (sus, rel) });
    const double hopMs = 1000.0 * hopFor (kFs) / kFs;
    int events = 0;
    for (auto& tk : takes)
    {
        Input whole;
        whole.x = tk.x;
        std::vector<Render> fix (runs.size()), old (runs.size());
        parallelFor ((int) runs.size() * 2, [&] (int i) { (i % 2 == 0 ? fix : old)[(size_t) i / 2] = render (whole, runs[(size_t) i / 2].p, i % 2 == 0 ? sputterFix() : TailSwitches {}); });
        for (auto& ev : tk.events)
        {
            if (ev.kind != TakeEvent::Glide && ev.kind != TakeEvent::Vibrato)
                continue;
            ++events;
            bool ok = true;
            float held = 0.0f, vs = 0.0f;
            double capped = 0.0, wrongExtra = 0.0;
            int locks = 0;
            juce::String where;
            for (size_t s = 0; s < runs.size(); ++s)
            {
                const bool sus100 = runs[s].p.sustainPercent >= 100.0f;
                const auto c = check (tk, ev, fix[s], old[s], sus100), b = check (tk, ev, old[s], old[s], sus100);
                const bool runOk = c.cappedMs == 0.0 && c.locks == 0 && c.wrongMs <= b.wrongMs + hopMs + 1e-9 && c.heldMinDb >= b.heldMinDb - 0.1f && c.vsBeforeMinDb >= -0.5f;
                if (! runOk)
                    where << " " << runs[s].name;
                ok = ok && runOk;
                held = std::min (held, c.heldMinDb - b.heldMinDb);
                vs = std::min (vs, c.vsBeforeMinDb);
                capped += c.cappedMs;
                locks += c.locks;
                wrongExtra = std::max (wrongExtra, c.wrongMs - b.wrongMs);
            }
            check (ok, tk.name.substring (0, 24) + "... " + describe (ev).substring (0, 72).trimEnd()
                           + juce::String::formatted (" -- 12 settings: level vs v0.5.4 %.1f dB at worst (held level %+.1f dB), one-way cap %.0f ms, locks %d, wrong note %+.0f ms",
                                                      vs, held, capped, locks, wrongExtra)
                           + (where.isEmpty() ? juce::String() : " -- fails at" + where));
        }
    }
    info (juce::String (events) + " slides, glides and vibrato in " + juce::String ((int) takes.size()) + " takes");
}

// The edge rule: no voiced reading (so no note) above kDetectorMaxHz with the fix, on the real takes; and real notes up
// to C#6 (the top of a 21-fret guitar) are untouched by it.
void testEdgeRule()
{
    using namespace sputter;
    const auto& takes = realTakes();
    for (auto& tk : takes)
    {
        Input whole;
        whole.x = tk.x;
        int above[2] = { 0, 0 }, noteAbove[2] = { 0, 0 };
        for (int fix = 0; fix < 2; ++fix)
            for (auto rel : { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Long })
            {
                const auto r = render (whole, baseParams (100.0f, rel), fix ? sputterFix() : TailSwitches {});
                for (auto& h : r.hops)
                {
                    above[fix] += h.voiced && h.f0 > kDetectorMaxHz ? 1 : 0;
                    noteAbove[fix] += h.voiced && hzFromMidi (h.note) > kDetectorMaxHz ? 1 : 0;
                }
            }
        check (above[1] == 0 && noteAbove[1] == 0, tk.name.substring (0, 40) + juce::String::formatted ("...: voiced hops above 1400 Hz %d, on a note above it %d (v0.5.4: %d / %d; Sustain 100%%, Instant / Fast / Long)",
                                                                                           above[1], noteAbove[1], above[0], noteAbove[0]));
    }
    // Plucks A5 .. C#6 at the default and the top window, Sustain 0 and 100%: the output with the rule = without it.
    for (int note : { 81, 83, 84, 85 })
        for (float windowHz : { defaultWindowHz(), (float) kWindowMaxHz })
            for (float sus : { 0.0f, 100.0f })
            {
                const double hz = hzFromMidi (note);
                const auto in = ksPluck ([hz] (double) { return hz; }, 2.0, 0.5f, ksLossFor (hz, 3.0), (uint32_t) note);
                EngineParams p = baseParams (sus);
                p.windowHz = windowHz;
                Engine a, b;
                a.prepare (kFs, 64);
                b.prepare (kFs, 64);
                auto t = sputterFix();
                t.edgeLagUnvoiced = false;
                b.setTailSwitches (t);
                const auto d = compareSamples (runEngine (a, in, p), runEngine (b, in, p)).values;
                check (d == 0 && a.tailCounts().edgeLags == 0,
                       juce::String::formatted ("%s pluck (%.0f Hz), window %.0f Hz, Sustain %.0f%%: = without the edge rule (%d samples differ, %d hops unvoiced by it)",
                                                (gtt::pitchClassName (note) + juce::String (note / 12 - 1)).toRawUTF8(), hz, windowHz, sus, d, a.tailCounts().edgeLags));
            }
}


void probeSputterFull()
{
    using namespace sputter;
    outDir.getChildFile ("sputter").createDirectory();
    const auto t0 = std::chrono::steady_clock::now();

    // 1. Detection and the baseline (Sustain 100%, Fast, default window, Saw, Chord Off), note by note, with the CSVs.
    info ("== 1. Baseline per note (Sustain 100%, Note Release Fast, Saw, Chord Off), by window ==");
    for (auto [wn, w] : { std::pair { "min", (float) derivedWindowMinHz() }, { "default", defaultWindowHz() }, { "top", (float) kWindowMaxHz } })
    {
        const char* wname = wn;
        const float wHz = w;
        EngineParams p = baseParams();
        p.windowHz = wHz;
        const auto& ins = inputs().plain;
        std::vector<Result> rs (ins.size());
        parallelFor ((int) ins.size(), [&] (int i) {
            const auto& in = ins[(size_t) i];
            if (in.note < lowestReliableNote (kFs, wHz))
            {
                rs[(size_t) i].skipped = true;
                return;
            }
            const auto r = render (in, p, {}, true);
            rs[(size_t) i] = analyse (in, r);
            writeHopCsv ("hops-" + in.name + "-window-" + wname, in, r, p);
        });
        for (size_t i = 0; i < ins.size(); ++i)
        {
            const auto& r = rs[i];
            if (r.skipped)
            {
                info (juce::String ("window ") + wname + " " + ins[i].name.paddedRight (' ', 22) + "below the window's range");
                continue;
            }
            info (juce::String ("window ") + wname + " " + ins[i].name.paddedRight (' ', 22) + "dropouts " + juce::String (r.dropouts).paddedLeft (' ', 3)
                  + " (rms " + juce::String (r.dropRms) + ", conf " + juce::String (r.dropConf) + "; 1 hop " + juce::String (r.dropShort) + ", 2-10 "
                  + juce::String (r.dropMid) + ", >10 " + juce::String (r.dropLong) + ", " + juce::String (r.dropMsTotal, 0) + " ms)"
                  + (r.firstDropS >= 0 ? " first at " + juce::String (r.firstDropS, 2) + "s " + juce::String (r.firstDropDbfs, 1) + "dBFS ("
                                             + juce::String (r.firstDropBelowDb, 1) + " dB under peak)"
                                       : juce::String (" none"))
                  + " | audible dips " + juce::String (r.dips3) + " (>=10dB " + juce::String (r.dips10) + ", max " + juce::String (r.maxDip, 1) + "dB)"
                  + (r.firstAudibleS >= 0 ? " first at " + juce::String (r.firstAudibleDbfs, 1) + "dBFS (" + juce::String (r.firstAudibleBelowDb, 1) + " under)" : juce::String())
                  + " | pitch oct/5th/semi/other " + juce::String (r.jumpOct) + "/" + juce::String (r.jumpFifth) + "/" + juce::String (r.jumpSemi) + "/"
                  + juce::String (r.jumpOther) + " guard " + juce::String (r.guard) + " max " + juce::String (r.maxCents, 0) + "c >20c " + juce::String (r.offMs, 0) + "ms"
                  + " | back louder " + juce::String (r.louder) + " (+" + juce::String (r.maxLouder, 1) + "dB)" + " | onsets " + juce::String (r.falseOnsets) + " | reopen " + juce::String (r.reopens) + " | level jumps "
                  + juce::String (r.levelJumps) + " | last voiced " + juce::String (r.lastVoicedS, 2) + "s " + juce::String (r.lastVoicedDbfs, 1) + "dBFS ("
                  + juce::String (r.lastVoicedBelowDb, 1) + " under) | tail end " + juce::String (r.tailEndS, 2) + "s");
        }
    }

    // A few more per-hop logs: other settings on E4 over the -60 dB floor.
    for (auto& in : inputs().plain)
        if (in.name == "E4-noise-60")
            for (auto [label, p] : { std::pair { "S0-instant", baseParams (0.0f, NoteReleaseV058::Instant) }, { "S0-fast", baseParams (0.0f) },
                                     { "S50-fast", baseParams (50.0f) }, { "S100-long", baseParams (100.0f, NoteReleaseV058::Long) } })
                writeHopCsv (juce::String ("hops-E4-noise-60-") + label, in, render (in, p, {}, true), p);
    for (auto& in : inputs().plain)
        if (in.name == "E5-noise-50")
        {
            EngineParams p = baseParams();
            p.windowHz = (float) kWindowMaxHz;
            writeHopCsv ("hops-E5-noise-50-window-top-fix", in, render (in, p, recommendedTail(), true), p);
        }

    // 2. Across settings (one factor at a time from the baseline; the Sustain x Note Release grid in full).
    info ("== 2. Across settings (totals over 6 notes x 3 noise floors) ==");
    std::vector<Setting> settings;
    for (float sus : { 0.0f, 50.0f, 100.0f })
        for (auto rel : { NoteReleaseV058::Instant, NoteReleaseV058::Fast, NoteReleaseV058::Medium, NoteReleaseV058::Long })
        {
            const char* rn[] = { "Instant", "Fast", "Medium", "Long" };
            settings.push_back ({ "Sustain " + juce::String ((int) sus) + " " + rn[(int) rel], baseParams (sus, rel) });
        }
    {
        Setting s { "S100 Fast, window min", baseParams() };
        s.p.windowHz = (float) derivedWindowMinHz();
        settings.push_back (s);
        s = { "S100 Fast, window top", baseParams() };
        s.p.windowHz = (float) kWindowMaxHz;
        settings.push_back (s);
        s = { "S100 Fast, Sine", baseParams() };
        s.p.wave = Wave::Sine;
        settings.push_back (s);
        s = { "S100 Fast, Stack C maj", baseParams() };
        setChordChoice (s.p, harmonizeChoice (0));
        settings.push_back (s);
        s = { "S100 Fast, Memory 3", baseParams() };
        setChordChoice (s.p, freezeChoice (3));
        s.prefix = true;
        settings.push_back (s);
        s = { "S100 Long, Memory 3", baseParams (100.0f, NoteReleaseV058::Long) };
        setChordChoice (s.p, freezeChoice (3));
        s.prefix = true;
        settings.push_back (s);
        s = { "S0 Fast, Memory 3", baseParams (0.0f) };
        setChordChoice (s.p, freezeChoice (3));
        s.prefix = true;
        settings.push_back (s);
    }
    for (auto& s : settings)
    {
        const auto t = total (sweep (s, {}));
        info (totalsLine (s.name, t) + (s.p.chordMode == ChordMode::Memory ? " | HELD changes " + juce::String (t.heldChanges) + ", clears mid-tail "
                                                                                 + juce::String (t.memoryClears) + ", chord lost mid-tail " + juce::String (t.chordLost)
                                                                           : juce::String()));
    }

    // 3. The mitigations at Sustain 100% (Fast, Instant, Long) and Sustain 50% Fast, with their costs.
    info ("== 3. Mitigations (totals over 6 notes x 3 noise floors; cost: palm mute to -40 dB, mean of B3 E4 A4, and a soft E4's length) ==");
    const auto ms = mitigations();
    for (auto [sname, p] : { std::pair { "S100 Fast", baseParams() }, { "S100 Instant", baseParams (100.0f, NoteReleaseV058::Instant) },
                             { "S100 Long", baseParams (100.0f, NoteReleaseV058::Long) }, { "S50 Fast", baseParams (50.0f) } })
    {
        info (juce::String ("-- ") + sname);
        for (auto& m : ms)
        {
            const auto t = total (sweep ({ sname, p }, m.sw));
            const auto c = cost (p, m.sw);
            info (totalsLine (m.name, t) + " | mute " + juce::String (c.muteMs, 0) + "ms, soft E4 " + juce::String (c.softS, 2) + "s, soft G4 after a mute sounds "
                  + juce::String (c.softAfterMuteMs, 0) + " ms after its pick, slide E4->G4 at 0.5 s / 3 s ends on "
                  + juce::String (gtt::pitchClassName (c.earlySlide)) + juce::String (c.earlySlide / 12 - 1) + " / " + gtt::pitchClassName (c.lateSlide) + juce::String (c.lateSlide / 12 - 1));
        }
    }

    // 4. The recommended fix across the settings of section 2.
    info ("== 4. The recommended fix across settings ==");
    for (auto& s : settings)
    {
        const auto rs = sweep (s, recommendedTail());
        const auto& ins = s.prefix ? inputs().prefixed : inputs().plain;
        for (size_t i = 0; i < rs.size(); ++i)
            if (! rs[i].skipped && rs[i].dips3 > 0)
                info ("  (" + s.name + ") " + ins[i].name + ": " + juce::String (rs[i].dips3) + " audible, first at " + juce::String (rs[i].firstAudibleS, 2) + "s "
                      + juce::String (rs[i].firstAudibleBelowDb, 1) + " dB under the peak, max " + juce::String (rs[i].maxDip, 1) + " dB; last voiced "
                      + juce::String (rs[i].lastVoicedS, 2) + "s");
        const auto t = total (rs);
        info (totalsLine (s.name, t) + (s.p.chordMode == ChordMode::Memory ? " | HELD changes " + juce::String (t.heldChanges) + ", clears mid-tail "
                                                                                 + juce::String (t.memoryClears) + ", chord lost mid-tail " + juce::String (t.chordLost)
                                                                           : juce::String()));
    }

    // 5. Ordinary playing: the existing phrases at the plugin's defaults (and Sustain 100%) with and without the fix.
    info ("== 5. Ordinary playing with the fix (output difference from without it) ==");
    std::vector<Mitigation> parts;
    {
        TailSwitches t;
        t.graceMs = 40.0f;
        parts.push_back ({ "grace 40", t });
        t = {};
        t.oneWayUnderDb = 20.0f;
        parts.push_back ({ "one-way 20", t });
        t = {};
        t.lockUnderRefDb = 20.0f;
        t.lockPersistMs = 30.0f;
        parts.push_back ({ "lock 20 / 30 ms", t });
        t.lockPersistMs = 1.0f;
        parts.push_back ({ "lock 20, harmonics only", t });
        t.lockPersistMs = 10.0f;
        parts.push_back ({ "lock 20, others after 10 ms", t });
        t.lockPersistMs = 30.0f;
        parts.push_back ({ "lock 20, others after 30 ms", t });
        t = {};
        t.reviveRiseDb = 6.0f;
        parts.push_back ({ "revive 6", t });
        t = {};
        t.pickRestartUnderDb = 20.0f;
        parts.push_back ({ "pick restarts 20 dB under", t });
        t.pickRestartUnderDb = 30.0f;
        parts.push_back ({ "pick restarts 30 dB under", t });
        parts.push_back ({ "RECOMMENDED", recommendedTail() });
    }
    for (auto& part : parts)
    for (auto [pname, x] : { std::pair { "regression phrase", regressionPhrase (kFs) }, { "separate plucks", separatePlucks (kFs) },
                             { "zone plucks, medium", zonePlucks (0.5f) }, { "memory phrase", memoryPhrase (kFs) } })
        for (float sus : { 0.0f, 100.0f })
        {
            EngineParams p = baseParams (sus);
            if (juce::String (pname) == "memory phrase")
                setChordChoice (p, freezeChoice (3));
            Input in;
            in.x = x;
            const auto a = render (in, p, {}), b = render (in, p, part.sw);
            // Pitch: the voice's T per hop (both voiced). Level: the predicted output peak per 1 ms, within 40 dB of the loudest.
            double maxCents = 0.0, centsMs = 0.0;
            size_t worstHop = 0;
            for (size_t k = 0; k < std::min (a.hops.size(), b.hops.size()); ++k)
                if (a.hops[k].voiced && b.hops[k].voiced)
                {
                    const double c = 100.0 * std::abs (a.hops[k].target - b.hops[k].target);
                    if (c > maxCents)
                        worstHop = k;
                    maxCents = std::max (maxCents, c);
                    centsMs += c > 10.0 ? 1000.0 * hopFor (kFs) / kFs : 0.0;
                }
            float peak = 0.0f;
            for (auto v : a.amp)
                peak = std::max (peak, v);
            double maxDb = 0.0, dbMs = 0.0, firstDb = -1.0;
            const size_t frame = (size_t) (0.001 * kFs);
            for (size_t i = 0; i + frame <= a.amp.size(); i += frame)
            {
                const float la = db (a.amp[i]), lb = db (b.amp[i]);
                if (std::max (la, lb) < db (peak) - 40.0f)
                    continue;
                const double d = std::abs (la - lb);
                maxDb = std::max (maxDb, d);
                if (d > 1.0)
                {
                    dbMs += 1.0;
                    if (firstDb < 0.0)
                        firstDb = (double) i / kFs;
                }
            }
            if (maxCents > 0.0 && part.name == "RECOMMENDED" && sus == 0.0f)
                for (size_t k = worstHop > 12 ? worstHop - 12 : 0; k < std::min (a.hops.size(), worstHop + 4); ++k)
                    info ("    t " + juce::String ((double) a.hops[k].time / kFs, 4) + " f0 " + juce::String (a.hops[k].f0, 1) + " conf " + juce::String (a.hops[k].confidence, 3)
                          + " rms " + juce::String (db (a.hops[k].rms), 1) + " | T without " + juce::String (a.hops[k].target, 2) + " with " + juce::String (b.hops[k].target, 2));
            info (part.name + ": " + juce::String (pname) + ", Sustain " + juce::String ((int) sus) + "%: pitch max " + juce::String (maxCents, 1) + " c, "
                  + juce::String (centsMs, 0) + " ms over 10 c | level max " + juce::String (maxDb, 1) + " dB, " + juce::String (dbMs, 0) + " ms over 1 dB"
                  + (firstDb >= 0.0 ? " (from " + juce::String (firstDb, 2) + " s)" : juce::String()));
        }

    // 6. The audio examples.
    writeExamples (sputterCandidates());

    // 7. The real takes.
    info ("== 7. Real recordings (Recordings/*.wav, batch 2) ==");
    sputterRecordings();
    info ("probe-sputter-full took " + juce::String (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count(), 0) + " s");
}

#include "BlipsProbes.inc"
#include "PickGlide.inc"
#include "Ceiling.inc"

#include "V06.inc"
#include "Golden.inc"
#include "Grid.inc"
#include "RC1QA.inc"
#include "RC2.inc"

// v0.5.10: the blip fixes on the real takes (docs/reports/BLIPS-REPORT.md). The engine starts with blipFix(); blips-dry has no blip with the
// fall lock; on the legato takes no note change follows more than 3 ms later than v0.5.9 but one quiet trill step (24 ms in
// the report); with the mute gate's pass the trills miss no note change and the pull-offs are off pitch no longer than the
// report's 414 ms.
void testBlips()
{
    using namespace blips;
    EngineParams sine;
    sine.wave = Wave::Sine;
    const auto& dry = dryTake().x;
    {
        // v0.6.2: run() starts from v0.5.9 (the pick glide and v0.6.2's fixes off too), so those go back on for the comparison
        const auto def = run (dry, sine, {}, true), set = run (dry, sine, [] (Engine& e) {
                                                        e.setPickSwitches (blipFix());
                                                        e.setPickGlide (true);
                                                        e.setV062Fixes (true);
                                                    });
        check (compareSamples (def.out, set.out).values == 0, "a fresh engine plays blipFix() (blips-dry, bit for bit)");
    }
    auto fixes = [] (bool fall, bool mute) { return [fall, mute] (Engine& e) { e.setBlipFixes (fall, mute); }; };
    const Variant v059 { "v0.5.9 (both off)", fixes (false, false), {} }, fallOnly { "fall lock alone", fixes (true, false), {} },
        muteOnly { "mute gate's pass alone", fixes (false, true), {} }, both { "both (the defaults)", fixes (true, true), {} };
    {
        const auto ps = takePicks (dry);
        for (const auto* v : { &v059, &fallOnly, &muteOnly, &both })
        {
            const auto r = run (dry, sine, v->setup);
            const auto sc = score (r.out, ps, r.latency);
            const juce::String text = "blips-dry, " + v->name + ": " + scoreText (sc);
            if (v == &fallOnly || v == &both)
                check (sc.blips == 0, text);
            else
                info (text);
        }
        const auto a = run (dry, sine, v059.setup), b = run (dry, sine, muteOnly.setup);
        check (compareSamples (a.out, b.out).values == 0, "blips-dry: the mute gate's pass alone changes nothing (bit for bit)");
    }
    int later = 0;
    double worstLater = 0.0;
    juce::String laterText;
    for (const auto& t : takes())
    {
        if (! (t.name == "hammer-ons" || t.name == "pull-offs" || t.name == "trills"))
            continue;
        const auto cs = inputChanges (t.x);
        const auto ra = run (t.x, sine, v059.setup), rb = run (t.x, sine, both.setup);
        const auto da = follows (ra, cs), db = follows (rb, cs);
        int missedA = 0, missedB = 0;
        for (size_t k = 0; k < cs.size(); ++k)
        {
            missedA += isMissed (da[k]) ? 1 : 0;
            missedB += isMissed (db[k]) ? 1 : 0;
            if (! isMissed (da[k]) && (isMissed (db[k]) || db[k] - da[k] > 3.0))
            {
                const double d = isMissed (db[k]) ? 1.0e9 : db[k] - da[k];
                ++later;
                worstLater = std::max (worstLater, d);
                laterText << juce::String::formatted ("\n      %s %.3f s %s -> %s: +%.1f ms", t.name.toRawUTF8(), cs[k].t / kFs,
                                                      juce::MidiMessage::getMidiNoteName (cs[k].from, true, true, 4).toRawUTF8(),
                                                      juce::MidiMessage::getMidiNoteName (cs[k].to, true, true, 4).toRawUTF8(), d);
            }
        }
        const double offA = trackingMs (t.x, v059), offB = trackingMs (t.x, both);
        const auto text = juce::String::formatted ("%s: %zu note changes; missed %d (v0.5.9 %d); output > 50 c off the input %.0f ms (v0.5.9 %.0f)", t.name.toRawUTF8(),
                                                   cs.size(), missedB, missedA, offB, offA);
        if (t.name == "trills")
            check (missedB == 0, text);
        else if (t.name == "pull-offs")
            check (offB <= 415.0, text + " (the report: 414 ms)");
        else
            info (text);
        if (t.name == "hammer-ons")
        {
            const auto rm = run (t.x, sine, muteOnly.setup);
            check (compareSamples (ra.out, rm.out).values == 0, "hammer-ons: the mute gate's pass alone changes nothing (bit for bit)");
        }
    }
    check (later <= 1 && worstLater <= 25.0, juce::String::formatted ("legato takes: %d note change(s) followed more than 3 ms later than v0.5.9 (at most 1, at most 25 ms: the quiet trill step)", later) + laterText);
}



// v0.5.11: a note above the board's top (C#6) shows ABOVE BOARD (noteDisplayFor, pure); every note up to C#6 shows as before. Also
// through the engine: D6 .. F6 plucks (under the 1400 Hz ceiling) read as voiced notes above the board.
void testAboveBoard()
{
    bool pure = gtt::kTopBoardNote == 85 && gtt::positionNote (5, gtt::kFretCount) == gtt::kTopBoardNote;
    for (int n = 40; n <= 100; ++n)
    {
        const auto d = gtt::noteDisplayFor (true, n, false, -1, 59, false);
        pure = pure && d.playing == n && d.aboveBoard == (n > 85 ? n : -1) && d.belowRange == -1;
        const auto u = gtt::noteDisplayFor (false, n, false, -1, 59, false); // unvoiced: nothing playing, nothing above
        pure = pure && u.playing == -1 && u.aboveBoard == -1;
    }
    check (pure, "noteDisplayFor: a voiced note above C#6 (the 21st fret on high e, MIDI 85) is ABOVE BOARD, C#6 and below aren't, an unvoiced one never (E2 .. E7)");
    juce::String line;
    bool engineOk = true;
    for (int note : { 85, 86, 88, 89 })
    {
        const double hz = hzFromMidi (note);
        const auto in = ksPluck ([hz] (double) { return hz; }, 1.0, 0.5f, ksLossFor (hz, 3.0), (uint32_t) note);
        Engine e;
        e.prepare (kFs, 64);
        int on = 0, n = 0;
        e.hopListener = [&] (const HopInfo& h) {
            if (h.time > (int64_t) (0.05 * kFs) && h.time < (int64_t) (0.6 * kFs))
            {
                ++n;
                on += h.detector.voiced && h.noteN == note ? 1 : 0;
            }
        };
        runEngine (e, in, EngineParams {});
        const auto d = gtt::noteDisplayFor (true, note, false, -1, 59, false);
        engineOk = engineOk && on >= (int) (0.8 * n) && (d.aboveBoard >= 0) == (note > 85);
        line << juce::String::formatted ("%s%s %d/%d hops voiced on it%s", line.isEmpty() ? "" : "; ", sputter::noteName (note).toRawUTF8(), on, n, d.aboveBoard >= 0 ? " (ABOVE BOARD)" : "");
    }
    check (engineOk, "plucks at the default window: " + line);
}

// v0.5.11: the pitch trace's feed (display only). With an editor reading it (TraceFifo::readers > 0) the engine's output is the same
// as with none, bit for bit (every Chord mode, 44.1 / 48 / 96 kHz); a frame per hop of the voice's timeline; and the input and
// output lines line up in time (the input drawn where the voice plays it, L after it was played): on a step E4 -> A4 at Retune
// Fast the first frame with the input on A4 and the first with the output on A4 are the same frame.
void testTrace()
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const auto in = regressionPhrase (fs);
        bool same = true;
        int frames = 0, expected = 0;
        for (int choice : { 0, freezeChoice (3), harmonizeChoice (0) })
        {
            const auto p = chordChoiceParams (choice);
            Engine a, b;
            a.prepare (fs, 64);
            b.prepare (fs, 64);
            auto feed = std::make_unique<TraceFifo>();
            ++feed->readers;
            b.setTraceFeed (feed.get());
            int got = 0;
            std::vector<float> ya (in.size()), yb (in.size());
            for (size_t start = 0; start < in.size(); start += 64)
            {
                const int n = (int) std::min ((size_t) 64, in.size() - start);
                a.process (in.data() + start, ya.data() + start, n, p);
                b.process (in.data() + start, yb.data() + start, n, p);
                feed->pull ([&got] (const TraceFrame&) { ++got; });
            }
            same = same && compareSamples (ya, yb).values == 0;
            frames += got;
            expected += (int) (in.size() / (size_t) b.hopSize());
        }
        check (same && std::abs (frames - expected) <= 3, juce::String::formatted ("@ %4.1f kHz: the trace's feed open = closed (Chord Off, Freeze 3, Harmonize; bit for bit); %d frames, one per hop (%d)", fs / 1000.0, frames, expected));
    }
    // Alignment: E4 then A4, a legato step on one saw, Retune Fast (T jumps with N).
    {
        std::vector<float> x;
        double phase = 0.0;
        for (size_t n = 0; n < (size_t) (1.2 * kFs); ++n)
        {
            const double hz = n < (size_t) (0.6 * kFs) ? 329.63 : 440.0;
            x.push_back (0.4f * (float) (2.0 * phase - 1.0));
            phase += hz / kFs;
            phase -= std::floor (phase);
        }
        EngineParams p;
        p.retuneSpeed = RetuneSpeed::Fast;
        Engine e;
        e.prepare (kFs, 64);
        auto feed = std::make_unique<TraceFifo>();
        ++feed->readers;
        e.setTraceFeed (feed.get());
        std::vector<TraceFrame> fr;
        for (size_t start = 0; start < x.size(); start += 64)
        {
            float y[64];
            e.process (x.data() + start, y, (int) std::min ((size_t) 64, x.size() - start), p);
            feed->pull ([&fr] (const TraceFrame& f) { fr.push_back (f); });
        }
        int64_t inAt = -1, outAt = -1;
        for (const auto& f : fr)
        {
            if (inAt < 0 && f.inMidi > 68.5f)
                inAt = f.time;
            if (outAt < 0 && f.outMidi > 68.5f)
                outAt = f.time;
        }
        check (inAt >= 0 && inAt == outAt, juce::String::formatted ("E4 -> A4 at 0.6 s (Retune Fast): the input line reaches A4 at %.4f s, the output at %.4f s on the voice's timeline (the same frame: the input is drawn L later than it was played, %.2f ms)",
                                                                  (double) inAt / kFs, (double) outAt / kFs, 1000.0 * e.currentLatencySamples() / kFs));
    }
}

struct TestCase
{
    const char* name;
    std::function<void()> fn;
    bool onRequest = false; // only with --only (too slow for the default run)
};

std::vector<TestCase> allTests()
{
    return {
        { "bypass", testBypassAndClip },
        { "quantizer", testQuantizerUnits },
        { "detector", testDetector },
        { "hysteresis", testHysteresis },
        { "unvoiced", testUnvoiced },
        { "synth", testSynth },
        { "octaves", testOctaves },
        { "octave-switch", testOctaveSwitch },
        { "triangle", testTriangleAliasing },
        { "window", testWindowSweep },
        { "readout-claim", testReadoutClaim },
        { "window-min", testWindowMinimum },
        { "tab", testTab },
        { "fretboard", testFretboard },
        { "probe-reliable", probeReliable },
        { "fixed-detection", testFixedDetection },
        { "manual", testManual },
        { "direct", testDirectWindowHold },
        { "hidden-manual", testHiddenManual },
        { "retune-speed", testRetuneSpeed },
        { "note-release", testNoteReleaseTail },
        { "sustain", testSustain },
        { "envelope-box", testEnvelopeBox },
        { "chord-stack", testChordStack },
        { "chord-octave", testChordOctave },
        { "chord-diatonic", testChordDiatonic },
        { "chord-level", testChordLevel },
        { "chord-memory", testChordMemory },
        { "chord-clicks", testChordClicks },
        { "chord-naming", testChordNaming },
        { "key-labels", testKeyLabels },
        { "glide", testGlide },
        { "onset-snap", testOnsetSnap },
        { "inheritance", testInheritance },
        { "dynamics", testDynamics },
        { "probe-dynamics", probeDynamics },
        { "wave-loudness", testWaveLoudness },
        { "probe-wave-loudness", probeWaveLoudness },
        { "probe-headroom", probeHeadroom },
        { "headroom", testHeadroom },
        { "headroom-motion", testHeadroomMotion },
        { "chord-selector", testChordSelector },
        { "memory-notes", testMemoryNotes },
        { "memory-clear", testMemoryClear },
        { "held", testHeld },
        { "dynamics-fix", testDynamicsFix },
        { "low-range", testLowRange },
        { "display-rules", testDisplayRules },
        { "probe-low-range", probeLowRange },
        { "probe-dynamics-fix", probeDynamicsFixWhere },
        { "pulse-waves", testPulseWaves },
        { "bottom-edge", rc2::testBottomEdge },
        { "level-tick", rc2::testLevelTick },
        { "chord-split", testChordSplit },
        { "latency-detents", testLatencyDetents },
        { "latency-text", testLatencyText },
        { "blips", testBlips },
        { "pick-glide", testPickGlide },
        { "above-board", testAboveBoard },
        { "ceiling", testCeiling },
        { "trace", testTrace },
        { "half-sine-organ", testHalfSineOrgan },
        { "headroom-v057", testHeadroomV057 },
        { "headroom-v058", testHeadroomV058 },
        { "folded", testFolded },
        { "gaps", testGaps },
        { "instant-grace", testInstantGrace },
        { "chugs", testChugs },
        { "probe-chug-lift", probeChugLift, true },
        { "probe-blips-timeline", probeBlipsTimeline, true },
        { "probe-blips-profile", probeBlipsProfile, true },
        { "probe-blips-spectrum", probeBlipsSpectrum, true },
        { "probe-blips-recorded", probeBlipsRecordedShape, true },
        { "probe-blips-recorded-cycles", probeBlipsRecordedCycles, true },
        { "probe-blips-mitigations", probeBlipsMitigations, true },
        { "probe-blips-remaining", probeBlipsRemaining, true },
        { "probe-blips-fall-lock", probeBlipsFallLock, true },
        { "probe-blips-table", probeBlipsTable, true },
        { "probe-blips-other-takes", probeBlipsOtherTakes, true },
        { "probe-blips-confidence-pass", probeBlipsConfidencePass, true },
        { "probe-blips-other-picks", probeBlipsOtherPicks, true },
        { "probe-blips-stale", probeBlipsStale, true },
        { "probe-blips-hammer-on", probeBlipsHammerOn, true },
        { "probe-blips-legato", probeBlipsLegato, true },
        { "probe-blips-hops", probeBlipsHops, true },
        { "probe-blips-legato-wavs", probeBlipsLegatoWavs, true },
        { "probe-pick-glide", probePickGlide, true },
        { "probe-rc1-match", rc1::probeMatch, true },
        { "probe-rc1-settings", rc1::probeSettings, true },
        { "probe-rc1-settings-detail", rc1::probeSettingsDetail, true },
        { "probe-rc1-segments", rc1::probeSegments, true },
        { "probe-rc1", rc1::probeQA, true },
        { "probe-rc1-offsets", rc1::probeOffsets, true },
        { "probe-rc1-spot", rc1::probeSpot, true },
        { "probe-rc1-gain", rc1::probeGain, true },
        { "probe-rc1-heard-waves", rc1::probeHeardWaves, true },
        { "probe-rc1-heard-switch", rc1::probeHeardSwitch, true },
        { "probe-rc1-heard-after", rc1::probeHeardAfter, true },
        { "probe-rc1-heard", rc1::probeHeard, true },
        { "probe-rc1-timeline", rc1::probeTimeline, true },
        { "probe-rc1-samples", rc1::probeSamples, true },
        { "probe-rc1-tuning", rc1::probeTuning, true },
        { "probe-rc1-edge", rc1::probeEdge, true },
        { "probe-rc2-edge-map", rc2::probeEdgeMap, true },
        { "probe-rc2", rc2::probeRC2, true },
        { "probe-rc2-regress", rc2::probeRegress, true },
        { "probe-rc2-hops", rc2::probeHops, true },
        { "probe-rc2-click", rc2::probeClickAt, true },
        { "probe-rc2-freeze", rc2::probeFreezeAt, true },
        { "sputter", testSputter },
        { "slides", testSlides },
        { "edge-rule", testEdgeRule },
        { "probe-instant-grace", probeInstantGrace, true },
        { "probe-sputter", probeSputter },
        { "probe-sputter-full", probeSputterFull, true },
        { "probe-sputter-recordings", probeSputterRecordings, true },
        { "chord-hint", testChordHint },
        { "probe-chord", probeChordThreshold }, // also part of the default run: it's quick
        { "hold", testHold },
        { "hold-recovery", testHoldRecovery },
        { "crossfades", testSwitchingCrossfades },
    };
}

// ---------------------------------------------------------------------------
// CPU benchmark: 48 kHz, 64-sample blocks, guitar-like slide phrase.
// ---------------------------------------------------------------------------
void runBench()
{
    juce::ScopedNoDenormals noDenormals;

    // Five 4 s plucks, each sliding up 5 semitones and back (sustained, like slide guitar). G3, A3, B3:
    // inside every window, so the voice (and the chord) sounds throughout (until v0.5 they started at
    // A2, below the playable zone, so the voice never opened).
    auto phrase = [] (double fs, double seconds) {
        std::vector<float> x;
        for (int k = 0; k * 4.0 < seconds; ++k)
        {
            const double base = 196.0 * std::pow (2.0, (k % 3) * 2.0 / 12.0);
            auto hz = [base] (double t) { return base * std::pow (2.0, 5.0 / 12.0 * std::sin (M_PI * std::min (t, 3.0) / 3.0)); };
            auto pl = ksPluck (hz, 4.0, 0.5f, ksLossFor (base, 12.0), (uint32_t) (50 + k), fs);
            x.insert (x.end(), pl.begin(), pl.end());
        }
        return x;
    };

    struct Config
    {
        const char* name;
        std::function<void (EngineParams&)> set;
    };
    const Config configs[] = {
        { "Defaults (main saw)", [] (EngineParams&) {} },
        { "Triple Sub", [] (EngineParams& p) { p.octave = Octave::TripleSub; } },
        { "Triple Sub, Sustain 50%", [] (EngineParams& p) {
              p.octave = Octave::TripleSub;
              p.sustainPercent = 50.0f;
          } },
        { "C maj / A min (Stack), Triple Sub", [] (EngineParams& p) {
              setChordChoice (p, harmonizeChoice (0));
              p.octave = Octave::TripleSub;
          } },
        { "Memory, Triple Sub", [] (EngineParams& p) {
              setChordChoice (p, freezeChoice (3));
              p.octave = Octave::TripleSub;
          } },
        { "C maj / A min, Triple Sub, Sus 50", [] (EngineParams& p) {
              setChordChoice (p, harmonizeChoice (0));
              p.octave = Octave::TripleSub;
              p.sustainPercent = 50.0f;
          } },
        { "Folded (v0.5.8)", [] (EngineParams& p) { p.wave = Wave::Folded; } },
        { "Folded, C maj / A min, Triple Sub", [] (EngineParams& p) {
              p.wave = Wave::Folded;
              setChordChoice (p, harmonizeChoice (0));
              p.octave = Octave::TripleSub;
          } },
    };

    for (double fs : { 48000.0, 96000.0 })
    {
        const int block = 64;
        const double seconds = 20.0;
        auto in = phrase (fs, seconds);
        std::vector<float> out (in.size());
        std::printf ("\nCPU @ %.0f kHz, %d-sample blocks (%.2f ms), %.0f s of slide phrase; %% of one core, best of 3 runs\n", fs / 1000.0,
                     block, 1000.0 * block / fs, seconds);

        for (const auto& z : { zoneSettings().front(), zoneSettings().back() }) // both ends of the window range
            for (auto& c : configs)
            {
                double bestTotal = 1e30, worstBlockOfBest = 0.0;
                for (int rep = 0; rep < 3; ++rep)
                {
                    auto p = z.p;
                    c.set (p);
                    Engine e;
                    e.prepare (fs, block);
                    double total = 0.0, worstBlock = 0.0;
                    for (size_t start = 0; start + (size_t) block <= in.size(); start += (size_t) block)
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        e.process (in.data() + start, out.data() + start, block, p);
                        const double dt = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
                        total += dt;
                        worstBlock = std::max (worstBlock, dt);
                    }
                    if (total < bestTotal)
                    {
                        bestTotal = total;
                        worstBlockOfBest = worstBlock;
                    }
                }
                const double audioSeconds = (double) in.size() / fs;
                std::printf ("  %-22s  %-38s avg %5.2f %%   worst block %5.1f %% of its deadline\n", z.name, c.name,
                             100.0 * bestTotal / audioSeconds, 100.0 * worstBlockOfBest / (block / fs));
            }

        // The detector on its own (spec budget: under ~3% of one core at 48 kHz).
        {
            PitchDetector d;
            d.prepare (fs, block);
            d.setWindow (derivedWindowMinHz());
            History h;
            h.prepare (4 * maxTauFor (fs));
            const int hop = hopFor (fs);
            double total = 0.0;
            for (size_t n = 0; n < in.size(); ++n)
            {
                h.push (in[n]);
                if ((n + 1) % (size_t) hop == 0)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    d.analyse (h, h.newest(), 0.85f);
                    total += std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
                }
            }
            std::printf ("  window %6.2f Hz  %-38s avg %5.2f %%\n", derivedWindowMinHz(), "YIN detector alone", 100.0 * total / ((double) in.size() / fs));
        }

        // v0.5.4's low-range display detector (message thread, only while an editor is open; not on the audio thread).
        {
            LowRangeDetector d;
            d.prepare (fs);
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t start = 0; start + (size_t) block <= in.size(); start += (size_t) block)
                d.push (in.data() + start, block);
            const double total = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            std::printf ("  window %6.2f Hz  %-38s avg %5.2f %% (message thread)\n", lowRangeWindowHz(), "low-range display detector", 100.0 * total / ((double) in.size() / fs));
        }
    }
}

} // namespace

int main (int argc, char** argv)
{
    juce::String only;
    bool bench = false;
    int golden = 0; // 1: --golden (compare with Tests/golden/manifest.json), 2: --golden-write
    int gridMode = 0; // 1: --grid <file> (compare), 2: --grid-write <file>
    juce::File gridFile;
    outDir = juce::File::getCurrentWorkingDirectory().getChildFile ("test-output");
    // GTT_BLIP_FIXES=0 runs every engine without v0.5.10's blip fixes (v0.5.9's behaviour), to see what they move.
    if (juce::SystemStats::getEnvironmentVariable ("GTT_BLIP_FIXES", {}) == "0")
    {
        Engine::defaultPickSwitches = {};
        std::printf ("GTT_BLIP_FIXES=0: every engine runs without the blip fixes (v0.5.9)\n");
    }
    // GTT_PICK_GLIDE=0 runs every engine without v0.5.11's pick glide (v0.5.10's behaviour).
    if (juce::SystemStats::getEnvironmentVariable ("GTT_PICK_GLIDE", {}) == "0")
    {
        Engine::defaultPickGlide = false;
        std::printf ("GTT_PICK_GLIDE=0: every engine runs without the pick glide (v0.5.10)\n");
    }

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--only" && i + 1 < argc)
            only = argv[++i];
        else if (a == "--bench")
            bench = true;
        else if (a == "--golden")
            golden = 1;
        else if (a == "--golden-write")
            golden = 2;
        else if ((a == "--grid" || a == "--grid-write") && i + 1 < argc)
        {
            gridMode = a == "--grid" ? 1 : 2;
            gridFile = juce::File::getCurrentWorkingDirectory().getChildFile (argv[++i]);
        }
        else
            outDir = juce::File::getCurrentWorkingDirectory().getChildFile (a);
    }
    outDir.createDirectory();
    std::printf ("ShortOrder offline tests -> %s\n", outDir.getFullPathName().toRawUTF8());

    // RC1's golden renders (Tests/Golden.inc): on their own, so they're quick to run after every commit.
    if (golden != 0)
    {
        golden::selfTest();
        std::printf ("\n[golden%s]\n", golden == 2 ? "-write" : "");
        const int r = golden == 2 ? golden::write() : golden::compare();
        std::printf ("\n%s\n", r == 0 ? "GOLDEN OK" : "GOLDEN FAILED");
        return r;
    }

    // v0.6.3: the grid (Tests/Grid.inc), the golden renders' companion at 44.1 / 48 / 96 kHz on the synthetic phrases.
    if (gridMode != 0)
    {
        std::printf ("\n[grid%s]\n", gridMode == 2 ? "-write" : "");
        const int r = gridMode == 2 ? grid::write (gridFile) : grid::compare (gridFile);
        std::printf ("\n%s\n", r == 0 ? "GRID OK" : "GRID FAILED");
        return r;
    }

    if (! bench || only.isNotEmpty())
    {
        for (auto& t : allTests())
        {
            if (only.isNotEmpty() ? only != t.name : t.onRequest)
                continue;
            std::printf ("\n[%s]\n", t.name);
            t.fn();
        }
    }

    if (bench)
        runBench();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
