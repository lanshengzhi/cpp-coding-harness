#include "codemode/Guest.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::codemode::detail {

WasmEdge_FunctionInstanceContext* Guest::find(const char* name) const {
    WasmEdge_String s = WasmEdge_StringCreateByCString(name);
    WasmEdge_FunctionInstanceContext* f = WasmEdge_ModuleInstanceFindFunction(module, s);
    WasmEdge_StringDelete(s);
    return f;
}

std::int32_t Guest::call_i32(WasmEdge_FunctionInstanceContext* fn, std::initializer_list<WasmEdge_Value> params) const {
    if (fn == nullptr) return 0;
    std::vector<WasmEdge_Value> in(params);
    WasmEdge_Value out[1]{};
    if (!WasmEdge_ResultOK(WasmEdge_ExecutorInvoke(
                executor, fn, in.empty() ? nullptr : in.data(), static_cast<std::uint32_t>(in.size()), out, 1))) {
        return 0;
    }
    return WasmEdge_ValueGetI32(out[0]);
}

void Guest::call_void(WasmEdge_FunctionInstanceContext* fn, std::initializer_list<WasmEdge_Value> params) const {
    if (fn == nullptr) return;
    std::vector<WasmEdge_Value> in(params);
    (void)WasmEdge_ExecutorInvoke(
            executor, fn, in.empty() ? nullptr : in.data(), static_cast<std::uint32_t>(in.size()), nullptr, 0);
}

std::uint32_t Guest::load_u32(std::uint32_t offset) const {
    std::uint32_t value = 0;
    if (std::uint8_t* p = WasmEdge_MemoryInstanceGetPointer(memory, offset, 4)) std::memcpy(&value, p, 4);
    return value;
}

void Guest::store_u32(std::uint32_t offset, std::uint32_t value) const {
    if (std::uint8_t* p = WasmEdge_MemoryInstanceGetPointer(memory, offset, 4)) std::memcpy(p, &value, 4);
}

void Guest::store_u64(std::uint32_t offset, std::uint64_t value) const {
    if (std::uint8_t* p = WasmEdge_MemoryInstanceGetPointer(memory, offset, 8)) std::memcpy(p, &value, 8);
}

std::uint32_t Guest::write_cstr(std::string_view text) const {
    const std::int32_t ptr = call_i32(find("wasm_malloc"), {gen_i32(static_cast<std::int32_t>(text.size() + 1))});
    if (ptr == 0) return 0;
    std::uint8_t* dst = WasmEdge_MemoryInstanceGetPointer(
            memory, static_cast<std::uint32_t>(ptr), static_cast<std::uint32_t>(text.size() + 1));
    if (dst == nullptr) return 0;
    std::memcpy(dst, text.data(), text.size());
    dst[text.size()] = '\0';
    return static_cast<std::uint32_t>(ptr);
}

std::string Guest::read_cstr(std::uint32_t pointer) const {
    std::string text;
    if (pointer == 0) return text;
    for (std::uint32_t offset = pointer;; ++offset) {
        const std::uint8_t* byte = WasmEdge_MemoryInstanceGetPointer(memory, offset, 1);
        if (byte == nullptr || *byte == 0) break;
        text.push_back(static_cast<char>(*byte));
    }
    return text;
}

std::string Guest::read_string(Handle handle) const {
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

Handle Guest::new_string(std::string_view text) const {
    const std::uint32_t ptr = write_cstr(text);
    return call_i32(find("qjs_new_string"),
            {gen_i32(static_cast<std::int32_t>(ptr)), gen_i32(static_cast<std::int32_t>(text.size()))});
}

Handle Guest::undefined() const { return call_i32(find("qjs_get_undefined"), {}); }
Handle Guest::true_value() const { return call_i32(find("qjs_get_true"), {}); }
Handle Guest::false_value() const { return call_i32(find("qjs_get_false"), {}); }

bool Guest::is_undefined(Handle handle) const { return call_i32(find("qjs_is_undefined"), {gen_i32(handle)}) != 0; }

Handle Guest::global() const { return call_i32(find("qjs_get_global"), {}); }

Handle Guest::get_prop(Handle object, std::string_view name) const {
    const std::uint32_t ptr = write_cstr(name);
    return call_i32(find("qjs_get_prop_string"), {gen_i32(object), gen_i32(static_cast<std::int32_t>(ptr))});
}

Handle Guest::call(Handle function, Handle this_value, const std::vector<Handle>& args) const {
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

bool Guest::is_exception(Handle handle) const { return call_i32(find("qjs_is_exception"), {gen_i32(handle)}) != 0; }

std::string Guest::exception_text() const {
    const Handle exc = call_i32(find("qjs_get_exception"), {});
    return read_string(exc);
}

CodemodeError Guest::exception_error() const {
    const Handle exc = call_i32(find("qjs_get_exception"), {});
    if (is_undefined(exc)) return CodemodeError{CodemodeError::Kind::Script, {}};
    CodemodeError error{CodemodeError::Kind::Script, {}};
    const Handle name = get_prop(exc, "name");
    const Handle message = get_prop(exc, "message");
    const Handle stack = get_prop(exc, "stack");
    if (!is_undefined(name)) error.name = read_string(name);
    if (!is_undefined(message)) error.message = read_string(message);
    if (!is_undefined(stack)) {
        std::string text = read_string(stack);
        // QuickJS frames-only stacks get the same `Name: message` head the
        // prelude's errorText and frozen pi both report.
        if (error.name.has_value() && !text.starts_with(*error.name)) {
            const std::string head = error.message.empty() ? *error.name : *error.name + ": " + error.message;
            if (!text.empty())
                text = head + "\n" + text;
            else
                text = head;
        }
        error.stack = std::move(text);
    }
    if (error.message.empty() && !is_undefined(exc)) error.message = read_string(exc);
    return error;
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

bool drain_jobs(const Guest& guest, const std::function<bool()>& should_stop) {
    while (guest.call_i32(guest.find("qjs_is_job_pending"), {}) != 0) {
        if (should_stop()) return false;
        if (guest.call_i32(guest.find("qjs_execute_pending_job"), {}) < 0) return false;
    }
    return true;
}

WasmEdge_Result HostPromiseRejection(
        void*, const WasmEdge_CallingFrameContext* frame, const WasmEdge_Value* in, WasmEdge_Value*) {
    // The shim frees the two heap-owned values the guest handed over; without a
    // rejection handler they would leak. The sandbox has no unhandled-rejection
    // surface: a rejection reaches the script through `settle`.
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

WasmEdge_Result HostModuleDenied(
        void*, const WasmEdge_CallingFrameContext*, const WasmEdge_Value*, WasmEdge_Value* out) {
    // No ESM loader is registered: an `import`/`require` in the script fails
    // closed rather than reaching any host module.
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

WasmEdge_Result HostWasiStub(void*, const WasmEdge_CallingFrameContext*, const WasmEdge_Value*, WasmEdge_Value* out) {
    out[0] = gen_i32(kWasiErnoNosys);
    return WasmEdge_Result_Success;
}

} // namespace cch::codemode::detail
