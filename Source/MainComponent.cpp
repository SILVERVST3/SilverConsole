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
    constexpr int kTopBarH = 72, kToolbarH = 38;
    int labelWidth (const juce::String& name) { return juce::jlimit (16, 140, 10 + 6 * name.length()); }

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
        juce::File frozenFile;                  // faixa congelada no projeto
        std::shared_ptr<StreamingAudio> frozen; // aberto junto com o audio
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
    auto base = on ? colours::accentDeep : colours::gun;
    if (down) base = base.darker (0.2f); else if (over) base = base.brighter (0.1f);
    auto topC = base.brighter (0.28f), botC = base.darker (0.35f);
    if (down) std::swap (topC, botC);
    g.setGradientFill ({ topC, 0, r.getY(), botC, 0, r.getBottom(), false });
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colours::white.withAlpha (down ? 0.04f : 0.16f));
    g.drawHorizontalLine ((int) r.getY() + 1, r.getX() + 3.0f, r.getRight() - 3.0f);
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);

    auto icon = r.withSizeKeepingCentre (14.0f, 14.0f);
    g.setColour (on ? juce::Colours::white : colours::silverTop);
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
        case Shape::undo:
        case Shape::redo:
        {
            // arco aberto embaixo, ponta da seta descendo a esquerda (refazer = espelhado)
            const float cx = icon.getCentreX(), cy = icon.getCentreY() + 1.5f, rad = 5.0f;
            juce::Path arc;
            arc.addCentredArc (cx, cy, rad, rad, 0.0f, -1.45f, 1.9f, true);
            const float ex = cx + rad * std::sin (-1.45f), ey = cy - rad * std::cos (-1.45f);
            p.addTriangle (ex - 3.8f, ey - 1.0f, ex + 3.8f, ey - 1.0f, ex, ey + 4.0f);
            const auto flip = shape == Shape::redo ? juce::AffineTransform::scale (-1.0f, 1.0f, cx, cy) : juce::AffineTransform();
            g.strokePath (arc, juce::PathStrokeType (2.0f), flip);
            p.applyTransform (flip);
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
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (r, 4.0f);
    g.setGradientFill ({ juce::Colour (0xff0b1a36), 0, r.getY(), colours::lcd, 0, r.getBottom(), false });
    g.fillRoundedRectangle (r.reduced (1.0f), 3.0f);
    const double sr = engine.getSampleRate();
    const double total = visibleSeconds();
    const auto len = engine.getLengthSamples();

    // regiao com audio
    g.setColour (colours::accentDeep);
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
            g.drawText (label, lr.reduced (6.0f, 0.0f).withTop (r.getY() + 15.0f).withHeight (12.0f).toNearestInt(),
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
        g.setColour (colours::grid.brighter (0.4f));
        g.drawVerticalLine ((int) x, r.getY() + 16.0f, r.getBottom() - 8.0f);
        const int m = (int) (t / 60.0);
        g.setColour (colours::textDim);
        g.drawText (juce::String (m) + ":" + juce::String ((int) t % 60).paddedLeft ('0', 2), (int) x + 3, 2, 40, 13,
                    juce::Justification::left);
    }

    // regioes com nome (faixa azul) e marcadores (bandeira laranja)
    if (regions != nullptr)
        for (auto& rg : *regions)
        {
            const float x0 = sampleToX ((juce::int64) (rg.start * sr)), x1 = sampleToX ((juce::int64) (rg.end * sr));
            if (x1 < r.getX() || x0 > r.getRight()) continue;
            auto band = juce::Rectangle<float>::leftTopRightBottom (x0, r.getY() + 28.0f, juce::jmax (x0 + 3.0f, x1), r.getY() + 37.0f);
            g.setColour (colours::accentDeep.withAlpha (0.9f));
            g.fillRoundedRectangle (band, 2.0f);
            g.setColour (colours::accent);
            g.drawRoundedRectangle (band, 2.0f, 1.0f);
            g.setColour (juce::Colours::white);
            g.setFont (uiFont (9.0f, true));
            g.drawText (rg.name, band.reduced (3.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, true);
        }
    if (markers != nullptr)
        for (auto& mk : *markers)
        {
            const float x = sampleToX ((juce::int64) (mk.t * sr));
            if (x < r.getX() - 2.0f || x > r.getRight()) continue;
            g.setColour (colours::mute.withAlpha (0.75f));
            g.drawVerticalLine ((int) x, r.getY() + 15.0f, r.getBottom() - 2.0f);
            auto flag = juce::Rectangle<float> (x, r.getY() + 15.0f, (float) labelWidth (mk.name), 12.0f);
            g.setColour (colours::mute);
            g.fillRoundedRectangle (flag, 2.0f);
            g.setColour (colours::ink);
            g.setFont (uiFont (9.5f, true));
            g.drawText (mk.name, flag.reduced (3.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, true);
        }

    const float px = sampleToX (engine.position.load());
    g.setColour (colours::text);
    g.fillRect (px - 1.0f, r.getY() + 1.0f, 2.0f, r.getHeight() - 2.0f);
    juce::Path head;
    head.addTriangle (px - 6.0f, r.getY(), px + 6.0f, r.getY(), px, r.getY() + 7.0f);
    g.fillPath (head);
}

int Timeline::markerAt (const juce::MouseEvent& e) const
{
    if (markers == nullptr || e.y < 15 || e.y > 27) return -1;
    const double sr = engine.getSampleRate();
    for (int i = (int) markers->size(); --i >= 0;)
    {
        const float x = sampleToX ((juce::int64) ((*markers)[(size_t) i].t * sr));
        if (e.x >= x - 3.0f && e.x <= x + (float) labelWidth ((*markers)[(size_t) i].name)) return i;
    }
    return -1;
}

int Timeline::regionAt (const juce::MouseEvent& e) const
{
    if (regions == nullptr || e.y < 28 || e.y > 37) return -1;
    const double sr = engine.getSampleRate();
    for (int i = (int) regions->size(); --i >= 0;)
    {
        const auto& rg = (*regions)[(size_t) i];
        if (e.x >= sampleToX ((juce::int64) (rg.start * sr)) && e.x <= sampleToX ((juce::int64) (rg.end * sr))) return i;
    }
    return -1;
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
    if (const int mk = markerAt (e); mk >= 0)
    {
        if (e.mods.isPopupMenu()) { if (onMarkerMenu) onMarkerMenu (mk); }
        else if (onMarkerClick) onMarkerClick (mk);
        return;
    }
    if (const int rg = regionAt (e); rg >= 0)
    {
        if (e.mods.isPopupMenu()) { if (onRegionMenu) onRegionMenu (rg); }
        else if (onRegionClick) onRegionClick (rg);
        return;
    }
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
    if (const int rg = regionAt (e); rg >= 0) { if (onRegionDoubleClick) onRegionDoubleClick (rg); return; }
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
    timeline.markers = &markers;
    timeline.regions = &regions;
    timeline.onMarkerClick = [this] (int i)
    {
        if (! juce::isPositiveAndBelow (i, (int) markers.size())) return;
        engine.stopAt = -1;
        engine.position = (juce::int64) (markers[(size_t) i].t * engine.getSampleRate());
    };
    timeline.onMarkerMenu = [this] (int i) { markerMenu (i); };
    timeline.onRegionClick = [this] (int i) { selectRegion (i); };
    timeline.onRegionMenu = [this] (int i) { regionMenu (i); };
    timeline.onRegionDoubleClick = [this] (int i) { selectRegion (i); playSelection(); };

    auto setupTool = [this] (juce::TextButton& b, const juce::String& tip)
    {
        b.setWantsKeyboardFocus (false);
        b.setTooltip (tip);
        addAndMakeVisible (b);
    };
    setupTool (newButton, "Novo projeto (Ctrl+N)");
    setupTool (openButton, "Abrir projeto .sconsole (Ctrl+O)");
    setupTool (saveButton, "Salvar (Ctrl+S) | Shift+clique: Salvar como");
    addAndMakeVisible (undoButton);
    addAndMakeVisible (redoButton);
    undoButton.onClick = [this] { undoOrRedo (false); };
    redoButton.onClick = [this] { undoOrRedo (true); };
    updateUndoButtons();
    setupTool (importButton, "Importar arquivos de audio como faixas (Ctrl+I). Tambem da para arrastar arquivos para a janela.");
    setupTool (addBusButton, "Criar bus auxiliar (reverb, delay, grupo)");
    setupTool (exportButton, "Exportar a mix para WAV (Ctrl+E)");
    setupTool (pluginsButton, "Escanear e gerenciar plugins VST3");
    setupTool (audioButton, "Configurar a placa de som (saida e buffer)");
    setupTool (automationButton, juce::String::fromUTF8 ("Editor de automa\xc3\xa7\xc3\xa3o (Tab). Clique de novo para voltar ao mixer."));
    automationButton.setClickingTogglesState (true);
    automationButton.onClick = [this] { setAutomationVisible (automationButton.getToggleState()); };
    exportButton.setColour (juce::TextButton::buttonColourId, colours::accentDeep);

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
    mixerContent.addChildComponent (dropMarker);
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
    engine.setCpuSaver (props.getUserSettings()->getBoolValue ("cpuSaver", true));
    normalization.mode = (Engine::Normalize::Mode) juce::jlimit (0, 3, props.getUserSettings()->getIntValue ("normMode", 0));
    normalization.target = (float) props.getUserSettings()->getDoubleValue ("normTarget", -14.0);
    normalization.truePeakCeiling = (float) props.getUserSettings()->getDoubleValue ("normCeiling", -1.0);
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
    undoManager.clearUndoHistory(); // plugins/faixas guardados no historico saem antes do motor
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
    // barra superior chumbo (como o cabecalho do SILVER AXES)
    g.setGradientFill ({ juce::Colour (0xff30353c), 0, 0, juce::Colour (0xff1a1d21), 0, (float) kTopBarH, false });
    g.fillRect (top);
    g.setGradientFill ({ juce::Colour (0xff1a1d21), 0, (float) kTopBarH, juce::Colour (0xff121417), 0, (float) (kTopBarH + kToolbarH), false });
    g.fillRect (0, kTopBarH, getWidth(), kToolbarH);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawHorizontalLine (1, 0, (float) getWidth());
    g.setColour (juce::Colours::black);
    g.drawHorizontalLine (kTopBarH - 1, 0, (float) getWidth());
    g.drawHorizontalLine (kTopBarH + kToolbarH - 1, 0, (float) getWidth());

    // logo
    g.setColour (colours::silverTop);
    g.setFont (uiFont (21.0f, true));
    g.drawText ("SILVER", 16, 14, 90, 26, juce::Justification::left);
    g.setColour (colours::accent);
    g.drawText ("CONSOLE", 96, 14, 110, 26, juce::Justification::left);
    g.setColour (colours::textDim);
    g.setFont (uiFont (10.5f, true));
    g.drawText ("MIX  /  VST3 HOST", 17, 40, 180, 14, juce::Justification::left);

    // relogio
    auto clock = juce::Rectangle<int> (382, 16, 150, 40);
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (clock.toFloat().expanded (1.0f), 5.0f);
    g.setGradientFill ({ juce::Colour (0xff0e2147), 0, (float) clock.getY(), colours::lcd, 0, (float) clock.getBottom(), false });
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
    tools.removeFromLeft (4);
    for (auto* b : { &undoButton, &redoButton })
    {
        b->setBounds (tools.removeFromLeft (34));
        tools.removeFromLeft (3);
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
    if (! vcaStrips.isEmpty()) x += 12; // separa os VCAs
    for (auto* v : vcaStrips)
    {
        v->setBounds (x, 0, ChannelStrip::width, h);
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
    if (tickCount % 6 == 0) for (auto* v : vcaStrips) v->tick();
    if (showAutomation) automationView->tick();
    timeline.repaint();
    repaint (382, 16, 150, 40);
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
            s << " (" << juce::String (workers + 1) << juce::String::fromUTF8 (workers > 0 ? " n\xc3\xba" "cleos" : " n\xc3\xba" "cleo");
            // economia de CPU: canais com plugins parados agora (silencio)
            if (engine.isCpuSaver()) s << ", " << juce::String (engine.getSleepingCount()) << " dormindo";
            s << ")";
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
    if (k == juce::KeyPress ('m')) { addMarkerAtCursor(); return true; }
    if (k == juce::KeyPress ('r')) { addRegionFromSelection(); return true; }
    if (k == juce::KeyPress (']')) { jumpToMarker (true); return true; }
    if (k == juce::KeyPress ('[')) { jumpToMarker (false); return true; }
    if (k == juce::KeyPress::tabKey) { automationButton.triggerClick(); return true; }
    if (k == juce::KeyPress ('z', cmd, 0)) { undoOrRedo (false); return true; }
    if (k == juce::KeyPress ('y', cmd, 0) || k == juce::KeyPress ('z', cmd | juce::ModifierKeys::shiftModifier, 0)) { undoOrRedo (true); return true; }
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
        snapshot->markers = markers;
        snapshot->regions = regions;
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
    vcaStrips.clear();
    for (int g = 0; g < kMaxGroups; ++g)
        if (engine.vca[(size_t) g].shown) mixerContent.addAndMakeVisible (vcaStrips.add (new VcaStrip (g, *this)));
    masterStrip->refresh();
    if (automationView != nullptr) automationView->rebuild();
    resized();
    repaint();
}

void MainComponent::stripDrag (Channel& c, const juce::MouseEvent& e, int phase)
{
    const int numTracks = (int) engine.tracks.size();
    int from = -1;
    for (int i = 0; i < numTracks; ++i)
        if (engine.tracks[(size_t) i].get() == &c) from = i;
    if (from < 0 || numTracks > strips.size()) { dropMarker.setVisible (false); return; }

    if (phase == 1)
    {
        const auto v = mixerViewport.getLocalPoint (nullptr, e.getScreenPosition());
        mixerViewport.autoScroll (v.x, v.y, 40, 14); // perto da borda: rola o mixer
    }
    const auto p = mixerContent.getLocalPoint (nullptr, e.getScreenPosition());
    int target = 0;
    for (int i = 0; i < numTracks; ++i)
        if (p.x > strips[i]->getBounds().getCentreX()) target = i + 1;

    if (phase == 2)
    {
        dropMarker.setVisible (false);
        const int to = target > from ? target - 1 : target;
        if (to == from) return;
        markDirty();
        // a faixa que recebeu o mouseUp e recriada: so depois deste evento
        juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<MainComponent> (this), from, to]
                                         { if (sp != nullptr) sp->moveTrackUndoable (from, to); });
        return;
    }
    const int mx = target < numTracks ? strips[target]->getX() - 2 : strips[numTracks - 1]->getRight() + 1;
    dropMarker.setBounds (mx - 1, 0, 3, mixerContent.getHeight());
    dropMarker.setVisible (true);
    dropMarker.toFront (false);
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
    if (b != nullptr) { clearUndo(); rebuildStrips(); markDirty(); } // indices de bus mudam: historico recomeca
    return b;
}

void MainComponent::removeChannel (Channel& c)
{
    closeEditorsFor (c);
    if (c.kind == ChannelKind::track)
    {
        // a faixa (com plugins e ajustes) fica guardada no historico
        struct Holder { Track* ptr = nullptr; int index = 0; std::unique_ptr<Track> stored; };
        auto h = std::make_shared<Holder>();
        h->ptr = static_cast<Track*> (&c);
        for (int i = 0; i < (int) engine.tracks.size(); ++i)
            if (engine.tracks[(size_t) i].get() == h->ptr) h->index = i;
        const auto name = c.name;
        auto removeIt = [this, h]
        {
            if (! channelExists (h->ptr)) return;
            closeEditorsFor (*h->ptr);
            h->stored = engine.removeTrack (h->ptr);
            rebuildStrips();
        };
        removeIt();
        recordUndo ("Remover faixa " + name, removeIt, [this, h]
        {
            if (h->stored == nullptr) return;
            engine.insertTrack (h->index, std::move (h->stored));
            rebuildStrips();
        }, 50);
    }
    else if (c.kind == ChannelKind::bus)
    {
        clearUndo();
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
    {
        const bool frozen = c.kind == ChannelKind::track && static_cast<Track&> (c).isFrozen();
        bool anyLoaded = false, anyOffline = false;
        for (auto& s : c.inserts)
        {
            anyLoaded = anyLoaded || s.processor != nullptr || s.remote != nullptr;
            anyOffline = anyOffline || s.isOffline();
        }
        m.addSeparator();
        if (c.kind == ChannelKind::track)
        {
            if (frozen) m.addItem (21, "Descongelar faixa (plugins voltam online)");
            else m.addItem (20, juce::String::fromUTF8 ("Congelar faixa (grava os plugins em \xc3\xa1udio e libera a CPU)"), anyLoaded);
        }
        m.addItem (22, juce::String::fromUTF8 ("Todos os plugins offline (sem CPU/mem\xc3\xb3ria)"), anyLoaded && ! frozen);
        juce::PopupMenu groupMenu;
        const int cur = c.group.load();
        groupMenu.addItem (300, "Nenhum", true, cur < 0);
        for (int g = 0; g < kMaxGroups; ++g)
            groupMenu.addColouredItem (301 + g, "Grupo " + juce::String (g + 1), groupColour (g), true, cur == g);
        groupMenu.addSeparator();
        if (cur >= 0)
            groupMenu.addItem (320 + cur, "Mostrar VCA do grupo " + juce::String (cur + 1), true, engine.vca[(size_t) cur].shown);
        groupMenu.addItem (330, "(Alt+arrastar o fader: move so esta faixa)", false);
        m.addSubMenu ("Grupo (faders, mute e solo juntos)", groupMenu);
        m.addItem (23, "Todos os plugins online", anyOffline && ! frozen);
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
        else if (r >= 100 && r < 110) // so os itens de cor (grupos usam 300+)
        {
            const auto before = cp->colour, after = colours::trackPalette (r - 100);
            auto setColour = [this, cp] (juce::Colour c) { if (! channelExists (cp)) return; cp->colour = c; if (auto* s = stripFor (cp)) s->repaint(); };
            setColour (after);
            recordUndo ("Cor - " + cp->name, [setColour, after] { setColour (after); }, [setColour, before] { setColour (before); });
            markDirty();
        }
        else if (r == 2) replaceAudio (*static_cast<Track*> (cp));
        else if (r == 3 || r == 4)
        {
            for (int i = 0; i < (int) engine.tracks.size(); ++i)
                if (engine.tracks[(size_t) i].get() == cp)
                {
                    const int to = juce::jlimit (0, (int) engine.tracks.size() - 1, i + (r == 3 ? -1 : 1));
                    if (to != i) moveTrackUndoable (i, to);
                    break;
                }
            markDirty();
        }
        else if (r >= 300 && r <= 308) setChannelGroup (*cp, r - 301);
        else if (r >= 320 && r < 320 + kMaxGroups) setVcaShown (r - 320, ! engine.vca[(size_t) (r - 320)].shown);
        else if (r == 20) freezeTrack (*static_cast<Track*> (cp));
        else if (r == 21) unfreezeTrack (*static_cast<Track*> (cp));
        else if (r == 22) setChannelPluginsOffline (*cp);
        else if (r == 23)
        {
            juce::StringArray failed;
            restoreOfflineInserts (*cp, -1, false, failed);
            if (! failed.isEmpty()) alert ("Plugins online", juce::String::fromUTF8 ("N\xc3\xa3o foi poss\xc3\xadvel recarregar (ficaram offline):\n") + failed.joinIntoString ("\n"));
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
    m.addItem (3, "Pre-fader (independe do volume da faixa)", s.bus.load() >= 0, s.preFader.load() && ! s.preFx.load());
    m.addItem (5, "Pre-FX (antes dos plugins, independe do volume)", s.bus.load() >= 0, s.preFx.load());
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
        const auto before = sendStateOf (snd);
        if (r == 4)
        {
            snd.bus = -1;
            setSendSlots (*tp, tp->sends.size() - 1);
            return;
        }
        if (r == 1) snd.bus = -1;
        else if (r == 2) { if (auto* b = createBus()) snd.bus = engine.indexOfBus (b); }
        else if (r == 3) { const bool on = ! (snd.preFader.load() && ! snd.preFx.load()); snd.preFx = false; snd.preFader = on; }
        else if (r == 5) { const bool on = ! snd.preFx.load(); snd.preFx = on; snd.preFader = false; }
        else snd.bus = r - 10;
        if (r == 2) clearUndo(); // criou bus
        else
        {
            const auto after = sendStateOf (snd);
            recordUndo ("Envio " + juce::String (send + 1) + " - " + tp->name,
                        [this, tp, send, after] { applySendState (tp, send, after); },
                        [this, tp, send, before] { applySendState (tp, send, before); });
        }
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
        const int before = tp->outputBus.load();
        if (r == 1) tp->outputBus = -1;
        else if (r == 2) { if (auto* b = createBus()) tp->outputBus = engine.indexOfBus (b); }
        else tp->outputBus = r - 10;
        const int after = tp->outputBus.load();
        if (r != 2 && before != after)
        {
            auto setOut = [this, tp] (int v) { if (! channelExists (tp)) return; tp->outputBus = v; refreshStrips(); };
            recordUndo ("Saida - " + tp->name, [setOut, after] { setOut (after); }, [setOut, before] { setOut (before); });
        }
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
    if (c.kind == ChannelKind::track && static_cast<Track&> (c).isFrozen())
    {
        // Congelada: os plugins estao gravados no audio. Nada muda ate descongelar.
        juce::PopupMenu m;
        m.addSectionHeader ("Faixa congelada - " + c.name);
        m.addItem (1, "Descongelar faixa (plugins voltam online)");
        auto* tp = static_cast<Track*> (&c);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source), [this, tp] (int r)
        {
            if (r == 1 && channelExists (tp)) unfreezeTrack (*tp);
        });
        return;
    }
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
    auto recordBypass = [this] (Channel* ch, int sl, bool before)
    {
        auto setB = [this, ch, sl] (bool v) { if (! channelExists (ch) || ! ch->hasInsert (sl)) return; engine.setBypass (*ch, sl, v); if (auto* st = stripFor (ch)) st->refresh(); };
        recordUndo ("Bypass - " + ch->inserts[(size_t) sl].getName(), [setB, before] { setB (! before); }, [setB, before] { setB (before); });
    };
    if (e.mods.isShiftDown() && ! e.mods.isPopupMenu())
    {
        const bool before = s.bypassed.load();
        engine.setBypass (c, slot, ! before);
        recordBypass (&c, slot, before);
        source.repaint();
        markDirty();
        return;
    }
    if (! e.mods.isPopupMenu() && s.processor != nullptr) { openEditor (c, slot); return; }

    juce::PopupMenu m;
    m.addSectionHeader (s.getName() + (s.isMissing() ? juce::String::fromUTF8 (" (n\xc3\xa3o encontrado)")
                                     : s.isOffline() ? juce::String (" (offline)") : juce::String()));
    if (s.isOffline())
        m.addItem (8, "Colocar online");
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
    if (s.processor != nullptr || s.remote != nullptr)
    {
        m.addItem (10, "Mix dry/wet... (" + juce::String (juce::roundToInt (s.mix.load() * 100.0f)) + "%)");
        juce::PopupMenu sc;
        sc.addItem (400, "Nenhum", true, s.sidechain == nullptr);
        for (int i = 0; i < (int) engine.tracks.size(); ++i)
            if (engine.tracks[(size_t) i].get() != &c)
                sc.addItem (401 + i, engine.tracks[(size_t) i]->name, true, s.sidechain == engine.tracks[(size_t) i].get());
        m.addSubMenu (Engine::supportsSidechain (s) ? juce::String ("Sidechain (entrada lateral)")
                                                    : juce::String ("Sidechain (este plugin nao tem entrada lateral)"),
                      sc, Engine::supportsSidechain (s));
        m.addItem (7, juce::String::fromUTF8 ("Colocar offline (libera CPU e mem\xc3\xb3ria, guarda o ajuste)"));
    }
    m.addSeparator();
    m.addItem (3, "Substituir...");
    m.addItem (4, "Mover para cima", slot > 0);
    m.addItem (5, "Mover para baixo", slot < c.inserts.size() - 1);
    m.addSeparator();
    m.addItem (9, "Remover");
    auto* cp = &c;
    juce::Component::SafePointer<juce::Component> src (&source);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source), [this, cp, slot, src, recordBypass] (int r)
    {
        if (r == 0 || ! channelExists (cp)) return;
        auto& sl = cp->inserts[(size_t) slot];
        if (r == 1) openEditor (*cp, slot);
        else if (r == 10) { if (src != nullptr) showInsertMix (*cp, slot, *src); return; }
        else if (r >= 400 && r < 400 + 1 + kMaxTracks)
        {
            Track* source = r == 400 ? nullptr
                          : juce::isPositiveAndBelow (r - 401, (int) engine.tracks.size()) ? engine.tracks[(size_t) (r - 401)].get() : nullptr;
            Track* before = sl.sidechain;
            if (source == before) return;
            if (! engine.setSidechain (*cp, slot, source))
            {
                alert ("Sidechain", juce::String::fromUTF8 ("O plugin n\xc3\xa3o aceitou a entrada lateral."));
                return;
            }
            auto setSc = [this, cp, slot] (Track* t)
            {
                if (! channelExists (cp) || ! cp->hasInsert (slot) || (t != nullptr && ! channelExists (t))) return;
                engine.setSidechain (*cp, slot, t);
                if (auto* st = stripFor (cp)) st->refresh();
            };
            recordUndo ("Sidechain - " + sl.getName(), [setSc, source] { setSc (source); }, [setSc, before] { setSc (before); });
        }
        else if (r == 7) setInsertOffline (*cp, slot);
        else if (r == 8)
        {
            juce::StringArray failed;
            restoreOfflineInserts (*cp, slot, false, failed);
            if (! failed.isEmpty()) alert ("Plugin", juce::String::fromUTF8 ("N\xc3\xa3o foi poss\xc3\xadvel recarregar:\n") + failed.joinIntoString ("\n"));
        }
        else if (r == 2)
        {
            const bool before = sl.bypassed.load();
            engine.setBypass (*cp, slot, ! before);
            recordBypass (cp, slot, before);
        }
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
        else if (r == 4 || r == 5)
        {
            const int other = slot + (r == 4 ? -1 : 1);
            auto swapIt = [this, cp, slot, other]
            {
                if (! channelExists (cp) || ! cp->hasInsert (slot) || ! cp->hasInsert (other)) return;
                engine.swapInserts (*cp, slot, other);
                lanesChanged();
                if (auto* st = stripFor (cp)) st->refresh();
            };
            swapIt();
            recordUndo ("Mover insert - " + cp->name, swapIt, swapIt);
        }
        else if (r == 9)
        {
            // o plugin sai do slot e fica no historico (desfazer devolve com o ajuste e a automacao)
            swapSlotUndoable (*cp, slot, Engine::SlotContent {}, "Remover " + sl.getName());
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
    Engine::SlotContent incoming;
    incoming.processor = std::move (inst);
    incoming.description = d;
    incoming.hasDescription = true;
    swapSlotUndoable (c, slot, std::move (incoming), (hadPlugin ? "Substituir por " : "Inserir ") + d.name);
    engine.removeLanesForSlot (c, slot); // parametros de outro plugin neste slot nao valem mais
    lanesChanged();
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
    Engine::SlotContent incoming;
    incoming.missingState = remote->getInitialState();
    incoming.remote = std::move (remote);
    incoming.description = d;
    incoming.hasDescription = true;
    incoming.preferRemote = true;
    swapSlotUndoable (c, slot, std::move (incoming), (hadPlugin ? "Substituir por " : "Inserir ") + d.name + " (isolado)");
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
        if (in.offline)
        {
            // Offline no projeto: so o estado; o plugin nao e carregado (sem CPU/memoria).
            engine.setMissingInsert (c, in.slot, in.desc, in.state, in.isolated, true);
            if (in.bypass) engine.setBypass (c, in.slot, true);
            c.inserts[(size_t) in.slot].mix = in.mix;
                continue;
        }
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
                c.inserts[(size_t) in.slot].mix = in.mix;
                continue;
            }
            engine.setMissingInsert (c, in.slot, in.desc, in.state, true);
            c.inserts[(size_t) in.slot].mix = in.mix;
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
                c.inserts[(size_t) in.slot].mix = in.mix;
                continue;
            }
        }
        engine.setMissingInsert (c, in.slot, in.desc, in.state);
        if (in.bypass) engine.setBypass (c, in.slot, true); // preserva o bypass ao salvar de novo
        c.inserts[(size_t) in.slot].mix = in.mix;
        problems.add (c.name + ": plugin " + in.desc.name + (err.isNotEmpty() ? " (" + err + ")" : juce::String()));
    }
}

