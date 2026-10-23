#include "codemode/Deterministic.hpp"

#include "codemode/Guest.hpp"
#include "codemode/Prelude.hpp"
#include "codemode/Shared.hpp"
#include "support/Json.hpp"

#include <wasmedge/wasmedge.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cch::codemode::detail {

std::uint32_t Mulberry32::draw() {
    // pi `prng` (`runtime/vm.ts`): state advances by a fixed odd increment, then
    // a 32-bit avalanche (mulberry32). Every step is mod 2^32.
    state_ += 0x6d2b79f5u;
    std::uint32_t value = (state_ ^ (state_ >> 15)) * (1u | state_);
    value = (value + ((value ^ (value >> 7)) * (61u | value))) ^ value;
    return value ^ (value >> 14);
}

std::string prelude_fingerprint_hash(std::string_view prelude) {
    // pi `fnv1a`: FNV-1a over UTF-16 code units, as eight lowercase hex digits.
    // The prelude is ASCII, so a byte is a code unit and the digests agree.
    std::uint32_t hash = 0x811c9dc5u;
    for (const char ch : prelude) {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= 0x01000193u;
    }
    return std::format("{:08x}", hash);
}

std::string deterministic_fingerprint(std::string_view quickjs_wasi_version,
        std::string_view quickjs_version,
        std::string_view prelude_hash,
        std::uint32_t max_stack_size,
        std::optional<std::uint64_t> memory_limit_bytes,
        std::optional<std::uint64_t> interrupt_budget) {
    return std::format("quickjs-wasi {}, quickjs {}, prelude {}, deterministic {}, stack {}, heap {}, budget {}",
            quickjs_wasi_version,
            quickjs_version,
            prelude_hash,
            kDeterministicVersion,
            max_stack_size,
            memory_limit_bytes.has_value() ? std::to_string(*memory_limit_bytes) : std::string{"none"},
            interrupt_budget.has_value() ? std::to_string(*interrupt_budget) : std::string{"none"});
}

namespace {

using detail::gen_i32;
using detail::Guest;
using detail::Handle;

/// The `quickjs-wasi` package version the guest bytes come from
/// (`fixtures/codemode/quickjs/README.md`: `quickjs-wasi@3.6.2`). The engine
/// version is read from the guest itself, so the fingerprint names the bytes
/// actually loaded.
constexpr std::string_view kQuickJsWasiVersion = "3.6.2";

/// pi `HostToWorkerMessage`'s `result`: one answer the host admitted, waiting
/// for the VM to reach its number.
struct QueuedResult {
    std::int64_t seq{0};
    std::int64_t id{0};
    bool ok{true};
    std::string payload;
    std::optional<std::string> properties;
    double now{0.0};
};

/// The state the caller's thread and the worker thread share.
struct Shared {
    /// Mutable so the read-only `finished()` query can take it.
    mutable std::mutex mutex;
    std::condition_variable cv;
    /// Answers the caller admitted, in arrival order.
    std::deque<QueuedResult> queued;
    /// pi `Execution.delivered`: result numbers already used.
    std::set<std::int64_t> delivered;
    /// Call ids the VM reported and the caller has not answered.
    std::set<std::int64_t> waiting;
    /// Set when the run has its outcome; later messages are ignored.
    bool finished{false};
    /// The final result has been published by the worker and may be consumed.
    bool outcome_ready{false};
    /// Set when the worker must stop at its next interrupt poll.
    bool interrupted{false};
    /// Wakes the parked delivery loop when the caller's stop token fires.
    std::optional<std::stop_callback<std::function<void()>>> stop_notify;
};

/// pi `deterministicWasi.random_get`: four bytes per draw, little-endian — the
/// same sequence pi's `prng` produces, so `Math.random()` (which QuickJS seeds
/// from the first clock read) and any guest PRNG agree with the frozen capture.
WasmEdge_Result RandomGet(
        void* data, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out) {
    auto* random = static_cast<Mulberry32*>(data);
    Guest guest{nullptr,
            const_cast<WasmEdge_ModuleInstanceContext*>(WasmEdge_CallingFrameGetModuleInstance(frame)),
            WasmEdge_CallingFrameGetMemoryInstance(frame, 0)};
    const std::uint32_t buf = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[0]));
    const std::uint32_t len = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[1]));
    if (std::uint8_t* p = WasmEdge_MemoryInstanceGetPointer(guest.memory, buf, len)) {
        for (std::uint32_t index = 0; index < len; index += 4) {
            const std::uint32_t value = random->draw();
            for (std::uint32_t byte = 0; byte < 4 && index + byte < len; ++byte)
                p[index + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
        }
    }
    out[0] = gen_i32(kWasiErnoSuccess);
    return WasmEdge_Result_Success;
}

