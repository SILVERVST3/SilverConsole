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
    // botao "+": mais um slot de insert (qualquer canal) ou de envio (faixa)
    virtual void addInsertSlot (Channel&) = 0;
    virtual void addSendSlot (Track&) = 0;
    // selecao de trecho (regua do tempo)
    virtual void timelineMenu (juce::Component&) {}
    virtual void playSelection() {}
    // arrastar a faixa pelo nome para mudar a posicao (0 inicio, 1 arrastando, 2 soltou)
    virtual void stripDrag (Channel&, const juce::MouseEvent&, int) {}
    // desfazer/refazer: a interface avisa cada edicao concluida (valor antes/depois)
    enum class Param { gain, pan, mute, solo, phase, mono };
    virtual void valueEdited (Channel&, Param, float before, float after) { juce::ignoreUnused (before, after); }
    /** Cada passo de um controle (inclusive durante o arraste): grupos movem juntos. */
    virtual void liveValueChanged (Channel&, Param, float before, float after) { juce::ignoreUnused (before, after); }
    virtual void vcaEdited (int group, bool mute, float before, float after) { juce::ignoreUnused (group, mute, before, after); }
    virtual void vcaMenu (int group, juce::Component&) { juce::ignoreUnused (group); }
    virtual void sendLevelEdited (Track&, int index, float before, float after) { juce::ignoreUnused (index, before, after); }
    virtual void renamed (Channel&, const juce::String& before) { juce::ignoreUnused (before); }
    virtual void automationEdited (Channel&, AutoTarget, int slot, int param, const std::vector<AutoPoint>& before)
    { juce::ignoreUnused (slot, param, before); }
};

juce::Colour autoModeColour (AutoMode);
juce::Colour groupColour (int group);

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
    bool gesture = false;
    float gestureStart = 0.0f;
};

//==============================================================================
/** Faixa de VCA de um grupo: fader e mute que valem para todos os membros. */
class VcaStrip : public juce::Component
{
public:
    VcaStrip (int group, StripHost&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void refresh();
    void tick();
    const int group;

private:
    StripHost& host;
    juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::TextButton muteButton { "M" };
    juce::Label nameLabel, gainLabel, infoLabel;
    bool gesture = false;
    float gestureStart = 0.0f;
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
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    /** Atualiza medidores (30 Hz). */
    void tick();
    /** Sincroniza controles com o estado do motor. */
    void refresh();
    /** Cria/remove slots de insert e envio na tela conforme o motor. */
    void syncSlots();

    /** Linhas de insert/envio de TODOS os canais (alinhados pelo que tem mais). */
    static void setLayoutRows (int insertRows, int sendRows) noexcept { layoutInsertRows = insertRows; layoutSendRows = sendRows; }
    static int minimumHeight() noexcept;

    Channel& channel;
    static constexpr int width = 104;
    static constexpr int masterWidth = 128;

private:
    void updateGainLabel();
    juce::String panText() const;
    static int bottomSectionHeight (ChannelKind) noexcept; // fader, botoes e nome (fixos)

    static inline int layoutInsertRows = kDefaultInserts, layoutSendRows = kDefaultSends;

    StripHost& host;
    juce::Label nameLabel, infoLabel, gainLabel, panLabel;
    // Inserts e envios em areas proprias com rolagem; a secao do fader fica fixa.
    juce::Viewport insertsView, sendsView;
    juce::Component insertsContent, sendsContent;
    juce::OwnedArray<InsertSlotComponent> slots;
    std::vector<int> lastInsertVisualState; // evita 30x repaints/s por faixa sem mudancas
    bool shownAsleep = false;               // indicador "zz" (economia de CPU)
    juce::OwnedArray<SendRow> sends;
    juce::TextButton addInsertButton { "+" }, addSendButton { "+" };
    void refreshAutoButton();
    juce::TextButton autoButton;
    AutoMode shownMode = AutoMode::off;
    juce::TextButton outputButton, muteButton { "M" }, soloButton { "S" }, phaseButton { juce::String::fromUTF8 ("\xc3\x98") },
                     monoButton { "MONO" };
    juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::Slider panKnob { juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox };
    LevelMeter meter;
    juce::Rectangle<int> insertsHeader, sendsHeader, faderArea, loudnessArea;
    bool draggingStrip = false;
    bool faderGesture = false, panGesture = false;
    float faderGestureStart = 0.0f, panGestureStart = 0.0f;
};
} // namespace sc
