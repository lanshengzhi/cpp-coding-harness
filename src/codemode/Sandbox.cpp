#include <cch/codemode/Sandbox.hpp>

#include <cch/codemode/Declarations.hpp>

#include "codemode/Deterministic.hpp"
#include "codemode/Guest.hpp"
#include "codemode/Prelude.hpp"
#include "codemode/Shared.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <wasmedge/wasmedge.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef CCH_SOURCE_DIR
#define CCH_SOURCE_DIR ""
#endif

namespace cch::codemode {

using detail::add_function;
using detail::gen_i32;
using detail::globals_json;
using detail::Guest;
using detail::Handle;
using detail::HostClockTimeGet;
using detail::HostFdFdstatGet;
using detail::HostFdWrite;
using detail::HostModuleDenied;
using detail::HostPromiseRejection;
using detail::HostRandomGet;
using detail::HostTimezone;
using detail::HostWasiStub;
using detail::kBridgeName;
using detail::kWasiErnoBadf;
using detail::kWasiErnoNosys;
using detail::kWasiErnoSuccess;
using detail::make_function;
using detail::make_module;
using detail::parse_store_writes;
using detail::sandbox_error;
using detail::store_json;
using detail::tools_json;
using detail::validate_globals;
using detail::VT;

std::filesystem::path default_codemode_guest_wasm_path() {
    // pi resolves the guest through the packaged module tree
    // (`createRequire(...).resolve("quickjs-wasi/quickjs.wasm")`), so the
    // installed product carries its own copy. Pike follows the same shape:
    // the installed Runtime looks for the guest at
    // `share/pike/codemode/quickjs.wasm` next to its executable, and a source
    // build falls back to the committed fixture. No resolution environment
    // variable exists.
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        auto installed = executable.parent_path() / "share" / "pike" / "codemode" / "quickjs.wasm";
        if (std::filesystem::exists(installed, error)) {
            return installed;
        }
    }
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "codemode" / "quickjs" / "quickjs.wasm";
}

namespace {

/// One queued `tools.*`/global call the script made and is awaiting.
struct PendingToolCall {
    Handle id = 0;
    std::string name;
    std::string arguments_json;
    /// Index into `RunState::calls` for this call's public record.
    std::size_t record = 0;
};

/// The bridge between the sandbox worker thread and the host executor. The
/// worker (which owns WasmEdge and can only be stopped by its interrupt flag)
/// parks on `condition_variable` while the host resolves a `tools.*` call
/// through the session's asynchronous tool pipeline. This is pi's worker
/// `parentPort` message pair, realized with a mutex and a condition variable
/// because the host side is an asio coroutine rather than a JS event loop.
struct WorkerAdmissionRelease {
    CodemodeWorkerRelease release;
    std::atomic<bool> released{false};

    void invoke() {
        if (!released.exchange(true) && release) release();
    }
};

struct SandboxLink {
    struct Request {
        int id{0};
        std::string name;
        std::string arguments_json;
    };
    struct Response {
        bool ok{false};
        std::string payload;
    };

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Request> requests;
    std::map<int, Response> responses;
    bool finished{false};
    std::optional<CodemodeRunResult> result;
    /// Set by the host when the run's deadline or cancellation must stop a
    /// wasm-spinning worker; read by the guest's `host_interrupt` import.
    std::atomic<bool> interrupt{false};
    /// The run's absolute deadline (nanoseconds since the steady clock's epoch),
    /// published by the worker when the limits carry a timeout; 0 means none.
    /// The host's request loop enforces it while a tool call waits.
    std::atomic<std::int64_t> deadline_nanos{0};
    /// Set by the worker when the script settled (success, failure, or
    /// exit()): calls still in flight are unawaited and must be cancelled by
    /// the host instead of waiting for their handlers.
    std::atomic<bool> script_settled{false};
    std::atomic<bool> closed{false};
    /// Wakes the host coroutine when the worker has a request or a result.
    std::function<void()> wake_host;

    /// Worker side: forward one call to the host without parking on it. The
    /// engine uses this for calls the script left unawaited at settlement: the
    /// host aborts their handlers so the cancellation is observed.
    void enqueue(int id, std::string_view name, std::string_view arguments_json) {
        {
            std::lock_guard lock(mutex);
            requests.push_back(Request{id, std::string{name}, std::string{arguments_json}});
        }
        if (wake_host) wake_host();
    }

    /// Worker side: enqueue one call and park until the host answers (or the
    /// run is interrupted). Returns nullopt when interrupted without an answer.
    [[nodiscard]] std::optional<Response> dispatch(int id, std::string_view name, std::string_view arguments_json) {
        {
            std::lock_guard lock(mutex);
            requests.push_back(Request{id, std::string{name}, std::string{arguments_json}});
        }
        if (wake_host) wake_host();
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return responses.contains(id) || interrupt.load(); });
        const auto found = responses.find(id);
        if (found == responses.end()) return std::nullopt;
        Response response = std::move(found->second);
        responses.erase(found);
        return response;
    }

    /// Host side: take every parked request.
    [[nodiscard]] std::deque<Request> take_requests() {
        std::lock_guard lock(mutex);
        std::deque<Request> taken;
        taken.swap(requests);
        return taken;
    }

    /// Host side: answer one parked call and wake the worker.
    void deliver(int id, bool ok, std::string payload) {
        {
            std::lock_guard lock(mutex);
            responses.emplace(id, Response{ok, std::move(payload)});
        }
        cv.notify_all();
    }

    /// Host side: flag the run interrupted and wake a worker parked on a call.
    /// The plain `interrupt` store alone cannot end `dispatch`'s wait: only a
    /// notify re-evaluates its predicate.
    void interrupt_worker() {
        interrupt.store(true);
        cv.notify_all();
    }

    /// Worker side: publish the terminal result and wake the host.
    void finish(CodemodeRunResult terminal) {
        {
            std::lock_guard lock(mutex);
            finished = true;
            result = std::move(terminal);
        }
        cv.notify_all();
        if (wake_host) wake_host();
    }

    [[nodiscard]] bool done() {
        std::lock_guard lock(mutex);
        return finished;
    }

    [[nodiscard]] std::optional<CodemodeRunResult> take_result() {
        std::lock_guard lock(mutex);
        if (!result) return std::nullopt;
        return std::move(result);
    }
};

