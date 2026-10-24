#pragma once

#include <cch/agent/harness/session/SessionEntry.hpp>

#include <cch/support/Error.hpp>

#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::harness {

/// A transcript-export run: writes the snapshot, returns the destination.
/// The handler is a stored single operation (CODING_STANDARDS 6.2), so it is
/// move-only; it is not independently copyable.
using TranscriptExportHandler = std::move_only_function<support::Expected<std::string>(std::stop_token)>;

/// Builds the transcript_export handler from the session metadata and active
/// branch captured by its caller. The handler owns those values and the
/// destination is replaced with the same snapshot on every run, so replay is
/// safe after a crash between the write and terminal commit.
[[nodiscard]] TranscriptExportHandler transcript_export_handler(cch::harness::session::SessionMetadata metadata,
        std::vector<cch::harness::session::SessionEntry> branch,
        std::string destination);

} // namespace cch::harness
