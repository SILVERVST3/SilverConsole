# SilverConsole 0.2

Programa de **mixagem** para Windows, com **hospedagem de plugins VST3**. Você importa os stems (cada arquivo vira uma faixa), coloca plugins nos inserts, cria buses auxiliares e exporta a mix em WAV. Feito do zero em C++17 / JUCE 8.0.6, com o visual SILVER (grafite e alumínio escovado).

Não é cópia de nenhum programa comercial e não tem afiliação com marcas de terceiros.

## Baixar o programa compilado

1. Abra **Actions → Build SilverConsole** no repositório.
2. Clique na execução mais recente com o símbolo verde.
3. Em **Artifacts**, baixe **SilverConsole-Windows-x64**.
4. Extraia o .zip e abra `SilverConsole/SilverConsole.exe`. Não precisa instalar nada; o runtime do Visual C++ já vai embutido.
5. Para testar, abra `SilverConsole/Demo/Demo.sconsole`: são 6 faixas sintetizadas, com buses de Bateria, Reverb e Delay.

O workflow compila o programa e roda os testes automáticos. Os testes cobrem a soma, o pan, mute/solo, buses, envios, a compensação de latência, o loop, a reamostragem, salvar/abrir projetos e o carregamento de um **plugin VST3 de verdade** (`SC Test Delay`, criado só para o teste).

## Recursos

**Faixas**
- Até 128 faixas de áudio mono ou estéreo: WAV, AIFF, FLAC, OGG e MP3.
- Importação múltipla, ou arraste os arquivos para a janela.
- Arquivos em outra taxa de amostragem são convertidos automaticamente, com sinc janelado de alta qualidade e sem deslocar o tempo.
- Arquivos grandes (32 MB ou mais) tocam **direto do disco** (streaming), inclusive em outra taxa de amostragem: a conversão é feita trecho a trecho e o arquivo não vai inteiro para a RAM.

**Canais (estilo console)**
- Inserts VST3 em todos os canais (faixas, buses e master): 4 por padrão; o botão **+** ao lado de INSERTS adiciona mais (até 64). Botão direito no último slot vazio: remover.
- Fader de -∞ a +12 dB e pan (potência constante em faixas mono, balance em estéreo).
- Mute, solo, inversão de polaridade (Ø) e medidor de pico L/R com indicador de clip.
- Nome editável (duplo clique) e cor da faixa.
- **Envios por faixa** (4 por padrão, botão **+** ao lado de ENVIOS para mais; o menu do último envio remove), pré ou pós-fader, para buses auxiliares (reverb, delay, paralelo).
- **Saída da faixa** para o Master ou para um bus (grupo, por exemplo um bus de bateria).
- Até 16 buses auxiliares, cada um com inserts, fader, pan e mute.
- **Master** com inserts, fader, botão **MONO** para conferir a mix e indicador de clip.

**Plugins VST3**
- Escaneamento da pasta padrão `C:\Program Files\Common Files\VST3` (ou de outras pastas). A lista fica salva; use **Plugins → Opções / Escanear...** para procurar plugins novos.
- Se um plugin travar durante o escaneamento, ele é pulado na próxima vez.
- Clique num slot vazio para escolher um plugin no menu, agrupado por fabricante.
- Clique num slot ocupado para abrir o editor do plugin. **Shift+clique** liga o bypass. O botão direito abre o menu: bypass, substituir, mover para cima/baixo e remover.
- **Janela do plugin**: alguns plugins aplicam a escala do Windows duas vezes e abriam gigantes (BSA Drum Bus num monitor 4K a 250% abria com 625%). A janela detecta isso e corrige sozinha; plugins redimensionáveis que ainda não cabem são reduzidos para caber no monitor (com bordas para redimensionar). Plugins não redimensionáveis maiores que a tela ficam com barras de rolagem. No menu do insert, **Escala da janela** permite escolher por plugin: *Automática* (padrão), *Sempre corrigir escala dupla*, *Escala do monitor, sem correção* ou *Modo compatível do Windows (sem DPI)*. A escolha fica salva por plugin.
- **Compensação automática de latência (PDC)** em faixas, buses e master: plugins com lookahead (limitadores, EQs de fase linear) não desalinham a mix.
- O estado de todos os plugins é salvo no projeto. Se um plugin não estiver instalado ao abrir, o slot fica **vermelho** e o estado é preservado; ao salvar, nada se perde.
- **Plugin offline**: no menu do insert (botão direito), **Colocar offline** descarrega o plugin (não usa CPU nem memória) e guarda o ajuste; o slot fica apagado com **OFF**. **Colocar online** recarrega com o mesmo ajuste. O menu do nome da faixa/bus tem **Todos os plugins offline/online**. Fica salvo no projeto (o plugin nem é carregado ao abrir).
- **Congelar faixa** (menu do nome da faixa): grava a faixa com os plugins (inversão de fase, inserts, automação dos plugins e a cauda de reverb/delay) num WAV ao lado do projeto (pasta "<projeto> - congeladas") e descarrega os plugins. A faixa mostra **CONGELADA** e toca o arquivo, lido do disco. Volume, pan, envios, mute e a automação de volume/pan continuam funcionando. **Descongelar** recarrega os plugins com o ajuste e manda o arquivo congelado para a lixeira.

