// Plugin VST3 minimo para testar a hospedagem do SilverConsole:
// atraso fixo de 64 amostras (reportado como latencia) + ganho.
#include <JuceHeader.h>

class TestDelayProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kLatency = 64;

    TestDelayProcessor()
        : AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
          params (*this, nullptr, "PARAMS",
                  { std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Gain",
                                                                 juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f) })
    {
        setLatencySamples (kLatency);
    }

    const juce::String getName() const override { return "SC Test Delay"; }
    void prepareToPlay (double, int) override { ring.setSize (2, kLatency); ring.clear(); idx = 0; }
    void releaseResources() override {}
    void reset() override { ring.clear(); idx = 0; }

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        const float g = juce::Decibels::decibelsToGain (params.getRawParameterValue ("gain")->load());
        const int n = b.getNumSamples();
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < 2; ++c)
            {
                auto* d = b.getWritePointer (c);
                auto* r = ring.getWritePointer (c);
                const float out = r[idx];
                r[idx] = d[i];
                d[i] = out * g;
            }
            if (++idx == kLatency) idx = 0;
        }
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& m) override
    {
        if (auto x = params.copyState().createXml()) copyXmlToBinary (*x, m);
    }
    void setStateInformation (const void* d, int size) override
    {
        if (auto x = getXmlFromBinary (d, size)) params.replaceState (juce::ValueTree::fromXml (*x));
    }

private:
    juce::AudioProcessorValueTreeState params;
    juce::AudioBuffer<float> ring;
    int idx = 0;
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestDelayProcessor(); }
