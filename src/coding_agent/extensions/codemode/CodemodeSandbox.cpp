#include "coding_agent/extensions/codemode/CodemodeSandbox.hpp"

#include "coding_agent/extensions/codemode/CodemodePrelude.hpp"

#include "support/Json.hpp"

#include <wasmedge/wasmedge.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

namespace {

using Handle = std::int32_t;
using VT = enum WasmEdge_ValType;

constexpr std::string_view kBridgeName = "bridge";
constexpr std::int32_t kWasiErnoSuccess = 0;
constexpr std::int32_t kWasiErnoBadf = 8;
constexpr std::int32_t kWasiErnoNosys = 52;

WasmEdge_Value gen_i32(std::int32_t value) { return WasmEdge_ValueGenI32(value); }

/// One instantiated guest plus the re-entrant call helpers. The helpers read
/// and call into the guest from inside a host import (the `host_call` bridge),
/// where WasmEdge exposes the calling frame's executor and module.
struct Guest {
    WasmEdge_ExecutorContext* executor = nullptr;
    WasmEdge_ModuleInstanceContext* module = nullptr;
    WasmEdge_MemoryInstanceContext* memory = nullptr;

    [[nodiscard]] WasmEdge_FunctionInstanceContext* find(const char* name) const {
        WasmEdge_String s = WasmEdge_StringCreateByCString(name);
        WasmEdge_FunctionInstanceContext* f = WasmEdge_ModuleInstanceFindFunction(module, s);
        WasmEdge_StringDelete(s);
        return f;
    }

    [[nodiscard]] std::int32_t call_i32(
            WasmEdge_FunctionInstanceContext* fn, std::initializer_list<WasmEdge_Value> params) const {
        if (fn == nullptr) return 0;
        std::vector<WasmEdge_Value> in(params);
        WasmEdge_Value out[1]{};
        if (!WasmEdge_ResultOK(WasmEdge_ExecutorInvoke(
                    executor, fn, in.empty() ? nullptr : in.data(), static_cast<std::uint32_t>(in.size()), out, 1))) {
            return 0;
        }
        return WasmEdge_ValueGetI32(out[0]);
    }

    void call_void(WasmEdge_FunctionInstanceContext* fn, std::initializer_list<WasmEdge_Value> params) const {
        if (fn == nullptr) return;
        std::vector<WasmEdge_Value> in(params);
        (void)WasmEdge_ExecutorInvoke(
                executor, fn, in.empty() ? nullptr : in.data(), static_cast<std::uint32_t>(in.size()), nullptr, 0);
    }

    [[nodiscard]] std::uint32_t load_u32(std::uint32_t offset) const {
        std::uint32_t value = 0;
        if (std::uint8_t* p = WasmEdge_MemoryInstanceGetPointer(memory, offset, 4)) std::memcpy(&value, p, 4);
        return value;
    }

    void store_u32(std::uint32_t offset, std::uint32_t value) const {
        if (std::uint8_t* p = WasmEdge_MemoryInstanceGetPointer(memory, offset, 4)) std::memcpy(p, &value, 4);
    }

    void store_u64(std::uint32_t offset, std::uint64_t value) const {
        if (std::uint8_t* p = WasmEdge_MemoryInstanceGetPointer(memory, offset, 8)) std::memcpy(p, &value, 8);
    }

    /// Copy a NUL-terminated string into guest memory; returns its pointer (0 on OOM).
    [[nodiscard]] std::uint32_t write_cstr(std::string_view text) const {
        const std::int32_t ptr = call_i32(find("wasm_malloc"), {gen_i32(static_cast<std::int32_t>(text.size() + 1))});
        if (ptr == 0) return 0;
        std::uint8_t* dst = WasmEdge_MemoryInstanceGetPointer(
                memory, static_cast<std::uint32_t>(ptr), static_cast<std::uint32_t>(text.size() + 1));
        if (dst == nullptr) return 0;
        std::memcpy(dst, text.data(), text.size());
        dst[text.size()] = '\0';
        return static_cast<std::uint32_t>(ptr);
    }