**Automação**
- Volume, pan, mute e **qualquer parâmetro dos plugins VST3** podem ser automatizados em faixas, buses e master.
- Botão **AUTO** em cada canal, com cinco modos:
  - **OFF**: ignora a automação.
  - **READ**: toca a automação; os faders, o pan e o mute se mexem sozinhos.
  - **TOUCH**: grava enquanto você segura o controle; ao soltar, volta suavemente para a automação que já existia.
  - **LATCH**: grava do toque até você parar o transporte.
  - **WRITE**: grava tudo do canal enquanto toca (volume, pan, mute e os parâmetros de plugin que já têm linha).
- Para gravar parâmetros de plugin, ponha o canal em TOUCH ou LATCH, dê play e mexa no botão dentro da janela do plugin.
- **Editor de automação** (botão **Automação** ou tecla **Tab**): uma linha por parâmetro, com a forma de onda da faixa atrás do volume.
  - Clique para criar um ponto e arraste para mover; o valor aparece enquanto você arrasta.
  - **Alt+clique** ou duplo clique apaga um ponto; **Ctrl+arrastar** desenha à mão livre (lápis).
  - O botão direito abre opções: apagar ponto, limpar a linha ou remover a linha.
  - **+** em cada canal mostra as linhas de pan, mute ou de qualquer parâmetro dos plugins dos inserts.
  - **Ctrl+roda** dá zoom, **Shift+roda** rola no tempo e **Tudo** mostra a música inteira. Clicar na régua move o cursor.
  - **Modo de todas** troca o modo de todos os canais de uma vez.
- A automação fica salva no projeto e entra na exportação da mix. Ao mover um plugin de slot, a automação dele vai junto; ao remover ou trocar o plugin, as linhas dele são apagadas.

**Transporte e projeto**
- Tocar/pausar, parar, voltar ao início e régua clicável.
- **Seleção de trecho**: arraste na régua do tempo (no topo ou na régua do editor de automação) para selecionar um trecho; arraste as bordas para ajustar. **Shift+Espaço** ou **duplo clique** na seleção toca só o trecho e para no fim (volta ao início do trecho). Com o **loop** (L) ligado, o trecho repete. **Esc** limpa a seleção. O botão direito na régua abre o menu do trecho: tocar, loop, exportar a mix do trecho, exportar as faixas do trecho, selecionar a música toda, selecionar do início até o cursor e limpar. A seleção fica salva no projeto.
- Projetos `.sconsole` em XML. O projeto guarda o caminho dos áudios, relativo à pasta do projeto, e não copia os arquivos.
- **Exportar mix** para WAV de 16, 24 ou 32 bits float, renderizada offline (mais rápido que tempo real e sem falhas). Inclui 3 s de cauda e já descontada a latência dos plugins. Avisa se a mix passou de 0 dBFS. Em 16 bits aplica **dither TPDF**.
- **Exportar a mix só do trecho selecionado**: com um trecho selecionado, o botão **Exportar** (e o menu da régua) oferece a mix só da seleção.
- **Exportar faixas individuais (stems)**, da música inteira ou só da seleção. Escolha as faixas, o formato e o conteúdo:
  - **Com processamento**: cada faixa sai como soa na mix (inserts, volume, pan, automação, compensação de latência), sem buses e sem master. Solo é ignorado; faixas em mute saem em silêncio. Opcional: 3 s de cauda e os **buses** (cada bus com tudo o que chega nele). É uma passada só de render, em vários núcleos.
  - **Áudio original**: o trecho do arquivo de cada faixa, sem processamento (mono continua mono).
  - Todos os arquivos começam no mesmo ponto (alinhados) e vão para uma **pasta nova** (nunca sobrescreve arquivos), com nomes numerados na ordem das faixas. Avisa quais faixas passaram de 0 dBFS.
