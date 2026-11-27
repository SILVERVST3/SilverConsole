// Testes do motor do SilverConsole (sem interface).
// Uso: SilverConsoleTests [caminho do "SC Test Delay.vst3"]
#include <JuceHeader.h>
#include "Engine.h"
#include "AudioLoader.h"
#include "Project.h"
#include "AutomationRecorder.h"
#include "PluginScanPolicy.h"
#include "PluginScanSupervisor.h"
#include "DspIpcWatchdog.h"
#include "RemoteDspInsert.h"
#include <cstdio>
#include <thread>
#include <stdexcept>
#include <limits>

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

// Simula uma excecao de um insert de terceiros. Um crash de processo
// real nao pode ser tratado assim e exige isolamento IPC (pendente).
class ThrowingProcessor : public InternalDelay
{
public:
    ThrowingProcessor() : InternalDelay (1) {}
    void processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override
    {
        // Um plugin pode deixar um bloco parcialmente corrompido ao falhar.
        audio.setSample (0, 0, std::numeric_limits<float>::quiet_NaN());
        throw std::runtime_error ("Falha de teste em plugin VST3");
    }
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

static void testPluginExceptionFallback()
{
    Engine e;
    e.prepare (48000.0, 256);
    auto* t = addTrack (e, makeBuffer (2, 4096, [] (int, int)
    {
        return 0.1f;
    }), "faulty insert");

    auto faulty = std::make_unique<ThrowingProcessor>();
    const auto error = e.prepareProcessor (*faulty);
    check (error.isEmpty(), "plugin exception: processador de teste preparado");
    if (error.isNotEmpty()) return;
    e.setInsert (*t, 0, std::move (faulty), nullptr);

    auto mixed = run (e, 512);
    bool valid = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < mixed.getNumSamples(); ++sample)
            valid = valid && std::isfinite (mixed.getSample (channel, sample));
    check (valid, "plugin exception: NaN parcial nao contamina o mixer");
    check (t->inserts[0].bypassed.load() && t->inserts[0].faulted.load()
           && e.getRealtimeStats().pluginExceptions == 1,
           "plugin exception: insert marcado ERR, em bypass e falha contada");
    e.pollLatencyChanges();
    check (e.getTotalLatency() == 0,
           "plugin exception: PDC atualizado apos bypass automatico");

    // O callback posterior deve preservar o audio original com insert em bypass.
    mixed = run (e, 1024);
    check (std::abs (mixed.getSample (0, 800)) > 0.01f,
           "plugin exception: playback recuperado por bypass automatico");

    e.resetRealtimeStats();
    check (e.getRealtimeStats().pluginExceptions == 0,
           "plugin exception: contador pode ser reiniciado");

    e.setBypass (*t, 0, false); // usuario tenta reativar
    check (! t->inserts[0].faulted.load() && ! t->inserts[0].bypassed.load(),
           "plugin exception: retry explicito limpa ERR anterior");
    run (e, 512);
    check (t->inserts[0].faulted.load() && t->inserts[0].bypassed.load()
           && e.getRealtimeStats().pluginExceptions == 1,
           "plugin exception: nova falha volta ao bypass automaticamente");
}

// IPC: sequencias, expiracao de buffers, bypass e inicializacao explicita.
static void testDspIpcWatchdog()
{
    DspIpcWatchdog watch (2);
    check (watch.snapshot().state == DspIpcWatchdog::State::starting,
           "ipc watchdog: nunca opera antes do handshake");
    watch.start (256, 64);
    check (watch.snapshot().latencySamples == 320,
           "ipc watchdog: transporte + PDC do plugin contabilizados");
    check (watch.onSubmit() == 1, "ipc watchdog: primeira sequencia");
    watch.onCompletion (999); // resposta futura, potencialmente corrompida
    check (watch.snapshot().completed == 0,
           "ipc watchdog: descarta sequencia maior que enviada");
    watch.onCompletion (1);
    check (watch.snapshot().completed == 1, "ipc watchdog: aceita resposta valida");
    watch.onSubmit(); // seq 2
    watch.onSubmit(); // seq 3
    check (watch.isHealthy(), "ipc watchdog: tolera buffers em andamento");
    watch.onSubmit(); // seq 4: 3 > limite de 2
    check (! watch.isHealthy() &&
           watch.snapshot().state == DspIpcWatchdog::State::bypassed,
           "ipc watchdog: troca para dry quando prazo de buffer expira");
    check (watch.onSubmit() == 0, "ipc watchdog: nao envia novos buffers apos falha");
    watch.stop();
    check (watch.snapshot().state == DspIpcWatchdog::State::stopped,
           "ipc watchdog: parada explicita");
    watch.start (128, 0);
    check (watch.snapshot().state == DspIpcWatchdog::State::running &&
           watch.snapshot().submitted == 0,
           "ipc watchdog: reinicio descarta sequencias anteriores");
}