//==============================================================================
// Plugin offline e congelar faixa
//==============================================================================
// Desfazer / refazer
namespace
{
    struct LambdaAction : juce::UndoableAction
    {
        LambdaAction (std::function<void()> r, std::function<void()> u, int sz) : redoFn (std::move (r)), undoFn (std::move (u)), size (sz) {}
        bool perform() override
        {
            if (skipFirst) { skipFirst = false; return true; } // a edicao ja foi feita pela interface
            redoFn();
            return true;
        }
        bool undo() override { undoFn(); return true; }
        int getSizeInUnits() override { return size; }
        std::function<void()> redoFn, undoFn;
        int size;
        bool skipFirst = true;
    };

    juce::String paramName (StripHost::Param p)
    {
        switch (p)
        {
            case StripHost::Param::gain:  return "Volume";
            case StripHost::Param::pan:   return "Pan";
            case StripHost::Param::mute:  return "Mute";
            case StripHost::Param::solo:  return "Solo";
            case StripHost::Param::phase: return "Fase";
            case StripHost::Param::mono:  return "Mono";
        }
        return {};
    }
}

void MainComponent::recordUndo (const juce::String& name, std::function<void()> redo, std::function<void()> undo, int units)
{
    lastValueEdit = {};
    undoManager.beginNewTransaction (name);
    undoManager.perform (new LambdaAction (std::move (redo), std::move (undo), units));
    updateUndoButtons();
}

