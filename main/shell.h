#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char* name;
    const char* category; // groups the command in "help"
    const char* usage;    // arguments, shown by "help <command>"
    const char* help;     // one line description
    void (*run)(int argc, char** argv);
} ShellCommand;

// Commands are kept by reference: pass a static array
void shell_register(const ShellCommand* commands, size_t count);

// Runs the local shell forever on the calling task
void shell_run(void);

// Parses and runs one command line (used by the menu)
void shell_run_line(char* line);

// Iterate the registered commands, shell builtins first
int shell_command_count(void);
const ShellCommand* shell_command_at(int index);

// For long running commands: true when Ctrl+C, Esc or 'q' was pressed (does not block)
bool shell_interrupted(void);

// Pager for long output: call shell_pager_start(), then shell_pager_line() after each printed line.
// Pauses when the screen is full; returns false when the user quits.
void shell_pager_start(void);
bool shell_pager_line(void);

// Prints "uso: <name> <usage>"
void shell_print_usage(const char* name);

// Current directory ("/sd" when the card is mounted)
const char* shell_cwd(void);
void shell_set_cwd(const char* path);
// Joins a relative path with the current directory and resolves "." and ".."
void shell_resolve_path(const char* path, char* out, size_t size);
