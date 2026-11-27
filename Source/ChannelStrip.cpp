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
        g.setColour (colours::lcd);
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
    if (s.isOffline()) c = colours::textDim.withAlpha (0.8f);
    else if (s.isMissing() || s.faulted.load (std::memory_order_relaxed)) c = colours::danger;
    else if (s.bypassed.load()) c = colours::textDim;
    if (channel.asleep.load (std::memory_order_relaxed)) c = c.withMultipliedAlpha (0.45f); // dormindo: sem CPU
    g.setColour (c);
    g.drawFittedText (s.getName(), text.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
    if (s.isOffline())
    {
        g.setColour (colours::silverBot);
        g.setFont (uiFont (9.0f, true));
        g.drawText ("OFF", r.withTrimmedRight (3.0f), juce::Justification::centredRight);
        const bool frozen = channel.kind == ChannelKind::track && static_cast<const Track&> (channel).isFrozen();
        setTooltip (frozen ? s.getName() + juce::String::fromUTF8 (" - gravado no \xc3\xa1udio congelado. Clique: descongelar")
                           : s.getName() + juce::String::fromUTF8 (" - offline (sem CPU/mem\xc3\xb3ria, ajuste guardado). Clique: colocar online"));
        return;
    }
    const float mixValue = s.mix.load (std::memory_order_relaxed);
    if (s.sidechain != nullptr && mixValue >= 0.999f && ! s.bypassed.load() && ! s.faulted.load())
    {
        g.setColour (colours::accent);
        g.setFont (uiFont (9.0f, true));
        g.drawText ("SC", r.withTrimmedRight (3.0f), juce::Justification::centredRight);
    }
    if (mixValue < 0.999f && ! s.bypassed.load() && ! s.faulted.load())
    {
        g.setColour (colours::silverBot);
        g.setFont (uiFont (9.0f, true));
        g.drawText (juce::String (juce::roundToInt (mixValue * 100.0f)) + "%", r.withTrimmedRight (3.0f), juce::Justification::centredRight);
    }
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
SendRow::SendRow (Channel& c, int i, StripHost& h) : channel (c), index (i), host (h)
{
    level.setSliderStyle (juce::Slider::LinearHorizontal);
    level.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    level.setRange (kMinDb, 6.0, 0.1);
    level.setSkewFactorFromMidPoint (-12.0);
    level.setDoubleClickReturnValue (true, 0.0);
    level.setColour (juce::Slider::trackColourId, colours::busTint);
    level.onDragStart = [this] { gesture = true; gestureStart = channel.sends[(size_t) index].levelDb.load(); };
    level.onDragEnd = [this]
    {
        gesture = false;
        host.sendLevelEdited (channel, index, gestureStart, channel.sends[(size_t) index].levelDb.load());
    };
    level.onValueChange = [this]
    {
        const float before = channel.sends[(size_t) index].levelDb.load();
        channel.sends[(size_t) index].levelDb = (float) level.getValue();
        if (! gesture) host.sendLevelEdited (channel, index, before, (float) level.getValue());
        level.setTooltip ("Envio " + juce::String (index + 1) + ": " + dbText ((float) level.getValue()) + " dB");
        host.markDirty();
        repaint();
    };
    addAndMakeVisible (level);
    refresh();
}

void SendRow::refresh()
{
    if (index >= channel.sends.size()) return;
    auto& s = channel.sends[(size_t) index];
    level.setValue (s.levelDb.load(), juce::dontSendNotification);
    level.setEnabled (s.bus.load() >= 0);
    level.setTooltip ("Envio " + juce::String (index + 1) + ": " + dbText (s.levelDb.load()) + " dB");
    repaint();
}

void SendRow::resized() { level.setBounds (getLocalBounds().withTrimmedLeft (36).reduced (0, 2)); }

void SendRow::paint (juce::Graphics& g)
{
    if (index >= channel.sends.size()) return;
    auto& s = channel.sends[(size_t) index];
    auto box = getLocalBounds().withWidth (34).toFloat().reduced (0.5f, 1.5f);
    g.setColour (colours::lcd);
    g.fillRoundedRectangle (box, 2.0f);
    const int b = s.bus.load();
    auto& buses = host.getEngine().buses;
    g.setFont (uiFont (10.5f, true));
    if (b >= 0 && b < (int) buses.size())
    {
        g.setColour (s.preFx.load() ? colours::accent : s.preFader.load() ? colours::mute : colours::busTint);
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
    if (e.x < 36) host.sendMenu (channel, index, *this);
}

//==============================================================================
void InsertSlotComponent::mouseDown (const juce::MouseEvent& e)
{
    clickPending = dragging = false;
    const auto& s = channel.inserts[(size_t) slot];
    const bool frozen = channel.kind == ChannelKind::track && static_cast<const Track&> (channel).isFrozen();
    // clique esquerdo simples num plugin: espera para ver se e arraste (mover o plugin)
    if (! e.mods.isPopupMenu() && ! e.mods.isShiftDown() && ! s.isEmpty() && ! frozen)
    {
        clickPending = true;
        return;
    }
    host.insertSlotClicked (channel, slot, *this, e);
}

void InsertSlotComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (! clickPending && ! dragging) return;
    if (! dragging && e.getDistanceFromDragStart() < 5) return;
    if (! dragging)
    {
        dragging = true;
        clickPending = false;
        host.insertDrag (channel, slot, e, 0);
    }
    host.insertDrag (channel, slot, e, 1);
}

void InsertSlotComponent::mouseUp (const juce::MouseEvent& e)
{
    if (dragging)
    {
        dragging = false;
        host.insertDrag (channel, slot, e, 2);
        return;
    }
    if (clickPending)
    {
        clickPending = false;
        host.insertSlotClicked (channel, slot, *this, e);
    }
}

//==============================================================================
ChannelStrip::ChannelStrip (Channel& c, StripHost& h) : channel (c), host (h)
{
    const bool isTrack = c.kind == ChannelKind::track;
    const bool isMaster = c.kind == ChannelKind::master;

    nameLabel.setText (c.name, juce::dontSendNotification);
    nameLabel.setFont (uiFont (13.0f, true));
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setColour (juce::Label::textColourId, colours::text);
    nameLabel.setEditable (false, ! isMaster, false);
    nameLabel.onTextChange = [this]
    {
        const auto before = channel.name;
        channel.name = nameLabel.getText().trim().substring (0, 40);
        if (channel.name.isEmpty()) channel.name = "Faixa";
        nameLabel.setText (channel.name, juce::dontSendNotification);
        if (channel.name != before) host.renamed (channel, before);
        host.markDirty();
    };
    nameLabel.addMouseListener (this, false);
    nameLabel.setTooltip (! isMaster ? "Arrastar (aqui ou no fundo do canal): mudar a posicao | duplo clique: renomear | botao direito: opcoes"
                                     : "Botao direito: opcoes");
    addAndMakeVisible (nameLabel);

    setupSmallLabel (infoLabel, 10.0f, colours::textDim);
    addAndMakeVisible (infoLabel);

    auto setupPlus = [this] (juce::TextButton& b, const juce::String& tip)
    {
        b.setWantsKeyboardFocus (false);
        b.setTooltip (tip);
        b.setColour (juce::TextButton::buttonColourId, colours::gun);
        b.setConnectedEdges (0);
        addAndMakeVisible (b);
    };
    setupPlus (addInsertButton, "Adicionar mais um slot de insert (botao direito no ultimo slot vazio: remover)");
    addInsertButton.onClick = [this] { host.addInsertSlot (channel); };

    if (! isMaster)
    {
        setupPlus (addSendButton, isTrack ? "Adicionar mais um envio (menu do ultimo envio: remover)"
                                          : "Adicionar mais um envio deste bus para outro bus (menu do ultimo envio: remover)");
        addSendButton.onClick = [this] { host.addSendSlot (channel); };
    }
    if (isMaster)
    {
        compPanel = std::make_unique<BusCompPanel> (static_cast<Master&> (c), h);
        addAndMakeVisible (*compPanel);
    }
    if (isTrack)
    {
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
        const float before = channel.pan.load();
        channel.pan = (float) panKnob.getValue();
        panLabel.setText (panText(), juce::dontSendNotification);
        if (! panGesture) host.valueEdited (channel, StripHost::Param::pan, before, channel.pan.load());
        host.markDirty();
    };
    panKnob.setTooltip (isTrack ? "Pan (duplo clique: centro)" : "Balance (duplo clique: centro)");
    panKnob.onDragStart = [this]
    {
        panGesture = true;
        panGestureStart = channel.pan.load();
        host.automationTouch (channel, AutoTarget::pan, true);
    };
    panKnob.onDragEnd = [this]
    {
        panGesture = false;
        host.automationTouch (channel, AutoTarget::pan, false);
        host.valueEdited (channel, StripHost::Param::pan, panGestureStart, channel.pan.load());
    };
    addAndMakeVisible (panKnob);
    setupSmallLabel (panLabel, 10.5f, colours::lcdText);
    addAndMakeVisible (panLabel);

    auto setupToggle = [this] (juce::TextButton& b, juce::Colour on, const juce::String& tip)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonOnColourId, on);
        b.setColour (juce::TextButton::textColourOnId, on == colours::accentDeep ? juce::Colours::white : colours::ink);
        b.setColour (juce::TextButton::buttonColourId, colours::gun);
        b.setWantsKeyboardFocus (false);
        b.setTooltip (tip);
        addAndMakeVisible (b);
    };
    setupToggle (muteButton, colours::mute, "Mute");
    muteButton.onClick = [this]
    {
        channel.mute = muteButton.getToggleState();
        host.muteToggled (channel);
        host.valueEdited (channel, StripHost::Param::mute, channel.mute.load() ? 0.0f : 1.0f, channel.mute.load() ? 1.0f : 0.0f);
        host.markDirty();
    };

    autoButton.setWantsKeyboardFocus (false);
    autoButton.setTooltip (juce::String::fromUTF8 ("Automa\xc3\xa7\xc3\xa3o: OFF, READ (toca), TOUCH (grava enquanto segura), LATCH (grava at\xc3\xa9 parar), WRITE (grava tudo)"));
    autoButton.onClick = [this] { host.autoModeMenu (channel, autoButton); };
    addAndMakeVisible (autoButton);
    if (! isMaster)
    {
        setupToggle (soloButton, colours::solo, isTrack ? juce::String ("Solo")
                                                        : juce::String::fromUTF8 ("Solo do bus: ouve o bus e o que chega nele (faixas roteadas e envios)"));
        soloButton.onClick = [this]
        {
            channel.solo = soloButton.getToggleState();
            host.valueEdited (channel, StripHost::Param::solo, channel.solo.load() ? 0.0f : 1.0f, channel.solo.load() ? 1.0f : 0.0f);
            host.markDirty();
        };
    }
    if (! isMaster)
    {
        setupToggle (phaseButton, colours::accentDeep, "Inverter polaridade");
        phaseButton.onClick = [this]
        {
            channel.phase = phaseButton.getToggleState();
            host.valueEdited (channel, StripHost::Param::phase, channel.phase.load() ? 0.0f : 1.0f, channel.phase.load() ? 1.0f : 0.0f);
            host.markDirty();
        };
    }
    else
    {
        setupToggle (monoButton, colours::accentDeep, "Ouvir a mix em mono");
        monoButton.onClick = [this]
        {
            const bool on = monoButton.getToggleState();
            host.getEngine().master.mono = on;
            host.valueEdited (channel, StripHost::Param::mono, on ? 0.0f : 1.0f, on ? 1.0f : 0.0f);
            host.markDirty();
        };
    }
    outputButton.setWantsKeyboardFocus (false);

    fader.setRange (kMinDb, 12.0, 0.1);
    fader.setSkewFactorFromMidPoint (-12.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setMouseDragSensitivity (300);
    fader.setVelocityModeParameters (0.4, 1, 0.05, true, juce::ModifierKeys::ctrlModifier);
    fader.onValueChange = [this]
    {
        const float before = channel.gainDb.load();
        channel.gainDb = (float) fader.getValue();
        updateGainLabel();
        host.liveValueChanged (channel, StripHost::Param::gain, before, channel.gainDb.load());
        if (! faderGesture) host.valueEdited (channel, StripHost::Param::gain, before, channel.gainDb.load());
        host.markDirty();
    };
    fader.setTooltip ("Volume (duplo clique: 0 dB | Ctrl+arrastar: ajuste fino)");
    fader.onDragStart = [this]
    {
        faderGesture = true;
        faderGestureStart = channel.gainDb.load();
        host.automationTouch (channel, AutoTarget::volume, true);
    };
    fader.onDragEnd = [this]
    {
        faderGesture = false;
        host.automationTouch (channel, AutoTarget::volume, false);
        host.valueEdited (channel, StripHost::Param::gain, faderGestureStart, channel.gainDb.load());
    };
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

    for (auto* v : { &insertsView, &sendsView })
    {
        v->setScrollBarsShown (true, false); // vertical, so quando precisa
        v->setScrollBarThickness (6);
        v->setWantsKeyboardFocus (false);
    }
    insertsView.setViewedComponent (&insertsContent, false);
    addAndMakeVisible (insertsView);
    if (! isMaster)
    {
        sendsView.setViewedComponent (&sendsContent, false);
        addAndMakeVisible (sendsView);
    }

    syncSlots();
    refresh();
}

