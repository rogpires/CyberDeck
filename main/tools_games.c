// Games category
#include "game_invaders.h"
#include "shell.h"

static void cmd_invaders(int argc, char** argv) {
    game_invaders();
}

static const ShellCommand commands[] = {
    {"invaders", "games", NULL, "Space Invaders (seta/espaco ou toque)", cmd_invaders},
};

void tools_games_register(void) {
    shell_register(commands, sizeof(commands) / sizeof(commands[0]));
}
