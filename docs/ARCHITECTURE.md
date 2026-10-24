# Arquitetura

Firmware ESP-IDF v6.1 em C, sem LVGL: tudo é desenhado à mão. Código em `main/`.

## Módulos

| Arquivo | Responsabilidade |
|---|---|
| `main.c` | Boot: NVS, tela, toque, terminal, Wi-Fi, barra, teclado, SD, registra ferramentas, abre o menu, depois a shell |
| `display.c` | ILI9341 via `esp_lcd`. `display_draw()` (buffer DMA interno) e `display_present()` (framebuffer em PSRAM, enviado em faixas). Um mutex serializa o acesso ao painel |
| `touch.c` | FT6336 por I2C, converte para coordenadas de tela; tarefa de gestos que gera eventos |
| `term.c` | Terminal: grade de células, parser ANSI/VT100 básico, UTF-8, rolagem (300 linhas em PSRAM), cabeçalho, tarefa de renderização. A área de texto encolhe quando o teclado na tela aparece (`term_reserve_bottom`) |
| `ui_draw.c` | Desenho compartilhado: texto, ícones, formas com anti-serrilhado, ícones de status, "◀ MENU", ícone de teclado |
| `osk.c` | Teclado na tela (3 camadas) |
| `fonts.h` | Fontes Spleen 5×8, 6×12, 8×16, 12×24 (gerado por `tools/gen_fonts.py`) |
| `input.c` | Fila única de **eventos**: teclas (teclado BLE, serial, teclado na tela) e toque (tocar, deslizar, voltar, botão de teclado) |
| `keyboard.c` + `esp_hid_gap.c` | Host BLE HID: procura, pareia ("Just Works"), reconecta, converte relatórios HID em teclas (layout US), repetição de tecla |
| `wifi.c` | Estação Wi-Fi, credenciais salvas pelo driver, reconexão |
| `statusbar.c` | Bateria (ADC, média, detecção de carregador) e relógio (SNTP, horário de Brasília) |
| `badusb.c` + `badusb_hid.c` | Bad USB: teclado USB (TinyUSB) e interpretador de DuckyScript |
| `wifitools.c` | Wi-Fi: tela de conexão (scan, lista, senha na tela) e o detector de deauth (modo monitor) |
| `settings.c` | Tela de Ajustes: brilho, volume, auto-off, fonte (lista tocável, tudo salvo na NVS) |
| `power.c` | Desligar (deep sleep), acordar por toque ou BOOT, desligamento automático por inatividade |
| `storage.c` | Monta o microSD em `/sd` |
| `shell.c` | Shell: registro de comandos por módulo, edição de linha, histórico, paginador, diretório atual |
| `tools_sys.c` / `tools_net.c` / `tools_fs.c` | Os comandos (ver README) |
| `sound.c` | Sons da interface no alto-falante: ES8311 (via `esp_codec_dev`) + I2S, síntese de tons com envelope, fila que descarta sons sobrepostos |
| `menu.c` | Itens do menu inicial e das categorias, roda as ferramentas |
| `carousel.c` | O carrossel animado (ver abaixo) |
| `icons.h` | Ícones como máscaras de transparência 96×96 (gerado por `tools/png2icons.py`) |

## Tela e renderização

- Paisagem 320×240.
- **Terminal:** as 16 linhas de cima são o **cabeçalho** (fundo preto, sem faixa nem linha): "◀ MENU", botão do teclado na tela, indicador de rolagem e os ícones de status. Redesenhado 1×/s ou quando muda. O corpo desenha só as linhas de células sujas, num buffer DMA interno.
- **Teclado na tela:** ocupa os 106 px de baixo; o terminal reserva esse espaço e rola o texto para o cursor continuar visível.
- **Home (carrossel):** ocupa a tela inteira. Pausa o terminal (`term_pause`) e esconde o cabeçalho (`term_hide_bar`). Usa três framebuffers RGB565 em PSRAM:
  - `bg` — o degradê de fundo, feito uma vez;
  - `base` — fundo + tiles laterais + rótulo + bolinhas + ícones de status (tudo menos o tile central);
  - `fb` — o que vai para a tela (`base` + tile central).
- Um mutex em `display_draw()` impede que a tarefa da barra e o carrossel mandem dados ao painel ao mesmo tempo.

## Carrossel

Inspirado no home do TentacleOS. Arquivo `main/carousel.c`.

