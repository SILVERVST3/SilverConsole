// SilverConsole - canal do mixer (faixa, bus e master). Codigo original (MIT).
#pragma once
#include "Engine.h"
#include "SilverLookAndFeel.h"

namespace sc
{
class StripHost
{
public:
    virtual ~StripHost() = default;
    virtual Engine& getEngine() = 0;
    virtual void insertSlotClicked (Channel&, int slot, juce::Component& source, const juce::MouseEvent&) = 0;
    virtual void channelMenu (Channel&, juce::Component& source) = 0;
    virtual void sendMenu (Track&, int send, juce::Component& source) = 0;
    virtual void outputMenu (Track&, juce::Component& source) = 0;
    virtual bool isEditorOpen (const Channel&, int slot) const = 0;
    virtual void markDirty() = 0;
    // automacao
    virtual void automationTouch (Channel&, AutoTarget, bool down) = 0;
    virtual void muteToggled (Channel&) = 0;
    virtual void autoModeMenu (Channel&, juce::Component& source) = 0;
    virtual void lanesChanged() = 0;
};

juce::Colour autoModeColour (AutoMode);

//==============================================================================
class LevelMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    void push (float l, float r);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { clip = false; repaint(); }
    static float dbToY (float db, float h);

private:
    float dispL = 0, dispR = 0, holdL = 0, holdR = 0;
    int holdTicks = 0;
    bool clip = false;
};

//==============================================================================
class InsertSlotComponent : public juce::Component, public juce::SettableTooltipClient
{
public:
    InsertSlotComponent (Channel& c, int s, StripHost& h) : channel (c), slot (s), host (h) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override { host.insertSlotClicked (channel, slot, *this, e); }

private:
    Channel& channel;
    const int slot;
    StripHost& host;
};

//==============================================================================
class SendRow : public juce::Component
{
public:
    SendRow (Track& t, int index, StripHost& h);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void refresh();

private:
    Track& track;
    const int index;
    StripHost& host;
    juce::Slider level;
};

//==============================================================================
class ChannelStrip : public juce::Component
{
public:
    ChannelStrip (Channel&, StripHost&);
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    /** Atualiza medidores (30 Hz). */
    void tick();
    /** Sincroniza controles com o estado do motor. */
    void refresh();

    Channel& channel;
    static constexpr int width = 104;
    static constexpr int masterWidth = 128;

private:
    void updateGainLabel();
    juce::String panText() const;

    StripHost& host;
    juce::Label nameLabel, infoLabel, gainLabel, panLabel;
    juce::OwnedArray<InsertSlotComponent> slots;
    juce::OwnedArray<SendRow> sends;
    void refreshAutoButton();
    juce::TextButton autoButton;
    AutoMode shownMode = AutoMode::off;
    juce::TextButton outputButton, muteButton { "M" }, soloButton { "S" }, phaseButton { juce::String::fromUTF8 ("\xc3\x98") },
                     monoButton { "MONO" };
    juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::Slider panKnob { juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox };
    LevelMeter meter;
    juce::Rectangle<int> insertsHeader, sendsHeader, faderArea;
};
} // namespace sc