#if JUCE_WINDOWS
// Testa o caminho REAL de memoria compartilhada com VST3 em processo FILHO.
static void testRemoteDspWorker (const juce::File& worker, const juce::File& pluginFile)
{
    check (worker.existsAsFile(), "ipc worker: executavel separado empacotado");
    if (! worker.existsAsFile()) return;
    juce::String error;
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descs;
    format.findAllTypesForFile (descs, pluginFile.getFullPathName());
    check (! descs.isEmpty(), "ipc worker: plugin de teste VST3 encontrado");
    if (descs.isEmpty()) return;

    auto missing = RemoteDspInsert::start (*descs[0], {}, worker.getSiblingFile ("no-worker.exe"),
                                            48000, 256, error);
    check (missing == nullptr && error.isNotEmpty(),
           "ipc worker: recusa helper inexistente sem abrir plugin no host");

    auto remote = RemoteDspInsert::start (*descs[0], {}, worker, 48000, 256, error);
    check (remote != nullptr, "ipc worker: handshake e instanciacao VST3 " + error);
    if (remote == nullptr) return;
    check (remote->getPluginLatencySamples() == 64 &&
           remote->getLatencySamples() == 320,
           "ipc worker: latencia fixa de 1 buffer + 64 amostras do plugin");

    Engine e;
    e.prepare (48000.0, 256);
    // O impulse ocorre DEPOIS da rampa de 20ms do fader/master; no
    // primeiro sample o mixer intencionalmente faz fade-in e atenua o teste.
    auto impulse = makeBuffer (2, 3072, [] (int, int i) { return i == 1024 ? 0.5f : 0.0f; });
    auto* track = addTrack (e, impulse, "ipc test");
    auto old = e.setRemoteInsert (*track, 0, remote, *descs[0]);
    check (old == nullptr && track->inserts[0].processor == nullptr
           && track->inserts[0].remote != nullptr,
           "ipc worker: DSP nao instancia AudioProcessor no processo principal");
    check (e.getTotalLatency() == 320,
           "ipc worker: PDC inclui latencia remota declarada");

    juce::AudioBuffer<float> output (2, 3072);
    output.clear();
    juce::int64 pos = 0;
    for (int b = 0; b < 12; ++b)
    {
        pos = e.render (output.getWritePointer (0, b * 256),
                        output.getWritePointer (1, b * 256), 256, pos, true, false);
        // O teste NAO e realtime: cede CPU ao worker para exercitar o caminho
        // assíncrono sem depender de scheduling ultra-baixa latencia no CI.
        juce::Thread::sleep (12);
    }
    check (remote->snapshot().completed > 0 && ! remote->isFaulted(),
           "ipc worker: worker devolveu blocos sem travar callback");
    check (output.getSample (0, 1344) > 0.3f &&
           output.getSample (1, 1344) > 0.3f,
           "ipc worker: impulso reproduzido com atraso de 1 buffer + PDC do plugin");

    const auto latencyBefore = e.getTotalLatency();
    remote->simulateWorkerCrashForTests(); // controle/teste, NUNCA callback
    for (int b = 0; b < 3; ++b)
    {
        const auto dry = e.render (output.getWritePointer (0),
                                   output.getWritePointer (1), 256, pos, true, false);
        pos = dry;
    }
    bool finite = true;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 256; ++i)
            finite = finite && std::isfinite (output.getSample (c, i));
    check (remote->isFaulted() && finite,
           "ipc worker: crash aciona dry seguro sem NaNs nem espera");
    check (e.getTotalLatency() == latencyBefore,
           "ipc worker: latencia alinhada e PDC preservada durante falha");
    check (remote->snapshot().state == DspIpcWatchdog::State::bypassed,
           "ipc worker: falha latched ate reiniciar processo na thread de controle");

    // ---- Estado com ganho de -6 dB: distingue o sinal processado (wet) do seco ----
    juce::MemoryBlock minus6;
    {
        juce::VST3PluginFormat fmt;
        auto local = fmt.createInstanceFromDescription (*descs[0], 48000, 256, error);
        if (local != nullptr && ! local->getParameters().isEmpty())
        {
            local->getParameters()[0]->setValue (0.375f); // -24..+24 dB -> -6 dB
            local->getStateInformation (minus6);
        }
    }
    check (minus6.getSize() > 0, "ipc worker: estado -6 dB do plugin de teste");
    const float wetGain = juce::Decibels::decibelsToGain (-6.0f);

    // (a) Ritmo de tempo real: um bloco de 256 a cada 5,33 ms, sem folga extra.
    //     Com polling por Sleep(1) o worker perdia prazos e o watchdog travava o bypass.
    {
        auto rt = RemoteDspInsert::start (*descs[0], minus6, worker, 48000, 256, error);
        check (rt != nullptr, "ipc tempo real: worker iniciado " + error);
        if (rt != nullptr)
        {
            juce::HeapBlock<float> l (256), r (256);
            const int blocks = 400; // ~2,1 s
            const double period = 256.0 * 1000.0 / 48000.0;
            int wetBlocks = 0;
            auto next = juce::Time::getMillisecondCounterHiRes();
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < 256; ++i) l[i] = r[i] = 0.5f;
                rt->process (l, r, 256, false);
                if (std::abs (l[200] - 0.5f * wetGain) < 1.0e-3f) ++wetBlocks;
                next += period;
                while (juce::Time::getMillisecondCounterHiRes() < next) {} // espera ativa precisa
            }
            const auto snap = rt->snapshot();
            std::printf ("       (tempo real: %d/%d blocos processados, %llu respostas, %llu perdidas)\n",
                         wetBlocks, blocks, (unsigned long long) snap.completed, (unsigned long long) rt->getMisses());
            check (! rt->isFaulted() && wetBlocks >= blocks * 9 / 10,
                   "ipc tempo real: worker cumpre o prazo de 1 buffer (>= 90% dos blocos processados)");
        }
    }

    // (b) Exportacao offline: o bounce e mais rapido que o tempo real e precisa
    //     esperar o worker; antes, saia o sinal seco (0,5) em vez do processado.
    {
        auto ex = RemoteDspInsert::start (*descs[0], minus6, worker, 48000, 256, error);
        check (ex != nullptr, "ipc export: worker iniciado " + error);
        if (ex != nullptr)
        {
            Engine e2;
            e2.prepare (48000.0, 256);
            auto* t2 = addTrack (e2, makeBuffer (2, 48000, [] (int, int i) { return i == 24000 ? 0.5f : 0.0f; }), "ipc export");
            e2.setRemoteInsert (*t2, 0, ex, *descs[0]);
            auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_ipc_export.wav");
            std::atomic<float> progress { 0 }; std::atomic<bool> cancel { false };
            juce::String err; bool clipped = false;
            const bool ok = e2.exportMix (file, 32, 0.0, progress, cancel, err, clipped);
            check (ok, "ipc export: exportacao com insert isolado " + err);
            juce::AudioFormatManager fm; registerAudioFormats (fm);
            auto loaded = loadAudioFile (fm, file);
            if (loaded.ok())
            {
                check (loaded.original->getNumSamples() == 48000, "ipc export: tamanho certo (PDC descontado)");
                check (peakIndex (*loaded.original, 0) == 24000
                       && near (loaded.original->getSample (0, 24000), 0.5f * wetGain, 1.0e-3f),
                       "ipc export: impulso processado pelo worker (-6 dB), alinhado, sem trecho seco");
            }
            else check (false, "ipc export: WAV legivel");
            check (! ex->isFaulted() && ! ex->isOffline(), "ipc export: worker saudavel e modo tempo real restaurado");

            // worker morto: exportacao recusada com mensagem, em vez de gravar mix seca
            ex->simulateWorkerCrashForTests();
            err.clear();
            check (! e2.exportMix (file, 32, 0.0, progress, cancel, err, clipped) && err.isNotEmpty(),
                   "ipc export: insert em falha bloqueia a exportacao com aviso");
            file.deleteFile();
        }
    }
}
#endif

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

    // exportacao de um trecho (regiao do loop): impulso em 1000 -> trecho 900..1500 = 600 amostras, impulso em 100
    progress = 0; err.clear(); clipped = false;
    check (e.exportMix (file, 32, 0.0, progress, cancel, err, clipped, 900, 1500), "exportar trecho " + err);
    loaded = loadAudioFile (fm, file);
    check (loaded.ok() && loaded.original->getNumSamples() == 600, "trecho exportado com o tamanho certo");
    if (loaded.ok())
        check (peakIndex (*loaded.original, 0) == 100 && near (loaded.original->getSample (0, 100), 2.0f),
               "trecho exportado alinhado (PDC descontado)");
    check (! e.exportMix (file, 32, 0.0, progress, cancel, err, clipped, 500, 500), "trecho vazio e recusado");
    file.deleteFile();
}

static void testExportDither()
{
    // 16 bits: silencio vira ruido de dither de no maximo 2 LSB; sinal fica preservado
    Engine e; e.prepare (48000, 256);
    addTrack (e, makeBuffer (1, 4800, [] (int, int i) { return i < 2400 ? 0.0f : 0.25f; }), "half");
    e.tracks[0]->pan = -1.0f; // mono -> so esquerda (potencia constante: L = 1)
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_export_dither.wav");
    std::atomic<float> progress { 0 }; std::atomic<bool> cancel { false };
    juce::String err; bool clipped = false;
    check (e.exportMix (file, 16, 0.0, progress, cancel, err, clipped), "exportar WAV 16 bits " + err);
    juce::AudioFormatManager fm; registerAudioFormats (fm);
    auto loaded = loadAudioFile (fm, file);
    check (loaded.ok() && loaded.original->getNumSamples() == 4800, "WAV 16 bits com o tamanho certo");
    if (loaded.ok())
    {
        const float lsb = 1.0f / 32768.0f;
        const float silence = loaded.original->getMagnitude (0, 0, 2400);
        const float noiseR = loaded.original->getMagnitude (1, 0, 4800);
        float maxErr = 0.0f;
        for (int i = 2400; i < 4800; ++i) maxErr = juce::jmax (maxErr, std::abs (loaded.original->getSample (0, i) - 0.25f));
        check (silence > 0.0f && silence <= 2.5f * lsb && noiseR <= 2.5f * lsb, "dither TPDF de no maximo 2 LSB");
        check (maxErr <= 2.5f * lsb, "sinal preservado com dither");
    }
    check (! clipped, "sem aviso de clipping");
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
        // Taxa diferente: o streaming converte trecho a trecho na thread de disco
        // (antes o arquivo inteiro ia para a RAM duas vezes). Deve ser identico
        // ao reamostrador offline aplicado ao arquivo inteiro.
        auto converted = StreamingAudio::open (file, 44100.0, error);
        check (converted != nullptr && converted->getLength() == resampledLength (120000, 48000.0, 44100.0),
               "stream: taxa diferente convertida no proprio streaming " + error);
        if (converted != nullptr)
        {
            juce::AudioFormatManager fm;
            registerAudioFormats (fm);
            const auto full = loadAudioFile (fm, file);
            const auto reference = full.ok() ? resampleBuffer (full.original, full.rate, 44100.0) : nullptr;
            float maxErr = reference != nullptr ? 0.0f : 1.0f;
            for (juce::int64 at : { (juce::int64) 0, (juce::int64) 5000, (juce::int64) 70000, converted->getLength() - 300 })
            {
                if (reference == nullptr) break;
                converted->prefetchBlocking (at, 256);
                float l[256] {}, r[256] {};
                converted->copy (l, r, 0, at, 256);
                const int cnt = (int) juce::jmin ((juce::int64) 256, converted->getLength() - at);
                for (int i = 0; i < cnt; ++i)
                    maxErr = juce::jmax (maxErr, std::abs (l[i] - reference->getSample (0, (int) at + i)),
                                         std::abs (r[i] - reference->getSample (1, (int) at + i)));
            }
            check (maxErr < 1.0e-6f, "stream: conversao por trechos identica a offline (erro max "
                                         + juce::String (maxErr, 9) + ")");
        }
    }
    check (file.deleteFile(), "stream: limpeza do arquivo de teste");
}

