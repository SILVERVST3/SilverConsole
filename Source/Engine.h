// SilverConsole - motor de mixagem com hospedagem de plugins VST3.
// Codigo original (MIT). Depende do JUCE (AGPLv3 ou licenca comercial).
#pragma once

#include <JuceHeader.h>
#include "Automation.h"
#include "StreamingAudio.h"
#include "Metering.h"
#include "RemoteDspInsert.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace sc
{
// Inserts e envios por canal sao dinamicos (botao "+" na faixa). Os limites
// abaixo so protegem contra arquivos de projeto corrompidos.
constexpr int kDefaultInserts = 4;
constexpr int kDefaultSends = 4;
constexpr int kMaxInsertsPerChannel = 64;
constexpr int kMaxSendsPerTrack = 64;
constexpr int kMaxBuses = 16;
constexpr int kMaxGroups = 8;          // grupos de faders (com VCA opcional)
constexpr int kMaxTracks = 128;
constexpr int kMaxPluginChannels = 31; // AudioBuffer de referencia sem alocacao
constexpr float kMinDb = -60.0f;       // fader no fim = -inf

inline float dbToGain (float db) noexcept { return db <= kMinDb ? 0.0f : juce::Decibels::decibelsToGain (db); }

inline void storePeak (std::atomic<float>& a, float v) noexcept
{
    auto cur = a.load (std::memory_order_relaxed);
    while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
}

//==============================================================================
/** Atraso estereo exato em amostras (compensacao de latencia). */
class DelayLine
{
public:
    void setDelay (int samples)
    {
        delay = juce::jmax (0, samples);
        ring.setSize (2, juce::jmax (1, delay), false, true, false);
        ring.clear();
        index = 0;
    }
    void clear() { ring.clear(); index = 0; }
    int getDelay() const noexcept { return delay; }

    void process (float* l, float* r, int n) noexcept
    {
        if (delay == 0) return;
        auto* bl = ring.getWritePointer (0);
        auto* br = ring.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float ol = bl[index], orr = br[index];
            bl[index] = l[i]; br[index] = r[i];
            l[i] = ol; r[i] = orr;
            if (++index == delay) index = 0;
        }
    }

private:
    juce::AudioBuffer<float> ring;
    int delay = 0, index = 0;
};

//==============================================================================
/** Lista de tamanho variavel com enderecos estaveis (os itens tem atomics e nao
    podem ser movidos). Mudar o tamanho so na thread de mensagens com Engine::lock;
    o audio itera sob o mesmo lock. */
template <typename T>
class SlotList
{
public:
    explicit SlotList (int initial) { resize (initial); }

    T& operator[] (size_t i) noexcept             { return *items[i]; }
    const T& operator[] (size_t i) const noexcept { return *items[i]; }
    int size() const noexcept                     { return (int) items.size(); }
    void resize (int n)
    {
        while ((int) items.size() < n) items.push_back (std::make_unique<T>());
        while ((int) items.size() > n) items.pop_back();
    }

    template <typename It, typename V>
    struct Iter
    {
        It it;
        V& operator*() const noexcept { return **it; }
        Iter& operator++() noexcept   { ++it; return *this; }
        bool operator!= (const Iter& o) const noexcept { return it != o.it; }
    };
    using iterator = Iter<typename std::vector<std::unique_ptr<T>>::iterator, T>;
    using const_iterator = Iter<typename std::vector<std::unique_ptr<T>>::const_iterator, const T>;
    iterator begin() noexcept             { return { items.begin() }; }
    iterator end() noexcept               { return { items.end() }; }
    const_iterator begin() const noexcept { return { items.cbegin() }; }
    const_iterator end() const noexcept   { return { items.cend() }; }

private:
    std::vector<std::unique_ptr<T>> items;
};

//==============================================================================
struct Track;

