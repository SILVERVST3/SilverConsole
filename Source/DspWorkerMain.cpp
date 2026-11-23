// SilverConsole — worker DSP VST3, processo SEPARADO do mixer.
// Protocolo ScDspIpcProtocol v1; sem UI, arquivos de projeto ou audio device.
// Permanece em processo filho mesmo quando um plugin causa access violation.
#include <JuceHeader.h>
#include "DspIpcProtocol.h"
#include "RealtimeThread.h"
#include <cmath>
#include <cstdint>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
#if JUCE_WINDOWS
static_assert (sizeof (LONG) == sizeof (std::int32_t), "Expected Win32 LONG");
LONG load (const std::int32_t& v) noexcept
{
    return ::InterlockedCompareExchange (
        reinterpret_cast<volatile LONG*> (const_cast<std::int32_t*> (&v)), 0, 0);
}
void store (std::int32_t& v, LONG value) noexcept
{
    ::InterlockedExchange (reinterpret_cast<volatile LONG*> (&v), value);
}
bool take (std::int32_t& v, LONG from, LONG to) noexcept
{
    return ::InterlockedCompareExchange (
        reinterpret_cast<volatile LONG*> (&v), to, from) == from;
}
#endif
}

int main (int argc, char** argv)
{
#if ! JUCE_WINDOWS
    juce::ignoreUnused (argc, argv);
    return 2;
#else
    // argv: mapeamento, config XML e (opcional) PID do host
    if (argc != 3 && argc != 4) return 2;
    juce::ScopedJuceInitialiser_GUI init;
    const auto name = juce::String::fromUTF8 (argv[1]);
    const juce::File config (juce::String::fromUTF8 (argv[2]));

    // Se o host morrer sem avisar (crash/kill), este processo encerra sozinho.
    HANDLE parent = nullptr;
    if (argc == 4)
    {
        const auto pid = (DWORD) juce::String::fromUTF8 (argv[3]).getLargeIntValue();
        if (pid != 0) parent = ::OpenProcess (SYNCHRONIZE, FALSE, pid);
    }
    // Evento "bloco pronto" criado pelo host; sem ele, volta ao polling.
    HANDLE wake = ::OpenEventW (SYNCHRONIZE, FALSE, (name + "_wake").toWideCharPointer());
    const auto closeHandles = [&]
    {
        if (wake != nullptr) { ::CloseHandle (wake); wake = nullptr; }
        if (parent != nullptr) { ::CloseHandle (parent); parent = nullptr; }
    };

    HANDLE mapping = ::OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE,
                                         name.toWideCharPointer());
    if (mapping == nullptr) { closeHandles(); return 3; }
    auto* shared = static_cast<sc::ipc::Shared*> (
        ::MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (sc::ipc::Shared)));
    if (shared == nullptr)
    {
        ::CloseHandle (mapping);
        closeHandles();
        return 4;
    }
    const auto release = [&] (int status)
    {
        ::UnmapViewOfFile (shared);
        ::CloseHandle (mapping);
        closeHandles();
        return status;
    };

    if (shared->headerMagic != sc::ipc::magic ||
        shared->protocolVersion != sc::ipc::version ||
        shared->blockFrames < 32 || shared->blockFrames > sc::ipc::maxFrames ||
        shared->sampleRate < 8000 || shared->sampleRate > 384000)
    {
        store (shared->ready, -1);
        return release (5);
    }

    auto configXml = juce::XmlDocument::parse (config);
    auto* pluginXml = configXml != nullptr ? configXml->getChildByName ("PLUGIN") : nullptr;
    juce::PluginDescription description;
    if (configXml == nullptr || ! configXml->hasTagName ("SILVERCONSOLE_DSP_CONFIG") ||
        configXml->getIntAttribute ("version") != (int) sc::ipc::version ||
        pluginXml == nullptr || ! description.loadFromXml (*pluginXml) ||
        description.pluginFormatName != "VST3")
    {
        store (shared->ready, -1);
        return release (6);
    }

    juce::AudioPluginFormatManager formats;
    formats.addDefaultFormats();
    juce::String error;
    auto plugin = formats.createPluginInstance (description,
                   (double) shared->sampleRate, shared->blockFrames, error);
    if (plugin == nullptr)
    {
        store (shared->ready, -1);
        return release (7);
    }

    try
    {
        const auto stereo = juce::AudioChannelSet::stereo();
        auto layout = plugin->getBusesLayout();
        if (layout.inputBuses.size() > 0)
            layout.inputBuses.getReference (0) = stereo;
        if (layout.outputBuses.size() > 0)
            layout.outputBuses.getReference (0) = stereo;
        if (plugin->checkBusesLayoutSupported (layout))
            plugin->setBusesLayout (layout);

        if (auto* state = configXml->getChildByName ("STATE"))
        {
            juce::MemoryBlock blob;
            if (blob.fromBase64Encoding (state->getAllSubText().trim()) && blob.getSize() > 0)
                plugin->setStateInformation (blob.getData(), (int) blob.getSize());
        }
        plugin->setRateAndBufferSizeDetails (shared->sampleRate, shared->blockFrames);
        plugin->prepareToPlay (shared->sampleRate, shared->blockFrames);
    }
    catch (...)
    {
        store (shared->ready, -1);
        return release (8);
    }

    if (plugin->getTotalNumOutputChannels() < 1 ||
        plugin->getTotalNumOutputChannels() > sc::ipc::channels ||
        plugin->getTotalNumInputChannels() > sc::ipc::channels ||
        plugin->getLatencySamples() > 16384)
    {
        store (shared->ready, -1);
        return release (9);
    }

    store (shared->pluginLatency, juce::jmax (0, plugin->getLatencySamples()));
    const bool monoOut = plugin->getTotalNumOutputChannels() == 1;
    // O DSP precisa responder dentro de um buffer: prioridade de tempo real
    // para esta thread (unica thread de processamento do worker).
    sc::rt::configureProcessForAudio();
    sc::rt::promoteAudioThread(); // MMCSS "Pro Audio" + sem economia de energia
    store (shared->ready, 1);

    juce::AudioBuffer<float> audio (sc::ipc::channels, sc::ipc::maxFrames);
    juce::MidiBuffer midi;
    int idleRounds = 0;
    while (load (shared->stop) == 0)
    {
        if (parent != nullptr && (idleRounds & 63) == 0
            && ::WaitForSingleObject (parent, 0) == WAIT_OBJECT_0)
            break; // host encerrou: nao deixar processo orfao

        bool processed = false;
        for (auto& slot : shared->frames)
        {
            if (! take (slot.state, sc::ipc::readyForWorker, sc::ipc::processing))
                continue;
            processed = true;
            const auto count = slot.frames;
            if (count <= 0 || count > sc::ipc::maxFrames ||
                count != shared->blockFrames || slot.sequence <= 0)
            {
                store (shared->ready, -1);
                store (slot.state, sc::ipc::free);
                plugin->releaseResources();
                return release (10);
            }
            for (int channel = 0; channel < 2; ++channel)
                juce::FloatVectorOperations::copy (audio.getWritePointer (channel),
                                                    slot.input[channel], count);
            midi.clear();
            try
            {
                juce::AudioBuffer<float> view (audio.getArrayOfWritePointers(), 2, count);
                plugin->processBlock (view, midi);
            }
            catch (...)
            {
                store (shared->ready, -1);
                store (slot.state, sc::ipc::free);
                return release (11);
            }
            if (monoOut) // plugin de saida mono: o canal direito recebe o mesmo sinal
                audio.copyFrom (1, 0, audio, 0, 0, count);
            bool finite = true;
            for (int channel = 0; channel < 2; ++channel)
            {
                const float* source = audio.getReadPointer (channel);
                for (int i = 0; i < count; ++i)
                {
                    if (! std::isfinite (source[i])) { finite = false; break; }
                    slot.output[channel][i] = source[i];
                }
            }
            if (! finite)
            {
                store (shared->ready, -1);
                store (slot.state, sc::ipc::free);
                return release (12);
            }
            store (slot.state, sc::ipc::readyForHost);
        }

        if (processed) idleRounds = 0;
        else
        {
            ++idleRounds;
            // Acorda assim que o host sinaliza um bloco; o prazo curto mantem
            // a checagem de stop/host vivo mesmo sem audio.
            if (wake != nullptr) ::WaitForSingleObject (wake, 2);
            else juce::Thread::sleep (1); // fallback: polling
        }
        ::InterlockedIncrement (reinterpret_cast<volatile LONG*> (&shared->heartbeat));
    }
    plugin->releaseResources();
    return release (0);
#endif
}