void ChannelStrip::syncSlots()
{
    bool changed = false;
    while (slots.size() > channel.inserts.size()) { slots.removeLast(); changed = true; }
    while (slots.size() < channel.inserts.size())
    {
        insertsContent.addAndMakeVisible (slots.add (new InsertSlotComponent (channel, slots.size(), host)));
        changed = true;
    }
    lastInsertVisualState.resize ((size_t) slots.size(), 0);
    if (channel.kind != ChannelKind::master)
    {
        while (sends.size() > channel.sends.size()) { sends.removeLast(); changed = true; }
        while (sends.size() < channel.sends.size())
        {
            sendsContent.addAndMakeVisible (sends.add (new SendRow (channel, sends.size(), host)));
            changed = true;
        }
    }
    if (changed && ! getBounds().isEmpty()) resized();
}

namespace
{
    constexpr int kSlotH = 17, kSendH = 20, kHeaderH = 14, kRackTop = 24, kFaderH = 232, kNameH = 22, kLoudnessH = 60;
}

int ChannelStrip::bottomSectionHeight (ChannelKind k) noexcept
{
    // (saida da faixa) + pan + automacao + botoes + fader + volume + nome
    return (k == ChannelKind::track ? 26 : 0) + 62 + 24 + 34 + kFaderH + 22 + kNameH + 8;
}

