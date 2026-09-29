#pragma once

// Poison case for the Owner Interface standalone-compile evidence (issue
// #834): exception types never cross the MCP Host Owner Interface, and the
// package compiles under the project's strict no-exception policy (ADR 0042,
// ADR 0046, ADR 0065). The standalone compile passes `-fno-exceptions`, so a
// header that catches one cannot build.

#include <exception>

namespace cch::tests::owner_interface {

inline bool catches_in_owner_interface() {
    try {
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace cch::tests::owner_interface
