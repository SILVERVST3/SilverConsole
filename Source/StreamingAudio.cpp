// Streaming assíncrono com cache imutavel de tamanho limitado (MIT).
#include "StreamingAudio.h"
#include "RealtimeThread.h"
#include "AudioLoader.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace sc
{
StreamingAudio::StreamingAudio (std::unique_ptr<juce::AudioFormatReader> r, double rate)
    : reader (std::move (r))
{
    sourceLength = reader->lengthInSamples;
    sourceRate = reader->sampleRate;
    engineRate = rate;
    channels = juce::jlimit (1, 2, (int) reader->numChannels);
    resampling = std::abs (sourceRate - engineRate) >= 0.01;
    length = resampling ? resampledLength (sourceLength, sourceRate, engineRate) : sourceLength;
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
    if (reader->lengthInSamples <= 0 || reader->numChannels <= 0
        || reader->sampleRate < 8000.0 || reader->sampleRate > 384000.0 || engineRate <= 0)
    {
        error = "Audio invalido: " + file.getFileName();
        return {};
    }
    auto stream = std::shared_ptr<StreamingAudio> (new StreamingAudio (std::move (reader), engineRate));
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
        const auto start = (juce::int64) i * sourceLength / bins;
        const int count = (int) juce::jmin ((juce::int64) probeSize, sourceLength - start);
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
    return visible.load (std::memory_order_seq_cst) != nullptr;
}

void StreamingAudio::service()
{
    // O export offline pode requerer o bloco imediatamente; protege o reader
    // contra leituras simultaneas pelos workers (o callback nunca usa mutex).
    std::lock_guard<std::mutex> readerLock (readerMutex);
    serviceLocked();
}

bool StreamingAudio::tryService()
{
    std::unique_lock<std::mutex> readerLock (readerMutex, std::try_to_lock);
    if (! readerLock.owns_lock()) return false;
    serviceLocked();
    return true;
}

juce::int64 StreamingAudio::lead() const noexcept
{
    // So o ponteiro e consultado (sem ler a janela, que outra thread pode liberar).
    if (visible.load (std::memory_order_acquire) == nullptr) return 0;
    const auto end = windowEndCache.load (std::memory_order_acquire);
    if (end >= length) return std::numeric_limits<juce::int64>::max() / 4; // ja carregou ate o fim
    return end - requested.load (std::memory_order_relaxed);
}

bool StreamingAudio::readWindow (juce::int64 start, int count, juce::AudioBuffer<float>& dest)
{
    if (! resampling)
    {
        if (! reader->read (&dest, 0, count, start, true, channels > 1)) return false;
    }
    else
    {
        // Le so o trecho de entrada necessario e converte com o sinc offline.
        juce::int64 first = 0, last = -1;
        resampleInputRange (sourceLength, sourceRate, engineRate, start, count, first, last);
        if (last < first) { dest.clear(); return true; }
        const int inCount = (int) (last - first + 1);
        sourceScratch.setSize (channels, inCount, false, false, true);
        if (! reader->read (&sourceScratch, 0, inCount, first, true, channels > 1)) return false;
        for (int c = 0; c < channels; ++c)
        {
            auto* src = sourceScratch.getWritePointer (c);
            for (int i = 0; i < inCount; ++i)
                if (! std::isfinite (src[i])) src[i] = 0.0f;
            resampleRange (src, first, inCount, sourceLength, sourceRate, engineRate,
                           start, count, dest.getWritePointer (c));
        }
    }
    for (int c = 0; c < channels; ++c)
    {
        auto* d = dest.getWritePointer (c);
        for (int i = 0; i < count; ++i)
            if (! std::isfinite (d[i])) d[i] = 0.0f;
    }
    return true;
}

void StreamingAudio::serviceLocked()
{
    // Liberacao de janelas antigas e exclusivamente tarefa de quem tem o reader.
    // Depois da troca de ponteiro, novos leitores nunca adquirem janela antiga.
    if (audioReaders.load (std::memory_order_seq_cst) == 0)
        retiredWindows.clear();
    const auto need = juce::jlimit ((juce::int64) 0, length, requested.load (std::memory_order_relaxed));
    if (need >= length) return;

    const auto* prior = visible.load (std::memory_order_seq_cst);
    if (prior != nullptr && need >= prior->start)
    {
        const auto end = prior->start + prior->size;
        // ainda ha bastante audio carregado a frente: nada a fazer
        if (need < end && (end >= length || end - need >= windowSamples / 2)) return;
    }
    // Cursor saltou (play, loop, clique na regua): janela curta primeiro, para o
    // som voltar rapido; a janela completa vem na proxima rodada do worker.
    const bool jumped = prior == nullptr || need < prior->start || need >= prior->start + prior->size;
    const auto start = juce::jmax ((juce::int64) 0, need - backMargin);
    const int count = (int) juce::jmin ((juce::int64) (jumped ? quickSamples : windowSamples), length - start);
    if (count <= 0) return;

    auto next = std::make_unique<Window> (channels, start, count);
    if (! readWindow (start, count, next->samples))
        return; // nao publica dados parciais em caso de falha de I/O

    // Manter antiga janela ate que todos os callbacks anteriores terminem.
    // Sem desalocacao, alocacao ou shared_ptr refcount na thread realtime.
    if (activeWindow != nullptr)
        retiredWindows.push_back (std::move (activeWindow));
    activeWindow = std::move (next);
    windowEndCache.store (activeWindow->start + activeWindow->size, std::memory_order_release);
    visible.store (activeWindow.get(), std::memory_order_seq_cst);
    if (audioReaders.load (std::memory_order_seq_cst) == 0)
        retiredWindows.clear();
}

void StreamingAudio::prefetchBlocking (juce::int64 start, int count)
{
    if (start < 0 || start >= length || count <= 0) return;
    requested.store (start, std::memory_order_relaxed);
    service();
}

void StreamingAudio::copy (float* left, float* right, int destOffset,
                           juce::int64 sourceStart, int count) noexcept
{
    if (count <= 0 || sourceStart >= length) return;
    // Informa ao worker onde o cursor esta (ele mantem a janela a frente dele).
    requested.store (juce::jmax ((juce::int64) 0, sourceStart), std::memory_order_relaxed);

    // Incrementar antes de ler visible; o worker somente recicla janelas
    // quando nenhum callback as referencia.
    audioReaders.fetch_add (1, std::memory_order_seq_cst);
    struct ReaderGuard
    {
        std::atomic<unsigned int>& readers;
        ~ReaderGuard() { readers.fetch_sub (1, std::memory_order_seq_cst); }
    } guard { audioReaders };
    const auto* window = visible.load (std::memory_order_seq_cst);
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

//==============================================================================
struct StreamWorker::IoThread final : public juce::Thread
{
    IoThread (StreamWorker& o, int index) : juce::Thread ("SilverConsole disk " + juce::String (index)), owner (o) {}
    void run() override
    {
        rt::disableThreadThrottling(); // leitura de disco sem nucleo de eficiencia
        while (! threadShouldExit()) owner.runOnce (*this);
    }
    StreamWorker& owner;
};

StreamWorker::StreamWorker()
{
    // Duas threads: enquanto uma converte/le um arquivo, a outra atende o
    // proximo mais urgente. Prioridade alta (abaixo da thread de audio).
    for (int i = 0; i < 2; ++i)
    {
        threads.push_back (std::make_unique<IoThread> (*this, i + 1));
        threads.back()->startThread (juce::Thread::Priority::high);
    }
}

StreamWorker::~StreamWorker()
{
    for (auto& t : threads) t->signalThreadShouldExit();
    for (auto& t : threads) t->stopThread (30000);
}

void StreamWorker::add (const std::shared_ptr<StreamingAudio>& s)
{
    std::lock_guard<std::mutex> hold (mutex); // somente thread de controle
    streams.push_back (s);
}

void StreamWorker::runOnce (juce::Thread& thread)
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
    // mais urgente primeiro: menos audio carregado a frente do cursor
    std::vector<std::pair<juce::int64, StreamingAudio*>> order;
    order.reserve (active.size());
    for (auto& s : active) order.emplace_back (s->lead(), s.get());
    std::sort (order.begin(), order.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [lead, stream] : order)
    {
        if (thread.threadShouldExit()) break;
        stream->tryService();
    }
    active.clear(); // libera referencias fora do lock
    thread.wait (2);
}
} // namespace sc
