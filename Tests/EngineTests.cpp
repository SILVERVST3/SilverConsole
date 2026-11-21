// Testes do motor do SilverConsole (sem interface).
// Uso: SilverConsoleTests [caminho do "SC Test Delay.vst3"]
#include <JuceHeader.h>
#include "Engine.h"
#include "AudioLoader.h"
#include "Project.h"
#include <cstdio>

using namespace sc;

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.toRawUTF8());
    if (! ok) ++failures;
}
static bool near (float a, float b, float tol = 1.0e-4f) { return std::abs (a - b) <= tol; }

// Processador interno com latencia (simula um plugin com lookahead).
class InternalDelay : public juce::AudioProcessor
{
public:
    explicit InternalDelay (int d)
        : AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo())
                                           .withOutput ("Out", juce::AudioChannelSet::stereo())), delay (d)
    { setLatencySamples (d); }
    const juce::String getName() const override { return "InternalDelay"; }
    void prepareToPlay (double, int) override { line.setDelay (delay); }
    void releaseResources() override {}
    void reset() override { line.clear(); }
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    { line.process (b.getWritePointer (0), b.getWritePointer (1), b.getNumSamples()); }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
private:
    int delay;
    DelayLine line;
};

static std::shared_ptr<const juce::AudioBuffer<float>> makeBuffer (int channels, int len, std::function<float (int, int)> fn)
{
    auto b = std::make_shared<juce::AudioBuffer<float>> (channels, len);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < len; ++i) b->setSample (c, i, fn (c, i));
    return b;
}

static Track* addTrack (Engine& e, std::shared_ptr<const juce::AudioBuffer<float>> buf, const juce::String& name)
{
    auto t = std::make_unique<Track>();
    t->name = name;
    auto* raw = e.addTrack (std::move (t));
    e.setTrackAudio (*raw, buf, e.getSampleRate(), buf, e.getSampleRate());
    return raw;
}

static juce::AudioBuffer<float> run (Engine& e, int total, juce::int64 startPos = 0, bool play = true)
{
    juce::AudioBuffer<float> out (2, total);
    const juce::ScopedLock sl (e.lock);
    juce::int64 pos = startPos;
    for (int done = 0; done < total;)
    {
        const int m = juce::jmin (e.getBlockSize(), total - done);
        pos = e.render (out.getWritePointer (0, done), out.getWritePointer (1, done), m, pos, play, true);
        done += m;
    }
    return out;
}

static int peakIndex (const juce::AudioBuffer<float>& b, int ch)
{
    int idx = 0; float best = 0;
    for (int i = 0; i < b.getNumSamples(); ++i)
        if (std::abs (b.getSample (ch, i)) > best) { best = std::abs (b.getSample (ch, i)); idx = i; }
    return idx;
}

static void testMixBasics()
{
    Engine e; e.prepare (48000, 256);
    auto* t = addTrack (e, makeBuffer (1, 48000, [] (int, int) { return 0.5f; }), "mono");
    auto out = run (e, 4096);
    const float expect = 0.5f * std::cos (juce::MathConstants<float>::pi * 0.25f);
    check (near (out.getSample (0, 4000), expect) && near (out.getSample (1, 4000), expect), "mono centro -3 dB");

    t->pan = -1.0f;
    out = run (e, 4096);
    check (near (out.getSample (0, 4000), 0.5f) && near (out.getSample (1, 4000), 0.0f), "mono pan total a esquerda");

    t->pan = 0.0f; t->gainDb = -6.0f;
    out = run (e, 4096);
    check (near (out.getSample (0, 4000), expect * juce::Decibels::decibelsToGain (-6.0f)), "fader -6 dB");

    t->gainDb = kMinDb;
    out = run (e, 4096);
    check (near (out.getSample (0, 4000), 0.0f), "fader no fim = silencio");
}

