// Ticket #877 (spec #865), reshaped for the inline tool (spec #882, #885):
// codemode output presentation. The model-facing `codemode` tool's text and
// image output is presented through the same ToolRendererRegistry fallback
// pair and the same inline image slots as any other tool's output; there is no
// codemode-specific renderer. The tool name (`codemode`) is unregistered,
// exactly like an MCP or extension tool name, so the registry hands it the
// fallback pair.
//
// Acceptance discipline: the cheap check is "the ToolResultMessage carries a
// text block and an image block" — a check that passes while the image never
// reaches the screen. The rendered-region assertions cover the rendered form
// instead: the fallback's own framing on screen, and the image sidecar the
// component places inline after the tool block. The separation case is the
// image: the stored-content check would pass even if the renderer dropped
// every image, so the rendered sidecar is asserted separately.
//
// The script runs in the real wasm sandbox, so the content blocks are the
// codemode mapping's own output rather than a hand-built stand-in.

#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"
#include "coding_agent/tui/ToolExecutionComponent.hpp"
#include "support/ImageFixture.hpp"
#include "support/Json.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/ToolRendererFixture.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

namespace tui = cch::coding_agent::tui;
namespace extensions = cch::coding_agent::extensions;

/// The model-visible result of the inline tool whose script emits `text()`,
/// `image()`, and a return value. Loaded and run through the real
/// `CodemodeToolSource`, so the content is the sandbox mapping's own output.
[[nodiscard]] extensions::ExtensionToolResult run_inline_tool() {
    coding_agent::extensions::CodemodeToolSource source{};
    auto tools = source.load_tools();
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 1);

    const std::string code = std::format("text('summarize: 3 files');\n"
                                         "image('data:image/png;base64,{}');\n"
                                         "return {{ files: 3 }};",
            tests::kTinyPngBase64);
    auto executed =
            tools->front().execute(support::JsonValue{support::JsonValue::object_t{{"code", code}}}, std::stop_token{});
    auto outcome = tests::run_async_result(std::move(executed));
    REQUIRE(outcome.has_value());
    return std::move(*outcome);
}

/// The model-visible result the extension runner would hand the transcript:
/// the same content, details, and error flag, under the given tool name.
[[nodiscard]] ai::ToolResultMessage to_tool_result(std::string tool_name, extensions::ExtensionToolResult result) {
    return ai::ToolResultMessage{
            .tool_call_id = "call_codemode",
            .tool_name = std::move(tool_name),
            .content = std::move(result.content),
            .details = std::move(result.details),
            .is_error = result.is_error,
            .timestamp = 0,
    };
}

/// The composed tool block as the user sees it: the visible rows (terminal
/// sequences stripped, the box margin dropped, right padding trimmed) plus the
/// inline image sidecars the component places after the block.
struct RenderedBlock {
    std::vector<std::string> visible;
    std::vector<cch::tui::InlineImageRenderRegion> images;
};

[[nodiscard]] RenderedBlock render_block(tui::ToolExecutionComponent& component, std::size_t width) {
    const auto rendered = component.render(width);
    REQUIRE(rendered);
    RenderedBlock block;
    block.images = rendered->images;
    for (const auto& line : rendered->lines) {
        auto visible = cch::tui::strip_terminal_sequences(line);
        const auto first = visible.find_first_not_of(' ');
        if (first == std::string::npos) {
            block.visible.emplace_back();
            continue;
        }
        const auto last = visible.find_last_not_of(' ');
        const auto start = std::min(first, tests::kToolBlockMargin);
        block.visible.push_back(visible.substr(start, last - start + 1));
    }
    return block;
}

[[nodiscard]] RenderedBlock render_tool(std::string tool_name, extensions::ExtensionToolResult result) {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    tui::ToolExecutionComponent component(theme, keybindings, tool_name, "call_codemode", "{}", "/workspace");
    component.update_result(to_tool_result(std::move(tool_name), std::move(result)));
    return render_block(component, 80);
}

