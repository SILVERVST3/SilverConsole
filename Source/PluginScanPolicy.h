// SilverConsole - politica simples de quarentena para scan de plugins.
// Nao armazena codigo de terceiros. Somente caminhos de candidatos que falharam.
#pragma once
#include <JuceHeader.h>

namespace sc
{
struct PluginScanPolicy
{
    static constexpr const char* settingsKey = "isolatedVst3FailedPlugins";

    static juce::StringArray deserialize (const juce::String& saved)
    {
        auto entries = juce::StringArray::fromLines (saved);
        entries.removeEmptyStrings();
        entries.removeDuplicates (true);
        return entries;
    }

    static juce::String serialize (const juce::StringArray& entries)
    {
        return entries.joinIntoString ("\n");
    }

    static bool shouldScan (const juce::StringArray& failed,
                            const juce::String& candidate, bool retryFailed)
    {
        const bool quarantined = failed.contains (candidate, true);
        // O comando "Reavaliar" examina SOMENTE os candidatos previamente
        // falhos. O escaneamento normal nunca os carrega sem consentimento.
        return retryFailed ? quarantined : ! quarantined;
    }

    static void reportResult (juce::StringArray& failed,
                              const juce::String& candidate, bool success)
    {
        if (success)
        {
            for (int i = failed.size(); --i >= 0;)
                if (failed[i].equalsIgnoreCase (candidate)) failed.remove (i);
        }
        else if (! failed.contains (candidate, true))
            failed.add (candidate);
    }
};
} // namespace sc
