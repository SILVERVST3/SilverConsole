// Testes do motor do SilverConsole (sem interface).
// Uso: SilverConsoleTests [caminho do "SC Test Delay.vst3"]
#include <JuceHeader.h>
#include "Engine.h"
#include "AudioLoader.h"
#include "Project.h"
#include "AutomationRecorder.h"
#include <cstdio>
#include <thread>

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

static void testAutomationRead()
{
    Engine e; e.prepare (48000, 256);
    auto* t = addTrack (e, makeBuffer (1, 96000, [] (int, int) { return 0.5f; }), "dc");
    const float c = 0.5f * std::cos (juce::MathConstants<float>::pi * 0.25f);
    auto* l = e.getOrCreateLane (*t, AutoTarget::volume);
    e.editAutomation ([&] { l->insert (0.0, 0.0f); l->insert (1.0, 0.0f); l->insert (1.0001, kMinDb); });
    auto out = run (e, 72000);
    check (near (out.getSample (0, 40000), c) && near (out.getSample (0, 70000), 0.0f), "automacao de volume (0 dB -> -inf em 1 s)");
    check (t->gainDb.load() <= kMinDb, "fader acompanha a automacao");

    e.editAutomation ([&] { l->points.clear(); l->insert (0.0, 0.0f); l->insert (1.0, -12.0f); });
    out = run (e, 30000);
    check (near (out.getSample (0, 24000), c * juce::Decibels::decibelsToGain (-6.0f), 0.01f), "rampa de volume interpolada (-6 dB no meio)");

    t->autoMode = (int) AutoMode::off;
    t->gainDb = 0.0f;
    out = run (e, 48000);
    check (near (out.getSample (0, 45000), c), "modo OFF ignora a automacao");

    t->autoMode = (int) AutoMode::read;
    l->writing = true;
    t->gainDb = 0.0f;
    out = run (e, 48000);
    check (near (out.getSample (0, 45000), c), "linha sendo gravada nao e aplicada");
    l->writing = false;

    e.editAutomation ([&] { l->points.clear(); });
    t->gainDb = 0.0f;
    auto* m = e.getOrCreateLane (*t, AutoTarget::mute);
    check (m->step, "mute e em degrau");
    e.editAutomation ([&] { m->insert (0.0, 0.0f); m->insert (0.5, 1.0f); });
    out = run (e, 48000);
    check (near (out.getSample (0, 12000), c) && near (out.getSample (0, 40000), 0.0f), "automacao de mute");

    // parado: so aplica quando o cursor muda de lugar
    t->mute = false;
    run (e, 256, 30000, false);
    check (t->mute.load(), "cursor movido parado aplica a automacao");
    t->mute = false;
    {
        const juce::ScopedLock sl (e.lock);
        juce::AudioBuffer<float> b (2, 256);
        e.render (b.getWritePointer (0), b.getWritePointer (1), 256, 30000, false, true);
    }
    check (! t->mute.load(), "parado no mesmo lugar nao briga com o usuario");
}

static void simulatePass (AutomationRecorder& rec, Channel& ch, double from, double to, double touchAt, double releaseAt, float newDb)
{
    double now = 0.0;
    for (double ts = from; ts <= to + 1.0e-9; ts += 0.02)
    {
        if (std::abs (ts - touchAt) < 0.005) rec.touch (ch, AutoTarget::volume, -1, -1, true, now);
        if (ts > touchAt + 0.01 && ts < releaseAt) ch.gainDb = newDb;
        if (std::abs (ts - releaseAt) < 0.005) rec.touch (ch, AutoTarget::volume, -1, -1, false, now);
        rec.process (ts, true, now);
        now += 20.0;
    }
    rec.process (to, false, now);
}

