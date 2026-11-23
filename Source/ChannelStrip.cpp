// SilverConsole - canal do mixer. Codigo original (MIT).
#include "ChannelStrip.h"

namespace sc
{
namespace
{
    constexpr float kMeterMinDb = -60.0f, kMeterMaxDb = 6.0f;

    juce::String dbText (float db)
    {
        if (db <= kMinDb + 0.01f) return "-inf";
        return (db > 0.005f ? "+" : "") + juce::String (db, 1);
    }

    void setupSmallLabel (juce::Label& l, float size, juce::Colour c, juce::Justification j = juce::Justification::centred)
    {
        l.setFont (uiFont (size, true));
        l.setColour (juce::Label::textColourId, c);
        l.setJustificationType (j);
        l.setInterceptsMouseClicks (false, false);
    }
}

juce::Colour autoModeColour (AutoMode m)
{
    switch (m)
    {
        case AutoMode::off:   return juce::Colour (0xff5a636c);
        case AutoMode::read:  return juce::Colour (0xff4cc46a);
        case AutoMode::touch: return juce::Colour (0xffe8c42a);
        case AutoMode::latch: return juce::Colour (0xffe0892c);
        case AutoMode::write: return juce::Colour (0xffe2453c);
    }
    return juce::Colours::grey;
}

//==============================================================================
float LevelMeter::dbToY (float db, float h)
{
    const float t = (juce::jlimit (kMeterMinDb, kMeterMaxDb, db) - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
    return h * (1.0f - std::pow (t, 1.6f)); // mais resolucao perto do topo
}

void LevelMeter::push (float l, float r)
{
    constexpr float decay = 0.89f; // ~ -30 dB/s a 30 Hz
    dispL = juce::jmax (l, dispL * decay);
    dispR = juce::jmax (r, dispR * decay);
    if (l > 1.0f || r > 1.0f) clip = true;
    if (l >= holdL || r >= holdR || --holdTicks <= 0)
    {
        if (holdTicks <= 0) { holdL = holdR = 0; }
        holdL = juce::jmax (holdL, l);
        holdR = juce::jmax (holdR, r);
        holdTicks = 45;
    }
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (r, 2.0f);
    auto bars = r.reduced (2.0f).withTrimmedTop (6.0f);
    const float bw = (bars.getWidth() - 2.0f) * 0.5f;
    const float h = bars.getHeight();

    juce::ColourGradient grad (juce::Colour (0xffff3b30), 0, bars.getY(), juce::Colour (0xff2fd36b), 0, bars.getBottom(), false);
    grad.addColour (1.0 - dbToY (-6.0f, h) / h, juce::Colour (0xffffd23f));
    grad.addColour (1.0 - dbToY (-18.0f, h) / h, juce::Colour (0xff7be36a));

    const float vals[] { dispL, dispR }, holds[] { holdL, holdR };
    for (int c = 0; c < 2; ++c)
    {
        auto col = juce::Rectangle<float> (bars.getX() + c * (bw + 2.0f), bars.getY(), bw, h);
        g.setColour (juce::Colour (0xff101316));
        g.fillRect (col);
        const float db = juce::Decibels::gainToDecibels (vals[c], -100.0f);
        const float y = dbToY (db, h);
        g.setGradientFill (grad);
        g.fillRect (col.withTop (bars.getY() + y));
        const float hy = bars.getY() + dbToY (juce::Decibels::gainToDecibels (holds[c], -100.0f), h);
        if (holds[c] > 0.001f) { g.setColour (juce::Colours::white.withAlpha (0.8f)); g.fillRect (col.getX(), hy, bw, 1.5f); }
    }
    // Clip latch remains visible until reset, even after a transient peak.
    g.setColour (clip ? colours::danger : juce::Colour (0xff3a1d1b));
    g.fillRoundedRectangle (r.reduced (2.0f).withHeight (4.0f), 1.0f);
    // Reference lines: 0, -6, -18 and -36 dBFS. Purely visual; no DSP changes.
    g.setColour (juce::Colours::white.withAlpha (0.38f));
    for (float db : { 0.0f, -6.0f, -18.0f, -36.0f })
    {
        const float yy = bars.getY() + dbToY (db, h);
        g.drawHorizontalLine ((int) yy, bars.getX(), bars.getRight());
    }
}

//==============================================================================
void InsertSlotComponent::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    auto& s = channel.inserts[(size_t) slot];
    g.setColour (colours::lcd);
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (r, 2.0f, 1.0f);

    g.setFont (uiFont (11.5f, ! s.isEmpty()));
    auto text = r.reduced (5.0f, 0.0f);
    if (s.isEmpty())
    {
        g.setColour (colours::textDim.withAlpha (0.45f));
        g.drawText (juce::String (slot + 1), text, juce::Justification::centredLeft);
        setTooltip ("Clique para inserir um plugin");
        return;
    }
    const bool open = host.isEditorOpen (channel, slot);
    if (open)
    {
        g.setColour (colours::accent);
        g.fillRect (r.getX() + 1.0f, r.getY() + 2.0f, 2.0f, r.getHeight() - 4.0f);
    }
    juce::Colour c = colours::lcdText;
    if (s.isMissing() || s.faulted.load (std::memory_order_relaxed)) c = colours::danger;
    else if (s.bypassed.load()) c = colours::textDim;
    g.setColour (c);
    g.drawFittedText (s.getName(), text.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
    if (s.bypassed.load() || s.faulted.load())
    {
        g.setColour (s.faulted.load() ? colours::danger : colours::mute);
        g.setFont (uiFont (9.0f, true));
        g.drawText (s.faulted.load (std::memory_order_relaxed) ? "ERR" : "BYP",
                    r.withTrimmedRight (3.0f), juce::Justification::centredRight);
    }
    setTooltip (s.isMissing() ? "Plugin nao encontrado: " + s.getName()
              : s.remote != nullptr
                ? (s.faulted.load() ? "DSP IPC com falha: " : "DSP IPC isolado: ") + s.getName()
                  + " | clique direito: reiniciar processo | estado de parametros nao editavel"
                : s.faulted.load (std::memory_order_relaxed)
                  ? "Falha de processamento: " + s.getName()
                    + " (bypass automatico). Shift+clique para tentar reativar."
                  : s.getName() + "  -  clique: editor | Shift+clique: bypass | botao direito: opcoes");
}

//==============================================================================
SendRow::SendRow (Track& t, int i, StripHost& h) : track (t), index (i), host (h)
{
    level.setSliderStyle (juce::Slider::LinearHorizontal);
    level.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    level.setRange (kMinDb, 6.0, 0.1);
    level.setSkewFactorFromMidPoint (-12.0);
    level.setDoubleClickReturnValue (true, 0.0);
    level.setColour (juce::Slider::trackColourId, colours::busTint);
    level.onValueChange = [this]
    {
        track.sends[(size_t) index].levelDb = (float) level.getValue();
        level.setTooltip ("Envio " + juce::String (index + 1) + ": " + dbText ((float) level.getValue()) + " dB");
        host.markDirty();
        repaint();
    };
    addAndMakeVisible (level);
    refresh();
}

void SendRow::refresh()
{
    auto& s = track.sends[(size_t) index];
    level.setValue (s.levelDb.load(), juce::dontSendNotification);
    level.setEnabled (s.bus.load() >= 0);
    level.setTooltip ("Envio " + juce::String (index + 1) + ": " + dbText (s.levelDb.load()) + " dB");
    repaint();
}

void SendRow::resized() { level.setBounds (getLocalBounds().withTrimmedLeft (36).reduced (0, 2)); }

void SendRow::paint (juce::Graphics& g)
{
    auto& s = track.sends[(size_t) index];
    auto box = getLocalBounds().withWidth (34).toFloat().reduced (0.5f, 1.5f);
    g.setColour (colours::lcd);
    g.fillRoundedRectangle (box, 2.0f);
    const int b = s.bus.load();
    auto& buses = host.getEngine().buses;
    g.setFont (uiFont (10.5f, true));
    if (b >= 0 && b < (int) buses.size())
    {
        g.setColour (s.preFader.load() ? colours::mute : colours::busTint);
        g.drawFittedText (buses[(size_t) b]->name, box.reduced (2.0f, 0).toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }
    else
    {
        g.setColour (colours::textDim.withAlpha (0.5f));
        g.drawText ("-", box, juce::Justification::centred);
    }
}

void SendRow::mouseDown (const juce::MouseEvent& e)
{
    if (e.x < 36) host.sendMenu (track, index, *this);
}

//==============================================================================
ChannelStrip::ChannelStrip (Channel& c, StripHost& h) : channel (c), host (h)
{
    const bool isTrack = c.kind == ChannelKind::track;
    const bool isMaster = c.kind == ChannelKind::master;

    nameLabel.setText (c.name, juce::dontSendNotification);
    nameLabel.setFont (uiFont (13.0f, true));
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setColour (juce::Label::textColourId, colours::ink);
    nameLabel.setEditable (false, ! isMaster, false);
    nameLabel.onTextChange = [this]
    {
        channel.name = nameLabel.getText().trim().substring (0, 40);
        if (channel.name.isEmpty()) channel.name = "Faixa";
        nameLabel.setText (channel.name, juce::dontSendNotification);
        host.markDirty();
    };
    nameLabel.addMouseListener (this, false);
    nameLabel.setTooltip ("Duplo clique: renomear | botao direito: opcoes");
    addAndMakeVisible (nameLabel);

    setupSmallLabel (infoLabel, 10.0f, colours::inkSoft);
    addAndMakeVisible (infoLabel);

    auto setupPlus = [this] (juce::TextButton& b, const juce::String& tip)
    {
        b.setWantsKeyboardFocus (false);
        b.setTooltip (tip);
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a4148));
        b.setConnectedEdges (0);
        addAndMakeVisible (b);
    };
    setupPlus (addInsertButton, "Adicionar mais um slot de insert (botao direito no ultimo slot vazio: remover)");
    addInsertButton.onClick = [this] { host.addInsertSlot (channel); };

    if (isTrack)
    {
        setupPlus (addSendButton, "Adicionar mais um envio (menu do ultimo envio: remover)");
        addSendButton.onClick = [this] { host.addSendSlot (static_cast<Track&> (channel)); };
        outputButton.onClick = [this] { host.outputMenu (static_cast<Track&> (channel), outputButton); };
        outputButton.setTooltip ("Saida da faixa: Master ou um bus");
        addAndMakeVisible (outputButton);
    }

    panKnob.setRange (-1.0, 1.0, 0.01);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setMouseDragSensitivity (160);
    panKnob.setColour (juce::Slider::trackColourId, colours::accent);
    panKnob.onValueChange = [this]
    {
        channel.pan = (float) panKnob.getValue();
        panLabel.setText (panText(), juce::dontSendNotification);
        host.markDirty();
    };
    panKnob.setTooltip (isTrack ? "Pan (duplo clique: centro)" : "Balance (duplo clique: centro)");
    panKnob.onDragStart = [this] { host.automationTouch (channel, AutoTarget::pan, true); };
    panKnob.onDragEnd = [this] { host.automationTouch (channel, AutoTarget::pan, false); };
    addAndMakeVisible (panKnob);
    setupSmallLabel (panLabel, 10.5f, colours::ink);
    addAndMakeVisible (panLabel);

    auto setupToggle = [this] (juce::TextButton& b, juce::Colour on, const juce::String& tip)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonOnColourId, on);
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a4148));
        b.setWantsKeyboardFocus (false);
        b.setTooltip (tip);
        addAndMakeVisible (b);
    };
    setupToggle (muteButton, colours::mute, "Mute");
    muteButton.onClick = [this] { channel.mute = muteButton.getToggleState(); host.muteToggled (channel); host.markDirty(); };

    autoButton.setWantsKeyboardFocus (false);
    autoButton.setTooltip (juce::String::fromUTF8 ("Automa\xc3\xa7\xc3\xa3o: OFF, READ (toca), TOUCH (grava enquanto segura), LATCH (grava at\xc3\xa9 parar), WRITE (grava tudo)"));
    autoButton.onClick = [this] { host.autoModeMenu (channel, autoButton); };
    addAndMakeVisible (autoButton);
    if (isTrack)
    {
        setupToggle (soloButton, colours::solo, "Solo");
        soloButton.onClick = [this] { channel.solo = soloButton.getToggleState(); host.markDirty(); };
    }
    if (! isMaster)
    {
        setupToggle (phaseButton, colours::accent, "Inverter polaridade");
        phaseButton.onClick = [this] { channel.phase = phaseButton.getToggleState(); host.markDirty(); };
    }
    else
    {
        setupToggle (monoButton, colours::accent, "Ouvir a mix em mono");
        monoButton.onClick = [this] { host.getEngine().master.mono = monoButton.getToggleState(); host.markDirty(); };
    }
    outputButton.setWantsKeyboardFocus (false);

    fader.setRange (kMinDb, 12.0, 0.1);
    fader.setSkewFactorFromMidPoint (-12.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setMouseDragSensitivity (300);
    fader.setVelocityModeParameters (0.4, 1, 0.05, true, juce::ModifierKeys::ctrlModifier);
    fader.onValueChange = [this]
    {
        channel.gainDb = (float) fader.getValue();
        updateGainLabel();
        host.markDirty();
    };
    fader.setTooltip ("Volume (duplo clique: 0 dB | Ctrl+arrastar: ajuste fino)");
    fader.onDragStart = [this] { host.automationTouch (channel, AutoTarget::volume, true); };
    fader.onDragEnd = [this] { host.automationTouch (channel, AutoTarget::volume, false); };
    addAndMakeVisible (fader);
    addAndMakeVisible (meter);
    meter.setTooltip ("Pico L/R (clique: limpa o clip)");

    gainLabel.setFont (uiFont (12.0f, true));
    gainLabel.setJustificationType (juce::Justification::centred);
    gainLabel.setColour (juce::Label::textColourId, colours::lcdText);
    gainLabel.setColour (juce::Label::backgroundColourId, colours::lcd);
    gainLabel.setEditable (false, true, false);
    gainLabel.setTooltip ("Duplo clique: digitar o volume em dB");
    gainLabel.onTextChange = [this]
    {
        const auto t = gainLabel.getText().trim().toLowerCase();
        const float v = t.contains ("inf") ? kMinDb : t.retainCharacters ("0123456789.,-+").replaceCharacter (',', '.').getFloatValue();
        fader.setValue (juce::jlimit (kMinDb, 12.0f, v), juce::sendNotificationSync);
        updateGainLabel();
    };
    addAndMakeVisible (gainLabel);

    syncSlots();
    refresh();
}

