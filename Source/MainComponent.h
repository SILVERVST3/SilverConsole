// SilverConsole - janela principal. Codigo original (MIT).
#pragma once
#include "Engine.h"
#include "Project.h"
#include "ChannelStrip.h"
#include "PluginHost.h"
#include "PluginWindow.h"
#include "AutomationRecorder.h"
#include "AutomationView.h"
#include "SilverLookAndFeel.h"

namespace sc
{
//==============================================================================
class Job : public juce::Thread
{
public:
    explicit Job (std::function<void (Job&)> fn) : Thread ("SilverConsole job"), work (std::move (fn)) {}
    ~Job() override { stopThread (60000); }
    void run() override { work (*this); }
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> cancel { false };

private:
    std::function<void (Job&)> work;
};

//==============================================================================
class IconButton : public juce::Button
{
public:
    enum class Shape { toStart, play, stop, loop };
    IconButton (Shape s, const juce::String& tip) : Button (tip), shape (s) { setTooltip (tip); setWantsKeyboardFocus (false); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
    bool showPause = false;

private:
    Shape shape;
};

//==============================================================================
/** Regua do tempo: clique posiciona o cursor; arrastar seleciona um trecho
    (Shift+arrastar tambem liga o loop); arrastar as bordas ajusta a selecao;
    duplo clique na selecao toca o trecho; botao direito abre o menu do trecho. */
class Timeline : public juce::Component
{
public:
    explicit Timeline (Engine& e) : engine (e) {}
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    std::function<void()> onEdited, onSelectionChanged, onPlaySelection, onMenu;

private:
    enum class Drag { none, click, select, startEdge, endEdge, scrub };
    Drag hitTestDrag (const juce::MouseEvent&) const;
    double visibleSeconds() const;
    juce::int64 xToSample (float x) const;
    float sampleToX (juce::int64 s) const;
    Engine& engine;
    juce::int64 dragAnchor = 0;
    Drag drag = Drag::none;
    bool loopWhileSelecting = false;
};

//==============================================================================
class BusyOverlay : public juce::Component
{
public:
    BusyOverlay();
    void show (const juce::String& message, bool cancellable);
    void paint (juce::Graphics&) override;
    void resized() override;
    double progress = 0.0;
    juce::TextButton cancelButton { "Cancelar" };

private:
    juce::Label message;
    juce::ProgressBar bar { progress };
};

//==============================================================================
class MainComponent : public juce::Component,
                      public StripHost,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    MainComponent (juce::ApplicationProperties&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    void requestQuit();
    /** Abre projeto ou importa audio. discardConfirmed: o usuario ja aceitou descartar as alteracoes. */
    void openFile (const juce::File&, bool discardConfirmed = false);

    // StripHost
    Engine& getEngine() override { return engine; }
    void insertSlotClicked (Channel&, int slot, juce::Component&, const juce::MouseEvent&) override;
    void channelMenu (Channel&, juce::Component&) override;
    void sendMenu (Track&, int send, juce::Component&) override;
    void outputMenu (Track&, juce::Component&) override;
    bool isEditorOpen (const Channel&, int slot) const override;
    void markDirty() override;
    void automationTouch (Channel&, AutoTarget, bool down) override;
    void muteToggled (Channel&) override;
    void autoModeMenu (Channel&, juce::Component&) override;
    void lanesChanged() override;
    void addInsertSlot (Channel& c) override { setInsertSlots (c, c.inserts.size() + 1); }
    void addSendSlot (Track& t) override { setSendSlots (t, t.sends.size() + 1); }

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // projeto
    void newProject();
    void openProjectDialog();
    void openMenu();
    void addRecentFile (const juce::File&);
    void save (std::function<void (bool)> after = {});
    void saveAs (std::function<void (bool)> after = {});
    bool writeTo (const juce::File&);
    void confirmDiscard (std::function<void()> proceed);
    void resetSession();
    void updateTitle();
    // Recuperacao: snapshots XML separados, sem sobrescrever o projeto normal.
    juce::File recoveryDirectory() const;
    juce::File recoveryFileFor (const juce::File&) const;
    void writeRecoverySnapshot();
    void offerRecoveryOnStartup();


    // audio
    void importDialog();
    void importFiles (const juce::Array<juce::File>&);
    void replaceAudio (Track&);
    void startResampleIfNeeded();
    void exportDialog();
    /** Mix estereo: escolhe o arquivo e exporta (rangeEnd < 0 = musica inteira). */
    void exportMixTo (int bits, juce::int64 rangeStart, juce::int64 rangeEnd);
    /** Faixas individuais (stems): dialogo com faixas, trecho e formato. */
    void stemsDialog (bool selectionOnly);
    void runStemExport (Engine::StemExport request);
    void timelineMenu (juce::Component&) override;
    void playSelection() override;
    void selectWholeSong();

    // plugins
    void showPluginMenu (Channel&, int slot, juce::Component&, bool replacing);
    void loadPluginInto (Channel&, int slot, const juce::PluginDescription&);
    void loadPluginIntoRemote (Channel&, int slot, const juce::PluginDescription&,
                               const juce::MemoryBlock& state = {});
    void loadInserts (Channel&, const ChannelData&, juce::StringArray& problems);
    // Plugin offline / congelar faixa (economia de CPU e memoria)
    void setInsertOffline (Channel&, int slot);
    bool createFromSlot (const InsertSlot&, Engine::RestoredInsert&, juce::String& error);
    /** Recria os plugins offline (onlySlot = -1: todos). Falhas ficam offline. */
    void restoreOfflineInserts (Channel&, int onlySlot, bool unfreeze, juce::StringArray& failed);
    void setChannelPluginsOffline (Channel&);
    void freezeTrack (Track&);
    void unfreezeTrack (Track&);
    juce::File freezeFileFor (const Track&) const;
    void openEditor (Channel&, int slot);
    PluginScaleMode scaleModeFor (const juce::PluginDescription&) const;
    void setScaleMode (const juce::PluginDescription&, PluginScaleMode);
    void closeEditor (Channel&, int slot);
    void closeEditorsFor (Channel&);
    void closeAllEditors();
    void showPluginManager (bool startScan);
    void scanPluginsIsolated (bool retryFailed = false, bool probeDsp = false);
    void showAudioSettings();
    void audioMenu();
    juce::Rectangle<int> statusArea() const;

    // estrutura
    void rebuildStrips();
    void refreshStrips();
    ChannelStrip* stripFor (const Channel*);
    bool channelExists (const Channel*) const;
    void removeChannel (Channel&);
    Bus* createBus();
    void setInsertSlots (Channel&, int count);
    void setSendSlots (Track&, int count);

    void runJob (const juce::String& title, bool cancellable, std::function<void (Job&)> work, std::function<void (Job&)> done);
    void togglePlay();
    void setAutomationVisible (bool);
    void recordTick();
    void alert (const juce::String& title, const juce::String& text, bool warning = true);

    juce::ApplicationProperties& props;
    SilverLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };
    juce::AudioDeviceManager deviceManager;
    PluginHost pluginHost;
    Engine engine;

