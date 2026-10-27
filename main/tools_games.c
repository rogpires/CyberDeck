// Games category
#include "game_enduro.h"
#include "game_invaders.h"
#include "shell.h"

static void cmd_invaders(int argc, char** argv) {
    game_invaders();
}

static void cmd_enduro(int argc, char** argv) {
    game_enduro();
}

static const ShellCommand commands[] = {
    {"invaders", "games", NULL, "Space Invaders (seta/espaco ou toque)", cmd_invaders},
    {"enduro",   "games", NULL, "Enduro: corrida (seta/dedo)", cmd_enduro},
};

void tools_games_register(void) {
    shell_register(commands, sizeof(commands) / sizeof(commands[0]));
}
