// SilverConsole - visual SILVER escuro (mesma paleta do SILVER AXES / SILVER ANGEL):
// fundo quase preto, placas azul-marinho, chumbo, prata e LEDs azul-gelo. Codigo original (MIT).
#pragma once
#include <JuceHeader.h>

namespace sc
{
namespace colours
{
    const juce::Colour background  { 0xff0a0b0d };
    const juce::Colour panel       { 0xff1c2026 }; // chumbo escuro (barras, dialogos)
    const juce::Colour panelLine   { 0xff3b4149 }; // chumbo
    const juce::Colour silverTop   { 0xffdde3ea }; // prata
    const juce::Colour silverBot   { 0xffaab3be }; // prata apagado
    const juce::Colour silverDark  { 0xff5b636d };
    const juce::Colour ink         { 0xff101418 }; // texto sobre fundo claro/colorido
    const juce::Colour inkSoft     { 0xff3a414a };
    const juce::Colour text        { 0xffdde3ea };
    const juce::Colour textDim     { 0xff8a94a0 };
    const juce::Colour lcd         { 0xff040a16 }; // tela
    const juce::Colour lcdText     { 0xff8cc4ff }; // azul gelo
    const juce::Colour accent      { 0xff8cc4ff }; // azul gelo (arcos, LEDs, linhas)
    const juce::Colour accentDeep  { 0xff2f5d96 }; // azul gelo escuro (botao ligado, selecao)
    const juce::Colour navyHi      { 0xff1a3468 }; // placa azul-marinho
    const juce::Colour navyLo      { 0xff081429 };
    const juce::Colour navyDeep    { 0xff050c1a };
    const juce::Colour gunHi       { 0xff5a626d }; // cinza chumbo
    const juce::Colour gun         { 0xff3b4149 };
    const juce::Colour gunLo       { 0xff23272c };
    const juce::Colour grid        { 0xff13264a };
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
        setColour (juce::TextButton::buttonColourId, gun);
        setColour (juce::TextButton::buttonOnColourId, accentDeep);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::Label::textColourId, text);
        setColour (juce::Slider::thumbColourId, silverTop);
        setColour (juce::Slider::trackColourId, accent);
        setColour (juce::Slider::backgroundColourId, lcd);
        setColour (juce::PopupMenu::backgroundColourId, navyDeep);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accentDeep);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TooltipWindow::backgroundColourId, navyDeep);
        setColour (juce::TooltipWindow::textColourId, text);
        setColour (juce::TooltipWindow::outlineColourId, accentDeep);
        setColour (juce::AlertWindow::backgroundColourId, panel);
        setColour (juce::AlertWindow::textColourId, text);
        setColour (juce::AlertWindow::outlineColourId, gun);
        setColour (juce::ComboBox::textColourId, lcdText);
        setColour (juce::ComboBox::outlineColourId, gun);
        setColour (juce::ComboBox::arrowColourId, silverBot);
        setColour (juce::ToggleButton::textColourId, text);
        setColour (juce::ToggleButton::tickColourId, accent);
        setColour (juce::ToggleButton::tickDisabledColourId, silverDark);
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
        setColour (juce::ProgressBar::foregroundColourId, accentDeep);
        setColour (juce::ProgressBar::backgroundColourId, lcd);
    }

    // placa de canal: azul-marinho (faixas/master) ou chumbo (buses), com textura escovada fina
    static void drawPlate (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour hi, juce::Colour lo, int seed)
    {
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.fillRoundedRectangle (r.translated (0.0f, 1.5f), 5.0f);
        g.setGradientFill ({ hi, r.getCentreX(), r.getY() + r.getHeight() * 0.35f, lo, r.getX(), r.getBottom(), true });
        g.fillRoundedRectangle (r, 5.0f);
        juce::Random rnd (seed);
        for (int y = (int) r.getY() + 2; y < (int) r.getBottom() - 2; y += 3)
        {
            g.setColour (juce::Colours::white.withAlpha (0.008f + rnd.nextFloat() * 0.018f));
            g.drawHorizontalLine (y, r.getX() + 2, r.getRight() - 2);
        }
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawHorizontalLine ((int) r.getY() + 1, r.getX() + 4, r.getRight() - 4);
        g.setColour (juce::Colours::black.withAlpha (0.9f));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int h) override { return uiFont (juce::jmin (13.0f, h * 0.62f), true); }
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }
    juce::Font getPopupMenuFont() override { return uiFont (14.0f); }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        // botao de chumbo (como os do SILVER AXES): gradiente vertical, brilho no topo, borda preta
        auto base = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : bg;
        if (over) base = base.brighter (0.12f);
        auto topC = base.brighter (0.28f), botC = base.darker (0.35f);
        if (down) std::swap (topC, botC);
        g.setGradientFill ({ topC, 0, r.getY(), botC, 0, r.getBottom(), false });
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (juce::Colours::white.withAlpha (down ? 0.04f : 0.16f));
        g.drawHorizontalLine ((int) r.getY() + 1, r.getX() + 3.0f, r.getRight() - 3.0f);
        g.setColour (juce::Colours::black);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
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
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRoundedRectangle (cap.translated (0, 2), 3.0f);
        g.setGradientFill ({ juce::Colour (0xfff6f8fb), 0, cap.getY(), juce::Colour (0xff6b737e), 0, cap.getBottom(), false });
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
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.strokePath (bg, juce::PathStrokeType (3.0f));
        const auto arcColour = s.findColour (juce::Slider::trackColourId);
        g.setColour (arcColour.withAlpha (0.25f));
        g.strokePath (arc, juce::PathStrokeType (5.0f));
        g.setColour (arcColour);
        g.strokePath (arc, juce::PathStrokeType (2.5f));

        // knob preto (como os do SILVER AXES): sombra, corpo escuro com brilho e ponteiro prata
        auto body = knob.reduced (5.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillEllipse (body.translated (0.0f, 1.5f));
        g.setGradientFill ({ juce::Colour (0xff3a3d43), body.getX() + body.getWidth() * 0.2f, body.getY(),
                             juce::Colour (0xff040405), body.getRight() - body.getWidth() * 0.2f, body.getBottom(), false });
        g.fillEllipse (body);
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.drawEllipse (body.reduced (0.5f), 1.0f);
        g.setColour (juce::Colours::black);
        g.drawEllipse (body, 1.0f);
        juce::Path ptr;
        ptr.addRoundedRectangle (-1.25f, -body.getHeight() * 0.5f + 2.0f, 2.5f, body.getHeight() * 0.40f, 1.0f);
        g.setColour (colours::silverTop);
        g.fillPath (ptr, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
    }

    void drawProgressBar (juce::Graphics& g, juce::ProgressBar&, int w, int h, double progress, const juce::String& text) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
        g.setColour (colours::lcd);
        g.fillRoundedRectangle (r, 3.0f);
        if (progress >= 0)
        {
            g.setColour (colours::accentDeep);
            g.fillRoundedRectangle (r.withWidth ((float) (w * juce::jlimit (0.0, 1.0, progress))), 3.0f);
        }
        g.setColour (juce::Colours::white);
        g.setFont (uiFont (13.0f, true));
        g.drawText (text, r, juce::Justification::centred);
    }
};
} // namespace sc
