// SilverConsole - janela principal. Codigo original (MIT).
#include "MainComponent.h"
#include "AudioLoader.h"
#include <thread>

namespace sc
{
namespace
{
    constexpr int kTopBarH = 64, kToolbarH = 38;

    juce::String timeText (juce::int64 samples, double sr)
    {
        const double secs = juce::jmax (0.0, (double) samples / juce::jmax (1.0, sr));
        const int m = (int) (secs / 60.0);
        const double s = secs - m * 60.0;
        return juce::String (m).paddedLeft ('0', 2) + ":" + juce::String (s, 3).paddedLeft ('0', 6);
    }

    /** Executa fn(i) em paralelo (decodificacao/reamostragem). */
    void parallelFor (int n, Job& job, const std::function<void (int)>& fn)
    {
        std::atomic<int> next { 0 }, done { 0 };
        const int workers = juce::jlimit (1, 8, (int) std::thread::hardware_concurrency() - 1);
        std::vector<std::thread> pool;
        for (int w = 0; w < juce::jmin (workers, juce::jmax (1, n)); ++w)
            pool.emplace_back ([&]
            {
                for (;;)
                {
                    const int i = next++;
                    if (i >= n || job.cancel.load()) break;
                    fn (i);
                    job.progress = (float) (++done) / (float) n;
                }
            });
        for (auto& t : pool) t.join();
    }

    struct DecodedFile
    {
        juce::File file;
        LoadedAudio audio;
        std::shared_ptr<const juce::AudioBuffer<float>> playback;
    };

    void decodeInto (DecodedFile& d, double sr)
    {
        juce::AudioFormatManager fm;
        registerAudioFormats (fm);
        d.audio = loadAudioFile (fm, d.file);
        if (d.audio.ok()) d.playback = resampleBuffer (d.audio.original, d.audio.rate, sr);
    }
}

//==============================================================================
void IconButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = getToggleState() || (shape == Shape::play && showPause);
    auto base = on ? colours::accent : juce::Colour (0xff2a3036);
    if (down) base = base.darker (0.2f); else if (over) base = base.brighter (0.1f);
    g.setGradientFill ({ base.brighter (0.1f), 0, r.getY(), base.darker (0.15f), 0, r.getBottom(), false });
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);

    auto icon = r.withSizeKeepingCentre (14.0f, 14.0f);
    g.setColour (on ? colours::ink : colours::text);
    juce::Path p;
    switch (shape)
    {
        case Shape::toStart:
            g.fillRect (icon.withWidth (2.5f));
            p.addTriangle (icon.getRight(), icon.getY(), icon.getRight(), icon.getBottom(), icon.getX() + 3.0f, icon.getCentreY());
            break;
        case Shape::play:
            if (showPause)
            {
                g.fillRect (icon.withWidth (4.5f));
                g.fillRect (icon.withTrimmedLeft (9.5f));
            }
            else p.addTriangle (icon.getX() + 1.0f, icon.getY(), icon.getX() + 1.0f, icon.getBottom(), icon.getRight(), icon.getCentreY());
            break;
        case Shape::stop:
            g.fillRect (icon.reduced (1.0f));
            break;
        case Shape::loop:
        {
            juce::Path a;
            a.addRoundedRectangle (icon.reduced (0.0f, 2.5f), 4.0f);
            g.strokePath (a, juce::PathStrokeType (2.0f));
            p.addTriangle (icon.getCentreX() - 1, icon.getY(), icon.getCentreX() + 4, icon.getY() + 2.5f, icon.getCentreX() - 1, icon.getY() + 5);
            break;
        }
    }
    g.fillPath (p);
}

//==============================================================================
double Timeline::visibleSeconds() const
{
    const double sr = engine.getSampleRate();
    return juce::jmax (10.0, (double) engine.getLengthSamples() / sr + 2.0);
}

juce::int64 Timeline::xToSample (float x) const
{
    const auto area = getLocalBounds().toFloat().reduced (8.0f, 0.0f);
    const double t = juce::jlimit (0.0, 1.0, (double) (x - area.getX()) / area.getWidth());
    return (juce::int64) (t * visibleSeconds() * engine.getSampleRate());
}

float Timeline::sampleToX (juce::int64 s) const
{
    const auto area = getLocalBounds().toFloat().reduced (8.0f, 0.0f);
    return area.getX() + (float) ((double) s / (visibleSeconds() * engine.getSampleRate())) * area.getWidth();
}

void Timeline::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colours::lcd);
    g.fillRoundedRectangle (r, 4.0f);
    const double sr = engine.getSampleRate();
    const double total = visibleSeconds();
    const auto len = engine.getLengthSamples();

    // regiao com audio
    g.setColour (colours::panelLine.withAlpha (0.8f));
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (sampleToX (0), r.getBottom() - 7.0f, sampleToX (len), r.getBottom() - 3.0f));

    // loop
    const auto ls = engine.loopStart.load(), le = engine.loopEnd.load();
    if (le > ls)
    {
        auto lr = juce::Rectangle<float>::leftTopRightBottom (sampleToX (ls), r.getY() + 2.0f, sampleToX (le), r.getBottom() - 2.0f);
        g.setColour ((engine.loopOn.load() ? colours::accent : colours::textDim).withAlpha (0.22f));
        g.fillRect (lr);
        g.setColour (engine.loopOn.load() ? colours::accent : colours::textDim);
        g.fillRect (lr.withWidth (2.0f));
        g.fillRect (lr.withLeft (lr.getRight() - 2.0f));
    }

    // regua
    double step = 1.0;
    for (double s : { 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 })
        if (total / s < getWidth() / 70.0) { step = s; break; }
    g.setFont (uiFont (10.0f));
    for (double t = 0.0; t <= total; t += step)
    {
        const float x = sampleToX ((juce::int64) (t * sr));
        g.setColour (colours::textDim.withAlpha (0.6f));
        g.drawVerticalLine ((int) x, r.getY() + 16.0f, r.getBottom() - 8.0f);
        const int m = (int) (t / 60.0);
        g.setColour (colours::textDim);
        g.drawText (juce::String (m) + ":" + juce::String ((int) t % 60).paddedLeft ('0', 2), (int) x + 3, 2, 40, 13,
                    juce::Justification::left);
    }

    const float px = sampleToX (engine.position.load());
    g.setColour (colours::text);
    g.fillRect (px - 1.0f, r.getY() + 1.0f, 2.0f, r.getHeight() - 2.0f);
    juce::Path head;
    head.addTriangle (px - 6.0f, r.getY(), px + 6.0f, r.getY(), px, r.getY() + 7.0f);
    g.fillPath (head);
}