    /// Stringify a guest value the way pi's `JSValueHandle.toString()` does: the
    /// explicit byte length is read so embedded NULs survive. Any value coerces
    /// to a string (executing guest `toString`, as pi's bridge does).
    [[nodiscard]] std::string read_string(Handle handle) const {
        const std::int32_t len_ptr = call_i32(find("wasm_malloc"), {gen_i32(4)});
        if (len_ptr == 0) return {};
        const std::int32_t cstr = call_i32(find("qjs_get_string_len"), {gen_i32(handle), gen_i32(len_ptr)});
        if (cstr == 0) return {};
        const std::uint32_t length = load_u32(static_cast<std::uint32_t>(len_ptr));
        std::string text;
        if (std::uint8_t* p = WasmEdge_MemoryInstanceGetPointer(memory, static_cast<std::uint32_t>(cstr), length)) {
            text.assign(reinterpret_cast<char*>(p), length);
        }
        call_void(find("qjs_free_cstring"), {gen_i32(cstr)});
        return text;
    }

    [[nodiscard]] Handle new_string(std::string_view text) const {
        const std::uint32_t ptr = write_cstr(text);
        return call_i32(find("qjs_new_string"),
                {gen_i32(static_cast<std::int32_t>(ptr)), gen_i32(static_cast<std::int32_t>(text.size()))});
    }

    [[nodiscard]] Handle undefined() const { return call_i32(find("qjs_get_undefined"), {}); }
    [[nodiscard]] Handle true_value() const { return call_i32(find("qjs_get_true"), {}); }
    [[nodiscard]] Handle false_value() const { return call_i32(find("qjs_get_false"), {}); }
    [[nodiscard]] bool is_undefined(Handle handle) const {
        return call_i32(find("qjs_is_undefined"), {gen_i32(handle)}) != 0;
    }

    [[nodiscard]] Handle global() const { return call_i32(find("qjs_get_global"), {}); }

    [[nodiscard]] Handle get_prop(Handle object, std::string_view name) const {
        const std::uint32_t ptr = write_cstr(name);
        return call_i32(find("qjs_get_prop_string"), {gen_i32(object), gen_i32(static_cast<std::int32_t>(ptr))});
    }

    Handle call(Handle function, Handle this_value, const std::vector<Handle>& args) const {
        std::uint32_t argv = 0;
        if (!args.empty()) {
            argv = static_cast<std::uint32_t>(
                    call_i32(find("wasm_malloc"), {gen_i32(static_cast<std::int32_t>(args.size() * 4))}));
            for (std::size_t i = 0; i < args.size(); ++i)
                store_u32(argv + static_cast<std::uint32_t>(i * 4), static_cast<std::uint32_t>(args[i]));
        }
        return call_i32(find("qjs_call"),
                {gen_i32(function),
                        gen_i32(this_value),
                        gen_i32(static_cast<std::int32_t>(args.size())),
                        gen_i32(static_cast<std::int32_t>(argv))});
    }

    [[nodiscard]] bool is_exception(Handle handle) const {
        return call_i32(find("qjs_is_exception"), {gen_i32(handle)}) != 0;
    }

    [[nodiscard]] std::string exception_text() const {
        const Handle exc = call_i32(find("qjs_get_exception"), {});
        return read_string(exc);
    }
};

/// One queued `tools.*`/global call the script made and is awaiting.
struct PendingToolCall {
    Handle id = 0;
    std::string name;
    std::string arguments_json;
};

/// All mutable state of one `run`. A pointer to it is the `data` of every host
/// import, so the bridge can reach the guest and the accumulating result.
struct RunState {
    Guest guest;
    CodemodeLimits limits;
    std::stop_token stop_token;
    CodemodeToolCallHandler handler;
    std::vector<CodemodeToolDescriptor> tools;
    std::chrono::steady_clock::time_point started;
    bool timed_out = false;
    bool finished = false;
    std::optional<std::string> value_json;
    std::optional<CodemodeError> error;
    std::vector<CodemodeOutputItem> output;
    std::vector<PendingToolCall> pending;

