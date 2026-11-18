#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
// The saved state carries a version (stateVersion). 22 since v0.7 (the 1.0 parameter contract: the IDs latency, release and wave, no
// ADVANCED parameters). Sessions saved before it (21 and older, or none) aren't migrated: before 1.0, sessions aren't kept
// compatible, so one loads as a fresh plugin (every parameter at its default). Bump it when the parameter set changes; after 1.0, an
// older version must be migrated instead.
const juce::Identifier kStateVersion ("stateVersion");
constexpr int kCurrentStateVersion = 22;
} // namespace

GTTProcessor::GTTProcessor (const ShowFlags& show)
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      flags (show),
      apvts (*this, nullptr, "ShortOrderState", createParameterLayout (show.advanced, [this] { return getSampleRate(); })),
      bypassParam (apvts.getParameter (ParamIDs::bypass)),
      params (apvts),
      defaults (apvts.copyState())
{
    engine.setTraceFeed (&trace); // v0.5.11: the pitch trace (pushed only while an editor reads it)
}

GTTProcessor::~GTTProcessor() = default;

bool GTTProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const bool inOk = in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
    const bool outOk = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    // mono->mono, mono->stereo, stereo->stereo
    return inOk && outOk && ! (in == juce::AudioChannelSet::stereo() && out == juce::AudioChannelSet::mono());
}

void GTTProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    mono.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0f);
    scopeIn.assign (mono.size(), 0.0f);
    engine.prepare (sampleRate, samplesPerBlock);
}

void GTTProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    params.read (engineParams);

    // Hosts may exceed the prepared block size; process in chunks rather than allocate.
    const int chunk = (int) mono.size();
    for (int start = 0; start < numSamples; start += chunk)
    {
        const int n = juce::jmin (chunk, numSamples - start);
        float* m = mono.data();

        if (numIn > 0)
        {
            const float scale = 1.0f / (float) numIn;
            juce::FloatVectorOperations::copyWithMultiply (m, buffer.getReadPointer (0, start), scale, n);
            for (int ch = 1; ch < numIn; ++ch)
                juce::FloatVectorOperations::addWithMultiply (m, buffer.getReadPointer (ch, start), scale, n);
        }
        else
        {
            juce::FloatVectorOperations::clear (m, n);
        }

        // The editor's low-range display detector gets the input (and the output, unused since v0.5.11's trace) while an editor is open.
        const bool toScope = scope.readers.load (std::memory_order_relaxed) > 0;
        if (toScope)
            juce::FloatVectorOperations::copy (scopeIn.data(), m, n);
        engine.process (m, m, n, engineParams);
        if (toScope)
            scope.push (scopeIn.data(), m, n);

        for (int ch = 0; ch < numOut; ++ch)
            juce::FloatVectorOperations::copy (buffer.getWritePointer (ch, start), m, n);
    }
}

juce::AudioProcessorEditor* GTTProcessor::createEditor()
{
    return new GTTEditor (*this);
}

void GTTProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (kStateVersion, kCurrentStateVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

bool GTTProcessor::isCurrentState (const juce::ValueTree& state)
{
    return state.hasType ("ShortOrderState") && (int) state.getProperty (kStateVersion, 0) >= kCurrentStateVersion;
}

void GTTProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // The state's tag is ShortOrderState (since v0.6.1; GTT Lab's GTTLabState, up to v0.6, isn't read). v0.7: a state older than
    // kCurrentStateVersion, another tag, or data that isn't a state at all loads as a fresh plugin (the defaults).
    std::unique_ptr<juce::XmlElement> xml;
    if (data != nullptr && sizeInBytes > 0)
        xml = getXmlFromBinary (data, sizeInBytes);
    auto state = xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree();
    apvts.replaceState (isCurrentState (state) ? state : defaults.createCopy());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GTTProcessor();
}