static void testAutomationRecord()
{
    Engine e; e.prepare (48000, 256);
    auto buf = makeBuffer (1, 48000 * 5, [] (int, int) { return 0.1f; });
    AutomationRecorder rec (e);

    auto* a = addTrack (e, buf, "latch");
    a->autoMode = (int) AutoMode::latch;
    simulatePass (rec, *a, 0.0, 4.0, 1.0, 2.0, -10.0f);
    auto* la = e.findLane (*a, AutoTarget::volume);
    check (la != nullptr && near (la->valueAt (0.5), 0.0f) && near (la->valueAt (1.5), -10.0f) && near (la->valueAt (3.5), -10.0f),
           "LATCH grava do toque ate parar");
    check (la != nullptr && la->points.size() < 12, "pontos afinados (" + juce::String (la != nullptr ? (int) la->points.size() : -1) + ")");
    check (la != nullptr && ! la->writing.load(), "parar encerra a gravacao");

    auto* b = addTrack (e, buf, "touch");
    b->autoMode = (int) AutoMode::touch;
    simulatePass (rec, *b, 0.0, 4.0, 1.0, 2.0, -10.0f);
    auto* lb = e.findLane (*b, AutoTarget::volume);
    check (lb != nullptr && near (lb->valueAt (1.5), -10.0f) && near (lb->valueAt (3.5), 0.0f) && near (lb->valueAt (0.5), 0.0f),
           "TOUCH volta ao valor anterior ao soltar");

    auto* c = addTrack (e, buf, "write");
    auto* lc = e.getOrCreateLane (*c, AutoTarget::volume);
    e.editAutomation ([&] { lc->insert (0.0, -20.0f); lc->insert (4.0, -20.0f); });
    c->autoMode = (int) AutoMode::write;
    c->gainDb = -3.0f;
    double now = 0.0;
    for (double ts = 1.0; ts <= 2.0 + 1.0e-9; ts += 0.02) { rec.process (ts, true, now); now += 20.0; }
    rec.process (2.0, false, now);
    check (near (lc->valueAt (0.5), -20.0f) && near (lc->valueAt (1.5), -3.0f) && near (lc->valueAt (3.0), -20.0f),
           "WRITE substitui so o trecho tocado");
    check (e.findLane (*c, AutoTarget::pan) != nullptr && e.findLane (*c, AutoTarget::mute) != nullptr, "WRITE grava volume, pan e mute");

    auto* d = addTrack (e, buf, "read");
    d->autoMode = (int) AutoMode::read;
    simulatePass (rec, *d, 0.0, 2.0, 0.5, 1.0, -10.0f);
    check (e.findLane (*d, AutoTarget::volume) == nullptr, "READ nao grava");
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
    t->autoMode = (int) AutoMode::touch;
    auto* vl = e.getOrCreateLane (*t, AutoTarget::volume);
    auto* pl = e.getOrCreateLane (*t, AutoTarget::plugin, 2, 5);
    e.editAutomation ([&] { vl->insert (0.5, -6.0f); vl->insert (1.25, -12.5f); pl->insert (2.0, 0.75f); });
    e.swapInserts (*t, 2, 3);
    check (pl->slot == 3 && t->inserts[3].isMissing(), "automacao acompanha o plugin ao mover o insert");
    e.swapInserts (*t, 2, 3);

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
        check (td.ch.autoMode == AutoMode::touch && td.ch.lanes.size() == 2, "modo e linhas de automacao salvos");
        if (td.ch.lanes.size() == 2)
        {
            const auto& v = td.ch.lanes[0];
            const auto& pp = td.ch.lanes[1];
            check (v.target == AutoTarget::volume && v.points.size() == 2 && near ((float) v.points[1].t, 1.25f) && near (v.points[1].v, -12.5f),
                   "pontos de volume");
            check (pp.target == AutoTarget::plugin && pp.slot == 2 && pp.param == 5 && near (pp.points[0].v, 0.75f), "linha de parametro de plugin");
            Engine e2; Track t2;
            applyChannelBasics (e2, t2, td.ch);
            check (t2.lanes.size() == 2 && t2.getAutoMode() == AutoMode::touch, "aplicar linhas ao abrir");
        }
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
    // automacao de parametro do plugin (ganho 0 dB -> -6 dB em 0.1 s) e eventos de gesto
    {
        Engine e3; e3.prepare (48000, 256);
        auto* dc = addTrack (e3, makeBuffer (2, 48000, [] (int, int) { return 0.5f; }), "dc");
        auto inst3 = fm.createPluginInstance (*found[0], 48000, 256, err);
        e3.prepareProcessor (*inst3);
        auto* raw = inst3.get();
        e3.setInsert (*dc, 0, std::move (inst3), &desc);
        auto* lane = e3.getOrCreateLane (*dc, AutoTarget::plugin, 0, 0);
        e3.editAutomation ([&] { lane->insert (0.0, 0.5f); lane->insert (0.1, 0.5f); lane->insert (0.1001, 0.375f); });
        auto o = run (e3, 9600);
        check (near (o.getSample (0, 2000), 0.5f, 1.0e-3f) && near (o.getSample (0, 9000), 0.5f * juce::Decibels::decibelsToGain (-6.0f), 2.0e-3f),
               "automacao de parametro VST3");
        raw->getParameters()[0]->beginChangeGesture();
        raw->getParameters()[0]->setValueNotifyingHost (0.25f);
        raw->getParameters()[0]->endChangeGesture();
        Engine::ParamEvent ev;
        int begins = 0, changes = 0, ends = 0;
        while (e3.popParamEvent (ev))
            if (ev.proc == raw) { begins += ev.type == 1; changes += ev.type == 0; ends += ev.type == 2; }
        check (begins == 1 && changes >= 1 && ends == 1, "gestos do plugin chegam ao gravador");
    }

    auto editorTest = e.clearInsert (*a, 0);
    check (editorTest != nullptr && e.getTotalLatency() == 0, "remover VST3");
}

