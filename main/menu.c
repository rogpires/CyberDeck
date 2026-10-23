#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "carousel.h"
#include "icons.h"
#include "input.h"
#include "osk.h"
#include "shell.h"
#include "term.h"

#define MAX_ITEMS 48

// Protocol accent colors from TentacleOS's theme
#define C_WIFI   0xBF00FF
#define C_BLE    0xD158F2
#define C_NFC    0xCC00FF
#define C_SUBGHZ 0x4CAF50
#define C_RFID   0xFFC107
#define C_IR     0xF44336
#define C_LORA   0x00BCD4
#define C_PURPLE 0xBF00FF

typedef enum { KIND_CATEGORY, KIND_TERMINAL, KIND_MODULE } Kind;

typedef struct {
    const char* label;
    const char* icon;     // icon name in icons.h
    uint32_t accent;
    Kind kind;
    const char* data;     // category name, or the module needed
} HomeItem;

// The home carousel. Radio protocols we have no hardware for show a "needs module" note.
static const HomeItem home[] = {
    {"WiFi",     "wifi",      C_WIFI,   KIND_CATEGORY, "rede"},
    {"Bluetooth","bluetooth", C_BLE,    KIND_CATEGORY, "radio"},
    {"SubGHz",   "subghz",    C_SUBGHZ, KIND_MODULE,   "CC1101 (433MHz)"},
    {"NFC",      "nfc",       C_NFC,    KIND_MODULE,   "PN532"},
    {"RFID",     "rfid",      C_RFID,   KIND_MODULE,   "RC522 (125kHz)"},
    {"LoRa",     "lora",      C_LORA,   KIND_MODULE,   "SX1276"},
    {"Infrared", "ir",        C_IR,     KIND_MODULE,   "LED/receptor IR"},
    {"Arquivos", "folder",    C_PURPLE, KIND_CATEGORY, "arquivos"},
    {"Sistema",  "gear",      C_PURPLE, KIND_CATEGORY, "sistema"},
    {"Utils",    "apps",      C_PURPLE, KIND_CATEGORY, "utils"},
    {"Terminal", "apps",      C_PURPLE, KIND_TERMINAL, NULL},
};
#define HOME_COUNT (sizeof(home) / sizeof(home[0]))

static int icon_id(const char* name) {
    for (size_t i = 0; i < ICON_COUNT; i++) {
        if (strcmp(icons[i].name, name) == 0) return i;
    }
    return 0;
}

// Waits for "continue": a key, a tap or "< MENU". Swipes scroll the output meanwhile.
static void wait_to_return(void) {
    term_puts("\r\n\x1b[7m-- toque ou tecla para voltar --\x1b[0m");
    while (true) {
        InputEvent event;
        input_wait(&event, UINT32_MAX);
        if (event.type == EV_SWIPE_UP) term_scroll(-(term_rows() / 2));
        else if (event.type == EV_SWIPE_DOWN) term_scroll(term_rows() / 2);
        else if (event.type == EV_KEY || event.type == EV_TAP || event.type == EV_BACK) break;
    }
    input_flush();
}

// Reads a tool's arguments, with the on-screen keyboard open. Returns false if cancelled.
static bool read_args(const char* name, const char* usage, char* out, size_t size) {
    term_clear();
    osk_show();
    term_printf("\x1b[1;36m%s\x1b[0m \x1b[90m%s\x1b[0m\r\n", name, usage ? usage : "");
    term_puts("\x1b[90mENT confirma, ESC ou MENU cancela\x1b[0m\r\n> ");

    int len = 0;
    bool ok = false;
    out[0] = 0;
    while (true) {
        InputEvent event;
        input_wait(&event, UINT32_MAX);
        if (event.type == EV_BACK) break;
        if (event.type == EV_KEYBOARD) {
            osk_toggle();
            continue;
        }
        if (event.type != EV_KEY) continue;
        char c = event.key;
        if (c == '\r' || c == '\n') {
            ok = true;
            break;
        }
        if (c == 0x1b) {
            // A lone Esc cancels; arrow keys (Esc [ X) are ignored here
            char next;
            if (!input_read(&next, 40)) break;
            if (next == '[' || next == 'O') {
                while (input_read(&next, 40) && next >= '0' && next <= ';') {}
            }
            continue;
        }
        if (c == 0x03) break;
        if (c == 0x7f || c == '\b') {
            if (len > 0) {
                len--;
                out[len] = 0;
                term_puts("\b \b");
            }
        } else if ((unsigned char)c >= 0x20 && len < (int)size - 1) {
            out[len++] = c;
            out[len] = 0;
            term_write(&c, 1);
        }
    }
    osk_hide();
    term_puts("\r\n");
    return ok;
}

static void run_command(const ShellCommand* command) {
    char line[160];
    strlcpy(line, command->name, sizeof(line));
    if (command->usage != NULL && command->usage[0] != 0) {
        char args[128];
        if (!read_args(command->name, command->usage, args, sizeof(args))) return;
        if (args[0] != 0) {
            strlcat(line, " ", sizeof(line));
            strlcat(line, args, sizeof(line));
        }
    }
    term_clear();
    term_printf("\x1b[1;32m> %s\x1b[0m\r\n\r\n", line);
    shell_run_line(line);
    wait_to_return();
}

static void category_menu(const char* category, const char* icon, uint32_t accent) {
    const char* labels[MAX_ITEMS];
    int icon_ids[MAX_ITEMS];
    uint32_t accents[MAX_ITEMS];
    const ShellCommand* commands[MAX_ITEMS];
    int count = 0;
    int id = icon_id(icon);

    for (int i = 0; i < shell_command_count() && count < MAX_ITEMS; i++) {
        const ShellCommand* command = shell_command_at(i);
        if (strcmp(command->category, category) != 0) continue;
        labels[count] = command->name;
        icon_ids[count] = id;
        accents[count] = accent;
        commands[count] = command;
        count++;
    }
    if (count == 0) return;

    int selected = 0;
    while (true) {
        selected = carousel_run(labels, icon_ids, accents, count, selected, true);
        if (selected < 0) return;
        run_command(commands[selected]);
    }
}

static void module_screen(const HomeItem* item) {
    term_clear();
    term_printf("\x1b[1;35m%s\x1b[0m\r\n\r\n", item->label);
    term_puts("Esta ferramenta precisa de um\r\nmodulo de hardware que o deck\r\nnao tem embutido:\r\n\r\n");
    term_printf("  \x1b[1;33m%s\x1b[0m\r\n\r\n", item->data);
    term_puts("\x1b[90mDa para ligar um nos pinos e eu\r\nimplemento o app depois.\x1b[0m\r\n");
    wait_to_return();
}

void menu_open(void) {
    const char* labels[HOME_COUNT];
    int icon_ids[HOME_COUNT];
    uint32_t accents[HOME_COUNT];
    for (size_t i = 0; i < HOME_COUNT; i++) {
        labels[i] = home[i].label;
        icon_ids[i] = icon_id(home[i].icon);
        accents[i] = home[i].accent;
    }

    osk_hide();
    input_flush();
    int selected = 0;
    while (true) {
        selected = carousel_run(labels, icon_ids, accents, HOME_COUNT, selected, false);
        if (selected < 0) continue; // no "back" at the top level
        const HomeItem* item = &home[selected];
        switch (item->kind) {
            case KIND_TERMINAL:
                term_clear();
                return;
            case KIND_MODULE:
                module_screen(item);
                break;
            case KIND_CATEGORY:
                category_menu(item->data, item->icon, item->accent);
                break;
        }
    }
}