static void testStereoBalanceMuteSolo()
{
    Engine e; e.prepare (48000, 256);
    auto* a = addTrack (e, makeBuffer (2, 48000, [] (int c, int) { return c == 0 ? 0.5f : 0.25f; }), "st");
    auto* b = addTrack (e, makeBuffer (1, 48000, [] (int, int) { return 0.1f; }), "b");
    b->pan = 1.0f; // so na direita
    a->pan = -1.0f;
    auto out = run (e, 4096);
    check (near (out.getSample (0, 4000), 0.5f) && near (out.getSample (1, 4000), 0.1f), "balance estereo + soma");

    a->mute = true;
    out = run (e, 4096);
    check (near (out.getSample (0, 4000), 0.0f) && near (out.getSample (1, 4000), 0.1f), "mute");

    a->mute = false; a->solo = true;
    out = run (e, 4096);
    check (near (out.getSample (0, 4000), 0.5f) && near (out.getSample (1, 4000), 0.0f), "solo");
}

static void testBusesAndSends()
{
    Engine e; e.prepare (48000, 256);
    auto* t = addTrack (e, makeBuffer (2, 48000, [] (int, int) { return 0.25f; }), "t");
    auto* bus = e.addBus();
    bus->gainDb = -6.0f;
    t->outputBus = 0;
    auto out = run (e, 4096);
    check (near (out.getSample (0, 4000), 0.25f * juce::Decibels::decibelsToGain (-6.0f)), "saida da faixa no bus (-6 dB)");

    bus->gainDb = 0.0f;
    t->outputBus = -1;
    t->sends[0].bus = 0; t->sends[0].levelDb = 0.0f;
    out = run (e, 4096);
    check (near (out.getSample (0, 4000), 0.5f), "send pos-fader soma com o direto");

    t->gainDb = kMinDb;
    out = run (e, 4096);
    check (near (out.getSample (0, 4000), 0.0f), "send pos-fader segue o fader");

    t->sends[0].preFader = true;
    out = run (e, 4096);
    check (near (out.getSample (0, 4000), 0.25f), "send pre-fader ignora o fader");

    auto removed = e.removeBus (bus);
    check (t->sends[0].bus.load() == -1 && e.buses.empty(), "remover bus limpa os envios");
}

static void testLatencyCompensation()
{
    Engine e; e.prepare (48000, 256);
    auto impulse = makeBuffer (2, 8000, [] (int, int i) { return i == 1000 ? 1.0f : 0.0f; });
    auto* a = addTrack (e, impulse, "a");
    addTrack (e, impulse, "b");
    a->pan = -1.0f; // a so na esquerda (balance), b nos dois

    auto p = std::make_unique<InternalDelay> (100);
    check (e.prepareProcessor (*p).isEmpty(), "preparar processador interno");
    e.setInsert (*a, 0, std::move (p), nullptr);
    check (e.getTotalLatency() == 100, "latencia total = 100");

    auto out = run (e, 8000);
    check (peakIndex (out, 0) == 1100 && near (out.getSample (0, 1100), 2.0f), "PDC alinha faixas (esquerda soma 2x)");
    check (peakIndex (out, 1) == 1100, "PDC na direita");

    // bus com latencia: faixa direta precisa esperar o bus
    auto* bus = e.addBus();
    auto pb = std::make_unique<InternalDelay> (37);
    e.prepareProcessor (*pb);
    e.setInsert (*bus, 3, std::move (pb), nullptr);
    e.tracks[1]->outputBus = 0;
    check (e.getTotalLatency() == 137, "latencia faixa + bus = 137");
    out = run (e, 8000);
    check (peakIndex (out, 0) == 1137 && near (out.getSample (0, 1137), 2.0f), "PDC com bus");

    e.setBypass (*a, 0, true);
    check (e.getTotalLatency() == 37, "bypass remove a latencia do plugin");

    // exportacao remove a latencia: impulso volta para 1000
    e.setBypass (*a, 0, false);
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_export_test.wav");
    std::atomic<float> progress { 0 }; std::atomic<bool> cancel { false };
    juce::String err; bool clipped = false;
    const bool ok = e.exportMix (file, 32, 0.0, progress, cancel, err, clipped);
    check (ok, "exportar WAV 32 float " + err);
    juce::AudioFormatManager fm; registerAudioFormats (fm);
    auto loaded = loadAudioFile (fm, file);
    check (loaded.ok() && loaded.original->getNumSamples() == 8000, "WAV exportado com o tamanho certo");
    if (loaded.ok())
        check (peakIndex (*loaded.original, 0) == 1000 && near (loaded.original->getSample (0, 1000), 2.0f),
               "exportacao sem deslocamento de latencia");
    check (clipped, "aviso de clipping na exportacao (pico 2.0)");
    file.deleteFile();
}

