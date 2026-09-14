// Minimal fixture source for the cch_coding_agent frontend implementation
// target. It compiles terminal presentation and may include the cch_tui Owner
// Interface that the headless owner library itself must not reach
// (manifest `implementation_owner_dependencies`, issue #658).
#include <cch/tui/Render.hpp>
int frontend_tui_render() { return 0; }
