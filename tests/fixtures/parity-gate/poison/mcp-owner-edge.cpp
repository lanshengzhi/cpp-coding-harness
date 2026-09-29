// Poison source: the cch_tui package reaching into the cch_mcp Owner package,
// which declares no legal Owner dependency (ADR 0065).
#include <cch/mcp/UpstreamTool.hpp>
int tui_render() { return 0; }
