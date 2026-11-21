# DSP IPC: integration contract (Phase 4, work in progress)

This branch introduces a lock-free watchdog policy in `Source/DspIpcWatchdog.h`. **It does not yet move live plugin processing out of process.** The existing in-process Engine path remains unchanged.

## Required architecture

* Dedicated DSP worker executable, separate from the scan helper. The main process must never instantiate untrusted plugin code when isolation is enabled.
* Versioned IPC control channel for plugin description, serialized state, parameter changes, configuration, errors and heartbeat.
* Preallocated, bounded, double/triple-buffered shared-memory audio slots, each with a monotonically increasing sequence number, frame count, channel count and completion flag. Validate every response before consumption.
* The audio callback must never wait for IPC, allocate, lock, or launch processes. Publish input for a future block and consume only completed output. Worker supervision, startup and restarts belong to a control thread.
* Fixed transport latency of at least one block, plus reported plugin latency. Update `Engine::chainLatencyOf` and PDC only on the control thread; align the dry bypass signal with the same latency, including when the worker fails.
* A deadline measured against the audio block period, with a configurable safety margin. If output is late, use the latency-aligned dry signal and record an xrun. On repeated misses, latch bypass and terminate/restart the worker outside the callback.
* Recover only after a fresh handshake, state restore and primed pipeline. Crossfade between delayed dry and wet to prevent clicks.
* Explicit lifecycle: start -> handshake -> prime -> running -> fault/bypass -> stop/restart. Guard stale completions with a generation token.

## Required verification before declaring complete

1. Unit tests for sequence handling, missed blocks, stale responses and explicit restart.
2. Worker integration test with gain/latency VST3 and output comparison.
3. Fault injection: worker crash, infinite DSP stall, IPC corruption, channel mismatch and sample-rate changes.
4. Run 44.1/48/96 kHz, 64/128/256/1024-frame buffers and multi-track loads; confirm no audio-thread blocking.
5. Validate saved sessions, automation, PDC and offline bounce.
6. Windows CI build plus actual realtime loopback test.

The watchdog tracks *buffer deadlines*, not wall-clock time. The IPC transport must enforce per-buffer wall-clock deadlines in its supervisor and call `fail()` on heartbeat timeout. A watchdog alone is not a complete timeout implementation.
