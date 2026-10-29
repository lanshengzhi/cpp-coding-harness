#include <cch/ai/Content.hpp>
#include <cch/ai/Models.hpp>
#include <cch/durable/Environment.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/Images.hpp>
#include <cch/durable/Storage.hpp>
#include "durable/ImageExpectation.hpp"
#include "durable/Outcome.hpp"
#include "support/ImageFixture.hpp"
#include "support/ImagePixels.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;

namespace {

[[nodiscard]] std::string frozen_text(std::string_view name, std::size_t index) {
    const auto scenario = image_scenario(name);
    return scenario.expected().at("content").get_array().at(index).at("text").get_string();
}

[[nodiscard]] std::string frozen_image_data(std::string_view name, std::size_t index) {
    const auto scenario = image_scenario(name);
    return scenario.expected().at("content").get_array().at(index).at("data").get_string();
}

[[nodiscard]] std::vector<std::uint8_t> frozen_pixels(std::string_view name, std::size_t index) {
    const auto scenario = image_scenario(name);
    return tests::decode_base64(
            scenario.expected().at("content").get_array().at(index).at("pixelsBase64").get_string());
}

const chord::Json& model_entry(const EntryRecord& entry) {
    const auto* envelope = entry.data.as_object();
    REQUIRE(envelope != nullptr);
    const auto found = envelope->find("model");
    REQUIRE(found != envelope->end());
    const auto* messages = found->second.as_array();
    REQUIRE(messages != nullptr);
    REQUIRE(messages->size() == 1);
    return messages->front();
}

struct ReadOutcome {
    int requests{};
    support::JsonValue result;
};

/// Submit one `read` tool call through the public Harness and return the
/// committed tool result. The stream factory records every model request, so a
/// capability-absent path can assert it did not ask a provider for anything
/// beyond the ordinary turn.
[[nodiscard]] ReadOutcome run_read(
        std::shared_ptr<Environment> environment, CodingToolOptions options, std::string path) {
    Registry registry;
    REQUIRE(registry.install(coding_tools(std::move(environment), std::move(options))).has_value());
    ReadOutcome outcome;
    HarnessOptions harness_options{.registry = &registry};
    harness_options.stream = [&outcome, path](ai::Model, ai::AiContext, ai::SimpleStreamOptions) {
        const int turn = outcome.requests++;
        return ai::ModelStream{ai::ModelStreamProducer{
                [turn, path](ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) noexcept {
                    auto message = ai::assistant_text_message("finished");
                    if (turn == 0) {
                        using Json = support::JsonValue;
                        message.content = {ai::ToolCallContent{
                                .id = "read-1", .name = "read", .arguments = Json{Json::object_t{{"path", path}}}}};
                        message.stop_reason = ai::AssistantStopReason::ToolUse;
                    }
                    auto delivered = sink(ai::AssistantDoneEvent{.reason = message.stop_reason, .message = message});
                    if (!delivered)
                        completion(std::unexpected(delivered.error()));
                    else
                        completion(std::move(message));
                }}};
    };
    MemoryStorage storage;
    auto opened = consume(Harness::open(storage, std::move(harness_options)));
    REQUIRE(opened.has_value());
    auto harness = std::move(opened.value());
    auto root = consume(harness->root());
    REQUIRE(root.has_value());
    auto submission = consume(harness->submit(
            root.value(), SubmissionDraft{.body = InputSubmissionDraft{.content = chord::Json{"read"}}}));
    REQUIRE(submission.has_value());
    auto settled = consume(submission.value().wait());
    REQUIRE(settled.has_value());
    CHECK(settled.value().status == "done");

    auto entries = storage.scan_entries(root.value(), 100);
    REQUIRE(entries.has_value());
    const auto result =
            std::ranges::find_if(*entries, [](const auto& entry) { return entry.kind == "pi.tool-result"; });
    REQUIRE(result != entries->end());
    outcome.result = model_entry(*result).to_support();
    REQUIRE(consume(harness->close()).has_value());
    return outcome;
}

[[nodiscard]] const support::JsonValue& content_at(const ReadOutcome& outcome, std::size_t index) {
    const auto& content = outcome.result.at("content").get_array();
    REQUIRE(index < content.size());
    return content.at(index);
}

[[nodiscard]] std::string text_at(const ReadOutcome& outcome, std::size_t index) {
    return content_at(outcome, index).at("text").get_string();
}

} // namespace