void Timeline::mouseDown (const juce::MouseEvent& e)
{
    loopDrag = e.mods.isShiftDown();
    dragAnchor = xToSample ((float) e.x);
    if (! loopDrag)
    {
        engine.position = dragAnchor;
        if (onEdited) onEdited();
    }
    repaint();
}

void Timeline::mouseDrag (const juce::MouseEvent& e)
{
    const auto s = xToSample ((float) e.x);
    if (loopDrag)
    {
        engine.setLoop (dragAnchor, s);
        engine.loopOn = true;
    }
    else engine.position = s;
    if (onEdited) onEdited();
    repaint();
}

//==============================================================================
BusyOverlay::BusyOverlay()
{
    message.setFont (uiFont (16.0f, true));
    message.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (message);
    addAndMakeVisible (bar);
    addAndMakeVisible (cancelButton);
    setVisible (false);
}

void BusyOverlay::show (const juce::String& m, bool cancellable)
{
    message.setText (m, juce::dontSendNotification);
    cancelButton.setVisible (cancellable);
    progress = 0.0;
    setVisible (true);
    toFront (false);
}

void BusyOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.6f));
    auto box = getLocalBounds().withSizeKeepingCentre (420, 150).toFloat();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (box, 8.0f);
    g.setColour (colours::silverDark);
    g.drawRoundedRectangle (box, 8.0f, 1.0f);
}

void BusyOverlay::resized()
{
    auto box = getLocalBounds().withSizeKeepingCentre (420, 150).reduced (20);
    message.setBounds (box.removeFromTop (34));
    box.removeFromTop (8);
    bar.setBounds (box.removeFromTop (22));
    box.removeFromTop (14);
    cancelButton.setBounds (box.removeFromTop (28).withSizeKeepingCentre (110, 28));
}

//==============================================================================
MainComponent::MainComponent (juce::ApplicationProperties& p)
    : props (p), pluginHost (*p.getUserSettings())
{
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
    setWantsKeyboardFocus (true);

    for (auto* b : { &toStartButton, &playButton, &stopButton, &loopButton })
        addAndMakeVisible (b);
    toStartButton.onClick = [this] { engine.position = 0; playStart = 0; };
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this]
    {
        if (engine.playing.load()) { engine.playing = false; engine.position = playStart; }
        else { engine.position = 0; playStart = 0; }
    };
    loopButton.setClickingTogglesState (true);
    loopButton.onClick = [this]
    {
        if (loopButton.getToggleState() && engine.loopEnd.load() <= engine.loopStart.load())
        {
            const auto len = juce::jmax (engine.getLengthSamples(), (juce::int64) (engine.getSampleRate() * 8));
            engine.setLoop (0, len);
        }
        engine.loopOn = loopButton.getToggleState();
        markDirty();
    };
    addAndMakeVisible (timeline);
    timeline.onEdited = [this] { if (! engine.playing.load()) playStart = engine.position.load(); };

    auto setupTool = [this] (juce::TextButton& b, const juce::String& tip)
    {
        b.setWantsKeyboardFocus (false);
        b.setTooltip (tip);
        addAndMakeVisible (b);
    };
    setupTool (newButton, "Novo projeto (Ctrl+N)");
    setupTool (openButton, "Abrir projeto .sconsole (Ctrl+O)");
    setupTool (saveButton, "Salvar (Ctrl+S) | Shift+clique: Salvar como");
    setupTool (importButton, "Importar arquivos de audio como faixas (Ctrl+I). Tambem da para arrastar arquivos para a janela.");
    setupTool (addBusButton, "Criar bus auxiliar (reverb, delay, grupo)");
    setupTool (exportButton, "Exportar a mix para WAV (Ctrl+E)");
    setupTool (pluginsButton, "Escanear e gerenciar plugins VST3");
    setupTool (audioButton, "Configurar a placa de som (saida e buffer)");
    setupTool (automationButton, juce::String::fromUTF8 ("Editor de automa\xc3\xa7\xc3\xa3o (Tab). Clique de novo para voltar ao mixer."));
    automationButton.setClickingTogglesState (true);
    automationButton.onClick = [this] { setAutomationVisible (automationButton.getToggleState()); };
    exportButton.setColour (juce::TextButton::buttonColourId, colours::accent.darker (0.35f));

    newButton.onClick = [this] { newProject(); };
    openButton.onClick = [this] { openProjectDialog(); };
    saveButton.onClick = [this]
    {
        if (juce::ModifierKeys::currentModifiers.isShiftDown()) saveAs();
        else save();
    };
    importButton.onClick = [this] { importDialog(); };
    addBusButton.onClick = [this] { if (createBus() == nullptr) alert ("Bus", "Limite de " + juce::String (kMaxBuses) + " buses."); };
    exportButton.onClick = [this] { exportDialog(); };
    pluginsButton.onClick = [this] { showPluginManager (false); };
    audioButton.onClick = [this] { showAudioSettings(); };

    mixerViewport.setViewedComponent (&mixerContent, false);
    mixerViewport.setScrollBarsShown (true, true);
    mixerViewport.setScrollBarThickness (10);
    addAndMakeVisible (mixerViewport);
    masterStrip = std::make_unique<ChannelStrip> (engine.master, *this);
    addAndMakeVisible (*masterStrip);

    automationView = std::make_unique<AutomationView> (engine, *this);
    addChildComponent (*automationView);
    autoTimer.fn = [this] { recordTick(); };
    autoTimer.startTimerHz (50);

    overlay.cancelButton.onClick = [this] { if (job != nullptr) job->cancel = true; };
    addChildComponent (overlay);

    // placa de som (sem entradas: o programa so mixa)
    auto saved = props.getUserSettings()->getXmlValue ("audioDevice");
    const auto err = deviceManager.initialise (0, 2, saved.get(), true);
    if (err.isNotEmpty()) statusText = "Audio: " + err;
    deviceManager.addAudioCallback (&engine);
    deviceManager.addChangeListener (this);

    setSize (1440, 900);
    updateTitle();
    startTimerHz (30);

}

