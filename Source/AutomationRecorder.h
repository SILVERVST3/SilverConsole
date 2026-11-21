// SilverConsole - gravacao de automacao (Touch / Latch / Write). Codigo original (MIT).
#pragma once
#include "Engine.h"
#include <map>
#include <tuple>

namespace sc
{
/** Roda na thread de mensagens (~50 Hz). Le os controles que o usuario esta mexendo
    e escreve pontos nas linhas de automacao enquanto o transporte toca. */
class AutomationRecorder
{
public:
    explicit AutomationRecorder (Engine& e) : engine (e) {}

    /** Fader/pan da interface: inicio e fim do toque. */
    void touch (Channel&, AutoTarget, int slot, int param, bool down, double nowMs);
    /** Mute clicado tocando em Touch/Latch/Write: grava o degrau e mantem. */
    void recordMuteStep (Channel&, double timeSec);

    /** Chamar periodicamente. timeSec = posicao do transporte. */
    void process (double timeSec, bool playing, double nowMs);

    /** Esquece toques e passes (estrutura mudou ou sessao nova). */
    void reset();

    bool isRecordingAnything() const noexcept { return ! passes.empty(); }

    double glideSeconds = 0.10;      // retorno suave ao soltar em Touch
    double touchTimeoutMs = 600.0;   // plugins que nao avisam fim do gesto

private:
    using Key = std::tuple<Channel*, int, int, int>; // canal, alvo, slot, param
    struct TouchState { bool down = false; bool latched = false; double lastChangeMs = -1.0e9; };
    struct Pass { Channel* channel; double lastT; float fallback; std::vector<AutoPoint> original; };

    void handleParamEvents (double nowMs);
    bool isTouched (const Key&, AutoMode, double nowMs) const;
    void writeValue (Channel&, AutoLane&, double t, float v);
    void endPass (AutoLane&, double t, bool returnToFallback);
    void endAllPasses();
    bool laneExists (const AutoLane*) const;

    Engine& engine;
    std::map<Key, TouchState> touches;
    std::map<AutoLane*, Pass> passes;
    double lastTime = 0.0;
};
} // namespace sc
