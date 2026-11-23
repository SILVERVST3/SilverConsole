// SilverConsole - motor de mixagem. Codigo original (MIT).
#include "Engine.h"
#include "RealtimeThread.h"
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace sc
{
// Workers iniciados UMA vez por exportacao, nunca na thread de audio da placa.
// Cada faixa possui buffers e plugins independentes. A soma/envios permanece
// sequencial, na ordem original, para preservar resultados e roteamento.
class OfflineTrackPool
{
public:
    explicit OfflineTrackPool (int workers)
    {
        for (int i = 0; i < workers; ++i)
            threads.emplace_back ([this] { worker(); });
    }
    ~OfflineTrackPool()
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            stopping = true;
        }
        wake.notify_all();
        for (auto& thread : threads)
            if (thread.joinable()) thread.join();
    }

    void run (int count, const std::function<void(int)>& process)
    {
        if (count <= 0) return;
        if (threads.empty())
        {
            for (int i = 0; i < count; ++i) process (i);
            return;
        }
        {
            std::lock_guard<std::mutex> guard (mutex);
            job = &process;
            jobCount = count;
            next.store (0, std::memory_order_relaxed);
            workersFinished.store (0, std::memory_order_relaxed);
            ++generation;
        }
        wake.notify_all();
        processAvailable (process, count); // export thread also works
        std::unique_lock<std::mutex> hold (mutex);
        finished.wait (hold, [this]
        {
            return workersFinished.load (std::memory_order_acquire) == (int) threads.size();
        });
        // Todos os workers terminaram antes de o ponteiro 'process' expirar.
    }

private:
    void processAvailable (const std::function<void(int)>& fn, int count)
    {
        for (;;)
        {
            const int index = next.fetch_add (1, std::memory_order_relaxed);
            if (index >= count) break;
            fn (index);
        }
    }
    void worker()
    {
        unsigned long long seen = 0;
        for (;;)
        {
            std::unique_lock<std::mutex> hold (mutex);
            wake.wait (hold, [this, &seen] { return stopping || generation != seen; });
            if (stopping) return;
            seen = generation;
            const auto* currentJob = job;
            const int count = jobCount;
            hold.unlock();

            processAvailable (*currentJob, count);
            if (workersFinished.fetch_add (1, std::memory_order_acq_rel) + 1 == (int) threads.size())
                finished.notify_one();
        }
    }

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable wake, finished;
    std::atomic<int> next { 0 }, workersFinished { 0 };
    unsigned long long generation = 0;
    bool stopping = false;
    int jobCount = 0;
    const std::function<void(int)>* job = nullptr;
};

//==============================================================================
// Pool PERSISTENTE para o callback da placa: as faixas (leitura, inserts, PDC)
// sao processadas em varios nucleos. Sem alocacao, mutex ou espera bloqueante na
// thread de audio: o trabalho e publicado num contador atomico (geracao + indice),
// os workers sao acordados por semaforo (ReleaseSemaphore nao bloqueia) e a
// thread de audio tambem processa faixas. Ela so espera faixas ja em andamento.
class RealtimeTrackPool
{
public:
    using Fn = void (*) (void* context, int index);

    explicit RealtimeTrackPool (int workers)
    {
       #if JUCE_WINDOWS
        semaphore = ::CreateSemaphoreW (nullptr, 0, 0x7fffffff, nullptr);
        if (semaphore != nullptr)
            for (int i = 0; i < workers; ++i)
                threads.emplace_back ([this] { workerLoop(); });
       #else
        juce::ignoreUnused (workers);
       #endif
    }

    ~RealtimeTrackPool()
    {
       #if JUCE_WINDOWS
        stopping.store (true);
        if (semaphore != nullptr) ::ReleaseSemaphore (semaphore, (LONG) threads.size() + 1, nullptr);
        for (auto& t : threads) if (t.joinable()) t.join();
        if (semaphore != nullptr) ::CloseHandle (semaphore);
       #endif
    }

    int size() const noexcept { return (int) threads.size(); }

    /** Thread de audio (ou de exportacao): executa fn(context, 0..count-1). */
    void run (int count, Fn fn, void* context) noexcept
    {
        if (count <= 0) return;
        if (threads.empty() || count == 1)
        {
            for (int i = 0; i < count; ++i) fn (context, i);
            return;
        }
       #if JUCE_WINDOWS
        const auto gen = ++generation; // so este produtor escreve
        auto& slot = jobs[gen & 1];
        slot.fn.store (fn, std::memory_order_relaxed);
        slot.context.store (context, std::memory_order_relaxed);
        slot.count.store (count, std::memory_order_relaxed);
        completed.store (0, std::memory_order_relaxed);
        work.store (gen << 32, std::memory_order_release);
        ::ReleaseSemaphore (semaphore, (LONG) juce::jmin (size(), count - 1), nullptr);

        claimAndRun();
        for (int spins = 0; completed.load (std::memory_order_acquire) < count;)
        {
            ::YieldProcessor();
            if (++spins == 2000) { spins = 0; ::SwitchToThread(); }
        }
       #endif
    }

private:
    struct Job
    {
        std::atomic<Fn> fn { nullptr };
        std::atomic<void*> context { nullptr };
        std::atomic<int> count { 0 };
    };

    void claimAndRun() noexcept
    {
        for (;;)
        {
            auto w = work.load (std::memory_order_acquire);
            const auto& job = jobs[(w >> 32) & 1];
            const auto index = (int) (w & 0xffffffffu);
            const auto fn = job.fn.load (std::memory_order_relaxed);
            auto* context = job.context.load (std::memory_order_relaxed);
            if (index >= job.count.load (std::memory_order_relaxed) || fn == nullptr) return;
            // A geracao no mesmo contador garante que o indice pertence ao job lido.
            if (! work.compare_exchange_weak (w, w + 1, std::memory_order_acq_rel)) continue;
            fn (context, index);
            completed.fetch_add (1, std::memory_order_release);
        }
    }

   #if JUCE_WINDOWS
    void workerLoop()
    {
        // Prioridade de audio: MMCSS "Pro Audio", tempo critico e sem economia
        // de energia do Windows 11 (nunca em nucleo de eficiencia/clock baixo).
        rt::promoteAudioThread();
        while (! stopping.load (std::memory_order_relaxed))
        {
            ::WaitForSingleObject (semaphore, INFINITE);
            if (stopping.load (std::memory_order_relaxed)) break;
            juce::ScopedNoDenormals noDenormals;
            claimAndRun();
        }
    }
    HANDLE semaphore = nullptr;
   #endif

    std::vector<std::thread> threads;
    std::atomic<bool> stopping { false };
    std::atomic<std::uint64_t> work { 0 };
    std::atomic<int> completed { 0 };
    std::uint64_t generation = 0;
    Job jobs[2];
};

namespace
{
    void panGains (float pan, bool monoSource, float& l, float& r) noexcept
    {
        pan = juce::jlimit (-1.0f, 1.0f, pan);
        if (monoSource)
        {
            // potencia constante, -3 dB no centro
            const float theta = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
            l = std::cos (theta);
            r = std::sin (theta);
        }
        else
        {
            // balance estereo, 0 dB no centro
            l = pan > 0.0f ? 1.0f - pan : 1.0f;
            r = pan < 0.0f ? 1.0f + pan : 1.0f;
        }
    }

    void addWithRamp (float* dst, const float* src, int n, juce::LinearSmoothedValue<float>& sm) noexcept
    {
        if (! sm.isSmoothing())
        {
            const float g = sm.getTargetValue();
            if (g != 0.0f) juce::FloatVectorOperations::addWithMultiply (dst, src, g, n);
            return;
        }
        for (int i = 0; i < n; ++i) dst[i] += src[i] * sm.getNextValue();
    }

    /** Novo alvo de ganho; no primeiro bloco de um render offline vai direto ao
        valor (sem rampa vinda do estado anterior do mixer). */
    void setGainTarget (juce::LinearSmoothedValue<float>& sm, float v, bool snap) noexcept
    {
        if (snap) sm.setCurrentAndTargetValue (v);
        else sm.setTargetValue (v);
    }

    /** Executa fn(0..count-1) em paralelo: pool offline (bounce), pool realtime
        (callback, sem alocacao) ou em serie. */
    template <typename Fn>
    void runChannels (OfflineTrackPool* offline, RealtimeTrackPool* realtime, bool multi, int count, Fn& fn)
    {
        if (count <= 0) return;
        if (offline != nullptr)
        {
            const std::function<void (int)> job = [&fn] (int i) { fn (i); };
            offline->run (count, job);
        }
        else if (realtime != nullptr && realtime->size() > 0 && count > 1 && multi)
        {
            // ponteiro de funcao + contexto: nada de std::function (alocacao) no callback
            realtime->run (count, [] (void* ctx, int index) { (*static_cast<Fn*> (ctx)) (index); },
                           const_cast<void*> (static_cast<const void*> (&fn)));
        }
        else
            for (int i = 0; i < count; ++i) fn (i);
    }

    juce::String stemFileName (const juce::String& prefix, int number, const juce::String& name)
    {
        auto base = (prefix.isNotEmpty() ? prefix + " - " : juce::String())
                    + juce::String (number).paddedLeft ('0', 2) + " - " + (name.isNotEmpty() ? name : juce::String ("Faixa"));
        return juce::File::createLegalFileName (base) + ".wav";
    }
}

Engine::Engine()
{
    master.colour = juce::Colour (0xffe8edf2);
    streamingWorker = std::make_unique<StreamWorker>();
    // um nucleo fica para a thread de audio, outro para interface/disco
    const int cores = (int) std::thread::hardware_concurrency();
    realtimePool = std::make_unique<RealtimeTrackPool> (juce::jlimit (0, 8, cores - 2));
    prepare (48000.0, 512);
}

