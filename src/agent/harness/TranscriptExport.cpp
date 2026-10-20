#include "agent/harness/TranscriptExport.hpp"

#include "agent/harness/session/EntrySerializer.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"

#include <utility>

namespace cch::harness {

TranscriptExportHandler transcript_export_handler(cch::harness::session::SessionStore& store, std::string destination) {
    return [&store, destination = std::move(destination)](std::stop_token token) -> support::Expected<std::string> {
        if (token.stop_requested()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Transcript export aborted"));
        }
        const cch::harness::session::EntrySerializer serializer;
        std::string transcript;
        for (const auto& entry : store.entries()) {
            auto line = serializer.serialize_entry(entry);
            if (!line) return std::unexpected(line.error());
            // `serialize_entry` already terminates the wire line, so appending
            // another newline here would emit a blank line after every entry.
            transcript += *line;
        }
        WorkspaceFileSystem filesystem;
        auto written = filesystem.write_file(destination, transcript, true, token);
        if (!written) return std::unexpected(written.error());
        return destination;
    };
}

} // namespace cch::harness