void MainComponent::clearUndo()
{
    lastValueEdit = {};
    undoManager.clearUndoHistory();
    updateUndoButtons();
}

void MainComponent::undoOrRedo (bool redo)
{
    if (job != nullptr) return;
    lastValueEdit = {};
    if (redo ? undoManager.redo() : undoManager.undo())
    {
        markDirty();
        repaint();
        if (automationView != nullptr) automationView->repaint();
    }
    updateUndoButtons();
}

void MainComponent::updateUndoButtons()
{
    undoButton.setEnabled (undoManager.canUndo());
    redoButton.setEnabled (undoManager.canRedo());
    undoButton.setTooltip (undoManager.canUndo() ? "Desfazer: " + undoManager.getUndoDescription() + " (Ctrl+Z)" : juce::String ("Nada para desfazer"));
    redoButton.setTooltip (undoManager.canRedo() ? "Refazer: " + undoManager.getRedoDescription() + " (Ctrl+Y)" : juce::String ("Nada para refazer"));
}

void MainComponent::applyParam (Channel* c, Param p, float v)
{
    if (! channelExists (c)) return;
    switch (p)
    {
        case Param::gain:  c->gainDb = v; break;
        case Param::pan:   c->pan = v; break;
        case Param::mute:  c->mute = v > 0.5f; break;
        case Param::solo:  c->solo = v > 0.5f; break;
        case Param::phase: c->phase = v > 0.5f; break;
        case Param::mono:  engine.master.mono = v > 0.5f; break;
    }
    if (auto* s = stripFor (c)) s->refresh();
}

void MainComponent::liveValueChanged (Channel& c, Param p, float before, float after)
{
    // fader de um membro do grupo: os outros andam a mesma quantidade de dB
    if (p != Param::gain || before == after || juce::ModifierKeys::currentModifiers.isAltDown()) return;
    const int g = c.group.load();
    if (! juce::isPositiveAndBelow (g, kMaxGroups)) return;
    const float delta = after - before;
    auto move = [&] (Channel& m)
    {
        if (&m == &c || m.group.load() != g) return;
        m.gainDb = juce::jlimit (kMinDb, 12.0f, m.gainDb.load() + delta);
    };
    for (auto& t : engine.tracks) move (*t);
    for (auto& b : engine.buses) move (*b);
}

void MainComponent::valueEdited (Channel& c, Param p, float before, float after)
{
    if (before == after) return;
    auto* cp = &c;
    const int grp = c.group.load();
    if (juce::isPositiveAndBelow (grp, kMaxGroups) && (p == Param::gain || p == Param::mute || p == Param::solo)
        && ! juce::ModifierKeys::currentModifiers.isAltDown())
    {
        // grupo: mute/solo copiados para os membros; volume ja andou junto (liveValueChanged).
        // Uma acao de desfazer volta todos os membros.
        struct Change { Channel* ch; float before, after; };
        auto changes = std::make_shared<std::vector<Change>>();
        changes->push_back ({ cp, before, after });
        auto visit = [&] (Channel& m)
        {
            if (&m == cp || m.group.load() != grp) return;
            if (p == Param::gain)
            {
                const float now = m.gainDb.load();
                changes->push_back ({ &m, juce::jlimit (kMinDb, 12.0f, now - (after - before)), now });
            }
            else
            {
                auto& flag = p == Param::mute ? m.mute : m.solo;
                const float was = flag.load() ? 1.0f : 0.0f;
                flag = after > 0.5f;
                if (was != after) changes->push_back ({ &m, was, after });
            }
        };
        for (auto& t : engine.tracks) visit (*t);
        for (auto& b : engine.buses) visit (*b);
        refreshStrips();
        recordUndo (paramName (p) + " - grupo " + juce::String (grp + 1),
                    [this, changes, p] { for (auto& ch : *changes) applyParam (ch.ch, p, ch.after); },
                    [this, changes, p] { for (auto& ch : *changes) applyParam (ch.ch, p, ch.before); });
        return;
    }
    const auto now = juce::Time::getMillisecondCounter();
    // passos seguidos do mesmo controle (roda do mouse, digitacao) viram uma acao so
    if (lastValueEdit.target == cp && lastValueEdit.param == (int) p && now - lastValueEdit.time < 700
        && lastValueEdit.after != nullptr && undoManager.canUndo())
    {
        *lastValueEdit.after = after;
        lastValueEdit.time = now;
        return;
    }
    auto afterValue = std::make_shared<float> (after);
    recordUndo (paramName (p) + " - " + c.name,
                [this, cp, p, afterValue] { applyParam (cp, p, *afterValue); },
                [this, cp, p, before] { applyParam (cp, p, before); });
    lastValueEdit = { cp, (int) p, now, afterValue };
}

void MainComponent::vcaEdited (int group, bool mute, float before, float after)
{
    if (before == after || ! juce::isPositiveAndBelow (group, kMaxGroups)) return;
    auto setVca = [this, group, mute] (float v)
    {
        auto& vv = engine.vca[(size_t) group];
        if (mute) vv.mute = v > 0.5f;
        else vv.gainDb = v;
        for (auto* s : vcaStrips) if (s->group == group) s->refresh();
    };
    recordUndo ((mute ? "Mute VCA " : "VCA ") + juce::String (group + 1),
                [setVca, after] { setVca (after); }, [setVca, before] { setVca (before); });
}