int ChannelStrip::minimumHeight() noexcept
{
    // rack com pelo menos 2 linhas de insert e 2 de envio visiveis (o resto rola)
    return kRackTop + 2 * kHeaderH + 4 + 2 * kSlotH + 2 * kSendH + 6 + bottomSectionHeight (ChannelKind::track);
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
        if (t.isFrozen()) info = "CONGELADA";
        else if (t.sourceChannels == 0) info = "SEM AUDIO";
        else
        {
            info = t.sourceChannels == 1 ? "MONO" : "ESTEREO";
            if (t.diskStream != nullptr) info << " / DISK";
            if (t.originalRate > 0 && std::abs (t.originalRate - engine.getSampleRate()) > 0.5)
                info << "  " << juce::String (t.originalRate / 1000.0, 1) << "k";
        }
        infoLabel.setColour (juce::Label::textColourId, t.isFrozen() ? colours::accent : colours::textDim);
        phaseButton.setEnabled (! t.isFrozen()); // a fase ja esta gravada no audio congelado
        phaseButton.setTooltip (t.isFrozen() ? juce::String::fromUTF8 ("Faixa congelada: a fase est\xc3\xa1 gravada no \xc3\xa1udio")
                                             : juce::String ("Inverter polaridade"));
        const int ob = t.outputBus.load();
        outputButton.setButtonText (juce::String::fromUTF8 ("\xe2\x86\x92 ")
                                    + (ob >= 0 && ob < (int) engine.buses.size() ? engine.buses[(size_t) ob]->name : juce::String ("MASTER")));
    }
    else if (channel.kind == ChannelKind::bus) info = "BUS AUX";
    else
    {
        info = "SAIDA";
        monoButton.setToggleState (engine.master.mono.load(), juce::dontSendNotification);
        if (compPanel != nullptr) compPanel->refresh();
    }
    for (auto* s : sends) s->refresh();
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
    autoButton.setColour (juce::TextButton::buttonColourId, shownMode == AutoMode::off ? colours::gun : c.darker (0.15f));
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
        repaint (loudnessArea);
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
        const int visual = (insert.faulted.load (std::memory_order_relaxed) ? 2
                          : insert.bypassed.load (std::memory_order_relaxed) ? 1
                          : insert.isOffline() ? 3 : 0)
                         + (channel.asleep.load (std::memory_order_relaxed) ? 8 : 0);
        if (lastInsertVisualState[(size_t) i] != visual)
        {
            lastInsertVisualState[(size_t) i] = visual;
            slots[i]->repaint();
        }
    }
    const bool asleepNow = channel.asleep.load (std::memory_order_relaxed);
    if (asleepNow != shownAsleep) { shownAsleep = asleepNow; repaint (insertsHeader); }
}

