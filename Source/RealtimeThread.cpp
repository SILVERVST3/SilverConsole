// SilverConsole - prioridade de audio no Windows. Codigo original (MIT).
#include "RealtimeThread.h"

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>

namespace sc::rt
{
namespace
{
    // Estruturas do SDK do Windows 10/11, declaradas aqui para compilar com SDKs antigos.
    struct PowerThrottlingState { ULONG version, controlMask, stateMask; };
    constexpr ULONG kThrottlingVersion = 1;
    constexpr ULONG kProcessExecutionSpeed = 0x1;     // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
    constexpr ULONG kProcessIgnoreTimerRes = 0x4;     // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    constexpr ULONG kThreadExecutionSpeed = 0x1;      // THREAD_POWER_THROTTLING_EXECUTION_SPEED
    constexpr int kProcessPowerThrottling = 4;        // ProcessPowerThrottling
    constexpr int kThreadPowerThrottling = 3;         // ThreadPowerThrottling

    using SetProcessInfoFn = BOOL (WINAPI*) (HANDLE, int, LPVOID, DWORD);
    using SetThreadInfoFn = BOOL (WINAPI*) (HANDLE, int, LPVOID, DWORD);
    using AvSetFn = HANDLE (WINAPI*) (LPCWSTR, LPDWORD);
    using TimeBeginFn = UINT (WINAPI*) (UINT);

    FARPROC kernelProc (const char* name)
    {
        if (auto* k = ::GetModuleHandleW (L"kernel32.dll")) return ::GetProcAddress (k, name);
        return nullptr;
    }
}

void configureProcessForAudio() noexcept
{
    if (auto fn = reinterpret_cast<SetProcessInfoFn> (kernelProc ("SetProcessInformation")))
    {
        // controlMask = o que controlamos; stateMask = 0 -> economia DESLIGADA
        PowerThrottlingState s { kThrottlingVersion, kProcessExecutionSpeed | kProcessIgnoreTimerRes, 0 };
        fn (::GetCurrentProcess(), kProcessPowerThrottling, &s, sizeof (s));
    }
    if (auto* winmm = ::LoadLibraryW (L"winmm.dll"))
        if (auto fn = reinterpret_cast<TimeBeginFn> (::GetProcAddress (winmm, "timeBeginPeriod")))
            fn (1);
    // "Acima do normal": a interface e os plugins nao disputam a CPU com apps
    // em segundo plano; as threads de audio ja usam MMCSS (prioridade propria).
    if (::GetPriorityClass (::GetCurrentProcess()) == NORMAL_PRIORITY_CLASS)
        ::SetPriorityClass (::GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
}

void disableThreadThrottling() noexcept
{
    if (auto fn = reinterpret_cast<SetThreadInfoFn> (kernelProc ("SetThreadInformation")))
    {
        PowerThrottlingState s { kThrottlingVersion, kThreadExecutionSpeed, 0 };
        fn (::GetCurrentThread(), kThreadPowerThrottling, &s, sizeof (s));
    }
}

void promoteAudioThread() noexcept
{
    static AvSetFn avSet = []
    {
        auto* avrt = ::LoadLibraryW (L"avrt.dll");
        return avrt != nullptr ? reinterpret_cast<AvSetFn> (::GetProcAddress (avrt, "AvSetMmThreadCharacteristicsW")) : nullptr;
    }();
    if (avSet != nullptr)
    {
        DWORD taskIndex = 0;
        avSet (L"Pro Audio", &taskIndex);
    }
    ::SetThreadPriority (::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    disableThreadThrottling();
}
} // namespace sc::rt

#else
namespace sc::rt
{
void configureProcessForAudio() noexcept {}
void disableThreadThrottling() noexcept {}
void promoteAudioThread() noexcept {}
}
#endif
