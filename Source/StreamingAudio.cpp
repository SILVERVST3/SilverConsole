// Streaming assíncrono com cache imutavel de tamanho limitado (MIT).
#include "StreamingAudio.h"
#include "AudioLoader.h"
#include <cmath>

namespace sc
{
StreamingAudio::StreamingAudio (std::unique_ptr<juce::AudioFormatReader> r)
    : reader (std::move (r))
{
    length = reader->lengthInSamples;
    rate = reader->sampleRate;
    channels = juce::jlimit (1, 2, (int) reader->numChannels);
}

std::shared_ptr<StreamingAudio> StreamingAudio::open (const juce::File& file,
                                                        double engineRate, juce::String& error)
{
    error.clear();
    if (! file.existsAsFile())
    {
        error = "Arquivo inexistente: " + file.getFileName();
        return {};
    }
    juce::AudioFormatManager formats;
    registerAudioFormats (formats);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Falha ao abrir arquivo para streaming: " + file.getFileName();
        return {};
    }
    if (reader->lengthInSamples <= 0 || reader->numChannels <= 0 || reader->sampleRate <= 0)
    {
        error = "Audio invalido: " + file.getFileName();
        return {};
    }
    // A politica de fallback em outra taxa mantem a reamostragem sinc original.
    if (std::abs (reader->sampleRate - engineRate) >= 0.01)
    {
        error = "Taxa de amostragem diferente da taxa do motor";
        return {};
    }
    auto stream = std::shared_ptr<StreamingAudio> (new StreamingAudio (std::move (reader)));
    stream->buildOverview();
    if (! stream->prime())
    {
        error = "Falha na leitura inicial do arquivo: " + file.getFileName();
        return {};
    }
    return stream;
}

void StreamingAudio::buildOverview()
{
    // Uma visao de ondas compacta para o editor de automacao.
    // Amostragem esparsa: nao decodifica o arquivo inteiro na RAM.
    constexpr int bins = 2048, probeSize = 128;
    overview.assign ((size_t) bins, 0.0f);
    juce::AudioBuffer<float> probe (channels, probeSize);
    for (int i = 0; i < bins; ++i)
    {
        const auto start = (juce::int64) i * length / bins;
        const int count = (int) juce::jmin ((juce::int64) probeSize, length - start);
        if (count <= 0 || ! reader->read (&probe, 0, count, start, true, channels > 1))
            continue;
        float peak = 0.0f;
        for (int c = 0; c < channels; ++c)
        {
            const auto* data = probe.getReadPointer (c);
            for (int j = 0; j < count; ++j)
                if (std::isfinite (data[j])) peak = juce::jmax (peak, std::abs (data[j]));
        }
        overview[(size_t) i] = peak;
    }
}

bool StreamingAudio::prime()
{
    service();
    return std::atomic_load_explicit (&visible, std::memory_order_acquire) != nullptr;
}

void StreamingAudio::service()
{
    const auto requestedAt = juce::jlimit ((juce::int64) 0, length, requested.load (std::memory_order_relaxed));
    auto prior = std::atomic_load_explicit (&visible, std::memory_order_acquire);
    constexpr juce::int64 margin = windowSamples / 4;
    if (prior != nullptr
        && requestedAt >= prior->start + margin
        && requestedAt < prior->start + prior->size - margin)
        return;
    const auto start = juce::jmax ((juce::int64) 0, requestedAt - margin);
    if (prior != nullptr && prior->start == start) return;

    const int count = (int) juce::jmin ((juce::int64) windowSamples, length - start);
    if (count <= 0) return;
    auto next = std::make_shared<Window> (channels, start, count);
    if (! reader->read (&next->samples, 0, count, start, true, channels > 1))
        return; // nao publica dados parciais em caso de falha de I/O
    for (int c = 0; c < channels; ++c)
    {
        auto* dst = next->samples.getWritePointer (c);
        for (int i = 0; i < count; ++i)
            if (! std::isfinite (dst[i])) dst[i] = 0.0f;
    }
    std::shared_ptr<const Window> published = std::move (next);
    std::atomic_store_explicit (&visible, std::move (published), std::memory_order_release);
}

void StreamingAudio::copy (float* left, float* right, int destOffset,
                           juce::int64 sourceStart, int count) noexcept
{
    if (count <= 0 || sourceStart >= length) return;
    // Solicitar o proximo trecho ANTES de chegar ao fim da janela.
    requested.store (juce::jmax ((juce::int64) 0,
                     sourceStart + count),
                     std::memory_order_relaxed);

    auto window = std::atomic_load_explicit (&visible, std::memory_order_acquire);
    if (window == nullptr)
    {
        missingSamples.fetch_add ((juce::uint64) count, std::memory_order_relaxed);
        return;
    }
    const auto from = juce::jmax (sourceStart, window->start);
    const auto until = juce::jmin (juce::jmin (sourceStart + count, length),
                                   window->start + window->size);
    if (until > from)
    {
        const int dst = destOffset + (int) (from - sourceStart);
        const int src = (int) (from - window->start);
        const int n = (int) (until - from);
        juce::FloatVectorOperations::copy (left + dst, window->samples.getReadPointer (0, src), n);
        juce::FloatVectorOperations::copy (right + dst, window->samples.getReadPointer (channels == 1 ? 0 : 1, src), n);
    }
    const juce::int64 expected = juce::jmax ((juce::int64) 0,
                                                  juce::jmin ((juce::int64) count, length - sourceStart));
    if (until <= from) missingSamples.fetch_add ((juce::uint64) expected, std::memory_order_relaxed);
    else if (until - from < expected)
        missingSamples.fetch_add ((juce::uint64) (expected - (until - from)), std::memory_order_relaxed);
    // As regioes fora da janela permanecem em silencio (scratch zerado pelo motor).
}

StreamWorker::StreamWorker() : juce::Thread ("SilverConsole disk prefetch")
{
    startThread();
}

StreamWorker::~StreamWorker()
{
    signalThreadShouldExit();
    stopThread (30000);
}

void StreamWorker::add (const std::shared_ptr<StreamingAudio>& s)
{
    std::lock_guard<std::mutex> hold (mutex); // somente thread de controle
    streams.push_back (s);
}

void StreamWorker::run()
{
    while (! threadShouldExit())
    {
        std::vector<std::shared_ptr<StreamingAudio>> active;
        {
            std::lock_guard<std::mutex> hold (mutex);
            for (auto it = streams.begin(); it != streams.end();)
            {
                if (auto s = it->lock()) { active.push_back (std::move (s)); ++it; }
                else it = streams.erase (it);
            }
        }
        for (auto& stream : active)
        {
            if (threadShouldExit()) break;
            stream->service();
        }
        juce::Thread::sleep (3);
    }
}
} // namespace sc