// Regressao de escala: 128 faixas ativas, render repetido e soma finita.
// Nao mede prazo realtime; mede estabilidade e ausencia de NaN/Inf.
// Streaming em disco: escrita/abertura/seek e integracao com a engine.
static void testDiskStreaming()
{
    auto file = juce::File::createTempFile (".wav");
    {
        auto output = file.createOutputStream();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer;
        if (output != nullptr)
        {
            writer.reset (wav.createWriterFor (output.get(), 48000.0, 2, 24, {}, 0));
            if (writer != nullptr) output.release();
        }
        check (writer != nullptr, "stream: criar WAV temporario");
        if (writer == nullptr) { file.deleteFile(); return; }
        juce::AudioBuffer<float> input (2, 120000);
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            input.setSample (0, i, 0.125f * std::sin (i * 0.01f));
            input.setSample (1, i, 0.125f * std::cos (i * 0.01f));
        }
        check (writer->writeFromAudioSampleBuffer (input, 0, input.getNumSamples()),
               "stream: arquivo de teste escrito");
    }
    {
        juce::String error;
        auto stream = StreamingAudio::open (file, 48000.0, error);
        check (stream != nullptr, "stream: abrir e preparar janela inicial " + error);
        if (stream != nullptr)
        {
            check (stream->getLength() == 120000 && stream->getChannels() == 2,
                   "stream: metadados e duracao");
            float left[256] {}, right[256] {};
            stream->copy (left, right, 0, 1000, 256);
            check (near (left[10], 0.125f * std::sin (1010 * 0.01f), 0.0001f)
                   && near (right[10], 0.125f * std::cos (1010 * 0.01f), 0.0001f),
                   "stream: leitura no cache inicial");
            // Seek fora da janela inicial: a thread faz o prefetch,
            // o callback nunca espera por leitura em disco.
            stream->copy (left, right, 0, 100000, 256);
            stream->service(); // simula o worker de I/O (teste deterministico)
            std::fill (std::begin (left), std::end (left), 0.0f);
            std::fill (std::begin (right), std::end (right), 0.0f);
            stream->copy (left, right, 0, 100000, 256);
            check (near (left[10], 0.125f * std::sin (100010 * 0.01f), 0.0001f),
                   "stream: reposicionamento com nova janela");

            // Testa publicacao/aposentadoria de janelas enquanto a leitura
            // do callback ocorre em paralelo. Falhas seriam UAF ou corrupcao.
            std::atomic<bool> keepServicing { true };
            std::thread io ([&]
            {
                while (keepServicing.load (std::memory_order_acquire))
                {
                    stream->service();
                    std::this_thread::yield();
                }
            });
            bool finiteRead = true;
            for (int step = 0; step < 2000; ++step)
            {
                std::fill (std::begin (left), std::end (left), 0.0f);
                std::fill (std::begin (right), std::end (right), 0.0f);
                stream->copy (left, right, 0, step % 2 == 0 ? 1000 : 100000, 256);
                for (int i : { 0, 100, 255 })
                    finiteRead = finiteRead && std::isfinite (left[i]) && std::isfinite (right[i]);
            }
            keepServicing.store (false, std::memory_order_release);
            io.join();
            check (finiteRead, "stream: publicacao concorrente sem amostras invalidas");

            Engine e;
            e.prepare (48000.0, 256);
            auto track = std::make_unique<Track>();
            track->name = "disk test";
            auto* raw = e.addTrack (std::move (track));
            auto freshStream = StreamingAudio::open (file, 48000.0, error);
            if (freshStream != nullptr)
            {
                e.setTrackStream (*raw, freshStream);
                check (raw->playback == nullptr && raw->original == nullptr
                       && e.getLengthSamples() == 120000,
                       "stream: engine usa disco sem buffer completo na RAM");
                auto mixed = run (e, 4096);
                check (near (mixed.getSample (0, 1000), 0.125f * std::sin (10.0f), 0.0001f),
                       "stream: render da engine usa cache");

                // A exportacao percorre o audio muito mais rapido que tempo real.
                // Testa o final do arquivo, alem da primeira janela de cache.
                auto exported = juce::File::createTempFile (".wav");
                std::atomic<float> progress { 0.0f };
                std::atomic<bool> cancel { false };
                bool clipped = false;
                juce::String exportError;
                const bool exportedOk = e.exportMix (exported, 24, 0.0,
                                                        progress, cancel, exportError, clipped);
                check (exportedOk, "stream: bounce offline " + exportError);
                if (exportedOk)
                {
                    juce::AudioFormatManager fm;
                    registerAudioFormats (fm);
                    const auto outputFile = loadAudioFile (fm, exported);
                    check (outputFile.ok() && outputFile.original->getNumSamples() == 120000,
                           "stream: bounce tem duracao integral");
                    if (outputFile.ok() && outputFile.original->getNumSamples() > 100000)
                        check (near (outputFile.original->getSample (0, 100000),
                                     0.125f * std::sin (100000 * 0.01f), 0.0001f),
                               "stream: fim do bounce sem silencio causado por cache");
                }
                exported.deleteFile();
            }
            else check (false, "stream: reabrir para teste integrado");
        }
        auto wrongRate = StreamingAudio::open (file, 44100.0, error);
        check (wrongRate == nullptr, "stream: taxa diferente usa caminho de fallback");
    }
    check (file.deleteFile(), "stream: limpeza do arquivo de teste");
}

