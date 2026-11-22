// SilverConsole — host de IPC para um insert VST3 remoto.
// Windows shared pagefile memory; audio callback apenas troca atomics e copia.
#include "RemoteDspInsert.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sc
{
#if JUCE_WINDOWS
namespace
{
static_assert (sizeof (LONG) == sizeof (std::int32_t), "IPC requires Win32 LONG");
LONG atomicRead (const std::int32_t& v) noexcept
{
    return ::InterlockedCompareExchange (
        reinterpret_cast<volatile LONG*> (const_cast<std::int32_t*> (&v)), 0, 0);
}
void atomicWrite (std::int32_t& v, LONG value) noexcept
{
    ::InterlockedExchange (reinterpret_cast<volatile LONG*> (&v), value);
}
}
#endif

struct RemoteDspInsert::HostResources
{
#if JUCE_WINDOWS
    HANDLE mapping = nullptr;
    HANDLE wake = nullptr; // evento auto-reset: host sinaliza bloco novo
#endif
    juce::ChildProcess process;
    juce::File config;
    juce::String mappingName;

    ~HostResources()
    {
        if (process.isRunning())
        {
            if (! process.waitForProcessToFinish (200))
                process.kill();
        }
        if (config.existsAsFile()) config.deleteFile();
#if JUCE_WINDOWS
        if (wake != nullptr) ::CloseHandle (wake);
        if (mapping != nullptr) ::CloseHandle (mapping);
#endif
    }
};

std::shared_ptr<RemoteDspInsert> RemoteDspInsert::start (
    const juce::PluginDescription& d, const juce::MemoryBlock& state,
    const juce::File& exe, double rate, int frames, juce::String& error)
{
    auto insert = std::shared_ptr<RemoteDspInsert> (new RemoteDspInsert());
    if (! insert->initialise (d, state, exe, rate, frames, error)) return {};
    return insert;
}

bool RemoteDspInsert::initialise (
    const juce::PluginDescription& d, const juce::MemoryBlock& state,
    const juce::File& exe, double rate, int frames, juce::String& error)
{
    error.clear();
#if ! JUCE_WINDOWS
    juce::ignoreUnused (d, state, exe, rate, frames);
    error = "O DSP IPC ainda so esta disponivel no Windows.";
    return false;
#else
    if (! exe.existsAsFile() || frames < 32 || frames > ipc::maxFrames ||
        rate < 8000.0 || rate > 384000.0 || d.pluginFormatName != "VST3")
    {
        error = "Parametros invalidos ou ScDspWorker.exe ausente.";
        return false;
    }

    resources = std::make_unique<HostResources>();
    const auto guid = juce::Uuid().toString().removeCharacters ("-");
    resources->mappingName = "Local\\SilverConsoleDSP_" + guid;
    const auto wide = resources->mappingName.toWideCharPointer();
    resources->mapping = ::CreateFileMappingW (
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD> (sizeof (ipc::Shared)), wide);
    if (resources->mapping == nullptr || ::GetLastError() == ERROR_ALREADY_EXISTS)
    {
        error = "Falha ao criar memoria compartilhada DSP.";
        return false;
    }
    shared = static_cast<ipc::Shared*> (
        ::MapViewOfFile (resources->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (ipc::Shared)));
    if (shared == nullptr)
    {
        error = "Falha ao mapear memoria compartilhada DSP.";
        return false;
    }
    new (shared) ipc::Shared();
    shared->blockFrames = frames;
    shared->sampleRate = juce::roundToInt (rate);

    // Evento de "bloco pronto". Sem ele o worker dependeria de Sleep(1), cuja
    // granularidade no Windows pode chegar a ~15,6 ms (maior que um buffer de
    // 256 amostras a 48 kHz), e o watchdog travaria o bypass. SetEvent nao
    // bloqueia e e o mecanismo usual de pontes de plugin em tempo real.
    resources->wake = ::CreateEventW (nullptr, FALSE, FALSE,
                                      (resources->mappingName + "_wake").toWideCharPointer());
    wakeEvent = resources->wake; // se falhar, o worker cai no modo polling

    resources->config = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("silverconsole-dsp-" + guid + ".xml");
    juce::XmlElement root ("SILVERCONSOLE_DSP_CONFIG");
    root.setAttribute ("version", (int) ipc::version);
    if (auto plugin = d.createXml()) root.addChildElement (plugin.release());
    root.createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
    if (! root.writeTo (resources->config))
    {
        error = "Nao foi possivel gravar a configuracao do processo DSP.";
        return false;
    }
    juce::StringArray args;
    args.add (exe.getFullPathName());
    args.add (resources->mappingName);
    args.add (resources->config.getFullPathName());
    // PID do host: o worker encerra sozinho se o SilverConsole fechar de forma
    // anormal (sem isso ficaria um ScDspWorker.exe orfao consumindo CPU).
    args.add (juce::String ((juce::int64) ::GetCurrentProcessId()));
    if (! resources->process.start (args, 0))
    {
        error = "Falha ao iniciar ScDspWorker.exe.";
        return false;
    }

    // Handshake exclusivamente na thread de controle, nunca no callback.
    const auto began = juce::Time::getMillisecondCounter();
    while (juce::Time::getMillisecondCounter() - began < 6000)
    {
        const auto status = atomicRead (shared->ready);
        if (status == 1) break;
        if (status == -1 || ! resources->process.isRunning())
        {
            error = "O processo auxiliar rejeitou o VST3 ou encerrou.";
            return false;
        }
        juce::Thread::sleep (5);
    }
    if (atomicRead (shared->ready) != 1)
    {
        error = "Tempo limite aguardando handshake do DSP remoto.";
        return false;
    }
    pluginLatency = juce::jlimit (0, 16384, (int) atomicRead (shared->pluginLatency));
    transportFrames = frames;
    configuredRate = juce::roundToInt (rate);
    dryLength = transportFrames + pluginLatency;
    dryCursor = 0;
    savedState = state;
    watchdog.start ((uint32_t) transportFrames, (uint32_t) pluginLatency);
    return true;
#endif
}

RemoteDspInsert::~RemoteDspInsert()
{
#if JUCE_WINDOWS
    if (shared != nullptr)
    {
        atomicWrite (shared->stop, 1);
        // O worker pode estar travado dentro do DSP; destruir/kill e exclusao
        // de handle acontecem apenas fora do callback.
        if (resources != nullptr && resources->process.isRunning())
        {
            if (! resources->process.waitForProcessToFinish (250))
                resources->process.kill();
        }
        ::UnmapViewOfFile (shared);
        shared = nullptr;
    }
#endif
}

void RemoteDspInsert::delayedDry (float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float srcL = l[i], srcR = r[i];
        l[i] = dryL[(size_t) dryCursor];
        r[i] = dryR[(size_t) dryCursor];
        dryL[(size_t) dryCursor] = srcL;
        dryR[(size_t) dryCursor] = srcR;
        if (++dryCursor >= dryLength) dryCursor = 0;
    }
}

