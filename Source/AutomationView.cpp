// SilverConsole - editor de automacao. Codigo original (MIT).
#include "AutomationView.h"

namespace sc
{
namespace
{
    constexpr int kHeaderRowH = 30, kLaneRowH = 74, kTopH = 40, kRulerH = 24;

    juce::NormalisableRange<float> volumeRange()
    {
        juce::NormalisableRange<float> r (kMinDb, 12.0f);
        r.setSkewForCentre (-12.0f);
        return r;
    }

    juce::AudioProcessorParameter* pluginParam (Channel& c, int slot, int param)
    {
        if (! c.hasInsert (slot)) return nullptr;
        auto* p = c.inserts[(size_t) slot].processor.get();
        if (p == nullptr) return nullptr;
        auto& params = p->getParameters();
        return juce::isPositiveAndBelow (param, params.size()) ? params[param] : nullptr;
    }

    double niceStep (double seconds, int width)
    {
        for (double s : { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 })
            if (seconds / s < width / 80.0) return s;
        return 1200.0;
    }

    juce::String clockLabel (double t)
    {
        const int m = (int) (t / 60.0);
        const double s = t - m * 60.0;
        return juce::String (m) + ":" + (s < 10 ? "0" : "") + juce::String (s, s == std::floor (s) ? 0 : 2);
    }
}

//==============================================================================
float AutomationView::toNorm (AutoTarget tg, float v)
{
    switch (tg)
    {
        case AutoTarget::volume: return volumeRange().convertTo0to1 (juce::jlimit (kMinDb, 12.0f, v));
        case AutoTarget::pan:    return (1.0f - juce::jlimit (-1.0f, 1.0f, v)) * 0.5f; // esquerda em cima
        case AutoTarget::mute:   return v >= 0.5f ? 1.0f : 0.0f;
        case AutoTarget::plugin: return juce::jlimit (0.0f, 1.0f, v);
    }
    return 0.0f;
}

float AutomationView::fromNorm (Channel& c, AutoTarget tg, int slot, int param, float n)
{
    n = juce::jlimit (0.0f, 1.0f, n);
    switch (tg)
    {
        case AutoTarget::volume:
        {
            const float db = volumeRange().convertFrom0to1 (n);
            return db <= kMinDb + 0.05f ? kMinDb : std::round (db * 10.0f) / 10.0f;
        }
        case AutoTarget::pan:  return std::round ((1.0f - 2.0f * n) * 100.0f) / 100.0f;
        case AutoTarget::mute: return n >= 0.5f ? 1.0f : 0.0f;
        case AutoTarget::plugin:
            if (auto* p = pluginParam (c, slot, param))
            {
                const int steps = p->getNumSteps();
                if ((p->isDiscrete() || p->isBoolean()) && steps > 1 && steps < 100000)
                    return std::round (n * (float) (steps - 1)) / (float) (steps - 1);
            }
            return n;
    }
    return n;
}

juce::String AutomationView::valueText (Channel& c, AutoTarget tg, int slot, int param, float v)
{
    switch (tg)
    {
        case AutoTarget::volume: return v <= kMinDb + 0.01f ? juce::String ("-inf dB") : juce::String (v, 1) + " dB";
        case AutoTarget::pan:
        {
            const int p = juce::roundToInt (v * 100.0f);
            return p == 0 ? juce::String ("C") : (p < 0 ? "L" : "R") + juce::String (std::abs (p));
        }
        case AutoTarget::mute: return v >= 0.5f ? "MUDO" : "ATIVO";
        case AutoTarget::plugin:
            if (auto* p = pluginParam (c, slot, param))
                return (p->getText (v, 24) + " " + p->getLabel()).trim();
            return juce::String (v, 3);
    }
    return {};
}

juce::String AutomationView::laneName (Channel& c, AutoTarget tg, int slot, int param)
{
    switch (tg)
    {
        case AutoTarget::volume: return "Volume";
        case AutoTarget::pan:    return c.kind == ChannelKind::track ? "Pan" : "Balance";
        case AutoTarget::mute:   return "Mute";
        case AutoTarget::plugin:
            if (auto* p = pluginParam (c, slot, param)) return p->getName (40);
            return juce::String::fromUTF8 ("Par\xc3\xa2metro ") + juce::String (param + 1);
    }
    return {};
}

//==============================================================================
class AutomationView::Ruler : public juce::Component
{
public:
    explicit Ruler (AutomationView& v) : owner (v) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (colours::panel);
        g.fillRect (r);
        const float ax = (float) headerW, aw = r.getWidth() - ax;
        g.setColour (colours::lcd);
        g.fillRect (ax, 0.0f, aw, r.getHeight());

