// SilverConsole - medicao de true peak por reconstrucao sinc 4x, sem alocacao realtime.
// A leitura representa picos entre amostras (estimativa dBTP), nao modifica o audio.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

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
        // K-weight: shelf de alta frequencia seguida de HP RLB.
        const double gain = std::pow (10.0, 3.999843853973347 / 40.0);
        const double w1 = 2.0 * pi * 1681.974450955533 / sr;
        const double a1 = std::sin (w1) / (2.0 * 0.7071752369554196);
        const double co1 = std::cos (w1);
        const double root = std::sqrt (gain);
        const double b0 = gain * ((gain + 1.0) + (gain - 1.0) * co1 + 2.0 * root * a1);
        const double b1 = -2.0 * gain * ((gain - 1.0) + (gain + 1.0) * co1);
        const double b2 = gain * ((gain + 1.0) + (gain - 1.0) * co1 - 2.0 * root * a1);
        const double a0 = (gain + 1.0) - (gain - 1.0) * co1 + 2.0 * root * a1;
        const double a1s = 2.0 * ((gain - 1.0) - (gain + 1.0) * co1);
        const double a2 = (gain + 1.0) - (gain - 1.0) * co1 - 2.0 * root * a1;

        const double w2 = 2.0 * pi * 38.13547087602444 / sr;
        const double alpha = std::sin (w2) / (2.0 * 0.5003270373238773);
        const double co2 = std::cos (w2);
        const double hb0 = (1.0 + co2) * 0.5;
        const double hb1 = -(1.0 + co2);
        const double hb2 = hb0;
        const double ha0 = 1.0 + alpha;
        const double ha1 = -2.0 * co2;
        const double ha2 = 1.0 - alpha;
        for (auto& filter : shelf) filter.setup (b0, b1, b2, a0, a1s, a2);
        for (auto& filter : highpass) filter.setup (hb0, hb1, hb2, ha0, ha1, ha2);
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

} // namespace sc
