// Named pi-v1.0.4 acceptance for fullscreen image placements (#1012): the
// frozen crop controls, placement-only reuse, per-id cleanup and the iTerm2
// session fallback. Every expectation is read from the verified named bundle
// through `scripts/tui/evidence.py`; nothing here regenerates expectations from
// Pike or falls back to the historical `fixtures/pi-tui/*.json` snapshots.
//
// The observation is a real `TuiAltScreen` write stream reduced to the image
// commands it carried and the screen row each command ran at. Kitty image ids
// are random by construction (pi `allocateImageId`, C++ `allocate_image_id`),
// so both traces rename each id to its first-appearance index: a reused id, a
// second identity for one resource, or a deletion aimed at the wrong resource
// still changes the renamed trace and fails.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Image.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Stack.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VStack.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cch::support::JsonValue;

/// One image command in write order with the screen row it executed at, or no
/// row when no cursor position preceded it inside its own write.
struct ObservedCommand {
    std::optional<std::size_t> row;
    std::string text;

    [[nodiscard]] bool operator==(const ObservedCommand& other) const = default;
};

std::ostream& operator<<(std::ostream& output, const ObservedCommand& command) {
    if (command.row) return output << "@" << *command.row << " " << command.text;
    return output << "@none " << command.text;
}

/// Every kitty graphics and iTerm2 image sequence written from index `from` on,
/// each with the row its own write positioned the cursor at.
[[nodiscard]] std::vector<ObservedCommand> observed_commands(
        const cch::tui::VirtualTerminal& terminal, std::size_t from) {
    std::vector<ObservedCommand> commands;
    const auto& writes = terminal.output();
    for (std::size_t write = from; write < writes.size(); ++write) {
        const auto& chunk = writes[write];
        std::optional<std::size_t> row;
        std::size_t cursor = 0;
        while (cursor < chunk.size()) {
            const auto escape = chunk.find('\x1b', cursor);
            if (escape == std::string::npos) break;
            if (chunk.compare(escape, 2, "\x1b[") == 0) {
                std::size_t probe = escape + 2;
                std::size_t value = 0;
                std::size_t digits = 0;
                while (probe < chunk.size() && std::isdigit(static_cast<unsigned char>(chunk[probe]))) {
                    value = value * 10 + static_cast<std::size_t>(chunk[probe] - '0');
                    ++probe;
                    ++digits;
                }
                if (digits > 0 && probe + 1 < chunk.size() && chunk[probe] == ';' &&
                        std::isdigit(static_cast<unsigned char>(chunk[probe + 1]))) {
                    const auto terminator = chunk.find('H', probe + 1);
                    if (terminator != std::string::npos && value > 0) {
                        row = value - 1;
                        cursor = terminator + 1;
                        continue;
                    }
                }
            } else if (chunk.compare(escape, 3, "\x1b_G") == 0) {
                const auto end = chunk.find("\x1b\\", escape + 3);
                if (end == std::string::npos) break;
                commands.push_back({.row = row, .text = chunk.substr(escape, end + 2 - escape)});
                cursor = end + 2;
                continue;
            } else if (chunk.compare(escape, 12, "\x1b]1337;File=") == 0) {
                const auto end = chunk.find('\x07', escape);
                if (end == std::string::npos) break;
                commands.push_back({.row = row, .text = chunk.substr(escape, end + 1 - escape)});
                cursor = end + 1;
                continue;
            }
            cursor = escape + 1;
        }
    }
    return commands;
}

/// Rename every kitty image id in place to its first-appearance index, so the
/// two random id spaces compare by identity instead of by value. Payload bytes
/// after the control separator never carry ids.
void rename_image_ids(std::vector<ObservedCommand>& commands) {
    std::vector<std::pair<std::string, std::size_t>> seen;
    for (auto& command : commands) {
        const auto controls_end = command.text.find(';');
        const auto controls = std::string_view{command.text}.substr(0, controls_end);
        std::size_t cursor = 0;
        while (true) {
            const auto at = controls.find("i=", cursor);
            if (at == std::string_view::npos) break;
            std::size_t end = at + 2;
            while (end < controls.size() && std::isdigit(static_cast<unsigned char>(controls[end]))) ++end;
            if (end == at + 2) {
                cursor = at + 2;
                continue;
            }
            const auto id = std::string{controls.substr(at + 2, end - at - 2)};
            const auto found = std::find_if(seen.begin(), seen.end(), [&](const auto& entry) {
                return entry.first == id;
            });
            std::size_t number = 0;
            if (found == seen.end()) {
                number = seen.size();
                seen.emplace_back(id, number);
            } else {
                number = found->second;
            }
            const auto replacement = std::format("i=#{}", number);
            const auto renamed_at = command.text.find("i=" + id, cursor);
            command.text.replace(renamed_at, id.size() + 2, replacement);
            cursor = renamed_at + replacement.size();
        }
    }
}

