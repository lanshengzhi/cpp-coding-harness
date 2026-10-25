#include "TranscriptExport.hpp"

#include "coding_agent/session/EntrySerializer.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"

#include <optional>
#include <utility>

namespace cch::harness {

TranscriptExportHandler transcript_export_handler(cch::harness::session::SessionMetadata metadata,
        std::vector<cch::harness::session::SessionEntry> branch,
        std::string destination) {
    return [metadata = std::move(metadata), branch = std::move(branch), destination = std::move(destination)](
                   std::stop_token token) -> support::Expected<std::string> {
        if (token.stop_requested()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "Transcript export aborted"));
        }
        const cch::harness::session::EntrySerializer serializer;

        // pi `serializeSessionBranch` pushes the SessionHeader before any
        // entry, so the exported file is a session file the loader accepts,
        // not a headerless transcript dump.
        auto header = serializer.serialize_header(metadata);
        if (!header) return std::unexpected(header.error());
        std::string transcript = *header;
        transcript += '\n';

        // pi exports `getBranch()` — the active leaf path — not `getEntries()`.
        // `get_branch` walks leaf-to-root (SessionStore.hpp); pi's `getBranch`
        // reverses the same walk, so the written order is root-to-leaf.
        // pi rewrites every written line's parentId to the previous written
        // entry's id (`entries.push({ ...entry, parentId }); parentId = entry.id`),
        // flattening the branch into one linear chain that starts at null. The
        // copy is what overwrites a stored parent rather than emitting both.
        std::optional<std::string> parent_id;
        for (auto entry = branch.rbegin(); entry != branch.rend(); ++entry) {
            auto chained = *entry;
            chained.parent_id = session::parent_id_from_optional(parent_id);
            auto line = serializer.serialize_entry(chained);
            if (!line) return std::unexpected(line.error());
            // `serialize_entry` already terminates the wire line, so appending
            // another newline here would emit a blank line after every entry.
            transcript += *line;
            parent_id = entry->entry_id;
        }

        WorkspaceFileSystem filesystem;
        auto written = filesystem.write_file(destination, transcript, true, token);
        if (!written) return std::unexpected(written.error());
        return destination;
    };
}

} // namespace cch::harness
