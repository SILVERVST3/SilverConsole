// SilverConsole - motor de mixagem. Codigo original (MIT).
#include "Engine.h"
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
}

Engine::Engine()
{
    master.colour = juce::Colour (0xffe8edf2);
    streamingWorker = std::make_unique<StreamWorker>();
    prepare (48000.0, 512);
}

Engine::~Engine()
{
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
    if (c.kind == ChannelKind::track)
    {
        auto& t = static_cast<Track&> (c);
        t.processingBuffer.setSize (kMaxPluginChannels + 1, blockSize);
        t.processingMidi.ensureSize (4096);
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
    jassert (juce::isPositiveAndBelow (slot, kNumInserts));
    auto& s = c.inserts[(size_t) slot];
    std::unique_ptr<juce::AudioProcessor> old;
    {
        const juce::ScopedLock sl (lock);
        old = std::move (s.processor);
        s.processor = std::move (p);
        s.hasDescription = desc != nullptr;
        if (desc != nullptr) s.description = *desc;
        s.missingState.reset();
        s.bypassed = false;
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

std::unique_ptr<juce::AudioProcessor> Engine::clearInsert (Channel& c, int slot)
{
    auto old = setInsert (c, slot, nullptr, nullptr);
    c.inserts[(size_t) slot].hasDescription = false;
    return old;
}

void Engine::setMissingInsert (Channel& c, int slot, const juce::PluginDescription& d, const juce::MemoryBlock& state)
{
    auto old = setInsert (c, slot, nullptr, &d);
    c.inserts[(size_t) slot].missingState = state;
    jassert (old == nullptr);
}

void Engine::swapInserts (Channel& c, int a, int b)
{
    if (! juce::isPositiveAndBelow (a, kNumInserts) || ! juce::isPositiveAndBelow (b, kNumInserts)) return;
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
    std::swap (x.description, y.description);
    std::swap (x.hasDescription, y.hasDescription);
    std::swap (x.missingState, y.missingState);
    std::swap (x.bufferChannels, y.bufferChannels);
    std::swap (x.mainOutChannels, y.mainOutChannels);
    const bool bx = x.bypassed.load();
    x.bypassed = y.bypassed.load();
    y.bypassed = bx;
}

void Engine::setBypass (Channel& c, int slot, bool b)
{
    const juce::ScopedLock sl (lock);
    c.inserts[(size_t) slot].bypassed = b;
    updateLatency();
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
        t.originalRate = stream->getRate();
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
            s.hasDescription = false;
            s.missingState.reset();
            s.bypassed = false;
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
            if (juce::isPositiveAndBelow (slot, kNumInserts))
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
    if (tg != AutoTarget::plugin || ! juce::isPositiveAndBelow (slot, kNumInserts)) return false;
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
                if (juce::isPositiveAndBelow (l.slot, kNumInserts))
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
        if (s.processor != nullptr && ! s.bypassed.load())
            sum += juce::jmax (0, s.processor->getLatencySamples());
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
        auto* p = s.processor.get();
        if (p == nullptr || s.bypassed.load(std::memory_order_relaxed)) continue;

        const int ch = s.bufferChannels;
        for (int i = 2; i < ch; ++i) work.clear (i, 0, n);
        juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), ch, n);
        eventMidi.clear();
        {
            const juce::ScopedLock pl (p->getCallbackLock());
            if (p->isSuspended()) continue;
            p->processBlock (view, eventMidi);
        }
        if (s.mainOutChannels == 1) work.copyFrom (1, 0, work, 0, 0, n);
    }
}

