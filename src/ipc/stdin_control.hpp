// stdin_control: human-friendly line-based dev console. NOT the real
// IPC — that's the unix socket. This stays around for quick manual
// testing and piped smoke tests.
#pragma once

#include "ipc/controller.hpp"

namespace tuxedo {

void run_stdin_control(Controller &controller);

} // namespace tuxedo
