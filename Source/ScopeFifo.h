#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

// Audio -> UI samples: guitar input and synth output, as pairs (v0.5.11: the editor's low-range display detector reads the input;
// the scope it was named for is gone, replaced by the pitch trace's own feed, gtt::TraceFifo). One producer
// (the audio thread), one consumer (the editor's timer on the message thread), lock-free
// (juce::AbstractFifo), allocated once at construction. A full FIFO drops new samples.
class ScopeFifo
{
public:
    static constexpr int kCapacity = 1 << 15;

    ScopeFifo() : fifo (kCapacity), in ((size_t) kCapacity, 0.0f), out ((size_t) kCapacity, 0.0f) {}

    // Audio thread. No allocation, no locks.
    void push (const float* x, const float* y, int n) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (n, s1, n1, s2, n2);
        std::copy (x, x + n1, in.begin() + s1);
        std::copy (y, y + n1, out.begin() + s1);
        std::copy (x + n1, x + n1 + n2, in.begin() + s2);
        std::copy (y + n1, y + n1 + n2, out.begin() + s2);
        fifo.finishedWrite (n1 + n2);
    }

    // Message thread: hands everything available to sink (const float* in, const float* out, int n).
    template <typename Sink>
    void pull (Sink&& sink)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (fifo.getNumReady(), s1, n1, s2, n2);
        if (n1 > 0)
            sink (in.data() + s1, out.data() + s1, n1);
        if (n2 > 0)
            sink (in.data() + s2, out.data() + s2, n2);
        fifo.finishedRead (n1 + n2);
    }

    // Editors open: the processor only pushes while this is > 0.
    std::atomic<int> readers { 0 };

private:
    juce::AbstractFifo fifo;
    std::vector<float> in, out;
};