void ChannelStrip::resized()
{
    const int w = getWidth(), H = getHeight();
    const bool isTrack = channel.kind == ChannelKind::track;
    const bool isMaster = channel.kind == ChannelKind::master;
    const bool hasSends = ! isMaster;
    infoLabel.setBounds (4, 8, w - 8, 13);

    // ---- secao fixa (de baixo para cima): nome, volume, fader, botoes, automacao, pan, saida
    int y = H - 4;
    nameLabel.setBounds (4, y - kNameH, w - 8, kNameH);
    y -= kNameH + 4;
    gainLabel.setBounds (10, y - 18, w - 20, 18);
    y -= 18 + 4;
    const int faderTop = y - kFaderH;
    faderArea = { 0, faderTop, w, kFaderH };
    const int meterW = isMaster ? 22 : 16;
    meter.setBounds (w - meterW - 6, faderTop, meterW, kFaderH);
    fader.setBounds (24, faderTop - 10, w - meterW - 34, kFaderH + 20);

    const int rowY = faderTop - 12 - 20;
    const int bw = (w - 16) / 3;
    if (isMaster)
    {
        muteButton.setBounds (6, rowY, 30, 20);
        monoButton.setBounds (40, rowY, w - 46, 20);
    }
    else
    {
        muteButton.setBounds (6, rowY, bw, 20);
        soloButton.setBounds (8 + bw, rowY, bw, 20);
        phaseButton.setBounds (10 + 2 * bw, rowY, bw, 20);
    }
    const int autoY = rowY - 24;
    autoButton.setBounds (6, autoY, w - 12, 20);
    const int panY = autoY - 4 - 56;
    panKnob.setBounds (w / 2 - 21, panY, 42, 42);
    panLabel.setBounds (4, panY + 41, w - 8, 13);
    int rackBottom = panY - 6;
    if (isTrack)
    {
        outputButton.setBounds (6, panY - 6 - 20, w - 12, 20);
        rackBottom = outputButton.getY() - 6;
    }
    if (isMaster)
    {
        // de baixo para cima: loudness, compressor de barramento, inserts (com rolagem).
        // Janela baixa: some o loudness, depois o compressor fica compacto (so a agulha).
        const int minInserts = kRackTop + kHeaderH + 2 * kSlotH;
        const int room = rackBottom - minInserts;
        const bool showLoudness = room >= BusCompPanel::fullHeight + kLoudnessH + 8;
        const bool full = room >= BusCompPanel::fullHeight + 4;
        loudnessArea = showLoudness ? juce::Rectangle<int> (6, rackBottom - kLoudnessH, w - 12, kLoudnessH) : juce::Rectangle<int>();
        if (showLoudness) rackBottom = loudnessArea.getY() - 4;
        compPanel->setCompact (! full);
        const int ch = full ? BusCompPanel::fullHeight : BusCompPanel::compactHeight;
        compPanel->setBounds (6, rackBottom - ch, w - 12, ch);
        rackBottom = compPanel->getY() - 4;
    }

    // ---- rack: inserts (e envios) com rolagem. Divisao igual em todas as faixas,
    // pelo canal que tem mais linhas, para os cabecalhos ficarem alinhados.
    insertsHeader = { 6, kRackTop, w - 12, 12 };
    addInsertButton.setBounds (w - 6 - 18, kRackTop - 1, 18, 13);
    const int avail = juce::jmax (0, rackBottom - kRackTop - kHeaderH - (hasSends ? kHeaderH + 4 : 0));
    int insH = avail, sendH = 0;
    if (hasSends)
    {
        const int needI = layoutInsertRows * kSlotH, needS = layoutSendRows * kSendH;
        if (needI + needS <= avail)          { sendH = needS; insH = avail - needS; }
        else if (needI <= avail * 55 / 100)  { insH = needI; sendH = avail - needI; }
        else if (needS <= avail * 45 / 100)  { sendH = needS; insH = avail - needS; }
        else                                 { insH = avail * 55 / 100; sendH = avail - insH; }
    }
    insertsView.setBounds (6, kRackTop + kHeaderH, w - 12, insH);
    {
        const int contentH = slots.size() * kSlotH;
        const int cw = insertsView.getWidth() - (contentH > insH ? 7 : 0);
        insertsContent.setSize (cw, contentH);
        for (int i = 0; i < slots.size(); ++i) slots[i]->setBounds (0, i * kSlotH, cw, kSlotH - 1);
    }
    if (hasSends)
    {
        const int sy = kRackTop + kHeaderH + insH + 4;
        sendsHeader = { 6, sy, w - 12, 12 };
        addSendButton.setBounds (w - 6 - 18, sy - 1, 18, 13);
        sendsView.setBounds (6, sy + kHeaderH, w - 12, sendH);
        const int contentH = sends.size() * kSendH;
        const int cw = sendsView.getWidth() - (contentH > sendH ? 7 : 0);
        sendsContent.setSize (cw, contentH);
        for (int i = 0; i < sends.size(); ++i) sends[i]->setBounds (0, i * kSendH, cw, kSendH - 1);
    }
}

void ChannelStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.5f, 0.0f);
    const bool isBus = channel.kind == ChannelKind::bus;
    const bool isMaster = channel.kind == ChannelKind::master;
    // faixas e master: placa azul-marinho; buses: placa chumbo (facil de achar em sessoes grandes)
    if (isBus) SilverLookAndFeel::drawPlate (g, r, colours::gunHi.darker (0.25f), colours::gunLo.darker (0.3f), channel.name.hashCode());
    else if (isMaster) SilverLookAndFeel::drawPlate (g, r, colours::navyHi.brighter (0.15f), colours::navyDeep, 7);
    else SilverLookAndFeel::drawPlate (g, r, colours::navyHi, colours::navyLo, channel.name.hashCode());

    g.setColour (channel.colour);
    g.fillRoundedRectangle (r.getX() + 4, 2.0f, r.getWidth() - 8, 4.0f, 1.5f);
    // Distinct master/bus outline for quicker navigation in large sessions.
    if (isBus || isMaster)
    {
        g.setColour (isMaster ? colours::accent.withAlpha (0.65f) : colours::busTint.withAlpha (0.38f));
        g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 1.5f);
    }

    auto header = [&g] (juce::Rectangle<int> a, const juce::String& t)
    {
        g.setColour (colours::silverBot.withAlpha (0.8f));
        g.setFont (uiFont (9.5f, true));
        g.drawText (t, a, juce::Justification::centredLeft);
    };
    header (insertsHeader, "INSERTS");
    if (channel.asleep.load (std::memory_order_relaxed))
    {
        // economia de CPU: plugins do canal parados enquanto ha silencio
        g.setColour (colours::accent.withAlpha (0.9f));
        g.setFont (uiFont (9.5f, true));
        g.drawText ("zz", insertsHeader.withTrimmedLeft (46), juce::Justification::centredLeft);
    }
    if (channel.kind != ChannelKind::master) header (sendsHeader, "ENVIOS");
    if (isMaster && ! loudnessArea.isEmpty())
    {
        const auto& m = static_cast<const Master&> (channel);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (loudnessArea.toFloat(), 3.0f);
        g.setColour (colours::textDim);
        g.setFont (uiFont (9.5f, true));
        g.drawText ("LOUDNESS (M)", loudnessArea.withHeight (16), juce::Justification::centred);
        g.setFont (uiFont (16.0f, true));
        g.setColour (colours::lcdText);
        const auto loudness = m.momentaryLufs.load (std::memory_order_relaxed);
        g.drawText (loudness <= -110.0f ? juce::String ("-inf LUFS") : juce::String (loudness, 1) + " LUFS",
                    loudnessArea.withTrimmedTop (14).withHeight (28), juce::Justification::centred);
        g.setColour (colours::textDim);
        g.setFont (uiFont (8.5f));
        g.drawText ("Momentary / 400ms", loudnessArea.withTrimmedTop (42).withHeight (14), juce::Justification::centred);
    }

    // nome no rodape (como no REAPER): caixa com a cor do canal
    {
        auto nb = nameLabel.getBounds().toFloat();
        g.setColour (colours::lcd);
        g.fillRoundedRectangle (nb, 3.0f);
        g.setColour (channel.colour.withAlpha (0.28f));
        g.fillRoundedRectangle (nb, 3.0f);
        g.setColour (channel.colour);
        g.fillRoundedRectangle (nb.withHeight (2.5f), 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawRoundedRectangle (nb, 3.0f, 1.0f);
        const int grp = channel.group.load();
        if (juce::isPositiveAndBelow (grp, kMaxGroups))
        {
            // grupo: bolinha colorida com o numero, no canto do nome
            auto dot = juce::Rectangle<float> (nb.getX() + 3.0f, nb.getCentreY() - 6.5f, 13.0f, 13.0f);
            g.setColour (groupColour (grp));
            g.fillEllipse (dot);
            g.setColour (colours::ink);
            g.setFont (uiFont (9.5f, true));
            g.drawText (juce::String (grp + 1), dot, juce::Justification::centred);
        }
    }

    // escala do fader
    g.setFont (uiFont (9.0f, true));
    for (float db : { 12.0f, 6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -40.0f, kMinDb })
    {
        const float y = (float) fader.getY() + (float) fader.getPositionOfValue (db);
        g.setColour (db == 0.0f ? colours::silverTop : colours::textDim);
        g.drawHorizontalLine ((int) y, 4.0f, 22.0f);
        g.drawText (db <= kMinDb ? juce::String::fromUTF8 ("\xe2\x88\x9e") : juce::String ((int) std::abs (db)),
                    2, (int) y - 9, 20, 8, juce::Justification::centredLeft);
    }
}

juce::Colour groupColour (int group)
{
    static const juce::uint32 c[] { 0xff8cc4ff, 0xffffb74d, 0xff7be08f, 0xffff7a8a, 0xffb48cff, 0xff4dd9c9, 0xfff2e05a, 0xffff9de2 };
    return juce::Colour (c[(size_t) juce::jlimit (0, 7, group)]);
}

//==============================================================================
VcaStrip::VcaStrip (int g, StripHost& h) : group (g), host (h)
{
    auto& v = host.getEngine().vca[(size_t) group];
    nameLabel.setText ("VCA " + juce::String (group + 1), juce::dontSendNotification);
    nameLabel.setFont (uiFont (13.0f, true));
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (nameLabel);
    infoLabel.setFont (uiFont (10.0f, true));
    infoLabel.setJustificationType (juce::Justification::centred);
    infoLabel.setColour (juce::Label::textColourId, colours::textDim);
    infoLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (infoLabel);

    fader.setRange (kMinDb, 12.0, 0.1);
    fader.setSkewFactorFromMidPoint (-12.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setMouseDragSensitivity (300);
    fader.setTooltip ("VCA: soma no volume de todas as faixas do grupo (duplo clique: 0 dB)");
    fader.setValue (v.gainDb.load(), juce::dontSendNotification);
    fader.onDragStart = [this] { gesture = true; gestureStart = host.getEngine().vca[(size_t) group].gainDb.load(); };
    fader.onDragEnd = [this] { gesture = false; host.vcaEdited (group, false, gestureStart, (float) fader.getValue()); };
    fader.onValueChange = [this]
    {
        auto& vv = host.getEngine().vca[(size_t) group];
        const float before = vv.gainDb.load();
        vv.gainDb = (float) fader.getValue();
        if (! gesture) host.vcaEdited (group, false, before, (float) fader.getValue());
        refresh();
        host.markDirty();
    };
    addAndMakeVisible (fader);
    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, colours::mute);
    muteButton.setColour (juce::TextButton::textColourOnId, colours::ink);
    muteButton.setColour (juce::TextButton::buttonColourId, colours::gun);
    muteButton.setTooltip ("Mute do grupo inteiro");
    muteButton.onClick = [this]
    {
        const bool on = muteButton.getToggleState();
        host.getEngine().vca[(size_t) group].mute = on;
        host.vcaEdited (group, true, on ? 0.0f : 1.0f, on ? 1.0f : 0.0f);
        host.markDirty();
    };
    addAndMakeVisible (muteButton);
    gainLabel.setFont (uiFont (12.0f, true));
    gainLabel.setJustificationType (juce::Justification::centred);
    gainLabel.setColour (juce::Label::textColourId, colours::lcdText);
    gainLabel.setColour (juce::Label::backgroundColourId, colours::lcd);
    gainLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (gainLabel);
    refresh();
}

