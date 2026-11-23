// SilverConsole - janela principal. Codigo original (MIT).
#include "MainComponent.h"
#include "AudioLoader.h"
#include "PluginScanPolicy.h"
#include "PluginScanSupervisor.h"
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
        std::shared_ptr<StreamingAudio> stream;
    };

    void decodeInto (DecodedFile& d, double sr)
    {
        juce::AudioFormatManager fm;
        registerAudioFormats (fm);
        // Arquivos grandes na taxa nativa: leitura com cache; os demais
        // preservam o decodificador e reamostrador sinc originais.
        if (d.file.getSize() >= StreamingAudio::minimumFileBytes)
        {
            juce::String reason;
            d.stream = StreamingAudio::open (d.file, sr, reason);
            if (d.stream != nullptr) return;
        }
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

    // selecao (= regiao do loop)
    const auto ls = engine.loopStart.load(), le = engine.loopEnd.load();
    if (le > ls)
    {
        const bool looping = engine.loopOn.load();
        auto lr = juce::Rectangle<float>::leftTopRightBottom (sampleToX (ls), r.getY() + 2.0f, sampleToX (le), r.getBottom() - 2.0f);
        const auto col = looping ? colours::accent : colours::silverTop;
        g.setColour (col.withAlpha (looping ? 0.22f : 0.16f));
        g.fillRect (lr);
        g.setColour (col);
        g.fillRect (lr.withWidth (2.0f));
        g.fillRect (lr.withLeft (lr.getRight() - 2.0f));
        const juce::String label = timeText (ls, sr) + "  -  " + timeText (le, sr)
                                   + "   (" + juce::String ((double) (le - ls) / sr, 1) + " s" + (looping ? ", loop)" : ")");
        g.setFont (uiFont (10.0f, true));
        if (lr.getWidth() > 190.0f)
            g.drawText (label, lr.reduced (6.0f, 0.0f).withTop (r.getBottom() - 20.0f).withHeight (12.0f).toNearestInt(),
                        juce::Justification::centred);
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

Timeline::Drag Timeline::hitTestDrag (const juce::MouseEvent& e) const
{
    const float x = (float) e.x;
    if (engine.hasSelection())
    {
        if (std::abs (x - sampleToX (engine.loopStart.load())) <= 5.0f) return Drag::startEdge;
        if (std::abs (x - sampleToX (engine.loopEnd.load())) <= 5.0f) return Drag::endEdge;
    }
    if (e.y < 12 && std::abs (x - sampleToX (engine.position.load())) <= 7.0f) return Drag::scrub;
    return Drag::click;
}

void Timeline::mouseMove (const juce::MouseEvent& e)
{
    const auto d = hitTestDrag (e);
    setMouseCursor (d == Drag::startEdge || d == Drag::endEdge ? juce::MouseCursor::LeftRightResizeCursor
                                                               : juce::MouseCursor::NormalCursor);
}

void Timeline::mouseDown (const juce::MouseEvent& e)
{
    drag = Drag::none;
    if (e.mods.isPopupMenu()) { if (onMenu) onMenu(); return; }
    dragAnchor = xToSample ((float) e.x);
    drag = hitTestDrag (e);
    loopWhileSelecting = e.mods.isShiftDown();
    if (drag == Drag::click && loopWhileSelecting) drag = Drag::select;
    repaint();
}

void Timeline::mouseDrag (const juce::MouseEvent& e)
{
    const auto s = xToSample ((float) e.x);
    if (drag == Drag::click && e.getDistanceFromDragStart() > 4) drag = Drag::select;
    switch (drag)
    {
        case Drag::select:
            engine.setLoop (dragAnchor, s);
            if (loopWhileSelecting) engine.loopOn = true;
            break;
        case Drag::startEdge: engine.setLoop (s, engine.loopEnd.load()); break;
        case Drag::endEdge:   engine.setLoop (engine.loopStart.load(), s); break;
        case Drag::scrub:
            engine.stopAt = -1;
            engine.position = s;
            if (onEdited) onEdited();
            break;
        case Drag::click: case Drag::none: break;
    }
    repaint();
}

void Timeline::mouseUp (const juce::MouseEvent&)
{
    if (drag == Drag::click)
    {
        engine.stopAt = -1;
        engine.position = dragAnchor;
        if (onEdited) onEdited();
    }
    else if (drag == Drag::select || drag == Drag::startEdge || drag == Drag::endEdge)
    {
        if (onSelectionChanged) onSelectionChanged();
    }
    drag = Drag::none;
    repaint();
}

void Timeline::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto s = xToSample ((float) e.x);
    if (engine.hasSelection() && s >= engine.loopStart.load() && s <= engine.loopEnd.load() && onPlaySelection)
        onPlaySelection();
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
    toStartButton.onClick = [this] { engine.stopAt = -1; engine.position = 0; playStart = 0; };
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this]
    {
        engine.stopAt = -1;
        if (engine.playing.load()) { engine.playing = false; engine.position = playStart; }
        else { engine.position = 0; playStart = 0; }
    };
    loopButton.setClickingTogglesState (true);
    loopButton.onClick = [this]
    {
        if (loopButton.getToggleState() && ! engine.hasSelection()) selectWholeSong();
        engine.loopOn = loopButton.getToggleState();
        if (engine.loopOn.load()) engine.stopAt = -1;
        markDirty();
    };
    addAndMakeVisible (timeline);
    timeline.onEdited = [this] { if (! engine.playing.load()) playStart = engine.position.load(); };
    timeline.onSelectionChanged = [this] { markDirty(); };
    timeline.onPlaySelection = [this] { playSelection(); };
    timeline.onMenu = [this] { timelineMenu (timeline); };

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
    openButton.onClick = [this] { openMenu(); };
    openButton.setTooltip ("Abrir projeto .sconsole (Ctrl+O) ou um projeto recente");
    recentFiles.setMaxNumberOfItems (10);
    recentFiles.restoreFromString (props.getUserSettings()->getValue ("recentProjects"));
    saveButton.onClick = [this]
    {
        if (juce::ModifierKeys::currentModifiers.isShiftDown()) saveAs();
        else save();
    };
    importButton.onClick = [this] { importDialog(); };
    addBusButton.onClick = [this] { if (createBus() == nullptr) alert ("Bus", "Limite de " + juce::String (kMaxBuses) + " buses."); };
    exportButton.onClick = [this] { exportDialog(); };
    pluginsButton.onClick = [this]
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Gerenciar plugins VST3...");
        menu.addItem (2, "Escanear VST3 com processo isolado");
        menu.addItem (3, "Reavaliar VST3 com falha anterior...");
        menu.addItem (4, "Testar DSP dos VST3 em processo isolado...");
        const juce::Component::SafePointer<MainComponent> self (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&pluginsButton),
                            [self] (int choice)
        {
            if (self == nullptr) return;
            if (choice == 1) self->showPluginManager (false);
            else if (choice == 2) self->scanPluginsIsolated();
            else if (choice == 3) self->scanPluginsIsolated (true);
            else if (choice == 4) self->scanPluginsIsolated (false, true);
        });
    };
    audioButton.onClick = [this] { audioMenu(); };

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
    engine.setMultiCore (props.getUserSettings()->getBoolValue ("multiCore", true));
    deviceManager.addAudioCallback (&engine);
    deviceManager.addChangeListener (this);

    setSize (1440, 900);
    updateTitle();
    unsavedRecoveryId = juce::Uuid().toString();
    lastAutosaveMs = juce::Time::currentTimeMillis();
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
    g.drawFittedText (statusText, statusArea(), juce::Justification::centredRight, 1, 0.75f);

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

    // Todos os canais alinhados: as linhas de insert/envio seguem o canal que tem mais.
    int insertRows = engine.master.inserts.size(), sendRows = kDefaultSends;
    for (auto& t : engine.tracks)
    {
        insertRows = juce::jmax (insertRows, t->inserts.size());
        sendRows = juce::jmax (sendRows, t->sends.size());
    }
    for (auto& b : engine.buses) insertRows = juce::jmax (insertRows, b->inserts.size());
    ChannelStrip::setLayoutRows (insertRows, sendRows);
    const int minH = ChannelStrip::minimumHeight();
    const int h = juce::jmax (minH, mixerViewport.getHeight() - 12);
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
    if (masterStrip->getHeight() < minH) masterStrip->setSize (masterStrip->getWidth(), minH);
    for (auto* s : strips) s->syncSlots();
    masterStrip->syncSlots();
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
            if (quitAfterJob) { quitAfterJob = false; requestQuit(); }
        }
        return;
    }

    engine.pollLatencyChanges();

    // Recuperacao antes de iniciar um novo trabalho. Nao interfere no audio.
    if (! recoveryPromptShown && tickCount > 35)
    {
        // Se o usuario ja comecou a editar, nao interrompa com alerta antigo.
        if (dirty) recoveryPromptShown = true;
        else if (juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0)
            offerRecoveryOnStartup();
    }

    // Snapshot somente quando parado e sem tarefas/modal abertos. Nao altera
    // dirty nem currentFile e jamais substitui o .sconsole principal.
    if (dirty && ! engine.playing.load() && recoveryPromptShown
        && juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0)
    {
        const auto now = juce::Time::currentTimeMillis();
        if (now - lastAutosaveMs >= 120000)
        {
            lastAutosaveMs = now;
            writeRecoverySnapshot();
        }
    }

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
              << bs << " (" << juce::String (bs * 1000.0 / sr, 1) << " ms)  |  CPU "
              << juce::roundToInt (deviceManager.getCpuUsage() * 100.0) << "%";
            const int workers = engine.isMultiCore() ? engine.getRealtimeWorkerCount() : 0;
            s << " (" << juce::String (workers + 1) << juce::String::fromUTF8 (workers > 0 ? " n\xc3\xba" "cleos" : " n\xc3\xba" "cleo") << ")";
            s << "  |  PDC " << juce::String (engine.getTotalLatency() * 1000.0 / sr, 1) << " ms";
            // Diagnostico de estalos: XRUN = callback passou do prazo (CPU);
            // DISCO = audio do disco nao chegou a tempo; LOCK = edicao concorrente.
            const auto rt = engine.getRealtimeStats();
            s << "  |  XRUN " << juce::String (rt.callbackOverruns)
              << "  |  DISCO " << juce::String (engine.getDiskMissingSamples() > 0 ? (int) juce::jmin<juce::uint64> (999999, engine.getDiskMissingSamples() / (juce::uint64) juce::jmax (1, bs)) : 0)
              << "  |  LOCK " << juce::String (rt.lockContentions);
            if (rt.pluginExceptions > 0) s << "  |  PLUG ERR " << juce::String (rt.pluginExceptions);
        }
        else s = "Sem dispositivo de audio - clique em Audio";
        if (s != statusText) { statusText = s; repaint (statusArea()); }
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
    if (k == juce::KeyPress (juce::KeyPress::spaceKey, juce::ModifierKeys::shiftModifier, 0)) { playSelection(); return true; }
    if (k == juce::KeyPress::spaceKey) { togglePlay(); return true; }
    if (k == juce::KeyPress::escapeKey && engine.hasSelection()) { engine.clearSelection(); markDirty(); return true; }
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
    if (engine.playing.load()) { engine.playing = false; engine.stopAt = -1; }
    else
    {
        engine.stopAt = -1;
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

juce::File MainComponent::recoveryDirectory() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("SilverConsole").getChildFile ("Recovery");
}

