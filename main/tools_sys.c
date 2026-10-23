// System and utility commands: device status, settings, calculator, generators, screensaver
#include "tools.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "power.h"
#include "input.h"
#include "shell.h"
#include "sound.h"
#include "statusbar.h"
#include "storage.h"
#include "term.h"
#include "menu.h"
#include "wifi.h"

static void print_uptime(void) {
    int64_t seconds = esp_timer_get_time() / 1000000;
    term_printf("%lldh %02lldm %02llds", seconds / 3600, (seconds / 60) % 60, seconds % 60);
}

static void cmd_wifi(int argc, char** argv) {
    const char* sub = argc > 1 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0) {
        wifi_print_status();
    } else if (strcmp(sub, "scan") == 0) {
        wifi_scan();
    } else if (strcmp(sub, "connect") == 0 && argc >= 3) {
        wifi_connect(argv[2], argc >= 4 ? argv[3] : "");
    } else if (strcmp(sub, "off") == 0) {
        wifi_disconnect();
        term_puts("[wifi] desligado\r\n");
    } else {
        shell_print_usage("wifi");
    }
}

static void cmd_bt(int argc, char** argv) {
    const char* sub = argc > 1 ? argv[1] : "status";
    if (strcmp(sub, "forget") == 0) {
        keyboard_forget();
        term_puts("[bt] pareamentos apagados\r\n");
    } else if (keyboard_is_connected()) {
        term_printf("teclado: %s\r\n", keyboard_name());
    } else {
        term_puts("nenhum teclado conectado\r\n");
    }
}