void MainComponent::vcaMenu (int group, juce::Component& source)
{
    juce::PopupMenu m;
    m.addSectionHeader ("VCA " + juce::String (group + 1));
    m.addItem (1, "Esconder (volta o VCA para 0 dB)");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source), [this, group] (int r)
    {
        if (r == 1) setVcaShown (group, false);
    });
}

void MainComponent::setChannelGroup (Channel& c, int group)
{
    const int before = c.group.load();
    if (before == group) return;
    auto* cp = &c;
    auto setG = [this, cp] (int g)
    {
        if (! channelExists (cp)) return;
        cp->group = g;
        if (auto* s = stripFor (cp)) s->repaint();
        for (auto* v : vcaStrips) v->refresh();
    };
    setG (group);
    recordUndo ("Grupo - " + c.name, [setG, group] { setG (group); }, [setG, before] { setG (before); });
    markDirty();
}

void MainComponent::setVcaShown (int group, bool shown)
{
    auto& v = engine.vca[(size_t) group];
    if (v.shown == shown) return;
    v.shown = shown;
    if (! shown) { v.gainDb = 0.0f; v.mute = false; } // escondido nao pode ficar mexendo no volume
    clearUndo();
    rebuildStrips();
    markDirty();
}

MainComponent::SendState MainComponent::sendStateOf (const Send& s)
{
    return { s.bus.load(), s.levelDb.load(), s.preFader.load(), s.preFx.load() };
}

void MainComponent::applySendState (Track* t, int index, SendState st)
{
    if (! channelExists (t) || index >= t->sends.size()) return;
    auto& s = t->sends[(size_t) index];
    s.bus = st.bus;
    s.levelDb = st.level;
    s.preFader = st.pre;
    s.preFx = st.preFx;
    refreshStrips();
}

void MainComponent::sendLevelEdited (Track& t, int index, float before, float after)
{
    if (before == after) return;
    auto* tp = &t;
    const auto now = juce::Time::getMillisecondCounter();
    const int key = 1000 + index;
    if (lastValueEdit.target == tp && lastValueEdit.param == key && now - lastValueEdit.time < 700
        && lastValueEdit.after != nullptr && undoManager.canUndo())
    {
        *lastValueEdit.after = after;
        lastValueEdit.time = now;
        return;
    }
    auto afterValue = std::make_shared<float> (after);
    auto setLevel = [this, tp, index] (float v)
    {
        if (! channelExists (tp) || index >= tp->sends.size()) return;
        tp->sends[(size_t) index].levelDb = v;
        refreshStrips();
    };
    recordUndo ("Envio " + juce::String (index + 1) + " - " + t.name,
                [setLevel, afterValue] { setLevel (*afterValue); }, [setLevel, before] { setLevel (before); });
    lastValueEdit = { tp, key, now, afterValue };
}

void MainComponent::renamed (Channel& c, const juce::String& before)
{
    auto* cp = &c;
    const auto after = c.name;
    auto setName = [this, cp] (const juce::String& n)
    {
        if (! channelExists (cp)) return;
        cp->name = n;
        if (auto* s = stripFor (cp)) s->refresh();
        if (automationView != nullptr) automationView->repaint();
    };
    recordUndo ("Renomear " + before, [setName, after] { setName (after); }, [setName, before] { setName (before); });
}

void MainComponent::applyLanePoints (Channel* c, AutoTarget tg, int slot, int param, const std::vector<AutoPoint>& pts)
{
    if (! channelExists (c)) return;
    auto* l = engine.findLane (*c, tg, slot, param);
    if (l == nullptr)
    {
        if (pts.empty()) return;
        l = engine.getOrCreateLane (*c, tg, slot, param);
        lanesChanged();
    }
    engine.editAutomation ([&] { l->points = pts; });
    if (automationView != nullptr) automationView->repaint();
}

void MainComponent::automationEdited (Channel& c, AutoTarget tg, int slot, int param, const std::vector<AutoPoint>& before)
{
    std::vector<AutoPoint> after;
    if (auto* l = engine.findLane (c, tg, slot, param)) after = l->points;
    if (after == before) return;
    auto* cp = &c;
    recordUndo (juce::String::fromUTF8 ("Automa\xc3\xa7\xc3\xa3o - ") + c.name,
                [this, cp, tg, slot, param, after] { applyLanePoints (cp, tg, slot, param, after); },
                [this, cp, tg, slot, param, before] { applyLanePoints (cp, tg, slot, param, before); },
                1 + (int) ((before.size() + after.size()) / 200));
}

void MainComponent::swapSlotUndoable (Channel& c, int slot, Engine::SlotContent incoming, const juce::String& name)
{
    auto holder = std::make_shared<Engine::SlotContent> (std::move (incoming));
    auto* cp = &c;
    auto doSwap = [this, cp, slot, holder]
    {
        if (! channelExists (cp)) return;
        closeEditor (*cp, slot);
        if (auto* p = holder->processor.get()) // voltou depois de mudar a taxa/buffer?
            if (p->getSampleRate() != engine.getSampleRate() || p->getBlockSize() != engine.getBlockSize())
                engine.prepareProcessor (*p);
        engine.swapSlotContent (*cp, slot, *holder);
        if (auto* st = stripFor (cp)) { st->syncSlots(); st->refresh(); }
    };
    doSwap();
    recordUndo (name, doSwap, doSwap, 20);
}

void MainComponent::mixEdited (Channel* c, int slot, float before, float after)
{
    if (before == after) return;
    const auto now = juce::Time::getMillisecondCounter();
    const int key = 2000 + slot;
    if (lastValueEdit.target == c && lastValueEdit.param == key && now - lastValueEdit.time < 700
        && lastValueEdit.after != nullptr && undoManager.canUndo())
    {
        *lastValueEdit.after = after;
        lastValueEdit.time = now;
        return;
    }
    auto afterValue = std::make_shared<float> (after);
    auto setMix = [this, c, slot] (float v)
    {
        if (! channelExists (c) || ! c->hasInsert (slot)) return;
        c->inserts[(size_t) slot].mix = v;
        if (auto* st = stripFor (c)) st->refresh();
    };
    recordUndo ("Mix dry/wet - " + c->inserts[(size_t) slot].getName(),
                [setMix, afterValue] { setMix (*afterValue); }, [setMix, before] { setMix (before); });
    lastValueEdit = { c, key, now, afterValue };
}

void MainComponent::moveTrackUndoable (int from, int to)
{
    auto doMove = [this] (int a, int b)
    {
        if (! juce::isPositiveAndBelow (a, (int) engine.tracks.size())) return;
        engine.moveTrack (a, b);
        rebuildStrips();
    };
    doMove (from, to);
    const auto name = juce::isPositiveAndBelow (to, (int) engine.tracks.size()) ? engine.tracks[(size_t) to]->name : juce::String();
    recordUndo ("Mover faixa " + name, [doMove, from, to] { doMove (from, to); }, [doMove, from, to] { doMove (to, from); });
}

void MainComponent::showInsertMix (Channel& c, int slot, juce::Component& source)
{
    // Balao com um controle de 0..100% (seco <-> plugin). Muda na hora, sem clique.
    struct MixBox : juce::Component
    {
        MixBox (MainComponent& o, Channel& ch, int sl) : owner (o), channel (&ch), slotIndex (sl)
        {
            title.setText ("Mix dry/wet - " + ch.inserts[(size_t) sl].getName(), juce::dontSendNotification);
            title.setFont (uiFont (12.0f, true));
            addAndMakeVisible (title);
            slider.setSliderStyle (juce::Slider::LinearHorizontal);
            slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
            slider.setRange (0.0, 100.0, 1.0);
            slider.setTextValueSuffix ("%");
            slider.setDoubleClickReturnValue (true, 100.0);
            slider.setValue (ch.inserts[(size_t) sl].mix.load() * 100.0, juce::dontSendNotification);
            slider.onDragStart = [this] { gesture = true; start = (float) (slider.getValue() / 100.0); };
            slider.onDragEnd = [this] { gesture = false; owner.mixEdited (channel, slotIndex, start, (float) (slider.getValue() / 100.0)); };
            slider.onValueChange = [this]
            {
                if (! owner.channelExists (channel) || ! channel->hasInsert (slotIndex)) return;
                const float before = channel->inserts[(size_t) slotIndex].mix.load();
                channel->inserts[(size_t) slotIndex].mix = (float) (slider.getValue() / 100.0);
                if (! gesture) owner.mixEdited (channel, slotIndex, before, (float) (slider.getValue() / 100.0));
                if (auto* st = owner.stripFor (channel)) st->refresh();
                owner.markDirty();
            };
            addAndMakeVisible (slider);
            hint.setText ("0% = seco, 100% = plugin. Duplo clique: 100%", juce::dontSendNotification);
            hint.setFont (uiFont (10.5f));
            hint.setColour (juce::Label::textColourId, colours::textDim);
            addAndMakeVisible (hint);
            setSize (300, 84);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            title.setBounds (r.removeFromTop (20));
            slider.setBounds (r.removeFromTop (30));
            hint.setBounds (r);
        }
        MainComponent& owner;
        Channel* channel;
        int slotIndex;
        bool gesture = false;
        float start = 1.0f;
        juce::Label title, hint;
        juce::Slider slider;
    };
    juce::CallOutBox::launchAsynchronously (std::make_unique<MixBox> (*this, c, slot), source.getScreenBounds(), nullptr);
}

void MainComponent::setInsertOffline (Channel& c, int slot)
{
    closeEditor (c, slot);
    auto old = engine.takeInsertOffline (c, slot);
    old.reset(); // destruido fora do lock do motor
    if (auto* st = stripFor (&c)) st->refresh();
    markDirty();
}

void MainComponent::setChannelPluginsOffline (Channel& c)
{
    closeEditorsFor (c);
    for (int i = 0; i < c.inserts.size(); ++i)
    {
        auto old = engine.takeInsertOffline (c, i);
        old.reset();
    }
    if (auto* st = stripFor (&c)) st->refresh();
    markDirty();
}

bool MainComponent::createFromSlot (const InsertSlot& s, Engine::RestoredInsert& out, juce::String& error)
{
    if (s.preferRemote)
    {
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("ScDspWorker.exe");
        out.remote = RemoteDspInsert::start (s.description, s.missingState, exe, engine.getSampleRate(), engine.getBlockSize(), error);
        return out.remote != nullptr;
    }
    auto inst = pluginHost.create (s.description, engine.getSampleRate(), engine.getBlockSize(), error);
    if (inst == nullptr) return false;
    if (s.missingState.getSize() > 0) inst->setStateInformation (s.missingState.getData(), (int) s.missingState.getSize());
    error = engine.prepareProcessor (*inst);
    if (error.isNotEmpty()) return false;
    out.local = std::move (inst);
    return true;
}

void MainComponent::restoreOfflineInserts (Channel& c, int onlySlot, bool unfreeze, juce::StringArray& failed)
{
    std::vector<Engine::RestoredInsert> restored;
    juce::MouseCursor::showWaitCursor();
    for (int i = 0; i < c.inserts.size(); ++i)
    {
        if (onlySlot >= 0 && i != onlySlot) continue;
        const auto& s = c.inserts[(size_t) i];
        if (! s.isOffline()) continue;
        Engine::RestoredInsert r;
        r.slot = i;
        juce::String err;
        if (createFromSlot (s, r, err)) restored.push_back (std::move (r));
        else failed.add (s.getName() + (err.isNotEmpty() ? " (" + err + ")" : juce::String()));
    }
    juce::MouseCursor::hideWaitCursor();
    engine.restoreInserts (c, restored, unfreeze); // tudo volta no mesmo instante
    if (auto* st = stripFor (&c)) st->refresh();
    markDirty();
}