MainComponent::~MainComponent()
{
    stopTimer();
    autoTimer.stopTimer();
    if (job != nullptr) { job->cancel = true; job->stopThread (60000); job.reset(); }
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (&engine);
    deviceManager.closeAudioDevice();
    closeAllEditors();
    pluginManagerWindow.reset();
    automationView.reset();
    strips.clear();
    masterStrip.reset();
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    auto top = getLocalBounds().removeFromTop (kTopBarH);
    g.setGradientFill ({ colours::panel.brighter (0.05f), 0, 0, colours::panel.darker (0.2f), 0, (float) kTopBarH, false });
    g.fillRect (top);
    g.setColour (colours::panelLine);
    g.drawHorizontalLine (kTopBarH - 1, 0, (float) getWidth());
    g.drawHorizontalLine (kTopBarH + kToolbarH - 1, 0, (float) getWidth());

    // logo
    g.setColour (colours::silverTop);
    g.setFont (uiFont (21.0f, true));
    g.drawText ("SILVER", 16, 10, 90, 26, juce::Justification::left);
    g.setColour (colours::accent);
    g.drawText ("CONSOLE", 96, 10, 110, 26, juce::Justification::left);
    g.setColour (colours::textDim);
    g.setFont (uiFont (10.5f, true));
    g.drawText ("MIX  /  VST3 HOST", 17, 36, 180, 14, juce::Justification::left);

    // relogio
    auto clock = juce::Rectangle<int> (382, 12, 150, 40);
    g.setColour (colours::lcd);
    g.fillRoundedRectangle (clock.toFloat(), 4.0f);
    g.setColour (colours::lcdText);
    g.setFont (uiFont (22.0f, true));
    g.drawText (timeText (engine.position.load(), engine.getSampleRate()), clock, juce::Justification::centred);

    // status
    g.setColour (colours::textDim);
    g.setFont (uiFont (11.5f));
    g.drawText (statusText, getWidth() - 470, kTopBarH + 4, 458, kToolbarH - 8, juce::Justification::centredRight);

    if (engine.tracks.empty())
    {
        auto area = mixerViewport.getBounds().reduced (40);
        g.setColour (colours::textDim.withAlpha (0.7f));
        g.setFont (uiFont (18.0f, true));
        g.drawFittedText (juce::String::fromUTF8 ("Arraste arquivos de \xc3\xa1udio (WAV, AIFF, FLAC, OGG, MP3) para c\xc3\xa1\n"
                                                  "ou clique em  Importar \xc3\xa1udio.\n\n"
                                                  "Cada arquivo vira uma faixa. Clique num slot de INSERT para colocar um plugin VST3."),
                          area, juce::Justification::centred, 5);
    }
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (kTopBarH).reduced (12, 12);
    top.removeFromLeft (210);
    for (auto* b : { &toStartButton, &playButton, &stopButton, &loopButton })
    {
        b->setBounds (top.removeFromLeft (36).withSizeKeepingCentre (36, 34));
        top.removeFromLeft (4);
    }
    top.removeFromLeft (166); // relogio
    timeline.setBounds (top);

    auto tools = r.removeFromTop (kToolbarH).reduced (12, 5);
    for (auto* b : { &newButton, &openButton, &saveButton })
    {
        b->setBounds (tools.removeFromLeft (70));
        tools.removeFromLeft (4);
    }
    tools.removeFromLeft (14);
    for (auto* b : { &importButton, &addBusButton })
    {
        b->setBounds (tools.removeFromLeft (b == &importButton ? 124 : 70));
        tools.removeFromLeft (4);
    }
    tools.removeFromLeft (14);
    exportButton.setBounds (tools.removeFromLeft (110));
    tools.removeFromLeft (18);
    pluginsButton.setBounds (tools.removeFromLeft (80));
    tools.removeFromLeft (4);
    audioButton.setBounds (tools.removeFromLeft (70));
    tools.removeFromLeft (18);
    automationButton.setBounds (tools.removeFromLeft (100));

    automationView->setBounds (r);
    r.reduce (8, 8);
    masterStrip->setBounds (r.removeFromRight (ChannelStrip::masterWidth).withTrimmedBottom (10));
    r.removeFromRight (10);
    mixerViewport.setBounds (r);

    const int h = juce::jmax (660, mixerViewport.getHeight() - 12);
    int x = 0;
    for (auto* s : strips)
    {
        if (s->channel.kind == ChannelKind::bus && x > 0 && strips.indexOf (s) > 0
            && strips[strips.indexOf (s) - 1]->channel.kind == ChannelKind::track)
            x += 12; // separa faixas e buses
        s->setBounds (x, 0, ChannelStrip::width, h);
        x += ChannelStrip::width + 3;
    }
    mixerContent.setSize (juce::jmax (x, 10), h);
    if (masterStrip->getHeight() < 660) masterStrip->setSize (masterStrip->getWidth(), 660);
    overlay.setBounds (getLocalBounds());
}

//==============================================================================
void MainComponent::timerCallback()
{
    ++tickCount;
    if (job != nullptr)
    {
        overlay.progress = job->progress.load();
        if (! job->isThreadRunning())
        {
            auto finished = std::move (job);
            auto done = std::move (jobDone);
            overlay.setVisible (false);
            if (done) done (*finished);
            finished.reset();
            grabKeyboardFocus();
        }
        return;
    }

    engine.pollLatencyChanges();

    // Primeiro uso: oferece escanear os plugins (sem sobrepor outros avisos).
    if (! scanPromptShown && tickCount > 15 && pluginHost.known.getNumTypes() == 0
        && juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0 && pluginManagerWindow == nullptr)
    {
        scanPromptShown = true;
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Plugins VST3")
                                          .withMessage ("Nenhum plugin foi escaneado ainda.\nEscanear agora a pasta padrao de VST3?\n"
                                                        "(A lista fica salva; depois use Plugins para procurar novos.)")
                                          .withButton ("Escanear")
                                          .withButton ("Depois"),
                                      [sp = juce::Component::SafePointer<MainComponent> (this)] (int r)
                                      { if (sp != nullptr && r == 1) sp->showPluginManager (true); });
    }
    if (engine.sampleRateChanged() || tickCount % 30 == 0) startResampleIfNeeded();

    for (auto* s : strips) s->tick();
    masterStrip->tick();
    if (showAutomation) automationView->tick();
    timeline.repaint();
    repaint (382, 12, 150, 40);
    playButton.showPause = engine.playing.load();
    playButton.repaint();
    if (loopButton.getToggleState() != engine.loopOn.load())
        loopButton.setToggleState (engine.loopOn.load(), juce::dontSendNotification);

    if (tickCount % 15 == 0)
    {
        juce::String s;
        if (auto* d = deviceManager.getCurrentAudioDevice())
        {
            const double sr = d->getCurrentSampleRate();
            const int bs = d->getCurrentBufferSizeSamples();
            s << d->getTypeName() << ": " << d->getName() << "  |  " << juce::String (sr / 1000.0, 1) << " kHz  |  "
              << bs << " amostras (" << juce::String (bs * 1000.0 / sr, 1) << " ms)  |  CPU "
              << juce::roundToInt (deviceManager.getCpuUsage() * 100.0) << "%  |  PDC "
              << juce::String (engine.getTotalLatency() * 1000.0 / sr, 1) << " ms";
            const auto rt = engine.getRealtimeStats();
            s << "  |  XRUN " << juce::String (rt.callbackOverruns)
              << "  |  LOCK " << juce::String (rt.lockContentions);
        }
        else s = "Sem dispositivo de audio - clique em Audio";
        if (s != statusText) { statusText = s; repaint (getWidth() - 470, kTopBarH, 470, kToolbarH); }
    }
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (auto xml = deviceManager.createStateXml())
    {
        props.getUserSettings()->setValue ("audioDevice", xml.get());
        props.getUserSettings()->saveIfNeeded();
    }
    refreshStrips();
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    if (job != nullptr) return true;
    const auto cmd = juce::ModifierKeys::commandModifier;
    if (k == juce::KeyPress::spaceKey) { togglePlay(); return true; }
    if (k == juce::KeyPress::homeKey) { engine.position = 0; playStart = 0; return true; }
    if (k == juce::KeyPress ('l')) { loopButton.triggerClick(); return true; }
    if (k == juce::KeyPress::tabKey) { automationButton.triggerClick(); return true; }
    if (k == juce::KeyPress ('s', cmd, 0)) { save(); return true; }
    if (k == juce::KeyPress ('s', cmd | juce::ModifierKeys::shiftModifier, 0)) { saveAs(); return true; }
    if (k == juce::KeyPress ('o', cmd, 0)) { openProjectDialog(); return true; }
    if (k == juce::KeyPress ('n', cmd, 0)) { newProject(); return true; }
    if (k == juce::KeyPress ('i', cmd, 0)) { importDialog(); return true; }
    if (k == juce::KeyPress ('e', cmd, 0)) { exportDialog(); return true; }
    return false;
}

