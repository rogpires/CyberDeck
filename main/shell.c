#include "shell.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "osk.h"
#include "sound.h"
#include "term.h"

#define LINE_MAX      160
#define MAX_ARGS      12
#define HISTORY_SIZE  16
#define MAX_GROUPS    16
#define PATH_MAX_LEN  128

typedef struct {
    const ShellCommand* commands;
    size_t count;
} CommandGroup;

static CommandGroup groups[MAX_GROUPS];
static int group_count;
static char history[HISTORY_SIZE][LINE_MAX];
static int history_count;
static char cwd[PATH_MAX_LEN] = "/";

static const ShellCommand builtins[4];
#define builtin_count (sizeof(builtins) / sizeof(builtins[0]))

void shell_register(const ShellCommand* commands, size_t count) {
    // The shell's own commands come first in "help"
    if (group_count == 0 && commands != builtins) {
        shell_register(builtins, builtin_count);
    }
    if (group_count < MAX_GROUPS) {
        groups[group_count++] = (CommandGroup) {commands, count};
    }
}

static const ShellCommand* find_command(const char* name) {
    for (int g = 0; g < group_count; g++) {
        for (size_t i = 0; i < groups[g].count; i++) {
            if (strcmp(groups[g].commands[i].name, name) == 0) return &groups[g].commands[i];
        }
    }
    return NULL;
}

void shell_print_usage(const char* name) {
    const ShellCommand* command = find_command(name);
    if (command != NULL) {
        term_printf("uso: %s %s\r\n", command->name, command->usage ? command->usage : "");
    }
}

bool shell_interrupted(void) {
    InputEvent event;
    while (input_wait(&event, 0)) {
        if (event.type == EV_BACK || event.type == EV_TAP) return true;
        if (event.type == EV_KEY && (event.key == 0x03 || event.key == 0x1b || event.key == 'q')) return true;
    }
    return false;
}

const char* shell_cwd(void) {
    return cwd;
}

void shell_set_cwd(const char* path) {
    strlcpy(cwd, path, sizeof(cwd));
}

void shell_resolve_path(const char* path, char* out, size_t size) {
    char joined[PATH_MAX_LEN * 2];
    if (path[0] == '/') {
        strlcpy(joined, path, sizeof(joined));
    } else {
        snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
    }

    // Rebuild the path segment by segment, dropping "." and applying ".."
    char result[PATH_MAX_LEN * 2] = "";
    size_t len = 0;
    char* save = NULL;
    for (char* part = strtok_r(joined, "/", &save); part != NULL; part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) {
            char* slash = strrchr(result, '/');
            if (slash != NULL) *slash = 0;
            len = strlen(result);
            continue;
        }
        len += snprintf(result + len, sizeof(result) - len, "/%s", part);
    }
    strlcpy(out, len == 0 ? "/" : result, size);
}

static int pager_lines;

void shell_pager_start(void) {
    pager_lines = 0;
}

bool shell_pager_line(void) {
    if (++pager_lines < term_rows() - 1) return true;
    term_puts("\x1b[7m-- mais: toque/enter, q sai --\x1b[0m");
    bool more = true;
    while (true) {
        InputEvent event;
        input_wait(&event, UINT32_MAX);
        if (event.type == EV_BACK) {
            more = false;
        } else if (event.type == EV_KEY) {
            more = !(event.key == 'q' || event.key == 0x03 || event.key == 0x1b);
        } else if (event.type != EV_TAP) {
            continue; // swipes and the keyboard button don't answer the pager
        }
        break;
    }
    term_puts("\r\x1b[K");
    pager_lines = 0;
    return more;
}

// Builtins