static void testLoop()
{
    Engine e; e.prepare (48000, 256);
    addTrack (e, makeBuffer (1, 48000, [] (int, int i) { return (float) i / 48000.0f; }), "ramp");
    e.setLoop (0, 10000);
    e.loopOn = true;
    juce::AudioBuffer<float> out (2, 256);
    juce::int64 pos;
    {
        const juce::ScopedLock sl (e.lock);
        pos = e.render (out.getWritePointer (0), out.getWritePointer (1), 256, 9900, true, true);
    }
    check (pos == 156, "loop volta ao inicio (pos " + juce::String (pos) + ")");
}

static void testDeviceCallback()
{
    Engine e; e.prepare (48000, 256);
    addTrack (e, makeBuffer (1, 48000, [] (int, int) { return 0.5f; }), "dc");
    e.playing = true;
    juce::AudioBuffer<float> out (2, 1000); // maior que o bloco: o motor divide
    juce::AudioIODeviceCallbackContext ctx;
    for (int k = 0; k < 4; ++k)
        e.audioDeviceIOCallbackWithContext (nullptr, 0, out.getArrayOfWritePointers(), 2, 1000, ctx);
    check (e.position.load() == 4000, "callback da placa avanca o transporte (" + juce::String (e.position.load()) + ")");
    const float expect = 0.5f * std::cos (juce::MathConstants<float>::pi * 0.25f);
    check (near (out.getSample (0, 999), expect) && near (out.getSample (1, 999), expect), "callback entrega o audio");
    e.playing = false;
    e.audioDeviceIOCallbackWithContext (nullptr, 0, out.getArrayOfWritePointers(), 2, 1000, ctx);
    check (e.position.load() == 4000, "parado nao avanca");
}

static void testResampler()
{
    auto imp = makeBuffer (1, 44100, [] (int, int i) { return i == 1000 ? 1.0f : 0.0f; });
    auto r = resampleBuffer (imp, 44100.0, 48000.0);
    check (r->getNumSamples() == 48000, "44.1k -> 48k tamanho");
    check (peakIndex (*r, 0) == 1088, "44.1k -> 48k sem atraso (pico " + juce::String (peakIndex (*r, 0)) + ")");

    auto sine = makeBuffer (1, 96000, [] (int, int i) { return (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 96000.0)); });
    auto d = resampleBuffer (sine, 96000.0, 48000.0);
    float peak = 0; for (int i = 2000; i < 46000; ++i) peak = juce::jmax (peak, std::abs (d->getSample (0, i)));
    check (near (peak, 0.5f, 0.005f), "96k -> 48k mantem amplitude (" + juce::String (peak, 4) + ")");

    auto hf = makeBuffer (1, 96000, [] (int, int i) { return (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 30000.0 * i / 96000.0)); });
    auto a = resampleBuffer (hf, 96000.0, 48000.0);
    float alias = 0; for (int i = 2000; i < 46000; ++i) alias = juce::jmax (alias, std::abs (a->getSample (0, i)));
    check (alias < 0.0005f, "96k -> 48k filtra 30 kHz (anti-alias, " + juce::String (juce::Decibels::gainToDecibels (alias / 0.5f), 1) + " dB)");
}

static void testProjectRoundTrip()
{
    Engine e; e.prepare (48000, 256);
    auto* t = addTrack (e, makeBuffer (1, 4800, [] (int, int) { return 0.1f; }), "Voz");
    t->sourceFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("voz.wav");
    t->gainDb = -3.5f; t->pan = 0.25f; t->mute = true; t->phase = true;
    e.addBus ("Reverb");
    t->sends[1].bus = 0; t->sends[1].levelDb = -12.0f; t->sends[1].preFader = true;
    juce::PluginDescription desc; desc.name = "Plugin X"; desc.pluginFormatName = "VST3";
    desc.fileOrIdentifier = "C:/x.vst3"; desc.uniqueId = 42;
    juce::MemoryBlock state ("abc", 3);
    e.setMissingInsert (*t, 2, desc, state);
    e.master.mono = true; e.master.gainDb = -1.0f;

    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_test.sconsole");
    juce::String err;
    check (writeProject (captureProject (e), file, err), "salvar projeto " + err);
    ProjectData p;
    check (readProject (file, p, err), "abrir projeto " + err);
    check (p.tracks.size() == 1 && p.buses.size() == 1, "faixas e buses");
    if (p.tracks.size() == 1)
    {
        const auto& td = p.tracks[0];
        check (td.ch.name == "Voz" && near (td.ch.gainDb, -3.5f) && near (td.ch.pan, 0.25f) && td.ch.mute && td.ch.phase, "parametros da faixa");
        check (td.sends[1].bus == 0 && near (td.sends[1].levelDb, -12.0f) && td.sends[1].pre, "envio salvo");
        check (td.ch.inserts.size() == 1 && td.ch.inserts[0].slot == 2 && td.ch.inserts[0].desc.name == "Plugin X"
               && td.ch.inserts[0].state == state, "plugin ausente preserva descricao e estado");
        check (td.file == t->sourceFile, "caminho do audio");
    }
    check (p.masterMono && near (p.master.gainDb, -1.0f) && p.buses[0].name == "Reverb", "master e bus");
    file.deleteFile();
}

