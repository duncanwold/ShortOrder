#pragma once

#include "DspUtils.h"
#include <array>

namespace gtt
{

// Onsets (a pluck, including a re-pluck of a note that's still ringing) and the guitar's level,
// fast. Shared by Sustain (its reference level and restarts) and the quantizer (its onsets). The engine
// runs it on every sample; it never changes the output on its own.
//  - Onsets are found on the undelayed input, L samples ahead of the voice: a fast level (one-period
//    mean |x|, 3 ms attack, 10 ms release) jumping 3 dB above its own 30 ms average (re-armed once
//    it's back within 1 dB). Each is stamped with the sample where that rise began, and the voice
//    takes it when its delayed timeline reaches that sample: before the rise reaches the level it
//    plays, as long as the detection took less than L (a few ms; at the window's top, L is 4.7 ms).
//  - level(): the same fast follower on the delayed timeline (the voice's own one-period mean |x|),
//    so Sustain reads a new, quieter note within a few ms (the envelope follower's 120 ms release
//    can't).
class OnsetDetector
{
public:
    static constexpr double kAttackMs = 3.0, kReleaseMs = 10.0, kAverageMs = 30.0;
    static constexpr float kRiseDb = 3.0f, kRearmDb = 1.0f;

    // maxPeriod: the longest box (samples), as the voice's envelope box.
    void prepare (double fs, int maxPeriod)
    {
        attackCoef = timeConstantCoef (fs, kAttackMs);
        releaseCoef = timeConstantCoef (fs, kReleaseMs);
        averageCoef = timeConstantCoef (fs, kAverageMs);
        riseGain = dbToGain (kRiseDb);
        rearmGain = dbToGain (kRearmDb);
        ring.assign ((size_t) nextPow2 (maxPeriod + 4), 0.0f);
        mask = (int) ring.size() - 1;
        reset();
    }

    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        write = 0;
        boxLen = 1;
        boxSum = 0.0;
        ahead = aheadPrev = average = delayed = 0.0f;
        armed = true;
        riseStart = 0;
        head = tail = 0;
        found = 0;
    }

    // The undelayed input: one sample x at absolute time `time`; period: the latest detected one.
    void push (float x, int64_t time, float period)
    {
        // One-period mean |x| (the box grows or shrinks exactly; see SynthSource::follow).
        ring[(size_t) write] = std::abs (x);
        boxSum += ring[(size_t) write];
        const int wanted = std::clamp ((int) std::lround (period), 1, mask);
        ++boxLen;
        while (boxLen < wanted)
            boxSum += ring[(size_t) ((write - boxLen++) & mask)];
        while (boxLen > wanted)
            boxSum -= ring[(size_t) ((write - --boxLen) & mask)];
        write = (write + 1) & mask;
        const float meanAbs = (float) std::max (0.0, boxSum / boxLen);

        ahead += (meanAbs > ahead ? attackCoef : releaseCoef) * (meanAbs - ahead);
        average += averageCoef * (ahead - average);
        if (ahead <= aheadPrev)
            riseStart = time; // the start of the current rise is the last sample that didn't rise
        aheadPrev = ahead;
        if (armed && ahead > average * riseGain)
        {
            armed = false;
            ++found;
            if (tail - head < (int) events.size())
                events[(size_t) (tail++ % (int) events.size())] = riseStart;
        }
        else if (ahead < average * rearmGain)
        {
            armed = true;
        }
    }

    // The voice's timeline: one sample at time t with its one-period mean |x|. True if an onset is due.
    bool process (int64_t t, float meanAbs)
    {
        delayed += (meanAbs > delayed ? attackCoef : releaseCoef) * (meanAbs - delayed);
        bool due = false;
        while (head < tail && events[(size_t) (head % (int) events.size())] <= t)
        {
            ++head;
            due = true;
        }
        return due;
    }

    float level() const { return delayed; }
    float levelAhead() const { return ahead; } // tests only (investigate-sputter): the same level on the undelayed input
    // Onsets found so far on the undelayed input (the quantizer's note starts, once per hop).
    uint32_t foundCount() const { return found; }

private:
    float attackCoef = 0.0f, releaseCoef = 0.0f, averageCoef = 0.0f, riseGain = 1.0f, rearmGain = 1.0f;
    std::vector<float> ring;
    int mask = 0, write = 0, boxLen = 1;
    double boxSum = 0.0;
    float ahead = 0.0f, aheadPrev = 0.0f, average = 0.0f, delayed = 0.0f;
    bool armed = true;
    int64_t riseStart = 0;
    std::array<int64_t, 16> events {};
    int head = 0, tail = 0;
    uint32_t found = 0;
};

} // namespace gtt
