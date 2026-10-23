// File commands for the microSD card (mounted at /sd)
#include "tools.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_vfs_fat.h"
#include "input.h"
#include "shell.h"
#include "sound.h"
#include "storage.h"
#include "term.h"

#define PATH_LEN 160

static void resolve(const char* path, char* out) {
    shell_resolve_path(path, out, PATH_LEN);
}

static void print_error(const char* what, const char* path) {
    term_printf("\x1b[31m%s %s: %s\x1b[0m\r\n", what, path, strerror(errno));
}

static void format_size(long size, char* out, size_t len) {
    if (size < 1024) snprintf(out, len, "%ldB", size);
    else if (size < 1024 * 1024) snprintf(out, len, "%.1fK", size / 1024.0);
    else snprintf(out, len, "%.1fM", size / (1024.0 * 1024.0));
}

// Pauses every screenful (the shell's pager, which also answers to touch); returns false when the user quits.
// `column` tracks wrapped long lines.
typedef struct {
    int column;
} Pager;

static bool pager_advance(Pager* pager) {
    pager->column = 0;
    return shell_pager_line();
}

static void cmd_pwd(int argc, char** argv) {
    term_printf("%s\r\n", shell_cwd());
}

static void cmd_cd(int argc, char** argv) {
    char path[PATH_LEN];
    resolve(argc > 1 ? argv[1] : SD_MOUNT_POINT, path);
    struct stat st;
    // "/" itself is not a real directory in the VFS, but it is where /sd lives
    if (strcmp(path, "/") != 0 && (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))) {
        term_printf("\x1b[31mcd: %s nao e uma pasta\x1b[0m\r\n", path);
        return;
    }
    shell_set_cwd(path);
}

static void cmd_ls(int argc, char** argv) {
    bool details = argc > 1 && strcmp(argv[1], "-l") == 0;
    const char* target = argc > (details ? 2 : 1) ? argv[details ? 2 : 1] : ".";
    char path[PATH_LEN];
    resolve(target, path);

    if (strcmp(path, "/") == 0) {
        term_printf("\x1b[1;34m%s/\x1b[0m%s\r\n", SD_MOUNT_POINT + 1, storage_is_mounted() ? "" : "  \x1b[90m(sem cartao)\x1b[0m");
        return;
    }
    DIR* dir = opendir(path);
    if (dir == NULL) {
        print_error("ls", path);
        return;
    }

    Pager pager = {0};
    shell_pager_start();
    int column = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        char full[PATH_LEN + sizeof(entry->d_name) + 2];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        struct stat st = {0};
        stat(full, &st);
        bool is_dir = entry->d_type == DT_DIR || S_ISDIR(st.st_mode);

        if (details) {
            char size[16] = "-";
            if (!is_dir) format_size(st.st_size, size, sizeof(size));
            term_printf("%7s  %s%s%s\x1b[0m\r\n", size, is_dir ? "\x1b[1;34m" : "", entry->d_name, is_dir ? "/" : "");
            if (!pager_advance(&pager)) break;
        } else {
            int width = strlen(entry->d_name) + (is_dir ? 1 : 0) + 2;
            if (column > 0 && column + width > term_cols()) {
                term_puts("\r\n");
                column = 0;
            }
            term_printf("%s%s%s\x1b[0m  ", is_dir ? "\x1b[1;34m" : "", entry->d_name, is_dir ? "/" : "");
            column += width;
        }
    }
    closedir(dir);
    if (!details && column > 0) term_puts("\r\n");
}

static void cmd_cat(int argc, char** argv) {
    if (argc < 2) {
        shell_print_usage("cat");
        return;
    }
    char path[PATH_LEN];
    resolve(argv[1], path);
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        print_error("cat", path);
        return;
    }
    Pager pager = {0};
    shell_pager_start();
    int c;
    bool ended_with_newline = true;
    while ((c = fgetc(file)) != EOF) {
        if (c == '\n') {
            term_puts("\r\n");
            if (!pager_advance(&pager)) break;
        } else if (c != '\r') {
            char ch = c;
            term_write(&ch, 1);
            // Long lines wrap: count them for the pager (UTF-8 continuation bytes take no column)
            if ((c & 0xC0) != 0x80 && ++pager.column >= term_cols()) {
                if (!pager_advance(&pager)) break;
            }
        }
        ended_with_newline = c == '\n';
    }
    fclose(file);
    if (!ended_with_newline) term_puts("\r\n");
}

static void cmd_hexdump(int argc, char** argv) {
    if (argc < 2) {
        shell_print_usage("hexdump");
        return;
    }
    char path[PATH_LEN];
    resolve(argv[1], path);
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        print_error("hexdump", path);
        return;
    }
    // 8 bytes per line fits 40 columns; wider fonts get 16
    const int per_line = term_cols() >= 72 ? 16 : 8;
    Pager pager = {0};
    shell_pager_start();
    uint8_t buffer[16];
    size_t read;
    long offset = 0;
    while ((read = fread(buffer, 1, per_line, file)) > 0) {
        term_printf("\x1b[90m%06lx\x1b[0m ", offset);
        for (int i = 0; i < per_line; i++) {
            if (i < (int)read) term_printf("%02x ", buffer[i]);
            else term_puts("   ");
        }
        term_puts("\x1b[36m");
        for (size_t i = 0; i < read; i++) {
            char c = (buffer[i] >= 0x20 && buffer[i] < 0x7f) ? buffer[i] : '.';
            term_write(&c, 1);
        }
        term_puts("\x1b[0m\r\n");
        offset += read;
        if (!pager_advance(&pager)) break;
    }
    fclose(file);
}

