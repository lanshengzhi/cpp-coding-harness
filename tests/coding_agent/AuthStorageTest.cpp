#include <cch/ai/CredentialStore.hpp>
#include <cch/coding_agent/AuthStorage.hpp>
#include "support/TempWorkspace.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void write_text(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.is_open());
    output << contents;
    REQUIRE(output.good());
}

std::string read_fixture(std::string_view name) {
    const auto path = std::filesystem::path(CCH_SOURCE_DIR) / "fixtures" / "pi-ai" / "auth-storage" / name;
    auto text = read_text(path);
    if (text.ends_with('\n')) {
        text.pop_back();
    }
    return text;
}

template <typename T, typename Action>
T run_async(Action action) {
    boost::asio::io_context io;
    std::optional<T> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result.emplace(co_await action());
            co_return;
        },
        boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

cch::ai::Credential api_key_credential(std::string key) {
    return cch::ai::ApiKeyCredential{.key = std::move(key), .env = {}};
}

} // namespace

TEST_CASE("AuthStorage round-trips pi auth.json without losing unrelated records", "[coding_agent][auth][issue337]") {
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "auth.json";
    write_text(path, read_fixture("round-trip-input.json"));

    cch::coding_agent::AuthStorage storage(path);
    cch::ai::CredentialStore& credentials = storage;

    const auto listed = run_async<cch::util::Expected<std::vector<cch::ai::CredentialInfo>>>(
        [&]() { return credentials.list(); });
    REQUIRE(listed);
    const std::vector<cch::ai::CredentialInfo> expected_metadata{
        {.provider_id = "custom-provider", .type = "future"},
        {.provider_id = "deepseek", .type = "api_key"},
        {.provider_id = "openai-codex", .type = "oauth"},
    };
    CHECK(*listed == expected_metadata);

    const auto codex = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>(
        [&]() { return credentials.read("openai-codex"); });
    REQUIRE(codex);
    REQUIRE(codex->has_value());
    const auto* oauth = std::get_if<cch::ai::OAuthCredential>(&**codex);
    REQUIRE(oauth != nullptr);
    CHECK(oauth->account_id == "dummy-account-id");

    const auto updated = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>([&]() {
        return credentials.modify(
            "openai-codex",
            [](std::optional<cch::ai::Credential> current)
                -> boost::asio::awaitable<cch::util::Expected<std::optional<cch::ai::Credential>>> {
                REQUIRE(current.has_value());
                auto refreshed = std::get<cch::ai::OAuthCredential>(std::move(*current));
                refreshed.access = "dummy-new-access-token";
                refreshed.expires = 2;
                co_return std::optional<cch::ai::Credential>{cch::ai::Credential{std::move(refreshed)}};
            });
    });
    REQUIRE(updated);

    const auto inserted = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>([&]() {
        return credentials.modify(
            "kimi-coding",
            [](std::optional<cch::ai::Credential>)
                -> boost::asio::awaitable<cch::util::Expected<std::optional<cch::ai::Credential>>> {
                co_return std::optional<cch::ai::Credential>{api_key_credential("dummy-kimi-key")};
            });
    });
    REQUIRE(inserted);

    CHECK(read_text(path) == read_fixture("round-trip-expected.json"));

    cch::coding_agent::AuthStorage reopened(path);
    const auto persisted = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>(
        [&]() { return reopened.read("openai-codex"); });
    REQUIRE(persisted);
    REQUIRE(persisted->has_value());
    CHECK(std::get<cch::ai::OAuthCredential>(**persisted).access == "dummy-new-access-token");
}

