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

/** Mesma reamostragem, para um trecho (streaming de disco). Calcula as amostras de
    saida [outStart, outStart + outCount) de um canal. 'in' contem as amostras de
    entrada a partir do indice absoluto inStart; srcLength e o tamanho total da
    fonte. Use resampleInputRange para saber quais amostras de entrada ler.
    O resultado e identico ao de resampleBuffer no arquivo inteiro. */
void resampleRange (const float* in, juce::int64 inStart, int inCount, juce::int64 srcLength,
                    double fromRate, double toRate, juce::int64 outStart, int outCount, float* out);
/** Amostras de entrada [first, last] necessarias para a saida [outStart, outStart + outCount). */
void resampleInputRange (juce::int64 srcLength, double fromRate, double toRate,
                         juce::int64 outStart, int outCount, juce::int64& first, juce::int64& last);
/** Numero de amostras de saida para uma fonte de srcLength amostras. */
juce::int64 resampledLength (juce::int64 srcLength, double fromRate, double toRate);

juce::String audioWildcard();
bool isAudioFile (const juce::File&);
} // namespace sc
