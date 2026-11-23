# DSP IPC: implementação experimental (Etapa 4)

Esta branch introduz uma **opcao real, mas experimental, de processar VST3 no executavel separado ScDspWorker.exe**, usando memoria compartilhada (Windows x64). O caminho padrao de plugins continua no proprio SilverConsole, para preservar sua mix atual. O recurso ainda **nao esta liberado para producao**.

**Arquitetura implementada:** o controle inicia um processo por insert remoto, grava descritor e estado inicial em XML temporario, cria um mapeamento de memoria unico e aguarda handshake (ate 6 s). O worker instancia o VST3, prepara 48/96 kHz conforme dispositivo, processa blocos estereo em ring com 4 slots; o audio callback so copia, publica/consome resultados e nunca aguarda pelo worker. O monitor DspIpcWatchdog contabiliza sequencias e aciona o bypass seco apos 3 buffers sem resposta. A PDC inclui `1 buffer de transporte + latencia reportada pelo plugin`. O sinal seco de fallback sempre sofre esse mesmo atraso, para evitar saltos de alinhamento durante falhas. A entrada no modo isolado e **opt-in** no menu de inserts e fica registrada no .sconsole via atributo `isolated=true`.

O worker não suporta editor remoto nem automacao de parametros do plugin nesta versao. Se o dispositivo mudar de taxa ou buffer, o insert entra em fallback seco ate reiniciar o worker manualmente. O controle do processo auxiliar, o mapeamento de memoria, a criacao/encerramento de processos e o handshake nunca acontecem no callback de audio.

## Limites e criterios antes de chamar de concluido

- Na falta do resultado esperado, o callback utiliza o dry atrasado; respostas obsoletas nao sao entregues e buffers do worker em processamento nao sao reciclados pelo host.
- O orçamento por buffer é efetivado pelo deadline no callback seguinte (um buffer de latência); ao exceder o limite de buffers perdidos, o insert fica em bypass de segurança até reiniciar manualmente.
- A memoria e vinculada ao processo local do Windows, com layout e versao fixos; format/channel/frame counts sao validados no worker e no host.
- Testes de regressao devem incluir handshake, impulso com plugin de 64 samples de latência, PDC, falha/kill do worker e somatorio sem NaNs.
- Antes de habilitar em producao: controle de parametros/automacao do VST3 atraves IPC, estado atualizado para salvamento, modos multi-bus/MIDI, integracao robusta com exportacao offline, afinidade/scheduling realtime, soak tests e multiplas interfaces ASIO/WASAPI.
- O novo scanner isolado e a interface GUI de plugins do modo padrao continuam os mesmos.

O caminho DSP em processo ainda nao e adequado para plugins que dependem de mais de dois canais, instrumentos MIDI ou GUIs de edicao. Não faça merge na main até Windows CI + testes de áudio reais validarem a opção.

## Revisao de 22/11/2026 (branch `claude/phase4-review`)

Correcoes feitas apos auditoria do caminho IPC:

- **Prazo por buffer em tempo real.** O worker esperava novos blocos com `Sleep(1)`. No Windows essa espera pode durar ate ~15,6 ms (resolucao padrao do timer), mais que um buffer de 256 amostras a 48 kHz (5,3 ms): o watchdog via 3 respostas atrasadas seguidas e travava o bypass seco. Agora o host cria um evento Win32 auto-reset (`<mapeamento>_wake`) e chama `SetEvent` (nao bloqueante) a cada bloco publicado; o worker espera nesse evento (`WaitForSingleObject`, prazo de 2 ms) e roda com `THREAD_PRIORITY_TIME_CRITICAL`. Sem o evento, volta ao polling antigo. O teste antigo nao pegava o problema porque dormia 12 ms entre blocos; o novo teste chama o insert no ritmo real (um bloco a cada 5,33 ms, 400 blocos) e exige >= 90% dos blocos processados.
- **Exportacao offline com insert isolado.** O bounce roda mais rapido que o tempo real e o insert remoto nao esperava o worker: a mix exportada saia com trechos secos (sem o plugin) e podia travar o bypass. Agora o `Engine::exportMix` coloca cada insert remoto em modo offline (`RemoteDspInsert::setOffline`): o bloco espera o worker (prazo de 5 s, fora do callback), respostas e audio seco antigos sao descartados ao entrar e sair do modo, e o bloco final e sempre renderizado no tamanho completo (blocos menores iam pelo caminho seco). Se um insert remoto ja estiver em falha, a exportacao e recusada com aviso; se falhar durante o bounce, a exportacao e interrompida em vez de gravar uma mix errada. Teste: impulso com o plugin de teste em -6 dB sai processado e alinhado no WAV; worker morto bloqueia a exportacao.
- **Worker orfao.** O host passa o proprio PID; o worker encerra se o SilverConsole fechar de forma anormal (antes ficava um `ScDspWorker.exe` rodando).
- **Plugin de saida mono no worker.** O canal direito recebe o mesmo sinal (antes repetia a entrada).

Continua pendente: reinicio automatico do worker, parametros/automacao e estado atualizado pelo IPC, editor remoto, mais de 2 canais/MIDI, reset do estado interno do plugin remoto antes do bounce (caudas de reverb do playback podem entrar no inicio da exportacao), testes longos com plugins comerciais e interfaces reais.

## Desempenho e mixer (23/11/2026, mesma branch)

- **Multicore em tempo real:** `RealtimeTrackPool` processa as faixas (leitura, inserts, PDC) em ate 8 threads persistentes. Sem alocacao nem mutex no callback: o trabalho e publicado num contador atomico (geracao + indice), os workers acordam por semaforo Win32 e rodam com MMCSS "Pro Audio". A soma, os envios, os buses e o master continuam sequenciais e na mesma ordem: o resultado e identico ao de um nucleo (teste compara amostra por amostra). Medicao: 24 faixas pesadas a 512 amostras, 8,1 ms/bloco em um nucleo contra 1,3 ms/bloco em multicore (6x). `ScopedNoDenormals` no callback e nos workers.
- **Streaming com conversao de taxa:** arquivos de 32 MB ou mais tocam do disco tambem quando a taxa e diferente da placa. A conversao usa o mesmo sinc do reamostrador offline, por trechos (`resampleRange`), com resultado identico (erro 0 no teste). Antes, cada arquivo em outra taxa ia inteiro para a RAM duas vezes (original + convertido), cerca de 2,2 GB por faixa de 50 min. Janelas de cerca de 5,5 s, renovadas na metade; janela curta apos salto do cursor; 2 threads de I/O que atendem primeiro a faixa com menos audio a frente.
- **Contadores na barra de status:** XRUN (CPU), DISCO (streaming atrasado), LOCK (edicao concorrente), com opcao de zerar.
- **Mixer:** 16 buses; inserts e envios dinamicos (4 por padrao, botao "+", limite de seguranca 64), salvos no projeto (`inserts=`/`sends=`); janela de plugin com escala do host desligada (`getDesktopScaleFactor = 1`), limites de tamanho do proprio editor, rolagem quando maior que a tela e modo de compatibilidade de DPI por plugin.