// Garante que a soma do bounce com workers multicore seja igual ao render
// sequencial (após acomodacao dos smoothing ramps).
static void testMulticoreOfflineMix()
{
    Engine e;
    e.prepare (48000.0, 256);
    auto source = makeBuffer (2, 12000, [] (int ch, int i)
    {
        return 0.012f * std::sin ((float) i * 0.012f + (float) ch * 0.3f);
    });
    for (int i = 0; i < 8; ++i)
        addTrack (e, source, "parallel " + juce::String (i));
    const auto reference = run (e, 12000);

    const auto out = juce::File::createTempFile (".wav");
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> cancel { false };
    juce::String error;
    bool clipped = false;
    const bool success = e.exportMix (out, 24, 0.0, progress, cancel, error, clipped);
    check (success, "multicore: exportacao em oito faixas " + error);
    if (success)
    {
        juce::AudioFormatManager fm;
        registerAudioFormats (fm);
        auto wav = loadAudioFile (fm, out);
        check (wav.ok() && wav.original->getNumSamples() == 12000,
               "multicore: duracao preservada");
        if (wav.ok() && wav.original->getNumSamples() == 12000)
        {
            bool same = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int i : { 2000, 4500, 9000, 11500 })
                    same = same && near (reference.getSample (ch, i),
                                        wav.original->getSample (ch, i), 0.0002f);
            check (same, "multicore: amostras coincidem com render sequencial");
        }
    }
    out.deleteFile();
}