        const double sr = owner.engine.getSampleRate();
        const auto ls = owner.engine.loopStart.load(), le = owner.engine.loopEnd.load();
        if (le > ls)
        {
            const float x0 = owner.timeToX ((double) ls / sr, ax, aw), x1 = owner.timeToX ((double) le / sr, ax, aw);
            g.setColour ((owner.engine.loopOn.load() ? colours::accent : colours::textDim).withAlpha (0.25f));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (juce::jmax (ax, x0), 0, juce::jmin (r.getRight(), x1), r.getHeight()));
        }

        const double step = niceStep (owner.viewLen, (int) aw);
        g.setFont (uiFont (10.0f));
        for (double t = std::ceil (owner.viewStart / step) * step; t <= owner.viewStart + owner.viewLen; t += step)
        {
            const float x = owner.timeToX (t, ax, aw);
            g.setColour (colours::textDim.withAlpha (0.7f));
            g.drawVerticalLine ((int) x, r.getHeight() - 8.0f, r.getHeight());
            g.setColour (colours::textDim);
            g.drawText (clockLabel (t), (int) x + 3, 2, 60, 14, juce::Justification::left);
        }
        const float px = owner.timeToX ((double) owner.engine.position.load() / sr, ax, aw);
        if (px >= ax)
        {
            g.setColour (colours::text);
            juce::Path head;
            head.addTriangle (px - 6.0f, 0.0f, px + 6.0f, 0.0f, px, 8.0f);
            g.fillPath (head);
            g.fillRect (px - 1.0f, 0.0f, 2.0f, r.getHeight());
        }
        g.setColour (colours::textDim);
        g.setFont (uiFont (11.0f, true));
        g.drawText ("TEMPO", 10, 0, headerW - 20, getHeight(), juce::Justification::centredLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override { seek (e); }
    void mouseDrag (const juce::MouseEvent& e) override { seek (e); }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const float ax = (float) headerW, aw = (float) getWidth() - ax;
        if (e.mods.isShiftDown()) owner.scrollBy (-w.deltaY * owner.viewLen * 0.25);
        else owner.zoomAround (owner.xToTime ((float) e.x, ax, aw), w.deltaY > 0 ? 0.8f : 1.25f);
    }

private:
    void seek (const juce::MouseEvent& e)
    {
        if (e.x < headerW) return;
        const float ax = (float) headerW, aw = (float) getWidth() - ax;
        const double t = juce::jmax (0.0, owner.xToTime ((float) e.x, ax, aw));
        owner.engine.position = (juce::int64) (t * owner.engine.getSampleRate());
        owner.repaint();
    }
    AutomationView& owner;
};

//==============================================================================
class AutomationView::HeaderRow : public juce::Component
{
public:
    HeaderRow (AutomationView& v, Channel& c) : owner (v), channel (c)
    {
        modeButton.setWantsKeyboardFocus (false);
        modeButton.onClick = [this] { owner.host.autoModeMenu (channel, modeButton); };
        modeButton.setTooltip ("Modo de automacao deste canal");
        addAndMakeVisible (modeButton);
        addButton.setWantsKeyboardFocus (false);
        addButton.setTooltip ("Mostrar outra linha: pan, mute ou parametro de um plugin");
        addButton.onClick = [this] { owner.showAddLaneMenu (channel, addButton); };
        addAndMakeVisible (addButton);
        refresh();
    }

    void refresh()
    {
        const auto m = channel.getAutoMode();
        modeButton.setButtonText (autoModeName (m));
        modeButton.setColour (juce::TextButton::buttonColourId, m == AutoMode::off ? juce::Colour (0xff3a4148) : autoModeColour (m).darker (0.15f));
        modeButton.setColour (juce::TextButton::textColourOffId, m == AutoMode::off ? colours::textDim : colours::ink);
        shown = m;
    }