int Engine::getRealtimeWorkerCount() const noexcept
{
    return realtimePool != nullptr ? realtimePool->size() : 0;
}

Engine::~Engine()
{
    realtimePool.reset();
    offlineTrackPool.reset();
    streamingWorker.reset(); // termina I/O antes dos objetos Track
    forEachChannel ([this] (Channel& c)
    {
        for (auto& s : c.inserts)
            if (s.processor != nullptr) s.processor->removeListener (this);
    });
}

void Engine::forEachChannel (const std::function<void (Channel&)>& fn)
{
    for (auto& t : tracks) fn (*t);
    for (auto& b : buses) fn (*b);
    fn (master);
}

//==============================================================================
void Engine::configureStereo (juce::AudioProcessor& p)
{
    const auto stereo = juce::AudioChannelSet::stereo();
    auto layout = p.getBusesLayout();
    if (layout.inputBuses.size() > 0)  layout.inputBuses.getReference (0) = stereo;
    if (layout.outputBuses.size() > 0) layout.outputBuses.getReference (0) = stereo;

    auto withoutAux = layout;
    for (int i = 1; i < withoutAux.inputBuses.size(); ++i)  withoutAux.inputBuses.getReference (i) = juce::AudioChannelSet::disabled();
    for (int i = 1; i < withoutAux.outputBuses.size(); ++i) withoutAux.outputBuses.getReference (i) = juce::AudioChannelSet::disabled();

    if (p.checkBusesLayoutSupported (withoutAux))  { p.setBusesLayout (withoutAux); return; }
    if (p.checkBusesLayoutSupported (layout))      { p.setBusesLayout (layout); return; }

    // mono -> estereo (alguns plugins so aceitam assim)
    auto monoIn = layout;
    if (monoIn.inputBuses.size() > 0) monoIn.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
    if (p.checkBusesLayoutSupported (monoIn)) p.setBusesLayout (monoIn);
}

juce::String Engine::prepareProcessor (juce::AudioProcessor& p)
{
    configureStereo (p);
    const double sr = sampleRate.load();
    p.setRateAndBufferSizeDetails (sr, blockSize);
    p.prepareToPlay (sr, blockSize);
    const int needed = juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
    if (needed > kMaxPluginChannels)
        return "O plugin pede " + juce::String (needed) + " canais; o limite e " + juce::String (kMaxPluginChannels) + ".";
    if (p.getTotalNumOutputChannels() == 0)
        return "O plugin nao tem saida de audio (instrumento/MIDI nao e suportado).";
    return {};
}

void Engine::prepareSlot (InsertSlot& s)
{
    if (s.remote != nullptr &&
        ! s.remote->matchesConfiguration (sampleRate.load (std::memory_order_relaxed), blockSize))
    {
        // Novas taxas/buffers exigem outro handshake. Nunca reiniciar o
        // processo no callback da interface de audio.
        s.remote->invalidateForReconfigure();
        s.faulted.store (true, std::memory_order_relaxed);
    }
    if (auto* p = s.processor.get())
    {
        p->releaseResources();
        prepareProcessor (*p);
        s.bufferChannels = juce::jlimit (2, kMaxPluginChannels,
                                         juce::jmax (p->getTotalNumInputChannels(), p->getTotalNumOutputChannels()));
        s.mainOutChannels = p->getMainBusNumOutputChannels();
    }
}

void Engine::prepareChannel (Channel& c)
{
    const double sr = sampleRate.load();
    for (auto* sm : { &c.smoothL, &c.smoothR })
    {
        const float target = sm->getTargetValue();
        sm->reset (sr, 0.02);
        sm->setCurrentAndTargetValue (target);
    }
    if (c.kind != ChannelKind::master)
    {
        c.processingBuffer.setSize (kMaxPluginChannels + 1, blockSize);
        c.processingMidi.ensureSize (4096);
    }
    if (c.kind == ChannelKind::track)
    {
        auto& t = static_cast<Track&> (c);
        for (auto& s : t.sends)
        {
            const float target = s.smooth.getTargetValue();
            s.smooth.reset (sr, 0.02);
            s.smooth.setCurrentAndTargetValue (target);
        }
    }
    for (auto& s : c.inserts) prepareSlot (s);
    c.pdc.clear();
}

void Engine::prepare (double newRate, int newBlock)
{
    const juce::ScopedLock sl (lock);
    if (newRate > 0 && newRate != sampleRate.load()) srChanged = true;
    if (newRate > 0) sampleRate = newRate;
    blockSize = juce::jmax (32, newBlock);
    scratch.setSize (kMaxPluginChannels + 1, blockSize);
    direct.setSize (2, blockSize);
    masterBuf.setSize (2, blockSize);
    deviceBuf.setSize (2, blockSize);
    for (auto& b : busIn) b.setSize (2, blockSize);
    midi.ensureSize (4096);
    truePeakMeter.reset();
    momentaryLoudness.prepare (sampleRate.load (std::memory_order_relaxed));
    master.momentaryLufs.store (-120.0f, std::memory_order_relaxed);
    master.truePeakL.store (0.0f, std::memory_order_relaxed);
    master.truePeakR.store (0.0f, std::memory_order_relaxed);
    forEachChannel ([this] (Channel& c) { prepareChannel (c); });
    directDelay.clear();
    updateLatency();
    updateLength();
}

//==============================================================================
Track* Engine::addTrack (std::unique_ptr<Track> t)
{
    prepareChannel (*t);
    auto* raw = t.get();
    {
        const juce::ScopedLock sl (lock);
        tracks.push_back (std::move (t));
        updateLatency();
    }
    updateLength();
    return raw;
}

std::unique_ptr<Track> Engine::removeTrack (Track* t)
{
    std::unique_ptr<Track> out;
    {
        const juce::ScopedLock sl (lock);
        for (auto it = tracks.begin(); it != tracks.end(); ++it)
            if (it->get() == t) { out = std::move (*it); tracks.erase (it); break; }
        updateLatency();
    }
    if (out != nullptr)
        for (auto& s : out->inserts)
            if (s.processor != nullptr) s.processor->removeListener (this);
    updateLength();
    return out;
}

void Engine::moveTrack (int from, int to)
{
    const juce::ScopedLock sl (lock);
    if (! juce::isPositiveAndBelow (from, (int) tracks.size())) return;
    to = juce::jlimit (0, (int) tracks.size() - 1, to);
    auto t = std::move (tracks[(size_t) from]);
    tracks.erase (tracks.begin() + from);
    tracks.insert (tracks.begin() + to, std::move (t));
}

Bus* Engine::addBus (const juce::String& name)
{
    if ((int) buses.size() >= kMaxBuses) return nullptr;
    auto b = std::make_unique<Bus>();
    b->name = name.isNotEmpty() ? name : "BUS " + juce::String ((int) buses.size() + 1);
    b->colour = juce::Colour (0xffffc46b);
    prepareChannel (*b);
    auto* raw = b.get();
    const juce::ScopedLock sl (lock);
    buses.push_back (std::move (b));
    updateLatency();
    return raw;
}

int Engine::indexOfBus (const Bus* b) const
{
    for (size_t i = 0; i < buses.size(); ++i)
        if (buses[i].get() == b) return (int) i;
    return -1;
}

std::unique_ptr<Bus> Engine::removeBus (Bus* b)
{
    std::unique_ptr<Bus> out;
    {
        const juce::ScopedLock sl (lock);
        const int idx = indexOfBus (b);
        if (idx < 0) return {};
        out = std::move (buses[(size_t) idx]);
        buses.erase (buses.begin() + idx);

        auto fix = [idx] (std::atomic<int>& v)
        {
            const int cur = v.load();
            if (cur == idx) v = -1;
            else if (cur > idx) v = cur - 1;
        };
        for (auto& t : tracks)
        {
            fix (t->outputBus);
            for (auto& s : t->sends) fix (s.bus);
        }
        updateLatency();
    }
    for (auto& s : out->inserts)
        if (s.processor != nullptr) s.processor->removeListener (this);
    return out;
}

std::unique_ptr<juce::AudioProcessor> Engine::setInsert (Channel& c, int slot, std::unique_ptr<juce::AudioProcessor> p,
                                                         const juce::PluginDescription* desc)
{
    jassert (juce::isPositiveAndBelow (slot, kMaxInsertsPerChannel));
    if (slot >= c.inserts.size()) setInsertCount (c, slot + 1); // projeto com slot alem dos visiveis
    auto& s = c.inserts[(size_t) slot];
    std::unique_ptr<juce::AudioProcessor> old;
    std::shared_ptr<RemoteDspInsert> retiredRemote;
    {
        const juce::ScopedLock sl (lock);
        old = std::move (s.processor);
        retiredRemote = std::move (s.remote);
        s.processor = std::move (p);
        s.hasDescription = desc != nullptr;
        s.preferRemote = false;
        if (desc != nullptr) s.description = *desc;
        s.missingState.reset();
        s.bypassed = false;
        s.offline = false;
        s.faulted.store (false, std::memory_order_relaxed);
        c.wake();
        if (auto* np = s.processor.get())
        {
            s.bufferChannels = juce::jlimit (2, kMaxPluginChannels,
                                             juce::jmax (np->getTotalNumInputChannels(), np->getTotalNumOutputChannels()));
            s.mainOutChannels = np->getMainBusNumOutputChannels();
            np->addListener (this);
        }
        updateLatency();
    }
    if (old != nullptr) old->removeListener (this);
    return old;
}

