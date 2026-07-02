The project uses **CMake** (minimum version 3.25) as its primary build system, targeting **C++23**. Dependency management is handled via **vcpkg** in manifest mode, with a fallback for system-installed packages.

### Build Configuration
- **CMakePresets.json**: Defines three main presets:
  - `vcpkg`: Debug build using the local or environment-defined vcpkg instance.
  - `vcpkg-release`: Release build using vcpkg.
  - `system`: Debug build using system-wide dependencies (no vcpkg).
- **CMakeLists.txt**: Organizes the codebase into several static libraries (`cch_util`, `cch_ai`, `cch_agent`, `cch_harness`, `cch_tools`, `cch_coding_agent_runtime`) and two executables (`cpp_harness` for the CLI and `cpp_harness_tests` for unit tests). Tests are enabled by default via the `CCH_BUILD_TESTS` option and use **CTest**.

### Dependency Management
- **vcpkg.json**: Declares dependencies including `glaze`, `boost-process`, `boost-beast`, `boost-asio`, `openssl`, `cli11`, and `catch2`. A specific `builtin-baseline` is pinned to ensure reproducible builds.
- **Bootstrap Script**: `scripts/bootstrap.sh` automates the setup process by cloning vcpkg into `.deps/vcpkg` (if not present), building the vcpkg binary, and then configuring/building/testing the project using the appropriate CMake preset. It handles cache invalidation when switching between vcpkg and system modes.

### Testing & Validation
- **Unit Tests**: Built as `cpp_harness_tests` using Catch2. The test executable is linked against an interface library `cpp_harness_lib` which aggregates the runtime components.
- **Smoke Tests**: `scripts/kimi_live_smoke.sh` provides an integration test that runs the built binary against the Kimi API (if enabled via `CCH_LIVE_KIMI=1`). It includes safety checks to ensure API keys are not leaked into logs or session files.

### Developer Workflow
1. **Initial Setup**: Run `./scripts/bootstrap.sh` to fetch dependencies and build the project in Debug mode.
2. **Building**: Use `cmake --preset vcpkg` or `cmake --build --preset vcpkg`.
3. **Testing**: Run `ctest --preset vcpkg` or use the `--test` flag with the bootstrap script.
4. **Release Builds**: Use the `vcpkg-release` preset or pass `--release` to the bootstrap script.