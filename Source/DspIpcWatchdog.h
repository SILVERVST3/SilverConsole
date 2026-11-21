// SilverConsole - politica de supervisao de DSP remoto.
// Sem alocacoes, mutexes, sleeps ou syscalls no callback de audio.
#pragma once
#include <atomic>
#include <cstdint>

namespace sc
{
class DspIpcWatchdog final
{
public:
    enum class State : uint8_t { starting, running, bypassed, stopped };
    struct Snapshot
    {
        State state;
        uint64_t submitted;
        uint64_t completed;
        uint64_t overruns;
        uint32_t latencySamples;
    };

    explicit DspIpcWatchdog (uint32_t maxMissedBuffers = 2) noexcept
        : missLimit (maxMissedBuffers == 0 ? 1 : maxMissedBuffers) {}

    // Control thread only. Must be called after worker and shared memory are ready.
    void start (uint32_t transportLatencySamples, uint32_t pluginLatencySamples) noexcept
    {
        submitted.store (0, std::memory_order_relaxed);
        completed.store (0, std::memory_order_relaxed);
        overruns.store (0, std::memory_order_relaxed);
        latency.store (transportLatencySamples + pluginLatencySamples, std::memory_order_relaxed);
        state.store (State::running, std::memory_order_release);
    }

    // Audio thread: publish a new sequence to the IPC ring separately.
    uint64_t onSubmit() noexcept
    {
        if (state.load (std::memory_order_acquire) != State::running) return 0;
        return submitted.fetch_add (1, std::memory_order_acq_rel) + 1;
    }

    // Called after validating a complete worker response (sequence + channel/frame bounds).
    void onCompletion (uint64_t sequence) noexcept
    {
        const auto sent = submitted.load (std::memory_order_acquire);
        if (sequence == 0 || sequence > sent) return;
        auto previous = completed.load (std::memory_order_relaxed);
        while (sequence > previous &&
               ! completed.compare_exchange_weak (previous, sequence,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)) {}
    }

    // Audio thread: evaluate without waiting. The caller must route aligned dry audio
    // whenever this returns false; never block waiting for the worker.
    bool isHealthy() noexcept
    {
        if (state.load (std::memory_order_acquire) != State::running) return false;
        const auto sent = submitted.load (std::memory_order_acquire);
        const auto done = completed.load (std::memory_order_acquire);
        if (sent > done && sent - done > missLimit)
        {
            overruns.fetch_add (1, std::memory_order_relaxed);
            state.store (State::bypassed, std::memory_order_release);
            return false;
        }
        return true;
    }

    // Worker supervisor thread: process crash, heartbeat timeout or IPC corruption.
    void fail() noexcept
    {
        state.store (State::bypassed, std::memory_order_release);
    }

    // Recovery is deliberately explicit; do not re-enable a broken plugin in audio callback.
    void stop() noexcept { state.store (State::stopped, std::memory_order_release); }

    Snapshot snapshot() const noexcept
    {
        return { state.load (std::memory_order_acquire),
                 submitted.load (std::memory_order_acquire),
                 completed.load (std::memory_order_acquire),
                 overruns.load (std::memory_order_relaxed),
                 latency.load (std::memory_order_relaxed) };
    }

private:
    const uint32_t missLimit;
    std::atomic<State> state { State::starting };
    std::atomic<uint64_t> submitted { 0 }, completed { 0 }, overruns { 0 };
    std::atomic<uint32_t> latency { 0 };
};
} // namespace sc