// Garante que a soma do bounce com workers multicore seja igual ao render
// sequencial (após acomodacao dos smoothing ramps).
// Multicore no callback em tempo real: resultado identico ao de um nucleo.
static void testRealtimeMulticore()
{
    auto build = [] (Engine& e)
    {
        e.prepare (48000.0, 256);
        for (int i = 0; i < 12; ++i)
        {
            auto src = makeBuffer (2, 24000, [i] (int ch, int s)
            { return 0.02f * std::sin ((float) s * (0.003f + 0.001f * (float) i) + (float) ch); });
            auto* t = addTrack (e, src, "rt " + juce::String (i));
            auto p = std::make_unique<InternalDelay> (10 + i * 7); // latencias diferentes: PDC + inserts
            e.prepareProcessor (*p);
            e.setInsert (*t, i % 4, std::move (p), nullptr);
            t->pan = (float) (i % 5) * 0.4f - 0.8f;
        }
    };
    Engine serial, parallel;
    build (serial);
    build (parallel);
    serial.setMultiCore (false);
    parallel.setMultiCore (true);
    const auto a = run (serial, 20000);
    const auto b = run (parallel, 20000);
    float maxDiff = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 20000; ++i)
            maxDiff = juce::jmax (maxDiff, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
    std::printf ("       (workers de tempo real: %d)\n", parallel.getRealtimeWorkerCount());
    check (maxDiff == 0.0f, "multicore tempo real: identico ao processamento em um nucleo");

    // Medicao (informativa): 24 faixas com um "plugin" pesado de CPU, buffer 512.
    struct Burn : InternalDelay
    {
        Burn() : InternalDelay (0) {}
        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
        {
            for (int c = 0; c < 2; ++c)
            {
                auto* d = b.getWritePointer (c);
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    float x = d[i];
                    for (int k = 0; k < 40; ++k) x = 0.999f * std::sin (x + 0.001f * (float) k);
                    d[i] = x;
                }
            }
        }
    };
    auto timeIt = [] (bool multi)
    {
        Engine e;
        e.prepare (48000.0, 512);
        e.setMultiCore (multi);
        for (int i = 0; i < 24; ++i)
        {
            auto* t = addTrack (e, makeBuffer (2, 48000, [] (int, int s) { return 0.1f * std::sin ((float) s * 0.01f); }), "burn");
            auto p = std::make_unique<Burn>();
            e.prepareProcessor (*p);
            e.setInsert (*t, 0, std::move (p), nullptr);
        }
        run (e, 2048); // aquecimento
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        run (e, 512 * 40);
        return (juce::Time::getMillisecondCounterHiRes() - t0) / 40.0; // ms por bloco de 512
    };
    const double one = timeIt (false), many = timeIt (true);
    std::printf ("       (24 faixas pesadas, buffer 512 = %.1f ms: 1 nucleo %.2f ms/bloco, multicore %.2f ms/bloco, %.1fx)\n",
                 512.0 * 1000.0 / 48000.0, one, many, one / juce::jmax (0.001, many));
}

// Buses com inserts processados em paralelo: identico ao processamento serial.
static void testParallelBuses()
{
    auto build = [] (Engine& e)
    {
        e.prepare (48000.0, 256);
        for (int b = 0; b < 6; ++b)
        {
            auto* bus = e.addBus ("B" + juce::String (b));
            auto p = std::make_unique<InternalDelay> (5 + b * 9);
            e.prepareProcessor (*p);
            e.setInsert (*bus, 0, std::move (p), nullptr);
            bus->gainDb = -1.0f * (float) b;
            bus->pan = 0.2f * (float) b - 0.5f;
        }
        for (int i = 0; i < 10; ++i)
        {
            auto* t = addTrack (e, makeBuffer (2, 24000, [i] (int ch, int s)
                                { return 0.03f * std::sin ((float) s * (0.002f + 0.0007f * (float) i) + (float) ch); }), "t" + juce::String (i));
            t->outputBus = i % 7 - 1; // -1 = master
            t->sends[0].bus = (i + 2) % 6;
            t->sends[0].levelDb = -6.0f;
            t->sends[1].bus = (i + 4) % 6;
            t->sends[1].preFader = true;
        }
    };
    Engine serial, parallel;
    build (serial); build (parallel);
    serial.setMultiCore (false);
    parallel.setMultiCore (true);
    const auto a = run (serial, 16000), b = run (parallel, 16000);
    float maxDiff = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 16000; ++i) maxDiff = juce::jmax (maxDiff, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
    check (maxDiff == 0.0f && a.getMagnitude (0, 16000) > 0.01f, "buses em paralelo: identico ao serial");
}

// "Tocar selecao" sem loop: para no fim do trecho (silencio depois) e volta ao inicio.
static void testSelectionPlayback()
{
    Engine e; e.prepare (48000, 256);
    addTrack (e, makeBuffer (1, 48000, [] (int, int) { return 0.5f; }), "dc");
    e.setLoop (1000, 6500);
    check (e.hasSelection(), "selecao definida");
    e.loopOn = false;
    e.stopReturn = 1000;
    e.stopAt = 6500;
    e.position = 1000;
    e.playing = true;
    juce::AudioBuffer<float> out (2, 1000);
    juce::AudioIODeviceCallbackContext ctx;
    for (int k = 0; k < 5; ++k) e.audioDeviceIOCallbackWithContext (nullptr, 0, out.getArrayOfWritePointers(), 2, 1000, ctx);
    check (e.playing.load() && e.position.load() == 6000, "toca o trecho (" + juce::String (e.position.load()) + ")");
    e.audioDeviceIOCallbackWithContext (nullptr, 0, out.getArrayOfWritePointers(), 2, 1000, ctx);
    check (out.getSample (0, 499) > 0.1f && out.getSample (0, 500) == 0.0f && out.getSample (0, 999) == 0.0f,
           "silencio exatamente apos o fim da selecao");
    check (! e.playing.load() && e.position.load() == 1000 && e.stopAt.load() == -1, "para sozinho e volta ao inicio do trecho");

    // com loop ligado, o fim nao para o transporte
    e.loopOn = true; e.stopAt = 6500; e.position = 6000; e.playing = true;
    e.audioDeviceIOCallbackWithContext (nullptr, 0, out.getArrayOfWritePointers(), 2, 1000, ctx);
    check (e.playing.load() && e.position.load() == 1500, "com loop a selecao repete (" + juce::String (e.position.load()) + ")");
    e.clearSelection();
    check (! e.hasSelection() && ! e.loopOn.load(), "limpar selecao");
}