- **Posição fracionária:** `pos` anda em direção ao item alvo (`pos += (alvo - pos) * 0.35` por quadro) → deslize suave.
- **Layout em perspectiva** para um item a `offset` posições do centro:
  - x = `160 + 118·tanh(0.85·offset)` — os de fora se acumulam nas bordas
  - tamanho = `56 + 46·e^(−offset²/1.1)`
  - brilho = `0.42 + 0.58·e^(−offset²/2.2)`
- **Coverflow:** os tiles laterais são desenhados "chapados" num buffer e deformados em trapézio (`warp_blit`): largura × 0,62, borda perto do centro com altura total e a de fora com 78%.
- **Tile central:** gradiente vertical tingido pela cor da categoria, borda e **glow** na cor de destaque, sombra, ícone branco.
- **Flutuação:** o tile central sobe e desce ±5 px num ciclo de 3,2 s (valores do TentacleOS: `HOME_FLOAT_AMP 5`, `HOME_FLOAT_MS 1600`).
- **Pop:** ao escolher, o tile cresce até 116% e volta antes de abrir.
- **Ícones:** máscaras alfa 96×96 amostradas com interpolação bilinear → bordas suaves em qualquer tamanho.
- **Ícones de status** (canto superior direito): Bluetooth, Wi-Fi em leque com 3 faixas acesas pelo sinal (RSSI > −60 / −72 dBm), bateria com nível colorido (verde/amarelo/vermelho) e raio quando carregando, porcentagem, hora. São formas preenchidas com anti-serrilhado 4×4.
- **Entrada:** setas/Enter/Esc, ou toque: deslizar > 30 px troca, tocar no centro abre, tocar nos lados anda um item, canto inferior esquerdo volta.

### Desempenho

Mandar a tela inteira pela SPI (40 MHz) leva ~30 ms, e compor tudo mais algumas dezenas. Por isso:

| Situação | O que é redesenhado |
|---|---|
| Deslizando | quadro inteiro (`frame_full`) |
| Parado, ícone flutuando | só um retângulo de 164×184 em volta do tile central, copiado de `base` (`frame_float` + `display_present_rect`) — e só quando o deslocamento muda 1 px |
| A cada 2 s parado | só reconstrói a camada estática em cache, depois um quadro |

**Camada estática em cache:** fundo, rótulo, bolinhas, botão e ícones de status mudam devagar, então são desenhados uma vez em `bg` (com anti-serrilhado caro) e só refeitos quando o item selecionado muda ou o status atualiza. Cada quadro do deslize só copia `bg` e desenha os tiles em movimento — cortou a composição de ~232 ms para ~120 ms. O deslize também usa ease-out em menos passos (0,55), ficando mais ágil. O clock SPI da tela fica em 40 MHz: 80 MHz trava o ILI9341 (boot loop).

### Paleta (do TentacleOS, `ui_theme.c`)

| Uso | Cor |
|---|---|
| Fundo | `#12001F` → `#000000` |
| Tile | `#0A0014` |
| Borda inativa | `#3A2A4A` |
| WiFi | `#BF00FF` |
| Bluetooth | `#D158F2` |
| NFC | `#CC00FF` |
| SubGHz | `#4CAF50` |
| RFID | `#FFC107` |
| Infrared | `#F44336` |
| LoRa | `#00BCD4` |

## Entrada

Tudo passa por uma fila de eventos (`input.c`), então cada tela funciona igual com caneta ou teclado.

| Evento | Origem |
|---|---|
| `EV_KEY` | um byte: teclado BLE, serial USB ou teclado na tela. Teclas especiais são sequências VT100 (`ESC [ A` = ↑, `ESC [ 5 ~` = PgUp, `ESC [ 1 ; 2 A` = Shift+↑) |
| `EV_TAP` | toque curto (movimento ≤ 12 px), com x, y |
| `EV_SWIPE_*` | arrastar ≥ 30 px; a direção é a do maior eixo |
| `EV_BACK` | toque em "◀ MENU" / botão ⏻ da home (x < 80, y < 44) |
| `EV_KEYBOARD` | toque no botão de teclado (x 84–124, y < 44) |

A tarefa de gestos (`touch.c`) lê o FT6336 a cada 15 ms. O comando `touchtest` marca na tela onde cada toque cai e mostra x/y, para conferir a precisão. Toques sobre o teclado na tela viram teclas ali mesmo e não chegam como `EV_TAP`.

