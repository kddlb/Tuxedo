// tuxedod: headless audio daemon. Reads commands from stdin, emits
// events on stdout. Controlled externally.
#include "core/player.hpp"
#include "ipc/stdin_control.hpp"
#include "plugin/registry.hpp"

#include <cstdio>

int main(int, char **) {
	tuxedo::register_builtin_plugins();
	tuxedo::Player player;
	std::puts("tuxedo ready");
	std::fflush(stdout);
	tuxedo::run_stdin_control(player);
	return 0;
}
