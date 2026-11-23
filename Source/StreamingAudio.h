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
    Arquivos em outra taxa de amostragem sao convertidos na propria thread de I/O,
    trecho a trecho, com o mesmo sinc do reamostrador offline (resultado identico):
    nada do arquivo inteiro fica na RAM. Indices de copy() estao na taxa do motor. */
class StreamingAudio
{
public:
    static constexpr int windowSamples = 1 << 18;    // ~5,5 s @ 48 kHz por janela
    static constexpr int quickSamples = 1 << 15;     // janela curta apos um salto do cursor
    static constexpr int backMargin = 2048;          // amostras mantidas antes do cursor
    static constexpr juce::int64 minimumFileBytes = 32 * 1024 * 1024;

    static std::shared_ptr<StreamingAudio> open (const juce::File&, double engineRate,
                                                  juce::String& error);
    bool prime(); // chamada na thread de importacao, antes de publicar no motor
    void service(); // worker de I/O ou thread de exportacao, nunca no callback
    bool tryService(); // como service(), mas desiste se outra thread ja esta lendo
    void prefetchBlocking (juce::int64 start, int count); // apenas render offline, fora do callback
    /** Amostras ja carregadas a frente do cursor (prioridade do worker de I/O). */
    juce::int64 lead() const noexcept;

    void copy (float* left, float* right, int destOffset, juce::int64 sourceStart, int count) noexcept;
    juce::int64 getLength() const noexcept { return length; }      // na taxa do motor
    double getRate() const noexcept { return engineRate; }          // taxa entregue ao motor
    double getSourceRate() const noexcept { return sourceRate; }    // taxa do arquivo
    int getChannels() const noexcept { return channels; }
    const std::vector<float>& getOverview() const noexcept { return overview; }
    juce::uint64 getMissingSamples() const noexcept { return missingSamples.load (std::memory_order_relaxed); }
    void resetMissingSamples() noexcept { missingSamples.store (0, std::memory_order_relaxed); }

private:
    explicit StreamingAudio (std::unique_ptr<juce::AudioFormatReader>, double engineRate);
    void buildOverview(); // thread de importacao; amplitude aproximada, memoria limitada
    void serviceLocked();
    bool readWindow (juce::int64 start, int count, juce::AudioBuffer<float>& dest);
    struct Window
    {
        Window (int channels, juce::int64 startIn, int count)
            : samples (channels, count), start (startIn), size (count) { samples.clear(); }
        juce::AudioBuffer<float> samples;
        juce::int64 start;
        int size;
    };

    std::unique_ptr<juce::AudioFormatReader> reader; // protegido para worker/exportacao
    std::mutex readerMutex;
    juce::int64 length = 0, sourceLength = 0;
    double sourceRate = 0.0, engineRate = 0.0;
    bool resampling = false;
    int channels = 0;
    juce::AudioBuffer<float> sourceScratch; // so a thread que tem readerMutex
    std::vector<float> overview;
    std::atomic<juce::int64> requested { 0 };
    std::atomic<juce::uint64> missingSamples { 0 };
    // Um ponteiro atomico em vez de atomic_shared_ptr no callback:
    // nenhuma contagem de referencias, mutex ou destruicao de buffers no audio.
    std::atomic<const Window*> visible { nullptr };
    std::atomic<unsigned int> audioReaders { 0 };
    std::atomic<juce::int64> windowEndCache { 0 }; // fim da janela publicada (lido por lead())
    std::unique_ptr<const Window> activeWindow; // owned by I/O thread
    std::vector<std::unique_ptr<const Window>> retiredWindows; // only freed by I/O thread
};

/** Threads de I/O compartilhadas por todas as faixas (nao uma por faixa).
    Atende primeiro as faixas com menos audio carregado a frente do cursor. */
class StreamWorker final
{
public:
    StreamWorker();
    ~StreamWorker();
    void add (const std::shared_ptr<StreamingAudio>&);

private:
    struct IoThread;
    void runOnce (juce::Thread&);
    std::mutex mutex;
    std::vector<std::weak_ptr<StreamingAudio>> streams;
    std::vector<std::unique_ptr<IoThread>> threads;
};
} // namespace sc