void MainComponent::togglePlay()
{
    if (engine.playing.load()) engine.playing = false;
    else
    {
        playStart = engine.position.load();
        engine.playing = true;
    }
}

void MainComponent::setAutomationVisible (bool show)
{
    showAutomation = show;
    automationButton.setToggleState (show, juce::dontSendNotification);
    automationView->setVisible (show);
    mixerViewport.setVisible (! show);
    masterStrip->setVisible (! show);
    if (show) { automationView->rebuild(); automationView->toFront (false); }
    overlay.toFront (false);
    repaint();
}

void MainComponent::recordTick()
{
    if (job != nullptr) return;
    recorder.process ((double) engine.position.load() / engine.getSampleRate(), engine.playing.load(),
                      juce::Time::getMillisecondCounterHiRes());
}

void MainComponent::automationTouch (Channel& c, AutoTarget tg, bool down)
{
    recorder.touch (c, tg, -1, -1, down, juce::Time::getMillisecondCounterHiRes());
}

void MainComponent::muteToggled (Channel& c)
{
    const auto mode = c.getAutoMode();
    if (engine.playing.load() && (mode == AutoMode::touch || mode == AutoMode::latch))
        recorder.recordMuteStep (c, (double) engine.position.load() / engine.getSampleRate());
}

void MainComponent::autoModeMenu (Channel& c, juce::Component& source)
{
    juce::PopupMenu m;
    m.addSectionHeader (juce::String::fromUTF8 ("Automa\xc3\xa7\xc3\xa3o - ") + c.name);
    const juce::String tips[] { "desligada (ignora as linhas)",
                                juce::String::fromUTF8 ("toca a automa\xc3\xa7\xc3\xa3o"),
                                juce::String::fromUTF8 ("grava enquanto voc\xc3\xaa segura o controle"),
                                juce::String::fromUTF8 ("grava do toque at\xc3\xa9 parar"),
                                "grava tudo enquanto toca" };
    for (int i = 0; i <= (int) AutoMode::write; ++i)
        m.addColouredItem (1 + i, autoModeName ((AutoMode) i) + "  -  " + tips[i], autoModeColour ((AutoMode) i), true, (int) c.getAutoMode() == i);
    auto* cp = &c;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source), [this, cp] (int r)
    {
        if (r == 0 || ! channelExists (cp)) return;
        cp->autoMode = r - 1;
        if (auto* s = stripFor (cp)) s->refresh();
        if (automationView != nullptr) automationView->repaint();
        markDirty();
    });
}

void MainComponent::lanesChanged()
{
    recorder.reset();
    if (automationView != nullptr) automationView->rebuild();
    markDirty();
}

void MainComponent::markDirty()
{
    if (! dirty) { dirty = true; updateTitle(); }
}

void MainComponent::updateTitle()
{
    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
        w->setName ("SilverConsole - " + (currentFile == juce::File() ? juce::String ("Sem titulo") : currentFile.getFileNameWithoutExtension())
                    + (dirty ? " *" : ""));
}

void MainComponent::alert (const juce::String& title, const juce::String& text, bool warning)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (warning ? juce::MessageBoxIconType::WarningIcon : juce::MessageBoxIconType::InfoIcon)
                                      .withTitle (title).withMessage (text).withButton ("OK"),
                                  nullptr);
}

void MainComponent::runJob (const juce::String& title, bool cancellable, std::function<void (Job&)> work, std::function<void (Job&)> done)
{
    if (job != nullptr) return;
    engine.playing = false;
    recorder.process ((double) engine.position.load() / engine.getSampleRate(), false, juce::Time::getMillisecondCounterHiRes());
    job = std::make_unique<Job> (std::move (work));
    jobDone = std::move (done);
    overlay.show (title, cancellable);
    job->startThread();
}

//==============================================================================
// Estrutura do mixer
void MainComponent::rebuildStrips()
{
    recorder.reset();
    strips.clear();
    for (auto& t : engine.tracks) mixerContent.addAndMakeVisible (strips.add (new ChannelStrip (*t, *this)));
    for (auto& b : engine.buses) mixerContent.addAndMakeVisible (strips.add (new ChannelStrip (*b, *this)));
    masterStrip->refresh();
    if (automationView != nullptr) automationView->rebuild();
    resized();
    repaint();
}

void MainComponent::refreshStrips()
{
    for (auto* s : strips) s->refresh();
    masterStrip->refresh();
}

ChannelStrip* MainComponent::stripFor (const Channel* c)
{
    if (c == &engine.master) return masterStrip.get();
    for (auto* s : strips) if (&s->channel == c) return s;
    return nullptr;
}

bool MainComponent::channelExists (const Channel* c) const
{
    if (c == &engine.master) return true;
    for (auto& t : engine.tracks) if (t.get() == c) return true;
    for (auto& b : engine.buses) if (b.get() == c) return true;
    return false;
}