struct InsertSlot
{
    std::unique_ptr<juce::AudioProcessor> processor;
    std::shared_ptr<RemoteDspInsert> remote; // processo DSP real, sem AudioProcessor no host
    juce::PluginDescription description;
    bool hasDescription = false;      // plugin externo (pode estar ausente)
    bool preferRemote = false;         // guarda modo em projetos e plugins ausentes
    juce::MemoryBlock missingState;   // estado guardado quando o plugin nao abriu
    std::atomic<bool> bypassed { false };
    std::atomic<bool> faulted { false }; // Falha C++ em processBlock; exibida na interface, sem locks
    bool offline = false;              // plugin descarregado de proposito (estado em missingState)
    std::atomic<float> mix { 1.0f };   // dry/wet: 1 = so o plugin, 0 = so o sinal seco
    float lastMix = 1.0f;              // rampa (thread de audio)
    DelayLine dryDelay;                // seco atrasado pela latencia do plugin (alinhado ao wet)
    Track* sidechain = nullptr;        // faixa na entrada lateral do plugin (sinal antes dos plugins); sob o lock
    int bufferChannels = 2;
    int mainOutChannels = 2;

    bool isEmpty() const noexcept     { return processor == nullptr && remote == nullptr && ! hasDescription; }
    bool isMissing() const noexcept   { return processor == nullptr && remote == nullptr && hasDescription && ! offline; }
    bool isOffline() const noexcept   { return processor == nullptr && remote == nullptr && hasDescription && offline; }
    juce::String getName() const
    {
        if (hasDescription) return description.name;
        return processor != nullptr ? processor->getName() : juce::String();
    }
};

enum class ChannelKind { track, bus, master };

struct Channel
{
    explicit Channel (ChannelKind k) : kind (k) {}
    virtual ~Channel() = default;

    const ChannelKind kind;
    juce::String name;
    juce::Colour colour { 0xff7fc8ff };

    std::atomic<float> gainDb { 0.0f }, pan { 0.0f };
    std::atomic<bool> mute { false }, solo { false }, phase { false };
    std::atomic<int> group { -1 }; // grupo de faders (0..kMaxGroups-1) ou -1

    SlotList<InsertSlot> inserts { kDefaultInserts };
    bool hasInsert (int slot) const noexcept { return juce::isPositiveAndBelow (slot, inserts.size()); }

    // medidores (audio escreve, interface le e zera)
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };

    // automacao
    std::atomic<int> autoMode { (int) AutoMode::read };
    std::vector<std::unique_ptr<AutoLane>> lanes;
    AutoMode getAutoMode() const noexcept { return (AutoMode) autoMode.load(); }

    // estado de render
    // Buffers exclusivos do canal (faixas e buses): inserts/PDC independentes,
    // prealocados fora do callback para processamento paralelo.
    juce::AudioBuffer<float> processingBuffer;
    juce::MidiBuffer processingMidi;
    juce::AudioBuffer<float> dryBuffer; // sinal seco para o mix dry/wet dos inserts
    juce::AudioBuffer<float> sidechainBuffer; // entrada lateral lida da faixa fonte
    juce::LinearSmoothedValue<float> smoothL, smoothR;
    DelayLine pdc;
    int chainLatency = 0;

    // Economia de CPU: com a entrada em silencio e a saida dos plugins abaixo de
    // -100 dBFS por sleepHoldSamples (cauda dos plugins + margem), o canal para
    // de chamar os plugins ate o som voltar. So a thread de audio mexe nestes,
    // exceto sleepHoldSamples (calculado em updateLatency, sob o lock).
    int sleepHoldSamples = 0; // 0 = sem plugins ativos; -1 = nunca dorme
    int silentSamples = 0;
    bool sleeping = false;
    std::atomic<bool> asleep { false }; // leitura pela interface
    void wake() noexcept { sleeping = false; silentSamples = 0; asleep.store (false, std::memory_order_relaxed); }
};

struct Send
{
    std::atomic<int> bus { -1 };
    std::atomic<float> levelDb { 0.0f };
    std::atomic<bool> preFader { false };
    std::atomic<bool> preFx { false }; // antes dos plugins (tambem independe do fader)
    juce::LinearSmoothedValue<float> smooth;
};

