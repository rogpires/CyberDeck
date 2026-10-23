#include "badusb.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "badusb_hid.h"
#include "class/hid/hid.h"
#include "display.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "osk.h"
#include "storage.h"
#include "term.h"

#define DIR_PATH   SD_MOUNT_POINT "/badusb"
#define MAX_SCRIPTS 32
#define SCRIPT_LINE 256

// A harmless example: opens a text editor and types a note. Authorized use only.
static const char* EXAMPLE =
    "REM Exemplo inofensivo: abre um editor e escreve um texto.\n"
    "REM Use apenas em computadores seus ou com permissao.\n"
    "DELAY 500\n"
    "STRING Ola do CyberDeck!\n";

void badusb_init(void) {
    if (!storage_is_mounted()) return;
    struct stat st;
    if (stat(DIR_PATH, &st) != 0) mkdir(DIR_PATH, 0775);
    char path[64];
    snprintf(path, sizeof(path), "%s/exemplo.txt", DIR_PATH);
    if (stat(path, &st) != 0) {
        FILE* f = fopen(path, "w");
        if (f) {
            fputs(EXAMPLE, f);
            fclose(f);
        }
    }
}

// DuckyScript keyword -> HID keycode (named keys and modifiers)
typedef struct { const char* name; uint8_t keycode; uint8_t modifier; } Keyword;
static const Keyword keywords[] = {
    {"ENTER", HID_KEY_ENTER, 0}, {"SPACE", HID_KEY_SPACE, 0}, {"TAB", HID_KEY_TAB, 0},
    {"ESC", HID_KEY_ESCAPE, 0}, {"ESCAPE", HID_KEY_ESCAPE, 0},
    {"BACKSPACE", HID_KEY_BACKSPACE, 0}, {"DELETE", HID_KEY_DELETE, 0}, {"DEL", HID_KEY_DELETE, 0},
    {"UP", HID_KEY_ARROW_UP, 0}, {"DOWN", HID_KEY_ARROW_DOWN, 0},
    {"LEFT", HID_KEY_ARROW_LEFT, 0}, {"RIGHT", HID_KEY_ARROW_RIGHT, 0},
    {"HOME", HID_KEY_HOME, 0}, {"END", HID_KEY_END, 0},
    {"PAGEUP", HID_KEY_PAGE_UP, 0}, {"PAGEDOWN", HID_KEY_PAGE_DOWN, 0},
    {"INSERT", HID_KEY_INSERT, 0}, {"PRINTSCREEN", HID_KEY_PRINT_SCREEN, 0},
    {"F1", HID_KEY_F1, 0}, {"F2", HID_KEY_F2, 0}, {"F3", HID_KEY_F3, 0}, {"F4", HID_KEY_F4, 0},
    {"F5", HID_KEY_F5, 0}, {"F6", HID_KEY_F6, 0}, {"F7", HID_KEY_F7, 0}, {"F8", HID_KEY_F8, 0},
    {"F9", HID_KEY_F9, 0}, {"F10", HID_KEY_F10, 0}, {"F11", HID_KEY_F11, 0}, {"F12", HID_KEY_F12, 0},
    // Modifiers (used alone or before another key)
    {"GUI", HID_KEY_GUI_LEFT, KEYBOARD_MODIFIER_LEFTGUI}, {"WINDOWS", HID_KEY_GUI_LEFT, KEYBOARD_MODIFIER_LEFTGUI},
    {"COMMAND", HID_KEY_GUI_LEFT, KEYBOARD_MODIFIER_LEFTGUI},
    {"CTRL", HID_KEY_CONTROL_LEFT, KEYBOARD_MODIFIER_LEFTCTRL}, {"CONTROL", HID_KEY_CONTROL_LEFT, KEYBOARD_MODIFIER_LEFTCTRL},
    {"SHIFT", HID_KEY_SHIFT_LEFT, KEYBOARD_MODIFIER_LEFTSHIFT},
    {"ALT", HID_KEY_ALT_LEFT, KEYBOARD_MODIFIER_LEFTALT}, {"OPTION", HID_KEY_ALT_LEFT, KEYBOARD_MODIFIER_LEFTALT},
};

static const Keyword* find_keyword(const char* word) {
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strcasecmp(word, keywords[i].name) == 0) return &keywords[i];
    }
    return NULL;
}