juce::File MainComponent::recoveryFileFor (const juce::File& project) const
{
    const juce::String key = project == juce::File()
        ? ("untitled-" + unsavedRecoveryId)
        : juce::String::toHexString (project.getFullPathName().hashCode64());
    return recoveryDirectory().getChildFile ("autosave-" + key + juce::String (kProjectExtension));
}

void MainComponent::writeRecoverySnapshot()
{
    auto folder = recoveryDirectory();
    if (! folder.isDirectory() && ! folder.createDirectory())
    {
        juce::Logger::writeToLog ("SilverConsole: nao foi possivel criar pasta Recovery");
        return;
    }
    // Plugins sao capturados enquanto transporte esta parado; o lock evita
    // concorrencia com a engine. Escrita atomica via writeProject/TemporaryFile.
    auto snapshot = std::make_shared<ProjectData>();
    {
        const juce::ScopedLock guard (engine.lock);
        *snapshot = captureProject (engine);
    }
    // Gerar o XML (estados de plugins em base64) e gravar no disco fica numa
    // thread separada: a interface nao trava em projetos grandes.
    if (backgroundWrites.getNumJobs() > 0) return; // gravacao anterior ainda em andamento
    backgroundWrites.addJob ([snapshot, file = recoveryFileFor (currentFile)]
    {
        juce::String error;
        if (! writeProject (*snapshot, file, error))
            juce::Logger::writeToLog ("SilverConsole autosave: " + error);
    });
}

void MainComponent::offerRecoveryOnStartup()
{
    recoveryPromptShown = true;
    const auto folder = recoveryDirectory();
    if (! folder.isDirectory()) return;
    juce::Array<juce::File> snapshots;
    folder.findChildFiles (snapshots, juce::File::findFiles, false, "*.sconsole");
    if (snapshots.isEmpty()) return;

    juce::File latest;
    for (const auto& candidate : snapshots)
        if (latest == juce::File() || candidate.getLastModificationTime() > latest.getLastModificationTime())
            latest = candidate;

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle ("Recuperacao SilverConsole")
            .withMessage ("Foi encontrada uma copia de recuperacao de projeto.\n"
                          "Deseja abri-la? Depois use Salvar como para criar um projeto normal.\n"
                          + latest.getFileName())
            .withButton ("Recuperar")
            .withButton ("Depois"),
        [safe = juce::Component::SafePointer<MainComponent> (this), latest] (int selection)
        {
            if (safe != nullptr && selection == 1)
                safe->openFile (latest);
        });
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

void MainComponent::setInsertSlots (Channel& c, int count)
{
    if (! channelExists (&c)) return;
    if (count > kMaxInsertsPerChannel)
    {
        alert ("Inserts", "Limite de " + juce::String (kMaxInsertsPerChannel) + " inserts por canal.");
        return;
    }
    if (engine.setInsertCount (c, count))
    {
        resized(); // realinha todos os canais e cria/remove os slots na tela
        if (auto* s = stripFor (&c)) s->refresh();
        markDirty();
    }
}

