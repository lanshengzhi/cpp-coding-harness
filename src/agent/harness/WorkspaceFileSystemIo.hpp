#pragma once

#include <cch/agent/harness/FileSystem.hpp>
#include "agent/harness/WorkspaceFileSystemErrors.hpp"

#include <stop_token>
#include <string>
#include <utility>

#include <unistd.h>

namespace cch::harness {

/// The shared raw `::read` loop behind the bounded pi-shaped reads: the
/// stop check, EINTR retry, EOF, and the read-failure diagnostic run once,
/// while the caller's chunk handler owns its own per-chunk accounting.
/// A chunk handler returns `void` to keep reading or fails the read with
/// its own FileError.
template <typename OnChunk>
[[nodiscard]] std::expected<void, FileError> read_file_chunks(
        int file_fd, const std::string& path, std::stop_token stop_token, OnChunk&& on_chunk) {
    char buffer[4096];
    for (;;) {
        if (stop_token.stop_requested()) {
            return std::unexpected(operation_aborted_error(path));
        }
        const auto count = ::read(file_fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(FileError{
                    .code = FileErrorCode::Unknown,
                    .message = "could not read file: " + path,
                    .path = std::string{path},
            });
        }
        if (count == 0) {
            return {};
        }
        if (auto chunk = on_chunk(buffer, static_cast<std::size_t>(count)); !chunk) {
            return std::unexpected(chunk.error());
        }
    }
}

} // namespace cch::harness