// Stems: uma passada, alinhados, solo ignorado, mute respeitado, buses opcionais.
static void testStemExport()
{
    Engine e; e.prepare (48000, 256);
    auto* a = addTrack (e, makeBuffer (2, 48000, [] (int ch, int i) { return 0.2f * std::sin ((float) i * 0.01f + (float) ch); }), "Bateria");
    auto* b = addTrack (e, makeBuffer (1, 48000, [] (int, int i) { return 0.3f * std::sin ((float) i * 0.0037f); }), "Baixo");
    auto* c = addTrack (e, makeBuffer (1, 48000, [] (int, int) { return 0.4f; }), "Mudo");
    auto* bus = e.addBus ("Grupo");
    {
        auto p = std::make_unique<InternalDelay> (37); // latencia na faixa: PDC
        e.prepareProcessor (*p);
        e.setInsert (*a, 0, std::move (p), nullptr);
        auto q = std::make_unique<InternalDelay> (11); // latencia no bus
        e.prepareProcessor (*q);
        e.setInsert (*bus, 0, std::move (q), nullptr);
    }
    a->gainDb = -3.0f; a->pan = -0.4f;
    b->pan = 0.3f; b->outputBus = 0;
    bus->gainDb = -2.0f;
    c->mute = true;
    a->solo = true; // solo nao pode silenciar os outros stems

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_stems_test");
    dir.deleteRecursively();
    std::atomic<float> progress { 0 }; std::atomic<bool> cancel { false };
    juce::String err;
    juce::StringArray written, clipped;
    Engine::StemExport req;
    req.folder = dir; req.prefix = "Teste"; req.bitDepth = 32;
    req.rangeStart = 4800; req.rangeEnd = 4800 + 9600;
    req.tracks = { 0, 1, 2 }; req.buses = { 0 };
    check (e.exportStems (req, progress, cancel, err, written, clipped), "exportar stems processados " + err);
    check (written.size() == 4 && dir.getChildFile ("Teste - 01 - Bateria.wav").existsAsFile()
           && dir.getChildFile ("Teste - 04 - BUS Grupo.wav").existsAsFile(), "4 arquivos com nomes numerados");
    check (a->solo.load() && ! e.rendering.load(), "estado do mixer preservado");

    juce::AudioFormatManager fm; registerAudioFormats (fm);
    auto load = [&fm] (const juce::File& f) { return loadAudioFile (fm, f); };
    auto stemA = load (dir.getChildFile ("Teste - 01 - Bateria.wav"));
    auto stemB = load (dir.getChildFile ("Teste - 02 - Baixo.wav"));
    auto stemC = load (dir.getChildFile ("Teste - 03 - Mudo.wav"));
    auto stemBus = load (dir.getChildFile ("Teste - 04 - BUS Grupo.wav"));
    check (stemA.ok() && stemB.ok() && stemC.ok() && stemBus.ok() && stemA.original->getNumSamples() == 9600
           && stemBus.original->getNumSamples() == 9600, "stems com o tamanho do trecho");

    // referencia: a mix exportada com so a faixa em solo (master sem processamento)
    auto mixOf = [&] (Track* solo, const juce::String& name)
    {
        for (auto& t : e.tracks) t->solo = t.get() == solo;
        auto f = dir.getChildFile (name);
        bool clip = false;
        e.exportMix (f, 32, 0.0, progress, cancel, err, clip, req.rangeStart, req.rangeEnd);
        return load (f);
    };
    auto mixA = mixOf (a, "mixA.wav");
    auto mixB = mixOf (b, "mixB.wav");
    float dA = 0.0f, dBus = 0.0f, dB = 0.0f;
    const float busGain = juce::Decibels::decibelsToGain (-2.0f);
    if (stemA.ok() && mixA.ok() && stemB.ok() && mixB.ok() && stemBus.ok())
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 9600; ++i)
            {
                dA = juce::jmax (dA, std::abs (stemA.original->getSample (ch, i) - mixA.original->getSample (ch, i)));
                dBus = juce::jmax (dBus, std::abs (stemBus.original->getSample (ch, i) - mixB.original->getSample (ch, i)));
                dB = juce::jmax (dB, std::abs (stemB.original->getSample (ch, i) * busGain - mixB.original->getSample (ch, i)));
            }
    check (dA < 1.0e-6f && stemA.original->getMagnitude (0, 9600) > 0.05f, "stem da faixa = faixa na mix (PDC alinhado) dif " + juce::String (dA, 8) + " pico " + juce::String (stemA.ok() ? stemA.original->getMagnitude (0, 9600) : -1.0f) + " mix " + juce::String (mixA.ok() ? mixA.original->getMagnitude (0, 9600) : -1.0f));
    check (dBus < 1.0e-6f, "stem do bus = bus na mix dif " + juce::String (dBus, 8));
    check (dB < 1.0e-5f, "faixa roteada ao bus sai antes do bus (pos-fader)");
    check (stemC.ok() && stemC.original->getMagnitude (0, 9600) == 0.0f, "faixa em mute sai em silencio");

    // audio original: mono continua mono, identico ao arquivo no trecho
    Engine::StemExport raw = req;
    raw.processed = false; raw.prefix = "Orig"; raw.tracks = { 1 }; raw.buses.clear();
    check (e.exportStems (raw, progress, cancel, err, written, clipped), "exportar audio original " + err);
    auto rawB = load (dir.getChildFile ("Orig - 01 - Baixo.wav"));
    float dRaw = 1.0f;
    if (rawB.ok() && rawB.original->getNumChannels() == 1 && rawB.original->getNumSamples() == 9600)
    {
        dRaw = 0.0f;
        for (int i = 0; i < 9600; ++i)
            dRaw = juce::jmax (dRaw, std::abs (rawB.original->getSample (0, i) - b->playback->getSample (0, 4800 + i)));
    }
    check (dRaw == 0.0f, "audio original mono, identico ao trecho do arquivo");

    // trecho vazio e cancelamento
    Engine::StemExport empty = req; empty.rangeStart = 100; empty.rangeEnd = 100;
    check (! e.exportStems (empty, progress, cancel, err, written, clipped), "trecho vazio recusado");
    cancel = true;
    Engine::StemExport cancelled = req; cancelled.folder = dir.getChildFile ("cancel");
    check (! e.exportStems (cancelled, progress, cancel, err, written, clipped) && ! e.rendering.load(), "cancelar encerra o render");
    dir.deleteRecursively();
}