void MainComponent::setSendSlots (Track& t, int count)
{
    if (! channelExists (&t)) return;
    if (count > kMaxSendsPerTrack)
    {
        alert ("Envios", "Limite de " + juce::String (kMaxSendsPerTrack) + " envios por faixa.");
        return;
    }
    if (engine.setSendCount (t, count))
    {
        resized();
        if (auto* s = stripFor (&t)) s->refresh();
        markDirty();
    }
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
    if (send == t.sends.size() - 1 && t.sends.size() > 1)
    {
        m.addSeparator();
        m.addItem (4, "Remover este envio");
    }

    auto* tp = &t;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source), [this, tp, send] (int r)
    {
        if (r == 0 || ! channelExists (tp) || send >= tp->sends.size()) return;
        auto& snd = tp->sends[(size_t) send];
        if (r == 4)
        {
            snd.bus = -1;
            setSendSlots (*tp, tp->sends.size() - 1);
            return;
        }
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
    if (! c.hasInsert (slot)) return;
    auto& s = c.inserts[(size_t) slot];
    if (s.isEmpty())
    {
        // botao direito no ultimo slot vazio: opcao de remove-lo
        if (e.mods.isPopupMenu() && slot == c.inserts.size() - 1 && c.inserts.size() > 1)
        {
            juce::PopupMenu m;
            m.addSectionHeader ("Insert " + juce::String (slot + 1) + " - " + c.name);
            m.addItem (1, "Inserir plugin...");
            m.addItem (2, "Remover este slot vazio");
            auto* cp = &c;
            juce::Component::SafePointer<juce::Component> src (&source);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source), [this, cp, slot, src] (int r)
            {
                if (r == 0 || ! channelExists (cp)) return;
                if (r == 1 && src != nullptr) showPluginMenu (*cp, slot, *src, false);
                if (r == 2) setInsertSlots (*cp, cp->inserts.size() - 1);
            });
            return;
        }
        showPluginMenu (c, slot, source, false);
        return;
    }
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
    m.addItem (2, "Bypass", s.processor != nullptr || s.remote != nullptr, s.bypassed.load());
    if (s.remote != nullptr)
        m.addItem (6, "Reiniciar DSP isolado (recuperar falha)");
    if (s.processor != nullptr && s.hasDescription)
    {
        const auto cur = scaleModeFor (s.description);
        juce::PopupMenu sm;
        sm.addItem (70, juce::String::fromUTF8 ("Autom\xc3\xa1tica (corrige plugin gigante)"), true, cur == PluginScaleMode::automatic);
        sm.addItem (72, "Sempre corrigir escala dupla", true, cur == PluginScaleMode::fixDouble);
        sm.addItem (71, juce::String::fromUTF8 ("Escala do monitor, sem corre\xc3\xa7\xc3\xa3o"), true, cur == PluginScaleMode::native);
        sm.addItem (73, juce::String::fromUTF8 ("Modo compat\xc3\xadvel do Windows (sem DPI)"), true, cur == PluginScaleMode::compatibility);
        m.addSubMenu ("Escala da janela", sm);
    }
    m.addSeparator();
    m.addItem (3, "Substituir...");
    m.addItem (4, "Mover para cima", slot > 0);
    m.addItem (5, "Mover para baixo", slot < c.inserts.size() - 1);
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
        else if (r == 6 && sl.remote != nullptr)
        {
            // Reiniciar e feito aqui, na thread de mensagens, nao no callback.
            loadPluginIntoRemote (*cp, slot, sl.description, sl.missingState);
        }
        else if (r >= 70 && r <= 73)
        {
            // salvo por plugin (vale para todas as instancias); reabre a janela no novo modo
            setScaleMode (sl.description, (PluginScaleMode) (r - 70));
            const bool wasOpen = isEditorOpen (*cp, slot);
            closeEditor (*cp, slot);
            if (wasOpen || sl.processor != nullptr) openEditor (*cp, slot);
            return;
        }
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
    if (! types.isEmpty())
    {
        juce::PopupMenu isolated;
        for (int i = 0; i < types.size(); ++i)
            isolated.addItem (10000 + i, types.getReference (i).name);
        m.addSubMenu ("Carregar VST3 isolado (experimental, sem editor)", isolated);
    }
    auto* cp = &c;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source).withMaximumNumColumns (2),
                     [this, cp, slot, types] (int r)
    {
        if (r == 0 || ! channelExists (cp)) return;
        if (r == -1) { showPluginManager (false); return; }
        if (r >= 10000 && juce::isPositiveAndBelow (r - 10000, types.size()))
        {
            loadPluginIntoRemote (*cp, slot, types.getReference (r - 10000));
            return;
        }
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
    const bool hadPlugin = ! c.inserts[(size_t) slot].isEmpty();
    auto old = engine.setInsert (c, slot, std::move (inst), &d);
    old.reset();
    if (hadPlugin) { engine.removeLanesForSlot (c, slot); lanesChanged(); } // parametros do plugin antigo nao valem mais
    if (auto* s = stripFor (&c)) s->refresh();
    markDirty();
    openEditor (c, slot);
}

void MainComponent::loadPluginIntoRemote (
    Channel& c, int slot, const juce::PluginDescription& d,
    const juce::MemoryBlock& state)
{
    // A criacao pode levar segundos, mas nunca acontece no callback da placa.
    // Nenhum AudioPluginInstance e carregado no processo principal.
    const auto worker = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                        .getSiblingFile ("ScDspWorker.exe");
    juce::String err;
    juce::MouseCursor::showWaitCursor();
    auto remote = RemoteDspInsert::start (d, state, worker,
                                          engine.getSampleRate(),
                                          engine.getBlockSize(), err);
    juce::MouseCursor::hideWaitCursor();
    if (remote == nullptr)
    {
        alert ("DSP isolado", "Nao foi possivel abrir " + d.name + " no processo DSP.\n" + err);
        return;
    }
    closeEditor (c, slot);
    const bool hadPlugin = ! c.inserts[(size_t) slot].isEmpty();
    auto old = engine.setRemoteInsert (c, slot, std::move (remote), d);
    old.reset(); // destruicao do plugin antigo fora do lock
    if (hadPlugin)
    {
        engine.removeLanesForSlot (c, slot);
        lanesChanged();
    }
    if (auto* strip = stripFor (&c)) strip->refresh();
    markDirty();
}

void MainComponent::loadInserts (Channel& c, const ChannelData& data, juce::StringArray& problems)
{
    for (auto& in : data.inserts)
    {
        juce::String err;
        if (in.isolated)
        {
            const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                 .getSiblingFile ("ScDspWorker.exe");
            auto worker = RemoteDspInsert::start (in.desc, in.state, exe,
                                                 engine.getSampleRate(), engine.getBlockSize(), err);
            if (worker != nullptr)
            {
                engine.setRemoteInsert (c, in.slot, std::move (worker), in.desc);
                if (in.bypass) engine.setBypass (c, in.slot, true);
                continue;
            }
            engine.setMissingInsert (c, in.slot, in.desc, in.state, true);
            problems.add (c.name + ": DSP isolado indisponivel - " + in.desc.name
                          + (err.isNotEmpty() ? " (" + err + ")" : juce::String()));
            continue;
        }
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
        if (in.bypass) engine.setBypass (c, in.slot, true); // preserva o bypass ao salvar de novo
        problems.add (c.name + ": plugin " + in.desc.name + (err.isNotEmpty() ? " (" + err + ")" : juce::String()));
    }
}