/// pi `deterministicWasi.clock_time_get`: virtual time. While the context is
/// created every read is seed-derived, because QuickJS seeds `Math.random()`
/// from the first clock read and which read comes first is the engine's
/// business; afterwards the realtime clock reads the result's `now` and the
/// monotonic clock reads the elapsed time since `started_at`.
WasmEdge_Result ClockTimeGet(
        void* data, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out) {
    auto* clock = static_cast<VirtualClock*>(data);
    const std::int32_t clock_id = WasmEdge_ValueGetI32(in[0]);
    if (clock_id != 0 && clock_id != 1) {
        out[0] = gen_i32(kWasiErnoNosys);
        return WasmEdge_Result_Success;
    }
    Guest guest{nullptr,
            const_cast<WasmEdge_ModuleInstanceContext*>(WasmEdge_CallingFrameGetModuleInstance(frame)),
            WasmEdge_CallingFrameGetMemoryInstance(frame, 0)};
    const std::uint32_t result_ptr = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[2]));
    std::uint64_t nanos = 0;
    if (clock->creating) {
        // QuickJS seeds `Math.random()` from this reading in microseconds and
        // turns 0 into 1, so seed s reads s + 1.
        nanos = clock_id == 0 ? (static_cast<std::uint64_t>(clock->seed) + 1) * 1000ull : 0ull;
    } else if (clock_id == 0) {
        nanos = static_cast<std::uint64_t>(std::llround(clock->now)) * 1000000ull;
    } else {
        nanos = static_cast<std::uint64_t>(std::llround(clock->now - clock->started_at)) * 1000000ull;
    }
    guest.store_u64(result_ptr, nanos);
    out[0] = gen_i32(kWasiErnoSuccess);
    return WasmEdge_Result_Success;
}

/// The worker's interrupt poll. pi's worker launcher counts no polls, so the
/// only stops are the run's deadline, the caller's cancellation, and the host's
/// request to end an already-settled run.
struct InterruptPoll {
    const std::chrono::steady_clock::time_point* deadline{nullptr};
    Shared* shared{nullptr};
    std::stop_token stop_token;
};

WasmEdge_Result HostInterrupt(
        void* data, const WasmEdge_CallingFrameContext*, const WasmEdge_Value*, WasmEdge_Value* out) {
    auto* poll = static_cast<InterruptPoll*>(data);
    bool interrupt = poll->stop_token.stop_requested();
    if (!interrupt && poll->deadline != nullptr) interrupt = std::chrono::steady_clock::now() >= *poll->deadline;
    if (!interrupt) {
        std::lock_guard lock(poll->shared->mutex);
        interrupt = poll->shared->interrupted;
    }
    out[0] = gen_i32(interrupt ? 1 : 0);
    return WasmEdge_Result_Success;
}

/// pi `handleMessage`'s `output` case for a deterministic run: the item keeps
/// its channel and its console marker.
void append_output(CodemodeRunResult& result, const Guest& guest, const std::vector<Handle>& args) {
    CodemodeOutputItem item;
    const std::string channel = args.size() > 1 ? guest.read_string(args[1]) : std::string{};
    item.kind = channel == "image" ? CodemodeOutputItem::Kind::Image : CodemodeOutputItem::Kind::Text;
    item.data = args.size() > 2 ? guest.read_string(args[2]) : std::string{};
    item.mime_type = args.size() > 3 ? guest.read_string(args[3]) : std::string{};
    item.console = channel == "console";
    result.output.push_back(std::move(item));
}