struct Track : Channel
{
    Track() : Channel (ChannelKind::track) {}

    juce::File sourceFile;
    int sourceChannels = 0;                                  // 0 = sem audio
    std::shared_ptr<const juce::AudioBuffer<float>> original; // na taxa do arquivo
    double originalRate = 0.0;
    std::shared_ptr<const juce::AudioBuffer<float>> playback; // na taxa do motor
    std::shared_ptr<StreamingAudio> diskStream; // prefetch em disco, alternativo ao playback RAM
    double playbackRate = 0.0;

    std::atomic<int> outputBus { -1 }; // -1 = Master
    SlotList<Send> sends { kDefaultSends };

    // Congelada: toca o render da faixa (inserts e inversao de fase ja aplicados,
    // antes do fader) lido do disco; os plugins ficam offline. O audio original
    // continua carregado para descongelar.
    juce::File frozenFile;
    std::shared_ptr<StreamingAudio> frozenStream;

    // Envios pre-FX: sinal antes dos inserts, atrasado pela latencia das faixas (PDC).
    juce::AudioBuffer<float> preFxBuffer;
    DelayLine preFxPdc;
    // solo: cala so a saida da faixa (com rampa); os envios continuam valendo
    juce::LinearSmoothedValue<float> soloGate { 1.0f };
    bool preFxActive = false; // thread de audio
    bool isFrozen() const noexcept { return frozenStream != nullptr; }
};

struct Bus : Channel { Bus() : Channel (ChannelKind::bus) {} };

struct Master : Channel
{
    Master() : Channel (ChannelKind::master) { name = "MASTER"; }
    std::atomic<bool> mono { false };
    std::atomic<bool> clipped { false };
    // Estimativa de true peak 4x, publicada sem locks para interface.
    std::atomic<float> truePeakL { 0.0f }, truePeakR { 0.0f };
    std::atomic<float> momentaryLufs { -120.0f }; // valor LUFS-M, nao integrado
};

//==============================================================================
class OfflineTrackPool; // pool somente para bounce, nunca usado pelo callback realtime
class RealtimeTrackPool; // pool persistente para o callback da placa (sem alocacao nem mutex)

