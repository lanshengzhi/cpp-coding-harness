// Ticket #877 (spec #865): codemode output presentation. A declared codemode
// tool's text and image output is presented through the same
// ToolRendererRegistry fallback pair and the same inline image slots as any
// other tool's output; there is no codemode-specific renderer. The declared
// tool's name (`summarize_repo`) is unregistered, exactly like an MCP or
// extension tool name, so the registry hands it the fallback pair.
//
// Acceptance discipline: the cheap check is "the ToolResultMessage carries a
// text block and an image block" — a check that passes while the image never
// reaches the screen. The golden and the image-region assertion cover the
// rendered form instead: the fallback's own framing on screen, and the image
// sidecar the component places inline after the tool block. The separation
// case is the image: the stored-content check would pass even if the renderer
// dropped every image, so the rendered sidecar is asserted separately.
//
// The script runs in the real wasm sandbox, so the content blocks are the
// codemode mapping's own output rather than a hand-built stand-in.

#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"
#include "coding_agent/tui/ToolExecutionComponent.hpp"
#include "support/ImageFixture.hpp"
#include "support/Json.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/ToolRendererFixture.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

namespace tui = cch::coding_agent::tui;
namespace extensions = cch::coding_agent::extensions;

[[nodiscard]] std::filesystem::path golden_path(std::string_view name) {
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "codemode" / "golden" / name;
}