/// pi `parseScriptError`: the prelude's `describeError` payload as the frozen
/// script error shape (`message` always, `name`/`stack` for an Error).
[[nodiscard]] CodemodeError script_error_from(std::string_view payload) {
    auto parsed = support::read_json(payload);
    if (parsed && parsed->holds<support::JsonValue::object_t>()) {
        const auto& object = parsed->get_object();
        CodemodeError error{CodemodeError::Kind::Script, std::string{payload}};
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
    return CodemodeError{CodemodeError::Kind::Script, std::string{payload}};
}

/// One guest instance's whole allocation set, released in one place.
struct GuestRun {
    WasmEdge_ConfigureContext* configure{nullptr};
    WasmEdge_StoreContext* store{nullptr};
    WasmEdge_ExecutorContext* executor{nullptr};
    WasmEdge_ModuleInstanceContext* env{nullptr};
    WasmEdge_ModuleInstanceContext* wasi{nullptr};
    WasmEdge_ModuleInstanceContext* module{nullptr};
    Guest guest;

    GuestRun() = default;
    GuestRun(const GuestRun&) = delete;
    GuestRun& operator=(const GuestRun&) = delete;
    ~GuestRun() {
        if (module != nullptr) WasmEdge_ModuleInstanceDelete(module);
        if (env != nullptr) WasmEdge_ModuleInstanceDelete(env);
        if (wasi != nullptr) WasmEdge_ModuleInstanceDelete(wasi);
        if (executor != nullptr) WasmEdge_ExecutorDelete(executor);
        if (store != nullptr) WasmEdge_StoreDelete(store);
        if (configure != nullptr) WasmEdge_ConfigureDelete(configure);
    }
};

} // namespace

/// The deterministic `host_call` import: unpack the guest's argument vector and
/// hand it to the run, which forwards the call to the caller's `on_call`. The
/// caller must not re-enter the VM from the callback (pi's `post` contract):
/// `deliver` only enqueues, so it is safe.
WasmEdge_Result HostCall(
        void* data, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out);

/// The deterministic run's worker: pi's `startVm` plus the delivery loop of
/// `Execution`.
struct DeterministicRun::Impl {
    Impl(WasmEdge_ASTModuleContext* ast, DeterministicStart start, DeterministicHost host);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool finished() const;
    void consume(support::AsyncCompletion<CodemodeRunResult, support::Error> completion);
    [[nodiscard]] std::optional<CodemodeDeliveryRejection> deliver(
            std::int64_t seq, double now, std::int64_t id, CodemodeOutcome outcome);

    /// The worker thread's whole life: build the VM, run the script, then apply
    /// admitted answers in `seq` order until the run has its outcome.
    void drive();

    /// pi `handleMessage`'s `done` case.
    void handle_done(const Guest& guest, const std::vector<Handle>& args);
    /// pi's prelude bridge (`bridge(kind, a, b, c)`), called from inside the VM.
    void handle_bridge(const Guest& guest, const std::vector<Handle>& args);

    /// pi `issued`: calls issued so far, tools and globals alike. Only the
    /// worker thread touches it, and only the worker reports it.
    WasmEdge_ASTModuleContext* ast;
    DeterministicStart start;
    DeterministicHost host;
    Shared shared;
    VirtualClock clock;
    Mulberry32 random;
    std::int64_t issued{0};
    /// pi `current`: the result the VM is processing (0 during the script's
    /// first run), reported with `done` as `during.seq`.
    std::int64_t current_seq{0};
    /// pi `nextSeq`: the next result number the VM applies.
    std::int64_t next_seq{1};
    /// pi `pending`: the tool call id to its record index, so `deliver` can
    /// settle the record it created.
    std::map<std::int64_t, std::size_t> records;
    std::optional<CodemodeRunResult> terminal;
    std::optional<support::AsyncCompletion<CodemodeRunResult, support::Error>> completion;
    std::thread worker;
};

DeterministicRun::DeterministicRun(WasmEdge_ASTModuleContext* ast, DeterministicStart start, DeterministicHost host)
    : impl_(std::make_unique<Impl>(ast, std::move(start), std::move(host))) {}

DeterministicRun::~DeterministicRun() = default;

bool DeterministicRun::finished() const { return impl_->finished(); }

void DeterministicRun::consume(support::AsyncCompletion<CodemodeRunResult, support::Error> completion) {
    impl_->consume(std::move(completion));
}

std::optional<CodemodeDeliveryRejection> DeterministicRun::deliver(
        std::int64_t seq, double now, std::int64_t id, CodemodeOutcome outcome) {
    return impl_->deliver(seq, now, id, std::move(outcome));
}

DeterministicRun::Impl::Impl(
        WasmEdge_ASTModuleContext* module_ast, DeterministicStart options, DeterministicHost callbacks)
    : ast(module_ast), start(std::move(options)), host(std::move(callbacks)),
      clock(VirtualClock{.creating = true,
              .started_at = static_cast<double>(start.started_at_ms),
              .now = static_cast<double>(start.started_at_ms),
              .seed = start.seed}),
      random(Mulberry32{static_cast<std::uint32_t>(start.seed) ^ 0x9e3779b9u}) {
    // A stop request must wake the delivery loop while it waits for the next
    // result number: the worker parks without a deadline and cannot poll the
    // token inside `condition_variable::wait`'s predicate without a notify.
    shared.stop_notify.emplace(start.stop_token, [&]() { shared.cv.notify_all(); });
    worker = std::thread([this]() { drive(); });
}

