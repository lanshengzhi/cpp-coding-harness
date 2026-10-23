#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::codemode {

/// The codemode guest module (`quickjs.wasm`) resolution, following pi's
/// `getQuickJSWasmPath` shape (`packages/coding-agent/src/config.ts`): a
/// compile-time fallback used by an installed Runtime, overridable for tests
/// and embedded hosts. Production resolves the guest that ships next to the
/// installed executable first, then the committed in-tree fixture, so a
/// source build and an installed build both find the module with no new
/// resolution environment variable.
[[nodiscard]] std::filesystem::path default_codemode_guest_wasm_path();

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
};

/// pi `CodemodeSandboxOptions`/`CodemodeExecuteOptions` bounds the sandbox can
/// enforce from outside the guest.
struct CodemodeLimits {
    /// pi's default execution timeout (300 s). The VM's `host_interrupt` import
    /// polls this; a script that exceeds it is interrupted and reported as a
    /// timeout. Zero disables the timeout.
    std::chrono::milliseconds timeout{std::chrono::seconds{300}};
    /// pi `memoryLimitBytes`: cap on the guest's linear memory. The guest
    /// reports an exhaustion as an `InternalError: out of memory` script
    /// error. Unset runs the guest without a memory cap (pi's default).
    std::optional<std::uint64_t> memory_limit_bytes;
};

/// Executes one tool a script requested through `tools.*` or a global, reusing
/// the host's existing tool implementations (and therefore their containment).
/// `arguments_json` is the JSON text of the call argument, or an empty string
/// when the call passed no argument. The result is the tool's JSON text, or an
/// error the sandbox reports to the script as a rejected call. The handler is
/// asynchronous because the host resolves the call through the session's
/// tool pipeline while the sandbox worker stays parked; `signal` is the run's
/// cancellation, so a cancelled run aborts an in-flight call. Stored for the
/// run's duration, so it is a move-only operation (§6.2).
using CodemodeToolCallHandler = std::move_only_function<support::AsyncResult<std::string>(
        std::string_view name, std::string_view arguments_json, std::stop_token signal)>;

/// One tool the script may call through `tools.<js_name>` (pi `CodemodeTool`).
/// The host lists these into the guest's `toolsJson`, so a call routes back
/// through `CodemodeToolCallHandler`.
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

/// The codemode wasm sandbox (spec #865, ticket #874). It loads pi's actual
/// codemode guest (QuickJS compiled to wasm) through WasmEdge, registers only
/// the twelve imports the guest declares (no filesystem, socket, process, or
/// module capability), evaluates pi's codemode prelude and the declared script
/// inside it, and returns the script's value and output.
///
/// The boundary fails closed: an unregistered import refuses instantiation, and
/// a script can reach the host only through the prelude's `bridge`, which the
/// host routes to `text`/`image`/`console` output, the tool call handler, and
/// `done`. There is no host shell or filesystem path into the guest.
///
/// `run` is asynchronous and drives WasmEdge on a fresh worker thread per run
/// (spec #882, #885 — pi `runtime/host.ts`'s "a fresh worker per run keeps
/// termination simple"), so a runaway script never occupies its caller's
/// executor; the VM's interrupt import is polled so a `stop_token` or the
/// timeout can end it. One sandbox owns one guest module; `run` instantiates a
/// fresh instance per call, so a crashed or interrupted guest never poisons a
/// later run.
class CodemodeSandbox {
    struct Impl;

public:
    /// Parse and validate the guest module at `wasm_path`. A missing or invalid
    /// module is an explicit error; nothing is deferred to the first `run`.
    /// `globals` are bound into every run's script surface; invalid names,
    /// duplicates, and namespace conflicts fail here, before any run.
    [[nodiscard]] static support::Expected<std::unique_ptr<CodemodeSandbox>> create(
            std::filesystem::path wasm_path, std::vector<CodemodeGlobal> globals = {});

    /// Construction passkey (§7.7): `std::make_unique` cannot reach a private
    /// constructor, so construction goes through the public constructor below,
    /// whose key only a member of this class can name. `create` is the sole
    /// caller.
    struct ConstructionKey {
    private:
        ConstructionKey() = default;
        friend class CodemodeSandbox;
    };
    CodemodeSandbox(ConstructionKey, std::unique_ptr<Impl> impl);

    CodemodeSandbox(CodemodeSandbox&&) noexcept;
    CodemodeSandbox& operator=(CodemodeSandbox&&) noexcept;
    ~CodemodeSandbox();
    CodemodeSandbox(const CodemodeSandbox&) = delete;
    CodemodeSandbox& operator=(const CodemodeSandbox&) = delete;

    /// Run `script` — a codemode source body (pi `parseCodemodeSource` result) —
    /// as an async function body on a dedicated worker thread, exactly as pi's
    /// worker does, and return its terminal outcome. `tools` become the
    /// script's `tools.*` surface; each call is resolved through `handler` on
    /// the caller's executor while the worker parks. A call with no `handler`,
    /// or a name not in `tools`, is a rejected call the script sees — never a
    /// silent success. A fresh worker (and a fresh guest instance) runs per
    /// call, so a crashed, timed-out, or aborted run never poisons a later one.
    /// `store` is the run's `store()`/`load()` snapshot: the caller seeds it
    /// from prior runs' reported writes and persists the reported writes after
    /// a successful run.
    [[nodiscard]] support::AsyncResult<CodemodeRunResult> run(std::string script,
            std::vector<CodemodeToolDescriptor> tools,
            CodemodeLimits limits,
            std::stop_token stop_token = {},
            CodemodeToolCallHandler handler = {},
            std::map<std::string, support::JsonValue> store = {});

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::codemode