// Plugin de teste: atraso + contador de chamadas + cauda/estado configuraveis.
class CountingDelay : public InternalDelay
{
public:
    explicit CountingDelay (int d, double tailSeconds = 0.0) : InternalDelay (d), tail (tailSeconds) {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override { ++calls; InternalDelay::processBlock (b, m); }
    double getTailLengthSeconds() const override { return tail; }
    void getStateInformation (juce::MemoryBlock& d) override { d.replaceAll (stateText.toRawUTF8(), stateText.getNumBytesAsUTF8()); }
    void setStateInformation (const void* d, int n) override { stateText = juce::String::fromUTF8 ((const char*) d, n); }
    int calls = 0;
    double tail;
    juce::String stateText { "ajuste-original" };
};

// Economia de CPU: canal em silencio dorme (sem chamar plugins), acorda no
// primeiro bloco com som e o resultado e identico ao processamento continuo.
static void testCpuSaverSleep()
{
    const int sr = 48000, len = sr * 7;
    auto signal = makeBuffer (2, len, [sr] (int ch, int i)
    {
        const bool on = i < 4800 || (i >= 6 * sr && i < 6 * sr + 4800);
        return on ? 0.3f * std::sin ((float) i * 0.013f + (float) ch) : 0.0f;
    });
    CountingDelay* plugins[2] = {};
    Engine engines[2];
    Track* tracks[2] = {};
    for (int k = 0; k < 2; ++k)
    {
        auto& e = engines[k];
        e.prepare (sr, 256);
        e.setCpuSaver (k == 0);
        tracks[k] = addTrack (e, signal, "t");
        auto p = std::make_unique<CountingDelay> (100);
        plugins[k] = p.get();
        e.prepareProcessor (*p);
        e.setInsert (*tracks[k], 0, std::move (p), nullptr);
    }
    const auto a1 = run (engines[0], 5 * sr), b1 = run (engines[1], 5 * sr);
    check (tracks[0]->asleep.load() && ! tracks[1]->asleep.load(), "canal em silencio dorme (so com a economia ligada)");
    check (engines[0].getSleepingCount() == 1, "contagem de canais dormindo");
    check (plugins[0]->calls < plugins[1]->calls * 2 / 3, "dormindo nao chama o plugin ("
           + juce::String (plugins[0]->calls) + " x " + juce::String (plugins[1]->calls) + ")");
    const auto a2 = run (engines[0], 2 * sr, 5 * sr), b2 = run (engines[1], 2 * sr, 5 * sr);
    check (! tracks[0]->asleep.load(), "acorda quando o som volta");
    float diff = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
    {
        for (int i = 0; i < 5 * sr; ++i) diff = juce::jmax (diff, std::abs (a1.getSample (ch, i) - b1.getSample (ch, i)));
        for (int i = 0; i < 2 * sr; ++i) diff = juce::jmax (diff, std::abs (a2.getSample (ch, i) - b2.getSample (ch, i)));
    }
    check (diff == 0.0f && a2.getMagnitude (0, 0, 2 * sr) > 0.1f, "resultado identico ao processamento continuo");

    // mudar o insert acorda o canal; transporte parado tambem dorme
    run (engines[0], 4 * sr, 0, false);
    check (tracks[0]->asleep.load(), "parado: plugins dormem");
    engines[0].setBypass (*tracks[0], 0, true);
    engines[0].setBypass (*tracks[0], 0, false);
    check (! tracks[0]->asleep.load(), "mexer no insert acorda o canal");

    // cauda longa declarada pelo plugin: espera a cauda; cauda infinita: nunca dorme
    {
        Engine e; e.prepare (sr, 256);
        auto* t = addTrack (e, makeBuffer (2, 4800, [] (int, int i) { return i < 2400 ? 0.2f : 0.0f; }), "rev");
        auto p = std::make_unique<CountingDelay> (0, 6.0);
        e.prepareProcessor (*p);
        e.setInsert (*t, 0, std::move (p), nullptr);
        run (e, 5 * sr);
        check (! t->asleep.load(), "respeita a cauda declarada (6 s)");
        run (e, 3 * sr, 5 * sr);
        check (t->asleep.load(), "dorme depois da cauda");
        auto q = std::make_unique<CountingDelay> (0, std::numeric_limits<double>::infinity());
        e.prepareProcessor (*q);
        e.setInsert (*t, 0, std::move (q), nullptr);
        run (e, 10 * sr, 0, false);
        check (! t->asleep.load(), "cauda infinita: espera mais que 10 s de silencio");
        run (e, 2 * sr, 0, false);
        check (t->asleep.load(), "cauda infinita: dorme depois de 10 s de saida em silencio");
    }

    // a exportacao processa tudo (nunca dorme)
    {
        auto& e = engines[0];
        const int before = plugins[0]->calls;
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_sleep_export.wav");
        std::atomic<float> progress { 0 }; std::atomic<bool> cancel { false };
        juce::String err; bool clip = false;
        check (e.exportMix (f, 24, 0.0, progress, cancel, err, clip), "exportar com economia ligada " + err);
        const int blocks = (len + 100 + 255) / 256;
        check (plugins[0]->calls - before >= blocks, "exportacao chama o plugin em todos os blocos");
        f.deleteFile();
    }
}

// Plugin offline: estado guardado, latencia fora da PDC, volta online igual.
static void testPluginOffline()
{
    Engine e; e.prepare (48000, 256);
    auto* t = addTrack (e, makeBuffer (2, 4800, [] (int, int) { return 0.1f; }), "Voz");
    auto p = std::make_unique<CountingDelay> (64);
    p->stateText = "ajuste-do-usuario";
    e.prepareProcessor (*p);
    juce::PluginDescription desc; desc.name = "Plug"; desc.pluginFormatName = "VST3"; desc.fileOrIdentifier = "C:/p.vst3";
    e.setInsert (*t, 1, std::move (p), &desc);
    e.setBypass (*t, 1, true);
    e.setBypass (*t, 1, false);
    check (e.getTotalLatency() == 64, "latencia do plugin na PDC");
    e.setBypass (*t, 1, true);
    auto old = e.takeInsertOffline (*t, 1);
    check (old != nullptr && t->inserts[1].isOffline() && ! t->inserts[1].isMissing() && t->inserts[1].processor == nullptr,
           "plugin descarregado (offline, nao ausente)");
    check (t->inserts[1].missingState.toString() == "ajuste-do-usuario", "estado guardado ao ficar offline");
    check (e.getTotalLatency() == 0 && t->inserts[1].bypassed.load(), "latencia sai da PDC; bypass mantido");
    old.reset();
    run (e, 2048); // toca sem o plugin

    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_offline.sconsole");
    juce::String err;
    ProjectData pd;
    check (writeProject (captureProject (e), file, err) && readProject (file, pd, err), "salvar/abrir com plugin offline " + err);
    check (pd.tracks.size() == 1 && pd.tracks[0].ch.inserts.size() == 1 && pd.tracks[0].ch.inserts[0].offline
           && pd.tracks[0].ch.inserts[0].state.toString() == "ajuste-do-usuario", "offline salvo no projeto com o estado");
    file.deleteFile();

    std::vector<Engine::RestoredInsert> back (1);
    back[0].slot = 1;
    auto np = std::make_unique<CountingDelay> (64);
    np->setStateInformation (t->inserts[1].missingState.getData(), (int) t->inserts[1].missingState.getSize());
    e.prepareProcessor (*np);
    back[0].local = std::move (np);
    e.restoreInserts (*t, back, false);
    auto* restored = dynamic_cast<CountingDelay*> (t->inserts[1].processor.get());
    check (restored != nullptr && restored->stateText == "ajuste-do-usuario" && ! t->inserts[1].isOffline(), "volta online com o ajuste");
    check (t->inserts[1].bypassed.load() && t->inserts[1].description.name == "Plug", "bypass e descricao preservados");
    e.setBypass (*t, 1, false);
    check (e.getTotalLatency() == 64, "latencia volta para a PDC");
}

// Congelar: render da faixa (fase + inserts, sem latencia) toca no lugar do
// audio com os plugins offline; a mix fica igual. Descongelar volta ao normal.
static void testFreezeTrack()
{
    const int sr = 48000, len = sr;
    Engine e; e.prepare (sr, 256);
    auto* a = addTrack (e, makeBuffer (2, len, [] (int ch, int i) { return 0.25f * std::sin ((float) i * 0.02f + (float) ch); }), "Guitarra");
    auto* b = addTrack (e, makeBuffer (1, len, [] (int, int i) { return 0.2f * std::sin ((float) i * 0.005f); }), "Baixo");
    auto p = std::make_unique<CountingDelay> (37);
    e.prepareProcessor (*p);
    juce::PluginDescription desc; desc.name = "Delay37"; desc.pluginFormatName = "VST3"; desc.fileOrIdentifier = "C:/d.vst3";
    e.setInsert (*a, 0, std::move (p), &desc);
    a->phase = true; a->gainDb = -3.0f; a->pan = 0.3f;
    b->pan = -0.2f;

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_freeze_test");
    dir.deleteRecursively();
    dir.createDirectory();
    std::atomic<float> progress { 0 }; std::atomic<bool> cancel { false };
    juce::String err; bool clip = false;
    juce::AudioFormatManager fm; registerAudioFormats (fm);
    auto mix = [&] (const juce::String& name)
    {
        auto f = dir.getChildFile (name);
        e.exportMix (f, 32, 0.5, progress, cancel, err, clip);
        return loadAudioFile (fm, f);
    };
    const auto ref = mix ("ref.wav");

    const auto frozenFile = dir.getChildFile ("Guitarra.wav");
    check (e.renderFreeze (*a, frozenFile, 1.0, progress, cancel, err), "congelar faixa " + err);
    const auto frozen = loadAudioFile (fm, frozenFile);
    float dFrozen = 1.0f;
    if (frozen.ok() && frozen.original->getNumSamples() == len + sr && frozen.original->getNumChannels() == 2)
    {
        dFrozen = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < len; ++i)
                dFrozen = juce::jmax (dFrozen, std::abs (frozen.original->getSample (ch, i) + a->playback->getSample (ch, i)));
        dFrozen = juce::jmax (dFrozen, frozen.original->getMagnitude (len, sr));
    }
    check (dFrozen < 1.0e-7f, "render congelado = fase + plugin, sem latencia, com cauda");

