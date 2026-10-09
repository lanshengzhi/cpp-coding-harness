#include "agent/harness/TranscriptExport.hpp"

#include "agent/harness/session/EntrySerializer.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"

#include <utility>

namespace cch::harness {

TaskScheduler::Handler transcript_export_handler(cch::harness::session::SessionStore& store, std::string destination) {
    return [&store, destination = std::move(destination)](std::stop_token token) -> Expected<std::string> {
        if (token.stop_requested()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Transcript export aborted"));
        }
        const cch::harness::session::EntrySerializer serializer;
        std::string transcript;
        for (const auto& entry : store.entries()) {
            auto line = serializer.serialize_entry(entry);
            if (!line) return std::unexpected(line.error());
            transcript += *line;
            transcript.push_back('\n');
        }
        WorkspaceFileSystem filesystem;
        auto written = filesystem.write_file(destination, transcript, true, token);
        if (!written) return std::unexpected(written.error());
        return destination;
    };
}

} // namespace cch::harness