juce::File MainComponent::freezeFileFor (const Track& t) const
{
    // Ao lado do projeto salvo; sem projeto, na pasta de dados do aplicativo.
    auto dir = currentFile != juce::File()
                   ? currentFile.getParentDirectory().getChildFile (currentFile.getFileNameWithoutExtension() + " - congeladas")
                   : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                         .getChildFile ("SilverConsole").getChildFile ("Congeladas");
    dir.createDirectory();
    return dir.getNonexistentChildFile (juce::File::createLegalFileName (t.name.isNotEmpty() ? t.name : juce::String ("Faixa")),
                                        ".wav", false);
}

void MainComponent::freezeTrack (Track& t)
{
    if (job != nullptr || t.isFrozen()) return;
    bool anyPlugin = false;
    double tail = 0.0;
    for (auto& s : t.inserts)
    {
        if (s.bypassed.load()) continue;
        if (s.remote != nullptr) anyPlugin = true;
        if (auto* p = s.processor.get())
        {
            anyPlugin = true;
            const double tl = p->getTailLengthSeconds();
            if (std::isfinite (tl)) tail = juce::jmax (tail, tl);
        }
    }
    if (! anyPlugin)
    {
        alert ("Congelar faixa", juce::String::fromUTF8 ("A faixa n\xc3\xa3o tem plugins ativos: n\xc3\xa3o h\xc3\xa1 CPU para economizar."), false);
        return;
    }
    const double tailSeconds = juce::jlimit (2.0, 20.0, tail + 1.0); // cauda de reverb/delay depois do fim do audio
    const auto file = freezeFileFor (t);
    closeEditorsFor (t);
    struct Result { bool ok = false; juce::String error; std::shared_ptr<StreamingAudio> stream; };
    auto result = std::make_shared<Result>();
    auto* tp = &t;
    const double sr = engine.getSampleRate();
    runJob ("Congelando " + t.name + "...", true,
            [this, tp, file, tailSeconds, sr, result] (Job& j)
            {
                result->ok = engine.renderFreeze (*tp, file, tailSeconds, j.progress, j.cancel, result->error);
                if (result->ok)
                {
                    result->stream = StreamingAudio::open (file, sr, result->error);
                    result->ok = result->stream != nullptr;
                }
            },
            [this, tp, file, result] (Job& j)
    {
        if (! result->ok || ! channelExists (tp))
        {
            file.deleteFile();
            if (! j.cancel.load() && channelExists (tp)) alert ("Congelar faixa", result->error);
            return;
        }
        clearUndo();
        std::vector<std::unique_ptr<juce::AudioProcessor>> unloaded;
        engine.setFrozen (*tp, file, result->stream, unloaded); // arquivo entra e plugins saem juntos
        unloaded.clear(); // libera a memoria dos plugins fora do lock
        if (auto* st = stripFor (tp)) st->refresh();
        lanesChanged();
        markDirty();
    });
}

void MainComponent::unfreezeTrack (Track& t)
{
    if (! t.isFrozen()) return;
    const auto file = t.frozenFile;
    clearUndo();
    juce::StringArray failed;
    restoreOfflineInserts (t, -1, true, failed);
    // O arquivo congelado vai para a lixeira (recuperavel), nao e apagado de vez.
    if (file.existsAsFile()) file.moveToTrash();
    lanesChanged();
    if (! failed.isEmpty())
        alert ("Descongelar faixa", juce::String::fromUTF8 ("Estes plugins n\xc3\xa3o abriram e ficaram offline (o ajuste continua guardado):\n")
                                    + failed.joinIntoString ("\n"));
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
    m.addItem (4, juce::String::fromUTF8 ("Economizar CPU: pausar plugins de canais em sil\xc3\xaancio"), true, engine.isCpuSaver());
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
        else if (r == 4)
        {
            engine.setCpuSaver (! engine.isCpuSaver());
            props.getUserSettings()->setValue ("cpuSaver", engine.isCpuSaver());
            props.getUserSettings()->saveIfNeeded();
        }
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
        std::vector<Track*> added;
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
            added.push_back (raw);
        }
        if (! added.empty())
        {
            struct Holder { std::vector<Track*> ptrs; std::vector<std::unique_ptr<Track>> stored; };
            auto h = std::make_shared<Holder>();
            h->ptrs = added;
            recordUndo ("Importar " + juce::String ((int) added.size()) + " faixa(s)", [this, h]
            {
                for (auto it = h->stored.rbegin(); it != h->stored.rend(); ++it)
                    engine.insertTrack ((int) engine.tracks.size(), std::move (*it));
                h->stored.clear();
                rebuildStrips();
            }, [this, h]
            {
                for (auto it = h->ptrs.rbegin(); it != h->ptrs.rend(); ++it)
                    if (channelExists (*it))
                    {
                        closeEditorsFor (**it);
                        h->stored.push_back (engine.removeTrack (*it));
                    }
                rebuildStrips();
            }, 50);
        }
        rebuildStrips();
        markDirty();
        if (j.cancel.load()) errors.add ("Importacao cancelada: alguns arquivos nao foram carregados.");
        if (! errors.isEmpty()) alert ("Importar", errors.joinIntoString ("\n"));
    });
}

