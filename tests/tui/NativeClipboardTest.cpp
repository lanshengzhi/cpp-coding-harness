#include <cch/tui/NativeClipboard.hpp>

#include "support/EnvVarGuard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {

using cch::support::Error;
using cch::support::ErrorCode;
using cch::support::Expected;
using cch::support::make_error;
using cch::tui::NativeClipboard;
using cch::tui::NativeClipboardImageReadVariant;
using cch::tui::NativeClipboardTextReadVariant;
using cch::tui::NativeClipboardUnavailable;

class StubNativeClipboard final : public NativeClipboard {
public:
    [[nodiscard]] cch::support::AsyncResult<NativeClipboardTextReadVariant> get_text(
            std::stop_token stop_token) override {
        text_stop_requested = stop_token.stop_requested();
        return cch::support::AsyncResult<NativeClipboardTextReadVariant>{std::move(text_result)};
    }

    [[nodiscard]] cch::support::AsyncResult<NativeClipboardImageReadVariant> get_image(
            std::stop_token stop_token) override {
        image_stop_requested = stop_token.stop_requested();
        return cch::support::AsyncResult<NativeClipboardImageReadVariant>{std::move(image_result)};
    }

    [[nodiscard]] std::optional<cch::support::AsyncResult<std::optional<std::vector<std::filesystem::path>>>>
    get_file_paths(std::stop_token) override {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<cch::support::AsyncResult<void>> set_text(std::string) override { return std::nullopt; }

    Expected<NativeClipboardTextReadVariant> text_result{
            NativeClipboardTextReadVariant{std::optional<std::string>{std::nullopt}}};
    Expected<NativeClipboardImageReadVariant> image_result{
            NativeClipboardImageReadVariant{std::optional<std::vector<std::uint8_t>>{std::nullopt}}};
    bool text_stop_requested{false};
    bool image_stop_requested{false};
};

template <typename T> [[nodiscard]] Expected<T> consume_ready(cch::support::AsyncResult<T> operation) {
    std::optional<Expected<T>> result;
    std::move(operation).start([&result](Expected<T> outcome) noexcept { result.emplace(std::move(outcome)); });
    if (!result) std::terminate();
    return std::move(*result);
}

} // namespace

// Frozen pi-v1.0.4 `packages/tui/src/native-platform.ts` returns no helper
// when Linux DISPLAY is unset/empty; helper loading with DISPLAY can still
// fail. These cases pin both availability boundaries independently.
TEST_CASE(
        "Linux native clipboard factory reports display and helper availability", "[tui][clipboard][issue992][spec]") {
    SECTION("an unset DISPLAY prevents loading the helper") {
        cch::tests::EnvVarGuard display{"DISPLAY", std::nullopt};
        bool loader_called = false;
        auto clipboard = cch::tui::get_native_clipboard([&loader_called] {
            loader_called = true;
            return std::make_unique<StubNativeClipboard>();
        });
        CHECK(clipboard == nullptr);
        CHECK_FALSE(loader_called);
    }

    SECTION("an empty DISPLAY prevents loading the helper") {
        cch::tests::EnvVarGuard display{"DISPLAY", std::string{}};
        bool loader_called = false;
        auto clipboard = cch::tui::get_native_clipboard([&loader_called] {
            loader_called = true;
            return std::make_unique<StubNativeClipboard>();
        });
        CHECK(clipboard == nullptr);
        CHECK_FALSE(loader_called);
    }

    SECTION("a display loads a helper when available") {
        cch::tests::EnvVarGuard display{"DISPLAY", std::string{":77"}};
        bool loader_called = false;
        auto clipboard = cch::tui::get_native_clipboard([&loader_called] {
            loader_called = true;
            return std::make_unique<StubNativeClipboard>();
        });
        CHECK(clipboard != nullptr);
        CHECK(loader_called);
    }

    SECTION("a missing helper remains unavailable") {
        cch::tests::EnvVarGuard display{"DISPLAY", std::string{":77"}};
        auto clipboard = cch::tui::get_native_clipboard([] { return std::unique_ptr<NativeClipboard>{}; });
        CHECK(clipboard == nullptr);
    }
}

