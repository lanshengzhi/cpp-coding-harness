#include "coding_agent/runtime/SessionEventCommitment.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;
namespace runtime = cch::coding_agent::runtime;

namespace {

ai::UserMessage user_msg(std::string text) {
    return ai::user_text_message(std::move(text));
}

class FakeStore final : public harness::session::SessionStore {
public:
    std::vector<ai::MessageVariant> appended;
    int appends{0};
    int fail_on_append{0};

    util::ExpectedVoid append(const ai::MessageVariant& message) override {
        ++appends;
        if (fail_on_append > 0 && appends >= fail_on_append) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Session,
                "could not persist session entry",
                "failure " + std::to_string(appends)));
        }
        appended.push_back(message);
        return {};
    }

    std::optional<std::filesystem::path> path() const override {
        return std::nullopt;
    }
};

} // namespace

TEST_CASE(
    "Session Event Commitment persists only completed Session Entry messages",
    "[coding-agent][runtime][commitment][issue36]") {
    FakeStore store;
    runtime::SessionEventCommitment commitment{store};
    auto sink = commitment.sink();

    REQUIRE(sink(agent::AgentStartEvent{}).has_value());
    REQUIRE(sink(agent::MessageStartEvent{user_msg("draft")}).has_value());

    ai::BashExecutionMessage bash;
    bash.command = "ls";
    REQUIRE(sink(agent::MessageEndEvent{std::move(bash)}).has_value());
    CHECK(store.appended.empty());

    REQUIRE(sink(agent::MessageEndEvent{user_msg("hello")}).has_value());
    REQUIRE(store.appended.size() == 1);
    CHECK(std::holds_alternative<ai::UserMessage>(store.appended.front()));
    CHECK(commitment.conclude(util::ExpectedVoid{}).has_value());
}

TEST_CASE(
    "Session Event Commitment returns the first persistence failure unwrapped",
    "[coding-agent][runtime][commitment][issue36]") {
    FakeStore store;
    store.fail_on_append = 1;
    runtime::SessionEventCommitment commitment{store};
    auto sink = commitment.sink();

    auto first = sink(agent::MessageEndEvent{user_msg("one")});
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().code == util::ErrorCode::Session);
    CHECK(first.error().message == "could not persist session entry");
    CHECK(first.error().detail == "failure 1");

    auto second = sink(agent::MessageEndEvent{user_msg("two")});
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().detail == "failure 2");

    util::ExpectedVoid wrapped = std::unexpected(util::make_error(
        util::ErrorCode::Unknown,
        "agent event commitment failed"));
    auto verdict = commitment.conclude(std::move(wrapped));
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == util::ErrorCode::Session);
    CHECK(verdict.error().message == "could not persist session entry");
    CHECK(verdict.error().detail == "failure 1");
}

TEST_CASE(
    "Session Event Commitment reports an unfinished Agent prompt",
    "[coding-agent][runtime][commitment][issue36]") {
    FakeStore store;
    runtime::SessionEventCommitment commitment{store};

    auto verdict = commitment.conclude(std::nullopt);
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == util::ErrorCode::Unknown);
    CHECK(verdict.error().message == "stateful Agent prompt did not finish");
}

TEST_CASE(
    "Session Event Commitment passes through Agent failures when storage succeeded",
    "[coding-agent][runtime][commitment][issue36]") {
    FakeStore store;
    runtime::SessionEventCommitment commitment{store};
    util::ExpectedVoid failed = std::unexpected(util::make_error(
        util::ErrorCode::Provider,
        "provider exploded"));

    auto verdict = commitment.conclude(std::move(failed));
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().code == util::ErrorCode::Provider);
    CHECK(verdict.error().message == "provider exploded");
}
