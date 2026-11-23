// SilverConsole - diagnostico da janela de plugin (nao vai no pacote).
// Uso: ScPluginWindowProbe <plugin.vst3> <log.txt> [compat|native|fix] [png]
// Abre o editor com a mesma PluginWindow do app, registra tamanhos/escala por
// alguns segundos, opcionalmente salva uma imagem da janela e fecha sozinho.
#include <JuceHeader.h>
#include "PluginWindow.h"
#include <windows.h>

namespace
{
juce::String rectText (juce::Rectangle<int> r) { return r.toString(); }

void captureWindow (juce::Component& w, const juce::File& png)
{
    auto* peer = w.getPeer();
    if (peer == nullptr) return;
    auto hwnd = (HWND) peer->getNativeHandle();
    RECT rc {};
    GetWindowRect (hwnd, &rc);
    const int width = rc.right - rc.left, height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;
    HDC screen = GetDC (nullptr);
    HDC mem = CreateCompatibleDC (screen);
    HBITMAP bmp = CreateCompatibleBitmap (screen, width, height);
    auto old = SelectObject (mem, bmp);
    PrintWindow (hwnd, mem, 2 /* PW_RENDERFULLCONTENT */);
    juce::Image img (juce::Image::ARGB, width, height, true);
    {
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        BITMAPINFO bi {};
        bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<juce::uint32> px ((size_t) width * (size_t) height);
        GetDIBits (mem, bmp, 0, (UINT) height, px.data(), &bi, DIB_RGB_COLORS);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                bd.setPixelColour (x, y, juce::Colour (px[(size_t) y * (size_t) width + (size_t) x] | 0xff000000u));
    }
    SelectObject (mem, old);
    DeleteObject (bmp);
    DeleteDC (mem);
    ReleaseDC (nullptr, screen);
    png.deleteFile();
    if (auto out = png.createOutputStream())
    {
        juce::PNGImageFormat f;
        f.writeImageToStream (img.rescaled (juce::jmax (1, width / 2), juce::jmax (1, height / 2)), *out);
    }
}
} // namespace

class ProbeApp : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "ScPluginWindowProbe"; }
    const juce::String getApplicationVersion() override { return "1"; }

    void initialise (const juce::String&) override
    {
        auto args = getCommandLineParameterArray();
        if (args.size() < 2) { quit(); return; }
        log = juce::File (args[1]);
        log.replaceWithText ({});
        auto mode = sc::PluginScaleMode::automatic;
        if (args.contains ("compat")) mode = sc::PluginScaleMode::compatibility;
        if (args.contains ("native")) mode = sc::PluginScaleMode::native;
        if (args.contains ("fix")) mode = sc::PluginScaleMode::fixDouble;
        if (args.size() > 2 && args[args.size() - 1].endsWithIgnoreCase (".png")) png = juce::File (args[args.size() - 1]);

        for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
            write ("display total=" + rectText (d.totalArea) + " user=" + rectText (d.userArea)
                   + " scale=" + juce::String (d.scale) + " dpi=" + juce::String (d.dpi) + (d.isMain ? " main" : ""));
        write ("globalScale=" + juce::String (juce::Desktop::getInstance().getGlobalScaleFactor()) + " mode=" + juce::String ((int) mode));

        formats.addFormat (new juce::VST3PluginFormat());
        juce::OwnedArray<juce::PluginDescription> found;
        juce::VST3PluginFormat vst3;
        vst3.findAllTypesForFile (found, args[0]);
        if (found.isEmpty()) { write ("plugin nao encontrado"); quit(); return; }
        juce::String err;
        instance = formats.createPluginInstance (*found[0], 48000.0, 512, err);
        if (instance == nullptr) { write ("erro: " + err); quit(); return; }
        write ("plugin=" + instance->getName());
        window = std::make_unique<sc::PluginWindow> (*instance, "probe", [] (sc::PluginWindow*) {}, mode);
        startTimer (250);
    }

    void timerCallback() override
    {
        ++ticks;
        if (window != nullptr)
        {
            juce::String s ("t=" + juce::String (ticks * 250) + "ms window=" + rectText (window->getBounds()));
            if (auto* c = window->getContentComponent()) s << " content=" << rectText (c->getBounds());
            juce::AudioProcessorEditor* ed = instance->getActiveEditor();
            if (ed != nullptr) s << " editor=" << rectText (ed->getBounds()) << " resizable=" << (int) ed->isResizable()
                                 << " editorScale=" << ed->getDesktopScaleFactor();
            if (auto* peer = window->getPeer()) s << " peerScale=" << peer->getPlatformScaleFactor();
            s << " windowResizable=" << (int) window->isResizable() << " scrolled=" << (int) window->isScrolled() << " corrected=" << (int) window->isScaleCorrected();
            write (s);
            if (ticks == 16 && png != juce::File()) captureWindow (*window, png);
        }
        if (ticks >= 20)
        {
            stopTimer();
            window.reset();
            instance.reset();
            quit();
        }
    }

    void shutdown() override { window.reset(); instance.reset(); }

private:
    void write (const juce::String& s) { log.appendText (s + "\n"); }

    juce::AudioPluginFormatManager formats;
    std::unique_ptr<juce::AudioPluginInstance> instance;
    std::unique_ptr<sc::PluginWindow> window;
    juce::File log, png;
    int ticks = 0;
};

START_JUCE_APPLICATION (ProbeApp)