[[nodiscard]] bool contains_row(const std::vector<std::string>& rows, std::string_view needle) {
    return std::ranges::any_of(
            rows, [needle](const std::string& row) { return row.find(needle) != std::string::npos; });
}

} // namespace

TEST_CASE("a codemode tool's text and image output render through the fallback and the inline image slot",
        "[coding_agent][codemode][issue877][spec]") {
    auto result = run_inline_tool();

    // The stored content: the pi result header, the script's text output, its
    // image, and the return value appended last. This is the cheap check the
    // rendered assertions below are not.
    REQUIRE_FALSE(result.is_error);
    REQUIRE(result.content.size() == 4);
    const auto* header = std::get_if<ai::TextContent>(&result.content[0]);
    REQUIRE(header != nullptr);
    CHECK(header->text.starts_with("Script completed\nWall time "));
    CHECK(header->text.ends_with(" seconds\nOutput:\n"));
    const auto* text_block = std::get_if<ai::TextContent>(&result.content[1]);
    REQUIRE(text_block != nullptr);
    CHECK(text_block->text == "summarize: 3 files");
    const auto* image_block = std::get_if<ai::ImageContent>(&result.content[2]);
    REQUIRE(image_block != nullptr);
    CHECK(image_block->mime_type == "image/png");
    CHECK(image_block->data == tests::kTinyPngBase64);
    const auto* value_text = std::get_if<ai::TextContent>(&result.content[3]);
    REQUIRE(value_text != nullptr);
    CHECK(value_text->text == "{\"files\":3}");

    const auto rendered = render_tool("codemode", std::move(result));

    // The rendered form, not the stored content: the fallback's own framing
    // (the bold tool name) and the script's output rows.
    CHECK(contains_row(rendered.visible, "codemode"));
    CHECK(contains_row(rendered.visible, "summarize: 3 files"));
    CHECK(contains_row(rendered.visible, "{\"files\":3}"));

    // The image sidecar the component placed inline after the tool block. The
    // stored-content check above would pass while the image never reached the
    // screen; this asserts it did, with the codemode image's own bytes.
    REQUIRE(rendered.images.size() == 1);
    const auto& image = rendered.images.front();
    CHECK(image.mime_type == "image/png");
    CHECK(image.encoded_data == tests::kTinyPngBase64);
    // The image lands after the tool block, not over it: it is the last row.
    CHECK(image.region.row == rendered.visible.size() - 1);
}

TEST_CASE("a codemode tool's output presents exactly like another unregistered tool's output",
        "[coding_agent][codemode][issue877][spec]") {
    const auto codemode = render_tool("codemode", run_inline_tool());
    // The same content under a name that also carries no renderer: MCP tools
    // and extension tools take this same fallback pair, so the presentation is
    // name-independent and the codemode name is not special.
    const auto probe = render_tool("probe", run_inline_tool());

    REQUIRE(codemode.visible.size() == probe.visible.size());
    for (std::size_t row = 0; row < codemode.visible.size(); ++row) {
        // The tool-name row differs by design, and the wall-time row is the
        // measured duration (nondeterministic); every other row is identical.
        if (codemode.visible[row].find("Wall time") != std::string::npos) {
            continue;
        }
        if (codemode.visible[row] == "codemode" || codemode.visible[row] == "probe") {
            continue;
        }
        CHECK(codemode.visible[row] == probe.visible[row]);
    }
    // The image lands in the same place and with the same bytes for both names.
    REQUIRE(codemode.images.size() == 1);
    REQUIRE(probe.images.size() == 1);
    CHECK(codemode.images.front().region.row == probe.images.front().region.row);
    CHECK(codemode.images.front().encoded_data == probe.images.front().encoded_data);
    CHECK(codemode.images.front().mime_type == probe.images.front().mime_type);
}