std::unique_ptr<juce::AudioProcessor> Engine::setRemoteInsert (
    Channel& c, int slot, std::shared_ptr<RemoteDspInsert> ipcInsert,
    const juce::PluginDescription& desc)
{
    if (! juce::isPositiveAndBelow (slot, kMaxInsertsPerChannel) || ipcInsert == nullptr)
        return {};
    if (slot >= c.inserts.size()) setInsertCount (c, slot + 1);
    auto& entry = c.inserts[(size_t) slot];
    std::unique_ptr<juce::AudioProcessor> old;
    std::shared_ptr<RemoteDspInsert> oldRemote;
    {
        const juce::ScopedLock sl (lock);
        old = std::move (entry.processor);
        oldRemote = std::move (entry.remote);
        entry.remote = std::move (ipcInsert);
        entry.description = desc;
        entry.hasDescription = true;
        entry.preferRemote = true;
        entry.missingState = entry.remote->getInitialState();
        entry.bypassed.store (false, std::memory_order_relaxed);
        entry.faulted.store (false, std::memory_order_relaxed);
        entry.offline = false;
        c.wake();
        entry.bufferChannels = 2;
        entry.mainOutChannels = 2;
        updateLatency();
    }
    if (old != nullptr) old->removeListener (this);
    // Worker encerrado e mapeamento liberado FORA do lock da engine.
    oldRemote.reset();
    return old;
}

std::unique_ptr<juce::AudioProcessor> Engine::clearInsert (Channel& c, int slot)
{
    auto old = setInsert (c, slot, nullptr, nullptr);
    c.inserts[(size_t) slot].hasDescription = false;
    return old;
}

void Engine::setMissingInsert (Channel& c, int slot, const juce::PluginDescription& d,
                               const juce::MemoryBlock& state, bool preferRemote, bool offline)
{
    auto old = setInsert (c, slot, nullptr, &d);
    c.inserts[(size_t) slot].missingState = state;
    c.inserts[(size_t) slot].preferRemote = preferRemote;
    c.inserts[(size_t) slot].offline = offline;
    jassert (old == nullptr);
}

std::unique_ptr<juce::AudioProcessor> Engine::takeInsertOffline (Channel& c, int slot)
{
    if (! c.hasInsert (slot)) return {};
    auto& s = c.inserts[(size_t) slot];
    if (s.processor == nullptr && s.remote == nullptr) return {};
    // Estado lido na thread de mensagens, antes de tirar o plugin do motor.
    juce::MemoryBlock state;
    if (s.processor != nullptr) s.processor->getStateInformation (state);
    else state = s.missingState; // DSP isolado: estado inicial guardado
    std::unique_ptr<juce::AudioProcessor> old;
    std::shared_ptr<RemoteDspInsert> retiredRemote;
    {
        const juce::ScopedLock sl (lock);
        old = std::move (s.processor);
        retiredRemote = std::move (s.remote);
        s.missingState = std::move (state);
        s.offline = true;
        s.faulted.store (false, std::memory_order_relaxed);
        c.wake();
        updateLatency();
    }
    if (old != nullptr) old->removeListener (this);
    retiredRemote.reset(); // processo DSP encerrado fora do lock
    return old;
}

void Engine::swapInserts (Channel& c, int a, int b)
{
    if (! c.hasInsert (a) || ! c.hasInsert (b)) return;
    const juce::ScopedLock sl (lock);
    for (auto& l : c.lanes) // a automacao acompanha o plugin
        if (l->target == AutoTarget::plugin)
        {
            if (l->slot == a) l->slot = b;
            else if (l->slot == b) l->slot = a;
        }
    auto& x = c.inserts[(size_t) a];
    auto& y = c.inserts[(size_t) b];
    std::swap (x.processor, y.processor);
    std::swap (x.remote, y.remote);
    std::swap (x.description, y.description);
    std::swap (x.hasDescription, y.hasDescription);
    std::swap (x.preferRemote, y.preferRemote);
    std::swap (x.missingState, y.missingState);
    std::swap (x.offline, y.offline);
    std::swap (x.bufferChannels, y.bufferChannels);
    std::swap (x.mainOutChannels, y.mainOutChannels);
    const bool bx = x.bypassed.load();
    x.bypassed = y.bypassed.load();
    y.bypassed = bx;
    const bool fx = x.faulted.load();
    x.faulted = y.faulted.load();
    y.faulted = fx;
    c.wake();
}

void Engine::setBypass (Channel& c, int slot, bool b)
{
    if (! c.hasInsert (slot)) return;
    const juce::ScopedLock sl (lock);
    auto& insert = c.inserts[(size_t) slot];
    insert.bypassed = b;
    if (! b) insert.faulted.store (false, std::memory_order_relaxed); // retry explicito
    c.wake();
    updateLatency();
}

bool Engine::setInsertCount (Channel& c, int count)
{
    count = juce::jlimit (1, kMaxInsertsPerChannel, count);
    const juce::ScopedLock sl (lock); // o audio itera os inserts sob este lock
    // so remove slots vazios do fim (plugins e automacao ficam intactos)
    const int before = c.inserts.size();
    while (c.inserts.size() > count && c.inserts[(size_t) c.inserts.size() - 1].isEmpty())
        c.inserts.resize (c.inserts.size() - 1);
    if (c.inserts.size() < count) c.inserts.resize (count);
    return c.inserts.size() != before;
}

bool Engine::setSendCount (Track& t, int count)
{
    count = juce::jlimit (1, kMaxSendsPerTrack, count);
    const juce::ScopedLock sl (lock);
    bool changed = false;
    while (t.sends.size() > count && t.sends[(size_t) t.sends.size() - 1].bus.load() < 0)
    {
        t.sends.resize (t.sends.size() - 1);
        changed = true;
    }
    if (t.sends.size() < count)
    {
        const int first = t.sends.size();
        t.sends.resize (count);
        for (int i = first; i < count; ++i)
        {
            auto& s = t.sends[(size_t) i];
            s.smooth.reset (sampleRate.load(), 0.02);
            s.smooth.setCurrentAndTargetValue (0.0f);
        }
        changed = true;
    }
    return changed;
}

void Engine::setTrackAudio (Track& t, std::shared_ptr<const juce::AudioBuffer<float>> original, double rate,
                            std::shared_ptr<const juce::AudioBuffer<float>> pb, double pbRate)
{
    {
        const juce::ScopedLock sl (lock);
        t.diskStream.reset();
        t.original = std::move (original);
        t.originalRate = rate;
        t.playback = std::move (pb);
        t.playbackRate = pbRate;
        t.sourceChannels = t.original != nullptr ? t.original->getNumChannels() : 0;
    }
    updateLength();
}

void Engine::setTrackStream (Track& t, std::shared_ptr<StreamingAudio> stream)
{
    if (stream == nullptr) return;
    {
        const juce::ScopedLock sl (lock);
        t.original.reset();
        t.playback.reset();
        t.originalRate = stream->getSourceRate(); // taxa do arquivo (exibida na faixa)
        t.playbackRate = stream->getRate();
        t.sourceChannels = stream->getChannels();
        t.diskStream = stream;
    }
    streamingWorker->add (stream); // fora do lock do callback
    updateLength();
}

void Engine::clearSession (std::vector<std::unique_ptr<juce::AudioProcessor>>& toDelete,
                           std::vector<std::unique_ptr<Track>>& oldTracks,
                           std::vector<std::unique_ptr<Bus>>& oldBuses)
{
    std::vector<std::shared_ptr<RemoteDspInsert>> retiredRemote;
    {
        const juce::ScopedLock sl (lock);
        playing = false;
        position = 0;
        loopOn = false;
        oldTracks = std::move (tracks);
        tracks.clear();
        oldBuses = std::move (buses);
        buses.clear();
        for (auto& s : master.inserts)
        {
            if (s.processor != nullptr) toDelete.push_back (std::move (s.processor));
            if (s.remote != nullptr) retiredRemote.push_back (std::move (s.remote));
            s.hasDescription = false;
            s.preferRemote = false;
            s.missingState.reset();
            s.bypassed = false;
            s.offline = false;
        }
        master.gainDb = 0.0f; master.pan = 0.0f; master.mute = false; master.mono = false;
        updateLatency();
    }
    for (auto& p : toDelete) p->removeListener (this);
    auto unlisten = [this] (Channel& c) { for (auto& s : c.inserts) if (s.processor) s.processor->removeListener (this); };
    for (auto& t : oldTracks) unlisten (*t);
    for (auto& b : oldBuses) unlisten (*b);
    updateLength();
}

//==============================================================================
// Automacao
AutoLane* Engine::findLane (Channel& c, AutoTarget tg, int slot, int param) const
{
    for (auto& l : c.lanes)
        if (l->matches (tg, slot, param)) return l.get();
    return nullptr;
}

AutoLane* Engine::getOrCreateLane (Channel& c, AutoTarget tg, int slot, int param)
{
    if (auto* l = findLane (c, tg, slot, param)) return l;
    auto lane = std::make_unique<AutoLane>();
    lane->target = tg;
    lane->slot = tg == AutoTarget::plugin ? slot : -1;
    lane->param = tg == AutoTarget::plugin ? param : -1;
    lane->step = isStepTarget (c, tg, slot, param);
    auto* raw = lane.get();
    const juce::ScopedLock sl (lock);
    c.lanes.push_back (std::move (lane));
    return raw;
}

void Engine::removeLane (Channel& c, AutoLane* lane)
{
    std::unique_ptr<AutoLane> gone;
    {
        const juce::ScopedLock sl (lock);
        for (auto it = c.lanes.begin(); it != c.lanes.end(); ++it)
            if (it->get() == lane) { gone = std::move (*it); c.lanes.erase (it); break; }
    }
}

void Engine::removeLanesForSlot (Channel& c, int slot)
{
    std::vector<std::unique_ptr<AutoLane>> gone;
    const juce::ScopedLock sl (lock);
    for (auto it = c.lanes.begin(); it != c.lanes.end();)
    {
        if ((*it)->target == AutoTarget::plugin && (*it)->slot == slot) { gone.push_back (std::move (*it)); it = c.lanes.erase (it); }
        else ++it;
    }
}

