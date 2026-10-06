#pragma once

// Shared helpers for the MCP transport session tests (§11.5): the fixture trace
// readers and the tool-call shapes both the stdio and HTTP session tests drive.
// Test support only; no production seam.

#include <cch/support/JsonValue.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::tests {

/// The fixture trace file's text, or an empty string when it is absent.
[[nodiscard]] inline std::string read_text_if_present(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// Whether the fixture trace file contains `needle`.
[[nodiscard]] inline bool trace_contains(const std::filesystem::path& path, std::string_view needle) {
    return read_text_if_present(path).find(needle) != std::string::npos;
}

/// Await until `predicate` holds or `budget` elapses; the caller asserts the
/// property afterwards, so a timeout stays a test failure rather than a hang.
[[nodiscard]] inline boost::asio::awaitable<void> wait_for(
        boost::asio::any_io_executor executor, std::function<bool()> predicate, std::chrono::milliseconds budget) {
    boost::asio::steady_timer timer(executor);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        timer.expires_after(std::chrono::milliseconds{10});
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
}

/// One `tools/call` parameter object (`{name, arguments}`).
[[nodiscard]] inline support::JsonValue tools_call_params(std::string name, support::JsonValue arguments) {
    return support::JsonValue{support::JsonValue::object_t{
            {"name", std::move(name)},
            {"arguments", std::move(arguments)},
    }};
}

/// The first `text` content block's text of a `tools/call` result, or nullopt.
[[nodiscard]] inline std::optional<std::string> first_text_content(const support::JsonValue& result) {
    const auto* object = result.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto content = object->find("content");
    if (content == object->end()) {
        return std::nullopt;
    }
    const auto* array = content->second.get_if<support::JsonValue::array_t>();
    if (array == nullptr) {
        return std::nullopt;
    }
    for (const auto& block : *array) {
        const auto* block_object = block.get_if<support::JsonValue::object_t>();
        if (block_object == nullptr) {
            continue;
        }
        const auto text = block_object->find("text");
        if (text != block_object->end() && text->second.holds<std::string>()) {
            return text->second.get_string();
        }
    }
    return std::nullopt;
}

} // namespace cch::tests