DeterministicRun::Impl::~Impl() {
    {
        std::lock_guard lock(shared.mutex);
        shared.interrupted = true;
    }
    shared.cv.notify_all();
    if (worker.joinable()) worker.join();
}

bool DeterministicRun::Impl::finished() const {
    std::lock_guard lock(shared.mutex);
    return shared.finished;
}

void DeterministicRun::Impl::consume(support::AsyncCompletion<CodemodeRunResult, support::Error> complete) {
    std::optional<CodemodeRunResult> ready;
    {
        std::lock_guard lock(shared.mutex);
        // The worker publishes the outcome while holding the same mutex. If it
        // won the race, transfer it here; otherwise the worker sees completion.
        if (shared.outcome_ready) {
            ready = std::move(terminal);
        } else {
            completion = std::move(complete);
        }
    }
    if (ready.has_value()) {
        std::move(complete)(support::Expected<CodemodeRunResult>{std::move(*ready)});
    }
}

std::optional<CodemodeDeliveryRejection> DeterministicRun::Impl::deliver(
        std::int64_t seq, double now, std::int64_t id, CodemodeOutcome outcome) {
    using Reason = CodemodeDeliveryRejection::Reason;
    // pi `Execution.deliver` validates on the caller's side before posting
    // anything, so a refused answer leaves the call waiting and the run
    // unsettled; the caller can retry with a valid one. Keep its validation
    // order because a message can violate more than one condition.
    if (!std::isfinite(now)) {
        std::lock_guard lock(shared.mutex);
        if (shared.finished) return std::nullopt;
        return CodemodeDeliveryRejection{
                Reason::Clock, std::format("The clock value of result {} is not a finite number", seq)};
    }

    std::lock_guard lock(shared.mutex);
    if (shared.finished) return std::nullopt; // a finished run ignores later answers
    if (seq < 1 || shared.delivered.contains(seq)) {
        return CodemodeDeliveryRejection{
                Reason::ResultNumber, std::format("Result number {} is not a positive integer used once", seq)};
    }
    if (!shared.waiting.contains(id)) {
        return CodemodeDeliveryRejection{Reason::UnknownCall, std::format("Call {} is not waiting for a result", id)};
    }
    if (!outcome.ok && outcome.properties_json.has_value()) {
        const auto properties = support::read_json(*outcome.properties_json);
        if (!properties.has_value() || !properties->holds<support::JsonValue::object_t>()) {
            return CodemodeDeliveryRejection{
                    Reason::Properties, std::format("The properties of result {} are not an object", seq)};
        }
    }

    if (outcome.ok && !outcome.value_json.empty()) {
        // pi serializes the value with `JSON.stringify` before recording
        // anything: a value JSON cannot hold throws and leaves the call
        // waiting for another answer. This API receives the text already, so
        // the same refusal is text that is not a JSON value.
        if (!support::read_json(outcome.value_json).has_value()) {
            return CodemodeDeliveryRejection{
                    Reason::Serialization, std::format("The value of result {} is not JSON", seq)};
        }
    }

    shared.delivered.insert(seq);
    shared.waiting.erase(id);
    shared.queued.push_back(QueuedResult{
            .seq = seq,
            .id = id,
            .ok = outcome.ok,
            .payload = outcome.ok ? outcome.value_json : outcome.message,
            .properties = outcome.properties_json,
            .now = now,
    });
    shared.cv.notify_all();
    return std::nullopt;
}

