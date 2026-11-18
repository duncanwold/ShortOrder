#pragma once

#include <atomic>
#include <cstdint>

namespace gtt
{

// Audio thread -> UI. Written once per detector hop, read by the editor at 30 Hz.
struct Readout
{
    std::atomic<bool> voiced { false };
    std::atomic<bool> hasNote { false };   // quantizer has ever latched a note
    std::atomic<int> noteN { 0 };          // quantizer note N (MIDI)
    std::atomic<int> outNote { 0 };        // round(T), the note being produced
    std::atomic<float> cents { 0.0f };     // (midi - N) * 100
    std::atomic<float> f0 { 0.0f };
    std::atomic<float> confidence { 0.0f };
    std::atomic<bool> twoPlusNotes { false }; // the "2+ NOTES?" hint
    std::atomic<int> voiceNote { 0 };      // outNote shifted down by the octave
    std::atomic<float> voiceHz { 0.0f };   // the voice's actual pitch: f(T) x octave ratio
    std::atomic<float> latencyMs { 0.0f };
    std::atomic<int> windowLength { 0 };   // L in samples, as the engine is running it
    // The voice on the delayed (sounding) timeline, per block.
    std::atomic<bool> voiceOpen { false };       // sounding or holding (not faded / not started)
    std::atomic<float> holdRemainingMs { 0.0f }; // while open: hold time left if pitch stays lost
    // The chord's tones sounding (not fading out), per block, as MIDI notes before Octave: one byte each, up to 8 (v0.5.4: Memory Notes 4 sounds up to 5), live note first, 0xff =
    // none (all 0xff while the voice is closed). Packed into one word so the editor never sees half of one
    // update (lock-free on every 64-bit target).
    std::atomic<uint64_t> chordTones { ~uint64_t { 0 } };
    // Memory (v0.5.4), per block: the remembered notes held for the next note, besides the one being played (all
    // of them in a pitch gap); 0 when memory is clear or Chord isn't Memory. The HELD light is on while it's > 0.
    std::atomic<int> memoryHeld { 0 };
    // The output stage's headroom gain (v0.5.3): the deepest reduction, in dB (0 = not reducing), since the
    // editor last took it (it exchanges it for 0), so a reduction between two of its updates still shows.
    std::atomic<float> gainReductionDb { 0.0f };
};

} // namespace gtt