Quem consome:

| Tela | Toque | Teclado |
|---|---|---|
| Carrossel | deslizar troca, tocar no centro abre, nos lados anda, ◀ volta | ← → Enter Esc |
| Prompt da shell | ◀ abre o menu, botão abre o teclado, deslizar rola | edição de linha, histórico |
| Comando rodando | tocar ou ◀ interrompe (`shell_interrupted`) | Ctrl+C, Esc, q |
| "-- mais --" | tocar continua, ◀ sai | Enter continua, q/Esc sai |
| Resultado de ferramenta | tocar ou ◀ volta; deslizar rola | qualquer tecla |
| Argumentos | teclado na tela abre sozinho; ◀ cancela | Enter confirma, Esc cancela |

A saída do terminal também é espelhada na serial USB, então o deck pode ser usado pelo computador.

## Energia

**Desligar** (`power_off`): mensagem e acorde descendente → espera o dedo sair da tela → Wi-Fi parado → codec fechado e amplificador desligado (GPIO1 alto, preso com `gpio_hold`) → toque em modo *monitor* (registrador 0xA5 = 1: baixo consumo, ainda puxa INT para baixo ao ser tocado) → backlight desligado e preso, painel em sleep → deep sleep.

**Acordar:** EXT1 com "qualquer pino em nível baixo" no GPIO0 (BOOT) e GPIO17 (INT do toque). Se a linha do toque estiver em nível baixo na hora de dormir, só o BOOT acorda, para não entrar num ciclo de liga/desliga. Acordar é um boot normal; `power_init` solta os pinos presos.

**Botão na home:** o canto superior esquerdo da home (onde as outras telas têm "◀ MENU") é o botão ⏻, um botão de 46×32 px com área de toque de 80×44 px (a borda de cima do painel é a menos precisa); toque nele ou Esc abre "Desligar o deck?" com **Cancelar** pré-selecionado, para um Enter ou toque acidental não desligar.

O tempo de auto-off é escolhido em **Ajustes** (nunca a 60 min), não é fixo no código.

**Automático:** qualquer evento de entrada ou saída no terminal conta como uso (`power_activity`). Uma tarefa verifica a cada 5 s; padrão 5 min, salvo na NVS (`autooff`).

Testado: depois de `desligar`, a USB some e não volta sozinha (não há ciclo de acordar).

## Bateria e carregamento

O chip carregador **não tem pino de status ligado ao ESP**, então o carregamento é deduzido:

1. **USB num computador:** `usb_serial_jtag_is_connected()`.
2. **Carregador de tomada:** a tarefa mede a bateria a cada 2 s (média de 16 leituras). Ao plugar, a corrente de carga faz a tensão **saltar** ≥ 60 mV em relação a 8–10 s antes; ao desplugar, ela cai o mesmo tanto.
3. Leitura ≥ 4,25 V: só alimentação externa chega lá.

Enquanto carrega, 100 mV são descontados para a porcentagem não pular para 100%. "Cheia" = carregando e ≥ 4,18 V. Plugar e desplugar tocam os sons de cartão SD do TentacleOS.

## Relógio

SNTP (`pool.ntp.org`, horário de Brasília). O primeiro pedido só sai quando o Wi-Fi recebe IP, e uma tarefa repete o pedido a cada 3 s até a hora chegar: o primeiro pacote logo após conectar costuma se perder, e o SNTP sozinho esperaria 15 s, 30 s... Sem o atraso aleatório de início (`CONFIG_LWIP_SNTP_STARTUP_DELAY=n`). A hora aparece ~15–20 s após ligar e se mantém durante o deep sleep.

## Ajustes

Tela própria (`settings.c`), aberta pelo item ⚙ do menu. Uma lista de opções, cada uma com `-`/`+` (toque) ou ←/→ (teclado), ↑/↓ troca de linha, ◀ MENU volta:

| Opção | Valores | Guardado em |
|---|---|---|
| Brilho | 5–100 % (PWM no backlight, LEDC) | `bright` |
| Volume | mudo, 10–100 % | `volume` |
| Desligar sozinho | nunca, 1, 2, 5, 10, 15, 30, 60 min | `autooff` |
| Fonte | 5x8, 6x12, 8x16, 12x24 | `font` |

O brilho virou PWM (antes o backlight era só liga/desliga): timer/canal LEDC próprios no GPIO45; no deep sleep o canal para em nível baixo e o pino é preso, como antes.