[[nodiscard]] std::vector<ObservedCommand> expected_commands(const JsonValue& phase) {
    std::vector<ObservedCommand> commands;
    for (const auto& entry : phase.at("images").get_array()) {
        const auto& row = entry.at("row");
        commands.push_back({
                .row = row.holds<std::nullptr_t>()
                               ? std::nullopt
                               : std::optional<std::size_t>{static_cast<std::size_t>(row.get_number())},
                .text = entry.at("command").get_string(),
        });
    }
    return commands;
}

[[nodiscard]] std::vector<std::string> string_lines(const JsonValue& value) {
    std::vector<std::string> lines;
    for (const auto& line : value.get_array())
        lines.push_back(line.get_string());
    return lines;
}

[[nodiscard]] std::vector<std::string> trimmed_rows(const cch::tui::VirtualTerminal& terminal) {
    std::vector<std::string> rows;
    rows.reserve(terminal.screen().size());
    for (const auto& row : terminal.screen()) {
        auto trimmed = row;
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        rows.push_back(std::move(trimmed));
    }
    return rows;
}

[[nodiscard]] const JsonValue& scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle.at("scenarios").get_array().front();
}

[[nodiscard]] std::size_t int_of(const JsonValue& value) {
    return static_cast<std::size_t>(value.get_number());
}

[[nodiscard]] cch::tui::VirtualTerminal terminal_of(const JsonValue& scenario,
        cch::tui::InlineImageProtocol inline_images) {
    return cch::tui::VirtualTerminal({.columns = int_of(scenario.at("dimensions").at("columns")),
            .rows = int_of(scenario.at("dimensions").at("rows")),
            .capabilities = {.inline_images = inline_images}});
}

/// The C++ shape of pi's plain `{ render: () => [...] }` transcript: the rows
/// it renders plus the placement sidecars that reserve some of them.
class PlacementChild final : public cch::tui::Component {
public:
    PlacementChild(std::vector<std::string> lines, std::vector<cch::tui::InlineImageRenderRegion> images)
        : lines_(std::move(lines)), images_(std::move(images)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = lines_, .images = images_};
    }

    void invalidate() override {}

private:
    std::vector<std::string> lines_;
    std::vector<cch::tui::InlineImageRenderRegion> images_;
};

/// A placement whose declared row count is the frozen crop denominator; an
/// ancestor clip is free to shorten `region.rows` and raise the source offset.
[[nodiscard]] cch::tui::InlineImageRenderRegion placement(const JsonValue& image,
        std::size_t row,
        std::size_t columns,
        std::size_t rows,
        std::uint64_t resource_id) {
    cch::tui::InlineImageRenderRegion region;
    region.resource_id = resource_id;
    region.revision = 1;
    region.encoded_data = image.at("data").get_string();
    region.mime_type = image.at("mimeType").get_string();
    region.pixel_width = int_of(image.at("widthPx"));
    region.pixel_height = int_of(image.at("heightPx"));
    region.fallback_text =
            cch::tui::image_fallback(region.mime_type, cch::tui::ImagePixelSize{.width = region.pixel_width,
                                                                               .height = region.pixel_height});
    region.region = cch::tui::CellRegion{.column = 0, .row = row, .columns = columns, .rows = rows};
    region.source_rows = rows;
    region.rows_reserved_in_lines = true;
    return region;
}

/// Replays one scenario phase by phase: the viewport rows and the image
/// commands issued since the previous phase must equal the frozen observation.
class PhaseChecker {
public:
    PhaseChecker(cch::tui::VirtualTerminal& terminal, const JsonValue& scenario)
        : terminal_(terminal), phases_(scenario.at("expected").at("phases").get_array()),
          from_(terminal.output().size()) {}

    void check(std::string_view action, bool compare_viewport = true) {
        REQUIRE(phase_index_ < phases_.size());
        const auto& phase = phases_[phase_index_++];
        CHECK(phase.at("action").get_string() == action);
        if (compare_viewport) CHECK(trimmed_rows(terminal_) == string_lines(phase.at("viewport")));
        auto observed = observed_commands(terminal_, from_);
        from_ = terminal_.output().size();
        auto expected = expected_commands(phase);
        rename_image_ids(observed);
        rename_image_ids(expected);
        CHECK(observed == expected);
    }

    /// The phase the last `check` compared, for its scroll or follow state.
    [[nodiscard]] const JsonValue& last() const { return phases_[phase_index_ - 1]; }

    void require_done() const { CHECK(phase_index_ == phases_.size()); }

private:
    cch::tui::VirtualTerminal& terminal_;
    const std::vector<JsonValue>& phases_;
    std::size_t phase_index_{0};
    std::size_t from_{0};
};

} // namespace

