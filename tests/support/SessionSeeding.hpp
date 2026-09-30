#pragma once

#include "agent/harness/session/EntrySerializer.hpp"
#include "agent/harness/session/JsonlSessionStore.hpp"

#include <cch/ai/Message.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace cch::tests {

/// The flushing user message: one fixed-timestamp text turn, which keeps the
/// seeded transcript deterministic.
[[nodiscard]] inline ai::MessageVariant flush_message(std::string_view user_text) {
    return ai::MessageVariant{ai::user_text_message(std::string{user_text}, 1'750'000'000'000)};
}

/// ADR 0064 defers the session file to the first user or assistant message:
/// a test that needs previously accepted entries on disk appends one user
/// message to flush them.
inline void flush_session_store(
    harness::session::SessionStore& store,
    std::string_view user_text = "seed") {
    REQUIRE(store.append(flush_message(user_text)).has_value());
}

/// JsonlSessionStore overload (AppendResult instead of the facade Expected).
inline void flush_session_store(
    harness::session::JsonlSessionStore& store,
    std::string_view user_text = "seed") {
    REQUIRE(store.append(flush_message(user_text)).status);
}

/// A "prior session" transcript: the header metadata plus one flushing user
/// message — exactly what ADR 0064 leaves on disk for a session that
/// prompted. Tests needing extra entries (model_change, thinking) create the
/// store themselves and finish with `flush_session_store`.
inline void seed_prior_session_file(
    const std::filesystem::path& path,
    harness::session::SessionMetadata metadata,
    std::string_view user_text = "seed") {
    auto store = harness::session::SessionStore::create_new(path, std::move(metadata));
    REQUIRE(store.has_value());
    flush_session_store(*store, user_text);
}

/// The serialized v3 session header line from the product's serializer — a
/// hand-written wire copy can drift from it (§16.1).
inline std::string session_header_line(const harness::session::SessionMetadata& metadata) {
    auto header = harness::session::EntrySerializer{}.serialize_header(metadata);
    REQUIRE(header.has_value());
    return *header;
}

/// Session files are owner-only; the resume and open paths refuse a loose
/// mode.
inline void make_session_file_private(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        ec);
    REQUIRE(!ec);
}

} // namespace cch::tests
