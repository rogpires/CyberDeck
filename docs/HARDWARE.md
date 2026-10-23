# Hardware

Placa: **LCDwiki 2.8" ES3C28P** — [lcdwiki.com/2.8inch_ESP32-S3_Display](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display)

Lido da placa com `esptool`: ESP32-S3 (QFN56) rev v0.2, 8 MB PSRAM octal, 16 MB flash, cristal 40 MHz, USB nativo (USB-Serial/JTAG).

## Pinagem usada pelo firmware

| Função | Periférico | Pinos |
|---|---|---|
| Tela ILI9341 | SPI2 | MOSI 11, SCLK 12, CS 10, DC 46, backlight 45 |
| Toque FT6336 (0x38) | I2C0 | SDA 16, SCL 15, RESET 18, INT 17 |
| microSD | SDMMC slot 1, 4 bits | CLK 38, CMD 40, D0 39, D1 41, D2 48, D3 47 |
| Bateria | ADC1 canal 8 | GPIO 9 (divisor 200K/200K → ×2) |
| Áudio ES8311 (0x18) + amp FM8002E | I2C0 + I2S0 | MCLK 4, BCLK 5, WS 7, DOUT 8, DIN 6; amp enable GPIO 1 (ativo baixo) |
| Botão BOOT | GPIO 0 (RTC) | acorda do deep sleep |
| INT do toque | GPIO 17 (RTC) | acorda do deep sleep ao tocar |

Fonte da pinagem: device tree do Tactility para esta placa (`Devices/es3c28p/es3c28p.dts`), confirmada funcionando, e a tabela de pinos da LCDwiki.

Outros pinos ligados na placa, ainda não usados pelo firmware: **IO42** (LED RGB de um fio), **IO13** (MISO do SPI da tela).

### Energia

- Não há chave liga/desliga nem pino de status do carregador ligado ao ESP.
- "Desligado" é deep sleep: o ESP32-S3 gasta microamperes, mas regulador, tela, carregador e codec em espera continuam consumindo um pouco. A bateria dura dias ou semanas parada, não meses.
- Para guardar o deck: uma **chave deslizante no fio positivo da bateria** zera o consumo. Com a chave entre a bateria e o conector, a bateria **não carrega** enquanto a chave estiver aberta: feche a chave para carregar.

### Orientação

- Tela em paisagem: `swap_xy` + `invert_color`, ordem BGR.
- Toque: lê X (0..239) e Y (0..319) em retrato, espelha X (`240 - x`) e troca X/Y → coordenadas de tela 320×240. Ver `main/touch.c`.

## Partições (16 MB)

| Nome | Tipo | Offset | Tamanho |
|---|---|---|---|
| nvs | dados | 0x9000 | 24 KB |
| otadata | ota | 0xF000 | 8 KB |
| phy_init | phy | 0x11000 | 4 KB |
| ota_0 | app | 0x20000 | 4 MB (este firmware) |
| ota_1 | app | 0x420000 | 4 MB (reservado: MicroPython) |
| storage | fat | 0x820000 | ~7.9 MB |

## Módulos externos

O TentacleOS roda num ESP32-P4 com rádios a bordo. Aqui eles são módulos externos; enquanto não estiverem ligados, o menu mostra o ícone e avisa qual módulo falta.

### Conectores livres

Todos são JST **1.25 mm, 4 pinos** (vendidos como "MX1.25" ou "GH1.25"). Fonte: tabela de pinos da LCDwiki.

| Conector | GPIOs | Observação |
|---|---|---|
| Expansão | **IO2, IO3, IO14, IO21** | uso geral. IO3 é pino de *strapping* (seleção de JTAG), sem efeito com os eFuses de fábrica |
| UART | **IO43** (RX0), **IO44** (TX0) | livres: o console do firmware é a USB-Serial/JTAG |
| I2C | **IO15** (SCL), **IO16** (SDA) | barramento compartilhado com o toque (0x38) e o codec (0x18) |

**Alimentação:** pela descrição da LCDwiki, o conector de expansão traz só os 4 GPIOs; 3,3 V e GND devem vir dos conectores I2C/UART. **Não confirmado pelo manual: medir com multímetro antes de ligar.**

### Plano de pinos

| Módulo | Para | Ligação | Pinos | Tensão |
|---|---|---|---|---|
| **PN532** | NFC e RFID 13,56 MHz (MIFARE) | I2C (chaves do módulo em modo I2C), endereço 0x24 | SDA IO16, SCL IO15 — nenhum GPIO extra | 3,3 V |
| **CC1101** | SubGHz (315/433/868/915 MHz) | SPI3 próprio no conector de expansão | SCK **IO14**, MOSI **IO21**, MISO **IO2**, CSN **IO3**, GDO0 **IO44** | 3,3 V (nunca 5 V) |
| **RDM6300** | RFID 125 kHz (EM4100: crachás, chaveiros) | UART 9600 baud, só leitura | TX do módulo → **IO43** | **5 V**: TX precisa de divisor (ex.: 10k em série, 20k para o GND) antes do IO43 |
| **SX1276 / SX1262** | LoRa | mesmo SPI do CC1101 com CS próprio | falta CS, IRQ (DIO0/DIO1) e RST → usar um expansor I2C | 3,3 V |
| **LED IR + TSOP38238** | Infravermelho | TX por RMT, RX por RMT | IO43/IO44 se não usar RDM6300 nem o GDO0 do CC1101 | 3,3 V (LED com transistor para ter alcance) |

### Prioridade (não há pinos para tudo ao mesmo tempo)

1. **PN532 no I2C**, que não gasta GPIO.
2. **CC1101 na expansão** + IO44.
3. **RDM6300 ou IR** no IO43.
4. Para LoRa junto com o resto: **expansor de GPIO I2C** (PCF8574 ou MCP23017, no mesmo barramento I2C) para CS, IRQ e RST, ou trocar o CC1101 pelo LoRa no SPI da expansão.