bool Engine::currentValue (Channel& c, AutoTarget tg, int slot, int param, float& value)
{
    switch (tg)
    {
        case AutoTarget::volume: value = c.gainDb.load(); return true;
        case AutoTarget::pan:    value = c.pan.load(); return true;
        case AutoTarget::mute:   value = c.mute.load() ? 1.0f : 0.0f; return true;
        case AutoTarget::plugin:
            if (c.hasInsert (slot))
                if (auto* p = c.inserts[(size_t) slot].processor.get())
                {
                    auto& params = p->getParameters();
                    if (juce::isPositiveAndBelow (param, params.size())) { value = params[param]->getValue(); return true; }
                }
            return false;
    }
    return false;
}

bool Engine::isStepTarget (Channel& c, AutoTarget tg, int slot, int param)
{
    if (tg == AutoTarget::mute) return true;
    if (tg != AutoTarget::plugin || ! c.hasInsert (slot)) return false;
    if (auto* p = c.inserts[(size_t) slot].processor.get())
    {
        auto& params = p->getParameters();
        if (juce::isPositiveAndBelow (param, params.size()))
            return params[param]->isDiscrete() || params[param]->isBoolean();
    }
    return false;
}

void Engine::applyAutomation (Channel& c, double t)
{
    if (c.lanes.empty() || c.getAutoMode() == AutoMode::off) return;
    for (auto& lp : c.lanes)
    {
        auto& l = *lp;
        if (l.points.empty() || l.writing.load (std::memory_order_relaxed)) continue;
        const float v = l.valueAt (t);
        switch (l.target)
        {
            case AutoTarget::volume: c.gainDb.store (juce::jlimit (kMinDb, 12.0f, v), std::memory_order_relaxed); break;
            case AutoTarget::pan:    c.pan.store (juce::jlimit (-1.0f, 1.0f, v), std::memory_order_relaxed); break;
            case AutoTarget::mute:   c.mute.store (v >= 0.5f, std::memory_order_relaxed); break;
            case AutoTarget::plugin:
                if (c.hasInsert (l.slot))
                    if (auto* p = c.inserts[(size_t) l.slot].processor.get())
                    {
                        auto& params = p->getParameters();
                        if (juce::isPositiveAndBelow (l.param, params.size()))
                        {
                            auto* pr = params[l.param];
                            const float nv = juce::jlimit (0.0f, 1.0f, v);
                            if (std::abs (pr->getValue() - nv) > 1.0e-6f) pr->setValue (nv);
                        }
                    }
                break;
        }
    }
}

void Engine::pushParamEvent (juce::AudioProcessor* p, int index, int type)
{
    const juce::SpinLock::ScopedLockType sl (paramPushLock);
    const auto scope = paramFifo.write (1);
    if (scope.blockSize1 > 0) paramEvents[(size_t) scope.startIndex1] = { p, index, type };
}

bool Engine::popParamEvent (ParamEvent& e)
{
    const auto scope = paramFifo.read (1);
    if (scope.blockSize1 <= 0) return false;
    e = paramEvents[(size_t) scope.startIndex1];
    return true;
}

void Engine::audioProcessorParameterChanged (juce::AudioProcessor* p, int index, float) { pushParamEvent (p, index, 0); }
void Engine::audioProcessorParameterChangeGestureBegin (juce::AudioProcessor* p, int index) { pushParamEvent (p, index, 1); }
void Engine::audioProcessorParameterChangeGestureEnd (juce::AudioProcessor* p, int index) { pushParamEvent (p, index, 2); }

//==============================================================================
void Engine::audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d)
{
    if (d.latencyChanged) latencyDirty = true;
}

void Engine::pollLatencyChanges()
{
    if (! latencyDirty.load() || rendering.load()) return;
    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked()) return;
    latencyDirty = false;
    updateLatency();
}

int Engine::chainLatencyOf (const Channel& c) const
{
    int sum = 0;
    for (auto& s : c.inserts)
    {
        // Mesmo durante bypass/falha, o DSP remoto devolve o dry atrasado.
        // A PDC nunca deve pular ao perder o worker.
        if (s.remote != nullptr)
            sum += s.remote->getLatencySamples();
        else if (s.processor != nullptr && ! s.bypassed.load())
            sum += juce::jmax (0, s.processor->getLatencySamples());
    }
    return sum;
}

void Engine::updateLatency()
{
    int trackMax = 0, busMax = 0;
    for (auto& t : tracks) { t->chainLatency = chainLatencyOf (*t); trackMax = juce::jmax (trackMax, t->chainLatency); }
    for (auto& b : buses)  { b->chainLatency = chainLatencyOf (*b); busMax = juce::jmax (busMax, b->chainLatency); }
    for (auto& t : tracks) if (t->pdc.getDelay() != trackMax - t->chainLatency) t->pdc.setDelay (trackMax - t->chainLatency);
    for (auto& b : buses)  if (b->pdc.getDelay() != busMax - b->chainLatency) b->pdc.setDelay (busMax - b->chainLatency);
    if (directDelay.getDelay() != busMax) directDelay.setDelay (busMax);
    master.chainLatency = chainLatencyOf (master);
    totalLatency = trackMax + busMax + master.chainLatency;
    for (auto& t : tracks) t->sleepHoldSamples = sleepHoldFor (*t);
    for (auto& b : buses)  b->sleepHoldSamples = sleepHoldFor (*b);
}

int Engine::sleepHoldFor (const Channel& c) const
{
    // Tempo de silencio na saida antes de dormir: cauda declarada pelos plugins
    // + 0,5 s, no minimo 3 s (cobre delays longos com eco esparso), mais a
    // latencia da cadeia. Cauda "infinita" (muitos reverbs/delays declaram assim
    // por nao saber o tamanho): espera 10 s de saida em silencio real; enquanto
    // o plugin produzir som (feedback, freeze) ele nunca dorme. DSP isolado: nunca.
    double tail = 0.0;
    bool any = false;
    for (auto& s : c.inserts)
    {
        if (s.remote != nullptr) return -1; // o worker espera blocos continuos
        if (s.processor == nullptr || s.bypassed.load (std::memory_order_relaxed)) continue;
        any = true;
        const double t = s.processor->getTailLengthSeconds();
        tail = juce::jmax (tail, (! std::isfinite (t) || t > 60.0) ? 10.0 : juce::jmax (0.0, t));
    }
    if (! any) return 0;
    const double sr = sampleRate.load (std::memory_order_relaxed);
    return (int) (juce::jmax (3.0, tail + 0.5) * sr) + c.chainLatency;
}

void Engine::updateLength()
{
    juce::int64 len = 0;
    const double sr = sampleRate.load();
    {
        const juce::ScopedLock sl (lock);
        for (auto& t : tracks)
        {
            if (t->diskStream != nullptr && t->playbackRate == sr)
                len = juce::jmax (len, t->diskStream->getLength());
            else if (t->playback != nullptr && t->playbackRate == sr)
                len = juce::jmax (len, (juce::int64) t->playback->getNumSamples());
            else if (t->original != nullptr && t->originalRate > 0)
                len = juce::jmax (len, (juce::int64) std::ceil (t->original->getNumSamples() * sr / t->originalRate));
        }
    }
    lengthSamples = len;
}

void Engine::setLoop (juce::int64 start, juce::int64 end)
{
    const juce::int64 minLen = juce::jmax ((juce::int64) blockSize * 2, (juce::int64) (sampleRate.load() * 0.1));
    if (end < start) std::swap (start, end);
    start = juce::jmax ((juce::int64) 0, start);
    if (end - start < minLen) end = start + minLen;
    loopStart = start;
    loopEnd = end;
}

//==============================================================================
void Engine::runInserts (Channel& c, int n,
                         juce::AudioBuffer<float>& work, juce::MidiBuffer& eventMidi)
{
    for (auto& s : c.inserts)
    {
        if (s.remote != nullptr)
        {
            s.remote->process (work.getWritePointer (0), work.getWritePointer (1),
                               n, s.bypassed.load (std::memory_order_relaxed));
            if (s.remote->isFaulted())
                s.faulted.store (true, std::memory_order_relaxed);
            continue;
        }
        auto* p = s.processor.get();
        if (p == nullptr || s.bypassed.load(std::memory_order_relaxed)) continue;

        const int ch = s.bufferChannels;
        for (int i = 2; i < ch; ++i) work.clear (i, 0, n);
        juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), ch, n);
        eventMidi.clear();
        bool failed = false;
        {
            const juce::ScopedLock pl (p->getCallbackLock());
            if (p->isSuspended()) continue;
            // Uma excecao C++ de plugin nao pode atravessar o callback da
            // interface de audio. Falhas de processo (SEH/access violation),
            // deadlocks e loops infinitos AINDA exigem DSP fora do processo.
            try
            {
                p->processBlock (view, eventMidi);
            }
            catch (...)
            {
                failed = true;
            }
        }
        if (failed)
        {
            // Silencia somente o bloco afetado; nos callbacks seguintes o
            // insert com falha fica automaticamente em bypass.
            s.faulted.store (true, std::memory_order_relaxed);
            s.bypassed.store (true, std::memory_order_relaxed);
            latencyDirty.store (true, std::memory_order_relaxed); // PDC refeito pela thread de controle
            pluginExceptions.fetch_add (1, std::memory_order_relaxed);
            work.clear (0, n);
            continue;
        }
        if (s.mainOutChannels == 1) work.copyFrom (1, 0, work, 0, 0, n);
    }
}

namespace
{
    constexpr float kSleepInputThreshold = 1.0e-6f;  // -120 dBFS: entrada em silencio
    constexpr float kSleepOutputThreshold = 1.0e-5f; // -100 dBFS: cauda dos plugins acabou
}