/// All mutable state of one `run`. A pointer to it is the `data` of every host
/// import, so the bridge can reach the guest and the accumulating result.
struct RunState {
    Guest guest;
    CodemodeLimits limits;
    std::stop_token stop_token;
    SandboxLink* link{nullptr};
    std::vector<CodemodeToolDescriptor> tools;
    std::chrono::steady_clock::time_point started;
    bool timed_out = false;
    bool finished = false;
    std::optional<std::string> value_json;
    std::optional<CodemodeError> error;
    std::optional<CodemodeStoreWrites> store_writes;
    std::vector<CodemodeOutputItem> output;
    std::vector<PendingToolCall> pending;
    /// pi `calls`: one public record per issued call, plus its issue time so
    /// the record gains a duration when the call settles or is cancelled.
    std::vector<CodemodeCallRecord> calls;
    std::vector<std::chrono::steady_clock::time_point> call_issued;
    std::uint64_t polls{0};
    bool budget_exhausted{false};
    /// pi's abort reason text (the AbortSignal reason's message); reported when
    /// the host cancels the run. Without a reason pi reports "Execution aborted".
    std::optional<std::string> abort_message;

    [[nodiscard]] bool interrupted() {
        // pi inline counts every interrupt poll across the whole execution and
        // stops the VM once the count passes the budget; the budget outranks
        // the deadline because a computing script reaches the poll count first
        // (frozen inline-interrupt evidence). An unset budget keeps pi's
        // worker-thread behavior: polling itself never fails the run.
        if (limits.interrupt_budget.has_value()) {
            ++polls;
            if (*limits.interrupt_budget > 0 && polls > *limits.interrupt_budget) {
                budget_exhausted = true;
                return true;
            }
        }
        // The script's own deadline is evaluated so a timeout is reported as a
        // timeout even when the host's monitor also flagged the worker.
        if (link != nullptr && link->deadline_nanos.load() > 0 &&
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                                .count() > link->deadline_nanos.load()) {
            timed_out = true;
            return true;
        }
        if (stop_token.stop_requested()) return true;
        if (link != nullptr && link->interrupt.load()) return true;
        return false;
    }

    void emit_done(const Guest& g, const std::vector<Handle>& args) {
        const bool ok = args.size() > 1 && !g.is_undefined(args[1]) && g.read_string(args[1]) == "true";
        if (ok) {
            if (args.size() > 2 && !g.is_undefined(args[2])) value_json = g.read_string(args[2]);
            // pi reports the write set only on success (including exit());
            // failed runs carry no storeWrites.
            if (args.size() > 3 && !g.is_undefined(args[3])) {
                store_writes = parse_store_writes(g.read_string(args[3]));
            }
        } else {
            error = args.size() > 2 ? error_from(args[2], g) : CodemodeError{CodemodeError::Kind::Script, {}};
        }
        finished = true;
        // The host stops waiting on in-flight handlers: anything still pending
        // is an unawaited call this run must report as cancelled.
        if (link != nullptr) {
            link->script_settled.store(true);
            if (link->wake_host) link->wake_host();
        }
    }

    /// The prelude reports a failed run with `describeError(error)`: a JSON
    /// object carrying `message` for every throw, plus `name` and `stack` when
    /// the thrown value was an Error (frozen pi's error taxonomy).
    [[nodiscard]] static CodemodeError error_from(Handle payload, const Guest& g) {
        const std::string raw = g.read_string(payload);
        auto parsed = support::read_json(raw);
        if (parsed && parsed->holds<support::JsonValue::object_t>()) {
            const auto& object = parsed->get_object();
            CodemodeError error{CodemodeError::Kind::Script, raw};
            bool described = false;
            if (const auto it = object.find("message"); it != object.end()) {
                if (const auto* message = it->second.get_if<std::string>()) {
                    error.message = *message;
                    described = true;
                }
            }
            if (const auto it = object.find("name"); it != object.end()) {
                if (const auto* name = it->second.get_if<std::string>()) error.name = *name;
            }
            if (const auto it = object.find("stack"); it != object.end()) {
                if (const auto* stack = it->second.get_if<std::string>()) error.stack = *stack;
            }
            if (described) return error;
        }
        return CodemodeError{CodemodeError::Kind::Script, raw};
    }

    void handle_bridge(const Guest& g, const std::vector<Handle>& args) {
        if (args.empty()) return;
        const std::string kind = g.read_string(args[0]);
        if (kind == "output") {
            CodemodeOutputItem item;
            const std::string channel = args.size() > 1 ? g.read_string(args[1]) : std::string{};
            item.kind = channel == "image" ? CodemodeOutputItem::Kind::Image : CodemodeOutputItem::Kind::Text;
            item.data = args.size() > 2 ? g.read_string(args[2]) : std::string{};
            item.mime_type = args.size() > 3 ? g.read_string(args[3]) : std::string{};
            // The frozen prelude routes `console.*` through its own channel;
            // the result keeps `type: "text"` and marks the item `console`.
            item.console = channel == "console";
            output.push_back(std::move(item));
            return;
        }
        if (kind == "call" || kind == "global") {
            PendingToolCall call;
            // The id is the prelude's counter, never script-controlled; parse it
            // with the non-throwing std::from_chars (the core is
            // -fno-exceptions, §9.3).
            const std::string id_text = args.size() > 1 ? g.read_string(args[1]) : std::string{};
            if (int parsed_id = 0;
                    std::from_chars(id_text.data(), id_text.data() + id_text.size(), parsed_id).ec == std::errc{}) {
                call.id = parsed_id;
            }
            call.name = args.size() > 2 ? g.read_string(args[2]) : std::string{};
            call.arguments_json = args.size() > 3 && !g.is_undefined(args[3]) ? g.read_string(args[3]) : std::string{};
            calls.push_back(CodemodeCallRecord{
                    .name = call.name, .status = CodemodeCallRecord::Status::Ok, .duration_ms = std::nullopt});
            call_issued.push_back(std::chrono::steady_clock::now());
            call.record = calls.size() - 1;
            pending.push_back(std::move(call));
            return;
        }
        if (kind == "done") emit_done(g, args);
    }
};

