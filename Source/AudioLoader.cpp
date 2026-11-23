// SilverConsole - leitura e reamostragem de arquivos de audio. Codigo original (MIT).
#include "AudioLoader.h"
#include <cmath>
#include <limits>
#include <vector>

namespace sc
{
void registerAudioFormats (juce::AudioFormatManager& fm)
{
    fm.registerBasicFormats(); // WAV, AIFF, FLAC, Ogg Vorbis, MP3 (JUCE_USE_MP3AUDIOFORMAT)
}

juce::String audioWildcard() { return "*.wav;*.wave;*.aif;*.aiff;*.flac;*.ogg;*.mp3"; }

bool isAudioFile (const juce::File& f)
{
    return f.hasFileExtension ("wav;wave;aif;aiff;flac;ogg;mp3");
}

LoadedAudio loadAudioFile (juce::AudioFormatManager& fm, const juce::File& file)
{
    LoadedAudio out;
    if (! file.existsAsFile()) { out.error = "Arquivo nao encontrado: " + file.getFullPathName(); return out; }

    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) { out.error = "Formato nao reconhecido: " + file.getFileName(); return out; }
    if (reader->lengthInSamples <= 0) { out.error = "Arquivo vazio: " + file.getFileName(); return out; }
    if (reader->lengthInSamples > (juce::int64) std::numeric_limits<int>::max() / 2)
    {
        out.error = "Arquivo longo demais: " + file.getFileName();
        return out;
    }
    if (reader->sampleRate < 8000.0 || reader->sampleRate > 384000.0)
    {
        out.error = "Taxa de amostragem invalida: " + file.getFileName();
        return out;
    }

    const int channels = reader->numChannels >= 2 ? 2 : 1;
    const int len = (int) reader->lengthInSamples;
    auto buf = std::make_shared<juce::AudioBuffer<float>> (channels, len);
    if (! reader->read (buf.get(), 0, len, 0, true, channels > 1))
    {
        out.error = "Falha ao ler: " + file.getFileName();
        return out;
    }
    for (int c = 0; c < channels; ++c)
    {
        auto* d = buf->getWritePointer (c);
        for (int i = 0; i < len; ++i)
            if (! std::isfinite (d[i])) d[i] = 0.0f;
    }
    out.original = std::move (buf);
    out.rate = reader->sampleRate;
    return out;
}

//==============================================================================
namespace
{
    constexpr int kZeroCrossings = 24;     // por lado
    constexpr double kCutoff = 0.95;       // fracao da menor Nyquist
    constexpr int kTableSize = kZeroCrossings * 1024 + 1;

    double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1e-12 * sum) break;
        }
        return sum;
    }

    /** Nucleo K(u), u em [0,1]: sinc(cutoff*N*u) * Kaiser(u). */
    const std::vector<float>& kernelTable()
    {
        static const std::vector<float> table = []
        {
            std::vector<float> t ((size_t) kTableSize + 1);
            const double beta = 8.6, i0b = besselI0 (beta);
            for (int i = 0; i < kTableSize; ++i)
            {
                const double u = (double) i / (double) (kTableSize - 1);
                const double x = kCutoff * kZeroCrossings * u * juce::MathConstants<double>::pi;
                const double sinc = x == 0.0 ? 1.0 : std::sin (x) / x;
                const double w = besselI0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - u * u))) / i0b;
                t[(size_t) i] = (float) (sinc * w);
            }
            t[(size_t) kTableSize] = 0.0f;
            return t;
        }();
        return table;
    }
}

std::shared_ptr<const juce::AudioBuffer<float>> resampleBuffer (std::shared_ptr<const juce::AudioBuffer<float>> src,
                                                                double fromRate, double toRate)
{
    if (src == nullptr || fromRate <= 0 || toRate <= 0) return src;
    if (std::abs (fromRate - toRate) < 1.0e-6) return src;

    const int inLen = src->getNumSamples();
    const int outLen = (int) resampledLength (inLen, fromRate, toRate);
    auto dst = std::make_shared<juce::AudioBuffer<float>> (src->getNumChannels(), juce::jmax (1, outLen));
    for (int c = 0; c < src->getNumChannels(); ++c)
        resampleRange (src->getReadPointer (c), 0, inLen, inLen, fromRate, toRate, 0, outLen, dst->getWritePointer (c));
    return dst;
}

juce::int64 resampledLength (juce::int64 srcLength, double fromRate, double toRate)
{
    if (fromRate <= 0 || toRate <= 0) return srcLength;
    return (juce::int64) std::ceil ((double) srcLength / (fromRate / toRate));
}

void resampleInputRange (juce::int64 srcLength, double fromRate, double toRate,
                         juce::int64 outStart, int outCount, juce::int64& first, juce::int64& last)
{
    const double ratio = fromRate / toRate;
    const double halfWidth = kZeroCrossings / juce::jmin (1.0, 1.0 / ratio);
    first = juce::jmax ((juce::int64) 0, (juce::int64) std::ceil ((double) outStart * ratio - halfWidth));
    last  = juce::jmin (srcLength - 1, (juce::int64) std::floor ((double) (outStart + juce::jmax (0, outCount - 1)) * ratio + halfWidth));
}

void resampleRange (const float* in, juce::int64 inStart, int inCount, juce::int64 srcLength,
                    double fromRate, double toRate, juce::int64 outStart, int outCount, float* out)
{
    const auto& table = kernelTable();
    const double ratio = fromRate / toRate;              // amostras de entrada por saida
    const double fc = juce::jmin (1.0, 1.0 / ratio);     // corte relativo a Nyquist da entrada
    const double halfWidth = kZeroCrossings / fc;        // em amostras de entrada
    const double gain = kCutoff * fc;
    const double tableScale = (double) (kTableSize - 1) / halfWidth;
    const juce::int64 inEnd = inStart + inCount; // exclusivo

    for (int j = 0; j < outCount; ++j)
    {
        const double t = (double) (outStart + j) * ratio;
        const juce::int64 k0 = juce::jmax ((juce::int64) 0, (juce::int64) std::ceil (t - halfWidth));
        const juce::int64 k1 = juce::jmin (srcLength - 1, (juce::int64) std::floor (t + halfWidth));
        double acc = 0.0;
        for (juce::int64 k = juce::jmax (k0, inStart); k <= k1 && k < inEnd; ++k)
        {
            const double pos = std::abs (t - (double) k) * tableScale;
            const int idx = (int) pos;
            if (idx >= kTableSize - 1) continue;
            const float frac = (float) (pos - idx);
            const float kv = table[(size_t) idx] + frac * (table[(size_t) idx + 1] - table[(size_t) idx]);
            acc += (double) in[k - inStart] * kv;
        }
        out[j] = (float) (acc * gain);
    }
}
} // namespace sc