static uint8_t char_keycode(char c, uint8_t* modifier) {
    static const uint8_t conv[128][2] = {HID_ASCII_TO_KEYCODE};
    if ((unsigned char)c >= 128 || conv[(int)c][1] == 0) return 0;
    *modifier = conv[(int)c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
    return conv[(int)c][1];
}

static void type_string(const char* text) {
    for (int i = 0; text[i]; i++) badusb_hid_char(text[i]);
}

// A line that is a sequence of modifier words then an optional final key, e.g. "GUI r" or "CTRL ALT DELETE"
static void run_combo(char* line) {
    uint8_t modifier = 0, keycode = 0;
    char* save = NULL;
    for (char* word = strtok_r(line, " \t", &save); word != NULL; word = strtok_r(NULL, " \t", &save)) {
        const Keyword* kw = find_keyword(word);
        if (kw != NULL && kw->modifier != 0 && kw->keycode >= HID_KEY_CONTROL_LEFT) {
            modifier |= kw->modifier; // it's a modifier
        } else if (kw != NULL) {
            keycode = kw->keycode;     // a named key (ENTER, F4...)
        } else if (strlen(word) == 1) {
            uint8_t m = 0;
            keycode = char_keycode(word[0], &m);
            modifier |= m;
        }
    }
    badusb_hid_tap(modifier, keycode);
}

// Runs the script at `path`. Returns the number of lines executed, or -1 if interrupted.
static int run_script(const char* path, int* total_lines) {
    FILE* f = fopen(path, "r");
    if (f == NULL) return -1;
    char line[SCRIPT_LINE];
    char last[SCRIPT_LINE] = "";
    int default_delay = 0;
    int executed = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        // Strip CR/LF
        line[strcspn(line, "\r\n")] = 0;
        // Touch still works while USB is a keyboard: a tap aborts
        InputEvent ev;
        if (input_wait(&ev, 0) && (ev.type == EV_TAP || ev.type == EV_BACK)) {
            fclose(f);
            return -1;
        }

        char* cmd = line;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (*cmd == 0 || strncasecmp(cmd, "REM", 3) == 0) continue;

        char* arg = strchr(cmd, ' ');
        if (arg) *arg++ = 0;

        if (strcasecmp(cmd, "DELAY") == 0) {
            if (arg) vTaskDelay(pdMS_TO_TICKS(atoi(arg)));
        } else if (strcasecmp(cmd, "DEFAULT_DELAY") == 0 || strcasecmp(cmd, "DEFAULTDELAY") == 0) {
            if (arg) default_delay = atoi(arg);
        } else if (strcasecmp(cmd, "STRING") == 0) {
            if (arg) type_string(arg);
        } else if (strcasecmp(cmd, "STRINGLN") == 0) {
            if (arg) type_string(arg);
            badusb_hid_tap(0, HID_KEY_ENTER);
        } else if (strcasecmp(cmd, "REPEAT") == 0) {
            int n = arg ? atoi(arg) : 0;
            for (int i = 0; i < n && last[0]; i++) {
                char copy[SCRIPT_LINE];
                strlcpy(copy, last, sizeof(copy));
                run_combo(copy);
                if (default_delay) vTaskDelay(pdMS_TO_TICKS(default_delay));
            }
            continue;
        } else {
            // A key or combo. Rebuild the full line for run_combo.
            if (arg) arg[-1] = ' ';
            char copy[SCRIPT_LINE];
            strlcpy(copy, cmd, sizeof(copy));
            run_combo(copy);
        }

        strlcpy(last, line, sizeof(last));
        executed++;
        if (default_delay) vTaskDelay(pdMS_TO_TICKS(default_delay));
    }
    fclose(f);
    if (total_lines) *total_lines = executed;
    return executed;
}

// UI: list, preview, confirm, inject

static int list_scripts(char names[][64]) {
    DIR* dir = opendir(DIR_PATH);
    if (dir == NULL) return 0;
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && count < MAX_SCRIPTS) {
        const char* dot = strrchr(entry->d_name, '.');
        if (dot && strcasecmp(dot, ".txt") == 0) strlcpy(names[count++], entry->d_name, 64);
    }
    closedir(dir);
    return count;
}

static void preview(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == NULL) return;
    char line[SCRIPT_LINE];
    int shown = 0;
    while (fgets(line, sizeof(line), f) && shown < term_rows() - 6) {
        line[strcspn(line, "\r\n")] = 0;
        term_printf("\x1b[90m| \x1b[0m%.*s\r\n", term_cols() - 2, line);
        shown++;
    }
    fclose(f);
}

