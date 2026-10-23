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
/// it. This class runs pi's worker mode, whose scripts are bounded by the
/// deadline and carry no poll budget, so an inline caller opts in by setting
/// `CodemodeLimits::interrupt_budget` (this constant when it wants pi's inline
/// default).
inline constexpr std::uint64_t kDefaultInterruptBudget = 100000;

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
    /// pi inline mode's `interruptBudget`: the number of guest interrupt polls
    /// one execution may use (counted across the whole run, including tool
    /// waits) before it fails with `Execution exceeded its interrupt budget of
    /// N`. This is the only limit on a script that computes without awaiting a
    /// tool. Unset keeps this class's worker-mode behavior (pi's worker thread
    /// has no poll budget; the deadline bounds it); inline callers opt in by
    /// setting it, typically to `kDefaultInterruptBudget`. An explicit 0 also
    /// leaves polling unbounded.
    std::optional<std::uint64_t> interrupt_budget;
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

/// One call a deterministic run issued, reported to the caller through
/// `CodemodeRunOptions::on_call`. The caller answers it with
/// `CodemodeRun::deliver`, so the host — not the sandbox — decides every
/// result a replayed script sees (pi `CodemodeHostCall`).
struct CodemodeHostCall {
    /// The VM's call id: unique within the run, numbered from 1 across tools
    /// and globals alike.
    std::int64_t id{0};
    /// pi `target`: a `tools.<name>` call or a bound global.
    enum class Target { Tool, Global };
    Target target{Target::Tool};
    /// The registered tool name, or the global's bound name.
    std::string name;
    /// The call argument as JSON text. Empty when the call passed no argument
    /// (pi `args === undefined`), which is not the same as a JSON `null`.
    std::optional<std::string> arguments_json;
};

/// pi `CodemodeOutcome`: the caller's answer to one issued call. `ok` carries
/// the JSON text of the tool's value (empty for `undefined`); a rejection
/// carries the error message and, optionally, a JSON object whose properties
/// the script's `Error` receives — pi's structured tool failure.
struct CodemodeOutcome {
    bool ok{true};
    /// `ok`: the value's JSON text, or empty for `undefined`.
    std::string value_json;
    /// `!ok`: the rejection message the script sees.
    std::string message;
    /// `!ok`: a JSON object whose properties are merged onto the script's
    /// `Error` (`name`, `message`, and `stack` excepted).
    std::optional<std::string> properties_json;
};

/// What a deterministic run reports as it advances (pi `CodemodeRunOptions`
/// callbacks). The fingerprint arrives before the script's first call; `idle`
/// arrives after each applied result with the total calls issued so far.
struct CodemodeRunCallbacks {
    /// pi `onStarted`: the VM's fingerprint, before the script runs and before
    /// any of its calls.
    std::function<void(std::string_view fingerprint)> on_started;
    /// pi `onCall`: the script issued a call; answer it through
    /// `CodemodeRun::deliver`. A call left unanswered keeps the script waiting.
    std::function<void(const CodemodeHostCall& call)> on_call;
    /// pi `onIdle(seq, issued)`: the VM processed result `seq` (0: the script's
    /// first run, before any result) and waits again.
    std::function<void(std::int64_t seq, std::int64_t issued)> on_idle;
};

/// pi `CodemodeRunOptions`: everything a deterministic run fixes so the same
/// inputs reproduce the same calls and output on any VM with the same
/// fingerprint.
struct CodemodeRunOptions {
    /// The VM clock's start in milliseconds since the epoch: `Date.now()` reads
    /// this until the first result arrives. A negative or non-finite value is
    /// rejected at `start`.
    std::int64_t started_at{0};
    /// Seeds `Math.random()` and the VM's other randomness. A 32-bit unsigned
    /// integer; anything else is rejected at `start`.
    std::uint64_t seed{0};
    /// The run's own tool surface. Names and derived script names must be
    /// unique; the sandbox's registered tools are not used.
    std::vector<CodemodeToolDescriptor> tools;
    /// The run's own globals; validated like `create`'s.
    std::vector<CodemodeGlobal> globals;
    /// `store()`/`load()` snapshot for this run.
    std::map<std::string, support::JsonValue> store;
    /// The fingerprint from the recorded run. If the VM produces a different
    /// fingerprint, the run reports a deterministic sandbox error before the
    /// script runs. Unset accepts the current VM and reports its fingerprint.
    std::optional<std::string> expected_fingerprint;
    /// The seed from the recorded run. A mismatch rejects `start` before a VM
    /// is created. Unset uses `seed` directly.
    std::optional<std::uint64_t> expected_seed;
    /// Per-run guest heap cap; it is part of the fingerprint. Unset leaves the
    /// guest uncapped.
    std::optional<std::uint64_t> memory_limit_bytes;
    /// Deadline for this run. Unset means no deadline, as pi's `start` defaults
    /// to (not the sandbox's default timeout).
    std::optional<std::chrono::milliseconds> timeout;
    /// The run's cancellation. A pre-cancelled token settles the run as aborted
    /// without creating a VM.
    std::stop_token stop_token;
    /// The run's progress callbacks.
    CodemodeRunCallbacks callbacks;
    /// pi's abort reason text, reported when `stop_token` cancels the run.
    std::optional<std::string> abort_message;
};