TEST_CASE("frozen fullscreen scroll crop keeps the destination rows and source bands",
        "[tui][image][alt-screen][issue1012][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("alt-screen-image.json");
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "image-scroll-crop-round-trip");
    const auto& image = scenario.at("inputs").at("image");

    auto terminal = terminal_of(scenario, cch::tui::InlineImageProtocol::Kitty);
    cch::tui::AltScreen tui(terminal);
    std::vector<std::string> lines{"alpha", "beta", "", "", "", "", "gamma", "delta"};
    std::vector<cch::tui::InlineImageRenderRegion> images;
    images.push_back(placement(image, 2, int_of(image.at("columns")), int_of(image.at("rows")),
                               int_of(image.at("id"))));
    auto viewport = std::make_unique<cch::tui::ScrollView>(
            std::make_unique<PlacementChild>(std::move(lines), std::move(images)),
            cch::tui::ScrollViewOptions{.follow_end = true, .primary = true});
    auto* scroll_top = viewport.get();
    tui.set_layout_root(std::move(viewport));
    PhaseChecker check(terminal, scenario);
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    check.check("initial");
    CHECK(scroll_top->scroll_top() == int_of(check.last().at("scrollTop")));
    tui.scroll_by(-1);
    REQUIRE(tui.render());
    check.check("scroll-up");
    CHECK(scroll_top->scroll_top() == int_of(check.last().at("scrollTop")));
    tui.scroll_by(1);
    REQUIRE(tui.render());
    check.check("scroll-back");
    CHECK(scroll_top->scroll_top() == int_of(check.last().at("scrollTop")));
    check.require_done();
    REQUIRE(tui.stop());
}

TEST_CASE("frozen fullscreen anchor move replays the upload as a placement",
        "[tui][image][alt-screen][issue1012][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("alt-screen-image.json");
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "image-anchor-move-reuse");
    const auto& inputs = scenario.at("inputs");
    const auto& image = inputs.at("image");

    auto terminal = terminal_of(scenario, cch::tui::InlineImageProtocol::Kitty);
    cch::tui::AltScreen tui(terminal);
    auto root = std::make_unique<cch::tui::VStack>();
    auto header = std::make_unique<cch::tui::Text>(inputs.at("content").at("header").get_string(), 0, 0);
    auto* header_pointer = header.get();
    REQUIRE(root->add_child(std::move(header)));
    std::vector<cch::tui::InlineImageRenderRegion> images;
    images.push_back(placement(image, 0, int_of(image.at("columns")), int_of(image.at("rows")),
                               int_of(image.at("id"))));
    REQUIRE(root->add_child(std::make_unique<PlacementChild>(std::vector<std::string>(4), std::move(images))));
    REQUIRE(root->add_child(std::make_unique<cch::tui::Text>(inputs.at("content").at("tail").get_string(), 0, 0)));
    tui.set_layout_root(std::move(root));
    PhaseChecker check(terminal, scenario);
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    check.check("initial");
    header_pointer->set_text(inputs.at("content").at("grownHeader").get_string());
    REQUIRE(tui.render());
    check.check("header-grows");
    check.require_done();
    REQUIRE(tui.stop());
}

TEST_CASE("frozen fullscreen image removal and session teardown clear placements",
        "[tui][image][alt-screen][issue1012][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("alt-screen-image.json");
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "image-removal-session-teardown");
    const auto& inputs = scenario.at("inputs");
    const auto& image = inputs.at("image");

    auto terminal = terminal_of(scenario, cch::tui::InlineImageProtocol::Kitty);
    cch::tui::AltScreen tui(terminal);
    auto root = std::make_unique<cch::tui::VStack>();
    auto* root_pointer = root.get();
    REQUIRE(root->add_child(std::make_unique<cch::tui::Text>(inputs.at("content").at("head").get_string(), 0, 0)));
    std::vector<cch::tui::InlineImageRenderRegion> images;
    images.push_back(placement(image, 0, int_of(image.at("columns")), int_of(image.at("rows")),
                               int_of(image.at("id"))));
    auto child = std::make_unique<PlacementChild>(std::vector<std::string>(4), std::move(images));
    auto* child_pointer = child.get();
    REQUIRE(root->add_child(std::move(child)));
    REQUIRE(root->add_child(std::make_unique<cch::tui::Text>(inputs.at("content").at("tail").get_string(), 0, 0)));
    tui.set_layout_root(std::move(root));
    PhaseChecker check(terminal, scenario);
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    check.check("initial");
    auto removed = root_pointer->remove_child(child_pointer);
    REQUIRE(removed != nullptr);
    REQUIRE(tui.render());
    check.check("image-removed");
    REQUIRE(tui.stop());
    // The alternate buffer returns to the host screen on stop, whose document
    // restore is the host root's contract; this stage owns the deletion.
    check.check("stopped", false);
    check.require_done();
}