    void paint (juce::Graphics& g) override
    {
        if (shown != channel.getAutoMode()) refresh();
        auto r = getLocalBounds();
        g.setGradientFill ({ colours::silverTop, 0, 0, colours::silverBot, 0, (float) getHeight(), false });
        g.fillRect (r.removeFromLeft (headerW));
        g.setColour (colours::panel.brighter (0.06f));
        g.fillRect (r);
        g.setColour (channel.colour);
        g.fillRect (0, 0, 5, getHeight());
        const bool open = owner.collapsed.count (&channel) == 0;
        juce::Path tri;
        if (open) tri.addTriangle (12.0f, 11.0f, 22.0f, 11.0f, 17.0f, 18.0f);
        else tri.addTriangle (13.0f, 9.0f, 20.0f, 14.5f, 13.0f, 20.0f);
        g.setColour (colours::ink);
        g.fillPath (tri);
        g.setFont (uiFont (13.0f, true));
        g.drawFittedText (channel.name, 28, 0, headerW - 28 - 104, getHeight(), juce::Justification::centredLeft, 1, 0.8f);
        g.setColour (colours::textDim);
        g.setFont (uiFont (11.0f));
        int n = 0;
        for (auto& l : channel.lanes) if (! l->points.empty()) ++n;
        g.drawText (channel.kind == ChannelKind::track ? "faixa" : channel.kind == ChannelKind::bus ? "bus" : "master",
                    headerW + 10, 0, 80, getHeight(), juce::Justification::centredLeft);
        g.drawText (juce::String (n) + (n == 1 ? " linha com pontos" : " linhas com pontos"), headerW + 90, 0, 200, getHeight(),
                    juce::Justification::centredLeft);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawHorizontalLine (getHeight() - 1, 0, (float) getWidth());
    }

    void resized() override
    {
        modeButton.setBounds (headerW - 100, 5, 62, getHeight() - 10);
        addButton.setBounds (headerW - 34, 5, 28, getHeight() - 10);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.x > headerW - 104) return;
        if (owner.collapsed.count (&channel) != 0) owner.collapsed.erase (&channel);
        else owner.collapsed.insert (&channel);
        juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<AutomationView> (&owner)] { if (sp != nullptr) sp->rebuild(); });
    }

private:
    AutomationView& owner;
    Channel& channel;
    juce::TextButton modeButton, addButton { "+" };
    AutoMode shown = AutoMode::read;
};

//==============================================================================
class AutomationView::LaneRow : public juce::Component, public juce::SettableTooltipClient
{
public:
    LaneRow (AutomationView& v, Channel& c, AutoTarget tg, int s, int p) : owner (v), channel (c), target (tg), slot (s), param (p)
    {
        setTooltip (juce::String::fromUTF8 ("Clique: novo ponto | Arraste: mover | Alt+clique ou duplo clique: apagar | Ctrl+arrastar: l\xc3\xa1pis | "
                                            "bot\xc3\xa3o direito: op\xc3\xa7\xc3\xb5\x65s | Ctrl+roda: zoom | Shift+roda: rolar"));
    }

    AutoLane* lane() const { return owner.engine.findLane (channel, target, slot, param); }