void MainComponent::replaceAudio (Track& t)
{
    if (t.isFrozen())
    {
        alert (juce::String::fromUTF8 ("Trocar \xc3\xa1udio"), juce::String::fromUTF8 ("Descongele a faixa antes de trocar o \xc3\xa1udio."));
        return;
    }
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
        bool needsAudio = true;
        juce::File frozenFile;                       // congelada em outra taxa: reabre o render
        std::shared_ptr<StreamingAudio> frozenStream;
    };
    auto items = std::make_shared<std::vector<Item>>();
    for (auto& t : engine.tracks)
    {
        const bool audio = (t->original != nullptr || t->diskStream != nullptr) && t->playbackRate != sr;
        const bool frozen = t->frozenStream != nullptr && t->frozenStream->getRate() != sr;
        if (! audio && ! frozen) continue;
        items->push_back ({ t.get(), t->original, nullptr, t->originalRate, t->sourceFile, t->diskStream != nullptr, nullptr,
                            audio, frozen ? t->frozenFile : juce::File(), nullptr });
    }
    if (items->empty()) return;

    runJob ("Ajustando a taxa de amostragem (" + juce::String (sr / 1000.0, 1) + " kHz)...", false,
            [items, sr] (Job& j)
            {
                parallelFor ((int) items->size(), j, [&] (int i)
                {
                    auto& it = (*items)[(size_t) i];
                    if (it.frozenFile.existsAsFile())
                    {
                        juce::String reason;
                        it.frozenStream = StreamingAudio::open (it.frozenFile, sr, reason); // conversao por trechos
                    }
                    if (! it.needsAudio) return;
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
            if (it.frozenStream != nullptr && it.track->isFrozen())
            {
                std::vector<std::unique_ptr<juce::AudioProcessor>> none; // plugins ja estao offline
                engine.setFrozen (*it.track, it.frozenFile, it.frozenStream, none);
            }
            if (! it.needsAudio) continue;
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
    std::function<void (Engine::StemExport)> onExport, onQueue;

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
        exportButton.onClick = [this] { submit (false); };
        queueButton.onClick = [this] { submit (true); };
        queueButton.setTooltip (juce::String::fromUTF8 ("Guarda este pedido na fila; Exportar \xe2\x86\x92 Fila de exporta\xc3\xa7\xc3\xa3o grava tudo de uma vez"));
        addAndMakeVisible (queueButton);
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
        buttons.removeFromRight (8);
        queueButton.setBounds (buttons.removeFromRight (140));
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

    void submit (bool toQueue)
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
        auto cb = toQueue ? onQueue : onExport;
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
    juce::TextButton allButton { "Todas" }, noneButton { "Nenhuma" }, exportButton { "Escolher pasta..." }, cancelButton { "Cancelar" },
                     queueButton { juce::String::fromUTF8 ("Adicionar \xc3\xa0 fila") };
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
    if (! regions.empty())
    {
        juce::PopupMenu reg;
        reg.addItem (324, "WAV 24 bits");
        reg.addItem (316, "WAV 16 bits (com dither)");
        reg.addItem (332, "WAV 32 bits float");
        m.addSubMenu (juce::String::fromUTF8 ("Cada regi\xc3\xa3o como um arquivo (mix)"), reg);
    }
    m.addSeparator();
    {
        // normalizacao (vale para mix, selecao, regioes e fila)
        juce::PopupMenu nm;
        using M = Engine::Normalize::Mode;
        const auto& n = normalization;
        auto is = [&n] (M mode, float t) { return n.mode == mode && std::abs (n.target - t) < 0.01f; };
        nm.addItem (400, juce::String::fromUTF8 ("Sem normaliza\xc3\xa7\xc3\xa3o"), true, n.mode == M::none);
        nm.addSeparator();
        nm.addItem (401, "Pico de amostra -1,0 dBFS", true, is (M::peak, -1.0f));
        nm.addItem (402, "Pico de amostra -0,1 dBFS", true, is (M::peak, -0.1f));
        nm.addItem (403, "True peak -1,0 dBTP", true, is (M::truePeak, -1.0f));
        nm.addSeparator();
        nm.addItem (404, "-14 LUFS (Spotify/YouTube), teto -1 dBTP", true, is (M::lufs, -14.0f));
        nm.addItem (405, "-16 LUFS (Apple Music/podcast), teto -1 dBTP", true, is (M::lufs, -16.0f));
        nm.addItem (406, "-23 LUFS (EBU R128 TV), teto -1 dBTP", true, is (M::lufs, -23.0f));
        nm.addSeparator();
        nm.addItem (407, "Personalizado...");
        m.addSubMenu (juce::String::fromUTF8 ("Normaliza\xc3\xa7\xc3\xa3o: ") + normalizationText (normalization), nm);
        m.addItem (410, juce::String::fromUTF8 ("Medir loudness da m\xc3\xbasica (sem gravar)"));
        if (hasSel) m.addItem (411, juce::String::fromUTF8 ("Medir loudness da sele\xc3\xa7\xc3\xa3o (sem gravar)"));
    }
    m.addSeparator();
    {
        juce::PopupMenu q;
        q.addItem (524, juce::String::fromUTF8 ("Mix da m\xc3\xbasica - WAV 24 bits"));
        q.addItem (516, juce::String::fromUTF8 ("Mix da m\xc3\xbasica - WAV 16 bits"));
        q.addItem (532, juce::String::fromUTF8 ("Mix da m\xc3\xbasica - WAV 32 bits float"));
        if (hasSel)
        {
            q.addSeparator();
            q.addItem (624, juce::String::fromUTF8 ("Mix da sele\xc3\xa7\xc3\xa3o - WAV 24 bits"));
            q.addItem (616, juce::String::fromUTF8 ("Mix da sele\xc3\xa7\xc3\xa3o - WAV 16 bits"));
            q.addItem (632, juce::String::fromUTF8 ("Mix da sele\xc3\xa7\xc3\xa3o - WAV 32 bits float"));
        }
        if (! regions.empty())
        {
            q.addSeparator();
            q.addItem (724, juce::String::fromUTF8 ("Cada regi\xc3\xa3o - WAV 24 bits"));
            q.addItem (716, juce::String::fromUTF8 ("Cada regi\xc3\xa3o - WAV 16 bits"));
        }
        q.addSeparator();
        q.addItem (800, juce::String::fromUTF8 ("Faixas individuais... (bot\xc3\xa3o \"Adicionar \xc3\xa0 fila\")"));
        m.addSubMenu (juce::String::fromUTF8 ("Adicionar \xc3\xa0 fila"), q);
        m.addItem (900, juce::String::fromUTF8 ("Fila de exporta\xc3\xa7\xc3\xa3o (") + juce::String ((int) exportQueue.size()) + ")...");
    }
    if (hasSel) m.addItem (201, juce::String::fromUTF8 ("Faixas individuais s\xc3\xb3 da sele\xc3\xa7\xc3\xa3o..."));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton), [this, ls, le] (int choice)
    {
        if (choice == 0) return;
        if (choice == 900) { showQueue(); return; }
        if (choice == 800) { stemsDialog (false); return; }
        if (choice >= 500 && choice < 800)
        {
            QueueItem item;
            item.bits = choice % 100;
            item.norm = normalization;
            const double sr = engine.getSampleRate();
            const auto fmt = juce::String (item.bits) + " bits, " + normalizationText (normalization);
            if (choice < 600) item.label = juce::String::fromUTF8 ("Mix da m\xc3\xbasica (") + fmt + ")";
            else if (choice < 700)
            {
                item.start = ls; item.end = le;
                item.label = juce::String::fromUTF8 ("Mix da sele\xc3\xa7\xc3\xa3o ") + timeText (ls, sr) + "-" + timeText (le, sr) + " (" + fmt + ")";
            }
            else
            {
                item.kind = QueueItem::Kind::regions;
                item.regions = regions;
                item.label = "Cada " + juce::String::fromUTF8 ("regi\xc3\xa3o (") + juce::String ((int) regions.size()) + ", " + fmt + ")";
            }
            addToQueue (std::move (item));
            return;
        }
        if (choice == 410 || choice == 411) { measureLoudness (choice == 411 ? ls : 0, choice == 411 ? le : -1); return; }
        if (choice >= 400 && choice < 410)
        {
            using M = Engine::Normalize::Mode;
            Engine::Normalize n;
            switch (choice)
            {
                case 401: n = { M::peak, -1.0f, -1.0f }; break;
                case 402: n = { M::peak, -0.1f, -1.0f }; break;
                case 403: n = { M::truePeak, -1.0f, -1.0f }; break;
                case 404: n = { M::lufs, -14.0f, -1.0f }; break;
                case 405: n = { M::lufs, -16.0f, -1.0f }; break;
                case 406: n = { M::lufs, -23.0f, -1.0f }; break;
                case 407: customNormalizationDialog(); return;
                default: break;
            }
            setNormalization (n);
            return;
        }
        if (choice >= 300) { exportRegions (choice % 100); return; }
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
        struct Result { bool ok = false, clipped = false; juce::String error; Engine::ExportReport report; };
        auto res = std::make_shared<Result>();
        closeAllEditors(); // evita repintar editores durante o render offline
        const auto norm = normalization;
        runJob ("Exportando " + file.getFileName() + "...", true,
                [this, res, file, bits, rangeStart, rangeEnd, norm] (Job& j)
                { res->ok = engine.exportMix (file, bits, 3.0, j.progress, j.cancel, res->error, res->clipped, rangeStart, rangeEnd, norm, &res->report); },
                [this, res, file, norm] (Job&)
        {
            refreshStrips();
            if (! res->ok) { alert ("Exportar", res->error); return; }
            juce::String msg = "Mix exportada:\n" + file.getFullPathName() + "\n\n" + normalizationText (norm) + "\n" + reportText (res->report);
            if (res->clipped) msg << "\n\nAtencao: a mix passou de 0 dBFS (clipping). Abaixe o master, use um limitador ou normalize.";
            alert ("Exportar", msg, res->clipped);
        });
    });
}

//==============================================================================
// Normalizacao, medicao e fila
juce::String MainComponent::normalizationText (const Engine::Normalize& n) const
{
    auto num = [] (float v) { return juce::String (v, 1).replaceCharacter ('.', ','); };
    switch (n.mode)
    {
        case Engine::Normalize::Mode::peak:     return "pico " + num (n.target) + " dBFS";
        case Engine::Normalize::Mode::truePeak: return "true peak " + num (n.target) + " dBTP";
        case Engine::Normalize::Mode::lufs:     return num (n.target) + " LUFS, teto " + num (n.truePeakCeiling) + " dBTP";
        case Engine::Normalize::Mode::none:     break;
    }
    return juce::String::fromUTF8 ("sem normaliza\xc3\xa7\xc3\xa3o");
}

juce::String MainComponent::reportText (const Engine::ExportReport& r)
{
    if (! r.measured) return {};
    auto v = [] (float x, const char* unit)
    {
        return (x <= -99.0f ? juce::String ("-inf") : juce::String (x, 1).replaceCharacter ('.', ',')) + " " + unit;
    };
    juce::String s;
    s << "Loudness integrado: " << v (r.integratedLufs, "LUFS")
      << "\n" << juce::String::fromUTF8 ("M\xc3\xa1ximo de curto prazo (3 s): ") << v (r.maxShortTermLufs, "LUFS")
      << "\nTrue peak: " << v (r.truePeakDb, "dBTP") << "   |   Pico de amostra: " << v (r.samplePeakDb, "dBFS");
    if (std::abs (r.gainDb) > 0.005f)
        s << "\nGanho aplicado: " << (r.gainDb > 0 ? "+" : "") << juce::String (r.gainDb, 2).replaceCharacter ('.', ',') << " dB";
    if (r.limitedByCeiling)
        s << juce::String::fromUTF8 ("\n(o teto de true peak segurou o ganho: para chegar ao alvo de LUFS, use um limitador no master)");
    return s;
}

void MainComponent::setNormalization (Engine::Normalize n)
{
    normalization = n;
    props.getUserSettings()->setValue ("normMode", (int) n.mode);
    props.getUserSettings()->setValue ("normTarget", (double) n.target);
    props.getUserSettings()->setValue ("normCeiling", (double) n.truePeakCeiling);
    props.getUserSettings()->saveIfNeeded();
}

void MainComponent::customNormalizationDialog()
{
    auto* w = new juce::AlertWindow (juce::String::fromUTF8 ("Normaliza\xc3\xa7\xc3\xa3o personalizada"),
                                     juce::String::fromUTF8 ("Um ganho \xc3\xbanico no arquivo inteiro (sem limitador)."),
                                     juce::MessageBoxIconType::NoIcon);
    w->addComboBox ("mode", { juce::String::fromUTF8 ("Sem normaliza\xc3\xa7\xc3\xa3o"), "Pico de amostra (dBFS)", "True peak (dBTP)", "Loudness integrado (LUFS)" }, "Tipo");
    if (auto* cb = w->getComboBoxComponent ("mode")) cb->setSelectedItemIndex ((int) normalization.mode);
    w->addTextEditor ("target", juce::String (normalization.target, 1), "Alvo (dB ou LUFS)");
    w->addTextEditor ("ceiling", juce::String (normalization.truePeakCeiling, 1), "Teto de true peak para LUFS (dBTP)");
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancelar", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> self (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, self] (int r)
    {
        if (r != 1 || self == nullptr) return;
        auto num = [w] (const char* id) { return w->getTextEditorContents (id).replaceCharacter (',', '.').getFloatValue(); };
        Engine::Normalize n;
        n.mode = (Engine::Normalize::Mode) juce::jlimit (0, 3, w->getComboBoxComponent ("mode")->getSelectedItemIndex());
        n.target = juce::jlimit (-70.0f, 0.0f, num ("target"));
        n.truePeakCeiling = juce::jlimit (-20.0f, 0.0f, num ("ceiling"));
        self->setNormalization (n);
    }), true);
}

void MainComponent::measureLoudness (juce::int64 rangeStart, juce::int64 rangeEnd)
{
    if (engine.tracks.empty() || job != nullptr) return;
    struct Result { bool ok = false, clipped = false; juce::String error; Engine::ExportReport report; };
    auto res = std::make_shared<Result>();
    closeAllEditors();
    runJob (juce::String::fromUTF8 ("Medindo loudness (render sem gravar)..."), true,
            [this, res, rangeStart, rangeEnd] (Job& j)
            {
                res->ok = engine.exportMix ({}, 24, 0.0, j.progress, j.cancel, res->error, res->clipped,
                                            rangeStart, rangeEnd, {}, &res->report, true);
            },
            [this, res, rangeEnd] (Job& j)
    {
        refreshStrips();
        if (j.cancel.load()) return;
        if (! res->ok) { alert ("Medir loudness", res->error); return; }
        alert (rangeEnd >= 0 ? juce::String::fromUTF8 ("Loudness da sele\xc3\xa7\xc3\xa3o") : juce::String::fromUTF8 ("Loudness da m\xc3\xbasica"),
               reportText (res->report) + juce::String::fromUTF8 ("\n\nMedido como seria exportado (sem normaliza\xc3\xa7\xc3\xa3o), BS.1770 / EBU R128."), false);
    });
}

void MainComponent::addToQueue (QueueItem item)
{
    exportQueue.push_back (std::move (item));
    statusText = juce::String::fromUTF8 ("Adicionado \xc3\xa0 fila de exporta\xc3\xa7\xc3\xa3o (") + juce::String ((int) exportQueue.size())
                 + juce::String::fromUTF8 (" itens). Exportar \xe2\x86\x92 Fila de exporta\xc3\xa7\xc3\xa3o para gravar.");
    repaint (statusArea());
    if (queueWindow != nullptr) showQueue(); // atualiza a lista aberta
}

namespace
{
    class QueuePanel : public juce::Component, private juce::ListBoxModel
    {
    public:
        QueuePanel (std::vector<juce::String> labels, std::function<void (int)> removeFn, std::function<void()> clearFn, std::function<void()> runFn)
            : items (std::move (labels)), onRemove (std::move (removeFn)), onClear (std::move (clearFn)), onRun (std::move (runFn))
        {
            title.setText (juce::String::fromUTF8 ("Fila de exporta\xc3\xa7\xc3\xa3o"), juce::dontSendNotification);
            title.setFont (uiFont (17.0f, true));
            addAndMakeVisible (title);
            hint.setText (juce::String::fromUTF8 ("Tudo vai para uma pasta nova, um item depois do outro. Adicione itens em Exportar \xe2\x86\x92 Adicionar \xc3\xa0 fila."),
                          juce::dontSendNotification);
            hint.setFont (uiFont (11.5f));
            hint.setColour (juce::Label::textColourId, colours::textDim);
            addAndMakeVisible (hint);
            list.setModel (this);
            list.setRowHeight (24);
            list.setColour (juce::ListBox::backgroundColourId, colours::lcd);
            addAndMakeVisible (list);
            for (auto* b : { &runButton, &removeButton, &clearButton }) addAndMakeVisible (b);
            runButton.setColour (juce::TextButton::buttonColourId, colours::accentDeep);
            runButton.onClick = [this] { if (! items.empty() && onRun) onRun(); };
            removeButton.onClick = [this] { const int r = list.getSelectedRow(); if (r >= 0 && onRemove) onRemove (r); };
            clearButton.onClick = [this] { if (onClear) onClear(); };
            runButton.setEnabled (! items.empty());
            setSize (620, 380);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (14);
            title.setBounds (r.removeFromTop (26));
            hint.setBounds (r.removeFromTop (34));
            auto buttons = r.removeFromBottom (32);
            runButton.setBounds (buttons.removeFromRight (170));
            buttons.removeFromRight (8);
            clearButton.setBounds (buttons.removeFromLeft (110));
            buttons.removeFromLeft (8);
            removeButton.setBounds (buttons.removeFromLeft (110));
            r.removeFromBottom (10);
            list.setBounds (r);
        }
    private:
        int getNumRows() override { return (int) items.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (selected) g.fillAll (colours::accentDeep);
            g.setColour (colours::text);
            g.setFont (uiFont (12.5f));
            if (juce::isPositiveAndBelow (row, (int) items.size()))
                g.drawFittedText (juce::String (row + 1) + ".  " + items[(size_t) row], 8, 0, w - 16, h, juce::Justification::centredLeft, 1);
        }
        std::vector<juce::String> items;
        std::function<void (int)> onRemove;
        std::function<void()> onClear, onRun;
        juce::Label title, hint;
        juce::ListBox list;
        juce::TextButton runButton { "Exportar tudo..." }, removeButton { "Remover" }, clearButton { "Limpar" };
    };
}