WasmEdge_Result HostCall(
        void* data, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out) {
    auto* state = static_cast<RunState*>(data);
    Guest guest{WasmEdge_CallingFrameGetExecutor(frame),
            const_cast<WasmEdge_ModuleInstanceContext*>(WasmEdge_CallingFrameGetModuleInstance(frame)),
            WasmEdge_CallingFrameGetMemoryInstance(frame, 0)};
    const std::uint32_t argc = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[3]));
    const std::uint32_t argv = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[4]));
    std::vector<Handle> args;
    args.reserve(argc);
    for (std::uint32_t i = 0; i < argc; ++i) {
        args.push_back(static_cast<Handle>(guest.load_u32(argv + i * 4)));
    }
    state->handle_bridge(guest, args);
    out[0] = gen_i32(guest.undefined());
    return WasmEdge_Result_Success;
}

WasmEdge_Result HostInterrupt(
        void* data, const WasmEdge_CallingFrameContext*, const WasmEdge_Value*, WasmEdge_Value* out) {
    auto* state = static_cast<RunState*>(data);
    out[0] = gen_i32(state->interrupted() ? 1 : 0);
    return WasmEdge_Result_Success;
}

} // namespace

namespace {

struct ActiveSandboxRun {
    std::shared_ptr<SandboxLink> link;
    std::shared_ptr<std::stop_source> call_stop;
};

struct NativeWorkerTask {
    CodemodeWorkerTask task;
};

struct NativeWorkerTaskState {
    std::unique_ptr<NativeWorkerTask> task;
};

struct NativeWorkerJoinState {
    std::mutex mutex;
    std::condition_variable condition;
    pthread_t worker{};
    CodemodeWorkerExit exited;
    bool ready{false};
    bool start_failed{false};
};

struct NativeWorkerJoinThreadState {
    std::shared_ptr<NativeWorkerJoinState> state;
};

void* run_worker_task(void* data) noexcept {
    std::unique_ptr<NativeWorkerTaskState> state{static_cast<NativeWorkerTaskState*>(data)};
    state->task->task();
    return nullptr;
}

void* join_native_worker(void* data) noexcept {
    std::unique_ptr<NativeWorkerJoinThreadState> thread_state{static_cast<NativeWorkerJoinThreadState*>(data)};
    const auto state = std::move(thread_state->state);
    pthread_t worker;
    {
        std::unique_lock lock(state->mutex);
        state->condition.wait(lock, [&state] { return state->ready; });
        if (state->start_failed) return nullptr;
        worker = state->worker;
    }
    if (pthread_join(worker, nullptr) != 0) std::terminate();
    state->exited();
    return nullptr;
}

[[nodiscard]] std::shared_ptr<std::mutex> default_worker_admission_mutex() {
    static auto mutex = std::make_shared<std::mutex>();
    return mutex;
}

[[nodiscard]] std::shared_ptr<std::size_t> default_worker_count() {
    static auto count = std::make_shared<std::size_t>(0);
    return count;
}

[[nodiscard]] support::Expected<void> start_native_worker(CodemodeWorkerTask task, CodemodeWorkerExit exited) {
    pthread_attr_t joiner_attributes;
    const int initialized = pthread_attr_init(&joiner_attributes);
    if (initialized != 0) {
        return std::unexpected(support::make_error(support::ErrorCode::Process,
                std::format("could not initialize worker join attributes: {}", initialized)));
    }
    const int detached = pthread_attr_setdetachstate(&joiner_attributes, PTHREAD_CREATE_DETACHED);
    if (detached != 0) {
        (void)pthread_attr_destroy(&joiner_attributes);
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, std::format("could not configure worker join thread: {}", detached)));
    }

    auto join_state = std::make_shared<NativeWorkerJoinState>();
    join_state->exited = std::move(exited);
    auto joiner_state = std::make_unique<NativeWorkerJoinThreadState>(NativeWorkerJoinThreadState{join_state});
    pthread_t joiner;
    const int joiner_created = pthread_create(&joiner, &joiner_attributes, join_native_worker, joiner_state.get());
    (void)pthread_attr_destroy(&joiner_attributes);
    if (joiner_created != 0) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, std::format("could not start worker join thread: {}", joiner_created)));
    }
    (void)joiner_state.release();

    auto worker_state = std::make_unique<NativeWorkerTaskState>(
            NativeWorkerTaskState{std::make_unique<NativeWorkerTask>(NativeWorkerTask{std::move(task)})});
    pthread_t worker;
    const int worker_created = pthread_create(&worker, nullptr, run_worker_task, worker_state.get());
    if (worker_created != 0) {
        {
            std::lock_guard lock(join_state->mutex);
            join_state->start_failed = true;
            join_state->ready = true;
        }
        join_state->condition.notify_one();
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, std::format("could not start codemode worker: {}", worker_created)));
    }
    (void)worker_state.release();
    {
        std::lock_guard lock(join_state->mutex);
        join_state->worker = worker;
        join_state->ready = true;
    }
    join_state->condition.notify_one();
    return {};
}

} // namespace

struct CodemodeSandbox::Impl {
    std::filesystem::path wasm_path;
    std::vector<CodemodeGlobal> globals;
    CodemodeWorkerHost worker_host;
    std::mutex worker_host_mutex;
    std::mutex lifecycle_mutex;
    bool closed{false};
    std::vector<std::weak_ptr<ActiveSandboxRun>> active_runs;
    WasmEdge_ConfigureContext* configure = nullptr;
    WasmEdge_ASTModuleContext* ast = nullptr;

    ~Impl() {
        if (ast != nullptr) WasmEdge_ASTModuleDelete(ast);
        if (configure != nullptr) WasmEdge_ConfigureDelete(configure);
    }
};

support::Expected<std::unique_ptr<CodemodeSandbox>> CodemodeSandbox::create(
        std::filesystem::path wasm_path, std::vector<CodemodeGlobal> globals) {
    return create(std::move(wasm_path), std::move(globals), {});
}