    juce::Rectangle<float> area() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft ((float) headerW).reduced (0.0f, 5.0f);
    }
    float yFor (float v) const
    {
        auto a = area().reduced (0.0f, 3.0f);
        return a.getBottom() - toNorm (target, v) * a.getHeight();
    }
    float valueForY (float y) const
    {
        auto a = area().reduced (0.0f, 3.0f);
        return fromNorm (channel, target, slot, param, (a.getBottom() - y) / juce::jmax (1.0f, a.getHeight()));
    }
    float xFor (double t) const { auto a = area(); return owner.timeToX (t, a.getX(), a.getWidth()); }
    double timeForX (float x) const { auto a = area(); return juce::jmax (0.0, owner.xToTime (x, a.getX(), a.getWidth())); }

    void paint (juce::Graphics& g) override
    {
        auto* l = lane();
        const auto mode = channel.getAutoMode();
        const bool active = l != nullptr && ! l->points.empty();
        auto full = getLocalBounds();
        auto head = full.removeFromLeft (headerW);

        // cabecalho
        g.setColour (colours::panel);
        g.fillRect (head);
        g.setColour (channel.colour.withAlpha (0.8f));
        g.fillRect (head.getX(), head.getY(), 3, head.getHeight());
        g.setColour (colours::text);
        g.setFont (uiFont (12.5f, true));
        g.drawFittedText (laneName (channel, target, slot, param), 14, 8, headerW - 22, 16, juce::Justification::centredLeft, 1, 0.8f);
        if (target == AutoTarget::plugin)
        {
            g.setColour (colours::textDim);
            g.setFont (uiFont (10.5f));
            g.drawFittedText (channel.inserts[(size_t) slot].getName() + "  (insert " + juce::String (slot + 1) + ")",
                              14, 25, headerW - 22, 14, juce::Justification::centredLeft, 1, 0.8f);
        }
        float cur = 0.0f;
        const bool hasCur = Engine::currentValue (channel, target, slot, param, cur);
        g.setColour (colours::lcdText);
        g.setFont (uiFont (12.0f, true));
        if (hasCur) g.drawText (valueText (channel, target, slot, param, cur), 14, getHeight() - 26, headerW - 22, 16, juce::Justification::centredLeft);
        if (l != nullptr && l->writing.load())
        {
            g.setColour (colours::danger);
            g.setFont (uiFont (10.0f, true));
            g.drawText ("GRAVANDO", head.withTrimmedRight (8).withTrimmedBottom (10), juce::Justification::bottomRight);
        }

        // area das linhas
        auto a = area();
        g.setColour (colours::lcd);
        g.fillRect (a);
        const double step = niceStep (owner.viewLen, (int) a.getWidth());
        g.setColour (colours::panelLine.withAlpha (0.6f));
        for (double t = std::ceil (owner.viewStart / step) * step; t <= owner.viewStart + owner.viewLen; t += step)
            g.drawVerticalLine ((int) xFor (t), a.getY(), a.getBottom());
        if (target == AutoTarget::pan || target == AutoTarget::volume)
        {
            g.setColour (colours::panelLine);
            g.drawHorizontalLine ((int) yFor (target == AutoTarget::pan ? 0.0f : 0.0f), a.getX(), a.getRight());
        }

        // forma de onda da faixa atras do volume
        if (target == AutoTarget::volume && channel.kind == ChannelKind::track)
        {
            auto& t = static_cast<Track&> (channel);
            const auto& peaks = owner.peaksFor (t);
            if (! peaks.empty() && (t.playback != nullptr || t.diskStream != nullptr))
            {
                const double len = (double) (t.diskStream != nullptr ? t.diskStream->getLength()
                                                                        : t.playback->getNumSamples()) / owner.engine.getSampleRate();
                g.setColour (channel.colour.withAlpha (0.18f));
                const float mid = a.getCentreY(), half = a.getHeight() * 0.48f;
                for (int x = (int) a.getX(); x < (int) a.getRight(); ++x)
                {
                    const double tt = owner.xToTime ((float) x, a.getX(), a.getWidth());
                    if (tt < 0.0 || tt >= len) continue;
                    const auto bin = (size_t) juce::jlimit (0, (int) peaks.size() - 1, (int) (tt / len * (double) peaks.size()));
                    const float h = juce::jmin (1.0f, peaks[bin]) * half;
                    g.drawVerticalLine (x, mid - h, mid + h);
                }
            }
        }

        const auto col = mode == AutoMode::off ? colours::textDim : channel.colour;
        if (active)
        {
            const double t0 = owner.viewStart, t1 = owner.viewStart + owner.viewLen;
            juce::Path p;
            p.startNewSubPath (a.getX(), yFor (l->valueAt (t0)));
            float lastY = yFor (l->valueAt (t0));
            int visible = 0;
            for (auto& pt : l->points)
            {
                if (pt.t <= t0) continue;
                const float x = xFor (pt.t), y = yFor (pt.v);
                if (l->step) p.lineTo (x, lastY);
                p.lineTo (x, y);
                lastY = y;
                ++visible;
                if (pt.t > t1) break;
            }
            p.lineTo (a.getRight(), yFor (l->valueAt (t1)));
            g.saveState();
            g.reduceClipRegion (a.toNearestInt());
            g.setColour (col);
            g.strokePath (p, juce::PathStrokeType (2.0f));
            if (visible < (int) a.getWidth() / 4)
                for (size_t i = 0; i < l->points.size(); ++i)
                {
                    const auto& pt = l->points[i];
                    if (pt.t < t0 || pt.t > t1) continue;
                    const auto c = juce::Point<float> (xFor (pt.t), yFor (pt.v));
                    g.setColour ((int) i == dragIndex ? colours::text : col.brighter (0.3f));
                    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (c));
                    g.setColour (juce::Colours::black);
                    g.drawEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (c), 1.0f);
                }
            g.restoreState();
        }
        else if (hasCur)
        {
            const float y = yFor (cur);
            juce::Path dash;
            dash.startNewSubPath (a.getX(), y);
            dash.lineTo (a.getRight(), y);
            float lens[] { 5.0f, 4.0f };
            juce::PathStrokeType (1.2f).createDashedStroke (dash, dash, lens, 2);
            g.setColour (col.withAlpha (0.6f));
            g.fillPath (dash);
            g.setColour (colours::textDim.withAlpha (0.7f));
            g.setFont (uiFont (11.0f));
            g.drawText (juce::String::fromUTF8 ("sem automa\xc3\xa7\xc3\xa3o \xe2\x80\x94 clique para criar pontos, ou grave em TOUCH / LATCH / WRITE"),
                        a.reduced (8, 2), juce::Justification::topLeft);
        }

        if (dragText.isNotEmpty())
        {
            g.setFont (uiFont (11.5f, true));
            auto box = juce::Rectangle<float> (dragPos.x + 10, dragPos.y - 22, 110, 18).constrainedWithin (a);
            g.setColour (juce::Colours::black.withAlpha (0.75f));
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (colours::text);
            g.drawText (dragText, box, juce::Justification::centred);
        }

        const float px = xFor ((double) owner.engine.position.load() / owner.engine.getSampleRate());
        if (px >= a.getX() && px <= a.getRight())
        {
            g.setColour (colours::text.withAlpha (0.85f));
            g.fillRect (px - 0.75f, a.getY(), 1.5f, a.getHeight());
        }
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawHorizontalLine (getHeight() - 1, 0, (float) getWidth());
    }

    int hitPoint (juce::Point<float> pos) const
    {
        auto* l = lane();
        if (l == nullptr) return -1;
        int best = -1;
        float bestD = 7.0f;
        for (size_t i = 0; i < l->points.size(); ++i)
        {
            const auto& pt = l->points[i];
            const float d = pos.getDistanceFrom ({ xFor (pt.t), yFor (pt.v) });
            if (d < bestD) { bestD = d; best = (int) i; }
        }
        return best;
    }

    void deletePoint (int idx)
    {
        if (auto* l = lane())
            owner.engine.editAutomation ([&] { if (juce::isPositiveAndBelow (idx, (int) l->points.size())) l->points.erase (l->points.begin() + idx); });
        owner.host.markDirty();
        owner.content.repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragIndex = -1;
        pencil = false;
        if (e.x < headerW)
        {
            if (e.mods.isPopupMenu()) laneMenu (-1);
            return;
        }
        const int hit = hitPoint (e.position);
        if (e.mods.isPopupMenu()) { laneMenu (hit); return; }
        if (hit >= 0 && e.mods.isAltDown()) { deletePoint (hit); return; }
        if (hit >= 0) { dragIndex = hit; updateDragText (e.position); return; }

        auto* l = owner.engine.getOrCreateLane (channel, target, slot, param);
        const double t = timeForX (e.position.x);
        const float v = valueForY (e.position.y);
        if (e.mods.isCommandDown())
        {
            pencil = true;
            lastPencilT = t;
            owner.engine.editAutomation ([&] { l->insert (t, v); });
        }
        else
        {
            owner.engine.editAutomation ([&]
            {
                l->insert (t, v);
                for (size_t i = 0; i < l->points.size(); ++i)
                    if (std::abs (l->points[i].t - t) < 1.0e-6) dragIndex = (int) i;
            });
        }
        updateDragText (e.position);
        owner.host.markDirty();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* l = lane();
        if (l == nullptr) return;
        const double t = timeForX (e.position.x);
        const float v = valueForY (e.position.y);
        if (pencil)
        {
            owner.engine.editAutomation ([&]
            {
                l->eraseRange (juce::jmin (lastPencilT, t) - 1.0e-9, juce::jmax (lastPencilT, t));
                l->insert (t, v);
            });
            lastPencilT = t;
        }
        else if (dragIndex >= 0)
        {
            owner.engine.editAutomation ([&]
            {
                auto& pts = l->points;
                if (! juce::isPositiveAndBelow (dragIndex, (int) pts.size())) return;
                const auto i = (size_t) dragIndex;
                const double lo = i > 0 ? pts[i - 1].t + 1.0e-4 : 0.0;
                const double hi = i + 1 < pts.size() ? pts[i + 1].t - 1.0e-4 : 1.0e9;
                pts[i].t = juce::jlimit (lo, juce::jmax (lo, hi), t);
                pts[i].v = v;
            });
        }
        updateDragText (e.position);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragIndex >= 0 || pencil) owner.host.markDirty();
        dragIndex = -1;
        pencil = false;
        dragText.clear();
        owner.content.repaint(); // atualiza tambem o contador do cabecalho
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int hit = hitPoint (e.position);
        if (hit >= 0) deletePoint (hit);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown()) owner.zoomAround (timeForX (e.position.x), w.deltaY > 0 ? 0.8f : 1.25f);
        else if (e.mods.isShiftDown()) owner.scrollBy (-w.deltaY * owner.viewLen * 0.25);
        else Component::mouseWheelMove (e, w);
    }