void Engine::processChain (Channel& c, int n, juce::AudioBuffer<float>& work, juce::MidiBuffer& eventMidi, bool allowSleep)
{
    const int hold = c.sleepHoldSamples;
    if (! allowSleep || hold <= 0)
    {
        if (c.sleeping || c.silentSamples != 0) c.wake();
        runInserts (c, n, work, eventMidi);
        return;
    }
    const bool inputSilent = juce::jmax (work.getMagnitude (0, 0, n), work.getMagnitude (1, 0, n)) < kSleepInputThreshold;
    if (inputSilent && c.sleeping)
    {
        // Dormindo: os plugins nao sao chamados. A saida fica em silencio exato
        // (a cauda deles ja estava abaixo de -100 dBFS ao adormecer).
        work.clear (0, 0, n);
        work.clear (1, 0, n);
        return;
    }
    if (c.sleeping) c.wake(); // som voltou: processa ja este bloco
    runInserts (c, n, work, eventMidi);
    if (inputSilent && juce::jmax (work.getMagnitude (0, 0, n), work.getMagnitude (1, 0, n)) < kSleepOutputThreshold)
    {
        c.silentSamples += n;
        if (c.silentSamples >= hold)
        {
            c.sleeping = true;
            c.asleep.store (true, std::memory_order_relaxed);
        }
    }
    else
        c.silentSamples = 0;
}

void Engine::readTrackSource (Track& t, float* TL, float* TR, const ReadSeg* segs, int numSegs, double sr) noexcept
{
    if (auto* frozen = t.frozenStream.get())
    {
        if (frozen->getRate() == sr)
            for (int i = 0; i < numSegs; ++i)
                if (segs[i].src >= 0) frozen->copy (TL, TR, segs[i].dst, segs[i].src, segs[i].len);
        return;
    }
    const auto* buf = t.playback.get();
    if (t.diskStream != nullptr && t.playbackRate == sr)
    {
        for (int i = 0; i < numSegs; ++i)
        {
            const auto& sg = segs[i];
            if (sg.src >= 0)
                t.diskStream->copy (TL, TR, sg.dst, sg.src, sg.len);
        }
    }
    else if (buf != nullptr && t.playbackRate == sr)
    {
        const auto len = (juce::int64) buf->getNumSamples();
        const bool mono = buf->getNumChannels() == 1;
        for (int i = 0; i < numSegs; ++i)
        {
            const auto& sg = segs[i];
            if (sg.src >= len || sg.src < 0) continue;
            const int cnt = (int) juce::jmin ((juce::int64) sg.len, len - sg.src);
            juce::FloatVectorOperations::copy (TL + sg.dst, buf->getReadPointer (0, (int) sg.src), cnt);
            juce::FloatVectorOperations::copy (TR + sg.dst, buf->getReadPointer (mono ? 0 : 1, (int) sg.src), cnt);
        }
    }
}

juce::int64 Engine::render (float* outL, float* outR, int n, juce::int64 pos, bool play, bool allowLoop)
{
    jassert (n <= blockSize);
    const double sr = sampleRate.load();

    // Trechos do arquivo lidos neste bloco (o loop pode dividir o bloco).
    ReadSeg segs[8];
    int numSegs = 0;
    juce::int64 p = pos;
    if (play)
    {
        const juce::int64 ls = loopStart.load(), le = loopEnd.load();
        const bool loop = allowLoop && loopOn.load() && le > ls;
        const juce::int64 stop = (allowLoop && ! loop) ? stopAt.load (std::memory_order_relaxed) : -1;
        int done = 0;
        while (done < n && numSegs < 8)
        {
            int len = n - done;
            if (stop >= 0)
            {
                if (p >= stop) break; // resto do bloco em silencio
                if (p + len > stop) len = (int) (stop - p);
            }
            if (loop && p < le && p + len > le) len = (int) (le - p);
            segs[numSegs++] = { p, done, len };
            done += len;
            p += len;
            if (loop && p == le) p = ls;
        }
    }

    const bool snap = snapGains;
    snapGains = false;

    // Automacao: aplicada tocando ou quando o cursor foi movido.
    const bool applyAuto = play || pos != lastRenderEnd;
    const double tSec = (double) pos / sr;
    if (applyAuto) forEachChannelNoAlloc ([this, tSec] (Channel& c) { applyAutomation (c, tSec); });

    const int numBuses = (int) buses.size();
    for (int b = 0; b < numBuses; ++b) busIn[(size_t) b].clear (0, n);
    direct.clear (0, n);

    bool anySolo = false;
    for (auto& t : tracks) anySolo = anySolo || t->solo.load (std::memory_order_relaxed);
    if (stemTaps != nullptr) anySolo = false; // stems: cada faixa sai mesmo sem solo

    auto* L = scratch.getWritePointer (0);
    auto* R = scratch.getWritePointer (1);
    // Economia de CPU so no playback: a exportacao processa tudo, sempre.
    const bool allowSleep = cpuSaver.load (std::memory_order_relaxed) && ! rendering.load (std::memory_order_relaxed);

    // Pre-processamento de leitura/inserts/PDC independente por faixa.
    // Na exportacao offline, distribui-se entre nucleos; no callback da
    // placa, roda sequencialmente, sem barreiras nem waits na thread realtime.
    const auto processTrack = [&] (int trackIndex)
    {
        auto& t = *tracks[(size_t) trackIndex];
        // Scratch por faixa: elimina compartilhamento de estado de plugin
        // com as outras faixas; sem alocacao dentro do callback.
        auto* TL = t.processingBuffer.getWritePointer (0);
        auto* TR = t.processingBuffer.getWritePointer (1);
        juce::FloatVectorOperations::clear (TL, n);
        juce::FloatVectorOperations::clear (TR, n);

        if (play) readTrackSource (t, TL, TR, segs, numSegs, sr);
        if (t.phase.load (std::memory_order_relaxed) && ! t.isFrozen()) // congelada: fase ja aplicada
        {
            juce::FloatVectorOperations::negate (TL, TL, n);
            juce::FloatVectorOperations::negate (TR, TR, n);
        }

        processChain (t, n, t.processingBuffer, t.processingMidi, allowSleep);
        t.pdc.process (TL, TR, n);

    };
    const bool multi = multiCore.load (std::memory_order_relaxed);
    runChannels (offlineTrackPool.get(), realtimePool.get(), multi, (int) tracks.size(), processTrack);

    // Somatoria deterministica: envios, faders e buses na ordem original.
    for (size_t ti = 0; ti < tracks.size(); ++ti)
    {
        auto& t = *tracks[ti];
        auto* TL = t.processingBuffer.getWritePointer (0);
        auto* TR = t.processingBuffer.getWritePointer (1);
        const bool audible = ! t.mute.load (std::memory_order_relaxed) && (! anySolo || t.solo.load (std::memory_order_relaxed));
        const float g = audible ? dbToGain (t.gainDb.load (std::memory_order_relaxed)) : 0.0f;
        float pl, pr;
        panGains (t.pan.load (std::memory_order_relaxed), t.sourceChannels == 1, pl, pr);
        setGainTarget (t.smoothL, g * pl, snap);
        setGainTarget (t.smoothR, g * pr, snap);

        // envios pre-fader
        for (auto& s : t.sends)
        {
            const int b = s.bus.load (std::memory_order_relaxed);
            const bool valid = b >= 0 && b < numBuses;
            setGainTarget (s.smooth, valid && audible ? dbToGain (s.levelDb.load (std::memory_order_relaxed)) : 0.0f, snap);
            if (! valid) { s.smooth.skip (n); continue; }
            if (! s.preFader.load (std::memory_order_relaxed)) continue;
            auto& dst = busIn[(size_t) b];
            auto sm = s.smooth; // mesma rampa para TL e TR
            addWithRamp (dst.getWritePointer (0), TL, n, sm);
            addWithRamp (dst.getWritePointer (1), TR, n, s.smooth);
        }

        // fader + pan
        if (t.smoothL.isSmoothing() || t.smoothR.isSmoothing())
            for (int i = 0; i < n; ++i) { TL[i] *= t.smoothL.getNextValue(); TR[i] *= t.smoothR.getNextValue(); }
        else
        {
            juce::FloatVectorOperations::multiply (TL, t.smoothL.getTargetValue(), n);
            juce::FloatVectorOperations::multiply (TR, t.smoothR.getTargetValue(), n);
        }

        // envios pos-fader
        for (auto& s : t.sends)
        {
            const int b = s.bus.load (std::memory_order_relaxed);
            if (b < 0 || b >= numBuses || s.preFader.load (std::memory_order_relaxed)) continue;
            auto& dst = busIn[(size_t) b];
            auto sm = s.smooth;
            addWithRamp (dst.getWritePointer (0), TL, n, sm);
            addWithRamp (dst.getWritePointer (1), TR, n, s.smooth);
        }

        if (stemTaps != nullptr)
            if (auto* tap = stemTaps->tracks[ti])
            {
                tap->copyFrom (0, 0, TL, n);
                tap->copyFrom (1, 0, TR, n);
            }

        const int ob = t.outputBus.load (std::memory_order_relaxed);
        auto& out = (ob >= 0 && ob < numBuses) ? busIn[(size_t) ob] : direct;
        out.addFrom (0, 0, TL, n);
        out.addFrom (1, 0, TR, n);

        storePeak (t.peakL, t.processingBuffer.getMagnitude (0, 0, n));
        storePeak (t.peakR, t.processingBuffer.getMagnitude (1, 0, n));
    }

    // ---------------- Buses ----------------
    // Cada bus tem buffer proprio: inserts dos buses (reverbs, delays) rodam em
    // paralelo como as faixas. A soma no master continua na ordem original.
    const auto processBus = [&] (int bi)
    {
        auto& b = *buses[(size_t) bi];
        auto* BL = b.processingBuffer.getWritePointer (0);
        auto* BR = b.processingBuffer.getWritePointer (1);
        juce::FloatVectorOperations::copy (BL, busIn[(size_t) bi].getReadPointer (0), n);
        juce::FloatVectorOperations::copy (BR, busIn[(size_t) bi].getReadPointer (1), n);
        if (b.phase.load (std::memory_order_relaxed))
        {
            juce::FloatVectorOperations::negate (BL, BL, n);
            juce::FloatVectorOperations::negate (BR, BR, n);
        }
        processChain (b, n, b.processingBuffer, b.processingMidi, allowSleep);
        b.pdc.process (BL, BR, n);

        const float g = b.mute.load (std::memory_order_relaxed) ? 0.0f : dbToGain (b.gainDb.load (std::memory_order_relaxed));
        float pl, pr;
        panGains (b.pan.load (std::memory_order_relaxed), false, pl, pr);
        setGainTarget (b.smoothL, g * pl, snap);
        setGainTarget (b.smoothR, g * pr, snap);
        for (int i = 0; i < n; ++i) { BL[i] *= b.smoothL.getNextValue(); BR[i] *= b.smoothR.getNextValue(); }
        storePeak (b.peakL, b.processingBuffer.getMagnitude (0, 0, n));
        storePeak (b.peakR, b.processingBuffer.getMagnitude (1, 0, n));
    };
    runChannels (offlineTrackPool.get(), realtimePool.get(), multi, numBuses, processBus);

    masterBuf.clear (0, n);
    for (int bi = 0; bi < numBuses; ++bi)
    {
        auto& b = *buses[(size_t) bi];
        masterBuf.addFrom (0, 0, b.processingBuffer, 0, 0, n);
        masterBuf.addFrom (1, 0, b.processingBuffer, 1, 0, n);
        if (stemTaps != nullptr)
            if (auto* tap = stemTaps->buses[(size_t) bi])
            {
                tap->copyFrom (0, 0, b.processingBuffer, 0, 0, n);
                tap->copyFrom (1, 0, b.processingBuffer, 1, 0, n);
            }
    }

    // faixas direto no master: atrasadas pela maior latencia de bus
    directDelay.process (direct.getWritePointer (0), direct.getWritePointer (1), n);
    masterBuf.addFrom (0, 0, direct, 0, 0, n);
    masterBuf.addFrom (1, 0, direct, 1, 0, n);

    // ---------------- Master ----------------
    juce::FloatVectorOperations::copy (L, masterBuf.getReadPointer (0), n);
    juce::FloatVectorOperations::copy (R, masterBuf.getReadPointer (1), n);
    runInserts (master, n, scratch, midi);
    {
        const float g = master.mute.load (std::memory_order_relaxed) ? 0.0f : dbToGain (master.gainDb.load (std::memory_order_relaxed));
        float pl, pr;
        panGains (master.pan.load (std::memory_order_relaxed), false, pl, pr);
        setGainTarget (master.smoothL, g * pl, snap);
        setGainTarget (master.smoothR, g * pr, snap);
        for (int i = 0; i < n; ++i) { L[i] *= master.smoothL.getNextValue(); R[i] *= master.smoothR.getNextValue(); }
        if (master.mono.load (std::memory_order_relaxed))
            for (int i = 0; i < n; ++i) L[i] = R[i] = 0.5f * (L[i] + R[i]);
    }
    const float ml = scratch.getMagnitude (0, 0, n), mr = scratch.getMagnitude (1, 0, n);
    storePeak (master.peakL, ml);
    storePeak (master.peakR, mr);
    if (ml > 1.0f || mr > 1.0f) master.clipped = true;

    const auto tp = truePeakMeter.process (L, R, n);
    storePeak (master.truePeakL, tp.left);
    storePeak (master.truePeakR, tp.right);
    momentaryLoudness.process (L, R, n);
    master.momentaryLufs.store (momentaryLoudness.getLufsM(), std::memory_order_relaxed);

    juce::FloatVectorOperations::copy (outL, L, n);
    juce::FloatVectorOperations::copy (outR, R, n);
    lastRenderEnd = play ? p : pos;
    return lastRenderEnd;
}