[[nodiscard]] std::string read_golden_text(std::string_view name) {
    std::ifstream input(golden_path(name), std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// Writes the golden when CCH_CAPTURE_GOLDENS=1, matching the session
/// rendering goldens' capture mechanism.
void capture_golden(std::string_view name, const std::string& text) {
    if (std::getenv("CCH_CAPTURE_GOLDENS") == nullptr) return;
    const auto path = golden_path(name);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output{path, std::ios::binary};
    output << text;
}

/// The model-visible result of the declared tool whose script emits `text()`,
/// `image()`, and a return value. Loaded and run through the real
/// `CodemodeToolSource`, so the content is the sandbox mapping's own output.
[[nodiscard]] extensions::ExtensionToolResult run_declared_tool() {
    tests::TempWorkspace workspace;
    workspace.write(".pi/codemode/summarize_repo.json",
            R"({"name":"summarize_repo","description":"Emit text, an image, and a value.","source":"summarize_repo.js"})");
    workspace.write(".pi/codemode/summarize_repo.js",
            std::format("text('summarize: 3 files');\n"
                        "image('data:image/png;base64,{}');\n"
                        "return {{ files: 3 }};",
                    tests::kTinyPngBase64));

    coding_agent::extensions::CodemodeToolSource source{workspace.path()};
    auto tools = source.load_tools();
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 1);

    auto executed = tools->front().execute(support::JsonValue::object_t{}, std::stop_token{});
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

/// The golden's own shape: the visible rows and the image sidecars, serialized
/// from a rendered block so the capture and the comparison agree byte for byte.
[[nodiscard]] std::string golden_json(const RenderedBlock& block) {
    support::JsonValue::array_t lines;
    lines.reserve(block.visible.size());
    for (const auto& line : block.visible)
        lines.emplace_back(line);
    support::JsonValue::array_t images;
    images.reserve(block.images.size());
    for (const auto& image : block.images) {
        support::JsonValue::object_t entry;
        entry.emplace("row", support::JsonValue{static_cast<double>(image.region.row)});
        entry.emplace("mime_type", support::JsonValue{image.mime_type});
        entry.emplace("encoded_data", support::JsonValue{image.encoded_data});
        images.emplace_back(std::move(entry));
    }
    support::JsonValue::object_t root;
    root.emplace("visible_lines", support::JsonValue{std::move(lines)});
    root.emplace("images", support::JsonValue{std::move(images)});
    auto text = support::write_json(support::JsonValue{std::move(root)});
    REQUIRE(text.has_value());
    return *text;
}

[[nodiscard]] std::vector<std::string> golden_visible_lines(const support::JsonValue& golden) {
    std::vector<std::string> lines;
    for (const auto& line : golden.get_object().at("visible_lines").get_array()) {
        REQUIRE(line.holds<std::string>());
        lines.push_back(line.get_string());
    }
    return lines;
}

} // namespace

TEST_CASE("a codemode tool's text and image output render through the fallback and the inline image slot",
        "[coding_agent][codemode][issue877][spec]") {
    auto result = run_declared_tool();

    // The stored content: the script's text output, its image, and the return
    // value appended last. This is the cheap check the golden below is not.
    REQUIRE_FALSE(result.is_error);
    REQUIRE(result.content.size() == 3);
    const auto* first_text = std::get_if<ai::TextContent>(&result.content[0]);
    REQUIRE(first_text != nullptr);
    CHECK(first_text->text == "summarize: 3 files");
    const auto* image_block = std::get_if<ai::ImageContent>(&result.content[1]);
    REQUIRE(image_block != nullptr);
    CHECK(image_block->mime_type == "image/png");
    CHECK(image_block->data == tests::kTinyPngBase64);
    const auto* value_text = std::get_if<ai::TextContent>(&result.content[2]);
    REQUIRE(value_text != nullptr);
    CHECK(value_text->text == "{\"files\":3}");

    const auto rendered = render_tool("summarize_repo", std::move(result));
    capture_golden("tool-result-render.json", golden_json(rendered) + "\n");

    auto golden = support::read_json(read_golden_text("tool-result-render.json"));
    REQUIRE(golden.has_value());
    // The rendered form, not the stored content: the fallback's own framing
    // (the bold tool name, a blank row, the argument JSON) and the output
    // rows. A named codemode renderer would fill this in instead.
    CHECK(rendered.visible == golden_visible_lines(*golden));

    // The image sidecar the component placed inline after the tool block. The
    // stored-content check above would pass while the image never reached the
    // screen; this asserts it did, with the codemode image's own bytes.
    const auto& golden_images = golden->get_object().at("images").get_array();
    REQUIRE(golden_images.size() == 1);
    REQUIRE(rendered.images.size() == 1);
    const auto& image = rendered.images.front();
    const auto& expected_image = golden_images.front().get_object();
    CHECK(image.mime_type == expected_image.at("mime_type").get_string());
    CHECK(image.encoded_data == expected_image.at("encoded_data").get_string());
    CHECK(image.region.row == static_cast<std::size_t>(expected_image.at("row").get_number()));
    // The image lands after the tool block, not over it: the box's own rows
    // precede it, and it is the last rendered row.
    CHECK(image.region.row == rendered.visible.size() - 1);
}

TEST_CASE("a codemode tool's output presents exactly like another unregistered tool's output",
        "[coding_agent][codemode][issue877][spec]") {
    const auto codemode = render_tool("summarize_repo", run_declared_tool());
    // The same content under a name that also carries no renderer: MCP tools
    // and extension tools take this same fallback pair, so the presentation is
    // name-independent and the codemode name is not special.
    const auto probe = render_tool("probe", run_declared_tool());

    REQUIRE(codemode.visible.size() == probe.visible.size());
    // Only the bold tool-name row differs; every other row, including the
    // output rows and the image row, is identical.
    CHECK(codemode.visible[1] == "summarize_repo");
    CHECK(probe.visible[1] == "probe");
    for (std::size_t row = 0; row < codemode.visible.size(); ++row) {
        if (row == 1) continue;
        CHECK(codemode.visible[row] == probe.visible[row]);
    }
    // The image lands in the same place and with the same bytes for both names.
    REQUIRE(codemode.images.size() == 1);
    REQUIRE(probe.images.size() == 1);
    CHECK(codemode.images.front().region.row == probe.images.front().region.row);
    CHECK(codemode.images.front().encoded_data == probe.images.front().encoded_data);
    CHECK(codemode.images.front().mime_type == probe.images.front().mime_type);
}