juce::int64 Engine::render (float* outL, float* outR, int n, juce::int64 pos, bool play, bool allowLoop)
{
    jassert (n <= blockSize);
    const double sr = sampleRate.load();

    // Trechos do arquivo lidos neste bloco (o loop pode dividir o bloco).
    struct Seg { juce::int64 src; int dst; int len; };
    Seg segs[8];
    int numSegs = 0;
    juce::int64 p = pos;
    if (play)
    {
        const juce::int64 ls = loopStart.load(), le = loopEnd.load();
        const bool loop = allowLoop && loopOn.load() && le > ls;
        int done = 0;
        while (done < n && numSegs < 8)
        {
            int len = n - done;
            if (loop && p < le && p + len > le) len = (int) (le - p);
            segs[numSegs++] = { p, done, len };
            done += len;
            p += len;
            if (loop && p == le) p = ls;
        }
    }

    // Automacao: aplicada tocando ou quando o cursor foi movido.
    const bool applyAuto = play || pos != lastRenderEnd;
    const double tSec = (double) pos / sr;
    if (applyAuto) forEachChannelNoAlloc ([this, tSec] (Channel& c) { applyAutomation (c, tSec); });

    const int numBuses = (int) buses.size();
    for (int b = 0; b < numBuses; ++b) busIn[(size_t) b].clear (0, n);
    direct.clear (0, n);

    bool anySolo = false;
    for (auto& t : tracks) anySolo = anySolo || t->solo.load (std::memory_order_relaxed);

    auto* L = scratch.getWritePointer (0);
    auto* R = scratch.getWritePointer (1);

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

        const auto* buf = t.playback.get();
        if (play && t.diskStream != nullptr && t.playbackRate == sr)
        {
            for (int i = 0; i < numSegs; ++i)
            {
                const auto& sg = segs[i];
                if (sg.src >= 0)
                    t.diskStream->copy (TL, TR, sg.dst, sg.src, sg.len);
            }
        }
        else if (play && buf != nullptr && t.playbackRate == sr)
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
        if (t.phase.load (std::memory_order_relaxed))
        {
            juce::FloatVectorOperations::negate (TL, TL, n);
            juce::FloatVectorOperations::negate (TR, TR, n);
        }

        runInserts (t, n, t.processingBuffer, t.processingMidi);
        t.pdc.process (TL, TR, n);

    };
    if (offlineTrackPool != nullptr)
    {
        const std::function<void(int)> job = processTrack;
        offlineTrackPool->run ((int) tracks.size(), job);
    }
    else
        for (int index = 0; index < (int) tracks.size(); ++index)
            processTrack (index);

    // Somatoria deterministica: envios, faders e buses na ordem original.
    for (auto& tp : tracks)
    {
        auto& t = *tp;
        auto* TL = t.processingBuffer.getWritePointer (0);
        auto* TR = t.processingBuffer.getWritePointer (1);
        const bool audible = ! t.mute.load (std::memory_order_relaxed) && (! anySolo || t.solo.load (std::memory_order_relaxed));
        const float g = audible ? dbToGain (t.gainDb.load (std::memory_order_relaxed)) : 0.0f;
        float pl, pr;
        panGains (t.pan.load (std::memory_order_relaxed), t.sourceChannels == 1, pl, pr);
        t.smoothL.setTargetValue (g * pl);
        t.smoothR.setTargetValue (g * pr);

        // envios pre-fader
        for (auto& s : t.sends)
        {
            const int b = s.bus.load (std::memory_order_relaxed);
            const bool valid = b >= 0 && b < numBuses;
            s.smooth.setTargetValue (valid && audible ? dbToGain (s.levelDb.load (std::memory_order_relaxed)) : 0.0f);
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

        const int ob = t.outputBus.load (std::memory_order_relaxed);
        auto& out = (ob >= 0 && ob < numBuses) ? busIn[(size_t) ob] : direct;
        out.addFrom (0, 0, TL, n);
        out.addFrom (1, 0, TR, n);

        storePeak (t.peakL, t.processingBuffer.getMagnitude (0, 0, n));
        storePeak (t.peakR, t.processingBuffer.getMagnitude (1, 0, n));
    }

    // ---------------- Buses ----------------
    masterBuf.clear (0, n);
    for (int bi = 0; bi < numBuses; ++bi)
    {
        auto& b = *buses[(size_t) bi];
        juce::FloatVectorOperations::copy (L, busIn[(size_t) bi].getReadPointer (0), n);
        juce::FloatVectorOperations::copy (R, busIn[(size_t) bi].getReadPointer (1), n);
        if (b.phase.load (std::memory_order_relaxed))
        {
            juce::FloatVectorOperations::negate (L, L, n);
            juce::FloatVectorOperations::negate (R, R, n);
        }
        runInserts (b, n, scratch, midi);
        b.pdc.process (L, R, n);

        const float g = b.mute.load (std::memory_order_relaxed) ? 0.0f : dbToGain (b.gainDb.load (std::memory_order_relaxed));
        float pl, pr;
        panGains (b.pan.load (std::memory_order_relaxed), false, pl, pr);
        b.smoothL.setTargetValue (g * pl);
        b.smoothR.setTargetValue (g * pr);
        for (int i = 0; i < n; ++i) { L[i] *= b.smoothL.getNextValue(); R[i] *= b.smoothR.getNextValue(); }

        masterBuf.addFrom (0, 0, L, n);
        masterBuf.addFrom (1, 0, R, n);
        storePeak (b.peakL, scratch.getMagnitude (0, 0, n));
        storePeak (b.peakR, scratch.getMagnitude (1, 0, n));
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
        master.smoothL.setTargetValue (g * pl);
        master.smoothR.setTargetValue (g * pr);
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
        position.compare_exchange_strong (expected, pos); // se a interface moveu o cursor, vale o da interface
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
bool Engine::exportMix (const juce::File& file, int bitDepth, double tailSeconds,
                        std::atomic<float>& progress, std::atomic<bool>& cancel,
                        juce::String& error, bool& clipped)
{
    const juce::ScopedLock sl (lock);
    rendering = true;
    clipped = false;
    const double sr = sampleRate.load();
    const int bs = blockSize;

    auto restore = [this]
    {
        offlineTrackPool.reset(); // workers encerrados antes de resetar os plugins
        forEachChannel ([] (Channel& c)
        {
            for (auto& s : c.inserts)
                if (auto* p = s.processor.get()) { p->setNonRealtime (false); p->reset(); }
            c.pdc.clear();
        });
        directDelay.clear();
        lastRenderEnd = -1;
        rendering = false;
    };

    juce::TemporaryFile temp (file);
    std::unique_ptr<juce::AudioFormatWriter> writer;
    {
        auto stream = temp.getFile().createOutputStream();
        if (stream == nullptr) { error = "Nao foi possivel criar o arquivo."; rendering = false; return false; }
        juce::WavAudioFormat wav;
        writer.reset (wav.createWriterFor (stream.get(), sr, 2, bitDepth, {}, 0));
        if (writer == nullptr) { error = "Formato WAV nao suportado nesta taxa/resolucao."; rendering = false; return false; }
        stream.release(); // o writer passou a ser dono do stream
    }

    forEachChannel ([] (Channel& c)
    {
        for (auto& s : c.inserts)
            if (auto* p = s.processor.get()) { p->setNonRealtime (true); p->reset(); }
        c.pdc.clear();
        c.smoothL.setCurrentAndTargetValue (c.smoothL.getTargetValue());
        c.smoothR.setCurrentAndTargetValue (c.smoothR.getTargetValue());
    });
    directDelay.clear();

    // Um pequeno pool persistente apenas durante o bounce; nunca criado
    // em audioDeviceIOCallback. A etapa de buses/master continua serial.
    const auto cores = std::thread::hardware_concurrency();
    const int workers = cores > 1 ? juce::jmin (3, (int) cores - 1) : 0;
    if (workers > 0 && tracks.size() >= 4)
        offlineTrackPool = std::make_unique<OfflineTrackPool> (workers);

    const juce::int64 length = lengthSamples.load();
    const juce::int64 total = length + (juce::int64) (tailSeconds * sr);
    const int latency = totalLatency.load();
    const juce::int64 toProduce = total + latency;

    juce::AudioBuffer<float> buf (2, bs);
    juce::int64 pos = 0, produced = 0;
    while (produced < toProduce)
    {
        if (cancel.load()) { restore(); writer.reset(); error = "Exportacao cancelada."; return false; }
        const int m = (int) juce::jmin ((juce::int64) bs, toProduce - produced);
        // O bounce corre mais rapido que o playback. Prefetch sincrono fora da
        // thread realtime impede silencio por cache vazio em exports longos.
        for (auto& t : tracks)
            if (t->diskStream != nullptr && t->playbackRate == sr)
                t->diskStream->prefetchBlocking (pos, m);
        pos = render (buf.getWritePointer (0), buf.getWritePointer (1), m, pos, true, false);
        const int skip = produced < latency ? (int) juce::jmin ((juce::int64) m, (juce::int64) latency - produced) : 0;
        const int w = m - skip;
        if (w > 0)
        {
            if (buf.getMagnitude (skip, w) > 1.0f) clipped = true;
            if (! writer->writeFromAudioSampleBuffer (buf, skip, w))
            {
                restore(); writer.reset(); error = "Falha ao gravar o arquivo (disco cheio?)."; return false;
            }
        }
        produced += m;
        progress = (float) ((double) produced / (double) juce::jmax ((juce::int64) 1, toProduce));
    }
    writer.reset();
    restore();
    if (! temp.overwriteTargetFileWithTemporary()) { error = "Nao foi possivel substituir o arquivo de destino."; return false; }
    progress = 1.0f;
    return true;
}

} // namespace sc
