#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"
#include "ScopeFifo.h"
#include "DSP/Engine.h"

class GTTProcessor : public juce::AudioProcessor
{
public:
    // show: what the editor shows (the compile-time flags; the snapshot tool builds one with every flag on).
    explicit GTTProcessor (const ShowFlags& show = {});
    ~GTTProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 1.0; }
    // v0.5.9: the host's bypass (Logic's power button, every DAW's bypass) is GTT's own Bypass parameter, so the wrappers keep
    // calling processBlock and the engine plays its clean bypass: the raw, undelayed input with the 10 ms crossfade.
    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Whether a saved state is restored as it is: a ShortOrderState of the current stateVersion (22, v0.7) or later. Anything else
    // loads as a fresh plugin (no compatibility before 1.0).
    static bool isCurrentState (const juce::ValueTree& state);

    const ShowFlags& showFlags() const { return flags; }

private:
    const ShowFlags flags; // what the editor shows (the layout depends on it only for the ADVANCED group, v0.7: flags.advanced)

public:
    juce::AudioProcessorValueTreeState apvts;

    const gtt::Readout& readout() { return engine.readout(); }
    // The headroom gain's deepest reduction since the last call (dB, 0 = none); the editor's line 2.
    float takeGainReductionDb() { return engine.readout().gainReductionDb.exchange (0.0f, std::memory_order_relaxed); }
    ScopeFifo& scopeFifo() { return scope; }  // the guitar input for the editor's low-range display detector (v0.5.11: no scope)
    gtt::TraceFifo& traceFifo() { return trace; } // v0.5.11: the pitch trace's feed (the engine pushes while an editor reads)

private:
    juce::AudioProcessorParameter* bypassParam = nullptr;
    ParamPointers params;
    const juce::ValueTree defaults; // the state of a fresh plugin, what an older or unreadable session loads as
    gtt::EngineParams engineParams;
    gtt::Engine engine;
    std::vector<float> mono, scopeIn;
    ScopeFifo scope;
    gtt::TraceFifo trace;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GTTProcessor)
};
