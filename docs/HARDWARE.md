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

Fonte da pinagem: device tree do Tactility para esta placa (`Devices/es3c28p/es3c28p.dts`), confirmada funcionando.

| Botão BOOT | GPIO 0 (RTC) | acorda do deep sleep |
| INT do toque | GPIO 17 (RTC) | acorda do deep sleep ao tocar |

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

## O que a placa NÃO tem

O TentacleOS roda num ESP32-P4 com rádios extras. Para as categorias abaixo, o menu mostra o ícone mas avisa que precisa do módulo:

| Categoria | Módulo necessário |
|---|---|
| SubGHz (433 MHz) | CC1101 (SPI) |
| NFC | PN532 (I2C/SPI) |
| RFID 125 kHz | RC522 / EM4100 |
| LoRa | SX1276/SX1262 (SPI) |
| Infrared | LED IR + receptor (ex.: TSOP38238) |

Pinos livres para ligar módulos: ver o conector de expansão da placa no manual da LCDwiki.
