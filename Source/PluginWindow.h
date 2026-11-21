// SilverConsole - janela do editor de plugin. Codigo original (MIT).
#pragma once
#include <JuceHeader.h>
#include "SilverLookAndFeel.h"

namespace sc
{
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessor& p, const juce::String& title, std::function<void (PluginWindow*)> closeFn)
        : DocumentWindow (title, colours::background, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          processor (p), onClose (std::move (closeFn))
    {
        setUsingNativeTitleBar (true);
        juce::AudioProcessorEditor* editor = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor (p);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);

        static int cascade = 0;
        const auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                              ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea
                              : juce::Rectangle<int> (0, 0, 1280, 800);
        const int offset = 40 + (cascade++ % 8) * 28;
        setTopLeftPosition (juce::jmin (area.getX() + 120 + offset, area.getRight() - getWidth()),
                            juce::jmin (area.getY() + 80 + offset, area.getBottom() - getHeight()));
        setVisible (true);
    }

    ~PluginWindow() override { clearContentComponent(); }

    void closeButtonPressed() override { if (onClose) onClose (this); }

    juce::AudioProcessor& processor;

private:
    std::function<void (PluginWindow*)> onClose;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};
} // namespace sc
