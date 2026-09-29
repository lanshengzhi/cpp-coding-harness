#pragma once

// Poison case for the Owner Interface standalone-compile evidence (issue
// #834): a third-party Boost.Asio header must not resolve, because the
// standalone compile provides no third-party include path. The MCP Host Owner
// Interface carries passive value contracts only, never an Asio type
// (ADR 0042, ADR 0046, ADR 0065).

#include <boost/asio.hpp>