    [[nodiscard]] bool interrupted() {
        if (stop_token.stop_requested()) return true;
        if (limits.timeout.count() > 0 && std::chrono::steady_clock::now() - started > limits.timeout) {
            timed_out = true;
            return true;
        }
        return false;
    }

    void emit_done(const Guest& g, const std::vector<Handle>& args) {
        const bool ok = args.size() > 1 && !g.is_undefined(args[1]) && g.read_string(args[1]) == "true";
        if (ok) {
            if (args.size() > 2 && !g.is_undefined(args[2])) value_json = g.read_string(args[2]);
        } else {
            error = CodemodeError{
                    CodemodeError::Kind::Script, args.size() > 2 ? message_from(args[2], g) : std::string{}};
        }
        finished = true;
    }

    [[nodiscard]] static std::string message_from(Handle payload, const Guest& g) {
        const std::string raw = g.read_string(payload);
        auto parsed = support::read_json(raw);
        if (parsed && parsed->holds<support::JsonValue::object_t>()) {
            const auto& object = parsed->get_object();
            if (const auto it = object.find("message"); it != object.end()) {
                if (const auto* message = it->second.get_if<std::string>()) return *message;
            }
        }
        return raw;
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

WasmEdge_Result HostPromiseRejection(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value*) {
    // The shim frees the two heap-owned values the guest handed over; without a
    // rejection handler they would leak. #874 has no unhandled-rejection surface.
    Guest guest{WasmEdge_CallingFrameGetExecutor(frame),
            const_cast<WasmEdge_ModuleInstanceContext*>(WasmEdge_CallingFrameGetModuleInstance(frame)),
            WasmEdge_CallingFrameGetMemoryInstance(frame, 0)};
    guest.call_void(guest.find("qjs_free_value"), {gen_i32(WasmEdge_ValueGetI32(in[0]))});
    guest.call_void(guest.find("qjs_free_value"), {gen_i32(WasmEdge_ValueGetI32(in[1]))});
    return WasmEdge_Result_Success;
}

WasmEdge_Result HostTimezone(void*, const WasmEdge_CallingFrameContext*, const WasmEdge_Value*, WasmEdge_Value* out) {
    out[0] = gen_i32(0);
    return WasmEdge_Result_Success;
}

/// No ESM loader is registered: a `import`/`require` in the script fails closed
/// rather than reaching any host module.
WasmEdge_Result HostModuleDenied(
        void*, const WasmEdge_CallingFrameContext*, const WasmEdge_Value*, WasmEdge_Value* out) {
    out[0] = gen_i32(0);
    return WasmEdge_Result_Success;
}

WasmEdge_Result HostFdWrite(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out) {
    // The guest's engine diagnostics go to fd 1/2; they belong to the host and
    // are discarded (pi `discardOutput`). Reported as written so libc does not retry.
    const std::int32_t fd = WasmEdge_ValueGetI32(in[0]);
    if (fd != 1 && fd != 2) {
        out[0] = gen_i32(kWasiErnoBadf);
        return WasmEdge_Result_Success;
    }
    Guest guest{nullptr,
            const_cast<WasmEdge_ModuleInstanceContext*>(WasmEdge_CallingFrameGetModuleInstance(frame)),
            WasmEdge_CallingFrameGetMemoryInstance(frame, 0)};
    const std::uint32_t iovs = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[1]));
    const std::uint32_t count = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[2]));
    const std::uint32_t nwritten = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[3]));
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < count; ++i)
        total += guest.load_u32(iovs + i * 8 + 4);
    guest.store_u32(nwritten, total);
    out[0] = gen_i32(kWasiErnoSuccess);
    return WasmEdge_Result_Success;
}

