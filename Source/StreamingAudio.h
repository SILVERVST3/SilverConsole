// Streaming assíncrono de audio: leitura/decompressao fora do callback (MIT).
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace sc
{
/** Cache imutavel publicado de uma thread de I/O para o callback de audio.
    Somente amostras na mesma taxa do motor: arquivos em outras taxas ainda
    usam o reamostrador sinc offline para preservar o som existente. */
class StreamingAudio
{
public:
    static constexpr int windowSamples = 65536; // ~1.37 s @ 48 kHz
    static constexpr juce::int64 minimumFileBytes = 32 * 1024 * 1024;

    static std::shared_ptr<StreamingAudio> open (const juce::File&, double engineRate,
                                                  juce::String& error);
    bool prime(); // chamada na thread de importacao, antes de publicar no motor
    void service(); // exclusivamente na thread de leitura, nunca no callback

    void copy (float* left, float* right, int destOffset, juce::int64 sourceStart, int count) noexcept;
    juce::int64 getLength() const noexcept { return length; }
    double getRate() const noexcept { return rate; }
    int getChannels() const noexcept { return channels; }
    const std::vector<float>& getOverview() const noexcept { return overview; }
    juce::uint64 getMissingSamples() const noexcept { return missingSamples.load (std::memory_order_relaxed); }

private:
    explicit StreamingAudio (std::unique_ptr<juce::AudioFormatReader>);
    void buildOverview(); // thread de importacao; amplitude aproximada, memoria limitada
    struct Window
    {
        Window (int channels, juce::int64 startIn, int count)
            : samples (channels, count), start (startIn), size (count) { samples.clear(); }
        juce::AudioBuffer<float> samples;
        juce::int64 start;
        int size;
    };

    std::unique_ptr<juce::AudioFormatReader> reader; // acessado apenas pela thread de I/O
    juce::int64 length = 0;
    double rate = 0.0;
    int channels = 0;
    std::vector<float> overview;
    std::atomic<juce::int64> requested { 0 };
    std::atomic<juce::uint64> missingSamples { 0 };
    std::shared_ptr<const Window> visible; // atomic_load/store; nunca alterado apos publicacao
};

/** Uma thread de I/O compartilhada por todas as faixas (nao uma por faixa). */
class StreamWorker final : private juce::Thread
{
public:
    StreamWorker();
    ~StreamWorker() override;
    void add (const std::shared_ptr<StreamingAudio>&);

private:
    void run() override;
    std::mutex mutex;
    std::vector<std::weak_ptr<StreamingAudio>> streams;
};
} // namespace sc
