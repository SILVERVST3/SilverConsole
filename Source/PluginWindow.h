// SilverConsole - janela do editor de plugin. Codigo original (MIT).
#pragma once
#include <JuceHeader.h>
#include "SilverLookAndFeel.h"

namespace sc
{
/** Como a janela trata a escala (DPI) de um plugin. Guardado por plugin. */
enum class PluginScaleMode
{
    automatic = 0,   // detecta escala dupla (plugin gigante) e corrige
    native = 1,      // nunca corrige: o plugin recebe a escala do monitor
    fixDouble = 2,   // sempre corrige escala dupla
    compatibility = 3 // janela sem DPI: o Windows amplia a imagem
};

/** Janela do editor de um plugin.

    Escala no Windows (diagnosticado com BSA Drum Bus num monitor a 250%):
    - O host informa ao plugin a escala do monitor (IPlugViewContentScaleSupport).
      Alguns plugins TAMBEM detectam o DPI sozinhos e aplicam a escala duas vezes:
      a 250% abrem com 625% (1920x2560 em vez de 768x1024).
    - Correcao: informar ao plugin escala efetiva 1.0 (setScaleFactor 1/monitor).
      O plugin continua desenhando nitido com a escala que ele mesmo detecta.
      No modo automatico isso so e feito quando o editor abre muito maior que a tela.
    - Plugins redimensionaveis que ainda nao cabem sao reduzidos para caber no
      monitor (o proprio plugin redesenha menor). Os nao redimensionaveis ficam
      numa area com rolagem dentro de uma janela do tamanho do monitor.
    - A janela nunca herda escala global da interface (getDesktopScaleFactor = 1). */
class PluginWindow : public juce::DocumentWindow,
                     private juce::Timer
{
public:
    PluginWindow (juce::AudioProcessor& p, const juce::String& title,
                  std::function<void (PluginWindow*)> closeFn, PluginScaleMode scaleMode)
        : DocumentWindow (title, colours::background, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          processor (p), onClose (std::move (closeFn)), mode (scaleMode)
    {
        // A janela (e a janela nativa do plugin dentro dela) e criada neste escopo.
        std::unique_ptr<juce::ScopedDPIAwarenessDisabler> dpi;
        if (mode == PluginScaleMode::compatibility) dpi = std::make_unique<juce::ScopedDPIAwarenessDisabler>();

        setUsingNativeTitleBar (true);
        auto* created = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        const bool generic = created == nullptr;
        editor.reset (generic ? new juce::GenericAudioProcessorEditor (p) : created);

        const auto* display = targetDisplay();
        const auto area = display != nullptr ? display->userArea : juce::Rectangle<int> (0, 0, 1280, 800);
        monitorScale = display != nullptr ? (float) display->scale : 1.0f;
        if (mode == PluginScaleMode::compatibility) monitorScale = 1.0f; // o Windows escala a janela
        const int maxW = juce::jmax (320, area.getWidth() - 24);
        const int maxH = juce::jmax (240, area.getHeight() - 48); // espaco para a barra de titulo

        // Escala dupla: o tamanho aparece ja multiplicado pela escala do monitor.
        if (! generic && monitorScale > 1.01f
            && (mode == PluginScaleMode::fixDouble
                || (mode == PluginScaleMode::automatic
                    && (editor->getWidth() > area.getWidth() * 1.4f || editor->getHeight() > area.getHeight() * 1.4f))))
        {
            editor->setScaleFactor (1.0f / monitorScale);
            scaleCorrected = true;
        }

        if ((editor->getWidth() > maxW || editor->getHeight() > maxH) && ! editor->isResizable())
        {
            // Maior que a tela e sem redimensionamento: rolagem, janela do tamanho do monitor.
            viewport = std::make_unique<juce::Viewport>();
            viewport->setScrollBarsShown (true, true);
            viewport->setViewedComponent (editor.get(), false);
            setContentNonOwned (viewport.get(), false);
            const int sb = viewport->getScrollBarThickness();
            setContentComponentSize (juce::jmin (maxW, editor->getWidth() + sb),
                                     juce::jmin (maxH, editor->getHeight() + sb));
            setResizable (true, false);
            setResizeLimits (240, 160, 16384, 16384);
        }
        else
        {
            setContentNonOwned (editor.get(), true);
            setResizable (editor->isResizable(), false);
            setConstrainer (&constrainer); // respeita os limites de tamanho do proprio plugin
        }

        static int cascade = 0;
        const int offset = 40 + (cascade++ % 8) * 28;
        placeInside (area, area.getX() + 60 + offset, area.getY() + 40 + offset);
        setVisible (true);
        // O plugin so recebe a escala real depois que a janela aparece (assincrono).
        startTimer (120);
    }

    ~PluginWindow() override
    {
        stopTimer();
        clearContentComponent();
        if (viewport != nullptr) viewport->setViewedComponent (nullptr, false);
        viewport.reset();
        editor.reset(); // avisa o processador (editorBeingDeleted)
    }

    void closeButtonPressed() override { if (onClose) onClose (this); }

    // Sem escala global do host sobre o editor (o plugin trata o DPI).
    float getDesktopScaleFactor() const override { return 1.0f; }

    juce::AudioProcessor& processor;
    bool isScrolled() const noexcept { return viewport != nullptr; }
    bool isScaleCorrected() const noexcept { return scaleCorrected; }

private:
    void timerCallback() override
    {
        ++ticks;
        // Janela movida para outro monitor: mantem a correcao coerente.
        if (scaleCorrected && mode != PluginScaleMode::compatibility)
            if (auto* peer = getPeer())
            {
                const auto s = (float) peer->getPlatformScaleFactor();
                if (s > 0.5f && std::abs (s - monitorScale) > 0.01f)
                {
                    monitorScale = s;
                    editor->setScaleFactor (1.0f / s);
                    ticks = 0; // refaz o ajuste ao monitor
                }
            }

        if (ticks <= 10 && viewport == nullptr && editor->isResizable()) fitToMonitor();
        if (ticks == 10) startTimer (500); // depois so acompanha troca de monitor
    }

    /** Reduz um editor redimensionavel que nao cabe no monitor. */
    void fitToMonitor()
    {
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds());
        if (display == nullptr) return;
        const auto area = display->userArea;
        const auto border = getContentComponentBorder();
        juce::BorderSize<int> frame;
        if (auto* peer = getPeer()) if (const auto f = peer->getFrameSizeIfPresent()) frame = *f;
        const int maxW = area.getWidth() - border.getLeftAndRight() - frame.getLeftAndRight() - 8;
        const int maxH = area.getHeight() - border.getTopAndBottom() - frame.getTopAndBottom() - 8;
        const int w = editor->getWidth(), h = editor->getHeight();
        if (w <= maxW && h <= maxH) return;
        const double k = juce::jmin ((double) maxW / w, (double) maxH / h);
        editor->setSize (juce::jmax (100, (int) (w * k)), juce::jmax (80, (int) (h * k)));
        // O plugin pode arredondar o tamanho (proporcao fixa); a janela acompanha.
        placeInside (area, getX(), getY());
    }

    const juce::Displays::Display* targetDisplay() const
    {
        auto& displays = juce::Desktop::getInstance().getDisplays();
        if (auto* d = displays.getDisplayForPoint (juce::Desktop::getMousePosition())) return d;
        return displays.getPrimaryDisplay();
    }

    /** Mantem a barra de titulo e as bordas dentro do monitor. */
    void placeInside (juce::Rectangle<int> area, int x, int y)
    {
        const int w = getWidth(), h = getHeight();
        x = juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - w), x);
        y = juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - h), y);
        setTopLeftPosition (x, y);
    }

    /** Repassa os limites de tamanho do editor, somando a moldura da janela. */
    class EditorConstrainer final : public juce::BorderedComponentBoundsConstrainer
    {
    public:
        explicit EditorConstrainer (juce::DocumentWindow& w) : window (w) {}
        juce::ComponentBoundsConstrainer* getWrappedConstrainer() const override
        {
            auto* ed = dynamic_cast<juce::AudioProcessorEditor*> (window.getContentComponent());
            return ed != nullptr ? ed->getConstrainer() : nullptr;
        }
        juce::BorderSize<int> getAdditionalBorder() const override
        {
            juce::BorderSize<int> frame;
            if (auto* peer = window.getPeer())
                if (const auto size = peer->getFrameSizeIfPresent()) frame = *size;
            return frame.addedTo (window.getContentComponentBorder());
        }
    private:
        juce::DocumentWindow& window;
    };

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<juce::Viewport> viewport;
    EditorConstrainer constrainer { *this };
    std::function<void (PluginWindow*)> onClose;
    PluginScaleMode mode;
    float monitorScale = 1.0f;
    bool scaleCorrected = false;
    int ticks = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};
} // namespace sc
