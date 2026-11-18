#include "SynthSource.h"
#include <cassert>

namespace gtt
{

void SynthSource::prepare (double sampleRate, int /*maxBlockSize*/)
{
    fs = sampleRate;
    attack = timeConstantCoef (fs, 3.0);
    release = timeConstantCoef (fs, 120.0);
    absRing.assign ((size_t) nextPow2 (maxTauFor (fs) + 4), 0.0f);
    absMask = (int) absRing.size() - 1;
    voice.prepare (fs);
    reset();
}

void SynthSource::reset()
{
    env = 0.0f;
    std::fill (absRing.begin(), absRing.end(), 0.0f);
    absWrite = 0;
    boxLen = 1;
    boxSum = 0.0;
    meanAbs = 0.0f;
    voice.reset();
}

void Tone::prepare (double sampleRate)
{
    fs = sampleRate;
    lpCoef = onePoleCoef (fs, 5000.0);
    rampLen = msToSamples (fs, 20.0);
    reset();
}

void Tone::reset()
{
    phase = 0.0;
    lpY = 0.0f;
    fold.clear();
    lastTarget = -1.0f;
    dt = 0.0;
    cachedCutoff = -1.0f;
    firstSettings = true;
}

void Tone::setVoice (const VoiceSettings& v)
{
    if (v.wave == Wave::Folded && wave != Wave::Folded) // no stale history from an earlier stretch of Folded
        fold.clear();
    wave = v.wave;
    scale = waveScale (wave);
    ratio = octaveRatio (v.octave); // a power of two, exactly

    const float cutoff = std::clamp (v.filterCutoffHz, 200.0f, 8000.0f);
    if (firstSettings)
    {
        cutoffHz.snap (cutoff);
        firstSettings = false;
    }
    cutoffHz.setTarget (cutoff, rampLen);
}

float Tone::filterCoef()
{
    // Fixed cutoff: v0.1's coefficient, computed once per cutoff value (recomputed while the
    // cutoff ramps after a change).
    const float base = cutoffHz.next();
    if (std::not_equal_to<float>() (base, cachedCutoff))
    {
        cachedCutoff = base;
        lpCoef = onePoleCoef (fs, base);
    }
    return lpCoef;
}

float SynthSource::follow (float delayedIn, float periodSamples)
{
    // Envelope: mean |x| over one input period (so the reading is the guitar's
    // mean-absolute level with no per-cycle ripple), then a 3 ms / 120 ms follower.
    // boxSum is the sum of the newest boxLen values of |x|.
    absRing[(size_t) absWrite] = std::abs (delayedIn);
    boxSum += absRing[(size_t) absWrite];
    const int wanted = std::clamp ((int) std::lround (periodSamples), 1, absMask);
    ++boxLen; // the new value is in: the box is offsets 0 .. boxLen - 1 back from absWrite
    while (boxLen < wanted) // grow: take in the next older value
        boxSum += absRing[(size_t) ((absWrite - boxLen++) & absMask)];
    while (boxLen > wanted) // shrink (or slide by one): drop the oldest
        boxSum -= absRing[(size_t) ((absWrite - --boxLen) & absMask)];
    assert (boxSum > -1.0e-6); // only rounding can take a sum of |x| below 0
    absWrite = (absWrite + 1) & absMask;
    meanAbs = (float) std::max (0.0, boxSum / boxLen);

    env += (meanAbs > env ? attack : release) * (meanAbs - env);
    return env;
}

float Tone::render (float targetMidi, float level)
{
    const float coef = filterCoef();

    if (std::not_equal_to<float>() (targetMidi, lastTarget) || std::not_equal_to<double>() (ratio, lastRatio))
    {
        lastTarget = targetMidi;
        lastRatio = ratio;
        dt = std::min (0.45, hzFromMidi (targetMidi) * ratio / fs);
    }

    // The oscillator. Phase-continuous: frequency (note or octave) changes never reset it.
    const bool folded = wave == Wave::Folded;
    float y = folded ? fold.next (foldShape(), phase, dt) : (float) oscSample (wave, phase, dt);
    phase += dt;
    if (phase >= 1.0)
        phase -= 1.0;
    lastPeak = scale * level;

    lpY += coef * (y - lpY); // keep the filter state running so wave switches are smooth
    if (wave != Wave::Sine)
        y = lpY;

    return y * scale * level;
}

} // namespace gtt
