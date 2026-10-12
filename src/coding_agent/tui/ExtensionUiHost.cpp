#include "ExtensionUiHost.hpp"

#include <utility>

namespace cch::coding_agent::tui {

void ExtensionUiHost::bind(cch::tui::ITui& tui, ExtensionUiRegistrationFactory factory) {
    dispose();
    tui_ = &tui;
    factory_ = std::move(factory);
    refresh();
}

void ExtensionUiHost::refresh() {
    if (tui_ == nullptr || !factory_) return;
    reset();
    factory_(*this);
}

void ExtensionUiHost::reset() noexcept { listeners_.clear(); }

void ExtensionUiHost::dispose() noexcept {
    reset();
    tui_ = nullptr;
    factory_ = nullptr;
}

void ExtensionUiHost::on_terminal_input(cch::tui::RawInputListener listener) {
    if (tui_ == nullptr || !listener) return;
    listeners_.push_back(tui_->add_raw_input_listener(std::move(listener)));
}

} // namespace cch::coding_agent::tui