// Modo de escala por plugin: linhas "modo|identificador" em pluginScaleModes.
// A lista antiga pluginCompatScaling continua valendo como modo compativel.
PluginScaleMode MainComponent::scaleModeFor (const juce::PluginDescription& d) const
{
    const auto id = d.createIdentifierString();
    for (auto& line : juce::StringArray::fromLines (props.getUserSettings()->getValue ("pluginScaleModes")))
        if (line.fromFirstOccurrenceOf ("|", false, false) == id)
            return (PluginScaleMode) juce::jlimit (0, 3, line.upToFirstOccurrenceOf ("|", false, false).getIntValue());
    if (juce::StringArray::fromLines (props.getUserSettings()->getValue ("pluginCompatScaling")).contains (id))
        return PluginScaleMode::compatibility;
    return PluginScaleMode::automatic;
}

void MainComponent::setScaleMode (const juce::PluginDescription& d, PluginScaleMode mode)
{
    const auto id = d.createIdentifierString();
    auto* settings = props.getUserSettings();
    auto legacy = juce::StringArray::fromLines (settings->getValue ("pluginCompatScaling"));
    legacy.removeString (id);
    legacy.removeEmptyStrings();
    settings->setValue ("pluginCompatScaling", legacy.joinIntoString ("\n"));

    juce::StringArray lines;
    for (auto& line : juce::StringArray::fromLines (settings->getValue ("pluginScaleModes")))
        if (line.isNotEmpty() && line.fromFirstOccurrenceOf ("|", false, false) != id) lines.add (line);
    if (mode != PluginScaleMode::automatic) lines.add (juce::String ((int) mode) + "|" + id);
    settings->setValue ("pluginScaleModes", lines.joinIntoString ("\n"));
    settings->saveIfNeeded();
}

void MainComponent::openEditor (Channel& c, int slot)
{
    if (! c.hasInsert (slot)) return;
    auto& insert = c.inserts[(size_t) slot];
    auto* p = insert.processor.get();
    if (p == nullptr) return;
    for (auto& w : pluginWindows)
        if (&w->processor == p) { w->setVisible (true); w->toFront (true); return; }

    const auto scaleMode = insert.hasDescription ? scaleModeFor (insert.description) : PluginScaleMode::automatic;
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
    }, scaleMode));
    if (auto* s = stripFor (&c)) s->refresh();
}

void MainComponent::closeEditor (Channel& c, int slot)
{
    if (! c.hasInsert (slot)) return;
    auto* p = c.inserts[(size_t) slot].processor.get();
    if (p == nullptr) return;
    for (auto it = pluginWindows.begin(); it != pluginWindows.end(); ++it)
        if (&(*it)->processor == p) { pluginWindows.erase (it); break; }
}

void MainComponent::closeEditorsFor (Channel& c)
{
    for (int i = 0; i < c.inserts.size(); ++i) closeEditor (c, i);
}

void MainComponent::closeAllEditors() { pluginWindows.clear(); }

bool MainComponent::isEditorOpen (const Channel& c, int slot) const
{
    if (! c.hasInsert (slot)) return false;
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
            list->setOptionsButtonText ("Opcoes / Scan interno (sem isolamento)");
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
    if (startScan) scanPluginsIsolated();
}

void MainComponent::scanPluginsIsolated (bool retryFailed, bool probeDsp)
{
    if (job != nullptr) return;

    const auto worker = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                            .getSiblingFile ("ScPluginScanWorker.exe");
    if (! worker.existsAsFile())
    {
        alert ("Scanner VST3", "O auxiliar ScPluginScanWorker.exe nao esta ao lado "
                             "do SilverConsole.exe. Recompile ou extraia o pacote completo.");
        return;
    }

    const auto remembered = PluginScanPolicy::deserialize (
        props.getUserSettings()->getValue (PluginScanPolicy::settingsKey));
    if (retryFailed && remembered.isEmpty())
    {
        alert ("Scanner VST3", "Nao existem plugins em quarentena para reavaliar.");
        return;
    }

    struct Result
    {
        std::vector<juce::PluginDescription> types;
        juce::StringArray successPaths, failedPaths;
        int total = 0, scanned = 0, skipped = 0, failed = 0, timedOut = 0;
        bool cancelled = false;
    };
    auto result = std::make_shared<Result>();
    runJob (retryFailed ? "Reavaliando VST3 com falhas anteriores..."
                        : probeDsp ? "Testando DSP VST3 em processo isolado..."
                        : "Escaneando VST3 em processo isolado...", true,
            [worker, result, remembered, retryFailed, probeDsp] (Job& j)
    {
        // Apenas a enumeracao de caminhos acontece no host. Todo carregamento
        // de VST3 para discovery acontece em processo filho independente.
        juce::VST3PluginFormat format;
        auto candidates = format.searchPathsForPlugins (
            format.getDefaultLocationsToSearch(), true, false);
        candidates.removeDuplicates (true);
        result->total = candidates.size();

        for (int i = 0; i < candidates.size(); ++i)
        {
            if (j.cancel.load()) { result->cancelled = true; break; }
            const auto& candidate = candidates.getReference (i);
            if (! PluginScanPolicy::shouldScan (remembered, candidate, retryFailed))
            {
                ++result->skipped;
                j.progress = (float) (i + 1) / (float) juce::jmax (1, candidates.size());
                continue;
            }

            ++result->scanned;
            const auto destination = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("silverconsole-scan-" + juce::Uuid().toString() + ".xml");
            juce::StringArray args;
            args.add (worker.getFullPathName());
            if (probeDsp) args.add ("--probe");
            args.add (candidate);
            args.add (destination.getFullPathName());

            bool passed = false;
            // Supervisor compartilhado com os testes de falha/cancelamento.
            // Nao roda na thread realtime e mata o filho em timeout/cancel.
            const auto scan = PluginScanSupervisor::run (args, j.cancel, 20000);
            if (scan.status == PluginScanSupervisor::Status::cancelled)
                result->cancelled = true;
            if (scan.status == PluginScanSupervisor::Status::timedOut)
                ++result->timedOut;

            if (scan.ok()
                && destination.getSize() > 0
                && destination.getSize() <= 4 * 1024 * 1024)
            {
                    if (auto xml = juce::XmlDocument::parse (destination))
                    {
                        if (xml->hasTagName ("SILVERCONSOLE_PLUGIN_SCAN")
                            && xml->getStringAttribute ("format") == "VST3"
                            && xml->getIntAttribute ("schemaVersion") == 1)
                        {
                            for (auto* node : xml->getChildIterator())
                            {
                                if (! node->hasTagName ("PLUGIN")) continue;
                                juce::PluginDescription desc;
                                if (desc.loadFromXml (*node) && desc.pluginFormatName == "VST3"
                                    && desc.name.isNotEmpty())
                                {
                                    result->types.push_back (std::move (desc));
                                    passed = true;
                                }
                            }
                        }
                    }
            }
            destination.deleteFile();
            if (result->cancelled) break;

            if (passed)
                result->successPaths.add (candidate);
            else
            {
                result->failedPaths.add (candidate);
                ++result->failed;
            }
            j.progress = (float) (i + 1) / (float) juce::jmax (1, candidates.size());
        }
    },
            [this, result, probeDsp] (Job&)
    {
        auto* settings = props.getUserSettings();
        auto failed = PluginScanPolicy::deserialize (
            settings->getValue (PluginScanPolicy::settingsKey));
        for (auto& path : result->successPaths)
            PluginScanPolicy::reportResult (failed, path, true);
        for (auto& path : result->failedPaths)
            PluginScanPolicy::reportResult (failed, path, false);
        settings->setValue (PluginScanPolicy::settingsKey,
                            PluginScanPolicy::serialize (failed));

        for (auto& description : result->types)
            pluginHost.known.addType (description);
        settings->saveIfNeeded();
        if (pluginManagerWindow != nullptr) pluginManagerWindow->repaint();

        const juce::String summary = "Reconhecidos " + juce::String ((int) result->types.size())
            + " tipo(s) VST3 em " + juce::String (result->scanned) + " candidato(s) examinados.\n"
            + juce::String (result->skipped) + " ignorado(s) por falha anterior.\n"
            + juce::String (result->failed) + (probeDsp ? " falha(s) no teste DSP, incluindo "
                                                          : " falha(s), incluindo ")
            + juce::String (result->timedOut) + " timeout(s).\n"
            + juce::String (failed.size()) + " candidato(s) na lista de quarentena."
            + (result->cancelled ? "\nVarredura cancelada, inclusive o processo em andamento."
                                 : "\nUse Reavaliar VST3 com falha para tentar novamente.");
        alert (probeDsp ? "Teste DSP isolado" : "Scanner isolado",
               summary, result->failed > 0);
    });
}