- **Projetos recentes**: o botão **Abrir** lista os últimos 10 projetos. O tamanho e a posição da janela também são lembrados.

## Escaneamento isolado de VST3 (Etapa 4 - experimental)

- No menu **Plugins**, escolha **Escanear VST3 com processo isolado**. A descoberta de cada arquivo VST3 ocorre em `ScPluginScanWorker.exe`, um executavel auxiliar distribuído ao lado de `SilverConsole.exe`.
- O SilverConsole pesquisa os caminhos VST3 padrao e inicia **um processo filho por candidato**. Se um plugin falhar ou nao responder em 20 segundos, a varredura continua nos proximos plugins sem executar o codigo desse candidato no processo principal.
- O resultado e um XML atomico com as descricoes dos plugins; o mixer importa apenas metadados validos. Um teste no GitHub Actions verifica o scanner isolado com o VST3 de teste do projeto.
- Os candidatos com crash, falha de leitura ou timeout sao colocados numa **lista persistente de quarentena**. O scanner normal pula esses arquivos nas verificacoes seguintes; **Plugins > Reavaliar VST3 com falha anterior...** testa **apenas os candidatos nessa lista**, sem repetir toda a varredura. Plugins recuperados saem da quarentena automaticamente.
- O botao **Cancelar** interrompe tambem o processo filho que estiver em andamento, sem precisar aguardar os 20 segundos. A supervisao tem testes automaticos de timeout real, codigo de erro do processo e cancelamento durante a execucao. Resultados XML acima de 4 MiB, corrompidos ou com formato/schema invalido nao sao importados.
- **Teste opcional de DSP em outro processo:** o comando **Plugins > Testar DSP dos VST3 em processo isolado...** abre cada candidato no `ScPluginScanWorker.exe`, instancia o plugin a 48 kHz/256 amostras e executa oito blocos silenciosos. Códigos de erro, amostras não finitas ou travamentos deixam o candidato em quarentena. O teste de integração usa o VST3 do próprio projeto.
- Esse teste é apenas uma verificação de inicialização e resposta. Não mede latência nem estabilidade prolongada, não testa parâmetros/automação em tempo real e **não move o DSP da mixagem para outro processo**. Plugins que exijam licença, entrada MIDI ou configuração específica podem falhar nesse teste sem estarem defeituosos.
- O gerenciador legado JUCE ainda oferece uma opcao de scan interno (sem isolamento), sinalizada como tal. Prefira o scan isolado.
- **Tratamento parcial de falhas DSP:** se o `processBlock` de um insert lancar uma **excecao C++**, a engine silencia o bloco possivelmente corrompido, coloca esse insert em bypass automaticamente e exibe a ocorrencia no contador `PLUG ERR`. Os proximos blocos podem seguir sem o insert problemático. Existe teste de regressao com plugin simulado que lança exceção e escreve NaN.
- **Esse bypass nao substitui isolamento de processo:** erros fatais nativos (access violation/SEH), travamentos, deadlocks e loops infinitos ainda podem bloquear ou derrubar o host. O tratamento nao garante recuperacao de todo plugin.\n
- **Limite de seguranca:** carregar/usar plugins em um insert ainda ocorre no processo principal. Isolamento de *processamento* VST3 e recuperacao de falhas em tempo real requerem uma engine separada e ainda nao foram implementados.

## Atalhos

| Tecla | Ação |
|---|---|
| Espaço | Tocar / pausar |
| Shift+Espaço | Tocar só o trecho selecionado |
| Esc | Limpar a seleção de trecho |
| Home | Voltar ao início |
| L | Liga/desliga o loop (repete a seleção) |
| Ctrl+N / Ctrl+O / Ctrl+S | Novo / Abrir / Salvar |
| Ctrl+Shift+S | Salvar como |
| Ctrl+I | Importar áudio |
| Ctrl+E | Exportar (mix, trecho ou faixas individuais) |
| Tab | Alterna entre mixer e editor de automação |
| Duplo clique no fader/pan | 0 dB / centro |
| Ctrl+arrastar o fader | Ajuste fino |
| Duplo clique no valor em dB | Digitar o volume |

