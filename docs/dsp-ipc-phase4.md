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
