The repository uses **vcpkg** in **manifest mode** as its primary dependency management system for C++ third-party libraries. Dependencies are declared in a root-level `vcpkg.json` manifest, which is automatically consumed by CMake via the vcpkg toolchain file.

### Dependency Declaration
- **Manifest File**: `vcpkg.json` lists direct dependencies: `glaze`, `boost-process`, `boost-beast`, `boost-asio`, `openssl`, `cli11`, and `catch2`.
- **Version Pinning**: The manifest includes a `builtin-baseline` hash (`f3e10653cc27d62a37a3763cd84b38bca07c6075`) to ensure reproducible builds by locking the vcpkg port versions to a specific commit in the vcpkg registry.

### Build System Integration
- **CMake Toolchain**: `CMakePresets.json` defines a `vcpkg` preset that sets `CMAKE_TOOLCHAIN_FILE` to `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`. This enables "manifest mode," where vcpkg reads `vcpkg.json` and installs dependencies into the build tree or a local cache before configuration.
- **Fallback Strategy**: A `system` preset is also provided in `CMakePresets.json` for building without vcpkg, relying on system-installed packages found via `find_package`.
- **Linking**: `CMakeLists.txt` uses standard `find_package` calls (e.g., `find_package(glaze CONFIG REQUIRED)`) and links against imported targets (e.g., `glaze::glaze`, `CLI11::CLI11`).

### Vendoring & Local Overrides
- **Local vcpkg Instance**: A full vcpkg repository is vendored under `.deps/vcpkg/`. This ensures that the port definitions and scripts are available even without an external vcpkg installation, though the toolchain file typically points to an environment variable `$VCPKG_ROOT`.
- **Minimal Third-Party Vendoring**: The `third_party/` directory contains a minimal, header-only stub for `catch2` (`catch_session.hpp`, `catch_test_macros.hpp`). This appears to be a lightweight internal test harness shim rather than the full Catch2 library, which is otherwise managed via vcpkg.

### Developer Conventions
1. **Add Dependencies via Manifest**: New libraries should be added to `vcpkg.json` rather than manually downloaded or added to `third_party/`.
2. **Use CMake Presets**: Developers should use `cmake --preset=vcpkg` to ensure dependencies are resolved consistently.
3. **Reproducibility**: The `builtin-baseline` in `vcpkg.json` should be updated deliberately to upgrade dependencies across the project.