/// The result of one `deliver` call: either the answer was accepted, or the
/// deterministic protocol refused it. pi throws from `deliver` for exactly
/// these refusals and leaves the run unsettled, so the caller can retry with a
/// valid answer; a rejected delivery never advances the script.
struct CodemodeDeliveryRejection {
    enum class Reason {
        /// The result number is not a positive integer used once.
        ResultNumber,
        /// The clock value is not finite.
        Clock,
        /// No call with that id is waiting for a result.
        UnknownCall,
        /// The rejection properties are not a JSON object.
        Properties,
        /// The answer could not be serialized (a value JSON cannot hold).
        Serialization,
    };
    Reason reason{Reason::UnknownCall};
    /// pi's message text, byte-for-byte.
    std::string message;
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
    CodemodeSandbox(ConstructionKey, std::shared_ptr<Impl> impl);

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
    /// Runs the script; `abort_message` is the frozen abort reason's text
    /// reported when `stop_token` cancels the run (pi reads the AbortSignal
    /// reason's message; the default without a reason is "Execution aborted").
    [[nodiscard]] support::AsyncResult<CodemodeRunResult> run(std::string script,
            std::vector<CodemodeToolDescriptor> tools,
            CodemodeLimits limits,
            std::stop_token stop_token = {},
            CodemodeToolCallHandler handler = {},
            std::map<std::string, support::JsonValue> store = {},
            std::optional<std::string> abort_message = std::nullopt);

    /// One running deterministic execution: the caller's handle on the VM
    /// between `start` and the run's terminal result. `deliver` advances it and
    /// `result` resolves once. pi `CodemodeRun`.
    class Run {
    public:
        Run(Run&&) noexcept;
        Run& operator=(Run&&) noexcept;
        ~Run();
        Run(const Run&) = delete;
        Run& operator=(const Run&) = delete;

        /// pi `CodemodeRun.result`: the run's terminal outcome, delivered once.
        /// A run the caller never answers stays pending, exactly as pi's does.
        [[nodiscard]] support::AsyncResult<CodemodeRunResult> result();

        /// pi `CodemodeRun.finished`: whether the run has its outcome. It is set
        /// when the outcome is decided, before `result` settles; it does not
        /// mean the VM has exited.
        [[nodiscard]] bool finished() const;

        /// pi `CodemodeRun.deliver(seq, now, id, outcome)`: answer call `id` as
        /// result number `seq`, with the VM clock reading `now` (ms since the
        /// epoch) while it processes this result. Results are numbered 1, 2, ...
        /// and applied strictly in that order whatever order they arrive in, so
        /// an answer delivered early waits for the missing number. A rejection
        /// leaves the run unsettled and the call still waiting.
        [[nodiscard]] std::optional<CodemodeDeliveryRejection> deliver(
                std::int64_t seq, double now, std::int64_t id, CodemodeOutcome outcome);

    private:
        struct State;
        explicit Run(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;
        friend class CodemodeSandbox;
    };

    /// pi `CodemodeSandboxBase.start`: run `script` deterministically with the
    /// caller answering its calls. The same script, start time, seed, tools,
    /// globals, store, and answers in the same `seq` order reproduce the same
    /// calls and output on any VM with the same fingerprint, so a host can
    /// replay the run after a restart. The VM runs in UTC, its clock reads
    /// `started_at` and then the `now` of the result being processed, its
    /// randomness comes from `seed`, and `WeakRef`/`FinalizationRegistry` do not
    /// exist. The sandbox's own tools and globals are not used.
    /// A rejected option set (negative start time, a seed that is not a 32-bit
    /// unsigned integer, duplicate tool names or script names, invalid globals)
    /// fails here, before any VM exists.
    [[nodiscard]] support::Expected<Run> start(std::string script, CodemodeRunOptions options);

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::codemode