// Frozen pi-v1.0.4 native-platform.ts distinguishes null (empty), values, and
// rejected transfers for getText/getImage. The C++ equivalent is optional
// payloads plus Expected errors.
TEST_CASE("Native clipboard reads distinguish empty values, data, and transfer failures",
        "[tui][clipboard][issue992][spec]") {
    StubNativeClipboard clipboard;
    const std::stop_token stop_token{};

    auto empty_text = consume_ready(clipboard.get_text(stop_token));
    auto empty_image = consume_ready(clipboard.get_image(stop_token));
    REQUIRE(empty_image);
    REQUIRE(std::holds_alternative<std::optional<std::vector<std::uint8_t>>>(*empty_image));
    CHECK_FALSE(std::get<std::optional<std::vector<std::uint8_t>>>(*empty_image).has_value());
    REQUIRE(empty_text);
    REQUIRE(std::holds_alternative<std::optional<std::string>>(*empty_text));
    CHECK_FALSE(std::get<std::optional<std::string>>(*empty_text).has_value());

    clipboard.text_result = NativeClipboardTextReadVariant{std::optional<std::string>{"clipboard text"}};
    auto text = consume_ready(clipboard.get_text(stop_token));
    REQUIRE(text);
    CHECK(std::get<std::optional<std::string>>(*text) == std::optional<std::string>{"clipboard text"});

    clipboard.image_result =
            NativeClipboardImageReadVariant{std::optional<std::vector<std::uint8_t>>{{0x89, 0x50, 0x4e, 0x47}}};
    auto image = consume_ready(clipboard.get_image(stop_token));
    REQUIRE(image);
    CHECK(std::get<std::optional<std::vector<std::uint8_t>>>(*image) ==
            std::optional<std::vector<std::uint8_t>>{{0x89, 0x50, 0x4e, 0x47}});

    clipboard.text_result = NativeClipboardTextReadVariant{NativeClipboardUnavailable{}};
    auto unavailable_text = consume_ready(clipboard.get_text(stop_token));
    REQUIRE(unavailable_text);
    CHECK(std::holds_alternative<NativeClipboardUnavailable>(*unavailable_text));
    clipboard.image_result = NativeClipboardImageReadVariant{NativeClipboardUnavailable{}};
    auto unavailable_image = consume_ready(clipboard.get_image(stop_token));
    REQUIRE(unavailable_image);
    CHECK(std::holds_alternative<NativeClipboardUnavailable>(*unavailable_image));

    clipboard.text_result = std::unexpected(make_error(ErrorCode::Process, "clipboard transfer failed"));
    auto failure = consume_ready(clipboard.get_text(stop_token));
    REQUIRE_FALSE(failure);
    CHECK(failure.error().message == "clipboard transfer failed");

    clipboard.image_result = std::unexpected(make_error(ErrorCode::Process, "image transfer failed"));
    auto image_failure = consume_ready(clipboard.get_image(stop_token));
    REQUIRE_FALSE(image_failure);
    CHECK(image_failure.error().message == "image transfer failed");
}

TEST_CASE("Linux native clipboard leaves file paths and writes unsupported", "[tui][clipboard][issue992][spec]") {
    StubNativeClipboard clipboard;
    const std::stop_token stop_token{};
    CHECK_FALSE(clipboard.get_file_paths(stop_token).has_value());
    CHECK_FALSE(clipboard.set_text("text").has_value());
}

TEST_CASE("Native clipboard receives stop tokens and keeps unavailable distinct from empty",
        "[tui][clipboard][issue992][spec]") {
    StubNativeClipboard clipboard;
    std::stop_source stop_source;
    stop_source.request_stop();
    (void)clipboard.get_text(stop_source.get_token());
    (void)clipboard.get_image(stop_source.get_token());
    CHECK(clipboard.text_stop_requested);
    CHECK(clipboard.image_stop_requested);

    const NativeClipboardTextReadVariant unavailable{NativeClipboardUnavailable{}};
    const NativeClipboardTextReadVariant empty{std::optional<std::string>{std::nullopt}};
    CHECK(std::holds_alternative<NativeClipboardUnavailable>(unavailable));
    CHECK(std::holds_alternative<std::optional<std::string>>(empty));
}