WasmEdge_Result HostFdFdstatGet(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out) {
    const std::int32_t fd = WasmEdge_ValueGetI32(in[0]);
    if (fd != 1 && fd != 2) {
        out[0] = gen_i32(kWasiErnoBadf);
        return WasmEdge_Result_Success;
    }
    Guest guest{nullptr,
            const_cast<WasmEdge_ModuleInstanceContext*>(WasmEdge_CallingFrameGetModuleInstance(frame)),
            WasmEdge_CallingFrameGetMemoryInstance(frame, 0)};
    const std::uint32_t stat = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[1]));
    if (std::uint8_t* p = WasmEdge_MemoryInstanceGetPointer(guest.memory, stat, 24)) {
        std::memset(p, 0, 24);
        p[0] = 2; // fs_filetype = CHARACTER_DEVICE
    }
    out[0] = gen_i32(kWasiErnoSuccess);
    return WasmEdge_Result_Success;
}

WasmEdge_Result HostClockTimeGet(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out) {
    const std::int32_t clock_id = WasmEdge_ValueGetI32(in[0]);
    if (clock_id != 0 && clock_id != 1) {
        out[0] = gen_i32(kWasiErnoNosys);
        return WasmEdge_Result_Success;
    }
    Guest guest{nullptr,
            const_cast<WasmEdge_ModuleInstanceContext*>(WasmEdge_CallingFrameGetModuleInstance(frame)),
            WasmEdge_CallingFrameGetMemoryInstance(frame, 0)};
    const std::uint32_t result_ptr = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[2]));
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    guest.store_u64(result_ptr, static_cast<std::uint64_t>(nanos));
    out[0] = gen_i32(kWasiErnoSuccess);
    return WasmEdge_Result_Success;
}

WasmEdge_Result HostRandomGet(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value* out) {
    Guest guest{nullptr,
            const_cast<WasmEdge_ModuleInstanceContext*>(WasmEdge_CallingFrameGetModuleInstance(frame)),
            WasmEdge_CallingFrameGetMemoryInstance(frame, 0)};
    const std::uint32_t buf = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[0]));
    const std::uint32_t len = static_cast<std::uint32_t>(WasmEdge_ValueGetI32(in[1]));
    if (std::uint8_t* p = WasmEdge_MemoryInstanceGetPointer(guest.memory, buf, len)) {
        // Not a cryptographic boundary: the guest only seeds Math.random and
        // extension PRNGs, and no host secret crosses it.
        std::uint64_t seed = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint32_t i = 0; i < len; ++i) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            p[i] = static_cast<std::uint8_t>(seed >> 33);
        }
    }
    out[0] = gen_i32(kWasiErnoSuccess);
    return WasmEdge_Result_Success;
}

WasmEdge_Result HostWasiStub(void*, const WasmEdge_CallingFrameContext*, const WasmEdge_Value*, WasmEdge_Value* out) {
    out[0] = gen_i32(kWasiErnoNosys);
    return WasmEdge_Result_Success;
}

WasmEdge_FunctionInstanceContext* make_function(
        WasmEdge_HostFunc_t function, void* data, std::initializer_list<VT> params, std::initializer_list<VT> returns) {
    std::vector<VT> p(params);
    std::vector<VT> r(returns);
    WasmEdge_FunctionTypeContext* type = WasmEdge_FunctionTypeCreate(p.empty() ? nullptr : p.data(),
            static_cast<std::uint32_t>(p.size()),
            r.empty() ? nullptr : r.data(),
            static_cast<std::uint32_t>(r.size()));
    WasmEdge_FunctionInstanceContext* instance = WasmEdge_FunctionInstanceCreate(type, function, data, 0);
    WasmEdge_FunctionTypeDelete(type);
    return instance;
}