juce::Rectangle<int> MainComponent::statusArea() const
{
    const int left = automationButton.getRight() + 16;
    return { left, kTopBarH + 4, juce::jmax (100, getWidth() - left - 12), kToolbarH - 8 };
}

void MainComponent::audioMenu()
{
    const int workers = engine.getRealtimeWorkerCount();
    juce::PopupMenu m;
    m.addItem (1, juce::String::fromUTF8 ("Placa de som (dispositivo, buffer)..."));
    m.addSeparator();
    m.addItem (2, juce::String::fromUTF8 ("Processar faixas em v\xc3\xa1rios n\xc3\xba" "cleos (")
                      + juce::String (workers + 1) + juce::String::fromUTF8 (" n\xc3\xba" "cleos)"),
               workers > 0, engine.isMultiCore() && workers > 0);
    m.addItem (3, juce::String::fromUTF8 ("Zerar contadores (XRUN / DISCO / LOCK)"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&audioButton), [this] (int r)
    {
        if (r == 1) showAudioSettings();
        else if (r == 2)
        {
            engine.setMultiCore (! engine.isMultiCore());
            props.getUserSettings()->setValue ("multiCore", engine.isMultiCore());
            props.getUserSettings()->saveIfNeeded();
        }
        else if (r == 3) engine.resetRealtimeStats();
    });
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
            if (! d.audio.ok() && d.stream == nullptr) { if (d.audio.error.isNotEmpty()) errors.add (d.audio.error); continue; }
            auto t = std::make_unique<Track>();
            t->name = d.file.getFileNameWithoutExtension().substring (0, 40);
            t->colour = colours::trackPalette ((int) engine.tracks.size());
            t->sourceFile = d.file;
            auto* raw = engine.addTrack (std::move (t));
            if (d.stream != nullptr) engine.setTrackStream (*raw, d.stream);
            else engine.setTrackAudio (*raw, d.audio.original, d.audio.rate, d.playback, sr);
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
            if (! item->audio.ok() && item->stream == nullptr) { alert ("Audio", item->audio.error); return; }
            tp->sourceFile = item->file;
            if (item->stream != nullptr) engine.setTrackStream (*tp, item->stream);
            else engine.setTrackAudio (*tp, item->audio.original, item->audio.rate, item->playback, sr);
            refreshStrips();
            markDirty();
        });
    });
}

void MainComponent::startResampleIfNeeded()
{
    if (job != nullptr) return;
    const double sr = engine.getSampleRate();
    struct Item
    {
        Track* track;
        std::shared_ptr<const juce::AudioBuffer<float>> original, result;
        double rate;
        juce::File file;
        bool wasStream = false;
        std::shared_ptr<StreamingAudio> stream;
    };
    auto items = std::make_shared<std::vector<Item>>();
    for (auto& t : engine.tracks)
        if ((t->original != nullptr || t->diskStream != nullptr) && t->playbackRate != sr)
            items->push_back ({ t.get(), t->original, nullptr, t->originalRate, t->sourceFile, t->diskStream != nullptr, nullptr });
    if (items->empty()) return;

    runJob ("Ajustando a taxa de amostragem (" + juce::String (sr / 1000.0, 1) + " kHz)...", false,
            [items, sr] (Job& j)
            {
                parallelFor ((int) items->size(), j, [&] (int i)
                {
                    auto& it = (*items)[(size_t) i];
                    // Faixa em streaming: so reabre o arquivo na nova taxa (a conversao
                    // e feita trecho a trecho pelo disco); nada vai inteiro para a RAM.
                    if (it.wasStream && it.file.existsAsFile())
                    {
                        juce::String reason;
                        it.stream = StreamingAudio::open (it.file, sr, reason);
                        if (it.stream != nullptr) return;
                    }
                    if (it.original == nullptr && it.file.existsAsFile())
                    {
                        juce::AudioFormatManager fm;
                        registerAudioFormats (fm);
                        auto decoded = loadAudioFile (fm, it.file);
                        it.original = decoded.original;
                        it.rate = decoded.rate;
                    }
                    if (it.original != nullptr)
                        it.result = resampleBuffer (it.original, it.rate, sr);
                });
            },
            [this, items, sr] (Job&)
    {
        if (sr != engine.getSampleRate()) return;
        for (auto& it : *items)
        {
            if (! channelExists (it.track)) continue;
            if (it.stream != nullptr) engine.setTrackStream (*it.track, it.stream);
            else if (it.result != nullptr) engine.setTrackAudio (*it.track, it.original, it.rate, it.result, sr);
        }
        refreshStrips();
    });
}

namespace
{
//==============================================================================
/** Dialogo de exportacao de faixas individuais (stems). */
class StemExportPanel : public juce::Component
{
public:
    std::function<void (Engine::StemExport)> onExport;