Bus* MainComponent::createBus()
{
    auto* b = engine.addBus();
    if (b != nullptr) { rebuildStrips(); markDirty(); }
    return b;
}

void MainComponent::removeChannel (Channel& c)
{
    closeEditorsFor (c);
    if (c.kind == ChannelKind::track)
    {
        auto removed = engine.removeTrack (static_cast<Track*> (&c));
        rebuildStrips();
    }
    else if (c.kind == ChannelKind::bus)
    {
        auto removed = engine.removeBus (static_cast<Bus*> (&c));
        rebuildStrips();
        refreshStrips();
    }
    markDirty();
}

void MainComponent::channelMenu (Channel& c, juce::Component& source)
{
    if (c.kind == ChannelKind::master) return;
    juce::PopupMenu m, colourMenu;
    m.addSectionHeader (c.name);
    m.addItem (1, "Renomear");
    for (int i = 0; i < 10; ++i) colourMenu.addColouredItem (100 + i, "Cor " + juce::String (i + 1), colours::trackPalette (i), true, c.colour == colours::trackPalette (i));
    m.addSubMenu ("Cor", colourMenu);
    if (c.kind == ChannelKind::track)
    {
        m.addItem (2, juce::String::fromUTF8 ("Trocar arquivo de \xc3\xa1udio..."));
        m.addSeparator();
        m.addItem (3, "Mover para a esquerda");
        m.addItem (4, "Mover para a direita");
    }
    m.addSeparator();
    m.addItem (9, c.kind == ChannelKind::track ? "Remover faixa" : "Remover bus");

    auto* cp = &c;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source),
                     [this, cp] (int r)
    {
        if (r == 0 || ! channelExists (cp)) return;
        if (r == 1)
        {
            if (auto* s = stripFor (cp))
                for (auto* ch : s->getChildren())
                    if (auto* l = dynamic_cast<juce::Label*> (ch); l != nullptr && l->getText() == cp->name) { l->showEditor(); break; }
        }
        else if (r >= 100) { cp->colour = colours::trackPalette (r - 100); if (auto* s = stripFor (cp)) s->repaint(); markDirty(); }
        else if (r == 2) replaceAudio (*static_cast<Track*> (cp));
        else if (r == 3 || r == 4)
        {
            for (int i = 0; i < (int) engine.tracks.size(); ++i)
                if (engine.tracks[(size_t) i].get() == cp) { engine.moveTrack (i, i + (r == 3 ? -1 : 1)); break; }
            rebuildStrips();
            markDirty();
        }
        else if (r == 9) removeChannel (*cp);
    });
}

void MainComponent::sendMenu (Track& t, int send, juce::Component& source)
{
    auto& s = t.sends[(size_t) send];
    juce::PopupMenu m;
    m.addSectionHeader ("Envio " + juce::String (send + 1) + " - " + t.name);
    m.addItem (1, "Nenhum", true, s.bus.load() < 0);
    for (int i = 0; i < (int) engine.buses.size(); ++i)
        m.addItem (10 + i, engine.buses[(size_t) i]->name, true, s.bus.load() == i);
    m.addItem (2, "+ Criar novo bus", (int) engine.buses.size() < kMaxBuses);
    m.addSeparator();
    m.addItem (3, "Pre-fader (independe do volume da faixa)", s.bus.load() >= 0, s.preFader.load());

    auto* tp = &t;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source), [this, tp, send] (int r)
    {
        if (r == 0 || ! channelExists (tp)) return;
        auto& snd = tp->sends[(size_t) send];
        if (r == 1) snd.bus = -1;
        else if (r == 2) { if (auto* b = createBus()) snd.bus = engine.indexOfBus (b); }
        else if (r == 3) snd.preFader = ! snd.preFader.load();
        else snd.bus = r - 10;
        refreshStrips();
        markDirty();
    });
}

void MainComponent::outputMenu (Track& t, juce::Component& source)
{
    juce::PopupMenu m;
    m.addSectionHeader ("Saida - " + t.name);
    m.addItem (1, "MASTER", true, t.outputBus.load() < 0);
    for (int i = 0; i < (int) engine.buses.size(); ++i)
        m.addItem (10 + i, engine.buses[(size_t) i]->name, true, t.outputBus.load() == i);
    m.addItem (2, "+ Criar novo bus", (int) engine.buses.size() < kMaxBuses);
    auto* tp = &t;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source), [this, tp] (int r)
    {
        if (r == 0 || ! channelExists (tp)) return;
        if (r == 1) tp->outputBus = -1;
        else if (r == 2) { if (auto* b = createBus()) tp->outputBus = engine.indexOfBus (b); }
        else tp->outputBus = r - 10;
        refreshStrips();
        markDirty();
    });
}

//==============================================================================
// Plugins
void MainComponent::insertSlotClicked (Channel& c, int slot, juce::Component& source, const juce::MouseEvent& e)
{
    auto& s = c.inserts[(size_t) slot];
    if (s.isEmpty()) { showPluginMenu (c, slot, source, false); return; }
    if (e.mods.isShiftDown() && ! e.mods.isPopupMenu())
    {
        engine.setBypass (c, slot, ! s.bypassed.load());
        source.repaint();
        markDirty();
        return;
    }
    if (! e.mods.isPopupMenu() && s.processor != nullptr) { openEditor (c, slot); return; }

    juce::PopupMenu m;
    m.addSectionHeader (s.getName() + (s.isMissing() ? juce::String::fromUTF8 (" (n\xc3\xa3o encontrado)") : juce::String()));
    m.addItem (1, "Abrir editor", s.processor != nullptr);
    m.addItem (2, "Bypass", s.processor != nullptr, s.bypassed.load());
    m.addSeparator();
    m.addItem (3, "Substituir...");
    m.addItem (4, "Mover para cima", slot > 0);
    m.addItem (5, "Mover para baixo", slot < kNumInserts - 1);
    m.addSeparator();
    m.addItem (9, "Remover");
    auto* cp = &c;
    juce::Component::SafePointer<juce::Component> src (&source);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source), [this, cp, slot, src] (int r)
    {
        if (r == 0 || ! channelExists (cp)) return;
        auto& sl = cp->inserts[(size_t) slot];
        if (r == 1) openEditor (*cp, slot);
        else if (r == 2) engine.setBypass (*cp, slot, ! sl.bypassed.load());
        else if (r == 3) { if (src != nullptr) showPluginMenu (*cp, slot, *src, true); }
        else if (r == 4 || r == 5) { engine.swapInserts (*cp, slot, slot + (r == 4 ? -1 : 1)); lanesChanged(); }
        else if (r == 9)
        {
            closeEditor (*cp, slot);
            auto old = engine.clearInsert (*cp, slot);
            engine.removeLanesForSlot (*cp, slot);
            lanesChanged();
        }
        if (auto* st = stripFor (cp)) st->refresh();
        markDirty();
    });
}