void add_function(WasmEdge_ModuleInstanceContext* module, const char* name, WasmEdge_FunctionInstanceContext* fn) {
    WasmEdge_String s = WasmEdge_StringCreateByCString(name);
    WasmEdge_ModuleInstanceAddFunction(module, s, fn);
    WasmEdge_StringDelete(s);
}

WasmEdge_ModuleInstanceContext* make_module(const char* name) {
    WasmEdge_String s = WasmEdge_StringCreateByCString(name);
    WasmEdge_ModuleInstanceContext* module = WasmEdge_ModuleInstanceCreate(s);
    WasmEdge_StringDelete(s);
    return module;
}

[[nodiscard]] support::Error sandbox_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Validation, std::move(message), std::move(detail));
}

/// pi `toCodemodeIdentifier`-shaped JSON list of the tools the script may call.
[[nodiscard]] std::string tools_json(const std::vector<CodemodeToolDescriptor>& tools) {
    support::JsonValue::array_t list;
    list.reserve(tools.size());
    for (const auto& tool : tools) {
        list.push_back(support::JsonValue::object_t{
                {"name", tool.name},
                {"jsName", tool.js_name},
                {"description", tool.description},
        });
    }
    auto written = support::write_json(support::JsonValue{std::move(list)});
    return written ? *written : std::string{"[]"};
}

} // namespace

struct CodemodeSandbox::Impl {
    std::filesystem::path wasm_path;
    WasmEdge_ConfigureContext* configure = nullptr;
    WasmEdge_ASTModuleContext* ast = nullptr;

    ~Impl() {
        if (ast != nullptr) WasmEdge_ASTModuleDelete(ast);
        if (configure != nullptr) WasmEdge_ConfigureDelete(configure);
    }
};

support::Expected<std::unique_ptr<CodemodeSandbox>> CodemodeSandbox::create(std::filesystem::path wasm_path) {
    std::error_code exists_error;
    if (!std::filesystem::exists(wasm_path, exists_error)) {
        return std::unexpected(
                sandbox_error(std::format("codemode sandbox guest '{}' does not exist", wasm_path.string()),
                        "the codemode guest module (quickjs.wasm) must be present to run declared scripts"));
    }
    auto impl = std::make_unique<Impl>();
    impl->wasm_path = std::move(wasm_path);
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
    return std::make_unique<CodemodeSandbox>(ConstructionKey{}, std::move(impl));
}