static void cmd_write(int argc, char** argv) {
    bool append = argc > 1 && strcmp(argv[1], "-a") == 0;
    int first = append ? 2 : 1;
    if (argc < first + 2) {
        shell_print_usage("write");
        return;
    }
    char path[PATH_LEN];
    resolve(argv[first], path);
    FILE* file = fopen(path, append ? "a" : "w");
    if (file == NULL) {
        print_error("write", path);
        return;
    }
    for (int i = first + 1; i < argc; i++) {
        fputs(argv[i], file);
        fputc(i + 1 < argc ? ' ' : '\n', file);
    }
    fclose(file);
    sound_play(SOUND_WRITE);
}

static void cmd_touch(int argc, char** argv) {
    if (argc < 2) {
        shell_print_usage("touch");
        return;
    }
    char path[PATH_LEN];
    resolve(argv[1], path);
    FILE* file = fopen(path, "a");
    if (file == NULL) {
        print_error("touch", path);
        return;
    }
    fclose(file);
}

static void cmd_mkdir(int argc, char** argv) {
    if (argc < 2) {
        shell_print_usage("mkdir");
        return;
    }
    char path[PATH_LEN];
    resolve(argv[1], path);
    if (mkdir(path, 0775) != 0) print_error("mkdir", path);
}

static void cmd_rm(int argc, char** argv) {
    if (argc < 2) {
        shell_print_usage("rm");
        return;
    }
    for (int i = 1; i < argc; i++) {
        char path[PATH_LEN];
        resolve(argv[i], path);
        struct stat st;
        if (stat(path, &st) != 0) {
            print_error("rm", path);
        } else if (S_ISDIR(st.st_mode) ? rmdir(path) != 0 : unlink(path) != 0) {
            print_error("rm", path);
        }
    }
}

static void cmd_mv(int argc, char** argv) {
    if (argc < 3) {
        shell_print_usage("mv");
        return;
    }
    char from[PATH_LEN], to[PATH_LEN];
    resolve(argv[1], from);
    resolve(argv[2], to);
    struct stat st;
    // Moving into a folder keeps the file name
    if (stat(to, &st) == 0 && S_ISDIR(st.st_mode)) {
        const char* name = strrchr(from, '/');
        strlcat(to, name, sizeof(to));
    }
    if (rename(from, to) != 0) print_error("mv", from);
}

static void cmd_df(int argc, char** argv) {
    uint64_t total = 0, free_bytes = 0;
    if (!storage_is_mounted() || esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_bytes) != ESP_OK) {
        term_puts("sem cartao sd\r\n");
        return;
    }
    double total_gb = total / (1024.0 * 1024.0 * 1024.0);
    double used_gb = (total - free_bytes) / (1024.0 * 1024.0 * 1024.0);
    int width = term_cols() - 16;
    if (width > 30) width = 30;
    int filled = total ? (int)((total - free_bytes) * width / total) : 0;
    term_printf("/sd  %.2f/%.2f GB\r\n[", used_gb, total_gb);
    for (int i = 0; i < width; i++) term_puts(i < filled ? "\x1b[36m█" : "\x1b[90m░");
    term_printf("\x1b[0m] %d%%\r\n", total ? (int)((total - free_bytes) * 100 / total) : 0);
}

static const ShellCommand commands[] = {
    {"ls",      "arquivos", "[-l] [pasta]",          "lista arquivos",                      cmd_ls},
    {"cd",      "arquivos", "[pasta]",               "muda de pasta (sem nada: /sd)",       cmd_cd},
    {"pwd",     "arquivos", NULL,                    "pasta atual",                         cmd_pwd},
    {"cat",     "arquivos", "<arquivo>",             "mostra o arquivo",                    cmd_cat},
    {"hexdump", "arquivos", "<arquivo>",             "mostra o arquivo em hexadecimal",     cmd_hexdump},
    {"write",   "arquivos", "[-a] <arquivo> <texto>", "grava texto (-a acrescenta)",        cmd_write},
    {"touch",   "arquivos", "<arquivo>",             "cria arquivo vazio",                  cmd_touch},
    {"mkdir",   "arquivos", "<pasta>",               "cria pasta",                          cmd_mkdir},
    {"rm",      "arquivos", "<arquivo|pasta vazia>...", "apaga",                            cmd_rm},
    {"mv",      "arquivos", "<origem> <destino>",    "move ou renomeia",                    cmd_mv},
    {"df",      "arquivos", NULL,                    "espaco no cartao",                    cmd_df},
};

void tools_fs_register(void) {
    shell_register(commands, sizeof(commands) / sizeof(commands[0]));
    if (storage_is_mounted()) shell_set_cwd(SD_MOUNT_POINT);
}