void ChannelStrip::syncSlots()
{
    bool changed = false;
    while (slots.size() > channel.inserts.size()) { slots.removeLast(); changed = true; }
    while (slots.size() < channel.inserts.size())
    {
        addAndMakeVisible (slots.add (new InsertSlotComponent (channel, slots.size(), host)));
        changed = true;
    }
    lastInsertVisualState.resize ((size_t) slots.size(), 0);
    if (channel.kind == ChannelKind::track)
    {
        auto& t = static_cast<Track&> (channel);
        while (sends.size() > t.sends.size()) { sends.removeLast(); changed = true; }
        while (sends.size() < t.sends.size())
        {
            addAndMakeVisible (sends.add (new SendRow (t, sends.size(), host)));
            changed = true;
        }
    }
    if (changed && ! getBounds().isEmpty()) resized();
}

namespace
{
    constexpr int kSlotH = 17, kSendH = 20, kInsertsTop = 54, kMinFaderH = 200;
}

int ChannelStrip::minimumHeight() noexcept
{
    const int sendsHeaderY = kInsertsTop + layoutInsertRows * kSlotH + 3;
    const int y0 = sendsHeaderY + 37 + layoutSendRows * kSendH + 4;
    return y0 + 112 + kMinFaderH + 28;
}

juce::Rectangle<int> ChannelStrip::sendBlock() const
{
    return sendsHeader.withHeight (37 + layoutSendRows * kSendH);
}