TEST_CASE("Read returns a prepared image and the frozen diagnostic through the public Harness",
        "[durable][images][issue1090][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("photo.jpg", tests::bytes_as_string(tests::decode_base64(tests::kPatternedJpegBase64)));

    auto options = CodingToolOptions{};
    options.images = local_image_processor();
    options.vision = true;
    options.limits.max_width = 1;
    options.limits.max_height = 1;
    options.limits.max_base64_bytes = 1024 * 1024;
    const auto outcome = run_read(local_environment(workspace.path().string()), std::move(options), "photo.jpg");

    CHECK(outcome.requests == 2);
    CHECK_FALSE(outcome.result.at("isError").get_boolean());
    CHECK(content_at(outcome, 0).at("type").get_string() == "image");
    CHECK(content_at(outcome, 0).at("mimeType").get_string() == "image/jpeg");
    const auto prepared_bytes = tests::decode_base64(content_at(outcome, 0).at("data").get_string());
    const auto prepared_dimensions = image_dimensions(prepared_bytes, "image/jpeg");
    REQUIRE(prepared_dimensions.has_value());
    CHECK(prepared_dimensions->width == 1);
    CHECK(prepared_dimensions->height == 1);
    const auto frozen_pixels = decode_rgba(tests::decode_base64(
            frozen_image_data("an EXIF-oriented JPEG is made upright before its resized diagnostic", 0)));
    const auto prepared_pixels = decode_rgba(prepared_bytes);
    REQUIRE(frozen_pixels.has_value());
    REQUIRE(prepared_pixels.has_value());
    REQUIRE(frozen_pixels->size() == 4);
    REQUIRE(prepared_pixels->size() == 4);
    for (std::size_t channel = 0; channel < 4; ++channel) {
        CHECK(std::abs(static_cast<int>((*prepared_pixels)[channel]) - static_cast<int>((*frozen_pixels)[channel])) <=
                24);
    }
    CHECK(text_at(outcome, 1) == frozen_text("an EXIF-oriented JPEG is made upright before its resized diagnostic", 1));
}

TEST_CASE("Read converts BMP to PNG through the configured processor", "[durable][images][issue1090][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("image.bmp", tests::bytes_as_string(tests::tiny_bmp()));
    auto options = CodingToolOptions{};
    options.images = local_image_processor();
    options.vision = true;
    const auto outcome = run_read(local_environment(workspace.path().string()), std::move(options), "image.bmp");

    CHECK(outcome.requests == 2);
    CHECK_FALSE(outcome.result.at("isError").get_boolean());
    CHECK(content_at(outcome, 0).at("mimeType").get_string() == "image/png");
    CHECK(text_at(outcome, 1) == frozen_text("a BMP read through the processor converts to PNG and says so", 1));
    const auto pixels = decode_rgba(decode_base64(content_at(outcome, 0).at("data").get_string()));
    REQUIRE(pixels.has_value());
    CHECK(*pixels == frozen_pixels("a BMP read through the processor converts to PNG and says so", 0));
}

TEST_CASE("An image the processor cannot decode is an explicit error", "[durable][images][issue1090][spec]") {
    tests::TempWorkspace workspace;
    auto broken = tests::decode_base64(tests::kTinyPngBase64);
    broken.resize(20);
    workspace.write("broken.png", tests::bytes_as_string(broken));
    auto options = CodingToolOptions{};
    options.images = local_image_processor();
    options.vision = true;
    const auto outcome = run_read(local_environment(workspace.path().string()), std::move(options), "broken.png");

    CHECK(outcome.requests == 2);
    CHECK(outcome.result.at("isError").get_boolean());
    CHECK(text_at(outcome, 0) == frozen_text("an image the processor cannot decode is an explicit error", 0));
}

TEST_CASE(
        "A model without vision gets the image and the frozen placeholder note", "[durable][images][issue1090][spec]") {
    constexpr std::string_view kScenario = "a model without image input gets the image and the frozen placeholder note";
    tests::TempWorkspace workspace;
    const auto frozen_data = frozen_image_data(kScenario, 0);
    workspace.write("plain.png", tests::bytes_as_string(tests::decode_base64(frozen_data)));
    auto options = CodingToolOptions{};
    options.vision = false;
    const auto outcome = run_read(local_environment(workspace.path().string()), std::move(options), "plain.png");

    CHECK(outcome.requests == 2);
    CHECK_FALSE(outcome.result.at("isError").get_boolean());
    CHECK(content_at(outcome, 0).at("type").get_string() == "image");
    CHECK(content_at(outcome, 0).at("data").get_string() == frozen_data);
    CHECK(text_at(outcome, 1) == frozen_text(kScenario, 1));
}