void VcaStrip::refresh()
{
    auto& e = host.getEngine();
    auto& v = e.vca[(size_t) group];
    if (! fader.isMouseButtonDown()) fader.setValue (v.gainDb.load(), juce::dontSendNotification);
    muteButton.setToggleState (v.mute.load(), juce::dontSendNotification);
    const float db = v.gainDb.load();
    gainLabel.setText (db <= kMinDb ? juce::String ("-inf") : (db > 0 ? "+" : "") + juce::String (db, 1), juce::dontSendNotification);
    int members = 0;
    for (auto& t : e.tracks) members += t->group.load() == group ? 1 : 0;
    for (auto& b : e.buses) members += b->group.load() == group ? 1 : 0;
    infoLabel.setText (juce::String (members) + (members == 1 ? " membro" : " membros"), juce::dontSendNotification);
}

void VcaStrip::tick() { refresh(); }

void VcaStrip::resized()
{
    const int w = getWidth(), H = getHeight();
    infoLabel.setBounds (4, 8, w - 8, 13);
    nameLabel.setBounds (4, H - 26, w - 8, 22);
    gainLabel.setBounds (10, H - 48, w - 20, 18);
    const int faderH = 232, faderTop = H - 52 - faderH;
    fader.setBounds (14, faderTop - 10, w - 28, faderH + 20);
    muteButton.setBounds (6, faderTop - 32, w - 12, 20);
}

void VcaStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.5f, 0.0f);
    SilverLookAndFeel::drawPlate (g, r, colours::gunHi.darker (0.4f), colours::gunLo.darker (0.45f), group * 31 + 7);
    const auto col = groupColour (group);
    g.setColour (col);
    g.fillRoundedRectangle (r.getX() + 4, 2.0f, r.getWidth() - 8, 4.0f, 1.5f);
    g.setColour (col.withAlpha (0.45f));
    g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 1.5f);
    auto nb = nameLabel.getBounds().toFloat();
    g.setColour (colours::lcd);
    g.fillRoundedRectangle (nb, 3.0f);
    g.setColour (col.withAlpha (0.3f));
    g.fillRoundedRectangle (nb, 3.0f);
    g.setColour (colours::textDim);
    g.setFont (uiFont (9.5f, true));
    g.drawFittedText (juce::String::fromUTF8 ("Soma no volume das faixas do grupo ")
                      + juce::String (group + 1) + ".", getLocalBounds().withTrimmedTop (30).withHeight (60).reduced (8, 0),
                      juce::Justification::centredTop, 4);
}

void VcaStrip::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) host.vcaMenu (group, *this);
}

void ChannelStrip::paintOverChildren (juce::Graphics& g)
{
    if (channel.kind == ChannelKind::track && channel.mute.load())
    {
        g.setColour (colours::mute.withAlpha (0.10f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f, 0), 4.0f);
    }
}

bool ChannelStrip::isDragArea (const juce::MouseEvent& e) const
{
    if (channel.kind == ChannelKind::master || e.mods.isPopupMenu()) return false;
    return e.eventComponent == &nameLabel || e.eventComponent == this;
}

void ChannelStrip::mouseDrag (const juce::MouseEvent& e)
{
    // arrastar pelo nome ou pelo fundo do canal muda a posicao (faixas entre faixas, buses entre buses)
    if (! isDragArea (e)) return;
    if (! draggingStrip && e.getDistanceFromDragStart() < 6) return;
    if (! draggingStrip)
    {
        draggingStrip = true;
        topClickPending = false;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        host.stripDrag (channel, e, 0);
    }
    host.stripDrag (channel, e, 1);
}

void ChannelStrip::mouseUp (const juce::MouseEvent& e)
{
    if (draggingStrip)
    {
        draggingStrip = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        host.stripDrag (channel, e, 2);
        return;
    }
    if (topClickPending)
    {
        topClickPending = false;
        host.channelMenu (channel, nameLabel);
    }
}

void ChannelStrip::mouseDown (const juce::MouseEvent& e)
{
    topClickPending = false;
    const bool onName = e.eventComponent == &nameLabel;
    const bool onStrip = e.eventComponent == this;
    if (e.mods.isPopupMenu() && (onName || onStrip)) { host.channelMenu (channel, nameLabel); return; }
    if (onStrip && e.y < 22) topClickPending = true; // menu ao soltar (se nao arrastar)
}

bool ChannelStrip::insertDropAt (juce::Point<int> screen, InsertDrop& d, juce::Rectangle<int>& marker) const
{
    const auto vb = insertsView.getScreenBounds();
    if (! vb.expanded (0, 6).contains (screen)) return false;
    const auto local = insertsContent.getLocalPoint (nullptr, screen);
    const int n = slots.size();
    auto lineAt = [&] (int pos)
    {
        const auto pt = insertsContent.localPointToGlobal (juce::Point<int> (0, pos * kSlotH - 1));
        return juce::Rectangle<int> (vb.getX(), juce::jlimit (vb.getY() - 1, vb.getBottom() - 2, pt.y - 1), vb.getWidth(), 3);
    };
    if (local.y >= n * kSlotH) { d = { n, false }; marker = lineAt (n); return true; }
    const int idx = juce::jlimit (0, juce::jmax (0, n - 1), local.y / kSlotH);
    if (n > 0 && channel.inserts[(size_t) idx].isEmpty())
    {
        d = { idx, true };
        marker = slots[idx]->getScreenBounds();
        return true;
    }
    const int pos = (local.y - idx * kSlotH) >= kSlotH / 2 ? idx + 1 : idx;
    d = { juce::jlimit (0, n, pos), false };
    marker = lineAt (d.slot);
    return true;
}

void ChannelStrip::autoScrollInserts (juce::Point<int> screen)
{
    const auto v = insertsView.getLocalPoint (nullptr, screen);
    insertsView.autoScroll (v.x, v.y, 14, 6);
}

//==============================================================================
GrMeter::GrMeter (Master& m) : master (m)
{
    setTooltip (juce::String::fromUTF8 ("Redu\xc3\xa7\xc3\xa3o de ganho do compressor (dB). Marca vermelha: maior redu\xc3\xa7\xc3\xa3o recente (clique: limpa)"));
    startTimerHz (60);
}

