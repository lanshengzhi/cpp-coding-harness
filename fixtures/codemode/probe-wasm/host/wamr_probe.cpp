// host/wamr_probe.cpp — probe #868: WAMR (wasm-micro-runtime) codemode probe.
//
// Second candidate. Same commands as the WasmEdge probe so the two can be
// compared directly. WAMR is not in the pinned vcpkg baseline, so this was built
// from upstream source (see README) — that build-adaptation cost is part of what
// the probe measures.
//
// The probe supplies the same minimal capability surface pi's host uses (a
// hand-written WASI shim, no runtime-provided filesystem or sockets), which is
// why WAMR is built without libc-wasi for the comparison.

#include "wasm_export.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_bridge_calls = 0;
int32_t g_undefined_handle = 0;
std::string g_fd_write_text;

// Returns a host address for `offset` when the range is valid, else nullptr.
void* MemOffsetToNative(wasm_exec_env_t env, uint32_t offset, uint32_t size) {
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    if (!wasm_runtime_validate_app_addr(inst, offset, size)) return nullptr;
    return wasm_runtime_addr_app_to_native(inst, offset);
}

int32_t HostCall(wasm_exec_env_t env,
        uint32_t name_ptr,
        uint32_t name_len,
        uint32_t this_ptr,
        uint32_t argc,
        uint32_t argv_ptr) {
    const void* host = MemOffsetToNative(env, name_ptr, name_len);
    const std::string name = host == nullptr ? "<oob>" : std::string(static_cast<const char*>(host), name_len);
    g_bridge_calls++;
    std::printf("[guest -> host bridge] host_call name=\"%s\" argc=%u (argv=0x%x)\n", name.c_str(), argc, argv_ptr);
    (void)this_ptr;
    return g_undefined_handle;
}

int32_t HostGetTimezoneOffset(wasm_exec_env_t, uint32_t, uint32_t) { return 0; }
int32_t HostInterrupt(wasm_exec_env_t) { return 0; }
void HostPromiseRejection(wasm_exec_env_t, uint32_t, uint32_t, uint32_t) {}
int32_t HostModuleLoader(wasm_exec_env_t, uint32_t, uint32_t) { return 0; }

// ---- WASI preview1 subset pi's guest imports (mirrors wasi-shim.js) ----

int32_t WasiClockTimeGet(wasm_exec_env_t env, uint32_t clock_id, uint64_t, uint32_t result_ptr) {
    if (clock_id != 0 && clock_id != 1) return 52; // NOSYS
    void* p = MemOffsetToNative(env, result_ptr, 8);
    if (p == nullptr) return 21; // FAULT
    *static_cast<uint64_t*>(p) = 1700000000000000000ull;
    return 0;
}

int32_t WasiRandomGet(wasm_exec_env_t env, uint32_t buf, uint32_t len) {
    void* p = MemOffsetToNative(env, buf, len);
    if (p == nullptr) return 21;
    std::memset(p, 0x42, len);
    return 0;
}

int32_t WasiFdWrite(wasm_exec_env_t env, uint32_t fd, uint32_t iovs, uint32_t iovs_len, uint32_t nwritten) {
    if (fd != 1 && fd != 2) return 8; // BADF
    uint32_t total = 0;
    for (uint32_t i = 0; i < iovs_len; i++) {
        void* iov = MemOffsetToNative(env, iovs + i * 8, 8);
        if (iov == nullptr) return 21;
        const uint32_t ptr = *static_cast<uint32_t*>(iov);
        const uint32_t len = *(reinterpret_cast<uint32_t*>(iov) + 1);
        void* buf = MemOffsetToNative(env, ptr, len);
        if (buf == nullptr) return 21;
        g_fd_write_text.append(static_cast<const char*>(buf), len);
        total += len;
    }
    void* nw = MemOffsetToNative(env, nwritten, 4);
    if (nw != nullptr) *static_cast<uint32_t*>(nw) = total;
    return 0;
}

int32_t WasiFdClose(wasm_exec_env_t, uint32_t) { return 52; }

int32_t WasiFdSeek(wasm_exec_env_t, uint32_t, uint64_t, uint32_t, uint32_t) { return 52; }

int32_t WasiFdFdstatGet(wasm_exec_env_t env, uint32_t fd, uint32_t stat_ptr) {
    if (fd != 1 && fd != 2) return 8;
    void* p = MemOffsetToNative(env, stat_ptr, 24);
    if (p == nullptr) return 21;
    std::memset(p, 0, 24);
    *static_cast<uint8_t*>(p) = 2; // fs_filetype = CHARACTER_DEVICE
    return 0;
}