support::Expected<std::unique_ptr<CodemodeSandbox>> CodemodeSandbox::create(
        std::filesystem::path wasm_path, std::vector<CodemodeGlobal> globals, CodemodeWorkerHost worker_host) {
    // pi `checkGlobals` rejects a bad surface before the guest module is
    // considered, so construction fails without touching the filesystem.
    if (auto valid = validate_globals(globals); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    std::error_code exists_error;
    if (!std::filesystem::exists(wasm_path, exists_error)) {
        return std::unexpected(
                sandbox_error(std::format("codemode sandbox guest '{}' does not exist", wasm_path.string()),
                        "the codemode guest module (quickjs.wasm) must be present to run declared scripts"));
    }
    auto impl = std::make_shared<Impl>();
    impl->wasm_path = std::move(wasm_path);
    impl->globals = std::move(globals);
    impl->worker_host = std::move(worker_host);
    impl->configure = WasmEdge_ConfigureCreate();
    WasmEdge_LoaderContext* loader = WasmEdge_LoaderCreate(impl->configure);
    const WasmEdge_Result loaded = WasmEdge_LoaderParseFromFile(loader, &impl->ast, impl->wasm_path.c_str());
    WasmEdge_LoaderDelete(loader);
    if (!WasmEdge_ResultOK(loaded)) {
        return std::unexpected(
                sandbox_error(std::format("codemode sandbox guest '{}' could not be loaded", impl->wasm_path.string()),
                        WasmEdge_ResultGetMessage(loaded)));
    }
    WasmEdge_ValidatorContext* validator = WasmEdge_ValidatorCreate(impl->configure);
    const WasmEdge_Result valid = WasmEdge_ValidatorValidate(validator, impl->ast);
    WasmEdge_ValidatorDelete(validator);
    if (!WasmEdge_ResultOK(valid)) {
        return std::unexpected(sandbox_error(
                std::format("codemode sandbox guest '{}' is not a valid WebAssembly module", impl->wasm_path.string()),
                WasmEdge_ResultGetMessage(valid)));
    }
    if (!impl->worker_host.acquire_worker) {
        const auto admission_mutex = default_worker_admission_mutex();
        const auto active_workers = default_worker_count();
        impl->worker_host.acquire_worker = [admission_mutex,
                                                   active_workers]() -> support::Expected<CodemodeWorkerRelease> {
            {
                std::lock_guard lock(*admission_mutex);
                if (*active_workers >= 2) {
                    return std::unexpected(support::make_error(support::ErrorCode::Busy, "worker capacity exhausted"));
                }
                ++*active_workers;
            }
            return CodemodeWorkerRelease{[admission_mutex, active_workers]() {
                std::lock_guard lock(*admission_mutex);
                --*active_workers;
            }};
        };
    }
    if (!impl->worker_host.start_worker) impl->worker_host.start_worker = start_native_worker;
    return std::make_unique<CodemodeSandbox>(ConstructionKey{}, std::move(impl));
}

CodemodeSandbox::CodemodeSandbox(ConstructionKey, std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

void CodemodeSandbox::close() {
    std::vector<std::shared_ptr<ActiveSandboxRun>> active_runs;
    {
        std::lock_guard lock(impl_->lifecycle_mutex);
        if (impl_->closed) return;
        impl_->closed = true;
        std::erase_if(impl_->active_runs, [&active_runs](const std::weak_ptr<ActiveSandboxRun>& weak) {
            if (auto active = weak.lock()) {
                active_runs.push_back(std::move(active));
                return false;
            }
            return true;
        });
    }
    for (const auto& active : active_runs) {
        active->link->closed.store(true);
        active->link->interrupt_worker();
        active->call_stop->request_stop();
        if (active->link->wake_host) active->link->wake_host();
    }
}

CodemodeSandbox::~CodemodeSandbox() = default;
CodemodeSandbox::CodemodeSandbox(CodemodeSandbox&&) noexcept = default;
CodemodeSandbox& CodemodeSandbox::operator=(CodemodeSandbox&&) noexcept = default;

namespace {

/// `drain` runs queued guest microtasks. It stops early once the script settled
/// or the host interrupted it, so a runaway microtask loop cannot spin here.
bool drain_jobs(const Guest& guest, const RunState& state) {
    return detail::drain_jobs(guest, [&state] {
        return state.finished || state.timed_out || state.stop_token.stop_requested() ||
               (state.link != nullptr && state.link->interrupt.load());
    });
}

} // namespace

namespace {

/// The synchronous sandbox engine, run on the dedicated worker thread. It
/// owns WasmEdge for the duration of one run and resolves every `tools.*` call
/// through `link`, parking while the host executor runs the session tool.
[[nodiscard]] CodemodeRunResult run_engine(std::string script,
        std::vector<CodemodeToolDescriptor> tools,
        CodemodeLimits limits,
        std::stop_token stop_token,
        SandboxLink& link,
        WasmEdge_ASTModuleContext* ast,
        const std::string& globals_list_json,
        const std::string& store_snapshot_json,
        std::optional<std::string> abort_message) {
    RunState state;
    state.limits = limits;
    state.stop_token = stop_token;
    state.link = &link;
    state.abort_message = std::move(abort_message);
    state.tools = std::move(tools);
    state.started = std::chrono::steady_clock::now();
    // The interrupt handler is armed only once the script starts (below), so the
    // timeout bounds the script, not the guest's one-time prelude evaluation.

    auto finish = [&state]() -> CodemodeRunResult {
        // Calls that never settled — the script returned or was cancelled with
        // the call unawaited — are pi `cancelled` records with a duration.
        const auto settled_at = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < state.calls.size(); ++i) {
            if (state.calls[i].duration_ms.has_value()) continue;
            state.calls[i].status = CodemodeCallRecord::Status::Cancelled;
            state.calls[i].duration_ms =
                    std::chrono::duration<double, std::milli>(settled_at - state.call_issued[i]).count();
        }
        CodemodeRunResult result;
        result.output = std::move(state.output);
        result.calls = std::move(state.calls);
        result.value_json = std::move(state.value_json);
        result.error = std::move(state.error);
        result.store_writes = std::move(state.store_writes);
        return result;
    };

    if (stop_token.stop_requested()) {
        // pi settles a pre-aborted execution with the abort reason's message
        // and never starts a VM for it (frozen inline-interrupt evidence).
        state.error = CodemodeError{
                CodemodeError::Kind::Aborted, state.abort_message.value_or(std::string{"Execution aborted"})};
        return finish();
    }

    // Each run gets its own configure: the executor references it for its
    // whole lifetime and WasmEdge's C API can only overwrite, never unset, a
    // page limit (a fresh configure defaults to 65536 pages — the 4 GiB
    // wasm32 ceiling, i.e. uncapped). pi `memoryLimitBytes` caps the guest's
    // linear memory in 64 KiB pages so an exhausting allocation fails inside
    // the guest as a catchable `InternalError: out of memory` script error.
    WasmEdge_ConfigureContext* run_configure = WasmEdge_ConfigureCreate();
    if (limits.memory_limit_bytes.has_value()) {
        WasmEdge_ConfigureSetMaxMemoryPage(
                run_configure, static_cast<std::uint32_t>((*limits.memory_limit_bytes + 65535) / 65536));
    }

    WasmEdge_StoreContext* store = WasmEdge_StoreCreate();
    WasmEdge_ExecutorContext* executor = WasmEdge_ExecutorCreate(run_configure, nullptr);

    auto fail_sandbox = [&](std::string message) {
        state.error = CodemodeError{CodemodeError::Kind::Sandbox, std::move(message)};
    };

    WasmEdge_ModuleInstanceContext* env = make_module("env");
    add_function(env,
            "host_call",
            make_function(HostCall,
                    &state,
                    {WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32},
                    {WasmEdge_ValType_I32}));
    add_function(env, "host_interrupt", make_function(HostInterrupt, &state, {}, {WasmEdge_ValType_I32}));
    add_function(env,
            "host_promise_rejection",
            make_function(HostPromiseRejection,
                    &state,
                    {WasmEdge_ValType_I32, WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                    {}));
    add_function(env,
            "host_get_timezone_offset",
            make_function(HostTimezone, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));
    add_function(env,
            "host_module_normalize",
            make_function(
                    HostModuleDenied, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));
    add_function(env,
            "host_module_load",
            make_function(
                    HostModuleDenied, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));

    WasmEdge_ModuleInstanceContext* wasi = make_module("wasi_snapshot_preview1");
    add_function(wasi,
            "fd_write",
            make_function(HostFdWrite,
                    &state,
                    {WasmEdge_ValType_I32, WasmEdge_ValType_I32, WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                    {WasmEdge_ValType_I32}));
    add_function(wasi, "fd_close", make_function(HostWasiStub, &state, {WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));
    add_function(wasi,
            "fd_fdstat_get",
            make_function(
                    HostFdFdstatGet, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));
    add_function(wasi,
            "fd_seek",
            make_function(HostWasiStub,
                    &state,
                    {WasmEdge_ValType_I32, WasmEdge_ValType_I64, WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                    {WasmEdge_ValType_I32}));
    add_function(wasi,
            "clock_time_get",
            make_function(HostClockTimeGet,
                    &state,
                    {WasmEdge_ValType_I32, WasmEdge_ValType_I64, WasmEdge_ValType_I32},
                    {WasmEdge_ValType_I32}));
    add_function(wasi,
            "random_get",
            make_function(HostRandomGet, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));

    // Not registering a module the guest imports refuses instantiation:
    // fail-closed at link time, before any script runs.
    const bool registered = WasmEdge_ResultOK(WasmEdge_ExecutorRegisterImport(executor, store, env)) &&
                            WasmEdge_ResultOK(WasmEdge_ExecutorRegisterImport(executor, store, wasi));

    WasmEdge_ModuleInstanceContext* module = nullptr;
    const WasmEdge_Result instantiated =
            registered ? WasmEdge_ExecutorInstantiate(executor, &module, store, ast) : WasmEdge_Result_Fail;
    if (!registered || !WasmEdge_ResultOK(instantiated)) {
        fail_sandbox(std::format("The codemode sandbox denied the script: the guest could not be instantiated "
                                 "({}). A required import was not provided.",
                registered ? WasmEdge_ResultGetMessage(instantiated) : "import registration failed"));
        WasmEdge_ModuleInstanceDelete(env);
        WasmEdge_ModuleInstanceDelete(wasi);
        WasmEdge_ExecutorDelete(executor);
        WasmEdge_StoreDelete(store);
        WasmEdge_ConfigureDelete(run_configure);
        return finish();
    }

    WasmEdge_String memory_name = WasmEdge_StringCreateByCString("memory");
    state.guest.executor = executor;
    state.guest.module = module;
    state.guest.memory = WasmEdge_ModuleInstanceFindMemory(module, memory_name);
    WasmEdge_StringDelete(memory_name);

    auto cleanup = [&]() {
        WasmEdge_ModuleInstanceDelete(env);
        WasmEdge_ModuleInstanceDelete(wasi);
        WasmEdge_ModuleInstanceDelete(module);
        WasmEdge_ExecutorDelete(executor);
        WasmEdge_StoreDelete(store);
        WasmEdge_ConfigureDelete(run_configure);
    };

    if (state.guest.memory == nullptr) {
        fail_sandbox("The codemode sandbox denied the script: the guest exports no linear memory.");
        cleanup();
        return finish();
    }

    const Guest& g = state.guest;
    g.call_void(g.find("_initialize"), {});
    if (g.call_i32(g.find("qjs_init"), {}) != 0) {
        fail_sandbox("The codemode sandbox guest failed to initialize (qjs_init).");
        cleanup();
        return finish();
    }

    // Evaluate pi's prelude; it must yield the API factory. A prelude failure is
    // a sandbox bug, not a script error.
    {
        const std::uint32_t code_ptr = g.write_cstr(kCodemodePreludeSource);
        const std::uint32_t name_ptr = g.write_cstr("codemode-prelude.js");
        const Handle prelude = g.call_i32(g.find("qjs_eval"),
                {gen_i32(static_cast<std::int32_t>(code_ptr)),
                        gen_i32(static_cast<std::int32_t>(std::strlen(kCodemodePreludeSource))),
                        gen_i32(static_cast<std::int32_t>(name_ptr)),
                        gen_i32(0)});
        if (g.is_exception(prelude)) {
            fail_sandbox(std::format("The codemode sandbox prelude failed: {}", g.exception_text()));
            cleanup();
            return finish();
        }
        const std::uint32_t bridge_name_ptr = g.write_cstr(kBridgeName);
        const Handle bridge = g.call_i32(g.find("qjs_new_host_function"),
                {gen_i32(static_cast<std::int32_t>(bridge_name_ptr)),
                        gen_i32(static_cast<std::int32_t>(kBridgeName.size())),
                        gen_i32(0)});
        const Handle undefined = g.undefined();
        const std::string tool_list = tools_json(state.tools);
        const Handle api = g.call(prelude,
                undefined,
                {bridge, g.new_string(tool_list), g.new_string(globals_list_json), g.new_string(store_snapshot_json)});
        const Handle run_fn = g.get_prop(api, "run");
        const Handle settle_fn = g.get_prop(api, "settle");
        const Handle stalled_fn = g.get_prop(api, "stalled");

        // Wrap the declared source as pi's worker does, so `return` and
        // top-level `await` work and line numbers match the source file.
        const std::string wrapped = "(async (tools, console) => {" + std::string{script} + "\n})";
        const std::uint32_t script_ptr = g.write_cstr(wrapped);
        const std::uint32_t script_name_ptr = g.write_cstr("codemode.js");
        const Handle script_fn = g.call_i32(g.find("qjs_eval"),
                {gen_i32(static_cast<std::int32_t>(script_ptr)),
                        gen_i32(static_cast<std::int32_t>(wrapped.size())),
                        gen_i32(static_cast<std::int32_t>(script_name_ptr)),
                        gen_i32(0)});
        if (g.is_exception(script_fn)) {
            state.error = g.exception_error();
            cleanup();
            return finish();
        }

        // Arm the deadline and interrupt before the script body runs, so the
        // timeout bounds the script (including a synchronous runaway loop), not
        // guest startup or fixed prelude evaluation. Publish it for the host
        // request loop and the run's cancellation monitor.
        state.started = std::chrono::steady_clock::now();
        if (limits.timeout.count() > 0) {
            const auto deadline = state.started + limits.timeout;
            link.deadline_nanos.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count());
        }
        g.call_void(g.find("qjs_set_interrupt_handler"), {gen_i32(1)});
        (void)g.call(run_fn, api, {script_fn});
        drain_jobs(g, state);

        // Settle every tool call the script is awaiting, then drain again. A
        // tool that is missing or has no handler rejects the call the script
        // sees — never a silent success.
        while (!state.finished) {
            if (state.interrupted()) break;
            if (state.pending.empty()) {
                (void)g.call(stalled_fn, api, {});
                drain_jobs(g, state);
                if (state.interrupted()) break;
                if (!state.finished) {
                    fail_sandbox("The codemode script did not settle: it is waiting on a promise that can "
                                 "never resolve, and the sandbox has no timers or I/O.");
                }
                break;
            }
            std::vector<PendingToolCall> calls = std::move(state.pending);
            state.pending.clear();
            for (std::size_t index = 0; index < calls.size(); ++index) {
                const auto& call = calls[index];
                const auto id_handle =
                        g.call_i32(g.find("qjs_new_number"), {WasmEdge_ValueGenF64(static_cast<double>(call.id))});
                const std::optional<SandboxLink::Response> response =
                        state.link->dispatch(call.id, call.name, call.arguments_json);
                if (!response) {
                    // The run was interrupted while the call was parked; the
                    // workers's `interrupted()` (or the timeout) ends the run.
                    // Requeue the undispatched remainder so the host aborts
                    // their handlers too.
                    state.pending.insert(
                            state.pending.end(), calls.begin() + static_cast<std::ptrdiff_t>(index + 1), calls.end());
                    break;
                }
                auto& record = state.calls[call.record];
                record.status = response->ok ? CodemodeCallRecord::Status::Ok : CodemodeCallRecord::Status::Error;
                record.duration_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - state.call_issued[call.record])
                                             .count();
                const Handle payload_handle = g.new_string(response->payload);
                (void)g.call(
                        settle_fn, api, {id_handle, response->ok ? g.true_value() : g.false_value(), payload_handle});
                drain_jobs(g, state);
            }
        }
    }

    // Calls still pending when the script settled are unawaited: forward them
    // to the host so each handler is invoked and aborted (the frozen rule the
    // tool observes), and so its record reports `cancelled` with a duration.
    for (const auto& call : state.pending) {
        state.link->enqueue(call.id, call.name, call.arguments_json);
    }
    state.pending.clear();

    if (!state.finished) {
        if (!state.timed_out && state.link != nullptr && state.link->deadline_nanos.load() > 0 &&
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                                .count() > state.link->deadline_nanos.load()) {
            state.timed_out = true;
        }
        if (state.budget_exhausted) {
            // The exhausted budget decides the outcome, like pi's inline
            // launcher: the run fails as a timeout with the budget message.
            state.error = CodemodeError{CodemodeError::Kind::Timeout,
                    std::format("Execution exceeded its interrupt budget of {}",
                            state.limits.interrupt_budget.value_or(0))};
        } else if (state.timed_out) {
            state.error = CodemodeError{CodemodeError::Kind::Timeout,
                    std::format("Execution timed out after {} ms", state.limits.timeout.count())};
        } else if (state.stop_token.stop_requested()) {
            state.error =
                    CodemodeError{CodemodeError::Kind::Aborted, state.abort_message.value_or("Execution aborted")};
        } else if (!state.error.has_value()) {
            fail_sandbox("The codemode sandbox guest stopped without reporting a result.");
        }
    }

    cleanup();
    return finish();
}

} // namespace

support::AsyncResult<CodemodeRunResult> CodemodeSandbox::run(std::string script,
        std::vector<CodemodeToolDescriptor> tools,
        CodemodeLimits limits,
        std::stop_token stop_token,
        CodemodeToolCallHandler handler,
        std::map<std::string, support::JsonValue> store,
        std::optional<std::string> abort_message) {
    return support::detail::make_async_result([impl = impl_,
                                                      script = std::move(script),
                                                      tools = std::move(tools),
                                                      limits,
                                                      stop_token,
                                                      handler = std::move(handler),
                                                      globals_list_json = globals_json(impl_->globals),
                                                      store_snapshot_json = store_json(store),
                                                      abort_message = std::move(abort_message)]() mutable
                                                      -> boost::asio::awaitable<support::Expected<CodemodeRunResult>> {
        auto executor = co_await boost::asio::this_coro::executor;
        auto link = std::make_shared<SandboxLink>();
        auto timer = std::make_shared<boost::asio::steady_timer>(executor);
        link->wake_host = [executor, timer]() { boost::asio::post(executor, [timer]() { timer->cancel(); }); };

        // The per-run cancellation the nested tool calls observe: requested
        // when the run's own stop token is, or when its deadline passes, so a
        // call in flight aborts instead of pinning the host forever.
        auto call_stop = std::make_shared<std::stop_source>();
        if (stop_token.stop_requested()) call_stop->request_stop();
        auto active_run = std::make_shared<ActiveSandboxRun>(ActiveSandboxRun{link, call_stop});
        {
            std::lock_guard lock(impl->lifecycle_mutex);
            if (impl->closed) {
                CodemodeRunResult closed_result;
                closed_result.error = CodemodeError{CodemodeError::Kind::Aborted, "Sandbox closed"};
                co_return closed_result;
            }
            std::erase_if(
                    impl->active_runs, [](const std::weak_ptr<ActiveSandboxRun>& weak) { return weak.expired(); });
            impl->active_runs.push_back(active_run);
        }
        auto monitor_timer = std::make_shared<boost::asio::steady_timer>(executor);
        boost::asio::co_spawn(
                executor,
                [stop_token, call_stop, monitor_timer, link]() -> boost::asio::awaitable<void> {
                    while (true) {
                        monitor_timer->expires_after(std::chrono::milliseconds{25});
                        const auto [error] =
                                co_await monitor_timer->async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                        if (error) co_return; // cancelled: the run finished
                        const auto deadline_nanos = link->deadline_nanos.load();
                        const bool expired =
                                deadline_nanos > 0 && std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                              std::chrono::steady_clock::now().time_since_epoch())
                                                                      .count() >= deadline_nanos;
                        if (stop_token.stop_requested() || expired) {
                            // Cancellation and the deadline stop a wasm-spinning
                            // worker and abort an in-flight nested call. Wake the
                            // host loop so it can settle without joining the worker.
                            link->interrupt_worker();
                            call_stop->request_stop();
                            if (link->wake_host) link->wake_host();
                            co_return;
                        }
                        if (link->done()) co_return;
                    }
                },
                boost::asio::detached);

        support::Expected<CodemodeWorkerRelease> admission;
        {
            std::lock_guard host_lock(impl->worker_host_mutex);
            admission = impl->worker_host.acquire_worker();
        }
        if (!admission) {
            monitor_timer->cancel();
            CodemodeRunResult denied;
            denied.error = CodemodeError{CodemodeError::Kind::Sandbox, admission.error().message};
            co_return denied;
        }
        auto release = std::make_shared<WorkerAdmissionRelease>();
        release->release = std::move(*admission);
        support::Expected<void> started;
        {
            std::lock_guard lifecycle_lock(impl->lifecycle_mutex);
            if (impl->closed) {
                release->invoke();
                monitor_timer->cancel();
                CodemodeRunResult closed_result;
                closed_result.error = CodemodeError{CodemodeError::Kind::Aborted, "Sandbox closed"};
                co_return closed_result;
            }
            std::lock_guard host_lock(impl->worker_host_mutex);
            started = impl->worker_host.start_worker(
                    [impl,
                            link,
                            script = std::move(script),
                            tools = std::move(tools),
                            limits,
                            stop_token,
                            globals_list_json = std::move(globals_list_json),
                            store_snapshot_json = std::move(store_snapshot_json),
                            abort_message = std::move(abort_message)]() mutable {
                        link->finish(run_engine(std::move(script),
                                std::move(tools),
                                limits,
                                stop_token,
                                *link,
                                impl->ast,
                                globals_list_json,
                                store_snapshot_json,
                                abort_message));
                    },
                    [release]() { release->invoke(); });
        }
        if (!started) {
            release->invoke();
            monitor_timer->cancel();
            CodemodeRunResult failed_start;
            failed_start.error = CodemodeError{CodemodeError::Kind::Sandbox, started.error().message};
            co_return failed_start;
        }

        // One handler resolves every call of this run; the shared owner lets
        // each in-flight call run as its own coroutine below.
        auto handler_ptr = std::make_shared<CodemodeToolCallHandler>(std::move(handler));
        while (true) {
            auto requests = link->take_requests();
            for (auto& request : requests) {
                auto slot = std::make_shared<std::optional<SandboxLink::Response>>();
                boost::asio::co_spawn(
                        executor,
                        [executor, handler_ptr, request, slot, call_stop, timer]() -> boost::asio::awaitable<void> {
                            std::string payload;
                            bool ok = false;
                            if (*handler_ptr) {
                                auto outcome = co_await support::detail::await_async_result(
                                        (*handler_ptr)(request.name, request.arguments_json, call_stop->get_token()));
                                if (outcome) {
                                    ok = true;
                                    payload = std::move(*outcome);
                                } else {
                                    payload = outcome.error().message;
                                }
                            } else {
                                payload =
                                        std::format("tool '{}' is not available to the codemode sandbox", request.name);
                            }
                            slot->emplace(SandboxLink::Response{ok, std::move(payload)});
                            // Wake the waiting loop without a full timer tick.
                            boost::asio::post(executor, [timer]() { timer->cancel(); });
                        },
                        boost::asio::detached);
                // Wait for the resolver. If the script settled first, this call
                // is unawaited: abort it and let the parked worker finish, then
                // keep waiting so the tool observes the cancellation before the
                // run completes (the frozen rule the host contract promises).
                bool answer_moot = false;
                while (!slot->has_value()) {
                    if (link->closed.load() || stop_token.stop_requested()) {
                        call_stop->request_stop();
                        link->interrupt_worker();
                        answer_moot = true;
                    }
                    const auto deadline_nanos = link->deadline_nanos.load();
                    if (deadline_nanos > 0 && std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                      std::chrono::steady_clock::now().time_since_epoch())
                                                              .count() >= deadline_nanos) {
                        call_stop->request_stop();
                        link->interrupt_worker();
                        answer_moot = true;
                    }
                    if (link->script_settled.load()) {
                        call_stop->request_stop();
                        link->interrupt_worker();
                        answer_moot = true;
                    }
                    timer->expires_after(std::chrono::milliseconds{5});
                    const auto [wait_error] =
                            co_await timer->async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                    (void)wait_error;
                    if (stop_token.stop_requested()) {
                        link->interrupt_worker();
                        call_stop->request_stop();
                    }
                }
                // The answer is moot when the script has already settled, or
                // when the host cancelled the run (stop or deadline): pi decides
                // the result first and later VM messages never reach the guest.
                const auto now_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                                               .count();
                const auto run_deadline = link->deadline_nanos.load();
                const bool cancelled_by_host = link->script_settled.load() || stop_token.stop_requested() ||
                                               (run_deadline > 0 && now_nanos > run_deadline);
                if (!answer_moot && !cancelled_by_host) {
                    link->deliver(request.id, (*slot)->ok, std::move((*slot)->payload));
                }
            }
            if (link->done() || link->closed.load() || stop_token.stop_requested()) break;
            const auto run_deadline = link->deadline_nanos.load();
            if (run_deadline > 0 && std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                                    .count() >= run_deadline) {
                link->interrupt_worker();
                call_stop->request_stop();
                break;
            }
            timer->expires_after(std::chrono::milliseconds{50});
            const auto [error] = co_await timer->async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            (void)error;
            if (stop_token.stop_requested()) {
                link->interrupt_worker();
                call_stop->request_stop();
            }
        }

        while (!link->done()) {
            timer->expires_after(std::chrono::milliseconds{5});
            const auto [error] = co_await timer->async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            (void)error;
        }
        monitor_timer->cancel();
        auto result = link->take_result();
        if (result) {
            if (link->closed.load()) {
                result->error = CodemodeError{CodemodeError::Kind::Aborted, "Sandbox closed"};
                result->value_json.reset();
            }
            co_return std::move(*result);
        }
        co_return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "the codemode sandbox worker did not report a result"));
    });
}

} // namespace cch::codemode