juce::String ChannelStrip::panText() const
{
    const int v = juce::roundToInt (channel.pan.load() * 100.0f);
    if (v == 0) return "C";
    return (v < 0 ? "L" : "R") + juce::String (std::abs (v));
}

void ChannelStrip::updateGainLabel()
{
    gainLabel.setText (dbText (channel.gainDb.load()), juce::dontSendNotification);
}

void ChannelStrip::refresh()
{
    nameLabel.setText (channel.name, juce::dontSendNotification);
    fader.setValue (channel.gainDb.load(), juce::dontSendNotification);
    panKnob.setValue (channel.pan.load(), juce::dontSendNotification);
    panLabel.setText (panText(), juce::dontSendNotification);
    muteButton.setToggleState (channel.mute.load(), juce::dontSendNotification);
    soloButton.setToggleState (channel.solo.load(), juce::dontSendNotification);
    phaseButton.setToggleState (channel.phase.load(), juce::dontSendNotification);
    updateGainLabel();

    juce::String info;
    auto& engine = host.getEngine();
    if (channel.kind == ChannelKind::track)
    {
        auto& t = static_cast<Track&> (channel);
        if (t.sourceChannels == 0) info = "SEM AUDIO";
        else
        {
            info = t.sourceChannels == 1 ? "MONO" : "ESTEREO";
            if (t.diskStream != nullptr) info << " / DISK";
            if (t.originalRate > 0 && std::abs (t.originalRate - engine.getSampleRate()) > 0.5)
                info << "  " << juce::String (t.originalRate / 1000.0, 1) << "k";
        }
        const int ob = t.outputBus.load();
        outputButton.setButtonText (juce::String::fromUTF8 ("\xe2\x86\x92 ")
                                    + (ob >= 0 && ob < (int) engine.buses.size() ? engine.buses[(size_t) ob]->name : juce::String ("MASTER")));
        for (auto* s : sends) s->refresh();
    }
    else if (channel.kind == ChannelKind::bus) info = "BUS AUX";
    else
    {
        info = "SAIDA";
        monoButton.setToggleState (engine.master.mono.load(), juce::dontSendNotification);
    }
    infoLabel.setText (info, juce::dontSendNotification);
    refreshAutoButton();
    for (auto* s : slots) s->repaint();
    repaint();
}

