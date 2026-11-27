// SilverConsole - compressor de barramento do master (estilo console classico:
// VCA feed-forward, estereo ligado, ratios 2/4/10, ataque e release em passos,
// release automatico de dois estagios). Codigo original (MIT). Sem latencia.
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cmath>

namespace sc
{
struct BusCompParams
{
    std::atomic<bool> enabled { false };
    std::atomic<float> thresholdDb { -10.0f }; // -30..+10
    std::atomic<int> ratio { 1 };              // indice em ratios()
    std::atomic<int> attack { 2 };             // indice em attacksMs()
    std::atomic<int> release { 4 };            // indice em releasesS() (ultimo = Auto)
    std::atomic<float> makeupDb { 0.0f };      // 0..+20
    std::atomic<float> mix { 1.0f };           // 0 = seco (paralelo), 1 = so comprimido
    std::atomic<int> sidechainHpf { 0 };       // indice em hpfHz() (0 = desligado)

    static constexpr int numRatios = 3, numAttacks = 6, numReleases = 5, numHpf = 4;
    static float ratioValue (int i)    { static const float v[] { 2.0f, 4.0f, 10.0f }; return v[juce::jlimit (0, numRatios - 1, i)]; }
    static float attackMs (int i)      { static const float v[] { 0.1f, 0.3f, 1.0f, 3.0f, 10.0f, 30.0f }; return v[juce::jlimit (0, numAttacks - 1, i)]; }
    static float releaseS (int i)      { static const float v[] { 0.1f, 0.3f, 0.6f, 1.2f, -1.0f }; return v[juce::jlimit (0, numReleases - 1, i)]; } // -1 = auto
    static float hpfHz (int i)         { static const float v[] { 0.0f, 60.0f, 90.0f, 150.0f }; return v[juce::jlimit (0, numHpf - 1, i)]; }
    static juce::String ratioText (int i)   { return juce::String ((int) ratioValue (i)) + ":1"; }
    static juce::String attackText (int i)  { const float a = attackMs (i); return a < 1.0f ? juce::String (a, 1) : juce::String ((int) a); }
    static juce::String releaseText (int i) { const float r = releaseS (i); return r < 0.0f ? juce::String ("AUTO") : juce::String (r, 1); }
    static juce::String hpfText (int i)     { const float h = hpfHz (i); return h <= 0.0f ? juce::String ("OFF") : juce::String ((int) h); }

    void copyFrom (const BusCompParams& o)
    {
        enabled = o.enabled.load(); thresholdDb = o.thresholdDb.load(); ratio = o.ratio.load(); attack = o.attack.load();
        release = o.release.load(); makeupDb = o.makeupDb.load(); mix = o.mix.load(); sidechainHpf = o.sidechainHpf.load();
    }
    void reset() { BusCompParams d; copyFrom (d); }

