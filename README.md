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

**Canais (estilo console)**
- 8 slots de insert VST3 por canal, inclusive buses e master.
- Fader de -∞ a +12 dB e pan (potência constante em faixas mono, balance em estéreo).
- Mute, solo, inversão de polaridade (Ø) e medidor de pico L/R com indicador de clip.
- Nome editável (duplo clique) e cor da faixa.
- **4 envios por faixa**, pré ou pós-fader, para buses auxiliares (reverb, delay, paralelo).
- **Saída da faixa** para o Master ou para um bus (grupo, por exemplo um bus de bateria).
- Até 8 buses auxiliares, cada um com 8 inserts, fader, pan e mute.
- **Master** com 8 inserts, fader, botão **MONO** para conferir a mix e indicador de clip.

**Plugins VST3**
- Escaneamento da pasta padrão `C:\Program Files\Common Files\VST3` (ou de outras pastas). A lista fica salva; use **Plugins → Opções / Escanear...** para procurar plugins novos.
- Se um plugin travar durante o escaneamento, ele é pulado na próxima vez.
- Clique num slot vazio para escolher um plugin no menu, agrupado por fabricante.
- Clique num slot ocupado para abrir o editor do plugin. **Shift+clique** liga o bypass. O botão direito abre o menu: bypass, substituir, mover para cima/baixo e remover.
- **Compensação automática de latência (PDC)** em faixas, buses e master: plugins com lookahead (limitadores, EQs de fase linear) não desalinham a mix.
- O estado de todos os plugins é salvo no projeto. Se um plugin não estiver instalado ao abrir, o slot fica **vermelho** e o estado é preservado; ao salvar, nada se perde.

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
- Tocar/pausar, parar, voltar ao início, régua clicável e **loop**: Shift+arrastar na régua define a região.
- Projetos `.sconsole` em XML. O projeto guarda o caminho dos áudios, relativo à pasta do projeto, e não copia os arquivos.
- **Exportar mix** para WAV de 16, 24 ou 32 bits float, renderizada offline (mais rápido que tempo real e sem falhas). Inclui 3 s de cauda e já descontada a latência dos plugins. Avisa se a mix passou de 0 dBFS.

## Atalhos

| Tecla | Ação |
|---|---|
| Espaço | Tocar / pausar |
| Home | Voltar ao início |
| L | Liga/desliga o loop |
| Ctrl+N / Ctrl+O / Ctrl+S | Novo / Abrir / Salvar |
| Ctrl+Shift+S | Salvar como |
| Ctrl+I | Importar áudio |
| Ctrl+E | Exportar mix |
| Tab | Alterna entre mixer e editor de automação |
| Duplo clique no fader/pan | 0 dB / centro |
| Ctrl+arrastar o fader | Ajuste fino |
| Duplo clique no valor em dB | Digitar o volume |

## Placa de som

Em **Áudio**, escolha o dispositivo e o tamanho do buffer. No Windows, use **Windows Audio (Exclusive Mode)** para ter latência baixa com a sua interface. O modo compartilhado também funciona.

## Limites desta versão

- **VST3 apenas.** VST2 não é suportado, porque o SDK do VST2 foi descontinuado pela Steinberg. Quase todos os plugins atuais têm versão VST3.
- **ASIO não está incluído.** Use WASAPI exclusivo. Dá para adicionar ASIO depois.
- O programa só mixa: não grava áudio, não tem MIDI/instrumentos, edição de clipes nem undo (inclusive nas edições de automação).
- A automação é aplicada a cada bloco de áudio (alguns milissegundos), com suavização no volume e no pan. Não tem precisão de amostra.
- **Streaming em disco experimental (Etapa 2):** arquivos com 32 MiB ou mais, na mesma taxa do motor, usam cache limitado de 65.536 amostras por faixa e uma thread de pré-leitura compartilhada; a forma de onda usa um resumo compacto. Arquivos menores e/ou em outra taxa continuam na RAM com reamostragem sinc. Se a taxa do dispositivo mudar, o fallback atual pode carregar o arquivo completo na RAM. A exportação offline lê os blocos necessários antes de renderizar.
- Na reprodução em streaming, saltos bruscos de cursor e discos lentos podem produzir silêncios momentâneos se o cache não estiver pronto. O streaming ainda precisa de testes prolongados; não é considerado pronto para produção.
- O processamento roda em um núcleo da CPU. Sessões muito pesadas podem precisar de um buffer maior.
- O escaneamento de plugins roda dentro do programa. Se um plugin derrubar o programa no scan, abra de novo e escaneie outra vez; o plugin problemático é pulado.

## Compilar no PC (opcional)

Instale Git, CMake 3.22+ e Visual Studio 2022 com **Desenvolvimento para desktop com C++**. Depois:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-windows.ps1
```

O .exe fica em `build/SilverConsole_artefacts/Release/SilverConsole.exe`.

## Estrutura do código

- `Source/Engine.*`: motor de mixagem em tempo real (faixas, inserts, envios, buses, master, PDC, transporte, exportação offline).
- `Source/AudioLoader.*`: leitura de arquivos e reamostragem.
- `Source/StreamingAudio.*`: cache por janela, pré-leitura assíncrona e visão resumida de forma de onda.
- `Source/Project.*`: formato `.sconsole`.
- `Source/Automation.h`, `Source/AutomationRecorder.*`, `Source/AutomationView.*`: linhas de automação, gravação TOUCH/LATCH/WRITE e editor.
- `Source/ChannelStrip.*`, `Source/MainComponent.*`, `Source/SilverLookAndFeel.h`: interface.
- `Source/PluginHost.h`, `Source/PluginWindow.h`: lista de plugins e janelas de editor.
- `Tests/`: testes do motor e o plugin VST3 de teste.

## Licença

O código deste projeto é MIT (veja `LICENSE`). Ele depende do JUCE, que tem licença própria (AGPLv3 ou comercial). Veja `THIRD_PARTY.md` antes de distribuir o programa.