//==============================================================================
void Engine::audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                               const juce::AudioIODeviceCallbackContext&)
{
    if (out == nullptr || n <= 0 || numOut <= 0) return;
    // Thread da placa (WASAPI ja usa MMCSS; ASIO e do driver): uma vez por thread,
    // tira do modo de economia do Windows 11. Chamada de sistema unica, sem alocacao.
    static thread_local bool throttlingOff = false;
    if (! throttlingOff) { throttlingOff = true; rt::disableThreadThrottling(); }
    // Denormais (caudas de reverb/filtros quase em silencio) podem multiplicar o
    // custo de CPU de plugins que nao se protegem: FTZ/DAZ no callback inteiro.
    juce::ScopedNoDenormals noDenormals;
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], n);

    // Nunca bloquear a thread de audio. Contadores ajudam a localizar dropout
    // por edicao concorrente e suspensao durante exportacao offline.
    if (n <= 0 || numOut <= 0 || out == nullptr) return;
    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked())
    {
        lockContentions.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    if (rendering.load (std::memory_order_relaxed))
    {
        renderSuspensions.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    processedCallbacks.fetch_add (1, std::memory_order_relaxed);
    const auto callbackStartTicks = juce::Time::getHighResolutionTicks();

    const auto startPos = position.load();
    const bool play = playing.load();
    auto pos = startPos;
    int done = 0;
    while (done < n)
    {
        const int m = juce::jmin (blockSize, n - done);
        auto* dl = deviceBuf.getWritePointer (0);
        auto* dr = deviceBuf.getWritePointer (1);
        pos = render (dl, dr, m, pos, play, true);
        if (numOut == 1 || out[1] == nullptr || out[0] == nullptr)
        {
            if (out[0] != nullptr)
                for (int i = 0; i < m; ++i) out[0][done + i] = 0.5f * (dl[i] + dr[i]);
        }
        else
        {
            juce::FloatVectorOperations::copy (out[0] + done, dl, m);
            juce::FloatVectorOperations::copy (out[1] + done, dr, m);
        }
        done += m;
    }

    // Saida de hardware limitada em +-1 (o motor interno e float).
    for (int c = 0; c < juce::jmin (2, numOut); ++c)
        if (out[c] != nullptr) juce::FloatVectorOperations::clip (out[c], out[c], -1.0f, 1.0f, n);

    // Conta callbacks cujo processamento ultrapassou a duracao do buffer.
    // Medicao apenas diagnostica; nao modifica o sinal nem bloqueia o audio.
    const double elapsed = juce::Time::highResolutionTicksToSeconds (
        juce::Time::getHighResolutionTicks() - callbackStartTicks);
    if (elapsed > (double) n / juce::jmax (1.0, sampleRate.load (std::memory_order_relaxed)))
        callbackOverruns.fetch_add (1, std::memory_order_relaxed);

    if (play)
    {
        auto expected = startPos;
        // se a interface moveu o cursor, vale o da interface (e o fim de selecao antigo nao conta)
        const bool advanced = position.compare_exchange_strong (expected, pos);
        const auto stop = stopAt.load();
        if (advanced && stop >= 0 && ! loopOn.load() && pos >= stop)
        {
            playing = false;
            position = stopReturn.load();
            stopAt = -1;
        }
        const auto len = lengthSamples.load();
        if (! loopOn.load() && len > 0 && pos > len + (juce::int64) (sampleRate.load() * 2.0))
            playing = false;
    }
}

void Engine::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    prepare (d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples());
}

void Engine::audioDeviceStopped() {}

//==============================================================================
juce::String Engine::beginOfflineRender()
{
    rendering = true;
    // Insert isolado (IPC) ja em falha tocaria so o sinal seco: o arquivo
    // ficaria diferente do que o usuario espera. Recusa em vez de exportar errado.
    juce::String faultedRemote;
    forEachChannel ([&faultedRemote] (Channel& c)
    {
        for (auto& s : c.inserts)
            if (s.remote != nullptr && s.remote->isFaulted() && ! s.bypassed.load() && faultedRemote.isEmpty())
                faultedRemote = c.name + " / " + s.getName();
    });
    if (faultedRemote.isNotEmpty())
    {
        rendering = false;
        return "O insert isolado " + faultedRemote + " esta em falha (sinal seco). "
               "Reinicie o processo do plugin ou coloque-o em bypass antes de exportar.";
    }

    forEachChannel ([] (Channel& c)
    {
        for (auto& s : c.inserts)
        {
            if (auto* p = s.processor.get()) { p->setNonRealtime (true); p->reset(); }
            if (s.remote != nullptr) s.remote->setOffline (true); // espera o worker no bounce
        }
        c.pdc.clear();
        c.wake();
    });
    directDelay.clear();
    snapGains = true; // primeiro bloco ja com os ganhos do estado atual (solo/mute/automacao)

    // Pool so durante o bounce, nunca criado no callback. Os workers realtime
    // ficam parados enquanto exporta (callback suspenso), entao usa o mesmo
    // numero de nucleos que o playback.
    const int cores = (int) std::thread::hardware_concurrency();
    const int workers = cores > 1 ? juce::jlimit (0, 8, cores - 1) : 0;
    if (workers > 0 && (tracks.size() >= 2 || buses.size() >= 2))
        offlineTrackPool = std::make_unique<OfflineTrackPool> (workers);
    return {};
}

void Engine::endOfflineRender()
{
    offlineTrackPool.reset(); // workers encerrados antes de resetar os plugins
    stemTaps = nullptr;
    forEachChannel ([] (Channel& c)
    {
        for (auto& s : c.inserts)
        {
            if (auto* p = s.processor.get()) { p->setNonRealtime (false); p->reset(); }
            if (s.remote != nullptr) s.remote->setOffline (false);
        }
        c.pdc.clear();
        c.wake();
    });
    directDelay.clear();
    lastRenderEnd = -1;
    rendering = false;
}

