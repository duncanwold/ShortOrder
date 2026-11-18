#pragma once

#include "DspUtils.h"

namespace gtt
{

// v0.5.3: the output stage's headroom gain, before the safety clip. The engine predicts the most the
// output could be this sample from what it already knows (each tone's level x its wave's scale,
// Tone::peak; the chord's gain law; the voice gate; the window fade), and this keeps that under
// kHeadroomCeiling. No look-ahead and no latency: the
// prediction is for the sample being made, so the gain is already low enough when a peak arrives.
//  - Down: at once (gain = ceiling / predicted peak). The prediction follows levels that are already
//    smooth (the 3 ms follower, 5 / 15 / 20 ms ramps), so the gain is as smooth.
//  - Up: a one-pole toward the wanted gain with a kHeadroomReleaseMs time constant, so a chord doesn't
//    pump as tones come and go or as the level decays; within 1e-5 of 1 it snaps to exactly 1.
// While nothing comes near the ceiling the gain is exactly 1 and the output is v0.5.2's bit for bit.
// The safety clip stays last, as a backstop.
constexpr float kSafetyClipKnee = 0.9f;                         // safetyClip is linear up to here
constexpr float kHeadroomCeiling = 0.8021265f;                  // 1 dB under the knee: 0.9 x 10^(-1/20)
constexpr double kHeadroomReleaseMs = 150.0;

class HeadroomGain
{
public:
    void prepare (double fs)
    {
        releaseCoef = timeConstantCoef (fs, kHeadroomReleaseMs);
        reset();
    }

    void reset() { g = 1.0f; }

    // predictedPeak: the bound on this sample's size before this gain (>= 0).
    float process (float predictedPeak)
    {
        const float want = predictedPeak > kHeadroomCeiling ? kHeadroomCeiling / predictedPeak : 1.0f;
        if (want < g)
            g = want;
        else if (g < 1.0f)
        {
            g += releaseCoef * (want - g);
            if (want >= 1.0f && g > 1.0f - 1.0e-5f)
                g = 1.0f;
        }
        return g;
    }

    float gain() const { return g; }

private:
    float releaseCoef = 0.0f, g = 1.0f;
};

} // namespace gtt