## Recuperacao de projetos (Etapa 3 - experimental)

- Quando o projeto tem alteracoes e o transporte esta parado, o SilverConsole cria um snapshot de recuperacao a cada aproximadamente **2 minutos**.
- Os snapshots ficam em `%APPDATA%\\SilverConsole\\Recovery` no Windows, separados do arquivo .sconsole normal. O arquivo principal nunca e substituido pelo autosave.
- Na proxima abertura do programa, se houver um snapshot, o usuario pode optar por recuperar o mais recente. A recuperacao abre como projeto ainda nao salvo; use **Salvar como** para guardar a versao definitiva.
- Apos salvar normalmente com sucesso, os snapshots correspondentes sao eliminados. Os arquivos de audio originais continuam externos ao projeto e precisam estar acessiveis.
- A captura e feita com audio parado para nao bloquear o callback durante o salvamento de estados VST3. Enquanto o transporte estiver tocando, nao ha novo snapshot.

## Medidores Master (Etapa 3 - em validacao)

- O MASTER mostra **TP dBTP**, uma estimativa de true peak por sobreamostragem FIR **4x** com historico entre callbacks; ela detecta picos que ocorrem entre amostras de audio.
- O painel de saida do MASTER mostra **Momentary LUFS (M)** em uma janela de aproximadamente **400 ms**, usando ponderacao K (high-shelf + RLB high-pass) e atualizacao em sub-janelas de 100 ms.
- Ambos sao somente medidores; nao alteram o sinal nem aplicam limitacao. Os testes verificam pico entre amostras, continuidade de estado, sonoridade em 48/96 kHz, silencio e reducao de 20 dB.
- **Limite:** a leitura LUFS-M ainda nao implementa medicao integrada com gate EBU R128, loudness range, short-term (3s) ou certificacao normativa. A leitura true peak FIR 4x e aproximada e exige comparacao com medidores de referencia.

## ASIO opcional (Windows, em validacao)

O JUCE 8.0.6 do projeto utiliza um SDK ASIO externo. A compilacao padrao continua com WASAPI e **sem ASIO**. Para habilitar ASIO localmente, instale um SDK ASIO cuja licenca permita o uso pretendido e configure:

```powershell
cmake -S . -B build-asio -G "Visual Studio 17 2022" -A x64 `
  -DSILVERCONSOLE_ENABLE_ASIO=ON `
  -DSILVERCONSOLE_ASIO_SDK_DIR="C:/caminho/para/ASIO_SDK"
cmake --build build-asio --config Release --target SilverConsole
```

O diretório informado deve conter `common/iasiodrv.h` e `host/asiodrivers.h`. Esse caminho ainda exige compilacao e testes com drivers ASIO reais; nao esta habilitado no artefato Windows do GitHub Actions.

## Placa de som

Em **Áudio → Placa de som**, escolha o dispositivo e o tamanho do buffer. No Windows, use **Windows Audio (Exclusive Mode)** para ter latência baixa com a sua interface. O modo compartilhado também funciona.