TEST_CASE("AuthStorage serializes concurrent whole-file modifications", "[coding_agent][auth][locking][issue337]") {
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "auth.json";
    write_text(path, "{}");
    cch::coding_agent::AuthStorage first(path);
    cch::coding_agent::AuthStorage second(path);

    boost::asio::io_context io;
    std::optional<cch::util::Expected<std::optional<cch::ai::Credential>>> first_result;
    std::optional<cch::util::Expected<std::optional<cch::ai::Credential>>> second_result;

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            first_result = co_await first.modify(
                "anthropic",
                [](std::optional<cch::ai::Credential>)
                    -> boost::asio::awaitable<cch::util::Expected<std::optional<cch::ai::Credential>>> {
                    auto executor = co_await boost::asio::this_coro::executor;
                    boost::asio::steady_timer timer(executor, 75ms);
                    boost::system::error_code timer_error;
                    co_await timer.async_wait(
                        boost::asio::redirect_error(boost::asio::use_awaitable, timer_error));
                    if (timer_error) {
                        co_return std::unexpected(cch::util::make_error(
                            cch::util::ErrorCode::Cancelled,
                            "test credential modification was cancelled"));
                    }
                    co_return std::optional<cch::ai::Credential>{
                        api_key_credential("dummy-anthropic-key")};
                });
            co_return;
        },
        boost::asio::detached);
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            second_result = co_await second.modify(
                "deepseek",
                [](std::optional<cch::ai::Credential>)
                    -> boost::asio::awaitable<cch::util::Expected<std::optional<cch::ai::Credential>>> {
                    co_return std::optional<cch::ai::Credential>{api_key_credential("dummy-deepseek-key")};
                });
            co_return;
        },
        boost::asio::detached);
    io.run();

    REQUIRE(first_result.has_value());
    REQUIRE(*first_result);
    REQUIRE(second_result.has_value());
    REQUIRE(*second_result);
    const auto persisted = read_text(path);
    CHECK(persisted.find("dummy-anthropic-key") != std::string::npos);
    CHECK(persisted.find("dummy-deepseek-key") != std::string::npos);
    std::error_code lock_error;
    const bool lock_exists = std::filesystem::exists(path.string() + ".lock", lock_error);
    REQUIRE_FALSE(lock_error);
    CHECK_FALSE(lock_exists);
}

TEST_CASE(
    "AuthStorage does not steal a live proper-lockfile lock",
    "[coding_agent][auth][locking][issue337]") {
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "auth.json";
    const auto lock_path = std::filesystem::path{path.string() + ".lock"};
    write_text(path, R"({"deepseek":{"type":"api_key","key":"dummy-stored-key"}})");

    std::error_code lock_error;
    const bool created = std::filesystem::create_directory(lock_path, lock_error);
    REQUIRE_FALSE(lock_error);
    REQUIRE(created);
    std::filesystem::last_write_time(
        lock_path,
        std::filesystem::file_time_type::clock::now() - std::chrono::seconds{20},
        lock_error);
    REQUIRE_FALSE(lock_error);

    cch::coding_agent::AuthStorage storage(path);
    CHECK(std::filesystem::exists(lock_path, lock_error));
    REQUIRE_FALSE(lock_error);

    CHECK(std::filesystem::remove(lock_path, lock_error));
    REQUIRE_FALSE(lock_error);
    storage.reload();
    const auto loaded = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>(
        [&]() { return storage.read("deepseek"); });
    REQUIRE(loaded);
    REQUIRE(loaded->has_value());
}

TEST_CASE(
    "AuthStorage reclaims a stale proper-lockfile lock",
    "[coding_agent][auth][locking][issue337]") {
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "auth.json";
    const auto lock_path = std::filesystem::path{path.string() + ".lock"};
    write_text(path, R"({"deepseek":{"type":"api_key","key":"dummy-stored-key"}})");

    std::error_code lock_error;
    REQUIRE(std::filesystem::create_directory(lock_path, lock_error));
    REQUIRE_FALSE(lock_error);
    std::filesystem::last_write_time(
        lock_path,
        std::filesystem::file_time_type::clock::now() - std::chrono::seconds{31},
        lock_error);
    REQUIRE_FALSE(lock_error);

    cch::coding_agent::AuthStorage storage(path);
    CHECK_FALSE(std::filesystem::exists(lock_path, lock_error));
    REQUIRE_FALSE(lock_error);
    const auto loaded = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>(
        [&]() { return storage.read("deepseek"); });
    REQUIRE(loaded);
    REQUIRE(loaded->has_value());
}