void RemoteDspInsert::switchFade (float* l, float* r, int n, bool wet) noexcept
{
    // Pequena rampa no inicio da transicao seco/umido para evitar degrau.
    if (hasOutput && wet != lastWet)
    {
        const int fade = juce::jmin (48, n);
        for (int i = 0; i < fade; ++i)
        {
            const float f = (float) (i + 1) / (float) fade;
            l[i] = lastOutL * (1.0f - f) + l[i] * f;
            r[i] = lastOutR * (1.0f - f) + r[i] * f;
        }
    }
    if (n > 0) { lastOutL = l[n - 1]; lastOutR = r[n - 1]; }
    hasOutput = true;
    lastWet = wet;
}

void RemoteDspInsert::process (float* l, float* r, int n, bool userBypass) noexcept
{
    if (l == nullptr || r == nullptr || n <= 0) return;
    bool wet = false;
#if JUCE_WINDOWS
    const bool canSubmit = shared != nullptr && ! failed.load (std::memory_order_relaxed)
                        && ! userBypass && n == transportFrames
                        && atomicRead (shared->ready) == 1;
    uint64_t sequence = 0;
    if (canSubmit)
    {
        sequence = watchdog.onSubmit();
        if (sequence != 0)
        {
            auto& slot = shared->frames[(sequence - 1) % ipc::slots];
            if (offline) waitOffline (slot, ipc::free, (std::int32_t) sequence);
            if (atomicRead (slot.state) == ipc::free)
            {
                slot.sequence = (std::int32_t) sequence;
                slot.frames = n;
                for (int i = 0; i < n; ++i)
                {
                    slot.input[0][i] = l[i];
                    slot.input[1][i] = r[i];
                }
                atomicWrite (slot.state, ipc::readyForWorker);
                if (wakeEvent != nullptr) ::SetEvent (static_cast<HANDLE> (wakeEvent));
            }
            else missed.fetch_add (1, std::memory_order_relaxed);
        }
    }

    // Sempre avanca a linha de atraso do sinal seco mesmo com bypass/timeout.
    delayedDry (l, r, n);

    if (sequence > 1 && sequence - 1 > discardThrough)
    {
        const auto target = sequence - 1;
        auto& previous = shared->frames[(target - 1) % ipc::slots];
        // Offline: o bounce nao tem relogio de tempo real; espera a resposta
        // (com prazo) em vez de gravar trechos secos na mix exportada.
        if (offline && ! waitOffline (previous, ipc::readyForHost, (std::int32_t) target))
        {
            failed.store (true, std::memory_order_relaxed);
            watchdog.fail();
        }
        if (atomicRead (previous.state) == ipc::readyForHost &&
            previous.sequence == (std::int32_t) target &&
            previous.frames == n)
        {
            bool finite = true;
            for (int i = 0; i < n; ++i)
                if (! std::isfinite (previous.output[0][i]) ||
                    ! std::isfinite (previous.output[1][i])) { finite = false; break; }
            if (finite)
            {
                for (int i = 0; i < n; ++i)
                {
                    l[i] = previous.output[0][i];
                    r[i] = previous.output[1][i];
                }
                wet = true;
                watchdog.onCompletion (target);
            }
            else { failed.store (true, std::memory_order_relaxed); watchdog.fail(); }
            atomicWrite (previous.state, ipc::free);
        }
        else
            missed.fetch_add (1, std::memory_order_relaxed);

        // Nunca consumir resposta obsoleta de outro bloco nem liberar slot
        // enquanto o processo auxiliar ainda esta gravando (state=processing).
        for (auto& slot : shared->frames)
            if (atomicRead (slot.state) == ipc::readyForHost &&
                slot.sequence < (std::int32_t) target)
                atomicWrite (slot.state, ipc::free);

        if (! watchdog.isHealthy())
            failed.store (true, std::memory_order_relaxed);
    }
    if (shared != nullptr && atomicRead (shared->ready) == -1)
    {
        failed.store (true, std::memory_order_relaxed);
        watchdog.fail();
    }
#else
    juce::ignoreUnused (userBypass);
    delayedDry (l, r, n);
#endif
    switchFade (l, r, n, wet);
}

