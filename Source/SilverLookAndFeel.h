// SilverConsole - visual SILVER (grafite + aluminio escovado). Codigo original (MIT).
#pragma once
#include <JuceHeader.h>

namespace sc
{
namespace colours
{
    const juce::Colour background  { 0xff11161c };
    const juce::Colour panel       { 0xff202832 };
    const juce::Colour panelLine   { 0xff35414d };
    const juce::Colour silverTop   { 0xffdfe3e7 };
    const juce::Colour silverBot   { 0xffa9b0b8 };
    const juce::Colour silverDark  { 0xff7d858e };
    const juce::Colour ink         { 0xff1b1f23 };
    const juce::Colour inkSoft     { 0xff4a525b };
    const juce::Colour text        { 0xffe4e8ec };
    const juce::Colour textDim     { 0xff8b949e };
    const juce::Colour lcd         { 0xff12161a };
    const juce::Colour lcdText     { 0xff8fd8ff };
    const juce::Colour accent      { 0xff46b9ed };
    const juce::Colour mute        { 0xffe0892c };
    const juce::Colour solo        { 0xffe8c42a };
    const juce::Colour danger      { 0xffe2453c };
    const juce::Colour busTint     { 0xffffc46b };

    inline juce::Colour trackPalette (int i)
    {
        static const juce::uint32 p[] { 0xff5ec8ff, 0xff7be08f, 0xffffb74d, 0xffff7a8a, 0xffb48cff,
                                        0xff4dd9c9, 0xfff2e05a, 0xffff9de2, 0xff9fb8ff, 0xffc7e86b };
        return juce::Colour (p[(size_t) (juce::jmax (0, i) % 10)]);
    }
}

inline juce::Font uiFont (float h, bool bold = false)
{
    return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
}

class SilverLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SilverLookAndFeel()
    {
        using namespace colours;
        setColourScheme ({ panel, background, panel, panelLine, text, accent, juce::Colours::black, accent, text });
        setColour (juce::ResizableWindow::backgroundColourId, background);
        setColour (juce::DocumentWindow::backgroundColourId, background);
        setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a3036));
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, ink);
        setColour (juce::Label::textColourId, text);
        setColour (juce::Slider::thumbColourId, silverTop);
        setColour (juce::Slider::trackColourId, accent);
        setColour (juce::Slider::backgroundColourId, lcd);
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff23282e));
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour (juce::PopupMenu::highlightedTextColourId, ink);
        setColour (juce::PopupMenu::headerTextColourId, textDim);
        setColour (juce::TextEditor::backgroundColourId, lcd);
        setColour (juce::TextEditor::textColourId, lcdText);
        setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.4f));
        setColour (juce::ScrollBar::thumbColourId, silverDark);
        setColour (juce::ScrollBar::trackColourId, background);
        setColour (juce::Slider::textBoxTextColourId, lcdText);
        setColour (juce::Slider::textBoxBackgroundColourId, lcd);
        setColour (juce::Slider::textBoxOutlineColourId, panelLine);
        setColour (juce::ListBox::backgroundColourId, panel);
        setColour (juce::ComboBox::backgroundColourId, lcd);
        setColour (juce::ProgressBar::foregroundColourId, accent);
        setColour (juce::ProgressBar::backgroundColourId, lcd);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int h) override { return uiFont (juce::jmin (13.0f, h * 0.62f), true); }
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }
    juce::Font getPopupMenuFont() override { return uiFont (14.0f); }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto base = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : bg;
        if (down) base = base.darker (0.25f);
        else if (over) base = base.brighter (0.12f);
        g.setGradientFill ({ base.brighter (0.10f), 0, r.getY(), base.darker (0.12f), 0, r.getBottom(), false });
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (over ? 0.18f : 0.07f));
        g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            // envio: barra horizontal fina
            auto r = juce::Rectangle<float> ((float) x, (float) y + h * 0.5f - 3.0f, (float) w, 6.0f);
            g.setColour (colours::lcd);
            g.fillRoundedRectangle (r, 2.0f);
            if (! s.isEnabled()) return; // envio sem destino: so o trilho vazio
            g.setColour (s.findColour (juce::Slider::trackColourId));
            g.fillRoundedRectangle (r.withRight (pos), 2.0f);
            g.setColour (colours::silverTop);
            g.fillRoundedRectangle (juce::Rectangle<float> (5.0f, (float) h - 2.0f).withCentre ({ pos, (float) y + h * 0.5f }), 1.5f);
            return;
        }

        // fader de console
        const float cx = x + w * 0.5f;
        auto groove = juce::Rectangle<float> (cx - 2.5f, (float) y, 5.0f, (float) h);
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (groove, 2.5f);
        g.setColour (colours::panelLine);
        g.drawRoundedRectangle (groove, 2.5f, 1.0f);

        auto cap = juce::Rectangle<float> (juce::jmin ((float) w - 4.0f, 36.0f), 24.0f).withCentre ({ cx, pos });
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (cap.translated (0, 2), 3.0f);
        g.setGradientFill ({ colours::silverTop, 0, cap.getY(), colours::silverDark, 0, cap.getBottom(), false });
        g.fillRoundedRectangle (cap, 3.0f);
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.drawHorizontalLine ((int) cap.getY() + 2, cap.getX() + 3, cap.getRight() - 3);
        g.setColour (colours::ink);
        g.fillRect (cap.getX() + 3, cap.getCentreY() - 1.0f, cap.getWidth() - 6, 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);
        juce::ignoreUnused (s);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s) override
    {
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
        const float size = juce::jmin (r.getWidth(), r.getHeight());
        auto knob = juce::Rectangle<float> (size, size).withCentre (r.getCentre());
        const auto c = knob.getCentre();
        const float radius = size * 0.5f;
        const float angle = a0 + pos * (a1 - a0);

        // arco: bipolar (pan) parte do centro
        const bool bipolar = s.getMinimum() < 0 && s.getMaximum() > 0;
        const float from = bipolar ? (a0 + a1) * 0.5f : a0;
        juce::Path bg, arc;
        bg.addCentredArc (c.x, c.y, radius - 1.5f, radius - 1.5f, 0, a0, a1, true);
        arc.addCentredArc (c.x, c.y, radius - 1.5f, radius - 1.5f, 0, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (colours::lcd);
        g.strokePath (bg, juce::PathStrokeType (3.0f));
        g.setColour (s.findColour (juce::Slider::trackColourId));
        g.strokePath (arc, juce::PathStrokeType (3.0f));

        auto body = knob.reduced (5.0f);
        g.setGradientFill ({ colours::silverTop, body.getX(), body.getY(), colours::silverDark, body.getRight(), body.getBottom(), false });
        g.fillEllipse (body);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawEllipse (body, 1.0f);
        juce::Path ptr;
        ptr.addRoundedRectangle (-1.25f, -body.getHeight() * 0.5f + 2.0f, 2.5f, body.getHeight() * 0.38f, 1.0f);
        g.setColour (colours::ink);
        g.fillPath (ptr, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
    }

    void drawProgressBar (juce::Graphics& g, juce::ProgressBar&, int w, int h, double progress, const juce::String& text) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
        g.setColour (colours::lcd);
        g.fillRoundedRectangle (r, 3.0f);
        if (progress >= 0)
        {
            g.setColour (colours::accent);
            g.fillRoundedRectangle (r.withWidth ((float) (w * juce::jlimit (0.0, 1.0, progress))), 3.0f);
        }
        g.setColour (colours::text);
        g.setFont (uiFont (13.0f, true));
        g.drawText (text, r, juce::Justification::centred);
    }
};
} // namespace sc
