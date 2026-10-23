# Histórico

Registro do que foi feito, mais recente primeiro. Atualizar a cada mudança.

## 2026-10-22

### Botão de desligar, % da bateria e hora
- **Botão ⏻** no canto superior esquerdo da home, com confirmação ("Cancelar" pré-selecionado). Depois aumentado para um botão de 46×32 px, com área de toque maior no topo (44 px), porque estava difícil de acertar.
- Comando `touchtest`: mostra onde cada toque cai, para conferir a precisão do toque.
- **Porcentagem da bateria** ao lado do ícone, na home e no cabeçalho.
- **Hora voltou:** o SNTP pedia a hora antes do Wi-Fi conectar e só tentava de novo bem depois. Agora pede ao receber IP e repete a cada 3 s até chegar (~17 s após ligar).

### Energia e carregamento
- **Desligar sem tecla:** *Sistema → desligar* põe o deck em deep sleep (tela, som, rádio e toque em baixo consumo). **Tocar na tela** ou **BOOT** liga.
- **Desligamento automático** após 5 min sem uso (`autooff`).
- **Indicador de carga:** raio no ícone da bateria (amarelo carregando, branco cheia), detectado pelo cabo no PC ou pelo salto de tensão do carregador; sons ao plugar/desplugar; comando `bateria`.
- Removido o código antigo da barra de status (`TermStatusProvider`).

### Sons
- Alto-falante ligado: codec ES8311 + amplificador, pelo componente `esp_codec_dev` 1.5.10 (o mesmo do Tactility nesta placa).
- Tons da interface copiados do TentacleOS (navegar, selecionar, clique de tecla, leitura, gravação, acorde de boot) + um som de erro nosso.
- Comando `volume` (0–100, salvo).

### Caneta e teclado em todas as telas
- **Sem barra em nenhuma tela:** no terminal fica um cabeçalho limpo com **◀ MENU**, botão de teclado e os ícones de status da home.
- **◀ MENU** em todas as telas: na shell abre o menu, em ferramentas interrompe e volta, nas categorias volta para a home.
- **Entrada unificada:** teclado, serial e toque viram eventos numa fila só (`input.c`); tarefa de gestos para tocar e deslizar (`touch.c`).
- **Teclado na tela** (`osk.c`): letras, números e símbolos, `Aa`, `^C`, setas, `ESC`. Abre sozinho nos argumentos das ferramentas.
- Deslizar rola o terminal; tocar continua o "-- mais --", fecha resultados e para comandos longos.
- Desenho compartilhado entre home e terminal (`ui_draw.c`).

### Home sem barra e sem travadas
- A home ocupa a tela inteira: a barra de cima some e no lugar ficam **ícones preenchidos** de Bluetooth, Wi-Fi (leque com força do sinal), bateria (nível colorido) e hora.
- **Travada corrigida:** a flutuação redesenhava e mandava a tela inteira a cada pixel, bloqueando toque e teclado. Agora o fundo e os tiles laterais ficam prontos num buffer e só o retângulo do tile central é atualizado.

### Menu em carrossel
- Efeito **coverflow**: tiles laterais deformados em trapézio, como se girados em 3D, empilhando para o fundo.
- **Flutuação** do tile central (±5 px, 3,2 s), com os valores do TentacleOS.
- Espaçamento mais justo entre o central e os vizinhos.
- Mutex no acesso à tela: a barra de status e o carrossel desenhavam de tarefas diferentes ao mesmo tempo.
- **Paleta e ícones do TentacleOS**: fundo quase preto, cor de destaque por categoria, ícones convertidos dos PNGs deles (`tools/png2icons.py`).
- Categorias do menu iguais às deles: WiFi, Bluetooth, SubGHz, NFC, RFID, LoRa, Infrared, Arquivos, Sistema, Utils, Terminal. As que precisam de módulo mostram qual.
- Animação de deslize, perspectiva (tamanho/brilho por distância) e "pop" ao selecionar.
- Ícones com anti-serrilhado (máscaras 96×96), tiles com gradiente, glow e sombra.
- Primeira versão do carrossel com toque (driver FT6336).

### Terminal e shell
- Menu de texto por categorias (depois substituído pelo carrossel).
- **Rolagem**: 300 linhas de histórico, PgUp/PgDn e Shift+↑/↓; indicador na barra.
- **Paginador**: `help`, `wifi scan`, `blescan`, `netscan`, `http`, `cat`, `ls -l`, `hexdump` pausam a cada tela.
- ~30 ferramentas: rede (`ping`, `netscan`, `portscan`, `dns`, `http`, `myip`, `weather`), rádio (`blescan`, `wifi scan` detalhado), arquivos no SD, sistema, utilitários.
- Barra de status (Wi-Fi, BT, bateria, relógio NTP), banner com moldura, UTF-8 e acentos.
- Fontes Spleen em 4 tamanhos (`font`), padrão 8×16.

### Base
- Firmware próprio em ESP-IDF v6.1 (o Tactility foi testado e descartado: bugs no assistente e no Wi-Fi).
- Tela, teclado BLE (pareamento sem código), Wi-Fi salvo, entrada pela serial.

## Próximos passos possíveis
- Bad USB (a placa como teclado USB)
- Cliente SSH (Linux e Python de verdade num servidor)
- MicroPython no segundo slot de app
- Apps reais para SubGHz/NFC/RFID/LoRa/IR quando houver os módulos
- Editor de texto