    StemExportPanel (Engine& e, bool selectionOnly, const juce::String& prefixName)
        : engine (e), prefix (prefixName)
    {
        title.setText ("Exportar faixas individuais", juce::dontSendNotification);
        title.setFont (uiFont (17.0f, true));
        addAndMakeVisible (title);

        const double sr = engine.getSampleRate();
        const bool hasSel = engine.hasSelection();
        rangeBox.addItem (juce::String::fromUTF8 ("M\xc3\xbasica inteira"), 1);
        rangeBox.addItem (juce::String::fromUTF8 ("S\xc3\xb3 a sele\xc3\xa7\xc3\xa3o (") + timeText (engine.loopStart.load(), sr)
                              + " - " + timeText (engine.loopEnd.load(), sr) + ")", 2);
        rangeBox.setItemEnabled (2, hasSel);
        rangeBox.setSelectedId (selectionOnly && hasSel ? 2 : 1, juce::dontSendNotification);
        rangeBox.onChange = [this] { tailToggle.setToggleState (rangeBox.getSelectedId() == 1, juce::dontSendNotification); };

        modeBox.addItem (juce::String::fromUTF8 ("Com processamento (inserts, volume, pan, automa\xc3\xa7\xc3\xa3o)"), 1);
        modeBox.addItem (juce::String::fromUTF8 ("\xc3\x81udio original da faixa (sem processamento)"), 2);
        modeBox.setSelectedId (1, juce::dontSendNotification);
        modeBox.onChange = [this] { updateEnablement(); };

        formatBox.addItem ("WAV 24 bits", 24);
        formatBox.addItem ("WAV 16 bits (com dither)", 16);
        formatBox.addItem ("WAV 32 bits float", 32);
        formatBox.setSelectedId (24, juce::dontSendNotification);

        tailToggle.setButtonText ("Incluir 3 s de cauda no fim (reverb/delay)");
        tailToggle.setToggleState (rangeBox.getSelectedId() == 1, juce::dontSendNotification);
        busToggle.setButtonText ("Exportar os buses (cada bus com tudo que chega nele)");
        info.setText (juce::String::fromUTF8 ("Com processamento: cada faixa sai como soa na mix, sem buses e sem o master. "
                                              "Solo \xc3\xa9 ignorado; faixas em mute saem em sil\xc3\xaancio. "
                                              "Todas as faixas come\xc3\xa7" "am no mesmo ponto (alinhadas)."),
                      juce::dontSendNotification);
        info.setFont (uiFont (12.0f));
        info.setColour (juce::Label::textColourId, colours::textDim);
        info.setJustificationType (juce::Justification::topLeft);

        for (auto* c : std::initializer_list<juce::Component*> { &rangeBox, &modeBox, &formatBox, &tailToggle, &busToggle, &info,
                                                                 &allButton, &noneButton, &exportButton, &cancelButton, &tracksView })
            addAndMakeVisible (c);
        for (auto* l : { &rangeLabel, &modeLabel, &formatLabel, &tracksLabel })
        {
            l->setFont (uiFont (13.0f, true));
            addAndMakeVisible (l);
        }

        for (auto& t : engine.tracks)
        {
            auto* b = trackToggles.add (new juce::ToggleButton (t->name.isNotEmpty() ? t->name : juce::String ("Faixa")));
            b->setToggleState (t->sourceChannels > 0 && ! t->mute.load(), juce::dontSendNotification);
            b->setColour (juce::ToggleButton::tickColourId, t->colour);
            trackPtrs.push_back (t.get());
            tracksContent.addAndMakeVisible (b);
        }
        tracksView.setViewedComponent (&tracksContent, false);
        tracksView.setScrollBarsShown (true, false);
        allButton.onClick = [this] { for (auto* b : trackToggles) b->setToggleState (true, juce::dontSendNotification); };
        noneButton.onClick = [this] { for (auto* b : trackToggles) b->setToggleState (false, juce::dontSendNotification); };
        cancelButton.onClick = [this] { close(); };
        exportButton.onClick = [this] { submit(); };
        updateEnablement();
        setSize (560, 600);
    }

    ~StemExportPanel() override { tracksView.setViewedComponent (nullptr, false); }

    void paint (juce::Graphics& g) override { g.fillAll (colours::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (18);
        title.setBounds (r.removeFromTop (28));
        r.removeFromTop (8);
        auto row = [&r] (juce::Label& l, juce::Component& c)
        {
            auto line = r.removeFromTop (28);
            l.setBounds (line.removeFromLeft (90));
            c.setBounds (line);
            r.removeFromTop (6);
        };
        row (rangeLabel, rangeBox);
        row (modeLabel, modeBox);
        row (formatLabel, formatBox);
        tailToggle.setBounds (r.removeFromTop (24).withTrimmedLeft (90));
        busToggle.setBounds (r.removeFromTop (24).withTrimmedLeft (90));
        r.removeFromTop (4);
        info.setBounds (r.removeFromTop (48));
        auto buttons = r.removeFromBottom (32);
        cancelButton.setBounds (buttons.removeFromRight (110));
        buttons.removeFromRight (8);
        exportButton.setBounds (buttons.removeFromRight (150));
        r.removeFromBottom (10);
        auto head = r.removeFromTop (26);
        tracksLabel.setBounds (head.removeFromLeft (120));
        noneButton.setBounds (head.removeFromRight (80));
        head.removeFromRight (6);
        allButton.setBounds (head.removeFromRight (80));
        r.removeFromTop (4);
        tracksView.setBounds (r);
        const int w = r.getWidth() - tracksView.getScrollBarThickness() - 4;
        const int colW = w / 2;
        for (int i = 0; i < trackToggles.size(); ++i)
            trackToggles[i]->setBounds ((i % 2) * colW, (i / 2) * 24, colW - 6, 22);
        tracksContent.setSize (w, ((trackToggles.size() + 1) / 2) * 24);
    }

private:
    void updateEnablement()
    {
        const bool processed = modeBox.getSelectedId() == 1;
        tailToggle.setEnabled (processed);
        busToggle.setEnabled (processed && ! engine.buses.empty());
    }

    void close()
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0);
    }

    void submit()
    {
        Engine::StemExport req;
        req.prefix = prefix;
        req.bitDepth = formatBox.getSelectedId();
        req.processed = modeBox.getSelectedId() == 1;
        req.tailSeconds = req.processed && tailToggle.getToggleState() ? 3.0 : 0.0;
        if (rangeBox.getSelectedId() == 2 && engine.hasSelection())
        {
            req.rangeStart = engine.loopStart.load();
            req.rangeEnd = engine.loopEnd.load();
        }
        for (size_t i = 0; i < trackPtrs.size(); ++i)
        {
            if (! trackToggles[(int) i]->getToggleState()) continue;
            for (size_t ti = 0; ti < engine.tracks.size(); ++ti)
                if (engine.tracks[ti].get() == trackPtrs[i]) req.tracks.push_back ((int) ti);
        }
        if (req.processed && busToggle.getToggleState())
            for (int bi = 0; bi < (int) engine.buses.size(); ++bi) req.buses.push_back (bi);
        if (req.tracks.empty() && req.buses.empty())
        {
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::InfoIcon)
                                              .withTitle ("Exportar faixas").withMessage ("Marque pelo menos uma faixa.").withButton ("OK"),
                                          nullptr);
            return;
        }
        auto cb = onExport;
        close(); // o painel e destruido junto com o dialogo
        if (cb) cb (std::move (req));
    }

    Engine& engine;
    juce::String prefix;
    juce::Label title, info;
    juce::Label rangeLabel { {}, "Trecho" }, modeLabel { {}, juce::String::fromUTF8 ("Conte\xc3\xba" "do") },
                formatLabel { {}, "Formato" }, tracksLabel { {}, "Faixas" };
    juce::ComboBox rangeBox, modeBox, formatBox;
    juce::ToggleButton tailToggle, busToggle;
    juce::TextButton allButton { "Todas" }, noneButton { "Nenhuma" }, exportButton { "Escolher pasta..." }, cancelButton { "Cancelar" };
    juce::Viewport tracksView;
    juce::Component tracksContent;
    juce::OwnedArray<juce::ToggleButton> trackToggles;
    std::vector<const Track*> trackPtrs;
};
} // namespace