void MainComponent::showPluginMenu (Channel& c, int slot, juce::Component& source, bool replacing)
{
    const auto types = pluginHost.getEffects();
    juce::PopupMenu m;
    m.addSectionHeader ((replacing ? "Substituir - " : "Insert " + juce::String (slot + 1) + " - ") + c.name);
    if (types.isEmpty()) m.addItem (-2, "Nenhum plugin escaneado", false);
    else juce::KnownPluginList::addToMenu (m, types, juce::KnownPluginList::sortByManufacturer);
    m.addSeparator();
    m.addItem (-1, "Escanear / gerenciar plugins...");
    auto* cp = &c;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source).withMaximumNumColumns (2),
                     [this, cp, slot, types] (int r)
    {
        if (r == 0 || ! channelExists (cp)) return;
        if (r == -1) { showPluginManager (false); return; }
        const int idx = juce::KnownPluginList::getIndexChosenByMenu (types, r);
        if (juce::isPositiveAndBelow (idx, types.size())) loadPluginInto (*cp, slot, types.getReference (idx));
    });
}

void MainComponent::loadPluginInto (Channel& c, int slot, const juce::PluginDescription& d)
{
    juce::String err;
    juce::MouseCursor::showWaitCursor();
    auto inst = pluginHost.create (d, engine.getSampleRate(), engine.getBlockSize(), err);
    juce::String prepErr = inst != nullptr ? engine.prepareProcessor (*inst) : juce::String();
    juce::MouseCursor::hideWaitCursor();
    if (inst == nullptr || prepErr.isNotEmpty())
    {
        alert ("Plugin", "Nao foi possivel carregar " + d.name + ".\n" + (err.isNotEmpty() ? err : prepErr));
        return;
    }
    closeEditor (c, slot);
    const bool hadPlugin = c.inserts[(size_t) slot].processor != nullptr || c.inserts[(size_t) slot].isMissing();
    auto old = engine.setInsert (c, slot, std::move (inst), &d);
    old.reset();
    if (hadPlugin) { engine.removeLanesForSlot (c, slot); lanesChanged(); } // parametros do plugin antigo nao valem mais
    if (auto* s = stripFor (&c)) s->refresh();
    markDirty();
    openEditor (c, slot);
}

void MainComponent::loadInserts (Channel& c, const ChannelData& data, juce::StringArray& problems)
{
    for (auto& in : data.inserts)
    {
        juce::String err;
        auto inst = pluginHost.create (in.desc, engine.getSampleRate(), engine.getBlockSize(), err);
        if (inst != nullptr)
        {
            if (in.state.getSize() > 0) inst->setStateInformation (in.state.getData(), (int) in.state.getSize());
            err = engine.prepareProcessor (*inst);
            if (err.isEmpty())
            {
                engine.setInsert (c, in.slot, std::move (inst), &in.desc);
                if (in.bypass) engine.setBypass (c, in.slot, true);
                continue;
            }
        }
        engine.setMissingInsert (c, in.slot, in.desc, in.state);
        problems.add (c.name + ": plugin " + in.desc.name + (err.isNotEmpty() ? " (" + err + ")" : juce::String()));
    }
}

void MainComponent::openEditor (Channel& c, int slot)
{
    auto* p = c.inserts[(size_t) slot].processor.get();
    if (p == nullptr) return;
    for (auto& w : pluginWindows)
        if (&w->processor == p) { w->setVisible (true); w->toFront (true); return; }

    auto* cp = &c;
    juce::Component::SafePointer<MainComponent> self (this);
    pluginWindows.push_back (std::make_unique<PluginWindow> (*p, c.name + " - " + p->getName(), [self, cp] (PluginWindow* w)
    {
        w->setVisible (false);
        juce::MessageManager::callAsync ([sp = self, w, cp]
        {
            if (sp == nullptr) return;
            auto& v = sp->pluginWindows;
            for (auto it = v.begin(); it != v.end(); ++it)
                if (it->get() == w) { v.erase (it); break; }
            if (auto* s = sp->stripFor (cp)) s->refresh();
        });
    }));
    if (auto* s = stripFor (&c)) s->refresh();
}

void MainComponent::closeEditor (Channel& c, int slot)
{
    auto* p = c.inserts[(size_t) slot].processor.get();
    if (p == nullptr) return;
    for (auto it = pluginWindows.begin(); it != pluginWindows.end(); ++it)
        if (&(*it)->processor == p) { pluginWindows.erase (it); break; }
}

void MainComponent::closeEditorsFor (Channel& c)
{
    for (int i = 0; i < kNumInserts; ++i) closeEditor (c, i);
}

void MainComponent::closeAllEditors() { pluginWindows.clear(); }

bool MainComponent::isEditorOpen (const Channel& c, int slot) const
{
    auto* p = c.inserts[(size_t) slot].processor.get();
    if (p == nullptr) return false;
    for (auto& w : pluginWindows)
        if (&w->processor == p && w->isVisible()) return true;
    return false;
}

namespace
{
    class PluginManagerWindow : public juce::DocumentWindow
    {
    public:
        PluginManagerWindow (PluginHost& host, juce::PropertiesFile* settings, std::function<void()> closeFn)
            : DocumentWindow ("Plugins VST3", colours::background, juce::DocumentWindow::closeButton), onClose (std::move (closeFn))
        {
            setUsingNativeTitleBar (true);
            list = new juce::PluginListComponent (host.formats, host.known, host.getDeadMansPedalFile(), settings, true);
            list->setOptionsButtonText (juce::String::fromUTF8 ("Op\xc3\xa7\xc3\xb5\x65s / Escanear..."));
            list->setScanDialogText ("Escaneando plugins", "Procurando plugins VST3...");
            setContentOwned (list, false);
            setResizable (true, false);
            centreWithSize (760, 520);
            setVisible (true);
        }
        void closeButtonPressed() override { if (onClose) onClose(); }
        juce::PluginListComponent* list = nullptr;

    private:
        std::function<void()> onClose;
    };
}

