#include "../../third_party/catch2/catch_test_macros.hpp"

#include <cch/tui/Container.hpp>
#include <cch/tui/Text.hpp>

#include <memory>
#include <string>
#include <vector>

TEST_CASE("Container renders children stacked vertically", "[tui][issue46][container]") {
    cch::tui::Container container;
    auto t1 = std::make_unique<cch::tui::Text>("hello", 0, 0);
    auto t2 = std::make_unique<cch::tui::Text>("world", 0, 0);
    REQUIRE(container.add_child(std::move(t1)));
    REQUIRE(container.add_child(std::move(t2)));

    auto result = container.render(10);
    REQUIRE(result);
    CHECK(result->size() >= 2);
    CHECK((*result)[0].find("hello") != std::string::npos);
    CHECK((*result)[1].find("world") != std::string::npos);
}

TEST_CASE("Container rejects null child", "[tui][issue46][container]") {
    cch::tui::Container container;
    auto result = container.add_child(nullptr);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::util::ErrorCode::Validation);
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
    CHECK(result->size() == 3);
    // Each line should be width 10
    for (const auto& line : *result) {
        CHECK(line.size() == 10);
    }
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
    CHECK(result->empty());
}

TEST_CASE("Spacer renders empty lines", "[tui][issue46][container]") {
    cch::tui::Spacer spacer(3);
    auto result = spacer.render(10);
    REQUIRE(result);
    CHECK(result->size() == 3);
    for (const auto& line : *result) {
        CHECK(line.empty());
    }
}

TEST_CASE("Spacer can have lines updated", "[tui][issue46][container]") {
    cch::tui::Spacer spacer(1);
    spacer.set_lines(5);
    auto result = spacer.render(10);
    REQUIRE(result);
    CHECK(result->size() == 5);
}