static void cmd_font(int argc, char** argv) {
    if (argc < 2) {
        char names[64];
        term_list_fonts(names, sizeof(names));
        term_printf("fontes: %s\r\n", names);
        shell_print_usage("font");
        return;
    }
    if (!term_set_font(argv[1])) {
        term_printf("\x1b[31mfonte desconhecida: %s\x1b[0m\r\n", argv[1]);
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open("deck", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "font", argv[1]);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    term_printf("fonte %s: %dx%d caracteres\r\n", argv[1], term_cols(), term_rows());
}

static void cmd_mem(int argc, char** argv) {
    term_printf("ram interna: %u KB livres (menor: %u KB)\r\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    term_printf("psram:       %u KB livres\r\n", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void cmd_uptime(int argc, char** argv) {
    print_uptime();
    term_puts("\r\n");
}

static void cmd_volume(int argc, char** argv) {
    if (argc > 1) {
        sound_set_volume(atoi(argv[1]));
        sound_play(SOUND_SELECT);
    }
    int v = sound_volume();
    // A bar so the level is easy to read at a glance
    term_printf("volume %3d%% [", v);
    for (int i = 0; i < 20; i++) term_puts(i < v / 5 ? "\x1b[35m\u2588" : "\x1b[90m\u2591");
    term_puts("\x1b[0m]\r\n");
    if (argc < 2) shell_print_usage("volume");
}

static void cmd_battery(int argc, char** argv) {
    int mv = statusbar_battery_mv();
    if (mv <= 0) {
        term_puts("bateria: sem leitura\r\n");
        return;
    }
    const char* state = statusbar_battery_full() ? "\x1b[32mcheia (na energia)"
                      : statusbar_charging() ? "\x1b[33mcarregando" : "\x1b[0mna bateria";
    term_printf("bateria: %d%%  %d.%02d V\r\nestado:  %s\x1b[0m\r\n", statusbar_battery_percent(), mv / 1000,
                (mv % 1000) / 10, state);
    term_puts("\x1b[90mcarga detectada por cabo no PC ou\r\npelo salto de tensao do carregador\x1b[0m\r\n");
}

// Marks each touch on the screen and prints its coordinates, to check the touch accuracy
static void cmd_touchtest(int argc, char** argv) {
    term_clear();
    term_puts("toque em varios pontos da tela\r\n\x1b[90mtecla ou \u25c0 MENU sai\x1b[0m\r\n");
    input_flush();
    while (true) {
        InputEvent event;
        input_wait(&event, UINT32_MAX);
        if (event.type == EV_KEY) break;
        const char* name = event.type == EV_TAP ? "toque" : event.type == EV_BACK ? "MENU" :
                           event.type == EV_KEYBOARD ? "teclado" : "deslize";
        // A '+' at the touched character cell (the text area starts below the 16 px header)
        int col = event.x / 8, row = (event.y - 16) / 16;
        if (row >= 2 && row < term_rows() && col < term_cols()) term_printf("\x1b[%d;%dH\x1b[1;33m+\x1b[0m", row + 1, col + 1);
        term_printf("\x1b[%d;1H\x1b[K%s x=%d y=%d", term_rows(), name, event.x, event.y);
        if (event.type == EV_BACK) break;
    }
    term_puts("\r\n");
}

static void cmd_poweroff(int argc, char** argv) {
    power_off();
}

static void cmd_autooff(int argc, char** argv) {
    if (argc > 1) power_set_autooff(atoi(argv[1]));
    int minutes = power_autooff();
    if (minutes == 0) term_puts("desligamento automatico: nunca\r\n");
    else term_printf("desliga apos %d min sem uso\r\n", minutes);
    if (argc < 2) shell_print_usage("autooff");
}

static void cmd_menu(int argc, char** argv) {
    menu_open();
}

static void cmd_reboot(int argc, char** argv) {
    term_puts("reiniciando...\r\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void cmd_date(int argc, char** argv) {
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2025) {
        term_puts("hora ainda nao sincronizada (precisa de wifi)\r\n");
        return;
    }
    static const char* days[] = {"domingo", "segunda", "terca", "quarta", "quinta", "sexta", "sabado"};
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", &local);
    term_printf("%s, %s (BRT)\r\n", days[local.tm_wday], buffer);
}

static void cmd_sysinfo(int argc, char** argv) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    static const char* logo[] = {
        "\x1b[36m┌──────┐",
        "\x1b[36m│\x1b[92m >_   \x1b[36m│",
        "\x1b[36m│\x1b[92m █    \x1b[36m│",
        "\x1b[36m└┬────┬┘",
        "\x1b[36m ▀▀▀▀▀▀ ",
        "        ",
        "        ",
        "        ",
    };
    char lines[8][96]; // room for the color escapes
    snprintf(lines[0], sizeof(lines[0]), "\x1b[1;36mdeck\x1b[0m@\x1b[1;36mcyberdeck\x1b[0m");
    snprintf(lines[1], sizeof(lines[1]), "\x1b[36mchip\x1b[0m  ESP32-S3 r%d.%d %dx240MHz", chip.revision / 100, chip.revision % 100, chip.cores);
    snprintf(lines[2], sizeof(lines[2]), "\x1b[36mram\x1b[0m   %uK livre/512K",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    snprintf(lines[3], sizeof(lines[3]), "\x1b[36mpsram\x1b[0m %uK livre/8M", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    snprintf(lines[4], sizeof(lines[4]), "\x1b[36mflash\x1b[0m %luMB", (unsigned long)(flash_size / (1024 * 1024)));
    snprintf(lines[5], sizeof(lines[5]), "\x1b[36midf\x1b[0m   %s", esp_get_idf_version());
    snprintf(lines[6], sizeof(lines[6]), "\x1b[36mtela\x1b[0m  320x240 %dx%d", term_cols(), term_rows());
    snprintf(lines[7], sizeof(lines[7]), "\x1b[36mwifi\x1b[0m  %s  \x1b[36mbt\x1b[0m %s",
             wifi_is_connected() ? "\x1b[32mon\x1b[0m" : "\x1b[90moff\x1b[0m",
             keyboard_is_connected() ? "\x1b[32mon\x1b[0m" : "\x1b[90moff\x1b[0m");
    for (int i = 0; i < 8; i++) {
        term_printf("%s\x1b[0m %s\r\n", logo[i], lines[i]);
    }
    term_puts("         \x1b[36mup\x1b[0m    ");
    print_uptime();
    term_puts("\r\n         ");
    for (int i = 0; i < 8; i++) term_printf("\x1b[4%dm  ", i);
    term_puts("\x1b[0m\r\n");
}

// calc: recursive descent over + - * / % ^, parentheses, functions and constants

typedef struct {
    const char* p;
    bool error;
} Parser;

static double parse_expression(Parser* parser);

static void skip_spaces(Parser* parser) {
    while (*parser->p == ' ') parser->p++;
}

static double parse_primary(Parser* parser) {
    skip_spaces(parser);
    const char* p = parser->p;
    if (*p == '(') {
        parser->p++;
        double value = parse_expression(parser);
        skip_spaces(parser);
        if (*parser->p == ')') parser->p++;
        else parser->error = true;
        return value;
    }
    if (isdigit((unsigned char)*p) || *p == '.') {
        char* end;
        double value = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ? (double)strtoll(p, &end, 16)
                     : (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) ? (double)strtoll(p + 2, &end, 2)
                     : strtod(p, &end);
        parser->p = end;
        return value;
    }
    if (isalpha((unsigned char)*p)) {
        char name[8] = {0};
        int len = 0;
        while (isalpha((unsigned char)*parser->p) && len < 7) name[len++] = *parser->p++;
        if (strcmp(name, "pi") == 0) return M_PI;
        if (strcmp(name, "e") == 0) return M_E;
        double arg = parse_primary(parser);
        if (strcmp(name, "sqrt") == 0) return sqrt(arg);
        if (strcmp(name, "sin") == 0) return sin(arg);
        if (strcmp(name, "cos") == 0) return cos(arg);
        if (strcmp(name, "tan") == 0) return tan(arg);
        if (strcmp(name, "log") == 0) return log10(arg);
        if (strcmp(name, "ln") == 0) return log(arg);
        if (strcmp(name, "abs") == 0) return fabs(arg);
        if (strcmp(name, "round") == 0) return round(arg);
    }
    parser->error = true;
    return 0;
}

static double parse_unary(Parser* parser) {
    skip_spaces(parser);
    if (*parser->p == '-') {
        parser->p++;
        return -parse_unary(parser);
    }
    if (*parser->p == '+') {
        parser->p++;
        return parse_unary(parser);
    }
    double base = parse_primary(parser);
    skip_spaces(parser);
    if (*parser->p == '^') {
        parser->p++;
        return pow(base, parse_unary(parser));
    }
    return base;
}

static double parse_term(Parser* parser) {
    double value = parse_unary(parser);
    while (true) {
        skip_spaces(parser);
        char op = *parser->p;
        if (op != '*' && op != '/' && op != '%') return value;
        parser->p++;
        double right = parse_unary(parser);
        value = op == '*' ? value * right : (op == '/' ? value / right : fmod(value, right));
    }
}

static double parse_expression(Parser* parser) {
    double value = parse_term(parser);
    while (true) {
        skip_spaces(parser);
        char op = *parser->p;
        if (op != '+' && op != '-') return value;
        parser->p++;
        double right = parse_term(parser);
        value = op == '+' ? value + right : value - right;
    }
}

static void cmd_calc(int argc, char** argv) {
    if (argc < 2) {
        shell_print_usage("calc");
        return;
    }
    char expression[128] = "";
    for (int i = 1; i < argc; i++) {
        strlcat(expression, argv[i], sizeof(expression));
        strlcat(expression, " ", sizeof(expression));
    }
    Parser parser = {expression, false};
    double value = parse_expression(&parser);
    skip_spaces(&parser);
    if (parser.error || *parser.p != 0) {
        term_printf("\x1b[31merro perto de: %s\x1b[0m\r\n", parser.p);
        return;
    }
    term_printf("= \x1b[1m%.10g\x1b[0m", value);
    if (value == floor(value) && fabs(value) < 1e15) {
        long long integer = (long long)value;
        term_printf("  \x1b[90m0x%llX\x1b[0m", integer);
    }
    term_puts("\r\n");
}

static void cmd_passgen(int argc, char** argv) {
    int length = argc > 1 ? atoi(argv[1]) : 16;
    if (length < 4 || length > 64) length = 16;
    bool symbols = !(argc > 2 && strcmp(argv[2], "-a") == 0);
    const char* alphabet = symbols
        ? "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#$%&*-_=+?"
        : "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    size_t size = strlen(alphabet);
    term_puts("\x1b[1;33m");
    for (int i = 0; i < length; i++) {
        // Rejection sampling keeps every character equally likely
        uint32_t r;
        do r = esp_random(); while (r >= UINT32_MAX - (UINT32_MAX % size));
        char c = alphabet[r % size];
        term_write(&c, 1);
    }
    term_puts("\x1b[0m\r\n");
}

static void print_hex(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) term_printf("%02x", data[i]);
    term_puts("\r\n");
}

static void cmd_sha256(int argc, char** argv) {
    if (argc < 2) {
        shell_print_usage("sha256");
        return;
    }
    psa_crypto_init();
    uint8_t hash[32];
    size_t hash_len = 0;

    if (strcmp(argv[1], "-f") == 0 && argc > 2) {
        char path[128];
        shell_resolve_path(argv[2], path, sizeof(path));
        FILE* file = fopen(path, "rb");
        if (file == NULL) {
            term_printf("\x1b[31mnao abriu %s\x1b[0m\r\n", path);
            return;
        }
        psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
        psa_hash_setup(&operation, PSA_ALG_SHA_256);
        uint8_t buffer[512];
        size_t read;
        while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
            psa_hash_update(&operation, buffer, read);
        }
        fclose(file);
        psa_hash_finish(&operation, hash, sizeof(hash), &hash_len);
    } else {
        char text[128] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) strlcat(text, " ", sizeof(text));
            strlcat(text, argv[i], sizeof(text));
        }
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)text, strlen(text), hash, sizeof(hash), &hash_len);
    }
    print_hex(hash, hash_len);
}