static juce::String scannedDescXml;

static void testVst3Hosting (const juce::File& pluginFile)
{
    if (! pluginFile.exists()) { check (false, "plugin de teste nao encontrado: " + pluginFile.getFullPathName()); return; }
    juce::AudioPluginFormatManager fm;
    fm.addDefaultFormats();
    juce::OwnedArray<juce::PluginDescription> found;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile (found, pluginFile.getFullPathName());
    check (found.size() == 1, "escanear VST3 (" + juce::String (found.size()) + " encontrado)");
    if (found.isEmpty()) return;
    if (auto x = found[0]->createXml()) scannedDescXml = x->toString();

    juce::String err;
    auto inst = fm.createPluginInstance (*found[0], 48000, 256, err);
    check (inst != nullptr, "instanciar VST3 " + err);
    if (inst == nullptr) return;

    Engine e; e.prepare (48000, 256);
    auto impulse = makeBuffer (2, 8000, [] (int, int i) { return i == 1000 ? 0.5f : 0.0f; });
    auto* a = addTrack (e, impulse, "a");
    addTrack (e, impulse, "b");
    check (e.prepareProcessor (*inst).isEmpty(), "preparar VST3 em estereo");

    // parametro de ganho -6 dB e restauracao de estado
    auto* gain = inst->getParameters()[0];
    gain->setValueNotifyingHost ((-6.0f + 24.0f) / 48.0f); // faixa -24..+24 dB
    juce::MemoryBlock st;
    inst->getStateInformation (st);
    auto inst2 = fm.createPluginInstance (*found[0], 48000, 256, err);
    inst2->setStateInformation (st.getData(), (int) st.getSize());
    check (near (inst2->getParameters()[0]->getValue(), gain->getValue()), "estado do VST3 restaurado");

    const auto desc = *found[0];
    e.setInsert (*a, 0, std::move (inst), &desc);
    check (e.getTotalLatency() == 64, "latencia do VST3 = 64");
    auto out = run (e, 8000);
    const float expect = 0.5f * (1.0f + juce::Decibels::decibelsToGain (-6.0f));
    check (peakIndex (out, 0) == 1064 && near (out.getSample (0, 1064), expect, 1.0e-3f), "VST3 processa com PDC e ganho");
    auto editorTest = e.clearInsert (*a, 0);
    check (editorTest != nullptr && e.getTotalLatency() == 0, "remover VST3");
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    testMixBasics();
    testStereoBalanceMuteSolo();
    testBusesAndSends();
    testLatencyCompensation();
    testLoop();
    testDeviceCallback();
    testResampler();
    testProjectRoundTrip();
    if (argc > 1) testVst3Hosting (juce::File (juce::String::fromUTF8 (argv[1])));
    else std::printf ("(sem caminho do plugin de teste: hospedagem VST3 nao testada)\n");
    if (argc > 2 && scannedDescXml.isNotEmpty()) // opcional: grava a descricao do plugin de teste
        juce::File (juce::String::fromUTF8 (argv[2])).replaceWithText (scannedDescXml);
    std::printf ("\n%s (%d falha(s))\n", failures == 0 ? "TODOS OS TESTES PASSARAM" : "HOUVE FALHAS", failures);
    return failures == 0 ? 0 : 1;
}