private:
    void updateDragText (juce::Point<float> pos)
    {
        dragPos = pos;
        if (auto* l = lane(); l != nullptr && dragIndex >= 0 && juce::isPositiveAndBelow (dragIndex, (int) l->points.size()))
        {
            const auto& pt = l->points[(size_t) dragIndex];
            dragText = clockLabel (pt.t) + "  " + valueText (channel, target, slot, param, pt.v);
        }
        else dragText = valueText (channel, target, slot, param, valueForY (pos.y));
    }

    void laneMenu (int hit)
    {
        juce::PopupMenu m;
        m.addSectionHeader (channel.name + " - " + laneName (channel, target, slot, param));
        m.addItem (1, "Apagar ponto", hit >= 0);
        m.addItem (2, "Limpar todos os pontos", lane() != nullptr);
        m.addItem (3, "Remover linha", lane() != nullptr && target != AutoTarget::volume);
        juce::Component::SafePointer<LaneRow> self (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [self, hit] (int r)
        {
            if (self == nullptr || r == 0) return;
            auto& o = self->owner;
            auto* l = self->lane();
            if (r == 1) self->deletePoint (hit);
            else if (r == 2 && l != nullptr) o.engine.editAutomation ([l] { l->points.clear(); });
            else if (r == 3 && l != nullptr)
            {
                o.engine.removeLane (self->channel, l);
                o.host.lanesChanged();
                return; // a linha foi destruida
            }
            o.host.markDirty();
            self->repaint();
        });
    }

    AutomationView& owner;
    Channel& channel;
    const AutoTarget target;
    const int slot, param;
    int dragIndex = -1;
    bool pencil = false;
    double lastPencilT = 0.0;
    juce::String dragText;
    juce::Point<float> dragPos;
};

