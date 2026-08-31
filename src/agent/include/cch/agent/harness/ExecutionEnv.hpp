#pragma once

#include <cch/agent/harness/FileSystem.hpp>
#include <cch/agent/harness/Shell.hpp>

#include <expected>

namespace cch::harness {

/// Temporary expand-contract composition of the complete filesystem and Shell
/// capabilities (ADR 0006). New code should depend on the narrow capability it
/// needs; existing production consumers remain on this composed seam until the
/// later Execution Environment migration.
class AsyncExecutionEnv : public AsyncFileSystem, public AsyncShell {
public:
    virtual ~AsyncExecutionEnv() = default;

    /// Preserve the old composed seam's no-op cleanup for existing fakes. The
    /// canonical AsyncFileSystem contract requires concrete filesystem
    /// adapters to provide their own tracked-resource cleanup.
    [[nodiscard]] support::AsyncResult<void, FileError> cleanup() override {
        return support::AsyncResult<void, FileError>{std::expected<void, FileError>{}};
    }
};

} // namespace cch::harness