class Engine : public juce::AudioIODeviceCallback,
               private juce::AudioProcessorListener
{
public:
    Engine();
    ~Engine() override;

    // ---- Estrutura (somente thread de mensagens) ------------------------------
    juce::CriticalSection lock;
    std::vector<std::unique_ptr<Track>> tracks;
    std::vector<std::unique_ptr<Bus>> buses;
    Master master;

    // VCA por grupo: soma dB no volume de todos os membros (sem audio proprio)
    struct Vca
    {
        std::atomic<float> gainDb { 0.0f };
        std::atomic<bool> mute { false };
        bool shown = false; // faixa de VCA visivel no mixer (interface)
    };
    std::array<Vca, kMaxGroups> vca;
    /** Ganho do fader com o VCA do grupo aplicado (0 = mudo). */
    float faderGain (const Channel&) const noexcept;

    Track* addTrack (std::unique_ptr<Track>);
    std::unique_ptr<Track> removeTrack (Track*);
    /** Recoloca uma faixa removida (desfazer) na posicao index. */
    Track* insertTrack (int index, std::unique_ptr<Track>);
    Bus* addBus (const juce::String& name = {});
    std::unique_ptr<Bus> removeBus (Bus*);
    int indexOfBus (const Bus*) const;
    void moveTrack (int from, int to);

    /** Prepara o processador para o motor. Retorna erro ou vazio. */
    juce::String prepareProcessor (juce::AudioProcessor&);
    /** Troca o insert; devolve o anterior para ser destruido fora do lock. */
    std::unique_ptr<juce::AudioProcessor> setInsert (Channel&, int slot, std::unique_ptr<juce::AudioProcessor>,
                                                     const juce::PluginDescription* desc);
    // Instala worker inicializado na thread de controle. A thread de audio
    // encontra o ponteiro somente sob lock de Engine, nao cria subprocessos.
    std::unique_ptr<juce::AudioProcessor> setRemoteInsert (
        Channel&, int slot, std::shared_ptr<RemoteDspInsert>, const juce::PluginDescription&);
    std::unique_ptr<juce::AudioProcessor> clearInsert (Channel&, int slot);

    /** Tudo o que um slot de insert guarda (plugin, estado, bypass, mix...). */
    struct SlotContent
    {
        std::unique_ptr<juce::AudioProcessor> processor;
        std::shared_ptr<RemoteDspInsert> remote;
        juce::PluginDescription description;
        bool hasDescription = false, preferRemote = false, offline = false, bypassed = false;
        float mix = 1.0f;
        Track* sidechain = nullptr;
        juce::MemoryBlock missingState;
    };
    /** Troca o conteudo do slot pelo de 'other' (desfazer/refazer de inserts:
        a mesma troca serve para os dois sentidos). O plugin que sai continua
        vivo em 'other', com o ajuste intacto. */
    void swapSlotContent (Channel&, int slot, SlotContent& other);

    /** Sidechain: liga a faixa 'source' (sinal antes dos plugins dela) na entrada
        lateral do plugin do slot; nullptr desliga. false = o plugin nao tem entrada
        lateral (ou e isolado). */
    bool setSidechain (Channel&, int slot, Track* source);
    static bool supportsSidechain (const InsertSlot&);
    void setMissingInsert (Channel&, int slot, const juce::PluginDescription&,
                           const juce::MemoryBlock& state, bool preferRemote = false, bool offline = false);
    /** Plugin offline: guarda o estado e descarrega o plugin (sem CPU nem memoria);
        a latencia sai da PDC. Devolve o processador para destruir fora do lock.
        Para voltar, a interface recria o plugin com o estado guardado. */
    std::unique_ptr<juce::AudioProcessor> takeInsertOffline (Channel&, int slot);
    void swapInserts (Channel&, int a, int b);
    void setBypass (Channel&, int slot, bool);
    /** Numero de slots de insert/envio. Diminuir so remove slots vazios do fim.
        Retorna false se nada mudou. */
    bool setInsertCount (Channel&, int count);
    bool setSendCount (Track&, int count);
    void setTrackAudio (Track&, std::shared_ptr<const juce::AudioBuffer<float>> original, double rate,
                        std::shared_ptr<const juce::AudioBuffer<float>> playback, double playbackRate);
    void setTrackStream (Track&, std::shared_ptr<StreamingAudio>);

    // ---- Automacao (thread de mensagens) --------------------------------------
    AutoLane* findLane (Channel&, AutoTarget, int slot = -1, int param = -1) const;
    AutoLane* getOrCreateLane (Channel&, AutoTarget, int slot = -1, int param = -1);
    void removeLane (Channel&, AutoLane*);
    void removeLanesForSlot (Channel&, int slot);
    /** Executa fn com o lock do motor (para editar pontos). */
    template <typename Fn> void editAutomation (Fn&& fn) { const juce::ScopedLock sl (lock); fn(); }
    /** Valor atual do alvo (volume dB, pan, mute 0/1, plugin 0..1). */
    static bool currentValue (Channel&, AutoTarget, int slot, int param, float& value);
    static bool isStepTarget (Channel&, AutoTarget, int slot, int param);

    /** Eventos de parametros dos plugins (mexidos pelo usuario na janela do plugin). */
    struct ParamEvent { juce::AudioProcessor* proc = nullptr; int index = 0; int type = 0; }; // 0 mudou, 1 inicio, 2 fim
    bool popParamEvent (ParamEvent&);

    /** Remove tudo (devolve processadores para destruir fora do lock). */
    void clearSession (std::vector<std::unique_ptr<juce::AudioProcessor>>& toDelete,
                       std::vector<std::unique_ptr<Track>>& oldTracks,
                       std::vector<std::unique_ptr<Bus>>& oldBuses);

    /** Chamar periodicamente pela interface: aplica mudancas de latencia. */
    void pollLatencyChanges();
    void updateLatency(); // requer lock

    // ---- Transporte -----------------------------------------------------------
    std::atomic<juce::int64> position { 0 };
    std::atomic<bool> playing { false };
    std::atomic<bool> loopOn { false };
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };
    void setLoop (juce::int64 start, juce::int64 end);
    /** "Tocar selecao" sem loop: para sozinho em stopAt e volta para stopReturn.
        -1 = desligado. A interface zera ao parar/mover o cursor. */
    std::atomic<juce::int64> stopAt { -1 }, stopReturn { 0 };
    // A selecao de trecho e a mesma regiao do loop (loopOn liga a repeticao).
    bool hasSelection() const noexcept { return loopEnd.load() > loopStart.load(); }
    void clearSelection() noexcept { loopOn = false; stopAt = -1; loopStart = 0; loopEnd = 0; }

    juce::int64 getLengthSamples() const noexcept { return lengthSamples.load(); }
    void updateLength();
    int getTotalLatency() const noexcept { return totalLatency.load(); }
    double getSampleRate() const noexcept { return sampleRate.load(); }
    int getBlockSize() const noexcept { return blockSize; }
    bool sampleRateChanged() { return srChanged.exchange (false); }

    // ---- Diagnostico realtime (leituras lock-free pela interface) -------------
    struct RealtimeStats
    {
        juce::uint64 lockContentions = 0;
        juce::uint64 renderSuspensions = 0;
        juce::uint64 processedCallbacks = 0;
        juce::uint64 callbackOverruns = 0;
        juce::uint64 pluginExceptions = 0;
    };
    RealtimeStats getRealtimeStats() const noexcept
    {
        return { lockContentions.load (std::memory_order_relaxed),
                 renderSuspensions.load (std::memory_order_relaxed),
                 processedCallbacks.load (std::memory_order_relaxed),
                 callbackOverruns.load (std::memory_order_relaxed),
                 pluginExceptions.load (std::memory_order_relaxed) };
    }
    /** Amostras que o streaming de disco nao entregou a tempo (soma das faixas). */
    juce::uint64 getDiskMissingSamples() const noexcept
    {
        juce::uint64 sum = 0;
        for (auto& t : tracks)
        {
            if (t->diskStream != nullptr) sum += t->diskStream->getMissingSamples();
            if (t->frozenStream != nullptr) sum += t->frozenStream->getMissingSamples();
        }
        return sum;
    }

    /** Economia de CPU: canais em silencio param de chamar os plugins. */
    void setCpuSaver (bool on) noexcept { cpuSaver.store (on, std::memory_order_relaxed); }
    bool isCpuSaver() const noexcept { return cpuSaver.load (std::memory_order_relaxed); }
    /** Canais com plugins que estao dormindo agora / que podem dormir (thread de mensagens). */
    int getSleepingCount() const noexcept
    {
        int n = 0;
        for (auto& t : tracks) n += t->asleep.load (std::memory_order_relaxed) ? 1 : 0;
        for (auto& b : buses) n += b->asleep.load (std::memory_order_relaxed) ? 1 : 0;
        return n;
    }

    // ---- Congelar faixa ---------------------------------------------------------
    /** Renderiza a faixa (leitura, inversao de fase, inserts com automacao,
        latencia removida; antes do fader) num WAV float 32 bits + cauda.
        Roda numa thread de trabalho (o audio fica suspenso como na exportacao). */
    bool renderFreeze (Track&, const juce::File&, double tailSeconds, std::atomic<float>& progress,
                       std::atomic<bool>& cancel, juce::String& error);
    /** Passa a tocar o arquivo congelado e, no MESMO instante, descarrega os
        plugins da faixa (estado guardado, slots offline). Os processadores voltam
        em 'unloaded' para destruir fora do lock. Tambem troca so o arquivo
        (mudanca de taxa) quando os plugins ja estao offline. */
    void setFrozen (Track&, const juce::File&, std::shared_ptr<StreamingAudio>,
                    std::vector<std::unique_ptr<juce::AudioProcessor>>& unloaded);

    /** Plugin recriado (na thread de mensagens) para voltar a um slot offline. */
    struct RestoredInsert
    {
        int slot = -1;
        std::unique_ptr<juce::AudioProcessor> local;
        std::shared_ptr<RemoteDspInsert> remote;
    };
    /** Coloca os plugins de volta online (mantem bypass e automacao). Com
        unfreeze, para de tocar o arquivo congelado no mesmo instante. */
    void restoreInserts (Channel&, std::vector<RestoredInsert>&, bool unfreeze);

    /** Processamento das faixas em varios nucleos no callback da placa. */
    void setMultiCore (bool shouldUse) noexcept { multiCore.store (shouldUse, std::memory_order_relaxed); }
    bool isMultiCore() const noexcept { return multiCore.load (std::memory_order_relaxed); }
    int getRealtimeWorkerCount() const noexcept;

    void resetRealtimeStats() noexcept
    {
        for (auto& t : tracks)
        {
            if (t->diskStream != nullptr) t->diskStream->resetMissingSamples();
            if (t->frozenStream != nullptr) t->frozenStream->resetMissingSamples();
        }
        lockContentions.store (0, std::memory_order_relaxed);
        renderSuspensions.store (0, std::memory_order_relaxed);
        processedCallbacks.store (0, std::memory_order_relaxed);
        callbackOverruns.store (0, std::memory_order_relaxed);
        pluginExceptions.store (0, std::memory_order_relaxed);
    }

    // ---- Render ---------------------------------------------------------------
    void prepare (double sampleRate, int blockSize); // pega o lock
    /** Renderiza n amostras estereo. Requer lock. Retorna a nova posicao. */
    juce::int64 render (float* outL, float* outR, int n, juce::int64 pos, bool play, bool allowLoop);

    /** Bounce offline (+ cauda). Roda numa thread de trabalho.
        rangeStart/rangeEnd em amostras; rangeEnd < 0 = ate o fim da musica.
        Em 16 bits aplica dither TPDF. */
    /** Normalizacao da mix exportada (ganho unico no arquivo inteiro, sem limitador). */
    struct Normalize
    {
        enum class Mode { none, peak, truePeak, lufs };
        Mode mode = Mode::none;
        float target = -1.0f;          // dBFS (pico), dBTP (true peak) ou LUFS integrado
        float truePeakCeiling = -1.0f; // so LUFS: o ganho nunca passa este teto de true peak
    };
    /** Medicoes da mix gravada (ja com o ganho de normalizacao aplicado). */
    struct ExportReport
    {
        bool measured = false;
        float integratedLufs = -100.0f, maxShortTermLufs = -100.0f, truePeakDb = -100.0f, samplePeakDb = -100.0f;
        float gainDb = 0.0f;
        bool limitedByCeiling = false; // LUFS: o teto de true peak segurou o ganho
    };
    /** measureOnly: so renderiza e mede (nao grava nada). */
    bool exportMix (const juce::File&, int bitDepth, double tailSeconds,
                    std::atomic<float>& progress, std::atomic<bool>& cancel,
                    juce::String& error, bool& clipped,
                    juce::int64 rangeStart = 0, juce::int64 rangeEnd = -1,
                    const Normalize& normalize = {}, ExportReport* report = nullptr, bool measureOnly = false);
    std::atomic<bool> rendering { false };

    /** Exportacao de faixas individuais (stems) de um trecho.
        processed = true: cada faixa sai como entra na mix (inserts, volume, pan,
        automacao, PDC), sem buses/master; solo e ignorado, mute e respeitado.
        Buses pedidos saem com tudo o que chega neles. Uma unica passada de render.
        processed = false: audio original da faixa no trecho (mono continua mono). */
    struct StemExport
    {
        juce::File folder;
        juce::String prefix;
        int bitDepth = 24;
        double tailSeconds = 0.0;
        juce::int64 rangeStart = 0, rangeEnd = -1;
        bool processed = true;
        std::vector<int> tracks, buses; // indices
    };
    bool exportStems (const StemExport&, std::atomic<float>& progress, std::atomic<bool>& cancel,
                      juce::String& error, juce::StringArray& written, juce::StringArray& clippedNames);

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    static void configureStereo (juce::AudioProcessor&);

