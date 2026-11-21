// SilverConsole - dados de automacao. Codigo original (MIT).
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <vector>

namespace sc
{
enum class AutoMode : int { off = 0, read, touch, latch, write };
enum class AutoTarget : int { volume = 0, pan, mute, plugin };

inline juce::String autoModeName (AutoMode m)
{
    switch (m)
    {
        case AutoMode::off:   return "OFF";
        case AutoMode::read:  return "READ";
        case AutoMode::touch: return "TOUCH";
        case AutoMode::latch: return "LATCH";
        case AutoMode::write: return "WRITE";
    }
    return {};
}

struct AutoPoint
{
    double t = 0.0; // segundos (independe da taxa de amostragem)
    float v = 0.0f; // volume: dB | pan: -1..1 | mute: 0/1 | plugin: 0..1
};

/** Valor de uma lista de pontos ordenada no tempo t (vazia: 0). */
inline float evalPoints (const std::vector<AutoPoint>& points, double t, bool step) noexcept
{
    if (points.empty()) return 0.0f;
    if (t <= points.front().t) return points.front().v;
    if (t >= points.back().t) return points.back().v;
    auto it = std::upper_bound (points.begin(), points.end(), t, [] (double x, const AutoPoint& p) { return x < p.t; });
    const auto& b = *it;
    const auto& a = *(it - 1);
    if (step || b.t - a.t <= 1.0e-9) return a.v;
    const double f = (t - a.t) / (b.t - a.t);
    return (float) (a.v + (b.v - a.v) * f);
}

/** Uma linha de automacao. Os pontos so mudam na thread de mensagens com o lock do motor;
    o audio so le (com o lock). */
struct AutoLane
{
    AutoTarget target = AutoTarget::volume;
    int slot = -1, param = -1; // so para plugin
    bool step = false;         // mute e parametros discretos: degrau, sem rampa
    std::vector<AutoPoint> points;
    std::atomic<bool> writing { false }; // gravando: o audio nao aplica

    bool matches (AutoTarget tg, int s, int p) const noexcept
    {
        return target == tg && (tg != AutoTarget::plugin || (slot == s && param == p));
    }

    /** Valor no tempo t. Requer pelo menos um ponto. */
    float valueAt (double t) const noexcept { return evalPoints (points, t, step); }

    /** Insere mantendo a ordem; substitui um ponto no mesmo instante. */
    void insert (double t, float v)
    {
        t = juce::jmax (0.0, t);
        auto it = std::lower_bound (points.begin(), points.end(), t, [] (const AutoPoint& p, double x) { return p.t < x; });
        if (it != points.end() && std::abs (it->t - t) < 1.0e-6) it->v = v;
        else points.insert (it, { t, v });
    }

    /** Remove pontos com t0 < t <= t1. */
    void eraseRange (double t0, double t1)
    {
        points.erase (std::remove_if (points.begin(), points.end(),
                                      [t0, t1] (const AutoPoint& p) { return p.t > t0 && p.t <= t1; }),
                      points.end());
    }

    bool hasPointsAfter (double t) const noexcept { return ! points.empty() && points.back().t > t; }
};
} // namespace sc