struct Instance {
    wasm_module_t module = nullptr;
    wasm_module_inst_t inst = nullptr;
    wasm_exec_env_t env = nullptr;

    ~Instance() {
        if (env) wasm_runtime_destroy_exec_env(env);
        if (inst) wasm_runtime_deinstantiate(inst);
        if (module) wasm_runtime_unload(module);
    }

    bool Call(wasm_function_inst_t fn, const std::vector<uint32_t>& args, uint32_t* result) {
        std::vector<uint32_t> argv(args.size() + 2, 0);
        for (size_t i = 0; i < args.size(); i++)
            argv[i] = args[i];
        if (!wasm_runtime_call_wasm(env, fn, static_cast<uint32_t>(args.size()), argv.data())) {
            std::printf("call_wasm failed: %s\n", wasm_runtime_get_exception(inst));
            return false;
        }
        if (result != nullptr) *result = argv[0];
        return true;
    }

    int32_t CallI32(const char* name, const std::vector<uint32_t>& args) {
        wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, name);
        if (fn == nullptr) {
            std::printf("lookup `%s` failed\n", name);
            return 0;
        }
        uint32_t result = 0;
        if (!Call(fn, args, &result)) return 0;
        return static_cast<int32_t>(result);
    }

    uint32_t WriteString(const std::string& text) {
        const int32_t ptr = CallI32("wasm_malloc", {static_cast<uint32_t>(text.size() + 1)});
        void* host = MemOffsetToNative(env, static_cast<uint32_t>(ptr), static_cast<uint32_t>(text.size() + 1));
        if (host == nullptr) return 0;
        std::memcpy(host, text.c_str(), text.size() + 1);
        return static_cast<uint32_t>(ptr);
    }

    std::string StringifyValue(int32_t handle) {
        const uint32_t len_ptr = WriteString("    ");
        const int32_t cstr = CallI32("qjs_get_string_len", {static_cast<uint32_t>(handle), len_ptr});
        if (cstr == 0) return "<null>";
        uint32_t length = 0;
        void* len_host = MemOffsetToNative(env, len_ptr, 4);
        if (len_host != nullptr) std::memcpy(&length, len_host, 4);
        void* str_host = MemOffsetToNative(env, static_cast<uint32_t>(cstr), length);
        if (str_host == nullptr) return "<oob>";
        return std::string(static_cast<const char*>(str_host), length);
    }
};

void RegisterEnvBridge() {
    static NativeSymbol symbols[] = {
            {"bridge", reinterpret_cast<void*>(HostCall), "(ii)i", nullptr},
    };
    wasm_runtime_register_natives("env", symbols, 1);
}

void RegisterEnvFull() {
    static NativeSymbol symbols[] = {
            {"host_get_timezone_offset", reinterpret_cast<void*>(HostGetTimezoneOffset), "(ii)i", nullptr},
            {"host_interrupt", reinterpret_cast<void*>(HostInterrupt), "()i", nullptr},
            {"host_promise_rejection", reinterpret_cast<void*>(HostPromiseRejection), "(iii)", nullptr},
            {"host_module_normalize", reinterpret_cast<void*>(HostModuleLoader), "(ii)i", nullptr},
            {"host_module_load", reinterpret_cast<void*>(HostModuleLoader), "(ii)i", nullptr},
            {"host_call", reinterpret_cast<void*>(HostCall), "(iiiii)i", nullptr},
    };
    wasm_runtime_register_natives("env", symbols, sizeof(symbols) / sizeof(symbols[0]));
}

void RegisterWasi() {
    static NativeSymbol symbols[] = {
            {"clock_time_get", reinterpret_cast<void*>(WasiClockTimeGet), "(iIi)i", nullptr},
            {"random_get", reinterpret_cast<void*>(WasiRandomGet), "(ii)i", nullptr},
            {"fd_write", reinterpret_cast<void*>(WasiFdWrite), "(iiii)i", nullptr},
            {"fd_close", reinterpret_cast<void*>(WasiFdClose), "(i)i", nullptr},
            {"fd_seek", reinterpret_cast<void*>(WasiFdSeek), "(iIii)i", nullptr},
            {"fd_fdstat_get", reinterpret_cast<void*>(WasiFdFdstatGet), "(ii)i", nullptr},
    };
    wasm_runtime_register_natives("wasi_snapshot_preview1", symbols, sizeof(symbols) / sizeof(symbols[0]));
}