## Som

Mesma abordagem do TentacleOS (`ui_feedback.c`): tons senoidais curtos gerados na hora, sem arquivos.

- **Hardware:** codec ES8311 no I2C0 (mesmo barramento do toque, endereço 0x18) e I2S0 a 22.050 Hz estéreo 16 bits; amplificador FM8002E ligado de vez (GPIO1 em nível baixo).
- **Síntese:** seno com envelope cosseno de 4 ms de ataque e 8 ms de release (sem estalos).
- **Sem acúmulo:** fila de 1 posição; um som novo é descartado se outro está tocando ou se o último foi há menos de 120 ms. Girar o carrossel rápido não empilha bipes.
- **Tarefa de prioridade 6**, acima do carrossel e do terminal, para o desenho não interromper as amostras.

| Som | Notas (Hz, ms) | Quando |
|---|---|---|
| NAV | 2000/32 | trocar item, voltar, caractere BEL |
| SELECT | 1568/40 | abrir item, mudar volume |
| KEY | 2000/30 (mais baixo) | tecla do teclado na tela |
| READ | 1318/60, 1976/95 | scan terminou com resultados |
| WRITE | 1568/45, 2093/80 | `write` salvou |
| ERROR | 392/90, 262/140 | comando inexistente, scan vazio (nosso; não existe no TentacleOS) |
| BOOT | 523/120, 659/120, 784/175 | ao ligar (dó-mi-sol) |
| OFF | 784/120, 659/120, 523/175 | ao desligar (nosso: o boot ao contrário) |
| PLUG | 1046/45, 1568/55, 2093/80 | carregador conectado (TentacleOS: SD inserido) |
| UNPLUG | 2093/45, 1568/55, 1046/80 | carregador removido (TentacleOS: SD removido) |

## Conectar no Wi-Fi

Tela cheia (`wifitools_connect_screen`, item **conectar** na categoria WiFi): `wifi_scan_list()` procura as redes, a lista mostra barras de sinal e cadeado (aberta/protegida), toca-se numa rede, a senha é digitada no teclado da tela (`osk`) e `wifi_connect()` conecta; espera até 10 s e mostra sucesso ou falha. As credenciais ficam salvas pelo driver e reconectam no boot.

## Detector de deauth

Escuta passiva em modo promíscuo (`esp_wifi_set_promiscuous`), filtro só de management frames. O callback conta os subtipos **0x0C (deauth)** e **0x0A (disassoc)** — o byte de frame control é `0xC0`/`0xA0` — e guarda a origem (addr2). O painel pula pelos canais 1–13 a cada 280 ms, agrega por segundo num gráfico de 48 s e alerta acima de 5/s. **Não transmite**: por escolha, o deck detecta o ataque, não o executa. Ao sair, `wifi_reconnect()` volta para a rede salva.

## Bad USB

A placa vira um **teclado USB** (TinyUSB HID) e "digita" um script no computador em que está ligada — automação e teste **autorizado**.

- **Scripts** em `/sd/badusb/*.txt`, em DuckyScript: `REM`, `DELAY ms`, `DEFAULT_DELAY`, `STRING`, `STRINGLN`, `REPEAT n`, teclas (`ENTER`, `TAB`, `F1`..`F12`, setas...) e combos (`GUI r`, `CTRL ALT DELETE`). Layout US, via a tabela `HID_ASCII_TO_KEYCODE` da TinyUSB.
- **Nunca digita sozinho:** lista os scripts, mostra o escolhido e injeta só depois do botão **INJETAR** na tela. Um exemplo inofensivo é criado no primeiro uso.
- **Abortar:** o toque continua funcionando durante a injeção (a USB virou teclado, mas a tela é separada); tocar aborta.
- **Custo:** ligar o HID **substitui o console serial USB** até reiniciar — a mesma porta não pode ser teclado e serial ao mesmo tempo. VID/PID da Espressif (0x303A/0x4010), sem imitar outro fabricante.

## Ferramentas geradoras

| Script | Gera |
|---|---|
| `tools/gen_fonts.py` | `main/fonts.h` a partir de `fonts/*.bdf` |
| `tools/png2icons.py` | `main/icons.h` a partir dos PNGs do TentacleOS (decodificador PNG próprio, sem dependências) |
| `tools/gen_icons.py` | versão antiga: ícones desenhados por código |