void DeterministicRun::Impl::handle_bridge(const Guest& guest, const std::vector<Handle>& args) {
    if (args.empty()) return;
    const std::string kind = guest.read_string(args[0]);
    if (kind == "output") {
        // Only the VM thread writes its output vector; no protocol mutex is
        // needed, and the guest string conversion may re-enter the bridge.
        append_output(*terminal, guest, args);
        return;
    }
    if (kind == "call" || kind == "global") {
        issued++;
        CodemodeHostCall call;
        call.target = kind == "global" ? CodemodeHostCall::Target::Global : CodemodeHostCall::Target::Tool;
        const std::string id_text = args.size() > 1 ? guest.read_string(args[1]) : std::string{};
        if (std::int64_t parsed = 0;
                std::from_chars(id_text.data(), id_text.data() + id_text.size(), parsed).ec == std::errc{}) {
            call.id = parsed;
        }
        call.name = args.size() > 2 ? guest.read_string(args[2]) : std::string{};
        if (args.size() > 3 && !guest.is_undefined(args[3])) call.arguments_json = guest.read_string(args[3]);
        // pi counts a global as an issued call too, and numbers both from one
        // counter (`issued++` in the VM bridge). The record keeps tools only,
        // exactly as pi's `calls` does.
        std::size_t record = 0;
        {
            std::lock_guard lock(shared.mutex);
            shared.waiting.insert(call.id);
            if (call.target == CodemodeHostCall::Target::Tool) {
                // pi `forwardCall`: the record starts as `cancelled` with a
                // duration, and `deliver` overwrites both.
                terminal->calls.push_back(CodemodeCallRecord{
                        .name = call.name, .status = CodemodeCallRecord::Status::Cancelled, .duration_ms = 0.0});
                record = terminal->calls.size() - 1;
                records.emplace(call.id, record);
            }
        }
        if (host.on_call) host.on_call(call);
        return;
    }
    if (kind == "done") handle_done(guest, args);
}

void DeterministicRun::Impl::handle_done(const Guest& guest, const std::vector<Handle>& args) {
    // `done(ok, payload, writes)`: pi decodes everything before finishing, and
    // `during` reports the result the VM was processing plus the calls issued.
    const bool ok = args.size() > 1 && !guest.is_undefined(args[1]) && guest.read_string(args[1]) == "true";
    if (ok) {
        if (args.size() > 2 && !guest.is_undefined(args[2])) terminal->value_json = guest.read_string(args[2]);
        // pi reports the write set only on success (including exit()).
        terminal->store_writes = CodemodeStoreWrites{};
        if (args.size() > 3 && !guest.is_undefined(args[3])) {
            terminal->store_writes = parse_store_writes(guest.read_string(args[3]));
        }
    } else {
        terminal->error = args.size() > 2 ? script_error_from(guest.read_string(args[2]))
                                          : CodemodeError{CodemodeError::Kind::Script, {}};
    }
    terminal->during = CodemodeDuring{.seq = current_seq, .issued = issued};
    {
        std::lock_guard lock(shared.mutex);
        shared.finished = true;
        shared.interrupted = true;
    }
    shared.cv.notify_all();
}