//==============================================================================
AutomationView::AutomationView (Engine& e, StripHost& h) : engine (e), host (h)
{
    ruler = std::make_unique<Ruler> (*this);
    addAndMakeVisible (*ruler);
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    for (auto* b : { &allModesButton, &zoomOutButton, &zoomInButton, &fitButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    allModesButton.setTooltip (juce::String::fromUTF8 ("Mudar o modo de automa\xc3\xa7\xc3\xa3o de todos os canais"));
    allModesButton.onClick = [this] { showAllModesMenu(); };
    zoomOutButton.onClick = [this] { zoomAround (viewStart + viewLen * 0.5, 1.5f); };
    zoomInButton.onClick = [this] { zoomAround (viewStart + viewLen * 0.5, 1.0f / 1.5f); };
    fitButton.onClick = [this] { userZoomed = false; zoomToFit(); };
    zoomOutButton.setTooltip ("Afastar (Ctrl+roda do mouse)");
    zoomInButton.setTooltip ("Aproximar (Ctrl+roda do mouse)");
    fitButton.setTooltip (juce::String::fromUTF8 ("Mostrar a m\xc3\xbasica inteira"));

    help.setText (juce::String::fromUTF8 ("Clique: ponto  \xc2\xb7  Arraste: mover  \xc2\xb7  Alt+clique: apagar  \xc2\xb7  Ctrl+arrastar: l\xc3\xa1pis  \xc2\xb7  "
                                          "Grava\xc3\xa7\xc3\xa3o: escolha TOUCH, LATCH ou WRITE e mexa nos controles tocando"),
                  juce::dontSendNotification);
    help.setFont (uiFont (11.5f));
    help.setColour (juce::Label::textColourId, colours::textDim);
    addAndMakeVisible (help);
}

AutomationView::~AutomationView() { rows.clear(); }

float AutomationView::timeToX (double t, float ax, float aw) const
{
    return ax + (float) ((t - viewStart) / viewLen) * aw;
}

double AutomationView::xToTime (float x, float ax, float aw) const
{
    return viewStart + (double) (x - ax) / juce::jmax (1.0f, aw) * viewLen;
}

void AutomationView::zoomToFit()
{
    viewStart = 0.0;
    viewLen = juce::jmax (10.0, (double) engine.getLengthSamples() / engine.getSampleRate() + 2.0);
    repaint();
}

void AutomationView::zoomAround (double t, float factor)
{
    userZoomed = true;
    const double newLen = juce::jlimit (0.5, 4.0 * 3600.0, viewLen * factor);
    const double frac = (t - viewStart) / viewLen;
    viewStart = juce::jmax (0.0, t - frac * newLen);
    viewLen = newLen;
    repaint();
}

void AutomationView::scrollBy (double seconds)
{
    userZoomed = true;
    viewStart = juce::jmax (0.0, viewStart + seconds);
    repaint();
}

const std::vector<float>& AutomationView::peaksFor (const Track& t)
{
    static const std::vector<float> none;
    if (t.diskStream != nullptr) return t.diskStream->getOverview();
    const auto* buf = t.playback.get();
    if (buf == nullptr) return none;
    auto it = peakCache.find (buf);
    if (it != peakCache.end() && it->second.source.lock() == t.playback) return it->second.peaks;
    constexpr int bins = 4096;
    std::vector<float> peaks ((size_t) bins, 0.0f);
    const int len = buf->getNumSamples();
    const int stride = juce::jmax (1, len / (bins * 64)); // amostragem para ser rapido
    for (int c = 0; c < buf->getNumChannels(); ++c)
    {
        const float* d = buf->getReadPointer (c);
        for (int i = 0; i < len; i += stride)
        {
            auto& p = peaks[(size_t) ((juce::int64) i * bins / len)];
            p = juce::jmax (p, std::abs (d[i]));
        }
    }
    if (peakCache.size() > 256) peakCache.clear();
    return peakCache.insert_or_assign (buf, PeakEntry { t.playback, std::move (peaks) }).first->second.peaks;
}

juce::String AutomationView::signature() const
{
    juce::String s;
    auto add = [&s, this] (const Channel& c)
    {
        int shown = 0; // so linhas que viram linha propria (volume sempre aparece)
        for (auto& l : c.lanes) if (l->target != AutoTarget::volume) ++shown;
        s << juce::String::toHexString ((juce::pointer_sized_int) &c) << ':' << shown << (collapsed.count (&c) ? "c" : "") << ';';
    };
    for (auto& t : engine.tracks) add (*t);
    for (auto& b : engine.buses) add (*b);
    add (engine.master);
    return s;
}

void AutomationView::rebuild()
{
    rows.clear();
    auto addChannel = [this] (Channel& c)
    {
        content.addAndMakeVisible (rows.add (new HeaderRow (*this, c)));
        if (collapsed.count (&c) != 0) return;
        content.addAndMakeVisible (rows.add (new LaneRow (*this, c, AutoTarget::volume, -1, -1)));
        for (auto& l : c.lanes)
            if (l->target != AutoTarget::volume)
                content.addAndMakeVisible (rows.add (new LaneRow (*this, c, l->target, l->slot, l->param)));
    };
    for (auto& t : engine.tracks) addChannel (*t);
    for (auto& b : engine.buses) addChannel (*b);
    addChannel (engine.master);
    lastSignature = signature();
    layoutRows();
}

void AutomationView::layoutRows()
{
    int y = 0;
    const int w = juce::jmax (400, viewport.getWidth() - viewport.getScrollBarThickness());
    for (auto* r : rows)
    {
        const int h = dynamic_cast<HeaderRow*> (r) != nullptr ? kHeaderRowH : kLaneRowH;
        r->setBounds (0, y, w, h);
        y += h;
    }
    content.setSize (w, juce::jmax (y, 10));
}

void AutomationView::tick()
{
    if (! isShowing()) return;
    if (signature() != lastSignature) rebuild();
    const auto len = engine.getLengthSamples();
    if (len != lastLength) { lastLength = len; if (! userZoomed) zoomToFit(); }
    const auto pos = engine.position.load();
    if (engine.playing.load() || pos != lastPos)
    {
        lastPos = pos;
        // acompanha o cursor quando ele sai da tela
        const double t = (double) pos / engine.getSampleRate();
        if (engine.playing.load() && (t > viewStart + viewLen || t < viewStart)) viewStart = juce::jmax (0.0, t - viewLen * 0.1);
        ruler->repaint();
        content.repaint();
    }
}

void AutomationView::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    g.setColour (colours::silverTop);
    g.setFont (uiFont (15.0f, true));
    g.drawText (juce::String::fromUTF8 ("AUTOMA\xc3\x87\xc3\x83O"), 10, 0, 160, kTopH, juce::Justification::centredLeft);
}

void AutomationView::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (kTopH).reduced (8, 6);
    top.removeFromLeft (130);
    allModesButton.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (16);
    zoomOutButton.setBounds (top.removeFromLeft (30));
    top.removeFromLeft (4);
    zoomInButton.setBounds (top.removeFromLeft (30));
    top.removeFromLeft (4);
    fitButton.setBounds (top.removeFromLeft (56));
    top.removeFromLeft (16);
    help.setBounds (top);
    ruler->setBounds (r.removeFromTop (kRulerH));
    viewport.setBounds (r);
    layoutRows();
}

