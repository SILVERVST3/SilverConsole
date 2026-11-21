// SilverConsole - gravacao de automacao. Codigo original (MIT).
#include "AutomationRecorder.h"
#include <set>

namespace sc
{
namespace
{
    template <typename Fn> void forEachChannel (Engine& e, Fn&& fn)
    {
        for (auto& t : e.tracks) fn (static_cast<Channel&> (*t));
        for (auto& b : e.buses) fn (static_cast<Channel&> (*b));
        fn (static_cast<Channel&> (e.master));
    }
    constexpr float kEps = 1.0e-5f;
}

bool AutomationRecorder::laneExists (const AutoLane* lane) const
{
    bool found = false;
    forEachChannel (engine, [&] (Channel& c) { for (auto& l : c.lanes) if (l.get() == lane) found = true; });
    return found;
}

void AutomationRecorder::touch (Channel& c, AutoTarget tg, int slot, int param, bool down, double nowMs)
{
    auto& s = touches[{ &c, (int) tg, slot, param }];
    s.down = down;
    if (down)
    {
        s.latched = true;
        s.lastChangeMs = nowMs;
        const auto mode = c.getAutoMode();
        if (mode == AutoMode::touch || mode == AutoMode::latch || mode == AutoMode::write)
            if (auto* lane = engine.findLane (c, tg, slot, param)) lane->writing = true; // o audio para de aplicar ja
    }
    else s.lastChangeMs = -1.0e9;
}

void AutomationRecorder::recordMuteStep (Channel& c, double t)
{
    auto* lane = engine.getOrCreateLane (c, AutoTarget::mute);
    const float v = c.mute.load() ? 1.0f : 0.0f;
    engine.editAutomation ([&]
    {
        if (lane->points.empty()) lane->insert (0.0, 1.0f - v); // estado anterior desde o inicio
        lane->insert (t, v);
    });
}

void AutomationRecorder::handleParamEvents (double nowMs)
{
    Engine::ParamEvent e;
    while (engine.popParamEvent (e))
    {
        Channel* owner = nullptr;
        int slot = -1;
        forEachChannel (engine, [&] (Channel& c)
        {
            for (int i = 0; i < kNumInserts; ++i)
                if (c.inserts[(size_t) i].processor.get() == e.proc) { owner = &c; slot = i; }
        });
        if (owner == nullptr) continue;
        auto& s = touches[{ owner, (int) AutoTarget::plugin, slot, e.index }];
        if (e.type == 1) { s.down = true; s.latched = true; s.lastChangeMs = nowMs; }
        else if (e.type == 2) { s.down = false; s.lastChangeMs = -1.0e9; }
        else if (! s.down) { s.latched = true; s.lastChangeMs = nowMs; } // plugin sem aviso de gesto
    }
}

bool AutomationRecorder::isTouched (const Key& k, AutoMode mode, double nowMs) const
{
    auto it = touches.find (k);
    if (it == touches.end()) return false;
    const auto& s = it->second;
    if (s.down || nowMs - s.lastChangeMs < touchTimeoutMs) return true;
    return mode == AutoMode::latch && s.latched;
}

void AutomationRecorder::writeValue (Channel& c, AutoLane& lane, double t, float v)
{
    auto it = passes.find (&lane);
    if (it != passes.end() && (t < it->second.lastT - 0.05 || t > it->second.lastT + 0.75))
    {
        // o cursor pulou (loop ou reposicionamento): fecha o passe e abre outro
        endPass (lane, it->second.lastT, false);
        it = passes.end();
    }

    engine.editAutomation ([&]
    {
        if (it == passes.end())
        {
            const float before = lane.points.empty() ? v : lane.valueAt (t);
            auto original = lane.points; // envelope antes do passe (para voltar a ele)
            if (! lane.points.empty() && std::abs (before - v) > kEps) lane.insert (juce::jmax (0.0, t - 0.0005), before);
            if (lane.points.empty() && t > 0.001) lane.insert (0.0, before); // valor anterior ate o inicio do passe
            lane.insert (t, v);
            passes[&lane] = { &c, t, before, std::move (original) };
            lane.writing = true;
            return;
        }

        auto& pass = it->second;
        lane.eraseRange (pass.lastT, t);
        // afina: valores iguais seguidos viram um segmento so
        auto& pts = lane.points;
        auto last = std::upper_bound (pts.begin(), pts.end(), pass.lastT + 1.0e-9, [] (double x, const AutoPoint& p) { return x < p.t; });
        const auto k = (int) std::distance (pts.begin(), last) - 1;
        if (k >= 1 && std::abs (pts[(size_t) k].v - v) < kEps && std::abs (pts[(size_t) k - 1].v - v) < kEps)
            pts[(size_t) k].t = t;
        else
            lane.insert (t, v);
        pass.lastT = t;
    });
}

void AutomationRecorder::endPass (AutoLane& lane, double t, bool returnToFallback)
{
    auto it = passes.find (&lane);
    if (it == passes.end()) { lane.writing = false; return; }
    const auto pass = it->second;
    passes.erase (it);

    engine.editAutomation ([&]
    {
        if (lane.hasPointsAfter (t + 1.0e-9))
        {
            if (! lane.step)
            {
                const double tg = t + glideSeconds;
                const float target = evalPoints (pass.original, tg, lane.step);
                lane.eraseRange (t, tg);
                lane.insert (tg, target);
            }
        }
        else if (returnToFallback)
        {
            lane.insert (t + (lane.step ? 0.001 : glideSeconds), pass.fallback);
        }
        lane.writing = false;
    });
}

void AutomationRecorder::endAllPasses()
{
    while (! passes.empty())
    {
        auto it = passes.begin();
        endPass (*it->first, it->second.lastT, false);
    }
}

void AutomationRecorder::reset()
{
    touches.clear();
    passes.clear();
    forEachChannel (engine, [] (Channel& c) { for (auto& l : c.lanes) l->writing = false; });
}

void AutomationRecorder::process (double t, bool playing, double nowMs)
{
    handleParamEvents (nowMs);
    for (auto it = passes.begin(); it != passes.end();)
        it = laneExists (it->first) ? std::next (it) : passes.erase (it);

    if (! playing)
    {
        endAllPasses();
        for (auto& [k, s] : touches) s.latched = s.down;
        forEachChannel (engine, [] (Channel& c) { for (auto& l : c.lanes) l->writing = false; });
        return;
    }
    lastTime = t;

    std::set<AutoLane*> active;
    forEachChannel (engine, [&] (Channel& c)
    {
        const auto mode = c.getAutoMode();
        if (mode == AutoMode::off || mode == AutoMode::read) return;

        std::vector<std::tuple<AutoTarget, int, int>> wanted;
        auto want = [&wanted] (AutoTarget tg, int s, int p)
        {
            for (auto& w : wanted) if (w == std::make_tuple (tg, s, p)) return;
            wanted.emplace_back (tg, s, p);
        };
        if (mode == AutoMode::write)
        {
            want (AutoTarget::volume, -1, -1);
            want (AutoTarget::pan, -1, -1);
            want (AutoTarget::mute, -1, -1);
            for (auto& l : c.lanes)
                if (l->target == AutoTarget::plugin) want (AutoTarget::plugin, l->slot, l->param);
        }
        for (auto& [k, s] : touches)
            if (std::get<0> (k) == &c && isTouched (k, mode, nowMs))
                want ((AutoTarget) std::get<1> (k), std::get<2> (k), std::get<3> (k));

        for (auto& [tg, s, p] : wanted)
        {
            float v = 0.0f;
            if (! Engine::currentValue (c, tg, s, p, v)) continue;
            auto* lane = engine.getOrCreateLane (c, tg, s, p);
            writeValue (c, *lane, t, v);
            active.insert (lane);
        }
    });

    for (auto it = passes.begin(); it != passes.end();)
    {
        if (active.count (it->first) == 0)
        {
            auto* lane = it->first;
            const bool touchMode = it->second.channel->getAutoMode() == AutoMode::touch;
            const double end = it->second.lastT;
            ++it;
            endPass (*lane, end, touchMode);
        }
        else ++it;
    }
    forEachChannel (engine, [&active] (Channel& c)
    {
        for (auto& l : c.lanes)
            if (l->writing.load() && active.count (l.get()) == 0) l->writing = false;
    });
}
} // namespace sc
