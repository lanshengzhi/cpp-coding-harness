#pragma once

#include <cch/support/Error.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent {

/// One persisted trust store file's whole contents: the flat JSON object the
/// pi `trust.json` shape defines, mapping each key — a canonicalized project
/// path for project trust, a Server Id for Upstream MCP Server trust — to its
/// boolean decision. A key whose value is absent carries no decision and is
/// dropped by the next write.
using TrustStoreMap = std::map<std::string, std::optional<bool>>;

/// A failure of the shared trust-store file mechanics. `ErrorCode::Validation`
/// because a caller resolves it into its own fail-closed trust diagnostic
/// rather than surfacing it as an I/O fault.
[[nodiscard]] support::Error trust_store_error(std::string message, std::string detail = {});

/// Read one trust store file. A missing file is an empty map, not a failure;
/// every other problem — unreadable, symlinked, group- or world-writable, not
/// a regular file, not a JSON object, or malformed — is an error, so a caller
/// resolves it as an unavailable store and denies by default. `store_name`
/// names the store in the diagnostics ("trust store", "MCP server trust
/// store").
[[nodiscard]] support::Expected<TrustStoreMap> read_trust_store_map(
        const std::filesystem::path& path, std::string_view store_name);

/// Write one trust store file atomically, with owner-only permissions and a
/// fresh `0700` parent directory.
[[nodiscard]] support::ExpectedVoid write_trust_store_map(
        const std::filesystem::path& path, const TrustStoreMap& data, std::string_view store_name);

} // namespace cch::coding_agent
