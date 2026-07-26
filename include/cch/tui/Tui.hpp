#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Terminal.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace cch::tui {

class Tui final {
public:
    explicit Tui(Terminal& terminal);
    Tui(Tui&&) = delete;
    Tui& operator=(Tui&&) = delete;
    ~Tui();

    Tui(const Tui&) = delete;
    Tui& operator=(const Tui&) = delete;

    [[nodiscard]] util::Expected<std::reference_wrapper<Component>> add_child(
        std::unique_ptr<Component> component);
    [[nodiscard]] util::ExpectedVoid start();
    [[nodiscard]] util::ExpectedVoid stop();
    [[nodiscard]] util::ExpectedVoid render();
    [[nodiscard]] util::ExpectedVoid set_focus(Component* component);
    void invalidate();

private:
    [[nodiscard]] bool owns(const Component* component) const;
    void handle_input(std::string input);
    void handle_resize(TerminalDimensions dimensions);

    Terminal& terminal_; // must outlive this Tui.
    std::vector<std::unique_ptr<Component>> children_;
    Component* focused_{nullptr}; // Null or aliases an element owned by children_.
    bool started_{false};
};

} // namespace cch::tui