void ChannelStrip::refreshAutoButton()
{
    shownMode = channel.getAutoMode();
    autoButton.setButtonText (autoModeName (shownMode));
    const auto c = autoModeColour (shownMode);
    autoButton.setColour (juce::TextButton::buttonColourId, shownMode == AutoMode::off ? juce::Colour (0xff3a4148) : c.darker (0.15f));
    autoButton.setColour (juce::TextButton::textColourOffId, shownMode == AutoMode::off ? colours::textDim : colours::ink);
}

void ChannelStrip::tick()
{
    meter.push (channel.peakL.exchange (0.0f), channel.peakR.exchange (0.0f));
    if (channel.kind == ChannelKind::master)
    {
        auto& masterChannel = static_cast<Master&> (channel);
        const auto peak = juce::jmax (masterChannel.truePeakL.exchange (0.0f),
                                      masterChannel.truePeakR.exchange (0.0f));
        const auto db = peak > 1.0e-9f
                            ? juce::String (juce::Decibels::gainToDecibels (peak), 1)
                            : juce::String ("-inf");
        const auto label = "TP " + db + " dBTP";
        if (infoLabel.getText() != label)
            infoLabel.setText (label, juce::dontSendNotification);
        repaint (sendBlock());
    }


    // a automacao move os controles
    if (! fader.isMouseButtonDown() && std::abs (fader.getValue() - channel.gainDb.load()) > 0.005)
    {
        fader.setValue (channel.gainDb.load(), juce::dontSendNotification);
        updateGainLabel();
    }
    if (! panKnob.isMouseButtonDown() && std::abs (panKnob.getValue() - channel.pan.load()) > 0.0005)
    {
        panKnob.setValue (channel.pan.load(), juce::dontSendNotification);
        panLabel.setText (panText(), juce::dontSendNotification);
    }
    if (muteButton.getToggleState() != channel.mute.load())
    {
        muteButton.setToggleState (channel.mute.load(), juce::dontSendNotification);
        repaint();
    }
    if (shownMode != channel.getAutoMode()) refreshAutoButton();
    // Atualiza apenas inserts alterados. Repintar todos os oito botoes de
    // cada uma das 128 faixas a 30 Hz seria custo inutil no mixer.
    for (int i = 0; i < slots.size(); ++i)
    {
        const auto& insert = channel.inserts[(size_t) i];
        const int visual = insert.faulted.load (std::memory_order_relaxed) ? 2
                         : insert.bypassed.load (std::memory_order_relaxed) ? 1 : 0;
        if (lastInsertVisualState[(size_t) i] != visual)
        {
            lastInsertVisualState[(size_t) i] = visual;
            slots[i]->repaint();
        }
    }
}

