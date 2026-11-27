// SilverConsole - medicao de true peak por reconstrucao sinc 4x, sem alocacao realtime.
// A leitura representa picos entre amostras (estimativa dBTP), nao modifica o audio.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace sc
{
/** Medidor estereo de pico entre amostras por FIR de 32 taps e 4 fases.
    Historico persistente preserva continuidade entre callbacks.
    Nao implementa por si so um verificador de conformidade EBU R128. */
class TruePeak4x
{
public:
    struct Peaks { float left = 0.0f, right = 0.0f; };
    static constexpr int taps = 32;
    static constexpr int phases = 4;

    TruePeak4x() { prepare(); }

    void prepare() noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        for (int phase = 0; phase < phases; ++phase)
        {
            double sum = 0.0;
            for (int j = 0; j < taps; ++j)
            {
                // Reconstrucao ideal atrasada em 15 amostras; Blackman limita
                // lobulos do sinc e permite operacao com janela curta.
                const double d = 15.0 + (double) phase / phases - (double) j;
                const double sinc = std::abs (d) < 1.0e-12
                                      ? 1.0 : std::sin (pi * d) / (pi * d);
                const double window = 0.42 - 0.5 * std::cos (2.0 * pi * j / (taps - 1))
                                           + 0.08 * std::cos (4.0 * pi * j / (taps - 1));
                const double tap = sinc * window;
                coefficients[(size_t) phase][(size_t) j] = (float) tap;
                sum += tap;
            }
            if (std::abs (sum) > 1.0e-12)
                for (auto& coef : coefficients[(size_t) phase])
                    coef = (float) (coef / sum);
        }
        reset();
    }

    void reset() noexcept
    {
        historyL.fill (0.0f);
        historyR.fill (0.0f);
        cursor = 0;
    }

    Peaks process (const float* l, const float* r, int count) noexcept
    {
        Peaks p;
        if (l == nullptr || r == nullptr || count <= 0) return p;
        for (int i = 0; i < count; ++i)
        {
            historyL[(size_t) cursor] = l[i];
            historyR[(size_t) cursor] = r[i];
            p.left  = std::max (p.left, std::abs (l[i]));
            p.right = std::max (p.right, std::abs (r[i]));
            for (int phase = 0; phase < phases; ++phase)
            {
                double vl = 0.0, vr = 0.0;
                for (int j = 0; j < taps; ++j)
                {
                    const auto index = (size_t) ((cursor - j) & (taps - 1));
                    const float coefficient = coefficients[(size_t) phase][(size_t) j];
                    vl += (double) historyL[index] * coefficient;
                    vr += (double) historyR[index] * coefficient;
                }
                p.left = std::max (p.left, (float) std::abs (vl));
                p.right = std::max (p.right, (float) std::abs (vr));
            }
            cursor = (cursor + 1) & (taps - 1);
        }
        return p;
    }

private:
    std::array<std::array<float, taps>, phases> coefficients {};
    std::array<float, taps> historyL {}, historyR {};
    int cursor = 0;
};
/** Sonoridade momentanea (aprox. 400 ms) com K-weighting de BS.1770.
    Usa sub-janelas de 100 ms. Nao faz gating/integracao EBU R128:
    NAO usar como medidor de LUFS integrado ou para certificacao de entrega. */
class MomentaryLoudness
{
public:
    void prepare (double rate) noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        const double sr = std::max (8000.0, rate);
        segmentSamples = std::max (1, (int) std::round (sr * 0.1));
        // Coeficientes exatos do pre-filtro BS.1770 (derivacao bilinear de referencia;
        // a 48 kHz da b = 1.53512486, -2.69169619, 1.19839281 / a = -1.69065929, 0.73248077).
        double K = std::tan (pi * 1681.974450955533 / sr);
        const double Q1 = 0.7071752369554196;
        const double Vh = std::pow (10.0, 3.999843853973347 / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double d1 = 1.0 + K / Q1 + K * K;
        const double sb0 = (Vh + Vb * K / Q1 + K * K) / d1, sb1 = 2.0 * (K * K - Vh) / d1, sb2 = (Vh - Vb * K / Q1 + K * K) / d1;
        const double sa1 = 2.0 * (K * K - 1.0) / d1, sa2 = (1.0 - K / Q1 + K * K) / d1;
        K = std::tan (pi * 38.13547087602444 / sr);
        const double Q2 = 0.5003270373238773;
        const double d2 = 1.0 + K / Q2 + K * K;
        const double ha1 = 2.0 * (K * K - 1.0) / d2, ha2 = (1.0 - K / Q2 + K * K) / d2;
        for (auto& filter : shelf) filter.setup (sb0, sb1, sb2, 1.0, sa1, sa2);
        for (auto& filter : highpass) filter.setup (1.0, -2.0, 1.0, 1.0, ha1, ha2);
        reset();
    }

