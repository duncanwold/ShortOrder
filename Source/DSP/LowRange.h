#pragma once

#include "DspUtils.h"
#include "Macro.h"
#include "PitchDetector.h"

namespace gtt
{

// What the low-range display detector hears: a note (MIDI) while voiced.
struct LowRangeReading
{
    bool voiced = false;        // a single note: periodic (the detector's voicing) and its fundamental present
    bool periodic = false;      // the detector's voicing alone
    int note = 0;
    float f0 = 0.0f, confidence = 0.0f;
    float fundamentalDb = -200.0f; // the fundamental's power re. the strongest of the first kHarmonics harmonics
};

// v0.5.4: a second pitch detector for the display only, so the fretboard and readout can show where the player is
// below the playable zone. The same YIN detector with a window whose lowest reliable note is C2 (lowRangeWindowHz():
// about 64 Hz, 31 ms at 48 kHz), run every four of the engine's hops on the guitar input.
//  - Single notes only: two notes together (a 4th, a 5th, a 6th) repeat at their common period, which the long window
//    finds as a confident note well below either (B3 + E4 reads E2). A played note has energy at its fundamental and that
//    period has none, so a reading counts only if the fundamental is within kMinFundamentalDb of the strongest of its
//    first harmonics (Goertzel on the analysis window, its mean removed, Hann-weighted). On synthetic plucks single notes
//    read 0 .. -9 dB (-24 at worst), intervals' common periods -37 dB and below.
//  - It never feeds the synth: the
// editor runs it on the message thread from the scope's samples (ScopeFifo), so it costs the audio thread nothing and
// runs only while an editor is open. prepare() allocates; push() doesn't.
class LowRangeDetector
{
public:
    static constexpr int kHopsPerAnalysis = 4;
    static constexpr int kHarmonics = 6;
    static constexpr float kMinFundamentalDb = -30.0f;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        const double window = lowRangeWindowHz();
        detector.prepare (fs, 0, window);
        detector.setWindow (window);
        history.prepare (2 * detector.windowLength());
        hann.resize ((size_t) detector.windowLength());
        for (size_t n = 0; n < hann.size(); ++n)
            hann[n] = (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * ((double) n + 0.5) / (double) hann.size()));
        hop = kHopsPerAnalysis * hopFor (fs);
        reset();
    }

    void reset()
    {
        history.reset();
        detector.reset();
        counter = 0;
        last = {};
    }

    // Guitar input samples, in order. Analyses the newest window every hop.
    void push (const float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            history.push (x[i]);
            if (++counter >= hop)
            {
                counter = 0;
                analyse();
            }
        }
    }

    const LowRangeReading& reading() const { return last; }
    double sampleRate() const { return fs; }
    int windowLength() const { return detector.windowLength(); }
    int hopSize() const { return hop; }

private:
    void analyse()
    {
        if (history.size() < detector.windowLength())
            return;
        const auto r = detector.analyse (history, history.newest(), kConfidence);
        last.periodic = r.voiced;
        last.f0 = r.f0;
        last.confidence = r.confidence;
        last.note = r.f0 > 0.0f ? nearestNote (r.f0) : 0;
        last.fundamentalDb = r.voiced ? fundamentalDb (r.f0) : -200.0f;
        last.voiced = r.voiced && last.fundamentalDb >= kMinFundamentalDb;
    }

    // Power at f0 re. the strongest of f0 .. kHarmonics x f0 (below 0.45 fs), over the analysis window.
    float fundamentalDb (double f0) const
    {
        const int L = detector.windowLength();
        const int64_t start = history.newest() - L + 1;
        double mean = 0.0; // without DC, which the window would spread over the lowest notes
        for (int n = 0; n < L; ++n)
            mean += history.at (start + n);
        mean /= L;
        double first = 0.0, strongest = 0.0;
        for (int k = 1; k <= kHarmonics && k * f0 < 0.45 * fs; ++k)
        {
            const double w = 2.0 * M_PI * k * f0 / fs, c = 2.0 * std::cos (w);
            double s1 = 0.0, s2 = 0.0;
            for (int n = 0; n < L; ++n)
            {
                const double s0 = hann[(size_t) n] * (history.at (start + n) - mean) + c * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            const double power = s1 * s1 + s2 * s2 - c * s1 * s2;
            if (k == 1)
                first = power;
            strongest = std::max (strongest, power);
        }
        return strongest > 0.0 ? (float) (10.0 * std::log10 (std::max (first, 1e-30) / strongest)) : -200.0f;
    }

    double fs = 48000.0;
    int hop = 512, counter = 0;
    History history;
    PitchDetector detector;
    std::vector<float> hann; // over the analysis window
    LowRangeReading last;
};

} // namespace gtt