void MainComponent::showPluginManager (bool startScan)
{
    if (pluginManagerWindow == nullptr)
    {
        juce::Component::SafePointer<MainComponent> self (this);
        pluginManagerWindow = std::make_unique<PluginManagerWindow> (pluginHost, props.getUserSettings(), [self]
        {
            juce::MessageManager::callAsync ([sp = self]
            {
                if (sp != nullptr) sp->pluginManagerWindow.reset();
            });
        });
    }
    pluginManagerWindow->toFront (true);
    if (startScan)
        if (auto* f = pluginHost.findVst3Format())
            static_cast<PluginManagerWindow*> (pluginManagerWindow.get())->list->scanFor (*f);
}

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (deviceManager, 0, 0, 2, 2, false, false, true, false);
    selector->setSize (560, 420);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = juce::String::fromUTF8 ("Configura\xc3\xa7\xc3\xb5\x65s de \xc3\xa1udio");
    o.dialogBackgroundColour = colours::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

//==============================================================================
// Audio
void MainComponent::importDialog()
{
    chooser = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Importar \xc3\xa1udio"),
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory), audioWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc) { importFiles (fc.getResults()); });
}

void MainComponent::importFiles (const juce::Array<juce::File>& files)
{
    if (files.isEmpty() || job != nullptr) return;
    const int room = kMaxTracks - (int) engine.tracks.size();
    if (room <= 0) { alert ("Importar", "Limite de " + juce::String (kMaxTracks) + " faixas."); return; }

    auto items = std::make_shared<std::vector<DecodedFile>>();
    for (auto& f : files)
        if ((int) items->size() < room) items->push_back ({ f, {}, {} });
    const double sr = engine.getSampleRate();

    runJob (juce::String::fromUTF8 ("Importando ") + juce::String ((int) items->size()) + " arquivo(s)...", true,
            [items, sr] (Job& j) { parallelFor ((int) items->size(), j, [&] (int i) { decodeInto ((*items)[(size_t) i], sr); }); },
            [this, items, sr] (Job& j)
    {
        juce::StringArray errors;
        for (auto& d : *items)
        {
            if (! d.audio.ok()) { if (d.audio.error.isNotEmpty()) errors.add (d.audio.error); continue; }
            auto t = std::make_unique<Track>();
            t->name = d.file.getFileNameWithoutExtension().substring (0, 40);
            t->colour = colours::trackPalette ((int) engine.tracks.size());
            t->sourceFile = d.file;
            auto* raw = engine.addTrack (std::move (t));
            engine.setTrackAudio (*raw, d.audio.original, d.audio.rate, d.playback, sr);
        }
        rebuildStrips();
        markDirty();
        if (j.cancel.load()) errors.add ("Importacao cancelada: alguns arquivos nao foram carregados.");
        if (! errors.isEmpty()) alert ("Importar", errors.joinIntoString ("\n"));
    });
}

void MainComponent::replaceAudio (Track& t)
{
    chooser = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Trocar \xc3\xa1udio - ") + t.name,
                                                   t.sourceFile.getParentDirectory(), audioWildcard());
    auto* tp = &t;
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, tp] (const juce::FileChooser& fc)
    {
        if (fc.getResult() == juce::File() || ! channelExists (tp)) return;
        auto item = std::make_shared<DecodedFile> (DecodedFile { fc.getResult(), {}, {} });
        const double sr = engine.getSampleRate();
        runJob (juce::String::fromUTF8 ("Carregando \xc3\xa1udio..."), false,
                [item, sr] (Job& j) { decodeInto (*item, sr); j.progress = 1.0f; },
                [this, item, tp, sr] (Job&)
        {
            if (! channelExists (tp)) return;
            if (! item->audio.ok()) { alert ("Audio", item->audio.error); return; }
            tp->sourceFile = item->file;
            engine.setTrackAudio (*tp, item->audio.original, item->audio.rate, item->playback, sr);
            refreshStrips();
            markDirty();
        });
    });
}

void MainComponent::startResampleIfNeeded()
{
    if (job != nullptr) return;
    const double sr = engine.getSampleRate();
    struct Item { Track* track; std::shared_ptr<const juce::AudioBuffer<float>> original, result; double rate; };
    auto items = std::make_shared<std::vector<Item>>();
    for (auto& t : engine.tracks)
        if (t->original != nullptr && t->playbackRate != sr)
            items->push_back ({ t.get(), t->original, nullptr, t->originalRate });
    if (items->empty()) return;

    runJob ("Ajustando a taxa de amostragem (" + juce::String (sr / 1000.0, 1) + " kHz)...", false,
            [items, sr] (Job& j) { parallelFor ((int) items->size(), j, [&] (int i) { auto& it = (*items)[(size_t) i]; it.result = resampleBuffer (it.original, it.rate, sr); }); },
            [this, items, sr] (Job&)
    {
        if (sr != engine.getSampleRate()) return; // mudou de novo: o timer refaz
        for (auto& it : *items)
            if (channelExists (it.track) && it.result != nullptr)
                engine.setTrackAudio (*it.track, it.original, it.rate, it.result, sr);
        refreshStrips();
    });
}

void MainComponent::exportDialog()
{
    if (engine.tracks.empty()) { alert ("Exportar", "Importe alguma faixa antes de exportar."); return; }
    juce::PopupMenu m;
    m.addSectionHeader ("Exportar mix (" + juce::String (engine.getSampleRate() / 1000.0, 1) + " kHz, estereo)");
    m.addItem (24, "WAV 24 bits");
    m.addItem (16, "WAV 16 bits");
    m.addItem (32, "WAV 32 bits float");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton), [this] (int bits)
    {
        if (bits == 0) return;
        auto start = currentFile != juce::File() ? currentFile.withFileExtension (".wav")
                                                 : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Mix.wav");
        chooser = std::make_unique<juce::FileChooser> ("Exportar mix", start, "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, bits] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File()) return;
            file = file.withFileExtension (".wav");
            struct Result { bool ok = false, clipped = false; juce::String error; };
            auto res = std::make_shared<Result>();
            closeAllEditors(); // evita repintar editores durante o render offline
            runJob ("Exportando " + file.getFileName() + "...", true,
                    [this, res, file, bits] (Job& j) { res->ok = engine.exportMix (file, bits, 3.0, j.progress, j.cancel, res->error, res->clipped); },
                    [this, res, file] (Job&)
            {
                refreshStrips();
                if (! res->ok) { alert ("Exportar", res->error); return; }
                juce::String msg = "Mix exportada:\n" + file.getFullPathName();
                if (res->clipped) msg << "\n\nAtencao: a mix passou de 0 dBFS (clipping). Abaixe o master ou use um limitador.";
                alert ("Exportar", msg, res->clipped);
            });
        });
    });
}

