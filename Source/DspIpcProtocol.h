// SilverConsole — protocolo ABI fixo entre host e ScDspWorker (Windows x64).
// Memoria compartilhada exclusiva de uma instancia; nunca colocar ponteiros
// C++ ou objetos JUCE nesta estrutura interprocesso.
#pragma once
#include <cstdint>

namespace sc
{
namespace ipc
{
constexpr std::uint32_t magic = 0x53434450u; // 'SCDP'
constexpr std::uint32_t version = 1;
constexpr int channels = 2;
constexpr int slots = 4;
constexpr int maxFrames = 4096;

enum FrameState : std::int32_t
{
    free = 0,
    readyForWorker = 1,
    processing = 2,
    readyForHost = 3
};

// Estados são escritos/lidos via Win32 Interlocked*, que inclui barreiras de
// memoria. Campos alinhados em 32 bits e compartilhados entre processos x64.
struct alignas(64) Frame
{
    alignas(64) std::int32_t state = free;
    std::int32_t sequence = 0;
    std::int32_t frames = 0;
    std::int32_t reserved = 0;
    float input[channels][maxFrames] {};
    float output[channels][maxFrames] {};
};
struct alignas(64) Shared
{
    std::uint32_t headerMagic = magic;
    std::uint32_t protocolVersion = version;
    std::int32_t blockFrames = 0;
    std::int32_t sampleRate = 0;
    alignas(64) std::int32_t ready = 0; // 0 criando, 1 ligado, -1 falha
    std::int32_t stop = 0;
    std::int32_t pluginLatency = 0;
    std::int32_t heartbeat = 0;
    Frame frames[slots];
};
} // namespace ipc
} // namespace sc