// LUFS momentaneo: K weighting, estabilidade e diferenca de 20 dB
// para uma reducao de amplitude por fator 10. Nao testa LUFS integrado.
static void testMomentaryLoudness()
{
    constexpr int n = 48000;
    juce::AudioBuffer<float> stereo (2, n);
    auto generate = [&] (float amp, double sr)
    {
        for (int i = 0; i < n; ++i)
        {
            const auto v = amp * std::sin ((double) i * 2.0
                                * juce::MathConstants<double>::pi * 1000.0 / sr);
            stereo.setSample (0, i, (float) v);
            stereo.setSample (1, i, (float) v);
        }
    };
    MomentaryLoudness meter;
    meter.prepare (48000.0);
    generate (0.1f, 48000.0);
    for (int i = 0; i < n; i += 256)
        meter.process (stereo.getReadPointer (0, i),
                       stereo.getReadPointer (1, i), juce::jmin (256, n - i));
    const auto loud = meter.getLufsM();
    check (std::isfinite (loud) && loud > -30.0f && loud < -15.0f,
           "lufs-m: valor plausivel de senoide 1 kHz em 48k");

    meter.reset();
    generate (0.01f, 48000.0);
    meter.process (stereo.getReadPointer (0), stereo.getReadPointer (1), n);
    check (near (loud - meter.getLufsM(), 20.0f, 0.3f),
           "lufs-m: diferenca correta de 20 dB");

    meter.reset();
    stereo.clear();
    meter.process (stereo.getReadPointer (0), stereo.getReadPointer (1), n);
    check (meter.getLufsM() <= -110.0f,
           "lufs-m: silencio apos reset");

    meter.prepare (96000.0);
    generate (0.1f, 96000.0);
    meter.process (stereo.getReadPointer (0), stereo.getReadPointer (1), n);
    check (std::isfinite (meter.getLufsM()) && meter.getLufsM() > -40.0f,
           "lufs-m: filtro em taxa de amostragem alternativa");
}

// True Peak: o pico entre amostras de 12 kHz nao coincide com nenhum
// sample na fase de 45 graus. O sinc 4x deve identifica-lo sem tocar audio.
static void testTruePeakMeter()
{
    constexpr int len = 2048;
    juce::AudioBuffer<float> tone (2, len);
    float samplePeak = 0.0f;
    for (int i = 0; i < len; ++i)
    {
        const float v = std::sin ((float) i * juce::MathConstants<float>::halfPi
                                  + juce::MathConstants<float>::pi * 0.25f);
        tone.setSample (0, i, v);
        tone.setSample (1, i, v * 0.5f);
        samplePeak = juce::jmax (samplePeak, std::abs (v));
    }

    TruePeak4x full;
    auto tp = full.process (tone.getReadPointer (0), tone.getReadPointer (1), len);
    check (tp.left > samplePeak + 0.12f && tp.left > 0.95f && tp.left < 1.06f,
           "true peak: 4x detecta pico entre amostras");
    check (near (tp.right, tp.left * 0.5f, 0.001f),
           "true peak: medicao estereo independente");

    TruePeak4x chunked;
    TruePeak4x::Peaks accumulated;
    for (int p = 0; p < len; p += 64)
    {
        const auto part = chunked.process (tone.getReadPointer (0, p),
                                           tone.getReadPointer (1, p), 64);
        accumulated.left = juce::jmax (accumulated.left, part.left);
        accumulated.right = juce::jmax (accumulated.right, part.right);
    }
    check (near (tp.left, accumulated.left, 1.0e-5f)
           && near (tp.right, accumulated.right, 1.0e-5f),
           "true peak: estado FIR preservado entre callbacks");

    full.reset();
    const float silence[256] {};
    const auto silentPeak = full.process (silence, silence, 256);
    check (silentPeak.left == 0.0f && silentPeak.right == 0.0f,
           "true peak: reset nao carrega amostras anteriores");

    Engine e;
    e.prepare (48000, 256);
    auto* track = addTrack (e, makeBuffer (2, 2048, [] (int c, int i)
    {
        return (c == 0 ? 0.5f : 0.25f) * std::sin (
            (float) i * juce::MathConstants<float>::halfPi
            + juce::MathConstants<float>::pi * 0.25f);
    }), "meter source");
    (void) track;
    run (e, 2048);
    check (e.master.truePeakL.load() > e.master.truePeakR.load()
           && e.master.truePeakL.load() > 0.0f,
           "true peak: medidor recebe o sinal final do master");
}

