#pragma once

#include <cch/tui/Component.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cch::tui {

/// A container that composes child components without padding or background.
class Container final : public Component {
public:
    Container() = default;
    Container(Container&&) noexcept;
    Container& operator=(Container&&) noexcept;
    ~Container() override;

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    [[nodiscard]] support::Expected<std::reference_wrapper<Component>> add_child(
        std::unique_ptr<Component> component);
    /// Remove all children (pi `Container.clear`); used by streaming
    /// components that rebuild their content on update.
    void clear();

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    std::vector<std::unique_ptr<Component>> children_;
};

/// A Container with padding and an optional background hook.
class Box final : public Component {
public:
    explicit Box(
        std::size_t padding_x = 1,
        std::size_t padding_y = 1,
        BackgroundHook background_hook = {});
    Box(Box&&) noexcept;
    Box& operator=(Box&&) noexcept;
    ~Box() override;

    Box(const Box&) = delete;
    Box& operator=(const Box&) = delete;

    [[nodiscard]] support::Expected<std::reference_wrapper<Component>> add_child(
        std::unique_ptr<Component> component);
    void clear();
    /// Replace the background hook (pi `Box.setBgFn`); used by tool
    /// execution to transition pending/success/error backgrounds.
    void set_background_hook(BackgroundHook background_hook);

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    std::vector<std::unique_ptr<Component>> children_;
    std::size_t padding_x_;
    std::size_t padding_y_;
    BackgroundHook background_hook_;
};

/// A Spacer renders empty lines.
class Spacer final : public Component {
public:
    explicit Spacer(std::size_t lines = 1);

    void set_lines(std::size_t lines);

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    std::size_t lines_;
};

} // namespace cch::tui