**Desempenho em sessões grandes**
- As faixas (leitura, inserts e compensação de latência) e depois os **buses** (inserts de reverb/delay etc.) são processados em **vários núcleos** ao mesmo tempo. Dá para desligar em **Áudio → Processar faixas em vários núcleos**. A soma das faixas, a soma dos buses e o master continuam em sequência, na ordem original (resultado idêntico ao de um núcleo).
- O que roda fora da thread de áudio: leitura do disco (2 threads de E/S), escaneamento de plugins (processo separado), plugins isolados (processo DSP separado, opcional por insert), importação/reamostragem e exportação (thread de trabalho) e a gravação da cópia de recuperação (thread separada, a interface não trava em projetos grandes).
- A exportação usa os mesmos núcleos do playback (até 8 workers), já que o áudio fica suspenso durante o render.
- **Economizar CPU** (Áudio → Economizar CPU, ligado por padrão): uma faixa ou bus com a entrada em silêncio, cuja saída dos plugins ficou abaixo de -100 dBFS pelo tempo da cauda declarada pelo plugin (no mínimo 3 s), para de chamar os plugins até o som voltar. O som volta no mesmo bloco, sem diferença no resultado. Com o transporte parado, praticamente todos os plugins dormem. O canal mostra **zz** ao lado de INSERTS e a barra de status mostra, ao lado do CPU, quantos canais estão **dormindo**. Plugins que declaram cauda "infinita" (comum em reverbs e delays) esperam 10 s de silêncio real na saída; enquanto produzirem som (feedback, freeze) não dormem. Inserts isolados (processo DSP) nunca dormem. A exportação sempre processa tudo.
- **Prioridade no Windows**: o SilverConsole desliga o modo de economia de energia do Windows 11 (que joga programas para os núcleos de eficiência e baixa o clock quando a janela não está em foco), pede timer de 1 ms e roda com prioridade "acima do normal". As threads que processam áudio usam a prioridade "Pro Audio" do Windows.
- A barra de status mostra os contadores que explicam estalos:
  - **XRUN**: o processamento passou do tempo do buffer (CPU). Aumente o buffer ou alivie os plugins.
  - **DISCO**: o áudio do disco não chegou a tempo (disco lento ou muitas faixas grandes).
  - **LOCK**: o áudio pulou um bloco porque a interface estava editando algo no mesmo instante.
  - **Áudio → Zerar contadores** zera os três para um novo teste.

## Limites desta versão

- **VST3 apenas.** VST2 não é suportado, porque o SDK do VST2 foi descontinuado pela Steinberg. Quase todos os plugins atuais têm versão VST3.
- **ASIO não está incluído.** Use WASAPI exclusivo. Dá para adicionar ASIO depois.
- O programa só mixa: não grava áudio, não tem MIDI/instrumentos, edição de clipes nem undo (inclusive nas edições de automação).
- A automação é aplicada a cada bloco de áudio (alguns milissegundos), com suavização no volume e no pan. Não tem precisão de amostra.
- **Streaming em disco (experimental):** arquivos com 32 MiB ou mais tocam do disco com cache limitado por faixa e duas threads de pré-leitura; arquivos em outra taxa são convertidos por trechos (sinc, idêntico à conversão offline), sem carregar o arquivo inteiro na RAM. Arquivos menores ficam na RAM. A exportação offline lê os blocos necessários antes de renderizar.
- Na reprodução em streaming, saltos bruscos de cursor e discos lentos podem produzir silêncios momentâneos se o cache não estiver pronto. O streaming ainda precisa de testes prolongados; não é considerado pronto para produção.
- **Multicore:** faixas e buses usam vários núcleos no playback e na exportação. A cadeia de inserts de um mesmo canal continua em um núcleo (é sequencial por natureza): um canal com muitos plugins pesados ainda pode exigir buffer maior.
- O escaneamento isolado e experimental e o scanner tradicional JUCE continua disponivel. Plugins em inserts continuam hospedados no processo principal, sem isolamento do DSP.

## Compilar no PC (opcional)

Instale Git, CMake 3.22+ e Visual Studio 2022 com **Desenvolvimento para desktop com C++**. Depois:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-windows.ps1
```

O .exe fica em `build/SilverConsole_artefacts/Release/SilverConsole.exe`.

## Estrutura do código

- `Source/Engine.*`: motor de mixagem em tempo real (faixas, inserts, envios, buses, master, PDC, transporte, exportação offline, economia de CPU, congelar).
- `Source/RealtimeThread.*`: prioridade de áudio no Windows (MMCSS, sem economia de energia).
- `Source/AudioLoader.*`: leitura de arquivos e reamostragem.
- `Source/StreamingAudio.*`: cache por janela, pré-leitura assíncrona e visão resumida de forma de onda.
- `Source/Project.*`: formato `.sconsole`.
- `Source/Automation.h`, `Source/AutomationRecorder.*`, `Source/AutomationView.*`: linhas de automação, gravação TOUCH/LATCH/WRITE e editor.
- `Source/ChannelStrip.*`, `Source/MainComponent.*`, `Source/SilverLookAndFeel.h`: interface.
- `Source/PluginHost.h`, `Source/PluginWindow.h`: lista de plugins e janelas de editor.
- `Tests/`: testes do motor e o plugin VST3 de teste.

## Licença

O código deste projeto é MIT (veja `LICENSE`). Ele depende do JUCE, que tem licença própria (AGPLv3 ou comercial). Veja `THIRD_PARTY.md` antes de distribuir o programa.
