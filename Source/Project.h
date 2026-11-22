// SilverConsole - projeto (.sconsole, XML). Codigo original (MIT).
#pragma once
#include "Engine.h"
#include "AudioLoader.h"

namespace sc
{
struct InsertData
{
    int slot = 0;
    bool bypass = false;
    bool hasDesc = false;
    bool isolated = false; // opcional, compativel com projetos anteriores
    juce::PluginDescription desc;
    juce::MemoryBlock state;
};

struct LaneData
{
    AutoTarget target = AutoTarget::volume;
    int slot = -1, param = -1;
    bool step = false;
    std::vector<AutoPoint> points;
};

struct ChannelData
{
    juce::String name;
    juce::Colour colour { 0xff7fc8ff };
    float gainDb = 0.0f, pan = 0.0f;
    bool mute = false, solo = false, phase = false;
    std::vector<InsertData> inserts;
    AutoMode autoMode = AutoMode::read;
    std::vector<LaneData> lanes;
};

struct SendData { int bus = -1; float levelDb = 0.0f; bool pre = false; };

struct TrackData
{
    ChannelData ch;
    juce::File file;
    int output = -1;
    std::array<SendData, kNumSends> sends;
    LoadedAudio audio; // preenchido ao carregar
};

struct ProjectData
{
    std::vector<TrackData> tracks;
    std::vector<ChannelData> buses;
    ChannelData master;
    bool masterMono = false;
    bool loopOn = false;
    double loopStartSec = 0.0, loopEndSec = 0.0;
};

constexpr const char* kProjectExtension = ".sconsole";

/** Copia o estado atual do motor (inclui o estado dos plugins). Thread de mensagens. */
ProjectData captureProject (Engine&);
/** Copia nome, cor, parametros, modo e linhas de automacao (com o lock do motor). */
void applyChannelBasics (Engine&, Channel&, const ChannelData&);

bool writeProject (const ProjectData&, const juce::File&, juce::String& error);
bool readProject (const juce::File&, ProjectData&, juce::String& error);
} // namespace sc