bool Engine::remoteFailedDuringRender()
{
    bool bad = false;
    forEachChannelNoAlloc ([&bad] (Channel& c)
    {
        for (auto& s : c.inserts)
            if (s.remote != nullptr && s.remote->isFaulted() && ! s.bypassed.load (std::memory_order_relaxed)) bad = true;
    });
    return bad;
}

namespace
{
    /** WAV num arquivo temporario ao lado do destino (substitui so no fim). */
    std::unique_ptr<juce::AudioFormatWriter> createWavWriter (juce::TemporaryFile& temp, double sr, int channels,
                                                              int bitDepth, juce::String& error)
    {
        auto stream = temp.getFile().createOutputStream();
        if (stream == nullptr) { error = "Nao foi possivel criar o arquivo " + temp.getTargetFile().getFullPathName(); return {}; }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sr, (unsigned int) channels, bitDepth, {}, 0));
        if (writer == nullptr) { error = "Formato WAV nao suportado nesta taxa/resolucao."; return {}; }
        stream.release(); // o writer passou a ser dono do stream
        return writer;
    }

    /** Dither TPDF (+-1 LSB) em 16 bits: evita distorcao de quantizacao em finais e fades. */
    void applyDither (juce::AudioBuffer<float>& buf, int channels, int start, int count, juce::Random& rng)
    {
        const float lsb = 1.0f / 32768.0f;
        for (int c = 0; c < channels; ++c)
        {
            auto* d = buf.getWritePointer (c, start);
            for (int i = 0; i < count; ++i) d[i] += (rng.nextFloat() - rng.nextFloat()) * lsb;
        }
    }
}

bool Engine::exportMix (const juce::File& file, int bitDepth, double tailSeconds,
                        std::atomic<float>& progress, std::atomic<bool>& cancel,
                        juce::String& error, bool& clipped,
                        juce::int64 rangeStart, juce::int64 rangeEnd)
{
    const juce::ScopedLock sl (lock);
    clipped = false;
    const double sr = sampleRate.load();
    const int bs = blockSize;

    const juce::int64 start = juce::jmax ((juce::int64) 0, rangeStart);
    const juce::int64 end = rangeEnd < 0 ? lengthSamples.load() : rangeEnd;
    const juce::int64 length = juce::jmax ((juce::int64) 0, end - start);
    const juce::int64 total = length + (juce::int64) (tailSeconds * sr);
    if (length <= 0 || total <= 0) { error = "Nada para exportar (trecho vazio)."; return false; }

    juce::TemporaryFile temp (file);
    auto writer = createWavWriter (temp, sr, 2, bitDepth, error);
    if (writer == nullptr) return false;

    error = beginOfflineRender();
    if (error.isNotEmpty()) return false;
    auto fail = [&] (const juce::String& message) { endOfflineRender(); writer.reset(); error = message; return false; };

    const int latency = totalLatency.load();
    const juce::int64 toProduce = total + latency;
    const bool dither = bitDepth == 16;
    juce::Random rng (0x51c0);

    juce::AudioBuffer<float> buf (2, bs);
    juce::int64 pos = start, produced = 0;
    while (produced < toProduce)
    {
        if (cancel.load()) return fail ("Exportacao cancelada.");
        // Sempre renderiza um bloco inteiro e grava so o necessario: inserts
        // isolados (IPC) so processam blocos do tamanho configurado, e um bloco
        // final menor sairia seco.
        const int m = (int) juce::jmin ((juce::int64) bs, toProduce - produced);
        // O bounce corre mais rapido que o playback. Prefetch sincrono fora da
        // thread realtime impede silencio por cache vazio em exports longos.
        for (auto& t : tracks)
        {
            if (t->frozenStream != nullptr) t->frozenStream->prefetchBlocking (pos, bs);
            else if (t->diskStream != nullptr && t->playbackRate == sr)
                t->diskStream->prefetchBlocking (pos, bs);
        }
        pos = render (buf.getWritePointer (0), buf.getWritePointer (1), bs, pos, true, false);
        if (remoteFailedDuringRender())
            return fail ("Um insert isolado (processo DSP) falhou ou travou durante a exportacao. "
                         "A exportacao foi interrompida para nao gravar trechos sem o plugin.");
        const int skip = produced < latency ? (int) juce::jmin ((juce::int64) m, (juce::int64) latency - produced) : 0;
        const int w = m - skip;
        if (w > 0)
        {
            if (buf.getMagnitude (skip, w) > 1.0f) clipped = true;
            if (dither) applyDither (buf, 2, skip, w, rng);
            if (! writer->writeFromAudioSampleBuffer (buf, skip, w))
                return fail ("Falha ao gravar o arquivo (disco cheio?).");
        }
        produced += m;
        progress = (float) ((double) produced / (double) juce::jmax ((juce::int64) 1, toProduce));
    }
    writer.reset();
    endOfflineRender();
    if (! temp.overwriteTargetFileWithTemporary()) { error = "Nao foi possivel substituir o arquivo de destino."; return false; }
    progress = 1.0f;
    return true;
}

bool Engine::exportStems (const StemExport& req, std::atomic<float>& progress, std::atomic<bool>& cancel,
                          juce::String& error, juce::StringArray& written, juce::StringArray& clippedNames)
{
    const juce::ScopedLock sl (lock);
    written.clear();
    clippedNames.clear();
    const double sr = sampleRate.load();
    const int bs = blockSize;

    const juce::int64 start = juce::jmax ((juce::int64) 0, req.rangeStart);
    const juce::int64 end = req.rangeEnd < 0 ? lengthSamples.load() : req.rangeEnd;
    const juce::int64 length = juce::jmax ((juce::int64) 0, end - start);
    if (length <= 0) { error = "Nada para exportar (trecho vazio)."; return false; }
    if (! req.folder.isDirectory() && ! req.folder.createDirectory())
    {
        error = "Nao foi possivel criar a pasta " + req.folder.getFullPathName();
        return false;
    }

    int trackLatency = 0;
    for (auto& t : tracks) trackLatency = juce::jmax (trackLatency, t->chainLatency);
    const int busLatency = trackLatency + directDelay.getDelay();
    const juce::int64 total = length + (req.processed ? (juce::int64) (req.tailSeconds * sr) : 0);

    struct Stem
    {
        juce::String name;
        Track* track = nullptr;
        int channels = 2, latency = 0;
        std::unique_ptr<juce::TemporaryFile> temp;
        std::unique_ptr<juce::AudioFormatWriter> writer;
        juce::AudioBuffer<float> buf;
        juce::int64 done = 0;
        bool clipped = false;
    };
    std::vector<std::unique_ptr<Stem>> stems;
    StemTaps taps;
    taps.tracks.assign (tracks.size(), nullptr);
    taps.buses.assign (buses.size(), nullptr);

    auto addStem = [&] (const juce::String& fileName, const juce::String& display, Track* track, int channels, int latency)
    {
        auto st = std::make_unique<Stem>();
        st->name = display;
        st->track = track;
        st->channels = channels;
        st->latency = latency;
        st->temp = std::make_unique<juce::TemporaryFile> (req.folder.getChildFile (fileName));
        st->writer = createWavWriter (*st->temp, sr, channels, req.bitDepth, error);
        if (st->writer == nullptr) return (Stem*) nullptr;
        st->buf.setSize (2, bs);
        stems.push_back (std::move (st));
        return stems.back().get();
    };

    int number = 1;
    for (int ti : req.tracks)
    {
        if (! juce::isPositiveAndBelow (ti, (int) tracks.size())) continue;
        auto& t = *tracks[(size_t) ti];
        if (! req.processed && t.sourceChannels == 0) continue; // faixa sem audio
        const int ch = req.processed ? 2 : (t.sourceChannels == 1 ? 1 : 2);
        auto* st = addStem (stemFileName (req.prefix, number++, t.name), t.name, &t, ch, trackLatency);
        if (st == nullptr) return false;
        taps.tracks[(size_t) ti] = &st->buf;
    }
    if (req.processed)
        for (int bi : req.buses)
        {
            if (! juce::isPositiveAndBelow (bi, (int) buses.size())) continue;
            auto& b = *buses[(size_t) bi];
            auto* st = addStem (stemFileName (req.prefix, number++, "BUS " + b.name), "BUS " + b.name, nullptr, 2, busLatency);
            if (st == nullptr) return false;
            taps.buses[(size_t) bi] = &st->buf;
        }
    if (stems.empty()) { error = "Nenhuma faixa com audio foi selecionada."; return false; }

    const bool dither = req.bitDepth == 16;
    juce::Random rng (0x51c0);
    auto finishWriters = [&]
    {
        for (auto& st : stems) st->writer.reset();
    };

    if (req.processed)
    {
        error = beginOfflineRender();
        if (error.isNotEmpty()) return false;
        stemTaps = &taps;
        auto fail = [&] (const juce::String& message) { endOfflineRender(); finishWriters(); error = message; return false; };

        const juce::int64 toProduce = total + busLatency;
        juce::AudioBuffer<float> mix (2, bs);
        juce::int64 pos = start, produced = 0;
        while (produced < toProduce)
        {
            if (cancel.load()) return fail ("Exportacao cancelada.");
            const int m = (int) juce::jmin ((juce::int64) bs, toProduce - produced);
            for (auto& t : tracks)
            {
                if (t->frozenStream != nullptr) t->frozenStream->prefetchBlocking (pos, bs);
                else if (t->diskStream != nullptr && t->playbackRate == sr)
                    t->diskStream->prefetchBlocking (pos, bs);
            }
            pos = render (mix.getWritePointer (0), mix.getWritePointer (1), bs, pos, true, false);
            if (remoteFailedDuringRender())
                return fail ("Um insert isolado (processo DSP) falhou ou travou durante a exportacao. "
                             "A exportacao foi interrompida para nao gravar trechos sem o plugin.");
            for (auto& st : stems)
            {
                const int skip = produced < st->latency ? (int) juce::jmin ((juce::int64) m, (juce::int64) st->latency - produced) : 0;
                const int w = (int) juce::jmin ((juce::int64) (m - skip), total - st->done);
                if (w <= 0) continue;
                if (st->buf.getMagnitude (skip, w) > 1.0f) st->clipped = true;
                if (dither) applyDither (st->buf, 2, skip, w, rng);
                if (! st->writer->writeFromAudioSampleBuffer (st->buf, skip, w))
                    return fail ("Falha ao gravar " + st->name + " (disco cheio?).");
                st->done += w;
            }
            produced += m;
            progress = (float) ((double) produced / (double) juce::jmax ((juce::int64) 1, toProduce));
        }
        endOfflineRender();
    }
    else
    {
        // Audio original: so leitura, sem plugins. O callback fica suspenso.
        rendering = true;
        auto fail = [&] (const juce::String& message) { rendering = false; finishWriters(); error = message; return false; };
        for (juce::int64 pos = start; pos < end; pos += bs)
        {
            if (cancel.load()) return fail ("Exportacao cancelada.");
            const int m = (int) juce::jmin ((juce::int64) bs, end - pos);
            for (auto& st : stems)
            {
                auto& t = *st->track;
                st->buf.clear();
                auto* L = st->buf.getWritePointer (0);
                auto* R = st->buf.getWritePointer (1);
                if (t.diskStream != nullptr && t.playbackRate == sr)
                {
                    t.diskStream->prefetchBlocking (pos, m);
                    t.diskStream->copy (L, R, 0, pos, m);
                }
                else if (auto* pb = t.playback.get(); pb != nullptr && t.playbackRate == sr)
                {
                    const auto len = (juce::int64) pb->getNumSamples();
                    if (pos < len)
                    {
                        const int cnt = (int) juce::jmin ((juce::int64) m, len - pos);
                        juce::FloatVectorOperations::copy (L, pb->getReadPointer (0, (int) pos), cnt);
                        juce::FloatVectorOperations::copy (R, pb->getReadPointer (pb->getNumChannels() > 1 ? 1 : 0, (int) pos), cnt);
                    }
                }
                if (st->buf.getMagnitude (0, m) > 1.0f) st->clipped = true;
                if (dither) applyDither (st->buf, st->channels, 0, m, rng);
                if (! st->writer->writeFromAudioSampleBuffer (st->buf, 0, m))
                    return fail ("Falha ao gravar " + st->name + " (disco cheio?).");
            }
            progress = (float) ((double) (pos + m - start) / (double) length);
        }
        rendering = false;
    }

    finishWriters();
    for (auto& st : stems)
    {
        if (! st->temp->overwriteTargetFileWithTemporary())
        {
            error = "Nao foi possivel gravar " + st->temp->getTargetFile().getFullPathName();
            return false;
        }
        written.add (st->temp->getTargetFile().getFullPathName());
        if (st->clipped && req.bitDepth != 32) clippedNames.add (st->name);
    }
    progress = 1.0f;
    return true;
}

