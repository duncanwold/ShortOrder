#pragma once

#include "DspUtils.h"
#include <juce_dsp/juce_dsp.h>
#include <memory>

namespace gtt
{

// Rejects single-hop jumps of >= 11 semitones: a jump is only accepted when the
// next hop agrees with it (within 1 semitone). Only active inside a voiced run.
class OctaveGuard
{
public:
    void reset() { haveLast = false; havePending = false; }

    // Returns true if the estimate should be accepted. When it returns false the
    // caller keeps the previously accepted estimate for this hop.
    bool accept (bool voiced, double midi)
    {
        if (! voiced)
        {
            reset();
            return true;
        }
        if (! haveLast || std::abs (midi - lastMidi) < 11.0)
        {
            accepted (midi);
            return true;
        }
        if (havePending && std::abs (midi - pendingMidi) < 1.0)
        {
            accepted (midi);
            return true;
        }
        havePending = true;
        pendingMidi = midi;
        return false;
    }

private:
    void accepted (double midi)
    {
        haveLast = true;
        lastMidi = midi;
        havePending = false;
    }

    bool haveLast = false, havePending = false;
    double lastMidi = 0.0, pendingMidi = 0.0;
};

constexpr float kVoicedRmsFloor = 1.0e-3f; // -60 dBFS: a window quieter than this is never voiced

struct DetectorResult
{
    bool voiced = false;
    float f0 = 0.0f;         // Hz (0 if never had an estimate)
    float period = 0.0f;     // samples
    float confidence = 0.0f; // 1 - CMND(tau*)
    float rms = 0.0f;        // over the analysed window
    bool guardRejected = false;
    // v0.6.2, the bottom edge (display of where the dip sits; analyse() never changes the reading for it): the dip is at the longest
    // lag searched (tauMax - 1) and still falling there (the difference function lower at tauMax), so the reading is within half a
    // sample of fs / tauMax, the window's own lowest frequency. beyondCents: the parabola's vertex past tauMax (unclamped), in cents
    // below fs / tauMax (0 when it isn't past it): an estimate of how far under the window the dip really is.
    bool atLongEdge = false;
    float beyondCents = 0.0f;
};

// YIN over the newest L samples of a History, with the difference function
// computed from an FFT cross-correlation plus running energy terms.
class PitchDetector
{
public:
    // minWindowHz: the longest window to allocate for. The plugin uses the playable zone's
    // minimum; the offline tests also build a measuring detector with longer windows.
    void prepare (double sampleRate, int maxBlockSize, double minWindowHz = derivedWindowMinHz());
    void reset();
    // Sets tauMax = round(fs / windowHz) and L = 2 * tauMax (windowHz clamped to minWindowHz .. 300).
    void setWindow (double windowHz);

    // Analyse the L samples ending at newestIdx (inclusive). rmsFloor: tests only (investigate-sputter's sticky voicing).
    DetectorResult analyse (const History& h, int64_t newestIdx, float confidenceThreshold, float rmsFloor = kVoicedRmsFloor);

    int windowLength() const { return L; } // L = 2 * tauMax
    int tauMax() const { return tMax; }
    int tauMin() const { return tMin; }
    // Tests only (v0.5.11, the ceiling investigation): the highest pitch searched (the shortest lag, floor (fs / hz)); prepare()
    // sets kDetectorMaxHz's.
    void setMaxHz (double hz) { tMin = std::max (2, (int) std::floor (fs / hz)); }

private:
    double fs = 48000.0;
    double windowHz = kWindowMaxHz, minWindow = kWindowMaxHz;
    int L = 0, tMax = 0, tMin = 2, fftSize = 0;
    juce::dsp::FFT* fft = nullptr;
    static constexpr int kMaxOrder = 16;
    std::unique_ptr<juce::dsp::FFT> ffts[kMaxOrder + 1]; // by order; only the orders the window range needs

    std::vector<float> x, bufA, bufB, cmnd;
    std::vector<double> cum, diff;

    OctaveGuard guard;
    DetectorResult last;
};

} // namespace gtt
