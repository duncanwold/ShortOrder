#pragma once

// v0.5.11: the pitch trace's feed (audio -> UI, display only). The engine pushes one TraceFrame per hop of the voice's (delayed)
// timeline while an editor is open: the input's detected pitch as the voice plays it (so the input line is already shifted by
// the latency L, in line with the output), the voice's sounding pitch and level, and the chord's other tones. Single producer
// (the audio thread), single consumer (the editor's timer), lock-free, a fixed array: no allocation. A full FIFO drops frames.

#include <array>
#include <atomic>
#include <cstdint>

namespace gtt
{
struct TraceFrame
{
    static constexpr int kChordTones = 4; // the chord's other tones drawn (Freeze 4 holds 3 besides the live note; Harmonize 2)
    int64_t time = 0;       // the voice's timeline (input samples; the output plays it L later)
    float inMidi = -1.0f;   // the detector's raw pitch for this moment (MIDI, unquantized), -1 where it has none
    float inConfidence = 0.0f;
    float outMidi = -1.0f;  // the voice's sounding pitch (T and Octave), -1 while it's silent
    float level = 0.0f;     // the voice's level (mean |x| scale, after the gate and the window fade)
    std::array<float, kChordTones> chordMidi { -1.0f, -1.0f, -1.0f, -1.0f }; // the other tones' sounding pitches, -1: none
    std::array<float, kChordTones> chordLevel {};                             // ... their levels (the live level x their presence)
};

class TraceFifo
{
public:
    static constexpr int kCapacity = 4096; // ~11 s of hops at 48 kHz; the editor takes them 30 times a second

    // Audio thread. No allocation, no locks.
    void push (const TraceFrame& f) noexcept
    {
        const uint32_t w = writeIndex.load (std::memory_order_relaxed);
        if (w - readIndex.load (std::memory_order_acquire) >= (uint32_t) kCapacity)
            return; // full: the editor has stalled; drop
        frames[w % (uint32_t) kCapacity] = f;
        writeIndex.store (w + 1, std::memory_order_release);
    }

    // Message thread: hands every frame available to sink (const TraceFrame&).
    template <typename Sink>
    void pull (Sink&& sink)
    {
        const uint32_t end = writeIndex.load (std::memory_order_acquire);
        uint32_t r = readIndex.load (std::memory_order_relaxed);
        for (; r != end; ++r)
            sink (frames[r % (uint32_t) kCapacity]);
        readIndex.store (r, std::memory_order_release);
    }

    // Editors open: the engine only pushes while this is > 0.
    std::atomic<int> readers { 0 };

private:
    std::array<TraceFrame, kCapacity> frames {};
    std::atomic<uint32_t> writeIndex { 0 }, readIndex { 0 };
};
} // namespace gtt
