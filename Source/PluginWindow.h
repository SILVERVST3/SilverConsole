// SilverConsole - janela do editor de plugin. Codigo original (MIT).
#pragma once
#include <JuceHeader.h>
#include "SilverLookAndFeel.h"

namespace sc
{
/** Janela do editor de um plugin.

    Escala no Windows:
    - O editor nao herda nenhuma escala global da interface do SilverConsole
      (getDesktopScaleFactor = 1): so o DPI do monitor, aplicado pelo proprio
      plugin. Evita dupla escala.
    - "Modo de compatibilidade de escala" (por plugin): a janela e criada sem
      reconhecimento de DPI e o Windows amplia a imagem. Resolve plugins que
      abrem gigantes ou minusculos em telas com 125%/150%/200%.
    - Se o editor for maior que a tela, ele fica dentro de uma area com barras
      de rolagem e a janela cabe no monitor, com bordas para redimensionar. */
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessor& p, const juce::String& title,
                  std::function<void (PluginWindow*)> closeFn, bool compatibilityScaling)
        : DocumentWindow (title, colours::background, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          processor (p), onClose (std::move (closeFn))
    {
        // A janela (e a janela nativa do plugin dentro dela) e criada neste escopo.
        std::unique_ptr<juce::ScopedDPIAwarenessDisabler> dpi;
        if (compatibilityScaling) dpi = std::make_unique<juce::ScopedDPIAwarenessDisabler>();

        setUsingNativeTitleBar (true);
        juce::AudioProcessorEditor* editor = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor (p);

        const auto area = screenArea();
        const int maxW = juce::jmax (320, area.getWidth() - 24);
        const int maxH = juce::jmax (240, area.getHeight() - 48); // espaco para a barra de titulo

        if (editor->getWidth() > maxW || editor->getHeight() > maxH)
        {
            // Maior que a tela: rolagem, janela do tamanho do monitor e redimensionavel.
            auto* view = new juce::Viewport();
            view->setScrollBarsShown (true, true);
            view->setViewedComponent (editor, true); // a viewport passa a ser dona do editor
            setContentOwned (view, false);
            const int sb = view->getScrollBarThickness();
            setContentComponentSize (juce::jmin (maxW, editor->getWidth() + sb),
                                     juce::jmin (maxH, editor->getHeight() + sb));
            setResizable (true, false);
            setResizeLimits (240, 160, 16384, 16384);
            scrolled = true;
        }
        else
        {
            setContentOwned (editor, true);
            setResizable (editor->isResizable(), false);
            setConstrainer (&constrainer); // respeita os limites de tamanho do proprio plugin
        }

        static int cascade = 0;
        const int offset = 40 + (cascade++ % 8) * 28;
        placeInside (area, area.getX() + 60 + offset, area.getY() + 40 + offset);
        setVisible (true);
    }

    ~PluginWindow() override { clearContentComponent(); }

    void closeButtonPressed() override { if (onClose) onClose (this); }

    // Sem escala global do host sobre o editor (o plugin trata o DPI).
    float getDesktopScaleFactor() const override { return 1.0f; }

    juce::AudioProcessor& processor;
    bool isScrolled() const noexcept { return scrolled; }

private:
    static juce::Rectangle<int> screenArea()
    {
        auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto mouse = juce::Desktop::getMousePosition();
        if (auto* d = displays.getDisplayForPoint (mouse)) return d->userArea;
        if (auto* d = displays.getPrimaryDisplay()) return d->userArea;
        return { 0, 0, 1280, 800 };
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
            auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (window.getContentComponent());
            return editor != nullptr ? editor->getConstrainer() : nullptr;
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

    EditorConstrainer constrainer { *this };
    std::function<void (PluginWindow*)> onClose;
    bool scrolled = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};
} // namespace sc
