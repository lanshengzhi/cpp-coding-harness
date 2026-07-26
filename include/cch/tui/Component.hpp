#pragma once

#include <cch/tui/Input.hpp>
#include <cch/util/Error.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace cch::tui {

using BackgroundHook = std::move_only_function<std::string(std::string)>;

/// A width-bounded piece of terminal presentation.
class Component {
public:
    virtual ~Component() = default;

    [[nodiscard]] virtual util::Expected<std::vector<std::string>> render(std::size_t width) = 0;
    virtual void invalidate() = 0;
};

/// An optional Component capability for receiving decoded terminal input.
class InputHandler {
public:
    virtual ~InputHandler() = default;

    virtual void handle_input(const InputEventVariant& input) = 0;
    [[nodiscard]] virtual bool accepts_key_releases() const = 0;
};

/// An optional Component capability for receiving TUI focus.
class Focusable {
public:
    virtual ~Focusable() = default;

    virtual void set_focused(bool focused) = 0;
    [[nodiscard]] virtual bool focused() const = 0;
};

} // namespace cch::tui
