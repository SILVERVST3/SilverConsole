// SilverConsole - editor de automacao (linhas desenhaveis). Codigo original (MIT).
#pragma once
#include "ChannelStrip.h"
#include <map>
#include <set>

namespace sc
{
class AutomationView : public juce::Component
{
public:
    AutomationView (Engine&, StripHost&);
    ~AutomationView() override;

    void rebuild();
    void tick();
    void zoomToFit();
    void paint (juce::Graphics&) override;
    void resized() override;

    // mapeamento tempo <-> x (x relativo a area das linhas, sem o cabecalho)
    static constexpr int headerW = 210;
    float timeToX (double t, float areaX, float areaW) const;
    double xToTime (float x, float areaX, float areaW) const;
    void zoomAround (double t, float factor);
    void scrollBy (double seconds);
    const std::vector<float>& peaksFor (const Track&);

    // valores
    static float toNorm (AutoTarget, float v);
    static float fromNorm (Channel&, AutoTarget, int slot, int param, float n);
    static juce::String valueText (Channel&, AutoTarget, int slot, int param, float v);
    static juce::String laneName (Channel&, AutoTarget, int slot, int param);

    Engine& engine;
    StripHost& host;
    std::set<const Channel*> collapsed;

private:
    class Ruler;
    class HeaderRow;
    class LaneRow;
    friend class Ruler;
    friend class HeaderRow;
    friend class LaneRow;

    juce::String signature() const;
    void layoutRows();
    void showAddLaneMenu (Channel&, juce::Component&);
    void showAllModesMenu();

    double viewStart = 0.0, viewLen = 10.0;
    bool userZoomed = false;
    juce::int64 lastLength = -1, lastPos = -1;
    juce::String lastSignature;

    juce::TextButton allModesButton { "Modo de todas" }, zoomOutButton { "-" }, zoomInButton { "+" }, fitButton { "Tudo" };
    juce::Label help;
    std::unique_ptr<Ruler> ruler;
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<juce::Component> rows;
    std::map<const juce::AudioBuffer<float>*, std::vector<float>> peakCache;
};
} // namespace sc