static void cmd_base64(int argc, char** argv) {
    if (argc < 3 || (strcmp(argv[1], "enc") != 0 && strcmp(argv[1], "dec") != 0)) {
        shell_print_usage("base64");
        return;
    }
    char text[128] = "";
    for (int i = 2; i < argc; i++) {
        if (i > 2) strlcat(text, " ", sizeof(text));
        strlcat(text, argv[i], sizeof(text));
    }
    unsigned char out[200];
    size_t out_len = 0;
    int ret = strcmp(argv[1], "enc") == 0
        ? mbedtls_base64_encode(out, sizeof(out) - 1, &out_len, (const unsigned char*)text, strlen(text))
        : mbedtls_base64_decode(out, sizeof(out) - 1, &out_len, (const unsigned char*)text, strlen(text));
    if (ret != 0) {
        term_puts("\x1b[31mentrada invalida\x1b[0m\r\n");
        return;
    }
    out[out_len] = 0;
    term_printf("%s\r\n", out);
}

// Digital rain until a key is pressed
static void cmd_matrix(int argc, char** argv) {
    const int cols = term_cols(), rows = term_rows();
    static int8_t head[TERM_MAX_COLS];
    static uint8_t length[TERM_MAX_COLS];
    static const char charset[] = "0123456789ABCDEF@#$%&*+=<>?/\\|";
    for (int x = 0; x < cols; x++) {
        head[x] = -(int)(esp_random() % rows);
        length[x] = 4 + esp_random() % (rows / 2);
    }
    term_puts("\x1b[?25l\x1b[2J");
    while (!shell_interrupted()) {
        for (int x = 0; x < cols; x++) {
            // White head, bright green right behind it, dim green further up the trail
            static const struct { int offset; const char* color; } shades[] = {{0, "97"}, {1, "92"}, {4, "32"}};
            int y = head[x];
            for (size_t s = 0; s < sizeof(shades) / sizeof(shades[0]); s++) {
                int row = y - shades[s].offset;
                if (row < 0 || row >= rows || shades[s].offset >= length[x]) continue;
                char c = charset[esp_random() % (sizeof(charset) - 1)];
                term_printf("\x1b[%d;%dH\x1b[%sm%c", row + 1, x + 1, shades[s].color, c);
            }
            int tail = y - length[x];
            if (tail >= 0 && tail < rows) term_printf("\x1b[%d;%dH ", tail + 1, x + 1);
            head[x]++;
            if (tail >= rows) {
                head[x] = -(int)(esp_random() % rows);
                length[x] = 4 + esp_random() % (rows / 2);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(70));
    }
    term_puts("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
}

static const ShellCommand commands[] = {
    {"menu",    "sistema", NULL,                "abre o menu de ferramentas",             cmd_menu},
    {"sysinfo", "sistema", NULL,                "informacoes do deck",                    cmd_sysinfo},
    {"date",    "sistema", NULL,                "data e hora",                            cmd_date},
    {"uptime",  "sistema", NULL,                "tempo ligado",                           cmd_uptime},
    {"mem",     "sistema", NULL,                "memoria livre",                          cmd_mem},
    {"wifi",    "sistema", "[status|scan|off] | connect <rede> [senha]", "rede wifi (aspas se tiver espaco)", cmd_wifi},
    {"bt",      "sistema", "[status|forget]",   "teclado bluetooth",                      cmd_bt},
    {"font",    "sistema", "<5x8|6x12|8x16|12x24>", "tamanho da letra",                   cmd_font},
    {"volume",  "sistema", "<0-100> (0 = mudo)", "volume dos sons",                      cmd_volume},
    {"touchtest","sistema", NULL,               "mostra onde cada toque cai na tela",     cmd_touchtest},
    {"bateria", "sistema", NULL,                "carga, tensao e se esta carregando",     cmd_battery},
    {"desligar","sistema", NULL,                "desliga; toque na tela ou BOOT liga",    cmd_poweroff},
    {"autooff", "sistema", "<minutos> (0 = nunca)", "desliga sozinho sem uso",            cmd_autooff},
    {"reboot",  "sistema", NULL,                "reinicia",                               cmd_reboot},
    {"calc",    "utils",   "<expressao>",       "calculadora: + - * / % ^ ( ) sqrt sin cos log ln pi 0x 0b", cmd_calc},
    {"passgen", "utils",   "[tamanho] [-a]",    "gera senha aleatoria (-a sem simbolos)", cmd_passgen},
    {"sha256",  "utils",   "<texto> | -f <arquivo>", "hash sha-256",                      cmd_sha256},
    {"base64",  "utils",   "enc|dec <texto>",   "codifica/decodifica base64",             cmd_base64},
    {"matrix",  "utils",   NULL,                "chuva digital (q para sair)",            cmd_matrix},
};

void tools_sys_register(void) {
    shell_register(commands, sizeof(commands) / sizeof(commands[0]));
}
