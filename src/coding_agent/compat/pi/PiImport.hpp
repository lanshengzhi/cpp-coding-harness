#pragma once

#include <cch/support/Error.hpp>

#include <cstddef>
#include <filesystem>

namespace cch::coding_agent::compat::pi {

/// Explicit source and destination for the one-time pi state import.
///
/// The source is read-only: import never modifies or deletes pi state. The
/// destination must not exist, so a failed or repeated import cannot overwrite
/// product-owned state.
struct ImportOptions {
    std::filesystem::path source_directory;
    std::filesystem::path destination_directory;
};

/// Counts the files and directories copied by a successful import.
struct ImportReport {
    std::size_t files_copied{0};
    std::size_t directories_copied{0};
};

/// The historical pi state root used by the explicit importer only. Runtime
/// code must use `coding_agent::agent_config_dir()` and never call this helper.
[[nodiscard]] std::filesystem::path default_source_directory();

/// Copy one pi config/session tree into the product namespace without
/// overwriting either source or an existing destination. Symlinks and special
/// files are rejected rather than followed.
[[nodiscard]] support::Expected<ImportReport> import_state(ImportOptions options);

} // namespace cch::coding_agent::compat::pi
