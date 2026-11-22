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
constexpr int kNumInserts = 8;
constexpr int kNumSends = 4;
constexpr int kMaxBuses = 8;
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
    int bufferChannels = 2;
    int mainOutChannels = 2;

    bool isEmpty() const noexcept     { return processor == nullptr && remote == nullptr && ! hasDescription; }
    bool isMissing() const noexcept   { return processor == nullptr && remote == nullptr && hasDescription; }
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

    std::array<InsertSlot, kNumInserts> inserts;

    // medidores (audio escreve, interface le e zera)
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };

    // automacao
    std::atomic<int> autoMode { (int) AutoMode::read };
    std::vector<std::unique_ptr<AutoLane>> lanes;
    AutoMode getAutoMode() const noexcept { return (AutoMode) autoMode.load(); }

    // estado de render
    juce::LinearSmoothedValue<float> smoothL, smoothR;
    DelayLine pdc;
    int chainLatency = 0;
};

struct Send
{
    std::atomic<int> bus { -1 };
    std::atomic<float> levelDb { 0.0f };
    std::atomic<bool> preFader { false };
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
    // Buffers exclusivos da faixa: inserts/PDC independentes por canal.
    // Prealocados fora do callback para viabilizar processamento paralelo.
    juce::AudioBuffer<float> processingBuffer;
    juce::MidiBuffer processingMidi;
    double playbackRate = 0.0;

    std::atomic<int> outputBus { -1 }; // -1 = Master
    std::array<Send, kNumSends> sends;
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

    Track* addTrack (std::unique_ptr<Track>);
    std::unique_ptr<Track> removeTrack (Track*);
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
    void setMissingInsert (Channel&, int slot, const juce::PluginDescription&,
                           const juce::MemoryBlock& state, bool preferRemote = false);
    void swapInserts (Channel&, int a, int b);
    void setBypass (Channel&, int slot, bool);
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
    void resetRealtimeStats() noexcept
    {
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
    bool exportMix (const juce::File&, int bitDepth, double tailSeconds,
                    std::atomic<float>& progress, std::atomic<bool>& cancel,
                    juce::String& error, bool& clipped,
                    juce::int64 rangeStart = 0, juce::int64 rangeEnd = -1);
    std::atomic<bool> rendering { false };

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
    void prepareSlot (InsertSlot&);
    int chainLatencyOf (const Channel&) const;
    void runInserts (Channel&, int n, juce::AudioBuffer<float>&, juce::MidiBuffer&);
    void forEachChannel (const std::function<void (Channel&)>&);
    template <typename Fn> void forEachChannelNoAlloc (Fn&& fn)
    {
        for (auto& t : tracks) fn (*t);
        for (auto& b : buses) fn (*b);
        fn (master);
    }

    std::unique_ptr<StreamWorker> streamingWorker;
    std::unique_ptr<OfflineTrackPool> offlineTrackPool;
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
