// SilverConsole — supervisor de processo para escaneamento VST3.
// Nunca chamar na thread de audio. Todos os limites expressos em ms.
#pragma once
#include <JuceHeader.h>
#include <atomic>

namespace sc
{
struct PluginScanSupervisor
{
    enum class Status { finished, couldNotStart, timedOut, cancelled };

    struct Result
    {
        Status status = Status::couldNotStart;
        int exitCode = -1;
        bool ok() const noexcept { return status == Status::finished && exitCode == 0; }
    };

    static Result run (const juce::StringArray& argv, std::atomic<bool>& cancel,
                       int timeoutMs = 20000) noexcept
    {
        if (cancel.load (std::memory_order_relaxed))
            return { Status::cancelled, -1 };
        if (argv.isEmpty() || timeoutMs <= 0)
            return { Status::couldNotStart, -1 };

        juce::ChildProcess child;
        if (! child.start (argv, 0))
            return { Status::couldNotStart, -1 };

        // Use tempo decorrido REAL, nao apenas a soma dos periodos solicitados
        // no wait; desse modo nenhum plugin recebe prazo infinito em CPU lenta.
        const auto start = juce::Time::getMillisecondCounter();
        for (;;)
        {
            if (cancel.load (std::memory_order_relaxed))
            {
                child.kill();
                return { Status::cancelled, -1 };
            }
            const auto elapsed = juce::Time::getMillisecondCounter() - start;
            if (elapsed >= (juce::uint32) timeoutMs)
            {
                child.kill();
                return { Status::timedOut, -1 };
            }
            const int waitMs = juce::jmin (100, timeoutMs - (int) elapsed);
            if (child.waitForProcessToFinish (waitMs))
                return { Status::finished, child.getExitCode() };
        }
    }
};
} // namespace sc
