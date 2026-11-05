#pragma once

// The codemode owner's passive value types (pi `packages/codemode/src/types.ts`).
// This header is part of the portable subset: it carries no Boost.Asio, no
// WasmEdge and no thread dependency, so the portable no-Node entry and its
// build-level check can include it (ADR 0075, #1194).

#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cch::codemode {

/// One item a codemode script emitted with `text()`, `image()`, or `console.*`
/// (pi `CodemodeOutputItem`). The kind is the pi channel, not a rendering hint.
struct CodemodeOutputItem {
    enum class Kind { Text, Image };
    Kind kind{Kind::Text};
    /// `Text`: the rendered text. `Image`: the base64 image data (no `data:`
    /// prefix), matching pi's `{ type: "image", data, mimeType }`.
    std::string data;
    /// `Image` only: the detected MIME type (pi detects it from the base64
    /// signature; the type in the data URI is ignored).
    std::string mime_type;
    /// pi's `console: true` flag: the item came from `console.*` rather than a
    /// script `text()` call. Frozen pi marks console output this way in the
    /// result's output items while keeping `type: "text"`.
    bool console{false};
};

/// Why a run did not produce a value (pi `CodemodeError`): a script-thrown
/// error, a sandbox-boundary failure, a timeout, or a host cancellation.
struct CodemodeError {
    enum class Kind { Script, Sandbox, Timeout, Aborted };
    Kind kind{Kind::Sandbox};
    /// The user-visible message. A script error carries the thrown error's
    /// `message`; a sandbox error names the boundary that failed.
    std::string message;
    /// Script errors only: the thrown error's `name` (frozen pi reports e.g.
    /// `Error`, `TypeError`, `SyntaxError`, `InternalError`). Absent when the
    /// guest threw a non-Error value, whose payload carries only `message`.
    std::optional<std::string> name{};
    /// Script errors only: the guest's `Name: message` text plus frames,
    /// matching frozen pi's stack field. Absent for non-Error throws.
    std::optional<std::string> stack{};
};

/// pi `CodemodeResultShape.storeWrites`: the write set one successful run
/// reports back to the host. The caller merges it into the snapshot it passes
/// to the next run.
struct CodemodeStoreWrites {
    /// Keys set or overwritten, with their JSON values.
    std::map<std::string, support::JsonValue> set;
    /// Keys deleted through `store(key, undefined)`.
    std::vector<std::string> deleted;
};

/// pi `CodemodeResultShape.calls`: one record per `tools.*`/global call the
/// script issued, in issue order. A call the script never awaited, or one cut
/// short by caller cancellation, is `Cancelled`; a rejected call is `Error`.
struct CodemodeCallRecord {
    enum class Status { Ok, Error, Cancelled };
    /// The registered tool or global name the script called.
    std::string name;
    Status status{Status::Ok};
    /// pi `durationMs`: the call's monotonic duration. Present once the call
    /// settled — for ok, error, and cancelled records alike.
    std::optional<double> duration_ms;
};

/// pi `CodemodeDuring` (`packages/codemode/src/types.ts`): what a
/// deterministic run reports about where the script ended. `seq` is the result
/// the VM was processing when the script finished (0: its first run, before any
/// result), `issued` the total calls it had issued by then.
struct CodemodeDuring {
    std::int64_t seq{0};
    std::int64_t issued{0};
};

/// pi `CodemodeResult`: the terminal outcome of one script run. `error` unset
/// means success and `value_json` (when present) holds the script's return
/// value as JSON text.
struct CodemodeRunResult {
    std::vector<CodemodeOutputItem> output;
    /// pi `calls`: every call the script issued, in issue order.
    std::vector<CodemodeCallRecord> calls;
    std::optional<std::string> value_json;
    std::optional<CodemodeError> error;
    /// pi `storeWrites`: the run's `store()`/`load()` write set. Present only
    /// for successful runs (including `exit()`); failed runs report no writes.
    std::optional<CodemodeStoreWrites> store_writes;
    /// pi `during`: present only for a deterministic (`start`) run, whose
    /// completion metadata names the result the VM was processing. A normal
    /// `run` reports no `during`, exactly as pi's `execute()` does.
    std::optional<CodemodeDuring> during;
};

/// pi inline mode's `DEFAULT_INTERRUPT_BUDGET` (packages/codemode inline.ts):
/// the poll budget an inline execution gets when the caller does not override
/// it. The worker-mode `CodemodeSandbox`'s scripts are bounded by the deadline
/// and carry no poll budget, so an inline caller opts in by setting
/// `CodemodeLimits::interrupt_budget` (this constant when it wants pi's inline
/// default).
inline constexpr std::uint64_t kDefaultInterruptBudget = 100000;

/// One tool the script may call through `tools.<js_name>` (pi `CodemodeTool`).
/// The host lists these into the guest's `toolsJson`, so a call routes back
/// through the tool call handler.
struct CodemodeToolDescriptor {
    /// The tool's registered name (`tools[name]` in the script).
    std::string name;
    /// The identifier form (`tools[jsName]`); the caller derives it as pi's
    /// `toCodemodeIdentifier` does.
    std::string js_name;
    std::string description;
};

/// pi `CodemodeGlobal` (packages/codemode/src/runtime/execution.ts): one host
/// global bound into the script surface for every run of this sandbox. A
/// `spread` global receives the call's argument array; a dotted name such as
/// `models.classify` groups with its namespace into one frozen namespace
/// object. Invalid names, duplicates, and namespace conflicts are construction
/// errors (audit-1053 script-execution.json).
struct CodemodeGlobal {
    /// Top-level identifier (`searchTools`) or dotted namespace member
    /// (`models.classify`).
    std::string name;
    /// pi `spread`: the call's arguments arrive as one array.
    bool spread{false};
};

} // namespace cch::codemode