void AutomationView::showAddLaneMenu (Channel& c, juce::Component& source)
{
    juce::PopupMenu m;
    m.addSectionHeader ("Linhas de " + c.name);
    m.addItem (2, c.kind == ChannelKind::track ? "Pan" : "Balance", true, engine.findLane (c, AutoTarget::pan) != nullptr);
    m.addItem (3, "Mute", true, engine.findLane (c, AutoTarget::mute) != nullptr);
    bool anyPlugin = false;
    for (int s = 0; s < c.inserts.size(); ++s)
    {
        auto* p = c.inserts[(size_t) s].processor.get();
        if (p == nullptr) continue;
        anyPlugin = true;
        juce::PopupMenu sub;
        auto& params = p->getParameters();
        const int count = juce::jmin (params.size(), 600);
        for (int i = 0; i < count; ++i)
            sub.addItem (1000 + s * 10000 + i, params[i]->getName (48), true, engine.findLane (c, AutoTarget::plugin, s, i) != nullptr);
        if (params.size() > count) sub.addItem (-1, "(+" + juce::String (params.size() - count) + juce::String::fromUTF8 (" par\xc3\xa2metros n\xc3\xa3o listados)"), false);
        m.addSubMenu (juce::String (s + 1) + ". " + p->getName(), sub, count > 0);
    }
    if (! anyPlugin) m.addItem (-2, "(sem plugins nos inserts deste canal)", false);

    auto* cp = &c;
    juce::Component::SafePointer<AutomationView> self (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source), [self, cp] (int r)
    {
        if (self == nullptr || r <= 0) return;
        auto& e = self->engine;
        bool exists = false;
        for (auto& t : e.tracks) exists = exists || t.get() == cp;
        for (auto& b : e.buses) exists = exists || b.get() == cp;
        exists = exists || cp == &e.master;
        if (! exists) return;
        if (r == 2) e.getOrCreateLane (*cp, AutoTarget::pan);
        else if (r == 3) e.getOrCreateLane (*cp, AutoTarget::mute);
        else if (r >= 1000) e.getOrCreateLane (*cp, AutoTarget::plugin, (r - 1000) / 10000, (r - 1000) % 10000);
        self->collapsed.erase (cp);
        self->rebuild();
    });
}

void AutomationView::showAllModesMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Todos os canais");
    for (int i = 0; i <= (int) AutoMode::write; ++i) m.addItem (1 + i, autoModeName ((AutoMode) i));
    juce::Component::SafePointer<AutomationView> self (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&allModesButton), [self] (int r)
    {
        if (self == nullptr || r == 0) return;
        auto& e = self->engine;
        for (auto& t : e.tracks) t->autoMode = r - 1;
        for (auto& b : e.buses) b->autoMode = r - 1;
        e.master.autoMode = r - 1;
        self->host.markDirty();
        self->repaint();
    });
}
} // namespace sc