    IconButton toStartButton { IconButton::Shape::toStart, "Voltar ao inicio (Home)" },
               playButton { IconButton::Shape::play, "Tocar / pausar (Espaco)" },
               stopButton { IconButton::Shape::stop, "Parar e voltar ao ponto de inicio" },
               loopButton { IconButton::Shape::loop, "Loop da selecao (L). Arraste na regua para selecionar um trecho" };
    Timeline timeline { engine };
    juce::TextButton newButton { "Novo" }, openButton { "Abrir" }, saveButton { "Salvar" },
                     importButton { juce::String::fromUTF8 ("Importar \xc3\xa1udio") }, addBusButton { "+ Bus" },
                     exportButton { "Exportar" }, pluginsButton { "Plugins" }, audioButton { juce::String::fromUTF8 ("\xc3\x81udio") };

    juce::Viewport mixerViewport;
    juce::Component mixerContent;
    juce::OwnedArray<ChannelStrip> strips;
    std::unique_ptr<ChannelStrip> masterStrip;
    AutomationRecorder recorder { engine };
    std::unique_ptr<AutomationView> automationView;
    juce::TextButton automationButton { juce::String::fromUTF8 ("Automa\xc3\xa7\xc3\xa3o") };
    bool showAutomation = false;
    struct CallbackTimer : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } autoTimer;
    BusyOverlay overlay;

    std::vector<std::unique_ptr<PluginWindow>> pluginWindows;
    std::unique_ptr<juce::DocumentWindow> pluginManagerWindow;

    std::unique_ptr<Job> job;
    std::function<void (Job&)> jobDone;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::File currentFile;
    bool dirty = false;
    juce::int64 playStart = 0;
    juce::String statusText;
    int tickCount = 0;
    bool scanPromptShown = false;
    bool recoveryPromptShown = false;
    juce::int64 lastAutosaveMs = 0;
    juce::File openedRecoveryFile;
    juce::String unsavedRecoveryId;
    bool quitAfterJob = false; // fechar a janela durante uma tarefa: cancela e fecha ao terminar
    juce::RecentlyOpenedFilesList recentFiles;
    // gravacao de recuperacao fora da thread da interface (destruida antes dos dados acima)
    juce::ThreadPool backgroundWrites { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("SilverConsole autosave") };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace sc