TEST_CASE("frozen fullscreen iTerm2 session shows the fallback without image sequences",
        "[tui][image][alt-screen][issue1012][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("iterm2-image.json");
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "iterm2-fullscreen-fallback");
    const auto& image = scenario.at("inputs").at("image");

    auto terminal = terminal_of(scenario, cch::tui::InlineImageProtocol::ITerm2);
    cch::tui::AltScreen tui(terminal);
    tui.set_layout_root(std::make_unique<cch::tui::Image>(
            cch::tui::ImageContent{.encoded_data = image.at("data").get_string(),
                    .mime_type = image.at("mimeType").get_string()},
            cch::tui::ImageOptions{
                    .dimensions = cch::tui::ImagePixelSize{.width = int_of(image.at("widthPx")),
                                                                   .height = int_of(image.at("heightPx"))},
                    .defer_protocol_selection = true}));
    PhaseChecker check(terminal, scenario);
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    check.check("initial");
    REQUIRE(tui.stop());
    check.check("stopped", false);
    check.require_done();
}

TEST_CASE("frozen fullscreen decoded-size cap evicts the offscreen upload by id",
        "[tui][image][alt-screen][issue1012][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("alt-screen-image.json");
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "image-decoded-bytes-eviction");
    const auto& image = scenario.at("inputs").at("image");

    auto terminal = terminal_of(scenario, cch::tui::InlineImageProtocol::Kitty);
    cch::tui::AltScreen tui(terminal);
    std::vector<std::string> lines{"", "one", "two", "three", "four", "five"};
    std::vector<cch::tui::InlineImageRenderRegion> images;
    images.push_back(placement(image, 0, int_of(image.at("columns")), int_of(image.at("rows")),
                               int_of(image.at("id"))));
    auto viewport = std::make_unique<cch::tui::ScrollView>(
            std::make_unique<PlacementChild>(std::move(lines), std::move(images)),
            cch::tui::ScrollViewOptions{.primary = true});
    auto* scroll_top = viewport.get();
    tui.set_layout_root(std::move(viewport));
    PhaseChecker check(terminal, scenario);
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    check.check("initial");
    CHECK(scroll_top->scroll_top() == int_of(check.last().at("scrollTop")));
    tui.scroll_by(1);
    REQUIRE(tui.render());
    check.check("scrolled-off");
    CHECK(scroll_top->scroll_top() == int_of(check.last().at("scrollTop")));
    check.require_done();
    REQUIRE(tui.stop());
}

TEST_CASE("frozen fullscreen offscreen count cap evicts the least recently used ids",
        "[tui][image][alt-screen][issue1012][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("alt-screen-image.json");
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "image-offscreen-count-eviction");
    const auto& inputs = scenario.at("inputs");
    const auto& image = inputs.at("image");
    const auto& image_ids = inputs.at("content").at("imageIds").get_array();
    const auto image_count = int_of(inputs.at("content").at("imageCount"));
    REQUIRE(image_ids.size() == image_count);

    auto terminal = terminal_of(scenario, cch::tui::InlineImageProtocol::Kitty);
    cch::tui::AltScreen tui(terminal);
    std::vector<std::string> lines(image_count);
    std::vector<cch::tui::InlineImageRenderRegion> images;
    images.reserve(image_count);
    for (std::size_t index = 0; index < image_count; ++index) {
        images.push_back(placement(image, index, int_of(image.at("columns")), int_of(image.at("rows")),
                                   int_of(image_ids[index])));
    }
    auto viewport = std::make_unique<cch::tui::ScrollView>(
            std::make_unique<PlacementChild>(std::move(lines), std::move(images)),
            cch::tui::ScrollViewOptions{.primary = true});
    auto* scroll_top = viewport.get();
    tui.set_layout_root(std::move(viewport));
    PhaseChecker check(terminal, scenario);
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    check.check("initial");
    for (std::size_t step = 0; step < 17; ++step) {
        tui.scroll_by(1);
        REQUIRE(tui.render());
        check.check(std::format("scroll-down-{}", step + 1));
        CHECK(scroll_top->scroll_top() == int_of(check.last().at("scrollTop")));
    }
    for (std::size_t step = 0; step < 17; ++step) {
        tui.scroll_by(-1);
        REQUIRE(tui.render());
        check.check(std::format("scroll-up-{}", step + 1));
        CHECK(scroll_top->scroll_top() == int_of(check.last().at("scrollTop")));
    }
    check.require_done();
    REQUIRE(tui.stop());
}

