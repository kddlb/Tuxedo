// stdin_control: line-based command parser driving a Player.
// This is the MVP "IPC" layer. A socket-based variant will replace it.
#pragma once

#include "core/player.hpp"

namespace tuxedo {

// Reads lines from stdin, dispatches them to `player`. Returns when
// stdin closes or a "quit" command is received.
void run_stdin_control(Player &player);

} // namespace tuxedo
