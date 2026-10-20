#pragma once

#include <cch/chord/Delta.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::chord::detail {

/// The frozen reserved keys reach the prototype chain in JavaScript. C++ has
/// no prototype chain, but the same paths remain invalid so a batch means the
/// same thing on both sides.
[[nodiscard]] bool is_reserved_segment(std::string_view key);

[[nodiscard]] bool path_is_prefix(const Path& prefix, const Path& path);

[[nodiscard]] support::ExpectedVoid validate_safe_path(const Path& path);

[[nodiscard]] support::ExpectedVoid validate_permutation(const std::vector<std::size_t>& permutation);

/// Look up a path; `std::nullopt` when any segment is absent or the containing
/// value has the wrong shape. A present JSON null is a value, not a miss.
[[nodiscard]] std::optional<Json> try_get_path(const Json& root, const Path& path);

[[nodiscard]] support::Expected<Json> get_path(const Json& root, const Path& path);

[[nodiscard]] support::Expected<Json> set_path(const Json& root, const Path& path, Json value, std::size_t offset = 0);

/// Erase one object key or array element. `changed` is false for a missing
/// object key (a delete of an absent key is a no-op); an absent array element
/// or intermediate path is an error.
[[nodiscard]] support::Expected<Json> erase_path(
        const Json& root, const Path& path, bool& changed, std::size_t offset = 0);

[[nodiscard]] support::Expected<Json> append_string_path(const Json& root, const Path& path, std::string_view text);

[[nodiscard]] support::Expected<Json> truncate_string_path(const Json& root, const Path& path, std::size_t count);

[[nodiscard]] support::Expected<Json> splice_path(const Json& root,
        const Path& path,
        std::size_t index,
        std::size_t remove,
        const std::vector<Json>& items,
        std::size_t& effective_index,
        std::size_t& effective_remove);

[[nodiscard]] support::Expected<Json> move_path(
        const Json& root, const Path& path, const std::vector<std::size_t>& permutation);

[[nodiscard]] bool path_is_identity_permutation(const std::vector<std::size_t>& permutation);

/// Append the ops that turn one string value into another at `path`: append,
/// truncate, or a whole set when no bounded suffix/prefix run exists.
void emit_string_change(const std::string& before, const std::string& after, const Path& path, OpBatch& operations);

} // namespace cch::chord::detail
