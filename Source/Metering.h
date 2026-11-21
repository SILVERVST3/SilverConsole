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
} // namespace sc