void GrMeter::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    const float dt = lastMs == 0 ? 1.0f / 60.0f : juce::jlimit (0.001f, 0.05f, (float) (now - lastMs) * 0.001f);
    lastMs = now;
    const bool on = master.comp.enabled.load (std::memory_order_relaxed);
    const float target = on ? master.compGr.exchange (0.0f) : 0.0f;
    if (! on) master.compGr.store (0.0f, std::memory_order_relaxed);
    // agulha com massa e mola (um pouco de "balanco", como um VU de verdade)
    constexpr float k = 260.0f, damping = 2.0f * 0.72f * 16.12f; // 16,12 = sqrt(k)
    const float before = pos;
    vel += (k * (target - pos) - damping * vel) * dt;
    pos += vel * dt;
    if (pos < 0.0f) { pos = 0.0f; vel = juce::jmax (0.0f, vel); }  // batente no 0
    if (pos > 22.0f) { pos = 22.0f; vel = juce::jmin (0.0f, vel); } // batente no fim
    if (target > peakHold) { peakHold = target; holdFrames = 90; }
    else if (--holdFrames <= 0) peakHold = juce::jmax (0.0f, peakHold - 0.25f);
    if (std::abs (pos - before) > 0.005f || holdFrames == 89 || (peakHold > 0.0f && holdFrames <= 0)) repaint();
}

void GrMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (colours::silverDark);
    g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
    const auto face = r.reduced (4.0f);
    g.setGradientFill ({ juce::Colour (0xfff7edd2), face.getCentreX(), face.getY(),
                         juce::Colour (0xffd9c48f), face.getCentreX(), face.getBottom(), false });
    g.fillRoundedRectangle (face, 3.0f);

    // Arco que ocupa a largura do mostrador: espaco para os numeros em cima e
    // para o texto embaixo; a meia-abertura sai da proporcao do mostrador.
    const float topRoom = 18.0f, bottomRoom = 11.0f;
    const float span = juce::jmax (8.0f, face.getHeight() - topRoom - bottomRoom);
    float half = 0.8f;
    for (float a = 0.25f; a <= 1.0f; a += 0.01f)
    {
        const float rr = span / (1.0f - std::cos (a));
        half = a;
        if (rr * std::sin (a) <= face.getWidth() * 0.40f) break;
    }
    const float radius = span / (1.0f - std::cos (half));
    const float cx = face.getCentreX();
    const float cy = face.getY() + topRoom + radius;
    const float a0 = -half, a1 = half; // 20 dB a esquerda, 0 dB a direita
    auto angleOf = [&] (float db) { return a1 - juce::jlimit (0.0f, 22.0f, db) / 20.0f * (a1 - a0); };
    auto at = [&] (float a, float rad) { return juce::Point<float> (cx + std::sin (a) * rad, cy - std::cos (a) * rad); };

    const juce::Colour ink (0xff2b2117);
    g.saveState();
    g.reduceClipRegion (face.toNearestInt());
    {
        juce::Path arc;
        arc.addCentredArc (cx, cy, radius, radius, 0.0f, a0, a1, true);
        g.setColour (ink);
        g.strokePath (arc, juce::PathStrokeType (1.3f));
        juce::Path red;
        red.addCentredArc (cx, cy, radius + 2.5f, radius + 2.5f, 0.0f, angleOf (20.0f), angleOf (12.0f), true);
        g.setColour (juce::Colour (0xffc0392b));
        g.strokePath (red, juce::PathStrokeType (3.0f));
    }
    g.setFont (uiFont (9.5f, true));
    for (int db = 0; db <= 20; ++db)
    {
        const bool major = db % 4 == 0;
        const float a = angleOf ((float) db);
        g.setColour (ink);
        g.drawLine ({ at (a, radius), at (a, radius + (major ? 7.0f : 4.0f)) }, major ? 1.4f : 0.9f);
        if (major)
        {
            const auto p = at (a, radius + 14.0f);
            g.drawText (juce::String (db), juce::Rectangle<float> (p.x - 10.0f, p.y - 6.0f, 20.0f, 12.0f), juce::Justification::centred);
        }
    }
    g.setColour (ink.withAlpha (0.8f));
    g.setFont (uiFont (8.0f, true));
    g.drawText ("GAIN REDUCTION  dB", face.withTrimmedTop (face.getHeight() - 15.0f), juce::Justification::centred);

    if (peakHold > 0.05f)
    {
        const float a = angleOf (peakHold);
        g.setColour (juce::Colour (0xffd0392b));
        g.drawLine ({ at (a, radius - 5.0f), at (a, radius + 3.0f) }, 2.0f);
    }
    // agulha
    {
        const float a = angleOf (pos);
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.drawLine ({ at (a, 0.0f).translated (1.5f, 1.0f), at (a, radius + 9.0f).translated (1.5f, 1.0f) }, 1.6f);
        g.setColour (juce::Colour (0xff141414));
        g.drawLine ({ at (a, 0.0f), at (a, radius + 9.0f) }, 1.5f);
    }
    g.restoreState();
    // vidro
    g.setGradientFill ({ juce::Colours::white.withAlpha (0.22f), face.getX(), face.getY(),
                         juce::Colours::white.withAlpha (0.0f), face.getX(), face.getCentreY(), false });
    g.fillRoundedRectangle (face.withHeight (face.getHeight() * 0.5f), 3.0f);
}

