// SilverConsole - processo separado para descobrir metadados de um VST3.
// Licenca MIT. Este executavel NAO hospeda processamento de audio em realtime.
#include <JuceHeader.h>
#include <cstdio>
#include <cmath>

int main (int argc, char** argv)
{
    // Modos internos para CTest exercitar prazo de vida, cancelamento e
    // retorno de falha sem instalar VST3 de terceiros no runner Windows.
    if (argc == 3)
    {
        const juce::String mode = juce::String::fromUTF8 (argv[1]);
        const int parameter = juce::String::fromUTF8 (argv[2]).getIntValue();
        if (mode == "--self-test-sleep")
        {
            juce::Thread::sleep (juce::jlimit (0, 10000, parameter));
            return 0;
        }
        if (mode == "--self-test-exit")
            return juce::jlimit (0, 100, parameter);
    }

    juce::ScopedJuceInitialiser_GUI init;

    // Modos:
    //   descoberta:  worker <candidato.vst3> <resultado.xml>
    //   DSP probe:   worker --probe <candidato.vst3> <resultado.xml>
    // O probe NAO transfere audio ao host e nao deve rodar na thread realtime.
    const bool probeDsp = argc == 4 && juce::String::fromUTF8 (argv[1]) == "--probe";
    if (argc != 3 && ! probeDsp)
    {
        std::fprintf (stderr, "Uso: ScPluginScanWorker [--probe] <arquivo.vst3> <resultado.xml>\n");
        return 2;
    }

    const juce::String source = juce::String::fromUTF8 (argv[probeDsp ? 2 : 1]);
    const juce::File output (juce::String::fromUTF8 (argv[probeDsp ? 3 : 2]));

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile (descriptions, source);
    if (descriptions.isEmpty())
    {
        std::fprintf (stderr, "Nenhum VST3 reconhecido nesse arquivo.\n");
        return 3;
    }

    if (probeDsp)
    {
        juce::AudioPluginFormatManager manager;
        manager.addDefaultFormats();
        for (auto* description : descriptions)
        {
            juce::String error;
            auto plugin = manager.createPluginInstance (*description, 48000.0, 256, error);
            if (plugin == nullptr)
            {
                std::fprintf (stderr, "Falha ao instanciar plugin em processo separado.\\n");
                return 5;
            }

            // Respeita o layout padrao do plugin (mono, stereo ou synth).
            // Nao configura hardware nem instancia plugins no host principal.
            const int channels = juce::jlimit (2, 32, juce::jmax (
                plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()));
            juce::AudioBuffer<float> work (channels, 256);
            juce::MidiBuffer midi;
            try
            {
                plugin->prepareToPlay (48000.0, 256);
                for (int block = 0; block < 8; ++block)
                {
                    work.clear();
                    midi.clear();
                    plugin->processBlock (work, midi);
                    for (int c = 0; c < channels; ++c)
                    {
                        const auto* ptr = work.getReadPointer (c);
                        for (int i = 0; i < 256; ++i)
                            if (! std::isfinite (ptr[i]))
                            {
                                std::fprintf (stderr, "DSP retornou amostras invalidas.\\n");
                                return 6;
                            }
                    }
                }
                plugin->releaseResources();
            }
            catch (...)
            {
                std::fprintf (stderr, "Excecao C++ no DSP durante teste isolado.\\n");
                return 7;
            }
        }
    }

    juce::XmlElement root ("SILVERCONSOLE_PLUGIN_SCAN");
    root.setAttribute ("format", "VST3");
    root.setAttribute ("schemaVersion", 1);
    if (probeDsp) root.setAttribute ("dspProbe", 1);
    for (auto* description : descriptions)
        if (auto node = description->createXml())
            root.addChildElement (node.release());

    // XML so e exposto ao host apos a escrita completa.
    juce::TemporaryFile temporary (output);
    if (! root.writeTo (temporary.getFile())
        || ! temporary.overwriteTargetFileWithTemporary())
    {
        std::fprintf (stderr, "Falha ao gravar resultado de varredura.\n");
        return 4;
    }

    return 0;
}