void MainComponent::showQueue()
{
    std::vector<juce::String> labels;
    for (auto& q : exportQueue) labels.push_back (q.label);
    juce::Component::SafePointer<MainComponent> self (this);
    auto* panel = new QueuePanel (labels,
        [self] (int i) { if (self == nullptr) return; if (juce::isPositiveAndBelow (i, (int) self->exportQueue.size())) self->exportQueue.erase (self->exportQueue.begin() + i); self->showQueue(); },
        [self] { if (self == nullptr) return; self->exportQueue.clear(); self->showQueue(); },
        [self] { if (self == nullptr) return; juce::MessageManager::callAsync ([self] { if (self != nullptr) { self->queueWindow.reset(); self->runQueue(); } }); });
    if (queueWindow == nullptr)
    {
        struct Win : juce::DialogWindow
        {
            Win (MainComponent& o) : DialogWindow (juce::String::fromUTF8 ("Fila de exporta\xc3\xa7\xc3\xa3o"), colours::background, true, true), owner (o) {}
            void closeButtonPressed() override { juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<MainComponent> (&owner)] { if (sp != nullptr) sp->queueWindow.reset(); }); }
            MainComponent& owner;
        };
        auto w = std::make_unique<Win> (*this);
        w->setUsingNativeTitleBar (true);
        w->setContentOwned (panel, true);
        w->centreAroundComponent (this, panel->getWidth(), panel->getHeight());
        w->setVisible (true);
        queueWindow = std::move (w);
    }
    else
        queueWindow->setContentOwned (panel, true);
}

void MainComponent::runQueue()
{
    if (exportQueue.empty() || job != nullptr) return;
    const auto start = currentFile.existsAsFile() ? currentFile.getParentDirectory()
                                                  : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Pasta onde criar a pasta da exporta\xc3\xa7\xc3\xa3o"), start);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
    {
        const auto parent = fc.getResult();
        if (parent == juce::File()) return;
        const auto prefix = currentFile.existsAsFile() ? currentFile.getFileNameWithoutExtension() : juce::String ("Mix");
        auto folder = parent.getNonexistentChildFile (juce::File::createLegalFileName (prefix + " - exportacao"), {}, false);
        if (! folder.createDirectory()) { alert ("Exportar", "Nao foi possivel criar a pasta " + folder.getFullPathName()); return; }

        // resolve as faixas dos stems agora (podem ter mudado de posicao)
        auto items = std::make_shared<std::vector<QueueItem>> (exportQueue);
        for (auto& it : *items)
            if (it.kind == QueueItem::Kind::stems)
            {
                it.stems.tracks.clear();
                for (auto* t : it.stemTracks)
                    for (size_t i = 0; i < engine.tracks.size(); ++i)
                        if (engine.tracks[i].get() == t) it.stems.tracks.push_back ((int) i);
                it.stems.buses.erase (std::remove_if (it.stems.buses.begin(), it.stems.buses.end(),
                                                      [this] (int b) { return b >= (int) engine.buses.size(); }), it.stems.buses.end());
            }
        auto lines = std::make_shared<juce::StringArray>();
        auto failures = std::make_shared<int> (0);
        const double sr = engine.getSampleRate();
        closeAllEditors();
        runJob (juce::String::fromUTF8 ("Fila de exporta\xc3\xa7\xc3\xa3o (") + juce::String ((int) items->size()) + " itens)...", true,
                [this, items, folder, prefix, lines, failures, sr] (Job& j)
        {
            const int count = (int) items->size();
            for (int k = 0; k < count && ! j.cancel.load(); ++k)
            {
                auto& it = (*items)[(size_t) k];
                const auto num = juce::String (k + 1).paddedLeft ('0', 2);
                std::atomic<float> progress { 0.0f };
                juce::String err;
                bool ok = false, clipped = false;
                juce::String detail;
                if (it.kind == QueueItem::Kind::mix)
                {
                    auto name = num + " - " + prefix + " - mix";
                    if (it.end >= 0) name << " " << timeText (it.start, sr).replaceCharacter (':', '.') << "-" << timeText (it.end, sr).replaceCharacter (':', '.');
                    Engine::ExportReport rep;
                    ok = engine.exportMix (folder.getChildFile (juce::File::createLegalFileName (name) + ".wav"), it.bits, it.end >= 0 ? 0.0 : 3.0,
                                           progress, j.cancel, err, clipped, it.start, it.end, it.norm, &rep);
                    if (ok) detail = reportText (rep).replace ("\n", "  |  ");
                }
                else if (it.kind == QueueItem::Kind::regions)
                {
                    auto sub = folder.getChildFile (juce::File::createLegalFileName (num + " - regioes"));
                    ok = sub.createDirectory();
                    int done = 0;
                    for (size_t i = 0; ok && i < it.regions.size() && ! j.cancel.load(); ++i)
                    {
                        const auto& rg = it.regions[i];
                        const auto name = juce::String ((int) i + 1).paddedLeft ('0', 2) + " - " + (rg.name.isNotEmpty() ? rg.name : juce::String ("Regiao"));
                        if (engine.exportMix (sub.getChildFile (juce::File::createLegalFileName (name) + ".wav"), it.bits, 0.0, progress, j.cancel, err, clipped,
                                              (juce::int64) (rg.start * sr), (juce::int64) (rg.end * sr), it.norm))
                            ++done;
                        else ok = false;
                    }
                    detail = juce::String (done) + " arquivo(s)";
                }
                else
                {
                    auto req = it.stems;
                    req.folder = folder.getChildFile (juce::File::createLegalFileName (num + " - faixas"));
                    req.prefix = prefix;
                    juce::StringArray written, clippedNames;
                    ok = ! req.tracks.empty() || ! req.buses.empty();
                    if (! ok) err = "as faixas escolhidas nao existem mais";
                    else ok = engine.exportStems (req, progress, j.cancel, err, written, clippedNames);
                    detail = juce::String (written.size()) + " arquivo(s)";
                }
                if (! ok) ++*failures;
                lines->add ((ok ? juce::String ("OK  ") : juce::String ("FALHA  ")) + num + ". " + it.label
                            + (ok ? (detail.isNotEmpty() ? "\n      " + detail : juce::String()) : "\n      " + err));
                j.progress = (float) (k + 1) / (float) count;
            }
        },
                [this, folder, lines, failures] (Job& j)
        {
            refreshStrips();
            juce::String msg = folder.getFullPathName() + "\n\n" + lines->joinIntoString ("\n");
            if (j.cancel.load()) msg << "\n\n(cancelado)";
            alert (juce::String::fromUTF8 ("Fila de exporta\xc3\xa7\xc3\xa3o"), msg, *failures > 0);
        });
    });
}

void MainComponent::stemsDialog (bool selectionOnly)
{
    if (engine.tracks.empty()) { alert ("Exportar", "Importe alguma faixa antes de exportar."); return; }
    auto* panel = new StemExportPanel (engine, selectionOnly,
                                       currentFile != juce::File() ? currentFile.getFileNameWithoutExtension() : juce::String ("Stems"));
    juce::Component::SafePointer<MainComponent> self (this);
    panel->onQueue = [self] (Engine::StemExport req)
    {
        juce::MessageManager::callAsync ([self, req]
        {
            if (self == nullptr) return;
            QueueItem item;
            item.kind = QueueItem::Kind::stems;
            item.stems = req;
            for (int i : req.tracks)
                if (juce::isPositiveAndBelow (i, (int) self->engine.tracks.size())) item.stemTracks.push_back (self->engine.tracks[(size_t) i].get());
            const double sr = self->engine.getSampleRate();
            item.label = "Faixas individuais (" + juce::String ((int) item.stemTracks.size()) + ", "
                         + (req.processed ? juce::String ("com processamento") : juce::String::fromUTF8 ("\xc3\xa1udio original"))
                         + (req.rangeEnd >= 0 ? ", trecho " + timeText (req.rangeStart, sr) + "-" + timeText (req.rangeEnd, sr) : juce::String())
                         + ", " + juce::String (req.bitDepth) + " bits)";
            self->addToQueue (std::move (item));
        });
    };
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
    m.addSeparator();
    m.addItem (10, "Adicionar marcador no cursor (M)");
    m.addItem (11, juce::String::fromUTF8 ("Criar regi\xc3\xa3o com a sele\xc3\xa7\xc3\xa3o (R)"), hasSel);
    if (! markers.empty() || ! regions.empty())
    {
        juce::PopupMenu go;
        for (int i = 0; i < (int) markers.size(); ++i)
            go.addItem (1000 + i, juce::String::fromUTF8 ("\xe2\x96\xb8 ") + markers[(size_t) i].name + "  (" + timeText ((juce::int64) (markers[(size_t) i].t * sr), sr) + ")");
        if (! markers.empty() && ! regions.empty()) go.addSeparator();
        for (int i = 0; i < (int) regions.size(); ++i)
            go.addItem (2000 + i, juce::String::fromUTF8 ("Regi\xc3\xa3o: ") + regions[(size_t) i].name);
        m.addSubMenu ("Ir para ([ e ] pulam entre marcadores)", go);
    }
    m.addItem (12, juce::String::fromUTF8 ("Exportar cada regi\xc3\xa3o como um arquivo (mix 24 bits)..."), ! regions.empty());
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this, ls, le, sr] (int r)
    {
        if (r >= 1000 && r < 2000 && juce::isPositiveAndBelow (r - 1000, (int) markers.size()))
        {
            engine.stopAt = -1;
            engine.position = (juce::int64) (markers[(size_t) (r - 1000)].t * sr);
            return;
        }
        if (r >= 2000) { selectRegion (r - 2000); return; }
        switch (r)
        {
            case 10: addMarkerAtCursor(); break;
            case 11: addRegionFromSelection(); break;
            case 12: exportRegions (24); break;
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
// Marcadores e regioes
void MainComponent::editMarkers (const juce::String& name, std::function<void()> change)
{
    const auto beforeM = markers;
    const auto beforeR = regions;
    change();
    std::sort (markers.begin(), markers.end(), [] (const MarkerData& a, const MarkerData& b) { return a.t < b.t; });
    std::sort (regions.begin(), regions.end(), [] (const RegionData& a, const RegionData& b) { return a.start < b.start; });
    const auto afterM = markers;
    const auto afterR = regions;
    auto set = [this] (const std::vector<MarkerData>& m, const std::vector<RegionData>& r) { markers = m; regions = r; timeline.repaint(); };
    recordUndo (name, [set, afterM, afterR] { set (afterM, afterR); }, [set, beforeM, beforeR] { set (beforeM, beforeR); });
    timeline.repaint();
    markDirty();
}

void MainComponent::addMarkerAtCursor()
{
    const double t = (double) engine.position.load() / engine.getSampleRate();
    for (auto& m : markers) if (std::abs (m.t - t) < 0.01) return; // ja existe aqui
    editMarkers ("Adicionar marcador", [this, t]
    {
        int n = 1;
        for (auto& m : markers) if (m.name.startsWith ("M")) n = juce::jmax (n, m.name.substring (1).getIntValue() + 1);
        markers.push_back ({ t, "M" + juce::String (n) });
    });
}

void MainComponent::addRegionFromSelection()
{
    if (! engine.hasSelection()) return;
    const double sr = engine.getSampleRate();
    const double a = (double) engine.loopStart.load() / sr, b = (double) engine.loopEnd.load() / sr;
    renameDialog (juce::String::fromUTF8 ("Nova regi\xc3\xa3o"), juce::String::fromUTF8 ("Regi\xc3\xa3o ") + juce::String ((int) regions.size() + 1),
                  [this, a, b] (juce::String name)
    {
        editMarkers (juce::String::fromUTF8 ("Criar regi\xc3\xa3o"), [this, a, b, name] { regions.push_back ({ a, b, name }); });
    });
}

void MainComponent::selectRegion (int i)
{
    if (! juce::isPositiveAndBelow (i, (int) regions.size())) return;
    const double sr = engine.getSampleRate();
    engine.setLoop ((juce::int64) (regions[(size_t) i].start * sr), (juce::int64) (regions[(size_t) i].end * sr));
    engine.stopAt = -1;
    engine.position = engine.loopStart.load();
    timeline.repaint();
    markDirty();
}

void MainComponent::jumpToMarker (bool next)
{
    if (markers.empty()) return;
    const double sr = engine.getSampleRate();
    const double now = (double) engine.position.load() / sr;
    double target = next ? -1.0 : -2.0;
    for (auto& m : markers)
    {
        if (next && m.t > now + 0.005) { target = m.t; break; }
        if (! next && m.t < now - 0.005) target = m.t;
    }
    if (target < 0.0) return;
    engine.stopAt = -1;
    engine.position = (juce::int64) (target * sr);
}

void MainComponent::renameDialog (const juce::String& title, const juce::String& current, std::function<void (juce::String)> done)
{
    auto* w = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", current);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancelar", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, done] (int r)
    {
        const auto name = w->getTextEditorContents ("name").trim().substring (0, 60);
        if (r == 1 && name.isNotEmpty()) done (name);
    }), true);
}

