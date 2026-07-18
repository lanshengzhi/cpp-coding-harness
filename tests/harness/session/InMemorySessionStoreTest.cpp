#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "harness/session/InMemorySessionStore.hpp"

#include "../../../include/cch/ai/Content.hpp"
#include "../../../include/cch/ai/Message.hpp"
#include "../../../include/cch/harness/session/SessionStore.hpp"

#include <filesystem>
#include <optional>
#include <type_traits>

using namespace cch;

TEST_CASE(
    "in-memory Session Store accepts Runtime appends and has no path",
    "[harness][session][in-memory]") {
    harness::session::InMemorySessionStore concrete;
    harness::session::SessionStore& store = concrete;

    CHECK_FALSE(store.path().has_value());
    CHECK(store.append(ai::MessageVariant{ai::user_text_message("hello")}).has_value());

    ai::AssistantMessage assistant;
    assistant.content.emplace_back(ai::TextContent{"response", std::nullopt});
    CHECK(store.append(ai::MessageVariant{std::move(assistant)}).has_value());
    CHECK_FALSE(store.path().has_value());

    using StorePath = decltype(std::declval<const harness::session::SessionStore&>().path());
    static_assert(std::is_same_v<StorePath, std::optional<std::filesystem::path>>);
}
