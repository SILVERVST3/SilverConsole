// SilverConsole - cliente DSP IPC (Windows). Um processo por insert isolado.
// Nao usar start/stop/destrutor na thread de audio.
#pragma once
#include <JuceHeader.h>
#include "DspIpcWatchdog.h"
#include "DspIpcProtocol.h"
#include <array>
#include <atomic>
#include <memory>

namespace sc
{
class RemoteDspInsert final
{
public:
    // Chamado na thread de controle. Instancia o plugin somente no worker.
    static std::shared_ptr<RemoteDspInsert> start (
        const juce::PluginDescription&, const juce::MemoryBlock& initialState,
        const juce::File& workerExe, double sampleRate, int blockFrames,
        juce::String& error);

    ~RemoteDspInsert();

    // Apenas audio thread sob Engine::lock, sem alocacao, espera nem syscalls.
    void process (float* left, float* right, int count, bool userBypass) noexcept;

    int getLatencySamples() const noexcept { return transportFrames + pluginLatency; }
    int getPluginLatencySamples() const noexcept { return pluginLatency; }
    bool isFaulted() const noexcept { return failed.load (std::memory_order_relaxed); }
    uint64_t getMisses() const noexcept { return missed.load (std::memory_order_relaxed); }
    DspIpcWatchdog::Snapshot snapshot() const noexcept { return watchdog.snapshot(); }
    const juce::MemoryBlock& getInitialState() const noexcept { return savedState; }

    // Chamadas somente no controle: verifica o fim do child, e pode forcar
    // bypass em caso de falha; nunca mata um processo no audio callback.
    void pollWorker() noexcept;
    bool matchesConfiguration (double sampleRate, int frames) const noexcept
    {
        return juce::roundToInt (sampleRate) == configuredRate && frames == transportFrames;
    }
    void invalidateForReconfigure() noexcept
    {
        failed.store (true, std::memory_order_relaxed);
        watchdog.fail(); // dry permanece alinhado; reiniciar na thread de controle
    }
    // Usado somente em testes de regressao fora do audio callback.
    void simulateWorkerCrashForTests() noexcept;

    /** Exportacao offline (thread de trabalho, com Engine::lock e sem callback de audio):
        process() passa a ESPERAR o worker (com prazo) em vez de cair no sinal seco,
        porque o bounce roda mais rapido que o tempo real. Ao entrar e ao sair,
        descarta respostas e audio seco antigos para nao misturar playback e bounce. */
    void setOffline (bool shouldBeOffline) noexcept;
    bool isOffline() const noexcept { return offline; }

private:
    RemoteDspInsert() = default;
    bool initialise (const juce::PluginDescription&, const juce::MemoryBlock&,
                     const juce::File&, double, int, juce::String&);
    void delayedDry (float*, float*, int) noexcept;
    void switchFade (float*, float*, int, bool wet) noexcept;
    void resetStream() noexcept;
    bool waitOffline (ipc::Frame&, std::int32_t wanted, std::int32_t forSequence) noexcept;
    bool workerAlive() const noexcept;

    struct HostResources;
    std::unique_ptr<HostResources> resources;
    ipc::Shared* shared = nullptr; // memoria compartilhada; somente audio faz R/W frames
    juce::MemoryBlock savedState;
    DspIpcWatchdog watchdog { 3 };
    std::array<float, ipc::maxFrames + 16384> dryL {}, dryR {};
    int transportFrames = 0, pluginLatency = 0, configuredRate = 0;
    int dryLength = 1, dryCursor = 0;
    int nextSequence = 0, lastFrameCount = 0;
    int previousL = 0; // reservado para diagnostico
    float lastOutL = 0.0f, lastOutR = 0.0f;
    bool lastWet = false, hasOutput = false;
    void* wakeEvent = nullptr;     // HANDLE de evento Win32: acorda o worker sem polling
    bool offline = false;          // exportacao: espera o worker em vez de usar seco
    uint64_t discardThrough = 0;   // respostas com sequencia <= isto sao de antes do reset
    std::atomic<bool> failed { false };
    std::atomic<uint64_t> missed { 0 };
    std::atomic<bool> workerFinished { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemoteDspInsert)
};
} // namespace sc
