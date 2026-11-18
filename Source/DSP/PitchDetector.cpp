#include "PitchDetector.h"

namespace gtt
{

void PitchDetector::prepare (double sampleRate, int /*maxBlockSize*/, double minWindowHz)
{
    fs = sampleRate;
    minWindow = std::min (minWindowHz, kWindowMaxHz);
    tMin = std::max (2, (int) std::floor (fs / kDetectorMaxHz));

    // Size every buffer for the largest L (the minimum window) at max(fs, 96 kHz), and
    // create an FFT for every size the window range can need at this rate.
    const int maxL = 2 * maxTauFor (fs, minWindow);
    const int maxN = nextPow2 (maxL);

    for (auto& f : ffts)
        f.reset();
    const int loOrder = (int) std::log2 (nextPow2 (latencyFor (fs, kWindowMaxHz)));
    const int hiOrder = std::min (kMaxOrder, (int) std::log2 (nextPow2 (2 * (int) std::lround (fs / minWindow))));
    for (int order = loOrder; order <= hiOrder; ++order)
        ffts[order] = std::make_unique<juce::dsp::FFT> (order);

    x.assign ((size_t) maxL, 0.0f);
    bufA.assign ((size_t) (2 * maxN), 0.0f);
    bufB.assign ((size_t) (2 * maxN), 0.0f);
    cmnd.assign ((size_t) (maxL / 2 + 2), 1.0f);
    cum.assign ((size_t) (maxL + 1), 0.0);
    diff.assign ((size_t) (maxL / 2 + 2), 0.0);

    setWindow (windowHz);
    reset();
}

void PitchDetector::reset()
{
    guard.reset();
    last = {};
}

void PitchDetector::setWindow (double hz)
{
    windowHz = std::clamp (hz, minWindow, kWindowMaxHz);
    tMax = (int) std::lround (fs / windowHz);
    L = 2 * tMax;
    fft = ffts[(int) std::log2 (nextPow2 (L))].get();
    fftSize = fft != nullptr ? fft->getSize() : 0;
}

DetectorResult PitchDetector::analyse (const History& h, int64_t newestIdx, float threshold, float rmsFloor)
{
    const int W = tMax;
    const int64_t start = newestIdx - L + 1;

    // Window x[0..L-1], oldest first, plus running energy prefix sums.
    cum[0] = 0.0;
    for (int j = 0; j < L; ++j)
    {
        const float v = h.at (start + j);
        x[(size_t) j] = v;
        cum[(size_t) j + 1] = cum[(size_t) j] + (double) v * v;
    }

    // r(tau) = sum_{j<W} x[j] x[j+tau]  via  IFFT(conj(FFT(a)) * FFT(b)).
    // a = first W samples zero-padded, b = all L samples zero-padded; N >= L so no wrap for tau <= W.
    const int N = fftSize;
    std::fill (bufA.begin(), bufA.begin() + 2 * N, 0.0f);
    std::fill (bufB.begin(), bufB.begin() + 2 * N, 0.0f);
    std::copy (x.begin(), x.begin() + W, bufA.begin());
    std::copy (x.begin(), x.begin() + L, bufB.begin());
    fft->performRealOnlyForwardTransform (bufA.data(), true);
    fft->performRealOnlyForwardTransform (bufB.data(), true);
    for (int k = 0; k <= N / 2; ++k)
    {
        const float ar = bufA[(size_t) (2 * k)], ai = bufA[(size_t) (2 * k + 1)];
        const float br = bufB[(size_t) (2 * k)], bi = bufB[(size_t) (2 * k + 1)];
        bufA[(size_t) (2 * k)] = ar * br + ai * bi;     // Re(conj(A) * B)
        bufA[(size_t) (2 * k + 1)] = ar * bi - ai * br; // Im(conj(A) * B)
    }
    fft->performRealOnlyInverseTransform (bufA.data());

    // Difference function d(tau) = e(0) + e(tau) - 2 r(tau), then CMND.
    const double e0 = cum[(size_t) W];
    double running = 0.0;
    cmnd[0] = 1.0f;
    diff[0] = 0.0;
    for (int tau = 1; tau <= W; ++tau)
    {
        const double eTau = cum[(size_t) (tau + W)] - cum[(size_t) tau];
        const double d = std::max (0.0, e0 + eTau - 2.0 * (double) bufA[(size_t) tau]);
        diff[(size_t) tau] = d;
        running += d;
        cmnd[(size_t) tau] = running > 0.0 ? (float) (d * tau / running) : 1.0f;
    }

    // First dip below 0.15 (then down to its local minimum), else the global minimum.
    const int lo = std::min (tMin, W - 1), hi = W - 1;
    int best = -1;
    for (int tau = lo; tau <= hi; ++tau)
    {
        if (cmnd[(size_t) tau] < 0.15f)
        {
            while (tau + 1 <= hi && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau])
                ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0)
    {
        best = lo;
        for (int tau = lo + 1; tau <= hi; ++tau)
            if (cmnd[(size_t) tau] < cmnd[(size_t) best])
                best = tau;
    }

    // Parabolic interpolation on the raw difference function.
    double period = best;
    double vertex = 0.0; // unclamped
    {
        const double a = diff[(size_t) best - 1], b = diff[(size_t) best], c = diff[(size_t) best + 1];
        const double denom = a - 2.0 * b + c;
        if (denom > 0.0)
        {
            vertex = 0.5 * (a - c) / denom;
            period += std::clamp (vertex, -1.0, 1.0);
        }
    }

    DetectorResult r;
    if (best == hi && diff[(size_t) W] < diff[(size_t) hi])
    {
        r.atLongEdge = true;
        if (vertex > 1.0)
            r.beyondCents = (float) (1200.0 * std::log2 (((double) hi + vertex) / (double) W));
    }
    r.confidence = std::clamp (1.0f - cmnd[(size_t) best], 0.0f, 1.0f);
    r.rms = (float) std::sqrt (cum[(size_t) L] / L);
    r.period = (float) period;
    r.f0 = (float) (fs / period);
    r.voiced = r.confidence >= threshold && r.rms > rmsFloor; // -60 dBFS

    if (! guard.accept (r.voiced, midiFromHz (r.f0)))
    {
        // Octave guard: keep the previously accepted pitch for this hop.
        r.f0 = last.f0;
        r.period = last.period;
        r.guardRejected = true;
    }
    else if (r.voiced)
    {
        last = r;
    }
    return r;
}

} // namespace gtt
