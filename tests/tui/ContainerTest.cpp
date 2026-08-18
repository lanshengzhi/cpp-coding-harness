#include <cch/tui/Container.hpp>
#include <cch/tui/Text.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class RawLineComponent final : public cch::tui::Component {
public:
    explicit RawLineComponent(std::string line)
        : line_(std::move(line)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {line_}};
    }

    void invalidate() override {}

private:
    std::string line_;
};

} // namespace

TEST_CASE("Container renders children stacked vertically", "[tui][issue46][container]") {
    cch::tui::Container container;
    auto first_child = std::make_unique<cch::tui::Text>("hello", 0, 0);
    auto second_child = std::make_unique<cch::tui::Text>("world", 0, 0);
    REQUIRE(container.add_child(std::move(first_child)));
    REQUIRE(container.add_child(std::move(second_child)));

    auto result = container.render(10);
    REQUIRE(result);
    REQUIRE(result->lines.size() >= 2);
    CHECK(result->lines[0].find("hello") != std::string::npos);
    CHECK(result->lines[1].find("world") != std::string::npos);
}

TEST_CASE("Container terminates child styling at its line boundary", "[tui][issue46][container]") {
    cch::tui::Container container;
    REQUIRE(container.add_child(std::make_unique<RawLineComponent>("\x1b[31mred")));
    REQUIRE(container.add_child(std::make_unique<RawLineComponent>("plain")));

    const auto result = container.render(8);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 2);
    CHECK(result->lines[0] == "\x1b[31mred\x1b[0m");
    CHECK(result->lines[1] == "plain");
}

TEST_CASE("Container rejects unsafe or overwide child lines", "[tui][issue46][container]") {
    cch::tui::Container unsafe;
    REQUIRE(unsafe.add_child(std::make_unique<RawLineComponent>("\x1b[10Gx")));
    CHECK_FALSE(unsafe.render(8));

    cch::tui::Container overwide;
    REQUIRE(overwide.add_child(std::make_unique<RawLineComponent>("too wide")));
    CHECK_FALSE(overwide.render(3));
}

TEST_CASE("Container rejects null child", "[tui][issue46][container]") {
    cch::tui::Container container;
    auto result = container.add_child(nullptr);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::support::ErrorCode::Validation);
}

TEST_CASE("Container requires positive width", "[tui][issue46][container]") {
    cch::tui::Container container;
    auto result = container.render(0);
    REQUIRE_FALSE(result);
}

TEST_CASE("Box renders with padding and background", "[tui][issue46][container]") {
    cch::tui::Box box(1, 1);
    auto text = std::make_unique<cch::tui::Text>("hi", 0, 0);
    REQUIRE(box.add_child(std::move(text)));

    auto result = box.render(10);
    REQUIRE(result);
    // 1 top padding + 1 content + 1 bottom padding = 3 lines
    CHECK(result->lines.size() == 3);
    // Each line should be width 10
    for (const auto& line : result->lines) {
        CHECK(line.size() == 10);
    }
}

TEST_CASE("Box bounds throwing and overwide background hooks", "[tui][issue46][container]") {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    // The staged build still defends against a throwing background hook; the
    // no-exception build enforces non-throwing hooks by construction.
    cch::tui::Box throwing_box(0, 0, [](std::string) -> std::string {
        throw std::runtime_error("background failed");
    });
    REQUIRE(throwing_box.add_child(std::make_unique<cch::tui::Text>("x", 0, 0)));
    CHECK_FALSE(throwing_box.render(4));
#endif

    cch::tui::Box overwide_box(0, 0, [](std::string line) {
        return line + "x";
    });
    REQUIRE(overwide_box.add_child(std::make_unique<cch::tui::Text>("x", 0, 0)));
    CHECK_FALSE(overwide_box.render(4));
}

TEST_CASE("Box owns a move-only background hook", "[tui][issue46][container]") {
    auto prefix = std::make_unique<std::string>("\x1b[44m");
    cch::tui::Box box(0, 0, [owned = std::move(prefix)](std::string line) {
        return *owned + line;
    });
    REQUIRE(box.add_child(std::make_unique<cch::tui::Text>("x", 0, 0)));

    const auto result = box.render(2);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 1);
    CHECK(result->lines[0].ends_with("\x1b[0m"));
}

TEST_CASE("Box rejects null child", "[tui][issue46][container]") {
    cch::tui::Box box;
    auto result = box.add_child(nullptr);
    REQUIRE_FALSE(result);
}

TEST_CASE("Box rejects width too small for padding", "[tui][issue46][container]") {
    cch::tui::Box box(2, 1);
    auto text = std::make_unique<cch::tui::Text>("hi", 0, 0);
    REQUIRE(box.add_child(std::move(text)));

    // width=2 with padding_x=2 means content_width= -2 → error
    auto result = box.render(2);
    REQUIRE_FALSE(result);
}

TEST_CASE("Box clear removes all children", "[tui][issue46][container]") {
    cch::tui::Box box;
    auto text = std::make_unique<cch::tui::Text>("hi", 0, 0);
    REQUIRE(box.add_child(std::move(text)));
    box.clear();

    auto result = box.render(10);
    REQUIRE(result);
    CHECK(result->lines.empty());
}

TEST_CASE("Spacer renders empty lines", "[tui][issue46][container]") {
    cch::tui::Spacer spacer(3);
    auto result = spacer.render(10);
    REQUIRE(result);
    CHECK(result->lines.size() == 3);
    for (const auto& line : result->lines) {
        CHECK(line.empty());
    }
}

TEST_CASE("Spacer can have lines updated", "[tui][issue46][container]") {
    cch::tui::Spacer spacer(1);
    spacer.set_lines(5);
    auto result = spacer.render(10);
    REQUIRE(result);
    CHECK(result->lines.size() == 5);
}
