#include <cch/codemode/Remote.hpp>

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
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cch::codemode {

namespace {

using detail::add_function;
using detail::drain_jobs;
using detail::gen_i32;
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
using detail::make_function;
using detail::make_module;
using detail::parse_store_writes;
using detail::store_json;
using detail::tools_json;

[[nodiscard]] CodemodeError parse_error_json(std::string_view raw) {
    auto parsed = support::read_json(raw);
    if (parsed && parsed->holds<support::JsonValue::object_t>()) {
        const auto& obj = parsed->get_object();
        CodemodeError err{.kind = CodemodeError::Kind::Script, .message = std::string{raw}};
        bool described = false;
        if (const auto it = obj.find("message"); it != obj.end()) {
            if (const auto* msg = it->second.get_if<std::string>()) {
                err.message = *msg;
                described = true;
            }
        }
        if (const auto it = obj.find("name"); it != obj.end()) {
            if (const auto* name = it->second.get_if<std::string>()) err.name = *name;
        }
        if (const auto it = obj.find("stack"); it != obj.end()) {
            if (const auto* stack = it->second.get_if<std::string>()) err.stack = *stack;
        }
        if (described) return err;
    }
    return CodemodeError{.kind = CodemodeError::Kind::Script, .message = std::string{raw}};
}

/// Owns remote execution threads after the caller has stopped awaiting them.
/// Completed workers are joined without delaying caller settlement.
class RemoteWorkerReaper {
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };

public:
    RemoteWorkerReaper() : reaper_([this](std::stop_token stop) { reap(stop); }) {}

    ~RemoteWorkerReaper() {
        reaper_.request_stop();
        condition_.notify_all();
        if (reaper_.joinable()) reaper_.join();
    }

    RemoteWorkerReaper(const RemoteWorkerReaper&) = delete;
    RemoteWorkerReaper& operator=(const RemoteWorkerReaper&) = delete;

    static RemoteWorkerReaper& instance() {
        static RemoteWorkerReaper reaper;
        return reaper;
    }

    void adopt(std::thread worker, std::shared_ptr<std::atomic<bool>> finished) {
        {
            std::lock_guard lock(mutex_);
            workers_.push_back(Worker{.thread = std::move(worker), .finished = std::move(finished)});
        }
        condition_.notify_one();
    }

    void notify() noexcept { condition_.notify_one(); }

private:
    void reap(std::stop_token stop) {
        while (true) {
            std::vector<std::thread> completed;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this, stop] {
                    return stop.stop_requested() ||
                           std::any_of(workers_.begin(), workers_.end(), [](const Worker& worker) {
                               return worker.finished->load(std::memory_order_acquire);
                           });
                });
                for (auto it = workers_.begin(); it != workers_.end();) {
                    if (stop.stop_requested() || it->finished->load(std::memory_order_acquire)) {
                        completed.push_back(std::move(it->thread));
                        it = workers_.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (stop.stop_requested() && workers_.empty() && completed.empty()) return;
            }
            for (auto& worker : completed) {
                if (worker.joinable()) worker.join();
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Worker> workers_;
    std::jthread reaper_;
};

/// Link coordinating the remote WasmEdge worker thread and the host coroutine in serve_codemode_remote.
struct RemoteServeLink {
    std::mutex mutex;
    std::condition_variable cv_worker;

    std::vector<CodemodeRemoteMessageVariant> outgoing_batch;
    bool has_outgoing{false};
    bool batch_in_flight{false};
    bool wait_for_reply{false};

    std::optional<CodemodeRemoteReplyVariant> incoming_reply;
    bool has_reply{false};

    std::atomic<bool> stop_requested{false};
    std::atomic<bool> worker_done{false};

    CodemodeRemoteReplyVariant send_batch(std::vector<CodemodeRemoteMessageVariant> batch, bool need_reply) {
        std::unique_lock lock(mutex);
        cv_worker.wait(lock, [&] { return !batch_in_flight || stop_requested.load(); });
        if (stop_requested.load()) return CodemodeRemoteStopReply{};

        outgoing_batch = std::move(batch);
        has_outgoing = true;
        batch_in_flight = true;
        wait_for_reply = need_reply;
        has_reply = false;

        if (need_reply) {
            cv_worker.wait(lock, [&] { return has_reply || stop_requested.load(); });
            if (stop_requested.load()) return CodemodeRemoteStopReply{};
            return std::move(*incoming_reply);
        }

        cv_worker.wait(lock, [&] { return !batch_in_flight || stop_requested.load(); });
        return stop_requested.load() ? CodemodeRemoteReplyVariant{CodemodeRemoteStopReply{}}
                                     : CodemodeRemoteReplyVariant{CodemodeRemoteNullReply{}};
    }

    void mark_worker_done() {
        {
            std::lock_guard lock(mutex);
            worker_done.store(true);
        }
        cv_worker.notify_all();
    }
};

struct RemoteVmState {
    Guest guest;
    RemoteServeLink* link{nullptr};
    std::uint64_t budget{0};
    std::uint64_t polls{0};
    bool budget_exhausted{false};
    bool stopped{false};
    bool finished{false};
    std::vector<CodemodeRemoteMessageVariant> buffered;
    std::size_t buffered_chars{0};
    Handle api{0};
    Handle settle_fn{0};

    [[nodiscard]] bool should_interrupt() {
        if (budget > 0) {
            ++polls;
            if (polls > budget) {
                budget_exhausted = true;
                return true;
            }
        }
        if (link != nullptr && link->stop_requested.load()) return true;
        if (stopped) return true;
        return false;
    }

    void handle_bridge(const Guest& g, const std::vector<Handle>& args) {
        if (args.empty() || stopped || finished) return;
        const std::string kind = g.read_string(args[0]);

        if (kind == "output") {
            CodemodeOutputItem item;
            const std::string channel = args.size() > 1 ? g.read_string(args[1]) : std::string{};
            item.kind = channel == "image" ? CodemodeOutputItem::Kind::Image : CodemodeOutputItem::Kind::Text;
            item.data = args.size() > 2 ? g.read_string(args[2]) : std::string{};
            item.mime_type = args.size() > 3 ? g.read_string(args[3]) : std::string{};
            item.console = (channel == "console");

            buffered_chars += item.data.size();
            buffered.push_back(CodemodeRemoteOutputMessage{std::move(item)});

            if (buffered.size() >= kRemoteFlushItems || buffered_chars >= kRemoteFlushChars) {
                auto batch = std::move(buffered);
                buffered.clear();
                buffered_chars = 0;
                const auto reply = link->send_batch(std::move(batch), /*need_reply=*/false);
                if (std::holds_alternative<CodemodeRemoteStopReply>(reply)) stopped = true;
            }
            return;
        }

        if (kind == "call" || kind == "global") {
            CodemodeRemoteCallMessage call;
            const std::string id_text = args.size() > 1 ? g.read_string(args[1]) : std::string{};
            if (int parsed_id = 0;
                    std::from_chars(id_text.data(), id_text.data() + id_text.size(), parsed_id).ec == std::errc{}) {
                call.id = parsed_id;
            }
            call.target = (kind == "global") ? "global" : "tool";
            call.name = args.size() > 2 ? g.read_string(args[2]) : std::string{};
            if (args.size() > 3 && !g.is_undefined(args[3])) {
                call.args = g.read_string(args[3]);
            }

            auto batch = std::move(buffered);
            buffered.clear();
            buffered_chars = 0;
            batch.push_back(std::move(call));

            auto reply = link->send_batch(std::move(batch), /*need_reply=*/true);
            if (std::holds_alternative<CodemodeRemoteStopReply>(reply)) {
                stopped = true;
                return;
            }
            if (std::holds_alternative<CodemodeRemoteResultReply>(reply)) {
                const auto& res = std::get<CodemodeRemoteResultReply>(reply);
                const auto id_handle =
                        g.call_i32(g.find("qjs_new_number"), {WasmEdge_ValueGenF64(static_cast<double>(res.id))});
                const Handle payload_handle = g.new_string(res.payload.value_or(""));
                (void)g.call(settle_fn, api, {id_handle, res.ok ? g.true_value() : g.false_value(), payload_handle});
                (void)drain_jobs(g, [this] { return should_interrupt(); });
            }
            return;
        }

        if (kind == "done") {
            CodemodeRemoteDoneMessage done;
            const bool ok = args.size() > 1 && !g.is_undefined(args[1]) && g.read_string(args[1]) == "true";
            done.ok = ok;
            if (ok) {
                if (args.size() > 2 && !g.is_undefined(args[2])) done.value = g.read_string(args[2]);
                if (args.size() > 3 && !g.is_undefined(args[3])) done.writes = g.read_string(args[3]);
            } else {
                if (args.size() > 2 && !g.is_undefined(args[2])) done.error = g.read_string(args[2]);
            }
            finished = true;

            auto batch = std::move(buffered);
            buffered.clear();
            buffered_chars = 0;
            batch.push_back(std::move(done));
            const auto reply = link->send_batch(std::move(batch), /*need_reply=*/false);
            if (std::holds_alternative<CodemodeRemoteStopReply>(reply)) stopped = true;
            return;
        }
    }
};

WasmEdge_Result RemoteHostCall(
        void* data, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out) {
    auto* state = static_cast<RemoteVmState*>(data);
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

WasmEdge_Result RemoteHostInterrupt(
        void* data, const WasmEdge_CallingFrameContext*, const WasmEdge_Value*, WasmEdge_Value* out) {
    auto* state = static_cast<RemoteVmState*>(data);
    out[0] = gen_i32(state->should_interrupt() ? 1 : 0);
    return WasmEdge_Result_Success;
}

void run_remote_worker(CodemodeRemoteRequest request,
        std::uint64_t budget,
        std::optional<std::uint64_t> memory_limit_bytes,
        std::filesystem::path wasm_path,
        RemoteServeLink* link) {
    RemoteVmState state;
    state.link = link;
    state.budget = budget;

    WasmEdge_ConfigureContext* configure = WasmEdge_ConfigureCreate();
    if (memory_limit_bytes.has_value()) {
        WasmEdge_ConfigureSetMaxMemoryPage(
                configure, static_cast<std::uint32_t>((*memory_limit_bytes + 65535) / 65536));
    }

    WasmEdge_StoreContext* store = WasmEdge_StoreCreate();
    WasmEdge_ExecutorContext* executor = WasmEdge_ExecutorCreate(configure, nullptr);

    WasmEdge_ASTModuleContext* ast = nullptr;
    const auto wasm_str = wasm_path.string();
    WasmEdge_LoaderContext* loader = WasmEdge_LoaderCreate(configure);
    const WasmEdge_Result loaded = WasmEdge_LoaderParseFromFile(loader, &ast, wasm_str.c_str());
    WasmEdge_LoaderDelete(loader);

    auto cleanup = [&]() {
        if (ast != nullptr) WasmEdge_ASTModuleDelete(ast);
        WasmEdge_ExecutorDelete(executor);
        WasmEdge_StoreDelete(store);
        WasmEdge_ConfigureDelete(configure);
        link->mark_worker_done();
    };

    if (!WasmEdge_ResultOK(loaded) || ast == nullptr) {
        CodemodeRemoteFailureMessage fail;
        fail.error = CodemodeError{.kind = CodemodeError::Kind::Sandbox, .message = "Failed to load guest module"};
        link->send_batch({std::move(fail)}, /*need_reply=*/false);
        cleanup();
        return;
    }

    WasmEdge_ValidatorContext* validator = WasmEdge_ValidatorCreate(configure);
    const WasmEdge_Result valid = WasmEdge_ValidatorValidate(validator, ast);
    WasmEdge_ValidatorDelete(validator);
    if (!WasmEdge_ResultOK(valid)) {
        CodemodeRemoteFailureMessage fail;
        fail.error = CodemodeError{.kind = CodemodeError::Kind::Sandbox, .message = "Failed to validate guest module"};
        link->send_batch({std::move(fail)}, /*need_reply=*/false);
        cleanup();
        return;
    }

    WasmEdge_ModuleInstanceContext* env = make_module("env");
    add_function(env,
            "host_call",
            make_function(RemoteHostCall,
                    &state,
                    {WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32,
                            WasmEdge_ValType_I32},
                    {WasmEdge_ValType_I32}));
    add_function(env, "host_interrupt", make_function(RemoteHostInterrupt, &state, {}, {WasmEdge_ValType_I32}));
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
    add_function(wasi, "proc_exit", make_function(HostWasiStub, &state, {WasmEdge_ValType_I32}, {}));
    add_function(wasi,
            "environ_sizes_get",
            make_function(HostWasiStub, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));
    add_function(wasi,
            "environ_get",
            make_function(HostWasiStub, &state, {WasmEdge_ValType_I32, WasmEdge_ValType_I32}, {WasmEdge_ValType_I32}));

    const bool env_ok = WasmEdge_ResultOK(WasmEdge_ExecutorRegisterImport(executor, store, env));
    const bool wasi_ok = WasmEdge_ResultOK(WasmEdge_ExecutorRegisterImport(executor, store, wasi));

    WasmEdge_ModuleInstanceContext* module = nullptr;
    const WasmEdge_Result inst_res =
            (env_ok && wasi_ok) ? WasmEdge_ExecutorInstantiate(executor, &module, store, ast) : WasmEdge_Result_Fail;

    if (!env_ok || !wasi_ok || !WasmEdge_ResultOK(inst_res)) {
        CodemodeRemoteFailureMessage fail;
        fail.error =
                CodemodeError{.kind = CodemodeError::Kind::Sandbox, .message = "Failed to instantiate guest module"};
        link->send_batch({std::move(fail)}, /*need_reply=*/false);
        WasmEdge_ModuleInstanceDelete(env);
        WasmEdge_ModuleInstanceDelete(wasi);
        cleanup();
        return;
    }

    WasmEdge_String mem_str = WasmEdge_StringCreateByCString("memory");
    state.guest.executor = executor;
    state.guest.module = module;
    state.guest.memory = WasmEdge_ModuleInstanceFindMemory(module, mem_str);
    WasmEdge_StringDelete(mem_str);

    auto module_cleanup = [&]() {
        WasmEdge_ModuleInstanceDelete(env);
        WasmEdge_ModuleInstanceDelete(wasi);
        WasmEdge_ModuleInstanceDelete(module);
        cleanup();
    };

    const Guest& g = state.guest;
    g.call_void(g.find("_initialize"), {});
    if (g.call_i32(g.find("qjs_init"), {}) != 0) {
        CodemodeRemoteFailureMessage fail;
        fail.error = CodemodeError{.kind = CodemodeError::Kind::Sandbox, .message = "qjs_init failed"};
        link->send_batch({std::move(fail)}, /*need_reply=*/false);
        module_cleanup();
        return;
    }

    // Evaluate prelude
    const std::uint32_t code_ptr = g.write_cstr(kCodemodePreludeSource);
    const std::uint32_t name_ptr = g.write_cstr("codemode-prelude.js");
    const Handle prelude = g.call_i32(g.find("qjs_eval"),
            {gen_i32(static_cast<std::int32_t>(code_ptr)),
                    gen_i32(static_cast<std::int32_t>(std::strlen(kCodemodePreludeSource))),
                    gen_i32(static_cast<std::int32_t>(name_ptr)),
                    gen_i32(0)});
    if (g.is_exception(prelude)) {
        CodemodeRemoteFailureMessage fail;
        fail.error = CodemodeError{
                .kind = CodemodeError::Kind::Sandbox, .message = "Prelude evaluation failed: " + g.exception_text()};
        link->send_batch({std::move(fail)}, /*need_reply=*/false);
        module_cleanup();
        return;
    }

    const std::uint32_t bridge_name_ptr = g.write_cstr(kBridgeName);
    const Handle bridge = g.call_i32(g.find("qjs_new_host_function"),
            {gen_i32(static_cast<std::int32_t>(bridge_name_ptr)),
                    gen_i32(static_cast<std::int32_t>(kBridgeName.size())),
                    gen_i32(0)});
    const Handle undefined = g.undefined();
    const std::string tool_list = tools_json(request.data.tools);
    const std::string globals_list = detail::globals_json(request.data.globals);

    // Build store json text snapshot
    support::JsonValue::object_t store_obj;
    for (const auto& [k, v] : request.data.store) {
        store_obj.emplace(k, support::JsonValue{v});
    }
    auto store_written = support::write_json(support::JsonValue{std::move(store_obj)});
    const std::string store_snapshot = store_written ? *store_written : std::string{"{}"};

    state.api = g.call(prelude,
            undefined,
            {bridge, g.new_string(tool_list), g.new_string(globals_list), g.new_string(store_snapshot)});
    const Handle run_fn = g.get_prop(state.api, "run");
    state.settle_fn = g.get_prop(state.api, "settle");
    const Handle stalled_fn = g.get_prop(state.api, "stalled");

    // Wrap script
    const std::string wrapped = "(async (tools, console) => {" + request.data.code + "\n})";
    const std::uint32_t script_ptr = g.write_cstr(wrapped);
    const std::uint32_t script_name_ptr = g.write_cstr("codemode.js");
    const Handle script_fn = g.call_i32(g.find("qjs_eval"),
            {gen_i32(static_cast<std::int32_t>(script_ptr)),
                    gen_i32(static_cast<std::int32_t>(wrapped.size())),
                    gen_i32(static_cast<std::int32_t>(script_name_ptr)),
                    gen_i32(0)});

    if (g.is_exception(script_fn)) {
        auto exc = g.exception_error();
        CodemodeRemoteDoneMessage done;
        done.ok = false;
        support::JsonValue::object_t err_obj;
        if (exc.name.has_value()) err_obj.emplace("name", support::JsonValue{*exc.name});
        err_obj.emplace("message", support::JsonValue{exc.message});
        if (exc.stack.has_value()) err_obj.emplace("stack", support::JsonValue{*exc.stack});
        auto written_err = support::write_json(support::JsonValue{std::move(err_obj)});
        done.error = written_err ? *written_err : exc.message;

        auto batch = std::move(state.buffered);
        state.buffered.clear();
        batch.push_back(std::move(done));
        link->send_batch(std::move(batch), /*need_reply=*/false);
        module_cleanup();
        return;
    }

    g.call_void(g.find("qjs_set_interrupt_handler"), {gen_i32(1)});
    (void)g.call(run_fn, state.api, {script_fn});
    (void)drain_jobs(g, [&] { return state.should_interrupt(); });

    while (!state.finished && !state.stopped && !state.should_interrupt()) {
        (void)g.call(stalled_fn, state.api, {});
        (void)drain_jobs(g, [&] { return state.should_interrupt(); });
        break;
    }

    if (!state.finished && !state.stopped) {
        if (state.budget_exhausted) {
            CodemodeRemoteFailureMessage fail;
            fail.error = CodemodeError{.kind = CodemodeError::Kind::Timeout,
                    .message = std::format("Execution exceeded its interrupt budget of {}", state.budget)};
            auto batch = std::move(state.buffered);
            state.buffered.clear();
            batch.push_back(std::move(fail));
            const auto reply = link->send_batch(std::move(batch), /*need_reply=*/false);
            if (std::holds_alternative<CodemodeRemoteStopReply>(reply)) state.stopped = true;
        } else if (g.is_exception(g.call_i32(g.find("qjs_get_exception"), {}))) {
            auto exc = g.exception_error();
            CodemodeRemoteDoneMessage done;
            done.ok = false;
            support::JsonValue::object_t err_obj;
            if (exc.name.has_value()) err_obj.emplace("name", support::JsonValue{*exc.name});
            err_obj.emplace("message", support::JsonValue{exc.message});
            if (exc.stack.has_value()) err_obj.emplace("stack", support::JsonValue{*exc.stack});
            auto written_err = support::write_json(support::JsonValue{std::move(err_obj)});
            done.error = written_err ? *written_err : exc.message;

            auto batch = std::move(state.buffered);
            state.buffered.clear();
            batch.push_back(std::move(done));
            const auto reply = link->send_batch(std::move(batch), /*need_reply=*/false);
            if (std::holds_alternative<CodemodeRemoteStopReply>(reply)) state.stopped = true;
        } else {
            CodemodeRemoteCrashMessage crash;
            crash.message = "The remote codemode guest stopped without reporting a result.";
            auto batch = std::move(state.buffered);
            state.buffered.clear();
            batch.push_back(std::move(crash));
            const auto reply = link->send_batch(std::move(batch), /*need_reply=*/false);
            if (std::holds_alternative<CodemodeRemoteStopReply>(reply)) state.stopped = true;
        }
    }

    module_cleanup();
}

} // namespace

support::AsyncResult<void> serve_codemode_remote(
        CodemodeRemoteRequest request, CodemodeRemoteExchangeHook exchange, CodemodeServeOptions options) {
    const std::uint64_t max_budget = options.max_interrupt_budget.value_or(kDefaultInterruptBudget);
    const std::uint64_t budget =
            request.interrupt_budget.has_value() ? std::min(*request.interrupt_budget, max_budget) : max_budget;

    const auto req_mem = request.data.memory_limit_bytes;
    const auto max_mem = options.max_memory_limit_bytes;
    std::optional<std::uint64_t> memory_limit_bytes;
    if (req_mem.has_value() && max_mem.has_value())
        memory_limit_bytes = std::min(*req_mem, *max_mem);
    else if (req_mem.has_value())
        memory_limit_bytes = req_mem;
    else
        memory_limit_bytes = max_mem;

    std::filesystem::path wasm_path =
            options.wasm_path.empty() ? default_codemode_guest_wasm_path() : options.wasm_path;

    return support::detail::make_async_result(
            [request = std::move(request),
                    exchange = std::move(exchange),
                    budget,
                    memory_limit_bytes,
                    wasm_path = std::move(wasm_path)]() mutable -> boost::asio::awaitable<support::Expected<void>> {
                auto executor = co_await boost::asio::this_coro::executor;
                auto link = std::make_shared<RemoteServeLink>();

                std::thread worker(
                        [request = std::move(request), budget, memory_limit_bytes, wasm_path, link]() mutable {
                            run_remote_worker(
                                    std::move(request), budget, memory_limit_bytes, std::move(wasm_path), link.get());
                        });

                boost::asio::steady_timer timer(executor);
                while (true) {
                    std::vector<CodemodeRemoteMessageVariant> batch;
                    bool need_reply = false;
                    bool has_batch = false;
                    bool worker_done = false;
                    {
                        std::lock_guard lock(link->mutex);
                        if (link->has_outgoing) {
                            batch = std::move(link->outgoing_batch);
                            need_reply = link->wait_for_reply;
                            link->has_outgoing = false;
                            has_batch = true;
                            link->cv_worker.notify_all();
                        }
                        worker_done = link->worker_done.load();
                    }

                    if (!has_batch) {
                        if (worker_done) break;
                        timer.expires_after(std::chrono::milliseconds{5});
                        const auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                        (void)ec;
                        continue;
                    }

                    auto reply_result = co_await support::detail::await_async_result(exchange(std::move(batch)));
                    {
                        std::lock_guard lock(link->mutex);
                        if (!reply_result || std::holds_alternative<CodemodeRemoteStopReply>(*reply_result)) {
                            link->stop_requested.store(true);
                        } else if (need_reply) {
                            link->incoming_reply = std::move(*reply_result);
                            link->has_reply = true;
                        }
                        link->batch_in_flight = false;
                        link->cv_worker.notify_all();
                    }
                    if (link->stop_requested.load()) break;
                }

                if (worker.joinable()) worker.join();
                co_return support::ExpectedVoid{};
            });
}

struct RemoteCodemodeSandbox::Impl {
    struct PendingExchange {
        std::mutex mutex;
        bool resolved{false};
        std::optional<CodemodeRemoteReplyVariant> reply;
        support::AsyncCompletion<CodemodeRemoteReplyVariant, support::Error> completion;

        void resolve(CodemodeRemoteReplyVariant value) {
            support::AsyncCompletion<CodemodeRemoteReplyVariant, support::Error> done;
            {
                std::lock_guard lock(mutex);
                if (resolved) return;
                resolved = true;
                if (completion) {
                    done = std::move(completion);
                } else {
                    reply = std::move(value);
                    return;
                }
            }
            done(support::Expected<CodemodeRemoteReplyVariant>{std::move(value)});
        }

        void set_completion(support::AsyncCompletion<CodemodeRemoteReplyVariant, support::Error> done) {
            std::optional<CodemodeRemoteReplyVariant> ready;
            {
                std::lock_guard lock(mutex);
                if (resolved) {
                    ready = std::move(reply);
                } else {
                    completion = std::move(done);
                    return;
                }
            }
            done(support::Expected<CodemodeRemoteReplyVariant>{std::move(*ready)});
        }
    };

    struct RunState {
        std::mutex mutex;
        bool settled{false};
        bool stopped{false};
        bool remote_returned{false};
        std::optional<support::Error> remote_error;
        std::optional<std::string> abort_message;
        CodemodeRunResult result;
        std::vector<std::chrono::steady_clock::time_point> call_issued;
        std::vector<std::weak_ptr<PendingExchange>> pending_exchanges;
        std::stop_source call_stop;
        std::move_only_function<void()> wake_host;

        void request_stop(std::string message) {
            std::vector<std::shared_ptr<PendingExchange>> pending;
            std::move_only_function<void()> wake;
            {
                std::lock_guard lock(mutex);
                if (stopped || settled) return;
                stopped = true;
                abort_message = message;
                result.error = CodemodeError{.kind = CodemodeError::Kind::Aborted, .message = message};
                const auto now = std::chrono::steady_clock::now();
                for (std::size_t i = 0; i < result.calls.size(); ++i) {
                    if (!result.calls[i].duration_ms.has_value()) {
                        result.calls[i].status = CodemodeCallRecord::Status::Cancelled;
                        result.calls[i].duration_ms =
                                std::chrono::duration<double, std::milli>(now - call_issued[i]).count();
                    }
                }
                wake = std::move(wake_host);
                std::erase_if(pending_exchanges, [&pending](const std::weak_ptr<PendingExchange>& weak) {
                    if (auto active = weak.lock()) {
                        pending.push_back(std::move(active));
                        return false;
                    }
                    return true;
                });
            }
            call_stop.request_stop();
            for (const auto& exchange : pending)
                exchange->resolve(CodemodeRemoteStopReply{});
            if (wake) wake();
        }
    };

    RemoteCodemodeSandboxOptions options;
    std::mutex mutex;
    bool closed{false};
    std::vector<std::weak_ptr<RunState>> active_runs;

    explicit Impl(RemoteCodemodeSandboxOptions opts) : options(std::move(opts)) {}
};

RemoteCodemodeSandbox::RemoteCodemodeSandbox(ConstructionKey, std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
RemoteCodemodeSandbox::RemoteCodemodeSandbox(RemoteCodemodeSandbox&&) noexcept = default;
RemoteCodemodeSandbox& RemoteCodemodeSandbox::operator=(RemoteCodemodeSandbox&&) noexcept = default;
RemoteCodemodeSandbox::~RemoteCodemodeSandbox() = default;

support::Expected<std::unique_ptr<RemoteCodemodeSandbox>> RemoteCodemodeSandbox::create(
        RemoteCodemodeSandboxOptions options) {
    return std::make_unique<RemoteCodemodeSandbox>(ConstructionKey{}, std::make_shared<Impl>(std::move(options)));
}

void RemoteCodemodeSandbox::close() {
    std::vector<std::shared_ptr<Impl::RunState>> active_runs;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closed) return;
        impl_->closed = true;
        std::erase_if(impl_->active_runs, [&active_runs](const std::weak_ptr<Impl::RunState>& weak) {
            if (auto active = weak.lock()) {
                active_runs.push_back(std::move(active));
                return false;
            }
            return true;
        });
    }
    for (const auto& state : active_runs)
        state->request_stop("Sandbox closed");
}

support::AsyncResult<CodemodeRunResult> RemoteCodemodeSandbox::run(std::string script,
        std::vector<CodemodeToolDescriptor> tools,
        CodemodeLimits limits,
        std::stop_token stop_token,
        CodemodeToolCallHandler handler,
        std::map<std::string, support::JsonValue> store,
        std::optional<std::string> abort_message) {
    if (stop_token.stop_requested()) {
        CodemodeRunResult result;
        result.error = CodemodeError{
                .kind = CodemodeError::Kind::Aborted, .message = abort_message.value_or("This operation was aborted")};
        return support::AsyncResult<CodemodeRunResult>{support::Expected<CodemodeRunResult>{std::move(result)}};
    }

    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closed) {
            CodemodeRunResult result;
            result.error = CodemodeError{.kind = CodemodeError::Kind::Aborted, .message = "Sandbox closed"};
            return support::AsyncResult<CodemodeRunResult>{support::Expected<CodemodeRunResult>{std::move(result)}};
        }
    }

    if (!impl_->options.remote) {
        CodemodeRunResult result;
        result.error = CodemodeError{.kind = CodemodeError::Kind::Sandbox,
                .message = "Failed to reach the remote sandbox: no remote configured"};
        return support::AsyncResult<CodemodeRunResult>{support::Expected<CodemodeRunResult>{std::move(result)}};
    }

    auto remote_res = impl_->options.remote();
    if (!remote_res) {
        CodemodeRunResult result;
        result.error = CodemodeError{.kind = CodemodeError::Kind::Sandbox,
                .message = std::format("Failed to reach the remote sandbox: {}", remote_res.error().message)};
        return support::AsyncResult<CodemodeRunResult>{support::Expected<CodemodeRunResult>{std::move(result)}};
    }

    auto remote = *remote_res;
    if (!remote) {
        CodemodeRunResult result;
        result.error = CodemodeError{
                .kind = CodemodeError::Kind::Sandbox, .message = "Failed to reach the remote sandbox: remote is null"};
        return support::AsyncResult<CodemodeRunResult>{support::Expected<CodemodeRunResult>{std::move(result)}};
    }

    CodemodeRemoteRequest request;
    request.data.code = std::move(script);
    request.data.tools = std::move(tools);
    request.data.globals = impl_->options.globals;
    request.data.memory_limit_bytes = limits.memory_limit_bytes;
    for (const auto& [k, v] : store) {
        auto written = support::write_json(v);
        request.data.store.emplace(k, written ? *written : "null");
    }
    request.interrupt_budget =
            limits.interrupt_budget.has_value() ? limits.interrupt_budget : impl_->options.interrupt_budget;

    return support::detail::make_async_result([impl = impl_,
                                                      remote = std::move(remote),
                                                      request = std::move(request),
                                                      stop_token,
                                                      handler = std::move(handler),
                                                      abort_message = std::move(abort_message)]() mutable
                                                      -> boost::asio::awaitable<support::Expected<CodemodeRunResult>> {
        auto executor = co_await boost::asio::this_coro::executor;
        auto state = std::make_shared<Impl::RunState>();
        auto timer = std::make_shared<boost::asio::steady_timer>(executor);
        {
            std::lock_guard lock(state->mutex);
            state->wake_host = [executor, timer] { boost::asio::post(executor, [timer] { timer->cancel(); }); };
        }
        {
            std::lock_guard lock(impl->mutex);
            if (impl->closed) {
                CodemodeRunResult result;
                result.error = CodemodeError{.kind = CodemodeError::Kind::Aborted, .message = "Sandbox closed"};
                co_return result;
            }
            impl->active_runs.push_back(state);
        }
        auto handler_ptr = std::make_shared<CodemodeToolCallHandler>(std::move(handler));

        auto exchange = [state, handler_ptr, executor](std::vector<CodemodeRemoteMessageVariant> messages)
                -> support::AsyncResult<CodemodeRemoteReplyVariant> {
            auto pending = std::make_shared<Impl::PendingExchange>();
            return support::AsyncResult<CodemodeRemoteReplyVariant>{support::AsyncProducer<CodemodeRemoteReplyVariant,
                    support::Error>{[state, handler_ptr, executor, messages = std::move(messages), pending](
                                            support::AsyncCompletion<CodemodeRemoteReplyVariant, support::Error>
                                                    done) mutable noexcept {
                pending->set_completion(std::move(done));
                bool stopped = false;
                {
                    std::lock_guard lock(state->mutex);
                    stopped = state->stopped;
                    if (!stopped) state->pending_exchanges.push_back(pending);
                }
                if (stopped) {
                    pending->resolve(CodemodeRemoteStopReply{});
                    return;
                }
                boost::asio::co_spawn(
                        executor,
                        [state, handler_ptr, messages = std::move(messages), pending]() mutable
                                -> boost::asio::awaitable<void> {
                            std::unique_lock lock(state->mutex);
                            if (state->stopped) {
                                lock.unlock();
                                pending->resolve(CodemodeRemoteStopReply{});
                                co_return;
                            }

                            std::optional<CodemodeRemoteCallMessage> call;
                            for (const auto& msg : messages) {
                                std::visit(
                                        [&](const auto& m) {
                                            using T = std::decay_t<decltype(m)>;
                                            if constexpr (std::is_same_v<T, CodemodeRemoteOutputMessage>) {
                                                state->result.output.push_back(m.item);
                                            } else if constexpr (std::is_same_v<T, CodemodeRemoteCallMessage>) {
                                                call = m;
                                            } else if constexpr (std::is_same_v<T, CodemodeRemoteDoneMessage>) {
                                                state->settled = true;
                                                if (m.ok) {
                                                    state->result.value_json = m.value;
                                                    if (!m.writes.empty()) {
                                                        state->result.store_writes = parse_store_writes(m.writes);
                                                    }
                                                } else if (m.error.has_value()) {
                                                    state->result.error = parse_error_json(*m.error);
                                                }
                                            } else if constexpr (std::is_same_v<T, CodemodeRemoteFailureMessage>) {
                                                state->settled = true;
                                                state->result.error = m.error;
                                            } else if constexpr (std::is_same_v<T, CodemodeRemoteCrashMessage>) {
                                                state->settled = true;
                                                state->result.error = CodemodeError{
                                                        .kind = CodemodeError::Kind::Sandbox, .message = m.message};
                                            }
                                        },
                                        msg);
                            }

                            if (!call.has_value()) {
                                lock.unlock();
                                pending->resolve(CodemodeRemoteNullReply{});
                                co_return;
                            }

                            const auto record_idx = state->result.calls.size();
                            state->result.calls.push_back(CodemodeCallRecord{.name = call->name,
                                    .status = CodemodeCallRecord::Status::Ok,
                                    .duration_ms = std::nullopt});
                            state->call_issued.push_back(std::chrono::steady_clock::now());
                            const std::string call_name = call->name;
                            const std::string call_args = call->args.value_or("");
                            const auto call_id = call->id;
                            lock.unlock();

                            std::string payload;
                            bool ok = false;
                            if (*handler_ptr) {
                                auto call_res = co_await support::detail::await_async_result(
                                        (*handler_ptr)(call_name, call_args, state->call_stop.get_token()));
                                if (call_res) {
                                    ok = true;
                                    payload = std::move(*call_res);
                                } else {
                                    payload = call_res.error().message;
                                }
                            } else {
                                payload = std::format("tool '{}' is not available to the codemode sandbox", call_name);
                            }

                            lock.lock();
                            const auto now = std::chrono::steady_clock::now();
                            if (!state->result.calls[record_idx].duration_ms.has_value()) {
                                state->result.calls[record_idx].duration_ms =
                                        std::chrono::duration<double, std::milli>(now - state->call_issued[record_idx])
                                                .count();
                            }
                            if (state->stopped) {
                                state->result.calls[record_idx].status = CodemodeCallRecord::Status::Cancelled;
                                lock.unlock();
                                pending->resolve(CodemodeRemoteStopReply{});
                                co_return;
                            }
                            state->result.calls[record_idx].status =
                                    ok ? CodemodeCallRecord::Status::Ok : CodemodeCallRecord::Status::Error;
                            CodemodeRemoteReplyVariant reply = CodemodeRemoteResultReply{.id = call_id,
                                    .ok = ok,
                                    .payload = std::move(payload),
                                    .seq = std::nullopt,
                                    .now = std::nullopt,
                                    .properties = {}};
                            lock.unlock();
                            pending->resolve(std::move(reply));
                        },
                        boost::asio::detached);
            }}};
        };

        auto worker_finished = std::make_shared<std::atomic<bool>>(false);
        std::thread worker([state,
                                   remote = std::move(remote),
                                   request = std::move(request),
                                   exchange = std::move(exchange),
                                   worker_finished]() mutable {
            boost::asio::io_context remote_ctx;
            boost::asio::co_spawn(
                    remote_ctx,
                    [state,
                            remote = std::move(remote),
                            request = std::move(request),
                            exchange = std::move(exchange)]() mutable -> boost::asio::awaitable<void> {
                        auto exec_res = co_await support::detail::await_async_result(
                                remote->execute(std::move(request), std::move(exchange)));
                        std::lock_guard lock(state->mutex);
                        state->remote_returned = true;
                        if (!exec_res) state->remote_error = exec_res.error();
                    },
                    boost::asio::detached);
            remote_ctx.run();
            worker_finished->store(true, std::memory_order_release);
            RemoteWorkerReaper::instance().notify();
        });
        RemoteWorkerReaper::instance().adopt(std::move(worker), worker_finished);

        std::stop_callback stop_callback(stop_token,
                [state, abort_message] { state->request_stop(abort_message.value_or("This operation was aborted")); });
        if (stop_token.stop_requested()) {
            state->request_stop(abort_message.value_or("This operation was aborted"));
        }
        while (true) {
            {
                std::lock_guard lock(state->mutex);
                if (state->stopped) {
                    state->wake_host = {};
                    co_return state->result;
                }
                if (state->remote_returned || state->settled) {
                    if (state->settled) {
                        state->wake_host = {};
                        co_return state->result;
                    }
                    if (state->remote_error.has_value()) {
                        state->result.error = CodemodeError{.kind = CodemodeError::Kind::Sandbox,
                                .message = std::format("Remote sandbox failed: {}", state->remote_error->message)};
                    } else {
                        state->result.error = CodemodeError{.kind = CodemodeError::Kind::Sandbox,
                                .message = "Remote sandbox returned before the script settled"};
                    }
                    state->wake_host = {};
                    co_return state->result;
                }
            }

            timer->expires_after(std::chrono::milliseconds{5});
            const auto [error] = co_await timer->async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            (void)error;
        }
    });
}

} // namespace cch::codemode