bool RemoteDspInsert::workerAlive() const noexcept
{
    return resources != nullptr && resources->process.isRunning();
}

bool RemoteDspInsert::waitOffline (ipc::Frame& frame, std::int32_t wanted, std::int32_t forSequence) noexcept
{
#if JUCE_WINDOWS
    // Somente na thread de exportacao (nunca no callback de audio).
    const auto began = juce::Time::getMillisecondCounter();
    for (int spins = 0;; ++spins)
    {
        const auto s = atomicRead (frame.state);
        if (s == wanted) return true;
        // libera respostas antigas que ocupam o slot (ex.: blocos do playback)
        if (wanted == ipc::free && s == ipc::readyForHost && frame.sequence < forSequence)
        {
            atomicWrite (frame.state, ipc::free);
            continue;
        }
        if (failed.load (std::memory_order_relaxed) || atomicRead (shared->ready) == -1) return false;
        if ((spins & 255) == 255)
        {
            if (! workerAlive()) return false;
            if (juce::Time::getMillisecondCounter() - began > 5000) return false; // worker travado
        }
        if (spins < 2000) ::SwitchToThread(); else ::Sleep (1);
    }
#else
    juce::ignoreUnused (frame, wanted, forSequence);
    return false;
#endif
}

void RemoteDspInsert::resetStream() noexcept
{
#if JUCE_WINDOWS
    if (shared != nullptr)
        for (auto& slot : shared->frames)
            if (atomicRead (slot.state) == ipc::readyForHost)
                atomicWrite (slot.state, ipc::free);
#endif
    discardThrough = watchdog.snapshot().submitted; // ainda pode chegar resposta antiga em processamento
    std::fill (dryL.begin(), dryL.end(), 0.0f);
    std::fill (dryR.begin(), dryR.end(), 0.0f);
    dryCursor = 0;
    hasOutput = false;
    lastWet = false;
}

void RemoteDspInsert::setOffline (bool shouldBeOffline) noexcept
{
    if (offline == shouldBeOffline) return;
    offline = shouldBeOffline;
    resetStream();
}

void RemoteDspInsert::simulateWorkerCrashForTests() noexcept
{
    if (resources != nullptr && resources->process.isRunning())
        resources->process.kill(); // explicit test/control thread only
    failed.store (true, std::memory_order_relaxed);
    watchdog.fail();
}

void RemoteDspInsert::pollWorker() noexcept
{
    if (resources != nullptr && ! resources->process.isRunning())
    {
        workerFinished.store (true, std::memory_order_relaxed);
        failed.store (true, std::memory_order_relaxed);
        watchdog.fail();
    }
}
} // namespace sc
