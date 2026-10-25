#pragma once

// Private to the `cch_codemode` owner: the guest-facing helpers both engines
// need. `Sandbox.cpp` runs pi's worker mode and `Deterministic.cpp` runs the
// deterministic mode; both build the same `toolsJson`/`globalsJson`/`storeJson`
// payloads, validate a global surface the same way, and reuse the same
// WasmEdge function plumbing.

#include <wasmedge/wasmedge.h>

#include <cch/codemode/Sandbox.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace cch::codemode::detail {

inline constexpr std::string_view kBridgeName = "bridge";

/// pi `checkGlobals` (execution.ts): one dot-separated part of a global name
/// must be a JavaScript identifier.
[[nodiscard]] bool is_js_identifier(std::string_view part);

/// pi `checkGlobals`, shaped only by the frozen audit-1053 observations:
/// identifier shape, the reserved name `tools`, duplicate names, and a plain
/// global conflicting with a namespace. The upstream reserved-name set beyond
/// `tools` is not captured, so no further names are invented here.
[[nodiscard]] support::ExpectedVoid validate_globals(const std::vector<CodemodeGlobal>& globals);

/// pi `toolsJson`: the tool table the prelude binds as `tools[jsName]`,
/// `tools[name]`, and `ALL_TOOLS`.
[[nodiscard]] std::string tools_json(const std::vector<CodemodeToolDescriptor>& tools);

/// pi `globalsJson`: the prelude binds each entry as a top-level callable, or
/// as a member of its dotted namespace.
[[nodiscard]] std::string globals_json(const std::vector<CodemodeGlobal>& globals);

/// pi `storeJson`: an object mapping each key to its value's JSON text; the
/// guest keeps the text form (sizes count JSON characters).
[[nodiscard]] std::string store_json(const std::map<std::string, support::JsonValue>& store);

/// pi `storeWrites` payload of `done(ok, payload, writes)`: `[[key, json],
/// [key]]` entries — a two-element entry sets the key, a one-element entry
/// deletes it.
[[nodiscard]] CodemodeStoreWrites parse_store_writes(std::string_view payload);

/// A validation error for a rejected sandbox construction or run option.
[[nodiscard]] support::Error sandbox_error(std::string message, std::string detail = {});

} // namespace cch::codemode::detail