//==============================================================================
BusCompPanel::BusCompPanel (Master& m, StripHost& h) : master (m), host (h), meter (m)
{
    addAndMakeVisible (meter);
    inButton.setClickingTogglesState (true);
    inButton.setColour (juce::TextButton::buttonOnColourId, colours::accent);
    inButton.setColour (juce::TextButton::textColourOnId, colours::ink);
    inButton.setColour (juce::TextButton::buttonColourId, colours::gun);
    inButton.setWantsKeyboardFocus (false);
    inButton.setTooltip (juce::String::fromUTF8 ("Liga o compressor de barramento do master (depois dos inserts, antes do fader)"));
    inButton.onClick = [this]
    {
        const float before = master.comp.get (BusCompParams::idEnabled);
        master.comp.set (BusCompParams::idEnabled, inButton.getToggleState() ? 1.0f : 0.0f);
        host.compEdited (BusCompParams::idEnabled, before, master.comp.get (BusCompParams::idEnabled));
        host.markDirty();
        repaint();
    };
    addAndMakeVisible (inButton);

    struct Spec { int id; double lo, hi, step, def; int sens; const char* tip; };
    const Spec specs[] {
        { BusCompParams::idThreshold, -30.0, 10.0, 0.5, -10.0, 240, "Limiar (dB). Duplo clique: -10" },
        { BusCompParams::idRatio, 0, BusCompParams::numRatios - 1, 1, 1, 60, "Ratio: 2:1, 4:1 ou 10:1" },
        { BusCompParams::idAttack, 0, BusCompParams::numAttacks - 1, 1, 2, 80, "Ataque (ms): 0,1 a 30" },
        { BusCompParams::idRelease, 0, BusCompParams::numReleases - 1, 1, 4, 80,
          "Release (s): 0,1 a 1,2 ou AUTO (dois estagios: rapido nos transientes, lento na compressao sustentada)" },
        { BusCompParams::idMakeup, 0.0, 20.0, 0.5, 0.0, 240, "Ganho de compensacao (dB)" },
        { BusCompParams::idMix, 0.0, 1.0, 0.01, 1.0, 240, "Mix: 0% = seco, 100% = comprimido (compressao paralela)" },
        { BusCompParams::idHpf, 0, BusCompParams::numHpf - 1, 1, 0, 80, "Filtro no sidechain: o grave aciona menos o compressor (OFF, 60, 90, 150 Hz)" },
    };
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto& k = knobs[i];
        const auto& sp = specs[i];
        k.id = sp.id;
        k.slider.setRange (sp.lo, sp.hi, sp.step);
        k.slider.setDoubleClickReturnValue (true, sp.def);
        k.slider.setMouseDragSensitivity (sp.sens);
        k.slider.setColour (juce::Slider::trackColourId, colours::accent);
        k.slider.setTooltip (juce::String::fromUTF8 (sp.tip));
        k.slider.setValue (master.comp.get (k.id), juce::dontSendNotification);
        k.slider.onDragStart = [this, &k] { k.gesture = true; k.start = master.comp.get (k.id); };
        k.slider.onDragEnd = [this, &k]
        {
            k.gesture = false;
            host.compEdited (k.id, k.start, master.comp.get (k.id));
        };
        k.slider.onValueChange = [this, &k]
        {
            const float before = master.comp.get (k.id);
            master.comp.set (k.id, (float) k.slider.getValue());
            if (! k.gesture) host.compEdited (k.id, before, master.comp.get (k.id));
            host.markDirty();
            repaint();
        };
        addAndMakeVisible (k.slider);
    }
    refresh();
}

juce::String BusCompPanel::valueText (int id) const
{
    const auto& c = master.comp;
    switch (id)
    {
        case BusCompParams::idThreshold: return juce::String (c.thresholdDb.load(), 1);
        case BusCompParams::idRatio:     return BusCompParams::ratioText (c.ratio.load());
        case BusCompParams::idAttack:    return BusCompParams::attackText (c.attack.load()) + " ms";
        case BusCompParams::idRelease:   { const auto t = BusCompParams::releaseText (c.release.load()); return c.releaseS (c.release.load()) < 0 ? t : t + " s"; }
        case BusCompParams::idMakeup:    return "+" + juce::String (c.makeupDb.load(), 1);
        case BusCompParams::idMix:       return juce::String (juce::roundToInt (c.mix.load() * 100.0f)) + "%";
        case BusCompParams::idHpf:       { const auto t = BusCompParams::hpfText (c.sidechainHpf.load()); return c.sidechainHpf.load() > 0 ? t + " Hz" : t; }
        default: return {};
    }
}

void BusCompPanel::refresh()
{
    inButton.setToggleState (master.comp.enabled.load(), juce::dontSendNotification);
    for (auto& k : knobs)
        if (! k.slider.isMouseButtonDown()) k.slider.setValue (master.comp.get (k.id), juce::dontSendNotification);
    repaint();
}

void BusCompPanel::setCompact (bool c)
{
    if (compact == c) return;
    compact = c;
    for (auto& k : knobs) k.slider.setVisible (! compact);
    meter.setTooltip (compact ? juce::String::fromUTF8 ("Aumente a altura da janela para ver os controles do compressor")
                              : juce::String::fromUTF8 ("Redu\xc3\xa7\xc3\xa3o de ganho do compressor (dB)"));
    resized();
    repaint();
}

void BusCompPanel::resized()
{
    const int w = getWidth();
    inButton.setBounds (w - 40, 2, 36, 15);
    meter.setBounds (4, 19, w - 8, compact ? getHeight() - 23 : 56);
    if (compact) return;
    // 2 linhas de botoes: THRESH RATIO ATTACK RELEASE / MAKEUP MIX S/C HPF
    const int top = 78, rowH = 43;
    const int cw = (w - 8) / 4;
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        const int row = (int) i / 4, col = (int) i % 4;
        const int rowCount = row == 0 ? 4 : (int) knobs.size() - 4;
        const int x0 = 4 + (w - 8 - rowCount * cw) / 2;
        knobs[i].slider.setBounds (x0 + col * cw + (cw - 26) / 2, top + row * rowH + 9, 26, 24);
    }
}

void BusCompPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.38f));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (master.comp.enabled.load() ? colours::accent.withAlpha (0.5f) : colours::panelLine);
    g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    g.setColour (colours::silverTop);
    g.setFont (uiFont (10.5f, true));
    g.drawText ("BUS COMP", 7, 2, 100, 16, juce::Justification::centredLeft);
    if (compact) return;
    for (auto& k : knobs)
    {
        const auto b = k.slider.getBounds();
        g.setColour (colours::textDim);
        g.setFont (uiFont (8.5f, true));
        g.drawText (BusCompParams::nameOf (k.id), b.getX() - 12, b.getY() - 9, b.getWidth() + 24, 9, juce::Justification::centred);
        g.setColour (colours::lcdText);
        g.setFont (uiFont (8.5f, true));
        g.drawText (valueText (k.id), b.getX() - 12, b.getBottom(), b.getWidth() + 24, 10, juce::Justification::centred);
    }
}
} // namespace sc