    auto stream = StreamingAudio::open (frozenFile, sr, err);
    check (stream != nullptr, "abrir arquivo congelado " + err);
    std::vector<std::unique_ptr<juce::AudioProcessor>> unloaded;
    e.setFrozen (*a, frozenFile, stream, unloaded);
    check (a->isFrozen() && unloaded.size() == 1 && a->inserts[0].isOffline() && e.getTotalLatency() == 0,
           "congelada: plugin offline e sem latencia");
    unloaded.clear();
    const auto frozenMix = mix ("frozen.wav");
    float d1 = 1.0f;
    if (ref.ok() && frozenMix.ok() && frozenMix.original->getNumSamples() == ref.original->getNumSamples())
    {
        d1 = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < ref.original->getNumSamples(); ++i)
                d1 = juce::jmax (d1, std::abs (ref.original->getSample (ch, i) - frozenMix.original->getSample (ch, i)));
    }
    check (d1 < 1.0e-6f, "mix com a faixa congelada = mix original (" + juce::String (d1) + ")");

    ProjectData pd;
    const auto proj = dir.getChildFile ("p.sconsole");
    check (writeProject (captureProject (e), proj, err) && readProject (proj, pd, err), "salvar/abrir com faixa congelada " + err);
    check (pd.tracks.size() == 2 && pd.tracks[0].frozenFile == frozenFile && pd.tracks[0].ch.inserts.size() == 1
           && pd.tracks[0].ch.inserts[0].offline, "projeto guarda o arquivo congelado e o plugin offline");

    std::vector<Engine::RestoredInsert> back (1);
    back[0].slot = 0;
    auto np = std::make_unique<CountingDelay> (37);
    e.prepareProcessor (*np);
    back[0].local = std::move (np);
    e.restoreInserts (*a, back, true);
    check (! a->isFrozen() && a->inserts[0].processor != nullptr && e.getTotalLatency() == 37, "descongelar: plugin de volta");
    const auto backMix = mix ("back.wav");
    float d2 = 1.0f;
    if (ref.ok() && backMix.ok() && backMix.original->getNumSamples() == ref.original->getNumSamples())
    {
        d2 = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < ref.original->getNumSamples(); ++i)
                d2 = juce::jmax (d2, std::abs (ref.original->getSample (ch, i) - backMix.original->getSample (ch, i)));
    }
    check (d2 == 0.0f, "descongelada: mix identica a original");
    stream.reset();
    dir.deleteRecursively();
}

// Plugin de teste com entrada lateral: a saida e o sinal da entrada lateral.
class SidechainProbe : public juce::AudioProcessor
{
public:
    SidechainProbe() : AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo())
                                                        .withOutput ("Out", juce::AudioChannelSet::stereo())
                                                        .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)) {}
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            && (l.inputBuses.size() < 2 || l.inputBuses[1].isDisabled() || l.inputBuses[1] == juce::AudioChannelSet::stereo());
    }
    const juce::String getName() const override { return "SidechainProbe"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        const int n = b.getNumSamples();
        if (getChannelCountOfBus (true, 1) == 2)
        {
            const int sc = getChannelIndexInProcessBlockBuffer (true, 1, 0);
            b.copyFrom (0, 0, b, sc, 0, n);
            b.copyFrom (1, 0, b, sc + 1, 0, n);
        }
        else b.clear (0, n), b.clear (1, 0, n);
    }
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
};

static float maxDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int n)
{
    float d = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < n; ++i) d = juce::jmax (d, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
    return d;
}

// Bloco B: solo de bus, envio pre-FX, dry/wet, VCA, sidechain, troca de slot.
static void testMixerBlockB()
{
    const int n = 4096;
    // ---- solo de bus: ouve o bus (faixas roteadas + envios); o resto fica mudo
    {
        Engine e; e.prepare (48000, 256);
        auto* a = addTrack (e, makeBuffer (2, n, [] (int, int) { return 0.1f; }), "a"); // -> bus 0
        auto* b = addTrack (e, makeBuffer (2, n, [] (int, int) { return 0.2f; }), "b"); // -> master, envio p/ bus 1
        auto* c = addTrack (e, makeBuffer (2, n, [] (int, int) { return 0.4f; }), "c"); // -> master
        auto* b0 = e.addBus ("Grupo");
        auto* b1 = e.addBus ("Reverb");
        a->outputBus = 0;
        b->sends[0].bus = 1; b->sends[0].levelDb = 0.0f;
        b0->solo = true;
        auto out = run (e, n);
        check (near (out.getSample (0, n - 1), 0.1f, 1.0e-5f), "solo do bus 0: so a faixa roteada nele");
        b0->solo = false; b1->solo = true;
        out = run (e, n);
        check (near (out.getSample (0, n - 1), 0.2f, 1.0e-5f), "solo do bus de reverb: ouve o envio (faixa seca muda) " + juce::String (out.getSample (0, n - 1)));
        c->solo = true;
        out = run (e, n);
        check (near (out.getSample (0, n - 1), 0.4f + 0.2f, 1.0e-5f), "solo de faixa + solo de bus somam " + juce::String (out.getSample (0, n - 1)));
        juce::ignoreUnused (b1);
    }
    // ---- envio pre-FX: antes do plugin, alinhado pela PDC
    {
        Engine e; e.prepare (48000, 256);
        auto* t = addTrack (e, makeBuffer (2, n, [] (int, int i) { return i == 1000 ? 1.0f : 0.0f; }), "imp");
        e.addBus ("FX");
        auto p = std::make_unique<InternalDelay> (50);
        e.prepareProcessor (*p);
        e.setInsert (*t, 0, std::move (p), nullptr);
        t->gainDb = kMinDb; // fader fechado: pre-FX continua
        t->sends[0].bus = 0; t->sends[0].preFx = true;
        auto out = run (e, n);
        check (peakIndex (out, 0) == 1000 + 50 && near (out.getSample (0, 1050), 1.0f, 1.0e-5f),
               "pre-FX: independe do fader e fica alinhado com a PDC (" + juce::String (peakIndex (out, 0)) + ")");
    }
    // ---- dry/wet com plugin de latencia: seco alinhado, 50% = media
    {
        Engine e; e.prepare (48000, 256);
        auto* t = addTrack (e, makeBuffer (2, n, [] (int, int i) { return i == 1000 ? 1.0f : 0.0f; }), "imp");
        auto p = std::make_unique<InternalDelay> (64);
        e.prepareProcessor (*p);
        e.setInsert (*t, 0, std::move (p), nullptr);
        t->inserts[0].mix = 0.5f;
        t->inserts[0].lastMix = 0.5f;
        auto out = run (e, n);
        // InternalDelay e um atraso puro: wet e dry atrasados iguais -> impulso inteiro
        check (peakIndex (out, 0) == 1000 + 64 && near (out.getSample (0, 1064), 1.0f, 1.0e-5f),
               "dry/wet: seco atrasado junto com o plugin (sem eco)");
    }
    // ---- VCA: soma dB no grupo
    {
        Engine e; e.prepare (48000, 256);
        auto* t = addTrack (e, makeBuffer (2, n, [] (int, int) { return 0.5f; }), "t");
        t->group = 2;
        e.vca[2].gainDb = -6.0f;
        auto out = run (e, n);
        check (near (out.getSample (0, n - 1), 0.5f * juce::Decibels::decibelsToGain (-6.0f), 1.0e-4f), "VCA -6 dB no membro");
        e.vca[2].mute = true;
        out = run (e, n, n);
        check (out.getMagnitude (0, n - 300, 300) == 0.0f, "mute do VCA cala o grupo");
    }
    // ---- sidechain: a fonte entra na entrada lateral (antes dos plugins dela)
    {
        Engine e; e.prepare (48000, 256);
        auto* dst = addTrack (e, makeBuffer (2, n, [] (int, int) { return 0.3f; }), "baixo");
        auto* src = addTrack (e, makeBuffer (2, n, [] (int, int i) { return 0.01f * (float) (i % 50); }), "bumbo");
        src->gainDb = kMinDb; // a fonte nao precisa tocar
        auto p = std::make_unique<SidechainProbe>();
        e.prepareProcessor (*p);
        e.setInsert (*dst, 0, std::move (p), nullptr);
        check (Engine::supportsSidechain (dst->inserts[0]), "plugin com entrada lateral");
        check (e.setSidechain (*dst, 0, src), "ligar sidechain");
        auto out = run (e, n);
        float d = 0.0f;
        for (int i = 2048; i < n; ++i) d = juce::jmax (d, std::abs (out.getSample (0, i) - 0.01f * (float) (i % 50))); // depois da rampa do fader
        check (d < 1.0e-6f, "plugin recebe o audio da faixa fonte");
        e.removeTrack (src);
        check (dst->inserts[0].sidechain == nullptr, "remover a fonte desliga o sidechain");
    }
    // ---- troca de slot (desfazer/refazer): o mesmo plugin volta com o ajuste
    {
        Engine e; e.prepare (48000, 256);
        auto* t = addTrack (e, makeBuffer (2, 512, [] (int, int) { return 0.1f; }), "t");
        auto p = std::make_unique<CountingDelay> (32);
        auto* raw = p.get();
        p->stateText = "meu ajuste";
        e.prepareProcessor (*p);
        e.setInsert (*t, 0, std::move (p), nullptr);
        t->inserts[0].mix = 0.7f;
        Engine::SlotContent empty;
        e.swapSlotContent (*t, 0, empty); // "remover"
        check (t->inserts[0].isEmpty() && empty.processor.get() == raw && near (empty.mix, 0.7f) && e.getTotalLatency() == 0,
               "remover guarda o plugin fora do slot");
        e.swapSlotContent (*t, 0, empty); // "desfazer"
        check (t->inserts[0].processor.get() == raw && near (t->inserts[0].mix.load(), 0.7f) && e.getTotalLatency() == 32
               && raw->stateText == "meu ajuste", "desfazer devolve o mesmo plugin, ajuste e mix");
        auto removed = e.removeTrack (t);
        auto* back = e.insertTrack (0, std::move (removed));
        check (back == t && e.tracks.size() == 1 && e.getTotalLatency() == 32, "faixa removida volta inteira");
    }
    // ---- projeto: grupo, VCA, marcadores, regioes, pre-FX, mix e sidechain
    {
        Engine e; e.prepare (48000, 256);
        auto* a = addTrack (e, makeBuffer (2, 512, [] (int, int) { return 0.1f; }), "a");
        auto* b = addTrack (e, makeBuffer (2, 512, [] (int, int) { return 0.1f; }), "b");
        e.addBus ("FX");
        a->group = 3;
        e.vca[3].gainDb = -4.5f; e.vca[3].shown = true;
        a->sends[0].bus = 0; a->sends[0].preFx = true;
        auto p = std::make_unique<SidechainProbe>();
        e.prepareProcessor (*p);
        juce::PluginDescription desc; desc.name = "SC"; desc.pluginFormatName = "VST3"; desc.fileOrIdentifier = "C:/sc.vst3";
        e.setInsert (*a, 0, std::move (p), &desc);
        e.setSidechain (*a, 0, b);
        a->inserts[0].mix = 0.25f;
        auto pd = captureProject (e);
        pd.markers.push_back ({ 1.5, "Refrao" });
        pd.regions.push_back ({ 2.0, 6.0, "Verso 1" });
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_blockb.sconsole");
        juce::String err;
        ProjectData r;
        check (writeProject (pd, file, err) && readProject (file, r, err), "salvar/abrir bloco B " + err);
        check (r.tracks.size() == 2 && r.tracks[0].ch.group == 3 && near (r.vca[3].gainDb, -4.5f) && r.vca[3].shown,
               "grupo e VCA salvos");
        check (r.tracks.size() == 2 && r.tracks[0].sends[0].preFx && r.tracks[0].ch.inserts.size() == 1
               && near (r.tracks[0].ch.inserts[0].mix, 0.25f) && r.tracks[0].ch.inserts[0].sidechain == 1,
               "pre-FX, mix e sidechain salvos");
        check (r.markers.size() == 1 && r.markers[0].name == "Refrao" && r.regions.size() == 1 && near ((float) r.regions[0].end, 6.0f)
               && r.regions[0].name == "Verso 1", "marcadores e regioes salvos");
        file.deleteFile();
    }
}

