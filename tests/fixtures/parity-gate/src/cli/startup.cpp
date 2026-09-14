// Minimal fixture source for the cch_coding_agent CLI frontend target. Like
// frontend_tui it is an `implementation` target of the owner library, so it
// may include the cch_tui Owner Interface the headless library must not
// (manifest `implementation_owner_dependencies`, issue #658).
#include <cch/tui/Render.hpp>
int frontend_cli_startup() { return 0; }