void DeterministicRun::Impl::drive() {
    terminal.emplace();
    auto finish = [this]() {
        std::optional<support::AsyncCompletion<CodemodeRunResult, support::Error>> complete;
        std::optional<CodemodeRunResult> ready;
        {
            std::lock_guard lock(shared.mutex);
            shared.finished = true;
            shared.interrupted = true;
            complete = std::move(completion);
            completion.reset();
            shared.outcome_ready = true;
            if (complete.has_value()) ready = std::move(terminal);
        }
        shared.cv.notify_all();
        if (complete.has_value() && ready.has_value()) {
            std::move (*complete)(support::Expected<CodemodeRunResult>{std::move(*ready)});
        }
    };

    if (start.stop_token.stop_requested()) {
        // pi settles a pre-aborted deterministic run without creating a VM, with
        // the AbortSignal reason's message (the sandbox's abort text otherwise).
        terminal->error = CodemodeError{
                CodemodeError::Kind::Aborted, start.abort_message.value_or(std::string{"Execution aborted"})};
        finish();
        return;
    }

    const auto deadline =
            start.timeout.has_value()
                    ? std::optional<std::chrono::steady_clock::time_point>{std::chrono::steady_clock::now() +
                                                                           *start.timeout}
                    : std::nullopt;
    InterruptPoll poll{
            .deadline = deadline.has_value() ? &*deadline : nullptr, .shared = &shared, .stop_token = start.stop_token};

    GuestRun run;
    run.configure = WasmEdge_ConfigureCreate();
    if (start.memory_limit_bytes.has_value()) {
        // The guest's linear memory cap in 64 KiB pages, exactly as worker mode
        // enforces `memoryLimitBytes`; the fingerprint reports the same number.
        WasmEdge_ConfigureSetMaxMemoryPage(
                run.configure, static_cast<std::uint32_t>((*start.memory_limit_bytes + 65535) / 65536));
    }
    run.store = WasmEdge_StoreCreate();
    run.executor = WasmEdge_ExecutorCreate(run.configure, nullptr);
    run.env = make_module("env");
    add_function(run.env,
            "host_call",
            make_function(HostCall,
                    this,
                    {WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32},
                    {WasmEdge_ValType_I32}));
    add_function(run.env, "host_interrupt", make_function(HostInterrupt, &poll, {}, {WasmEdge_ValType_I32}));
    add_function(run.env,
            "host_promise_rejection",
            make_function(HostPromiseRejection,
                    nullptr,
                    {WasmEdge_ValType_I32, WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                    {}));
    add_function(run.env,
            "host_get_timezone_offset",
            make_function(HostTimezone, nullptr, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));
    add_function(run.env,
            "host_module_normalize",
            make_function(
                    HostModuleDenied, nullptr, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));
    add_function(run.env,
            "host_module_load",
            make_function(
                    HostModuleDenied, nullptr, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));

    run.wasi = make_module("wasi_snapshot_preview1");
    add_function(run.wasi,
            "fd_write",
            make_function(HostFdWrite,
                    nullptr,
                    {WasmEdge_ValType_I32, WasmEdge_ValType_I32, WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                    {WasmEdge_ValType_I32}));
    add_function(
            run.wasi, "fd_close", make_function(HostWasiStub, nullptr, {WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));
    add_function(run.wasi,
            "fd_fdstat_get",
            make_function(
                    HostFdFdstatGet, nullptr, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));
    add_function(run.wasi,
            "fd_seek",
            make_function(HostWasiStub,
                    nullptr,
                    {WasmEdge_ValType_I32, WasmEdge_ValType_I64, WasmEdge_ValType_I32, WasmEdge_ValType_I32},
                    {WasmEdge_ValType_I32}));
    add_function(run.wasi,
            "clock_time_get",
            make_function(ClockTimeGet,
                    &clock,
                    {WasmEdge_ValType_I32, WasmEdge_ValType_I64, WasmEdge_ValType_I32},
                    {WasmEdge_ValType_I32}));
    add_function(run.wasi,
            "random_get",
            make_function(RandomGet, &random, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));

    const bool registered = WasmEdge_ResultOK(WasmEdge_ExecutorRegisterImport(run.executor, run.store, run.env)) &&
                            WasmEdge_ResultOK(WasmEdge_ExecutorRegisterImport(run.executor, run.store, run.wasi));
    const WasmEdge_Result instantiated =
            registered ? WasmEdge_ExecutorInstantiate(run.executor, &run.module, run.store, ast) : WasmEdge_Result_Fail;
    if (!registered || !WasmEdge_ResultOK(instantiated)) {
        terminal->error = CodemodeError{CodemodeError::Kind::Sandbox,
                std::format("The codemode sandbox denied the script: the guest could not be instantiated ({}). A "
                            "required import was not provided.",
                        registered ? WasmEdge_ResultGetMessage(instantiated) : "import registration failed")};
        finish();
        return;
    }

    WasmEdge_String memory_name = WasmEdge_StringCreateByCString("memory");
    run.guest.executor = run.executor;
    run.guest.module = run.module;
    run.guest.memory = WasmEdge_ModuleInstanceFindMemory(run.module, memory_name);
    WasmEdge_StringDelete(memory_name);
    if (run.guest.memory == nullptr) {
        terminal->error = CodemodeError{CodemodeError::Kind::Sandbox,
                "The codemode sandbox denied the script: the guest exports no linear memory."};
        finish();
        return;
    }

    const Guest& g = run.guest;
    g.call_void(g.find("_initialize"), {});
    // pi `startVm`: a deterministic VM installs every intrinsic except
    // `WeakRef`/`FinalizationRegistry` and pins the stack guard.
    if (g.call_i32(g.find("qjs_init2"), {gen_i32(static_cast<std::int32_t>(kDeterministicIntrinsics))}) != 0) {
        terminal->error = CodemodeError{
                CodemodeError::Kind::Sandbox, "The codemode sandbox guest failed to initialize (qjs_init2)."};
        finish();
        return;
    }
    g.call_void(g.find("qjs_set_max_stack_size"), {gen_i32(static_cast<std::int32_t>(kMaxStackSize))});

    const std::uint32_t code_ptr = g.write_cstr(kCodemodePreludeSource);
    const std::uint32_t name_ptr = g.write_cstr("codemode-prelude.js");
    const Handle prelude = g.call_i32(g.find("qjs_eval"),
            {gen_i32(static_cast<std::int32_t>(code_ptr)),
                    gen_i32(static_cast<std::int32_t>(std::strlen(kCodemodePreludeSource))),
                    gen_i32(static_cast<std::int32_t>(name_ptr)),
                    gen_i32(0)});
    if (g.is_exception(prelude)) {
        terminal->error = CodemodeError{CodemodeError::Kind::Sandbox,
                std::format("The codemode sandbox prelude failed: {}", g.exception_text())};
        finish();
        return;
    }
    clock.creating = false;

    const std::uint32_t bridge_name_ptr = g.write_cstr(kBridgeName);
    const Handle bridge = g.call_i32(g.find("qjs_new_host_function"),
            {gen_i32(static_cast<std::int32_t>(bridge_name_ptr)),
                    gen_i32(static_cast<std::int32_t>(kBridgeName.size())),
                    gen_i32(0)});
    const Handle api = g.call(prelude,
            g.undefined(),
            {bridge,
                    g.new_string(tools_json(start.tools)),
                    g.new_string(globals_json(start.globals)),
                    g.new_string(start.store_json)});
    const Handle run_fn = g.get_prop(api, "run");
    const Handle settle_fn = g.get_prop(api, "settle");
    const Handle stalled_fn = g.get_prop(api, "stalled");

    // pi `startVm` reports `started` with the fingerprint before the script runs
    // and before any of its calls.
    const std::string fingerprint = deterministic_fingerprint(kQuickJsWasiVersion,
            g.read_cstr(static_cast<std::uint32_t>(g.call_i32(g.find("qjs_get_quickjs_version"), {}))),
            prelude_fingerprint_hash(kCodemodePreludeSource),
            kMaxStackSize,
            start.memory_limit_bytes,
            std::nullopt);
    // The actual fingerprint is observable even for a replay mismatch, just
    // like pi's `started` message; the mismatch check still precedes script
    // evaluation and every host call.
    if (host.on_started) host.on_started(fingerprint);
    if (start.expected_fingerprint.has_value() && fingerprint != *start.expected_fingerprint) {
        terminal->error = CodemodeError{CodemodeError::Kind::Sandbox,
                std::format("Deterministic replay fingerprint mismatch: expected '{}', got '{}'",
                        *start.expected_fingerprint,
                        fingerprint)};
        terminal->during = CodemodeDuring{.seq = 0, .issued = 0};
        finish();
        return;
    }

    // The script runs inside an async wrapper so `return` and top-level `await`
    // work, with the first line shared so reported line numbers match the source.
    const std::string wrapped = "(async (tools, console) => {" + start.script + "\n})";
    const std::uint32_t script_ptr = g.write_cstr(wrapped);
    const std::uint32_t script_name_ptr = g.write_cstr("codemode.js");
    const Handle script_fn = g.call_i32(g.find("qjs_eval"),
            {gen_i32(static_cast<std::int32_t>(script_ptr)),
                    gen_i32(static_cast<std::int32_t>(wrapped.size())),
                    gen_i32(static_cast<std::int32_t>(script_name_ptr)),
                    gen_i32(0)});
    if (g.is_exception(script_fn)) {
        // pi's eval failure reports a `done` without `during`: the script did
        // not start running inside the deterministic VM.
        terminal->error = g.exception_error();
        finish();
        return;
    }

    auto interrupted = [&] {
        if (poll.stop_token.stop_requested()) return true;
        if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) return true;
        std::lock_guard lock(shared.mutex);
        return shared.interrupted;
    };

    auto interruption_error = [&] {
        if (poll.stop_token.stop_requested()) {
            terminal->error = CodemodeError{
                    CodemodeError::Kind::Aborted, start.abort_message.value_or(std::string{"Execution aborted"})};
        } else if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
            terminal->error = CodemodeError{CodemodeError::Kind::Timeout,
                    std::format("Execution timed out after {} ms", start.timeout->count())};
        } else {
            terminal->error = CodemodeError{CodemodeError::Kind::Aborted, "Execution aborted"};
        }
    };
    auto drain_once = [&] {
        if (drain_jobs(g, [&] { return finished() || interrupted(); })) return true;
        if (finished()) return true;
        if (interrupted())
            interruption_error();
        else
            terminal->error = CodemodeError{CodemodeError::Kind::Sandbox, "Promise job failed"};
        return false;
    };
    auto drain = [&]() {
        if (!drain_once() || finished()) return finished();
        // pi calls `stalled()` at the end of every drain; a pending host call
        // keeps it false, while an otherwise unresolved promise settles as the
        // frozen sandbox error.
        (void)g.call(stalled_fn, api, {});
        return drain_once();
    };

    g.call_void(g.find("qjs_set_interrupt_handler"), {gen_i32(1)});
    (void)g.call(run_fn, api, {script_fn});
    if (!drain() && !finished()) {
        finish();
        return;
    }

    // pi's `idle` after the script's first run, with the calls it issued. A
    // script that already finished reports none: the frozen idles start after
    // the first delivered result.
    if (!finished() && host.on_idle) host.on_idle(0, issued);

    while (true) {
        if (poll.stop_token.stop_requested()) {
            // pi's abort is an error the script cannot catch: the VM is
            // interrupted and the pending calls keep their `cancelled` record.
            terminal->error = CodemodeError{
                    CodemodeError::Kind::Aborted, start.abort_message.value_or(std::string{"Execution aborted"})};
            finish();
            return;
        }
        if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
            terminal->error = CodemodeError{CodemodeError::Kind::Timeout,
                    std::format("Execution timed out after {} ms", start.timeout->count())};
            finish();
            return;
        }
        if (finished()) {
            finish();
            return;
        }
        bool interrupted_by_owner = false;
        {
            std::lock_guard lock(shared.mutex);
            interrupted_by_owner = shared.interrupted;
        }
        if (interrupted_by_owner) {
            terminal->error = CodemodeError{CodemodeError::Kind::Aborted, "Execution aborted"};
            finish();
            return;
        }
        std::optional<QueuedResult> next;
        {
            std::unique_lock lock(shared.mutex);
            auto ready = [&] {
                if (shared.finished || shared.interrupted || start.stop_token.stop_requested()) return true;
                for (const auto& queued : shared.queued) {
                    if (queued.seq == next_seq) return true;
                }
                return false;
            };
            if (deadline.has_value())
                shared.cv.wait_until(lock, *deadline, ready);
            else
                shared.cv.wait(lock, ready);
            for (auto it = shared.queued.begin(); it != shared.queued.end(); ++it) {
                if (it->seq != next_seq) continue;
                next = std::move(*it);
                shared.queued.erase(it);
                break;
            }
        }
        if (!next.has_value()) continue;
        // pi `vm.deliver`: the clock never goes backwards (a resumed host's may
        // be behind the one that started the run), then the answer is applied.
        clock.now = std::max(clock.now, next->now);
        current_seq = next->seq;
        if (const auto found = records.find(next->id); found != records.end()) {
            CodemodeCallRecord& record = terminal->calls[found->second];
            record.status = next->ok ? CodemodeCallRecord::Status::Ok : CodemodeCallRecord::Status::Error;
            // pi `durationMs` is wall-clock and normalized out of replay
            // equality; a deterministic run reports the presence only.
            record.duration_ms = 0.0;
            records.erase(found);
        }
        const Handle id_handle =
                g.call_i32(g.find("qjs_new_number"), {WasmEdge_ValueGenF64(static_cast<double>(next->id))});
        const Handle payload = next->ok && next->payload.empty() ? g.undefined() : g.new_string(next->payload);
        const Handle properties = next->properties.has_value() ? g.new_string(*next->properties) : g.undefined();
        (void)g.call(settle_fn, api, {id_handle, next->ok ? g.true_value() : g.false_value(), payload, properties});
        if (!drain() && !finished()) {
            finish();
            return;
        }
        // pi posts this result's idle before advancing; a run that finished in
        // this drain reports none, exactly as the frozen idles show.
        if (!finished() && host.on_idle) host.on_idle(next_seq, issued);
        ++next_seq;
    }
}

WasmEdge_Result HostCall(
        void* data, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out) {
    auto* impl = static_cast<DeterministicRun::Impl*>(data);
    Guest guest{WasmEdge_CallingFrameGetExecutor(frame),
            const_cast<WasmEdge_ModuleInstanceContext*>(WasmEdge_CallingFrameGetModuleInstance(frame)),
            WasmEdge_CallingFrameGetMemoryInstance(frame, 0)};
    const std::uint32_t argc = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[3]));
    const std::uint32_t argv = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[4]));
    std::vector<Handle> args;
    args.reserve(argc);
    for (std::uint32_t i = 0; i < argc; ++i)
        args.push_back(static_cast<Handle>(guest.load_u32(argv + i * 4)));
    impl->handle_bridge(guest, args);
    out[0] = gen_i32(guest.undefined());
    return WasmEdge_Result_Success;
}

} // namespace cch::codemode::detail
