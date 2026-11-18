#pragma once

#include "DspUtils.h"
#include "Macro.h"

namespace gtt
{

// Oscillator waveforms on a phase p in [0, 1) that advances by dt (cycles per sample).
// Saw, square and the pulses are PolyBLEP band-limited, the triangle and the half-sine PolyBLAMP band-limited; sine and organ
// are plain (sums of sines). Folded is PolyBLAMP band-limited at every fold and run at twice the rate (FoldOversampler).

// Two-sample polynomial BLEP residual, for a step of 2.
inline double polyBlep (double t, double dt)
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

// Two-sample polynomial BLAMP residual: the BLEP above integrated once, for a slope change
// of +1 per sample at t = 0. (x + 1)^3 / 6 just before the corner, (1 - x)^3 / 6 just after.
inline double polyBlamp (double t, double dt)
{
    if (t < dt)
    {
        const double x = 1.0 - t / dt;
        return x * x * x / 6.0;
    }
    if (t > 1.0 - dt)
    {
        const double x = (t - 1.0) / dt + 1.0;
        return x * x * x / 6.0;
    }
    return 0.0;
}

// The NES's thinner pulse (v0.5.7; NES Lead): high for the first `duty` of the cycle, PolyBLEP at both edges as Square. Unlike
// Square it carries DC (its mean is 2 duty - 1), which is taken off and the result scaled to peak at +1 (it runs
// from -duty / (1 - duty) to +1), so every waveform still stays within +/-1 (the headroom gain's bound).
constexpr double kPulse25Duty = 0.25;
inline double pulseSample (double duty, double p, double dt)
{
    double p2 = p + (1.0 - duty);
    if (p2 >= 1.0)
        p2 -= 1.0;
    const double y = (p < duty ? 1.0 : -1.0) + polyBlep (p, dt) - polyBlep (p2, dt);
    return (y - (2.0 * duty - 1.0)) / (2.0 - 2.0 * duty);
}

// +1 at p = 0, -1 at p = 0.5: slope -4 then +4 per cycle.
inline double naiveTriangle (double p) { return 4.0 * std::abs (p - 0.5) - 1.0; }

// Half-Sine (v0.5.8): a half-wave rectified sine, sin (2 pi p) for the first half of the cycle and 0 for the second: the
// fundamental and the even harmonics only (falling as 1 / (4k^2 - 1)). Its two corners (p = 0 and 0.5) each turn the slope
// up by 2 pi per cycle, so each gets a PolyBLAMP as the triangle's (the two corrections together repeat every half cycle,
// so they add even harmonics only). PolyBLAMP's residual is one-sided, adding pi dt^2 / 3 of DC on average (1/12 of the
// corner's slope change per corner), which is taken off with the wave's own DC (1 / pi); then scaled to peak at +1, as the
// pulses: -1 / (pi - 1) (-0.467) .. +1.
constexpr double kHalfSineDc = 1.0 / M_PI;
inline double halfSineSample (double p, double dt)
{
    double p2 = p + 0.5;
    if (p2 >= 1.0)
        p2 -= 1.0;
    const double naive = p < 0.5 ? std::sin (2.0 * M_PI * p) : 0.0;
    const double y = naive + 2.0 * M_PI * dt * (polyBlamp (p, dt) + polyBlamp (p2, dt)) - M_PI * dt * dt / 3.0;
    return (y - kHalfSineDc) / (1.0 - kHalfSineDc);
}

// Organ (v0.5.8): sines at 1, 2 and 3 times the frequency on the one phase (so phase-locked, and Octave moves the whole
// tone), at kOrganLevel1 / 2 / 3 (Macro.h), divided by the sum's peak so it peaks at +/-1. No DC (sines only); all three
// are far below Nyquist at every note (3 x 1400 Hz = 4.2 kHz), so it needs no band-limiting.
inline double organRaw (double p)
{
    const double s = std::sin (2.0 * M_PI * p), c = std::cos (2.0 * M_PI * p);
    return kOrganLevel1 * s + kOrganLevel2 * 2.0 * s * c + kOrganLevel3 * s * (3.0 - 4.0 * s * s); // sin 2x = 2 sin cos, sin 3x = 3 sin - 4 sin^3
}
// The sum's peak (it's an odd function of the phase about 0, so its trough is minus this): the largest of a fine grid,
// refined by Newton on the derivative. Computed once, when the program loads.
inline double organPeakFor()
{
    constexpr int n = 4096;
    int best = 0;
    for (int k = 1; k < n; ++k)
        if (std::abs (organRaw ((double) k / n)) > std::abs (organRaw ((double) best / n)))
            best = k;
    double p = (double) best / n;
    const double h = 1.0e-5;
    for (int it = 0; it < 30; ++it)
    {
        const double d1 = (organRaw (p + h) - organRaw (p - h)) / (2.0 * h), d2 = (organRaw (p + h) - 2.0 * organRaw (p) + organRaw (p - h)) / (h * h);
        if (d2 == 0.0)
            break;
        p -= d1 / d2;
    }
    return std::max (std::abs (organRaw (p)), std::abs (organRaw ((double) best / n)));
}
inline const double kOrganPeak = organPeakFor();
inline double organSample (double p) { return organRaw (p) / kOrganPeak; }

// The mean |x| of a wave over one cycle (its level scale's starting point, for the waves without a closed form), from a fine grid.
template <typename Fn>
inline double meanAbsOf (Fn wave)
{
    constexpr int n = 1 << 16;
    double sum = 0.0;
    for (int k = 0; k < n; ++k)
        sum += std::abs (wave ((k + 0.5) / n));
    return sum / n;
}
// Folded (v0.5.8): a sine through a wavefolder whose drive (1 .. kFoldMaxDrive, Macro.h) follows the note's level. The folder
// is a triangle folder (x for |x| <= 1, then reflected at +/-1 again and again), so a sine of amplitude d gains a fold on each
// side for every 2 of d above 1; bounded at +/-1.
inline double foldTriangle (double x)
{
    const double u = 0.25 * (x + 1.0);
    return 1.0 - 4.0 * std::abs (u - std::floor (u) - 0.5);
}
// One triangle folder's brightness (spectral centroid) isn't monotonic in its drive: it dips by about 20% just below every
// odd drive (2.5 -> 3.25, 4.5 -> 5), where the crest folds right over and the 3rd harmonic takes the power, so a decaying hard
// note would brighten again on its way down. Three in parallel at kFoldStageScale of the drive (Buchla-style), each weighted
// by its own scale, rise steadily instead (with the 5 kHz filter, at E4: 330 Hz at drive 1, 456 at 2, 1035 at 3, 1321 at 4,
// 1869 at 5, never falling). The sum is divided by sum (scale^2), so at drive 1 it's exactly a sine of amplitude 1; it never
// passes +/-1 at any drive up to 6 (test folded checks it; a clamp guards it anyway).
inline double foldStages (double drive, double sine)
{
    double y = 0.0;
    for (double sc : kFoldStageScale)
        y += sc * foldTriangle (drive * sc * sine);
    return y / kFoldStageNorm;
}
// The folds' corners at a drive: where a stage's drive x sin (2 pi p) crosses an odd m, its folder's slope flips, so the output's
// slope changes by -2 fold(m) |dx/dp| x (its weight) = -4 pi fold(m) sqrt (d^2 - m^2) sc / norm per cycle, always toward the
// inside (every corner is at that stage's +/-1). Each gets a PolyBLAMP, as the triangle's. Computed once per sample by the
// engine and shared by every tone (the chord's included: the same drive as the live note).
struct FoldShape
{
    static constexpr int kMaxCorners = 32; // drive up to 7: 12 + 8 + 8 corners (stages at 1, 0.7, 0.5)
    double drive = 1.0;
    int corners = 0;
    double at[kMaxCorners] {};   // the corners' phases, 0 .. 1
    double jump[kMaxCorners] {}; // the slope change at each, per cycle
    void set (double newDrive)
    {
        drive = newDrive;
        corners = 0;
        for (double sc : kFoldStageScale)
        {
            const double d = newDrive * sc;
            for (int m = 1; m < d && corners + 4 <= kMaxCorners; m += 2)
            {
                const double a = std::asin (m / d) / (2.0 * M_PI); // 0 .. 1/4
                const double j = 4.0 * M_PI * std::sqrt (d * d - (double) (m * m)) * sc / kFoldStageNorm * (m % 4 == 1 ? -1.0 : 1.0); // fold(m) = +1 at m = 1, 5; -1 at 3
                const double ps[4] = { a, 0.5 - a, 0.5 + a, 1.0 - a }; // +m going up, +m coming down, -m going down, -m coming up
                const double js[4] = { j, j, -j, -j };
                for (int k = 0; k < 4; ++k)
                {
                    at[corners] = ps[k];
                    jump[corners] = js[k];
                    ++corners;
                }
            }
        }
    }
};
inline double foldedSample (const FoldShape& f, double p, double dt)
{
    double y = foldStages (f.drive, std::sin (2.0 * M_PI * p));
    for (int i = 0; i < f.corners; ++i)
    {
        double t = p - f.at[i];
        if (t < 0.0)
            t += 1.0;
        if (t < dt || t > 1.0 - dt)
            y += f.jump[i] * dt * polyBlamp (t, dt);
    }
    return std::clamp (y, -1.0, 1.0);
}
// Twice the rate: two folded samples per output sample (at p and p + dt / 2, each with PolyBLAMP at the doubled rate), then
// the 7-tap binomial filter (1 6 15 20 15 6 1) / 64 and every second sample. Its taps are all positive and sum to 1, so the
// output stays within +/-1 (the headroom gain's bound holds as for every other wave); it's -0.7 dB at 5 kHz, -3 dB at 10 kHz
// (48 kHz; under the 5 kHz filter anyway). Delay: 3 samples at the doubled rate (1.5 at the output's). Plain data (the
// history is an in-place array), so a Tone stays copyable without allocating.
struct FoldOversampler
{
    static constexpr int kTaps = 7;
    float x[kTaps] {}; // the doubled rate's newest samples, oldest first
    void clear() { std::fill (std::begin (x), std::end (x), 0.0f); }
    float next (const FoldShape& f, double p, double dt)
    {
        const double half = 0.5 * dt;
        double p1 = p + half;
        if (p1 >= 1.0)
            p1 -= 1.0;
        std::copy (x + 2, x + kTaps, x);
        x[kTaps - 2] = (float) foldedSample (f, p, half);
        x[kTaps - 1] = (float) foldedSample (f, p1, half);
        return (float) (((double) x[0] + x[6] + 6.0 * ((double) x[1] + x[5]) + 15.0 * ((double) x[2] + x[4]) + 20.0 * x[3]) / 64.0);
    }
};

inline const double kHalfSineMeanAbs = meanAbsOf ([] (double p) { return ((p < 0.5 ? std::sin (2.0 * M_PI * p) : 0.0) - kHalfSineDc) / (1.0 - kHalfSineDc); });
inline const double kOrganMeanAbs = meanAbsOf (organSample);

inline double oscSample (Wave w, double p, double dt)
{
    switch (w)
    {
        case Wave::Saw:
            return 2.0 * p - 1.0 - polyBlep (p, dt);
        case Wave::Square:
        {
            double p2 = p + 0.5;
            if (p2 >= 1.0)
                p2 -= 1.0;
            return (p < 0.5 ? 1.0 : -1.0) + polyBlep (p, dt) - polyBlep (p2, dt);
        }
        case Wave::Pulse25:
            return pulseSample (kPulse25Duty, p, dt);
        case Wave::HalfSine:
            return halfSineSample (p, dt);
        case Wave::Organ:
            return organSample (p);
        case Wave::Folded: // (bare, at drive 1: a sine; Tone renders the drive, with FoldOversampler)
            return foldedSample (FoldShape {}, p, dt);
        case Wave::Triangle:
        {
            // Corners: the slope changes by -8 dt per sample at the peak (p = 0) and by
            // +8 dt at the trough (p = 0.5).
            double p2 = p + 0.5;
            if (p2 >= 1.0)
                p2 -= 1.0;
            return naiveTriangle (p) + 8.0 * dt * (polyBlamp (p2, dt) - polyBlamp (p, dt));
        }
        case Wave::Sine:
        default:
            return std::sin (2.0 * M_PI * p);
    }
}

// Each wave's level against Saw, in dB (v0.5.1): matched by loudness, ITU-R BS.1770 (K-weighted
// LUFS), over every note G3 .. E5 at Octave Main and the default Low Pass Filter (test
// wave-loudness; on the mean-|x| scale alone they measured -1.37 / -0.32 / -0.65 LU under Saw, the same
// within 0.1 dB at every note). Saw is the reference and stays as it was.
constexpr float kSquareLoudnessDb = 1.37f, kTriangleLoudnessDb = 0.32f, kSineLoudnessDb = 0.65f;
// v0.5.2: Triangle and Sine raised by ear on top of that match. BS.1770 has no spectral loudness
// summation, so a pure tone at the same LUFS sounds much weaker than a bright saw (CLAUDE.md, v0.5.1).
constexpr float kTriangleByEarDb = 10.0f, kSineByEarDb = 10.0f;
// v0.5.7: NES Lead (Pulse 25%) matched to Saw the same way (BS.1770, G3 .. E5, Octave Main; test pulse-waves). On its mean-|x|
// scale it measured -0.02 LU re. Saw, within 0.3 dB at every note and rate.
constexpr float kPulse25LoudnessDb = 0.02f;
// v0.5.8: Half-Sine and Organ the same way (test half-sine-organ). On their mean-|x| scales they measured -0.69 / -0.08 LU
// re. Saw (within 0.15 LU at every note G3 .. E5 and rate).
constexpr float kHalfSineLoudnessDb = 0.70f, kOrganLoudnessDb = 0.08f;
// v0.5.8: Folded, on the sine's mean-|x| scale (it's a sine at drive 1), matched to Saw at a medium pick (the engine at the
// plugin's defaults, single 0.5-peak plucks G3 .. E5; test folded): v0.5.8 measured -0.31 LU re. Saw there, +0.31; v0.5.9's
// drive (before Dynamics, up to 6) -0.34 LU (mean, before the gain), +0.34. Folding doesn't add loudness: the bare folder at E4
// measures -0.31 / -0.15 / -1.77 / -1.80 / -2.30 / -2.96 LU re. Saw at drive 1 / 2 / 3 / 4 / 5 / 6 (on this scale, with this
// gain; the folds lower its RMS more than K-weighting's high shelf adds), so a hard pick comes out under Saw's (test folded).
constexpr float kFoldedLoudnessDb = 0.34f;

// Scale each waveform to a mean-absolute value of 1 (v0.1 .. v0.5), then to Saw's loudness (v0.5.1), then
// Triangle and Sine by ear (v0.5.2). levels (tests only): stop after an earlier step.
inline float waveScale (Wave w, WaveLevels levels = WaveLevels::Current)
{
    const bool matched = levels != WaveLevels::MeanAbs, byEar = levels == WaveLevels::Current;
    switch (w)
    {
        case Wave::Saw: return 2.0f;                                                                // mean |saw| = 1/2
        case Wave::Square: return 1.0f * (matched ? dbToGain (kSquareLoudnessDb) : 1.0f);            // mean |square| = 1
        case Wave::Pulse25: return 2.0f * (matched ? dbToGain (kPulse25LoudnessDb) : 1.0f);          // mean |pulse| = 2 duty (as pulseSample scales it)
        case Wave::HalfSine: return (float) (1.0 / kHalfSineMeanAbs) * (matched ? dbToGain (kHalfSineLoudnessDb) : 1.0f);
        case Wave::Organ: return (float) (1.0 / kOrganMeanAbs) * (matched ? dbToGain (kOrganLoudnessDb) : 1.0f);
        case Wave::Folded: return (float) (M_PI / 2.0) * (matched ? dbToGain (kFoldedLoudnessDb) : 1.0f);
        case Wave::Triangle:                                                                        // mean |triangle| = 1/2
            return 2.0f * (matched ? dbToGain (kTriangleLoudnessDb) : 1.0f) * (byEar ? dbToGain (kTriangleByEarDb) : 1.0f);
        case Wave::Sine:                                                                            // mean |sine| = 2/pi
        default: return (float) (M_PI / 2.0) * (matched ? dbToGain (kSineLoudnessDb) : 1.0f) * (byEar ? dbToGain (kSineByEarDb) : 1.0f);
    }
}

} // namespace gtt