private:
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override;
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override;
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) override;
    void pushParamEvent (juce::AudioProcessor*, int index, int type);
    void applyAutomation (Channel&, double timeSec);

    void prepareChannel (Channel&);
    juce::String beginOfflineRender(); // requer lock; erro ou vazio
    void endOfflineRender();
    bool remoteFailedDuringRender();
    struct StemTaps { std::vector<juce::AudioBuffer<float>*> tracks, buses; };
    StemTaps* stemTaps = nullptr; // so durante exportStems (sob lock)
    bool snapGains = false;       // proximo render comeca sem rampa de ganho (sob lock)
    void prepareSlot (InsertSlot&);
    int chainLatencyOf (const Channel&) const;
    void runInserts (Channel&, int n, juce::AudioBuffer<float>&, juce::MidiBuffer&);
    /** runInserts com a economia de CPU (canal em silencio dorme). */
    void processChain (Channel&, int n, juce::AudioBuffer<float>&, juce::MidiBuffer&, bool allowSleep);
    int sleepHoldFor (const Channel&) const;
    struct ReadSeg { juce::int64 src; int dst; int len; };
    // trechos do bloco atual (render/congelar), para ler a fonte do sidechain
    const ReadSeg* blockSegs = nullptr;
    int blockNumSegs = 0;
    bool applySidechainLayout (juce::AudioProcessor&, bool enable);
    /** Le o audio da faixa (congelado, disco ou RAM) nos trechos pedidos. */
    void readTrackSource (Track&, float* left, float* right, const ReadSeg*, int numSegs, double sr) noexcept;
    void forEachChannel (const std::function<void (Channel&)>&);
    template <typename Fn> void forEachChannelNoAlloc (Fn&& fn)
    {
        for (auto& t : tracks) fn (*t);
        for (auto& b : buses) fn (*b);
        fn (master);
    }

    std::unique_ptr<StreamWorker> streamingWorker;
    std::unique_ptr<OfflineTrackPool> offlineTrackPool;
    std::unique_ptr<RealtimeTrackPool> realtimePool;
    std::atomic<bool> multiCore { true };
    std::atomic<bool> cpuSaver { true };
    TruePeak4x truePeakMeter;
    MomentaryLoudness momentaryLoudness; // estado exclusivo do motor sob engine.lock
    std::atomic<juce::uint64> lockContentions { 0 }, renderSuspensions { 0 }, processedCallbacks { 0 }, callbackOverruns { 0 }, pluginExceptions { 0 };
    std::atomic<double> sampleRate { 48000.0 };
    int blockSize = 512;
    std::atomic<bool> srChanged { false };
    std::atomic<bool> latencyDirty { false };
    std::atomic<juce::int64> lengthSamples { 0 };
    std::atomic<int> totalLatency { 0 };

    juce::AudioBuffer<float> scratch, direct, masterBuf, deviceBuf;
    std::array<juce::AudioBuffer<float>, kMaxBuses> busIn;
    juce::MidiBuffer midi;
    DelayLine directDelay;
    juce::int64 lastRenderEnd = -1;

    std::array<ParamEvent, 1024> paramEvents;
    juce::AbstractFifo paramFifo { 1024 };
    juce::SpinLock paramPushLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

} // namespace sc
