// SilverConsole - formatos de plugin e lista de plugins escaneados. Codigo original (MIT).
#pragma once
#include <JuceHeader.h>

namespace sc
{
/** Mantem a lista de plugins VST3 escaneados (salva entre sessoes). */
class PluginHost : private juce::ChangeListener
{
public:
    explicit PluginHost (juce::PropertiesFile& settingsFile) : settings (settingsFile)
    {
        formats.addDefaultFormats(); // VST3 no Windows (VST2 exige SDK descontinuado)
        if (auto xml = settings.getXmlValue ("pluginList"))
            known.recreateFromXml (*xml);
        known.addChangeListener (this);
    }

    ~PluginHost() override { known.removeChangeListener (this); }

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;

    /** Arquivo "pedal do morto": um plugin que travar o scan e pulado no proximo. */
    juce::File getDeadMansPedalFile() const
    {
        return settings.getFile().getSiblingFile ("PluginScanCrash.txt");
    }

    juce::Array<juce::PluginDescription> getEffects() const
    {
        juce::Array<juce::PluginDescription> list;
        for (auto& d : known.getTypes())
            if (! d.isInstrument) list.add (d);
        return list;
    }

    std::unique_ptr<juce::AudioPluginInstance> create (const juce::PluginDescription& d, double sr, int bs, juce::String& error)
    {
        return formats.createPluginInstance (d, sr, bs, error);
    }

    juce::AudioPluginFormat* findVst3Format()
    {
        for (auto* f : formats.getFormats())
            if (f->getName() == "VST3") return f;
        return nullptr;
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (auto xml = known.createXml())
        {
            settings.setValue ("pluginList", xml.get());
            settings.saveIfNeeded();
        }
    }

    juce::PropertiesFile& settings;
};
} // namespace sc