void MainComponent::markerMenu (int i)
{
    if (! juce::isPositiveAndBelow (i, (int) markers.size())) return;
    juce::PopupMenu m;
    m.addSectionHeader ("Marcador " + markers[(size_t) i].name);
    m.addItem (1, "Ir para o marcador");
    m.addItem (2, "Renomear...");
    m.addItem (3, "Mover para o cursor");
    m.addItem (4, "Apagar");
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this, i] (int r)
    {
        if (! juce::isPositiveAndBelow (i, (int) markers.size())) return;
        if (r == 1) { engine.stopAt = -1; engine.position = (juce::int64) (markers[(size_t) i].t * engine.getSampleRate()); }
        else if (r == 2)
            renameDialog ("Renomear marcador", markers[(size_t) i].name, [this, i] (juce::String name)
            {
                editMarkers ("Renomear marcador", [this, i, name] { if (juce::isPositiveAndBelow (i, (int) markers.size())) markers[(size_t) i].name = name; });
            });
        else if (r == 3)
        {
            const double t = (double) engine.position.load() / engine.getSampleRate();
            editMarkers ("Mover marcador", [this, i, t] { markers[(size_t) i].t = t; });
        }
        else if (r == 4) editMarkers ("Apagar marcador", [this, i] { markers.erase (markers.begin() + i); });
    });
}

void MainComponent::regionMenu (int i)
{
    if (! juce::isPositiveAndBelow (i, (int) regions.size())) return;
    juce::PopupMenu m;
    m.addSectionHeader (juce::String::fromUTF8 ("Regi\xc3\xa3o ") + regions[(size_t) i].name);
    m.addItem (1, juce::String::fromUTF8 ("Selecionar a regi\xc3\xa3o"));
    m.addItem (2, juce::String::fromUTF8 ("Tocar a regi\xc3\xa3o (duplo clique)"));
    m.addItem (3, "Renomear...");
    m.addItem (4, juce::String::fromUTF8 ("Usar a sele\xc3\xa7\xc3\xa3o atual como limites"), engine.hasSelection());
    m.addItem (5, "Apagar");
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this, i] (int r)
    {
        if (! juce::isPositiveAndBelow (i, (int) regions.size())) return;
        if (r == 1) selectRegion (i);
        else if (r == 2) { selectRegion (i); playSelection(); }
        else if (r == 3)
            renameDialog (juce::String::fromUTF8 ("Renomear regi\xc3\xa3o"), regions[(size_t) i].name, [this, i] (juce::String name)
            {
                editMarkers (juce::String::fromUTF8 ("Renomear regi\xc3\xa3o"), [this, i, name] { if (juce::isPositiveAndBelow (i, (int) regions.size())) regions[(size_t) i].name = name; });
            });
        else if (r == 4)
        {
            const double sr = engine.getSampleRate();
            const double a = (double) engine.loopStart.load() / sr, b = (double) engine.loopEnd.load() / sr;
            editMarkers (juce::String::fromUTF8 ("Ajustar regi\xc3\xa3o"), [this, i, a, b] { regions[(size_t) i].start = a; regions[(size_t) i].end = b; });
        }
        else if (r == 5) editMarkers (juce::String::fromUTF8 ("Apagar regi\xc3\xa3o"), [this, i] { regions.erase (regions.begin() + i); });
    });
}

void MainComponent::exportRegions (int bits)
{
    if (regions.empty() || job != nullptr) return;
    const auto start = currentFile.existsAsFile() ? currentFile.getParentDirectory()
                                                  : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    chooser = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Escolha a pasta (ser\xc3\xa1 criada uma pasta nova dentro dela)"), start);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, bits] (const juce::FileChooser& fc)
    {
        const auto parent = fc.getResult();
        if (parent == juce::File()) return;
        const auto prefix = currentFile.existsAsFile() ? currentFile.getFileNameWithoutExtension() : juce::String ("Mix");
        auto folder = parent.getNonexistentChildFile (juce::File::createLegalFileName (prefix + " - regioes"), {}, false);
        if (! folder.createDirectory()) { alert ("Exportar", "Nao foi possivel criar a pasta " + folder.getFullPathName()); return; }
        auto list = regions;
        const double sr = engine.getSampleRate();
        auto errors = std::make_shared<juce::StringArray>();
        auto written = std::make_shared<int> (0);
        runJob (juce::String::fromUTF8 ("Exportando ") + juce::String ((int) list.size()) + juce::String::fromUTF8 (" regi\xc3\xb5" "es..."), true,
                [this, list, folder, bits, sr, errors, written, norm = normalization] (Job& j)
                {
                    for (size_t i = 0; i < list.size() && ! j.cancel.load(); ++i)
                    {
                        const auto& rg = list[i];
                        const auto name = juce::String ((int) i + 1).paddedLeft ('0', 2) + " - " + (rg.name.isNotEmpty() ? rg.name : juce::String ("Regiao"));
                        auto f = folder.getChildFile (juce::File::createLegalFileName (name) + ".wav");
                        std::atomic<float> progress { 0.0f };
                        juce::String err;
                        bool clipped = false;
                        if (engine.exportMix (f, bits, 0.0, progress, j.cancel, err, clipped,
                                              (juce::int64) (rg.start * sr), (juce::int64) (rg.end * sr), norm))
                            ++*written;
                        else if (! j.cancel.load()) errors->add (rg.name + ": " + err);
                        j.progress = (float) (i + 1) / (float) list.size();
                    }
                },
                [this, folder, errors, written] (Job&)
        {
            if (*written == 0) folder.deleteRecursively();
            juce::String msg = juce::String (*written) + juce::String::fromUTF8 (" arquivo(s) em\n") + folder.getFullPathName();
            if (! errors->isEmpty()) msg << "\n\nFalhas:\n" << errors->joinIntoString ("\n");
            alert ("Exportar regioes", msg, ! errors->isEmpty());
        });
    });
}

//==============================================================================
// Projeto
void MainComponent::resetSession()
{
    clearUndo();
    closeAllEditors();
    std::vector<std::unique_ptr<juce::AudioProcessor>> procs;
    std::vector<std::unique_ptr<Track>> oldTracks;
    std::vector<std::unique_ptr<Bus>> oldBuses;
    recorder.reset();
    engine.clearSession (procs, oldTracks, oldBuses);
    engine.master.autoMode = (int) AutoMode::read;
    engine.editAutomation ([this] { engine.master.lanes.clear(); });
    strips.clear();
    vcaStrips.clear();
    markers.clear();
    regions.clear();
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
    for (auto& t : data->tracks) decoded->push_back ({ t.file, {}, {}, {}, t.frozenFile, {} });
    const double sr = engine.getSampleRate();

    runJob ("Abrindo " + file.getFileName() + "...", true,
            [decoded, sr] (Job& j)
            {
                parallelFor ((int) decoded->size(), j, [&] (int i)
                {
                    auto& d = (*decoded)[(size_t) i];
                    if (d.file != juce::File()) decodeInto (d, sr);
                    if (d.frozenFile.existsAsFile())
                    {
                        juce::String reason;
                        d.frozen = StreamingAudio::open (d.frozenFile, sr, reason);
                    }
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
                t->sends[(size_t) s].preFx = td.sends[(size_t) s].preFx;
            }
            auto* raw = engine.addTrack (std::move (t));
            if (d.stream != nullptr) engine.setTrackStream (*raw, d.stream);
            else if (d.audio.ok()) engine.setTrackAudio (*raw, d.audio.original, d.audio.rate, d.playback, sr);
            else problems.add (raw->name + ": " + (d.audio.error.isNotEmpty() ? d.audio.error : juce::String ("sem arquivo de audio")));
            loadInserts (*raw, td.ch, problems);
            if (td.frozenFile != juce::File())
            {
                if (d.frozen != nullptr)
                {
                    std::vector<std::unique_ptr<juce::AudioProcessor>> unloaded;
                    engine.setFrozen (*raw, td.frozenFile, d.frozen, unloaded);
                }
                else
                {
                    juce::StringArray failed;
                    restoreOfflineInserts (*raw, -1, false, failed);
                    problems.add (raw->name + juce::String::fromUTF8 (": arquivo congelado n\xc3\xa3o encontrado; a faixa foi descongelada")
                                  + (failed.isEmpty() ? juce::String() : " (offline: " + failed.joinIntoString (", ") + ")"));
                }
            }
        }
        applyChannelBasics (engine, engine.master, data->master);
        engine.master.name = "MASTER";
        engine.master.mono = data->masterMono;
        for (int g = 0; g < kMaxGroups; ++g)
        {
            engine.vca[(size_t) g].gainDb = data->vca[(size_t) g].gainDb;
            engine.vca[(size_t) g].mute = data->vca[(size_t) g].mute;
            engine.vca[(size_t) g].shown = data->vca[(size_t) g].shown;
        }
        loadInserts (engine.master, data->master, problems);
        // sidechains depois de todas as faixas existirem (a fonte pode vir depois)
        auto applySidechains = [this, &problems] (Channel& ch, const ChannelData& cd)
        {
            for (auto& in : cd.inserts)
                if (juce::isPositiveAndBelow (in.sidechain, (int) engine.tracks.size()))
                    if (! engine.setSidechain (ch, in.slot, engine.tracks[(size_t) in.sidechain].get()))
                        problems.add (ch.name + ": sidechain indisponivel em " + in.desc.name);
        };
        for (size_t i = 0; i < data->tracks.size() && i < engine.tracks.size(); ++i) applySidechains (*engine.tracks[i], data->tracks[i].ch);
        for (size_t i = 0; i < data->buses.size() && i < engine.buses.size(); ++i) applySidechains (*engine.buses[i], data->buses[i]);
        applySidechains (engine.master, data->master);
        if (data->loopEndSec > data->loopStartSec)
            engine.setLoop ((juce::int64) (data->loopStartSec * sr), (juce::int64) (data->loopEndSec * sr));
        engine.loopOn = data->loopOn;
        markers = data->markers;
        regions = data->regions;
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
    auto pd = captureProject (engine);
    pd.markers = markers;
    pd.regions = regions;
    if (! writeProject (pd, f, err)) { alert ("Salvar", err); return false; }
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
