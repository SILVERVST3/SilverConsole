// SilverConsole - prioridade de audio no Windows. Codigo original (MIT).
#pragma once

namespace sc::rt
{
/** Processo inteiro: desliga o "EcoQoS"/power throttling do Windows 11 (que
    joga um app em segundo plano nos nucleos de eficiencia e baixa o clock),
    pede resolucao de timer de 1 ms e prioridade "acima do normal".
    Chamar uma vez no inicio (app e worker DSP). */
void configureProcessForAudio() noexcept;

/** Thread atual: nunca rodar em modo de economia (nucleos E / clock baixo). */
void disableThreadThrottling() noexcept;

/** Thread atual: MMCSS "Pro Audio", prioridade de tempo critico e sem
    economia de energia. Para threads que processam audio a cada buffer. */
void promoteAudioThread() noexcept;
} // namespace sc::rt