static void cmd_help(int argc, char** argv) {
    if (argc > 1) {
        const ShellCommand* command = find_command(argv[1]);
        if (command == NULL) {
            term_printf("\x1b[31m%s: comando nao encontrado\x1b[0m\r\n", argv[1]);
            return;
        }
        term_printf("\x1b[1m%s\x1b[0m %s\r\n  %s\r\n", command->name, command->usage ? command->usage : "", command->help);
        return;
    }

    // Commands grouped by category, names wrapped to the screen width, a screenful at a time
    shell_pager_start();
    const char* printed[MAX_GROUPS * 4];
    int printed_count = 0;
    for (int g = 0; g < group_count; g++) {
        for (size_t i = 0; i < groups[g].count; i++) {
            const char* category = groups[g].commands[i].category;
            bool seen = false;
            for (int p = 0; p < printed_count; p++) seen |= strcmp(printed[p], category) == 0;
            if (seen || printed_count >= (int)(sizeof(printed) / sizeof(printed[0]))) continue;
            printed[printed_count++] = category;

            term_printf("\x1b[36m%s\x1b[0m\r\n", category);
            if (!shell_pager_line()) return;
            term_puts(" ");
            int column = 1;
            for (int g2 = 0; g2 < group_count; g2++) {
                for (size_t j = 0; j < groups[g2].count; j++) {
                    const ShellCommand* command = &groups[g2].commands[j];
                    if (strcmp(command->category, category) != 0) continue;
                    int width = strlen(command->name) + 1;
                    if (column + width > term_cols()) {
                        term_puts("\r\n");
                        if (!shell_pager_line()) return;
                        term_puts(" ");
                        column = 1;
                    }
                    term_printf("%s ", command->name);
                    column += width;
                }
            }
            term_puts("\r\n");
            if (!shell_pager_line()) return;
        }
    }
    term_puts("\x1b[90mhelp <comando>: detalhes | PgUp: rolar\x1b[0m\r\n");
}

static void cmd_clear(int argc, char** argv) {
    term_clear();
}

static void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        term_printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    }
    term_puts("\r\n");
}

static void cmd_history(int argc, char** argv) {
    int oldest = history_count > HISTORY_SIZE ? history_count - HISTORY_SIZE : 0;
    for (int i = oldest; i < history_count; i++) {
        term_printf("%3d  %s\r\n", i + 1, history[i % HISTORY_SIZE]);
    }
}

static const ShellCommand builtins[4] = {
    {"help",    "shell", "[comando]", "lista os comandos ou mostra detalhes de um", cmd_help},
    {"clear",   "shell", NULL,        "limpa a tela (ou Ctrl+L)",                   cmd_clear},
    {"echo",    "shell", "<texto>",   "mostra o texto",                             cmd_echo},
    {"history", "shell", NULL,        "ultimos comandos (setas sobem/descem)",      cmd_history},
};

// Line editing

// Splits in place, honoring "double quotes"
static int parse_args(char* line, char** argv) {
    int argc = 0;
    char* p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
        }
        if (*p) *p++ = 0;
    }
    return argc;
}

// Iterate all registered commands (shell builtins first)
int shell_command_count(void) {
    int total = 0;
    for (int g = 0; g < group_count; g++) total += groups[g].count;
    return total;
}

const ShellCommand* shell_command_at(int index) {
    for (int g = 0; g < group_count; g++) {
        if (index < (int)groups[g].count) return &groups[g].commands[index];
        index -= groups[g].count;
    }
    return NULL;
}

void shell_run_line(char* line) {
    char* argv[MAX_ARGS];
    int argc = parse_args(line, argv);
    if (argc == 0) return;
    const ShellCommand* command = find_command(argv[0]);
    if (command == NULL) {
        sound_play(SOUND_ERROR);
        term_printf("\x1b[31m%s: comando nao encontrado\x1b[0m (digite help)\r\n", argv[0]);
        return;
    }
    command->run(argc, argv);
    // Leftover Ctrl+C/q presses and taps from an interrupted command must not reach the next prompt
    input_flush();
}

static void execute(char* line) {
    shell_run_line(line);
}

static void prompt(void) {
    term_printf("\x1b[1;32mdeck\x1b[0m:\x1b[1;34m%s\x1b[0m$ ", cwd);
}

static void replace_line(char* line, int* len, const char* text) {
    while (*len > 0) {
        (*len)--;
        // One column per character: UTF-8 continuation bytes take no space
        if (((unsigned char)line[*len] & 0xC0) != 0x80) term_puts("\b \b");
    }
    strlcpy(line, text, LINE_MAX);
    *len = strlen(line);
    term_puts(line);
}