static void test128TrackStress()
{
    Engine e;
    e.prepare (48000.0, 256);
    auto tone = makeBuffer (2, 4096, [] (int c, int i)
    {
        return 0.001f * std::sin (0.017f * (float) i + (float) c * 0.2f);
    });
    for (int i = 0; i < kMaxTracks; ++i)
        addTrack (e, tone, "stress " + juce::String (i + 1));
    check ((int) e.tracks.size() == kMaxTracks, "stress: 128 faixas criadas");
    auto output = run (e, 4096);
    // Medicao repetivel de custo de render offline; nao equivale a teste
    // realtime em hardware, mas revela regressoes severas de desempenho.
    constexpr int iterations = 32;
    const auto t0 = juce::Time::getHighResolutionTicks();
    for (int i = 0; i < iterations; ++i)
        run (e, 4096);
    const auto ticks = juce::Time::getHighResolutionTicks() - t0;
    const double elapsedMs = juce::Time::highResolutionTicksToSeconds (ticks) * 1000.0;
    const double audioMs = iterations * 4096.0 / e.getSampleRate() * 1000.0;
    const double realtimeFactor = elapsedMs > 0.0 ? audioMs / elapsedMs : 0.0;
    std::printf ("[PERF] 128 tracks: %.1f ms processing %.1f ms audio, %.2fx realtime (offline)\\n",
                 elapsedMs, audioMs, realtimeFactor);
    check (std::isfinite (realtimeFactor) && realtimeFactor > 0.0,
           "stress: medicao de desempenho valida");
    bool finite = true, nonzero = false;
    for (int c = 0; c < output.getNumChannels(); ++c)
        for (int i = 0; i < output.getNumSamples(); ++i)
        {
            const float v = output.getSample (c, i);
            finite = finite && std::isfinite (v);
            nonzero = nonzero || std::abs (v) > 1.0e-7f;
        }
    check (finite && nonzero, "stress: mix de 128 faixas finita e nao silenciosa");
    const auto stats = e.getRealtimeStats();
    check (stats.lockContentions == 0 && stats.renderSuspensions == 0 && stats.callbackOverruns == 0,
           "stress: contadores realtime inicializados");
    e.resetRealtimeStats();
    check (e.getRealtimeStats().processedCallbacks == 0 && e.getRealtimeStats().callbackOverruns == 0,
           "stress: reset de diagnostico");
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    testMixBasics();
    testTruePeakMeter();
    testMomentaryLoudness();
    test128TrackStress();
    testMulticoreOfflineMix();
    testDiskStreaming();
    testStereoBalanceMuteSolo();
    testBusesAndSends();
    testLatencyCompensation();
    testLoop();
    testAutomationRead();
    testAutomationRecord();
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