void ChannelStrip::resized()
{
    const int w = getWidth();
    nameLabel.setBounds (4, 6, w - 8, 20);
    infoLabel.setBounds (4, 26, w - 8, 13);
    // Layout vertical calculado: as linhas seguem o canal com mais inserts/envios,
    // para que todos os canais fiquem alinhados.
    insertsHeader = { 6, 41, w - 12, 12 };
    for (int i = 0; i < slots.size(); ++i) slots[i]->setBounds (6, kInsertsTop + i * kSlotH, w - 12, kSlotH - 1);
    addInsertButton.setBounds (w - 6 - 18, 40, 18, 13);

    const int sendsY = kInsertsTop + layoutInsertRows * kSlotH + 3;
    sendsHeader = { 6, sendsY, w - 12, 12 };
    for (int i = 0; i < sends.size(); ++i) sends[i]->setBounds (6, sendsY + 13 + i * kSendH, w - 12, kSendH - 1);
    addSendButton.setBounds (w - 6 - 18, sendsY - 1, 18, 13);
    outputButton.setBounds (6, sendsY + 13 + layoutSendRows * kSendH + 4, w - 12, 20);

    const int y0 = sendsY + 37 + layoutSendRows * kSendH + 4;
    panKnob.setBounds (w / 2 - 21, y0, 42, 42);
    panLabel.setBounds (4, y0 + 41, w - 8, 13);

    autoButton.setBounds (6, y0 + 58, w - 12, 20);
    const int bw = (w - 16) / 3;
    const int rowY = y0 + 82;
    if (channel.kind == ChannelKind::master)
    {
        muteButton.setBounds (6, rowY, 30, 20);
        monoButton.setBounds (40, rowY, w - 46, 20);
    }
    else
    {
        muteButton.setBounds (6, rowY, bw, 20);
        if (channel.kind == ChannelKind::track) soloButton.setBounds (8 + bw, rowY, bw, 20);
        phaseButton.setBounds (10 + 2 * bw, rowY, bw, 20);
    }

    const int bottom = getHeight() - 28;
    const int faderTop = y0 + 112;
    faderArea = { 0, faderTop, w, juce::jmax (80, bottom - faderTop) };
    const int meterW = channel.kind == ChannelKind::master ? 22 : 16;
    meter.setBounds (w - meterW - 6, faderArea.getY(), meterW, faderArea.getHeight());
    fader.setBounds (24, faderArea.getY() - 10, w - meterW - 34, faderArea.getHeight() + 20);
    gainLabel.setBounds (10, getHeight() - 24, w - 20, 18);
}

void ChannelStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.5f, 0.0f);
    const bool isBus = channel.kind == ChannelKind::bus;
    const bool isMaster = channel.kind == ChannelKind::master;
    auto top = isBus ? colours::silverTop.interpolatedWith (colours::busTint, 0.12f) : colours::silverTop;
    auto bot = isMaster ? colours::silverBot.darker (0.08f) : colours::silverBot;
    g.setGradientFill ({ top, 0, 0, bot, 0, (float) getHeight(), false });
    g.fillRoundedRectangle (r, 4.0f);
    // textura escovada
    juce::Random rnd (channel.name.hashCode());
    for (int y = 2; y < getHeight(); y += 3)
    {
        g.setColour (juce::Colours::white.withAlpha (0.03f + rnd.nextFloat() * 0.04f));
        g.drawHorizontalLine (y, r.getX() + 2, r.getRight() - 2);
    }
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);

    g.setColour (channel.colour);
    g.fillRoundedRectangle (r.getX() + 4, 2.0f, r.getWidth() - 8, 4.0f, 1.5f);
    // Distinct master/bus outline for quicker navigation in large sessions.
    if (isBus || isMaster)
    {
        g.setColour ((isMaster ? colours::accent : colours::busTint).withAlpha (0.65f));
        g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 1.5f);
    }

    auto header = [&g] (juce::Rectangle<int> a, const juce::String& t)
    {
        g.setColour (colours::inkSoft);
        g.setFont (uiFont (9.5f, true));
        g.drawText (t, a, juce::Justification::centredLeft);
    };
    header (insertsHeader, "INSERTS");
    if (channel.kind == ChannelKind::track) header (sendsHeader, "ENVIOS");
    else
    {
        g.setColour (juce::Colours::black.withAlpha (0.08f));
        g.fillRoundedRectangle (sendBlock().toFloat(), 3.0f);
        g.setColour (colours::inkSoft.withAlpha (0.7f));
        g.setFont (uiFont (10.0f, true));
        if (isMaster)
        {
            const auto& m = static_cast<const Master&> (channel);
            g.drawText ("LOUDNESS (M)", sendsHeader.withHeight (32), juce::Justification::centred);
            g.setFont (uiFont (16.0f, true));
            g.setColour (colours::ink);
            const auto loudness = m.momentaryLufs.load (std::memory_order_relaxed);
            const auto value = loudness <= -110.0f
                                   ? juce::String ("-inf LUFS")
                                   : juce::String (loudness, 1) + " LUFS";
            g.drawText (value, sendsHeader.withTrimmedTop (31).withHeight (57),
                        juce::Justification::centred);
            g.setFont (uiFont (8.5f));
            g.drawText ("Momentary / 400ms", sendsHeader.withTrimmedTop (82).withHeight (30),
                        juce::Justification::centred);
        }
        else
            g.drawText (juce::String ("MASTER"),
                        sendBlock(), juce::Justification::centred);
    }

    // escala do fader
    g.setFont (uiFont (9.0f, true));
    for (float db : { 12.0f, 6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -40.0f, kMinDb })
    {
        const float y = (float) fader.getY() + (float) fader.getPositionOfValue (db);
        g.setColour (db == 0.0f ? colours::ink : colours::inkSoft);
        g.drawHorizontalLine ((int) y, 4.0f, 22.0f);
        g.drawText (db <= kMinDb ? juce::String::fromUTF8 ("\xe2\x88\x9e") : juce::String ((int) std::abs (db)),
                    2, (int) y - 9, 20, 8, juce::Justification::centredLeft);
    }
}

void ChannelStrip::paintOverChildren (juce::Graphics& g)
{
    if (channel.kind == ChannelKind::track && channel.mute.load())
    {
        g.setColour (colours::mute.withAlpha (0.10f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f, 0), 4.0f);
    }
}

void ChannelStrip::mouseDown (const juce::MouseEvent& e)
{
    const bool onName = e.eventComponent == &nameLabel;
    const bool onTop = e.eventComponent == this && e.y < 40;
    if ((onName && e.mods.isPopupMenu()) || onTop)
        host.channelMenu (channel, nameLabel);
}
} // namespace sc
