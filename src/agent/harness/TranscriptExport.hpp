#pragma once

#include <cch/agent/harness/session/SessionStore.hpp>

#include <cch/support/Error.hpp>

#include <functional>
#include <stop_token>
#include <string>

namespace cch::harness {

/// A transcript-export run: writes the snapshot, returns the destination.
using TranscriptExportHandler = std::function<support::Expected<std::string>(std::stop_token)>;

/// Builds the transcript_export handler. The destination is replaced with the
/// same snapshot on every run, so a crash after the write and before terminal
/// commit is safe to recover by replay.
[[nodiscard]] TranscriptExportHandler transcript_export_handler(
        cch::harness::session::SessionStore& store, std::string destination);

} // namespace cch::harness