//==============================================================================
// Congelar faixa: so esta faixa e processada (os outros plugins ficam parados).
bool Engine::renderFreeze (Track& t, const juce::File& file, double tailSeconds, std::atomic<float>& progress,
                           std::atomic<bool>& cancel, juce::String& error)
{
    const juce::ScopedLock sl (lock);
    const double sr = sampleRate.load();
    const int bs = blockSize;
    if (t.isFrozen()) { error = "A faixa ja esta congelada."; return false; }

    juce::int64 sourceLength = 0;
    if (t.diskStream != nullptr && t.playbackRate == sr) sourceLength = t.diskStream->getLength();
    else if (t.playback != nullptr && t.playbackRate == sr) sourceLength = t.playback->getNumSamples();
    if (sourceLength <= 0) { error = "A faixa nao tem audio carregado na taxa atual do motor."; return false; }

    juce::TemporaryFile temp (file);
    auto writer = createWavWriter (temp, sr, 2, 32, error); // WAV float: sem perda nem clip
    if (writer == nullptr) return false;

    error = beginOfflineRender();
    if (error.isNotEmpty()) return false;
    offlineTrackPool.reset(); // uma faixa so: nada para dividir entre nucleos
    auto fail = [&] (const juce::String& message) { endOfflineRender(); writer.reset(); error = message; return false; };

    const int latency = t.chainLatency; // a saida dos plugins vem atrasada: descarta o inicio
    const juce::int64 toProduce = sourceLength + (juce::int64) (juce::jmax (0.0, tailSeconds) * sr) + latency;
    auto& work = t.processingBuffer;
    juce::AudioBuffer<float> out (2, bs);
    juce::int64 pos = 0, produced = 0;
    while (produced < toProduce)
    {
        if (cancel.load()) return fail ("Congelamento cancelado.");
        // Bloco inteiro sempre (inserts isolados so aceitam o tamanho configurado).
        const int m = (int) juce::jmin ((juce::int64) bs, toProduce - produced);
        if (t.diskStream != nullptr && t.playbackRate == sr) t.diskStream->prefetchBlocking (pos, bs);
        auto* TL = work.getWritePointer (0);
        auto* TR = work.getWritePointer (1);
        juce::FloatVectorOperations::clear (TL, bs);
        juce::FloatVectorOperations::clear (TR, bs);
        const ReadSeg seg { pos, 0, bs };
        readTrackSource (t, TL, TR, &seg, 1, sr);
        if (t.phase.load (std::memory_order_relaxed))
        {
            juce::FloatVectorOperations::negate (TL, TL, bs);
            juce::FloatVectorOperations::negate (TR, TR, bs);
        }
        applyAutomation (t, (double) pos / sr); // automacao dos plugins fica gravada no render
        runInserts (t, bs, work, t.processingMidi);
        if (remoteFailedDuringRender())
            return fail ("Um insert isolado (processo DSP) falhou durante o congelamento.");

        const int skip = produced < latency ? (int) juce::jmin ((juce::int64) m, (juce::int64) latency - produced) : 0;
        const int w = m - skip;
        if (w > 0)
        {
            out.copyFrom (0, 0, work, 0, skip, w);
            out.copyFrom (1, 0, work, 1, skip, w);
            if (! writer->writeFromAudioSampleBuffer (out, 0, w))
                return fail ("Falha ao gravar o arquivo congelado (disco cheio?).");
        }
        pos += bs;
        produced += m;
        progress = (float) ((double) produced / (double) juce::jmax ((juce::int64) 1, toProduce));
    }
    writer.reset();
    endOfflineRender();
    if (! temp.overwriteTargetFileWithTemporary()) { error = "Nao foi possivel gravar " + file.getFullPathName(); return false; }
    progress = 1.0f;
    return true;
}

void Engine::setFrozen (Track& t, const juce::File& file, std::shared_ptr<StreamingAudio> stream,
                        std::vector<std::unique_ptr<juce::AudioProcessor>>& unloaded)
{
    if (stream == nullptr) return;
    // Estados lidos antes (thread de mensagens), fora do lock do audio.
    std::vector<juce::MemoryBlock> states ((size_t) t.inserts.size());
    for (int i = 0; i < t.inserts.size(); ++i)
        if (auto* p = t.inserts[(size_t) i].processor.get()) p->getStateInformation (states[(size_t) i]);

    std::vector<std::shared_ptr<RemoteDspInsert>> retiredRemote;
    {
        const juce::ScopedLock sl (lock);
        for (int i = 0; i < t.inserts.size(); ++i)
        {
            auto& s = t.inserts[(size_t) i];
            if (s.processor == nullptr && s.remote == nullptr) continue;
            if (s.processor != nullptr)
            {
                s.missingState = std::move (states[(size_t) i]);
                unloaded.push_back (std::move (s.processor));
            }
            if (s.remote != nullptr) retiredRemote.push_back (std::move (s.remote)); // estado inicial ja em missingState
            s.offline = true;
            s.faulted.store (false, std::memory_order_relaxed);
        }
        t.frozenFile = file;
        t.frozenStream = stream;
        t.pdc.clear();
        t.wake();
        updateLatency();
    }
    for (auto& p : unloaded) if (p != nullptr) p->removeListener (this);
    retiredRemote.clear(); // processos DSP encerrados fora do lock
    streamingWorker->add (stream);
}

void Engine::restoreInserts (Channel& c, std::vector<RestoredInsert>& restored, bool unfreeze)
{
    std::shared_ptr<StreamingAudio> oldFrozen;
    {
        const juce::ScopedLock sl (lock);
        for (auto& r : restored)
        {
            if (! c.hasInsert (r.slot)) continue;
            auto& s = c.inserts[(size_t) r.slot];
            if (! s.isOffline() && ! s.isMissing()) continue;
            if (r.local != nullptr)
            {
                s.processor = std::move (r.local);
                s.bufferChannels = juce::jlimit (2, kMaxPluginChannels,
                                                 juce::jmax (s.processor->getTotalNumInputChannels(), s.processor->getTotalNumOutputChannels()));
                s.mainOutChannels = s.processor->getMainBusNumOutputChannels();
                s.missingState.reset();
                s.preferRemote = false;
                s.processor->addListener (this);
            }
            else if (r.remote != nullptr)
            {
                s.remote = std::move (r.remote);
                s.missingState = s.remote->getInitialState();
                s.preferRemote = true;
                s.bufferChannels = 2;
                s.mainOutChannels = 2;
            }
            else continue;
            s.offline = false;
            s.faulted.store (false, std::memory_order_relaxed);
        }
        if (unfreeze && c.kind == ChannelKind::track)
        {
            auto& t = static_cast<Track&> (c);
            oldFrozen = std::move (t.frozenStream);
            t.frozenFile = juce::File();
            t.pdc.clear();
        }
        c.wake();
        updateLatency();
    }
    oldFrozen.reset(); // fecha o arquivo fora do lock
}

} // namespace sc