    void reset() noexcept
    {
        energies.fill (0.0);
        index = 0;
        cursor = 0;
        populated = 0;
        latest = -120.0f;
        for (auto& b : shelf) b.reset();
        for (auto& b : highpass) b.reset();
    }

    void process (const float* left, const float* right, int n) noexcept
    {
        if (left == nullptr || right == nullptr || n <= 0) return;
        for (int i = 0; i < n; ++i)
        {
            const double l = highpass[0].process (shelf[0].process (left[i]));
            const double r = highpass[1].process (shelf[1].process (right[i]));
            energies[(size_t) index] += l * l + r * r;
            if (++cursor == segmentSamples)
            {
                populated = std::min (4, populated + 1);
                double total = 0.0;
                for (double e : energies) total += e;
                const double mean = total / (double) (populated * segmentSamples);
                latest = mean > 1.0e-15
                             ? (float) (-0.691 + 10.0 * std::log10 (mean))
                             : -120.0f;
                index = (index + 1) & 3;
                energies[(size_t) index] = 0.0;
                cursor = 0;
            }
        }
    }

    float getLufsM() const noexcept { return latest; }

private:
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        void setup (double bb0, double bb1, double bb2,
                    double aa0, double aa1, double aa2) noexcept
        {
            b0 = bb0 / aa0; b1 = bb1 / aa0; b2 = bb2 / aa0;
            a1 = aa1 / aa0; a2 = aa2 / aa0;
            reset();
        }
        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0.0; }
    };

    std::array<Biquad, 2> shelf {}, highpass {};
    std::array<double, 4> energies {};
    int segmentSamples = 4800;
    int cursor = 0, index = 0, populated = 0;
    float latest = -120.0f;
};

/** Filtro K (BS.1770): shelf de agudos + passa-altas RLB, um canal. */
class KWeighting
{
public:
    void prepare (double rate) noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        const double sr = std::max (8000.0, rate);
        // Coeficientes exatos do pre-filtro BS.1770 (derivacao bilinear de referencia;
        // a 48 kHz da b = 1.53512486, -2.69169619, 1.19839281 / a = -1.69065929, 0.73248077).
        double K = std::tan (pi * 1681.974450955533 / sr);
        const double Q1 = 0.7071752369554196;
        const double Vh = std::pow (10.0, 3.999843853973347 / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double d1 = 1.0 + K / Q1 + K * K;
        const double sb0 = (Vh + Vb * K / Q1 + K * K) / d1, sb1 = 2.0 * (K * K - Vh) / d1, sb2 = (Vh - Vb * K / Q1 + K * K) / d1;
        const double sa1 = 2.0 * (K * K - 1.0) / d1, sa2 = (1.0 - K / Q1 + K * K) / d1;
        K = std::tan (pi * 38.13547087602444 / sr);
        const double Q2 = 0.5003270373238773;
        const double d2 = 1.0 + K / Q2 + K * K;
        const double ha1 = 2.0 * (K * K - 1.0) / d2, ha2 = (1.0 - K / Q2 + K * K) / d2;
        shelf.setup (sb0, sb1, sb2, 1.0, sa1, sa2);
        highpass.setup (1.0, -2.0, 1.0, 1.0, ha1, ha2);
    }
    double process (double x) noexcept { return highpass.process (shelf.process (x)); }

private:
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
        void setup (double bb0, double bb1, double bb2, double aa0, double aa1, double aa2) noexcept
        {
            b0 = bb0 / aa0; b1 = bb1 / aa0; b2 = bb2 / aa0; a1 = aa1 / aa0; a2 = aa2 / aa0; z1 = z2 = 0.0;
        }
        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    Biquad shelf, highpass;
};