TEST_CASE("An inline GIF passes through the public Harness unchanged", "[durable][images][issue1090][spec]") {
    constexpr std::string_view kScenario = "an inline GIF passes through unchanged without a processor";
    tests::TempWorkspace workspace;
    const auto frozen_data = frozen_image_data(kScenario, 0);
    workspace.write("plain.gif", tests::bytes_as_string(tests::decode_base64(frozen_data)));
    auto options = CodingToolOptions{};
    options.vision = true;
    const auto outcome = run_read(local_environment(workspace.path().string()), std::move(options), "plain.gif");

    CHECK(outcome.requests == 2);
    CHECK_FALSE(outcome.result.at("isError").get_boolean());
    CHECK(content_at(outcome, 0).at("type").get_string() == "image");
    CHECK(content_at(outcome, 0).at("mimeType").get_string() == "image/gif");
    CHECK(content_at(outcome, 0).at("data").get_string() == frozen_data);
    CHECK(text_at(outcome, 1) == frozen_text(kScenario, 1));
}

TEST_CASE("Capability-absent image reads fail explicitly without another model turn",
        "[durable][images][issue1090][spec]") {
    tests::TempWorkspace workspace;
    const auto png = tests::decode_base64(tests::kTinyPngBase64);
    workspace.write("image.bmp", tests::bytes_as_string(tests::tiny_bmp()));
    workspace.write("image.png", tests::bytes_as_string(png));
    auto wide = png;
    wide[16] = 0;
    wide[17] = 0;
    wide[18] = 0x07;
    wide[19] = 0xd1; // 2001 pixels wide
    workspace.write("wide.png", tests::bytes_as_string(wide));
    auto truncated = png;
    truncated.resize(20);
    workspace.write("truncated.png", tests::bytes_as_string(truncated));

    const auto environment = local_environment(workspace.path().string());
    auto options = CodingToolOptions{};
    options.vision = true;
    SECTION("BMP needs converting") {
        const auto outcome = run_read(environment, std::move(options), "image.bmp");
        CHECK(outcome.requests == 2);
        CHECK(outcome.result.at("isError").get_boolean());
        CHECK(text_at(outcome, 0) ==
                frozen_text("reading a BMP without an image processor is an explicit unsupported-image result", 0));
    }
    SECTION("an inline image in the limits passes through unchanged") {
        const auto outcome = run_read(environment, std::move(options), "image.png");
        CHECK(outcome.requests == 2);
        CHECK_FALSE(outcome.result.at("isError").get_boolean());
        CHECK(content_at(outcome, 0).at("data").get_string() ==
                frozen_image_data("an inline image within the limits passes through unchanged without a processor", 0));
        CHECK(text_at(outcome, 1) ==
                frozen_text("an inline image within the limits passes through unchanged without a processor", 1));
    }
    SECTION("an image larger than the limits cannot be shrunk") {
        const auto outcome = run_read(environment, std::move(options), "wide.png");
        CHECK(outcome.requests == 2);
        CHECK(outcome.result.at("isError").get_boolean());
        CHECK(text_at(outcome, 0) == frozen_text("an inline image larger than the limits cannot be shrunk", 0));
    }
    SECTION("an inline image above the byte limit cannot be shrunk") {
        auto huge = png;
        huge.resize(3'670'016, 0); // 3.5 MiB, whose base64 exceeds 4.5 MiB
        workspace.write("huge.png", tests::bytes_as_string(huge));
        const auto outcome = run_read(environment, std::move(options), "huge.png");
        CHECK(outcome.requests == 2);
        CHECK(outcome.result.at("isError").get_boolean());
        CHECK(text_at(outcome, 0) == frozen_text("an inline image above the byte limit cannot be shrunk", 0));
    }
    SECTION("an unreadable header is reported, not guessed") {
        const auto outcome = run_read(environment, std::move(options), "truncated.png");
        CHECK(outcome.requests == 2);
        CHECK(outcome.result.at("isError").get_boolean());
        CHECK(text_at(outcome, 0) == frozen_text("an image whose header cannot be read is an explicit error", 0));
    }
}

} // namespace cch::tests::durable
