#pragma once

#include <string_view>

namespace cch::ai {

/// The bundled model catalog generated from the hash-pinned T0 snapshot.
/// Provider display names, authentication, and dispatch remain private
/// policy in BuiltinProviders.cpp and the provider implementation.
[[nodiscard]] std::string_view default_models_json() noexcept;

} // namespace cch::ai