void MainComponent::exportDialog()
{
    if (engine.tracks.empty()) { alert ("Exportar", "Importe alguma faixa antes de exportar."); return; }
    const double sr = engine.getSampleRate();
    const bool hasSel = engine.hasSelection();
    const auto ls = engine.loopStart.load(), le = engine.loopEnd.load();
    juce::PopupMenu m;
    m.addSectionHeader (juce::String::fromUTF8 ("Mix est\xc3\xa9reo - m\xc3\xbasica inteira (") + juce::String (sr / 1000.0, 1) + " kHz)");
    m.addItem (24, "WAV 24 bits");
    m.addItem (16, "WAV 16 bits (com dither)");
    m.addItem (32, "WAV 32 bits float");
    if (hasSel)
    {
        m.addSectionHeader (juce::String::fromUTF8 ("Mix est\xc3\xa9reo - s\xc3\xb3 a sele\xc3\xa7\xc3\xa3o (") + timeText (ls, sr) + " - " + timeText (le, sr) + ")");
        m.addItem (124, "WAV 24 bits");
        m.addItem (116, "WAV 16 bits (com dither)");
        m.addItem (132, "WAV 32 bits float");
    }
    m.addSeparator();
    m.addItem (200, "Faixas individuais (stems)...");
    if (hasSel) m.addItem (201, juce::String::fromUTF8 ("Faixas individuais s\xc3\xb3 da sele\xc3\xa7\xc3\xa3o..."));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton), [this, ls, le] (int choice)
    {
        if (choice == 0) return;
        if (choice >= 200) { stemsDialog (choice == 201); return; }
        const bool selectionOnly = choice > 100;
        exportMixTo (choice % 100, selectionOnly ? ls : 0, selectionOnly ? le : -1);
    });
}

void MainComponent::exportMixTo (int bits, juce::int64 rangeStart, juce::int64 rangeEnd)
{
    if (engine.tracks.empty()) { alert ("Exportar", "Importe alguma faixa antes de exportar."); return; }
    auto base = currentFile != juce::File() ? currentFile.getFileNameWithoutExtension() : juce::String ("Mix");
    if (rangeEnd >= 0) base << juce::String::fromUTF8 (" - trecho ") << timeText (rangeStart, engine.getSampleRate()).replaceCharacter (':', '.');
    auto folder = currentFile != juce::File() ? currentFile.getParentDirectory()
                                              : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    auto start = folder.getChildFile (juce::File::createLegalFileName (base) + ".wav");
    chooser = std::make_unique<juce::FileChooser> ("Exportar mix", start, "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, bits, rangeStart, rangeEnd] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File()) return;
        file = file.withFileExtension (".wav");
        struct Result { bool ok = false, clipped = false; juce::String error; };
        auto res = std::make_shared<Result>();
        closeAllEditors(); // evita repintar editores durante o render offline
        runJob ("Exportando " + file.getFileName() + "...", true,
                [this, res, file, bits, rangeStart, rangeEnd] (Job& j)
                { res->ok = engine.exportMix (file, bits, 3.0, j.progress, j.cancel, res->error, res->clipped, rangeStart, rangeEnd); },
                [this, res, file] (Job&)
        {
            refreshStrips();
            if (! res->ok) { alert ("Exportar", res->error); return; }
            juce::String msg = "Mix exportada:\n" + file.getFullPathName();
            if (res->clipped) msg << "\n\nAtencao: a mix passou de 0 dBFS (clipping). Abaixe o master ou use um limitador.";
            alert ("Exportar", msg, res->clipped);
        });
    });
}

void MainComponent::stemsDialog (bool selectionOnly)
{
    if (engine.tracks.empty()) { alert ("Exportar", "Importe alguma faixa antes de exportar."); return; }
    auto* panel = new StemExportPanel (engine, selectionOnly,
                                       currentFile != juce::File() ? currentFile.getFileNameWithoutExtension() : juce::String ("Stems"));
    juce::Component::SafePointer<MainComponent> self (this);
    panel->onExport = [self] (Engine::StemExport req)
    {
        // depois que o dialogo fechou
        juce::MessageManager::callAsync ([self, req]
        {
            if (self != nullptr) self->runStemExport (req);
        });
    };
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (panel);
    o.dialogTitle = "Exportar faixas individuais";
    o.dialogBackgroundColour = colours::background;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.escapeKeyTriggersCloseButton = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::runStemExport (Engine::StemExport request)
{
    auto start = currentFile != juce::File() ? currentFile.getParentDirectory()
                                             : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Pasta onde criar a pasta das faixas"), start);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, request] (const juce::FileChooser& fc) mutable
    {
        const auto parent = fc.getResult();
        if (parent == juce::File()) return;
        // Sempre numa pasta nova: nunca sobrescreve arquivos existentes.
        juce::String folderName = request.prefix + " - faixas";
        if (request.rangeEnd >= 0)
            folderName << " " << timeText (request.rangeStart, engine.getSampleRate()).replaceCharacter (':', '.')
                       << " a " << timeText (request.rangeEnd, engine.getSampleRate()).replaceCharacter (':', '.');
        request.folder = parent.getNonexistentChildFile (juce::File::createLegalFileName (folderName), {}, true);

        struct Result { bool ok = false; juce::String error; juce::StringArray written, clipped; };
        auto res = std::make_shared<Result>();
        closeAllEditors();
        runJob ("Exportando " + juce::String ((int) (request.tracks.size() + request.buses.size())) + " faixas...", true,
                [this, res, request] (Job& j)
                { res->ok = engine.exportStems (request, j.progress, j.cancel, res->error, res->written, res->clipped); },
                [this, res, folder = request.folder] (Job&)
        {
            refreshStrips();
            if (! res->ok)
            {
                alert ("Exportar faixas", res->error);
                // trabalho cancelado/falhou: nao deixa pasta vazia para tras
                if (folder.isDirectory() && folder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
                    folder.deleteFile();
                return;
            }
            juce::String msg = juce::String (res->written.size()) + " arquivos exportados em:\n" + folder.getFullPathName();
            if (! res->clipped.isEmpty())
                msg << "\n\nPassaram de 0 dBFS (clipping): " << res->clipped.joinIntoString (", ")
                    << "\nUse 32 bits float ou abaixe o volume dessas faixas.";
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (res->clipped.isEmpty() ? juce::MessageBoxIconType::InfoIcon
                                                                                     : juce::MessageBoxIconType::WarningIcon)
                                              .withTitle ("Exportar faixas")
                                              .withMessage (msg)
                                              .withButton ("Abrir pasta")
                                              .withButton ("OK"),
                                          [folder] (int r) { if (r == 1) folder.startAsProcess(); });
        });
    });
}

void MainComponent::selectWholeSong()
{
    const auto len = juce::jmax (engine.getLengthSamples(), (juce::int64) (engine.getSampleRate() * 8));
    engine.setLoop (0, len);
}

