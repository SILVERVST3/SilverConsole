// SilverConsole - leitura e reamostragem de arquivos de audio. Codigo original (MIT).
#pragma once
#include <JuceHeader.h>
#include <memory>

namespace sc
{
struct LoadedAudio
{
    std::shared_ptr<const juce::AudioBuffer<float>> original;
    double rate = 0.0;
    juce::String error;
    bool ok() const noexcept { return original != nullptr && error.isEmpty(); }
};

/** Formatos aceitos: WAV, AIFF, FLAC, Ogg (e MP3/WMA pelo Windows Media no Windows). */
void registerAudioFormats (juce::AudioFormatManager&);

/** Decodifica o arquivo inteiro na memoria (mono ou os 2 primeiros canais). */
LoadedAudio loadAudioFile (juce::AudioFormatManager&, const juce::File&);

/** Reamostragem offline por sinc janelado, sem deslocamento de tempo. */
std::shared_ptr<const juce::AudioBuffer<float>> resampleBuffer (std::shared_ptr<const juce::AudioBuffer<float>> src,
                                                                double fromRate, double toRate);

juce::String audioWildcard();
bool isAudioFile (const juce::File&);
} // namespace sc