static void history_add(const char* line) {
    if (line[0] == 0) return;
    if (history_count > 0 && strcmp(history[(history_count - 1) % HISTORY_SIZE], line) == 0) return;
    strlcpy(history[history_count % HISTORY_SIZE], line, LINE_MAX);
    history_count++;
}

static bool read_line(char* line) {
    int len = 0;
    int history_index = history_count;
    int escape = 0; // 0 none, 1 after ESC, 2 after ESC [
    char escape_params[8];
    int escape_len = 0;
    char previous = 0;
    line[0] = 0;

    while (true) {
        InputEvent event;
        input_wait(&event, UINT32_MAX);
        switch (event.type) {
            case EV_BACK:
                term_puts("\r\n");
                line[0] = 0;
                return true;
            case EV_KEYBOARD:
                osk_toggle();
                continue;
            case EV_SWIPE_UP:   // finger up: toward the newest lines
                term_scroll(-(term_rows() / 2));
                continue;
            case EV_SWIPE_DOWN: // finger down: back in time
                term_scroll(term_rows() / 2);
                continue;
            case EV_KEY:
                break;
            default:
                continue;
        }
        char c = event.key;

        if (escape == 1) {
            escape = (c == '[' || c == 'O') ? 2 : 0;
            escape_len = 0;
            continue;
        }
        if (escape == 2) {
            if ((c >= '0' && c <= '9') || c == ';') { // e.g. "\x1b[5~" or "\x1b[1;2A"
                if (escape_len < (int)sizeof(escape_params) - 1) escape_params[escape_len++] = c;
                continue;
            }
            escape_params[escape_len] = 0;
            escape = 0;
            const int page = term_rows() - 1;
            if (c == '~' && strcmp(escape_params, "5") == 0) { // Page Up
                term_scroll(page);
                continue;
            }
            if (c == '~' && strcmp(escape_params, "6") == 0) { // Page Down
                term_scroll(-page);
                continue;
            }
            if (strcmp(escape_params, "1;2") == 0 && (c == 'A' || c == 'B')) { // Shift+Up/Down
                term_scroll(c == 'A' ? 1 : -1);
                continue;
            }
            int oldest = history_count > HISTORY_SIZE ? history_count - HISTORY_SIZE : 0;
            if (c == 'A' && history_index > oldest) {
                history_index--;
                replace_line(line, &len, history[history_index % HISTORY_SIZE]);
            } else if (c == 'B' && history_index < history_count) {
                history_index++;
                replace_line(line, &len, history_index == history_count ? "" : history[history_index % HISTORY_SIZE]);
            }
            continue;
        }

        if (c == '\n' && previous == '\r') {
            previous = c;
            continue; // CR LF from the serial console
        }
        previous = c;

        if (c == '\x1b') {
            escape = 1;
        } else if (c == '\r' || c == '\n') {
            term_puts("\r\n");
            line[len] = 0;
            return false;
        } else if (c == 0x7f || c == '\b') {
            if (len > 0) {
                // Removes a whole UTF-8 character: continuation bytes, then the lead byte
                while (len > 0 && ((unsigned char)line[len - 1] & 0xC0) == 0x80) len--;
                if (len > 0) len--;
                line[len] = 0;
                term_puts("\b \b");
            }
        } else if (c == 0x03) { // Ctrl+C
            term_puts("^C\r\n");
            line[0] = 0;
            return false;
        } else if (c == 0x15) { // Ctrl+U
            replace_line(line, &len, "");
        } else if (c == 0x0c) { // Ctrl+L
            term_clear();
            prompt();
            term_write(line, len);
        } else if ((unsigned char)c >= 0x20 && (unsigned char)c != 0x7f && len < LINE_MAX - 1) {
            line[len++] = c;
            line[len] = 0;
            term_write(&c, 1);
        }
    }
}

void shell_run(void) {
    if (group_count == 0) shell_register(builtins, builtin_count);
    char line[LINE_MAX];
    while (true) {
        prompt();
        if (read_line(line)) {
            // "< MENU": the menu returns here when Terminal is chosen
            strlcpy(line, "menu", sizeof(line));
            execute(line);
            continue;
        }
        history_add(line);
        execute(line);
    }
}
