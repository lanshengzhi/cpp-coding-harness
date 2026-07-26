#pragma once

#include <cch/tui/Component.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cch::tui {

/// A container that composes child components without padding or background.
///
/// Children are rendered at the available width and stacked vertically.
/// Styling from one child does not leak into adjacent children or padding.
class Container final : public Component {
public:
    Container() = default;

    [[nodiscard]] util::Expected<std::reference_wrapper<Component>> add_child(
        std::unique_ptr<Component> component);

    [[nodiscard]] util::Expected<std::vector<std::string>> render(std::size_t width) override;
    void invalidate() override;

private:
    std::vector<std::unique_ptr<Component>> children_;
};

/// A Box is a Container with padding and optional background color.
///
/// Children are rendered inside the content area (width minus padding)
/// and the box applies uniform padding and background to the full width.
class Box final : public Component {
public:
    explicit Box(std::size_t padding_x = 1,
                 std::size_t padding_y = 1,
                 std::function<std::string(std::string)> bg_fn = {});

    [[nodiscard]] util::Expected<std::reference_wrapper<Component>> add_child(
        std::unique_ptr<Component> component);
    void clear();

    [[nodiscard]] util::Expected<std::vector<std::string>> render(std::size_t width) override;
    void invalidate() override;

private:
    std::vector<std::unique_ptr<Component>> children_;
    std::size_t padding_x_;
    std::size_t padding_y_;
    std::function<std::string(std::string)> bg_fn_;
};

/// A Spacer renders empty lines.
class Spacer final : public Component {
public:
    explicit Spacer(std::size_t lines = 1);

    void set_lines(std::size_t lines);

    [[nodiscard]] util::Expected<std::vector<std::string>> render(std::size_t width) override;
    void invalidate() override;

private:
    std::size_t lines_;
};

} // namespace cch::tui