TEST_CASE(
    "AuthStorage detects a replaced proper-lockfile lease without writing or unlocking it",
    "[coding_agent][auth][locking][issue337]") {
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "auth.json";
    const auto lock_path = std::filesystem::path{path.string() + ".lock"};
    const std::string original = R"({"deepseek":{"type":"api_key","key":"dummy-stored-key"}})";
    write_text(path, original);
    cch::coding_agent::AuthStorage storage(path);

    const auto modified = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>([&]() {
        return storage.modify(
            "deepseek",
            [&](std::optional<cch::ai::Credential>)
                -> boost::asio::awaitable<cch::util::Expected<std::optional<cch::ai::Credential>>> {
                std::error_code lock_error;
                const bool removed = std::filesystem::remove(lock_path, lock_error);
                if (!removed || lock_error) {
                    co_return std::unexpected(cch::util::make_error(
                        cch::util::ErrorCode::Unknown,
                        "test could not replace the auth lock"));
                }
                const bool created = std::filesystem::create_directory(lock_path, lock_error);
                if (!created || lock_error) {
                    co_return std::unexpected(cch::util::make_error(
                        cch::util::ErrorCode::Unknown,
                        "test could not recreate the auth lock"));
                }
                std::filesystem::last_write_time(
                    lock_path,
                    std::filesystem::file_time_type::clock::now() + std::chrono::seconds{1},
                    lock_error);
                if (lock_error) {
                    co_return std::unexpected(cch::util::make_error(
                        cch::util::ErrorCode::Unknown,
                        "test could not age the replacement auth lock"));
                }
                co_return std::optional<cch::ai::Credential>{api_key_credential("dummy-new-key")};
            });
    });
    REQUIRE_FALSE(modified);
    CHECK(read_text(path) == original);
    std::error_code lock_error;
    CHECK(std::filesystem::exists(lock_path, lock_error));
    REQUIRE_FALSE(lock_error);
    CHECK(std::filesystem::remove(lock_path, lock_error));
    REQUIRE_FALSE(lock_error);
}

TEST_CASE(
    "AuthStorage preserves its last valid snapshot and never overwrites invalid JSON",
    "[coding_agent][auth][issue337]") {
    cch::tests::TempWorkspace workspace;
    const auto path = workspace.path() / "auth.json";
    write_text(path, R"({"deepseek":{"type":"api_key","key":"dummy-stored-key"}})");
    cch::coding_agent::AuthStorage storage(path);

    write_text(path, "{invalid-json");
    storage.reload();

    const auto retained = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>(
        [&]() { return storage.read("deepseek"); });
    REQUIRE(retained);
    REQUIRE(retained->has_value());
    CHECK(std::get<cch::ai::ApiKeyCredential>(**retained).key == "dummy-stored-key");

    const auto modified = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>([&]() {
        return storage.modify(
            "openai",
            [](std::optional<cch::ai::Credential>)
                -> boost::asio::awaitable<cch::util::Expected<std::optional<cch::ai::Credential>>> {
                co_return std::optional<cch::ai::Credential>{api_key_credential("dummy-new-key")};
            });
    });
    REQUIRE_FALSE(modified);
    CHECK(read_text(path) == "{invalid-json");
}

TEST_CASE(
    "AuthStorage creates private pi-compatible paths and delete preserves other records",
    "[coding_agent][auth][permissions][issue337]") {
    cch::tests::TempWorkspace workspace;
    const auto agent_dir = workspace.path() / "nested" / "agent";
    const auto path = agent_dir / "auth.json";
    cch::coding_agent::AuthStorage storage(path);

    const auto inserted = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>([&]() {
        return storage.modify(
            "deepseek",
            [](std::optional<cch::ai::Credential>)
                -> boost::asio::awaitable<cch::util::Expected<std::optional<cch::ai::Credential>>> {
                co_return std::optional<cch::ai::Credential>{api_key_credential("dummy-deepseek-key")};
            });
    });
    REQUIRE(inserted);
    const auto second = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>([&]() {
        return storage.modify(
            "kimi-coding",
            [](std::optional<cch::ai::Credential>)
                -> boost::asio::awaitable<cch::util::Expected<std::optional<cch::ai::Credential>>> {
                co_return std::optional<cch::ai::Credential>{api_key_credential("dummy-kimi-key")};
            });
    });
    REQUIRE(second);

    const auto removed = run_async<cch::util::ExpectedVoid>([&]() { return storage.remove("deepseek"); });
    REQUIRE(removed);
    const auto remaining = run_async<cch::util::Expected<std::optional<cch::ai::Credential>>>(
        [&]() { return storage.read("kimi-coding"); });
    REQUIRE(remaining);
    REQUIRE(remaining->has_value());

#if defined(__unix__) || defined(__APPLE__)
    std::error_code directory_status_error;
    const auto directory_status = std::filesystem::status(agent_dir, directory_status_error);
    REQUIRE_FALSE(directory_status_error);
    std::error_code file_status_error;
    const auto file_status = std::filesystem::status(path, file_status_error);
    REQUIRE_FALSE(file_status_error);
    const auto directory_permissions = directory_status.permissions() & std::filesystem::perms::all;
    const auto file_permissions = file_status.permissions() & std::filesystem::perms::all;
    CHECK(directory_permissions == std::filesystem::perms::owner_all);
    CHECK(file_permissions == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
#else
    SUCCEED("POSIX permission assertions are not available on this platform");
#endif
}