bool LoadAndInstantiate(const char* path, Instance& inst, bool env_full) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("cannot read %s\n", path);
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    if (env_full) {
        RegisterEnvFull();
    } else {
        RegisterEnvBridge();
    }
    RegisterWasi();

    char error[256] = {0};
    inst.module = wasm_runtime_load(bytes.data(), static_cast<uint32_t>(bytes.size()), error, sizeof(error));
    std::printf("load: %s%s%s\n",
            inst.module != nullptr ? "ok" : "FAILED",
            inst.module != nullptr ? "" : " (",
            inst.module != nullptr ? "" : error);
    if (inst.module == nullptr) return false;

    inst.inst = wasm_runtime_instantiate(inst.module, 2 * 1024 * 1024, 0, error, sizeof(error));
    std::printf("instantiate: %s%s%s\n",
            inst.inst != nullptr ? "ok" : "FAILED",
            inst.inst != nullptr ? "" : " (",
            inst.inst != nullptr ? "" : error);
    if (inst.inst == nullptr) return false;

    inst.env = wasm_runtime_create_exec_env(inst.inst, 2 * 1024 * 1024);
    return inst.env != nullptr;
}

int CommandRawGuest(const char* path) {
    wasm_runtime_init();
    Instance inst;
    if (!LoadAndInstantiate(path, inst, /*env_full=*/false)) {
        std::printf("FAIL-CLOSED: instantiation refused because an import was not provided\n");
        return 3;
    }
    wasm_function_inst_t run = wasm_runtime_lookup_function(inst.inst, "run");
    if (run == nullptr) {
        std::printf("no exported `run`\n");
        return 1;
    }
    uint32_t result = 0;
    if (!inst.Call(run, {}, &result)) {
        std::printf("FAIL-CLOSED: calling an unprovided import threw inside the guest\n");
        return 3;
    }
    std::printf("run() -> %d, host bridge calls = %d\n", static_cast<int32_t>(result), g_bridge_calls);
    return 0;
}

int CommandQuickjs(const char* wasm_path, const char* script_path) {
    wasm_runtime_init();
    Instance inst;
    if (!LoadAndInstantiate(wasm_path, inst, /*env_full=*/true)) return 1;
    std::printf("_initialize() -> %d\n", inst.CallI32("_initialize", {}));
    const int32_t qjs_rc = inst.CallI32("qjs_init", {});
    std::printf("qjs_init() -> %d\n", qjs_rc);
    if (qjs_rc != 0) return 1;
    g_undefined_handle = inst.CallI32("qjs_get_undefined", {});

    const uint32_t name_ptr = inst.WriteString("output");
    const int32_t fn_handle = inst.CallI32("qjs_new_host_function", {name_ptr, 6, 0});
    const int32_t global_handle = inst.CallI32("qjs_get_global", {});
    const uint32_t global_name = inst.WriteString("output");
    inst.CallI32("qjs_define_prop_string",
            {static_cast<uint32_t>(global_handle), global_name, static_cast<uint32_t>(fn_handle), 7});
    inst.CallI32("qjs_call", {static_cast<uint32_t>(fn_handle), static_cast<uint32_t>(g_undefined_handle), 0, 0});
    std::printf("guest host-function `output` created; host bridge calls after host-side call = %d\n", g_bridge_calls);

    std::ifstream file(script_path);
    if (!file) {
        std::printf("cannot read script %s\n", script_path);
        return 1;
    }
    const std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::printf("script: %s (%zu bytes)\n", script_path, code.size());

    const uint32_t code_ptr = inst.WriteString(code);
    const uint32_t fn_ptr = inst.WriteString("<probe>");
    const int32_t result = inst.CallI32("qjs_eval", {code_ptr, static_cast<uint32_t>(code.size()), fn_ptr, 0});

    if (inst.CallI32("qjs_is_exception", {static_cast<uint32_t>(result)}) != 0) {
        const int32_t exc = inst.CallI32("qjs_get_exception", {});
        std::printf("RESULT: guest threw: %s\n", inst.StringifyValue(exc).c_str());
        return 0;
    }
    std::printf("RESULT: script returned value:\n%s\n", inst.StringifyValue(result).c_str());
    if (!g_fd_write_text.empty()) std::printf("guest fd_write captured %zu bytes\n", g_fd_write_text.size());
    return 0;
}

void Usage(const char* argv0) {
    std::printf("usage:\n"
                "  %s raw-guest <guest.wasm>\n"
                "  %s quickjs <quickjs.wasm> <script.js>\n",
            argv0,
            argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        Usage(argv[0]);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "raw-guest") return CommandRawGuest(argv[2]);
    if (cmd == "quickjs" && argc >= 4) return CommandQuickjs(argv[2], argv[3]);
    Usage(argv[0]);
    return 2;
}