// Waits for a yes/no: tap the matching button, or Enter/Esc. The on-screen keyboard is not shown here.
static bool confirm_inject(const char* name) {
    term_clear();
    term_printf("\x1b[1;35mBAD USB\x1b[0m  %s\r\n", name);
    term_puts("\x1b[90m--- script ---\x1b[0m\r\n");
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", DIR_PATH, name);
    preview(path);
    term_puts("\r\n\x1b[1;33mA placa vai virar teclado USB e\r\n"
              "digitar isso no computador ligado.\x1b[0m\r\n");
    term_puts("\x1b[90mUse so em maquina sua ou autorizada.\r\n"
              "A serial USB cai ate reiniciar.\x1b[0m\r\n\r\n");
    term_puts("\x1b[7m INJETAR \x1b[0m   \x1b[90m[toque / ENTER]\x1b[0m\r\n");
    term_puts("\x1b[90mcancelar: toque em cima / ESC / MENU\x1b[0m");

    while (true) {
        InputEvent event;
        input_wait(&event, UINT32_MAX);
        if (event.type == EV_BACK) return false;
        if (event.type == EV_KEY) {
            if (event.key == '\r' || event.key == '\n') return true;
            if (event.key == 0x1b || event.key == 'q') return false;
        }
        if (event.type == EV_TAP) {
            // Lower half confirms, upper half cancels
            return event.y > DISPLAY_HEIGHT / 2;
        }
    }
}

void badusb_browse(void) {
    if (!storage_is_mounted()) {
        term_clear();
        term_puts("\x1b[31mprecisa de cartao SD\x1b[0m\r\n");
        term_puts("crie scripts em /sd/badusb/*.txt\r\n\r\n");
        term_puts("\x1b[7m-- toque para voltar --\x1b[0m");
        InputEvent e;
        do { input_wait(&e, UINT32_MAX); } while (e.type != EV_TAP && e.type != EV_KEY && e.type != EV_BACK);
        return;
    }
    badusb_init();

    char names[MAX_SCRIPTS][64];
    int count = list_scripts(names);
    int selected = 0;

    while (true) {
        term_clear();
        term_puts("\x1b[1;35mBAD USB\x1b[0m \x1b[90mscripts em /sd/badusb\x1b[0m\r\n\r\n");
        if (count == 0) {
            term_puts("nenhum script .txt\r\n");
            term_puts("\x1b[90mcrie em /sd/badusb/ pelo PC\x1b[0m\r\n\r\n");
            term_puts("\x1b[7m-- toque para voltar --\x1b[0m");
            InputEvent e;
            do { input_wait(&e, UINT32_MAX); } while (e.type != EV_TAP && e.type != EV_KEY && e.type != EV_BACK);
            return;
        }
        for (int i = 0; i < count; i++) {
            term_printf("%s %s\r\n", i == selected ? "\x1b[7m>\x1b[0m" : " ", names[i]);
        }
        term_puts("\r\n\x1b[90msetas/toque escolhem, ENTER abre, MENU sai\x1b[0m");

        InputEvent event;
        input_wait(&event, UINT32_MAX);
        if (event.type == EV_BACK) return;
        if (event.type == EV_TAP) {
            int row = (event.y - 32) / 16; // after the two header lines
            if (row >= 0 && row < count) {
                selected = row;
                if (confirm_inject(names[selected])) break;
            }
            continue;
        }
        if (event.type != EV_KEY) continue;
        char c = event.key;
        if (c == '\r' || c == '\n') {
            if (confirm_inject(names[selected])) break;
        } else if (c == 0x1b) {
            char next;
            if (!input_read(&next, 40)) return; // lone Esc
            if (next == '[' || next == 'O') {
                char code;
                if (input_read(&code, 40)) {
                    if (code == 'A' && selected > 0) selected--;
                    if (code == 'B' && selected < count - 1) selected++;
                }
            }
        }
    }

    // Confirmed: become a keyboard and inject
    term_clear();
    term_puts("\x1b[1;35mBAD USB\x1b[0m\r\n\r\nvirando teclado USB...\r\n");
    if (!badusb_hid_begin()) {
        term_puts("\x1b[31mo computador nao reconheceu\r\n"
                  "o teclado (conecte o cabo de dados)\x1b[0m\r\n\r\n");
        term_puts("\x1b[7m-- toque para voltar --\x1b[0m");
        InputEvent e;
        do { input_wait(&e, UINT32_MAX); } while (e.type != EV_TAP && e.type != EV_KEY && e.type != EV_BACK);
        return;
    }
    term_puts("\x1b[32mconectado. digitando...\x1b[0m\r\n");
    term_puts("\x1b[90m(toque para abortar)\x1b[0m\r\n");

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", DIR_PATH, names[selected]);
    int lines = 0;
    int result = run_script(path, &lines);

    if (result < 0) term_puts("\r\n\x1b[33mabortado\x1b[0m\r\n");
    else term_printf("\r\n\x1b[32mfeito: %d linhas\x1b[0m\r\n", lines);
    term_puts("\x1b[90mo USB fica como teclado ate reiniciar\x1b[0m\r\n\r\n");
    term_puts("\x1b[7m-- toque para voltar --\x1b[0m");
    InputEvent e;
    do { input_wait(&e, UINT32_MAX); } while (e.type != EV_TAP && e.type != EV_KEY && e.type != EV_BACK);
}