    // parametros por numero (desfazer e interface)
    enum Id { idEnabled, idThreshold, idRatio, idAttack, idRelease, idMakeup, idMix, idHpf, numIds };
    float get (int id) const
    {
        switch (id)
        {
            case idEnabled:   return enabled.load() ? 1.0f : 0.0f;
            case idThreshold: return thresholdDb.load();
            case idRatio:     return (float) ratio.load();
            case idAttack:    return (float) attack.load();
            case idRelease:   return (float) release.load();
            case idMakeup:    return makeupDb.load();
            case idMix:       return mix.load();
            case idHpf:       return (float) sidechainHpf.load();
            default:          return 0.0f;
        }
    }
    void set (int id, float v)
    {
        switch (id)
        {
            case idEnabled:   enabled = v > 0.5f; break;
            case idThreshold: thresholdDb = juce::jlimit (-30.0f, 10.0f, v); break;
            case idRatio:     ratio = juce::jlimit (0, numRatios - 1, juce::roundToInt (v)); break;
            case idAttack:    attack = juce::jlimit (0, numAttacks - 1, juce::roundToInt (v)); break;
            case idRelease:   release = juce::jlimit (0, numReleases - 1, juce::roundToInt (v)); break;
            case idMakeup:    makeupDb = juce::jlimit (0.0f, 20.0f, v); break;
            case idMix:       mix = juce::jlimit (0.0f, 1.0f, v); break;
            case idHpf:       sidechainHpf = juce::jlimit (0, numHpf - 1, juce::roundToInt (v)); break;
            default: break;
        }
    }
    static juce::String nameOf (int id)
    {
        static const char* n[] { "IN", "THRESH", "RATIO", "ATTACK", "RELEASE", "MAKEUP", "MIX", "S/C HPF" };
        return juce::isPositiveAndBelow (id, (int) numIds) ? juce::String (n[id]) : juce::String();
    }
};

class BusCompressor
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate > 0 ? sampleRate : 48000.0;
        reset();
        lastHpf = -1;
    }

    void reset()
    {
        envDb = 0.0f; grFast = grSlow = 0.0f; gainNow = 1.0f;
        for (auto& s : hpState) s = {};
        wasEnabled = false;
    }

    /** Processa no lugar. Publica a maior reducao de ganho do bloco (dB >= 0) em grOut. */
    void process (float* L, float* R, int n, const BusCompParams& p, std::atomic<float>& grOut) noexcept
    {
        const bool on = p.enabled.load (std::memory_order_relaxed);
        if (! on)
        {
            if (wasEnabled) reset();
            return;
        }
        wasEnabled = true;

        const float threshold = p.thresholdDb.load (std::memory_order_relaxed);
        const float ratio = BusCompParams::ratioValue (p.ratio.load (std::memory_order_relaxed));
        const float slope = 1.0f - 1.0f / ratio;
        const float aMs = BusCompParams::attackMs (p.attack.load (std::memory_order_relaxed));
        const float rS = BusCompParams::releaseS (p.release.load (std::memory_order_relaxed));
        const bool autoRelease = rS < 0.0f;
        const float makeup = juce::Decibels::decibelsToGain (p.makeupDb.load (std::memory_order_relaxed));
        const float wet = juce::jlimit (0.0f, 1.0f, p.mix.load (std::memory_order_relaxed));
        const int hpf = p.sidechainHpf.load (std::memory_order_relaxed);
        if (hpf != lastHpf) { setupHpf (BusCompParams::hpfHz (hpf)); lastHpf = hpf; }

        const float att = coeff (aMs * 0.001f);
        const float rel = coeff (autoRelease ? 0.1f : rS);
        // Release automatico: um envelope rapido (100 ms) para transientes e um lento
        // (1,2 s, com ataque de ~150 ms) para compressao sustentada; vale o maior.
        const float slowAtt = coeff (0.15f), slowRel = coeff (1.2f);
        const float kneeDb = 2.0f;
        // ganho aplicado com rampa curta (evita "zipper" quando o envelope pula)
        const float gainSmooth = coeff (0.0003f);
        float maxGr = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            float dl = L[i], dr = R[i];
            if (hpfOn)
            {
                dl = hpState[0].process (dl, hp);
                dr = hpState[1].process (dr, hp);
            }
            const float level = juce::jmax (std::abs (dl), std::abs (dr));
            const float inDb = level > 1.0e-6f ? 20.0f * std::log10 (level) : -120.0f;

            // curva com joelho suave
            const float over = inDb - threshold;
            float target = 0.0f;
            if (over > kneeDb * 0.5f) target = over * slope;
            else if (over > -kneeDb * 0.5f)
            {
                const float x = over + kneeDb * 0.5f;
                target = slope * x * x / (2.0f * kneeDb);
            }

            // detector suave no dominio dB (ataque/release desacoplados)
            envDb = target > envDb ? target + att * (envDb - target) : target + rel * (envDb - target);
            float gr = envDb;
            if (autoRelease)
            {
                grSlow = target > grSlow ? target + slowAtt * (grSlow - target) : target + slowRel * (grSlow - target);
                gr = juce::jmax (envDb, grSlow * 0.85f);
            }
            maxGr = juce::jmax (maxGr, gr);

            const float g = juce::Decibels::decibelsToGain (-gr) * makeup;
            gainNow = g + gainSmooth * (gainNow - g);
            const float mixG = wet * gainNow + (1.0f - wet);
            L[i] *= mixG;
            R[i] *= mixG;
        }
        float cur = grOut.load (std::memory_order_relaxed);
        while (maxGr > cur && ! grOut.compare_exchange_weak (cur, maxGr, std::memory_order_relaxed)) {}
    }

private:
    float coeff (float seconds) const noexcept
    {
        return seconds <= 0.0f ? 0.0f : std::exp (-1.0f / (seconds * (float) sr));
    }

    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct BiquadState
    {
        float z1 = 0, z2 = 0;
        float process (float x, const Biquad& c) noexcept
        {
            const float y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }
    };

    void setupHpf (float hz)
    {
        hpfOn = hz > 0.0f;
        for (auto& s : hpState) s = {};
        if (! hpfOn) return;
        // Butterworth 2a ordem (RBJ, Q = 0,7071)
        const double w0 = juce::MathConstants<double>::twoPi * hz / sr;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha;
        hp.b0 = (float) ((1.0 + cw) * 0.5 / a0);
        hp.b1 = (float) (-(1.0 + cw) / a0);
        hp.b2 = hp.b0;
        hp.a1 = (float) (-2.0 * cw / a0);
        hp.a2 = (float) ((1.0 - alpha) / a0);
    }

    double sr = 48000.0;
    float envDb = 0.0f, grFast = 0.0f, grSlow = 0.0f, gainNow = 1.0f;
    Biquad hp;
    BiquadState hpState[2];
    bool hpfOn = false, wasEnabled = false;
    int lastHpf = -1;
};
} // namespace sc
