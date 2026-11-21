// SilverConsole - processo separado para descobrir metadados de um VST3.
// Licenca MIT. Este executavel NAO hospeda processamento de audio em realtime.
#include <JuceHeader.h>
#include <cstdio>

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

    // Cada invocacao examina somente um candidato, isolando eventuais crashes
    // de construtores de terceiros do processo principal do SilverConsole.
    if (argc != 3)
    {
        std::fprintf (stderr, "Uso: ScPluginScanWorker <arquivo.vst3> <resultado.xml>\n");
        return 2;
    }

    const juce::String source = juce::String::fromUTF8 (argv[1]);
    const juce::File output (juce::String::fromUTF8 (argv[2]));

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile (descriptions, source);
    if (descriptions.isEmpty())
    {
        std::fprintf (stderr, "Nenhum VST3 reconhecido nesse arquivo.\n");
        return 3;
    }

    juce::XmlElement root ("SILVERCONSOLE_PLUGIN_SCAN");
    root.setAttribute ("format", "VST3");
    root.setAttribute ("schemaVersion", 1);
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