/** Medicao offline para exportacao (BS.1770-4 / EBU R128): loudness integrado
    com gate absoluto (-70 LUFS) e relativo (-10 LU), maximo de curto prazo
    (janela de 3 s), true peak (4x) e pico de amostra. Aloca: so fora do audio. */
class LoudnessAnalyzer
{
public:
    void prepare (double rate)
    {
        for (auto& k : kw) k.prepare (rate);
        segmentSamples = std::max (1, (int) std::round (std::max (8000.0, rate) * 0.1));
        segments.clear();
        accumulated = 0.0;
        cursor = 0;
        samplePeak = truePeak = 0.0f;
        peakMeter.reset();
    }

    void process (const float* l, const float* r, int n)
    {
        if (l == nullptr || r == nullptr || n <= 0) return;
        for (int i = 0; i < n; ++i)
        {
            samplePeak = std::max (samplePeak, std::max (std::abs (l[i]), std::abs (r[i])));
            const double kl = kw[0].process (l[i]), kr = kw[1].process (r[i]);
            accumulated += kl * kl + kr * kr;
            if (++cursor == segmentSamples)
            {
                segments.push_back (accumulated / (double) segmentSamples);
                accumulated = 0.0;
                cursor = 0;
            }
        }
        const auto p = peakMeter.process (l, r, n);
        truePeak = std::max (truePeak, std::max (p.left, p.right));
    }

    static float toLufs (double meanSquare) noexcept
    {
        return meanSquare > 1.0e-20 ? (float) (-0.691 + 10.0 * std::log10 (meanSquare)) : -std::numeric_limits<float>::infinity();
    }

    /** -inf se for silencio (todos os blocos abaixo de -70 LUFS). */
    float integratedLufs() const
    {
        std::vector<double> blocks; // 400 ms com passo de 100 ms (75% de sobreposicao)
        if (segments.size() < 4)
        {
            double sum = 0.0;
            for (double e : segments) sum += e;
            if (! segments.empty()) blocks.push_back (sum / (double) segments.size());
        }
        else
            for (size_t k = 3; k < segments.size(); ++k)
                blocks.push_back ((segments[k - 3] + segments[k - 2] + segments[k - 1] + segments[k]) * 0.25);
        const double absGate = std::pow (10.0, (-70.0 + 0.691) / 10.0);
        double sum = 0.0;
        int count = 0;
        for (double e : blocks) if (e > absGate) { sum += e; ++count; }
        if (count == 0) return -std::numeric_limits<float>::infinity();
        const double relGate = std::max (absGate, (sum / count) * 0.1); // -10 LU
        sum = 0.0;
        count = 0;
        for (double e : blocks) if (e > relGate) { sum += e; ++count; }
        return count > 0 ? toLufs (sum / count) : -std::numeric_limits<float>::infinity();
    }

    float maxShortTermLufs() const
    {
        if (segments.empty()) return -std::numeric_limits<float>::infinity();
        const size_t w = std::min<size_t> (30, segments.size());
        double run = 0.0, best = 0.0;
        for (size_t k = 0; k < segments.size(); ++k)
        {
            run += segments[k];
            if (k >= w) run -= segments[k - w];
            if (k + 1 >= w) best = std::max (best, run / (double) w);
        }
        return toLufs (best);
    }

    static float toDb (float g) noexcept { return g > 1.0e-9f ? 20.0f * std::log10 (g) : -std::numeric_limits<float>::infinity(); }
    float samplePeakDb() const noexcept { return toDb (samplePeak); }
    float truePeakDb() const noexcept { return toDb (truePeak); }

private:
    KWeighting kw[2];
    TruePeak4x peakMeter;
    std::vector<double> segments; // energia media K-ponderada (L+R) a cada 100 ms
    double accumulated = 0.0;
    int segmentSamples = 4800, cursor = 0;
    float samplePeak = 0.0f, truePeak = 0.0f;
};

} // namespace sc