// Bloco C: medicao BS.1770 (integrado com gates) e normalizacao na exportacao.
static void testLoudnessNormalize()
{
    const int sr = 48000, len = sr * 10;
    Engine e; e.prepare (sr, 256);
    // seno de 997 Hz a -20 dBFS nos dois canais: referencia BS.1770 = -20,0 LUFS (-23 com um canal so)
    addTrack (e, makeBuffer (2, len, [sr] (int, int i)
              { return 0.1f * std::sin (2.0f * juce::MathConstants<float>::pi * 997.0f * (float) i / (float) sr); }), "seno");
    std::atomic<float> progress { 0 }; std::atomic<bool> cancel { false };
    juce::String err; bool clip = false;
    Engine::ExportReport rep;
    check (e.exportMix ({}, 24, 0.0, progress, cancel, err, clip, 0, -1, {}, &rep, true), "medir sem gravar " + err);
    check (std::abs (rep.integratedLufs - (-20.0f)) < 0.05f, "seno -20 dBFS nos 2 canais = -20,0 LUFS (" + juce::String (rep.integratedLufs, 2) + ")");
    check (std::abs (rep.samplePeakDb - (-20.0f)) < 0.05f && std::abs (rep.truePeakDb - (-20.0f)) < 0.2f,
           "picos do seno (" + juce::String (rep.samplePeakDb, 2) + " / " + juce::String (rep.truePeakDb, 2) + ")");

    // silencio antes e depois nao muda o integrado (gate absoluto de -70 LUFS)
    {
        Engine g; g.prepare (sr, 256);
        addTrack (g, makeBuffer (2, len * 2, [sr, len] (int, int i)
                  { return i < len / 2 || i >= len / 2 + len ? 0.0f
                         : 0.1f * std::sin (2.0f * juce::MathConstants<float>::pi * 997.0f * (float) i / (float) sr); }), "s");
        Engine::ExportReport r2;
        g.exportMix ({}, 24, 0.0, progress, cancel, err, clip, 0, -1, {}, &r2, true);
        check (std::abs (r2.integratedLufs - rep.integratedLufs) < 0.2f, "gate: silencio nao puxa o integrado para baixo ("
               + juce::String (r2.integratedLufs, 2) + ")");
    }

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_norm_test");
    dir.deleteRecursively();
    dir.createDirectory();
    juce::AudioFormatManager fm; registerAudioFormats (fm);
    auto exportWith = [&] (Engine::Normalize n, const juce::String& name, int bits, Engine::ExportReport& out)
    {
        auto f = dir.getChildFile (name);
        const bool ok = e.exportMix (f, bits, 0.0, progress, cancel, err, clip, 0, -1, n, &out);
        return ok ? loadAudioFile (fm, f) : LoadedAudio {};
    };
    using M = Engine::Normalize::Mode;
    Engine::ExportReport r;
    auto a = exportWith ({ M::lufs, -14.0f, -1.0f }, "lufs.wav", 24, r);
    float measured = -100.0f;
    if (a.ok())
    {
        LoudnessAnalyzer an; an.prepare (sr);
        an.process (a.original->getReadPointer (0), a.original->getReadPointer (1), a.original->getNumSamples());
        measured = an.integratedLufs();
    }
    check (std::abs (r.integratedLufs - (-14.0f)) < 0.05f && std::abs (measured - (-14.0f)) < 0.1f && std::abs (r.gainDb - 6.0f) < 0.1f,
           "normalizar para -14 LUFS (arquivo mede " + juce::String (measured, 2) + ", ganho " + juce::String (r.gainDb, 2) + " dB)");
    auto b = exportWith ({ M::peak, -1.0f, -1.0f }, "peak.wav", 24, r);
    check (b.ok() && std::abs (juce::Decibels::gainToDecibels (b.original->getMagnitude (0, b.original->getNumSamples())) - (-1.0f)) < 0.02f,
           "normalizar pico para -1 dBFS");
    exportWith ({ M::lufs, 0.0f, -1.0f }, "ceiling.wav", 32, r);
    check (r.limitedByCeiling && std::abs (r.truePeakDb - (-1.0f)) < 0.05f && r.integratedLufs < -0.5f,
           "teto de true peak segura o ganho (" + juce::String (r.truePeakDb, 2) + " dBTP)");
    auto c = exportWith ({ M::lufs, -14.0f, -1.0f }, "lufs16.wav", 16, r);
    check (c.ok() && c.original->getNumSamples() == len, "normalizado em 16 bits com dither");
    cancel = true;
    check (! e.exportMix (dir.getChildFile ("x.wav"), 24, 0.0, progress, cancel, err, clip, 0, -1, { M::lufs, -14.0f, -1.0f }) && ! e.rendering.load(),
           "cancelar a exportacao normalizada");
    cancel = false;
    dir.deleteRecursively();
}

