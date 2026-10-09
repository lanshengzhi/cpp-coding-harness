#pragma once

#include "agent/harness/TaskScheduler.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>

#include <string>

namespace cch::harness {

/// Builds the durable transcript_export handler. The destination is replaced
/// with the same snapshot on every run, so a crash after the write and before
/// terminal commit is safe to recover by replay.
[[nodiscard]] TaskScheduler::Handler transcript_export_handler(
        cch::harness::session::SessionStore& store, std::string destination);

} // namespace cch::harness
