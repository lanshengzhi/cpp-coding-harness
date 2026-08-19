#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <type_traits>

using namespace cch;

TEST_CASE(
    "in-memory Session Store accepts Runtime appends and has no path",
    "[harness][session][in-memory]") {
    auto store = harness::session::SessionStore::in_memory(
        harness::session::SessionMetadata{
            .session_id = "in-memory-test",
            .created_at = "2026-07-05T00:00:00Z",
            .workspace = {},
            .provider = "fake",
            .model = "fake-model",
        });

    CHECK_FALSE(store.path().has_value());
    CHECK(store.append(ai::MessageVariant{ai::user_text_message("hello")}).has_value());

    ai::AssistantMessage assistant;
    assistant.content.emplace_back(ai::TextContent{"response", std::nullopt});
    CHECK(store.append(ai::MessageVariant{std::move(assistant)}).has_value());
    CHECK_FALSE(store.path().has_value());

    // Appends maintain the live in-memory tree: both messages reconstruct.
    CHECK(store.build_context().messages.size() == 2);

    using StorePath = decltype(std::declval<const harness::session::SessionStore&>().path());
    static_assert(std::is_same_v<StorePath, std::optional<std::filesystem::path>>);
}