void MainComponent::playSelection()
{
    if (! engine.hasSelection())
    {
        alert (juce::String::fromUTF8 ("Tocar sele\xc3\xa7\xc3\xa3o"),
               juce::String::fromUTF8 ("Arraste na r\xc3\xa9gua do tempo para selecionar um trecho."), false);
        return;
    }
    const auto ls = engine.loopStart.load(), le = engine.loopEnd.load();
    engine.playing = false;
    playStart = ls;
    engine.stopReturn = ls;
    engine.stopAt = engine.loopOn.load() ? -1 : le; // com loop, repete; sem loop, para no fim
    engine.position = ls;
    engine.playing = true;
}

void MainComponent::timelineMenu (juce::Component&)
{
    const double sr = engine.getSampleRate();
    const bool hasSel = engine.hasSelection();
    const auto ls = engine.loopStart.load(), le = engine.loopEnd.load();
    juce::PopupMenu m;
    m.addSectionHeader (hasSel ? juce::String::fromUTF8 ("Sele\xc3\xa7\xc3\xa3o ") + timeText (ls, sr) + " - " + timeText (le, sr)
                               : juce::String::fromUTF8 ("Nenhum trecho (arraste na r\xc3\xa9gua para selecionar)"));
    m.addItem (1, juce::String::fromUTF8 ("Tocar sele\xc3\xa7\xc3\xa3o (Shift+Espa\xc3\xa7o, duplo clique)"), hasSel);
    m.addItem (2, juce::String::fromUTF8 ("Repetir a sele\xc3\xa7\xc3\xa3o - loop (L)"), hasSel, engine.loopOn.load());
    m.addSeparator();
    juce::PopupMenu mix;
    mix.addItem (124, "WAV 24 bits");
    mix.addItem (116, "WAV 16 bits (com dither)");
    mix.addItem (132, "WAV 32 bits float");
    m.addSubMenu (juce::String::fromUTF8 ("Exportar mix da sele\xc3\xa7\xc3\xa3o"), mix, hasSel);
    m.addItem (3, juce::String::fromUTF8 ("Exportar faixas individuais da sele\xc3\xa7\xc3\xa3o..."), hasSel);
    m.addSeparator();
    m.addItem (4, juce::String::fromUTF8 ("Selecionar a m\xc3\xbasica toda"));
    m.addItem (5, juce::String::fromUTF8 ("Selecionar do in\xc3\xad" "cio at\xc3\xa9 o cursor"), engine.position.load() > 0);
    m.addItem (6, juce::String::fromUTF8 ("Limpar sele\xc3\xa7\xc3\xa3o (Esc)"), hasSel);
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this, ls, le] (int r)
    {
        switch (r)
        {
            case 1: playSelection(); break;
            case 2: loopButton.triggerClick(); break;
            case 3: stemsDialog (true); break;
            case 4: selectWholeSong(); markDirty(); break;
            case 5: engine.setLoop (0, engine.position.load()); markDirty(); break;
            case 6: engine.clearSelection(); markDirty(); break;
            case 124: case 116: case 132: exportMixTo (r % 100, ls, le); break;
            default: break;
        }
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
        openedRecoveryFile = juce::File();
        currentFile = juce::File();
        unsavedRecoveryId = juce::Uuid().toString();
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
        // Nao zera "dirty" aqui: se a abertura falhar ou for cancelada, o projeto atual continua
        // marcado como alterado e o aviso ao sair nao se perde.
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult() != juce::File()) openFile (fc.getResult(), true); });
    });
}

void MainComponent::openMenu()
{
    if (job != nullptr) return;
    // remove da lista os projetos que nao existem mais
    recentFiles.removeNonExistentFiles();
    if (recentFiles.getNumFiles() == 0) { openProjectDialog(); return; }

    juce::PopupMenu m;
    m.addItem (1, "Abrir projeto... (Ctrl+O)");
    m.addSeparator();
    m.addSectionHeader ("Recentes");
    recentFiles.createPopupMenuItems (m, 100, false, true);
    m.addSeparator();
    m.addItem (2, "Limpar lista de recentes");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&openButton), [this] (int r)
    {
        if (r == 1) openProjectDialog();
        else if (r == 2)
        {
            recentFiles.clear();
            props.getUserSettings()->setValue ("recentProjects", recentFiles.toString());
            props.getUserSettings()->saveIfNeeded();
        }
        else if (r >= 100)
        {
            const auto f = recentFiles.getFile (r - 100);
            if (f.existsAsFile()) openFile (f);
            else alert ("Abrir", juce::String::fromUTF8 ("O arquivo n\xc3\xa3o existe mais:\n") + f.getFullPathName());
        }
    });
}

void MainComponent::addRecentFile (const juce::File& f)
{
    recentFiles.addFile (f);
    props.getUserSettings()->setValue ("recentProjects", recentFiles.toString());
    props.getUserSettings()->saveIfNeeded();
}

void MainComponent::openFile (const juce::File& file, bool discardConfirmed)
{
    if (job != nullptr) return;
    if (! file.hasFileExtension (kProjectExtension))
    {
        if (isAudioFile (file)) importFiles ({ file });
        return;
    }
    if (dirty && ! discardConfirmed) { confirmDiscard ([this, file] { openFile (file, true); }); return; }

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
            engine.setSendCount (*t, (int) td.sends.size());
            for (int s = 0; s < (int) td.sends.size() && s < t->sends.size(); ++s)
            {
                t->sends[(size_t) s].bus = td.sends[(size_t) s].bus;
                t->sends[(size_t) s].levelDb = td.sends[(size_t) s].levelDb;
                t->sends[(size_t) s].preFader = td.sends[(size_t) s].pre;
            }
            auto* raw = engine.addTrack (std::move (t));
            if (d.stream != nullptr) engine.setTrackStream (*raw, d.stream);
            else if (d.audio.ok()) engine.setTrackAudio (*raw, d.audio.original, d.audio.rate, d.playback, sr);
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

        // Snapshots sao recuperados como projetos nao salvos, evitando que
        // Ctrl+S sobrescreva a unica copia de recuperacao.
        if (file.getParentDirectory() == recoveryDirectory())
        {
            openedRecoveryFile = file;
            currentFile = juce::File();
            dirty = true;
        }
        else
        {
            openedRecoveryFile = juce::File();
            currentFile = file;
            dirty = false;
            addRecentFile (file);
        }
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
    // Limpar apenas apos confirmar que a gravacao normal terminou bem.
    const auto oldSnapshot = recoveryFileFor (currentFile);
    const auto newSnapshot = recoveryFileFor (f);
    if (oldSnapshot != f) oldSnapshot.deleteFile();
    if (newSnapshot != f) newSnapshot.deleteFile();
    if (openedRecoveryFile != juce::File() && openedRecoveryFile != f)
        openedRecoveryFile.deleteFile();
    openedRecoveryFile = juce::File();
    currentFile = f;
    dirty = false;
    lastAutosaveMs = juce::Time::currentTimeMillis();
    addRecentFile (f);
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
    if (job != nullptr)
    {
        // cancela a tarefa (se der) e fecha quando ela terminar
        job->cancel = true;
        quitAfterJob = true;
        return;
    }
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