// Inserts e envios dinamicos (botao "+"), 16 buses e persistencia no projeto.
static void testDynamicSlots()
{
    Engine e;
    e.prepare (48000.0, 256);
    auto* t = addTrack (e, makeBuffer (2, 4800, [] (int, int) { return 0.1f; }), "slots");
    check (t->inserts.size() == kDefaultInserts && t->sends.size() == kDefaultSends,
           "slots: padrao de 4 inserts e 4 envios");
    check (e.setInsertCount (*t, 10) && t->inserts.size() == 10, "slots: adicionar inserts alem de 8");
    auto p = std::make_unique<InternalDelay> (5);
    e.prepareProcessor (*p);
    e.setInsert (*t, 8, std::move (p), nullptr);
    check (e.getTotalLatency() == 5, "slots: plugin no insert 9 processa (latencia 5)");
    e.setInsertCount (*t, 2);
    check (t->inserts.size() == 9, "slots: reduzir so remove slots vazios do fim (plugin no 9 preservado)");
    check (e.setSendCount (*t, 7) && t->sends.size() == 7, "slots: adicionar envios");
    for (int i = 0; i < kMaxBuses; ++i) e.addBus();
    check ((int) e.buses.size() == 16 && e.addBus() == nullptr, "slots: ate 16 buses");
    t->sends[6].bus = 15;
    t->sends[6].levelDb = -3.0f;
    // insert alem do 9 vem do projeto mesmo sem plugin (slot vazio adicionado pelo usuario)
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_slots.sconsole");
    juce::String err;
    check (writeProject (captureProject (e), file, err), "slots: salvar " + err);
    ProjectData pd;
    check (readProject (file, pd, err), "slots: abrir " + err);
    check (pd.tracks.size() == 1 && pd.tracks[0].ch.numInserts == 9 && pd.tracks[0].sends.size() == 7
           && pd.tracks[0].sends[6].bus == 15 && near (pd.tracks[0].sends[6].levelDb, -3.0f)
           && pd.buses.size() == 16,
           "slots: projeto guarda numero de inserts, envios e 16 buses");
    Engine e2;
    auto t2 = std::make_unique<Track>();
    applyChannelBasics (e2, *t2, pd.tracks[0].ch);
    check (t2->inserts.size() == 9, "slots: ao abrir, recria os slots");
    file.deleteFile();
}

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

static void testScanSupervisor (const juce::File& worker)
{
    check (worker.existsAsFile(), "scan supervisor: executavel auxiliar presente");
    if (! worker.existsAsFile()) return;

    auto argv = [&] (const juce::String& command, const juce::String& value)
    {
        juce::StringArray args;
        args.add (worker.getFullPathName());
        args.add (command);
        args.add (value);
        return args;
    };

    std::atomic<bool> cancelled { false };
    const auto success = PluginScanSupervisor::run (
        argv ("--self-test-exit", "0"), cancelled, 2000);
    check (success.ok(), "scan supervisor: resultado positivo");

    const auto failure = PluginScanSupervisor::run (
        argv ("--self-test-exit", "7"), cancelled, 2000);
    check (failure.status == PluginScanSupervisor::Status::finished
           && failure.exitCode == 7 && ! failure.ok(),
           "scan supervisor: falha de processo retorna codigo sem derrubar host");

    const auto started = juce::Time::getMillisecondCounter();
    const auto timeout = PluginScanSupervisor::run (
        argv ("--self-test-sleep", "3000"), cancelled, 250);
    const auto elapsed = juce::Time::getMillisecondCounter() - started;
    check (timeout.status == PluginScanSupervisor::Status::timedOut
           && elapsed < 1800,
           "scan supervisor: mata scanner travado dentro do prazo");

    cancelled = true;
    const auto alreadyCancelled = PluginScanSupervisor::run (
        argv ("--self-test-sleep", "3000"), cancelled, 2000);
    check (alreadyCancelled.status == PluginScanSupervisor::Status::cancelled,
           "scan supervisor: nao inicia processo com pedido de cancelamento");
    cancelled = false;

    std::thread cancelThread ([&]
    {
        juce::Thread::sleep (160);
        cancelled.store (true, std::memory_order_release);
    });
    const auto interrupted = PluginScanSupervisor::run (
        argv ("--self-test-sleep", "3000"), cancelled, 2000);
    cancelThread.join();
    check (interrupted.status == PluginScanSupervisor::Status::cancelled,
           "scan supervisor: cancelamento encerra filho ja em execucao");
}

static void testPluginScanQuarantine()
{
    auto failed = PluginScanPolicy::deserialize (
        "C:/VST3/Crashing.vst3\nC:/VST3/CRASHING.vst3\n\n");
    check (failed.size() == 1,
           "scan quarantine: entradas duplicadas ignoradas sem diferenciar maiusculas");
    check (! PluginScanPolicy::shouldScan (failed, "c:/vst3/crashing.vst3", false),
           "scan quarantine: plugin previamente falho e ignorado por padrao");
    check (PluginScanPolicy::shouldScan (failed, "c:/vst3/crashing.vst3", true),
           "scan quarantine: usuario pode reavaliar plugin falho");
    check (! PluginScanPolicy::shouldScan (failed, "C:/VST3/NewlyDiscovered.vst3", true),
           "scan quarantine: reavaliacao nao examina plugins sem falhas previas");
    check (PluginScanPolicy::shouldScan (failed, "C:/VST3/NewlyDiscovered.vst3", false),
           "scan quarantine: varredura normal examina novos plugins");

    PluginScanPolicy::reportResult (failed, "C:/VST3/new.vst3", false);
    PluginScanPolicy::reportResult (failed, "C:/VST3/new.vst3", false);
    check (failed.size() == 2, "scan quarantine: falhas registradas uma unica vez");
    const auto stored = PluginScanPolicy::serialize (failed);
    auto restored = PluginScanPolicy::deserialize (stored);
    check (restored.size() == 2, "scan quarantine: persistencia recupera candidatos");
    PluginScanPolicy::reportResult (restored, "c:/VST3/Crashing.vst3", true);
    check (restored.size() == 1
           && PluginScanPolicy::shouldScan (restored, "C:/VST3/Crashing.vst3", false),
           "scan quarantine: sucesso remove bloqueio anterior");
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
    testDspIpcWatchdog();
    testPluginExceptionFallback();
    testPluginScanQuarantine();
    testTruePeakMeter();
    testMomentaryLoudness();
    test128TrackStress();
    testMulticoreOfflineMix();
    testRealtimeMulticore();
    testParallelBuses();
    testSelectionPlayback();
    testStemExport();
    testCpuSaverSleep();
    testPluginOffline();
    testFreezeTrack();
    testMixerBlockB();
    testLoudnessNormalize();
    testDynamicSlots();
    testDiskStreaming();
    testStereoBalanceMuteSolo();
    testBusesAndSends();
    testLatencyCompensation();
    testExportDither();
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
    if (argc > 3)
        testScanSupervisor (juce::File (juce::String::fromUTF8 (argv[3])));
#if JUCE_WINDOWS
    if (argc > 4 && argc > 1)
        testRemoteDspWorker (juce::File (juce::String::fromUTF8 (argv[4])),
                             juce::File (juce::String::fromUTF8 (argv[1])));
#endif
    std::printf ("\n%s (%d falha(s))\n", failures == 0 ? "TODOS OS TESTES PASSARAM" : "HOUVE FALHAS", failures);
    return failures == 0 ? 0 : 1;
}