CodemodeSandbox::CodemodeSandbox(ConstructionKey, std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CodemodeSandbox::~CodemodeSandbox() = default;
CodemodeSandbox::CodemodeSandbox(CodemodeSandbox&&) noexcept = default;
CodemodeSandbox& CodemodeSandbox::operator=(CodemodeSandbox&&) noexcept = default;

namespace {

/// `drain` runs queued guest microtasks. It stops early once the script settled
/// or the host interrupted it, so a runaway microtask loop cannot spin here.
bool drain_jobs(const Guest& guest, const RunState& state) {
    while (guest.call_i32(guest.find("qjs_is_job_pending"), {}) != 0) {
        if (state.finished || state.timed_out || state.stop_token.stop_requested()) return false;
        if (guest.call_i32(guest.find("qjs_execute_pending_job"), {}) < 0) return false;
    }
    return true;
}

} // namespace

CodemodeRunResult CodemodeSandbox::run(std::string_view script,
        const std::vector<CodemodeToolDescriptor>& tools,
        CodemodeLimits limits,
        std::stop_token stop_token,
        CodemodeToolCallHandler handler) {
    RunState state;
    state.limits = limits;
    state.stop_token = stop_token;
    state.handler = std::move(handler);
    state.tools = tools;
    state.started = std::chrono::steady_clock::now();
    // The interrupt handler is armed only once the script starts (below), so the
    // timeout bounds the script, not the guest's one-time prelude evaluation.

    auto finish = [&state]() -> CodemodeRunResult {
        CodemodeRunResult result;
        result.output = std::move(state.output);
        result.value_json = std::move(state.value_json);
        result.error = std::move(state.error);
        return result;
    };

    if (stop_token.stop_requested()) {
        state.error = CodemodeError{CodemodeError::Kind::Aborted, "The script was cancelled before it ran."};
        return finish();
    }

    WasmEdge_StoreContext* store = WasmEdge_StoreCreate();
    WasmEdge_ExecutorContext* executor = WasmEdge_ExecutorCreate(impl_->configure, nullptr);

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
            registered ? WasmEdge_ExecutorInstantiate(executor, &module, store, impl_->ast) : WasmEdge_Result_Fail;
    if (!registered || !WasmEdge_ResultOK(instantiated)) {
        fail_sandbox(std::format("The codemode sandbox denied the script: the guest could not be instantiated "
                                 "({}). A required import was not provided.",
                registered ? WasmEdge_ResultGetMessage(instantiated) : "import registration failed"));
        WasmEdge_ModuleInstanceDelete(env);
        WasmEdge_ModuleInstanceDelete(wasi);
        WasmEdge_ExecutorDelete(executor);
        WasmEdge_StoreDelete(store);
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
        const std::string no_globals = "[]";
        const std::string no_store = "{}";
        const std::string tool_list = tools_json(state.tools);
        const Handle api = g.call(prelude,
                undefined,
                {bridge, g.new_string(tool_list), g.new_string(no_globals), g.new_string(no_store)});
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
            state.error = CodemodeError{CodemodeError::Kind::Script, g.exception_text()};
            cleanup();
            return finish();
        }

        // Arm the deadline and the interrupt before the script body runs, so the
        // timeout bounds the script (including a synchronous runaway loop) and
        // not the fixed prelude evaluation above.
        state.started = std::chrono::steady_clock::now();
        g.call_void(g.find("qjs_set_interrupt_handler"), {gen_i32(1)});
        g.call(run_fn, api, {script_fn});
        drain_jobs(g, state);

        // Settle every tool call the script is awaiting, then drain again. A
        // tool that is missing or has no handler rejects the call the script
        // sees — never a silent success.
        while (!state.finished && !state.timed_out && !state.stop_token.stop_requested()) {
            if (state.pending.empty()) {
                g.call(stalled_fn, api, {});
                drain_jobs(g, state);
                if (!state.finished) {
                    fail_sandbox("The codemode script did not settle: it is waiting on a promise that can "
                                 "never resolve, and the sandbox has no timers or I/O.");
                }
                break;
            }
            std::vector<PendingToolCall> calls = std::move(state.pending);
            state.pending.clear();
            for (const auto& call : calls) {
                const auto id_handle =
                        g.call_i32(g.find("qjs_new_number"), {WasmEdge_ValueGenF64(static_cast<double>(call.id))});
                std::optional<std::string> payload;
                bool ok = false;
                if (state.handler) {
                    auto outcome = state.handler(call.name, call.arguments_json);
                    if (outcome) {
                        ok = true;
                        payload = std::move(*outcome);
                    } else {
                        payload = outcome.error().message;
                    }
                } else {
                    payload = std::format("tool '{}' is not available to the codemode sandbox", call.name);
                }
                const Handle payload_handle = payload.has_value() ? g.new_string(*payload) : g.undefined();
                g.call(settle_fn, api, {id_handle, ok ? g.true_value() : g.false_value(), payload_handle});
                drain_jobs(g, state);
            }
        }
    }

    if (!state.finished) {
        if (state.timed_out) {
            state.error = CodemodeError{CodemodeError::Kind::Timeout,
                    std::format("Execution timed out after {} ms", state.limits.timeout.count())};
        } else if (state.stop_token.stop_requested()) {
            state.error = CodemodeError{CodemodeError::Kind::Aborted, "The script was cancelled."};
        } else if (!state.error.has_value()) {
            fail_sandbox("The codemode sandbox guest stopped without reporting a result.");
        }
    }

    cleanup();
    return finish();
}

} // namespace cch::coding_agent::extensions