namespace cch::codemode {

/// pi `CodemodeRun`: the caller's handle on one deterministic execution.
/// `State` owns the VM driver and the single-consumption completion; `deliver`
/// and `finished` forward to it, and `result` hands out the terminal outcome.
struct CodemodeSandbox::Run::State {
    /// Keeps the AST module alive even if the owning sandbox is destroyed.
    std::shared_ptr<CodemodeSandbox::Impl> owner;
    /// The run's driver, started when `start` returns.
    std::unique_ptr<detail::DeterministicRun> driver;
    /// Guards `taken`: `result()` is a single-consumption operation (§6.2).
    std::mutex mutex;
    bool taken{false};
};

CodemodeSandbox::Run::Run(std::shared_ptr<State> state) : state_(std::move(state)) {}
CodemodeSandbox::Run::Run(Run&&) noexcept = default;
CodemodeSandbox::Run& CodemodeSandbox::Run::operator=(Run&&) noexcept = default;
CodemodeSandbox::Run::~Run() = default;

bool CodemodeSandbox::Run::finished() const { return state_ != nullptr && state_->driver->finished(); }

std::optional<CodemodeDeliveryRejection> CodemodeSandbox::Run::deliver(
        std::int64_t seq, double now, std::int64_t id, CodemodeOutcome outcome) {
    if (state_ == nullptr) {
        return CodemodeDeliveryRejection{
                CodemodeDeliveryRejection::Reason::UnknownCall, std::format("Call {} is not waiting for a result", id)};
    }
    return state_->driver->deliver(seq, now, id, std::move(outcome));
}

support::AsyncResult<CodemodeRunResult> CodemodeSandbox::Run::result() {
    // A moved-from handle has no run to report; pi's `CodemodeRun.result` is a
    // promise that is always present, so a second consumption is a caller bug
    // the single-consumption contract reports rather than a silent hang.
    if (state_ == nullptr) {
        return support::AsyncResult<CodemodeRunResult>{support::Expected<CodemodeRunResult>{std::unexpected(
                support::make_error(support::ErrorCode::Validation, "the codemode run handle was moved from"))}};
    }
    {
        std::lock_guard lock(state_->mutex);
        if (state_->taken) {
            return support::AsyncResult<CodemodeRunResult>{
                    support::Expected<CodemodeRunResult>{std::unexpected(support::make_error(
                            support::ErrorCode::Validation, "the codemode run result was already consumed"))}};
        }
        state_->taken = true;
    }
    // The producer contract is `noexcept` (AsyncResult.hpp): a throwing or
    // duplicate completion is an invariant violation, and the driver delivers
    // exactly once.
    support::AsyncResult<CodemodeRunResult>::producer_type producer =
            [state = state_](support::AsyncCompletion<CodemodeRunResult, support::Error> complete) noexcept {
                state->driver->consume(std::move(complete));
            };
    return support::AsyncResult<CodemodeRunResult>{std::move(producer)};
}

support::Expected<CodemodeSandbox::Run> CodemodeSandbox::start(std::string script, CodemodeRunOptions options) {
    // pi `start`'s validation, in its order, before any VM exists: a rejected
    // option set never creates a worker. The Remote caller shares this
    // validator, so both paths refuse the same sets with the same text.
    if (auto valid = detail::validate_run_options(options); !valid) {
        return std::unexpected(std::move(valid.error()));
    }

    detail::DeterministicStart start;
    start.script = std::move(script);
    start.tools = std::move(options.tools);
    for (auto& tool : start.tools) {
        if (tool.js_name.empty()) tool.js_name = to_codemode_identifier(tool.name);
    }
    start.globals = std::move(options.globals);
    start.store_json = store_json(options.store);
    start.started_at_ms = options.started_at;
    start.seed = static_cast<std::uint32_t>(options.seed);
    start.memory_limit_bytes = options.memory_limit_bytes;
    start.expected_fingerprint = options.expected_fingerprint;
    start.timeout = options.timeout;
    start.stop_token = options.stop_token;
    start.abort_message = std::move(options.abort_message);

    detail::DeterministicHost host;
    // pi `forwardCall`/`onStarted`/`onIdle`: the callbacks run on the run's own
    // worker thread, so a callback must not block. `deliver` only enqueues.
    host.on_started = std::move(options.callbacks.on_started);
    host.on_call = std::move(options.callbacks.on_call);
    host.on_idle = std::move(options.callbacks.on_idle);

    auto state = std::make_shared<Run::State>();
    state->owner = impl_;
    state->driver = std::make_unique<detail::DeterministicRun>(impl_->ast, std::move(start), std::move(host));
    return Run{std::move(state)};
}

} // namespace cch::codemode