//==============================================================================
// Projeto
void MainComponent::resetSession()
{
    closeAllEditors();
    std::vector<std::unique_ptr<juce::AudioProcessor>> procs;
    std::vector<std::unique_ptr<Track>> oldTracks;
    std::vector<std::unique_ptr<Bus>> oldBuses;
    recorder.reset();
    engine.clearSession (procs, oldTracks, oldBuses);
    engine.master.autoMode = (int) AutoMode::read;
    engine.editAutomation ([this] { engine.master.lanes.clear(); });
    strips.clear();
    masterStrip->refresh();
    procs.clear();
    oldTracks.clear();
    oldBuses.clear();
    engine.updateLength();
    playStart = 0;
}

void MainComponent::confirmDiscard (std::function<void()> proceed)
{
    if (! dirty) { proceed(); return; }
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle ("SilverConsole")
                                      .withMessage (juce::String::fromUTF8 ("O projeto tem altera\xc3\xa7\xc3\xb5\x65s n\xc3\xa3o salvas."))
                                      .withButton ("Salvar")
                                      .withButton ("Descartar")
                                      .withButton ("Cancelar"),
                                  [this, proceed] (int r)
    {
        if (r == 1) save ([proceed] (bool ok) { if (ok) proceed(); });
        else if (r == 2) proceed();
    });
}

void MainComponent::newProject()
{
    if (job != nullptr) return;
    confirmDiscard ([this]
    {
        resetSession();
        currentFile = juce::File();
        dirty = false;
        rebuildStrips();
        updateTitle();
    });
}

void MainComponent::openProjectDialog()
{
    if (job != nullptr) return;
    confirmDiscard ([this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Abrir projeto", currentFile.getParentDirectory(),
                                                       juce::String ("*") + kProjectExtension);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult() != juce::File()) { dirty = false; openFile (fc.getResult()); } });
    });
}

void MainComponent::openFile (const juce::File& file)
{
    if (job != nullptr) return;
    if (! file.hasFileExtension (kProjectExtension))
    {
        if (isAudioFile (file)) importFiles ({ file });
        return;
    }
    if (dirty) { confirmDiscard ([this, file] { dirty = false; openFile (file); }); return; }

    auto data = std::make_shared<ProjectData>();
    juce::String err;
    if (! readProject (file, *data, err)) { alert ("Abrir", err); return; }

    auto decoded = std::make_shared<std::vector<DecodedFile>>();
    for (auto& t : data->tracks) decoded->push_back ({ t.file, {}, {} });
    const double sr = engine.getSampleRate();

    runJob ("Abrindo " + file.getFileName() + "...", true,
            [decoded, sr] (Job& j)
            {
                parallelFor ((int) decoded->size(), j, [&] (int i)
                {
                    auto& d = (*decoded)[(size_t) i];
                    if (d.file != juce::File()) decodeInto (d, sr);
                });
            },
            [this, data, decoded, file, sr] (Job& j)
    {
        if (j.cancel.load()) return;
        resetSession();
        juce::StringArray problems;
        juce::MouseCursor::showWaitCursor();

        for (auto& bd : data->buses)
            if (auto* b = engine.addBus (bd.name))
            {
                applyChannelBasics (engine, *b, bd);
                loadInserts (*b, bd, problems);
            }
        for (size_t i = 0; i < data->tracks.size(); ++i)
        {
            auto& td = data->tracks[i];
            auto& d = (*decoded)[i];
            auto t = std::make_unique<Track>();
            applyChannelBasics (engine, *t, td.ch);
            t->sourceFile = td.file;
            t->outputBus = td.output;
            for (int s = 0; s < kNumSends; ++s)
            {
                t->sends[(size_t) s].bus = td.sends[(size_t) s].bus;
                t->sends[(size_t) s].levelDb = td.sends[(size_t) s].levelDb;
                t->sends[(size_t) s].preFader = td.sends[(size_t) s].pre;
            }
            auto* raw = engine.addTrack (std::move (t));
            if (d.audio.ok()) engine.setTrackAudio (*raw, d.audio.original, d.audio.rate, d.playback, sr);
            else problems.add (raw->name + ": " + (d.audio.error.isNotEmpty() ? d.audio.error : juce::String ("sem arquivo de audio")));
            loadInserts (*raw, td.ch, problems);
        }
        applyChannelBasics (engine, engine.master, data->master);
        engine.master.name = "MASTER";
        engine.master.mono = data->masterMono;
        loadInserts (engine.master, data->master, problems);
        if (data->loopEndSec > data->loopStartSec)
            engine.setLoop ((juce::int64) (data->loopStartSec * sr), (juce::int64) (data->loopEndSec * sr));
        engine.loopOn = data->loopOn;
        juce::MouseCursor::hideWaitCursor();

        currentFile = file;
        dirty = false;
        rebuildStrips();
        updateTitle();
        if (! problems.isEmpty())
            alert ("Projeto aberto com avisos", problems.joinIntoString ("\n")
                   + "\n\nPlugins ausentes ficam em vermelho e guardam o estado; ao salvar, nada se perde.");
    });
}

bool MainComponent::writeTo (const juce::File& f)
{
    juce::String err;
    if (! writeProject (captureProject (engine), f, err)) { alert ("Salvar", err); return false; }
    currentFile = f;
    dirty = false;
    updateTitle();
    return true;
}

void MainComponent::save (std::function<void (bool)> after)
{
    if (currentFile == juce::File()) { saveAs (std::move (after)); return; }
    const bool ok = writeTo (currentFile);
    if (after) after (ok);
}

void MainComponent::saveAs (std::function<void (bool)> after)
{
    auto start = currentFile != juce::File() ? currentFile
                                             : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (juce::String ("Mix") + kProjectExtension);
    chooser = std::make_unique<juce::FileChooser> ("Salvar projeto", start, juce::String ("*") + kProjectExtension);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, after] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        bool ok = false;
        if (f != juce::File()) ok = writeTo (f.withFileExtension (kProjectExtension));
        if (after) after (ok);
    });
}

void MainComponent::requestQuit()
{
    if (job != nullptr) { job->cancel = true; return; }
    confirmDiscard ([] { juce::JUCEApplication::getInstance()->quit(); });
}

//==============================================================================
bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (isAudioFile (juce::File (f)) || juce::File (f).hasFileExtension (kProjectExtension)) return true;
    return false;
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    juce::Array<juce::File> audio;
    for (auto& f : files)
    {
        juce::File file (f);
        if (file.hasFileExtension (kProjectExtension)) { openFile (file); return; }
        if (isAudioFile (file)) audio.add (file);
    }
    audio.sort();
    importFiles (audio);
}
} // namespace sc
